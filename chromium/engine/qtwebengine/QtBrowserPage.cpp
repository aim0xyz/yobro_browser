#include "engine/qtwebengine/QtBrowserPage.hpp"

#include <QAuthenticator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTimer>
#include <QVariant>
#include <QCryptographicHash>
#include <QSslCertificate>
#include <QStringList>
#include <QWebEngineCertificateError>
#include <QWebEngineDesktopMediaRequest>
#include <QWebEngineFindTextResult>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>
#include <QWebEngineWebAuthUxRequest>
#include <QPointer>

#include <algorithm>
#include <memory>
#include <map>
#include <utility>
#include <vector>

namespace yobro::qtwebengine {
namespace {

constexpr auto pageOwnerProperty = "_yobro_page_owner";

std::string compactJson(const QVariant &value) {
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (json.isObject())
        return QJsonDocument(json.toObject()).toJson(QJsonDocument::Compact).toStdString();
    if (json.isArray())
        return QJsonDocument(json.toArray()).toJson(QJsonDocument::Compact).toStdString();
    QJsonArray wrapper;
    wrapper.append(json);
    const QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return std::string(encoded.constData() + 1, static_cast<std::size_t>(encoded.size() - 2));
}

engine::EngineError error(engine::EngineErrorCode code, std::string message) {
    return {.code = code, .message = std::move(message)};
}

engine::RendererTerminationKind rendererTerminationKind(
    QWebEnginePage::RenderProcessTerminationStatus status
) {
    switch (status) {
    case QWebEnginePage::NormalTerminationStatus:
        return engine::RendererTerminationKind::normal;
    case QWebEnginePage::AbnormalTerminationStatus:
        return engine::RendererTerminationKind::abnormal;
    case QWebEnginePage::CrashedTerminationStatus:
        return engine::RendererTerminationKind::crashed;
    case QWebEnginePage::KilledTerminationStatus:
        return engine::RendererTerminationKind::killed;
    }
    return engine::RendererTerminationKind::abnormal;
}

std::string rendererTerminationMessage(
    engine::RendererTerminationKind kind,
    int exitCode
) {
    std::string reason;
    switch (kind) {
    case engine::RendererTerminationKind::normal:
        reason = "exited normally";
        break;
    case engine::RendererTerminationKind::abnormal:
        reason = "terminated abnormally";
        break;
    case engine::RendererTerminationKind::crashed:
        reason = "crashed";
        break;
    case engine::RendererTerminationKind::killed:
        reason = "was killed";
        break;
    }
    return "Chromium renderer " + reason + " with exit code "
        + std::to_string(exitCode) + ".";
}

std::string canonicalSecurityOrigin(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return {};
    const QString host = url.host(QUrl::FullyDecoded);
    if (host.isEmpty())
        return {};
    const int defaultPort = scheme == QStringLiteral("https") ? 443 : 80;
    const int port = url.port(defaultPort);
    if (port <= 0 || port > 65535)
        return {};

    QUrl origin;
    origin.setScheme(scheme);
    origin.setHost(host);
    if (port != defaultPort)
        origin.setPort(port);
    return origin.toEncoded(QUrl::FullyEncoded).toStdString();
}

std::optional<engine::WebPermission> webPermission(
    QWebEnginePermission::PermissionType permission
) {
    switch (permission) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
        return engine::WebPermission::microphone;
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
        return engine::WebPermission::camera;
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
        return engine::WebPermission::microphoneAndCamera;
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
        return engine::WebPermission::screenShare;
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
        return engine::WebPermission::screenShareWithAudio;
    case QWebEnginePermission::PermissionType::Geolocation:
        return engine::WebPermission::geolocation;
    case QWebEnginePermission::PermissionType::Notifications:
        return engine::WebPermission::notifications;
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
        return engine::WebPermission::clipboard;
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
        return engine::WebPermission::localFonts;
    case QWebEnginePermission::PermissionType::MouseLock:
        return engine::WebPermission::pointerLock;
    case QWebEnginePermission::PermissionType::Unsupported:
        return std::nullopt;
    }
    return std::nullopt;
}

/// SHA-256 over the leaf certificate, grouped in pairs like the WebKit build
/// shows it, so the two engines display comparable fingerprints.
std::string leafFingerprint(const QList<QSslCertificate> &chain) {
    if (chain.isEmpty()) return {};
    const QByteArray digest = chain.first().digest(QCryptographicHash::Sha256);
    if (digest.isEmpty()) return {};
    const QString hex = QString::fromLatin1(digest.toHex()).toUpper();
    QStringList pairs;
    for (int index = 0; index + 1 < hex.size(); index += 2) pairs.append(hex.mid(index, 2));
    return pairs.join(QLatin1Char(' ')).toStdString();
}

std::string leafField(
    const QList<QSslCertificate> &chain,
    QSslCertificate::SubjectInfo field,
    bool fromIssuer
) {
    if (chain.isEmpty()) return {};
    const QStringList values = fromIssuer
        ? chain.first().issuerInfo(field)
        : chain.first().subjectInfo(field);
    return values.isEmpty() ? std::string{} : values.first().toStdString();
}

std::string leafValidity(const QList<QSslCertificate> &chain) {
    if (chain.isEmpty()) return {};
    const QSslCertificate leaf = chain.first();
    const QString from = leaf.effectiveDate().toLocalTime().toString(QStringLiteral("dd.MM.yyyy"));
    const QString until = leaf.expiryDate().toLocalTime().toString(QStringLiteral("dd.MM.yyyy"));
    if (from.isEmpty() || until.isEmpty()) return {};
    return (from + QStringLiteral(" – ") + until).toStdString();
}

class PermissionDenyGuard final {
public:
    explicit PermissionDenyGuard(const QWebEnginePermission &permission)
        : permission_(permission) {}

