#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/CertificateTrust.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using yobro::spike::CertificateTrustStore;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

template<typename Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout, std::string_view expectation) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) return;
        QTest::qWait(10);
    }
    fail("Timed out waiting for " + std::string(expectation) + ".");
}

/// A self-signed certificate for 127.0.0.1. Chromium rejects it, which is
/// exactly the situation under test.
struct SelfSignedIdentity {
    QSslCertificate certificate;
    QSslKey key;
    QString fingerprint;
};

SelfSignedIdentity createIdentity(const QString &directory) {
    const QString keyPath = directory + QStringLiteral("/key.pem");
    const QString certificatePath = directory + QStringLiteral("/cert.pem");
    const QString configPath = directory + QStringLiteral("/openssl.cnf");
    {
        QFile config(configPath);
        check(config.open(QIODevice::WriteOnly | QIODevice::Text), "Could not write the OpenSSL config.");
        config.write(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n"
            "[dn]\nCN=YOBRO Test Server\nO=YOBRO Test\n"
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
          "OpenSSL could not create the test certificate: "
          + QString::fromUtf8(openssl.readAllStandardError()).toStdString());

    SelfSignedIdentity identity;
    const QList<QSslCertificate> certificates = QSslCertificate::fromPath(certificatePath);
    check(!certificates.isEmpty(), "The generated certificate could not be read.");
    identity.certificate = certificates.first();

    QFile keyFile(keyPath);
    check(keyFile.open(QIODevice::ReadOnly), "The generated key could not be read.");
    identity.key = QSslKey(&keyFile, QSsl::Rsa);
    check(!identity.key.isNull(), "The generated key is not usable.");

    const QString hex = QString::fromLatin1(
        identity.certificate.digest(QCryptographicHash::Sha256).toHex()
    ).toUpper();
    QStringList pairs;
    for (int index = 0; index + 1 < hex.size(); index += 2) pairs.append(hex.mid(index, 2));
    identity.fingerprint = pairs.join(QLatin1Char(' '));
    return identity;
}

void serve(QSslServer &server) {
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("served").toBool()) { (void)socket->readAll(); return; }
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                socket->setProperty("served", true);
                const QByteArray body = QByteArrayLiteral(
                    "<!doctype html><meta charset=\"utf-8\"><title>Certificate Fixture</title><p>secure</p>"
                );
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

/// The eligibility rule is what keeps this feature from becoming a
/// click-through-anything button, so it is checked on its own.
void checkEligibility() {
    for (const QString &host : {
             QStringLiteral("localhost"), QStringLiteral("::1"), QStringLiteral("dev.localhost"),
             QStringLiteral("printer.local"), QStringLiteral("api.internal"), QStringLiteral("shop.test"),
             QStringLiteral("nas.home.arpa"), QStringLiteral("127.0.0.1"), QStringLiteral("10.1.2.3"),
             QStringLiteral("192.168.1.1"), QStringLiteral("172.16.0.9"), QStringLiteral("172.31.255.255"),
             QStringLiteral("169.254.1.1"), QStringLiteral("LOCALHOST"),
         }) {
        check(CertificateTrustStore::isLocal(host),
              "A local host was not recognised: " + host.toStdString());
    }
    for (const QString &host : {
             QStringLiteral("example.com"), QStringLiteral("bank.de"), QStringLiteral("localhost.evil.com"),
             QStringLiteral("test.example.com"), QStringLiteral("8.8.8.8"), QStringLiteral("172.32.0.1"),
             QStringLiteral("172.15.0.1"), QStringLiteral("192.169.1.1"), QStringLiteral("11.0.0.1"),
             QStringLiteral("local.example"), QStringLiteral(""),
         }) {
        check(!CertificateTrustStore::isLocal(host),
              "A host that could be a public website was treated as local: " + host.toStdString());
    }
}

void checkStore() {
    CertificateTrustStore store;
    check(store.exceptionCount() == 0, "A fresh store already holds exceptions.");
    store.accept(QStringLiteral("Dev.Localhost"), QStringLiteral("AA BB"));
    check(store.isAccepted(QStringLiteral("dev.localhost"), QStringLiteral("AA BB")),
          "The exception was not found again, case-insensitively.");
    // A swapped certificate has to ask again instead of inheriting the approval.
    check(!store.isAccepted(QStringLiteral("dev.localhost"), QStringLiteral("CC DD")),
          "A different certificate reused the exception.");
    check(!store.isAccepted(QStringLiteral("other.localhost"), QStringLiteral("AA BB")),
          "The exception leaked to another host.");
    store.accept(QStringLiteral("a.test"), QStringLiteral("11"));
    const std::vector<QString> hosts = store.hosts();
    check(hosts.size() == 2 && hosts.front() == QStringLiteral("a.test"),
          "The exception listing is not sorted by host.");
    store.forget(QStringLiteral("DEV.LOCALHOST"));
    check(store.exceptionCount() == 1, "Forgetting an exception did not remove it.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-certificate-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT CERTIFICATE INTEGRATION FAIL: Could not create an isolated directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    try {
        checkEligibility();
        checkStore();

        const SelfSignedIdentity identity = createIdentity(home.path());
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(identity.certificate);
        configuration.setPrivateKey(identity.key);
        QSslServer server;
        server.setSslConfiguration(configuration);
        check(server.listen(QHostAddress::LocalHost, 0), "Could not start the TLS fixture.");
        serve(server);
        const std::string url = QStringLiteral("https://127.0.0.1:%1/page")
            .arg(server.serverPort()).toStdString();

        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "certificate-integration",
            .storagePath = (home.path() + QStringLiteral("/storage")).toStdString(),
            .cachePath = (home.path() + QStringLiteral("/cache")).toStdString(),
            .persistent = true,
        });
        auto generic = profile.createPage(
            "certificate-user", yobro::engine::PageOwner::user, false
        );
        auto *page = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(generic.get());
        check(page != nullptr, "Could not create the certificate test page.");

        std::optional<yobro::engine::CertificateProblem> seen;
        bool decision = false;

        // Without a handler nothing may get through. This runs first and against
        // a host of its own, because Chromium remembers an accepted certificate
        // for the life of the profile and would not ask a second time.
        const std::string otherHostUrl = QStringLiteral("https://localhost:%1/page")
            .arg(server.serverPort()).toStdString();
        (void)page->navigate(otherHostUrl);
        waitUntil([&] { return !page->state().loading; }, 20s, "the unhandled navigation to end");
        check(page->state().title != "Certificate Fixture",
              "A page without a certificate handler was loaded anyway.");

        page->setCertificateProblemHandler(
            [&seen, &decision](const yobro::engine::CertificateProblem &problem) {
                seen = problem;
                return decision;
            }
        );

        // Refused: the navigation must not deliver the page.
        seen.reset();
        decision = false;
        (void)page->navigate(url);
        waitUntil([&] { return seen.has_value(); }, 20s, "the certificate problem to be reported");
        check(seen->host == "127.0.0.1", "The reported host is wrong: " + seen->host);
        check(seen->overridable, "Chromium reported a self-signed certificate as not overridable.");
        check(seen->mainFrame, "The top-level navigation was not reported as the main frame.");
        check(seen->fingerprint == identity.fingerprint.toStdString(),
              "The reported fingerprint does not match the served certificate: " + seen->fingerprint);
        check(seen->subject == "YOBRO Test Server", "The reported subject is wrong: " + seen->subject);
        check(!seen->description.empty(), "The engine's description was dropped.");
        waitUntil([&] { return !page->state().loading; }, 20s, "the refused navigation to end");
        check(page->state().title != "Certificate Fixture",
              "A refused certificate still delivered the page.");

        // Accepted: the very same certificate now loads.
        seen.reset();
        decision = true;
        (void)page->navigate(url);
        waitUntil([&] {
            const auto state = page->state();
            return !state.loading && state.title == "Certificate Fixture";
        }, 20s, "the accepted navigation to deliver the page");
        check(seen.has_value(), "The accepted navigation did not go through the handler.");

        server.close();
        std::cout << "QT CERTIFICATE INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT CERTIFICATE INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
