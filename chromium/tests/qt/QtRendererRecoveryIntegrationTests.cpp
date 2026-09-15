#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/controller/ProtocolV2Controller.hpp"
#include "yobro/core/Json.hpp"

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
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineView>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace yobro::qtwebengine {

class QtBrowserPageTestPeer final {
public:
    static engine::NavigationToken beginWithoutNativeSignal(QtBrowserPage &page) {
        return page.beginNavigation([] {});
    }

    static void loadStarted(QtBrowserPage &page) {
        page.handleLoadStarted();
    }

    static void loadFinished(QtBrowserPage &page, bool ok) {
        page.handleLoadFinished(ok);
    }

    static void rendererTerminated(
        QtBrowserPage &page,
        QWebEnginePage::RenderProcessTerminationStatus status,
        int exitCode
    ) {
        page.handleRendererTerminated(static_cast<int>(status), exitCode);
    }

    static void expireActiveWatchdog(QtBrowserPage &page) {
        page.handleNavigationWatchdog(
            page.activeNavigationToken_,
            page.activeNavigationGeneration_
        );
    }

    static std::size_t retiredLoadCount(const QtBrowserPage &page) {
        return page.retiredLoadGenerations_.size();
    }
};

} // namespace yobro::qtwebengine

namespace {

using namespace std::chrono_literals;

const QByteArray pageA = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8"><title>Renderer A</title>
<h1 id="marker">A</h1>)HTML");

const QByteArray pageB = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8"><title>Renderer B</title>
<h1 id="marker">B</h1>)HTML");

const QByteArray agentPage = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8"><title>Renderer Agent</title>
<h1 id="marker">Agent</h1>)HTML");

