#include "yobro/controller/BrowserSession.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace yobro::controller {
namespace {

constexpr std::string_view operationPaused = "Agent operation was paused.";
constexpr std::string_view stillLoading = "Seite lädt noch. Später erneut mit read prüfen.";
constexpr std::string_view unknownTab = "Unbekannter Tab.";
constexpr std::string_view userTab = "This is a user tab. Open its URL with new to work in the right agent pane.";
constexpr std::string_view libraryBlocked = "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.";
constexpr std::string_view repeatedRendererCrash = "Der Seiteninhalt ist mehrfach abgestürzt. Lade sie neu oder öffne sie in einem neuen Tab.";
constexpr std::string_view rendererRecoveryFailed = "Der Seiteninhalt konnte nicht automatisch wiederhergestellt werden. Lade sie neu oder öffne sie in einem neuen Tab.";
constexpr std::string_view agentCallbackTimedOut = "Agent page operation timed out.";

std::string lowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return std::string(value.substr(first, last - first));
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool hasHttpHost(std::string_view value) {
    const std::string lowered = lowerAscii(value);
    std::size_t authorityStart = 0;
    if (startsWith(lowered, "https://"))
        authorityStart = 8;
    else if (startsWith(lowered, "http://"))
        authorityStart = 7;
    else
        return false;
    const std::size_t authorityEnd = value.find_first_of("/?#", authorityStart);
    std::string_view authority = value.substr(
        authorityStart,
        authorityEnd == std::string_view::npos ? value.size() - authorityStart : authorityEnd - authorityStart
    );
    if (const std::size_t user = authority.rfind('@'); user != std::string_view::npos)
        authority.remove_prefix(user + 1);
    if (authority.empty())
        return false;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        return close != std::string_view::npos && close > 1;
    }
    const std::size_t colon = authority.rfind(':');
    const std::string_view host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
    return !host.empty();
}

// Mirrors `tab.webView.url?.host` in the WebKit build: the registrable
// authority without user info or port, and nothing for a page that has no
// hierarchical URL yet. Agent log details fall back to a title when empty.
std::optional<std::string> hostOf(std::string_view value) {
    const std::size_t scheme = value.find("://");
    if (scheme == std::string_view::npos)
        return std::nullopt;
    std::string_view authority = value.substr(scheme + 3);
    if (const std::size_t end = authority.find_first_of("/?#"); end != std::string_view::npos)
        authority = authority.substr(0, end);
    if (const std::size_t user = authority.rfind('@'); user != std::string_view::npos)
        authority.remove_prefix(user + 1);
    if (authority.empty())
        return std::nullopt;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close <= 1)
            return std::nullopt;
        return std::string(authority.substr(0, close + 1));
    }
    const std::size_t colon = authority.rfind(':');
    const std::string_view host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
    if (host.empty())
        return std::nullopt;
    return std::string(host);
}
std::string percentEncodeQuery(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        const bool unreserved = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.' || character == '~';
        if (unreserved) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[(character >> 4U) & 0x0fU]);
            result.push_back(hex[character & 0x0fU]);
        }
    }
    return result;
}

std::string normalizedNavigationInput(std::string_view input) {
    const std::string text = trim(input);
    if (text.empty())
        return {};
    std::string candidate;
    if (text.find("://") != std::string::npos) {
        candidate = text;
    } else if (text.find(' ') == std::string::npos
               && (text.find('.') != std::string::npos
                   || startsWith(lowerAscii(text), "localhost")
                   || startsWith(text, "127.0.0.1"))) {
        candidate = (startsWith(lowerAscii(text), "localhost") || startsWith(text, "127.0.0.1")
            ? "http://"
            : "https://") + text;
    } else {
        candidate = "https://duckduckgo.com/?q=" + percentEncodeQuery(text);
    }
    if (!hasHttpHost(candidate))
        throw std::runtime_error("Nur gültige HTTP- und HTTPS-Adressen werden unterstützt.");
    return candidate;
}

std::string randomUuid() {
    std::array<unsigned char, 16> bytes {};
    std::random_device random;
    for (auto &byte : bytes)
        byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            output << '-';
        output << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return output.str();
}

const core::Json *field(const core::Json::Object &request, std::string_view name) {
    const auto found = request.find(name);
    return found == request.end() ? nullptr : &found->second;
}

std::optional<std::string> stringField(const core::Json::Object &request, std::string_view name) {
    const core::Json *value = field(request, name);
    if (!value || !value->isString())
        return std::nullopt;
    return value->asString();
}

bool boolField(const core::Json::Object &request, std::string_view name, bool fallback) {
    const core::Json *value = field(request, name);
    return value && value->isBoolean() ? value->asBoolean() : fallback;
}

std::int64_t integerField(const core::Json::Object &request, std::string_view name, std::int64_t fallback) {
    const core::Json *value = field(request, name);
    return value && value->isInteger() ? value->asInteger() : fallback;
}

class EmptyLibrary final : public BrowserLibrary {
public:
    core::Json history(std::string_view, std::size_t) const override { return core::Json(core::Json::Array{}); }
    core::Json downloads() const override { return core::Json(core::Json::Array{}); }
    std::string downloadDirectory() const override { return {}; }
    void recordVisit(const engine::PageState &) override {}
    void startDownload(engine::BrowserPage &, std::string_view, DownloadCompletion completion) override {
        completion("Downloads are unavailable in this Chromium profile.");
    }
    CancelDownloadResult cancelDownload(std::string_view) override { return CancelDownloadResult::unknown; }
    void endAgentDownloads() override {}
    void shutdownDownloads() override {}
};

} // namespace

struct BrowserSession::Impl {
    struct Tab {
        struct CancelledRecoveryToken {
            engine::NavigationToken token = 0;
            std::uint64_t cancellationGeneration = 0;
        };

        std::uint64_t instanceId = 0;
        std::string id;
        engine::PageOwner owner = engine::PageOwner::user;
        std::unique_ptr<engine::BrowserPage> page;
        std::unique_ptr<engine::PageEventSubscription> subscription;
        engine::PageState state;
        std::string space;
        std::string fallbackTitle;
        bool pinned = false;
        bool privatePage = false;
        bool adoptingPopup = false;
        bool awaitingLoadStart = false;
        std::uint64_t documentEpoch = 0;
        std::map<engine::NavigationToken, engine::NavigationResult> navigations;
        RendererRecoveryState rendererRecovery = RendererRecoveryState::healthy;
        std::optional<engine::RendererTerminationKind> lastRendererTermination;
        unsigned rendererCrashCount = 0;
        std::uint64_t recoveryGeneration = 0;
        std::optional<engine::NavigationToken> recoveryToken;
        std::array<CancelledRecoveryToken, 8> cancelledRecoveryTokens{};
        std::size_t nextCancelledRecoveryToken = 0;
        std::optional<std::uint64_t> profileCancellationGeneration;
        std::shared_ptr<ScheduledTask> recoveryTask;
        std::shared_ptr<ScheduledTask> stabilityTask;
    };

    struct Operation {
        std::uint64_t generation = 0;
        ProtocolCompletion completion;
        bool finished = false;
        bool readyElapsed = false;
        std::string targetTabId;
        std::uint64_t targetInstanceId = 0;
        std::uint64_t targetDocumentEpoch = 0;
        std::optional<engine::NavigationToken> waitingToken;
        std::function<void()> afterLoad;
        // Evaluated once, only when the command succeeds, with the tab the
        // operation ended on (null once the tab is gone). Commands the WebKit
        // build does not log leave this empty.
        std::function<AgentEvent(Tab *)> recordBuilder;
        std::shared_ptr<ScheduledTask> readyTimer;
        std::shared_ptr<ScheduledTask> timeoutTimer;
        std::shared_ptr<ScheduledTask> callbackTimer;
    };

    struct PendingPermission {
        PermissionPrompt prompt;
        std::uint64_t tabInstanceId = 0;
        std::uint64_t documentEpoch = 0;
        std::string documentUrl;
        std::unique_ptr<engine::PermissionRequest> request;
        std::shared_ptr<ScheduledTask> timeoutTimer;
    };

    Impl(
        std::unique_ptr<engine::BrowserProfile> openedProfile,
        EventLoop &configuredLoop,
        BrowserSessionConfig configured
    ) : profile(std::move(openedProfile)), loop(configuredLoop), config(std::move(configured)), lifetime(std::make_shared<int>(0)) {
        if (!profile)
            throw std::invalid_argument("BrowserSession requires a browser profile.");
        if (!config.generateId)
            config.generateId = randomUuid;
        if (!config.library)
            config.library = std::make_shared<EmptyLibrary>();
        if (config.profileName.empty())
            config.profileName = profile->spec().id;
    }

    ~Impl() {
        if (current)
            finish(current, core::Json(nullptr), std::string(operationPaused));
        (void)cancelPendingPermission(false);
        for (Tab &tab : tabs)
            cancelRendererTasks(tab);
        lifetime.reset();
        tabs.clear();
    }

