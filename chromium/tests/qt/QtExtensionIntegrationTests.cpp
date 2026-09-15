#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QSettings>
#include <QUrl>
#include <QVariant>
#include <QWebEngineExtensionInfo>
#include <QWebEngineExtensionManager>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void waitUntil(const std::function<bool()> &ready, std::chrono::milliseconds timeout, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (ready()) return;
        QTest::qWait(10);
    }
    throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path &path, const std::string &body) {
    std::ofstream output(path, std::ios::binary);
    output << body;
    check(output.good(), "Could not write extension fixture.");
}

void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("served").toBool()) return;
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                socket->setProperty("served", true);
                const QByteArray body = QByteArrayLiteral("<!doctype html><title>Extension Fixture</title><p id='target'>ready</p>");
                QByteArray response = QByteArrayLiteral(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                );
                response += QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

QString marker(yobro::qtwebengine::QtBrowserPage &page) {
    std::optional<QString> value;
    page.view()->page()->runJavaScript(
        QStringLiteral("document.documentElement.getAttribute('data-yobro-extension') || ''"),
        [&value](const QVariant &result) { value = result.toString(); }
    );
    waitUntil([&] { return value.has_value(); }, 5s, "Extension marker JavaScript timed out.");
    return *value;
}

void load(yobro::qtwebengine::QtBrowserPage &page, const QString &url) {
    (void)page.navigate(url.toStdString());
    waitUntil([&] {
        const auto state = page.state();
        return !state.loading && state.title == "Extension Fixture";
    }, 20s, "Extension fixture page did not load.");
}

std::unique_ptr<yobro::engine::BrowserProfile> openProfile(
    yobro::qtwebengine::QtBrowserEngine &engine,
    const std::filesystem::path &root
) {
    return engine.openProfile({
        .id = "extension-test",
        .storagePath = (root / "storage").string(),
        .cachePath = (root / "cache").string(),
        .persistent = true,
    });
}

void unloadForCleanExit(QWebEngineExtensionManager &manager, const QWebEngineExtensionInfo &extension) {
    bool unloaded = false;
    QObject::connect(&manager, &QWebEngineExtensionManager::unloadFinished,
                     &manager, [&unloaded, id = extension.id()](const QWebEngineExtensionInfo &value) {
                         if (value.id() == id) unloaded = true;
                     });
    manager.unloadExtension(extension);
    waitUntil([&] { return unloaded; }, 10s, "MV3 fixture did not unload cleanly.");
    QElapsedTimer settled;
    settled.start();
    while (settled.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }
}

void installForRestart(const std::filesystem::path &root, const std::filesystem::path &source) {
    yobro::qtwebengine::QtBrowserEngine engine;
    auto profile = openProfile(engine, root);
    auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
    check(qtProfile != nullptr, "Could not create persistent extension profile.");
    auto *manager = qtProfile->persistentProfile()->extensionManager();
    std::optional<QWebEngineExtensionInfo> installed;
    QObject::connect(manager, &QWebEngineExtensionManager::installFinished,
                     manager, [&installed](const QWebEngineExtensionInfo &extension) {
                         installed = extension;
                     });
    manager->installExtension(QString::fromStdString(source.string()));
    waitUntil([&] { return installed.has_value(); }, 20s, "MV3 installation did not finish.");
    check(installed->isLoaded() && installed->isInstalled(), "MV3 fixture did not install.");
    QTest::qWait(yobro::qtwebengine::QtBrowserProfile::installSettleDelayMilliseconds);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    qtProfile->enableExtensionAfterInstall(*installed);
    if (yobro::qtwebengine::QtBrowserProfile::extensionSwitchingWorks()) {
        waitUntil([&] {
            for (const auto &extension : manager->extensions())
                if (extension.id() == installed->id()) return extension.isEnabled();
            return false;
        }, 10s, "MV3 fixture did not enable before restart.");
    } else {
        // Qt 6.11.2 cannot switch an extension on without crashing, so the state
        // is only recorded. This branch asserts the part that does work; it turns
        // into the branch above as soon as Qt is fixed.
        bool enabled = false;
        for (const auto &extension : manager->extensions())
            if (extension.id() == installed->id()) enabled = extension.isEnabled();
        check(!enabled, "Qt now enables extensions by itself; re-check extensionSwitchingWorks().");
    }
    writeFile(root / "extension-id.txt", installed->id().toStdString());
    QTest::qWait(500);
    QWebEngineExtensionInfo current;
    for (const auto &extension : manager->extensions())
        if (extension.id() == installed->id()) current = extension;
    check(current.isLoaded(), "Enabled MV3 fixture disappeared before restart.");
    unloadForCleanExit(*manager, current);
}

