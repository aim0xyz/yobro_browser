#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "yobro/controller/BrowserSession.hpp"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QPoint>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

const QByteArray parentBody = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Popup Parent</title>
<style>
html, body { margin: 0; width: 100%; height: 100%; background: #fff; }
button { position: fixed; left: 40px; top: 40px; width: 240px; height: 80px; font: 20px sans-serif; }
#status { position: fixed; left: 40px; top: 150px; font: 18px sans-serif; }
</style>
<button id="open" type="button">Open child</button>
<div id="status">waiting</div>
<script>
globalThis.__popupAttempts = 0;
globalThis.__popupReady = false;
globalThis.__openedPopup = null;
addEventListener("message", event => {
  if (event.origin === location.origin
      && event.source === globalThis.__openedPopup
      && event.data === "popup-ready") {
    globalThis.__popupReady = true;
    document.querySelector("#status").textContent = "opener confirmed";
  }
});
document.querySelector("#open").addEventListener("click", () => {
  globalThis.__popupAttempts += 1;
  const caseName = new URLSearchParams(location.search).get("case") || "unknown";
  globalThis.__openedPopup = window.open(
    "/popup-child?case=" + encodeURIComponent(caseName),
    "phase1d-" + caseName + "-" + globalThis.__popupAttempts,
    "width=420,height=320"
  );
});
</script>)HTML");

const QByteArray childBody = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Popup Child</title>
<style>
html, body { margin: 0; width: 100%; height: 100%; background: #fff; }
button { position: fixed; left: 40px; top: 40px; width: 240px; height: 80px; font: 20px sans-serif; }
</style>
<button id="close" type="button">Close child</button>
<script>
addEventListener("load", () => {
  if (window.opener && !window.opener.closed)
    window.opener.postMessage("popup-ready", location.origin);
});
document.querySelector("#close").addEventListener("click", () => window.close());
</script>)HTML");

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void check(bool condition, std::string_view message) {
    if (!condition)
        fail(std::string(message));
}

template<typename Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout, std::string_view expectation) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (predicate())
            return;
        QTest::qWait(10);
    }
    fail("Timed out waiting for " + std::string(expectation) + ".");
}

void sendBody(QTcpSocket *socket, const QByteArray &body, QByteArray contentType) {
    QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ");
    response += contentType;
    response += QByteArrayLiteral("\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    response += QByteArray::number(body.size());
    response += QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (socket->property("responded").toBool()) {
                    (void)socket->readAll();
                    return;
                }
                QByteArray request = socket->property("request").toByteArray();
                request.append(socket->readAll());
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n"))
                    return;
                socket->setProperty("responded", true);
                const QByteArray requestLine = request.left(request.indexOf("\r\n"));
                const QList<QByteArray> parts = requestLine.split(' ');
                const QByteArray path = parts.size() >= 2 ? parts.at(1) : QByteArray();
                if (path.startsWith(QByteArrayLiteral("/popup-parent"))) {
                    sendBody(socket, parentBody, QByteArrayLiteral("text/html; charset=utf-8"));
                } else if (path.startsWith(QByteArrayLiteral("/popup-child"))) {
                    sendBody(socket, childBody, QByteArrayLiteral("text/html; charset=utf-8"));
                } else {
                    const QByteArray body = QByteArrayLiteral("not found\n");
                    QByteArray response = QByteArrayLiteral(
                        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: "
                    );
                    response += QByteArray::number(body.size());
                    response += QByteArrayLiteral("\r\n\r\n");
                    response += body;
                    socket->write(response);
                    socket->disconnectFromHost();
                }
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
}

struct JavaScriptProbe {
    bool ready = false;
    QVariant value;
};

QVariant evaluateJavaScript(
    yobro::qtwebengine::QtBrowserPage &page,
    const QString &source,
    std::chrono::milliseconds timeout = 5s
) {
    auto probe = std::make_shared<JavaScriptProbe>();
    page.view()->page()->runJavaScript(source, [probe](const QVariant &value) {
        probe->value = value;
        probe->ready = true;
    });
    waitUntil([probe] { return probe->ready; }, timeout, "JavaScript evaluation");
    return probe->value;
}

void waitForJavaScriptTrue(
    yobro::qtwebengine::QtBrowserPage &page,
    const QString &source,
    std::string_view expectation
) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 20'000) {
        if (evaluateJavaScript(page, source).toBool())
            return;
        QTest::qWait(20);
    }
    fail("Timed out waiting for " + std::string(expectation) + ".");
}