    ~PermissionDenyGuard() noexcept {
        if (!active_)
            return;
        try {
            if (permission_.isValid())
                permission_.deny();
        } catch (...) {
            // No C++ exception may escape Qt signal dispatch.
        }
    }

    void dismiss() noexcept { active_ = false; }

private:
    const QWebEnginePermission &permission_;
    bool active_ = true;
};

class QtPermissionState final {
public:
    explicit QtPermissionState(QWebEnginePermission permission)
        : permission_(std::move(permission)) {}

    ~QtPermissionState() {
        resolve(engine::PermissionDecision::deny);
    }

    void resolve(engine::PermissionDecision decision) noexcept {
        if (resolved_)
            return;
        resolved_ = true;
        try {
            if (!permission_.isValid())
                return;
            if (decision == engine::PermissionDecision::grant)
                permission_.grant();
            else
                permission_.deny();
        } catch (...) {
            if (decision == engine::PermissionDecision::grant) {
                try {
                    if (permission_.isValid())
                        permission_.deny();
                } catch (...) {
                    // Best-effort deny after a native grant failure.
                }
            }
        }
    }

private:
    QWebEnginePermission permission_;
    bool resolved_ = false;
};

class QtPermissionRequest final : public engine::PermissionRequest {
public:
    QtPermissionRequest(
        std::shared_ptr<QtPermissionState> state,
        engine::WebPermission type,
        std::string origin
    ) : state_(std::move(state)), type_(type), origin_(std::move(origin)) {}

    ~QtPermissionRequest() override {
        resolve(engine::PermissionDecision::deny);
    }

    [[nodiscard]] engine::WebPermission permission() const noexcept override {
        return type_;
    }

    [[nodiscard]] std::string_view origin() const noexcept override {
        return origin_;
    }

    void resolve(engine::PermissionDecision decision) noexcept override {
        if (state_)
            state_->resolve(decision);
    }

private:
    std::shared_ptr<QtPermissionState> state_;
    engine::WebPermission type_;
    std::string origin_;
};

} // namespace

struct QtBrowserPage::EventRegistry {
    std::size_t nextId = 1;
    std::map<std::size_t, engine::PageEventHandlers> handlers;
};

class QtBrowserPage::Subscription final : public engine::PageEventSubscription {
public:
    Subscription(std::weak_ptr<EventRegistry> registry, std::size_t id)
        : registry_(std::move(registry)), id_(id) {}

    ~Subscription() override {
        if (const auto registry = registry_.lock())
            registry->handlers.erase(id_);
    }

private:
    std::weak_ptr<EventRegistry> registry_;
    std::size_t id_ = 0;
};

