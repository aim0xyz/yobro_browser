#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/SpaceProxyController.hpp"
#include "spike/SpaceProxyStore.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using yobro::spike::SpaceProxyConfig;
using yobro::spike::SpaceProxyController;
using yobro::spike::SpaceProxyStore;
using yobro::spike::SpaceProxyType;

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

void respond(QTcpSocket *socket, const QByteArray &title) {
    const QByteArray body = QByteArrayLiteral("<!doctype html><meta charset=\"utf-8\"><title>")
        + title + QByteArrayLiteral("</title>");
    socket->write(
        QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ")
        + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body
    );
    socket->disconnectFromHost();
}

/// Serves requests and counts them. `absoluteFormOnly` makes it behave like a
/// proxy: it answers only requests whose line carries a full URL, which is what
/// a client sends to an HTTP proxy.
class CountingServer {
public:
    CountingServer(QTcpServer &server, QByteArray title, bool absoluteFormOnly)
        : server_(server), title_(std::move(title)), absoluteFormOnly_(absoluteFormOnly) {
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket *socket = server_.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("served").toBool()) { (void)socket->readAll(); return; }
                    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    socket->setProperty("served", true);
                    if (absoluteFormOnly_ && !request.startsWith("GET http://")) {
                        socket->write(QByteArrayLiteral(
                            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                        ));
                        socket->disconnectFromHost();
                        return;
                    }
                    // Favicon requests would inflate the count.
                    if (!request.contains("favicon.ico")) ++requests_;
                    respond(socket, title_);
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    [[nodiscard]] int requests() const { return requests_; }

private:
    QTcpServer &server_;
    QByteArray title_;
    bool absoluteFormOnly_ = false;
    int requests_ = 0;
};

QString titleOf(yobro::qtwebengine::QtBrowserPage &page) {
    return QString::fromStdString(page.state().title);
}

