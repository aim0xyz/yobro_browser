#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/OnboardingDialog.hpp"
#include "spike/OnboardingProgress.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using yobro::spike::ImportedBrowserData;
using yobro::spike::ImportedLink;
using yobro::spike::ImportProfileEntry;
using yobro::spike::OnboardingDialog;
using yobro::spike::OnboardingProgress;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct App {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    App() : paths(yobro::core::ProfilePaths::forProfile("onboarding")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        // Only these named files are removed, never a directory.
        for (const char *name : {"onboarding.json", "bookmarks.json", "history.json"})
            QFile::remove(QString::fromStdString((paths.profile / name).string()));
        QFile::remove(QString::fromStdString(paths.session.string()));

        profile = engine.openProfile({
            .id = "onboarding",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the onboarding test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "onboarding",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
    }

    ~App() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

template <typename Widget>
Widget *find(const QObject &root, const QString &name) {
    Widget *found = root.findChild<Widget *>(name);
    check(found != nullptr, "The setup is missing " + name.toStdString() + ".");
    return found;
}

// MARK: - The marker file

void checkProgressFile(const QString &root) {
    const std::filesystem::path directory = (root + QStringLiteral("/progress")).toStdString();
    // A profile that was never set up must not count as finished.
    check(!OnboardingProgress::isComplete(directory), "A missing marker counted as finished.");
    check(OnboardingProgress::finish(directory).isEmpty(), "The marker could not be written.");
    check(OnboardingProgress::isComplete(directory), "The written marker was not read back.");

    // The same file and key the WebKit build uses.
    QFile file(QString::fromStdString((directory / "onboarding.json").string()));
    check(file.open(QIODevice::ReadOnly), "The marker file is not called onboarding.json.");
    check(QString::fromUtf8(file.readAll()).contains(QStringLiteral("\"completed\":true")),
          "The marker does not carry the completed flag.");
    file.close();

    check(OnboardingProgress::reset(directory).isEmpty(), "The marker could not be reset.");
    check(!OnboardingProgress::isComplete(directory), "A reset marker still counted as finished.");
    check(OnboardingProgress::reset(directory).isEmpty(), "Resetting an absent marker failed.");

    // Anything unreadable counts as unfinished: asking once too often is
    // harmless, skipping the setup on a fresh profile is not.
    check(QDir().mkpath(root + QStringLiteral("/damaged")), "Could not create the damaged directory.");
    const std::filesystem::path damaged = (root + QStringLiteral("/damaged")).toStdString();
    for (const QByteArray &content : {
             QByteArrayLiteral("not json"),
             QByteArrayLiteral("[]"),
             QByteArrayLiteral("{}"),
             QByteArrayLiteral("{\"completed\":false}"),
             QByteArrayLiteral("{\"completed\":\"yes\"}"),
         }) {
        QFile broken(QString::fromStdString((damaged / "onboarding.json").string()));
        check(broken.open(QIODevice::WriteOnly), "Could not write the damaged marker.");
        broken.write(content);
        broken.close();
        check(!OnboardingProgress::isComplete(damaged),
              "An unusable marker counted as finished: " + std::string(content.constData()));
    }
}

// MARK: - The dialog

/// Stands in for the browsers on this Mac.
struct FakeSource {
    int reads = 0;
    QStringList requestedKinds;
    ImportedBrowserData answer;
    std::vector<ImportProfileEntry> profiles{
        {QStringLiteral("chrome-default"), QStringLiteral("Chrome"), QStringLiteral("Arbeit"),
         QStringLiteral("/fake/Chrome/Default")},
    };
    QString findProblem;

    FakeSource() {
        answer.bookmarks.push_back({QStringLiteral("Beispiel"), QStringLiteral("https://example.com/"),
                                    QStringLiteral("Lesezeichen"), 0, 1, false});
        answer.history.push_back({QStringLiteral("Alt"), QStringLiteral("https://example.org/"),
                                  {}, 0, 3, false});
        answer.availability.insert(QStringLiteral("cookies"),
                                   QStringLiteral("Verschlüsselt, Exportdatei nötig."));
    }
};

std::unique_ptr<OnboardingDialog> makeDialog(
    SpikeWindow &window,
    const QString &directory,
    FakeSource &source,
    QStringList *written
) {
    check(QDir().mkpath(directory), "Could not create the dialog directory.");
    auto dialog = std::make_unique<OnboardingDialog>(
        directory.toStdString(),
        [&window, written](const ImportedBrowserData &data, QString *problem) {
            const QStringList summary = window.applyImportedBrowserData(data, problem);
            if (written) *written = summary;
            return summary;
        },
        nullptr
    );
    dialog->setProfileFinder([&source](QString *problem) {
        if (problem) *problem = source.findProblem;
        return source.profiles;
    });
    dialog->setProfileReader([&source](const QString &, const QString &, const QStringList &kinds) {
        ++source.reads;
        source.requestedKinds = kinds;
        return source.answer;
    });
    return dialog;
}

void checkStepFlow(SpikeWindow &window, const QString &root) {
    FakeSource source;
    auto dialog = makeDialog(window, root + QStringLiteral("/flow"), source, nullptr);

    for (const QString &name : {
             QStringLiteral("onboardingHeading"), QStringLiteral("onboardingStepLabel"),
             QStringLiteral("onboardingPages"), QStringLiteral("onboardingSourceList"),
             QStringLiteral("onboardingProfilePicker"), QStringLiteral("onboardingFindButton"),
             QStringLiteral("onboardingPreview"), QStringLiteral("onboardingPreviewButton"),
             QStringLiteral("onboardingImportButton"), QStringLiteral("onboardingResult"),
             QStringLiteral("onboardingBackButton"), QStringLiteral("onboardingSkipButton"),
             QStringLiteral("onboardingTransferButton"), QStringLiteral("onboardingMoreButton"),
             QStringLiteral("onboardingStartButton"),
         }) {
        check(dialog->findChild<QWidget *>(name) != nullptr,
              "The setup is missing " + name.toStdString() + ".");
    }
    // The five browsers the welcome screen names.
    const QString sources = find<QLabel>(*dialog, QStringLiteral("onboardingSourceList"))->text();
    for (const QString &name : {QStringLiteral("Safari"), QStringLiteral("Arc"), QStringLiteral("Brave"),
                                QStringLiteral("Chrome"), QStringLiteral("Firefox")}) {
        check(sources.contains(name), "The welcome screen does not name " + name.toStdString() + ".");
    }

    auto *counter = find<QLabel>(*dialog, QStringLiteral("onboardingStepLabel"));
    auto *back = find<QPushButton>(*dialog, QStringLiteral("onboardingBackButton"));
    auto *skip = find<QPushButton>(*dialog, QStringLiteral("onboardingSkipButton"));
    auto *transfer = find<QPushButton>(*dialog, QStringLiteral("onboardingTransferButton"));
    auto *more = find<QPushButton>(*dialog, QStringLiteral("onboardingMoreButton"));
    auto *start = find<QPushButton>(*dialog, QStringLiteral("onboardingStartButton"));

    check(dialog->step() == 0, "The setup does not start on the first step.");
    check(counter->text() == QStringLiteral("1 / 3"), "The step counter is wrong on step one.");
    // Only the last step may finish the setup.
    check(!start->isVisibleTo(dialog.get()), "The finish button is offered on the first step.");
    check(!back->isVisibleTo(dialog.get()), "A back button is offered on the first step.");
    check(transfer->isVisibleTo(dialog.get()), "The transfer button is missing on the first step.");

    transfer->click();
    check(dialog->step() == 1, "The transfer button did not move to step two.");
    check(counter->text() == QStringLiteral("2 / 3"), "The step counter is wrong on step two.");
    check(back->isVisibleTo(dialog.get()), "There is no way back from step two.");
    // Entering the step looks for profiles by itself.
    check(find<QComboBox>(*dialog, QStringLiteral("onboardingProfilePicker"))->count() == 1,
          "The profiles were not looked up when the step opened.");

    back->click();
    check(dialog->step() == 0, "Going back did not work.");

    // Skipping lands on the last step and says the import can be done later.
    skip->click();
    check(dialog->step() == 2, "Skipping did not move to the last step.");
    check(counter->text() == QStringLiteral("3 / 3"), "The step counter is wrong on the last step.");
    check(find<QLabel>(*dialog, QStringLiteral("onboardingResult"))->text()
              .contains(QStringLiteral("Einstellungen")),
          "A skipped setup does not point at the settings.");
    check(more->isVisibleTo(dialog.get()), "The last step offers no way back into the import.");
    check(start->isVisibleTo(dialog.get()), "The last step has no finish button.");

    more->click();
    check(dialog->step() == 1, "The last step could not return to the import.");
}

void checkTransfer(SpikeWindow &window, const QString &root) {
    FakeSource source;
    QStringList written;
    auto dialog = makeDialog(window, root + QStringLiteral("/transfer"), source, &written);
    find<QPushButton>(*dialog, QStringLiteral("onboardingTransferButton"))->click();

    // Cookies start off: they carry live sessions and can sign the user out of
    // the browser they came from.
    check(!find<QCheckBox>(*dialog, QStringLiteral("onboardingKind-cookies"))->isChecked(),
          "Cookies are transferred by default.");
    for (const QString &kind : {QStringLiteral("bookmarks"), QStringLiteral("history"),
                                QStringLiteral("tabs")}) {
        check(find<QCheckBox>(*dialog, QStringLiteral("onboardingKind-") + kind)->isChecked(),
              "The setup does not offer " + kind.toStdString() + " by default.");
    }

    auto *importButton = find<QPushButton>(*dialog, QStringLiteral("onboardingImportButton"));
    check(!importButton->isEnabled(), "Data could be transferred before a preview.");

    find<QPushButton>(*dialog, QStringLiteral("onboardingPreviewButton"))->click();
    check(source.reads == 1, "The preview did not read the profile.");
    check(!source.requestedKinds.contains(QStringLiteral("cookies")),
          "The preview asked for cookies although they are switched off.");
    const QString preview = find<QLabel>(*dialog, QStringLiteral("onboardingPreview"))->text();
    check(preview.contains(QStringLiteral("1")), "The preview shows no counts.");
    // The preview must say plainly that nothing has been written yet.
    check(preview.contains(QStringLiteral("Die Quelle bleibt unverändert")),
          "The preview does not say that nothing was written yet.");
    // A note only belongs to a kind that was actually asked for.
    check(!preview.contains(QStringLiteral("Exportdatei")),
          "A note about a kind that was not requested showed up.");
    check(importButton->isEnabled(), "A successful preview did not offer the transfer.");

    // With cookies switched on, the source's explanation has to be passed on.
    auto *cookies = find<QCheckBox>(*dialog, QStringLiteral("onboardingKind-cookies"));
    cookies->setChecked(true);
    find<QPushButton>(*dialog, QStringLiteral("onboardingPreviewButton"))->click();
    check(source.requestedKinds.contains(QStringLiteral("cookies")),
          "Cookies were not requested although they are switched on.");
    check(find<QLabel>(*dialog, QStringLiteral("onboardingPreview"))->text()
              .contains(QStringLiteral("Exportdatei")),
          "The note about encrypted cookies was dropped.");
    cookies->setChecked(false);
    find<QPushButton>(*dialog, QStringLiteral("onboardingPreviewButton"))->click();

    importButton->click();
    check(dialog->step() == 2, "A finished transfer did not move to the last step.");
    check(!written.isEmpty(), "The transfer wrote nothing.");
    check(find<QLabel>(*dialog, QStringLiteral("onboardingResult"))->text()
              == written.join(QStringLiteral("\n")),
          "The last step does not show what was transferred.");
    // A second transfer would duplicate everything, so the button is spent.
    check(!importButton->isEnabled(), "The transfer could be repeated with the same preview.");

    // With no data kind chosen there is nothing to preview.
    find<QPushButton>(*dialog, QStringLiteral("onboardingMoreButton"))->click();
    for (const QString &kind : {QStringLiteral("bookmarks"), QStringLiteral("history"),
                                QStringLiteral("tabs")}) {
        find<QCheckBox>(*dialog, QStringLiteral("onboardingKind-") + kind)->setChecked(false);
    }
    const int readsBefore = source.reads;
    find<QPushButton>(*dialog, QStringLiteral("onboardingPreviewButton"))->click();
    check(source.reads == readsBefore, "An empty selection still read the profile.");
    check(!importButton->isEnabled(), "An empty selection offered a transfer.");

    // A source that cannot be read is reported, not swallowed.
    find<QCheckBox>(*dialog, QStringLiteral("onboardingKind-bookmarks"))->setChecked(true);
    source.answer.problem = QStringLiteral("Profil ist gesperrt.");
    find<QPushButton>(*dialog, QStringLiteral("onboardingPreviewButton"))->click();
    check(find<QLabel>(*dialog, QStringLiteral("onboardingPreview"))->text()
              == QStringLiteral("Profil ist gesperrt."),
          "A read failure was not reported.");
    check(!importButton->isEnabled(), "A failed read still offered a transfer.");
}

void checkNoProfiles(SpikeWindow &window, const QString &root) {
    FakeSource source;
    source.profiles.clear();
    auto dialog = makeDialog(window, root + QStringLiteral("/empty"), source, nullptr);
    find<QPushButton>(*dialog, QStringLiteral("onboardingTransferButton"))->click();
    check(find<QLabel>(*dialog, QStringLiteral("onboardingPreview"))->text()
              .contains(QStringLiteral("Keine Profile")),
          "An empty result was not explained.");
    check(!find<QPushButton>(*dialog, QStringLiteral("onboardingImportButton"))->isEnabled(),
          "A transfer was offered without a profile.");

    // A lookup that failed says why.
    FakeSource failing;
    failing.findProblem = QStringLiteral("Python fehlt.");
    auto second = makeDialog(window, root + QStringLiteral("/failing"), failing, nullptr);
    find<QPushButton>(*second, QStringLiteral("onboardingFindButton"))->click();
    check(find<QLabel>(*second, QStringLiteral("onboardingPreview"))->text()
              == QStringLiteral("Python fehlt."),
          "A failed lookup was not reported.");
}

void checkFinishing(SpikeWindow &window, const QString &root) {
    const QString directory = root + QStringLiteral("/finish");
    FakeSource source;
    auto dialog = makeDialog(window, directory, source, nullptr);
    int completed = 0;
    QObject::connect(dialog.get(), &OnboardingDialog::completed, [&completed] { ++completed; });

    find<QPushButton>(*dialog, QStringLiteral("onboardingSkipButton"))->click();
    check(!OnboardingProgress::isComplete(directory.toStdString()),
          "Skipping already counted as finished.");
    find<QPushButton>(*dialog, QStringLiteral("onboardingStartButton"))->click();
    check(completed == 1, "Finishing was not reported exactly once.");
    check(OnboardingProgress::isComplete(directory.toStdString()),
          "Finishing was not written to the profile.");
}

void checkWindowShowsSetupOnce(SpikeWindow &window, const std::filesystem::path &profile) {
    // The profile has no marker yet, so the window offers the setup.
    check(window.findChild<OnboardingDialog *>(QStringLiteral("onboardingDialog")) == nullptr,
          "The setup was already open.");
    check(window.showOnboardingIfNeeded(), "The setup was not offered on a fresh profile.");
    auto *dialog = window.findChild<OnboardingDialog *>(QStringLiteral("onboardingDialog"));
    check(dialog != nullptr, "The setup did not open.");

    // Once per window, even before it was finished, so it cannot keep popping up.
    check(!window.showOnboardingIfNeeded(), "The setup was offered a second time.");

    find<QPushButton>(*dialog, QStringLiteral("onboardingSkipButton"))->click();
    find<QPushButton>(*dialog, QStringLiteral("onboardingStartButton"))->click();
    QTest::qWait(50);
    check(OnboardingProgress::isComplete(profile),
          "The window's setup did not record itself as finished.");

    // The settings offer it again afterwards.
    check(window.findChild<QPushButton *>(QStringLiteral("reopenOnboardingButton")) == nullptr,
          "The settings were already open.");
    window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
    auto *reopen = window.findChild<QPushButton *>(QStringLiteral("reopenOnboardingButton"));
    check(reopen != nullptr, "The settings offer no way back to the setup.");
    reopen->click();
    check(window.findChild<OnboardingDialog *>(QStringLiteral("onboardingDialog")) != nullptr,
          "The settings could not reopen the setup.");
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        checkProgressFile(work.path());
        App app;
        SpikeWindow &window = *app.window;
        checkStepFlow(window, work.path());
        checkTransfer(window, work.path());
        checkNoProfiles(window, work.path());
        checkFinishing(window, work.path());
        checkWindowShowsSetupOnce(window, app.paths.profile);
    } catch (const std::exception &error) {
        std::cerr << "ONBOARDING FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ONBOARDING PASS\n";
    return 0;
}