QtBrowserPage::QtBrowserPage(
    QWebEngineProfile *profile,
    std::string id,
    engine::PageOwner owner
) : view_(std::make_unique<QWebEngineView>(profile)), events_(std::make_shared<EventRegistry>()) {
    state_.id = std::move(id);
    state_.owner = owner;
    setNativePageOwner(view_->page(), owner);
    retiredLoadGenerations_.reserve(maxRetiredLoadGenerations);

    if (owner == engine::PageOwner::agent)
        installAgentBridge();

    // Qt delivers authentication challenges synchronously. Only the owning
    // session may authorize a prompt and return credentials for this page.
    QObject::connect(
        view_->page(),
        &QWebEnginePage::authenticationRequired,
        view_.get(),
        [this](const QUrl &url, QAuthenticator *authenticator) {
            if (!authenticator || !authenticationChallengeHandler_) return;
            const engine::AuthenticationChallenge challenge{
                .origin = canonicalSecurityOrigin(url),
                .realm = authenticator->realm().toStdString(),
                .proxy = false,
            };
            const auto credentials = authenticationChallengeHandler_(challenge);
            if (!credentials) return;
            authenticator->setUser(QString::fromStdString(credentials->user));
            authenticator->setPassword(QString::fromStdString(credentials->password));
        }
    );
    QObject::connect(
        view_->page(),
        &QWebEnginePage::proxyAuthenticationRequired,
        view_.get(),
        [this](const QUrl &, QAuthenticator *authenticator, const QString &proxyHost) {
            if (!authenticator || !authenticationChallengeHandler_) return;
            const engine::AuthenticationChallenge challenge{
                .origin = proxyHost.toStdString(),
                .realm = authenticator->realm().toStdString(),
                .proxy = true,
            };
            const auto credentials = authenticationChallengeHandler_(challenge);
            if (!credentials) return;
            authenticator->setUser(QString::fromStdString(credentials->user));
            authenticator->setPassword(QString::fromStdString(credentials->password));
        }
    );

    // A passkey request is a multi-step conversation, so it is handed to the host
    // as a state machine rather than answered on the spot.
    QObject::connect(
        view_->page(),
        &QWebEnginePage::webAuthUxRequested,
        view_.get(),
        [this](QWebEngineWebAuthUxRequest *request) {
            try {
                handlePasskeyRequest(request);
            } catch (...) {
                // No C++ exception may escape Qt signal dispatch, and an
                // unanswered request would hang the page.
                if (request) request->cancel();
            }
        }
    );

    // Screen sharing. Qt hands over two model lists and expects exactly one
    // answer; there is no system picker to fall back on, so an unanswered
    // request would leave the page waiting forever.
    QObject::connect(
        view_->page(),
        &QWebEnginePage::desktopMediaRequested,
        view_.get(),
        [this](const QWebEngineDesktopMediaRequest &request) {
            try {
                handleDesktopMediaRequest(request);
            } catch (...) {
                request.cancel();
            }
        }
    );

    // Certificate errors are delivered synchronously as well. Without a handler
    // the navigation ends, which is the safe default for a public website.
    QObject::connect(
        view_->page(),
        &QWebEnginePage::certificateError,
        view_.get(),
        [this](QWebEngineCertificateError error) {
            try {
                if (!certificateProblemHandler_ || !error.isOverridable()) {
                    error.rejectCertificate();
                    return;
                }
                const engine::CertificateProblem problem{
                    .host = error.url().host().toLower().toStdString(),
                    .description = error.description().toStdString(),
                    .overridable = true,
                    .mainFrame = error.isMainFrame(),
                    .fingerprint = leafFingerprint(error.certificateChain()),
                    .subject = leafField(error.certificateChain(), QSslCertificate::CommonName, false),
                    .issuer = leafField(error.certificateChain(), QSslCertificate::CommonName, true),
                    .validity = leafValidity(error.certificateChain()),
                };
                if (problem.fingerprint.empty() || certificateProblemHandler_(problem) == false)
                    error.rejectCertificate();
                else
                    error.acceptCertificate();
            } catch (...) {
                // No C++ exception may escape Qt signal dispatch, and an
                // unexpected failure must not turn into an accepted certificate.
                try { error.rejectCertificate(); } catch (...) {}
            }
        }
    );

    QObject::connect(view_.get(), &QWebEngineView::titleChanged, view_.get(), [this](const QString &title) {
        state_.title = title.toStdString();
        publishState();
    });
    QObject::connect(view_.get(), &QWebEngineView::urlChanged, view_.get(), [this](const QUrl &url) {
        state_.url = url.toString().toStdString();
        state_.securityOrigin = canonicalSecurityOrigin(url);
        state_.canGoBack = view_->history()->canGoBack();
        state_.canGoForward = view_->history()->canGoForward();
        publishState();
    });
    QObject::connect(view_.get(), &QWebEngineView::loadStarted, view_.get(), [this] {
        handleLoadStarted();
    });
    QObject::connect(view_.get(), &QWebEngineView::loadFinished, view_.get(), [this](bool ok) {
        handleLoadFinished(ok);
    });
    QObject::connect(
        view_.get(),
        &QWebEngineView::renderProcessTerminated,
        view_.get(),
        [this](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
            handleRendererTerminated(static_cast<int>(status), exitCode);
        }
    );
    QObject::connect(
        view_->page(),
        &QWebEnginePage::permissionRequested,
        view_.get(),
        [this](QWebEnginePermission nativePermission) {
            PermissionDenyGuard denyGuard(nativePermission);
            std::shared_ptr<QtPermissionState> capability;
            try {
                if (!nativePermission.isValid()
                    || nativePermission.state() != QWebEnginePermission::State::Ask) {
                    return;
                }
                const auto type = webPermission(nativePermission.permissionType());
                const std::string origin = canonicalSecurityOrigin(nativePermission.origin());
                if (!type || origin.empty())
                    return;

                const engine::PermissionRequestHandler handler = permissionRequestHandler_;
                if (!handler)
                    return;
                capability = std::make_shared<QtPermissionState>(nativePermission);
                auto request = std::make_unique<QtPermissionRequest>(
                    capability,
                    *type,
                    origin
                );
                handler(std::move(request));
                denyGuard.dismiss();
            } catch (...) {
                if (capability) {
                    capability->resolve(engine::PermissionDecision::deny);
                    denyGuard.dismiss();
                }
                // The guard denies allocation, conversion, copy, and consumer failures.
            }
        }
    );
    QObject::connect(
        view_->page(),
        &QWebEnginePage::newWindowRequested,
        view_.get(),
        [this](QWebEngineNewWindowRequest &nativeRequest) {
            bool accepted = false;
            const engine::NewWindowRequest request{
                .openIn = [this, &nativeRequest, &accepted](engine::BrowserPage &child) {
                    if (accepted)
                        return false;
                    auto *qtChild = dynamic_cast<QtBrowserPage *>(&child);
                    if (!qtChild || qtChild == this || qtChild->profile() != this->profile())
                        return false;
                    nativeRequest.openIn(qtChild->view()->page());
                    accepted = true;
                    return true;
                },
            };
            const auto handlers = events_->handlers;
            for (const auto &[id, callbacks] : handlers) {
                (void)id;
                if (!callbacks.newWindowRequested)
                    continue;
                try {
                    callbacks.newWindowRequested(request);
                } catch (...) {
                    // Exceptions must not unwind through Qt's signal dispatch.
                }
                if (accepted)
                    break;
            }
        }
    );
    QObject::connect(
        view_->page(),
        &QWebEnginePage::windowCloseRequested,
        view_.get(),
        [this] {
            const auto handlers = events_->handlers;
            for (const auto &[id, callbacks] : handlers) {
                (void)id;
                if (callbacks.windowCloseRequested)
                    callbacks.windowCloseRequested();
            }
        }
    );
}

