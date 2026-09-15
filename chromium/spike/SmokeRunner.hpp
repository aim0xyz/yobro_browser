#pragma once

#include "engine/api/BrowserEngine.hpp"

#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <string>

class QTcpServer;
class QTcpSocket;

namespace yobro::qtwebengine {
class QtBrowserPage;
class QtBrowserProfile;
}

namespace yobro::spike {

class SmokeRunner final : public QObject {
public:
    using Completion = std::function<void(int)>;

    SmokeRunner(
        qtwebengine::QtBrowserProfile &primaryProfile,
        qtwebengine::QtBrowserProfile &secondaryProfile,
        Completion completion
    );
    ~SmokeRunner() override;
    void start();

private:
    static constexpr int repeatedJourneyCount = 100;

    bool startFixtureServer();
    void respondToFixtureRequest(QTcpSocket *socket);
    [[nodiscard]] QUrl fixtureUrl(const QString &path) const;
    void loadHtml(
        qtwebengine::QtBrowserPage &page,
        const QString &html,
        const QUrl &baseUrl,
        std::function<void(bool)> completion
    );
    void waitForLoad(
        qtwebengine::QtBrowserPage &page,
        const QUrl &expectedUrl,
        std::function<void()> trigger,
        std::function<void(bool)> completion
    );

    void verifyUserOwnershipIsolation();
    void loadCookieOwner();
    void loadAgentFixture();
    void verifyInitialSnapshot(std::string json, std::optional<engine::EngineError> error);
    void verifyFilledSnapshot(std::string json, std::optional<engine::EngineError> error);
    void verifyFileUploadRejected();
    void verifyNewDocument(std::string json, std::optional<engine::EngineError> error);
    void verifyPrivateIsolation();
    void verifySecondProfileIsolation();

    void runRepeatedJourney();
    void verifyJourneySnapshot(std::string json, std::optional<engine::EngineError> error);
    void verifyJourneyResult(std::string json, std::optional<engine::EngineError> error);

    void runLifecycleNavigation();
    void navigateLifecycleA();
    void navigateLifecycleB();
    void goBackLifecycleA();
    void goForwardLifecycleB();
    void reloadLifecycleB();
    void followLifecycleRedirect();

    void fail(const QString &message);
    void succeed();
    void finish(int code);

    qtwebengine::QtBrowserProfile &primaryProfile_;
    qtwebengine::QtBrowserProfile &secondaryProfile_;
    Completion completion_;
    std::unique_ptr<QTcpServer> fixtureServer_;
    QUrl fixtureBaseUrl_;
    std::unique_ptr<qtwebengine::QtBrowserPage> userPage_;
    std::unique_ptr<qtwebengine::QtBrowserPage> agentPage_;
    std::unique_ptr<qtwebengine::QtBrowserPage> privatePage_;
    std::unique_ptr<qtwebengine::QtBrowserPage> secondaryPage_;
    std::unique_ptr<qtwebengine::QtBrowserPage> lifecyclePage_;
    QString documentId_;
    QString fieldRef_;
    QString buttonRef_;
    QString fileRef_;
    QString previousJourneyDocumentId_;
    int journeyIteration_ = 0;
    bool finished_ = false;
};

} // namespace yobro::spike
