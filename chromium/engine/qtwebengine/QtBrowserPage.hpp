#pragma once

#include "engine/api/BrowserEngine.hpp"

#include <QUrl>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QWebEngineDesktopMediaRequest;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;
class QWebEngineWebAuthUxRequest;

namespace yobro::qtwebengine {

class QtBrowserPageTestPeer;

class QtBrowserPage final : public engine::BrowserPage {
public:
    QtBrowserPage(
        QWebEngineProfile *profile,
        std::string id,
        engine::PageOwner owner
    );
    ~QtBrowserPage() override;

    [[nodiscard]] engine::PageState state() const override;
    [[nodiscard]] std::unique_ptr<engine::PageEventSubscription> subscribe(
        engine::PageEventHandlers handlers
    ) override;
    void setPermissionRequestHandler(engine::PermissionRequestHandler handler) override;
    void setAuthenticationChallengeHandler(engine::AuthenticationChallengeHandler handler) override;
    void setCertificateProblemHandler(engine::CertificateProblemHandler handler) override;
    void setDesktopMediaHandler(engine::DesktopMediaHandler handler) override;
    void setPasskeyHandler(engine::PasskeyHandler handler) override;
    [[nodiscard]] engine::NavigationToken navigate(std::string_view url) override;
    void stop() override;
    [[nodiscard]] engine::NavigationToken reload() override;
    [[nodiscard]] engine::NavigationToken goBack() override;
    [[nodiscard]] engine::NavigationToken goForward() override;
    void readAgentSnapshot(JsonCallback callback) override;
    void performAgentAction(std::string_view requestJson, JsonCallback callback) override;
    void findText(std::string_view query, bool backwards, BoolCallback callback) override;
    void scrollBy(int amount, BoolCallback callback) override;

    [[nodiscard]] QWebEngineView *view() const;
    [[nodiscard]] QWebEngineProfile *profile() const;
    [[nodiscard]] static std::optional<engine::PageOwner> ownerForNativePage(
        const QWebEnginePage *page
    );
    static void setNativePageOwner(QWebEnginePage *page, engine::PageOwner owner);
    void setHtml(const QString &html, const QUrl &baseUrl = {});
    void readPlainText(std::function<void(QString)> callback) const;

private:
    friend class QtBrowserPageTestPeer;

    struct EventRegistry;
    class Subscription;

    static constexpr std::size_t maxRetiredLoadGenerations = 8;

    void installAgentBridge();
    void publishState();
    void handleLoadStarted();
    void handleLoadFinished(bool ok);
    void handleRendererTerminated(
        int nativeStatus,
        int exitCode
    );
    void handleNavigationWatchdog(
        engine::NavigationToken token,
        std::uint64_t navigationGeneration
    );
    [[nodiscard]] std::optional<engine::NavigationResult> takeNavigationResult(
        engine::NavigationToken token,
        std::uint64_t navigationGeneration,
        std::optional<engine::EngineError> error
    );
    void dispatchNavigationResult(const engine::NavigationResult &result);
    bool finishNavigation(
        engine::NavigationToken token,
        std::uint64_t navigationGeneration,
        std::optional<engine::EngineError> error = std::nullopt,
        bool publishFinalState = false
    );
    void finishFailedNavigation(
        engine::NavigationToken token,
        std::uint64_t navigationGeneration
    );
    void armNavigationWatchdog(
        engine::NavigationToken token,
        std::uint64_t navigationGeneration
    );
    void retireLoadGeneration(std::uint64_t navigationGeneration);
    [[nodiscard]] bool consumeRetiredFailedFinish();
    [[nodiscard]] bool isRetiredLoadGeneration(std::uint64_t navigationGeneration) const;
    [[nodiscard]] engine::NavigationToken beginNavigation(std::function<void()> trigger);
    void runAgentJavaScript(const QString &source, JsonCallback callback);

    std::unique_ptr<QWebEngineView> view_;
    engine::PageState state_;
    std::shared_ptr<EventRegistry> events_;
    engine::PermissionRequestHandler permissionRequestHandler_;
    engine::AuthenticationChallengeHandler authenticationChallengeHandler_;
    engine::CertificateProblemHandler certificateProblemHandler_;
    engine::DesktopMediaHandler desktopMediaHandler_;
    engine::PasskeyHandler passkeyHandler_;
    void handleDesktopMediaRequest(const QWebEngineDesktopMediaRequest &request);
    void handlePasskeyRequest(QWebEngineWebAuthUxRequest *request);
    engine::NavigationToken nextNavigationToken_ = 1;
    engine::NavigationToken activeNavigationToken_ = 0;
    std::uint64_t nextNavigationGeneration_ = 1;
    std::uint64_t activeNavigationGeneration_ = 0;
    std::uint64_t pendingLoadGeneration_ = 0;
    std::uint64_t signalLoadGeneration_ = 0;
    std::uint64_t ambiguousFailedFinishGeneration_ = 0;
    std::vector<std::uint64_t> retiredLoadGenerations_;
};

} // namespace yobro::qtwebengine
