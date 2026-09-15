#include "spike/SmokeRunner.hpp"

#include "engine/api/BrowserEngine.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineView>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace yobro::spike {
namespace {

std::unique_ptr<qtwebengine::QtBrowserPage> asQtPage(std::unique_ptr<engine::BrowserPage> page) {
    auto *qtPage = dynamic_cast<qtwebengine::QtBrowserPage *>(page.release());
    if (!qtPage)
        throw std::runtime_error("The smoke runner requires the Qt page adapter.");
    return std::unique_ptr<qtwebengine::QtBrowserPage>(qtPage);
}

std::optional<QJsonObject> objectFrom(const std::string &json, QString *failure) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (failure)
            *failure = QStringLiteral("AgentBridge returned invalid JSON: ") + error.errorString();
        return std::nullopt;
    }
    return document.object();
}

QString findReference(const QJsonArray &elements, const QString &label, QString *value = nullptr) {
    for (const QJsonValue &entry : elements) {
        const QJsonObject element = entry.toObject();
        if (element.value(QStringLiteral("label")).toString() != label)
            continue;
        if (value)
            *value = element.value(QStringLiteral("value")).toVariant().toString();
        return element.value(QStringLiteral("ref")).toString();
    }
    return {};
}

QJsonObject actionRequest(
    const QString &action,
    const QString &document,
    const QString &reference,
    const QString &value = {}
) {
    return {
        {QStringLiteral("action"), action},
        {QStringLiteral("document"), document},
        {QStringLiteral("ref"), reference},
        {QStringLiteral("value"), value},
    };
}

std::string compact(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace

SmokeRunner::SmokeRunner(
    qtwebengine::QtBrowserProfile &primaryProfile,
    qtwebengine::QtBrowserProfile &secondaryProfile,
    Completion completion
) : primaryProfile_(primaryProfile), secondaryProfile_(secondaryProfile), completion_(std::move(completion)) {}

SmokeRunner::~SmokeRunner() = default;

void SmokeRunner::start() {
    qInfo().noquote()
        << "CHROMIUM SMOKE ENGINE: Qt WebEngine" << qWebEngineVersion()
        << "| Chromium" << qWebEngineChromiumVersion()
        << "| security patch" << qWebEngineChromiumSecurityPatchVersion();
    qInfo("CHROMIUM SMOKE: starting Phase 1B with sandbox and site isolation defaults");
    QTimer::singleShot(180'000, this, [this] {
        if (!finished_)
            fail(QStringLiteral("Timed out after 180 seconds."));
    });

    if (!startFixtureServer())
        return;

    try {
        userPage_ = asQtPage(primaryProfile_.createPage("smoke-user", engine::PageOwner::user));
        agentPage_ = asQtPage(primaryProfile_.createPage("smoke-agent", engine::PageOwner::agent));
        privatePage_ = asQtPage(primaryProfile_.createPage("smoke-private", engine::PageOwner::user, true));
        secondaryPage_ = asQtPage(secondaryProfile_.createPage("smoke-secondary", engine::PageOwner::user));
        lifecyclePage_ = asQtPage(primaryProfile_.createPage("smoke-lifecycle", engine::PageOwner::user));
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }

    if (primaryProfile_.persistentProfile()->isOffTheRecord()
        || secondaryProfile_.persistentProfile()->isOffTheRecord()) {
        fail(QStringLiteral("A named Chromium profile is unexpectedly off the record."));
        return;
    }
    if (!primaryProfile_.privateProfile()->isOffTheRecord()) {
        fail(QStringLiteral("The private Chromium profile is unexpectedly persistent."));
        return;
    }
    if (userPage_->profile() != agentPage_->profile()
        || userPage_->profile() == privatePage_->profile()
        || userPage_->profile() == secondaryPage_->profile()
        || primaryProfile_.spec().storagePath == secondaryProfile_.spec().storagePath
        || primaryProfile_.spec().cachePath == secondaryProfile_.spec().cachePath) {
        fail(QStringLiteral("User/agent sharing or private/second-profile isolation is incorrect."));
        return;
    }

    bool privateAgentRejected = false;
    try {
        auto forbidden = primaryProfile_.createPage("forbidden-private-agent", engine::PageOwner::agent, true);
    } catch (const std::invalid_argument &) {
        privateAgentRejected = true;
    }
    if (!privateAgentRejected) {
        fail(QStringLiteral("An agent-owned private page was accepted."));
        return;
    }

    verifyUserOwnershipIsolation();
}

