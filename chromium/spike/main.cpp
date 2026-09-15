#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/PrivacySettings.hpp"
#include "spike/SmokeRunner.hpp"
#include "spike/SpaceProxyController.hpp"
#include "spike/SpaceProxyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/core/ProfilePaths.hpp"
#include "yobro/transport/LocalControlServer.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QMessageBox>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEngineProfile>

#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <optional>
#include <utility>

int main(int argc, char *argv[]) {
    // Declared before QApplication so the temporary smoke home is removed only
    // after Qt WebEngine and its helper processes have fully shut down.
    std::unique_ptr<QTemporaryDir> smokeHome;
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("yobro.bro"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Chromium Feasibility"));
    QCoreApplication::setApplicationVersion(QStringLiteral(YOBRO_CHROMIUM_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "YOBRO Chromium engine feasibility harness with Agent Protocol v2."
    ));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption profileOption(
        {QStringLiteral("p"), QStringLiteral("profile")},
        QStringLiteral("Use the isolated Chromium profile id."),
        QStringLiteral("id"),
        QStringLiteral("default")
    );
    QCommandLineOption urlOption(
        {QStringLiteral("u"), QStringLiteral("url")},
        QStringLiteral("Open an initial URL in the engineering shell."),
        QStringLiteral("url")
    );
    QCommandLineOption smokeOption(
        QStringLiteral("smoke"),
        QStringLiteral("Run deterministic Chromium/profile/AgentBridge checks and exit.")
    );
    QCommandLineOption packageProtocolSmokeOption(
        QStringLiteral("package-protocol-smoke"),
        QStringLiteral(
            "Run the normal packaged controller/socket path without mapping the window; "
            "exit when the validating parent closes stdin."
        )
    );
    parser.addOption(profileOption);
    parser.addOption(urlOption);
    parser.addOption(smokeOption);
    parser.addOption(packageProtocolSmokeOption);
    parser.process(application);

    try {
        const bool smoke = parser.isSet(smokeOption);
        const bool packageProtocolSmoke = parser.isSet(packageProtocolSmokeOption);
        if (smoke && packageProtocolSmoke)
            throw std::invalid_argument("--smoke and --package-protocol-smoke cannot be combined.");
        if (smoke) {
            smokeHome = std::make_unique<QTemporaryDir>();
            if (!smokeHome->isValid())
                throw std::runtime_error("Could not create the isolated Chromium smoke-test home.");
            qputenv("YOBRO_CHROMIUM_HOME", smokeHome->path().toUtf8());
        }

        std::string primaryId = smoke
            ? std::string("smoke-primary")
            : parser.value(profileOption).toStdString();
        // A profile switch rebuilds everything below for the requested profile,
        // because storage, agent socket and extensions belong to exactly one
        // profile. The window itself is recreated in the same application run.
        std::optional<std::string> requestedProfile;
        int result = 0;
        do {
        if (requestedProfile) {
            primaryId = *requestedProfile;
            requestedProfile.reset();
        }
        const auto paths = yobro::core::ProfilePaths::forProfile(primaryId);
        // Site data can only be removed while no profile has it open, so a
        // pending request from the last run is applied before the directories are
        // recreated and the profile opens.
        const std::size_t clearedSiteData = yobro::spike::PrivacySettings::applyPendingSiteDataClear(paths.profile);
        if (clearedSiteData > 0)
            qInfo("Removed %zu site-data directories requested in the previous run.", clearedSiteData);
        paths.createDirectories();
        // Qt WebEngine reads the proxy while it builds its network context, so
        // the active space's proxy has to be in place before the first page
        // exists. Switching it later has no effect; the window then asks for a
        // restart instead of loading unprotected.
        const yobro::spike::SpaceProxyStore spaceProxies(paths.profile);
        const QString proxySpace =
            yobro::spike::SpaceProxyController::spaceFromSession(paths.profile / "session.json");
        std::optional<yobro::spike::SpaceProxyConfig> startupProxy;
        if (!proxySpace.isEmpty()) {
            if (const auto stored = spaceProxies.config(proxySpace); stored && stored->enabled)
                startupProxy = spaceProxies.resolved(proxySpace, *stored);
        }
        if (const QString problem = yobro::spike::SpaceProxyController::apply(startupProxy);
            !problem.isEmpty()) {
            qWarning("The proxy for the last active space could not be applied: %s",
                     problem.toUtf8().constData());
        }

        const yobro::spike::PrivacySettings privacy(paths.profile);
        yobro::spike::BridgePolicyStore bridgePolicy(paths.bridgePolicy);
        const yobro::spike::BridgePolicyLoadResult bridgePolicyState = bridgePolicy.load();
        if (bridgePolicyState.problem)
            qWarning("%s", bridgePolicyState.problem->c_str());

        yobro::qtwebengine::QtBrowserEngine engine;
        yobro::engine::ProfileSpec spec{
            .id = primaryId,
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        };
        auto genericProfile = engine.openProfile(std::move(spec));
        auto *profile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(genericProfile.get());
        if (!profile)
            throw std::runtime_error("Qt WebEngine profile creation failed.");
        // The retention choice has to be applied before any page can set a cookie.
        profile->persistentProfile()->setPersistentCookiesPolicy(
            privacy.cookieRetention() == yobro::spike::CookieRetention::sessionOnly
                ? QWebEngineProfile::NoPersistentCookies
                : QWebEngineProfile::AllowPersistentCookies
        );

        if (smoke) {
            const auto secondaryPaths = yobro::core::ProfilePaths::forProfile("smoke-secondary");
            secondaryPaths.createDirectories();
            yobro::engine::ProfileSpec secondarySpec{
                .id = "smoke-secondary",
                .storagePath = secondaryPaths.storage.string(),
                .cachePath = secondaryPaths.cache.string(),
                .persistent = true,
            };
            auto genericSecondaryProfile = engine.openProfile(std::move(secondarySpec));
            auto *secondaryProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(genericSecondaryProfile.get());
            if (!secondaryProfile)
                throw std::runtime_error("Second Qt WebEngine profile creation failed.");

            yobro::spike::SmokeRunner runner(*profile, *secondaryProfile, [&application](int code) {
                application.exit(code);
            });
            QTimer::singleShot(0, &runner, [&runner] { runner.start(); });
            return application.exec();
        }

        yobro::qtwebengine::QtEventLoop eventLoop;
        const std::filesystem::path packageDownloadDirectory = packageProtocolSmoke
            ? paths.profile / "package-protocol-downloads"
            : std::filesystem::path{};
        auto library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(
            *profile,
            paths.profile,
            packageDownloadDirectory
        );
        yobro::controller::BrowserSession session(
            std::move(genericProfile),
            eventLoop,
            {
                .browser = "YoBro",
                .version = YOBRO_CHROMIUM_VERSION,
                .engine = "Chromium",
                .socketPath = paths.control.string(),
                .profileName = primaryId,
                .space = "Personal",
                .profileActive = true,
                .agentEnabled = true,
                .libraryAccess = bridgePolicyState.allowsLibraryAccess,
                .library = library,
            }
        );
        yobro::controller::ProtocolV2Controller controller(eventLoop, session);
        auto controlServer = yobro::transport::makeLocalControlServer({
            .path = paths.control,
            .maxRequestBytes = 1'048'576,
            .clientTimeout = std::chrono::seconds(10),
            .backlog = 8,
        });
        controlServer->start([&controller](
            std::string request,
            yobro::transport::LocalControlServer::ResponseCallback respond
        ) {
            controller.handleLine(std::move(request), std::move(respond));
        });

        std::unique_ptr<QSocketNotifier> validatingParent;
        {
            yobro::spike::SpikeWindow window(
                session,
                *library,
                bridgePolicy,
                paths,
                parser.value(urlOption),
                bridgePolicyState.problem
                    ? QString::fromStdString(*bridgePolicyState.problem)
                    : QString()
            );
            window.setPermissionSurfaceAllowed(!packageProtocolSmoke);
            window.setProfileSwitchHandler([&application, &requestedProfile](const std::string &profileId) {
                requestedProfile = profileId;
                // Leaving the event loop lets the scope below tear the session
                // down in the established order before the rebuild.
                application.quit();
            });
            if (packageProtocolSmoke) {
                // Keep the exact normal UI/session/controller object graph and
                // widget layout without presenting a validation window. stdin
                // is an inherited pipe; EOF means the external validator has
                // received the final protocol response and requests a clean
                // event-loop/server shutdown.
                window.setAttribute(Qt::WA_DontShowOnScreen);
                validatingParent = std::make_unique<QSocketNotifier>(
                    0,
                    QSocketNotifier::Read,
                    &application
                );
                QObject::connect(
                    validatingParent.get(),
                    &QSocketNotifier::activated,
                    &application,
                    [&application, notifier = validatingParent.get()] {
                        notifier->setEnabled(false);
                        application.quit();
                    }
                );
            }
            window.show();
            result = application.exec();
            library->shutdownDownloads();
            validatingParent.reset();
        }
        controlServer->stop();
        library.reset();
        } while (requestedProfile);
        return result;
    } catch (const std::exception &error) {
        if (parser.isSet(packageProtocolSmokeOption))
            qCritical("CHROMIUM PACKAGED PROTOCOL FAIL: %s", error.what());
        else if (parser.isSet(smokeOption))
            qCritical("CHROMIUM SMOKE FAIL: %s", error.what());
        else
            QMessageBox::critical(nullptr, QStringLiteral("YOBRO Chromium Feasibility"), QString::fromUtf8(error.what()));
        return 1;
    }
}
