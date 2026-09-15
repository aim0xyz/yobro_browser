#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMessageBox>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;

const QByteArray mediaBody = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Permission Fixture</title>
<style>
html, body { margin: 0; width: 100%; height: 100%; background: #fff; }
button { position: fixed; left: 40px; width: 240px; height: 50px; font: 16px sans-serif; }
#audio { top: 20px; } #video { top: 80px; } #both { top: 140px; }
#geo { top: 200px; } #notify { top: 260px; }
#status { position: fixed; left: 40px; top: 330px; font: 18px monospace; }
</style>
<button id="audio" type="button">Request microphone</button>
<button id="video" type="button">Request camera</button>
<button id="both" type="button">Request both</button>
<button id="geo" type="button">Request geolocation</button>
<button id="notify" type="button">Request notifications</button>
<div id="status">ready</div>
<script>
const status = document.querySelector("#status");
async function requestMedia(constraints) {
  status.textContent = "pending";
  try {
    const stream = await navigator.mediaDevices.getUserMedia(constraints);
    const kinds = stream.getTracks().map(track => track.kind).sort().join("+");
    stream.getTracks().forEach(track => track.stop());
    status.textContent = "granted:" + kinds;
  } catch (error) {
    status.textContent = "denied:" + (error && error.name ? error.name : String(error));
  }
}
document.querySelector("#audio").addEventListener("click", () => requestMedia({audio: true}));
document.querySelector("#video").addEventListener("click", () => requestMedia({video: true}));
document.querySelector("#both").addEventListener("click", () => requestMedia({audio: true, video: true}));
document.querySelector("#geo").addEventListener("click", () => {
  status.textContent = "geo-pending";
  navigator.geolocation.getCurrentPosition(
    () => { status.textContent = "geo-granted"; },
    error => { status.textContent = "geo-denied:" + error.code; },
    {timeout: 2000}
  );
});
document.querySelector("#notify").addEventListener("click", async () => {
  status.textContent = "notify-pending";
  try {
    status.textContent = "notify:" + await Notification.requestPermission();
  } catch (error) {
    status.textContent = "notify-failed:" + (error && error.name ? error.name : String(error));
  }
});
</script>)HTML");

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void check(bool condition, std::string_view message) {
    if (!condition)
        fail(std::string(message));
}

template<typename Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout, std::string_view expectation) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (predicate())
            return;
        QTest::qWait(10);
    }
    fail("Timed out waiting for " + std::string(expectation) + ".");
}

void sendBody(QTcpSocket *socket, const QByteArray &body) {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: "
    );
    response += QByteArray::number(body.size());
    response += QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("responded").toBool()) {
                    (void)socket->readAll();
                    return;
                }
                QByteArray request = socket->property("request").toByteArray();
                request.append(socket->readAll());
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n"))
                    return;
                socket->setProperty("responded", true);
                if (request.startsWith("GET /auth ")) {
                    if (request.toLower().contains("authorization: basic dxnlcjpwyxnz")) {
                        sendBody(socket, QByteArrayLiteral("<title>Authenticated Fixture</title><p>signed in</p>"));
                    } else {
                        socket->write(QByteArrayLiteral(
                            "HTTP/1.1 401 Unauthorized\r\n"
                            "WWW-Authenticate: Basic realm=\"YOBRO Test\"\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n"
                        ));
                        socket->disconnectFromHost();
                    }
                    return;
                }
                sendBody(socket, mediaBody);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

struct JavaScriptProbe {
    bool ready = false;
    QVariant value;
};

QVariant evaluateJavaScript(
    yobro::qtwebengine::QtBrowserPage &page,
    const QString &source,
    std::chrono::milliseconds timeout = 5s
) {
    auto probe = std::make_shared<JavaScriptProbe>();
    page.view()->page()->runJavaScript(source, [probe](const QVariant &value) {
        probe->value = value;
        probe->ready = true;
    });
    waitUntil([probe] { return probe->ready; }, timeout, "JavaScript evaluation");
    return probe->value;
}

void waitForStatus(
    yobro::qtwebengine::QtBrowserPage &page,
    std::string_view expected,
    std::chrono::milliseconds timeout = 20s
) {
    QElapsedTimer elapsed;
    elapsed.start();
    std::string observed;
    while (elapsed.elapsed() < timeout.count()) {
        observed = evaluateJavaScript(
            page,
            QStringLiteral("document.querySelector('#status').textContent")
        ).toString().toStdString();
        if (observed == expected)
            return;
        QTest::qWait(20);
    }
    fail("Timed out waiting for media status " + std::string(expected)
         + "; observed " + observed + ".");
}