const QByteArray mediaPage = QByteArrayLiteral(R"HTML(<!doctype html>
<meta charset="utf-8"><title>Renderer Media</title>
<style>button{position:fixed;left:40px;top:40px;width:260px;height:80px;font:18px sans-serif}</style>
<button id="media" type="button">Request microphone</button>
<script>
document.querySelector("#media").addEventListener("click", async () => {
  try {
    globalThis.__mediaStream = await navigator.mediaDevices.getUserMedia({audio:true});
    document.title = "Renderer Media Granted";
  } catch (error) {
    document.title = "Renderer Media Denied";
  }
});
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

class LoopbackFixture final {
public:
    LoopbackFixture() {
        check(server_.listen(QHostAddress::LocalHost, 0),
              "Could not start the renderer-recovery loopback fixture.");
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket *socket = server_.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray request = socket->property("request").toByteArray();
                    request.append(socket->readAll());
                    socket->setProperty("request", request);
                    if (socket->property("responded").toBool() || !request.contains("\r\n\r\n"))
                        return;
                    socket->setProperty("responded", true);
                    const QByteArray requestLine = request.left(request.indexOf("\r\n"));
                    const QList<QByteArray> parts = requestLine.split(' ');
                    QByteArray path = parts.size() >= 2 ? parts.at(1) : QByteArrayLiteral("/");
                    const qsizetype query = path.indexOf('?');
                    if (query >= 0)
                        path.truncate(query);
                    ++requests_[path];
                    if (path == QByteArrayLiteral("/a")) {
                        send(socket, pageA);
                    } else if (path == QByteArrayLiteral("/b")) {
                        send(socket, pageB);
                    } else if (path == QByteArrayLiteral("/agent")) {
                        send(socket, agentPage);
                    } else if (path == QByteArrayLiteral("/redirect")) {
                        send(
                            socket,
                            {},
                            QByteArrayLiteral("Location: /agent\r\n"),
                            QByteArrayLiteral("302 Found")
                        );
                    } else if (path == QByteArrayLiteral("/media")) {
                        send(socket, mediaPage, QByteArrayLiteral("Permissions-Policy: microphone=(self)\r\n"));
                    } else if (path == QByteArrayLiteral("/slow")) {
                        QTimer::singleShot(600, socket, [socket] {
                            send(socket, pageB);
                        });
                    } else {
                        send(socket, QByteArrayLiteral("not found\n"), {}, "404 Not Found", "text/plain");
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    [[nodiscard]] quint16 port() const { return server_.serverPort(); }

    [[nodiscard]] int requests(std::string_view path) const {
        const auto found = requests_.find(QByteArray(path.data(), static_cast<qsizetype>(path.size())));
        return found == requests_.end() ? 0 : found->second;
    }

    void close() { server_.close(); }

private:
    static void send(
        QTcpSocket *socket,
        const QByteArray &body,
        const QByteArray &extraHeaders = {},
        const QByteArray &status = QByteArrayLiteral("200 OK"),
        const QByteArray &contentType = QByteArrayLiteral("text/html; charset=utf-8")
    ) {
        QByteArray response = QByteArrayLiteral("HTTP/1.1 ");
        response += status;
        response += QByteArrayLiteral("\r\nContent-Type: ");
        response += contentType;
        response += QByteArrayLiteral("\r\nCache-Control: no-store\r\nConnection: close\r\n");
        response += extraHeaders;
        response += QByteArrayLiteral("Content-Length: ");
        response += QByteArray::number(body.size());
        response += QByteArrayLiteral("\r\n\r\n");
        response += body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    std::map<QByteArray, int> requests_;
};

yobro::qtwebengine::QtBrowserPage &asQtPage(yobro::engine::BrowserPage &page) {
    auto *qtPage = dynamic_cast<yobro::qtwebengine::QtBrowserPage *>(&page);
    check(qtPage != nullptr, "Qt profile returned a non-Qt page.");
    return *qtPage;
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

void showPage(yobro::qtwebengine::QtBrowserPage &page) {
    page.view()->resize(800, 600);
    page.view()->show();
    page.view()->setFocus(Qt::OtherFocusReason);
}

void waitForLoad(
    yobro::qtwebengine::QtBrowserPage &page,
    std::string_view url,
    std::string_view title,
    std::chrono::milliseconds timeout = 20s
) {
    waitUntil([&] {
        const auto state = page.state();
        return !state.loading && !state.error && state.url == url && state.title == title;
    }, timeout, title);
}

QVariant evaluateJavaScript(
    yobro::qtwebengine::QtBrowserPage &page,
    const QString &source,
    std::chrono::milliseconds timeout = 5s
) {
    struct Probe {
        bool ready = false;
        QVariant value;
    };
    auto probe = std::make_shared<Probe>();
    page.view()->page()->runJavaScript(source, [probe](const QVariant &value) {
        probe->value = value;
        probe->ready = true;
    });
    waitUntil([probe] { return probe->ready; }, timeout, "JavaScript evaluation");
    return probe->value;
}

qint64 rendererPid(yobro::qtwebengine::QtBrowserPage &page) {
    const qint64 pid = page.view()->page()->renderProcessPid();
    check(pid > 1 && pid != static_cast<qint64>(::getpid()),
          "Qt did not expose a safe renderer child PID.");
    return pid;
}

void terminateRenderer(yobro::qtwebengine::QtBrowserPage &page, int signal) {
    const qint64 pid = rendererPid(page);
    check(::kill(static_cast<pid_t>(pid), signal) == 0,
          "Could not terminate the Qt renderer child process.");
}

void clickMediaButton(yobro::qtwebengine::QtBrowserPage &page) {
    showPage(page);
    QWidget *target = page.view()->focusProxy();
    if (!target)
        target = page.view();
    target->setFocus(Qt::OtherFocusReason);
    QTest::qWait(50);
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, QPoint(170, 80), 20);
}

const yobro::core::Json &required(const yobro::core::Json &object, std::string_view key) {
    const auto *value = object.find(key);
    if (!value)
        fail("Missing response field: " + std::string(key));
    return *value;
}

void runNavigationLifecycleDeterminism(QWebEngineProfile *profile) {
    using Peer = yobro::qtwebengine::QtBrowserPageTestPeer;
    yobro::qtwebengine::QtBrowserPage page(
        profile,
        "renderer-lifecycle-determinism",
        yobro::engine::PageOwner::user
    );
    std::vector<yobro::engine::NavigationResult> results;
    std::vector<yobro::engine::RendererTermination> terminations;
    auto subscription = page.subscribe({
        .navigationFinished = [&](const yobro::engine::NavigationResult &result) {
            results.push_back(result);
        },
        .rendererTerminated = [&](const yobro::engine::RendererTermination &termination) {
            terminations.push_back(termination);
        },
    });
    const auto resultCount = [&](yobro::engine::NavigationToken token) {
        return static_cast<int>(std::count_if(
            results.begin(),
            results.end(),
            [token](const yobro::engine::NavigationResult &result) {
                return result.token == token;
            }
        ));
    };
    const auto resultFor = [&](yobro::engine::NavigationToken token)
        -> const yobro::engine::NavigationResult & {
        const auto found = std::find_if(
            results.begin(),
            results.end(),
            [token](const yobro::engine::NavigationResult &result) {
                return result.token == token;
            }
        );
        check(found != results.end(), "Expected deterministic navigation result is missing.");
        return *found;
    };

    const auto noStartSuccess = Peer::beginWithoutNativeSignal(page);
    Peer::loadFinished(page, true);
    check(resultCount(noStartSuccess) == 1 && !resultFor(noStartSuccess).error,
          "A no-loadStarted success did not settle its exact token once.");

    const auto noStartFailure = Peer::beginWithoutNativeSignal(page);
    Peer::loadFinished(page, false);
    QTest::qWait(40);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(resultCount(noStartFailure) == 1
          && resultFor(noStartFailure).error
          && resultFor(noStartFailure).error->code
              == yobro::engine::EngineErrorCode::navigationFailed,
          "A no-loadStarted failure did not settle as navigationFailed once.");

    const auto watchdogToken = Peer::beginWithoutNativeSignal(page);
    Peer::expireActiveWatchdog(page);
    Peer::loadFinished(page, false);
    Peer::expireActiveWatchdog(page);
    check(resultCount(watchdogToken) == 1
          && resultFor(watchdogToken).error
          && resultFor(watchdogToken).error->code
              == yobro::engine::EngineErrorCode::navigationFailed,
          "The start watchdog was not exact-once or a late finish revived its token.");

    const auto terminatedA = Peer::beginWithoutNativeSignal(page);
    Peer::loadStarted(page);
    Peer::rendererTerminated(
        page,
        QWebEnginePage::CrashedTerminationStatus,
        6
    );
    check(resultCount(terminatedA) == 1
          && resultFor(terminatedA).error
          && resultFor(terminatedA).error->code
              == yobro::engine::EngineErrorCode::rendererTerminated
          && terminations.size() == 1,
          "A deterministic renderer death did not settle A exactly once.");
    const auto pendingB = Peer::beginWithoutNativeSignal(page);
    Peer::loadFinished(page, false);
    check(resultCount(pendingB) == 0,
          "A stale false finish from terminated A was assigned to pending B.");
    Peer::loadFinished(page, true);
    check(resultCount(pendingB) == 1 && !resultFor(pendingB).error,
          "A no-loadStarted true finish did not complete B after stale A false.");

    const auto terminatedAgain = Peer::beginWithoutNativeSignal(page);
    Peer::loadStarted(page);
    Peer::rendererTerminated(
        page,
        QWebEnginePage::KilledTerminationStatus,
        9
    );
    const auto ambiguousB = Peer::beginWithoutNativeSignal(page);
    Peer::loadFinished(page, false);
    const auto newerC = Peer::beginWithoutNativeSignal(page);
    check(resultCount(terminatedAgain) == 1
          && resultCount(ambiguousB) == 1
          && resultFor(ambiguousB).error
          && resultFor(ambiguousB).error->code
              == yobro::engine::EngineErrorCode::navigationFailed,
          "An ambiguous B failure was mislabeled as a clean supersession.");
    Peer::loadFinished(page, false);
    Peer::expireActiveWatchdog(page);
    check(resultCount(newerC) == 1
          && resultFor(newerC).error
          && resultFor(newerC).error->code
              == yobro::engine::EngineErrorCode::navigationFailed,
          "The conservative ambiguous-finish watchdog did not settle C once.");

    bool reenterOnTerminalState = false;
    yobro::engine::NavigationToken reentrantToken = 0;
    auto reentrantSubscription = page.subscribe({
        .stateChanged = [&](const yobro::engine::PageState &state) {
            if (reenterOnTerminalState && !state.loading) {
                reenterOnTerminalState = false;
                reentrantToken = Peer::beginWithoutNativeSignal(page);
            }
        },
    });
    const auto reentrantSource = Peer::beginWithoutNativeSignal(page);
    Peer::loadStarted(page);
    reenterOnTerminalState = true;
    Peer::loadFinished(page, true);
    check(reentrantToken != 0
          && resultCount(reentrantSource) == 1
          && !resultFor(reentrantSource).error,
          "A terminal state callback lost or duplicated its captured navigation result.");
    Peer::loadFinished(page, true);
    check(resultCount(reentrantToken) == 1 && !resultFor(reentrantToken).error,
          "A reentrant newer navigation was cleared by the older completion.");

    for (int index = 0; index < 12; ++index)
        (void)Peer::beginWithoutNativeSignal(page);
    check(Peer::retiredLoadCount(page) <= 8,
          "Retired Qt load generations exceeded the bounded tombstone capacity.");
    Peer::expireActiveWatchdog(page);
}

void runUserRecovery(
    yobro::controller::BrowserSession &session,
    LoopbackFixture &fixture,
    const std::string &urlA,
    const std::string &urlB
) {
    auto &page = asQtPage(session.newUserTab(urlA));
    showPage(page);
    waitForLoad(page, urlA, "Renderer A");
    const auto pageAddress = static_cast<yobro::engine::BrowserPage *>(&page);
    QWebEngineView *const viewAddress = page.view();
    QWebEnginePage *const nativePageAddress = page.view()->page();
    QWebEngineProfile *const profileAddress = page.profile();
    std::vector<std::string> nativeEvents;
    QObject::connect(page.view(), &QWebEngineView::loadStarted, page.view(), [&] {
        nativeEvents.emplace_back("loadStarted");
    });
    QObject::connect(page.view(), &QWebEngineView::loadFinished, page.view(), [&](bool ok) {
        nativeEvents.emplace_back(ok ? "loadFinished(true)" : "loadFinished(false)");
    });
    QObject::connect(
        page.view(),
        &QWebEngineView::renderProcessTerminated,
        page.view(),
        [&](QWebEnginePage::RenderProcessTerminationStatus, int) {
            nativeEvents.emplace_back("renderProcessTerminated");
        }
    );

    std::vector<yobro::engine::NavigationResult> navigationResults;
    std::vector<yobro::engine::RendererTermination> terminations;
    auto subscription = page.subscribe({
        .navigationFinished = [&](const yobro::engine::NavigationResult &result) {
            navigationResults.push_back(result);
        },
        .rendererTerminated = [&](const yobro::engine::RendererTermination &termination) {
            terminations.push_back(termination);
        },
    });

    (void)page.navigate(urlB);
    waitForLoad(page, urlB, "Renderer B");
    check(page.state().canGoBack, "Renderer fixture did not establish back history.");
    check(evaluateJavaScript(page, QStringLiteral("document.querySelector('#marker').textContent")).toString()
              == QStringLiteral("B"),
          "Renderer fixture B DOM is unavailable before the crash.");

    const int requestsBeforeCrash = fixture.requests("/b");
    const qint64 firstPid = rendererPid(page);
    terminateRenderer(page, SIGABRT);
    waitUntil([&] { return !terminations.empty(); }, 10s, "SIGABRT renderer termination");
    check(terminations.back().kind == yobro::engine::RendererTerminationKind::crashed,
          "SIGABRT was not classified as a crashed Qt renderer.");
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return fixture.requests("/b") > requestsBeforeCrash
            && view
            && view->rendererRecovery == yobro::controller::RendererRecoveryState::recovering
            && !page.state().loading
            && !page.state().error
            && page.state().title == "Renderer B"
            && page.view()->page()->renderProcessPid() > 1
            && page.view()->page()->renderProcessPid() != firstPid;
    }, 30s, "same-page automatic renderer recovery");
    check(session.activeUserPage() == pageAddress
          && page.view() == viewAddress
          && page.view()->page() == nativePageAddress
          && page.profile() == profileAddress,
          "Renderer recovery replaced the BrowserPage, view, native page, or profile.");
    check(page.state().url == urlB && page.state().canGoBack,
          "Renderer recovery lost URL or history state.");
    check(evaluateJavaScript(page, QStringLiteral("document.querySelector('#marker').textContent")).toString()
              == QStringLiteral("B"),
          "Renderer recovery did not restore the B document.");

    (void)page.goBack();
    waitForLoad(page, urlA, "Renderer A");
    check(evaluateJavaScript(page, QStringLiteral("document.querySelector('#marker').textContent")).toString()
              == QStringLiteral("A"),
          "Back navigation after recovery did not restore A.");
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return view && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy;
    }, 8s, "stable post-crash history navigation");

    const int requestsBeforeKill = fixture.requests("/a");
    const qint64 preKillPid = rendererPid(page);
    terminateRenderer(page, SIGKILL);
    waitUntil([&] { return terminations.size() >= 2; }, 10s, "SIGKILL renderer termination");
    check(terminations.back().kind == yobro::engine::RendererTerminationKind::killed,
          "SIGKILL was not classified as a killed Qt renderer.");
    waitUntil([&] {
        return fixture.requests("/a") > requestsBeforeKill
            && !page.state().loading
            && !page.state().error
            && page.state().title == "Renderer A"
            && page.view()->page()->renderProcessPid() > 1
            && page.view()->page()->renderProcessPid() != preKillPid;
    }, 30s, "SIGKILL automatic recovery");
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return view && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy;
    }, 8s, "stable SIGKILL recovery");

    (void)page.navigate(urlB);
    waitForLoad(page, urlB, "Renderer B");
    const int beforeFirstLoopCrash = fixture.requests("/b");
    const qint64 firstLoopPid = rendererPid(page);
    terminateRenderer(page, SIGABRT);
    waitUntil([&] {
        return fixture.requests("/b") > beforeFirstLoopCrash
            && !page.state().loading
            && !page.state().error
            && page.state().title == "Renderer B"
            && page.view()->page()->renderProcessPid() > 1
            && page.view()->page()->renderProcessPid() != firstLoopPid;
    }, 30s, "first crash-loop reload");
    const int afterFirstLoopReload = fixture.requests("/b");
    terminateRenderer(page, SIGABRT);
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return view
            && view->rendererRecovery == yobro::controller::RendererRecoveryState::failed
            && view->state.error
            && *view->state.error == "Der Seiteninhalt ist mehrfach abgestürzt. Lade sie neu oder öffne sie in einem neuen Tab.";
    }, 10s, "second-crash visible failure");
    const int beforeManualRetry = fixture.requests("/b");
    check(beforeManualRetry == afterFirstLoopReload,
          "Second pre-stability crash issued an automatic request before failure surfaced.");
    check(session.reloadUserTab(page.state().id),
          "Immediate manual retry was rejected after the real crash loop.");
    waitForLoad(page, urlB, "Renderer B", 30s);
    QTest::qWait(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    check(fixture.requests("/b") == beforeManualRetry + 1,
          "Immediate manual retry was swallowed or duplicated by a stale failed finish.");
    check(session.activeUserPage() == pageAddress
          && page.view() == viewAddress
          && page.view()->page() == nativePageAddress,
          "Manual crash-loop retry replaced the existing page identity.");

    const std::string slowUrl = urlA.substr(0, urlA.rfind('/')) + "/slow";
    const int slowRequestsBeforeActiveCrash = fixture.requests("/slow");
    const int bRequestsBeforeActiveCrash = fixture.requests("/b");
    const std::size_t navigationResultsBeforeActiveCrash = navigationResults.size();
    nativeEvents.clear();
    const auto activeSlowToken = page.navigate(slowUrl);
    waitUntil([&] {
        return page.state().loading
            && fixture.requests("/slow") > slowRequestsBeforeActiveCrash;
    }, 10s, "active slow navigation before renderer crash");
    const qint64 activeLoadPid = rendererPid(page);
    terminateRenderer(page, SIGABRT);
    bool activeLoadRecovered = false;
    QElapsedTimer activeRecoveryElapsed;
    activeRecoveryElapsed.start();
    while (activeRecoveryElapsed.elapsed() < 30'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        activeLoadRecovered = fixture.requests("/b") > bRequestsBeforeActiveCrash
            && !page.state().loading
            && !page.state().error
            && page.state().url == urlB
            && page.state().title == "Renderer B"
            && page.view()->page()->renderProcessPid() > 1
            && page.view()->page()->renderProcessPid() != activeLoadPid;
        if (activeLoadRecovered)
            break;
        QTest::qWait(10);
    }
    if (!activeLoadRecovered) {
        const auto failedView = tabViewFor(session, &page);
        std::string eventLog;
        for (const auto &event : nativeEvents) {
            if (!eventLog.empty())
                eventLog += ',';
            eventLog += event;
        }
        fail("Active-load recovery state: slowRequests="
             + std::to_string(fixture.requests("/slow"))
             + ", slowBaseline=" + std::to_string(slowRequestsBeforeActiveCrash)
             + ", bRequests=" + std::to_string(fixture.requests("/b"))
             + ", bBaseline=" + std::to_string(bRequestsBeforeActiveCrash)
             + ", loading=" + (page.state().loading ? "true" : "false")
             + ", error=" + page.state().error.value_or("<none>")
             + ", url=" + page.state().url
             + ", title=" + page.state().title
             + ", pid=" + std::to_string(page.view()->page()->renderProcessPid())
             + ", oldPid=" + std::to_string(activeLoadPid)
             + ", events=" + eventLog
             + ", recovery=" + std::to_string(failedView
                 ? static_cast<int>(failedView->rendererRecovery)
                 : -1));
    }
    QTest::qWait(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto activeResultCount = std::count_if(
        navigationResults.begin() + static_cast<std::ptrdiff_t>(navigationResultsBeforeActiveCrash),
        navigationResults.end(),
        [activeSlowToken](const yobro::engine::NavigationResult &result) {
            return result.token == activeSlowToken
                && result.error
                && result.error->code == yobro::engine::EngineErrorCode::rendererTerminated;
        }
    );
    const auto recoverySuccessCount = std::count_if(
        navigationResults.begin() + static_cast<std::ptrdiff_t>(navigationResultsBeforeActiveCrash),
        navigationResults.end(),
        [activeSlowToken](const yobro::engine::NavigationResult &result) {
            return result.token != activeSlowToken && !result.error;
        }
    );
    const auto terminationEvent = std::find(
        nativeEvents.begin(),
        nativeEvents.end(),
        "renderProcessTerminated"
    );
    const auto successfulFinishAfterTermination = terminationEvent == nativeEvents.end()
        ? nativeEvents.end()
        : std::find(
            std::next(terminationEvent),
            nativeEvents.end(),
            "loadFinished(true)"
        );
    check(activeResultCount == 1
          && recoverySuccessCount == 1
          && navigationResults.size() == navigationResultsBeforeActiveCrash + 2
          && successfulFinishAfterTermination != nativeEvents.end(),
          "Real active-load recovery did not produce exactly one terminated A and one successful B result in native signal order.");
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return view && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy;
    }, 8s, "stable active-load recovery");

    (void)page.navigate(slowUrl);
    waitForLoad(page, slowUrl, "Renderer B", 30s);
    const int slowRequestsBeforeDeactivation = fixture.requests("/slow");
    terminateRenderer(page, SIGABRT);
    waitUntil([&] {
        return fixture.requests("/slow") > slowRequestsBeforeDeactivation
            && page.state().loading;
    }, 10s, "in-flight automatic reload before profile deactivation");
    session.setProfileActive(false);
    auto inactiveView = tabViewFor(session, &page);
    check(inactiveView
          && inactiveView->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && inactiveView->state.error,
          "Profile cancellation did not synchronously expose a failed recovery.");
    session.setProfileActive(true);
    QTest::qWait(5'500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    inactiveView = tabViewFor(session, &page);
    check(inactiveView
          && inactiveView->rendererRecovery == yobro::controller::RendererRecoveryState::failed
          && inactiveView->state.error,
          "A late real recovery result restored health after immediate profile reactivation.");
    check(session.navigateUserTab(page.state().id, urlB),
          "Session-owned address navigation was rejected after real profile cancellation.");
    waitForLoad(page, urlB, "Renderer B", 30s);
    waitUntil([&] {
        const auto view = tabViewFor(session, &page);
        return view
            && view->rendererRecovery == yobro::controller::RendererRecoveryState::healthy;
    }, 8s, "stable session-owned navigation after profile reactivation");
}

