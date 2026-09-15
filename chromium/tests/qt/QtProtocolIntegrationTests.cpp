#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/core/Json.hpp"
#include "yobro/core/ProfilePaths.hpp"
#include "yobro/transport/LocalControlServer.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using yobro::core::Json;
using namespace std::chrono_literals;

const QByteArray fastDownloadBody = QByteArrayLiteral("YOBRO fast fixture\n");
constexpr qint64 slowDownloadSize = 8 * 1024 * 1024;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

const Json &required(const Json &value, std::string_view key) {
    const Json *field = value.find(key);
    if (!field) fail("Missing JSON field: " + std::string(key));
    return *field;
}

const Json &success(const Json &response) {
    if (!required(response, "ok").asBoolean())
        fail(required(response, "error").asString());
    return required(response, "result");
}

std::string error(const Json &response) {
    check(!required(response, "ok").asBoolean(), "Expected protocol error.");
    return required(response, "error").asString();
}

sockaddr_un addressFor(const std::filesystem::path &path, socklen_t &length) {
    const std::string encoded = path.string();
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
#if defined(__APPLE__) || defined(__FreeBSD__)
    address.sun_len = static_cast<std::uint8_t>(offsetof(sockaddr_un, sun_path) + encoded.size() + 1);
#endif
    check(encoded.size() < sizeof(address.sun_path), "Integration socket path is too long.");
    std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + encoded.size() + 1);
    return address;
}

Json request(const std::filesystem::path &path, Json::Object object) {
    int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) fail("Could not create integration client socket.");
    timeval timeout{30, 0};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    socklen_t length = 0;
    const sockaddr_un address = addressFor(path, length);
    if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), length) != 0) {
        ::close(descriptor);
        fail("Could not connect to the Chromium control socket.");
    }
    std::string encoded = Json(std::move(object)).serialize();
    encoded.push_back('\n');
    std::size_t sent = 0;
    while (sent < encoded.size()) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t count = ::send(descriptor, encoded.data() + sent, encoded.size() - sent, flags);
        if (count <= 0) {
            ::close(descriptor);
            fail("Could not send integration request.");
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    while (response.find('\n') == std::string::npos) {
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            ::close(descriptor);
            fail("Chromium control socket closed before a response for " + encoded);
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    response.resize(response.find('\n'));
    return Json::parse(response);
}

QByteArray pageBody(const QByteArray &title) {
    QByteArray body = QByteArrayLiteral("<!doctype html><html><head><meta charset=\"utf-8\"><title>");
    body += title;
    body += QByteArrayLiteral(R"HTML(</title></head>
<body><label>Name<input id="name"></label>
<button id="apply" onclick="document.getElementById('out').textContent=document.getElementById('name').value">Apply</button>
<p id="out">Ready</p></body></html>)HTML");
    return body;
}

void sendBody(
    QTcpSocket *socket,
    const QByteArray &body,
    const QByteArray &contentType,
    const QByteArray &disposition = {}
) {
    QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ");
    response += contentType;
    response += QByteArrayLiteral("\r\nConnection: close\r\nContent-Length: ");
    response += QByteArray::number(body.size());
    if (!disposition.isEmpty()) {
        response += QByteArrayLiteral("\r\nContent-Disposition: attachment; filename=\"");
        response += disposition;
        response += QByteArrayLiteral("\"");
    }
    response += QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void sendSlowDownload(QTcpSocket *socket) {
    QByteArray response = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"protocol-slow.bin\"\r\n"
        "Connection: close\r\n"
        "Content-Length: "
    );
    response += QByteArray::number(slowDownloadSize);
    response += QByteArrayLiteral("\r\n\r\n");
    socket->write(response);
    socket->setProperty("slowSent", qint64{0});

    auto *timer = new QTimer(socket);
    timer->setInterval(25);
    QObject::connect(timer, &QTimer::timeout, socket, [socket, timer] {
        if (socket->state() != QAbstractSocket::ConnectedState) {
            timer->stop();
            return;
        }
        qint64 sent = socket->property("slowSent").toLongLong();
        const qint64 count = std::min<qint64>(64 * 1024, slowDownloadSize - sent);
        if (count <= 0) {
            timer->stop();
            socket->disconnectFromHost();
            return;
        }
        socket->write(QByteArray(static_cast<qsizetype>(count), 'S'));
        sent += count;
        socket->setProperty("slowSent", sent);
        if (sent == slowDownloadSize) {
            timer->stop();
            socket->disconnectFromHost();
        }
    });
    timer->start();
}

void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("responded").toBool()) {
                    (void)socket->readAll();
                    return;
                }
                QByteArray accumulated = socket->property("request").toByteArray();
                accumulated.append(socket->readAll());
                socket->setProperty("request", accumulated);
                if (!accumulated.contains("\r\n\r\n")) return;
                socket->setProperty("responded", true);

                const QByteArray requestLine = accumulated.left(accumulated.indexOf("\r\n"));
                const QList<QByteArray> parts = requestLine.split(' ');
                const QByteArray path = parts.size() >= 2 ? parts.at(1) : QByteArray();
                if (path == QByteArrayLiteral("/fixture")) {
                    sendBody(socket, pageBody("Protocol Fixture"), QByteArrayLiteral("text/html; charset=utf-8"));
                } else if (path == QByteArrayLiteral("/private")) {
                    sendBody(socket, pageBody("Private Fixture"), QByteArrayLiteral("text/html; charset=utf-8"));
                } else if (path == QByteArrayLiteral("/download/fast")) {
                    sendBody(
                        socket,
                        fastDownloadBody,
                        QByteArrayLiteral("application/octet-stream"),
                        QByteArrayLiteral("protocol-fast.bin")
                    );
                } else if (path == QByteArrayLiteral("/download/user")) {
                    sendBody(
                        socket,
                        QByteArrayLiteral("user download must be rejected\n"),
                        QByteArrayLiteral("application/octet-stream"),
                        QByteArrayLiteral("user-prompt.bin")
                    );
                } else if (path == QByteArrayLiteral("/download/slow")) {
                    sendSlowDownload(socket);
                } else {
                    const QByteArray body = QByteArrayLiteral("not found\n");
                    QByteArray response = QByteArrayLiteral(
                        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: "
                    );
                    response += QByteArray::number(body.size());
                    response += QByteArrayLiteral("\r\n\r\n");
                    response += body;
                    socket->write(response);
                    socket->disconnectFromHost();
                }
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

