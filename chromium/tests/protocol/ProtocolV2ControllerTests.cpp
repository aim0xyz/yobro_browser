#include "engine/api/BrowserEngine.hpp"
#include "yobro/controller/BrowserLibrary.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/controller/EventLoop.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/controller/ProtocolV2Host.hpp"
#include "yobro/core/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using yobro::core::Json;
using namespace std::chrono_literals;

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void check(bool condition, std::string_view message) {
    if (!condition) fail(std::string(message));
}

const Json &required(const Json &object, std::string_view key) {
    const Json *value = object.find(key);
    if (!value) fail("Missing JSON field: " + std::string(key));
    return *value;
}

Json decodedResponse(const std::string &encoded) {
    return Json::parse(encoded);
}

const Json &successResult(const Json &response) {
    check(required(response, "ok").asBoolean(), "Expected a successful response.");
    return required(response, "result");
}

std::string errorMessage(const Json &response) {
    check(!required(response, "ok").asBoolean(), "Expected an error response.");
    return required(response, "error").asString();
}

class ManualScheduledTask final : public yobro::controller::ScheduledTask {
public:
    void cancel() noexcept override { cancelled = true; }
    bool cancelled = false;
};

class ManualEventLoop final : public yobro::controller::EventLoop {
public:
    void post(Task task) override { posted.push_back(std::move(task)); }

    std::shared_ptr<yobro::controller::ScheduledTask> scheduleAfter(
        std::chrono::milliseconds delay,
        Task task
    ) override {
        auto token = std::make_shared<ManualScheduledTask>();
        timers.push_back({now + delay, nextTimerId++, token, std::move(task)});
        return token;
    }

    void drain() {
        while (true) {
            bool progressed = false;
            while (!posted.empty()) {
                Task task = std::move(posted.front());
                posted.pop_front();
                task();
                progressed = true;
            }
            auto due = std::min_element(timers.begin(), timers.end(), [](const Timer &left, const Timer &right) {
                return left.due < right.due || (left.due == right.due && left.id < right.id);
            });
            if (due != timers.end() && due->due <= now) {
                Timer timer = std::move(*due);
                timers.erase(due);
                if (!timer.token->cancelled) timer.task();
                progressed = true;
            }
            if (!progressed) return;
        }
    }

    void advance(std::chrono::milliseconds amount) {
        now += amount;
        drain();
    }

private:
    struct Timer {
        std::chrono::milliseconds due;
        std::uint64_t id;
        std::shared_ptr<ManualScheduledTask> token;
        Task task;
    };

    std::chrono::milliseconds now{0};
    std::uint64_t nextTimerId = 1;
    std::deque<Task> posted;
    std::vector<Timer> timers;
};

class FakeSubscription final : public yobro::engine::PageEventSubscription {
public:
    struct Registry {
        std::size_t nextId = 1;
        std::map<std::size_t, yobro::engine::PageEventHandlers> handlers;
    };

    FakeSubscription(std::weak_ptr<Registry> registry, std::size_t id)
        : registry_(std::move(registry)), id_(id) {}
    ~FakeSubscription() override {
        if (auto registry = registry_.lock()) registry->handlers.erase(id_);
    }

private:
    std::weak_ptr<Registry> registry_;
    std::size_t id_;
};

class FakePermissionRequest final : public yobro::engine::PermissionRequest {
public:
    struct Probe {
        std::optional<yobro::engine::PermissionDecision> decision;
        int resolveCount = 0;
    };

    FakePermissionRequest(
        yobro::engine::WebPermission permission,
        std::string origin,
        std::shared_ptr<Probe> probe
    ) : permission_(permission), origin_(std::move(origin)), probe_(std::move(probe)) {}

    ~FakePermissionRequest() override {
        resolve(yobro::engine::PermissionDecision::deny);
    }

    yobro::engine::WebPermission permission() const noexcept override { return permission_; }
    std::string_view origin() const noexcept override { return origin_; }

    void resolve(yobro::engine::PermissionDecision decision) noexcept override {
        if (resolved_)
            return;
        resolved_ = true;
        probe_->decision = decision;
        ++probe_->resolveCount;
    }

private:
    yobro::engine::WebPermission permission_;
    std::string origin_;
    std::shared_ptr<Probe> probe_;
    bool resolved_ = false;
};

class FakePage final : public yobro::engine::BrowserPage {
public:
    FakePage(std::string id, yobro::engine::PageOwner owner)
        : registry_(std::make_shared<FakeSubscription::Registry>()) {
        state_.id = std::move(id);
        state_.owner = owner;
        state_.title = owner == yobro::engine::PageOwner::agent ? "Agent page" : "User page";
    }

    yobro::engine::PageState state() const override { return state_; }

    std::unique_ptr<yobro::engine::PageEventSubscription> subscribe(
        yobro::engine::PageEventHandlers handlers
    ) override {
        const std::size_t id = registry_->nextId++;
        registry_->handlers.emplace(id, std::move(handlers));
        return std::make_unique<FakeSubscription>(registry_, id);
    }

    void setPermissionRequestHandler(
        yobro::engine::PermissionRequestHandler handler
    ) override {
        permissionRequestHandler_ = std::move(handler);
    }

    void setAuthenticationChallengeHandler(
        yobro::engine::AuthenticationChallengeHandler handler
    ) override {
        authenticationChallengeHandler_ = std::move(handler);
    }

    std::optional<yobro::engine::AuthenticationCredentials> requestAuthentication(
        yobro::engine::AuthenticationChallenge challenge
    ) {
        return authenticationChallengeHandler_
            ? authenticationChallengeHandler_(challenge) : std::nullopt;
    }

    yobro::engine::NavigationToken navigate(std::string_view url) override {
        state_.url = std::string(url);
        const std::size_t scheme = url.find("://");
        const std::size_t originEnd = scheme == std::string_view::npos
            ? std::string_view::npos
            : url.find_first_of("/?#", scheme + 3);
        state_.securityOrigin = scheme == std::string_view::npos
            ? std::string()
            : std::string(url.substr(0, originEnd));
        return beginNavigation();
    }
    void stop() override {
        ++stopCount;
        if (state_.loading && !deferStop)
            finishNavigation(false, "Stopped");
    }
    yobro::engine::NavigationToken reload() override {
        ++reloadCount;
        return beginNavigation();
    }
    yobro::engine::NavigationToken goBack() override {
        ++backCount;
        return beginNavigation();
    }
    yobro::engine::NavigationToken goForward() override {
        ++forwardCount;
        return beginNavigation();
    }

    void readAgentSnapshot(JsonCallback callback) override {
        if (state_.owner != yobro::engine::PageOwner::agent) {
            callback({}, yobro::engine::EngineError{
                .code = yobro::engine::EngineErrorCode::forbidden,
                .message = "Agent snapshots are forbidden on user-owned pages.",
            });
            return;
        }
        if (deferSnapshot) {
            pendingSnapshot = std::move(callback);
            return;
        }
        callback(snapshot, std::nullopt);
    }

    void performAgentAction(std::string_view requestJson, JsonCallback callback) override {
        if (state_.owner != yobro::engine::PageOwner::agent) {
            callback({}, yobro::engine::EngineError{
                .code = yobro::engine::EngineErrorCode::forbidden,
                .message = "Agent actions are forbidden on user-owned pages.",
            });
            return;
        }
        const Json request = Json::parse(requestJson);
        const std::string action = required(request, "action").asString();
        Json::Object result;
        result.emplace("performed", Json(action));
        const std::string encoded = Json(std::move(result)).serialize();
        if (deferAction) {
            pendingAction = std::move(callback);
            pendingActionResult = encoded;
            return;
        }
        callback(encoded, std::nullopt);
    }

    void findText(std::string_view query, bool backwards, BoolCallback callback) override {
        lastFindBackwards = backwards;
        if (deferFind) {
            pendingFind = std::move(callback);
            pendingFindResult = query == "needle";
            return;
        }
        callback(query == "needle", std::nullopt);
    }

    void scrollBy(int amount, BoolCallback callback) override {
        lastScrollAmount = amount;
        if (deferScroll) {
            pendingScroll = std::move(callback);
            return;
        }
        callback(true, std::nullopt);
    }

    void finishNavigation(bool ok = true, std::string message = {}) {
        check(activeToken != 0, "No fake navigation is active.");
        const auto token = activeToken;
        activeToken = 0;
        state_.loading = false;
        std::optional<yobro::engine::EngineError> error;
        if (!ok) {
            state_.error = message;
            error = yobro::engine::EngineError{
                .code = yobro::engine::EngineErrorCode::navigationFailed,
                .message = std::move(message),
            };
        }
        emitState();
        yobro::engine::NavigationResult result{
            .token = token,
            .state = state_,
            .error = std::move(error),
        };
        if (deferNavigationResult) {
            pendingNavigationResult = std::move(result);
            return;
        }
        emitNavigationResult(result);
    }

    void completeNavigationResult() {
        if (!pendingNavigationResult)
            return;
        const auto result = std::move(*pendingNavigationResult);
        pendingNavigationResult.reset();
        emitNavigationResult(result);
    }