    Tab *findTab(std::string_view id) {
        const std::string wanted = lowerAscii(id);
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&wanted](const Tab &tab) {
            return tab.id == wanted;
        });
        return found == tabs.end() ? nullptr : &*found;
    }

    const Tab *findTab(std::string_view id) const {
        return const_cast<Impl *>(this)->findTab(id);
    }

    Tab *findTabInstance(std::string_view id, std::uint64_t instanceId) {
        Tab *tab = findTab(id);
        return tab && tab->instanceId == instanceId ? tab : nullptr;
    }

    [[nodiscard]] bool isCancelledRecoveryToken(
        const Tab &tab,
        engine::NavigationToken token
    ) const noexcept {
        return token != 0
            && std::any_of(
                tab.cancelledRecoveryTokens.begin(),
                tab.cancelledRecoveryTokens.end(),
                [token](const Tab::CancelledRecoveryToken &cancelled) {
                    return cancelled.token == token;
                }
            );
    }

    void cancelRendererTasks(Tab &tab) noexcept {
        ++tab.recoveryGeneration;
        if (tab.recoveryToken) {
            tab.cancelledRecoveryTokens[tab.nextCancelledRecoveryToken] = {
                .token = *tab.recoveryToken,
                .cancellationGeneration = tab.recoveryGeneration,
            };
            tab.nextCancelledRecoveryToken =
                (tab.nextCancelledRecoveryToken + 1) % tab.cancelledRecoveryTokens.size();
        }
        if (tab.recoveryTask)
            tab.recoveryTask->cancel();
        if (tab.stabilityTask)
            tab.stabilityTask->cancel();
        tab.recoveryTask.reset();
        tab.stabilityTask.reset();
        tab.recoveryToken.reset();
        tab.awaitingLoadStart = false;
    }

    bool operationTargets(
        const std::shared_ptr<Operation> &operation,
        const Tab &tab,
        bool requireSameDocument = true
    ) const {
        return operation
            && !operation->finished
            && operation->targetInstanceId != 0
            && lowerAscii(operation->targetTabId) == tab.id
            && operation->targetInstanceId == tab.instanceId
            && (!requireSameDocument
                || operation->targetDocumentEpoch == tab.documentEpoch);
    }

    void bindOperation(const std::shared_ptr<Operation> &operation, const Tab &tab) {
        operation->targetTabId = tab.id;
        operation->targetInstanceId = tab.instanceId;
        operation->targetDocumentEpoch = tab.documentEpoch;
    }

    Tab *operationTab(
        const std::shared_ptr<Operation> &operation,
        bool requireSameDocument = true
    ) {
        if (!operation || operation->targetInstanceId == 0)
            return nullptr;
        Tab *tab = findTabInstance(operation->targetTabId, operation->targetInstanceId);
        return tab && operationTargets(operation, *tab, requireSameDocument)
            ? tab
            : nullptr;
    }

    bool isAgent(std::string_view id) const {
        const std::string wanted = lowerAscii(id);
        return std::any_of(agentIds.begin(), agentIds.end(), [&wanted](const std::string &candidate) {
            return lowerAscii(candidate) == wanted;
        });
    }

    void notify() noexcept {
        if (!observer)
            return;
        try {
            observer();
        } catch (...) {
            // UI refresh failures must not roll back committed page ownership.
        }
    }

    static void deny(std::unique_ptr<engine::PermissionRequest> request) noexcept {
        if (request)
            request->resolve(engine::PermissionDecision::deny);
    }

    bool permissionEligible(
        const Tab &tab,
        std::string_view origin
    ) const {
        return config.profileActive
            && permissionSurfaceVisible
            && static_cast<bool>(permissionPromptPresenter)
            && tab.owner == engine::PageOwner::user
            && tab.rendererRecovery == RendererRecoveryState::healthy
            && activeUserId
            && *activeUserId == tab.id
            && !tab.adoptingPopup
            && !tab.state.loading
            && !tab.state.securityOrigin.empty()
            && origin == tab.state.securityOrigin;
    }

    std::optional<engine::AuthenticationCredentials> onAuthenticationRequested(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::AuthenticationChallenge &challenge
    ) noexcept {
        if (challenge.proxy || challenge.origin.empty() || authenticationPromptActive
            || !config.profileActive || !permissionSurfaceVisible || !authenticationPromptPresenter)
            return std::nullopt;
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab || tab->owner != engine::PageOwner::user || tab->adoptingPopup
            || tab->rendererRecovery != RendererRecoveryState::healthy
            || !activeUserId || *activeUserId != tab->id
            || tab->state.securityOrigin != challenge.origin)
            return std::nullopt;
        authenticationPromptActive = true;
        std::optional<engine::AuthenticationCredentials> credentials;
        try {
            credentials = authenticationPromptPresenter(challenge);
        } catch (...) {
            credentials.reset();
        }
        authenticationPromptActive = false;
        tab = findTabInstance(id, instanceId);
        if (!tab || !config.profileActive || !permissionSurfaceVisible
            || tab->owner != engine::PageOwner::user || !activeUserId
            || *activeUserId != tab->id || tab->state.securityOrigin != challenge.origin)
            return std::nullopt;
        return credentials;
    }

    /// Same gate as the authentication prompt: only the active, visible user tab
    /// of an active profile may ask, and never an agent-owned page. Anything else
    /// keeps the engine's rejection.
    bool onCertificateProblem(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::CertificateProblem &problem
    ) noexcept {
        if (problem.host.empty() || !problem.mainFrame || certificatePromptActive
            || !config.profileActive || !permissionSurfaceVisible || !certificatePromptPresenter)
            return false;
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab || tab->owner != engine::PageOwner::user || tab->adoptingPopup
            || tab->rendererRecovery != RendererRecoveryState::healthy
            || !activeUserId || *activeUserId != tab->id)
            return false;
        certificatePromptActive = true;
        bool accepted = false;
        try {
            accepted = certificatePromptPresenter(problem);
        } catch (...) {
            accepted = false;
        }
        certificatePromptActive = false;
        // The tab may have gone away or lost focus while the sheet was open.
        tab = findTabInstance(id, instanceId);
        if (!tab || !config.profileActive || !permissionSurfaceVisible
            || tab->owner != engine::PageOwner::user || !activeUserId || *activeUserId != tab->id)
            return false;
        return accepted;
    }

    /// Same gate as the certificate sheet. A passkey is a sign-in credential, so
    /// an agent-owned page, a background page or an inactive profile never gets
    /// to ask for one; the request is cancelled instead.
    void onPasskeyRequest(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::PasskeyRequest &request,
        const engine::PasskeyControls &controls
    ) noexcept {
        const auto refuse = [&controls]() noexcept {
            try {
                if (controls.cancel) controls.cancel();
            } catch (...) {
                // Nothing else can be done about a cancel that throws.
            }
        };
        if (!config.profileActive || !permissionSurfaceVisible || !passkeyPromptPresenter) {
            refuse();
            return;
        }
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab || tab->owner != engine::PageOwner::user || tab->adoptingPopup
            || tab->rendererRecovery != RendererRecoveryState::healthy
            || !activeUserId || *activeUserId != tab->id) {
            refuse();
            return;
        }
        try {
            passkeyPromptPresenter(request, controls);
        } catch (...) {
            refuse();
        }
    }

    /// Same gate again. Screen sharing hands over a window of the user's own
    /// desktop, so a page that is not the one in front of them never gets to
    /// ask, and neither does an agent-owned page.
    void onDesktopMediaRequest(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::DesktopMediaRequest &request,
        const engine::DesktopMediaControls &controls
    ) noexcept {
        const auto refuse = [&controls]() noexcept {
            try {
                if (controls.cancel) controls.cancel();
            } catch (...) {
                // Nothing else can be done about a cancel that throws.
            }
        };
        if (!config.profileActive || !permissionSurfaceVisible || !desktopMediaPromptPresenter) {
            refuse();
            return;
        }
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab || tab->owner != engine::PageOwner::user || tab->privatePage || tab->adoptingPopup
            || tab->rendererRecovery != RendererRecoveryState::healthy
            || !activeUserId || *activeUserId != tab->id) {
            refuse();
            return;
        }
        try {
            desktopMediaPromptPresenter(request, controls);
        } catch (...) {
            refuse();
        }
    }
    bool cancelPendingPermission(bool notifyObserver) noexcept {
        if (!pendingPermission)
            return false;
        PendingPermission cancelled = std::move(*pendingPermission);
        pendingPermission.reset();
        if (cancelled.timeoutTimer)
            cancelled.timeoutTimer->cancel();
        deny(std::move(cancelled.request));
        if (notifyObserver)
            notify();
        return true;
    }

    bool cancelPendingPermissionForTab(
        std::string_view id,
        std::uint64_t instanceId,
        bool notifyObserver
    ) noexcept {
        if (!pendingPermission
            || pendingPermission->prompt.tabId != id
            || pendingPermission->tabInstanceId != instanceId) {
            return false;
        }
        return cancelPendingPermission(notifyObserver);
    }

    void invalidateDocument(Tab &tab, bool preserveWaitingNavigation = false) noexcept {
        const std::shared_ptr<Operation> preservedOperation =
            preserveWaitingNavigation
            && current
            && current->waitingToken
            && operationTargets(current, tab)
                ? current
                : nullptr;
        if (tab.recoveryTask || tab.stabilityTask || tab.recoveryToken)
            cancelRendererTasks(tab);
        ++tab.documentEpoch;
        (void)cancelPendingPermissionForTab(tab.id, tab.instanceId, false);
        if (preservedOperation) {
            preservedOperation->targetDocumentEpoch = tab.documentEpoch;
        } else if (current
            && operationTargets(current, tab, false)
            && current->targetDocumentEpoch != tab.documentEpoch) {
            finish(current, core::Json(nullptr), std::string(operationPaused));
        }
    }

    void prepareNavigation(Tab &tab) noexcept {
        invalidateDocument(tab);
        // Any explicit navigation is a newer owner of visible page state than
        // a profile-cancelled recovery callback.
        tab.profileCancellationGeneration.reset();
        tab.awaitingLoadStart = true;
    }

    void onPermissionRequested(
        std::string_view id,
        std::uint64_t instanceId,
        std::unique_ptr<engine::PermissionRequest> request
    ) noexcept {
        if (!request)
            return;
        try {
            Tab *tab = findTabInstance(id, instanceId);
            if (!tab || pendingPermission || !permissionEligible(*tab, request->origin())) {
                deny(std::move(request));
                return;
            }

            PermissionPrompt prompt{
                .id = nextPermissionRequestId++,
                .tabId = tab->id,
                .origin = std::string(request->origin()),
                .permission = request->permission(),
                .privatePage = tab->privatePage,
            };
            const std::uint64_t requestId = prompt.id;
            pendingPermission = PendingPermission{
                .prompt = std::move(prompt),
                .tabInstanceId = tab->instanceId,
                .documentEpoch = tab->documentEpoch,
                .documentUrl = tab->state.url,
                .request = std::move(request),
            };
            const std::weak_ptr<int> weakLifetime = lifetime;
            pendingPermission->timeoutTimer = loop.scheduleAfter(
                std::chrono::seconds(30),
                [this, weakLifetime, requestId] {
                    if (weakLifetime.expired()
                        || !pendingPermission
                        || pendingPermission->prompt.id != requestId) {
                        return;
                    }
                    (void)cancelPendingPermission(true);
                }
            );

            bool delivered = false;
            try {
                delivered = permissionPromptPresenter(pendingPermission->prompt);
            } catch (...) {
                delivered = false;
            }
            if (!pendingPermission || pendingPermission->prompt.id != requestId)
                return;
            if (!delivered) {
                (void)cancelPendingPermission(false);
                notify();
                return;
            }
            notify();
        } catch (...) {
            const bool cancelled = cancelPendingPermission(false);
            deny(std::move(request));
            if (cancelled)
                notify();
        }
    }

    bool resolvePermission(
        std::uint64_t requestId,
        engine::PermissionDecision decision
    ) noexcept {
        if (!pendingPermission || pendingPermission->prompt.id != requestId)
            return false;

        PendingPermission resolved = std::move(*pendingPermission);
        pendingPermission.reset();
        if (resolved.timeoutTimer)
            resolved.timeoutTimer->cancel();
        Tab *tab = nullptr;
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const Tab &candidate) {
            return candidate.id == resolved.prompt.tabId
                && candidate.instanceId == resolved.tabInstanceId;
        });
        if (found != tabs.end())
            tab = &*found;
        bool mayGrant = false;
        try {
            mayGrant = decision == engine::PermissionDecision::grant
                && tab
                && permissionEligible(*tab, resolved.prompt.origin)
                && tab->documentEpoch == resolved.documentEpoch
                && tab->state.url == resolved.documentUrl;
        } catch (...) {
            mayGrant = false;
        }
        if (resolved.request) {
            resolved.request->resolve(mayGrant
                ? engine::PermissionDecision::grant
                : engine::PermissionDecision::deny);
        }
        notify();
        return true;
    }

    void registerTab(Tab &tab) {
        if (tab.owner == engine::PageOwner::agent) {
            agentIds.push_back(tab.id);
            activeAgentId = tab.id;
            agentPaneVisible = true;
        } else {
            if (!activeUserId || *activeUserId != tab.id)
                (void)cancelPendingPermission(false);
            activeUserId = tab.id;
        }
    }

    Tab &storeTab(
        engine::PageOwner owner,
        bool privatePage,
        std::string space,
        bool adoptingPopup
    ) {
        std::string id = lowerAscii(config.generateId());
        if (id.empty() || findTab(id))
            throw std::runtime_error("The tab id generator returned an empty or duplicate id.");
        auto page = profile->createPage(id, owner, privatePage);
        Tab tab;
        tab.instanceId = nextTabInstanceId++;
        tab.id = id;
        tab.owner = owner;
        tab.state = page->state();
        tab.state.id = id;
        tab.state.owner = owner;
        tab.space = std::move(space);
        tab.fallbackTitle = owner == engine::PageOwner::agent ? "Agent page" : "New tab";
        if (tab.state.title.empty())
            tab.state.title = tab.fallbackTitle;
        tab.privatePage = privatePage;
        tab.adoptingPopup = adoptingPopup;
        tab.page = std::move(page);
        tabs.push_back(std::move(tab));
        Tab &stored = tabs.back();
        const std::uint64_t instanceId = stored.instanceId;
        const std::weak_ptr<int> weakLifetime = lifetime;
        try {
            stored.page->setPermissionRequestHandler(
                [this, weakLifetime, id, instanceId](
                    std::unique_ptr<engine::PermissionRequest> request
                ) mutable {
                    if (weakLifetime.expired()) {
                        deny(std::move(request));
                        return;
                    }
                    onPermissionRequested(id, instanceId, std::move(request));
                }
            );
            stored.page->setAuthenticationChallengeHandler(
                [this, weakLifetime, id, instanceId](
                    const engine::AuthenticationChallenge &challenge
                ) -> std::optional<engine::AuthenticationCredentials> {
                    if (weakLifetime.expired()) return std::nullopt;
                    return onAuthenticationRequested(id, instanceId, challenge);
                }
            );
            stored.page->setCertificateProblemHandler(
                [this, weakLifetime, id, instanceId](
                    const engine::CertificateProblem &problem
                ) -> bool {
                    if (weakLifetime.expired()) return false;
                    return onCertificateProblem(id, instanceId, problem);
                }
            );
            stored.page->setDesktopMediaHandler(
                [this, weakLifetime, id, instanceId](
                    const engine::DesktopMediaRequest &request,
                    const engine::DesktopMediaControls &controls
                ) {
                    if (weakLifetime.expired()) {
                        if (controls.cancel) controls.cancel();
                        return;
                    }
                    onDesktopMediaRequest(id, instanceId, request, controls);
                }
            );
            stored.page->setPasskeyHandler(
                [this, weakLifetime, id, instanceId](
                    const engine::PasskeyRequest &request,
                    const engine::PasskeyControls &controls
                ) {
                    if (weakLifetime.expired()) {
                        if (controls.cancel) controls.cancel();
                        return;
                    }
                    onPasskeyRequest(id, instanceId, request, controls);
                }
            );
            stored.subscription = stored.page->subscribe({
                .stateChanged = [this, weakLifetime, id, instanceId](const engine::PageState &state) {
                    if (weakLifetime.expired()) return;
                    onStateChanged(id, instanceId, state);
                },
                .navigationFinished = [this, weakLifetime, id, instanceId](const engine::NavigationResult &result) {
                    if (weakLifetime.expired()) return;
                    onNavigationFinished(id, instanceId, result);
                },
                .rendererTerminated = [this, weakLifetime, id, instanceId](const engine::RendererTermination &termination) {
                    if (weakLifetime.expired()) return;
                    onRendererTerminated(id, instanceId, termination);
                },
                .newWindowRequested = [this, weakLifetime, id, instanceId](const engine::NewWindowRequest &request) {
                    if (weakLifetime.expired()) return;
                    onNewWindowRequested(id, instanceId, request);
                },
                .windowCloseRequested = [this, weakLifetime, id, instanceId] {
                    if (weakLifetime.expired()) return;
                    queueWindowClose(id, instanceId);
                },
            });
        } catch (...) {
            tabs.pop_back();
            throw;
        }
        return stored;
    }

    Tab &createTab(engine::PageOwner owner, bool privatePage) {
        Tab &stored = storeTab(owner, privatePage, config.space, false);
        const std::string id = stored.id;
        const std::uint64_t instanceId = stored.instanceId;
        registerTab(stored);
        notify();
        Tab *resolved = findTabInstance(id, instanceId);
        if (!resolved)
            throw std::runtime_error("The newly created tab was removed by a session observer.");
        return *resolved;
    }

    void discardProvisionalTab(std::string_view id, std::uint64_t instanceId) {
        const std::string wanted = lowerAscii(id);
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const Tab &tab) {
            return tab.id == wanted && tab.instanceId == instanceId;
        });
        if (found != tabs.end()) {
            (void)cancelPendingPermissionForTab(found->id, found->instanceId, false);
            cancelRendererTasks(*found);
            tabs.erase(found);
        }
    }

    void onNewWindowRequested(
        std::string_view openerId,
        std::uint64_t openerInstanceId,
        const engine::NewWindowRequest &request
    ) noexcept {
        const Tab *opener = findTabInstance(openerId, openerInstanceId);
        if (!opener || !request.openIn)
            return;
        const engine::PageOwner owner = opener->owner;
        const bool privatePage = opener->privatePage;
        const std::string space = opener->space;
        if (owner == engine::PageOwner::agent && (!config.profileActive || !config.agentEnabled))
            return;

        std::string childId;
        std::uint64_t childInstanceId = 0;
        std::vector<std::string> previousAgentIds;
        std::optional<std::string> previousActiveUserId;
        std::optional<std::string> previousActiveAgentId;
        bool previousAgentPaneVisible = false;
        bool registrationStarted = false;
        const std::optional<std::uint64_t> previousPermissionId = pendingPermission
            ? std::optional<std::uint64_t>(pendingPermission->prompt.id)
            : std::nullopt;
        const auto notifyIfPermissionCancelled = [&] {
            if (previousPermissionId
                && (!pendingPermission || pendingPermission->prompt.id != *previousPermissionId)) {
                notify();
            }
        };
        const auto restoreRegistration = [&] {
            agentIds = std::move(previousAgentIds);
            activeUserId = std::move(previousActiveUserId);
            activeAgentId = std::move(previousActiveAgentId);
            agentPaneVisible = previousAgentPaneVisible;
        };

        try {
            previousAgentIds = agentIds;
            previousActiveUserId = activeUserId;
            previousActiveAgentId = activeAgentId;
            previousAgentPaneVisible = agentPaneVisible;

            Tab &child = storeTab(owner, privatePage, space, true);
            childId = child.id;
            childInstanceId = child.instanceId;
            registrationStarted = true;
            registerTab(child);
            if (!request.openIn(*child.page)) {
                restoreRegistration();
                discardProvisionalTab(childId, childInstanceId);
                notifyIfPermissionCancelled();
                return;
            }
        } catch (...) {
            if (registrationStarted)
                restoreRegistration();
            if (!childId.empty())
                discardProvisionalTab(childId, childInstanceId);
            notifyIfPermissionCancelled();
            return;
        }

        Tab *adopted = findTabInstance(childId, childInstanceId);
        if (!adopted) {
            restoreRegistration();
            notifyIfPermissionCancelled();
            return;
        }
        adopted->adoptingPopup = false;
        notify();
    }

    void queueWindowClose(std::string id, std::uint64_t instanceId) {
        const std::weak_ptr<int> weakLifetime = lifetime;
        loop.post([this, weakLifetime, id = std::move(id), instanceId] {
            if (weakLifetime.expired())
                return;
            removeTab(id, true, instanceId, true);
        });
    }

    engine::NavigationToken navigate(Tab &tab, std::string_view input) {
        const std::string target = normalizedNavigationInput(input);
        if (target.empty())
            return 0;
        prepareNavigation(tab);
        tab.state.url = target;
        tab.state.error.reset();
        engine::NavigationToken token = 0;
        try {
            token = tab.page->navigate(target);
        } catch (...) {
            tab.awaitingLoadStart = false;
            throw;
        }
        notify();
        return token;
    }

    bool navigateUserTab(std::string_view id, std::string_view input) {
        Tab *tab = findTab(id);
        if (!tab || tab->owner != engine::PageOwner::user)
            return false;
        try {
            return navigate(*tab, input) != 0;
        } catch (const std::exception &navigationError) {
            tab = findTab(id);
            if (tab) {
                tab->state.error = navigationError.what();
                notify();
            }
        } catch (...) {
            tab = findTab(id);
            if (tab) {
                tab->state.error = "Chromium could not start the requested navigation.";
                notify();
            }
        }
        return false;
    }

    bool traverseUserHistory(std::string_view id, bool backwards) {
        Tab *tab = findTab(id);
        if (!tab || tab->owner != engine::PageOwner::user)
            return false;
        const std::string canonicalId = tab->id;
        const std::uint64_t instanceId = tab->instanceId;
        prepareNavigation(*tab);
        engine::NavigationToken token = 0;
        try {
            token = backwards ? tab->page->goBack() : tab->page->goForward();
        } catch (const std::exception &navigationError) {
            tab = findTabInstance(canonicalId, instanceId);
            if (tab) {
                tab->awaitingLoadStart = false;
                tab->state.error = navigationError.what();
                notify();
            }
            return false;
        } catch (...) {
            tab = findTabInstance(canonicalId, instanceId);
            if (tab) {
                tab->awaitingLoadStart = false;
                tab->state.error = "Chromium could not traverse page history.";
                notify();
            }
            return false;
        }
        tab = findTabInstance(canonicalId, instanceId);
        if (!tab)
            return false;
        if (token == 0) {
            tab->awaitingLoadStart = false;
            tab->state.error = "Chromium could not traverse page history.";
            notify();
            return false;
        }
        notify();
        return true;
    }

    bool reloadUserTab(std::string_view id) {
        Tab *tab = findTab(id);
        if (!tab || tab->owner != engine::PageOwner::user)
            return false;
        const std::string canonicalId = tab->id;
        const std::uint64_t instanceId = tab->instanceId;
        cancelRendererTasks(*tab);
        tab->rendererCrashCount = 0;
        tab->rendererRecovery = RendererRecoveryState::healthy;
        tab->lastRendererTermination.reset();
        tab->state.error.reset();
        prepareNavigation(*tab);
        engine::NavigationToken token = 0;
        try {
            token = tab->page->reload();
        } catch (const std::exception &error) {
            tab = findTabInstance(canonicalId, instanceId);
            if (tab) {
                tab->awaitingLoadStart = false;
                tab->rendererRecovery = RendererRecoveryState::failed;
                tab->state.error = error.what();
                notify();
            }
            return false;
        } catch (...) {
            tab = findTabInstance(canonicalId, instanceId);
            if (tab) {
                tab->awaitingLoadStart = false;
                tab->rendererRecovery = RendererRecoveryState::failed;
                tab->state.error = std::string(rendererRecoveryFailed);
                notify();
            }
            return false;
        }
        tab = findTabInstance(canonicalId, instanceId);
        if (!tab)
            return false;
        if (token == 0) {
            tab->awaitingLoadStart = false;
            tab->rendererRecovery = RendererRecoveryState::failed;
            tab->state.error = std::string(rendererRecoveryFailed);
            notify();
            return false;
        }
        notify();
        return true;
    }

    Tab &newUserTab(std::string_view url, bool privatePage) {
        Tab &tab = createTab(engine::PageOwner::user, privatePage);
        if (!url.empty())
            (void)navigate(tab, url);
        return tab;
    }

    Tab &newAgentTab(std::string_view url) {
        Tab &tab = createTab(engine::PageOwner::agent, false);
        if (!url.empty())
            (void)navigate(tab, url);
        return tab;
    }

    void onStateChanged(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::PageState &state
    ) {
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab)
            return;
        // A cancelled recovery publishes an untagged terminal state before its
        // tagged NavigationResult. Keep the explicit profile failure visible
        // across deactivate -> immediate reactivate until a newer navigation
        // takes ownership in prepareNavigation().
        if (tab->profileCancellationGeneration
            && *tab->profileCancellationGeneration == tab->recoveryGeneration) {
            if (!state.loading)
                return;
            // A fresh loadStarted is the only untagged engine event that can
            // prove a newer in-page/link navigation took ownership.
            tab->profileCancellationGeneration.reset();
        }
        // Qt can publish a final rollback state from the dead load after the
        // typed termination callback. It must not cancel the queued recovery;
        // an actual user navigation emits loadStarted (or goes through the
        // session retry seam, which cancels the task explicitly).
        if (tab->recoveryTask
            && tab->rendererRecovery == RendererRecoveryState::recovering
            && !state.loading) {
            return;
        }
        const bool loadStarted = state.loading && !tab->state.loading;
        const bool urlChanged = !state.url.empty() && state.url != tab->state.url;
        if (tab->awaitingLoadStart) {
            if (loadStarted)
                tab->awaitingLoadStart = false;
        } else if (urlChanged
            && state.loading
            && tab->state.loading
            && current
            && current->waitingToken
            && operationTargets(current, *tab)) {
            // A redirect changes origin/document identity but remains part of
            // the exact navigation token the protocol operation is awaiting.
            invalidateDocument(*tab, true);
        } else if (loadStarted || urlChanged) {
            invalidateDocument(*tab);
        }
        const std::string requestedUrl = tab->state.url;
        tab->state = state;
        tab->state.id = tab->id;
        tab->state.owner = tab->owner;
        if (tab->state.title.empty())
            tab->state.title = tab->fallbackTitle;
        if (tab->state.url.empty() && !requestedUrl.empty())
            tab->state.url = requestedUrl;
        const bool adoptingPopup = tab->adoptingPopup;
        maybeResumeWait(*tab);
        if (!adoptingPopup)
            notify();
    }

    void scheduleRendererStability(Tab &tab) {
        if (tab.stabilityTask)
            tab.stabilityTask->cancel();
        tab.stabilityTask.reset();
        tab.rendererRecovery = RendererRecoveryState::recovering;
        const std::uint64_t recoveryGeneration = tab.recoveryGeneration;
        const std::uint64_t documentEpoch = tab.documentEpoch;
        const std::string id = tab.id;
        const std::uint64_t instanceId = tab.instanceId;
        const std::weak_ptr<int> weakLifetime = lifetime;
        tab.stabilityTask = loop.scheduleAfter(
            std::max(config.rendererStabilityDelay, std::chrono::milliseconds::zero()),
            [this, weakLifetime, id, instanceId, recoveryGeneration, documentEpoch] {
                if (weakLifetime.expired())
                    return;
                Tab *stable = findTabInstance(id, instanceId);
                if (!stable
                    || stable->recoveryGeneration != recoveryGeneration
                    || stable->documentEpoch != documentEpoch
                    || stable->rendererRecovery != RendererRecoveryState::recovering
                    || !config.profileActive
                    || stable->state.loading
                    || stable->state.error) {
                    return;
                }
                stable->stabilityTask.reset();
                stable->rendererCrashCount = 0;
                stable->rendererRecovery = RendererRecoveryState::healthy;
                notify();
            }
        );
    }

    void onRendererTerminated(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::RendererTermination &termination
    ) {
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab)
            return;
        const std::string terminationError = termination.state.error.value_or(
            "Chromium renderer terminated."
        );
        if (current && operationTargets(current, *tab, false))
            finish(current, core::Json(nullptr), terminationError);

        cancelRendererTasks(*tab);
        invalidateDocument(*tab);
        tab = findTabInstance(id, instanceId);
        if (!tab)
            return;
        const std::string requestedUrl = tab->state.url;
        tab->state = termination.state;
        tab->state.id = tab->id;
        tab->state.owner = tab->owner;
        if (tab->state.title.empty())
            tab->state.title = tab->fallbackTitle;
        if (tab->state.url.empty())
            tab->state.url = requestedUrl;
        tab->state.loading = false;
        tab->lastRendererTermination = termination.kind;

        if (tab->owner == engine::PageOwner::agent) {
            tab->rendererRecovery = RendererRecoveryState::failed;
            tab->state.error = terminationError;
            notify();
            return;
        }

        if (!config.profileActive || tab->state.url.empty() || tab->rendererCrashCount >= 1) {
            ++tab->rendererCrashCount;
            tab->rendererRecovery = RendererRecoveryState::failed;
            tab->state.error = std::string(repeatedRendererCrash);
            notify();
            return;
        }

        ++tab->rendererCrashCount;
        tab->rendererRecovery = RendererRecoveryState::recovering;
        tab->state.error.reset();
        const std::uint64_t recoveryGeneration = tab->recoveryGeneration;
        const std::string tabId = tab->id;
        const std::uint64_t tabInstanceId = tab->instanceId;
        const std::weak_ptr<int> weakLifetime = lifetime;
        tab->recoveryTask = loop.scheduleAfter(
            std::max(config.rendererRecoveryDelay, std::chrono::milliseconds::zero()),
            [this, weakLifetime, tabId, tabInstanceId, recoveryGeneration] {
                if (weakLifetime.expired())
                    return;
                Tab *recovering = findTabInstance(tabId, tabInstanceId);
                if (!recovering
                    || recovering->recoveryGeneration != recoveryGeneration
                    || recovering->rendererRecovery != RendererRecoveryState::recovering
                    || recovering->owner != engine::PageOwner::user
                    || !config.profileActive) {
                    return;
                }
                recovering->recoveryTask.reset();
                prepareNavigation(*recovering);
                recovering->state.error.reset();
                engine::NavigationToken token = 0;
                try {
                    token = recovering->page->reload();
                } catch (const std::exception &error) {
                    recovering = findTabInstance(tabId, tabInstanceId);
                    if (!recovering)
                        return;
                    recovering->awaitingLoadStart = false;
                    recovering->rendererRecovery = RendererRecoveryState::failed;
                    recovering->state.error = error.what();
                    notify();
                    return;
                } catch (...) {
                    recovering = findTabInstance(tabId, tabInstanceId);
                    if (!recovering)
                        return;
                    recovering->awaitingLoadStart = false;
                    recovering->rendererRecovery = RendererRecoveryState::failed;
                    recovering->state.error = std::string(rendererRecoveryFailed);
                    notify();
                    return;
                }
                recovering = findTabInstance(tabId, tabInstanceId);
                if (!recovering)
                    return;
                if (token == 0) {
                    recovering->awaitingLoadStart = false;
                    recovering->rendererRecovery = RendererRecoveryState::failed;
                    recovering->state.error = std::string(rendererRecoveryFailed);
                } else {
                    recovering->recoveryToken = token;
                }
                notify();
            }
        );
        notify();
    }

    void onNavigationFinished(
        std::string_view id,
        std::uint64_t instanceId,
        const engine::NavigationResult &result
    ) {
        Tab *tab = findTabInstance(id, instanceId);
        if (!tab)
            return;
        // Recovery cancellation is administrative, not a new document result.
        // Its bounded token tombstone survives profile reactivation so a late
        // success cannot clear the failure or arm a new stability window.
        if (isCancelledRecoveryToken(*tab, result.token))
            return;
        const bool recoveryNavigation = tab->recoveryToken
            && *tab->recoveryToken == result.token;
        tab->navigations[result.token] = result;
        while (tab->navigations.size() > 8)
            tab->navigations.erase(tab->navigations.begin());
        onStateChanged(id, instanceId, result.state);
        tab = findTabInstance(id, instanceId);
        if (!tab)
            return;
        tab->awaitingLoadStart = false;
        if (recoveryNavigation)
            tab->recoveryToken.reset();
        if (result.error) {
            if (result.error->code != engine::EngineErrorCode::rendererTerminated
                && tab->rendererRecovery == RendererRecoveryState::recovering) {
                if (tab->stabilityTask)
                    tab->stabilityTask->cancel();
                tab->stabilityTask.reset();
                tab->rendererRecovery = RendererRecoveryState::failed;
                if (!tab->state.error)
                    tab->state.error = result.error->message;
                notify();
            }
        } else if (recoveryNavigation && tab->owner == engine::PageOwner::agent) {
            tab->rendererCrashCount = 0;
            tab->rendererRecovery = RendererRecoveryState::healthy;
            tab->state.error.reset();
            notify();
        } else if (tab->rendererCrashCount > 0) {
            if (config.profileActive) {
                scheduleRendererStability(*tab);
            } else {
                tab->rendererRecovery = RendererRecoveryState::failed;
                tab->state.error = std::string(repeatedRendererCrash);
                notify();
            }
        }
        if (!tab->privatePage && !result.error && hasHttpHost(tab->state.url))
            config.library->recordVisit(tab->state);
        maybeResumeWait(*tab);
    }

    void clearWait(const std::shared_ptr<Operation> &operation) {
        if (operation->readyTimer) operation->readyTimer->cancel();
        if (operation->timeoutTimer) operation->timeoutTimer->cancel();
        if (operation->callbackTimer) operation->callbackTimer->cancel();
        operation->readyTimer.reset();
        operation->timeoutTimer.reset();
        operation->callbackTimer.reset();
        operation->waitingToken.reset();
        operation->afterLoad = {};
    }

    void armCallbackDeadline(const std::shared_ptr<Operation> &operation) {
        if (operation->callbackTimer)
            operation->callbackTimer->cancel();
        const std::weak_ptr<int> weakLifetime = lifetime;
        operation->callbackTimer = loop.scheduleAfter(
            std::chrono::seconds(20),
            [this, weakLifetime, operation] {
                if (weakLifetime.expired() || operation->finished)
                    return;
                finish(operation, core::Json(nullptr), std::string(agentCallbackTimedOut));
            }
        );
    }

    void finish(
        const std::shared_ptr<Operation> &operation,
        core::Json result,
        std::optional<std::string> error = std::nullopt
    ) {
        // Callers commonly pass the `current` member itself. Retain the
        // operation before resetting that member so the reference cannot turn
        // into an empty shared_ptr midway through completion.
        const std::shared_ptr<Operation> retained = operation;
        if (!retained || retained->finished)
            return;
        // Resolve the tab while the operation is still live: operationTargets()
        // refuses a finished operation, and `close` has already dropped its tab.
        Tab *resolved = !error && retained->recordBuilder ? operationTab(retained) : nullptr;
        retained->finished = true;
        clearWait(retained);
        if (!error && retained->recordBuilder) {
            auto builder = std::move(retained->recordBuilder);
            retained->recordBuilder = nullptr;
            try {
                record(builder(resolved));
            } catch (...) {
                // A log entry must never turn a completed command into a failure.
            }
        }
        if (current == retained)
            current.reset();
        auto completion = std::move(retained->completion);
        if (completion) {
            try {
                completion(std::move(result), std::move(error));
            } catch (...) {
                // A protocol consumer must not unwind through engine or Qt callbacks.
            }
        }
    }

    std::string downloadName(std::string_view id) const {
        const core::Json entries = config.library->downloads();
        if (!entries.isArray())
            return {};
        const std::string wanted = lowerAscii(id);
        for (const core::Json &entry : entries.asArray()) {
            if (!entry.isObject())
                continue;
            const core::Json *value = field(entry.asObject(), "id");
            if (!value || !value->isString() || lowerAscii(value->asString()) != wanted)
                continue;
            const core::Json *name = field(entry.asObject(), "name");
            return name && name->isString() ? name->asString() : std::string();
        }
        return {};
    }
    /// The shared tail of BrowserModel.handle: host of the page the command
    /// ended on, the tab title when there is no host, attributed to the tab's
    /// space. Falls back to the state captured now once the tab is gone, which
    /// is what `close` needs.
    static std::function<AgentEvent(Tab *)> tailRecord(std::string action, const Tab &tab) {
        const std::string fallbackTitle = tab.state.title.empty() ? tab.fallbackTitle : tab.state.title;
        const std::string fallbackUrl = tab.state.url;
        const std::string fallbackSpace = tab.space;
        return [action = std::move(action), fallbackTitle, fallbackUrl, fallbackSpace](Tab *resolved) {
            const std::string url = resolved ? resolved->state.url : fallbackUrl;
            const std::string title = resolved
                ? (resolved->state.title.empty() ? resolved->fallbackTitle : resolved->state.title)
                : fallbackTitle;
            return AgentEvent{
                action,
                hostOf(url).value_or(title),
                resolved ? resolved->space : fallbackSpace,
            };
        };
    }
    void record(AgentEvent event) {
        if (event.space.empty())
            event.space = config.space;
        events.insert(events.begin(), event);
        if (events.size() > 50)
            events.resize(50);
        if (agentEventObserver) {
            try {
                agentEventObserver(event);
            } catch (...) {
                // Same reasoning as in finish(): logging is not a failure path.
            }
        }
    }
    bool valid(const std::shared_ptr<Operation> &operation) {
        if (!operation || operation->finished)
            return false;
        if (!config.profileActive || !config.agentEnabled || operation->generation != generation) {
            finish(operation, core::Json(nullptr), std::string(operationPaused));
            return false;
        }
        return true;
    }

    void continueAfterLoad(const std::shared_ptr<Operation> &operation, Tab &tab) {
        if (!valid(operation))
            return;
        if (!operationTargets(operation, tab)) {
            finish(operation, core::Json(nullptr), std::string(operationPaused));
            return;
        }
        auto continuation = std::move(operation->afterLoad);
        clearWait(operation);
        if (tab.state.error) {
            finish(operation, core::Json(nullptr), *tab.state.error);
            return;
        }
        if (continuation)
            continuation();
    }

    void evaluateWait(const std::shared_ptr<Operation> &operation, bool deadlineReached = false) {
        if (!valid(operation))
            return;
        Tab *tab = operationTab(operation);
        if (!tab || tab->owner != engine::PageOwner::agent) {
            finish(operation, core::Json(nullptr), std::string(operationPaused));
            return;
        }
        if (operation->waitingToken) {
            const auto found = tab->navigations.find(*operation->waitingToken);
            if (found != tab->navigations.end()) {
                const auto navigation = found->second;
                tab->navigations.erase(found);
                if (navigation.error) {
                    finish(operation, core::Json(nullptr), navigation.error->message);
                    return;
                }
                continueAfterLoad(operation, *tab);
                return;
            }
            if (deadlineReached)
                finish(operation, core::Json(nullptr), std::string(stillLoading));
            return;
        }
        if (!tab->state.loading) {
            continueAfterLoad(operation, *tab);
            return;
        }
        if (deadlineReached)
            finish(operation, core::Json(nullptr), std::string(stillLoading));
    }

    void waitForLoad(
        const std::shared_ptr<Operation> &operation,
        Tab &tab,
        std::optional<engine::NavigationToken> token,
        std::function<void()> continuation
    ) {
        bindOperation(operation, tab);
        operation->waitingToken = token && *token != 0 ? token : std::nullopt;
        operation->afterLoad = std::move(continuation);
        operation->readyElapsed = false;
        const std::weak_ptr<int> weakLifetime = lifetime;
        operation->readyTimer = loop.scheduleAfter(std::chrono::milliseconds(150), [this, weakLifetime, operation] {
            if (weakLifetime.expired() || operation->finished) return;
            operation->readyElapsed = true;
            evaluateWait(operation);
        });
        operation->timeoutTimer = loop.scheduleAfter(std::chrono::seconds(20), [this, weakLifetime, operation] {
            if (weakLifetime.expired() || operation->finished) return;
            operation->readyElapsed = true;
            evaluateWait(operation, true);
        });
    }

    void maybeResumeWait(Tab &tab) {
        if (!current || current->finished || !current->readyElapsed
            || current->targetInstanceId == 0 || !operationTargets(current, tab)) {
            return;
        }
        if (!tab.state.loading || (current->waitingToken && tab.navigations.contains(*current->waitingToken)))
            evaluateWait(current);
    }

    core::Json tabInfo(const Tab &tab) const {
        core::Json::Object result;
        result.emplace("id", core::Json(tab.id));
        result.emplace("title", core::Json(tab.state.title.empty() ? tab.fallbackTitle : tab.state.title));
        result.emplace("url", core::Json(tab.state.url));
        result.emplace("space", core::Json(tab.space));
        result.emplace("owner", core::Json(isAgent(tab.id) ? "agent" : "user"));
        result.emplace("agentActive", core::Json(activeAgentId && lowerAscii(*activeAgentId) == tab.id));
        result.emplace("pinned", core::Json(tab.pinned));
        result.emplace("active", core::Json(activeUserId && lowerAscii(*activeUserId) == tab.id));
        result.emplace("loading", core::Json(tab.state.loading));
        result.emplace("error", tab.state.error ? core::Json(*tab.state.error) : core::Json(nullptr));
        return core::Json(std::move(result));
    }

    core::Json tabInfoSnapshot(
        const engine::PageState &state,
        std::string_view space,
        std::string_view fallbackTitle,
        bool pinned
    ) const {
        Tab snapshot;
        snapshot.id = lowerAscii(state.id);
        snapshot.owner = state.owner;
        snapshot.state = state;
        snapshot.state.id = snapshot.id;
        snapshot.space = std::string(space);
        snapshot.fallbackTitle = std::string(fallbackTitle);
        snapshot.pinned = pinned;
        return tabInfo(snapshot);
    }

    core::Json allTabs() const {
        core::Json::Array values;
        values.reserve(tabs.size());
        for (const Tab &tab : tabs)
            values.push_back(tabInfo(tab));
        core::Json::Object result;
        result.emplace("tabs", core::Json(std::move(values)));
        result.emplace("space", core::Json(config.space));
        return core::Json(std::move(result));
    }

    bool removeTab(
        std::string_view id,
        bool cancelPendingWhenAgentsEnd,
        std::optional<std::uint64_t> expectedInstanceId = std::nullopt,
        bool cancelPendingForTab = true
    ) {
        const std::string wanted = lowerAscii(id);
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const Tab &tab) {
            return tab.id == wanted && (!expectedInstanceId || tab.instanceId == *expectedInstanceId);
        });
        if (found == tabs.end())
            return false;
        const bool wasAgent = found->owner == engine::PageOwner::agent;
        const bool cancelTargetOperation = cancelPendingForTab
            && current
            && operationTargets(current, *found, false);
        (void)cancelPendingPermissionForTab(found->id, found->instanceId, false);
        cancelRendererTasks(*found);
        tabs.erase(found);
        agentIds.erase(std::remove_if(agentIds.begin(), agentIds.end(), [&wanted](const std::string &candidate) {
            return lowerAscii(candidate) == wanted;
        }), agentIds.end());
        if (activeAgentId && lowerAscii(*activeAgentId) == wanted)
            activeAgentId = agentIds.empty() ? std::nullopt : std::optional<std::string>(agentIds.back());
        if (activeUserId && lowerAscii(*activeUserId) == wanted) {
            activeUserId.reset();
            for (auto iterator = tabs.rbegin(); iterator != tabs.rend(); ++iterator) {
                if (iterator->owner == engine::PageOwner::user) {
                    activeUserId = iterator->id;
                    break;
                }
            }
        }
        bool cancelOperation = cancelTargetOperation;
        if (wasAgent && agentIds.empty()) {
            agentPaneVisible = false;
            ++generation;
            cancelOperation = cancelOperation || cancelPendingWhenAgentsEnd;
        }
        if (cancelOperation && current)
            finish(current, core::Json(nullptr), std::string(operationPaused));
        notify();
        return true;
    }

    Tab *target(const core::Json::Object &request, std::string &targetError) {
        if (const auto requested = stringField(request, "tab")) {
            Tab *tab = findTab(*requested);
            if (!tab) {
                targetError = unknownTab;
                return nullptr;
            }
            if (tab->owner != engine::PageOwner::agent) {
                targetError = userTab;
                return nullptr;
            }
            activeAgentId = tab->id;
            notify();
            return tab;
        }
        if (activeAgentId) {
            if (Tab *tab = findTab(*activeAgentId))
                return tab;
        }
        return &newAgentTab({});
    }

    void completeTab(const std::shared_ptr<Operation> &operation) {
        if (!valid(operation))
            return;
        Tab *tab = operationTab(operation);
        if (!tab || tab->owner != engine::PageOwner::agent) {
            finish(operation, core::Json(nullptr), std::string(operationPaused));
            return;
        }
        finish(operation, tabInfo(*tab));
    }

    void execute(std::string command, const core::Json::Object &request, ProtocolCompletion completion) {
        if (current && !current->finished) {
            completion(core::Json(nullptr), "Agent is busy. Retry after the current command completes.");
            return;
        }
        auto operation = std::make_shared<Operation>();
        operation->generation = generation;
        operation->completion = std::move(completion);
        current = operation;

        if ((command == "history" || command == "downloads") && !config.libraryAccess) {
            finish(operation, core::Json(nullptr), std::string(libraryBlocked));
            return;
        }
        if (command == "history") {
            const std::size_t limit = static_cast<std::size_t>(std::clamp<std::int64_t>(integerField(request, "limit", 50), 1, 200));
            const std::string query = stringField(request, "query").value_or("");
            core::Json::Object result;
            result.emplace("history", config.library->history(query, limit));
            finish(operation, core::Json(std::move(result)));
            return;
        }
        if (command == "downloads") {
            core::Json::Object result;
            result.emplace("downloads", config.library->downloads());
            result.emplace("directory", core::Json(config.library->downloadDirectory()));
            finish(operation, core::Json(std::move(result)));
            return;
        }
        if (command == "cancel-download") {
            const auto id = stringField(request, "id");
            // Read the name before cancelling; the entry may be rewritten by
            // the time the library reports back.
            const std::string name = id ? downloadName(*id) : std::string();
            const CancelDownloadResult cancelled = id
                ? config.library->cancelDownload(*id)
                : CancelDownloadResult::unknown;
            if (cancelled == CancelDownloadResult::unknown) {
                finish(operation, core::Json(nullptr), "Unbekannter Download.");
            } else if (cancelled == CancelDownloadResult::inactive) {
                finish(operation, core::Json(nullptr), "Download ist nicht aktiv.");
            } else {
                core::Json::Object result;
                result.emplace("cancelRequested", core::Json(*id));
                operation->recordBuilder = [name](Tab *) {
                    return AgentEvent{"cancel-download", name, {}};
                };
                finish(operation, core::Json(std::move(result)));
            }
            return;
        }
        if (command == "new") {
            Tab &tab = newAgentTab({});
            operation->recordBuilder = [](Tab *resolved) {
                const auto host = resolved ? hostOf(resolved->state.url) : std::nullopt;
                return AgentEvent{"new", host.value_or("Neuer Tab"), {}};
            };
            const auto url = stringField(request, "url");
            if (!url) {
                finish(operation, tabInfo(tab));
                return;
            }
            try {
                const engine::NavigationToken token = navigate(tab, *url);
                waitForLoad(operation, tab, token, [this, operation] { completeTab(operation); });
            } catch (const std::exception &error) {
                finish(operation, core::Json(nullptr), error.what());
            }
            return;
        }
        if (command == "split") {
            Tab *tab = activeAgentId ? findTab(*activeAgentId) : nullptr;
            if (!tab)
                tab = &newAgentTab({});
            core::Json::Object result;
            result.emplace("split", core::Json(tab->id));
            finish(operation, core::Json(std::move(result)));
            return;
        }

        // Local development verification only. Never advertised in `status`,
        // never available without the environment opt-in, and it never touches
        // a tab, exactly as in the WebKit build.
        if (command == "capture-window") {
            const char *optIn = std::getenv("YOBRO_DEV_CAPTURE");
            if (!windowCapture || !optIn || std::string_view(optIn) != "1") {
                finish(operation, core::Json(nullptr), "Unbekannter Befehl: " + command);
                return;
            }
            std::string captureError;
            std::optional<std::string> path;
            try {
                path = windowCapture(captureError);
            } catch (const std::exception &error) {
                captureError = error.what();
                path.reset();
            }
            if (!path) {
                finish(operation, core::Json(nullptr), captureError.empty() ? "Kein Fenster." : captureError);
                return;
            }
            core::Json::Object result;
            result.emplace("path", core::Json(*path));
            finish(operation, core::Json(std::move(result)));
            return;
        }
        std::string targetError;
        Tab *tab = target(request, targetError);
        if (!tab) {
            finish(operation, core::Json(nullptr), std::move(targetError));
            return;
        }
        const std::string id = tab->id;

        if (command == "find") {
            const auto query = stringField(request, "query");
            if (!query) {
                finish(operation, core::Json(nullptr), "Suchtext fehlt.");
                return;
            }
            waitForLoad(operation, *tab, std::nullopt, [this, operation, query = *query, backwards = boolField(request, "backwards", false)] {
                if (!valid(operation)) return;
                Tab *currentTab = operationTab(operation);
                if (!currentTab) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                const std::weak_ptr<int> weakLifetime = lifetime;
                armCallbackDeadline(operation);
                currentTab->page->findText(query, backwards, [this, weakLifetime, operation, query](bool found, std::optional<engine::EngineError> error) mutable {
                    loop.post([this, weakLifetime, operation, query, found, error = std::move(error)]() mutable {
                        if (weakLifetime.expired() || !valid(operation)) return;
                        Tab *resolved = operationTab(operation);
                        if (!resolved) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                        if (error) { finish(operation, core::Json(nullptr), error->message); return; }
                        core::Json::Object result;
                        result.emplace("tab", tabInfo(*resolved));
                        result.emplace("found", core::Json(found));
                        result.emplace("query", core::Json(query));
                        operation->recordBuilder = [](Tab *tabForLog) {
                            const auto host = tabForLog ? hostOf(tabForLog->state.url) : std::nullopt;
                            return AgentEvent{"find", host.value_or("Seitensuche"), {}};
                        };
                        finish(operation, core::Json(std::move(result)));
                    });
                });
            });
            return;
        }
        if (command == "duplicate") {
            const std::string sourceTitle = tab->state.title.empty() ? tab->fallbackTitle : tab->state.title;
            Tab &duplicate = newAgentTab(tab->state.url);
            operation->recordBuilder = [sourceTitle](Tab *) {
                return AgentEvent{"duplicate", sourceTitle, {}};
            };
            finish(operation, tabInfo(duplicate));
            return;
        }
        if (command == "download") {
            const auto url = stringField(request, "url");
            if (!url || !hasHttpHost(*url)) {
                finish(operation, core::Json(nullptr), "Gültige HTTP- oder HTTPS-Downloadadresse erforderlich.");
                return;
            }
            bindOperation(operation, *tab);
            operation->recordBuilder = [detail = hostOf(*url).value_or("Datei")](Tab *) {
                return AgentEvent{"download", detail, {}};
            };
            const std::weak_ptr<int> weakLifetime = lifetime;
            config.library->startDownload(*tab->page, *url, [this, weakLifetime, operation](std::optional<std::string> error) mutable {
                loop.post([this, weakLifetime, operation, error = std::move(error)]() mutable {
                    if (weakLifetime.expired() || !valid(operation)) return;
                    if (!operationTab(operation)) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                    if (error) { finish(operation, core::Json(nullptr), std::move(*error)); return; }
                    core::Json::Object result;
                    result.emplace("started", core::Json(true));
                    result.emplace("next", core::Json("Use downloads to check progress and retrieve the final file path."));
                    finish(operation, core::Json(std::move(result)));
                });
            });
            return;
        }
        if (command == "focus") {
            operation->recordBuilder = tailRecord("focus", *tab);
            activeAgentId = id;
            notify();
            finish(operation, tabInfo(*tab));
            return;
        }
        if (command == "close") {
            const engine::PageState state = tab->state;
            const std::string tabSpace = tab->space;
            const std::string fallback = tab->fallbackTitle;
            const bool pinned = tab->pinned;
            operation->recordBuilder = tailRecord("close", *tab);
            removeTab(id, false, std::nullopt, false);
            finish(operation, tabInfoSnapshot(state, tabSpace, fallback, pinned));
            return;
        }
        if (command == "open") {
            const auto url = stringField(request, "url");
            if (!url) {
                finish(operation, core::Json(nullptr), "URL fehlt.");
                return;
            }
            operation->recordBuilder = tailRecord("open", *tab);
            try {
                const engine::NavigationToken token = navigate(*tab, *url);
                waitForLoad(operation, *tab, token, [this, operation] { completeTab(operation); });
            } catch (const std::exception &error) {
                finish(operation, core::Json(nullptr), error.what());
            }
            return;
        }
        if (command == "back" || command == "forward" || command == "reload") {
            operation->recordBuilder = tailRecord(command, *tab);
            prepareNavigation(*tab);
            engine::NavigationToken token = 0;
            try {
                if (command == "back") token = tab->page->goBack();
                else if (command == "forward") token = tab->page->goForward();
                else {
                    tab->state.error.reset();
                    token = tab->page->reload();
                }
            } catch (const std::exception &error) {
                tab->awaitingLoadStart = false;
                finish(operation, core::Json(nullptr), error.what());
                return;
            }
            if (command == "reload"
                && token != 0
                && tab->rendererRecovery == RendererRecoveryState::failed) {
                tab->rendererRecovery = RendererRecoveryState::recovering;
                tab->recoveryToken = token;
                notify();
            }
            waitForLoad(operation, *tab, token, [this, operation] { completeTab(operation); });
            return;
        }
        if (command == "pin") {
            operation->recordBuilder = tailRecord("pin", *tab);
            tab->pinned = !tab->pinned;
            notify();
            finish(operation, tabInfo(*tab));
            return;
        }
        if (command == "read") {
            waitForLoad(operation, *tab, std::nullopt, [this, operation] {
                if (!valid(operation)) return;
                Tab *currentTab = operationTab(operation);
                if (!currentTab) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                if (currentTab->state.url.empty()) {
                    finish(operation, core::Json(nullptr), "Dieser Tab zeigt die Startseite. Zuerst eine URL öffnen.");
                    return;
                }
                const std::weak_ptr<int> weakLifetime = lifetime;
                armCallbackDeadline(operation);
                currentTab->page->readAgentSnapshot([this, weakLifetime, operation](std::string json, std::optional<engine::EngineError> error) mutable {
                    loop.post([this, weakLifetime, operation, json = std::move(json), error = std::move(error)]() mutable {
                        if (weakLifetime.expired() || !valid(operation)) return;
                        Tab *resolved = operationTab(operation);
                        if (!resolved) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                        if (error) { finish(operation, core::Json(nullptr), error->message); return; }
                        try {
                            core::Json::Object result;
                            result.emplace("tab", tabInfo(*resolved));
                            result.emplace("page", core::Json::parse(json));
                            operation->recordBuilder = [](Tab *tabForLog) {
                                const auto host = tabForLog ? hostOf(tabForLog->state.url) : std::nullopt;
                                return AgentEvent{
                                    "read",
                                    host.value_or("Seite gelesen"),
                                    tabForLog ? tabForLog->space : std::string(),
                                };
                            };
                            finish(operation, core::Json(std::move(result)));
                        } catch (const std::exception &) {
                            finish(operation, core::Json(nullptr), "The isolated AgentBridge returned an invalid result.");
                        }
                    });
                });
            });
            return;
        }
        if (command == "click" || command == "fill") {
            const auto reference = stringField(request, "ref");
            const auto document = stringField(request, "document");
            if (!reference || !document) {
                finish(operation, core::Json(nullptr), "ref und document aus einem aktuellen read sind erforderlich.");
                return;
            }
            const auto value = stringField(request, "value");
            if (command == "fill" && !value) {
                finish(operation, core::Json(nullptr), "Wert fehlt.");
                return;
            }
            // The WebKit build refuses to inject the bridge into a tab that
            // shows the start page, for read and for actions alike.
            if (tab->state.url.empty()) {
                finish(operation, core::Json(nullptr), "Dieser Tab zeigt die Startseite. Zuerst eine URL öffnen.");
                return;
            }
            if (!valid(operation)) return;
            core::Json::Object action;
            action.emplace("action", core::Json(command));
            action.emplace("ref", core::Json(*reference));
            action.emplace("document", core::Json(*document));
            action.emplace("value", core::Json(value.value_or("")));
            const std::string encoded = core::Json(std::move(action)).serialize();
            bindOperation(operation, *tab);
            operation->recordBuilder = [command = std::string(command), reference = *reference](Tab *tabForLog) {
                const auto host = tabForLog ? hostOf(tabForLog->state.url) : std::nullopt;
                return AgentEvent{command, reference + " · " + host.value_or("Seite"), {}};
            };
            const std::weak_ptr<int> weakLifetime = lifetime;
            armCallbackDeadline(operation);
            tab->page->performAgentAction(encoded, [this, weakLifetime, operation](std::string json, std::optional<engine::EngineError> error) mutable {
                loop.post([this, weakLifetime, operation, json = std::move(json), error = std::move(error)]() mutable {
                    if (weakLifetime.expired() || !valid(operation)) return;
                    Tab *resolved = operationTab(operation);
                    if (!resolved) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                    if (error) { finish(operation, core::Json(nullptr), error->message); return; }
                    try {
                        core::Json::Object result;
                        result.emplace("tab", tabInfo(*resolved));
                        result.emplace("action", core::Json::parse(json));
                        result.emplace("next", core::Json("Read again to verify the outcome; pages may update asynchronously."));
                        finish(operation, core::Json(std::move(result)));
                    } catch (const std::exception &) {
                        finish(operation, core::Json(nullptr), "The isolated AgentBridge returned an invalid result.");
                    }
                });
            });
            return;
        }
        if (command == "scroll") {
            const int amount = static_cast<int>(std::clamp<std::int64_t>(integerField(request, "amount", 600), -5000, 5000));
            bindOperation(operation, *tab);
            operation->recordBuilder = tailRecord("scroll", *tab);
            const std::weak_ptr<int> weakLifetime = lifetime;
            armCallbackDeadline(operation);
            tab->page->scrollBy(amount, [this, weakLifetime, operation](bool, std::optional<engine::EngineError> error) mutable {
                loop.post([this, weakLifetime, operation, error = std::move(error)]() mutable {
                    if (weakLifetime.expired() || !valid(operation)) return;
                    if (!operationTab(operation)) { finish(operation, core::Json(nullptr), std::string(operationPaused)); return; }
                    if (error) { finish(operation, core::Json(nullptr), error->message); return; }
                    completeTab(operation);
                });
            });
            return;
        }

        finish(operation, core::Json(nullptr), "Unbekannter Befehl: " + command);
    }

    std::unique_ptr<engine::BrowserProfile> profile;
    EventLoop &loop;
    BrowserSessionConfig config;
    std::shared_ptr<int> lifetime;
    std::vector<Tab> tabs;
    std::vector<std::string> agentIds;
    std::optional<std::string> activeUserId;
    std::optional<std::string> activeAgentId;
    bool agentPaneVisible = false;
    bool permissionSurfaceVisible = false;
    std::uint64_t generation = 1;
    std::uint64_t nextTabInstanceId = 1;
    std::uint64_t nextPermissionRequestId = 1;
    Observer observer;
    std::vector<AgentEvent> events;
    AgentEventObserver agentEventObserver;
    WindowCapture windowCapture;
    PermissionPromptPresenter permissionPromptPresenter;
    AuthenticationPromptPresenter authenticationPromptPresenter;
    CertificatePromptPresenter certificatePromptPresenter;
    bool certificatePromptActive = false;
    PasskeyPromptPresenter passkeyPromptPresenter;
    DesktopMediaPromptPresenter desktopMediaPromptPresenter;
    bool authenticationPromptActive = false;
    std::shared_ptr<Operation> current;
    std::optional<PendingPermission> pendingPermission;
};

