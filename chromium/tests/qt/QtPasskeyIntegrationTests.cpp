#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using yobro::qtwebengine::QtBrowserProfile;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
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

/// Serves one page over loopback. `http://localhost` is a secure context, which
/// is what a passkey call needs.
class Server {
public:
    explicit Server(QByteArray body) : body_(std::move(body)) {
        check(server_.listen(QHostAddress::LocalHost), "The test server could not listen.");
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            QTcpSocket *socket = server_.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                socket->readAll();
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                    + QByteArray::number(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_);
                socket->flush();
                socket->disconnectFromHost();
            });
        });
    }

    [[nodiscard]] QString url() const {
        return QStringLiteral("http://localhost:%1/").arg(server_.serverPort());
    }

private:
    QTcpServer server_;
    QByteArray body_;
};

/// A page that records how a passkey request ends.
QByteArray passkeyPage() {
    return QByteArrayLiteral(R"HTML(<!doctype html><meta charset="utf-8"><body>
<script>
  window.outcome = "pending";
  window.platform = "unknown";
  if (window.PublicKeyCredential &&
      window.PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable) {
    window.PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()
      .then(v => { window.platform = v ? "yes" : "no"; })
      .catch(() => { window.platform = "error"; });
  }
  navigator.credentials.get({publicKey:{
    challenge: new Uint8Array(32),
    rpId: "localhost",
    allowCredentials: [{type: "public-key", id: new Uint8Array(32), transports: ["usb"]}],
    timeout: 4000,
    userVerification: "discouraged"
  }}).then(() => { window.outcome = "asserted"; })
    .catch(e => { window.outcome = "rejected:" + e.name; });
</script></body>)HTML");
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

    App() : paths(yobro::core::ProfilePaths::forProfile("passkeys")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        QFile::remove(QString::fromStdString(paths.session.string()));
        QFile::remove(QString::fromStdString((paths.profile / "onboarding.json").string()));

        profile = engine.openProfile({
            .id = "passkeys",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the passkey test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "passkeys",
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

QString readValue(QWebEnginePage *page, const QString &expression, int milliseconds = 3000) {
    QString result;
    bool answered = false;
    page->runJavaScript(expression, [&result, &answered](const QVariant &value) {
        result = value.toString();
        answered = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!answered && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
    return result;
}

/// The engine's own behaviour, measured rather than assumed.
///
/// This is what justifies the shim: a page's passkey request is accepted and
/// then never answered, not even after the page's own timeout. The check fails
/// once a Qt version settles the promise, which is the signal to set
/// `QtBrowserProfile::passkeysWork()` to true and delete the shim.
void checkEngineStillCannotAnswer(App &app) {
    if (QtBrowserProfile::passkeysWork()) return;
    Server server(passkeyPage());
    // A profile of its own, which the shell never touched: no injected scripts,
    // so this measures the engine and nothing else.
    QWebEngineProfile bare(QStringLiteral("passkey-bare-measurement"));
    QWebEngineView view;
    view.setPage(new QWebEnginePage(&bare, &view));
    view.resize(320, 240);
    view.load(QUrl(server.url()));
    pump(1500);
    // The page's own timeout is 4 seconds; this waits well past it.
    pump(9000);

    const QString outcome = readValue(view.page(), QStringLiteral("window.outcome"));
    check(outcome == QStringLiteral("pending"),
          "The engine now answers passkey requests (" + outcome.toStdString()
              + "). Set QtBrowserProfile::passkeysWork() to true, remove the shim and re-run.");
}

/// What the shell does about it: the request is declined at once, so a site's
/// fallback to another sign-in method actually runs.
void checkShimRejectsFast(App &app) {
    if (QtBrowserProfile::passkeysWork()) return;
    Server server(passkeyPage());
    // Navigated through the window's own address bar, so the profile's injected
    // scripts are in place exactly as they are for a user.
    auto *address = app.window->findChild<QLineEdit *>(QStringLiteral("topAddress"));
    check(address != nullptr, "The address field is missing.");
    address->setText(server.url());
    Q_EMIT address->returnPressed();
    pump(3000);

    QWebEnginePage *loaded = nullptr;
    for (QWebEngineView *view : app.window->findChildren<QWebEngineView *>()) {
        if (view->url().host() == QStringLiteral("localhost")) loaded = view->page();
    }
    check(loaded != nullptr, "The test page did not open in the window.");

    const QString outcome = readValue(loaded, QStringLiteral("window.outcome"));
    // NotAllowedError is what a browser reports when nothing can answer, so a
    // site treats it as a normal decline.
    check(outcome == QStringLiteral("rejected:NotAllowedError"),
          "A passkey request was not declined right away: " + outcome.toStdString());
    // A promise that settles this quickly is the whole point.
    const QString platform = readValue(loaded, QStringLiteral("window.platform"));
    check(platform == QStringLiteral("no"),
          "The page was told a platform authenticator is available: " + platform.toStdString());

    // Password credentials must keep working: the shim only refuses public keys.
    const QString passwordPath = readValue(loaded, QStringLiteral(
        "(typeof navigator.credentials.get === 'function') ? 'callable' : 'missing'"));
    check(passwordPath == QStringLiteral("callable"),
          "The shim removed navigator.credentials.get entirely.");
}

/// The dialog is driven through the presenter, because no authenticator exists
/// in a test machine to produce the real states.
void checkDialogSteps(App &app) {
    SpikeWindow &window = *app.window;
    using yobro::engine::PasskeyControls;
    using yobro::engine::PasskeyFailure;
    using yobro::engine::PasskeyPinError;
    using yobro::engine::PasskeyPinReason;
    using yobro::engine::PasskeyRequest;
    using yobro::engine::PasskeyStage;

    QStringList answers;
    PasskeyControls controls;
    controls.selectAccount = [&answers](const std::string &account) {
        answers.append(QStringLiteral("account:") + QString::fromStdString(account));
    };
    controls.setPin = [&answers](const std::string &pin) {
        answers.append(QStringLiteral("pin:") + QString::fromStdString(pin));
    };
    controls.retry = [&answers]() { answers.append(QStringLiteral("retry")); };
    controls.cancel = [&answers]() { answers.append(QStringLiteral("cancel")); };

    PasskeyRequest request;
    request.relyingPartyId = "example.com";
    request.stage = PasskeyStage::selectAccount;
    request.userNames = {"ada@example.com", "grace@example.com"};
    window.presentPasskeyStep(request, controls);

    auto *dialog = window.findChild<QDialog *>(QStringLiteral("passkeyDialog"));
    check(dialog != nullptr, "The passkey dialog did not open.");
    auto *accounts = dialog->findChild<QListWidget *>(QStringLiteral("passkeyAccountList"));
    auto *pin = dialog->findChild<QLineEdit *>(QStringLiteral("passkeyPinField"));
    auto *message = dialog->findChild<QLabel *>(QStringLiteral("passkeyMessage"));
    auto *continueButton = dialog->findChild<QPushButton *>(QStringLiteral("passkeyContinueButton"));
    auto *retryButton = dialog->findChild<QPushButton *>(QStringLiteral("passkeyRetryButton"));
    auto *cancelButton = dialog->findChild<QPushButton *>(QStringLiteral("passkeyCancelButton"));
    check(accounts && pin && message && continueButton && retryButton && cancelButton,
          "The passkey dialog is missing a control.");
    check(pin->echoMode() == QLineEdit::Password, "The security key PIN is shown in clear text.");

    check(accounts->count() == 2, "The accounts were not offered.");
    check(message->text().contains(QStringLiteral("example.com")),
          "The dialog does not name the site asking.");
    check(!pin->isVisible(), "The PIN field is shown while an account is being chosen.");
    accounts->setCurrentRow(1);
    continueButton->click();
    check(answers == QStringList({QStringLiteral("account:grace@example.com")}),
          "The chosen account was not passed on: " + answers.join(QLatin1Char(',')).toStdString());

    // PIN entry, with the reason, the error, the minimum length and how many
    // tries are left.
    answers.clear();
    request.stage = PasskeyStage::collectPin;
    request.pinReason = PasskeyPinReason::challenge;
    request.pinError = PasskeyPinError::wrongPin;
    request.minimumPinLength = 6;
    request.remainingAttempts = 2;
    window.presentPasskeyStep(request, controls);
    check(pin->isVisible(), "The PIN field is not shown while a PIN is asked for.");
    check(!accounts->isVisible(), "The account list is shown while a PIN is asked for.");
    check(message->text().contains(QStringLiteral("6")), "The minimum PIN length is not shown.");
    check(message->text().contains(QStringLiteral("2")), "The remaining attempts are not shown.");
    pin->setText(QStringLiteral("123456"));
    continueButton->click();
    check(answers == QStringList({QStringLiteral("pin:123456")}), "The PIN was not passed on.");
    check(pin->text().isEmpty(), "The PIN stayed in the field after it was sent.");

    // Touch the key: nothing to answer, so no continue button.
    answers.clear();
    request.stage = PasskeyStage::finishTokenCollection;
    window.presentPasskeyStep(request, controls);
    check(!continueButton->isVisible(), "A continue button is offered while waiting for the key.");
    check(!pin->isVisible() && !accounts->isVisible(), "An input is shown while waiting for the key.");

    // A failure explains itself and offers another attempt.
    request.stage = PasskeyStage::requestFailed;
    request.failure = PasskeyFailure::softPinBlock;
    window.presentPasskeyStep(request, controls);
    check(retryButton->isVisible(), "A failed request offers no retry.");
    check(message->text() == SpikeWindow::passkeyFailureText(PasskeyFailure::softPinBlock),
          "The failure was not explained.");
    retryButton->click();
    check(answers == QStringList({QStringLiteral("retry")}), "Retry was not passed on.");

    // Every failure reason has wording of its own, so none of them shows a
    // placeholder.
    for (const PasskeyFailure failure : {
             PasskeyFailure::timeout, PasskeyFailure::keyNotRegistered,
             PasskeyFailure::keyAlreadyRegistered, PasskeyFailure::softPinBlock,
             PasskeyFailure::hardPinBlock, PasskeyFailure::authenticatorRemovedDuringPinEntry,
             PasskeyFailure::authenticatorMissingResidentKeys,
             PasskeyFailure::authenticatorMissingUserVerification,
             PasskeyFailure::authenticatorMissingLargeBlob, PasskeyFailure::noCommonAlgorithms,
             PasskeyFailure::storageFull, PasskeyFailure::userConsentDenied,
             PasskeyFailure::windowsUserCancelled, PasskeyFailure::unknown,
         }) {
        check(!SpikeWindow::passkeyFailureText(failure).isEmpty(),
              "A failure reason has no wording.");
    }

    // Closing the dialog is a decline, never a silent hang.
    answers.clear();
    request.stage = PasskeyStage::collectPin;
    window.presentPasskeyStep(request, controls);
    cancelButton->click();
    check(answers.contains(QStringLiteral("cancel")), "Cancelling was not passed on.");

    // A finished request closes the dialog instead of leaving it stale.
    request.stage = PasskeyStage::completed;
    window.presentPasskeyStep(request, controls);
    pump(150);
    check(window.findChild<QDialog *>(QStringLiteral("passkeyDialog")) == nullptr
              || !window.findChild<QDialog *>(QStringLiteral("passkeyDialog"))->isVisible(),
          "A completed request left the dialog open.");
}

void checkSettingsNote(App &app) {
    SpikeWindow &window = *app.window;
    window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
    auto *note = window.findChild<QLabel *>(QStringLiteral("passkeyStateNote"));
    check(note != nullptr, "The settings do not mention passkeys.");
    check(!note->text().isEmpty(), "The passkey note is empty.");
    // The wording has to match what the engine can actually do.
    if (QtBrowserProfile::passkeysWork()) {
        check(note->text().contains(QStringLiteral("verfügbar"))
                  || note->text().contains(QStringLiteral("available")),
              "The note does not say that passkeys work.");
    } else {
        check(note->text().contains(QStringLiteral("andere Anmeldemethode"))
                  || note->text().contains(QStringLiteral("another sign-in method")),
              "The note does not point at another sign-in method.");
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    try {
        App app;
        checkEngineStillCannotAnswer(app);
        checkShimRejectsFast(app);
        checkDialogSteps(app);
        checkSettingsNote(app);
    } catch (const std::exception &error) {
        std::cerr << "PASSKEY FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASSKEY PASS\n";
    return 0;
}