void waitForDeniedMedia(
    yobro::qtwebengine::QtBrowserPage &page,
    std::chrono::milliseconds timeout = 20s
) {
    QElapsedTimer elapsed;
    elapsed.start();
    std::string observed;
    while (elapsed.elapsed() < timeout.count()) {
        observed = evaluateJavaScript(
            page,
            QStringLiteral("document.querySelector('#status').textContent")
        ).toString().toStdString();
        // Qt WebEngine 6.11 maps an explicit host deny to AbortError on the
        // offscreen/fake-device backend and to NotAllowedError on native UI.
        if (observed == "denied:AbortError" || observed == "denied:NotAllowedError")
            return;
        QTest::qWait(20);
    }
    fail("Timed out waiting for a denied media request; observed " + observed + ".");
}

void waitForLoad(yobro::qtwebengine::QtBrowserPage &page, std::string_view url) {
    waitUntil([&] {
        const auto state = page.state();
        return !state.loading && state.url == url && state.title == "Permission Fixture";
    }, 20s, "permission fixture load");
}

yobro::qtwebengine::QtBrowserPage &asQtPage(yobro::engine::BrowserPage &page) {
    auto *qtPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&page);
    check(qtPage != nullptr, "Qt profile returned a non-Qt page.");
    return *qtPage;
}

// Vertical centres of the fixture buttons, in the same order as the markup.
constexpr int microphoneButtonY = 45;
constexpr int cameraButtonY = 105;
constexpr int bothButtonY = 165;
constexpr int geolocationButtonY = 225;
constexpr int notificationButtonY = 285;

void clickFixtureButton(yobro::qtwebengine::QtBrowserPage &page, int y) {
    page.view()->setFocus(Qt::OtherFocusReason);
    QTest::qWait(50);
    QWidget *target = page.view()->focusProxy();
    if (!target)
        target = page.view();
    target->setFocus(Qt::OtherFocusReason);
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, QPoint(160, y), 20);
}