BrowserSession::BrowserSession(
    std::unique_ptr<engine::BrowserProfile> profile,
    EventLoop &eventLoop,
    BrowserSessionConfig config
) : impl_(std::make_unique<Impl>(std::move(profile), eventLoop, std::move(config))) {}

BrowserSession::~BrowserSession() = default;

engine::BrowserProfile &BrowserSession::profile() const { return *impl_->profile; }

engine::BrowserPage &BrowserSession::newUserTab(std::string_view url, bool privatePage) {
    return *impl_->newUserTab(url, privatePage).page;
}

engine::BrowserPage &BrowserSession::newAgentTab(std::string_view url) {
    return *impl_->newAgentTab(url).page;
}

bool BrowserSession::closeTab(std::string_view id) { return impl_->removeTab(id, true); }

bool BrowserSession::navigateUserTab(std::string_view id, std::string_view url) {
    return impl_->navigateUserTab(id, url);
}

bool BrowserSession::goBackUserTab(std::string_view id) {
    return impl_->traverseUserHistory(id, true);
}

bool BrowserSession::goForwardUserTab(std::string_view id) {
    return impl_->traverseUserHistory(id, false);
}

bool BrowserSession::reloadUserTab(std::string_view id) {
    return impl_->reloadUserTab(id);
}

