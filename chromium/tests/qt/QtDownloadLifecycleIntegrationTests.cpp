#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtDownloadFilename.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/Json.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QFile>
#include <QHostAddress>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using yobro::core::Json;
using namespace std::chrono_literals;

constexpr qint64 slowDownloadSize = 2 * 1024 * 1024;
const QByteArray acceptedBody = QByteArrayLiteral("accepted user download\n");
const QByteArray privateBody = QByteArrayLiteral("private download\n");
const QByteArray promptBody = QByteArrayLiteral("accepted through prompt seam\n");
const QByteArray collisionBody = QByteArrayLiteral("collision download\n");
const QByteArray timeoutRetryBody = QByteArrayLiteral("timeout retry download\n");

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }

/// Qt complains when a profile is released while one of its pages is still
/// alive, and then keeps going. That warning meant a real ownership bug here:
/// the download library kept native pages for late download requests, and the
/// shell releases the profile right after asking the library to shut down.
/// A warning nobody fails on is a warning nobody fixes, so this test fails.
std::atomic_bool sawProfileReleaseWarning{false};
QtMessageHandler previousMessageHandler = nullptr;

void recordQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (message.contains(QStringLiteral("WebEnginePage still not deleted")))
        sawProfileReleaseWarning.store(true);
    if (previousMessageHandler)
        previousMessageHandler(type, context, message);
}
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

const Json &required(const Json &value, std::string_view key) {
    const Json *field = value.find(key);
    if (!field) fail("Missing JSON field: " + std::string(key));
    return *field;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("Could not read file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path &path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) fail("Could not write file: " + path.string());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) fail("Could not finish writing file: " + path.string());
}

bool directoryEmpty(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) return true;
    return std::filesystem::directory_iterator(path) == std::filesystem::directory_iterator();
}

void spinUntil(const std::function<bool()> &predicate, std::chrono::milliseconds timeout, std::string_view expectation) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        QTest::qWait(10);
    check(predicate(), "Timed out waiting for " + std::string(expectation) + ".");
}

std::vector<Json> entriesFor(const yobro::qtwebengine::QtBrowserLibrary &library, std::string_view source) {
    std::vector<Json> matches;
    const Json values = library.downloads();
    if (!values.isArray()) return matches;
    for (const Json &entry : values.asArray()) {
        const Json *candidate = entry.isObject() ? entry.find("source") : nullptr;
        if (candidate && candidate->isString() && candidate->asString() == source)
            matches.push_back(entry);
    }
    return matches;
}

std::optional<Json> entryFor(const yobro::qtwebengine::QtBrowserLibrary &library, std::string_view source) {
    const std::vector<Json> matches = entriesFor(library, source);
    return matches.empty() ? std::nullopt : std::optional<Json>(matches.front());
}

Json waitForEntry(
    const yobro::qtwebengine::QtBrowserLibrary &library,
    std::string_view source,
    const std::function<bool(const Json &)> &predicate,
    std::string_view expectation,
    std::chrono::milliseconds timeout = 20s
) {
    std::optional<Json> matched;
    spinUntil([&] {
        matched = entryFor(library, source);
        return matched && predicate(*matched);
    }, timeout, expectation);
    return *matched;
}

void sendBody(QTcpSocket *socket, const QByteArray &body, const QByteArray &filename) {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\""
    );
    response += filename;
    response += QByteArrayLiteral("\"\r\nConnection: close\r\nContent-Length: ");
    response += QByteArray::number(body.size());
    response += QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void sendSlow(QTcpSocket *socket, QByteArray filename, char byte = 'S') {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\""
    );
    response += filename;
    response += QByteArrayLiteral("\"\r\nConnection: close\r\nContent-Length: ");
    response += QByteArray::number(slowDownloadSize);
    response += QByteArrayLiteral("\r\n\r\n");
    socket->write(response);
    socket->setProperty("sent", qint64{0});
    auto *timer = new QTimer(socket);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, socket, [socket, timer, byte] {
        if (socket->state() != QAbstractSocket::ConnectedState) {
            timer->stop();
            return;
        }
        qint64 sent = socket->property("sent").toLongLong();
        const qint64 count = std::min<qint64>(32 * 1024, slowDownloadSize - sent);
        if (count <= 0) {
            timer->stop();
            socket->disconnectFromHost();
            return;
        }
        socket->write(QByteArray(static_cast<qsizetype>(count), byte));
        sent += count;
        socket->setProperty("sent", sent);
        if (sent == slowDownloadSize) {
            timer->stop();
            socket->disconnectFromHost();
        }
    });
    timer->start();
}

