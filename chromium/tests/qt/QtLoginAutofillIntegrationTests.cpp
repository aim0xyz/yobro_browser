// Login autofill without polling. The isolated world used to queue events for a
// 350 ms poll; it now pushes them over a QWebChannel bound to that same world.
// This test drives a real HTTPS page in a real window with real mouse input,
// because the script only reacts to trusted user gestures.
#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/LoginChannel.hpp"
#include "spike/PasswordVault.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QTest>
#include <QTimer>
#include <QWebEngineScript>
#include <QWebEngineView>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;
using yobro::spike::LoginAutofillChannel;
using yobro::spike::LoginCredential;
using yobro::spike::PasswordVault;
using yobro::spike::SpikeWindow;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

template<typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout.count()) {
        if (predicate()) return true;
        QTest::qWait(10);
    }
    return predicate();
}

template<typename Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout, std::string_view expectation) {
    if (!waitFor(predicate, timeout))
        fail("Timed out waiting for " + std::string(expectation) + ".");
}

struct SelfSignedIdentity {
    QSslCertificate certificate;
    QSslKey key;
};

/// HTTPS is required: the login script refuses to run on anything else, exactly
/// like the WebKit build's origin rule.
SelfSignedIdentity createIdentity(const QString &directory) {
    const QString keyPath = directory + QStringLiteral("/key.pem");
    const QString certificatePath = directory + QStringLiteral("/cert.pem");
    const QString configPath = directory + QStringLiteral("/openssl.cnf");
    {
        QFile config(configPath);
        check(config.open(QIODevice::WriteOnly | QIODevice::Text), "Could not write the OpenSSL config.");
        config.write(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n"
            "[dn]\nCN=YOBRO Login Fixture\nO=YOBRO Test\n"
            "[ext]\nsubjectAltName=IP:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\n"
        );
    }
    QProcess openssl;
    openssl.start(QStringLiteral("/usr/bin/openssl"), {
        QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
        QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("30"),
        QStringLiteral("-keyout"), keyPath, QStringLiteral("-out"), certificatePath,
        QStringLiteral("-config"), configPath,
    });
    check(openssl.waitForFinished(30'000) && openssl.exitCode() == 0,
          "OpenSSL could not create the fixture certificate: "
              + QString::fromUtf8(openssl.readAllStandardError()).toStdString());
    SelfSignedIdentity identity;
    const QList<QSslCertificate> certificates = QSslCertificate::fromPath(certificatePath);
    check(!certificates.isEmpty(), "The fixture certificate could not be read.");
    identity.certificate = certificates.first();
    QFile keyFile(keyPath);
    check(keyFile.open(QIODevice::ReadOnly), "The fixture key could not be read.");
    identity.key = QSslKey(&keyFile, QSsl::Rsa);
    check(!identity.key.isNull(), "The fixture key is not usable.");
    return identity;
}

// Each control is exactly 50 pixels high, so the click positions below are
// stable regardless of the platform font.
constexpr int usernameFieldY = 25;
constexpr int passwordFieldY = 75;
constexpr int signInButtonY = 125;

QByteArray fixtureBody() {
    return QByteArrayLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>Login Fixture</title>"
        "<style>html,body{margin:0;padding:0}"
        "input,button{display:block;width:320px;height:50px;box-sizing:border-box;margin:0;font-size:16px}</style>"
        "<form id=\"form\" action=\"/login\" method=\"post\">"
        "<input id=\"user\" name=\"username\" type=\"text\" autocomplete=\"username\">"
        "<input id=\"pass\" name=\"password\" type=\"password\" autocomplete=\"current-password\">"
        // Deliberately not a submit button: the capture must be observable
        // without navigating away from the document under test.
        "<button id=\"go\" type=\"button\">Sign in</button>"
        "</form>"
    );
}

void serve(QSslServer &server) {
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("served").toBool()) { (void)socket->readAll(); return; }
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                socket->setProperty("served", true);
                const QByteArray body = fixtureBody();
                QByteArray response = QByteArrayLiteral(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                ) + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

