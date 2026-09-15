#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace yobro::engine {

enum class PageOwner { user, agent };

enum class EngineErrorCode {
    forbidden,
    invalidRequest,
    bridgeUnavailable,
    bridgeRejected,
    navigationFailed,
    navigationSuperseded,
    rendererTerminated,
    operationFailed,
};

struct EngineError {
    EngineErrorCode code = EngineErrorCode::operationFailed;
    std::string message;

    [[nodiscard]] operator const std::string &() const noexcept { return message; }

    friend bool operator==(const EngineError &, const EngineError &) = default;
    friend bool operator==(const EngineError &error, std::string_view message) {
        return error.message == message;
    }
};

struct ProfileSpec {
    std::string id;
    std::string storagePath;
    std::string cachePath;
    bool persistent = true;
};

struct PageState {
    std::string id;
    PageOwner owner = PageOwner::user;
    std::string title;
    std::string url;
    std::string securityOrigin;
    bool loading = false;
    bool canGoBack = false;
    bool canGoForward = false;
    std::optional<std::string> error;
};

using NavigationToken = std::uint64_t;

struct NavigationResult {
    NavigationToken token = 0;
    PageState state;
    std::optional<EngineError> error;
};

enum class RendererTerminationKind {
    normal,
    abnormal,
    crashed,
    killed,
};

struct RendererTermination {
    RendererTerminationKind kind = RendererTerminationKind::abnormal;
    int exitCode = 0;
    PageState state;
};

/// The web capabilities a page may ask for. Chromium supports more of these
/// than WebKit does, so the shell has to describe and prompt for each one
/// instead of denying it silently.
enum class WebPermission {
    microphone,
    camera,
    microphoneAndCamera,
    geolocation,
    notifications,
    /// Reading the system clipboard without a paste gesture.
    clipboard,
    localFonts,
    /// Pointer lock, used by games and 3D viewers.
    pointerLock,
    /// Sharing a screen or a window through `getDisplayMedia`.
    screenShare,
    /// The same, with the audio of the shared source.
    screenShareWithAudio,
};

enum class PermissionDecision { deny, grant };

class PermissionRequest {
public:
    virtual ~PermissionRequest() = default;
    [[nodiscard]] virtual WebPermission permission() const noexcept = 0;
    [[nodiscard]] virtual std::string_view origin() const noexcept = 0;
    virtual void resolve(PermissionDecision decision) noexcept = 0;
};

using PermissionRequestHandler = std::function<void(
    std::unique_ptr<PermissionRequest> request
)>;

struct AuthenticationChallenge {
    std::string origin;
    std::string realm;
    bool proxy = false;
};

struct AuthenticationCredentials {
    std::string user;
    std::string password;
};

using AuthenticationChallengeHandler = std::function<std::optional<AuthenticationCredentials>(
    const AuthenticationChallenge &challenge
)>;

/// A rejected server certificate, described well enough for a user to judge it.
struct CertificateProblem {
    std::string host;
    /// The engine's own wording, e.g. that the issuer is unknown.
    std::string description;
    /// False when the engine refuses to let anyone past this error at all.
    bool overridable = false;
    bool mainFrame = true;
    /// SHA-256 over the leaf certificate in uppercase hex pairs. This is the
    /// value to compare against the server, not the names below.
    std::string fingerprint;
    std::string subject;
    std::string issuer;
    std::string validity;
};

/// Returns true to continue to the site anyway. Not answering means reject.
using CertificateProblemHandler = std::function<bool(const CertificateProblem &problem)>;

/// How far along a passkey request is. Mirrors the engine's own states so no
/// stage is silently collapsed into another.
enum class PasskeyStage {
    notStarted,
    selectAccount,
    collectPin,
    finishTokenCollection,
    requestFailed,
    cancelled,
    completed,
};

enum class PasskeyPinReason { set, change, challenge };

enum class PasskeyPinError {
    none,
    userVerificationLocked,
    wrongPin,
    tooShort,
    invalidCharacters,
    sameAsCurrentPin,
};

enum class PasskeyFailure {
    timeout,
    keyNotRegistered,
    keyAlreadyRegistered,
    softPinBlock,
    hardPinBlock,
    authenticatorRemovedDuringPinEntry,
    authenticatorMissingResidentKeys,
    authenticatorMissingUserVerification,
    authenticatorMissingLargeBlob,
    noCommonAlgorithms,
    storageFull,
    userConsentDenied,
    windowsUserCancelled,
    unknown,
};

/// What a passkey request looks like at one moment.
struct PasskeyRequest {
    PasskeyStage stage = PasskeyStage::notStarted;
    /// The site asking, as the engine sees it.
    std::string relyingPartyId;
    /// The accounts to choose from while the stage is `selectAccount`.
    std::vector<std::string> userNames;
    PasskeyPinReason pinReason = PasskeyPinReason::challenge;
    PasskeyPinError pinError = PasskeyPinError::none;
    int minimumPinLength = 0;
    int remainingAttempts = 0;
    PasskeyFailure failure = PasskeyFailure::unknown;
};