    void redirect(std::string url) {
        check(state_.loading, "A fake redirect requires an active navigation.");
        state_.url = std::move(url);
        const std::size_t scheme = state_.url.find("://");
        const std::size_t originEnd = scheme == std::string::npos
            ? std::string::npos
            : state_.url.find_first_of("/?#", scheme + 3);
        state_.securityOrigin = scheme == std::string::npos
            ? std::string()
            : state_.url.substr(0, originEnd);
        emitState();
    }

    yobro::engine::NavigationToken lastToken() const { return activeToken; }

    FakePage *requestPopup(bool accept = true) {
        FakePage *opened = nullptr;
        const yobro::engine::NewWindowRequest request{
            .openIn = [&opened, accept](yobro::engine::BrowserPage &candidate) {
                if (!accept)
                    return false;
                opened = dynamic_cast<FakePage *>(&candidate);
                return opened != nullptr;
            },
        };
        const auto handlers = registry_->handlers;
        for (const auto &[id, callbacks] : handlers) {
            (void)id;
            if (!callbacks.newWindowRequested)
                continue;
            callbacks.newWindowRequested(request);
            if (opened)
                break;
        }
        return opened;
    }

    std::shared_ptr<FakePermissionRequest::Probe> requestPermission(
        yobro::engine::WebPermission permission,
        std::string origin
    ) {
        auto probe = std::make_shared<FakePermissionRequest::Probe>();
        auto request = std::make_unique<FakePermissionRequest>(
            permission,
            std::move(origin),
            probe
        );
        const auto handler = permissionRequestHandler_;
        if (!handler)
            return probe;
        try {
            handler(std::move(request));
        } catch (...) {
            // The request destructor provides the same fail-closed behavior as Qt.
        }
        return probe;
    }

    void requestClose() {
        const auto handlers = registry_->handlers;
        for (const auto &[id, callbacks] : handlers) {
            (void)id;
            if (callbacks.windowCloseRequested)
                callbacks.windowCloseRequested();
        }
    }

    void emitUntrustedState(std::string id, yobro::engine::PageOwner owner) {
        state_.id = std::move(id);
        state_.owner = owner;
        emitState();
    }

    void terminateRenderer(
        yobro::engine::RendererTerminationKind kind = yobro::engine::RendererTerminationKind::crashed,
        int exitCode = 11
    ) {
        state_.loading = false;
        state_.error = "Renderer terminated";
        emitState();
        if (activeToken != 0) {
            const auto token = activeToken;
            activeToken = 0;
            const yobro::engine::EngineError rendererError{
                .code = yobro::engine::EngineErrorCode::rendererTerminated,
                .message = *state_.error,
            };
            const auto navigationHandlers = registry_->handlers;
            for (const auto &[id, callbacks] : navigationHandlers) {
                (void)id;
                if (callbacks.navigationFinished) {
                    callbacks.navigationFinished({
                        .token = token,
                        .state = state_,
                        .error = rendererError,
                    });
                }
            }
        }
        const yobro::engine::RendererTermination termination{
            .kind = kind,
            .exitCode = exitCode,
            .state = state_,
        };
        const auto handlers = registry_->handlers;
        for (const auto &[id, callbacks] : handlers) {
            (void)id;
            if (callbacks.rendererTerminated)
                callbacks.rendererTerminated(termination);
        }
    }

    JsonCallback takePendingSnapshot() {
        JsonCallback callback;
        if (pendingSnapshot) {
            callback = std::move(*pendingSnapshot);
            pendingSnapshot.reset();
        }
        return callback;
    }

    void completeSnapshot() {
        JsonCallback callback = takePendingSnapshot();
        if (callback)
            callback(snapshot, std::nullopt);
    }

    void completeAction() {
        if (!pendingAction)
            return;
        JsonCallback callback = std::move(*pendingAction);
        pendingAction.reset();
        callback(std::move(pendingActionResult), std::nullopt);
    }

    void completeFind() {
        if (!pendingFind)
            return;
        BoolCallback callback = std::move(*pendingFind);
        pendingFind.reset();
        callback(pendingFindResult, std::nullopt);
    }

    void completeScroll() {
        if (!pendingScroll)
            return;
        BoolCallback callback = std::move(*pendingScroll);
        pendingScroll.reset();
        callback(true, std::nullopt);
    }

    std::string snapshot = R"({"document":"doc-1","text":"Ready","elements":[]})";
    int lastScrollAmount = 0;
    bool lastFindBackwards = false;
    int reloadCount = 0;
    int backCount = 0;
    int forwardCount = 0;
    int stopCount = 0;
    bool deferStop = false;
    bool deferSnapshot = false;
    bool deferAction = false;
    bool deferFind = false;
    bool deferScroll = false;
    bool deferNavigationResult = false;

private:
    void emitNavigationResult(const yobro::engine::NavigationResult &result) {
        const auto handlers = registry_->handlers;
        for (const auto &[id, callbacks] : handlers) {
            (void)id;
            if (callbacks.navigationFinished)
                callbacks.navigationFinished(result);
        }
    }

    yobro::engine::NavigationToken beginNavigation() {
        activeToken = nextToken++;
        state_.loading = true;
        state_.error.reset();
        emitState();
        return activeToken;
    }

    void emitState() {
        const auto handlers = registry_->handlers;
        for (const auto &[id, callbacks] : handlers) {
            (void)id;
            if (callbacks.stateChanged) callbacks.stateChanged(state_);
        }
    }

    yobro::engine::PageState state_;
    std::shared_ptr<FakeSubscription::Registry> registry_;
    yobro::engine::PermissionRequestHandler permissionRequestHandler_;
    yobro::engine::AuthenticationChallengeHandler authenticationChallengeHandler_;
    std::optional<JsonCallback> pendingSnapshot;
    std::optional<JsonCallback> pendingAction;
    std::optional<BoolCallback> pendingFind;
    std::optional<BoolCallback> pendingScroll;
    std::optional<yobro::engine::NavigationResult> pendingNavigationResult;
    std::string pendingActionResult;
    bool pendingFindResult = false;
    yobro::engine::NavigationToken nextToken = 1;
    yobro::engine::NavigationToken activeToken = 0;
};

class FakeProfile final : public yobro::engine::BrowserProfile {
public:
    struct Creation {
        std::string id;
        yobro::engine::PageOwner owner = yobro::engine::PageOwner::user;
        bool privatePage = false;
    };

    FakeProfile() {
        spec_.id = "fake";
        spec_.storagePath = "/fake/storage";
        spec_.cachePath = "/fake/cache";
    }

    const yobro::engine::ProfileSpec &spec() const override { return spec_; }

    std::unique_ptr<yobro::engine::BrowserPage> createPage(
        std::string id,
        yobro::engine::PageOwner owner,
        bool privatePage
    ) override {
        if (owner == yobro::engine::PageOwner::agent && privatePage)
            throw std::invalid_argument("Agent-owned pages may not use off-the-record browsing contexts.");
        creations.push_back({.id = id, .owner = owner, .privatePage = privatePage});
        auto page = std::make_unique<FakePage>(std::move(id), owner);
        pages.push_back(page.get());
        return page;
    }

    FakePage *lastPage() const {
        return pages.empty() ? nullptr : pages.back();
    }

    std::vector<FakePage *> pages;
    std::vector<Creation> creations;

private:
    yobro::engine::ProfileSpec spec_;
};

class FakeLibrary final : public yobro::controller::BrowserLibrary {
public:
    Json history(std::string_view query, std::size_t limit) const override {
        Json::Object entry;
        entry.emplace("id", Json("history-1"));
        entry.emplace("title", Json(std::string(query)));
        entry.emplace("url", Json("https://history.example"));
        entry.emplace("date", Json("2026-09-13T00:00:00Z"));
        entry.emplace("visits", Json(std::int64_t{2}));
        Json::Array result;
        if (limit > 0) result.emplace_back(std::move(entry));
        return Json(std::move(result));
    }

    Json downloads() const override {
        Json::Object entry;
        entry.emplace("id", Json("download-1"));
        entry.emplace("state", Json(downloadActive ? "downloading" : "cancelled"));
        return Json(Json::Array{Json(std::move(entry))});
    }

    std::string downloadDirectory() const override { return "/tmp/YOBRO"; }
    void recordVisit(const yobro::engine::PageState &) override { ++visitsRecorded; }
    void startDownload(yobro::engine::BrowserPage &, std::string_view url, DownloadCompletion completion) override {
        lastDownloadUrl = std::string(url);
        downloadActive = true;
        completion(std::nullopt);
    }
    yobro::controller::CancelDownloadResult cancelDownload(std::string_view id) override {
        if (id != "download-1") return yobro::controller::CancelDownloadResult::unknown;
        if (!downloadActive) return yobro::controller::CancelDownloadResult::inactive;
        downloadActive = false;
        return yobro::controller::CancelDownloadResult::requested;
    }
    void endAgentDownloads() override { ++agentDownloadEndCount; }
    void shutdownDownloads() override { ++downloadShutdownCount; }

    mutable bool downloadActive = true;
    int visitsRecorded = 0;
    int agentDownloadEndCount = 0;
    int downloadShutdownCount = 0;
    std::string lastDownloadUrl;
};

