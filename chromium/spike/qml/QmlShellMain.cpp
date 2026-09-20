#include "spike/Localization.hpp"
#include "spike/PrivacySettings.hpp"
#include "spike/SpaceProxyController.hpp"
#include "spike/SpaceProxyStore.hpp"
#include "spike/Theme.hpp"
#include "spike/qml/QmlBrowserModel.hpp"
#include "spike/qml/QmlImageProviders.hpp"
#include "spike/qml/QmlShellSupport.hpp"
#include "spike/qml/QmlTheme.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickWebEngineProfile>
#include <QStyleHints>
#include <QFont>
#include <QTimer>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "yobro/core/ProfilePaths.hpp"

#include <exception>
#include <filesystem>
#include <optional>

int main(int argc, char *argv[]) {
    // Qt WebEngine requires initialization before the QGuiApplication exists.
    QtWebEngineQuick::initialize();
    // The shell customizes its controls; the native macOS style forbids that.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("yobro.bro"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Chromium QML Shell"));
    // The shell speaks the WebKit typography: SF Pro as the default family.
    application.setFont(QFont(QStringLiteral(".AppleSystemUIFont")));
    // The theme layer is Qt-Core-only; the system appearance probe lives here.
    yobro::spike::setSystemDarkProbe([] {
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    });

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "YOBRO Chromium QML shell — the Qt Quick front end over the shared engine core."
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
        QStringLiteral("Open an initial URL."),
        QStringLiteral("url")
    );
    QCommandLineOption captureOption(
        QStringLiteral("capture"),
        QStringLiteral("Show the shell at the reference window size, save a PNG to <file> and exit."),
        QStringLiteral("file")
    );
    parser.addOption(profileOption);
    parser.addOption(urlOption);
    parser.addOption(captureOption);
    parser.process(application);
    const bool capture = parser.isSet(captureOption);

    const auto paths = yobro::core::ProfilePaths::forProfile(parser.value(profileOption).toStdString());
    // Site data can only be removed while no profile has it open, so a pending
    // request from the last run is applied before the directories are created.
    const std::size_t clearedSiteData = yobro::spike::PrivacySettings::applyPendingSiteDataClear(paths.profile);
    if (clearedSiteData > 0)
        qInfo("Removed %zu site-data directories requested in the previous run.", clearedSiteData);
    paths.createDirectories();
    // Qt WebEngine reads the proxy while it builds its network context, so the
    // active space's proxy has to be in place before the first page exists.
    // Without this check the shell would load unprotected behind the user's
    // back — the same fail-closed rule the widget shell follows.
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

    // A real persistent profile at the shared storage location — the same
    // directory the widget shell and engine adapter use.
    auto *webProfile = new QQuickWebEngineProfile(&application);
    webProfile->setPersistentStoragePath(QString::fromStdString(paths.storage.string()));
    webProfile->setCachePath(QString::fromStdString(paths.cache.string()));
    webProfile->setPersistentCookiesPolicy(
        privacy.cookieRetention() == yobro::spike::CookieRetention::sessionOnly
            ? QQuickWebEngineProfile::NoPersistentCookies
            : QQuickWebEngineProfile::AllowPersistentCookies
    );

    yobro::spike::QmlTheme theme;
    yobro::spike::QmlBrowserModel browser(parser.value(urlOption));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("yobroicon"), new yobro::spike::QmlIconProvider());
    engine.addImageProvider(QStringLiteral("yobroglow"), new yobro::spike::QmlGlowProvider());
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("Browser"), &browser);
    engine.rootContext()->setContextProperty(QStringLiteral("WebProfile"), webProfile);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &application,
        [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection
    );
    engine.loadFromModule(QStringLiteral("Yobro.Shell"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) {
        qCritical("The QML shell root is not a window.");
        return 1;
    }

    if (capture) {
        // The reference situation: the exact WebKit capture window format at
        // this scale factor, settled for one second so fonts, icons and the
        // profile-backed page are done.
        window->resize(1360, 808);
        window->show();
        const QString captureFile = parser.value(captureOption);
        QTimer::singleShot(1000, &application, [window, captureFile] {
            const QImage shot = window->grabWindow();
            if (shot.isNull() || !shot.save(captureFile)) {
                qCritical("CHROMIUM QML CAPTURE FAIL: could not write %s",
                          captureFile.toUtf8().constData());
                QCoreApplication::exit(1);
                return;
            }
            QCoreApplication::exit(0);
        });
    } else {
        yobro::spike::configureQmlMacWindowFrame(window);
        window->show();
    }
    return application.exec();
}