void QtBrowserPage::handleLoadStarted() {
    if (activeNavigationToken_ == 0) {
        activeNavigationToken_ = nextNavigationToken_++;
        activeNavigationGeneration_ = nextNavigationGeneration_++;
    }
    if (signalLoadGeneration_ != 0
        && signalLoadGeneration_ != activeNavigationGeneration_) {
        retireLoadGeneration(signalLoadGeneration_);
    }
    signalLoadGeneration_ = activeNavigationGeneration_;
    if (pendingLoadGeneration_ == activeNavigationGeneration_)
        pendingLoadGeneration_ = 0;
    if (ambiguousFailedFinishGeneration_ == activeNavigationGeneration_)
        ambiguousFailedFinishGeneration_ = 0;
    state_.loading = true;
    state_.error.reset();
    publishState();
}

void QtBrowserPage::handleLoadFinished(bool ok) {
    // Qt does not attach a navigation id to loadFinished. A renderer death
    // or superseded load can therefore leave one stale false signal behind.
    // Consume that bounded debt before it can terminate a newer token.
    if (!ok && consumeRetiredFailedFinish()) {
        if (activeNavigationToken_ != 0)
            ambiguousFailedFinishGeneration_ = activeNavigationGeneration_;
        return;
    }

    const std::uint64_t loadGeneration = signalLoadGeneration_ != 0
        ? signalLoadGeneration_
        : pendingLoadGeneration_;
    if (loadGeneration == 0)
        return;
    if (activeNavigationToken_ == 0
        || activeNavigationGeneration_ != loadGeneration) {
        if (signalLoadGeneration_ == loadGeneration)
            signalLoadGeneration_ = 0;
        if (pendingLoadGeneration_ == loadGeneration)
            pendingLoadGeneration_ = 0;
        return;
    }

    const engine::NavigationToken token = activeNavigationToken_;
    if (!ok) {
        // A generic load failure can race the stronger renderer signal. The
        // delayed completion is bound to this exact identity.
        QTimer::singleShot(25, view_.get(), [this, token, loadGeneration] {
            finishFailedNavigation(token, loadGeneration);
        });
        return;
    }

    state_.loading = false;
    state_.error.reset();
    state_.canGoBack = view_->history()->canGoBack();
    state_.canGoForward = view_->history()->canGoForward();
    (void)finishNavigation(token, loadGeneration, std::nullopt, true);
}

void QtBrowserPage::handleRendererTerminated(int nativeStatus, int exitCode) {
    const auto status = static_cast<QWebEnginePage::RenderProcessTerminationStatus>(nativeStatus);
    const engine::RendererTerminationKind kind = rendererTerminationKind(status);
    const std::uint64_t terminatedGeneration = signalLoadGeneration_ != 0
        ? signalLoadGeneration_
        : (pendingLoadGeneration_ != 0
            ? pendingLoadGeneration_
            : activeNavigationGeneration_);
    if (terminatedGeneration != 0)
        retireLoadGeneration(terminatedGeneration);
    if (activeNavigationGeneration_ != 0
        && activeNavigationGeneration_ != terminatedGeneration) {
        retireLoadGeneration(activeNavigationGeneration_);
    }

    const engine::NavigationToken token = activeNavigationToken_;
    const std::uint64_t navigationGeneration = activeNavigationGeneration_;
    state_.loading = false;
    state_.error = rendererTerminationMessage(kind, exitCode);
    if (token == 0
        || !finishNavigation(
            token,
            navigationGeneration,
            error(engine::EngineErrorCode::rendererTerminated, *state_.error),
            true
        )) {
        publishState();
    }
    const engine::RendererTermination termination{
        .kind = kind,
        .exitCode = exitCode,
        .state = state_,
    };
    const auto handlers = events_->handlers;
    for (const auto &[id, callbacks] : handlers) {
        (void)id;
        if (!callbacks.rendererTerminated)
            continue;
        try {
            callbacks.rendererTerminated(termination);
        } catch (...) {
            // Exceptions must not unwind through Qt signal dispatch.
        }
    }
}

QtBrowserPage::~QtBrowserPage() {
    permissionRequestHandler_ = {};
    events_->handlers.clear();
}

engine::PageState QtBrowserPage::state() const {
    return state_;
}

std::unique_ptr<engine::PageEventSubscription> QtBrowserPage::subscribe(
    engine::PageEventHandlers handlers
) {
    const std::size_t id = events_->nextId++;
    events_->handlers.emplace(id, std::move(handlers));
    return std::make_unique<Subscription>(events_, id);
}

void QtBrowserPage::setPermissionRequestHandler(
    engine::PermissionRequestHandler handler
) {
    permissionRequestHandler_ = std::move(handler);
}

void QtBrowserPage::setCertificateProblemHandler(
    engine::CertificateProblemHandler handler
) {
    certificateProblemHandler_ = std::move(handler);
}

void QtBrowserPage::setDesktopMediaHandler(engine::DesktopMediaHandler handler) {
    desktopMediaHandler_ = std::move(handler);
}

void QtBrowserPage::setPasskeyHandler(engine::PasskeyHandler handler) {
    passkeyHandler_ = std::move(handler);
}