void sendInterrupted(QTcpSocket *socket) {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"interrupted.bin\"\r\n"
        "Connection: close\r\n"
        "Content-Length: 2097152\r\n\r\n"
    );
    response += QByteArray(64 * 1024, 'I');
    socket->write(response);
    socket->flush();
    QTimer::singleShot(40, socket, [socket] { socket->abort(); });
}

void serveFixtures(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &server] {
                if (socket->property("responded").toBool()) {
                    (void)socket->readAll();
                    return;
                }
                QByteArray request = socket->property("request").toByteArray();
                request += socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                socket->setProperty("responded", true);
                const QByteArray line = request.left(request.indexOf("\r\n"));
                const QList<QByteArray> parts = line.split(' ');
                const QByteArray path = parts.size() >= 2 ? parts.at(1) : QByteArray();
                if (path == QByteArrayLiteral("/accepted")) {
                    sendBody(socket, acceptedBody, QByteArrayLiteral("accepted-user.bin"));
                } else if (path == QByteArrayLiteral("/prompt-accepted")) {
                    sendBody(socket, promptBody, QByteArrayLiteral("prompt-accepted.bin"));
                } else if (path == QByteArrayLiteral("/prompt-denied")) {
                    sendBody(socket, QByteArrayLiteral("must not persist\n"), QByteArrayLiteral("prompt-denied.bin"));
                } else if (path == QByteArrayLiteral("/private")) {
                    sendBody(socket, privateBody, QByteArrayLiteral(".env"));
                } else if (path == QByteArrayLiteral("/collision")) {
                    sendBody(socket, collisionBody, QByteArrayLiteral("collision.bin"));
                } else if (path == QByteArrayLiteral("/timeout-retry")) {
                    const int attempt = server.property("timeoutRetryAttempts").toInt() + 1;
                    server.setProperty("timeoutRetryAttempts", attempt);
                    if (attempt == 1) {
                        QTimer::singleShot(10'000, socket, [socket] { socket->abort(); });
                    } else {
                        sendBody(socket, timeoutRetryBody, QByteArrayLiteral("timeout-retry.bin"));
                    }
                } else if (path == QByteArrayLiteral("/agent-native")) {
                    sendSlow(socket, QByteArrayLiteral("agent-native.bin"), 'N');
                } else if (path == QByteArrayLiteral("/interrupt")) {
                    sendInterrupted(socket);
                } else if (path == QByteArrayLiteral("/late")) {
                    sendSlow(socket, QByteArrayLiteral("late.bin"), 'L');
                } else if (path == QByteArrayLiteral("/agent")) {
                    sendSlow(socket, QByteArrayLiteral("agent-active.bin"), 'A');
                } else if (path == QByteArrayLiteral("/user-survives-end")) {
                    sendSlow(socket, QByteArrayLiteral("user-survives-end.bin"), 'U');
                } else if (path == QByteArrayLiteral("/shutdown")) {
                    sendSlow(socket, QByteArrayLiteral("shutdown-active.bin"), 'X');
                } else {
                    sendBody(socket, QByteArrayLiteral("not found\n"), QByteArrayLiteral("not-found.txt"));
                }
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

void nativeDownload(yobro::qtwebengine::QtBrowserPage &page, std::string_view url) {
    page.view()->page()->download(QUrl(QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()))));
}