struct LoginApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    explicit LoginApp(const char *profileId = "login-autofill")
        : paths(yobro::core::ProfilePaths::forProfile(profileId)), policy(paths.bridgePolicy) {
        paths.createDirectories();
        QFile::remove(QString::fromStdString(paths.session.string()));
        QFile::remove(QString::fromStdString((paths.profile / "passwords.vault").string()));
        profile = engine.openProfile({
            .id = profileId,
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the login test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = profileId,
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->resize(900, 700);
        window->show();
    }
    ~LoginApp() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

/// Runs script in the login world, where the isolated script and the channel
/// transport live, and waits for the answer.
QVariant evaluateInLoginWorld(yobro::qtwebengine::QtBrowserPage &page, const QString &script) {
    std::optional<QVariant> answer;
    page.view()->page()->runJavaScript(script, QWebEngineScript::UserWorld, [&answer](const QVariant &value) {
        answer = value;
    });
    waitUntil([&answer] { return answer.has_value(); }, 10s, "a result from the login world");
    return *answer;
}

QVariant evaluateInPage(yobro::qtwebengine::QtBrowserPage &page, const QString &script) {
    std::optional<QVariant> answer;
    page.view()->page()->runJavaScript(script, [&answer](const QVariant &value) { answer = value; });
    waitUntil([&answer] { return answer.has_value(); }, 10s, "a result from the page");
    return *answer;
}

void clickFixture(yobro::qtwebengine::QtBrowserPage &page, int y) {
    page.view()->setFocus(Qt::OtherFocusReason);
    QTest::qWait(50);
    QWidget *target = page.view()->focusProxy();
    if (!target) target = page.view();
    target->setFocus(Qt::OtherFocusReason);
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, QPoint(160, y), 20);
}

QFrame *visibleSavePrompt(SpikeWindow &window) {
    for (QFrame *frame : window.findChildren<QFrame *>(QStringLiteral("loginSavePrompt"))) {
        if (frame->isVisible()) return frame;
    }
    return nullptr;
}

QWidget *visibleSuggestionPopup(SpikeWindow &window) {
    for (QWidget *widget : window.findChildren<QWidget *>(QStringLiteral("loginSuggestionPopup"))) {
        if (widget->isVisible()) return widget;
    }
    return nullptr;
}

/// The receiver is the only door from the renderer into credential handling, so
/// its input validation is checked directly instead of only through a page.
void checkChannelGuards() {
    LoginAutofillChannel channel;
    int received = 0;
    QJsonObject last;
    QObject::connect(&channel, &LoginAutofillChannel::received, &channel, [&](const QJsonObject &event) {
        ++received;
        last = event;
    });
    for (const QString &payload : {
             QString(),
             QStringLiteral("not json"),
             QStringLiteral("[1,2,3]"),
             QStringLiteral("\"text\""),
             QStringLiteral("17"),
             QStringLiteral("null"),
             QStringLiteral("{}"),
             QStringLiteral("{\"kind\":\"focus\""),
         }) {
        channel.report(payload);
    }
    check(received == 0, "The login receiver accepted a malformed payload.");
    channel.report(QStringLiteral("{\"kind\":\"focus\",\"origin\":\"https://example.com\"}"));
    check(received == 1, "The login receiver dropped a well-formed event.");
    check(last.value(QStringLiteral("kind")).toString() == QStringLiteral("focus"),
          "The forwarded event lost its kind.");
    const QString oversized = QStringLiteral("{\"kind\":\"save\",\"password\":\"")
        + QString(LoginAutofillChannel::maximumPayloadBytes, QLatin1Char('x'))
        + QStringLiteral("\"}");
    channel.report(oversized);
    check(received == 1, "The login receiver accepted an oversized payload.");
}

void runChecks(LoginApp &app, const QString &fixtureUrl, const QString &origin) {
    auto *address = app.window->findChild<QLineEdit *>(QStringLiteral("topAddress"));
    check(address != nullptr, "The window has no address field.");
    address->setText(fixtureUrl);
    Q_EMIT address->returnPressed();
    waitUntil(
        [&app] {
            auto *current = app.window->visibleUserPage();
            return current && !current->state().loading
                && QString::fromStdString(current->state().url).startsWith(QStringLiteral("https://127.0.0.1"));
        },
        30s,
        "the HTTPS fixture to finish loading"
    );
    auto *page = app.window->visibleUserPage();
    check(page != nullptr, "The fixture page disappeared.");
    check(evaluateInPage(*page, QStringLiteral("document.getElementById('pass') !== null")).toBool(),
          "The fixture document did not load; the certificate was probably refused.");

    // The pull API is gone. If this ever comes back, the poll came back with it.
    check(evaluateInLoginWorld(*page, QStringLiteral("!!window.__yobroLogin")).toBool(),
          "The isolated login script did not run on the HTTPS fixture.");
    check(
        evaluateInLoginWorld(*page, QStringLiteral("typeof window.__yobroLogin.takeEvent")).toString()
            == QStringLiteral("undefined"),
        "The polled takeEvent() entry point still exists."
    );
    check(evaluateInLoginWorld(*page, QStringLiteral("typeof qt")).toString() == QStringLiteral("object"),
          "The channel transport was not bound to the login world.");
    check(evaluateInLoginWorld(*page, QStringLiteral("typeof QWebChannel")).toString() == QStringLiteral("function"),
          "The channel client was not prepended to the login script.");
    // The page's own world must not see any of it.
    check(evaluateInPage(*page, QStringLiteral("typeof window.__yobroLogin")).toString() == QStringLiteral("undefined"),
          "The page world can see the login script.");
    check(evaluateInPage(*page, QStringLiteral("typeof qt")).toString() == QStringLiteral("undefined"),
          "The page world can see the channel transport.");
    for (QTimer *timer : app.window->findChildren<QTimer *>()) {
        check(timer->interval() != 350, "A 350 ms timer is back in the window; the poll may have returned.");
    }

    PasswordVault vault(app.paths.profile);
    if (!vault.available()) {
        std::cout << "QT LOGIN AUTOFILL: keychain unavailable, credential surfaces skipped\n";
        return;
    }
    // A leftover entry from an earlier run would change what the popup offers.
    for (const LoginCredential &entry : vault.entries(origin.toStdString()))
        (void)vault.remove(entry.origin, entry.username);
    check(vault.store({origin.toStdString(), "ada@example.com", "stored-secret"}, true)
              != yobro::spike::PasswordStoreResult::failed,
          "The fixture credential could not be stored.");

    // Focusing a login field must open the suggestions on its own, and it has
    // to be quick: the old poll could only answer after up to 350 ms.
    QElapsedTimer latency;
    latency.start();
    clickFixture(*page, passwordFieldY);
    waitUntil([&app] { return visibleSuggestionPopup(*app.window) != nullptr; }, 10s,
              "the suggestion popup after focusing the password field");
    const qint64 pushLatency = latency.elapsed();
    check(pushLatency < 300, "The pushed focus event took " + std::to_string(pushLatency) + " ms to arrive.");
    QWidget *popup = visibleSuggestionPopup(*app.window);
    check(popup != nullptr, "The suggestion popup vanished.");
    QPushButton *entry = nullptr;
    for (QPushButton *button : popup->findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("ada@example.com")) entry = button;
    }
    check(entry != nullptr, "The suggestion popup did not offer the stored account.");
    QTest::mouseClick(entry, Qt::LeftButton);
    waitUntil(
        [&] {
            return evaluateInPage(*page, QStringLiteral("document.getElementById('pass').value"))
                .toString() == QStringLiteral("stored-secret");
        },
        10s,
        "the password field to be filled"
    );
    check(evaluateInPage(*page, QStringLiteral("document.getElementById('user').value")).toString()
              == QStringLiteral("ada@example.com"),
          "The username field was not filled.");

    // A submitted form with a different password must offer to save it. The
    // click is real input, because the script ignores untrusted gestures.
    (void)evaluateInPage(*page, QStringLiteral(
        "document.getElementById('user').value='grace@example.com';"
        "document.getElementById('pass').value='fresh-secret';true"
    ));
    clickFixture(*page, signInButtonY);
    waitUntil([&app] { return visibleSavePrompt(*app.window) != nullptr; }, 10s,
              "the save prompt after pressing Sign in");
    QFrame *prompt = visibleSavePrompt(*app.window);
    check(prompt != nullptr, "The save prompt vanished.");
    bool namesAccount = false;
    for (QLabel *label : prompt->findChildren<QLabel *>()) {
        if (label->text() == QStringLiteral("grace@example.com")) namesAccount = true;
    }
    check(namesAccount, "The save prompt did not name the submitted account.");
    auto *confirm = prompt->findChild<QPushButton *>(QStringLiteral("loginSaveConfirm"));
    check(confirm != nullptr, "The save prompt has no confirm button.");
    QTest::mouseClick(confirm, Qt::LeftButton);
    waitUntil(
        [&] {
            for (const LoginCredential &stored : vault.entries(origin.toStdString())) {
                if (stored.username == "grace@example.com" && stored.password == "fresh-secret") return true;
            }
            return false;
        },
        10s,
        "the confirmed credential to reach the vault"
    );

    // Submitting a credential that is already stored must stay silent.
    QTest::qWait(100);
    check(visibleSavePrompt(*app.window) == nullptr, "The save prompt stayed open after saving.");
    clickFixture(*page, usernameFieldY);
    QTest::qWait(200);
    (void)evaluateInPage(*page, QStringLiteral(
        "document.getElementById('user').value='grace@example.com';"
        "document.getElementById('pass').value='fresh-secret';true"
    ));
    clickFixture(*page, signInButtonY);
    QTest::qWait(600);
    check(visibleSavePrompt(*app.window) == nullptr,
          "An already stored credential was offered for saving again.");

    for (const LoginCredential &stored : vault.entries(origin.toStdString()))
        (void)vault.remove(stored.origin, stored.username);

    // A private tab gets no credential channel at all.
    auto *privateButton = app.window->findChild<QPushButton *>(QStringLiteral("privateTabButton"));
    check(privateButton != nullptr, "The window has no private tab button.");
    QTest::mouseClick(privateButton, Qt::LeftButton);
    QTest::qWait(100);
    address->setText(fixtureUrl);
    Q_EMIT address->returnPressed();
    waitUntil(
        [&app, page] {
            auto *current = app.window->visibleUserPage();
            return current && current != page && !current->state().loading
                && QString::fromStdString(current->state().url).startsWith(QStringLiteral("https://127.0.0.1"));
        },
        30s,
        "the private fixture tab to load"
    );
    auto *privatePage = app.window->visibleUserPage();
    check(privatePage != nullptr && privatePage != page, "The private tab did not become the visible page.");
    check(evaluateInLoginWorld(*privatePage, QStringLiteral("typeof qt")).toString() == QStringLiteral("undefined"),
          "A private tab received the credential channel.");
}
/// The same window, but with the portable password store, which is what every
/// platform without a Keychain gets. It has one state the Keychain never has:
/// locked. Nothing may be offered or saved until the user opens it.
void runPortableStoreChecks(const QString &fixtureUrl, const QString &origin) {
    qputenv("YOBRO_PASSWORD_STORE", "file");
    struct Reset {
        ~Reset() { qunsetenv("YOBRO_PASSWORD_STORE"); }
    } reset;
    LoginApp app("login-autofill-portable");
    auto *address = app.window->findChild<QLineEdit *>(QStringLiteral("topAddress"));
    check(address != nullptr, "The portable window has no address field.");
    address->setText(fixtureUrl);
    Q_EMIT address->returnPressed();
    waitUntil(
        [&app] {
            auto *current = app.window->visibleUserPage();
            return current && !current->state().loading
                && QString::fromStdString(current->state().url).startsWith(QStringLiteral("https://127.0.0.1"));
        },
        30s,
        "the HTTPS fixture in the portable window"
    );
    auto *page = app.window->visibleUserPage();
    check(page != nullptr, "The portable fixture page disappeared.");

    // Locked: focusing a login field must offer nothing at all.
    clickFixture(*page, passwordFieldY);
    QTest::qWait(400);
    check(visibleSuggestionPopup(*app.window) == nullptr,
          "A locked store offered suggestions.");
    (void)evaluateInPage(*page, QStringLiteral(
        "document.getElementById('user').value='locked@example.com';"
        "document.getElementById('pass').value='locked-secret';true"
    ));
    clickFixture(*page, signInButtonY);
    QTest::qWait(600);
    check(visibleSavePrompt(*app.window) == nullptr, "A locked store offered to save a credential.");

    // The dialog has to explain the state and offer a way out of it.
    auto *settings = app.window->findChild<QAction *>(QStringLiteral("settingsAction"));
    check(settings != nullptr, "The window has no settings command.");
    settings->trigger();
    waitUntil(
        [&app] { return app.window->findChild<QPushButton *>(QStringLiteral("managePasswordsButton")) != nullptr; },
        10s,
        "the settings dialog"
    );
    auto *manage = app.window->findChild<QPushButton *>(QStringLiteral("managePasswordsButton"));
    check(manage->isEnabled(), "Managing passwords was disabled for the portable store.");
    QTest::mouseClick(manage, Qt::LeftButton);
    waitUntil(
        [&app] { return app.window->findChild<QLabel *>(QStringLiteral("passwordVaultStateNote")) != nullptr; },
        10s,
        "the password store section"
    );
    auto *note = app.window->findChild<QLabel *>(QStringLiteral("passwordVaultStateNote"));
    auto *passphrase = app.window->findChild<QLineEdit *>(QStringLiteral("passwordVaultPassphraseField"));
    auto *unlock = app.window->findChild<QPushButton *>(QStringLiteral("passwordVaultUnlockButton"));
    auto *lockButton = app.window->findChild<QPushButton *>(QStringLiteral("passwordVaultLockButton"));
    check(note && passphrase && unlock && lockButton, "The password store section is incomplete.");
    check(passphrase->echoMode() == QLineEdit::Password, "The passphrase field shows what is typed.");
    const QString lockedNote = note->text();
    check(!lockedNote.isEmpty(), "The locked state was not explained.");

    passphrase->setText(QStringLiteral("a portable passphrase"));
    QTest::mouseClick(unlock, Qt::LeftButton);
    waitUntil([note, lockedNote] { return note->text() != lockedNote; }, 30s, "the store to unlock");
    check(passphrase->text().isEmpty(), "The passphrase stayed in the field after unlocking.");
    check(QFile::exists(QString::fromStdString((app.paths.profile / "passwords.vault").string())),
          "Unlocking did not create the store file.");

    // Unlocked: the same submission now offers to save, and the credential
    // really lands in the encrypted file.
    auto *passwordsDialog = app.window->findChild<QDialog *>(QStringLiteral("savedPasswordsDialog"));
    check(passwordsDialog != nullptr, "The password dialog vanished.");
    passwordsDialog->close();
    QTest::qWait(100);
    clickFixture(*page, usernameFieldY);
    QTest::qWait(200);
    (void)evaluateInPage(*page, QStringLiteral(
        "document.getElementById('user').value='portable@example.com';"
        "document.getElementById('pass').value='portable-secret';true"
    ));
    clickFixture(*page, signInButtonY);
    waitUntil([&app] { return visibleSavePrompt(*app.window) != nullptr; }, 10s,
              "the save prompt with the portable store unlocked");
    QFrame *prompt = visibleSavePrompt(*app.window);
    auto *confirm = prompt->findChild<QPushButton *>(QStringLiteral("loginSaveConfirm"));
    check(confirm != nullptr, "The save prompt has no confirm button.");
    QTest::mouseClick(confirm, Qt::LeftButton);
    QTest::qWait(300);

    PasswordVault reader(app.paths.profile, yobro::spike::PasswordStoreKind::encryptedFile);
    check(reader.unlock("a portable passphrase"), "The written store did not open: " + reader.problem());
    bool found = false;
    for (const LoginCredential &entry : reader.entries(origin.toStdString())) {
        if (entry.username == "portable@example.com" && entry.password == "portable-secret") found = true;
    }
    check(found, "The confirmed credential did not reach the encrypted store.");
    check(!reader.entries(origin.toStdString()).empty(), "The store came back empty.");
    // Nothing that was submitted while the store was locked may be in there.
    for (const LoginCredential &entry : reader.entries())
        check(entry.username != "locked@example.com", "A credential from the locked phase was stored.");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-login-XXXXXX"));
    QTemporaryDir certificates(QStringLiteral("/tmp/yobro-login-cert-XXXXXX"));
    if (!home.isValid() || !certificates.isValid()) {
        std::cerr << "QT LOGIN AUTOFILL FAIL: Could not create the isolated directories.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Login Autofill"));
    try {
        const SelfSignedIdentity identity = createIdentity(certificates.path());
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(identity.certificate);
        configuration.setPrivateKey(identity.key);
        QSslServer server;
        server.setSslConfiguration(configuration);
        check(server.listen(QHostAddress::LocalHost, 0), "Could not start the HTTPS fixture server.");
        serve(server);
        const QString origin = QStringLiteral("https://127.0.0.1:%1").arg(server.serverPort());
        const QString fixtureUrl = origin + QStringLiteral("/login");
        checkChannelGuards();
        {
            LoginApp app;
            runChecks(app, fixtureUrl, origin);
        }
        runPortableStoreChecks(fixtureUrl, origin);
        std::cout << "QT LOGIN AUTOFILL PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT LOGIN AUTOFILL FAIL: " << error.what() << '\n';
        return 1;
    }
}