namespace {

engine::PasskeyStage passkeyStage(QWebEngineWebAuthUxRequest::WebAuthUxState state) {
    using State = QWebEngineWebAuthUxRequest::WebAuthUxState;
    switch (state) {
    case State::NotStarted: return engine::PasskeyStage::notStarted;
    case State::SelectAccount: return engine::PasskeyStage::selectAccount;
    case State::CollectPin: return engine::PasskeyStage::collectPin;
    case State::FinishTokenCollection: return engine::PasskeyStage::finishTokenCollection;
    case State::RequestFailed: return engine::PasskeyStage::requestFailed;
    case State::Cancelled: return engine::PasskeyStage::cancelled;
    case State::Completed: return engine::PasskeyStage::completed;
    }
    return engine::PasskeyStage::notStarted;
}

engine::PasskeyPinReason passkeyPinReason(QWebEngineWebAuthUxRequest::PinEntryReason reason) {
    using Reason = QWebEngineWebAuthUxRequest::PinEntryReason;
    switch (reason) {
    case Reason::Set: return engine::PasskeyPinReason::set;
    case Reason::Change: return engine::PasskeyPinReason::change;
    case Reason::Challenge: return engine::PasskeyPinReason::challenge;
    }
    return engine::PasskeyPinReason::challenge;
}

engine::PasskeyPinError passkeyPinError(QWebEngineWebAuthUxRequest::PinEntryError error) {
    using Error = QWebEngineWebAuthUxRequest::PinEntryError;
    switch (error) {
    case Error::NoError: return engine::PasskeyPinError::none;
    case Error::InternalUvLocked: return engine::PasskeyPinError::userVerificationLocked;
    case Error::WrongPin: return engine::PasskeyPinError::wrongPin;
    case Error::TooShort: return engine::PasskeyPinError::tooShort;
    case Error::InvalidCharacters: return engine::PasskeyPinError::invalidCharacters;
    case Error::SameAsCurrentPin: return engine::PasskeyPinError::sameAsCurrentPin;
    }
    return engine::PasskeyPinError::none;
}

engine::PasskeyFailure passkeyFailure(QWebEngineWebAuthUxRequest::RequestFailureReason reason) {
    using Reason = QWebEngineWebAuthUxRequest::RequestFailureReason;
    switch (reason) {
    case Reason::Timeout: return engine::PasskeyFailure::timeout;
    case Reason::KeyNotRegistered: return engine::PasskeyFailure::keyNotRegistered;
    case Reason::KeyAlreadyRegistered: return engine::PasskeyFailure::keyAlreadyRegistered;
    case Reason::SoftPinBlock: return engine::PasskeyFailure::softPinBlock;
    case Reason::HardPinBlock: return engine::PasskeyFailure::hardPinBlock;
    case Reason::AuthenticatorRemovedDuringPinEntry:
        return engine::PasskeyFailure::authenticatorRemovedDuringPinEntry;
    case Reason::AuthenticatorMissingResidentKeys:
        return engine::PasskeyFailure::authenticatorMissingResidentKeys;
    case Reason::AuthenticatorMissingUserVerification:
        return engine::PasskeyFailure::authenticatorMissingUserVerification;
    case Reason::AuthenticatorMissingLargeBlob:
        return engine::PasskeyFailure::authenticatorMissingLargeBlob;
    case Reason::NoCommonAlgorithms: return engine::PasskeyFailure::noCommonAlgorithms;
    case Reason::StorageFull: return engine::PasskeyFailure::storageFull;
    case Reason::UserConsentDenied: return engine::PasskeyFailure::userConsentDenied;
    case Reason::WinUserCancelled: return engine::PasskeyFailure::windowsUserCancelled;
    }
    return engine::PasskeyFailure::unknown;
}

engine::PasskeyRequest describePasskey(const QWebEngineWebAuthUxRequest *request) {
    engine::PasskeyRequest description;
    description.stage = passkeyStage(request->state());
    description.relyingPartyId = request->relyingPartyId().toStdString();
    for (const QString &name : request->userNames())
        description.userNames.push_back(name.toStdString());
    const QWebEngineWebAuthPinRequest pin = request->pinRequest();
    description.pinReason = passkeyPinReason(pin.reason);
    description.pinError = passkeyPinError(pin.error);
    description.minimumPinLength = static_cast<int>(pin.minPinLength);
    description.remainingAttempts = pin.remainingAttempts;
    description.failure = passkeyFailure(request->requestFailureReason());
    return description;
}

} // namespace

void QtBrowserPage::handleDesktopMediaRequest(const QWebEngineDesktopMediaRequest &request) {
    if (!desktopMediaHandler_) {
        request.cancel();
        return;
    }
    QAbstractListModel *screens = request.screensModel();
    QAbstractListModel *windows = request.windowsModel();
    engine::DesktopMediaRequest described;
    described.origin = canonicalSecurityOrigin(state_.url.empty()
        ? QUrl()
        : QUrl(QString::fromStdString(state_.url)));
    const auto collect = [&described](QAbstractListModel *model, bool window) {
        if (!model) return;
        for (int row = 0; row < model->rowCount(); ++row) {
            described.sources.push_back({
                .window = window,
                .index = row,
                .name = model->data(model->index(row, 0), Qt::DisplayRole).toString().toStdString(),
            });
        }
    };
    collect(screens, false);
    collect(windows, true);
    // Qt accepts exactly one answer per request. A second one would reach a
    // controller that has already been handed to Chromium.
    auto answered = std::make_shared<bool>(false);
    engine::DesktopMediaControls controls;
    controls.select = [request, screens, windows, answered](bool window, int index) {
        if (*answered) return false;
        QAbstractListModel *model = window ? windows : screens;
        if (!model || index < 0 || index >= model->rowCount())
            return false;
        *answered = true;
        if (window)
            request.selectWindow(model->index(index, 0));
        else
            request.selectScreen(model->index(index, 0));
        return true;
    };
    controls.cancel = [request, answered]() {
        if (*answered) return;
        *answered = true;
        request.cancel();
    };
    const engine::DesktopMediaHandler handler = desktopMediaHandler_;
    handler(described, controls);
    // A handler that showed nothing still has to end the request.
    if (!*answered) {
        *answered = true;
        request.cancel();
    }
}