class StubHost final : public yobro::controller::ProtocolV2Host {
public:
    yobro::controller::ProtocolHostState protocolState() const override { return state; }
    Json tabsResult() const override {
        Json::Object result;
        result.emplace("tabs", Json(Json::Array{}));
        result.emplace("space", Json("Personal"));
        return Json(std::move(result));
    }
    void endAgentWorkspace() override { ++endCount; }
    void presentAgentWorkspace() override { ++presentCount; }
    void execute(
        std::string_view command,
        const Json::Object &,
        yobro::controller::ProtocolCompletion completion
    ) override {
        if (command == "hold") {
            held = std::move(completion);
            return;
        }
        completion(Json(nullptr), "Unbekannter Befehl: " + std::string(command));
    }

    yobro::controller::ProtocolHostState state{
        .profileActive = true,
        .agentEnabled = true,
        .agentPaneVisible = false,
        .libraryAccess = false,
        .browser = "YoBro",
        .version = "0.6.3",
        .engine = "Chromium",
        .socketPath = "/tmp/control.sock",
        .profileName = "Test",
    };
    int endCount = 0;
    int presentCount = 0;
    yobro::controller::ProtocolCompletion held;
};

std::optional<std::string> issue(
    yobro::controller::ProtocolV2Controller &controller,
    ManualEventLoop &loop,
    std::string request
) {
    auto result = std::make_shared<std::optional<std::string>>();
    controller.handleLine(std::move(request), [result](std::string response) {
        *result = std::move(response);
    });
    loop.drain();
    return *result;
}

void testJsonCore() {
    const Json parsed = Json::parse(R"({"z":1,"a":[true,null,"Grüße\u0020\ud83d\ude80"]})");
    check(parsed.serialize() == R"({"a":[true,null,"Grüße 🚀"],"z":1})", "JSON output must be sorted and Unicode-safe.");

    bool duplicateRejected = false;
    try { (void)Json::parse(R"({"a":1,"a":2})"); }
    catch (const yobro::core::JsonError &) { duplicateRejected = true; }
    check(duplicateRejected, "Duplicate object keys must be rejected.");

    bool invalidUtf8Rejected = false;
    try {
        const std::string invalid{'"', static_cast<char>(0xc0), static_cast<char>(0x80), '"'};
        (void)Json::parse(invalid);
    } catch (const yobro::core::JsonError &) { invalidUtf8Rejected = true; }
    check(invalidUtf8Rejected, "Overlong UTF-8 must be rejected.");

    bool depthRejected = false;
    try { (void)Json::parse("[[[0]]]", {.maxBytes = 100, .maxDepth = 2}); }
    catch (const yobro::core::JsonError &) { depthRejected = true; }
    check(depthRejected, "Configured JSON depth must be enforced.");

    bool sizeRejected = false;
    try { (void)Json::parse("true", {.maxBytes = 3}); }
    catch (const yobro::core::JsonError &) { sizeRejected = true; }
    check(sizeRejected, "Configured JSON byte limit must be enforced.");
}

void testDispatchPrecedence() {
    ManualEventLoop loop;
    StubHost host;
    yobro::controller::ProtocolV2Controller controller(loop, host);

    auto response = issue(controller, loop, R"({"command":7})");
    check(response.has_value(), "Default status response was not produced.");
    const Json status = successResult(decodedResponse(*response));
    check(required(status, "version").asString() == "0.6.3", "Status version is missing or wrong.");
    check(required(status, "commands").asArray().size() == 22, "Status command order is incomplete.");

    auto heldResponse = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"hold"})", [heldResponse](std::string value) { *heldResponse = std::move(value); });
    loop.drain();
    check(!heldResponse->has_value() && controller.busy(), "Held operation must keep the controller busy.");

    response = issue(controller, loop, R"({"command":"status"})");
    check(required(successResult(decodedResponse(*response)), "agentAction").asString() == "hold", "Status must bypass busy and expose agentAction.");
    response = issue(controller, loop, R"({"command":"tabs"})");
    (void)successResult(decodedResponse(*response));
    response = issue(controller, loop, R"({"command":"new"})");
    check(errorMessage(decodedResponse(*response)) == "Agent is busy. Retry after the current command completes.", "Busy error differs from WebKit.");
    response = issue(controller, loop, R"({"command":"end"})");
    check(required(successResult(decodedResponse(*response)), "ended").asBoolean(), "End must bypass busy.");
    check(host.endCount == 1, "End did not reach the host.");

    host.held(Json(Json::Object{{"done", Json(true)}}), std::nullopt);
    loop.drain();
    check(heldResponse->has_value() && !controller.busy(), "Held operation did not complete exactly once.");

    response = issue(controller, loop, R"({"command":"space"})");
    check(errorMessage(decodedResponse(*response)) == "This command changes the user workspace and is unavailable in agent mode.", "Blocked-command error differs.");

    host.state.agentEnabled = false;
    response = issue(controller, loop, R"({"command":"tabs"})");
    check(errorMessage(decodedResponse(*response)) == "Agentenzugriff ist im Browser pausiert.", "Paused error differs.");
    response = issue(controller, loop, R"({"command":"status"})");
    check(!required(successResult(decodedResponse(*response)), "enabled").asBoolean(), "Status must remain available while paused.");

    host.state.profileActive = false;
    response = issue(controller, loop, R"({"command":"status"})");
    check(errorMessage(decodedResponse(*response)) == "This profile is inactive. Switch to it in the browser.", "Inactive profile must reject status.");

    response = issue(controller, loop, "not json");
    check(errorMessage(decodedResponse(*response)) == "Invalid JSON request or request too large.", "Invalid JSON envelope differs.");
}

