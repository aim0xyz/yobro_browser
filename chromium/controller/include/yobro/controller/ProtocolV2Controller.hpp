#pragma once

#include "yobro/controller/EventLoop.hpp"
#include "yobro/controller/ProtocolV2Host.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yobro::controller {

class ProtocolV2Controller final {
public:
    using ResponseCallback = std::function<void(std::string response)>;

    ProtocolV2Controller(EventLoop &eventLoop, ProtocolV2Host &host);

    void handleLine(std::string request, ResponseCallback callback);

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] const std::optional<std::string> &agentAction() const noexcept;
    [[nodiscard]] static const std::vector<std::string> &commands();

private:
    void dispatch(std::string request, ResponseCallback callback);
    void respondSuccess(ResponseCallback callback, core::Json result) const;
    void respondError(ResponseCallback callback, std::string error) const;
    [[nodiscard]] core::Json statusResult() const;

    EventLoop &eventLoop_;
    ProtocolV2Host &host_;
    bool busy_ = false;
    std::optional<std::string> agentAction_;
};

} // namespace yobro::controller
