// Screen sharing. The WebKit build gets a picker from WebKit itself; Qt hands
// over two lists of sources and expects exactly one answer, so the picker is
// ours and it has to be right: an unanswered request leaves the page waiting,
// and a wrong answer shares the wrong surface.
#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "spike/SpikeWindowInternal.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEngineView>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace std::chrono_literals;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout.count()) {
        if (predicate()) return true;
        QTest::qWait(10);
    }
    return predicate();
}

struct Answer {
    bool selected = false;
    bool cancelled = false;
    bool window = false;
    int index = -1;
    /// What select() reports back to the shell.
    bool selectSucceeds = true;
};

yobro::engine::DesktopMediaControls controlsFor(Answer &answer) {
    yobro::engine::DesktopMediaControls controls;
    controls.select = [&answer](bool window, int index) {
        if (!answer.selectSucceeds) return false;
        answer.selected = true;
        answer.window = window;
        answer.index = index;
        return true;
    };
    controls.cancel = [&answer] { answer.cancelled = true; };
    return controls;
}

yobro::engine::DesktopMediaRequest twoScreensAndAWindow() {
    return {
        .origin = "https://meet.example.test",
        .sources = {
            {.window = false, .index = 0, .name = "Built-in Display"},
            {.window = false, .index = 1, .name = "External Display"},
            {.window = true, .index = 0, .name = "Notes"},
            {.window = true, .index = 1, .name = ""},
        },
    };
}

struct ShareApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    ShareApp() : paths(yobro::core::ProfilePaths::forProfile("screen-share")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        QFile::remove(QString::fromStdString(paths.session.string()));
        profile = engine.openProfile({
            .id = "screen-share",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the screen-share test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "screen-share",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->resize(900, 640);
        window->show();
    }
    ~ShareApp() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

QDialog *visiblePicker(SpikeWindow &window) {
    for (QDialog *dialog : window.findChildren<QDialog *>(QStringLiteral("desktopMediaDialog"))) {
        if (dialog->isVisible()) return dialog;
    }
    return nullptr;
}

/// The picker blocks until it has an answer, so the clicks are scheduled first.
void whileOpen(SpikeWindow &window, const std::function<void(QDialog &)> &act) {
    auto *poll = new QTimer(&window);
    poll->setInterval(20);
    QObject::connect(poll, &QTimer::timeout, poll, [&window, act, poll] {
        QDialog *dialog = visiblePicker(window);
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        act(*dialog);
    });
    poll->start();
}

void checkPickerOffersEverySource(ShareApp &app) {
    Answer answer;
    QStringList labels;
    int rows = 0;
    whileOpen(*app.window, [&labels, &rows](QDialog &dialog) {
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("desktopMediaList"));
        if (list) {
            rows = list->count();
            for (int index = 0; index < list->count(); ++index)
                labels.append(list->item(index)->text());
        }
        auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaCancelButton"));
        if (cancel) QTest::mouseClick(cancel, Qt::LeftButton);
    });
    app.window->presentDesktopMediaPicker(twoScreensAndAWindow(), controlsFor(answer));
    check(rows == 4, "The picker did not offer every source: " + std::to_string(rows));
    check(labels.at(0).contains(QStringLiteral("Built-in Display")), "The first screen is missing its name.");
    check(labels.at(0).startsWith(QStringLiteral("Bildschirm:")), "A screen was not labelled as a screen.");
    check(labels.at(2).startsWith(QStringLiteral("Fenster:")), "A window was not labelled as a window.");
    check(labels.at(3).contains(QStringLiteral("Ohne Namen")), "An unnamed window has no readable label.");
    check(answer.cancelled && !answer.selected, "Declining the picker did not cancel the request.");
}

void checkCancelIsTheDefault(ShareApp &app) {
    Answer answer;
    bool cancelIsDefault = false;
    bool shareIsDefault = true;
    whileOpen(*app.window, [&](QDialog &dialog) {
        auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaCancelButton"));
        auto *share = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaShareButton"));
        if (cancel) cancelIsDefault = cancel->isDefault();
        if (share) shareIsDefault = share->isDefault();
        // Escape must count as "do not share".
        QTest::keyClick(&dialog, Qt::Key_Escape);
    });
    app.window->presentDesktopMediaPicker(twoScreensAndAWindow(), controlsFor(answer));
    check(cancelIsDefault, "Declining is not the default button.");
    check(!shareIsDefault, "Sharing is the default button.");
    check(answer.cancelled && !answer.selected, "Escape did not cancel the request.");
}

void checkSharingPassesTheChosenSource(ShareApp &app) {
    // Third row: the first window, index 0.
    Answer answer;
    whileOpen(*app.window, [](QDialog &dialog) {
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("desktopMediaList"));
        if (list) list->setCurrentRow(2);
        auto *share = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaShareButton"));
        if (share) QTest::mouseClick(share, Qt::LeftButton);
    });
    app.window->presentDesktopMediaPicker(twoScreensAndAWindow(), controlsFor(answer));
    check(answer.selected && !answer.cancelled, "Sharing did not answer the request.");
    check(answer.window, "A window was reported as a screen.");
    check(answer.index == 0, "The window index was wrong: " + std::to_string(answer.index));

    // Second row: the second screen, index 1.
    Answer second;
    whileOpen(*app.window, [](QDialog &dialog) {
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("desktopMediaList"));
        if (list) list->setCurrentRow(1);
        auto *share = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaShareButton"));
        if (share) QTest::mouseClick(share, Qt::LeftButton);
    });
    app.window->presentDesktopMediaPicker(twoScreensAndAWindow(), controlsFor(second));
    check(second.selected && !second.window && second.index == 1, "The chosen screen was not passed through.");
}

