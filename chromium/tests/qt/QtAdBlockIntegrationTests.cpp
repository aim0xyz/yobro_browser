#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/AdBlockInterceptor.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using yobro::spike::AdBlockInterceptor;

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

void respond(QTcpSocket *socket, const QByteArray &contentType, const QByteArray &body) {
    QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ") + contentType
        + QByteArrayLiteral("\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ")
        + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
    socket->write(response);
    socket->disconnectFromHost();
}

/// A one-pixel PNG, small enough to inline.
QByteArray pixel() {
    return QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="
    );
}

/// The page loads two images from the other loopback origin: one on an
/// advertising path, one on a harmless path. Whether the ad path is blocked is
/// what the test observes.
QByteArray pageBody(const QString &otherOrigin) {
    return QStringLiteral(R"HTML(<!doctype html>
<meta charset="utf-8">
<title>AdBlock Fixture</title>
<div id="status">pending</div>
<script>
const results = {};
function record(name, value) {
  results[name] = value;
  if (Object.keys(results).length === 2)
    document.querySelector("#status").textContent = "ad=" + results.ad + " plain=" + results.plain;
}
function probe(name, url) {
  const image = new Image();
  image.onload = () => record(name, "loaded");
  image.onerror = () => record(name, "failed");
  image.src = url;
}
probe("ad", "%1/ads/pixel.png?cachebust=" + Math.random());
probe("plain", "%1/assets/pixel.png?cachebust=" + Math.random());
</script>)HTML").arg(otherOrigin).toUtf8();
}

/// The status element only exists once the fixture document is in place, so a
/// missing element is reported as an empty string rather than as a failure.
QString statusOf(QWebEnginePage &page) {
    bool ready = false;
    QString value;
    page.runJavaScript(
        QStringLiteral("document.querySelector('#status')?.textContent || ''"),
        [&ready, &value](const QVariant &result) { value = result.toString(); ready = true; }
    );
    waitUntil([&ready] { return ready; }, 5s, "status read");
    return value;
}

void waitForStatus(QWebEnginePage &page, const QString &expected) {
    QString observed;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 20'000) {
        observed = statusOf(page);
        if (observed == expected) return;
        QTest::qWait(50);
    }
    fail("Timed out waiting for status " + expected.toStdString() + "; observed " + observed.toStdString() + ".");
}

/// Loads a URL and waits for that specific load to finish. Waiting only for the
/// status text would pass immediately whenever the previous load left the same
/// text behind.
void loadAndWait(QWebEnginePage &page, const QString &url, const QString &expectedStatus) {
    bool finished = false;
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(&page, &QWebEnginePage::loadFinished, &page, [&finished, connection](bool) {
        finished = true;
        QObject::disconnect(*connection);
    });
    page.load(QUrl(url));
    waitUntil([&finished] { return finished; }, 20s, "page load of " + url.toStdString());
    waitForStatus(page, expectedStatus);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-adblock-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT ADBLOCK INTEGRATION FAIL: Could not create an isolated profile directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    try {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "Could not start the ad-block loopback fixture.");
        // Two origins on the same loopback socket: the page is served under the
        // name, the subresources under the address, which makes them a different
        // site and therefore subject to the third-party rules.
        const QString pageOrigin = QStringLiteral("http://localhost:%1").arg(server.serverPort());
        const QString assetOrigin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &assetOrigin] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &assetOrigin] {
                    if (socket->property("served").toBool()) { (void)socket->readAll(); return; }
                    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    socket->setProperty("served", true);
                    if (request.contains("pixel.png"))
                        respond(socket, QByteArrayLiteral("image/png"), pixel());
                    else
                        respond(socket, QByteArrayLiteral("text/html; charset=utf-8"), pageBody(assetOrigin));
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });

        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "adblock-integration",
            .storagePath = (home.path() + QStringLiteral("/storage")).toStdString(),
            .cachePath = (home.path() + QStringLiteral("/cache")).toStdString(),
            .persistent = true,
        });
        AdBlockInterceptor interceptor;
        profile.persistentProfile()->setUrlRequestInterceptor(&interceptor);

        QWebEngineView view;
        auto page = std::make_unique<QWebEnginePage>(profile.persistentProfile(), &view);
        view.setPage(page.get());
        view.resize(400, 300);
        view.show();

        // With the filter on, the advertising path must not reach the network
        // while the harmless image still loads.
        loadAndWait(*page, pageOrigin + QStringLiteral("/page"), QStringLiteral("ad=failed plain=loaded"));
        check(interceptor.blockedCount() >= 1, "The interceptor did not report a blocked request.");

        // Switched off, the same page must load everything again.
        const std::uint64_t blockedBefore = interceptor.blockedCount();
        interceptor.setEnabled(false);
        loadAndWait(*page, pageOrigin + QStringLiteral("/page"), QStringLiteral("ad=loaded plain=loaded"));
        check(interceptor.blockedCount() == blockedBefore,
              "The disabled interceptor still blocked a request.");
        interceptor.setEnabled(true);

        // A top-level navigation must arrive without its click identifiers.
        loadAndWait(*page, pageOrigin + QStringLiteral("/page?id=7&utm_source=mail&gclid=abc"),
                    QStringLiteral("ad=failed plain=loaded"));
        check(page->url().query() == QStringLiteral("id=7"),
              "The cleaned address lost or kept the wrong parameters: "
              + page->url().toString().toStdString());

        // A normal address must survive untouched, so nothing reloads in a loop.
        loadAndWait(*page, pageOrigin + QStringLiteral("/page?id=9"),
                    QStringLiteral("ad=failed plain=loaded"));
        check(page->url().query() == QStringLiteral("id=9"),
              "A clean address was rewritten anyway.");

        server.close();
        std::cout << "QT ADBLOCK INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT ADBLOCK INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