void QtBrowserPage::handlePasskeyRequest(QWebEngineWebAuthUxRequest *request) {
    if (!request) return;
    // A prompt nobody can see must not sit there waiting for an answer.
    if (!passkeyHandler_) {
        request->cancel();
        return;
    }

    // The controls are guarded by a pointer that the request clears when it dies,
    // so a late answer from a dialog cannot reach a freed request.
    auto alive = std::make_shared<QPointer<QWebEngineWebAuthUxRequest>>(request);
    engine::PasskeyControls controls;
    controls.selectAccount = [alive](const std::string &account) {
        if (*alive) (*alive)->setSelectedAccount(QString::fromStdString(account));
    };
    controls.setPin = [alive](const std::string &pin) {
        if (*alive) (*alive)->setPin(QString::fromStdString(pin));
    };
    controls.retry = [alive]() {
        if (*alive) (*alive)->retry();
    };
    controls.cancel = [alive]() {
        if (*alive) (*alive)->cancel();
    };

    const engine::PasskeyHandler handler = passkeyHandler_;
    QObject::connect(request, &QWebEngineWebAuthUxRequest::stateChanged, request,
                     [alive, handler, controls](QWebEngineWebAuthUxRequest::WebAuthUxState) {
        if (!*alive) return;
        handler(describePasskey(*alive), controls);
    });
    handler(describePasskey(request), controls);
}

void QtBrowserPage::setAuthenticationChallengeHandler(
    engine::AuthenticationChallengeHandler handler
) {
    authenticationChallengeHandler_ = std::move(handler);
}

engine::NavigationToken QtBrowserPage::beginNavigation(std::function<void()> trigger) {
    std::optional<engine::NavigationResult> superseded;
    if (activeNavigationToken_ != 0) {
        const engine::NavigationToken previousToken = activeNavigationToken_;
        const std::uint64_t previousGeneration = activeNavigationGeneration_;
        const bool previousFinishWasAmbiguous =
            ambiguousFailedFinishGeneration_ == previousGeneration;
        retireLoadGeneration(previousGeneration);
        superseded = takeNavigationResult(
            previousToken,
            previousGeneration,
            previousFinishWasAmbiguous
                ? error(
                    engine::EngineErrorCode::navigationFailed,
                    "Chromium reported an ambiguous failed load before the navigation was superseded."
                )
                : error(
                    engine::EngineErrorCode::navigationSuperseded,
                    "Navigation was superseded by a newer operation."
                )
        );
    } else {
        if (signalLoadGeneration_ != 0)
            retireLoadGeneration(signalLoadGeneration_);
        if (pendingLoadGeneration_ != 0)
            retireLoadGeneration(pendingLoadGeneration_);
        signalLoadGeneration_ = 0;
        pendingLoadGeneration_ = 0;
        ambiguousFailedFinishGeneration_ = 0;
    }

    const engine::NavigationToken token = nextNavigationToken_++;
    const std::uint64_t navigationGeneration = nextNavigationGeneration_++;
    activeNavigationToken_ = token;
    activeNavigationGeneration_ = navigationGeneration;
    pendingLoadGeneration_ = navigationGeneration;
    signalLoadGeneration_ = 0;
    ambiguousFailedFinishGeneration_ = 0;

    // Install the newer identity before dispatching the superseded result.
    // A reentrant navigation from that callback is newer still and must win.
    if (superseded)
        dispatchNavigationResult(*superseded);
    if (activeNavigationToken_ != token
        || activeNavigationGeneration_ != navigationGeneration) {
        return token;
    }

    try {
        trigger();
    } catch (...) {
        if (activeNavigationToken_ == token
            && activeNavigationGeneration_ == navigationGeneration) {
            retireLoadGeneration(navigationGeneration);
            (void)takeNavigationResult(token, navigationGeneration, std::nullopt);
        }
        throw;
    }
    armNavigationWatchdog(token, navigationGeneration);
    return token;
}

engine::NavigationToken QtBrowserPage::navigate(std::string_view url) {
    const QString target = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    return beginNavigation([this, target] { view_->load(QUrl(target)); });
}

void QtBrowserPage::stop() {
    view_->stop();
}

engine::NavigationToken QtBrowserPage::reload() {
    return beginNavigation([this] { view_->reload(); });
}

engine::NavigationToken QtBrowserPage::goBack() {
    return beginNavigation([this] { view_->back(); });
}

engine::NavigationToken QtBrowserPage::goForward() {
    return beginNavigation([this] { view_->forward(); });
}

void QtBrowserPage::readAgentSnapshot(JsonCallback callback) {
    runAgentJavaScript(
        QStringLiteral("globalThis.__yobro ? globalThis.__yobro.snapshot() : null"),
        std::move(callback)
    );
}

