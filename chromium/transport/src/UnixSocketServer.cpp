#include "yobro/transport/LocalControlServer.hpp"

#if defined(_WIN32)
#error "UnixSocketServer.cpp must not be built on Windows."
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace yobro::transport {
namespace {

constexpr std::string_view invalidRequestResponse =
    "{\"error\":\"Invalid JSON request or request too large.\",\"ok\":false}";
constexpr std::string_view internalErrorResponse =
    "{\"error\":\"Unexpected controller failure.\",\"ok\":false}";

[[noreturn]] void systemFailure(const std::string &operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void closeDescriptor(int &descriptor) noexcept {
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

void setDescriptorFlags(int descriptor) {
    const int statusFlags = ::fcntl(descriptor, F_GETFL, 0);
    if (statusFlags < 0 || ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) < 0)
        systemFailure("Could not make local-control descriptor nonblocking");
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptorFlags < 0 || ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
        systemFailure("Could not mark local-control descriptor close-on-exec");
}

struct SocketIdentity {
    dev_t device = 0;
    ino_t inode = 0;
    bool valid = false;
};

bool sameIdentity(const struct stat &status, const SocketIdentity &identity) {
    return identity.valid && status.st_dev == identity.device && status.st_ino == identity.inode;
}

void validatePrivateParent(const std::filesystem::path &path) {
    if (!path.is_absolute())
        throw std::invalid_argument("The local-control socket path must be absolute.");
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty())
        throw std::invalid_argument("The local-control socket requires a parent directory.");

    struct stat status {};
    if (::lstat(parent.c_str(), &status) != 0)
        systemFailure("Could not inspect the local-control parent directory");
    if (!S_ISDIR(status.st_mode))
        throw std::runtime_error("The local-control parent must be a real directory, not a symlink.");
    if (status.st_uid != ::geteuid())
        throw std::runtime_error("The local-control parent directory is not owned by this user.");
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw std::runtime_error("The local-control parent directory must be accessible only to its owner.");
}

sockaddr_un socketAddress(const std::filesystem::path &path, socklen_t &length) {
    const std::string encoded = path.string();
    sockaddr_un address {};
    if (encoded.empty() || encoded.size() >= sizeof(address.sun_path))
        throw std::invalid_argument("The local-control socket path is too long.");
    address.sun_family = AF_UNIX;
#if defined(__APPLE__) || defined(__FreeBSD__)
    address.sun_len = static_cast<std::uint8_t>(
        offsetof(sockaddr_un, sun_path) + encoded.size() + 1
    );
#endif
    std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + encoded.size() + 1);
    return address;
}

bool endpointIsLive(const sockaddr_un &address, socklen_t length) {
    int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0)
        systemFailure("Could not create a local-control endpoint probe");
    try {
        setDescriptorFlags(probe);
    } catch (...) {
        closeDescriptor(probe);
        throw;
    }
    const int result = ::connect(probe, reinterpret_cast<const sockaddr *>(&address), length);
    const int connectError = result == 0 ? 0 : errno;
    closeDescriptor(probe);
    if (result == 0 || connectError == EINPROGRESS || connectError == EAGAIN || connectError == EALREADY)
        return true;
    if (connectError == ECONNREFUSED || connectError == ENOENT)
        return false;
    throw std::system_error(
        connectError,
        std::generic_category(),
        "Could not safely probe the existing local-control endpoint"
    );
}

bool peerBelongsToCurrentUser(int descriptor) noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid = std::numeric_limits<uid_t>::max();
    gid_t gid = std::numeric_limits<gid_t>::max();
    return ::getpeereid(descriptor, &uid, &gid) == 0 && uid == ::geteuid();
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    return ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0
        && credentials.uid == ::geteuid();
#else
    (void)descriptor;
    return false;
#endif
}

void suppressBrokenPipe(int descriptor) {
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        systemFailure("Could not configure local-control broken-pipe handling");
#else
    (void)descriptor;
#endif
}

std::string normalizedResponse(std::string response) {
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();
    if (response.find('\n') != std::string::npos || response.find('\r') != std::string::npos)
        response.assign(internalErrorResponse);
    response.push_back('\n');
    return response;
}

class UnixSocketServer final : public LocalControlServer {
private:
    enum class ClientPhase { reading, waiting, writing };