bool BrowserSession::setActiveUserTab(std::string_view id) {
    auto *tab = impl_->findTab(id);
    if (!tab || tab->owner != engine::PageOwner::user) return false;
    if (!impl_->activeUserId || *impl_->activeUserId != tab->id)
        (void)impl_->cancelPendingPermission(false);
    impl_->activeUserId = tab->id;
    impl_->notify();
    return true;
}

bool BrowserSession::setActiveAgentTab(std::string_view id) {
    auto *tab = impl_->findTab(id);
    if (!tab || tab->owner != engine::PageOwner::agent) return false;
    impl_->activeAgentId = tab->id;
    impl_->notify();
    return true;
}

engine::BrowserPage *BrowserSession::activeUserPage() const {
    if (!impl_->activeUserId) return nullptr;
    const auto *tab = impl_->findTab(*impl_->activeUserId);
    return tab ? tab->page.get() : nullptr;
}

engine::BrowserPage *BrowserSession::activeAgentPage() const {
    if (!impl_->activeAgentId) return nullptr;
    const auto *tab = impl_->findTab(*impl_->activeAgentId);
    return tab ? tab->page.get() : nullptr;
}

std::vector<SessionTabView> BrowserSession::tabViews() const {
    std::vector<SessionTabView> result;
    result.reserve(impl_->tabs.size());
    for (const auto &tab : impl_->tabs) {
        result.push_back({
            .page = tab.page.get(),
            .state = tab.state,
            .space = tab.space,
            .pinned = tab.pinned,
            .privatePage = tab.privatePage,
            .rendererRecovery = tab.rendererRecovery,
            .lastRendererTermination = tab.lastRendererTermination,
        });
    }
    return result;
}