void checkTerminalCleanup(const std::filesystem::path &profileDirectory) {
    spinUntil(
        [&] { return directoryEmpty(profileDirectory / "download-staging"); },
        5s,
        "owned download staging cleanup"
    );
}

void testFilenameParity() {
    const char32_t emojiScalar = 0x1f600;
    const QString emoji = QString::fromUcs4(&emojiScalar, 1);
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(emoji + QStringLiteral(" report.txt"))
            == emoji + QStringLiteral(" report.txt"),
        "Filename sanitizer removed a valid non-BMP character."
    );
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(
            QString(QChar(0x202e)) + QStringLiteral("evil") + QString(QChar(0x200d)) + QStringLiteral(".txt")
        ) == QStringLiteral("evil.txt"),
        "Filename sanitizer retained Unicode format controls."
    );
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(QStringLiteral("folder/name\n.txt"))
            == QStringLiteral("name.txt"),
        "Filename sanitizer did not apply last-component/control cleanup."
    );
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(QStringLiteral(".env"))
            == QStringLiteral("Download.env"),
        "Filename sanitizer did not prefix a hidden filename."
    );
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(QStringLiteral(".."))
            == QStringLiteral("Download"),
        "Filename sanitizer did not replace a traversal component."
    );
    QString longName(179, QChar('a'));
    longName += emoji;
    const QString expected = longName;
    longName += QStringLiteral("tail");
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(longName) == expected,
        "Filename sanitizer did not truncate at 180 grapheme clusters."
    );
    QString combined;
    for (int index = 0; index < 180; ++index)
        combined += QStringLiteral("e\u0301");
    check(
        yobro::qtwebengine::sanitizedDownloadFilename(combined + QStringLiteral("x")) == combined,
        "Filename sanitizer split or miscounted combining graphemes."
    );
}

void testRestartAndCorruptState(const std::filesystem::path &root) {
    const auto restartProfileDirectory = root / "restart-profile";
    const auto restartDownloads = root / "restart-downloads";
    std::filesystem::create_directories(restartProfileDirectory / "download-staging" / "orphan-id");
    std::filesystem::create_directories(restartDownloads);
    writeFile(restartProfileDirectory / "download-staging" / "orphan-id" / "payload.yobro-part", "partial");
    writeFile(
        restartProfileDirectory / "downloads.json",
        R"JSON([{"id":"11111111-1111-4111-8111-111111111111","name":"restart.bin","source":"https://restart.invalid/file","date":"2026-09-13T12:00:00Z","state":"downloading","received":7,"expected":99,"path":"/unsafe/legacy-part","error":null}])JSON"
    );
    {
        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "download-restart",
            .storagePath = (restartProfileDirectory / "storage").string(),
            .cachePath = (restartProfileDirectory / "cache").string(),
            .persistent = true,
        });
        yobro::qtwebengine::QtBrowserLibrary library(profile, restartProfileDirectory, restartDownloads);
        const auto entry = entryFor(library, "https://restart.invalid/file");
        check(entry.has_value(), "Restart conversion lost the persisted download.");
        check(required(*entry, "state").asString() == "interrupted", "Restart did not convert downloading to interrupted.");
        check(required(*entry, "path").isNull(), "Restart exposed a non-final legacy path.");
        check(required(*entry, "error").asString() == "Durch Neustart unterbrochen.", "Restart interruption reason differs.");
        check(directoryEmpty(restartProfileDirectory / "download-staging"), "Restart did not clean owned staging.");
        library.shutdownDownloads();
    }
    const Json persisted = Json::parse(readFile(restartProfileDirectory / "downloads.json"));
    check(required(persisted.asArray().front(), "state").asString() == "interrupted", "Restart conversion was not persisted immediately.");

    const auto corruptProfileDirectory = root / "corrupt-profile";
    const auto corruptDownloads = root / "corrupt-downloads";
    std::filesystem::create_directories(corruptProfileDirectory);
    std::filesystem::create_directories(corruptDownloads);
    constexpr std::string_view corruptBytes = "{private-corrupt-download-state";
    writeFile(corruptProfileDirectory / "downloads.json", corruptBytes);
    {
        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "download-corrupt",
            .storagePath = (corruptProfileDirectory / "storage").string(),
            .cachePath = (corruptProfileDirectory / "cache").string(),
            .persistent = true,
        });
        yobro::qtwebengine::QtBrowserLibrary library(profile, corruptProfileDirectory, corruptDownloads);
        check(library.downloads().asArray().empty(), "Corrupt state produced download records.");
        library.shutdownDownloads();
    }
    int quarantineCount = 0;
    for (const auto &item : std::filesystem::directory_iterator(corruptProfileDirectory)) {
        const std::string name = item.path().filename().string();
        if (name.starts_with("downloads.json.corrupt-")) {
            ++quarantineCount;
            check(readFile(item.path()) == corruptBytes, "Quarantine did not preserve the corrupt bytes.");
        }
    }
    check(quarantineCount == 1, "Corrupt download state was not quarantined exactly once.");
    check(Json::parse(readFile(corruptProfileDirectory / "downloads.json")).asArray().empty(), "Fresh state after quarantine is not an empty array.");
}