void showPage(yobro::qtwebengine::QtBrowserPage &page) {
    page.view()->resize(800, 600);
    page.view()->show();
    page.view()->setFocus(Qt::OtherFocusReason);
}

yobro::qtwebengine::QtBrowserPage &asQtPage(yobro::engine::BrowserPage &page) {
    auto *qtPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&page);
    check(qtPage != nullptr, "Qt profile returned a non-Qt page.");
    return *qtPage;
}

void waitForLoad(
    yobro::qtwebengine::QtBrowserPage &page,
    std::string_view expectedUrl,
    std::string_view expectedTitle
) {
    waitUntil([&] {
        const yobro::engine::PageState state = page.state();
        return !state.loading && state.url == expectedUrl && state.title == expectedTitle;
    }, 20s, expectedTitle);
}

std::optional<yobro::controller::SessionTabView> tabViewFor(
    const yobro::controller::BrowserSession &session,
    const yobro::engine::BrowserPage *page
) {
    for (const auto &view : session.tabViews()) {
        if (view.page == page)
            return view;
    }
    return std::nullopt;
}

void clickPrimaryButton(yobro::qtwebengine::QtBrowserPage &page) {
    showPage(page);
    QTest::qWait(50);
    QWidget *target = page.view()->focusProxy();
    if (!target)
        target = page.view();
    target->setFocus(Qt::OtherFocusReason);
    QTest::mouseClick(
        target,
        Qt::LeftButton,
        Qt::NoModifier,
        QPoint(160, 80),
        20
    );
}

yobro::qtwebengine::QtBrowserPage &openPopup(
    yobro::controller::BrowserSession &session,
    yobro::qtwebengine::QtBrowserPage &opener,
    yobro::engine::PageOwner expectedOwner,
    bool expectedPrivate
) {
    const std::size_t before = session.tabViews().size();
    const int beforeAttempts = evaluateJavaScript(
        opener,
        QStringLiteral("globalThis.__popupAttempts || 0")
    ).toInt();
    clickPrimaryButton(opener);
    waitForJavaScriptTrue(
        opener,
        QStringLiteral("globalThis.__popupAttempts === %1").arg(beforeAttempts + 1),
        "QTest mouse gesture delivery"
    );
    waitUntil([&] { return session.tabViews().size() == before + 1; }, 20s, "session-owned popup creation");

    yobro::qtwebengine::QtBrowserPage *child = nullptr;
    std::optional<yobro::controller::SessionTabView> childView;
    for (const auto &view : session.tabViews()) {
        if (view.page == &opener)
            continue;
        child = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(view.page);
        childView = view;
    }
    check(child != nullptr && childView.has_value(), "Popup child is not owned by the Qt session.");
    check(childView->state.owner == expectedOwner, "Popup child inherited the wrong owner.");
    check(childView->privatePage == expectedPrivate, "Popup child inherited the wrong privacy context.");
    check(childView->space == "Popup Space", "Popup child inherited the wrong space.");
    check(child->profile() == opener.profile(), "Popup child was created in a different QWebEngineProfile.");

    const std::string childUrl = opener.state().url.substr(0, opener.state().url.find("/popup-parent"))
        + "/popup-child?case="
        + (expectedOwner == yobro::engine::PageOwner::agent ? "agent" : (expectedPrivate ? "private" : "user"));
    waitForLoad(*child, childUrl, "Popup Child");
    waitForJavaScriptTrue(
        opener,
        QStringLiteral("globalThis.__popupReady === true"),
        "same-origin window.opener confirmation"
    );
    return *child;
}

QWebEngineProfile *runUserCase(
    yobro::controller::BrowserSession &session,
    const std::string &parentUrl,
    bool privatePage
) {
    yobro::qtwebengine::QtBrowserPage &parent = asQtPage(session.newUserTab(parentUrl, privatePage));
    showPage(parent);
    waitForLoad(parent, parentUrl, "Popup Parent");
    check(parent.profile()->isOffTheRecord() == privatePage, "User opener selected the wrong Qt profile context.");
    const std::string parentId = parent.state().id;

    yobro::qtwebengine::QtBrowserPage &child = openPopup(
        session,
        parent,
        yobro::engine::PageOwner::user,
        privatePage
    );
    QWebEngineProfile *profile = parent.profile();
    clickPrimaryButton(child);
    waitUntil([&] {
        const auto views = session.tabViews();
        return views.size() == 1 && views.front().page == &parent;
    }, 20s, "native popup window.close removal");
    check(tabViewFor(session, &parent).has_value(), "Closing a popup also removed its opener.");
    check(session.closeTab(parentId), "Could not close the user popup opener.");
    check(session.tabViews().empty(), "User popup case left a page behind.");
    return profile;
}