std::vector<engine::BrowserPage *> BrowserSession::userPages() const {
    std::vector<engine::BrowserPage *> result;
    for (const auto &tab : impl_->tabs)
        if (tab.owner == engine::PageOwner::user) result.push_back(tab.page.get());
    return result;
}

std::vector<engine::BrowserPage *> BrowserSession::agentPages() const {
    std::vector<engine::BrowserPage *> result;
    for (const auto &tab : impl_->tabs)
        if (tab.owner == engine::PageOwner::agent) result.push_back(tab.page.get());
    return result;
}

std::optional<PermissionPrompt> BrowserSession::pendingPermission() const {
    return impl_->pendingPermission
        ? std::optional<PermissionPrompt>(impl_->pendingPermission->prompt)
        : std::nullopt;
}

bool BrowserSession::resolvePermission(
    std::uint64_t requestId,
    engine::PermissionDecision decision
) {
    return impl_->resolvePermission(requestId, decision);
}

void BrowserSession::setPermissionSurfaceVisible(bool visible) {
    if (impl_->permissionSurfaceVisible == visible)
        return;
    impl_->permissionSurfaceVisible = visible;
    const bool cancelled = !visible && impl_->cancelPendingPermission(false);
    if (cancelled)
        impl_->notify();
}

void BrowserSession::setPermissionPromptPresenter(PermissionPromptPresenter presenter) {
    const bool cancelled = impl_->cancelPendingPermission(false);
    impl_->permissionPromptPresenter = std::move(presenter);
    if (cancelled)
        impl_->notify();
}