std::optional<Json> entryWithField(
    const Json::Array &entries,
    std::string_view field,
    std::string_view value
) {
    for (const Json &entry : entries) {
        if (!entry.isObject()) continue;
        const Json *candidate = entry.find(field);
        if (candidate && candidate->isString() && candidate->asString() == value)
            return entry;
    }
    return std::nullopt;
}

Json downloadsResult(const std::filesystem::path &socketPath) {
    const Json response = request(socketPath, Json::Object{{"command", Json("downloads")}});
    return success(response);
}

Json waitForDownload(
    const std::filesystem::path &socketPath,
    std::string_view source,
    const std::function<bool(const Json &)> &predicate,
    std::string_view expectation
) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    std::string lastState = "missing";
    while (std::chrono::steady_clock::now() < deadline) {
        const Json result = downloadsResult(socketPath);
        const auto entry = entryWithField(required(result, "downloads").asArray(), "source", source);
        if (entry) {
            const Json *state = entry->find("state");
            if (state && state->isString()) lastState = state->asString();
            if (predicate(*entry)) return *entry;
        }
        std::this_thread::sleep_for(25ms);
    }
    fail("Timed out waiting for " + std::string(expectation) + "; last state was " + lastState + ".");
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("Could not read integration file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

struct PromptSnapshot {
    int count = 0;
    std::string name;
    std::string directory;
};

class PromptProbe {
public:
    bool reject(std::string name, std::string directory) {
        {
            std::lock_guard lock(mutex_);
            ++snapshot_.count;
            snapshot_.name = std::move(name);
            snapshot_.directory = std::move(directory);
        }
        changed_.notify_all();
        return false;
    }

    bool waitForCount(int count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return snapshot_.count >= count; });
    }

    PromptSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    PromptSnapshot snapshot_;
};

void waitForPrivateNavigation(const std::atomic_int &result) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    check(result.load() == 1, "Private loopback navigation failed or timed out.");
}

