#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/WebAppearanceStore.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr int appearanceWorld = QWebEngineScript::UserWorld + 1;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

QString readResource(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly | QIODevice::Text), "A shared appearance script is missing.");
    return QString::fromUtf8(file.readAll());
}

/// Installs the same engine the shell installs, with the supplied settings.
void install(QWebEngineProfile &profile, const std::string &json) {
    auto *scripts = profile.scripts();
    const QString name = QStringLiteral("YOBRO WebAppearance");
    for (const QWebEngineScript &existing : scripts->find(name)) scripts->remove(existing);
    QWebEngineScript script;
    script.setName(name);
    script.setSourceCode(
        readResource(QStringLiteral(":/yobro/DarkReader.js")) + QStringLiteral("\n")
        + readResource(QStringLiteral(":/yobro/WebAppearance.js")) + QStringLiteral("\n")
        + QStringLiteral("window.__yobroAppearance?.configure(%1);").arg(QString::fromStdString(json))
    );
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setWorldId(appearanceWorld);
    script.setRunsOnSubFrames(true);
    scripts->insert(script);
}

QVariant evaluate(QWebEngineView &view, const QString &script) {
    std::optional<QVariant> result;
    view.page()->runJavaScript(script, appearanceWorld, [&result](const QVariant &value) { result = value; });
    QElapsedTimer timer;
    timer.start();
    while (!result && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
    check(result.has_value(), "The appearance world did not answer.");
    return *result;
}

void load(QWebEngineView &view, const QString &html, const QUrl &base) {
    bool finished = false;
    QObject::connect(&view, &QWebEngineView::loadFinished, &view, [&finished](bool) { finished = true; });
    view.setHtml(html, base);
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
    check(finished, "A fixture page did not finish loading.");
    // The engine settles asynchronously after DocumentReady.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 900) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
}

QString stateOf(QWebEngineView &view) {
    return evaluate(view, QStringLiteral("window.__yobroAppearance?.status().state ?? 'missing'")).toString();
}

const char *lightPage =
    "<!doctype html><html><head><title>Hell</title></head>"
    "<body style='background:#ffffff;color:#111111'>"
    "<div style='background:#ffffff;height:100vh'>Helle Seite</div></body></html>";

const char *darkPage =
    "<!doctype html><html><head><title>Dunkel</title></head>"
    "<body style='background:#101010;color:#f0f0f0'>"
    "<div style='background:#101010;height:100vh'>Dunkle Seite</div></body></html>";

} // namespace

int main(int argc, char *argv[]) {
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const std::filesystem::path rootPath = root.path().toStdString();
    qputenv("YOBRO_CHROMIUM_HOME", QString::fromStdString((rootPath / "chromium").string()).toUtf8());
    qputenv("YOBRO_WEBKIT_HOME", QString::fromStdString((rootPath / "webkit").string()).toUtf8());
    QApplication application(argc, argv);

    try {
        // The store keeps the settings and produces the payload for the script.
        {
            yobro::spike::WebAppearanceStore store(rootPath / "store");
            check(!store.enabled(), "Darkening must be off by default.");
            store.setEnabled(true);
            store.setExcluded("Excluded.Example.Test", true);
            check(store.isExcluded("excluded.example.test"), "Host exclusion must ignore letter case.");
            check(!store.isExcluded("other.example.test"), "An unrelated host must not be excluded.");
            const QJsonObject payload = QJsonDocument::fromJson(
                QByteArray::fromStdString(store.json(true))
            ).object();
            check(payload.value(QStringLiteral("enabled")).toBool(), "The payload lost the enabled flag.");
            check(payload.value(QStringLiteral("systemDark")).toBool(), "The payload lost the system mode.");
        }
        {
            // Settings must survive a restart of the profile.
            yobro::spike::WebAppearanceStore reopened(rootPath / "store");
            check(reopened.enabled(), "The enabled flag was not persisted.");
            check(reopened.isExcluded("excluded.example.test"), "The exclusion was not persisted.");
            reopened.setExcluded("excluded.example.test", false);
            check(!reopened.isExcluded("excluded.example.test"), "Removing an exclusion failed.");
        }

        QWebEngineProfile profile(QStringLiteral("appearance-test"));
        QWebEngineView view(&profile);
        view.resize(900, 700);
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.show();

        // Off by default: the page keeps its own colours.
        install(profile, R"JSON({"enabled":false,"excludedHosts":[],"systemDark":true})JSON");
        load(view, QString::fromLatin1(lightPage), QUrl(QStringLiteral("https://light.example.test/")));
        check(stateOf(view) == QStringLiteral("original"),
              "A light page was converted although darkening is disabled.");

        // Enabled while the system is light must also leave the page alone.
        install(profile, R"JSON({"enabled":true,"excludedHosts":[],"systemDark":false})JSON");
        load(view, QString::fromLatin1(lightPage), QUrl(QStringLiteral("https://light.example.test/")));
        check(stateOf(view) == QStringLiteral("original"),
              "A light page was converted although the system is in light mode.");

        // Enabled and the system is dark: the light page is converted.
        install(profile, R"JSON({"enabled":true,"excludedHosts":[],"systemDark":true})JSON");
        load(view, QString::fromLatin1(lightPage), QUrl(QStringLiteral("https://light.example.test/")));
        check(stateOf(view) == QStringLiteral("converted"),
              "A light page was not darkened although it should be.");

        // A page that is already dark keeps its own colours.
        load(view, QString::fromLatin1(darkPage), QUrl(QStringLiteral("https://dark.example.test/")));
        check(stateOf(view) == QStringLiteral("native"),
              "An already dark page must not be converted again.");

        // An excluded host stays untouched.
        install(profile, R"JSON({"enabled":true,"excludedHosts":["light.example.test"],"systemDark":true})JSON");
        load(view, QString::fromLatin1(lightPage), QUrl(QStringLiteral("https://light.example.test/")));
        check(stateOf(view) == QStringLiteral("original"),
              "An excluded host was darkened anyway.");

        // The page world must not see the engine.
        install(profile, R"JSON({"enabled":true,"excludedHosts":[],"systemDark":true})JSON");
        load(view, QString::fromLatin1(lightPage), QUrl(QStringLiteral("https://light.example.test/")));
        std::optional<QVariant> pageWorld;
        view.page()->runJavaScript(
            QStringLiteral("typeof window.__yobroAppearance"),
            QWebEngineScript::MainWorld,
            [&pageWorld](const QVariant &value) { pageWorld = value; }
        );
        QElapsedTimer timer;
        timer.start();
        while (!pageWorld && timer.elapsed() < 10'000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            QTest::qWait(10);
        }
        check(pageWorld.has_value() && pageWorld->toString() == QStringLiteral("undefined"),
              "Page scripts must not be able to reach the appearance engine.");
    } catch (const std::exception &error) {
        std::cerr << "QT WEB APPEARANCE FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "QT WEB APPEARANCE PASS\n";
    return 0;
}
