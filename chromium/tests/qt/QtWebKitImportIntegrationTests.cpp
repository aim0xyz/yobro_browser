#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTreeWidget>
#include <QTimer>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void waitUntil(const std::function<bool()> &ready, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (ready()) return;
        QTest::qWait(10);
    }
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    check(output.good(), "Could not write import fixture.");
}

std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct PreviewApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<yobro::spike::SpikeWindow> window;

    PreviewApp()
        : paths(yobro::core::ProfilePaths::forProfile("import-integration")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        profile = engine.openProfile({
            .id = "import-integration",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create Chromium import test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "import-integration",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<yobro::spike::SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
    }

    ~PreviewApp() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

} // namespace

int main(int argc, char *argv[]) {
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const std::filesystem::path rootPath = root.path().toStdString();
    const auto chromiumHome = rootPath / "chromium";
    const auto source = rootPath / "webkit-source";
    qputenv("YOBRO_CHROMIUM_HOME", QString::fromStdString(chromiumHome.string()).toUtf8());
    qputenv("YOBRO_WEBKIT_HOME", QString::fromStdString(source.string()).toUtf8());
    QApplication application(argc, argv);
    try {
        write(source / "history.json", R"JSON([{"title":"Imported history","url":"https://history.example.test","visits":4},{"title":"Rejected","url":"file:///private"}])JSON");
        write(source / "bookmarks.json", R"JSON([{"title":"Imported bookmark","url":"https://bookmark.example.test"}])JSON");
        write(source / "session.json", R"JSON({"tabs":[{"title":"Imported tab","url":"https://tab.example.test"},{"title":"Rejected","url":"data:text/html,no"}]})JSON");
        const std::string historyBefore = read(source / "history.json");
        const std::string bookmarksBefore = read(source / "bookmarks.json");
        const std::string sessionBefore = read(source / "session.json");

        {
            PreviewApp preview;
            auto *productShell = preview.window->findChild<QWidget *>(QStringLiteral("productShell"));
            auto *brand = preview.window->findChild<QLabel *>(QStringLiteral("productBrand"));
            auto *topNavigation = preview.window->findChild<QWidget *>(QStringLiteral("topNavigation"));
            auto *workspaceSidebar = preview.window->findChild<QWidget *>(QStringLiteral("workspaceSidebar"));
            auto *topAddress = preview.window->findChild<QLineEdit *>(QStringLiteral("topAddress"));
            check(productShell != nullptr && brand != nullptr && brand->text() == QStringLiteral("yobro")
                  && topNavigation != nullptr && workspaceSidebar != nullptr
                  && workspaceSidebar->minimumWidth() == 248 && workspaceSidebar->maximumWidth() == 248
                  && topAddress != nullptr,
                  "The product sidebar shell is missing its primary navigation.");
            check(preview.window->windowTitle() == QStringLiteral("YOBRO"),
                  "The product window still exposes engineering chrome.");
            auto *openImport = preview.window->findChild<QPushButton *>(QStringLiteral("webkitImportButton"));
            check(openImport != nullptr, "Import entry point is missing.");
            openImport->click();
            auto *sourceField = preview.window->findChild<QLineEdit *>(QStringLiteral("webkitImportSource"));
            auto *previewButton = preview.window->findChild<QPushButton *>(QStringLiteral("webkitImportPreview"));
            auto *commitButton = preview.window->findChild<QPushButton *>(QStringLiteral("webkitImportCommit"));
            check(sourceField && previewButton && commitButton, "Import dialog controls are missing.");
            sourceField->setText(QString::fromStdString(source.string()));
            previewButton->click();
            waitUntil([&] { return commitButton->isEnabled(); }, "Import preview did not enable confirmation.");
            QTimer::singleShot(150, &application, [&application] {
                auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (message) {
                    if (auto *yes = message->button(QMessageBox::Yes)) yes->click();
                    else message->done(QMessageBox::Yes);
                }
            });
            commitButton->click();
            waitUntil([&] { return preview.window->findChild<QDialog *>(QStringLiteral("webkitImportDialog")) == nullptr; }, "Import confirmation did not finish.");
            check(read(source / "history.json") == historyBefore
                  && read(source / "bookmarks.json") == bookmarksBefore
                  && read(source / "session.json") == sessionBefore,
                  "Import modified the WebKit source profile.");
            check(preview.library->history({}, 10).asArray().size() == 1,
                  "Imported history was not added to the Chromium library.");
            check(preview.library->bookmarks({}, 10).asArray().size() == 1,
                  "Imported bookmark was not added to the Chromium library.");
            auto *openLibrary = preview.window->findChild<QPushButton *>(QStringLiteral("libraryButton"));
            check(openLibrary != nullptr, "Library entry point is missing.");
            openLibrary->click();
            auto *bookmarkList = preview.window->findChild<QListWidget *>(QStringLiteral("bookmarksLibraryList"));
            check(bookmarkList && bookmarkList->count() == 1,
                  "Imported bookmark is not visible in the Chromium library UI.");
            check(preview.window->findChild<QTabWidget *>()->count() >= 2,
                  "Imported tab was not opened in the Chromium session.");
            check(read(preview.paths.session).find("tab.example.test") != std::string::npos,
                  "Imported tab was not written to the persistent Chromium session.");
            check(std::filesystem::exists(preview.paths.profile / "WebKit Import" / "webkit-import-journal.json"),
                  "Committed import journal is missing from the Chromium profile.");
            preview.window->close();
        }
        const auto persistedPaths = yobro::core::ProfilePaths::forProfile("import-integration");
        const std::string persistedSession = read(persistedPaths.session);
        check(persistedSession.find("tab.example.test") != std::string::npos,
              "Closing the Chromium window did not retain the imported session tab.");
        check(QJsonDocument::fromJson(QByteArray::fromStdString(persistedSession)).object()
                  .value(QStringLiteral("tabs")).toArray().size() >= 1,
              "The persisted Chromium session no longer contains its import tabs.");
        QTest::qWait(300);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        write(persistedPaths.session, R"JSON({"version":2,"activeSpace":"Work","spaces":["Personal","Work"],"spaceIcons":{"Work":"◆"},"folders":[{"id":"project","name":"Project","space":"Work","color":"#2f6f68"}],"tabs":[{"url":"https://personal.example.test","space":"Personal","folder":"","pinned":false},{"url":"https://tab.example.test","space":"Work","folder":"project","pinned":true}],"closedTabs":[{"url":"https://closed.example.test","title":"Closed tab","space":"Work","folder":"project","pinned":true},{"url":"file:///private","title":"Rejected","space":"Work","folder":"project","pinned":false}]})JSON");

        {
            PreviewApp restarted;
            check(restarted.paths.session == persistedPaths.session,
                  "Restart used a different Chromium profile session path.");
            check(restarted.library->history({}, 10).asArray().size() == 1,
                  "Imported history was not retained after profile restart.");
            check(restarted.library->bookmarks({}, 10).asArray().size() == 1,
                  "Imported bookmark was not retained after profile restart.");
            check(restarted.session->userPages().size() >= 1,
                  "Imported session URL was not restored into the browser controller.");
            auto *tabs = restarted.window->findChild<QTabWidget *>();
            auto *workspaceTree = restarted.window->findChild<QTreeWidget *>(QStringLiteral("workspaceTree"));
            auto *spaces = restarted.window->findChild<QComboBox *>(QStringLiteral("spacePicker"));
            auto *folders = restarted.window->findChild<QComboBox *>(QStringLiteral("folderPicker"));
            check(tabs != nullptr && !tabs->tabBar()->isVisible() && workspaceTree != nullptr && spaces != nullptr && folders != nullptr,
                  "The WebKit-style sidebar shell or hidden native tab strip is missing after restore.");
            check(spaces->count() == 2 && spaces->currentText() == QStringLiteral("Work"),
                  "Spaces were not restored into the product toolbar.");
            check(folders->findText(QStringLiteral("Project")) >= 0,
                  "Tab folder was not restored into the product toolbar.");
            const QJsonObject restoredSession = QJsonDocument::fromJson(QByteArray::fromStdString(read(restarted.paths.session))).object();
            check(restoredSession.value(QStringLiteral("spaceIcons")).toObject().value(QStringLiteral("Work")).toString() == QStringLiteral("◆"),
                  "Space icon metadata was not retained in the v2 session.");
            const QJsonArray restoredFolders = restoredSession.value(QStringLiteral("folders")).toArray();
            check(!restoredFolders.isEmpty() && restoredFolders.at(0).toObject().value(QStringLiteral("color")).toString() == QStringLiteral("#2f6f68"),
                  "Folder color metadata was not retained in the v2 session.");
            check(workspaceTree->topLevelItemCount() == 1 && workspaceTree->topLevelItem(0)->text(0) == QStringLiteral("◈  ◆ Work"),
                  "Space icon metadata was not rendered in the workspace sidebar.");
            waitUntil([workspaceTree] {
                return !workspaceTree->findItems(QStringLiteral("ANGEPINNT"), Qt::MatchExactly | Qt::MatchRecursive).isEmpty()
                    && !workspaceTree->findItems(QStringLiteral("Project"), Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
            }, "Workspace sidebar did not render pinned and folder groups.");
            const auto pinnedGroups = workspaceTree->findItems(
                QStringLiteral("ANGEPINNT"), Qt::MatchExactly | Qt::MatchRecursive
            );
            check(!pinnedGroups.isEmpty() && pinnedGroups.front()->childCount() == 1,
                  "Pinned restored workspace tab is missing from the sidebar.");
            const auto projectGroups = workspaceTree->findItems(
                QStringLiteral("Project"), Qt::MatchExactly | Qt::MatchRecursive
            );
            check(!projectGroups.isEmpty(), "Restored Project sidebar group is missing.");
            workspaceTree->collapseItem(projectGroups.front());
            waitUntil([&restarted] {
                const QJsonObject session = QJsonDocument::fromJson(QByteArray::fromStdString(read(restarted.paths.session))).object();
                return session.value(QStringLiteral("collapsedFolders")).toArray().contains(QStringLiteral("project"));
            }, "Collapsed folder state was not persisted.");
            auto *agentToggle = restarted.window->findChild<QPushButton *>(QStringLiteral("agentPaneToggle"));
            auto *agentPane = restarted.window->findChild<QWidget *>(QStringLiteral("agentPane"));
            check(agentToggle != nullptr && agentPane != nullptr && agentPane->isHidden(),
                  "Agent pane is not initially collapsed.");
            agentToggle->click();
            check(!agentPane->isHidden()
                      && agentToggle->text().startsWith(QStringLiteral("●  Agentenfläche offen")),
                  "Agent pane did not expand from the visible UI toggle.");
            agentToggle->click();
            check(agentPane->isHidden()
                      && agentToggle->text().startsWith(QStringLiteral("●  Bereit für deine Agenten")),
                  "Agent pane did not collapse from the visible UI toggle.");
            auto *reopenClosedTab = restarted.window->findChild<QPushButton *>(QStringLiteral("reopenClosedTabButton"));
            check(reopenClosedTab != nullptr && reopenClosedTab->isEnabled(),
                  "Persisted public closed tabs do not enable the reopen action.");
            reopenClosedTab->click();
            waitUntil([&restarted] { return restarted.session->userPages().size() == 3; },
                      "Reopening a closed tab did not create a new public user tab.");
            const QJsonObject afterReopen = QJsonDocument::fromJson(QByteArray::fromStdString(read(restarted.paths.session))).object();
            const QJsonArray afterReopenTabs = afterReopen.value(QStringLiteral("tabs")).toArray();
            check(afterReopen.value(QStringLiteral("closedTabs")).toArray().isEmpty() && afterReopenTabs.size() == 3,
                  "Reopened tab was not removed from the persistent closed-tab stack.");
            bool restoredClosedTab = false;
            for (const QJsonValue &value : afterReopenTabs) {
                const QJsonObject tab = value.toObject();
                if (tab.value(QStringLiteral("url")).toString() == QStringLiteral("https://closed.example.test")) {
                    restoredClosedTab = tab.value(QStringLiteral("space")).toString() == QStringLiteral("Work")
                        && tab.value(QStringLiteral("folder")).toString() == QStringLiteral("project")
                        && tab.value(QStringLiteral("pinned")).toBool(false);
                }
            }
            check(restoredClosedTab, "Closed tab did not retain its Workspace metadata after reopen.");
            waitUntil([tabs] { return tabs->count() >= 1; }, "Imported tab was not restored after profile restart.");
        }
        {
            PreviewApp restoredCollapse;
            auto *workspaceTree = restoredCollapse.window->findChild<QTreeWidget *>(QStringLiteral("workspaceTree"));
            const auto projectGroups = workspaceTree ? workspaceTree->findItems(
                QStringLiteral("Project"), Qt::MatchExactly | Qt::MatchRecursive
            ) : QList<QTreeWidgetItem *>{};
            check(!projectGroups.isEmpty() && !projectGroups.front()->isExpanded(),
                  "Collapsed folder state was not restored after profile restart.");
        }
        std::cout << "QT WEBKIT IMPORT INTEGRATION TESTS PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT WEBKIT IMPORT INTEGRATION TESTS FAIL: " << error.what() << '\n';
        return 1;
    }
}
