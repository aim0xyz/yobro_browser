#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "spike/SyncAccount.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTest>

#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using yobro::spike::AuthClient;
using yobro::spike::EncryptedSyncPayload;
using yobro::spike::SupabaseAuthSession;
using yobro::spike::SyncCipher;
using yobro::spike::SyncRecord;
using yobro::spike::SyncService;
using yobro::spike::SyncSnapshot;
using yobro::spike::SyncTab;
using yobro::spike::SyncTabResolution;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

/// One browser window on its own profile.
struct Instance {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    explicit Instance(const char *id)
        : paths(yobro::core::ProfilePaths::forProfile(id)), policy(paths.bridgePolicy) {
        paths.createDirectories();
        // Only these named files are removed, never a directory.
        for (const char *name : {"sync.json", "bookmarks.json", "history.json", "onboarding.json"})
            QFile::remove(QString::fromStdString((paths.profile / name).string()));
        QFile::remove(QString::fromStdString(paths.session.string()));

        profile = engine.openProfile({
            .id = id,
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create a sync test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = id,
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
    }

    ~Instance() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

/// One shared backend for both windows, in memory.
class FakeBackend final : public SyncService {
public:
    std::optional<SyncRecord> stored;
    QString problem;

    void fetch(
        const QString &,
        std::function<void(QString, std::optional<SyncRecord>)> done
    ) override {
        done(problem, problem.isEmpty() ? stored : std::nullopt);
    }

    void push(
        const QString &profileId,
        qint64 expectedRevision,
        const EncryptedSyncPayload &payload,
        std::function<void(QString, std::optional<SyncRecord>)> done
    ) override {
        if (!problem.isEmpty()) {
            done(problem, std::nullopt);
            return;
        }
        const qint64 current = stored ? stored->revision : -1;
        if (expectedRevision != current) {
            done(QStringLiteral("revision conflict"), std::nullopt);
            return;
        }
        SyncRecord record;
        record.profileId = profileId;
        record.revision = current + 1;
        record.payload = payload;
        stored = record;
        done({}, record);
    }
};

class FakeAuth final : public AuthClient {
public:
    int signOuts = 0;

    void signIn(const QString &email, const QString &, Answer done) override { answer(email, done); }
    void signUp(const QString &email, const QString &, Answer done) override { answer(email, done); }
    void signOut(const QString &) override { ++signOuts; }

private:
    static void answer(const QString &email, const Answer &done) {
        SupabaseAuthSession session;
        session.accessToken = QStringLiteral("token");
        session.userId = QStringLiteral("user-1");
        session.email = email;
        done({}, session);
    }
};

template <typename Widget>
Widget *find(const QObject &root, const QString &name) {
    Widget *found = root.findChild<Widget *>(name);
    check(found != nullptr, "The settings are missing " + name.toStdString() + ".");
    return found;
}

void waitFor(const std::function<bool()> &ready, const std::string &what) {
    for (int attempt = 0; attempt < 200 && !ready(); ++attempt) QTest::qWait(25);
    check(ready(), what);
}

void checkSnapshotContents(Instance &first) {
    SpikeWindow &window = *first.window;
    // A bookmark, a note and two pages, one of them private.
    check(first.library->addBookmark("Beispiel", "https://example.com/", "Reisen"),
          "The fixture bookmark was not stored.");
    window.findChild<QPushButton *>(QStringLiteral("sidebarNewNoteButton"))->click();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("browserTabs"));
    check(tabs != nullptr, "The tab bar is missing.");
    window.findChild<QPushButton *>(QStringLiteral("sidebarPrivateTabButton"))->click();
    QTest::qWait(100);

    const SyncSnapshot snapshot = window.buildSyncSnapshot();
    check(!snapshot.spaces.isEmpty(), "The snapshot has no spaces.");
    check(snapshot.spaces.contains(snapshot.currentSpace),
          "The snapshot's current space is not in its space list.");
    check(snapshot.validationProblem().isEmpty(),
          "The window produced an invalid snapshot: " + snapshot.validationProblem().toStdString());

    bool sawBookmark = false;
    for (const auto &bookmark : snapshot.bookmarks)
        if (bookmark.url == QStringLiteral("https://example.com/")) sawBookmark = true;
    check(sawBookmark, "The bookmark is missing from the snapshot.");

    bool sawNote = false;
    for (const SyncTab &tab : snapshot.tabs)
        if (tab.note) sawNote = true;
    check(sawNote, "The note is missing from the snapshot.");

    // Nothing from a private tab may travel, not even its address.
    for (const SyncTab &tab : snapshot.tabs) {
        check(!tab.url.startsWith(QStringLiteral("about:")),
              "A tab without a web address ended up in the snapshot.");
    }
    // Neither may a password, a cookie or a mail account: the snapshot has no
    // place for them at all, which is checked by looking at what it serialises.
    const QString encoded = QString::fromUtf8(
        QJsonDocument(snapshot.toJson()).toJson(QJsonDocument::Compact));
    for (const QString &forbidden : {QStringLiteral("password"), QStringLiteral("cookie"),
                                     QStringLiteral("favicon"), QStringLiteral("mail")}) {
        check(!encoded.contains(forbidden, Qt::CaseInsensitive),
              "The snapshot carries a field it must not: " + forbidden.toStdString());
    }
}

QString connectAndPush(Instance &first, FakeBackend &backend, FakeAuth &auth) {
    SpikeWindow &window = *first.window;
    auto *controller = window.syncController();
    check(controller != nullptr, "The window has no sync controller.");
    controller->setService(&backend);
    controller->setAuthClient(&auth);

    QString reported = QStringLiteral("not called");
    controller->signIn(QStringLiteral("a@b.de"), QStringLiteral("password"), {},
                       [&reported](QString problem) { reported = problem; });
    check(reported.isEmpty(), "Signing in failed: " + reported.toStdString());
    check(controller->signedIn(), "The window did not sign in.");
    const QString code = controller->recoveryCode();
    check(!code.isEmpty(), "There is no recovery code.");

    reported = QStringLiteral("not called");
    controller->syncNow([&reported](QString problem) { reported = problem; });
    waitFor([&reported] { return reported != QStringLiteral("not called"); }, "The first sync never ended.");
    check(reported.isEmpty(), "The first sync failed: " + reported.toStdString());
    check(backend.stored.has_value(), "The first sync stored nothing.");
    return code;
}

void checkSecondDeviceAdopts(FakeBackend &backend, FakeAuth &auth, const QString &code) {
    Instance second("sync-b");
    SpikeWindow &window = *second.window;
    auto *controller = window.syncController();
    check(controller != nullptr, "The second window has no sync controller.");
    controller->setService(&backend);
    controller->setAuthClient(&auth);
    // A second device is keyed by its recovery code, not by a shared Keychain.
    check(controller->profileId() != QStringLiteral(""), "The second window has no profile id.");

    QString reported = QStringLiteral("not called");
    controller->signIn(QStringLiteral("a@b.de"), QStringLiteral("password"), code,
                       [&reported](QString problem) { reported = problem; });
    check(reported.isEmpty(), "The second device could not sign in: " + reported.toStdString());

    // The row belongs to the first profile, so this device starts empty and the
    // data has to be handed over deliberately.
    const std::optional<SyncSnapshot> remote = SyncCipher::open(
        backend.stored->payload, SyncCipher::decodeKey(code).value());
    check(remote.has_value(), "The stored payload did not open with the recovery code.");
    check(!remote->bookmarks.empty(), "The stored snapshot has no bookmarks.");

    const QString problem = window.applySyncSnapshot(*remote, SyncTabResolution::adoptRemote);
    check(problem.isEmpty(), "Applying the snapshot failed: " + problem.toStdString());

    const SyncSnapshot local = window.buildSyncSnapshot();
    bool sawBookmark = false;
    for (const auto &bookmark : local.bookmarks)
        if (bookmark.url == QStringLiteral("https://example.com/")) sawBookmark = true;
    check(sawBookmark, "The bookmark did not arrive on the second device.");
    bool sawNote = false;
    for (const SyncTab &tab : local.tabs)
        if (tab.note) sawNote = true;
    check(sawNote, "The note did not arrive on the second device.");

    // Applying the same snapshot twice must not duplicate anything.
    const std::size_t bookmarksBefore = local.bookmarks.size();
    const std::size_t tabsBefore = local.tabs.size();
    check(window.applySyncSnapshot(*remote, SyncTabResolution::adoptRemote).isEmpty(),
          "Applying the snapshot a second time failed.");
    const SyncSnapshot again = window.buildSyncSnapshot();
    check(again.bookmarks.size() == bookmarksBefore, "Applying twice duplicated bookmarks.");
    check(again.tabs.size() == tabsBefore, "Applying twice duplicated tabs.");

    // Invalid data is refused rather than written.
    SyncSnapshot broken = *remote;
    broken.spaces.clear();
    check(!window.applySyncSnapshot(broken, SyncTabResolution::adoptRemote).isEmpty(),
          "An invalid snapshot was applied.");
}

void checkSettingsSurface(Instance &first, FakeBackend &backend, FakeAuth &auth) {
    SpikeWindow &window = *first.window;
    window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
    for (const QString &name : {
             QStringLiteral("syncEmailField"), QStringLiteral("syncPasswordField"),
             QStringLiteral("syncRecoveryField"), QStringLiteral("syncSignInButton"),
             QStringLiteral("syncSignUpButton"), QStringLiteral("syncNowButton"),
             QStringLiteral("syncSignOutButton"), QStringLiteral("syncStatusLabel"),
             QStringLiteral("syncRecoveryLabel"),
         }) {
        check(window.findChild<QWidget *>(name) != nullptr,
              "The settings are missing " + name.toStdString() + ".");
    }
    // The password must never be readable in the window.
    check(find<QLineEdit>(window, QStringLiteral("syncPasswordField"))->echoMode()
              == QLineEdit::Password,
          "The account password field shows the password.");

    auto *controller = window.syncController();
    // Signed in from the earlier step, so the recovery code is on display.
    check(find<QLabel>(window, QStringLiteral("syncRecoveryLabel"))->text()
              .contains(controller->recoveryCode()),
          "The recovery code is not shown while signed in.");
    check(find<QPushButton>(window, QStringLiteral("syncNowButton"))->isEnabled(),
          "Syncing is not offered while signed in.");
    check(!find<QPushButton>(window, QStringLiteral("syncSignInButton"))->isEnabled(),
          "Signing in is offered although the window is signed in.");

    find<QPushButton>(window, QStringLiteral("syncNowButton"))->click();
    waitFor([controller] { return !controller->busy(); }, "The sync from the settings never ended.");
    check(!find<QLabel>(window, QStringLiteral("syncStatusLabel"))->text().isEmpty(),
          "The account section says nothing after a sync.");

    find<QPushButton>(window, QStringLiteral("syncSignOutButton"))->click();
    check(!controller->signedIn(), "Signing out from the settings did not work.");
    check(auth.signOuts >= 1, "The account service was not told about the sign-out.");
    check(find<QLabel>(window, QStringLiteral("syncRecoveryLabel"))->text().isEmpty(),
          "The recovery code is still shown after signing out.");
    check(!find<QPushButton>(window, QStringLiteral("syncNowButton"))->isEnabled(),
          "Syncing is offered after signing out.");

    // A backend that fails says so and changes nothing.
    backend.problem = QStringLiteral("Server nicht erreichbar.");
    QString reported = QStringLiteral("not called");
    controller->signIn(QStringLiteral("a@b.de"), QStringLiteral("password"), {},
                       [&reported](QString problem) { reported = problem; });
    check(reported.isEmpty(), "Signing in again failed.");
    reported = QStringLiteral("not called");
    controller->syncNow([&reported](QString problem) { reported = problem; });
    waitFor([&reported] { return reported != QStringLiteral("not called"); },
            "The failing sync never ended.");
    check(reported == QStringLiteral("Server nicht erreichbar."), "A backend failure was not reported.");
    backend.problem.clear();
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    try {
        FakeBackend backend;
        FakeAuth auth;
        Instance first("sync-a");
        checkSnapshotContents(first);
        const QString code = connectAndPush(first, backend, auth);
        checkSecondDeviceAdopts(backend, auth, code);
        checkSettingsSurface(first, backend, auth);
    } catch (const std::exception &error) {
        std::cerr << "SYNC UI FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "SYNC UI PASS\n";
    return 0;
}