void loadAndSettle(yobro::qtwebengine::QtBrowserPage &page, const std::string &url) {
    (void)page.navigate(url);
    waitUntil([&] { return !page.state().loading; }, 25s, "the navigation to end for " + url);
    // The published state follows the load slightly.
    QTest::qWait(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void checkConfigValidation() {
    SpaceProxyConfig config;
    check(!config.validationProblem().isEmpty(), "An empty host was accepted.");
    check(config.displayLabel() == QStringLiteral("Kein Proxy")
              || config.displayLabel() == QStringLiteral("No proxy"),
          "An empty configuration has no readable label.");

    config.host = QStringLiteral("socks5://proxy.example.com/");
    check(config.cleanHost() == QStringLiteral("proxy.example.com"),
          "The scheme and slash were not stripped: " + config.cleanHost().toStdString());
    check(config.validationProblem().isEmpty(), "A valid SOCKS5 configuration was rejected.");
    check(config.displayLabel() == QStringLiteral("proxy.example.com:1080"),
          "The fallback label is wrong: " + config.displayLabel().toStdString());
    config.label = QStringLiteral("  Arbeit  ");
    check(config.displayLabel() == QStringLiteral("Arbeit"), "The label was not trimmed.");

    config.port = 0;
    check(!config.validationProblem().isEmpty(), "Port 0 was accepted.");
    config.port = 65536;
    check(!config.validationProblem().isEmpty(), "A port above the range was accepted.");
    config.port = 65535;
    check(config.validationProblem().isEmpty(), "The highest valid port was rejected.");

    // A TLS connection to the proxy has to be refused rather than downgraded,
    // which would put the proxy credentials on the wire in the clear.
    config.type = SpaceProxyType::httpsConnect;
    check(!config.validationProblem().isEmpty(), "A TLS proxy was silently accepted.");

    SpaceProxyConfig plain;
    plain.host = QStringLiteral("proxy.example.com");
    check(!SpaceProxyController::hasUnenforceableRules(plain),
          "The default loopback exceptions were reported as unenforceable.");
    plain.excludedDomains.append(QStringLiteral("intranet.test"));
    check(SpaceProxyController::hasUnenforceableRules(plain),
          "A custom exception was not reported as unenforceable.");
    SpaceProxyConfig matched;
    matched.host = QStringLiteral("proxy.example.com");
    matched.matchDomains = {QStringLiteral("work.example")};
    check(SpaceProxyController::hasUnenforceableRules(matched),
          "A match list was not reported as unenforceable.");
}

void checkStore(const std::filesystem::path &directory) {
    SpaceProxyStore store(directory);
    check(store.spaces().empty(), "A fresh store already holds configurations.");
    check(!store.config(QStringLiteral("Arbeit")).has_value(), "An unknown space returned a configuration.");

    SpaceProxyConfig config;
    config.host = QStringLiteral("  http://proxy.example.com/  ");
    config.port = 8080;
    config.type = SpaceProxyType::httpConnect;
    config.username = QStringLiteral(" scout ");
    config.password = QStringLiteral("s3cret");
    config.matchDomains = {QStringLiteral(" work.example "), QStringLiteral("  ")};
    check(store.set(QStringLiteral("Arbeit"), config).isEmpty(), "Storing a configuration failed.");

    const auto stored = store.config(QStringLiteral("Arbeit"));
    check(stored.has_value(), "The stored configuration was not found.");
    check(stored->host == QStringLiteral("proxy.example.com"), "The host was not cleaned before storing.");
    check(stored->username == QStringLiteral("scout"), "The user name was not trimmed.");
    check(stored->matchDomains == QStringList{QStringLiteral("work.example")},
          "Empty entries were not dropped from the domain list.");
    check(stored->password.isEmpty(), "The password stayed in the in-memory configuration.");

    // The password belongs in the keychain, never in the settings file.
    QFile file(QString::fromStdString((directory / "space-proxies.json").string()));
    check(file.open(QIODevice::ReadOnly), "The settings file is missing.");
    const QByteArray contents = file.readAll();
    check(!contents.contains("s3cret"), "The password was written to the settings file.");
    check(!contents.contains("\"password\""), "The settings file has a password field.");
    check(contents.contains("HTTP CONNECT"), "The proxy kind was not stored in the WebKit wording.");

    // An invalid configuration must not overwrite a working one.
    SpaceProxyConfig broken;
    broken.host = QString();
    check(!store.set(QStringLiteral("Arbeit"), broken).isEmpty(), "An invalid configuration was accepted.");
    check(store.config(QStringLiteral("Arbeit"))->host == QStringLiteral("proxy.example.com"),
          "A rejected configuration changed the stored one.");

    const bool keychainWorks = !store.resolved(QStringLiteral("Arbeit"), *stored).password.isEmpty();
    if (keychainWorks) {
        check(store.resolved(QStringLiteral("Arbeit"), *stored).password == QStringLiteral("s3cret"),
              "The password did not come back from the keychain.");
    } else {
        std::cout << "NOTE: no usable keychain in this environment; password round-trip not checked.\n";
    }

    // Renaming a space has to take the configuration and the password with it.
    check(store.rename(QStringLiteral("Arbeit"), QStringLiteral("Beruf")).isEmpty(), "Renaming failed.");
    check(!store.config(QStringLiteral("Arbeit")).has_value(), "The old space name still has a configuration.");
    const auto renamed = store.config(QStringLiteral("Beruf"));
    check(renamed.has_value() && renamed->host == QStringLiteral("proxy.example.com"),
          "The configuration did not move to the new space name.");
    if (keychainWorks) {
        check(store.resolved(QStringLiteral("Beruf"), *renamed).password == QStringLiteral("s3cret"),
              "The password did not move to the new space name.");
    }

    SpaceProxyConfig second;
    second.host = QStringLiteral("proxy2.example.com");
    check(store.set(QStringLiteral("Alltag"), second).isEmpty(), "Storing a second configuration failed.");
    const std::vector<QString> spaces = store.spaces();
    check(spaces.size() == 2 && spaces.front() == QStringLiteral("Alltag"),
          "The configuration listing is not sorted by space.");

    {
        // Everything but the password has to survive a restart.
        SpaceProxyStore reopened(directory);
        const auto persisted = reopened.config(QStringLiteral("Beruf"));
        check(persisted.has_value(), "The configuration did not survive a restart.");
        check(persisted->port == 8080 && persisted->type == SpaceProxyType::httpConnect,
              "Port or kind did not survive a restart.");
        check(persisted->username == QStringLiteral("scout"), "The user name did not survive a restart.");
    }

    check(store.remove(QStringLiteral("Beruf")).isEmpty(), "Removing a configuration failed.");
    check(!store.config(QStringLiteral("Beruf")).has_value(), "The configuration was not removed.");
    check(store.remove(QStringLiteral("Alltag")).isEmpty(), "Removing the second configuration failed.");
    // Removing an unknown space is not an error.
    check(store.remove(QStringLiteral("Beruf")).isEmpty(), "Removing twice reported an error.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-proxy-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT SPACE PROXY INTEGRATION FAIL: Could not create an isolated directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    try {
        checkConfigValidation();
        checkStore(std::filesystem::path(home.path().toStdString()) / "store");

        QTcpServer proxyServer;
        check(proxyServer.listen(QHostAddress::LocalHost, 0), "Could not start the proxy fixture.");
        CountingServer proxy(proxyServer, QByteArrayLiteral("Proxied Fixture"), true);

        QTcpServer directServer;
        check(directServer.listen(QHostAddress::LocalHost, 0), "Could not start the loopback fixture.");
        CountingServer direct(directServer, QByteArrayLiteral("Direct Fixture"), false);

        SpaceProxyConfig config;
        config.type = SpaceProxyType::httpConnect;
        config.host = QStringLiteral("127.0.0.1");
        config.port = proxyServer.serverPort();
        check(config.validationProblem().isEmpty(),
              "The fixture configuration was rejected: " + config.validationProblem().toStdString());

        // A rejected configuration must not leave a proxy behind.
        SpaceProxyConfig unusable = config;
        unusable.type = SpaceProxyType::httpsConnect;
        check(!SpaceProxyController::apply(unusable).isEmpty(), "A TLS proxy configuration was accepted.");
        check(SpaceProxyController::appliedLabel().isEmpty(),
              "A rejected configuration left a proxy in front of the application.");

        // The proxy has to be in place before the engine builds its network
        // context; this is the ordering the app uses in main.cpp.
        check(SpaceProxyController::apply(config).isEmpty(), "Applying the configuration failed.");
        check(SpaceProxyController::matchesApplied(config),
              "The applied proxy is not the configured one.");
        check(!SpaceProxyController::matchesApplied(std::nullopt),
              "An active proxy was reported as matching 'no proxy'.");

        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "proxy-integration",
            .storagePath = (home.path() + QStringLiteral("/storage")).toStdString(),
            .cachePath = (home.path() + QStringLiteral("/cache")).toStdString(),
            .persistent = true,
        });
        auto generic = profile.createPage("proxy-user", yobro::engine::PageOwner::user, false);
        auto *page = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(generic.get());
        check(page != nullptr, "Could not create the proxy test page.");

        // This host does not exist, so only a proxy can answer for it.
        const std::string proxiedTarget = "http://proxied.invalid/page";
        const std::string loopbackTarget =
            QStringLiteral("http://127.0.0.1:%1/local").arg(directServer.serverPort()).toStdString();

        loadAndSettle(*page, proxiedTarget);
        check(titleOf(*page) == QStringLiteral("Proxied Fixture"),
              "The request did not go through the proxy. Observed title: " + titleOf(*page).toStdString());
        check(proxy.requests() >= 1, "The proxy fixture never saw a request.");

        // Chromium exempts loopback by itself, which is what keeps a local
        // development server reachable while a proxy is active. This is why the
        // WebKit build's default exceptions need no extra handling here.
        loadAndSettle(*page, loopbackTarget);
        check(titleOf(*page) == QStringLiteral("Direct Fixture"),
              "A loopback address did not bypass the proxy. Observed title: " + titleOf(*page).toStdString());
        check(direct.requests() >= 1, "The loopback fixture never saw a request.");

        // The limitation the shell has to work around: once the engine runs, the
        // proxy can no longer be changed. If this ever starts working, the
        // restart prompt in the settings can go away.
        const int proxiedBefore = proxy.requests();
        check(SpaceProxyController::apply(std::nullopt).isEmpty(), "Removing the proxy reported a problem.");
        loadAndSettle(*page, "http://proxied.invalid/again");
        check(titleOf(*page) == QStringLiteral("Proxied Fixture")
                  && proxy.requests() > proxiedBefore,
              "Qt WebEngine now honours a proxy change at runtime; the restart prompt is obsolete.");

        proxyServer.close();
        directServer.close();
        std::cout << "QT SPACE PROXY INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT SPACE PROXY INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