void checkVanishedSourceIsCancelled(ShareApp &app) {
    Answer answer;
    answer.selectSucceeds = false;
    whileOpen(*app.window, [](QDialog &dialog) {
        auto *share = dialog.findChild<QPushButton *>(QStringLiteral("desktopMediaShareButton"));
        if (share) QTest::mouseClick(share, Qt::LeftButton);
    });
    app.window->presentDesktopMediaPicker(twoScreensAndAWindow(), controlsFor(answer));
    check(answer.cancelled, "A source that disappeared did not end the request.");
    check(!answer.selected, "A refused selection was reported as successful.");
}

void checkEmptyRequestIsCancelledWithoutADialog(ShareApp &app) {
    Answer answer;
    bool sawDialog = false;
    auto *poll = new QTimer(app.window.get());
    poll->setInterval(10);
    QObject::connect(poll, &QTimer::timeout, poll, [&app, &sawDialog] {
        if (visiblePicker(*app.window)) sawDialog = true;
    });
    poll->start();
    app.window->presentDesktopMediaPicker({.origin = "https://meet.example.test", .sources = {}}, controlsFor(answer));
    QTest::qWait(120);
    poll->stop();
    poll->deleteLater();
    check(answer.cancelled, "An empty source list did not cancel the request.");
    check(!sawDialog, "An empty source list still opened a picker.");
}