void verifyAgentBridge(yobro::qtwebengine::QtBrowserPage &page) {
    struct Probe {
        bool ready = false;
        std::string json;
        std::optional<yobro::engine::EngineError> error;
    };
    auto probe = std::make_shared<Probe>();
    page.readAgentSnapshot([probe](std::string json, std::optional<yobro::engine::EngineError> error) {
        probe->json = std::move(json);
        probe->error = std::move(error);
        probe->ready = true;
    });
    waitUntil([probe] { return probe->ready; }, 10s, "AgentBridge popup snapshot");
    check(!probe->error.has_value() && !probe->json.empty(),
          "Agent popup did not receive the isolated AgentBridge before native adoption.");
}

void runAgentCase(
    yobro::controller::BrowserSession &session,
    const std::string &parentUrl
) {
    yobro::qtwebengine::QtBrowserPage &parent = asQtPage(session.newAgentTab(parentUrl));
    showPage(parent);
    waitForLoad(parent, parentUrl, "Popup Parent");
    check(!parent.profile()->isOffTheRecord(), "Agent opener incorrectly used an off-the-record profile.");

    session.setProfileActive(false);
    clickPrimaryButton(parent);
    waitForJavaScriptTrue(
        parent,
        QStringLiteral("globalThis.__popupAttempts === 1"),
        "inactive-profile popup gesture"
    );
    QTest::qWait(750);
    check(session.agentPages().size() == 1, "Inactive profile accepted an agent popup.");

    session.setProfileActive(true);
    yobro::qtwebengine::QtBrowserPage &child = openPopup(
        session,
        parent,
        yobro::engine::PageOwner::agent,
        false
    );
    verifyAgentBridge(child);
    check(session.agentPages().size() == 2, "Agent opener and popup were not both registered.");
    session.endAgentWorkspace();
    check(session.agentPages().empty() && session.tabViews().empty() && !session.agentPaneVisible(),
          "Agent end did not destroy both the opener and popup.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-popup-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT POPUP OWNERSHIP INTEGRATION FAIL: Could not create an isolated profile directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Popup Ownership Integration"));

    try {
        QTcpServer fixture;
        check(fixture.listen(QHostAddress::LocalHost, 0), "Could not start the popup loopback fixture.");
        serveFixture(fixture);
        const std::string baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fixture.serverPort()).toStdString();

        yobro::qtwebengine::QtBrowserEngine engine;
        auto profile = engine.openProfile({
            .id = "popup-integration",
            .storagePath = (home.path() + QStringLiteral("/storage")).toStdString(),
            .cachePath = (home.path() + QStringLiteral("/cache")).toStdString(),
            .persistent = true,
        });
        yobro::qtwebengine::QtEventLoop eventLoop;
        const std::vector<std::string> ids{
            "30000000-0000-4000-8000-000000000001",
            "30000000-0000-4000-8000-000000000002",
            "30000000-0000-4000-8000-000000000003",
            "30000000-0000-4000-8000-000000000004",
            "30000000-0000-4000-8000-000000000005",
            "30000000-0000-4000-8000-000000000006",
        };
        std::size_t nextId = 0;
        yobro::controller::BrowserSession session(
            std::move(profile),
            eventLoop,
            {
                .profileName = "Popup Integration",
                .space = "Popup Space",
                .generateId = [&] { return ids.at(nextId++); },
            }
        );
        session.setObserver([&session] {
            for (const auto &view : session.tabViews()) {
                auto *page = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(view.page);
                if (page)
                    showPage(*page);
            }
        });

        QWebEngineProfile *persistentProfile = runUserCase(
            session,
            baseUrl + "/popup-parent?case=user",
            false
        );
        QWebEngineProfile *privateProfile = runUserCase(
            session,
            baseUrl + "/popup-parent?case=private",
            true
        );
        check(persistentProfile != privateProfile, "Persistent and private popup cases shared a Qt profile.");
        runAgentCase(session, baseUrl + "/popup-parent?case=agent");

        session.setObserver({});
        fixture.close();
        std::cout << "QT POPUP/OPENER OWNERSHIP INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT POPUP OWNERSHIP INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