void testBrowserSessionCommands() {
    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    FakeProfile *fakeProfile = profile.get();
    auto library = std::make_shared<FakeLibrary>();
    std::vector<std::string> ids{
        "00000000-0000-4000-8000-000000000001",
        "00000000-0000-4000-8000-000000000002",
        "00000000-0000-4000-8000-000000000003",
        "00000000-0000-4000-8000-000000000004",
        "00000000-0000-4000-8000-000000000005",
    };
    std::size_t nextId = 0;
    yobro::controller::BrowserSession session(
        std::move(profile),
        loop,
        {
            .version = "0.6.3",
            .socketPath = "/tmp/test.sock",
            .profileName = "Test",
            .space = "Personal",
            .library = library,
            .generateId = [&] { return ids.at(nextId++); },
        }
    );
    yobro::controller::ProtocolV2Controller controller(loop, session);

    yobro::engine::BrowserPage &user = session.newUserTab();
    auto response = issue(controller, loop, "{}");
    check(response.has_value(), "Status request failed.");
    check(required(successResult(decodedResponse(*response)), "engine").asString() == "Chromium", "Engine identity differs.");
    check(!required(successResult(decodedResponse(*response)), "libraryAccess").asBoolean(), "Library access must default to disabled.");

    response = issue(controller, loop, "{\"command\":\"focus\",\"tab\":\"" + user.state().id + "\"}");
    check(errorMessage(decodedResponse(*response)) == "This is a user tab. Open its URL with new to work in the right agent pane.", "User ownership guard differs.");

    response = issue(controller, loop, R"({"command":"new"})");
    const Json created = successResult(decodedResponse(*response));
    const std::string agentId = required(created, "id").asString();
    check(required(created, "owner").asString() == "agent", "New did not create an agent-owned tab.");
    auto *agent = fakeProfile->lastPage();
    check(agent && agent->state().id == agentId, "Fake agent page is unavailable.");

    auto openResult = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"open","url":"example.com/path"})", [openResult](std::string value) {
        *openResult = std::move(value);
    });
    loop.drain();
    check(!openResult->has_value(), "Open returned before navigation readiness.");
    loop.advance(150ms);
    check(!openResult->has_value(), "Open returned while the page was still loading.");
    agent->finishNavigation();
    loop.drain();
    check(openResult->has_value(), "Open did not finish after the correlated navigation.");
    check(required(successResult(decodedResponse(**openResult)), "url").asString() == "https://example.com/path", "Navigation input normalization differs.");
    check(library->visitsRecorded == 1, "Completed navigation was not recorded in history.");

    auto redirectResult = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"open","url":"https://redirect.example/start"})", [redirectResult](std::string value) {
        *redirectResult = std::move(value);
    });
    loop.drain();
    agent->redirect("https://final.example/landing");
    loop.advance(150ms);
    check(!redirectResult->has_value(), "Redirect completed before its navigation token settled.");
    agent->finishNavigation();
    loop.drain();
    check(redirectResult->has_value()
          && required(successResult(decodedResponse(**redirectResult)), "url").asString()
              == "https://final.example/landing",
          "A redirect cancelled or lost its token-owning protocol open.");

    auto splitTokenResult = std::make_shared<std::optional<std::string>>();
    agent->deferNavigationResult = true;
    controller.handleLine(R"({"command":"reload"})", [splitTokenResult](std::string value) {
        *splitTokenResult = std::move(value);
    });
    loop.drain();
    loop.advance(150ms);
    agent->finishNavigation();
    loop.drain();
    check(!splitTokenResult->has_value(),
          "A token-bound operation completed from loading=false before navigationFinished.");
    agent->completeNavigationResult();
    loop.drain();
    agent->deferNavigationResult = false;
    check(splitTokenResult->has_value(),
          "A token-bound operation did not complete after its exact result arrived.");

    auto readResult = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"read"})", [readResult](std::string value) { *readResult = std::move(value); });
    loop.drain();
    check(!readResult->has_value(), "Read must perform the 150 ms idle check.");
    loop.advance(150ms);
    check(readResult->has_value(), "Read did not complete after idle readiness.");
    check(required(required(successResult(decodedResponse(**readResult)), "page"), "document").asString() == "doc-1", "Read did not preserve bridge JSON.");

    response = issue(controller, loop, R"({"command":"fill","document":"doc-1","ref":"e1"})");
    check(errorMessage(decodedResponse(*response)) == "Wert fehlt.", "Missing fill value error differs.");
    response = issue(controller, loop, R"({"command":"click","document":"doc-1"})");
    check(errorMessage(decodedResponse(*response)) == "ref und document aus einem aktuellen read sind erforderlich.", "Missing action references error differs.");
    response = issue(controller, loop, R"({"command":"fill","document":"doc-1","ref":"e1","value":"Laura"})");
    const Json action = successResult(decodedResponse(*response));
    check(required(required(action, "action"), "performed").asString() == "fill", "Fill did not cross the bridge.");
    check(required(action, "next").asString() == "Read again to verify the outcome; pages may update asynchronously.", "Action next hint differs.");

    auto findResult = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"find","query":"needle","backwards":true})", [findResult](std::string value) { *findResult = std::move(value); });
    loop.drain();
    loop.advance(150ms);
    check(findResult->has_value(), "Find did not complete after readiness.");
    check(required(successResult(decodedResponse(**findResult)), "found").asBoolean(), "Find result differs.");
    check(agent->lastFindBackwards, "Find backwards flag was lost.");

    response = issue(controller, loop, R"({"command":"scroll","amount":9000})");
    (void)successResult(decodedResponse(*response));
    check(agent->lastScrollAmount == 5000, "Scroll clamp differs from WebKit.");

    response = issue(controller, loop, R"({"command":"history"})");
    check(errorMessage(decodedResponse(*response)) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.", "Library opt-in error differs.");
    session.setLibraryAccess(true);
    response = issue(controller, loop, R"({"command":"status"})");
    check(required(successResult(decodedResponse(*response)), "libraryAccess").asBoolean(), "Status did not expose an enabled library policy.");
    response = issue(controller, loop, R"({"command":"history","query":"needle","limit":500})");
    check(required(successResult(decodedResponse(*response)), "history").asArray().size() == 1, "History result shape differs.");
    response = issue(controller, loop, R"({"command":"downloads"})");
    check(required(successResult(decodedResponse(*response)), "directory").asString() == "/tmp/YOBRO", "Download directory is missing.");
    session.setLibraryAccess(false);
    response = issue(controller, loop, R"({"command":"status"})");
    check(!required(successResult(decodedResponse(*response)), "libraryAccess").asBoolean(), "Status did not expose a disabled library policy.");
    response = issue(controller, loop, R"({"command":"history"})");
    check(errorMessage(decodedResponse(*response)) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.", "History was not blocked after disabling library access.");
    response = issue(controller, loop, R"({"command":"downloads"})");
    check(errorMessage(decodedResponse(*response)) == "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”.", "Downloads were not blocked after disabling library access.");
    response = issue(controller, loop, R"({"command":"download","url":"file:///tmp/no"})");
    check(errorMessage(decodedResponse(*response)) == "Gültige HTTP- oder HTTPS-Downloadadresse erforderlich.", "Download URL validation differs.");
    response = issue(controller, loop, R"({"command":"download","url":"https://files.example/a.zip"})");
    check(required(successResult(decodedResponse(*response)), "started").asBoolean(), "Download did not start.");
    check(library->lastDownloadUrl == "https://files.example/a.zip", "Download URL was changed.");
    response = issue(controller, loop, R"({"command":"cancel-download","id":"download-1"})");
    check(required(successResult(decodedResponse(*response)), "cancelRequested").asString() == "download-1", "Cancel response differs.");
    response = issue(controller, loop, R"({"command":"cancel-download","id":"download-1"})");
    check(errorMessage(decodedResponse(*response)) == "Download ist nicht aktiv.", "Inactive download error differs.");

    response = issue(controller, loop, R"({"command":"pin"})");
    check(required(successResult(decodedResponse(*response)), "pinned").asBoolean(), "Pin did not toggle.");
    response = issue(controller, loop, R"({"command":"split"})");
    check(required(successResult(decodedResponse(*response)), "split").asString() == agentId, "Split did not retain the active agent tab.");

    response = issue(controller, loop, R"({"command":"close"})");
    const Json closed = successResult(decodedResponse(*response));
    check(required(closed, "owner").asString() == "user", "Close response quirk was not preserved.");

    response = issue(controller, loop, R"({"command":"teleport"})");
    check(errorMessage(decodedResponse(*response)) == "Unbekannter Befehl: teleport", "Unknown-command error differs.");
    check(session.agentPages().size() == 1, "Unknown command must resolve/create its target before validation.");
    response = issue(controller, loop, R"({"command":"end"})");
    check(required(successResult(decodedResponse(*response)), "ended").asBoolean(), "End result differs.");
    check(session.agentPages().empty(), "End must discard all agent pages.");
    check(library->agentDownloadEndCount == 1, "End did not terminate agent-owned downloads exactly once.");

    session.setAgentEnabled(false);
    response = issue(controller, loop, R"({"command":"tabs"})");
    check(errorMessage(decodedResponse(*response)) == "Agentenzugriff ist im Browser pausiert.", "Paused tabs error differs.");
    session.setAgentEnabled(true);
    session.setProfileActive(false);
    response = issue(controller, loop, R"({"command":"status"})");
    check(errorMessage(decodedResponse(*response)) == "This profile is inactive. Switch to it in the browser.", "Inactive status error differs.");
}

void testPopupOwnershipAndQueuedClose() {
    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    FakeProfile *fakeProfile = profile.get();
    const std::vector<std::string> ids{
        "20000000-0000-4000-8000-000000000001",
        "20000000-0000-4000-8000-000000000002",
        "20000000-0000-4000-8000-000000000002",
        "20000000-0000-4000-8000-000000000003",
        "20000000-0000-4000-8000-000000000004",
        "20000000-0000-4000-8000-000000000005",
        "20000000-0000-4000-8000-000000000006",
        "20000000-0000-4000-8000-000000000007",
        "20000000-0000-4000-8000-000000000008",
    };
    std::size_t nextId = 0;
    yobro::controller::BrowserSession session(
        std::move(profile),
        loop,
        {
            .profileName = "Popup Test",
            .space = "Research",
            .generateId = [&] { return ids.at(nextId++); },
        }
    );

    const auto viewFor = [&session](const yobro::engine::BrowserPage *page) {
        std::optional<yobro::controller::SessionTabView> result;
        for (const auto &view : session.tabViews()) {
            if (view.page == page) {
                result = view;
                break;
            }
        }
        return result;
    };

    auto &userGeneric = session.newUserTab();
    auto *user = dynamic_cast<FakePage *>(&userGeneric);
    check(user != nullptr, "User opener is not a fake page.");
    const std::string userId = user->state().id;
    int throwingObserverCalls = 0;
    session.setObserver([&throwingObserverCalls] {
        ++throwingObserverCalls;
        throw std::runtime_error("observer failure");
    });
    FakePage *userPopup = user->requestPopup();
    session.setObserver({});
    check(throwingObserverCalls == 1, "Committed popup did not notify the throwing observer exactly once.");
    check(userPopup != nullptr, "Persistent user popup was not adopted.");
    check(session.activeUserPage() == userPopup,
          "Throwing observer rolled back the committed user popup or its active index.");
    const std::string userPopupId = userPopup->state().id;
    check(session.userPages().size() == 2 && session.agentPages().empty(), "Persistent user popup ownership differs.");
    const auto userPopupView = viewFor(userPopup);
    check(userPopupView.has_value(), "Persistent user popup is not session-owned.");
    check(userPopupView->state.owner == yobro::engine::PageOwner::user, "Persistent user popup did not inherit its owner.");
    check(!userPopupView->privatePage && userPopupView->space == "Research", "Persistent user popup did not inherit privacy and space.");
    check(!fakeProfile->creations.back().privatePage
          && fakeProfile->creations.back().owner == yobro::engine::PageOwner::user,
          "Persistent user popup was created in the wrong profile context.");

    check(session.closeTab(userId), "Could not close the persistent user opener.");
    check(session.userPages().size() == 1 && session.userPages().front() == userPopup,
          "Closing an opener incorrectly destroyed its adopted child.");
    userPopup->requestClose();
    userPopup->requestClose();
    check(session.userPages().size() == 1, "windowCloseRequested deleted its page reentrantly.");
    check(session.closeTab(userPopupId), "Could not manually close the queued popup.");
    auto &replacementGeneric = session.newUserTab();
    auto *replacement = dynamic_cast<FakePage *>(&replacementGeneric);
    check(replacement && replacement->state().id == userPopupId, "The ABA close test did not reuse the popup id.");
    loop.drain();
    check(session.userPages().size() == 1 && session.userPages().front() == replacement,
          "A stale queued close deleted a newer tab with the same id.");
    check(session.closeTab(userPopupId), "Could not clean up the replacement user tab.");

    auto &privateGeneric = session.newUserTab({}, true);
    auto *privateOpener = dynamic_cast<FakePage *>(&privateGeneric);
    check(privateOpener != nullptr, "Private opener is not a fake page.");
    const std::string privateId = privateOpener->state().id;
    privateOpener->emitUntrustedState("forged-engine-id", yobro::engine::PageOwner::agent);
    const auto repaired = viewFor(privateOpener);
    check(repaired && repaired->state.id == privateId
          && repaired->state.owner == yobro::engine::PageOwner::user,
          "Session did not restore canonical id and owner after an untrusted engine event.");

    FakePage *privatePopup = privateOpener->requestPopup();
    check(privatePopup != nullptr, "Private user popup was not adopted.");
    const std::string privatePopupId = privatePopup->state().id;
    const auto privatePopupView = viewFor(privatePopup);
    check(privatePopupView && privatePopupView->state.owner == yobro::engine::PageOwner::user,
          "Private popup inherited the forged engine owner.");
    check(privatePopupView->privatePage && privatePopupView->space == "Research",
          "Private popup did not inherit its off-the-record context and space.");
    check(fakeProfile->creations.back().privatePage,
          "Private popup was not created through the private profile path.");

    const std::size_t beforeRejectedPopup = session.tabViews().size();
    const std::size_t beforeRejectedCreation = fakeProfile->creations.size();
    check(privateOpener->requestPopup(false) == nullptr, "Rejected openIn unexpectedly returned a child.");
    check(session.tabViews().size() == beforeRejectedPopup,
          "Rejected openIn left a provisional tab in the session.");
    check(fakeProfile->creations.size() == beforeRejectedCreation + 1,
          "Rejected openIn did not exercise provisional page creation.");
    check(session.closeTab(privateId), "Canonical private opener id no longer resolved after forged state.");
    privatePopup->requestClose();
    loop.drain();
    check(session.userPages().empty(), "Queued close did not remove the private popup.");

    auto &agentGeneric = session.newAgentTab();
    auto *agentOpener = dynamic_cast<FakePage *>(&agentGeneric);
    check(agentOpener != nullptr, "Agent opener is not a fake page.");
    session.setObserver([] { throw std::runtime_error("agent observer failure"); });
    FakePage *agentPopup = agentOpener->requestPopup();
    session.setObserver({});
    check(agentPopup != nullptr, "Agent popup was not adopted.");
    check(session.activeAgentPage() == agentPopup && session.agentPaneVisible(),
          "Throwing observer corrupted the committed agent popup indices.");
    const auto agentPopupView = viewFor(agentPopup);
    check(agentPopupView && agentPopupView->state.owner == yobro::engine::PageOwner::agent,
          "Agent popup did not inherit agent ownership.");
    check(!agentPopupView->privatePage && agentPopupView->space == "Research",
          "Agent popup inherited an invalid privacy or space context.");
    check(session.agentPages().size() == 2, "Agent opener and popup are not both session-owned.");

    session.setProfileActive(false);
    const std::size_t beforeInactiveRequest = fakeProfile->creations.size();
    check(agentOpener->requestPopup() == nullptr, "Inactive profile accepted an agent popup.");
    check(fakeProfile->creations.size() == beforeInactiveRequest && session.agentPages().size() == 2,
          "Inactive profile created a provisional agent popup.");
    session.setProfileActive(true);
    session.endAgentWorkspace();
    check(session.agentPages().empty() && !session.agentPaneVisible(),
          "Agent end did not destroy both opener and popup.");

    session.setAgentEnabled(false);
    auto &disabledGeneric = session.newAgentTab();
    auto *disabledOpener = dynamic_cast<FakePage *>(&disabledGeneric);
    check(disabledOpener != nullptr, "Disabled-agent opener is not a fake page.");
    const std::size_t beforeDisabledRequest = fakeProfile->creations.size();
    check(disabledOpener->requestPopup() == nullptr, "Disabled agent accepted a popup.");
    check(fakeProfile->creations.size() == beforeDisabledRequest && session.agentPages().size() == 1,
          "Disabled agent created a provisional popup.");
    session.endAgentWorkspace();
    check(session.tabViews().empty(), "Popup ownership test left session pages behind.");

    ManualEventLoop teardownLoop;
    {
        auto teardownProfile = std::make_unique<FakeProfile>();
        const std::vector<std::string> teardownIds{
            "21000000-0000-4000-8000-000000000001",
            "21000000-0000-4000-8000-000000000002",
        };
        std::size_t nextTeardownId = 0;
        yobro::controller::BrowserSession teardownSession(
            std::move(teardownProfile),
            teardownLoop,
            {.generateId = [&] { return teardownIds.at(nextTeardownId++); }}
        );
        auto *teardownOpener = dynamic_cast<FakePage *>(&teardownSession.newUserTab());
        check(teardownOpener != nullptr, "Teardown opener is not a fake page.");
        FakePage *teardownPopup = teardownOpener->requestPopup();
        check(teardownPopup != nullptr, "Teardown popup was not adopted.");
        teardownPopup->requestClose();
        check(teardownSession.tabViews().size() == 2,
              "Teardown close did not remain queued before session destruction.");
    }
    teardownLoop.drain();
}

void testPermissionAuthorityAndLifecycle() {
    const auto checkDecision = [](
        const std::shared_ptr<FakePermissionRequest::Probe> &probe,
        yobro::engine::PermissionDecision expected,
        std::string_view message
    ) {
        check(probe->resolveCount == 1 && probe->decision == expected, message);
    };

    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    const std::vector<std::string> ids{
        "40000000-0000-4000-8000-000000000001",
        "40000000-0000-4000-8000-000000000002",
        "40000000-0000-4000-8000-000000000003",
        "40000000-0000-4000-8000-000000000004",
        "40000000-0000-4000-8000-000000000004",
        "40000000-0000-4000-8000-000000000005",
    };
    std::size_t nextId = 0;
    yobro::controller::BrowserSession session(
        std::move(profile),
        loop,
        {
            .profileName = "Permission Test",
            .generateId = [&] { return ids.at(nextId++); },
        }
    );
    int presentedPrompts = 0;
    session.setPermissionPromptPresenter([&](const yobro::controller::PermissionPrompt &) {
        ++presentedPrompts;
        return true;
    });

    auto *user = dynamic_cast<FakePage *>(&session.newUserTab("https://media.example/page"));
    check(user != nullptr, "Permission user page is not fake.");
    user->finishNavigation();

    auto probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A missing permission surface did not deny immediately.");
    check(!session.pendingPermission(), "A missing surface created a prompt.");

    session.setPermissionSurfaceVisible(true);
    probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://forged.example"
    );
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A mismatched permission origin was not denied.");

    probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    const auto firstPrompt = session.pendingPermission();
    check(firstPrompt && firstPrompt->tabId == user->state().id
          && firstPrompt->origin == "https://media.example"
          && firstPrompt->permission == yobro::engine::WebPermission::microphone
          && !firstPrompt->privatePage,
          "The active user permission prompt lost canonical request metadata.");
    auto concurrent = user->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://media.example"
    );
    checkDecision(concurrent, yobro::engine::PermissionDecision::deny,
                  "A concurrent permission request was queued instead of denied.");
    check(!session.resolvePermission(firstPrompt->id + 10, yobro::engine::PermissionDecision::grant),
          "A stale permission id resolved the live request.");
    check(session.resolvePermission(firstPrompt->id, yobro::engine::PermissionDecision::grant),
          "The active user permission was not resolved.");
    checkDecision(probe, yobro::engine::PermissionDecision::grant,
                  "An eligible explicit user grant was not forwarded exactly once.");
    check(!session.resolvePermission(firstPrompt->id, yobro::engine::PermissionDecision::grant),
          "A permission request accepted a second resolution.");

    auto *privatePage = dynamic_cast<FakePage *>(&session.newUserTab(
        "https://private.example/room",
        true
    ));
    check(privatePage != nullptr, "Private permission page is not fake.");
    privatePage->finishNavigation();
    probe = privatePage->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://private.example"
    );
    const auto privatePrompt = session.pendingPermission();
    check(privatePrompt && privatePrompt->privatePage,
          "A visible private user tab did not receive the nonpersistent prompt.");
    check(session.resolvePermission(privatePrompt->id, yobro::engine::PermissionDecision::grant),
          "Private permission resolution failed.");
    checkDecision(probe, yobro::engine::PermissionDecision::grant,
                  "A private user grant was incorrectly blocked.");

    probe = user->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://media.example"
    );
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A background user tab reached the permission surface.");

    auto *agent = dynamic_cast<FakePage *>(&session.newAgentTab("https://agent.example/page"));
    check(agent != nullptr, "Agent permission page is not fake.");
    agent->finishNavigation();
    probe = agent->requestPermission(
        yobro::engine::WebPermission::microphoneAndCamera,
        "https://agent.example"
    );
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "An agent-owned page reached the permission surface.");

    probe = privatePage->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://private.example"
    );
    const auto switchPrompt = session.pendingPermission();
    check(switchPrompt.has_value(), "Active-switch cancellation setup did not prompt.");
    check(session.setActiveUserTab(user->state().id), "Could not activate the original user tab.");
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "Switching the active user tab left a stale grant-capable prompt.");

    probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    check(session.pendingPermission().has_value(), "Navigation cancellation setup did not prompt.");
    (void)user->reload();
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A document reload did not deny its pending permission.");
    user->finishNavigation();

    probe = user->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://media.example"
    );
    check(session.pendingPermission().has_value(), "Renderer cancellation setup did not prompt.");
    user->terminateRenderer();
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "Renderer termination did not deny its pending permission.");
    loop.advance(100ms);
    check(user->state().loading, "User renderer recovery did not queue its one automatic reload.");
    user->finishNavigation();
    loop.advance(5s);

    probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    check(session.pendingPermission().has_value(), "Surface cancellation setup did not prompt.");
    session.setPermissionSurfaceVisible(false);
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "Hiding the prompt surface did not deny the pending request.");
    auto hidden = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    checkDecision(hidden, yobro::engine::PermissionDecision::deny,
                  "A hidden surface accepted a new permission request.");
    session.setPermissionSurfaceVisible(true);

    probe = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    check(session.pendingPermission().has_value(), "Profile cancellation setup did not prompt.");
    session.setProfileActive(false);
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "An inactive profile retained a pending permission.");
    auto inactive = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://media.example"
    );
    checkDecision(inactive, yobro::engine::PermissionDecision::deny,
                  "An inactive profile accepted a permission request.");
    session.setProfileActive(true);

    auto *closing = dynamic_cast<FakePage *>(&session.newUserTab("https://close.example/page"));
    check(closing != nullptr, "Closing permission page is not fake.");
    closing->finishNavigation();
    probe = closing->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://close.example"
    );
    const auto closingPrompt = session.pendingPermission();
    check(closingPrompt.has_value(), "Close cancellation setup did not prompt.");
    const std::string reusedId = closing->state().id;
    check(session.closeTab(reusedId), "Could not close a tab with a pending permission.");
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "Closing a tab did not deny its pending permission.");
    auto *replacement = dynamic_cast<FakePage *>(&session.newUserTab("https://close.example/replacement"));
    check(replacement && replacement->state().id == reusedId,
          "The permission ABA test did not reuse the closed tab id.");
    replacement->finishNavigation();
    check(!session.resolvePermission(closingPrompt->id, yobro::engine::PermissionDecision::grant),
          "A stale permission response reached an ABA replacement tab.");

    probe = replacement->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://close.example"
    );
    check(session.pendingPermission().has_value(), "Popup rollback cancellation setup did not prompt.");
    int popupRollbackNotifications = 0;
    session.setObserver([&] { ++popupRollbackNotifications; });
    check(replacement->requestPopup(false) == nullptr, "Rejected popup unexpectedly adopted a child.");
    session.setObserver({});
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "Rejected popup registration left an older permission grant-capable.");
    check(!session.pendingPermission() && popupRollbackNotifications == 1,
          "Rejected popup rollback did not close its cancelled permission UI.");

    probe = replacement->requestPermission(
        yobro::engine::WebPermission::camera,
        "https://close.example"
    );
    check(session.pendingPermission().has_value(), "Timeout cancellation setup did not prompt.");
    loop.advance(30s);
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A permission prompt did not fail closed at its timeout.");
    check(!session.pendingPermission(), "A timed-out permission remained visible.");

    session.setPermissionPromptPresenter([](const yobro::controller::PermissionPrompt &) -> bool {
        throw std::runtime_error("prompt delivery failed");
    });
    probe = replacement->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://close.example"
    );
    checkDecision(probe, yobro::engine::PermissionDecision::deny,
                  "A throwing permission presenter left a native request pending.");
    check(!session.pendingPermission(), "A throwing presenter left an invisible prompt slot.");
    session.setPermissionPromptPresenter([&](const yobro::controller::PermissionPrompt &) {
        ++presentedPrompts;
        return true;
    });
    check(presentedPrompts > 0, "The deterministic permission presenter was never invoked.");

    FakePage handlerless("handlerless", yobro::engine::PageOwner::user);
    auto handlerlessProbe = handlerless.requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://handlerless.example"
    );
    checkDecision(handlerlessProbe, yobro::engine::PermissionDecision::deny,
                  "A page without a permission consumer did not fail closed.");

    std::shared_ptr<FakePermissionRequest::Probe> teardownProbe;
    ManualEventLoop teardownLoop;
    {
        auto teardownProfile = std::make_unique<FakeProfile>();
        yobro::controller::BrowserSession teardownSession(
            std::move(teardownProfile),
            teardownLoop,
            {.generateId = [] { return "41000000-0000-4000-8000-000000000001"; }}
        );
        teardownSession.setPermissionPromptPresenter([](const yobro::controller::PermissionPrompt &) {
            return true;
        });
        teardownSession.setPermissionSurfaceVisible(true);
        auto *teardownPage = dynamic_cast<FakePage *>(&teardownSession.newUserTab(
            "https://teardown.example/page"
        ));
        check(teardownPage != nullptr, "Teardown permission page is not fake.");
        teardownPage->finishNavigation();
        teardownProbe = teardownPage->requestPermission(
            yobro::engine::WebPermission::microphone,
            "https://teardown.example"
        );
        check(teardownSession.pendingPermission().has_value(),
              "Session teardown cancellation setup did not prompt.");
    }
    checkDecision(teardownProbe, yobro::engine::PermissionDecision::deny,
                  "Session teardown did not deny its pending permission exactly once.");
    teardownLoop.drain();
}

