#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QIcon>
#include <QPixmap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
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
        : paths(yobro::core::ProfilePaths::forProfile("session-restore")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        profile = engine.openProfile({
            .id = "session-restore",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the session-restore test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "session-restore",
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

    [[nodiscard]] QTabWidget *tabs() const {
        return window->findChild<QTabWidget *>(QStringLiteral("browserTabs"));
    }
    [[nodiscard]] QTabWidget *splitTabs() const {
        return window->findChild<QTabWidget *>(QStringLiteral("splitTabs"));
    }
};

/// The sidebar tab rows, so restored labels and icons can be inspected.
std::vector<QTreeWidgetItem *> sidebarTabRows(const yobro::spike::SpikeWindow &window) {
    std::vector<QTreeWidgetItem *> rows;
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("workspaceTree"));
    if (!tree) return rows;
    for (int top = 0; top < tree->topLevelItemCount(); ++top) {
        QTreeWidgetItem *space = tree->topLevelItem(top);
        for (int groupIndex = 0; groupIndex < space->childCount(); ++groupIndex) {
            QTreeWidgetItem *group = space->child(groupIndex);
            for (int tabIndex = 0; tabIndex < group->childCount(); ++tabIndex)
                rows.push_back(group->child(tabIndex));
        }
    }
    return rows;
}

QJsonObject readSession(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return QJsonDocument::fromJson(QByteArray::fromStdString(bytes)).object();
}

} // namespace

