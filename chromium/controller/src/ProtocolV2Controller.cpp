#include "yobro/controller/ProtocolV2Controller.hpp"

#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace yobro::controller {
namespace {

constexpr std::string_view invalidRequest = "Invalid JSON request or request too large.";
constexpr std::string_view inactiveProfile = "This profile is inactive. Switch to it in the browser.";
constexpr std::string_view pausedAgent = "Agentenzugriff ist im Browser pausiert.";
constexpr std::string_view busyAgent = "Agent is busy. Retry after the current command completes.";
constexpr std::string_view blockedWorkspace = "This command changes the user workspace and is unavailable in agent mode.";

bool isBlockedCommand(std::string_view command) {
    static constexpr std::array blocked{
        std::string_view("space"),
        std::string_view("panel"),
        std::string_view("restore"),
        std::string_view("move"),
    };
    for (const auto candidate : blocked) {
        if (candidate == command)
            return true;
    }
    return false;
}

bool presentsAgentWorkspace(std::string_view command) {
    return command != "history" && command != "downloads" && command != "cancel-download";
}

core::Json envelope(bool ok, core::Json payload) {
    core::Json::Object object;
    object.emplace("ok", core::Json(ok));
    object.emplace(ok ? "result" : "error", std::move(payload));
    return core::Json(std::move(object));
}

} // namespace

ProtocolV2Controller::ProtocolV2Controller(EventLoop &eventLoop, ProtocolV2Host &host)
    : eventLoop_(eventLoop), host_(host) {}

void ProtocolV2Controller::handleLine(std::string request, ResponseCallback callback) {
    eventLoop_.post([this, request = std::move(request), callback = std::move(callback)]() mutable {
        dispatch(std::move(request), std::move(callback));
    });
}

bool ProtocolV2Controller::busy() const noexcept {
    return busy_;
}

const std::optional<std::string> &ProtocolV2Controller::agentAction() const noexcept {
    return agentAction_;
}

const std::vector<std::string> &ProtocolV2Controller::commands() {
    static const std::vector<std::string> values{
        "status", "tabs", "open", "new", "focus", "close", "read", "click", "fill",
        "scroll", "back", "forward", "reload", "pin", "split", "find", "history",
        "downloads", "download", "cancel-download", "duplicate", "end",
    };
    return values;
}

void ProtocolV2Controller::dispatch(std::string request, ResponseCallback callback) {
    core::Json decoded;
    try {
        decoded = core::Json::parse(request, {.maxBytes = 1'048'576, .maxDepth = 64, .rejectDuplicateKeys = true});
    } catch (const std::exception &) {
        respondError(std::move(callback), std::string(invalidRequest));
        return;
    }
    if (!decoded.isObject()) {
        respondError(std::move(callback), std::string(invalidRequest));
        return;
    }

    const auto &object = decoded.asObject();
    std::string command = "status";
    if (const auto found = object.find("command"); found != object.end() && found->second.isString())
        command = found->second.asString();

    ProtocolHostState state;
    try {
        state = host_.protocolState();
    } catch (const std::exception &error) {
        respondError(std::move(callback), error.what());
        return;
    }
    if (!state.profileActive) {
        respondError(std::move(callback), std::string(inactiveProfile));
        return;
    }
    if (command == "status") {
        respondSuccess(std::move(callback), statusResult());
        return;
    }
    if (!state.agentEnabled) {
        respondError(std::move(callback), std::string(pausedAgent));
        return;
    }
    if (command == "tabs") {
        try {
            respondSuccess(std::move(callback), host_.tabsResult());
        } catch (const std::exception &error) {
            respondError(std::move(callback), error.what());
        }
        return;
    }
    if (command == "end") {
        try {
            host_.endAgentWorkspace();
            core::Json::Object result;
            result.emplace("ended", core::Json(true));
            respondSuccess(std::move(callback), core::Json(std::move(result)));
        } catch (const std::exception &error) {
            respondError(std::move(callback), error.what());
        }
        return;
    }
    if (busy_) {
        respondError(std::move(callback), std::string(busyAgent));
        return;
    }
    if (isBlockedCommand(command)) {
        respondError(std::move(callback), std::string(blockedWorkspace));
        return;
    }

    busy_ = true;
    agentAction_ = command;
    if (presentsAgentWorkspace(command)) {
        try {
            host_.presentAgentWorkspace();
        } catch (const std::exception &error) {
            busy_ = false;
            agentAction_.reset();
            respondError(std::move(callback), error.what());
            return;
        }
    }

    auto claimed = std::make_shared<std::atomic_bool>(false);
    auto response = std::make_shared<ResponseCallback>(std::move(callback));
    auto completion = [this, claimed, response](
                          core::Json result,
                          std::optional<std::string> error
                      ) mutable {
        if (claimed->exchange(true))
            return;
        eventLoop_.post([
            this,
            result = std::move(result),
            error = std::move(error),
            callback = std::move(*response)
        ]() mutable {
            busy_ = false;
            agentAction_.reset();
            if (error)
                respondError(std::move(callback), std::move(*error));
            else
                respondSuccess(std::move(callback), std::move(result));
        });
    };

    try {
        host_.execute(command, object, std::move(completion));
    } catch (const std::exception &error) {
        if (!claimed->exchange(true)) {
            busy_ = false;
            agentAction_.reset();
            respondError(std::move(*response), error.what());
        }
    } catch (...) {
        if (!claimed->exchange(true)) {
            busy_ = false;
            agentAction_.reset();
            respondError(std::move(*response), "Unexpected controller failure.");
        }
    }
}

void ProtocolV2Controller::respondSuccess(ResponseCallback callback, core::Json result) const {
    if (callback)
        callback(envelope(true, std::move(result)).serialize());
}

void ProtocolV2Controller::respondError(ResponseCallback callback, std::string error) const {
    if (callback)
        callback(envelope(false, core::Json(std::move(error))).serialize());
}

core::Json ProtocolV2Controller::statusResult() const {
    const ProtocolHostState state = host_.protocolState();
    core::Json::Object result;
    result.emplace("browser", core::Json(state.browser));
    result.emplace("version", core::Json(state.version));
    result.emplace("engine", core::Json(state.engine));
    result.emplace("protocol", core::Json(std::int64_t{2}));
    result.emplace("enabled", core::Json(state.agentEnabled));
    result.emplace("agentPaneVisible", core::Json(state.agentPaneVisible));
    result.emplace("agentAction", agentAction_ ? core::Json(*agentAction_) : core::Json(nullptr));
    result.emplace("socket", core::Json(state.socketPath));
    result.emplace("profile", core::Json(state.profileName));
    result.emplace("libraryAccess", core::Json(state.libraryAccess));
    core::Json::Array commandValues;
    commandValues.reserve(commands().size());
    for (const auto &command : commands())
        commandValues.emplace_back(command);
    result.emplace("commands", core::Json(std::move(commandValues)));
    return core::Json(std::move(result));
}

} // namespace yobro::controller
