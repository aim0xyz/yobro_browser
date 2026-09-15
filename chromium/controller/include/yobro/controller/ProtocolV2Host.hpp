#pragma once

#include "yobro/core/Json.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace yobro::controller {

struct ProtocolHostState {
    bool profileActive = true;
    bool agentEnabled = true;
    bool agentPaneVisible = false;
    bool libraryAccess = false;
    std::string browser = "YoBro";
    std::string version;
    std::string engine = "Chromium";
    std::string socketPath;
    std::string profileName;
};

using ProtocolCompletion = std::function<void(
    core::Json result,
    std::optional<std::string> error
)>;

class ProtocolV2Host {
public:
    virtual ~ProtocolV2Host() = default;

    [[nodiscard]] virtual ProtocolHostState protocolState() const = 0;
    [[nodiscard]] virtual core::Json tabsResult() const = 0;
    virtual void endAgentWorkspace() = 0;
    virtual void presentAgentWorkspace() = 0;
    virtual void execute(
        std::string_view command,
        const core::Json::Object &request,
        ProtocolCompletion completion
    ) = 0;
};

} // namespace yobro::controller