void checkHistory(
    const std::filesystem::path &socketPath,
    std::string_view fixtureUrl,
    std::string_view privateUrl,
    std::int64_t visits
) {
    const Json response = request(socketPath, Json::Object{
        {"command", Json("history")}, {"query", Json("")}, {"limit", Json(std::int64_t{20})},
    });
    const Json &result = success(response);
    const Json::Array &entries = required(result, "history").asArray();
    const auto fixture = entryWithField(entries, "url", fixtureUrl);
    check(fixture.has_value(), "Real Qt history omitted the completed agent navigation.");
    check(required(*fixture, "title").asString() == "Protocol Fixture", "History title differs.");
    check(required(*fixture, "visits").asInteger() == visits, "History visit count differs.");
    check(!required(*fixture, "id").asString().empty(), "History id is empty.");
    check(!required(*fixture, "date").asString().empty(), "History date is empty.");
    check(!entryWithField(entries, "url", privateUrl).has_value(), "Private navigation leaked into persistent history.");
}

void runJourney(
    const std::filesystem::path &socketPath,
    const std::filesystem::path &profileDirectory,
    const std::filesystem::path &downloadDirectory,
    std::string fixtureUrl,
    std::string privateUrl,
    std::string fastDownloadUrl,
    std::string slowDownloadUrl,
    std::string userDownloadUrl,
    std::string userTabId,
    yobro::qtwebengine::QtBrowserPage &userPage,
    const std::atomic_int &privateNavigation,
    PromptProbe &promptProbe,
    const std::function<std::optional<std::string>(bool)> &setLibraryAccess
) {
    Json response = request(socketPath, Json::Object{{"command", Json("status")}});
    const Json &status = success(response);
    check(required(status, "engine").asString() == "Chromium", "Status did not report Chromium.");
    check(required(status, "version").asString() == "0.6.3", "Status version differs.");
    check(!required(status, "libraryAccess").asBoolean(), "Real Qt library access did not start fail-closed.");
    check(!required(status, "agentPaneVisible").asBoolean(), "Status unexpectedly started an agent pane.");

    response = request(socketPath, Json::Object{{"command", Json("history")}});
    check(error(response) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.",
          "Real Qt history was not blocked before opt-in.");
    response = request(socketPath, Json::Object{{"command", Json("downloads")}});
    check(error(response) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.",
          "Real Qt downloads were not blocked before opt-in.");

    const std::optional<std::string> enableProblem = setLibraryAccess(true);
    check(!enableProblem.has_value(), enableProblem.value_or("Could not enable real Qt library access."));
    response = request(socketPath, Json::Object{{"command", Json("status")}});
    check(required(success(response), "libraryAccess").asBoolean(), "Real Qt status did not expose the committed opt-in.");
    const Json enabledPolicy = Json::parse(readFile(profileDirectory / "bridge-policy.json"));
    check(required(enabledPolicy, "allowsLibraryAccess").asBoolean(), "The enabled bridge policy was not persisted.");

    waitForPrivateNavigation(privateNavigation);
    response = request(socketPath, Json::Object{{"command", Json("focus")}, {"tab", Json(userTabId)}});
    check(error(response) == "This is a user tab. Open its URL with new to work in the right agent pane.", "User tab ownership guard differs over the real socket.");

    response = request(socketPath, Json::Object{{"command", Json("new")}, {"url", Json(fixtureUrl)}});
    const Json &created = success(response);
    const std::string tabId = required(created, "id").asString();
    check(required(created, "owner").asString() == "agent", "New did not create an agent tab.");
    check(required(created, "url").asString() == fixtureUrl, "New returned the wrong URL.");
    checkHistory(socketPath, fixtureUrl, privateUrl, 1);

    response = request(socketPath, Json::Object{{"command", Json("read")}, {"tab", Json(tabId)}});
    const Json &read = success(response);
    const Json &page = required(read, "page");
    const std::string document = required(page, "document").asString();
    std::string inputRef;
    std::string buttonRef;
    for (const Json &element : required(page, "elements").asArray()) {
        if (!element.isObject()) continue;
        const Json *tag = element.find("tag");
        const Json *reference = element.find("ref");
        if (!tag || !tag->isString() || !reference || !reference->isString()) continue;
        if (tag->asString() == "input") inputRef = reference->asString();
        if (tag->asString() == "button") buttonRef = reference->asString();
    }
    check(!document.empty() && !inputRef.empty() && !buttonRef.empty(), "Real AgentBridge snapshot omitted actionable refs.");

    response = request(socketPath, Json::Object{
        {"command", Json("fill")}, {"tab", Json(tabId)}, {"document", Json(document)},
        {"ref", Json(inputRef)}, {"value", Json("Parallel works")},
    });
    (void)success(response);
    response = request(socketPath, Json::Object{
        {"command", Json("click")}, {"tab", Json(tabId)}, {"document", Json(document)}, {"ref", Json(buttonRef)},
    });
    (void)success(response);
    response = request(socketPath, Json::Object{{"command", Json("read")}, {"tab", Json(tabId)}});
    check(required(required(success(response), "page"), "text").asString().find("Parallel works") != std::string::npos,
          "Socket fill/click/read journey did not update the real Chromium document.");

    response = request(socketPath, Json::Object{{"command", Json("find")}, {"tab", Json(tabId)}, {"query", Json("Apply")}});
    check(required(success(response), "found").asBoolean(), "Real Chromium find command failed.");
    response = request(socketPath, Json::Object{{"command", Json("reload")}, {"tab", Json(tabId)}});
    (void)success(response);
    checkHistory(socketPath, fixtureUrl, privateUrl, 2);

    response = request(socketPath, Json::Object{
        {"command", Json("download")}, {"tab", Json(tabId)}, {"url", Json(fastDownloadUrl)},
    });
    const Json &fastStarted = success(response);
    check(required(fastStarted, "started").asBoolean(), "Fast agent download did not start.");
    check(required(fastStarted, "next").asString() == "Use downloads to check progress and retrieve the final file path.", "Download next hint differs.");
    const Json fast = waitForDownload(socketPath, fastDownloadUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "completed";
    }, "fast download completion");
    check(required(fast, "name").asString() == "protocol-fast.bin", "Fast download filename differs.");
    check(required(fast, "received").asInteger() == fastDownloadBody.size(), "Fast download received count differs.");
    check(required(fast, "expected").asInteger() == fastDownloadBody.size(), "Fast download expected count differs.");
    check(required(fast, "error").isNull(), "Completed download unexpectedly has an error.");
    const std::filesystem::path fastPath(required(fast, "path").asString());
    check(fastPath.parent_path() == downloadDirectory, "Fast download escaped the isolated directory.");
    check(readFile(fastPath) == fastDownloadBody.toStdString(), "Fast download file bytes differ.");
    check(promptProbe.snapshot().count == 0, "Explicit agent download incorrectly invoked the user prompt.");

    const Json initialDownloads = downloadsResult(socketPath);
    check(required(initialDownloads, "directory").asString() == downloadDirectory.string(), "Protocol download directory differs from the isolated directory.");

    response = request(socketPath, Json::Object{
        {"command", Json("download")}, {"tab", Json(tabId)}, {"url", Json(slowDownloadUrl)},
    });
    check(required(success(response), "started").asBoolean(), "Slow agent download did not start.");
    const Json slowActive = waitForDownload(socketPath, slowDownloadUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "downloading"
            && required(entry, "received").asInteger() > 0
            && required(entry, "received").asInteger() < required(entry, "expected").asInteger()
            && required(entry, "expected").asInteger() == slowDownloadSize;
    }, "slow download progress");
    const std::string slowId = required(slowActive, "id").asString();
    check(required(slowActive, "name").asString() == "protocol-slow.bin", "Slow download filename differs.");

    response = request(socketPath, Json::Object{{"command", Json("cancel-download")}, {"id", Json(slowId)}});
    check(required(success(response), "cancelRequested").asString() == slowId, "Slow cancellation response differs.");
    const Json slowCancelled = waitForDownload(socketPath, slowDownloadUrl, [](const Json &entry) {
        return required(entry, "state").asString() == "cancelled";
    }, "slow download cancellation");
    check(required(slowCancelled, "error").isNull(), "Cancelled download unexpectedly has an error.");
    response = request(socketPath, Json::Object{{"command", Json("cancel-download")}, {"id", Json(slowId)}});
    check(error(response) == "Download ist nicht aktiv.", "Second cancellation did not report an inactive download.");
    response = request(socketPath, Json::Object{{"command", Json("cancel-download")}, {"id", Json("missing-download")}});
    check(error(response) == "Unbekannter Download.", "Unknown cancellation error differs.");
    check(promptProbe.snapshot().count == 0, "Agent download cancellation path invoked the user prompt.");

    const Json persisted = Json::parse(readFile(profileDirectory / "downloads.json"));
    check(persisted.isArray(), "Persisted downloads root is not an array.");
    const auto persistedFast = entryWithField(persisted.asArray(), "source", fastDownloadUrl);
    const auto persistedSlow = entryWithField(persisted.asArray(), "source", slowDownloadUrl);
    check(persistedFast && required(*persistedFast, "state").asString() == "completed", "Completed metadata was not persisted.");
    check(persistedSlow && required(*persistedSlow, "state").asString() == "cancelled", "Cancelled metadata was not persisted.");

    QWebEnginePage *userWebPage = userPage.view()->page();
    const bool queued = QMetaObject::invokeMethod(
        userWebPage,
        [userWebPage, url = QUrl(QString::fromStdString(userDownloadUrl))] { userWebPage->download(url); },
        Qt::QueuedConnection
    );
    check(queued, "Could not queue the user download request on the Qt thread.");
    check(promptProbe.waitForCount(1, 10s), "User download prompt was not invoked.");
    const PromptSnapshot prompt = promptProbe.snapshot();
    check(prompt.count == 1, "User download prompt count differs.");
    check(prompt.name == "user-prompt.bin", "User prompt filename differs.");
    check(prompt.directory == downloadDirectory.string(), "User prompt directory differs.");
    const Json afterUserDownload = downloadsResult(socketPath);
    check(!entryWithField(required(afterUserDownload, "downloads").asArray(), "source", userDownloadUrl),
          "Rejected user download was incorrectly persisted.");

    const std::optional<std::string> disableProblem = setLibraryAccess(false);
    check(!disableProblem.has_value(), disableProblem.value_or("Could not disable real Qt library access."));
    response = request(socketPath, Json::Object{{"command", Json("status")}});
    check(!required(success(response), "libraryAccess").asBoolean(), "Real Qt status did not expose the committed opt-out.");
    response = request(socketPath, Json::Object{{"command", Json("history")}});
    check(error(response) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.",
          "Real Qt history remained exposed after opt-out.");
    response = request(socketPath, Json::Object{{"command", Json("downloads")}});
    check(error(response) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.",
          "Real Qt downloads remained exposed after opt-out.");
    const Json disabledPolicy = Json::parse(readFile(profileDirectory / "bridge-policy.json"));
    check(!required(disabledPolicy, "allowsLibraryAccess").asBoolean(), "The disabled bridge policy was not persisted.");

    response = request(socketPath, Json::Object{{"command", Json("tabs")}, {"probe", Json("before-end")}});
    check(required(success(response), "tabs").asArray().size() == 3, "Tabs did not expose user, private, and agent pages.");
    response = request(socketPath, Json::Object{{"command", Json("end")}});
    check(required(success(response), "ended").asBoolean(), "End failed over the real socket.");
    response = request(socketPath, Json::Object{{"command", Json("tabs")}, {"probe", Json("after-end")}});
    check(required(success(response), "tabs").asArray().size() == 2, "End did not discard the real agent page.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-qt-XXXXXX"));
    QTemporaryDir downloads(QStringLiteral("/tmp/yobro-dl-XXXXXX"));
    if (!home.isValid() || !downloads.isValid()) {
        std::cerr << "QT PROTOCOL SOCKET INTEGRATION FAIL: Could not create isolated integration directories.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Protocol Integration"));

    try {
        const auto paths = yobro::core::ProfilePaths::forProfile("protocol-integration");
        paths.createDirectories();

        QTcpServer fixture;
        check(fixture.listen(QHostAddress::LocalHost, 0), "Could not start loopback fixture server.");
        serveFixture(fixture);
        const QString fixtureBase = QStringLiteral("http://127.0.0.1:%1").arg(fixture.serverPort());
        const std::string fixtureUrl = (fixtureBase + QStringLiteral("/fixture")).toStdString();
        const std::string privateUrl = (fixtureBase + QStringLiteral("/private")).toStdString();
        const std::string fastDownloadUrl = (fixtureBase + QStringLiteral("/download/fast")).toStdString();
        const std::string slowDownloadUrl = (fixtureBase + QStringLiteral("/download/slow")).toStdString();
        const std::string userDownloadUrl = (fixtureBase + QStringLiteral("/download/user")).toStdString();

        yobro::qtwebengine::QtBrowserEngine engine;
        auto profile = engine.openProfile({
            .id = "protocol-integration",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Qt engine returned a non-Qt profile.");

        PromptProbe promptProbe;
        yobro::spike::BridgePolicyStore bridgePolicy(paths.bridgePolicy);
        const yobro::spike::BridgePolicyLoadResult initialPolicy = bridgePolicy.load();
        check(!initialPolicy.problem.has_value(), "A missing integration policy was treated as corrupt.");
        check(!initialPolicy.allowsLibraryAccess, "A missing integration policy did not fail closed.");
        auto library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(
            *qtProfile,
            paths.profile,
            downloads.path().toStdString()
        );
        library->setUserDownloadPrompt([&promptProbe](const std::string &name, const std::string &directory) {
            return promptProbe.reject(name, directory);
        });

        yobro::qtwebengine::QtEventLoop eventLoop;
        yobro::controller::BrowserSession session(
            std::move(profile),
            eventLoop,
            {
                .version = "0.6.3",
                .socketPath = paths.control.string(),
                .profileName = "Integration",
                .libraryAccess = initialPolicy.allowsLibraryAccess,
                .library = library,
            }
        );
        auto &userGeneric = session.newUserTab();
        auto *userPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&userGeneric);
        check(userPage != nullptr, "Qt profile returned a non-Qt user page.");
        userPage->setHtml(QStringLiteral("<!doctype html><title>User</title><p>User page</p>"));
        userPage->view()->resize(800, 600);
        userPage->view()->show();
        const std::string userTabId = userPage->state().id;

        std::atomic_int privateNavigation{0};
        auto &privateGeneric = session.newUserTab({}, true);
        auto *privatePage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&privateGeneric);
        check(privatePage != nullptr, "Qt profile returned a non-Qt private page.");
        privatePage->view()->resize(800, 600);
        privatePage->view()->show();
        auto privateSubscription = privateGeneric.subscribe({
            .navigationFinished = [&privateNavigation](const yobro::engine::NavigationResult &result) {
                privateNavigation.store(result.error ? -1 : 1);
            },
        });
        (void)privateGeneric.navigate(privateUrl);

        session.setObserver([&session] {
            auto *generic = session.activeAgentPage();
            auto *page = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(generic);
            if (!page) return;
            page->view()->resize(800, 600);
            page->view()->show();
        });

        yobro::controller::ProtocolV2Controller controller(eventLoop, session);
        auto controlServer = yobro::transport::makeLocalControlServer({
            .path = paths.control,
            .maxRequestBytes = 1'048'576,
            .clientTimeout = std::chrono::seconds(10),
            .backlog = 8,
        });
        controlServer->start([&controller](std::string encoded, auto respond) {
            controller.handleLine(std::move(encoded), std::move(respond));
        });

        std::string workerError;
        std::atomic_int workerResult{1};
        std::atomic_bool watchdogExpired{false};
        const auto setLibraryAccess = [&application, &bridgePolicy, &session](bool enabled) {
            std::optional<std::string> problem;
            const bool invoked = QMetaObject::invokeMethod(
                &application,
                [&] {
                    problem = bridgePolicy.persistAndApply(
                        enabled,
                        [&session](bool value) { session.setLibraryAccess(value); }
                    );
                },
                Qt::BlockingQueuedConnection
            );
            if (!invoked)
                return std::optional<std::string>("Could not invoke the bridge-policy update on the Qt thread.");
            return problem;
        };
        std::thread worker([&] {
            try {
                runJourney(
                    paths.control,
                    paths.profile,
                    downloads.path().toStdString(),
                    fixtureUrl,
                    privateUrl,
                    fastDownloadUrl,
                    slowDownloadUrl,
                    userDownloadUrl,
                    userTabId,
                    *userPage,
                    privateNavigation,
                    promptProbe,
                    setLibraryAccess
                );
                workerResult.store(0);
            } catch (const std::exception &exception) {
                workerError = exception.what();
            }
            QMetaObject::invokeMethod(&application, [&application] { application.exit(); }, Qt::QueuedConnection);
        });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &application, [&] {
            watchdogExpired.store(true);
            application.exit(2);
        });
        watchdog.start(75'000);
        const int eventResult = application.exec();
        worker.join();
        controlServer->stop();
        session.setObserver({});
        library->setUserDownloadPrompt({});
        library.reset();
        if (eventResult != 0 || workerResult.load() != 0) {
            if (watchdogExpired.load())
                fail("Qt protocol integration watchdog expired.");
            fail(workerError.empty() ? "Qt event loop exited with an error." : workerError);
        }
        std::cout << "QT PROTOCOL SOCKET/LIBRARY INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT PROTOCOL SOCKET INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