void testRendererRecoveryAndAgentLifecycle() {
    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    const std::vector<std::string> ids{
        "50000000-0000-4000-8000-000000000001",
        "50000000-0000-4000-8000-000000000002",
        "50000000-0000-4000-8000-000000000003",
        "50000000-0000-4000-8000-000000000004",
        "50000000-0000-4000-8000-000000000005",
        "50000000-0000-4000-8000-000000000006",
        "50000000-0000-4000-8000-000000000006",
    };
    std::size_t nextId = 0;
    yobro::controller::BrowserSession session(
        std::move(profile),
        loop,
        {
            .profileName = "Renderer Recovery Test",
            .rendererRecoveryDelay = 100ms,
            .rendererStabilityDelay = 1s,
            .generateId = [&] { return ids.at(nextId++); },
        }
    );
    yobro::controller::ProtocolV2Controller controller(loop, session);

    const auto viewFor = [&session](const yobro::engine::BrowserPage *page) {
        for (const auto &view : session.tabViews()) {
            if (view.page == page)
                return std::optional<yobro::controller::SessionTabView>(view);
        }
        return std::optional<yobro::controller::SessionTabView>{};
    };

    session.setPermissionPromptPresenter([](const yobro::controller::PermissionPrompt &) {
        return true;
    });
    session.setPermissionSurfaceVisible(true);
    auto *user = dynamic_cast<FakePage *>(&session.newUserTab("https://recovery.example/a"));
    check(user != nullptr, "Renderer recovery user page is not fake.");
    user->finishNavigation();
    yobro::engine::BrowserPage *const originalUserPage = user;
    std::vector<yobro::engine::RendererTerminationKind> terminations;
    std::vector<yobro::engine::NavigationResult> navigationResults;
    auto evidenceSubscription = user->subscribe({
        .navigationFinished = [&](const yobro::engine::NavigationResult &result) {
            navigationResults.push_back(result);
        },
        .rendererTerminated = [&](const yobro::engine::RendererTermination &termination) {
            terminations.push_back(termination.kind);
        },
    });

    auto permission = user->requestPermission(
        yobro::engine::WebPermission::microphone,
        "https://recovery.example"
    );
    check(session.pendingPermission().has_value(),
          "Renderer permission-cancellation setup did not prompt.");
    user->terminateRenderer(yobro::engine::RendererTerminationKind::abnormal, 17);
    check(permission->resolveCount == 1
          && permission->decision == yobro::engine::PermissionDecision::deny,
          "Renderer death did not deny its pending permission exactly once.");
    auto view = viewFor(user);
    check(view && view->rendererRecovery == yobro::controller::RendererRecoveryState::recovering
          && view->lastRendererTermination == yobro::engine::RendererTerminationKind::abnormal,
          "First user renderer death did not enter typed recovery.");
    check(user->reloadCount == 0, "User renderer recovery reloaded reentrantly.");
    loop.advance(99ms);
    check(user->reloadCount == 0, "User renderer recovery ignored its queue delay.");
    loop.advance(1ms);
    check(user->reloadCount == 1 && user->state().loading,
          "First user renderer death did not reload exactly once.");
    check(session.activeUserPage() == originalUserPage,
          "Renderer recovery replaced the session-owned page.");
    const auto recoveryToken = user->lastToken();

    user->terminateRenderer(yobro::engine::RendererTerminationKind::crashed, 6);
    check(navigationResults.size() == 1
          && navigationResults.front().token == recoveryToken
          && navigationResults.front().error
          && navigationResults.front().error->code == yobro::engine::EngineErrorCode::rendererTerminated,
          "A crash during recovery did not settle its navigation token exactly once.");
    view = viewFor(user);
    check(view && view->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && view->state.error == "Der Seiteninhalt ist mehrfach abgestürzt. Lade sie neu oder öffne sie in einem neuen Tab."
          && view->lastRendererTermination == yobro::engine::RendererTerminationKind::crashed,
          "A second pre-stability crash did not expose the WebKit-parity failure.");
    loop.advance(2s);
    check(user->reloadCount == 1, "Crash-loop budget scheduled a second automatic reload.");

    check(session.reloadUserTab(user->state().id),
          "Manual user retry did not restart a failed renderer page.");
    check(user->reloadCount == 2 && user->state().loading,
          "Manual user retry did not use the same page.");
    user->finishNavigation();
    view = viewFor(user);
    check(view && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy,
          "Successful manual retry did not reset the crash budget.");

    user->terminateRenderer(yobro::engine::RendererTerminationKind::normal, 0);
    loop.advance(100ms);
    check(user->reloadCount == 3, "A post-manual renderer exit did not receive a fresh retry budget.");
    user->finishNavigation();
    view = viewFor(user);
    check(view && view->rendererRecovery == yobro::controller::RendererRecoveryState::recovering,
          "Automatic recovery was marked stable before its guard interval.");
    loop.advance(1s);
    view = viewFor(user);
    check(view && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy,
          "Stable automatic recovery did not renew the crash budget.");
    user->terminateRenderer(yobro::engine::RendererTerminationKind::killed, 9);
    loop.advance(100ms);
    check(user->reloadCount == 4,
          "A killed renderer did not use the renewed recovery budget.");
    user->finishNavigation();
    (void)user->navigate("https://recovery.example/failure-during-stability");
    user->finishNavigation(false, "Navigation failed during renderer stability.");
    view = viewFor(user);
    check(view
          && view->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && view->state.error == "Navigation failed during renderer stability.",
          "A failed navigation during the stability guard left stale recovering state.");
    check(session.reloadUserTab(user->state().id),
          "Could not manually restore a stability-guard failure.");
    user->finishNavigation();
    check(terminations.size() == 4
          && terminations[0] == yobro::engine::RendererTerminationKind::abnormal
          && terminations[1] == yobro::engine::RendererTerminationKind::crashed
          && terminations[2] == yobro::engine::RendererTerminationKind::normal
          && terminations[3] == yobro::engine::RendererTerminationKind::killed,
          "Typed renderer termination evidence lost one of Qt's four statuses.");

    auto *foreground = dynamic_cast<FakePage *>(&session.newUserTab("https://recovery.example/foreground"));
    check(foreground != nullptr, "Background recovery fixture is not fake.");
    foreground->finishNavigation();
    user->terminateRenderer();
    loop.advance(100ms);
    check(user->reloadCount == 6,
          "A background user tab did not receive its one safe automatic reload.");
    user->finishNavigation();
    loop.advance(1s);

    auto *privatePage = dynamic_cast<FakePage *>(&session.newUserTab(
        "https://recovery.example/private",
        true
    ));
    check(privatePage != nullptr, "Private recovery fixture is not fake.");
    privatePage->finishNavigation();
    privatePage->terminateRenderer();
    loop.advance(100ms);
    check(privatePage->reloadCount == 1,
          "A private user tab did not recover in its existing off-record page.");
    privatePage->finishNavigation();
    loop.advance(1s);

    auto *inactivePage = dynamic_cast<FakePage *>(&session.newUserTab("https://recovery.example/inactive"));
    check(inactivePage != nullptr, "Inactive-profile recovery fixture is not fake.");
    inactivePage->finishNavigation();
    inactivePage->deferStop = true;
    inactivePage->terminateRenderer();
    loop.advance(100ms);
    check(inactivePage->reloadCount == 1 && inactivePage->state().loading,
          "In-flight profile cancellation did not reach the recovery reload.");
    inactivePage->deferNavigationResult = true;
    session.setProfileActive(false);
    check(inactivePage->stopCount == 1,
          "Profile deactivation did not stop an in-flight recovery navigation.");
    session.setProfileActive(true);
    inactivePage->finishNavigation();
    view = viewFor(inactivePage);
    check(view
          && view->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && view->state.error,
          "An untagged late recovery state cleared failure after immediate profile reactivation.");
    inactivePage->completeNavigationResult();
    loop.advance(2s);
    view = viewFor(inactivePage);
    check(view
          && view->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && view->state.error
          && inactivePage->reloadCount == 1,
          "A tombstoned recovery result restored health after immediate profile reactivation.");
    inactivePage->deferNavigationResult = false;
    inactivePage->deferStop = false;
    const auto linkToken = inactivePage->navigate(
        "https://recovery.example/reactivated-link"
    );
    check(linkToken != 0,
          "A fresh in-page navigation was not started after profile reactivation.");
    inactivePage->finishNavigation();
    view = viewFor(inactivePage);
    check(view
          && !view->state.loading
          && !view->state.error
          && view->state.url == "https://recovery.example/reactivated-link"
          && view->rendererRecovery == yobro::controller::RendererRecoveryState::recovering,
          "A fresh loadStarted did not release the old profile-cancellation guard.");
    loop.advance(1s);
    view = viewFor(inactivePage);
    check(view
          && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy,
          "A stable post-reactivation link navigation did not reset the crash budget.");
    check(session.navigateUserTab(
              inactivePage->state().id,
              "https://recovery.example/reactivated-address"
          ),
          "Session-owned address navigation was rejected after profile cancellation.");
    inactivePage->finishNavigation();
    view = viewFor(inactivePage);
    check(view
          && !view->state.loading
          && !view->state.error
          && view->state.url == "https://recovery.example/reactivated-address",
          "Session-owned address navigation did not publish its terminal state.");
    check(session.goBackUserTab(inactivePage->state().id)
          && inactivePage->backCount == 1,
          "Session-owned Back did not cross the navigation lifecycle seam.");
    inactivePage->finishNavigation();
    check(session.goForwardUserTab(inactivePage->state().id)
          && inactivePage->forwardCount == 1,
          "Session-owned Forward did not cross the navigation lifecycle seam.");
    inactivePage->finishNavigation();

    auto *closable = dynamic_cast<FakePage *>(&session.newUserTab("https://recovery.example/close"));
    check(closable != nullptr, "Close-cancellation recovery fixture is not fake.");
    closable->finishNavigation();
    closable->terminateRenderer();
    loop.advance(100ms);
    check(closable->state().loading,
          "Close-cancellation fixture did not enter an active recovery navigation.");
    const std::string closableId = closable->state().id;
    check(session.closeTab(closableId), "Could not close a tab with queued recovery.");
    loop.advance(1s);
    check(!session.setActiveUserTab(closableId),
          "A closed recovery tab survived its queued task.");

    auto *agent = dynamic_cast<FakePage *>(&session.newAgentTab("https://agent-recovery.example/page"));
    check(agent != nullptr, "Renderer recovery agent page is not fake.");
    agent->finishNavigation();

    const auto recoverAgent = [&] {
        auto response = std::make_shared<std::optional<std::string>>();
        controller.handleLine(R"({"command":"reload"})", [response](std::string value) {
            *response = std::move(value);
        });
        loop.drain();
        check(!response->has_value() && agent->state().loading,
              "Explicit agent reload did not wait for navigation.");
        agent->finishNavigation();
        loop.advance(150ms);
        check(response->has_value() && required(decodedResponse(**response), "ok").asBoolean(),
              "Explicit agent reload did not restore command readiness.");
        const auto recoveredView = viewFor(agent);
        check(recoveredView
              && recoveredView->rendererRecovery == yobro::controller::RendererRecoveryState::healthy,
              "Successful explicit agent reload left stale failed recovery state.");
    };

    int completionCount = 0;
    auto response = std::make_shared<std::optional<std::string>>();
    agent->deferSnapshot = true;
    controller.handleLine(R"({"command":"read"})", [response, &completionCount](std::string value) {
        ++completionCount;
        *response = std::move(value);
    });
    loop.drain();
    loop.advance(150ms);
    check(controller.busy(), "Deferred agent read did not remain busy.");
    agent->terminateRenderer(yobro::engine::RendererTerminationKind::crashed, 7);
    loop.drain();
    check(completionCount == 1 && response->has_value() && !controller.busy()
          && errorMessage(decodedResponse(**response)) == "Renderer terminated",
          "Agent read was not failed closed exactly once on renderer death.");
    loop.advance(1s);
    check(agent->reloadCount == 0, "Agent renderer death triggered an automatic reload.");
    agent->completeSnapshot();
    loop.drain();
    check(completionCount == 1, "Late read callback completed a crashed operation twice.");
    agent->deferSnapshot = false;
    recoverAgent();

    response = std::make_shared<std::optional<std::string>>();
    completionCount = 0;
    agent->deferFind = true;
    controller.handleLine(R"({"command":"find","query":"needle"})", [response, &completionCount](std::string value) {
        ++completionCount;
        *response = std::move(value);
    });
    loop.drain();
    loop.advance(150ms);
    agent->terminateRenderer(yobro::engine::RendererTerminationKind::abnormal, 8);
    loop.drain();
    agent->completeFind();
    loop.drain();
    check(completionCount == 1 && !controller.busy(),
          "Agent find callback was not crash-cancelled exactly once.");
    agent->deferFind = false;
    recoverAgent();

    response = std::make_shared<std::optional<std::string>>();
    completionCount = 0;
    agent->deferAction = true;
    controller.handleLine(
        R"({"command":"click","document":"doc-1","ref":"e1"})",
        [response, &completionCount](std::string value) {
            ++completionCount;
            *response = std::move(value);
        }
    );
    loop.drain();
    agent->terminateRenderer(yobro::engine::RendererTerminationKind::killed, 9);
    loop.drain();
    agent->completeAction();
    loop.drain();
    check(completionCount == 1 && !controller.busy(),
          "Agent action callback was not crash-cancelled exactly once.");
    agent->deferAction = false;
    recoverAgent();

    response = std::make_shared<std::optional<std::string>>();
    completionCount = 0;
    agent->deferScroll = true;
    controller.handleLine(R"({"command":"scroll","amount":600})", [response, &completionCount](std::string value) {
        ++completionCount;
        *response = std::move(value);
    });
    loop.drain();
    agent->terminateRenderer(yobro::engine::RendererTerminationKind::normal, 0);
    loop.drain();
    agent->completeScroll();
    loop.drain();
    check(completionCount == 1 && !controller.busy(),
          "Agent scroll callback was not crash-cancelled exactly once.");
    agent->deferScroll = false;
    recoverAgent();

    const auto expectCallbackTimeout = [&](
        std::string request,
        bool waitsForReadiness,
        std::function<void()> prepare,
        std::function<void()> completeLate
    ) {
        prepare();
        auto timeoutResponse = std::make_shared<std::optional<std::string>>();
        int timeoutCompletions = 0;
        controller.handleLine(std::move(request), [timeoutResponse, &timeoutCompletions](std::string value) {
            ++timeoutCompletions;
            *timeoutResponse = std::move(value);
        });
        loop.drain();
        if (waitsForReadiness)
            loop.advance(150ms);
        check(controller.busy(), "Deferred agent callback did not keep the controller busy before timeout.");
        loop.advance(20s);
        check(timeoutResponse->has_value()
              && !controller.busy()
              && errorMessage(decodedResponse(**timeoutResponse)) == "Agent page operation timed out."
              && timeoutCompletions == 1,
              "Agent callback phase did not terminate at its bounded deadline.");
        completeLate();
        loop.drain();
        check(timeoutCompletions == 1,
              "A callback arriving after its deadline completed the operation twice.");
    };

    expectCallbackTimeout(
        R"({"command":"read"})",
        true,
        [&] { agent->deferSnapshot = true; },
        [&] { agent->completeSnapshot(); agent->deferSnapshot = false; }
    );
    expectCallbackTimeout(
        R"({"command":"find","query":"needle"})",
        true,
        [&] { agent->deferFind = true; },
        [&] { agent->completeFind(); agent->deferFind = false; }
    );
    expectCallbackTimeout(
        R"({"command":"click","document":"doc-1","ref":"e1"})",
        false,
        [&] { agent->deferAction = true; },
        [&] { agent->completeAction(); agent->deferAction = false; }
    );
    expectCallbackTimeout(
        R"({"command":"scroll","amount":600})",
        false,
        [&] { agent->deferScroll = true; },
        [&] { agent->completeScroll(); agent->deferScroll = false; }
    );

    response = std::make_shared<std::optional<std::string>>();
    completionCount = 0;
    agent->deferSnapshot = true;
    controller.handleLine(R"({"command":"read"})", [response, &completionCount](std::string value) {
        ++completionCount;
        *response = std::move(value);
    });
    loop.drain();
    loop.advance(150ms);
    auto staleCallback = agent->takePendingSnapshot();
    const std::string staleJson = agent->snapshot;
    const std::string reusedAgentId = agent->state().id;
    check(static_cast<bool>(staleCallback), "ABA read callback was not captured.");
    check(session.closeTab(reusedAgentId), "Could not close the ABA agent tab.");
    loop.drain();
    check(completionCount == 1 && !controller.busy(),
          "Closing an agent tab did not settle its operation once.");
    auto *replacement = dynamic_cast<FakePage *>(&session.newAgentTab("https://agent-recovery.example/replacement"));
    check(replacement && replacement->state().id == reusedAgentId,
          "Agent callback ABA test did not reuse the tab id.");
    replacement->finishNavigation();
    staleCallback(staleJson, std::nullopt);
    loop.drain();
    check(completionCount == 1 && session.activeAgentPage() == replacement,
          "A stale callback crossed into an ABA replacement tab.");
}