bool SmokeRunner::startFixtureServer() {
    fixtureServer_ = std::make_unique<QTcpServer>();
    QObject::connect(fixtureServer_.get(), &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = fixtureServer_->nextPendingConnection()) {
            socket->setParent(fixtureServer_.get());
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                respondToFixtureRequest(socket);
            });
            if (socket->bytesAvailable() > 0)
                QTimer::singleShot(0, socket, [this, socket] { respondToFixtureRequest(socket); });
        }
    });
    if (!fixtureServer_->listen(QHostAddress::LocalHost, 0)) {
        fail(QStringLiteral("Could not start the local lifecycle fixture server: ") + fixtureServer_->errorString());
        return false;
    }
    fixtureBaseUrl_ = QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(fixtureServer_->serverPort()));
    return true;
}

void SmokeRunner::respondToFixtureRequest(QTcpSocket *socket) {
    if (socket->property("yobroResponded").toBool())
        return;
    QByteArray request = socket->property("yobroRequest").toByteArray();
    request.append(socket->readAll());
    socket->setProperty("yobroRequest", request);
    if (!request.contains("\r\n\r\n"))
        return;
    socket->setProperty("yobroResponded", true);

    const QByteArray requestLine = request.left(request.indexOf("\r\n"));
    const QList<QByteArray> parts = requestLine.split(' ');
    const QByteArray rawTarget = parts.size() > 1 ? parts.at(1) : QByteArray("/");
    const QByteArray path = rawTarget.left(rawTarget.indexOf('?') >= 0 ? rawTarget.indexOf('?') : rawTarget.size());

    if (path == "/redirect") {
        const QByteArray response =
            "HTTP/1.1 302 Found\r\nLocation: /history-b\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }
    if (path == "/favicon.ico") {
        socket->write("HTTP/1.1 204 No Content\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        socket->disconnectFromHost();
        return;
    }

    QString title;
    if (path == "/history-a")
        title = QStringLiteral("History A");
    else if (path == "/history-b")
        title = QStringLiteral("History B");
    else
        title = QStringLiteral("Not Found");
    const QByteArray body = QStringLiteral(
        "<!doctype html><meta charset='utf-8'><title>%1</title><body><h1>%1</h1></body>"
    ).arg(title).toUtf8();
    const QByteArray status = path == "/history-a" || path == "/history-b"
        ? QByteArray("HTTP/1.1 200 OK\r\n")
        : QByteArray("HTTP/1.1 404 Not Found\r\n");
    QByteArray response = status;
    response += "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

QUrl SmokeRunner::fixtureUrl(const QString &path) const {
    QUrl result = fixtureBaseUrl_;
    result.setPath(path);
    return result;
}

void SmokeRunner::loadHtml(
    qtwebengine::QtBrowserPage &page,
    const QString &html,
    const QUrl &baseUrl,
    std::function<void(bool)> completion
) {
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(
        page.view(),
        &QWebEngineView::loadFinished,
        this,
        [this, connection, completion = std::move(completion)](bool ok) mutable {
            QObject::disconnect(*connection);
            if (!finished_)
                completion(ok);
        }
    );
    page.setHtml(html, baseUrl);
}

void SmokeRunner::waitForLoad(
    qtwebengine::QtBrowserPage &page,
    const QUrl &expectedUrl,
    std::function<void()> trigger,
    std::function<void(bool)> completion
) {
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(
        page.view(),
        &QWebEngineView::loadFinished,
        this,
        [this, &page, expectedUrl, connection, completion = std::move(completion)](bool ok) mutable {
            QObject::disconnect(*connection);
            if (!finished_)
                completion(ok && page.view()->url() == expectedUrl);
        }
    );
    trigger();
}

void SmokeRunner::verifyUserOwnershipIsolation() {
    userPage_->readAgentSnapshot([this](std::string json, std::optional<engine::EngineError> error) {
        if (!json.empty() || !error || *error != "Agent snapshots are forbidden on user-owned pages.") {
            fail(QStringLiteral("A user-owned page accepted an agent snapshot."));
            return;
        }
        userPage_->performAgentAction("{}", [this](std::string actionJson, std::optional<engine::EngineError> actionError) {
            if (!actionJson.empty() || !actionError || *actionError != "Agent actions are forbidden on user-owned pages.") {
                fail(QStringLiteral("A user-owned page accepted an agent action."));
                return;
            }
            userPage_->view()->page()->runJavaScript(
                QStringLiteral("typeof globalThis.__yobro"),
                QWebEngineScript::ApplicationWorld,
                [this](const QVariant &value) {
                    if (value.toString() != QStringLiteral("undefined")) {
                        fail(QStringLiteral("AgentBridge was injected into a user-owned page."));
                        return;
                    }
                    loadCookieOwner();
                }
            );
        });
    });
}

void SmokeRunner::loadCookieOwner() {
    const QUrl origin(QStringLiteral("https://fixture.yobro.invalid/"));
    const QString cookieSetter = QStringLiteral(R"HTML(
<!doctype html><meta charset="utf-8"><title>Cookie owner</title>
<script>document.cookie='yobro_shared=phase1b; SameSite=Lax; path=/';</script>
<body>User profile cookie owner</body>
)HTML");
    loadHtml(*userPage_, cookieSetter, origin, [this](bool ok) {
        if (!ok) {
            fail(QStringLiteral("Could not load the persistent user fixture."));
            return;
        }
        loadAgentFixture();
    });
}

void SmokeRunner::loadAgentFixture() {
    const QString agentFixture = QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8"><title>Agent fixture</title></head>
<body><h1>Agent fixture</h1><p id="cookie"></p>
<label>Password <input type="password" aria-label="Password" value="super-secret"></label>
<label>Upload <input type="file" aria-label="Upload"></label>
<div id="shadow-host"></div>
<iframe title="Fixture Frame" srcdoc="<button aria-label='Iframe Confirm'>Must stay inaccessible</button>"></iframe>
<p id="result">waiting</p>
<script>
document.getElementById('cookie').textContent='cookie:'+document.cookie;
const root=document.getElementById('shadow-host').attachShadow({mode:'open'});
root.innerHTML='<label>Shadow Name <input aria-label="Shadow Name"></label><button aria-label="Shadow Confirm">Confirm</button>';
root.querySelector('button').addEventListener('click',()=>document.getElementById('result').textContent='clicked');
</script>
</body></html>
)HTML");
    loadHtml(*agentPage_, agentFixture, QUrl(QStringLiteral("https://fixture.yobro.invalid/agent")), [this](bool loaded) {
        if (!loaded) {
            fail(QStringLiteral("Could not load the agent fixture."));
            return;
        }
        agentPage_->readAgentSnapshot([this](std::string json, std::optional<engine::EngineError> error) {
            verifyInitialSnapshot(std::move(json), std::move(error));
        });
    });
}

void SmokeRunner::verifyInitialSnapshot(std::string json, std::optional<engine::EngineError> error) {
    if (error) {
        fail(QString::fromStdString(*error));
        return;
    }
    QString parseFailure;
    const auto snapshot = objectFrom(json, &parseFailure);
    if (!snapshot) {
        fail(parseFailure);
        return;
    }
    documentId_ = snapshot->value(QStringLiteral("document")).toString();
    const QString text = snapshot->value(QStringLiteral("text")).toString();
    const QJsonArray elements = snapshot->value(QStringLiteral("elements")).toArray();
    QString passwordValue;
    fieldRef_ = findReference(elements, QStringLiteral("Shadow Name"));
    buttonRef_ = findReference(elements, QStringLiteral("Shadow Confirm"));
    fileRef_ = findReference(elements, QStringLiteral("Upload"));
    findReference(elements, QStringLiteral("Password"), &passwordValue);

    bool frameMetadataPresent = false;
    for (const QJsonValue &entry : snapshot->value(QStringLiteral("frames")).toArray()) {
        const QJsonObject frame = entry.toObject();
        if (frame.value(QStringLiteral("title")).toString() == QStringLiteral("Fixture Frame")
            && frame.value(QStringLiteral("note")).toString() == QStringLiteral("Frame content is not included.")) {
            frameMetadataPresent = true;
        }
    }

    if (documentId_.isEmpty() || fieldRef_.isEmpty() || buttonRef_.isEmpty() || fileRef_.isEmpty()) {
        fail(QStringLiteral("Snapshot is missing its document id or expected open-shadow references."));
        return;
    }
    if (!findReference(elements, QStringLiteral("Iframe Confirm")).isEmpty() || !frameMetadataPresent) {
        fail(QStringLiteral("Iframe content leaked into elements or frame exclusion metadata is missing."));
        return;
    }
    if (passwordValue != QStringLiteral("[redacted]")) {
        fail(QStringLiteral("Password values are not redacted."));
        return;
    }
    if (!text.contains(QStringLiteral("yobro_shared=phase1b"))) {
        fail(QStringLiteral("The agent page did not receive the user page's same-profile cookie."));
        return;
    }

    agentPage_->performAgentAction(
        compact(actionRequest(QStringLiteral("fill"), documentId_, fieldRef_, QStringLiteral("Laura"))),
        [this](std::string, std::optional<engine::EngineError> fillError) {
            if (fillError) {
                fail(QStringLiteral("Open-shadow fill failed: ") + QString::fromStdString(*fillError));
                return;
            }
            agentPage_->performAgentAction(
                compact(actionRequest(QStringLiteral("click"), documentId_, buttonRef_)),
                [this](std::string, std::optional<engine::EngineError> clickError) {
                    if (clickError) {
                        fail(QStringLiteral("Open-shadow click failed: ") + QString::fromStdString(*clickError));
                        return;
                    }
                    agentPage_->readAgentSnapshot([this](std::string nextJson, std::optional<engine::EngineError> nextError) {
                        verifyFilledSnapshot(std::move(nextJson), std::move(nextError));
                    });
                }
            );
        }
    );
}

void SmokeRunner::verifyFilledSnapshot(std::string json, std::optional<engine::EngineError> error) {
    if (error) {
        fail(QString::fromStdString(*error));
        return;
    }
    QString parseFailure;
    const auto snapshot = objectFrom(json, &parseFailure);
    if (!snapshot) {
        fail(parseFailure);
        return;
    }
    QString fieldValue;
    findReference(snapshot->value(QStringLiteral("elements")).toArray(), QStringLiteral("Shadow Name"), &fieldValue);
    if (fieldValue != QStringLiteral("Laura")) {
        fail(QStringLiteral("Open-shadow fill did not update the field through the isolated bridge."));
        return;
    }
    if (!snapshot->value(QStringLiteral("text")).toString().contains(QStringLiteral("clicked"))) {
        fail(QStringLiteral("Open-shadow click did not produce an observable page change."));
        return;
    }
    verifyFileUploadRejected();
}

void SmokeRunner::verifyFileUploadRejected() {
    agentPage_->performAgentAction(
        compact(actionRequest(QStringLiteral("fill"), documentId_, fileRef_, QStringLiteral("/tmp/forbidden"))),
        [this](std::string, std::optional<engine::EngineError> uploadError) {
            if (!uploadError) {
                fail(QStringLiteral("AgentBridge accepted a file upload fill."));
                return;
            }
            const QString replacement = QStringLiteral(
                "<!doctype html><meta charset=utf-8><title>New document</title>"
                "<body><h1>New document</h1><input aria-label='Name'></body>"
            );
            loadHtml(*agentPage_, replacement, QUrl(QStringLiteral("https://fixture.yobro.invalid/next")), [this](bool ok) {
                if (!ok) {
                    fail(QStringLiteral("Could not load the replacement document."));
                    return;
                }
                agentPage_->readAgentSnapshot([this](std::string nextJson, std::optional<engine::EngineError> nextError) {
                    verifyNewDocument(std::move(nextJson), std::move(nextError));
                });
            });
        }
    );
}

void SmokeRunner::verifyNewDocument(std::string json, std::optional<engine::EngineError> error) {
    if (error) {
        fail(QString::fromStdString(*error));
        return;
    }
    QString parseFailure;
    const auto snapshot = objectFrom(json, &parseFailure);
    if (!snapshot) {
        fail(parseFailure);
        return;
    }
    const QString newDocumentId = snapshot->value(QStringLiteral("document")).toString();
    if (newDocumentId.isEmpty() || newDocumentId == documentId_) {
        fail(QStringLiteral("Navigation did not rotate the AgentBridge document id."));
        return;
    }
    previousJourneyDocumentId_ = newDocumentId;

    agentPage_->performAgentAction(
        compact(actionRequest(QStringLiteral("fill"), documentId_, fieldRef_, QStringLiteral("must-not-apply"))),
        [this](std::string, std::optional<engine::EngineError> staleError) {
            if (!staleError) {
                fail(QStringLiteral("A stale document/reference pair was accepted after navigation."));
                return;
            }
            verifyPrivateIsolation();
        }
    );
}

void SmokeRunner::verifyPrivateIsolation() {
    const QString fixture = QStringLiteral(R"HTML(
<!doctype html><meta charset="utf-8"><title>Private fixture</title>
<body><p id="cookie"></p><script>document.getElementById('cookie').textContent='cookie:'+document.cookie;</script></body>
)HTML");
    loadHtml(*privatePage_, fixture, QUrl(QStringLiteral("https://fixture.yobro.invalid/private")), [this](bool ok) {
        if (!ok) {
            fail(QStringLiteral("Could not load the private fixture."));
            return;
        }
        privatePage_->readPlainText([this](QString text) {
            if (text.contains(QStringLiteral("yobro_shared=phase1b"))) {
                fail(QStringLiteral("The private profile received a persistent-profile cookie."));
                return;
            }
            verifySecondProfileIsolation();
        });
    });
}

void SmokeRunner::verifySecondProfileIsolation() {
    const QString fixture = QStringLiteral(R"HTML(
<!doctype html><meta charset="utf-8"><title>Second profile fixture</title>
<body><p id="cookie"></p><script>document.getElementById('cookie').textContent='cookie:'+document.cookie;</script></body>
)HTML");
    loadHtml(*secondaryPage_, fixture, QUrl(QStringLiteral("https://fixture.yobro.invalid/secondary")), [this](bool ok) {
        if (!ok) {
            fail(QStringLiteral("Could not load the second-profile fixture."));
            return;
        }
        secondaryPage_->readPlainText([this](QString text) {
            if (text.contains(QStringLiteral("yobro_shared=phase1b"))) {
                fail(QStringLiteral("The second named profile received the primary profile's cookie."));
                return;
            }
            runRepeatedJourney();
        });
    });
}

void SmokeRunner::runRepeatedJourney() {
    if (finished_)
        return;
    if (journeyIteration_ >= repeatedJourneyCount) {
        qInfo("CHROMIUM SMOKE: 100 repeated ownership/navigation journeys passed");
        runLifecycleNavigation();
        return;
    }

    userPage_->readAgentSnapshot([this](std::string json, std::optional<engine::EngineError> error) {
        if (!json.empty() || !error) {
            fail(QStringLiteral("Journey %1: user-page ownership guard failed.").arg(journeyIteration_ + 1));
            return;
        }
        const int number = journeyIteration_ + 1;
        const QString html = QStringLiteral(R"HTML(
<!doctype html><meta charset="utf-8"><title>Journey %1</title>
<body><label>Journey Field <input aria-label="Journey Field"></label>
<button aria-label="Journey Confirm" onclick="document.getElementById('result').textContent='clicked-%1'">Confirm</button>
<p id="result">waiting</p></body>
)HTML").arg(number);
        loadHtml(
            *agentPage_,
            html,
            QUrl(QStringLiteral("https://journey.yobro.invalid/%1").arg(number)),
            [this](bool ok) {
                if (!ok) {
                    fail(QStringLiteral("Journey %1: fixture load failed.").arg(journeyIteration_ + 1));
                    return;
                }
                agentPage_->readAgentSnapshot([this](std::string snapshot, std::optional<engine::EngineError> snapshotError) {
                    verifyJourneySnapshot(std::move(snapshot), std::move(snapshotError));
                });
            }
        );
    });
}

void SmokeRunner::verifyJourneySnapshot(std::string json, std::optional<engine::EngineError> error) {
    const int number = journeyIteration_ + 1;
    if (error) {
        fail(QStringLiteral("Journey %1 read failed: ").arg(number) + QString::fromStdString(*error));
        return;
    }
    QString parseFailure;
    const auto snapshot = objectFrom(json, &parseFailure);
    if (!snapshot) {
        fail(QStringLiteral("Journey %1: ").arg(number) + parseFailure);
        return;
    }
    const QString document = snapshot->value(QStringLiteral("document")).toString();
    const QJsonArray elements = snapshot->value(QStringLiteral("elements")).toArray();
    const QString field = findReference(elements, QStringLiteral("Journey Field"));
    const QString button = findReference(elements, QStringLiteral("Journey Confirm"));
    if (document.isEmpty() || document == previousJourneyDocumentId_ || field.isEmpty() || button.isEmpty()) {
        fail(QStringLiteral("Journey %1: document rotation or references are invalid.").arg(number));
        return;
    }
    previousJourneyDocumentId_ = document;
    documentId_ = document;
    fieldRef_ = field;
    buttonRef_ = button;
    const QString expectedValue = QStringLiteral("journey-%1").arg(number);

    agentPage_->performAgentAction(
        compact(actionRequest(QStringLiteral("fill"), document, field, expectedValue)),
        [this, document, button](std::string, std::optional<engine::EngineError> fillError) {
            if (fillError) {
                fail(QStringLiteral("Journey %1 fill failed: ").arg(journeyIteration_ + 1) + QString::fromStdString(*fillError));
                return;
            }
            agentPage_->performAgentAction(
                compact(actionRequest(QStringLiteral("click"), document, button)),
                [this](std::string, std::optional<engine::EngineError> clickError) {
                    if (clickError) {
                        fail(QStringLiteral("Journey %1 click failed: ").arg(journeyIteration_ + 1) + QString::fromStdString(*clickError));
                        return;
                    }
                    agentPage_->readAgentSnapshot([this](std::string result, std::optional<engine::EngineError> resultError) {
                        verifyJourneyResult(std::move(result), std::move(resultError));
                    });
                }
            );
        }
    );
}

void SmokeRunner::verifyJourneyResult(std::string json, std::optional<engine::EngineError> error) {
    const int number = journeyIteration_ + 1;
    if (error) {
        fail(QStringLiteral("Journey %1 reread failed: ").arg(number) + QString::fromStdString(*error));
        return;
    }
    QString parseFailure;
    const auto snapshot = objectFrom(json, &parseFailure);
    if (!snapshot) {
        fail(QStringLiteral("Journey %1: ").arg(number) + parseFailure);
        return;
    }
    QString value;
    findReference(snapshot->value(QStringLiteral("elements")).toArray(), QStringLiteral("Journey Field"), &value);
    if (value != QStringLiteral("journey-%1").arg(number)
        || !snapshot->value(QStringLiteral("text")).toString().contains(QStringLiteral("clicked-%1").arg(number))) {
        fail(QStringLiteral("Journey %1 did not preserve fill/click/reread behavior.").arg(number));
        return;
    }
    ++journeyIteration_;
    QTimer::singleShot(0, this, [this] { runRepeatedJourney(); });
}

void SmokeRunner::runLifecycleNavigation() {
    qInfo("CHROMIUM SMOKE: testing loopback navigation/history/back/forward/reload/redirect");
    navigateLifecycleA();
}

void SmokeRunner::navigateLifecycleA() {
    const QUrl expected = fixtureUrl(QStringLiteral("/history-a"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this, expected] { (void)lifecyclePage_->navigate(expected.toString().toStdString()); },
        [this](bool ok) {
            if (!ok || lifecyclePage_->state().title != "History A" || lifecyclePage_->state().loading) {
                fail(QStringLiteral("Lifecycle navigation to page A failed."));
                return;
            }
            navigateLifecycleB();
        }
    );
}

void SmokeRunner::navigateLifecycleB() {
    const QUrl expected = fixtureUrl(QStringLiteral("/history-b"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this, expected] { (void)lifecyclePage_->navigate(expected.toString().toStdString()); },
        [this](bool ok) {
            const auto state = lifecyclePage_->state();
            if (!ok || state.title != "History B" || !state.canGoBack || state.loading || state.error) {
                fail(QStringLiteral("Lifecycle navigation to page B or back-history state failed."));
                return;
            }
            goBackLifecycleA();
        }
    );
}

void SmokeRunner::goBackLifecycleA() {
    const QUrl expected = fixtureUrl(QStringLiteral("/history-a"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this] { (void)lifecyclePage_->goBack(); },
        [this](bool ok) {
            const auto state = lifecyclePage_->state();
            if (!ok || state.title != "History A" || !state.canGoForward || state.loading || state.error) {
                fail(QStringLiteral("Lifecycle back navigation or forward-history state failed."));
                return;
            }
            goForwardLifecycleB();
        }
    );
}

void SmokeRunner::goForwardLifecycleB() {
    const QUrl expected = fixtureUrl(QStringLiteral("/history-b"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this] { (void)lifecyclePage_->goForward(); },
        [this](bool ok) {
            const auto state = lifecyclePage_->state();
            if (!ok || state.title != "History B" || !state.canGoBack || state.loading || state.error) {
                fail(QStringLiteral("Lifecycle forward navigation failed."));
                return;
            }
            reloadLifecycleB();
        }
    );
}

void SmokeRunner::reloadLifecycleB() {
    const QUrl expected = fixtureUrl(QStringLiteral("/history-b"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this] { (void)lifecyclePage_->reload(); },
        [this](bool ok) {
            const auto state = lifecyclePage_->state();
            if (!ok || state.title != "History B" || state.loading || state.error) {
                fail(QStringLiteral("Lifecycle reload failed."));
                return;
            }
            followLifecycleRedirect();
        }
    );
}

void SmokeRunner::followLifecycleRedirect() {
    const QUrl triggerUrl = fixtureUrl(QStringLiteral("/redirect"));
    const QUrl expected = fixtureUrl(QStringLiteral("/history-b"));
    waitForLoad(
        *lifecyclePage_,
        expected,
        [this, triggerUrl] { (void)lifecyclePage_->navigate(triggerUrl.toString().toStdString()); },
        [this](bool ok) {
            const auto state = lifecyclePage_->state();
            if (!ok || state.title != "History B" || state.loading || state.error) {
                fail(QStringLiteral("Lifecycle redirect failed."));
                return;
            }
            succeed();
        }
    );
}

void SmokeRunner::fail(const QString &message) {
    if (finished_)
        return;
    qCritical().noquote() << "CHROMIUM SMOKE FAIL:" << message;
    finish(1);
}

void SmokeRunner::succeed() {
    if (finished_)
        return;
    qInfo("CHROMIUM SMOKE PASS: two named profiles, private isolation, ownership guards, shadow/iframe policy, 100 journeys, and lifecycle navigation");
    finish(0);
}

void SmokeRunner::finish(int code) {
    if (finished_)
        return;
    finished_ = true;
    completion_(code);
}

} // namespace yobro::spike
