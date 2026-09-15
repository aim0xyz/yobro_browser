#pragma once

#include "engine/api/BrowserEngine.hpp"
#include "yobro/controller/BrowserLibrary.hpp"
#include "yobro/controller/EventLoop.hpp"
#include "yobro/controller/ProtocolV2Host.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yobro::controller {

/// One completed agent command, newest first in BrowserSession::agentEvents().
/// Mirrors AgentEvent in the WebKit build: the user must be able to read back
/// what the agent did in their browser without trusting the agent's own report.
struct AgentEvent {
    std::string action;
    std::string detail;
    /// Space the event is attributed to. Matches the `in:` argument of
    /// BrowserModel.record; commands without one use the session space.
    std::string space;
};

struct BrowserSessionConfig {
    std::string browser = "YoBro";
    std::string version;
    std::string engine = "Chromium";
    std::string socketPath;
    std::string profileName;
    std::string space = "Personal";
    bool profileActive = true;
    bool agentEnabled = true;
    bool libraryAccess = false;
    std::chrono::milliseconds rendererRecoveryDelay{100};
    std::chrono::milliseconds rendererStabilityDelay{5000};
    std::shared_ptr<BrowserLibrary> library;
    std::function<std::string()> generateId;
};

enum class RendererRecoveryState {
    healthy,
    recovering,
    failed,
};

struct SessionTabView {
    engine::BrowserPage *page = nullptr;
    engine::PageState state;
    std::string space;
    bool pinned = false;
    bool privatePage = false;
    RendererRecoveryState rendererRecovery = RendererRecoveryState::healthy;
    std::optional<engine::RendererTerminationKind> lastRendererTermination;
};

struct PermissionPrompt {
    std::uint64_t id = 0;
    std::string tabId;
    std::string origin;
    engine::WebPermission permission = engine::WebPermission::microphone;
    bool privatePage = false;
};

class BrowserSession final : public ProtocolV2Host {
public:
    using Observer = std::function<void()>;
    using PermissionPromptPresenter = std::function<bool(const PermissionPrompt &)>;
    using AuthenticationPromptPresenter = std::function<std::optional<engine::AuthenticationCredentials>(
        const engine::AuthenticationChallenge &
    )>;
    /// Returns true to continue to a site whose certificate the engine rejected.
    using CertificatePromptPresenter = std::function<bool(const engine::CertificateProblem &)>;
    /// Shows the passkey conversation. Called for every state of one request.
    using PasskeyPromptPresenter = std::function<void(
        const engine::PasskeyRequest &,
        const engine::PasskeyControls &
    )>;
    /// Shows the screen and window picker. Must answer before returning.
    using DesktopMediaPromptPresenter = std::function<void(
        const engine::DesktopMediaRequest &,
        const engine::DesktopMediaControls &
    )>;
    /// Called once per recorded agent command, on the event loop thread.
    using AgentEventObserver = std::function<void(const AgentEvent &)>;
    /// Local development verification only. Returns the written file path, or
    /// reports why no window could be read. Without a capture installed the
    /// `capture-window` command does not exist at all.
    using WindowCapture = std::function<std::optional<std::string>(std::string &error)>;

    BrowserSession(
        std::unique_ptr<engine::BrowserProfile> profile,
        EventLoop &eventLoop,
        BrowserSessionConfig config
    );
    ~BrowserSession() override;

    BrowserSession(const BrowserSession &) = delete;
    BrowserSession &operator=(const BrowserSession &) = delete;

    [[nodiscard]] engine::BrowserProfile &profile() const;
    [[nodiscard]] engine::BrowserPage &newUserTab(
        std::string_view url = {},
        bool privatePage = false
    );
    [[nodiscard]] engine::BrowserPage &newAgentTab(std::string_view url = {});
    [[nodiscard]] bool closeTab(std::string_view id);
    // All shell-owned user navigation must cross the session boundary so
    // recovery cancellation, document epochs, and permission state stay bound.
    [[nodiscard]] bool navigateUserTab(std::string_view id, std::string_view url);
    [[nodiscard]] bool goBackUserTab(std::string_view id);
    [[nodiscard]] bool goForwardUserTab(std::string_view id);
    // User-triggered reloads reset a failed renderer-recovery budget. The
    // shell must use this instead of bypassing session lifecycle policy.
    [[nodiscard]] bool reloadUserTab(std::string_view id);
    [[nodiscard]] bool setActiveUserTab(std::string_view id);
    [[nodiscard]] bool setActiveAgentTab(std::string_view id);
    [[nodiscard]] engine::BrowserPage *activeUserPage() const;
    [[nodiscard]] engine::BrowserPage *activeAgentPage() const;
    [[nodiscard]] std::vector<SessionTabView> tabViews() const;
    [[nodiscard]] std::vector<engine::BrowserPage *> userPages() const;
    [[nodiscard]] std::vector<engine::BrowserPage *> agentPages() const;
    [[nodiscard]] std::optional<PermissionPrompt> pendingPermission() const;
    [[nodiscard]] bool resolvePermission(
        std::uint64_t requestId,
        engine::PermissionDecision decision
    );

    // The embedding shell must explicitly opt in only while an interactive,
    // prompt-capable user surface is mounted. Hidden package harnesses stay off.
    void setPermissionSurfaceVisible(bool visible);
    void setPermissionPromptPresenter(PermissionPromptPresenter presenter);
    void setAuthenticationPromptPresenter(AuthenticationPromptPresenter presenter);
    void setCertificatePromptPresenter(CertificatePromptPresenter presenter);
    /// Without a presenter every passkey request is cancelled.
    void setPasskeyPromptPresenter(PasskeyPromptPresenter presenter);
    /// Without a presenter every screen-sharing request is cancelled.
    void setDesktopMediaPromptPresenter(DesktopMediaPromptPresenter presenter);
    void setObserver(Observer observer);
    /// Newest first, at most 50 entries, same bound as the WebKit build.
    [[nodiscard]] std::vector<AgentEvent> agentEvents() const;
    void setAgentEventObserver(AgentEventObserver observer);
    /// Only honoured while YOBRO_DEV_CAPTURE=1 is set in the environment.
    void setWindowCapture(WindowCapture capture);
    void setProfileActive(bool active);
    void setAgentEnabled(bool enabled);
    void setLibraryAccess(bool enabled);
    [[nodiscard]] bool agentEnabled() const;
    [[nodiscard]] bool agentPaneVisible() const;

    [[nodiscard]] ProtocolHostState protocolState() const override;
    [[nodiscard]] core::Json tabsResult() const override;
    void endAgentWorkspace() override;
    void presentAgentWorkspace() override;
    void execute(
        std::string_view command,
        const core::Json::Object &request,
        ProtocolCompletion completion
    ) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yobro::controller
