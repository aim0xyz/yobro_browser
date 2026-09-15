#include "yobro/controller/EventLoop.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/controller/ProtocolV2Host.hpp"
#include "yobro/core/Json.hpp"
#include "yobro/transport/LocalControlServer.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using yobro::core::Json;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/yobro-control-tests-XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path_ = created;
        if (::chmod(path_.c_str(), 0700) != 0) throw std::runtime_error("chmod temp directory failed");
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    const std::filesystem::path &path() const { return path_; }
private:
    std::filesystem::path path_;
};

sockaddr_un addressFor(const std::filesystem::path &path, socklen_t &length) {
    const std::string encoded = path.string();
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
#if defined(__APPLE__) || defined(__FreeBSD__)
    address.sun_len = static_cast<std::uint8_t>(offsetof(sockaddr_un, sun_path) + encoded.size() + 1);
#endif
    check(encoded.size() < sizeof(address.sun_path), "Test socket path is too long.");
    std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + encoded.size() + 1);
    return address;
}

int connectClient(const std::filesystem::path &path) {
    int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) fail("Could not create test client socket.");
    timeval timeout{5, 0};
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
        fail("Could not connect to test server.");
    }
    return descriptor;
}

void writeAll(int descriptor, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t count = ::send(descriptor, bytes.data() + sent, bytes.size() - sent, flags);
        if (count <= 0) fail("Test client write failed.");
        sent += static_cast<std::size_t>(count);
    }
}

std::string readLine(int descriptor) {
    std::string result;
    char buffer[8192];
    while (true) {
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count < 0) fail("Test client read timed out or failed.");
        if (count == 0) break;
        result.append(buffer, static_cast<std::size_t>(count));
        const std::size_t newline = result.find('\n');
        if (newline != std::string::npos) {
            result.resize(newline);
            break;
        }
    }
    return result;
}

std::string exchange(const std::filesystem::path &path, std::string request, bool addNewline = true) {
    int descriptor = connectClient(path);
    if (addNewline) request.push_back('\n');
    writeAll(descriptor, request);
    if (!addNewline) ::shutdown(descriptor, SHUT_WR);
    std::string response = readLine(descriptor);
    ::close(descriptor);
    return response;
}

std::unique_ptr<yobro::transport::LocalControlServer> serverAt(
    const std::filesystem::path &path,
    yobro::transport::LocalControlServer::RequestHandler handler,
    std::chrono::milliseconds timeout = 2s
) {
    auto server = yobro::transport::makeLocalControlServer({
        .path = path,
        .maxRequestBytes = 1'048'576,
        .clientTimeout = timeout,
        .backlog = 8,
    });
    server->start(std::move(handler));
    return server;
}

void testFramingAndPermissions() {
    TemporaryDirectory directory;
    const auto socketPath = directory.path() / "control.sock";
    auto server = serverAt(socketPath, [](std::string request, auto respond) {
        Json::Object result;
        result.emplace("length", Json(static_cast<std::int64_t>(request.size())));
        respond(Json(std::move(result)).serialize());
    });

    struct stat status {};
    check(::lstat(socketPath.c_str(), &status) == 0, "Bound socket is missing.");
    check(S_ISSOCK(status.st_mode), "Bound endpoint is not a socket.");
    check((status.st_mode & 0777) == 0600, "Socket mode is not 0600.");
    check(status.st_uid == ::geteuid(), "Socket owner differs from effective uid.");

    Json response = Json::parse(exchange(socketPath, "{}"));
    check(response.find("length")->asInteger() == 2, "Simple framed request changed.");

    const std::string exact(1'048'576, 'x');
    response = Json::parse(exchange(socketPath, exact));
    check(response.find("length")->asInteger() == 1'048'576, "Exactly 1 MiB before LF must be accepted.");

    const std::string oversized(1'048'577, 'x');
    response = Json::parse(exchange(socketPath, oversized));
    check(!response.find("ok")->asBoolean(), "Oversized request was accepted.");
    check(response.find("error")->asString() == "Invalid JSON request or request too large.", "Oversized framing error differs.");

    response = Json::parse(exchange(socketPath, "{}", false));
    check(!response.find("ok")->asBoolean(), "Missing LF was accepted.");

    int descriptor = connectClient(socketPath);
    writeAll(descriptor, "{\"part\":");
    std::this_thread::sleep_for(10ms);
    writeAll(descriptor, "true}\n");
    response = Json::parse(readLine(descriptor));
    ::close(descriptor);
    check(response.find("length")->asInteger() == 13, "Partial request reads were not reassembled.");

    server->stop();
    check(!std::filesystem::exists(socketPath), "Owned socket path was not removed on stop.");
}

void testPartialResponseAndTimeout() {
    TemporaryDirectory directory;
    const auto socketPath = directory.path() / "control.sock";
    const std::string large(2 * 1'048'576, 'r');
    auto server = serverAt(socketPath, [large](std::string, auto respond) { respond(large); }, 500ms);
    const std::string response = exchange(socketPath, "{}");
    check(response.size() == large.size(), "Large response was not fully written.");

    int idle = connectClient(socketPath);
    std::this_thread::sleep_for(800ms);
    char byte = 0;
    const ssize_t count = ::recv(idle, &byte, 1, 0);
    check(count == 0, "Idle client was not closed after timeout.");
    ::close(idle);
}