void BrowserSession::setAuthenticationPromptPresenter(AuthenticationPromptPresenter presenter) {
    impl_->authenticationPromptPresenter = std::move(presenter);
}

void BrowserSession::setCertificatePromptPresenter(CertificatePromptPresenter presenter) {
    impl_->certificatePromptPresenter = std::move(presenter);
}

void BrowserSession::setDesktopMediaPromptPresenter(DesktopMediaPromptPresenter presenter) {
    impl_->desktopMediaPromptPresenter = std::move(presenter);
}

void BrowserSession::setPasskeyPromptPresenter(PasskeyPromptPresenter presenter) {
    impl_->passkeyPromptPresenter = std::move(presenter);
}

void BrowserSession::setObserver(Observer observer) { impl_->observer = std::move(observer); }

std::vector<AgentEvent> BrowserSession::agentEvents() const { return impl_->events; }

void BrowserSession::setAgentEventObserver(AgentEventObserver observer) {
    impl_->agentEventObserver = std::move(observer);
}

void BrowserSession::setWindowCapture(WindowCapture capture) {
    impl_->windowCapture = std::move(capture);
}

void BrowserSession::setProfileActive(bool active) {
    if (impl_->config.profileActive == active) return;
    impl_->config.profileActive = active;
    if (!active) {
        ++impl_->generation;
        (void)impl_->cancelPendingPermission(false);
        std::vector<std::pair<std::string, std::uint64_t>> recoveringTabs;
        for (auto &tab : impl_->tabs) {
            if (tab.recoveryTask
                || tab.stabilityTask
                || tab.recoveryToken
                || tab.rendererRecovery == RendererRecoveryState::recovering) {
                impl_->cancelRendererTasks(tab);
                tab.profileCancellationGeneration = tab.recoveryGeneration;
                tab.rendererRecovery = RendererRecoveryState::failed;
                tab.state.loading = false;
                tab.state.error = std::string(repeatedRendererCrash);
                recoveringTabs.emplace_back(tab.id, tab.instanceId);
            }
        }
        for (const auto &[id, instanceId] : recoveringTabs) {
            if (auto *tab = impl_->findTabInstance(id, instanceId)) {
                try {
                    tab->page->stop();
                } catch (...) {
                    // Generation and token invalidation already make late results inert.
                }
            }
        }
        if (impl_->current) impl_->finish(impl_->current, core::Json(nullptr), std::string(operationPaused));
    }
    impl_->notify();
}