void runPermissionCancellation(
    yobro::controller::BrowserSession &session,
    LoopbackFixture &fixture,
    const std::string &mediaUrl
) {
    auto &page = asQtPage(session.newUserTab(mediaUrl));
    showPage(page);
    waitForLoad(page, mediaUrl, "Renderer Media");
    session.setPermissionSurfaceVisible(true);
    session.setPermissionPromptPresenter([](const yobro::controller::PermissionPrompt &) {
        return true;
    });

    std::optional<QWebEnginePermission> nativePermission;
    QObject::connect(
        page.view()->page(),
        &QWebEnginePage::permissionRequested,
        page.view(),
        [&](QWebEnginePermission permission) {
            nativePermission = std::move(permission);
        }
    );
    clickMediaButton(page);
    waitUntil([&] {
        return session.pendingPermission().has_value() && nativePermission.has_value();
    }, 10s, "real pending media permission");
    const int requestsBeforeCrash = fixture.requests("/media");
    terminateRenderer(page, SIGABRT);
    waitUntil([&] { return !session.pendingPermission().has_value(); }, 10s,
              "permission cancellation on renderer death");
    check(!nativePermission->isValid()
              || nativePermission->state() == QWebEnginePermission::State::Denied,
          "Renderer death left the native media capability grant-capable.");
    waitUntil([&] {
        return fixture.requests("/media") > requestsBeforeCrash
            && !page.state().loading
            && !page.state().error
            && page.state().title == "Renderer Media";
    }, 30s, "media page recovery after permission cancellation");
    session.setPermissionPromptPresenter({});
    session.setPermissionSurfaceVisible(false);
}