/// The answers a host may give while a passkey request runs. Calling one after
/// the request ended does nothing.
struct PasskeyControls {
    std::function<void(const std::string &account)> selectAccount;
    std::function<void(const std::string &pin)> setPin;
    std::function<void()> retry;
    std::function<void()> cancel;
};

/// Called for the first state and for every change afterwards.
///
/// Without a handler the engine's request is cancelled: a passkey prompt nobody
/// can see must not sit there waiting.
using PasskeyHandler = std::function<void(const PasskeyRequest &, const PasskeyControls &)>;

/// One thing the user could share: a whole screen or a single window.
struct DesktopMediaSource {
    /// True for a window, false for a whole screen. The engine keeps two
    /// separate lists and the index is only meaningful together with this flag.
    bool window = false;
    int index = 0;
    /// What the platform calls it. May be empty for an unnamed window.
    std::string name;
};

/// What a page asked to capture, with everything the user can choose from.
struct DesktopMediaRequest {
    std::string origin;
    /// Screens first, then windows, in the engine's own order.
    std::vector<DesktopMediaSource> sources;
};

struct DesktopMediaControls {
    /// Picks one source. Returns false when the choice no longer exists, which
    /// happens if a window closed while the user was looking at the list.
    std::function<bool(bool window, int index)> select;
    std::function<void()> cancel;
};

/// Shows the screen and window picker for one `getDisplayMedia` call.
///
/// Without a handler the request is cancelled. Qt has no system picker of its
/// own: if nobody chooses a source, the page waits forever, so silence is not
/// an option here. The handler must answer before it returns — the engine
/// cancels the request otherwise — so the picker has to be a modal surface.
using DesktopMediaHandler = std::function<void(const DesktopMediaRequest &, const DesktopMediaControls &)>;

class BrowserPage;

struct NewWindowRequest {
    // The host must call this synchronously while handling newWindowRequested.
    // A successful call transfers Chromium's pending window into the supplied,
    // already host-owned page without replaying the requested URL.
    std::function<bool(BrowserPage &)> openIn;
};

struct PageEventHandlers {
    std::function<void(const PageState &)> stateChanged;
    std::function<void(const NavigationResult &)> navigationFinished;
    std::function<void(const RendererTermination &)> rendererTerminated;
    std::function<void(const NewWindowRequest &)> newWindowRequested;
    std::function<void()> windowCloseRequested;
};

class PageEventSubscription {
public:
    virtual ~PageEventSubscription() = default;
};

class BrowserPage {
public:
    using JsonCallback = std::function<void(
        std::string json,
        std::optional<EngineError> error
    )>;
    using BoolCallback = std::function<void(
        bool value,
        std::optional<EngineError> error
    )>;

    virtual ~BrowserPage() = default;
    [[nodiscard]] virtual PageState state() const = 0;
    [[nodiscard]] virtual std::unique_ptr<PageEventSubscription> subscribe(
        PageEventHandlers handlers
    ) = 0;
    virtual void setPermissionRequestHandler(PermissionRequestHandler handler) = 0;
    virtual void setAuthenticationChallengeHandler(AuthenticationChallengeHandler handler) {
        (void)handler;
    }
    /// Without a handler an invalid certificate always ends the navigation.
    virtual void setCertificateProblemHandler(CertificateProblemHandler handler) {
        (void)handler;
    }
    /// Without a handler a passkey request is cancelled immediately.
    /// Installs the screen and window picker. Without one every screen-sharing
    /// request is cancelled.
    virtual void setDesktopMediaHandler(DesktopMediaHandler handler) {
        (void)handler;
    }
    virtual void setPasskeyHandler(PasskeyHandler handler) {
        (void)handler;
    }
    [[nodiscard]] virtual NavigationToken navigate(std::string_view url) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual NavigationToken reload() = 0;
    [[nodiscard]] virtual NavigationToken goBack() = 0;
    [[nodiscard]] virtual NavigationToken goForward() = 0;
    virtual void readAgentSnapshot(JsonCallback callback) = 0;
    virtual void performAgentAction(std::string_view requestJson, JsonCallback callback) = 0;
    virtual void findText(std::string_view query, bool backwards, BoolCallback callback) = 0;
    virtual void scrollBy(int amount, BoolCallback callback) = 0;
};

class BrowserProfile {
public:
    virtual ~BrowserProfile() = default;
    [[nodiscard]] virtual const ProfileSpec &spec() const = 0;
    [[nodiscard]] virtual std::unique_ptr<BrowserPage> createPage(
        std::string id,
        PageOwner owner,
        bool privatePage = false
    ) = 0;
};

class BrowserEngine {
public:
    virtual ~BrowserEngine() = default;
    [[nodiscard]] virtual std::unique_ptr<BrowserProfile> openProfile(ProfileSpec spec) = 0;
};

} // namespace yobro::engine
