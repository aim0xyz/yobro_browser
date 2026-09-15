// Verifies the parts of Agent Protocol v2 that exist so the user can audit the
// agent instead of trusting it: the recorded event log, its bound size, the
// commands that deliberately record nothing, the start-page guard for actions,
// and the development-only window capture. Runs the real controller against a
// real session and a real window; no stub protocol host.
#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpaceChat.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/core/Json.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using yobro::controller::AgentEvent;
using yobro::core::Json;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

/// Serves one small HTML document so `new`, `read` and `close` can log a real
/// host instead of a title.
void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        QTcpSocket *socket = server.nextPendingConnection();
        if (!socket) return;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            const QByteArray head = socket->readAll();
            if (!head.contains("\r\n\r\n")) return;
            const QByteArray body =
                QByteArrayLiteral("<!doctype html><title>Log Fixture</title>"
                                  "<p id=\"note\">Agent log fixture</p>"
                                  "<input id=\"field\"><button id=\"apply\">Apply</button>");
            QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n");
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
            response += body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
}

struct LogApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;
    std::unique_ptr<yobro::controller::ProtocolV2Controller> controller;

    LogApp() : paths(yobro::core::ProfilePaths::forProfile("agent-log")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        // Leftovers from an earlier run would change the recorded counts. Only
        // these named files are removed, never a directory.
        QFile::remove(QString::fromStdString((paths.profile / "space-chat.json").string()));
        QFile::remove(QString::fromStdString((paths.profile / "window.png").string()));
        QFile::remove(QString::fromStdString(paths.session.string()));
        profile = engine.openProfile({
            .id = "agent-log",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the agent-log test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "agent-log",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = true, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->resize(900, 600);
        window->show();
        controller = std::make_unique<yobro::controller::ProtocolV2Controller>(eventLoop, *session);
    }
    ~LogApp() {
        controller.reset();
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }

    /// Runs one command through the real controller and pumps the Qt loop until
    /// the response arrives, exactly like a socket client would observe it.
    Json call(Json::Object request) {
        std::optional<std::string> encoded;
        controller->handleLine(
            Json(std::move(request)).serialize(),
            [&encoded](std::string response) { encoded = std::move(response); }
        );
        QElapsedTimer timer;
        timer.start();
        while (!encoded && timer.elapsed() < 30'000)
            QTest::qWait(10);
        check(encoded.has_value(), "A protocol command never answered.");
        return Json::parse(*encoded);
    }
    Json result(Json::Object request) {
        const Json response = call(std::move(request));
        check(response.isObject(), "Protocol response is not an object.");
        const Json *error = response.find("error");
        const Json *ok = response.find("ok");
        check(
            ok && ok->asBoolean(),
            "Expected a successful command but got: "
                + std::string(error && error->isString() ? error->asString() : "no error field")
        );
        const Json *value = response.find("result");
        check(value != nullptr, "A successful response carries no result.");
        return *value;
    }
    std::string failure(Json::Object request) {
        const Json response = call(std::move(request));
        check(response.isObject(), "Protocol response is not an object.");
        const Json *ok = response.find("ok");
        check(ok && !ok->asBoolean(), "Expected a failing command but it succeeded.");
        const Json *error = response.find("error");
        check(error && error->isString(), "Protocol failure carries no error text.");
        return error->asString();
    }
};

const Json &required(const Json &value, const std::string &key) {
    check(value.isObject(), "Expected an object while reading '" + key + "'.");
    const Json *found = value.find(key);
    check(found != nullptr, "Protocol result is missing '" + key + "'.");
    return *found;
}

QStringList activityRows(const SpikeWindow &window) {
    QStringList rows;
    auto *list = window.findChild<QListWidget *>(QStringLiteral("agentActivityList"));
    check(list != nullptr, "The window has no agentActivityList.");
    for (int index = 0; index < list->count(); ++index)
        rows.append(list->item(index)->text());
    return rows;
}

QStringList chatActions(SpikeWindow &window, const QString &space) {
    QStringList texts;
    for (const yobro::spike::SpaceChatEntry &entry : window.assistantStore().entries(space)) {
        if (entry.kind == QStringLiteral("action"))
            texts.append(entry.text);
    }
    return texts;
}

void runChecks(LogApp &app, const std::string &fixtureUrl) {
    check(app.session->agentEvents().empty(), "A fresh session already had agent events.");
    check(activityRows(*app.window).isEmpty(), "A fresh window already listed agent activity.");

    // Neither the read-only introspection commands nor the pane teardown are
    // agent actions in the WebKit build, so none of them may appear in the log.
    (void)app.result({{"command", Json("status")}});
    (void)app.result({{"command", Json("tabs")}});
    (void)app.result({{"command", Json("history")}});
    (void)app.result({{"command", Json("downloads")}});
    (void)app.result({{"command", Json("split")}});
    check(app.session->agentEvents().empty(), "status/tabs/history/downloads/split must not be logged.");

    // An empty agent tab has no host, so the log falls back the same way the
    // WebKit build does.
    const Json emptyTab = app.result({{"command", Json("new")}});
    const std::string emptyTabId = required(emptyTab, "id").asString();
    std::vector<AgentEvent> events = app.session->agentEvents();
    check(events.size() == 1, "The new command was not logged exactly once.");
    check(events.front().action == "new", "Logged action for new differs.");
    check(events.front().detail == "Neuer Tab", "An empty new tab did not fall back to the placeholder detail.");
    check(events.front().space == "Personal", "new was not attributed to the session space.");
    check(
        activityRows(*app.window) == QStringList{QStringLiteral("new: Neuer Tab · Personal")},
        "The activity list does not show the logged command."
    );
    check(
        chatActions(*app.window, QStringLiteral("Personal")) == QStringList{QStringLiteral("new: Neuer Tab")},
        "The assistant timeline did not receive the action entry."
    );

    // pin has no host either, so it reports the tab title.
    (void)app.result({{"command", Json("pin")}, {"tab", Json(emptyTabId)}});
    events = app.session->agentEvents();
    check(events.size() == 2 && events.front().action == "pin", "pin was not logged.");
    check(events.front().detail == "Agent page", "pin did not fall back to the tab title.");

    // Actions are refused on the start page, like prepare() in the WebKit build.
    check(
        app.failure({
            {"command", Json("fill")}, {"tab", Json(emptyTabId)},
            {"document", Json("doc")}, {"ref", Json("ref")}, {"value", Json("text")},
        }) == "Dieser Tab zeigt die Startseite. Zuerst eine URL öffnen.",
        "fill did not refuse a tab that shows the start page."
    );
    check(
        app.failure({
            {"command", Json("click")}, {"tab", Json(emptyTabId)},
            {"document", Json("doc")}, {"ref", Json("ref")},
        }) == "Dieser Tab zeigt die Startseite. Zuerst eine URL öffnen.",
        "click did not refuse a tab that shows the start page."
    );
    check(app.session->agentEvents().size() == 2, "A refused command was logged.");

    // A rejected navigation must not be logged either.
    (void)app.failure({{"command", Json("open")}, {"tab", Json(emptyTabId)}, {"url", Json("ftp://example.com")}});
    check(app.session->agentEvents().size() == 2, "A refused open was logged.");

    // With a real page the detail is the host, and read is attributed to the
    // tab's own space.
    const Json loaded = app.result({{"command", Json("new")}, {"url", Json(fixtureUrl)}});
    const std::string loadedTabId = required(loaded, "id").asString();
    events = app.session->agentEvents();
    check(events.size() == 3 && events.front().action == "new", "new with a URL was not logged.");
    check(events.front().detail == "127.0.0.1", "new did not log the host of the loaded page.");
    (void)app.result({{"command", Json("read")}, {"tab", Json(loadedTabId)}});
    events = app.session->agentEvents();
    check(events.size() == 4 && events.front().action == "read", "read was not logged.");
    check(events.front().detail == "127.0.0.1", "read did not log the host.");
    check(events.front().space == "Personal", "read was not attributed to the tab space.");

    // click and fill log the reference together with the host.
    const Json page = required(app.result({{"command", Json("read")}, {"tab", Json(loadedTabId)}}), "page");
    const std::string document = required(page, "document").asString();
    std::string inputRef;
    for (const Json &element : required(page, "elements").asArray()) {
        if (!element.isObject()) continue;
        const Json *tag = element.find("tag");
        const Json *reference = element.find("ref");
        if (!tag || !tag->isString() || !reference || !reference->isString()) continue;
        if (tag->asString() == "input") inputRef = reference->asString();
    }
    check(!inputRef.empty(), "The fixture snapshot exposed no input reference.");
    (void)app.result({
        {"command", Json("fill")}, {"tab", Json(loadedTabId)},
        {"document", Json(document)}, {"ref", Json(inputRef)}, {"value", Json("logged")},
    });
    events = app.session->agentEvents();
    check(events.front().action == "fill", "fill was not logged.");
    check(events.front().detail == inputRef + " · 127.0.0.1", "fill did not log the reference and host.");

    // duplicate reports the title of the source tab, not of the copy.
    const Json copy = app.result({{"command", Json("duplicate")}, {"tab", Json(loadedTabId)}});
    check(required(copy, "id").asString() != loadedTabId, "duplicate returned the source tab.");
    events = app.session->agentEvents();
    check(events.front().action == "duplicate", "duplicate was not logged.");
    check(events.front().detail == "Log Fixture", "duplicate did not log the source title.");

    // close still logs the page it closed, although the tab is already gone
    // when the log entry is written.
    (void)app.result({{"command", Json("close")}, {"tab", Json(loadedTabId)}});
    events = app.session->agentEvents();
    check(events.front().action == "close", "close was not logged.");
    check(events.front().detail == "127.0.0.1", "close did not log the host of the closed page.");

    // The development capture stays unavailable without the environment opt-in,
    // and reports the same unknown-command error as any other invalid command.
    check(qEnvironmentVariableIsEmpty("YOBRO_DEV_CAPTURE"), "The capture opt-in leaked into the test environment.");
    check(
        app.failure({{"command", Json("capture-window")}}) == "Unbekannter Befehl: capture-window",
        "capture-window was not hidden without the environment opt-in."
    );
    qputenv("YOBRO_DEV_CAPTURE", "1");
    const std::size_t beforeCapture = app.session->agentEvents().size();
    const Json captured = app.result({{"command", Json("capture-window")}});
    const std::string capturePath = required(captured, "path").asString();
    check(
        capturePath == (app.paths.profile / "window.png").string(),
        "capture-window wrote outside the profile directory."
    );
    check(!QImage(QString::fromStdString(capturePath)).isNull(), "capture-window did not write a readable image.");
    check(app.session->agentEvents().size() == beforeCapture, "capture-window was written to the agent log.");
    qunsetenv("YOBRO_DEV_CAPTURE");
    check(
        app.failure({{"command", Json("capture-window")}}) == "Unbekannter Befehl: capture-window",
        "capture-window stayed reachable after the opt-in was removed."
    );

    // The log is bounded, newest first, like the 50-entry list in the WebKit build.
    for (int index = 0; index < 60; ++index)
        (void)app.result({{"command", Json("focus")}, {"tab", Json(emptyTabId)}});
    events = app.session->agentEvents();
    check(events.size() == 50, "The agent log is not bounded to 50 entries.");
    for (const AgentEvent &event : events)
        check(event.action == "focus", "Older entries were not evicted from the agent log.");
    check(activityRows(*app.window).size() == 50, "The activity list does not follow the bounded log.");

    // end tears the pane down and is not an agent action.
    const std::size_t beforeEnd = app.session->agentEvents().size();
    check(required(app.result({{"command", Json("end")}}), "ended").asBoolean(), "end did not report success.");
    check(app.session->agentEvents().size() == beforeEnd, "end was written to the agent log.");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-agent-log-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT AGENT LOG FAIL: Could not create an isolated home.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    qunsetenv("YOBRO_DEV_CAPTURE");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Agent Log"));
    try {
        QTcpServer fixture;
        check(fixture.listen(QHostAddress::LocalHost, 0), "Could not start the loopback fixture server.");
        serveFixture(fixture);
        const std::string fixtureUrl =
            QStringLiteral("http://127.0.0.1:%1/fixture").arg(fixture.serverPort()).toStdString();
        LogApp app;
        runChecks(app, fixtureUrl);
        std::cout << "QT AGENT LOG PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT AGENT LOG FAIL: " << error.what() << '\n';
        return 1;
    }
}
