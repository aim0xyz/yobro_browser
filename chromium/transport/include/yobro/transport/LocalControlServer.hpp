#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace yobro::transport {

struct LocalControlServerOptions {
    std::filesystem::path path;
    std::size_t maxRequestBytes = 1'048'576;
    std::chrono::milliseconds clientTimeout = std::chrono::seconds(10);
    int backlog = 8;
};

class LocalControlServer {
public:
    using ResponseCallback = std::function<void(std::string response)>;
    using RequestHandler = std::function<void(
        std::string request,
        ResponseCallback respond
    )>;

    virtual ~LocalControlServer() = default;
    virtual void start(RequestHandler handler) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual const std::filesystem::path &path() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<LocalControlServer> makeLocalControlServer(
    LocalControlServerOptions options
);

} // namespace yobro::transport