    struct Client {
        std::uint64_t id = 0;
        int descriptor = -1;
        ClientPhase phase = ClientPhase::reading;
        std::string input;
        std::string output;
        std::size_t sent = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    struct PendingResponse {
        std::uint64_t clientId = 0;
        std::string response;
    };

    struct SharedState final : public std::enable_shared_from_this<SharedState> {
        explicit SharedState(LocalControlServerOptions configured) : options(std::move(configured)) {}

        void enqueue(std::uint64_t clientId, std::string response) noexcept {
            if (stopping.load())
                return;
            {
                std::lock_guard lock(pendingMutex);
                pending.push_back({clientId, std::move(response)});
            }
            const unsigned char signal = 1;
            const ssize_t ignored = ::write(wakeWrite, &signal, sizeof(signal));
            (void)ignored;
        }

        LocalControlServerOptions options;
        RequestHandler handler;
        std::atomic_bool running = false;
        std::atomic_bool stopping = false;
        int listener = -1;
        int wakeRead = -1;
        int wakeWrite = -1;
        SocketIdentity identity;
        std::mutex pendingMutex;
        std::deque<PendingResponse> pending;
    };

public:
    explicit UnixSocketServer(LocalControlServerOptions options)
        : state_(std::make_shared<SharedState>(std::move(options))) {
        if (state_->options.path.empty())
            throw std::invalid_argument("A local-control socket path is required.");
        if (state_->options.maxRequestBytes == 0)
            throw std::invalid_argument("The local-control request limit must be positive.");
        if (state_->options.clientTimeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("The local-control timeout must be positive.");
        if (state_->options.backlog <= 0)
            throw std::invalid_argument("The local-control backlog must be positive.");
    }

    ~UnixSocketServer() override {
        stop();
    }

    void start(RequestHandler handler) override {
        if (!handler)
            throw std::invalid_argument("A local-control request handler is required.");
        if (started_)
            throw std::logic_error("The local-control server cannot be started more than once.");
        started_ = true;
        state_->handler = std::move(handler);

        try {
            prepareSocket(*state_);
            prepareWakePipe(*state_);
            state_->running.store(true);
            worker_ = std::thread([state = state_] { run(state); });
        } catch (...) {
            state_->running.store(false);
            state_->stopping.store(true);
            cleanup(*state_);
            throw;
        }
    }

    void stop() noexcept override {
        if (!started_)
            return;
        state_->stopping.store(true);
        const unsigned char signal = 1;
        if (state_->wakeWrite >= 0) {
            const ssize_t ignored = ::write(state_->wakeWrite, &signal, sizeof(signal));
            (void)ignored;
        }
        if (worker_.joinable())
            worker_.join();
        state_->running.store(false);
        cleanup(*state_);
    }

    bool running() const noexcept override {
        return state_->running.load() && !state_->stopping.load();
    }

    const std::filesystem::path &path() const noexcept override {
        return state_->options.path;
    }

private:
    static void prepareSocket(SharedState &state) {
        validatePrivateParent(state.options.path);
        socklen_t addressLength = 0;
        const sockaddr_un address = socketAddress(state.options.path, addressLength);

        struct stat existing {};
        if (::lstat(state.options.path.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode))
                throw std::runtime_error("Refusing to replace a non-socket local-control path.");
            if (existing.st_uid != ::geteuid())
                throw std::runtime_error("Refusing to replace a local-control socket owned by another user.");
            if (endpointIsLive(address, addressLength))
                throw std::runtime_error("Another YOBRO instance already uses this local-control socket.");
            struct stat unchanged {};
            if (::lstat(state.options.path.c_str(), &unchanged) != 0
                || unchanged.st_dev != existing.st_dev
                || unchanged.st_ino != existing.st_ino
                || !S_ISSOCK(unchanged.st_mode)) {
                throw std::runtime_error("The stale local-control socket changed during validation.");
            }
            if (::unlink(state.options.path.c_str()) != 0)
                systemFailure("Could not remove the validated stale local-control socket");
        } else if (errno != ENOENT) {
            systemFailure("Could not inspect the local-control socket path");
        }

        state.listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (state.listener < 0)
            systemFailure("Could not create the local-control socket");
        setDescriptorFlags(state.listener);
        suppressBrokenPipe(state.listener);
        if (::bind(state.listener, reinterpret_cast<const sockaddr *>(&address), addressLength) != 0)
            systemFailure("Could not bind the local-control socket");
        if (::chmod(state.options.path.c_str(), S_IRUSR | S_IWUSR) != 0)
            systemFailure("Could not secure the local-control socket mode");

        struct stat bound {};
        if (::lstat(state.options.path.c_str(), &bound) != 0)
            systemFailure("Could not verify the bound local-control socket");
        if (!S_ISSOCK(bound.st_mode)
            || bound.st_uid != ::geteuid()
            || (bound.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
            throw std::runtime_error("The bound local-control socket failed its owner or mode check.");
        }
        state.identity = {bound.st_dev, bound.st_ino, true};
        if (::listen(state.listener, state.options.backlog) != 0)
            systemFailure("Could not listen on the local-control socket");
    }

    static void prepareWakePipe(SharedState &state) {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0)
            systemFailure("Could not create the local-control wake pipe");
        state.wakeRead = descriptors[0];
        state.wakeWrite = descriptors[1];
        try {
            setDescriptorFlags(state.wakeRead);
            setDescriptorFlags(state.wakeWrite);
        } catch (...) {
            closeDescriptor(state.wakeRead);
            closeDescriptor(state.wakeWrite);
            throw;
        }
    }