void QtBrowserPage::performAgentAction(std::string_view requestJson, JsonCallback callback) {
    if (state_.owner != engine::PageOwner::agent) {
        callback({}, error(engine::EngineErrorCode::forbidden, "Agent actions are forbidden on user-owned pages."));
        return;
    }
    QJsonParseError parseError;
    const QByteArray bytes(requestJson.data(), static_cast<qsizetype>(requestJson.size()));
    const QJsonDocument request = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !request.isObject()) {
        callback({}, error(engine::EngineErrorCode::invalidRequest, "Agent action must be a JSON object."));
        return;
    }
    const QString payload = QString::fromUtf8(request.toJson(QJsonDocument::Compact));
    runAgentJavaScript(
        QStringLiteral("globalThis.__yobro ? globalThis.__yobro.act(%1) : null").arg(payload),
        std::move(callback)
    );
}

void QtBrowserPage::findText(std::string_view query, bool backwards, BoolCallback callback) {
    const QString text = QString::fromUtf8(query.data(), static_cast<qsizetype>(query.size()));
    const QWebEnginePage::FindFlags flags = backwards
        ? QWebEnginePage::FindFlags(QWebEnginePage::FindBackward)
        : QWebEnginePage::FindFlags();
    view_->page()->findText(text, flags, [callback = std::move(callback)](const QWebEngineFindTextResult &result) mutable {
        callback(result.numberOfMatches() > 0, std::nullopt);
    });
}

void QtBrowserPage::scrollBy(int amount, BoolCallback callback) {
    const QString script = QStringLiteral("window.scrollBy(0, %1); true").arg(amount);
    view_->page()->runJavaScript(
        script,
        QWebEngineScript::ApplicationWorld,
        [callback = std::move(callback)](const QVariant &value) mutable {
            if (!value.isValid()) {
                callback(false, error(engine::EngineErrorCode::operationFailed, "Chromium could not scroll the page."));
                return;
            }
            callback(value.toBool(), std::nullopt);
        }
    );
}

QWebEngineView *QtBrowserPage::view() const {
    return view_.get();
}

QWebEngineProfile *QtBrowserPage::profile() const {
    return view_->page()->profile();
}

std::optional<engine::PageOwner> QtBrowserPage::ownerForNativePage(const QWebEnginePage *page) {
    if (!page) return std::nullopt;
    const QVariant encoded = page->property(pageOwnerProperty);
    if (!encoded.isValid()) return std::nullopt;
    bool ok = false;
    const int value = encoded.toInt(&ok);
    if (!ok || value < static_cast<int>(engine::PageOwner::user)
        || value > static_cast<int>(engine::PageOwner::agent)) {
        return std::nullopt;
    }
    return static_cast<engine::PageOwner>(value);
}

void QtBrowserPage::setNativePageOwner(QWebEnginePage *page, engine::PageOwner owner) {
    if (page) page->setProperty(pageOwnerProperty, static_cast<int>(owner));
}

void QtBrowserPage::setHtml(const QString &html, const QUrl &baseUrl) {
    view_->setHtml(html, baseUrl);
}

void QtBrowserPage::readPlainText(std::function<void(QString)> callback) const {
    view_->page()->toPlainText(std::move(callback));
}

void QtBrowserPage::installAgentBridge() {
    QFile source(QStringLiteral(":/yobro/AgentBridge.js"));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        state_.error = "The shared AgentBridge.js resource is missing.";
        return;
    }
    QWebEngineScript bridge;
    bridge.setName(QStringLiteral("YOBRO AgentBridge v2"));
    bridge.setSourceCode(QString::fromUtf8(source.readAll()));
    bridge.setInjectionPoint(QWebEngineScript::DocumentReady);
    bridge.setWorldId(QWebEngineScript::ApplicationWorld);
    bridge.setRunsOnSubFrames(false);
    view_->page()->scripts().insert(bridge);
}

void QtBrowserPage::publishState() {
    const auto handlers = events_->handlers;
    for (const auto &[id, callbacks] : handlers) {
        (void)id;
        if (callbacks.stateChanged)
            callbacks.stateChanged(state_);
    }
}

std::optional<engine::NavigationResult> QtBrowserPage::takeNavigationResult(
    engine::NavigationToken token,
    std::uint64_t navigationGeneration,
    std::optional<engine::EngineError> navigationError
) {
    if (token == 0
        || activeNavigationToken_ != token
        || activeNavigationGeneration_ != navigationGeneration) {
        return std::nullopt;
    }

    engine::NavigationResult result{
        .token = token,
        .state = state_,
        .error = std::move(navigationError),
    };
    activeNavigationToken_ = 0;
    activeNavigationGeneration_ = 0;
    if (pendingLoadGeneration_ == navigationGeneration)
        pendingLoadGeneration_ = 0;
    if (signalLoadGeneration_ == navigationGeneration)
        signalLoadGeneration_ = 0;
    if (ambiguousFailedFinishGeneration_ == navigationGeneration)
        ambiguousFailedFinishGeneration_ = 0;
    return result;
}

void QtBrowserPage::dispatchNavigationResult(const engine::NavigationResult &result) {
    const auto handlers = events_->handlers;
    for (const auto &[id, callbacks] : handlers) {
        (void)id;
        if (!callbacks.navigationFinished)
            continue;
        try {
            callbacks.navigationFinished(result);
        } catch (...) {
            // Exceptions must not unwind through Qt signal dispatch.
        }
    }
}