void runLiveLifecycle(
    QApplication &application,
    const yobro::core::ProfilePaths &paths,
    const std::filesystem::path &downloadDirectory,
    const QString &baseUrl
) {
    yobro::qtwebengine::QtBrowserEngine engine;
    auto profile = engine.openProfile({
        .id = "download-lifecycle",
        .storagePath = paths.storage.string(),
        .cachePath = paths.cache.string(),
        .persistent = true,
    });
    auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
    check(qtProfile != nullptr, "Qt engine returned a non-Qt profile.");
    auto library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(
        *qtProfile,
        paths.profile,
        downloadDirectory
    );
    yobro::qtwebengine::QtEventLoop eventLoop;
    yobro::controller::BrowserSession session(
        std::move(profile),
        eventLoop,
        {
            .version = "0.6.3",
            .profileName = "Download lifecycle",
            .libraryAccess = true,
            .library = library,
        }
    );
    yobro::spike::BridgePolicyStore bridgePolicy(paths.bridgePolicy);
    yobro::spike::SpikeWindow window(session, *library, bridgePolicy, paths);
    QString revealedPath;
    int revealCount = 0;
    window.setFileRevealHandler([&](const QString &path) {
        ++revealCount;
        revealedPath = path;
        return true;
    });
    window.show();
    QTest::qWait(50);

    const auto url = [&baseUrl](QString path) { return (baseUrl + std::move(path)).toStdString(); };
    const std::string acceptedUrl = url(QStringLiteral("/accepted"));
    const std::string promptAcceptedUrl = url(QStringLiteral("/prompt-accepted"));
    const std::string promptDeniedUrl = url(QStringLiteral("/prompt-denied"));
    const std::string privateUrl = url(QStringLiteral("/private"));
    const std::string collisionUrl = url(QStringLiteral("/collision"));
    const std::string timeoutRetryUrl = url(QStringLiteral("/timeout-retry"));
    const std::string agentNativeUrl = url(QStringLiteral("/agent-native"));
    const std::string interruptedUrl = url(QStringLiteral("/interrupt"));
    const std::string lateUrl = url(QStringLiteral("/late"));
    const std::string agentUrl = url(QStringLiteral("/agent"));
    const std::string userSurvivesUrl = url(QStringLiteral("/user-survives-end"));
    const std::string shutdownUrl = url(QStringLiteral("/shutdown"));

    check(!session.userPages().empty(), "Visible shell did not create its initial user tab.");
    auto *userPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(session.userPages().front());
    check(userPage != nullptr, "Initial user tab is not a Qt page.");

    nativeDownload(*userPage, acceptedUrl);
    const Json accepted = waitForEntry(*library, acceptedUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "accepted visible user download");
    check(required(accepted, "name").asString() == "accepted-user.bin", "Accepted user filename differs.");
    check(required(accepted, "error").isNull(), "Accepted user download has an error.");
    const std::filesystem::path acceptedPath(required(accepted, "path").asString());
    check(acceptedPath.parent_path() == downloadDirectory, "Accepted user download escaped the configured directory.");
    check(readFile(acceptedPath) == acceptedBody.toStdString(), "Accepted user bytes differ.");
    checkTerminalCleanup(paths.profile);

    auto *downloadsButton = window.findChild<QPushButton *>(QStringLiteral("downloadsButton"));
    check(downloadsButton != nullptr && downloadsButton->isVisible(), "Visible Downloads button is missing.");
    QTest::mouseClick(downloadsButton, Qt::LeftButton);
    spinUntil([&] {
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("downloadLibraryDialog"));
        return dialog && dialog->isVisible();
    }, 2s, "visible download library dialog");
    auto *downloadsList = window.findChild<QListWidget *>(QStringLiteral("downloadsList"));
    check(downloadsList != nullptr && downloadsList->count() >= 1, "Download library did not show the accepted user item.");
    check(downloadsList->item(0)->text().contains(QStringLiteral("Abgeschlossen")), "Download library status is not WebKit-parity German text.");
    auto *revealButton = window.findChild<QPushButton *>(QStringLiteral("revealDownloadButton"));
    check(revealButton != nullptr && revealButton->isEnabled(), "Completed download did not enable Finder reveal.");
    QTest::mouseClick(revealButton, Qt::LeftButton);
    check(revealCount == 1, "Finder reveal handler was not invoked exactly once.");
    check(revealedPath == QString::fromStdString(acceptedPath.string()), "Finder reveal did not select the completed file path.");
    window.findChild<QDialog *>(QStringLiteral("downloadLibraryDialog"))->close();
    QTest::qWait(20);

    int promptAcceptCount = 0;
    library->setUserDownloadPrompt([&](const std::string &name, const std::string &directory) {
        ++promptAcceptCount;
        check(name == "prompt-accepted.bin", "Accept-prompt filename differs.");
        check(directory == downloadDirectory.string(), "Accept-prompt directory differs.");
        return true;
    });
    nativeDownload(*userPage, promptAcceptedUrl);
    (void)waitForEntry(*library, promptAcceptedUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "prompt-seam accepted user download");
    check(promptAcceptCount == 1, "Accept prompt seam was not invoked exactly once.");

    int promptDenyCount = 0;
    library->setUserDownloadPrompt([&](const std::string &name, const std::string &) {
        ++promptDenyCount;
        check(name == "prompt-denied.bin", "Deny-prompt filename differs.");
        return false;
    });
    nativeDownload(*userPage, promptDeniedUrl);
    spinUntil([&] { return promptDenyCount == 1; }, 5s, "prompt-seam rejection");
    QTest::qWait(100);
    check(!entryFor(*library, promptDeniedUrl).has_value(), "Rejected user download was persisted.");
    library->setUserDownloadPrompt({});

    auto &privateGeneric = session.newUserTab({}, true);
    auto *privatePage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&privateGeneric);
    check(privatePage != nullptr && privatePage->profile() == qtProfile->privateProfile(), "Private page profile differs.");
    nativeDownload(*privatePage, privateUrl);
    const Json privateEntry = waitForEntry(*library, privateUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "private-profile download");
    check(required(privateEntry, "name").asString() == "Download.env", "Leading-dot filename sanitizer differs from WebKit.");
    check(readFile(required(privateEntry, "path").asString()) == privateBody.toStdString(), "Private download bytes differ.");
    checkTerminalCleanup(paths.profile);

    writeFile(downloadDirectory / "collision.bin", "external sentinel");
    nativeDownload(*userPage, collisionUrl);
    const Json collision = waitForEntry(*library, collisionUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "collision-safe completion");
    check(required(collision, "name").asString() == "collision (1).bin", "Collision suffix differs from WebKit.");
    check(readFile(downloadDirectory / "collision.bin") == "external sentinel", "Existing collision target was overwritten.");
    check(readFile(required(collision, "path").asString()) == collisionBody.toStdString(), "Collision download bytes differ.");

    nativeDownload(*userPage, interruptedUrl);
    const Json interrupted = waitForEntry(*library, interruptedUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "failed";
    }, "real DownloadInterrupted failure");
    check(required(interrupted, "path").isNull(), "Interrupted download exposed a final path.");
    check(required(interrupted, "error").isString() && !required(interrupted, "error").asString().empty(), "Interrupted download has no concrete reason.");
    checkTerminalCleanup(paths.profile);

    nativeDownload(*userPage, lateUrl);
    const Json lateActive = waitForEntry(*library, lateUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "downloading"
            && required(entry, "received").asInteger() > 0;
    }, "late-collision active transfer");
    check(required(lateActive, "name").asString() == "late.bin", "Late-collision reserved filename differs.");
    check(required(lateActive, "path").isNull(), "Active transfer exposed its staging or reserved path.");
    writeFile(downloadDirectory / "late.bin", "late external sentinel");
    const Json lateFailed = waitForEntry(*library, lateUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "failed";
    }, "late external target no-clobber failure");
    check(required(lateFailed, "error").asString().find("already exists") != std::string::npos, "Late collision error differs.");
    check(readFile(downloadDirectory / "late.bin") == "late external sentinel", "Late external target was overwritten.");
    checkTerminalCleanup(paths.profile);

    auto &agentGeneric = session.newAgentTab();
    auto *agentPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&agentGeneric);
    check(agentPage != nullptr, "Agent tab is not a Qt page.");
    agentPage->view()->show();

    bool timeoutFinished = false;
    std::optional<std::string> timeoutError;
    library->startDownload(agentGeneric, timeoutRetryUrl, [&](std::optional<std::string> error) {
        timeoutError = std::move(error);
        timeoutFinished = true;
    });
    spinUntil([&] { return timeoutFinished; }, 7s, "first explicit agent-download timeout");
    check(
        timeoutError == "Chromium did not create the download request.",
        "First explicit timeout returned the wrong error."
    );
    bool retryStarted = false;
    std::optional<std::string> retryError;
    library->startDownload(agentGeneric, timeoutRetryUrl, [&](std::optional<std::string> error) {
        retryError = std::move(error);
        retryStarted = true;
    });
    spinUntil([&] { return retryStarted; }, 5s, "same-page/same-URL retry start");
    check(!retryError.has_value(), retryError.value_or("Same-URL retry failed."));
    const Json retry = waitForEntry(*library, timeoutRetryUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "same-page/same-URL retry completion");
    // The timed-out request left a retired native page behind, which is what
    // used to outlive the profile.
    check(library->retainedNativePages() >= 1, "The timed-out explicit download retired no native page.");
    check(readFile(required(retry, "path").asString()) == timeoutRetryBody.toStdString(), "Same-URL retry bytes differ.");

    bool agentStarted = false;
    std::optional<std::string> agentStartError;
    library->startDownload(agentGeneric, agentUrl, [&](std::optional<std::string> error) {
        agentStartError = std::move(error);
        agentStarted = true;
    });
    spinUntil([&] { return agentStarted; }, 5s, "agent download start completion");
    check(!agentStartError.has_value(), agentStartError.value_or("Agent download start failed."));
    (void)waitForEntry(*library, agentUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "downloading"
            && required(entry, "received").asInteger() > 0;
    }, "active explicit agent transfer");

    QSignalSpy agentPageLoaded(agentPage->view(), &QWebEngineView::loadFinished);
    const QString nativeUrl = QString::fromStdString(agentNativeUrl).toHtmlEscaped();
    agentPage->setHtml(QStringLiteral(
        "<!doctype html>"
        "<a id='native-download-1' href='%1' download>Download one</a>"
        "<a id='native-download-2' href='%1' download>Download two</a>"
    ).arg(nativeUrl));
    check(agentPageLoaded.wait(5'000), "Agent download-link page did not load.");
    bool firstNativeClickIssued = false;
    agentPage->view()->page()->runJavaScript(
        QStringLiteral("document.getElementById('native-download-1').click()"),
        [&firstNativeClickIssued](const QVariant &) { firstNativeClickIssued = true; }
    );
    spinUntil([&] { return firstNativeClickIssued; }, 2s, "first agent page-script download click");
    spinUntil([&] {
        const std::vector<Json> matches = entriesFor(*library, agentNativeUrl);
        return matches.size() == 1 && required(matches.front(), "state").asString() == "downloading";
    }, 5s, "first native agent download");
    bool secondNativeClickIssued = false;
    agentPage->view()->page()->runJavaScript(
        QStringLiteral("document.getElementById('native-download-2').click()"),
        [&secondNativeClickIssued](const QVariant &) { secondNativeClickIssued = true; }
    );
    spinUntil([&] { return secondNativeClickIssued; }, 2s, "second agent page-script download click");
    spinUntil([&] {
        const std::vector<Json> matches = entriesFor(*library, agentNativeUrl);
        return matches.size() == 2 && std::all_of(matches.begin(), matches.end(), [](const Json &entry) {
            return required(entry, "state").asString() == "downloading";
        });
    }, 5s, "two concurrent native same-URL agent downloads");

    nativeDownload(*userPage, userSurvivesUrl);
    (void)waitForEntry(*library, userSurvivesUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "downloading";
    }, "parallel user transfer before agent end");
    session.endAgentWorkspace();
    (void)waitForEntry(*library, agentUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "cancelled";
    }, "explicit agent transfer cancellation during end");
    spinUntil([&] {
        const std::vector<Json> matches = entriesFor(*library, agentNativeUrl);
        return matches.size() == 2 && std::all_of(matches.begin(), matches.end(), [](const Json &entry) {
            return required(entry, "state").asString() == "cancelled";
        });
    }, 5s, "native agent transfer cancellation during end");
    (void)waitForEntry(*library, userSurvivesUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "user transfer survival across agent end");
    check(!session.activeAgentPage(), "Agent end retained the agent page.");
    checkTerminalCleanup(paths.profile);

    nativeDownload(*userPage, shutdownUrl);
    (void)waitForEntry(*library, shutdownUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "downloading"
            && required(entry, "received").asInteger() > 0;
    }, "active transfer before app shutdown");
    library->shutdownDownloads();
    check(
        library->retainedNativePages() == 0,
        "Shutting down left native pages alive; they would outlive the profile."
    );
    const Json shutdown = waitForEntry(*library, shutdownUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "interrupted";
    }, "shutdown interruption");
    check(required(shutdown, "path").isNull(), "Shutdown exposed a non-final path.");
    check(required(shutdown, "error").asString() == "Browser closed before the download completed.", "Shutdown reason differs.");
    checkTerminalCleanup(paths.profile);

    const Json persisted = Json::parse(readFile(paths.profile / "downloads.json"));
    check(persisted.isArray(), "Final downloads.json root is not an array.");
    const auto persistedPrivate = std::find_if(persisted.asArray().begin(), persisted.asArray().end(), [&](const Json &entry) {
        const Json *source = entry.isObject() ? entry.find("source") : nullptr;
        return source && source->isString() && source->asString() == privateUrl;
    });
    check(persistedPrivate != persisted.asArray().end(), "Private download metadata was not persisted profile-wide.");

    window.close();
    QTest::qWait(20);
    (void)application;
}