    static void cleanup(SharedState &state) noexcept {
        closeDescriptor(state.listener);
        closeDescriptor(state.wakeRead);
        closeDescriptor(state.wakeWrite);
        if (state.identity.valid) {
            struct stat current {};
            if (::lstat(state.options.path.c_str(), &current) == 0
                && sameIdentity(current, state.identity)
                && S_ISSOCK(current.st_mode)) {
                ::unlink(state.options.path.c_str());
            }
            state.identity.valid = false;
        }
    }

    static void closeClient(std::map<std::uint64_t, Client> &clients, std::uint64_t id) noexcept {
        const auto found = clients.find(id);
        if (found == clients.end())
            return;
        int descriptor = found->second.descriptor;
        closeDescriptor(descriptor);
        clients.erase(found);
    }

    static void beginWrite(Client &client, std::string response) {
        client.phase = ClientPhase::writing;
        client.output = normalizedResponse(std::move(response));
        client.sent = 0;
    }

    static void acceptClients(const std::shared_ptr<SharedState> &state, std::map<std::uint64_t, Client> &clients, std::uint64_t &nextId) {
        while (true) {
            int descriptor = ::accept(state->listener, nullptr, nullptr);
            if (descriptor < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                if (!state->stopping.load())
                    state->stopping.store(true);
                return;
            }
            try {
                setDescriptorFlags(descriptor);
                suppressBrokenPipe(descriptor);
            } catch (...) {
                closeDescriptor(descriptor);
                continue;
            }
            if (!peerBelongsToCurrentUser(descriptor)) {
                closeDescriptor(descriptor);
                continue;
            }
            Client client;
            client.id = nextId++;
            client.descriptor = descriptor;
            client.deadline = std::chrono::steady_clock::now() + state->options.clientTimeout;
            clients.emplace(client.id, std::move(client));
        }
    }

    static void drainWakePipe(int descriptor) noexcept {
        unsigned char bytes[64];
        while (::read(descriptor, bytes, sizeof(bytes)) > 0) {}
    }

    static void applyPendingResponses(const std::shared_ptr<SharedState> &state, std::map<std::uint64_t, Client> &clients) {
        std::deque<PendingResponse> responses;
        {
            std::lock_guard lock(state->pendingMutex);
            responses.swap(state->pending);
        }
        for (auto &pending : responses) {
            const auto found = clients.find(pending.clientId);
            if (found == clients.end() || found->second.phase != ClientPhase::waiting)
                continue;
            beginWrite(found->second, std::move(pending.response));
            found->second.deadline = std::chrono::steady_clock::now() + state->options.clientTimeout;
        }
    }

    static void dispatchRequest(const std::shared_ptr<SharedState> &state, Client &client, std::string request) {
        client.phase = ClientPhase::waiting;
        client.deadline = std::chrono::steady_clock::now() + state->options.clientTimeout;
        const std::uint64_t id = client.id;
        std::weak_ptr<SharedState> weakState = state;
        try {
            state->handler(std::move(request), [weakState, id](std::string response) mutable {
                if (const auto locked = weakState.lock())
                    locked->enqueue(id, std::move(response));
            });
        } catch (...) {
            state->enqueue(id, std::string(internalErrorResponse));
        }
    }