void testEndCancelsNavigation() {
    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    FakeProfile *fakeProfile = profile.get();
    yobro::controller::BrowserSession session(
        std::move(profile),
        loop,
        {
            .version = "0.6.3",
            .socketPath = "/tmp/test.sock",
            .profileName = "Test",
            .generateId = [] { return "10000000-0000-4000-8000-000000000001"; },
        }
    );
    yobro::controller::ProtocolV2Controller controller(loop, session);
    (void)issue(controller, loop, R"({"command":"new"})");
    FakePage *agent = fakeProfile->lastPage();

    auto reload = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"reload"})", [reload](std::string value) { *reload = std::move(value); });
    loop.drain();
    loop.advance(150ms);
    check(!reload->has_value() && agent->state().loading, "Reload should still be pending.");

    const auto status = issue(controller, loop, R"({"command":"status"})");
    check(required(successResult(decodedResponse(*status)), "agentAction").asString() == "reload", "Busy status lost the action.");
    const auto tabs = issue(controller, loop, R"({"command":"tabs"})");
    (void)successResult(decodedResponse(*tabs));
    const auto busy = issue(controller, loop, R"({"command":"new"})");
    check(errorMessage(decodedResponse(*busy)) == "Agent is busy. Retry after the current command completes.", "Busy serialization differs.");
    const auto ended = issue(controller, loop, R"({"command":"end"})");
    (void)successResult(decodedResponse(*ended));
    loop.drain();
    check(reload->has_value(), "End did not settle the pending navigation.");
    check(errorMessage(decodedResponse(**reload)) == "Agent operation was paused.", "End cancellation error differs.");
}