void testFailClosedPaths() {
    TemporaryDirectory directory;
    const auto socketPath = directory.path() / "control.sock";
    auto first = serverAt(socketPath, [](std::string, auto respond) { respond("{}"); });

    bool liveRejected = false;
    try {
        auto second = yobro::transport::makeLocalControlServer({.path = socketPath});
        second->start([](std::string, auto) {});
    } catch (const std::exception &) { liveRejected = true; }
    check(liveRejected, "A live socket endpoint was stolen.");
    first->stop();

    const auto regularPath = directory.path() / "regular";
    {
        std::ofstream file(regularPath);
        file << "preserve";
    }
    bool regularRejected = false;
    try {
        auto server = yobro::transport::makeLocalControlServer({.path = regularPath});
        server->start([](std::string, auto) {});
    } catch (const std::exception &) { regularRejected = true; }
    check(regularRejected && std::filesystem::is_regular_file(regularPath), "Non-socket target was replaced.");

    const auto symlinkPath = directory.path() / "link";
    check(::symlink(regularPath.c_str(), symlinkPath.c_str()) == 0, "Could not create test symlink.");
    bool symlinkRejected = false;
    try {
        auto server = yobro::transport::makeLocalControlServer({.path = symlinkPath});
        server->start([](std::string, auto) {});
    } catch (const std::exception &) { symlinkRejected = true; }
    check(symlinkRejected && std::filesystem::is_symlink(symlinkPath), "Symlink endpoint was replaced.");

    check(::chmod(directory.path().c_str(), 0755) == 0, "Could not make test parent insecure.");
    bool parentRejected = false;
    try {
        auto server = yobro::transport::makeLocalControlServer({.path = socketPath});
        server->start([](std::string, auto) {});
    } catch (const std::exception &) { parentRejected = true; }
    check(parentRejected, "Non-private parent directory was accepted.");
    check(::chmod(directory.path().c_str(), 0700) == 0, "Could not restore test parent mode.");
}

void testInodeSafeCleanup() {
    TemporaryDirectory directory;
    const auto socketPath = directory.path() / "control.sock";
    auto server = serverAt(socketPath, [](std::string, auto respond) { respond("{}"); });
    check(::unlink(socketPath.c_str()) == 0, "Could not unlink live test socket name.");
    {
        std::ofstream replacement(socketPath);
        replacement << "replacement";
    }
    server->stop();
    check(std::filesystem::is_regular_file(socketPath), "Stop removed a replacement inode.");
}

class ImmediateTask final : public yobro::controller::ScheduledTask {
public:
    void cancel() noexcept override {}
};

class ImmediateEventLoop final : public yobro::controller::EventLoop {
public:
    void post(Task task) override { task(); }
    std::shared_ptr<yobro::controller::ScheduledTask> scheduleAfter(std::chrono::milliseconds, Task) override {
        return std::make_shared<ImmediateTask>();
    }
};

class StatusHost final : public yobro::controller::ProtocolV2Host {
public:
    yobro::controller::ProtocolHostState protocolState() const override {
        return {
            .profileActive = true,
            .agentEnabled = true,
            .agentPaneVisible = false,
            .libraryAccess = false,
            .browser = "YoBro",
            .version = "0.6.3",
            .engine = "Chromium",
            .socketPath = "/tmp/test.sock",
            .profileName = "Transport",
        };
    }
    Json tabsResult() const override { return Json(Json::Object{{"tabs", Json(Json::Array{})}, {"space", Json("Personal")}}); }
    void endAgentWorkspace() override {}
    void presentAgentWorkspace() override {}
    void execute(std::string_view command, const Json::Object &, yobro::controller::ProtocolCompletion completion) override {
        completion(Json(nullptr), "Unbekannter Befehl: " + std::string(command));
    }
};

void testControllerOverSocket() {
    TemporaryDirectory directory;
    const auto socketPath = directory.path() / "control.sock";
    ImmediateEventLoop loop;
    StatusHost host;
    yobro::controller::ProtocolV2Controller controller(loop, host);
    auto server = serverAt(socketPath, [&controller](std::string request, auto respond) {
        controller.handleLine(std::move(request), std::move(respond));
    });
    const Json response = Json::parse(exchange(socketPath, R"({"command":"status"})"));
    check(response.find("ok")->asBoolean(), "Controller status failed over AF_UNIX.");
    const Json &result = *response.find("result");
    check(result.find("protocol")->asInteger() == 2, "Protocol field changed over transport.");
    check(result.find("version")->asString() == "0.6.3", "Version changed over transport.");
}

void run(const char *name, const std::function<void()> &test, int &count) {
    test();
    ++count;
    std::cout << "PASS " << name << '\n';
}

} // namespace

int main() {
    try {
        int count = 0;
        run("framing-and-permissions", testFramingAndPermissions, count);
        run("partial-response-and-timeout", testPartialResponseAndTimeout, count);
        run("fail-closed-paths", testFailClosedPaths, count);
        run("inode-safe-cleanup", testInodeSafeCleanup, count);
        run("controller-over-socket", testControllerOverSocket, count);
        std::cout << "UNIX SOCKET SERVER TESTS PASS (" << count << ")\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "UNIX SOCKET SERVER TESTS FAIL: " << error.what() << '\n';
        return 1;
    }
}