void checkReloadedLifecycle(
    const yobro::core::ProfilePaths &paths,
    const std::filesystem::path &downloadDirectory,
    const QString &baseUrl
) {
    yobro::qtwebengine::QtBrowserProfile profile({
        .id = "download-lifecycle-reload",
        .storagePath = paths.storage.string(),
        .cachePath = paths.cache.string(),
        .persistent = true,
    });
    yobro::qtwebengine::QtBrowserLibrary library(profile, paths.profile, downloadDirectory);
    const auto url = [&baseUrl](QString path) { return (baseUrl + std::move(path)).toStdString(); };
    const auto checkRecords = [&](QString path, std::size_t count, std::string_view state) {
        const std::string source = url(std::move(path));
        const std::vector<Json> matches = entriesFor(library, source);
        check(matches.size() == count, "Reloaded terminal record count differs for " + source + ".");
        for (const Json &entry : matches)
            check(required(entry, "state").asString() == state, "Reloaded terminal state differs for " + source + ".");
        return matches;
    };

    (void)checkRecords(QStringLiteral("/accepted"), 1, "completed");
    (void)checkRecords(QStringLiteral("/prompt-accepted"), 1, "completed");
    (void)checkRecords(QStringLiteral("/prompt-denied"), 0, "cancelled");
    (void)checkRecords(QStringLiteral("/private"), 1, "completed");
    (void)checkRecords(QStringLiteral("/collision"), 1, "completed");
    const std::vector<Json> interrupted = checkRecords(QStringLiteral("/interrupt"), 1, "failed");
    check(required(interrupted.front(), "path").isNull(), "Reloaded failed transfer exposed staging.");
    check(required(interrupted.front(), "error").isString() && !required(interrupted.front(), "error").asString().empty(),
          "Reloaded failed transfer lost its reason.");
    const std::vector<Json> late = checkRecords(QStringLiteral("/late"), 1, "failed");
    check(required(late.front(), "path").isNull(), "Reloaded late-collision transfer exposed staging.");
    (void)checkRecords(QStringLiteral("/timeout-retry"), 1, "completed");
    const std::vector<Json> explicitAgent = checkRecords(QStringLiteral("/agent"), 1, "cancelled");
    check(required(explicitAgent.front(), "path").isNull(), "Reloaded explicit agent cancellation exposed staging.");
    const std::vector<Json> nativeAgent = checkRecords(QStringLiteral("/agent-native"), 2, "cancelled");
    check(std::all_of(nativeAgent.begin(), nativeAgent.end(), [](const Json &entry) {
        return required(entry, "path").isNull();
    }), "Reloaded native agent cancellation exposed staging.");
    (void)checkRecords(QStringLiteral("/user-survives-end"), 1, "completed");
    const std::vector<Json> shutdown = checkRecords(QStringLiteral("/shutdown"), 1, "interrupted");
    check(required(shutdown.front(), "path").isNull(), "Reloaded shutdown interruption exposed staging.");
    check(required(shutdown.front(), "error").asString() == "Browser closed before the download completed.",
          "Reloaded shutdown interruption lost its exact reason.");
    checkTerminalCleanup(paths.profile);
    library.shutdownDownloads();
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-download-home-XXXXXX"));
    QTemporaryDir downloads(QStringLiteral("/tmp/yobro-download-files-XXXXXX"));
    if (!home.isValid() || !downloads.isValid()) {
        std::cerr << "QT DOWNLOAD LIFECYCLE FAIL: Could not create isolated directories.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    previousMessageHandler = qInstallMessageHandler(recordQtMessage);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Download Lifecycle"));

    try {
        testFilenameParity();
        testRestartAndCorruptState(std::filesystem::path(home.path().toStdString()) / "state-tests");
        const auto paths = yobro::core::ProfilePaths::forProfile("download-lifecycle");
        paths.createDirectories();
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "Could not start download fixture server.");
        serveFixtures(server);
        const QString baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        runLiveLifecycle(application, paths, downloads.path().toStdString(), baseUrl);
        checkReloadedLifecycle(paths, downloads.path().toStdString(), baseUrl);
        check(
            !sawProfileReleaseWarning.load(),
            "Qt warned that a WebEnginePage outlived its profile."
        );
        std::cout << "QT DOWNLOAD LIFECYCLE PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT DOWNLOAD LIFECYCLE FAIL: " << error.what() << '\n';
        return 1;
    }
}