bool QtBrowserPage::finishNavigation(
    engine::NavigationToken token,
    std::uint64_t navigationGeneration,
    std::optional<engine::EngineError> navigationError,
    bool publishFinalState
) {
    auto result = takeNavigationResult(
        token,
        navigationGeneration,
        std::move(navigationError)
    );
    if (!result)
        return false;

    // Clear the exact identity before any external callback can reenter and
    // start a newer navigation. The captured result remains immutable.
    if (publishFinalState)
        publishState();
    dispatchNavigationResult(*result);
    return true;
}

void QtBrowserPage::finishFailedNavigation(
    engine::NavigationToken token,
    std::uint64_t navigationGeneration
) {
    if (activeNavigationToken_ != token
        || activeNavigationGeneration_ != navigationGeneration
        || isRetiredLoadGeneration(navigationGeneration)) {
        return;
    }
    state_.loading = false;
    state_.error = "Chromium could not load the page.";
    state_.canGoBack = view_->history()->canGoBack();
    state_.canGoForward = view_->history()->canGoForward();
    (void)finishNavigation(
        token,
        navigationGeneration,
        error(engine::EngineErrorCode::navigationFailed, *state_.error),
        true
    );
}

void QtBrowserPage::armNavigationWatchdog(
    engine::NavigationToken token,
    std::uint64_t navigationGeneration
) {
    QTimer::singleShot(5'000, view_.get(), [this, token, navigationGeneration] {
        handleNavigationWatchdog(token, navigationGeneration);
    });
}

void QtBrowserPage::handleNavigationWatchdog(
    engine::NavigationToken token,
    std::uint64_t navigationGeneration
) {
    if (activeNavigationToken_ != token
        || activeNavigationGeneration_ != navigationGeneration
        || (pendingLoadGeneration_ != navigationGeneration
            && ambiguousFailedFinishGeneration_ != navigationGeneration)) {
        return;
    }

    retireLoadGeneration(navigationGeneration);
    state_.loading = false;
    state_.error = "Chromium did not start or finish the requested navigation.";
    state_.canGoBack = view_->history()->canGoBack();
    state_.canGoForward = view_->history()->canGoForward();
    (void)finishNavigation(
        token,
        navigationGeneration,
        error(engine::EngineErrorCode::navigationFailed, *state_.error),
        true
    );
}

void QtBrowserPage::retireLoadGeneration(std::uint64_t navigationGeneration) {
    if (navigationGeneration == 0 || isRetiredLoadGeneration(navigationGeneration))
        return;
    if (retiredLoadGenerations_.size() == maxRetiredLoadGenerations)
        retiredLoadGenerations_.erase(retiredLoadGenerations_.begin());
    retiredLoadGenerations_.push_back(navigationGeneration);
}

bool QtBrowserPage::consumeRetiredFailedFinish() {
    if (retiredLoadGenerations_.empty())
        return false;
    retiredLoadGenerations_.erase(retiredLoadGenerations_.begin());
    return true;
}

bool QtBrowserPage::isRetiredLoadGeneration(std::uint64_t navigationGeneration) const {
    return std::find(
        retiredLoadGenerations_.begin(),
        retiredLoadGenerations_.end(),
        navigationGeneration
    ) != retiredLoadGenerations_.end();
}

void QtBrowserPage::runAgentJavaScript(const QString &source, JsonCallback callback) {
    if (state_.owner != engine::PageOwner::agent) {
        callback({}, error(engine::EngineErrorCode::forbidden, "Agent snapshots are forbidden on user-owned pages."));
        return;
    }
    const QString wrapped = QStringLiteral(R"JS(
(function () {
  try {
    const value = (%1);
    return {ok: true, value};
  } catch (exception) {
    return {ok: false, error: String(exception && exception.message ? exception.message : exception)};
  }
})()
)JS").arg(source);
    view_->page()->runJavaScript(
        wrapped,
        QWebEngineScript::ApplicationWorld,
        [callback = std::move(callback)](const QVariant &raw) mutable {
            if (!raw.isValid() || raw.isNull()) {
                callback({}, error(
                    engine::EngineErrorCode::bridgeUnavailable,
                    "The isolated AgentBridge returned no value; the page may have changed."
                ));
                return;
            }
            const QJsonObject envelope = QJsonValue::fromVariant(raw).toObject();
            if (envelope.isEmpty() || !envelope.value(QStringLiteral("ok")).isBool()) {
                callback({}, error(
                    engine::EngineErrorCode::bridgeUnavailable,
                    "The isolated AgentBridge returned an invalid result."
                ));
                return;
            }
            if (!envelope.value(QStringLiteral("ok")).toBool()) {
                callback({}, error(
                    engine::EngineErrorCode::bridgeRejected,
                    envelope.value(QStringLiteral("error")).toString(QStringLiteral("AgentBridge rejected the operation.")).toStdString()
                ));
                return;
            }
            const QJsonValue value = envelope.value(QStringLiteral("value"));
            if (value.isNull() || value.isUndefined()) {
                callback({}, error(
                    engine::EngineErrorCode::bridgeUnavailable,
                    "The isolated AgentBridge returned no value; the page may have changed."
                ));
                return;
            }
            callback(compactJson(value.toVariant()), std::nullopt);
        }
    );
}

} // namespace yobro::qtwebengine