    static void readClient(const std::shared_ptr<SharedState> &state, Client &client) {
        char buffer[4096];
        while (client.phase == ClientPhase::reading) {
            const ssize_t count = ::recv(client.descriptor, buffer, sizeof(buffer), 0);
            if (count > 0) {
                client.input.append(buffer, static_cast<std::size_t>(count));
                const std::size_t newline = client.input.find('\n');
                if (newline != std::string::npos) {
                    if (newline > state->options.maxRequestBytes) {
                        beginWrite(client, std::string(invalidRequestResponse));
                    } else {
                        std::string request = client.input.substr(0, newline);
                        dispatchRequest(state, client, std::move(request));
                    }
                    return;
                }
                if (client.input.size() > state->options.maxRequestBytes) {
                    beginWrite(client, std::string(invalidRequestResponse));
                    return;
                }
                continue;
            }
            if (count == 0) {
                beginWrite(client, std::string(invalidRequestResponse));
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            client.output.clear();
            client.sent = 0;
            return;
        }
    }

    static bool writeClient(Client &client) noexcept {
        while (client.sent < client.output.size()) {
#if defined(MSG_NOSIGNAL)
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const ssize_t count = ::send(
                client.descriptor,
                client.output.data() + client.sent,
                client.output.size() - client.sent,
                flags
            );
            if (count > 0) {
                client.sent += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return false;
            return true;
        }
        return true;
    }

    static void run(const std::shared_ptr<SharedState> &state) noexcept {
        std::map<std::uint64_t, Client> clients;
        std::uint64_t nextId = 1;
        while (!state->stopping.load()) {
            std::vector<pollfd> descriptors;
            std::vector<std::uint64_t> clientIds;
            descriptors.push_back({state->listener, POLLIN, 0});
            descriptors.push_back({state->wakeRead, POLLIN, 0});
            for (const auto &[id, client] : clients) {
                short events = 0;
                if (client.phase == ClientPhase::reading)
                    events = POLLIN;
                else if (client.phase == ClientPhase::writing)
                    events = POLLOUT;
                descriptors.push_back({client.descriptor, events, 0});
                clientIds.push_back(id);
            }

            const int result = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 100);
            if (result < 0 && errno != EINTR) {
                state->stopping.store(true);
                break;
            }
            if (state->stopping.load())
                break;
            if (result > 0 && (descriptors[0].revents & POLLIN) != 0)
                acceptClients(state, clients, nextId);
            if (result > 0 && (descriptors[1].revents & POLLIN) != 0) {
                drainWakePipe(state->wakeRead);
                applyPendingResponses(state, clients);
            }

            std::vector<std::uint64_t> closeIds;
            for (std::size_t index = 0; index < clientIds.size(); ++index) {
                const std::uint64_t id = clientIds[index];
                const auto found = clients.find(id);
                if (found == clients.end())
                    continue;
                Client &client = found->second;
                const short events = descriptors[index + 2].revents;
                if ((events & (POLLERR | POLLNVAL)) != 0) {
                    closeIds.push_back(id);
                    continue;
                }
                if (client.phase == ClientPhase::reading && (events & (POLLIN | POLLHUP)) != 0)
                    readClient(state, client);
                if (client.phase == ClientPhase::writing && (events & POLLOUT) != 0 && writeClient(client))
                    closeIds.push_back(id);
                else if (client.phase != ClientPhase::writing && (events & POLLHUP) != 0)
                    closeIds.push_back(id);
            }

            const auto now = std::chrono::steady_clock::now();
            for (const auto &[id, client] : clients) {
                // Match the WebKit bridge: socket timeouts bound receive and
                // send operations, but an accepted command may legitimately
                // spend up to 20 seconds in browser navigation before replying.
                if (client.phase != ClientPhase::waiting && now >= client.deadline)
                    closeIds.push_back(id);
            }
            for (const std::uint64_t id : closeIds)
                closeClient(clients, id);
        }

        for (auto &[id, client] : clients) {
            (void)id;
            closeDescriptor(client.descriptor);
        }
        clients.clear();
        state->running.store(false);
    }

    std::shared_ptr<SharedState> state_;
    std::thread worker_;
    bool started_ = false;
};

} // namespace

std::unique_ptr<LocalControlServer> makeLocalControlServer(LocalControlServerOptions options) {
    return std::make_unique<UnixSocketServer>(std::move(options));
}

} // namespace yobro::transport