void runAgentCrash(
    yobro::controller::BrowserSession &session,
    yobro::controller::ProtocolV2Controller &controller,
    LoopbackFixture &fixture,
    const std::string &agentUrl
) {
    auto &page = asQtPage(session.newAgentTab(agentUrl));
    showPage(page);
    waitForLoad(page, agentUrl, "Renderer Agent");

    const std::string redirectUrl = agentUrl.substr(0, agentUrl.rfind('/')) + "/redirect";
    auto redirectResponse = std::make_shared<std::optional<std::string>>();
    controller.handleLine(
        "{\"command\":\"open\",\"url\":\"" + redirectUrl + "\"}",
        [redirectResponse](std::string value) { *redirectResponse = std::move(value); }
    );
    waitUntil([&] { return redirectResponse->has_value(); }, 30s,
              "real protocol redirect completion");
    const yobro::core::Json redirected = yobro::core::Json::parse(**redirectResponse);
    check(required(redirected, "ok").asBoolean()
          && page.state().url == agentUrl
          && fixture.requests("/redirect") > 0,
          "A real redirect cancelled its token-owning protocol operation.");

    struct ScriptProbe { bool ready = false; };
    auto scriptProbe = std::make_shared<ScriptProbe>();
    page.view()->page()->runJavaScript(
        QStringLiteral("globalThis.__yobro.snapshot = function () { for (;;) {} }; true"),
        QWebEngineScript::ApplicationWorld,
        [scriptProbe](const QVariant &) { scriptProbe->ready = true; }
    );
    waitUntil([scriptProbe] { return scriptProbe->ready; }, 5s, "slow AgentBridge override");

    const int agentRequests = fixture.requests("/agent");
    int completionCount = 0;
    auto response = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"read"})", [response, &completionCount](std::string value) {
        ++completionCount;
        *response = std::move(value);
    });
    waitUntil([&] { return controller.busy(); }, 2s, "busy slow agent read");
    QTest::qWait(250);
    terminateRenderer(page, SIGABRT);
    waitUntil([&] { return response->has_value() && !controller.busy(); }, 15s,
              "agent operation failure on real renderer crash");
    const yobro::core::Json decoded = yobro::core::Json::parse(**response);
    check(!required(decoded, "ok").asBoolean() && completionCount == 1,
          "Real agent renderer crash did not produce one error response.");
    QTest::qWait(500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    check(completionCount == 1 && fixture.requests("/agent") == agentRequests,
          "Agent renderer crash reloaded automatically or completed twice.");

    auto reloadResponse = std::make_shared<std::optional<std::string>>();
    controller.handleLine(R"({"command":"reload"})", [reloadResponse](std::string value) {
        *reloadResponse = std::move(value);
    });
    waitUntil([&] {
        return reloadResponse->has_value() && !page.state().loading && !page.state().error;
    }, 30s, "explicit agent reload after crash");
    const yobro::core::Json reloaded = yobro::core::Json::parse(**reloadResponse);
    const auto recoveredView = tabViewFor(session, &page);
    check(required(reloaded, "ok").asBoolean()
          && fixture.requests("/agent") > agentRequests
          && recoveredView
          && recoveredView->rendererRecovery == yobro::controller::RendererRecoveryState::healthy,
          "Explicit agent reload did not restore the crashed page and lifecycle state.");

    auto actionProbe = std::make_shared<ScriptProbe>();
    page.view()->page()->runJavaScript(
        QStringLiteral("globalThis.__yobro.act = function () { for (;;) {} }; true"),
        QWebEngineScript::ApplicationWorld,
        [actionProbe](const QVariant &) { actionProbe->ready = true; }
    );
    waitUntil([actionProbe] { return actionProbe->ready; }, 5s, "slow AgentBridge action override");
    const int requestsBeforeActionCrash = fixture.requests("/agent");
    int actionCompletions = 0;
    auto actionResponse = std::make_shared<std::optional<std::string>>();
    controller.handleLine(
        R"({"command":"click","document":"doc","ref":"e1"})",
        [actionResponse, &actionCompletions](std::string value) {
            ++actionCompletions;
            *actionResponse = std::move(value);
        }
    );
    waitUntil([&] { return controller.busy(); }, 2s, "busy slow agent action");
    QTest::qWait(250);
    terminateRenderer(page, SIGABRT);
    waitUntil([&] { return actionResponse->has_value() && !controller.busy(); }, 15s,
              "agent action failure on real renderer crash");
    const yobro::core::Json failedAction = yobro::core::Json::parse(**actionResponse);
    const bool actionFailed = !required(failedAction, "ok").asBoolean()
        && !required(failedAction, "error").asString().empty();
    QTest::qWait(500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    check(actionFailed
          && actionCompletions == 1
          && fixture.requests("/agent") == requestsBeforeActionCrash,
          "Real non-read agent crash did not fail once without an automatic reload.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-renderer-recovery-qt-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT RENDERER RECOVERY INTEGRATION FAIL: Could not create an isolated profile directory.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Renderer Recovery Integration"));

    try {
        LoopbackFixture fixture;
        const std::string ipBase = QStringLiteral("http://127.0.0.1:%1")
            .arg(fixture.port()).toStdString();
        const std::string hostBase = QStringLiteral("http://localhost:%1")
            .arg(fixture.port()).toStdString();

        yobro::qtwebengine::QtBrowserEngine engine;
        auto profile = engine.openProfile({
            .id = "renderer-recovery-integration",
            .storagePath = (home.path() + QStringLiteral("/storage")).toStdString(),
            .cachePath = (home.path() + QStringLiteral("/cache")).toStdString(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Renderer integration profile is not Qt WebEngine.");
        runNavigationLifecycleDeterminism(qtProfile->persistentProfile());
        yobro::qtwebengine::QtEventLoop eventLoop;
        const std::vector<std::string> ids{
            "60000000-0000-4000-8000-000000000001",
            "60000000-0000-4000-8000-000000000002",
            "60000000-0000-4000-8000-000000000003",
        };
        std::size_t nextId = 0;
        yobro::controller::BrowserSession session(
            std::move(profile),
            eventLoop,
            {
                .profileName = "Renderer Recovery Integration",
                .rendererRecoveryDelay = 150ms,
                .rendererStabilityDelay = 5s,
                .generateId = [&] { return ids.at(nextId++); },
            }
        );
        yobro::controller::ProtocolV2Controller controller(eventLoop, session);

        runUserRecovery(session, fixture, ipBase + "/a", ipBase + "/b");
        runPermissionCancellation(session, fixture, ipBase + "/media");
        runAgentCrash(session, controller, fixture, hostBase + "/agent");

        session.setPermissionPromptPresenter({});
        session.setPermissionSurfaceVisible(false);
        fixture.close();
        std::cout << "QT RENDERER RECOVERY INTEGRATION PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT RENDERER RECOVERY INTEGRATION FAIL: " << error.what() << '\n';
        return 1;
    }
}