int main(int argc, char *argv[]) {
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const std::filesystem::path rootPath = root.path().toStdString();
    qputenv("YOBRO_CHROMIUM_HOME", QString::fromStdString((rootPath / "chromium").string()).toUtf8());
    qputenv("YOBRO_WEBKIT_HOME", QString::fromStdString((rootPath / "webkit").string()).toUtf8());
    QApplication application(argc, argv);
    std::filesystem::path sessionFile;

    try {
        // First run: create two web tabs, split them, and shut down cleanly.
        {
            PreviewApp first;
            sessionFile = first.paths.session;
            auto *tabs = first.tabs();
            check(tabs != nullptr, "The browser tab host is missing.");

            auto *sidebar = first.window->findChild<QWidget *>(QStringLiteral("workspaceSidebar"));
            check(sidebar != nullptr, "The sidebar is missing in the first run.");
            sidebar->setVisible(false);
            auto &alpha = first.session->newUserTab("https://alpha.example.test/");
            auto &beta = first.session->newUserTab("https://beta.example.test/");
            pump(1200);
            check(tabs->count() >= 2, "The first run did not open both tabs.");

            // Give both restored tabs a title and an icon the way a real page
            // would, so persistence can be observed without network access.
            const std::string betaId = beta.state().id;
            check(first.session->setActiveUserTab(betaId), "Could not activate the second tab.");
            pump(200);

            // A note is a tab of its own kind, so it belongs to the session too.
            auto *noteButton = first.window->findChild<QPushButton *>(QStringLiteral("newNoteButton"));
            check(noteButton != nullptr, "The new-note button is missing.");
            const int tabsBeforeNote = tabs->count();
            noteButton->click();
            pump(300);
            check(tabs->count() == tabsBeforeNote + 1, "Creating a note did not add a tab.");
            auto *editor = first.window->findChild<yobro::spike::NoteEditor *>();
            check(editor != nullptr, "The note editor is missing.");
            auto *titleField = editor->findChild<QLineEdit *>(QStringLiteral("noteTitleField"));
            auto *contentField = editor->findChild<QTextEdit *>(QStringLiteral("noteContentField"));
            check(titleField != nullptr && contentField != nullptr, "The note editor is incomplete.");
            titleField->setText(QStringLiteral("Einkaufsliste"));
            contentField->setHtml(QStringLiteral("<p>Milch und <b>Brot</b></p>"));
            pump(200);
            check(tabs->tabText(tabs->count() - 1) == QStringLiteral("Einkaufsliste"),
                  "The note tab does not carry the note title.");
            // The note also shows up in the sidebar, marked as a note.
            const auto rowsWithNote = sidebarTabRows(*first.window);
            const bool noteListed = std::any_of(rowsWithNote.begin(), rowsWithNote.end(), [](QTreeWidgetItem *row) {
                return row->text(0).contains(QStringLiteral("Einkaufsliste"));
            });
            check(noteListed, "The note is missing from the sidebar.");

            // Splitting pairs the active tab with the other visible one.
            QMetaObject::invokeMethod(first.window.get(), "close");
            pump(300);
            check(std::filesystem::exists(sessionFile), "The first run wrote no session file.");
            (void)alpha;
        }

        QJsonObject saved = readSession(sessionFile);
        const QJsonArray savedTabs = saved.value(QStringLiteral("tabs")).toArray();
        check(savedTabs.size() == 2, "The session file did not record both tabs.");
        check(saved.contains(QStringLiteral("activeTab")), "The session file has no active tab.");
        check(saved.value(QStringLiteral("activeTab")).toInt(-1) == 1,
              "The session file recorded the wrong active tab.");
        check(saved.value(QStringLiteral("sidebarVisible")).toBool(true) == false,
              "The hidden sidebar was not persisted.");
        check(!saved.value(QStringLiteral("geometry")).toString().isEmpty(),
              "The window geometry was not persisted.");
        check(saved.value(QStringLiteral("splitTab")).toInt(-1) == -1,
              "No split was active, so no split index may be stored.");
        const QJsonArray savedNotes = saved.value(QStringLiteral("notes")).toArray();
        check(savedNotes.size() == 1, "The session file did not record the note.");
        {
            const QJsonObject note = savedNotes.first().toObject();
            check(note.value(QStringLiteral("title")).toString() == QStringLiteral("Einkaufsliste"),
                  "The note title was not persisted.");
            // The formatting has to survive, not just the words.
            const QString html = note.value(QStringLiteral("html")).toString();
            check(html.contains(QStringLiteral("Milch")), "The note content was not persisted.");
            check(html.contains(QStringLiteral("font-weight")) || html.contains(QStringLiteral("<b")),
                  "The note lost its formatting.");
            check(!note.value(QStringLiteral("space")).toString().isEmpty(),
                  "The note was stored without a space.");
        }
        // Titles are written so a restored tab can be labelled before it loads.
        const bool titlesRecorded = std::all_of(savedTabs.begin(), savedTabs.end(), [](const QJsonValue &value) {
            return !value.toObject().value(QStringLiteral("title")).toString().isEmpty();
        });
        check(titlesRecorded, "The session file did not record tab titles.");

        // A favicon and a title are injected into the saved record; the second
        // run must show both before the pages have loaded again.
        QJsonArray patched;
        QIcon icon;
        {
            QPixmap pixmap(16, 16);
            pixmap.fill(Qt::red);
            icon = QIcon(pixmap);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            check(buffer.open(QIODevice::WriteOnly), "Could not encode the fixture icon.");
            check(pixmap.save(&buffer, "PNG"), "Could not save the fixture icon.");
            for (int index = 0; index < savedTabs.size(); ++index) {
                QJsonObject record = savedTabs.at(index).toObject();
                record.insert(QStringLiteral("favicon"), QString::fromLatin1(bytes.toBase64()));
                record.insert(QStringLiteral("title"), QStringLiteral("Gemerkter Titel %1").arg(index));
                patched.append(record);
            }
        }
        saved.insert(QStringLiteral("tabs"), patched);
        saved.insert(QStringLiteral("splitTab"), 0);
        {
            std::ofstream output(sessionFile, std::ios::binary | std::ios::trunc);
            const QByteArray bytes = QJsonDocument(saved).toJson(QJsonDocument::Indented);
            output.write(bytes.constData(), bytes.size());
            check(output.good(), "Could not rewrite the session fixture.");
        }

        // Second run: everything must come back from the file alone.
        {
            PreviewApp second;
            pump(900);
            auto *tabs = second.tabs();
            auto *split = second.splitTabs();
            check(tabs != nullptr && split != nullptr, "The tab hosts are missing after restore.");
            check(tabs->count() + split->count() == 2, "The restored run lost a tab.");
            check(split->count() == 1, "The stored split partner was not restored into the split host.");
            check(split->isVisible(), "Split view was not re-enabled after restore.");

            // The note comes back with its title and its formatting.
            auto *restoredNote = second.window->findChild<yobro::spike::NoteEditor *>();
            check(restoredNote != nullptr, "The note was not restored.");
            check(restoredNote->title() == QStringLiteral("Einkaufsliste"),
                  "The restored note lost its title.");
            check(restoredNote->plainText().contains(QStringLiteral("Milch")),
                  "The restored note lost its content.");
            check(restoredNote->html().contains(QStringLiteral("font-weight"))
                      || restoredNote->html().contains(QStringLiteral("<b")),
                  "The restored note lost its formatting.");
            check(restoredNote->displayTitle() == QStringLiteral("Einkaufsliste"),
                  "The restored note shows the wrong label.");

            const std::vector<QTreeWidgetItem *> rows = sidebarTabRows(*second.window);
            const bool noteRowPresent = std::any_of(rows.begin(), rows.end(), [](QTreeWidgetItem *row) {
                return row->text(0).contains(QStringLiteral("Einkaufsliste"));
            });
            check(noteRowPresent, "The restored note is missing from the sidebar.");
            std::vector<QTreeWidgetItem *> pageRows;
            for (QTreeWidgetItem *row : rows) {
                if (!row->text(0).contains(QStringLiteral("Einkaufsliste"))) pageRows.push_back(row);
            }
            const std::vector<QTreeWidgetItem *> &pagesOnly = pageRows;
            check(pagesOnly.size() == 2, "The sidebar did not list both restored tabs.");
            // The persisted icon is applied even though these hosts never load,
            // so no live favicon can arrive to mask a missing restore.
            const bool restoredIcon = std::all_of(pagesOnly.begin(), pagesOnly.end(), [](QTreeWidgetItem *row) {
                return !row->icon(0).isNull();
            });
            check(restoredIcon, "Restored tabs did not use the persisted favicon.");
            const bool neutralMarkerGone = std::none_of(pagesOnly.begin(), pagesOnly.end(), [](QTreeWidgetItem *row) {
                return row->text(0).startsWith(QStringLiteral("◎"));
            });
            check(neutralMarkerGone, "A restored tab kept the placeholder marker despite having an icon.");

            auto *sidebar = second.window->findChild<QWidget *>(QStringLiteral("workspaceSidebar"));
            check(sidebar != nullptr && !sidebar->isVisible(),
                  "The persisted collapsed sidebar was not restored.");
            // Collapsing must fall back to the icon column, not to nothing.
            auto *compact = second.window->findChild<QWidget *>(QStringLiteral("compactSidebar"));
            check(compact != nullptr && compact->isVisible(),
                  "The collapsed sidebar did not come back as the icon column.");
            auto *compactTabs = second.window->findChild<QListWidget *>(QStringLiteral("compactTabs"));
            check(compactTabs != nullptr && compactTabs->count() == 2,
                  "The icon column did not list the restored tabs.");
            for (const char *name : {"compactExpandButton", "compactAddressButton", "compactSpaceButton",
                                     "compactNewTabButton", "compactHistoryButton",
                                     "compactDownloadsButton", "compactProfileButton"}) {
                check(second.window->findChild<QPushButton *>(QString::fromLatin1(name)) != nullptr,
                      "The icon column is missing one of its controls.");
            }

            // The menu bar is the single source for commands and keys, so the
            // WebKit key assignments are asserted through it.
            const auto shortcutOf = [&second](const char *name) {
                auto *action = second.window->findChild<QAction *>(QString::fromLatin1(name));
                check(action != nullptr, "A menu command is missing.");
                return action->shortcut();
            };
            check(shortcutOf("bookmarkPageAction") == QKeySequence(Qt::CTRL | Qt::Key_D),
                  "⌘D must bookmark the page, matching the WebKit build.");
            check(shortcutOf("newPrivateTabAction") == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
                  "⇧⌘T must open a private tab, matching the WebKit build.");
            check(shortcutOf("reopenClosedTabMenuAction").isEmpty(),
                  "Reopening a closed tab must stay without a key so ⇧⌘T is free.");
            check(shortcutOf("duplicateTabMenuAction").isEmpty(),
                  "Duplicating must not occupy ⌘D.");
            check(shortcutOf("printPageAction") == QKeySequence(Qt::CTRL | Qt::Key_P),
                  "⌘P must print.");
            check(shortcutOf("zoomInAction") == QKeySequence(Qt::CTRL | Qt::Key_Plus)
                  && shortcutOf("zoomOutAction") == QKeySequence(Qt::CTRL | Qt::Key_Minus)
                  && shortcutOf("zoomResetAction") == QKeySequence(Qt::CTRL | Qt::Key_0),
                  "The zoom commands are not bound as in the WebKit build.");
            check(shortcutOf("bookmarksAction") == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B),
                  "⌥⌘B must open the bookmarks.");
            check(shortcutOf("backAction") == QKeySequence(Qt::CTRL | Qt::Key_BracketLeft)
                  && shortcutOf("forwardAction") == QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
                  "⌘[ and ⌘] must navigate the history.");
            check(shortcutOf("selectTab1Action") == QKeySequence(Qt::CTRL | Qt::Key_1)
                  && shortcutOf("selectTab9Action") == QKeySequence(Qt::CTRL | Qt::Key_9),
                  "⌘1 to ⌘9 must select tabs.");

            // Two commands sharing one key would make it ambiguous at runtime.
            std::vector<QKeySequence> keys;
            for (QAction *action : second.window->actions()) {
                if (action->shortcut().isEmpty()) continue;
                keys.push_back(action->shortcut());
            }
            const std::size_t before = keys.size();
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            check(keys.size() == before, "Two commands are bound to the same key.");
        }
    } catch (const std::exception &error) {
        std::cerr << "QT SESSION RESTORE FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "QT SESSION RESTORE PASS\n";
    return 0;
}