void verifyAfterRestart(const std::filesystem::path &root, const QString &url) {
    std::ifstream idInput(root / "extension-id.txt");
    std::string storedId;
    std::getline(idInput, storedId);
    check(!storedId.empty(), "Installed extension id was not persisted by the first process.");
    const QString wantedId = QString::fromStdString(storedId);

    yobro::qtwebengine::QtBrowserEngine engine;
    auto profile = openProfile(engine, root);
    auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
    check(qtProfile != nullptr, "Could not reopen persistent extension profile.");
    auto *manager = qtProfile->persistentProfile()->extensionManager();
    QObject::connect(manager, &QWebEngineExtensionManager::loadFinished, manager,
                     [qtProfile](const QWebEngineExtensionInfo &extension) {
                         qtProfile->restoreExtensionState(extension);
                     });
    qtProfile->loadInstalledExtensions();
    auto genericPage = profile->createPage("extension-restarted-user", yobro::engine::PageOwner::user, false);
    auto *page = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(genericPage.get());
    check(page != nullptr, "Could not create restarted extension page.");
    waitUntil([&] {
        for (const auto &extension : manager->extensions()) {
            if (extension.id() != wantedId) continue;
            qtProfile->restoreExtensionState(extension);
            return extension.isLoaded() && extension.isInstalled();
        }
        return false;
    }, 20s, "Installed MV3 extension was not loaded by the restarted profile.");
    load(*page, url);
    if (yobro::qtwebengine::QtBrowserProfile::extensionSwitchingWorks()) {
        waitUntil([&] {
            for (const auto &extension : manager->extensions())
                if (extension.id() == wantedId) return extension.isEnabled();
            return false;
        }, 10s, "Persisted MV3 activation was not restored after restart.");
        waitUntil([&] { return marker(*page) == QStringLiteral("active"); },
                  10s, "Restored MV3 content script did not run after process restart.");
    } else {
        // Without activation the content script must not run. This is the honest
        // state of extensions on this Qt version, recorded so a Qt update shows up
        // here as a failing test rather than as a silent change.
        QTest::qWait(1'500);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        check(marker(*page) != QStringLiteral("active"),
              "A content script ran although the extension was never enabled; "
              "re-check extensionSwitchingWorks().");
    }

    QWebEngineExtensionInfo restored;
    for (const auto &extension : manager->extensions())
        if (extension.id() == wantedId) restored = extension;
    check(restored.isLoaded(), "Restored MV3 extension disappeared before shutdown.");
    unloadForCleanExit(*manager, restored);
}

void runChild(const QStringList &arguments) {
    QString failures;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments(arguments);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start();
        if (!child.waitForStarted(10'000)) {
            failures += QStringLiteral("attempt %1 could not start: %2\n").arg(attempt).arg(child.errorString());
            continue;
        }
        const bool finished = [&] {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 45'000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                if (child.state() == QProcess::NotRunning) return true;
                QTest::qWait(10);
            }
            return false;
        }();
        if (finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0) return;
        if (!finished) child.kill();
        child.waitForFinished(2'000);
        failures += QStringLiteral("attempt %1 exit=%2 status=%3 output=%4\n")
            .arg(attempt).arg(child.exitCode()).arg(static_cast<int>(child.exitStatus()))
            .arg(QString::fromUtf8(child.readAll()).trimmed());
        QTest::qWait(250);
    }
    throw std::runtime_error("Extension restart child failed after retries: " + failures.toStdString());
}

} // namespace

int main(int argc, char **argv) {
    const QString mode = argc > 1 ? QString::fromUtf8(argv[1]) : QString();
    const QString modeRoot = argc > 2 ? QString::fromUtf8(argv[2]) : QString();
    const QString modeValue = argc > 3 ? QString::fromUtf8(argv[3]) : QString();
    if (!mode.isEmpty()) argc = 1;
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    try {
        if (mode == QStringLiteral("--install-for-restart")) {
            installForRestart(modeRoot.toStdString(), modeValue.toStdString());
            return 0;
        }
        if (mode == QStringLiteral("--verify-after-restart")) {
            verifyAfterRestart(modeRoot.toStdString(), modeValue);
            return 0;
        }
        QTemporaryDir home(QStringLiteral("/tmp/yobro-extension-qt-XXXXXX"));
        check(home.isValid(), "Could not create isolated extension test directory.");
        const std::filesystem::path root = home.path().toStdString();
        const auto source = root / "extension-source";
        std::filesystem::create_directories(source);
        writeFile(source / "manifest.json", R"JSON({"manifest_version":3,"name":"YOBRO Extension Fixture","version":"1.0","content_scripts":[{"matches":["http://127.0.0.1/*"],"js":["content.js"],"run_at":"document_end"}]})JSON");
        writeFile(source / "content.js", "document.documentElement.setAttribute('data-yobro-extension', 'active');");

        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "Could not start extension fixture server.");
        serveFixture(server);
        const QString url = QStringLiteral("http://127.0.0.1:%1/page").arg(server.serverPort());

        runChild({QStringLiteral("--install-for-restart"), home.path(),
                  QString::fromStdString(source.string())});
        runChild({QStringLiteral("--verify-after-restart"), home.path(), url});

        std::cout << "QT EXTENSION RESTART INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT EXTENSION INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