void testAuthenticationAuthority() {
    ManualEventLoop loop;
    auto profile = std::make_unique<FakeProfile>();
    yobro::controller::BrowserSession session(
        std::move(profile), loop,
        {.profileName = "Auth Test"}
    );
    auto *user = dynamic_cast<FakePage *>(&session.newUserTab("https://auth.example/secure"));
    auto *agent = dynamic_cast<FakePage *>(&session.newAgentTab("https://auth.example/secure"));
    check(user && agent, "Authentication pages were not created.");
    user->finishNavigation();
    agent->finishNavigation();
    const yobro::engine::AuthenticationChallenge challenge{
        .origin = "https://auth.example", .realm = "Private", .proxy = false,
    };
    int prompts = 0;
    session.setAuthenticationPromptPresenter([&](const auto &) {
        ++prompts;
        return yobro::engine::AuthenticationCredentials{"person", "secret"};
    });
    check(!user->requestAuthentication(challenge), "Hidden auth surface returned credentials.");
    session.setPermissionSurfaceVisible(true);
    check(!agent->requestAuthentication(challenge), "Agent page received an auth prompt.");
    check(!user->requestAuthentication({.origin = "https://other.example", .realm = "Private"}),
          "Cross-origin auth challenge received credentials.");
    check(!user->requestAuthentication({.origin = "proxy.example", .realm = "Proxy", .proxy = true}),
          "Proxy auth challenge received credentials.");
    check(prompts == 0, "Denied authentication challenge reached the presenter.");
    auto accepted = user->requestAuthentication(challenge);
    check(accepted && accepted->user == "person" && accepted->password == "secret"
          && prompts == 1, "Active matching User challenge was not presented.");
    session.setAuthenticationPromptPresenter([&](const auto &) {
        (void)session.newUserTab("https://other.example/");
        return yobro::engine::AuthenticationCredentials{"person", "secret"};
    });
    check(!user->requestAuthentication(challenge),
          "Authentication credentials survived a tab switch during the prompt.");
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
        run("json-core", testJsonCore, count);
        run("dispatch-precedence", testDispatchPrecedence, count);
        run("browser-session-commands", testBrowserSessionCommands, count);
        run("popup-ownership-queued-close", testPopupOwnershipAndQueuedClose, count);
        run("permission-authority-lifecycle", testPermissionAuthorityAndLifecycle, count);
        run("authentication-authority", testAuthenticationAuthority, count);
        run("renderer-recovery-agent-lifecycle", testRendererRecoveryAndAgentLifecycle, count);
        run("end-cancels-navigation", testEndCancelsNavigation, count);
        std::cout << "PROTOCOL V2 CONTROLLER TESTS PASS (" << count << ")\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "PROTOCOL V2 CONTROLLER TESTS FAIL: " << error.what() << '\n';
        return 1;
    }
}