QMessageBox *visiblePermissionDialog(yobro::spike::SpikeWindow &window) {
    const auto dialogs = window.findChildren<QMessageBox *>(QStringLiteral("mediaPermissionPrompt"));
    for (QMessageBox *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

QMessageBox &waitForPermissionDialog(yobro::spike::SpikeWindow &window) {
    static int sequence = 0;
    const int expectedSequence = ++sequence;
    QMessageBox *dialog = nullptr;
    waitUntil([&] {
        dialog = visiblePermissionDialog(window);
        return dialog != nullptr;
    }, 10s, "asynchronous native permission dialog #" + std::to_string(expectedSequence));
    return *dialog;
}

void clickDialogButton(QMessageBox &dialog, const char *objectName) {
    auto *button = dialog.findChild<QPushButton *>(QString::fromLatin1(objectName));
    check(button != nullptr && button->isVisible(), "Permission dialog button is missing.");
    QTest::mouseClick(button, Qt::LeftButton);
}

void checkAskEveryTime(yobro::qtwebengine::QtBrowserProfile &profile) {
    check(
        profile.persistentProfile()->persistentPermissionsPolicy()
            == QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime,
        "Persistent Chromium permissions are not AskEveryTime."
    );
    check(
        profile.privateProfile()->persistentPermissionsPolicy()
            == QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime,
        "Private Chromium permissions are not AskEveryTime."
    );
}

void verifyLegacyPermissionMigration(
    const yobro::core::ProfilePaths &paths,
    const std::string &url
) {
    {
        QWebEngineProfile legacy(QStringLiteral("permission-integration"));
        legacy.setPersistentStoragePath(QString::fromStdString(paths.storage.string()));
        legacy.setCachePath(QString::fromStdString(paths.cache.string()));
        legacy.setPersistentPermissionsPolicy(
            QWebEngineProfile::PersistentPermissionsPolicy::StoreOnDisk
        );
        const QWebEnginePermission seeded = legacy.queryPermission(
            QUrl(QString::fromStdString(url)),
            QWebEnginePermission::PermissionType::Geolocation
        );
        check(seeded.isValid(), "Could not create a legacy media permission.");
        seeded.grant();
        check(seeded.state() == QWebEnginePermission::State::Granted,
              "Legacy media grant was not seeded.");
        check(!legacy.listAllPermissions().isEmpty(),
              "Legacy StoreOnDisk profile did not expose the seeded grant.");
        // Persist the seed before destroying its profile; otherwise its async
        // write can race after the migration profile has already enumerated an
        // apparently empty permission store.
        QTest::qWait(300);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    // The Chromium permission backend commits the StoreOnDisk seed while the
    // profile is closing, so also wait after destruction before reopening it.
    QTest::qWait(500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    {
        yobro::qtwebengine::QtBrowserEngine engine;
        auto migrated = engine.openProfile({
            .id = "permission-integration",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *profile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(migrated.get());
        check(profile != nullptr, "Could not open the migration profile.");
        checkAskEveryTime(*profile);
        for (const QWebEnginePermission &permission : profile->persistentProfile()->listAllPermissions()) {
            check(permission.state() == QWebEnginePermission::State::Ask,
                  "Legacy permission reset was not immediately effective in memory.");
        }
        // QWebEngine persists permission resets asynchronously. Keep the
        // migrated profile alive while its StoreOnDisk update reaches disk;
        // destroying it immediately can cancel that flush and makes restart
        // verification timing-dependent.
        QTest::qWait(300);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    // Let Chromium's permission-store backend finish closing and committing
    // the reset before a new profile instance verifies the same disk state.
    QTest::qWait(300);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    {
        QWebEngineProfile verification(QStringLiteral("permission-integration"));
        verification.setPersistentStoragePath(QString::fromStdString(paths.storage.string()));
        verification.setCachePath(QString::fromStdString(paths.cache.string()));
        verification.setPersistentPermissionsPolicy(
            QWebEngineProfile::PersistentPermissionsPolicy::StoreOnDisk
        );
        const QWebEnginePermission migrated = verification.queryPermission(
            QUrl(QString::fromStdString(url)),
            QWebEnginePermission::PermissionType::Geolocation
        );
        check(migrated.state() == QWebEnginePermission::State::Ask,
              "Profile startup did not reset a legacy on-disk media grant.");
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

struct SessionFixture {
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<yobro::spike::BridgePolicyStore> bridgePolicy;
    std::unique_ptr<yobro::spike::SpikeWindow> window;
};

SessionFixture makeSession(
    const yobro::core::ProfilePaths &paths,
    const QString &url
) {
    yobro::qtwebengine::QtBrowserEngine engine;
    auto genericProfile = engine.openProfile({
        .id = "permission-integration",
        .storagePath = paths.storage.string(),
        .cachePath = paths.cache.string(),
        .persistent = true,
    });
    auto *profile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(genericProfile.get());
    check(profile != nullptr, "Could not create the Qt permission profile.");
    checkAskEveryTime(*profile);

    SessionFixture fixture;
    fixture.library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(
        *profile,
        paths.profile,
        paths.profile / "downloads"
    );
    static yobro::qtwebengine::QtEventLoop eventLoop;
    fixture.session = std::make_unique<yobro::controller::BrowserSession>(
        std::move(genericProfile),
        eventLoop,
        yobro::controller::BrowserSessionConfig{
            .profileName = "Permission Integration",
            .space = "Permission Space",
            .library = fixture.library,
        }
    );
    fixture.bridgePolicy = std::make_unique<yobro::spike::BridgePolicyStore>(paths.bridgePolicy);
    fixture.window = std::make_unique<yobro::spike::SpikeWindow>(
        *fixture.session,
        *fixture.library,
        *fixture.bridgePolicy,
        paths,
        url
    );
    fixture.window->setPermissionSurfaceAllowed(true);
    fixture.window->show();
    waitUntil([&] { return fixture.window->isVisible(); }, 5s, "interactive permission surface");
    return fixture;
}

void runInitialSession(
    const yobro::core::ProfilePaths &paths,
    const std::string &url
) {
    SessionFixture fixture = makeSession(paths, QString::fromStdString(url));
    auto &session = *fixture.session;
    auto &window = *fixture.window;
    auto &user = asQtPage(*session.activeUserPage());
    waitForLoad(user, url);

    clickFixtureButton(user, microphoneButtonY);
    QMessageBox &denyDialog = waitForPermissionDialog(window);
    check(denyDialog.text() == QStringLiteral("127.0.0.1 möchte das Mikrofon verwenden."),
          "Microphone prompt title differs from WebKit.");
    check(denyDialog.informativeText() == QStringLiteral("Die Freigabe gilt nur für diesen Seitenaufruf."),
          "Permission prompt visit-only text differs from WebKit.");
    check(denyDialog.defaultButton()
              == denyDialog.findChild<QPushButton *>(QStringLiteral("mediaPermissionDeny")),
          "Permission prompt default action is not deny.");
    bool timerFired = false;
    QTimer::singleShot(0, &window, [&timerFired] { timerFired = true; });
    waitUntil([&] { return timerFired; }, 2s, "event processing while permission dialog is open");
    clickDialogButton(denyDialog, "mediaPermissionDeny");
    waitForDeniedMedia(user);
    check(!session.pendingPermission(), "Denied media request remained pending.");

    auto &privatePage = asQtPage(session.newUserTab(url, true));
    waitForLoad(privatePage, url);
    check(privatePage.profile()->isOffTheRecord(), "Private permission tab used persistent storage.");

    // The persistent page is now backgrounded and must fail closed without UI.
    (void)evaluateJavaScript(user, QStringLiteral("document.querySelector('#video').click(); true"));
    waitForDeniedMedia(user);
    check(visiblePermissionDialog(window) == nullptr && !session.pendingPermission(),
          "A background user page reached the permission prompt.");

    clickFixtureButton(privatePage, cameraButtonY);
    QMessageBox &privateDialog = waitForPermissionDialog(window);
    check(privateDialog.text() == QStringLiteral("127.0.0.1 möchte die Kamera verwenden."),
          "Private camera prompt title differs from WebKit.");
    clickDialogButton(privateDialog, "mediaPermissionAllowOnce");
    waitForStatus(privatePage, "granted:video");

    auto &agent = asQtPage(session.newAgentTab(url));
    waitForLoad(agent, url);
    (void)evaluateJavaScript(agent, QStringLiteral("document.querySelector('#audio').click(); true"));
    waitForDeniedMedia(agent);
    check(visiblePermissionDialog(window) == nullptr && !session.pendingPermission(),
          "An agent-owned page reached the permission prompt.");

    check(session.setActiveUserTab(user.state().id), "Could not reactivate the persistent user tab.");
    waitUntil([&] { return user.view()->isVisible(); }, 5s, "reactivated user view");
    // Geolocation and notifications are Chromium capabilities that WebKit does
    // not expose at all. They now go through the same prompt as media instead of
    // being denied silently, and a denial still reaches the page.
    clickFixtureButton(user, geolocationButtonY);
    QMessageBox &locationDialog = waitForPermissionDialog(window);
    check(locationDialog.text() == QStringLiteral("127.0.0.1 möchte deinen Standort verwenden."),
          "Geolocation prompt does not name the location.");
    // macOS hides message-box window titles, so the visit-only note is what the
    // user actually reads here.
    check(locationDialog.informativeText() == QStringLiteral("Die Freigabe gilt nur für diesen Seitenaufruf."),
          "Geolocation prompt lost the visit-only note.");
    clickDialogButton(locationDialog, "mediaPermissionDeny");
    waitForStatus(user, "geo-denied:1");
    check(!session.pendingPermission(), "Denied geolocation remained pending.");

    clickFixtureButton(user, notificationButtonY);
    QMessageBox &notificationDialog = waitForPermissionDialog(window);
    check(notificationDialog.text() == QStringLiteral("127.0.0.1 möchte dir Mitteilungen senden."),
          "Notification prompt does not use a sending sentence.");
    clickDialogButton(notificationDialog, "mediaPermissionDeny");
    waitForStatus(user, "notify:denied");
    check(!session.pendingPermission(), "Denied notifications remained pending.");

    auto &surfacePage = asQtPage(session.newUserTab(url));
    waitForLoad(surfacePage, url);
    clickFixtureButton(surfacePage, bothButtonY);
    (void)waitForPermissionDialog(window);
    window.hide();
    waitUntil([&] {
        return !session.pendingPermission() && visiblePermissionDialog(window) == nullptr;
    }, 5s, "hidden-surface permission cancellation");
    waitForDeniedMedia(surfacePage);
    window.show();
    waitUntil([&] { return window.isVisible(); }, 5s, "restored permission surface");

    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.setPermissionSurfaceAllowed(false);
    auto &packagePage = asQtPage(session.newUserTab(url));
    waitForLoad(packagePage, url);
    (void)evaluateJavaScript(packagePage, QStringLiteral("document.querySelector('#audio').click(); true"));
    waitForDeniedMedia(packagePage);
    check(visiblePermissionDialog(window) == nullptr && !session.pendingPermission(),
          "The hidden package harness reached the permission prompt.");
    window.setAttribute(Qt::WA_DontShowOnScreen, false);
    window.setPermissionSurfaceAllowed(true);

    session.setProfileActive(false);
    (void)evaluateJavaScript(packagePage, QStringLiteral("document.querySelector('#video').click(); true"));
    waitForDeniedMedia(packagePage);
    check(visiblePermissionDialog(window) == nullptr && !session.pendingPermission(),
          "An inactive profile reached the permission prompt.");
    session.setProfileActive(true);
}

void runRestartSession(
    const yobro::core::ProfilePaths &paths,
    const std::string &url,
    bool grant
) {
    SessionFixture fixture = makeSession(paths, QString::fromStdString(url));
    auto &session = *fixture.session;
    auto &window = *fixture.window;
    auto &user = asQtPage(*session.activeUserPage());
    waitForLoad(user, url);

    // A previous profile session made a decision for this origin. The same
    // storage must still ask because neither grants nor denials are persisted.
    clickFixtureButton(user, microphoneButtonY);
    QMessageBox &dialog = waitForPermissionDialog(window);
    check(session.pendingPermission().has_value(),
          "Restart reused a stored media decision instead of prompting again.");
    clickDialogButton(
        dialog,
        grant ? "mediaPermissionAllowOnce" : "mediaPermissionDeny"
    );
    if (grant) {
        waitForStatus(user, "granted:audio");
        auto &agent = asQtPage(session.newAgentTab(url));
        waitForLoad(agent, url);
        (void)evaluateJavaScript(agent, QStringLiteral("document.querySelector('#audio').click(); true"));
        waitForDeniedMedia(agent);
        check(visiblePermissionDialog(window) == nullptr && !session.pendingPermission(),
              "A same-origin agent page inherited a user media grant.");
    } else {
        waitForDeniedMedia(user);
    }
}

void runAuthenticationSession(
    const yobro::core::ProfilePaths &paths,
    const std::string &mediaUrl,
    const std::string &authUrl
) {
    SessionFixture fixture = makeSession(paths, QString::fromStdString(mediaUrl));
    auto &session = *fixture.session;
    auto &user = asQtPage(*session.activeUserPage());
    waitForLoad(user, mediaUrl);
    bool prompted = false;
    QTimer promptAutomation;
    QObject::connect(&promptAutomation, &QTimer::timeout, &promptAutomation, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || dialog->objectName() != QStringLiteral("httpAuthenticationPrompt"))
                continue;
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("httpAuthenticationUser"));
            auto *password = dialog->findChild<QLineEdit *>(QStringLiteral("httpAuthenticationPassword"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            check(name && password && buttons, "HTTP authentication prompt is incomplete.");
            name->setText(QStringLiteral("user"));
            password->setText(QStringLiteral("pass"));
            prompted = true;
            promptAutomation.stop();
            buttons->button(QDialogButtonBox::Ok)->click();
            return;
        }
    });
    promptAutomation.start(10);
    check(session.navigateUserTab(user.state().id, authUrl), "Could not navigate to the auth fixture.");
    waitUntil([&] {
        const auto state = user.state();
        return prompted && !state.loading && state.title == "Authenticated Fixture";
    }, 20s, "authenticated HTTP page load");
    check(user.state().url == authUrl, "Authentication navigated to a different URL.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-permission-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT PERMISSION/MEDIA INTEGRATION FAIL: Could not create an isolated profile directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Permission Integration"));

    try {
        QTcpServer fixtureServer;
        check(fixtureServer.listen(QHostAddress::LocalHost, 0),
              "Could not start the permission loopback fixture.");
        serveFixture(fixtureServer);
        const std::string url = QStringLiteral("http://127.0.0.1:%1/media")
            .arg(fixtureServer.serverPort()).toStdString();
        const auto paths = yobro::core::ProfilePaths::forProfile("permission-integration");
        paths.createDirectories();

        verifyLegacyPermissionMigration(paths, url);
        runInitialSession(paths, url);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        runRestartSession(paths, url, true);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        runRestartSession(paths, url, false);
        const std::string authUrl = QStringLiteral("http://127.0.0.1:%1/auth")
            .arg(fixtureServer.serverPort()).toStdString();
        runAuthenticationSession(paths, url, authUrl);

        fixtureServer.close();
        std::cout << "QT PERMISSION/MEDIA INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT PERMISSION/MEDIA INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