/// What the engine really does. The picker above is only worth having if Qt asks
/// for it, so this drives a real page over a real click.
void checkEngineAsksForAPicker(ShareApp &app, const QString &fixtureUrl) {
    auto *address = app.window->findChild<QLineEdit *>(QStringLiteral("topAddress"));
    check(address != nullptr, "The window has no address field.");
    address->setText(fixtureUrl);
    Q_EMIT address->returnPressed();
    check(
        waitFor([&app] {
            auto *page = app.window->visibleUserPage();
            return page && !page->state().loading
                && QString::fromStdString(page->state().url).startsWith(QStringLiteral("http://127.0.0.1"));
        }, 30s),
        "The fixture page did not load."
    );
    auto *page = app.window->visibleUserPage();
    check(page != nullptr, "The fixture page disappeared.");

    // Answer whatever the engine asks for, and remember that it asked.
    bool asked = false;
    std::size_t offered = 0;
    page->setDesktopMediaHandler(
        [&asked, &offered](
            const yobro::engine::DesktopMediaRequest &request,
            const yobro::engine::DesktopMediaControls &controls
        ) {
            asked = true;
            offered = request.sources.size();
            if (controls.cancel) controls.cancel();
        }
    );
    // getDisplayMedia needs a real user gesture.
    page->view()->setFocus(Qt::OtherFocusReason);
    QTest::qWait(50);
    QWidget *target = page->view()->focusProxy();
    if (!target) target = page->view();
    target->setFocus(Qt::OtherFocusReason);
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, QPoint(160, 25), 20);
    check(
        waitFor([&asked] { return asked; }, 15s),
        "The engine never asked for a picker. Measured on Qt 6.11.2 it does, even offscreen; "
        "if this machine denies screen recording to this binary, Chromium stops before the request."
    );
    check(offered > 0, "The engine asked for a picker without offering a single source.");
    std::cout << "QT SCREEN SHARE: the engine asked for a picker with " << offered << " source(s)\n";
    // Declining has to reach the page, otherwise a site waits forever for a
    // promise that will never settle.
    check(
        waitFor([page] {
            return QString::fromStdString(page->state().title).startsWith(QStringLiteral("refused:"));
        }, 15s),
        "Declining the picker did not reject the page's promise; the title stayed "
            + page->state().title
    );
    page->setDesktopMediaHandler({});
}

/// The two new permission types have to reach the prompt with words of their
/// own; a screen share must not be announced as "media devices".
void checkPermissionWording() {
    using yobro::engine::WebPermission;
    namespace support = yobro::spike::windowSupport;
    for (const WebPermission permission : {WebPermission::screenShare, WebPermission::screenShareWithAudio}) {
        const QString device = support::permissionDevice(permission);
        check(device.contains(QStringLiteral("Bildschirm")), "A screen share has no wording of its own.");
        check(support::isMediaPermission(permission), "A screen share is not treated as a capture.");
        check(!support::permissionCaution(permission).isEmpty(), "A screen share carries no caution.");
    }
    check(
        support::permissionDevice(WebPermission::screenShareWithAudio)
            != support::permissionDevice(WebPermission::screenShare),
        "Sharing with audio is announced exactly like sharing without it."
    );
}

void serveFixture(QTcpServer &server) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        QTcpSocket *socket = server.nextPendingConnection();
        if (!socket) return;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            if (!socket->readAll().contains("\r\n\r\n")) return;
            const QByteArray body = QByteArrayLiteral(
                "<!doctype html><meta charset=\"utf-8\"><title>Share Fixture</title>"
                "<style>html,body{margin:0}button{display:block;width:320px;height:50px;font-size:16px}</style>"
                "<button id=\"go\">Share</button>"
                "<script>document.getElementById('go').addEventListener('click',()=>{"
                "navigator.mediaDevices.getDisplayMedia({video:true}).then(()=>{document.title='shared'})"
                ".catch(e=>{document.title='refused:'+e.name});});</script>"
            );
            QByteArray response = QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
            ) + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QTemporaryDir home(QStringLiteral("/tmp/yobro-share-XXXXXX"));
    if (!home.isValid()) {
        std::cerr << "QT SCREEN SHARE FAIL: Could not create an isolated home.\n";
        return 1;
    }
    qputenv("YOBRO_CHROMIUM_HOME", home.path().toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("YOBRO"));
    QCoreApplication::setApplicationName(QStringLiteral("YOBRO Screen Share"));
    try {
        QTcpServer fixture;
        check(fixture.listen(QHostAddress::LocalHost, 0), "Could not start the fixture server.");
        serveFixture(fixture);
        const QString fixtureUrl =
            QStringLiteral("http://127.0.0.1:%1/share").arg(fixture.serverPort());
        ShareApp app;
        checkPermissionWording();
        checkPickerOffersEverySource(app);
        checkCancelIsTheDefault(app);
        checkSharingPassesTheChosenSource(app);
        checkVanishedSourceIsCancelled(app);
        checkEmptyRequestIsCancelledWithoutADialog(app);
        checkEngineAsksForAPicker(app, fixtureUrl);
        std::cout << "QT SCREEN SHARE PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "QT SCREEN SHARE FAIL: " << error.what() << '\n';
        return 1;
    }
}