void BrowserSession::setAgentEnabled(bool enabled) {
    if (impl_->config.agentEnabled == enabled) return;
    impl_->config.agentEnabled = enabled;
    if (!enabled) endAgentWorkspace();
    impl_->notify();
}

void BrowserSession::setLibraryAccess(bool enabled) {
    if (impl_->config.libraryAccess == enabled) return;
    impl_->config.libraryAccess = enabled;
    impl_->notify();
}

bool BrowserSession::agentEnabled() const { return impl_->config.agentEnabled; }
bool BrowserSession::agentPaneVisible() const { return impl_->agentPaneVisible; }

ProtocolHostState BrowserSession::protocolState() const {
    return {
        .profileActive = impl_->config.profileActive,
        .agentEnabled = impl_->config.agentEnabled,
        .agentPaneVisible = impl_->agentPaneVisible,
        .libraryAccess = impl_->config.libraryAccess,
        .browser = impl_->config.browser,
        .version = impl_->config.version,
        .engine = impl_->config.engine,
        .socketPath = impl_->config.socketPath,
        .profileName = impl_->config.profileName,
    };
}

core::Json BrowserSession::tabsResult() const { return impl_->allTabs(); }

void BrowserSession::endAgentWorkspace() {
    impl_->agentPaneVisible = false;
    ++impl_->generation;
    if (impl_->current)
        impl_->finish(impl_->current, core::Json(nullptr), std::string(operationPaused));
    impl_->config.library->endAgentDownloads();
    for (auto &tab : impl_->tabs) {
        if (tab.owner == engine::PageOwner::agent)
            impl_->cancelRendererTasks(tab);
    }
    impl_->tabs.erase(std::remove_if(impl_->tabs.begin(), impl_->tabs.end(), [](const Impl::Tab &tab) {
        return tab.owner == engine::PageOwner::agent;
    }), impl_->tabs.end());
    impl_->agentIds.clear();
    impl_->activeAgentId.reset();
    impl_->notify();
}

void BrowserSession::presentAgentWorkspace() {
    impl_->agentPaneVisible = !impl_->agentIds.empty();
    impl_->notify();
}

void BrowserSession::execute(
    std::string_view command,
    const core::Json::Object &request,
    ProtocolCompletion completion
) {
    impl_->execute(std::string(command), request, std::move(completion));
}

} // namespace yobro::controller
