#include <QCoreApplication>
#include <QElapsedTimer>
#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/SpaceChat.hpp"
#include "spike/SpaceChatPanel.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTest>

#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using yobro::spike::ChatConnection;
using yobro::spike::ChatProvider;
using yobro::spike::ChatProviderPresets;
using yobro::spike::SpaceChatEntry;
using yobro::spike::SpaceChatRunner;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct ChatApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    ChatApp() : paths(yobro::core::ProfilePaths::forProfile("space-chat")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        // Leftovers from an earlier run would make the counts wrong. Only these
        // two named files are removed, never a directory.
        QFile::remove(QString::fromStdString((paths.profile / "space-chat.json").string()));
        QFile::remove(QString::fromStdString(paths.session.string()));
        profile = engine.openProfile({
            .id = "space-chat",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the assistant test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "space-chat",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
    }

    ~ChatApp() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

/// Answers queued replies without touching the network.
class FakeTransport {
public:
    std::deque<QByteArray> replies;
    QList<QJsonObject> requests;

    SpaceChatRunner::Transport handler() {
        return [this](const QUrl &, const QJsonObject &, const QByteArray &body,
                      std::function<void(int, QByteArray)> done) {
            requests.append(QJsonDocument::fromJson(body).object());
            QByteArray reply = QByteArrayLiteral(R"({"choices":[{"message":{"content":"ok"}}]})");
            if (!replies.empty()) {
                reply = replies.front();
                replies.pop_front();
            }
            done(200, reply);
        };
    }
};

QByteArray reply(const QString &content, const QJsonArray &calls = {}) {
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    message.insert(QStringLiteral("content"), content);
    if (!calls.isEmpty()) message.insert(QStringLiteral("tool_calls"), calls);
    QJsonObject choice;
    choice.insert(QStringLiteral("message"), message);
    QJsonObject root;
    root.insert(QStringLiteral("choices"), QJsonArray{choice});
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject call(const QString &id, const QString &name, const QJsonObject &arguments) {
    QJsonObject function;
    function.insert(QStringLiteral("name"), name);
    function.insert(QStringLiteral("arguments"),
                    QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
    QJsonObject item;
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("type"), QStringLiteral("function"));
    item.insert(QStringLiteral("function"), function);
    return item;
}

/// The visible chat lines, without the speaker prefixes.
QStringList timelineTexts(const SpikeWindow &window) {
    QStringList texts;
    auto *list = window.findChild<QListWidget *>(QStringLiteral("spaceChatTimeline"));
    if (!list) return texts;
    for (int index = 0; index < list->count(); ++index) texts.append(list->item(index)->text());
    return texts;
}

void checkPanelExists(const SpikeWindow &window) {
    for (const QString &name : {
             QStringLiteral("spaceChatPanel"), QStringLiteral("spaceChatTimeline"),
             QStringLiteral("spaceChatInput"), QStringLiteral("spaceChatSendButton"),
             QStringLiteral("spaceChatStopButton"), QStringLiteral("spaceChatCircleButton"),
             QStringLiteral("spaceChatProviderPicker"),
             QStringLiteral("spaceChatEndpointField"), QStringLiteral("spaceChatModelPicker"),
             QStringLiteral("spaceChatKeyField"), QStringLiteral("spaceChatSaveButton"),
             QStringLiteral("spaceChatForgetButton"), QStringLiteral("spaceChatStatus"),
         }) {
        check(window.findChild<QWidget *>(name) != nullptr,
              "The assistant surface is missing " + name.toStdString() + ".");
    }
    // The key field must never show what is typed into it.
    auto *keyField = window.findChild<QLineEdit *>(QStringLiteral("spaceChatKeyField"));
    check(keyField->echoMode() == QLineEdit::Password, "The API key field shows the key.");
    // Every provider preset has to be offered.
    auto *picker = window.findChild<QComboBox *>(QStringLiteral("spaceChatProviderPicker"));
    check(picker->count() == static_cast<int>(ChatProviderPresets::all().size()),
          "Not every provider is offered.");
    // The connection details start hidden, behind their own button.
    check(!window.findChild<QWidget *>(QStringLiteral("spaceChatConnectionPane"))->isVisibleTo(
              window.findChild<QWidget *>(QStringLiteral("spaceChatPanel"))),
          "The connection settings are shown before they are asked for.");
}

void sendPrompt(SpikeWindow &window, const QString &prompt) {
    auto *input = window.findChild<QPlainTextEdit *>(QStringLiteral("spaceChatInput"));
    auto *button = window.findChild<QPushButton *>(QStringLiteral("spaceChatSendButton"));
    input->setPlainText(prompt);
    button->click();
}

void checkNoteTools(ChatApp &app, FakeTransport &transport) {
    SpikeWindow &window = *app.window;
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("browserTabs"));
    const int tabsBefore = tabs->count();

    transport.replies.push_back(reply(QStringLiteral("Ich lege sie an."), QJsonArray{
        call(QStringLiteral("c1"), QStringLiteral("create_note"), QJsonObject{
            {QStringLiteral("title"), QStringLiteral("Einkauf")},
            {QStringLiteral("content"), QStringLiteral("Milch")},
        }),
    }));
    transport.replies.push_back(reply(QStringLiteral("Fertig."), QJsonArray{
        call(QStringLiteral("c2"), QStringLiteral("list_notes"), {}),
    }));
    transport.replies.push_back(reply(QStringLiteral("Die Notiz heißt Einkauf.")));
    sendPrompt(window, QStringLiteral("Notiere Milch."));

    // The reworked panel settles the reply and its tool calls on the event
    // loop, so pump until the note tab appears instead of assuming a
    // synchronous send.
    QElapsedTimer noteDeadline; noteDeadline.start();
    while (tabs->count() != tabsBefore + 1 && noteDeadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(tabs->count() == tabsBefore + 1, "The assistant did not open a note tab.");
    check(tabs->tabText(tabs->count() - 1) == QStringLiteral("Einkauf"),
          "The note tab does not carry the title the assistant gave it.");
    const QJsonArray notes = window.listNotes();
    check(notes.size() == 1, "The note is not listed for this Space.");
    const QString noteId = notes.first().toObject().value(QStringLiteral("id")).toString();
    const auto stored = window.readNote(noteId);
    check(stored.has_value(), "The created note cannot be read back.");
    check(stored->value(QStringLiteral("content")).toString() == QStringLiteral("Milch"),
          "The note content was not written.");

    // The list the assistant received has to name the note it just created.
    const QJsonArray lastMessages = transport.requests.at(2).value(QStringLiteral("messages")).toArray();
    check(lastMessages.last().toObject().value(QStringLiteral("content")).toString()
              .contains(QStringLiteral("Einkauf")),
          "The note list sent back to the model does not contain the note.");

    // Appending keeps what was there; replacing is only done when asked for.
    check(window.writeNote(noteId, QStringLiteral("Brot"), QStringLiteral("append")),
          "The note could not be appended to.");
    check(window.readNote(noteId)->value(QStringLiteral("content")).toString()
              == QStringLiteral("Milch\nBrot"),
          "Appending replaced the note instead of extending it.");
    check(window.writeNote(noteId, QStringLiteral("Nur das"), QStringLiteral("replace")),
          "The note could not be replaced.");
    check(window.readNote(noteId)->value(QStringLiteral("content")).toString() == QStringLiteral("Nur das"),
          "Replacing did not replace the note.");
    check(!window.writeNote(QStringLiteral("note-does-not-exist"), QStringLiteral("x"),
                            QStringLiteral("append")),
          "An unknown note was written to.");
    check(!window.readNote(QStringLiteral("note-does-not-exist")).has_value(),
          "An unknown note was read.");

    // Text from the model is data: angle brackets stay angle brackets.
    check(window.writeNote(noteId, QStringLiteral("<b>fett</b>"), QStringLiteral("replace")),
          "The note could not be written.");
    check(window.readNote(noteId)->value(QStringLiteral("content")).toString()
              == QStringLiteral("<b>fett</b>"),
          "Model output was interpreted as markup instead of text.");

    const QStringList timeline = timelineTexts(window);
    // The question plus one line per answer the model produced along the way.
    check(timeline.size() == 4,
          "The timeline should show the question and all three answers, got "
          + std::to_string(timeline.size()) + ".");
    check(timeline.first().contains(QStringLiteral("Notiere Milch.")), "The question is not shown.");
    check(timeline.last().contains(QStringLiteral("Die Notiz heißt Einkauf.")),
          "The final answer is not shown.");
}

void checkSpaceIsolation(ChatApp &app) {
    SpikeWindow &window = *app.window;
    check(window.space() == QStringLiteral("Personal"), "The assistant reports the wrong Space.");
    check(!window.listNotes().isEmpty(), "The fixture has no note in the starting Space.");

    // A line of another Space must not appear in this Space's timeline.
    const int shown = timelineTexts(window).size();
    window.assistantStore().append(
        QStringLiteral("user"), QStringLiteral("Gehört woandershin"), QStringLiteral("Freizeit"));
    window.findChild<yobro::spike::SpaceChatPanel *>(QStringLiteral("spaceChatPanel"))->refresh();
    check(timelineTexts(window).size() == shown,
          "A conversation line from another Space showed up in this timeline.");
    check(window.assistantStore().entries(QStringLiteral("Freizeit")).size() == 1,
          "The line was not stored under its own Space.");
}

void checkInventory(ChatApp &app) {
    SpikeWindow &window = *app.window;
    const QJsonArray inventory = window.inventory();
    check(!inventory.isEmpty(), "The inventory is empty although a tab and a note exist.");
    bool sawNote = false;
    for (const QJsonValue &value : inventory) {
        const QJsonObject item = value.toObject();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        check(kind == QStringLiteral("TAB") || kind == QStringLiteral("NOTE"),
              "The inventory carries an unknown kind: " + kind.toStdString());
        if (kind == QStringLiteral("NOTE")) sawNote = true;
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("url")})
            check(item.contains(field), "The inventory is missing " + field.toStdString() + ".");
    }
    check(sawNote, "The note is missing from the inventory.");

    // A private tab must not appear, not even by address.
    auto *privateButton = window.findChild<QPushButton *>(QStringLiteral("sidebarPrivateTabButton"));
    check(privateButton != nullptr, "The private-tab button is missing.");
    const int before = window.inventory().size();
    privateButton->click();
    QTest::qWait(200);
    check(window.inventory().size() == before,
          "A private tab showed up in the list the assistant is given.");
}

void checkApprovalRefusal(ChatApp &app, FakeTransport &transport) {
    SpikeWindow &window = *app.window;
    // With the grant-capable surface switched off there is no dialog, so the
    // request has to come back as declined rather than opening anything.
    window.setPermissionSurfaceAllowed(false);
    transport.replies.push_back(reply(QString(), QJsonArray{
        call(QStringLiteral("o1"), QStringLiteral("open_url"), QJsonObject{
            {QStringLiteral("url"), QStringLiteral("https://example.com/a")},
        }),
    }));
    transport.replies.push_back(reply(QStringLiteral("Verstanden.")));
    const int requestsBefore = transport.requests.size();
    sendPrompt(window, QStringLiteral("Öffne example.com"));

    check(transport.requests.size() == requestsBefore + 2, "The run did not continue after the refusal.");
    const QJsonArray messages = transport.requests.last().value(QStringLiteral("messages")).toArray();
    check(messages.last().toObject().value(QStringLiteral("content")).toString()
              .contains(QStringLiteral("declined")),
          "The refusal was not passed back to the model.");
    window.setPermissionSurfaceAllowed(true);
}

void checkPersistence(ChatApp &app) {
    const QString path = QString::fromStdString((app.paths.profile / "space-chat.json").string());
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "The conversation was not written to the profile.");
    const QByteArray raw = file.readAll();
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    check(document.isArray(), "The conversation file is not a list of lines.");
    check(!document.array().isEmpty(), "The conversation file is empty.");
    // The key lives in the Keychain; it must not be anywhere in this file.
    check(!QString::fromUtf8(raw).contains(QStringLiteral("sk-test")),
          "The API key was written into the conversation file.");
    for (const QJsonValue &value : document.array()) {
        const QJsonObject entry = value.toObject();
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("date"),
                                     QStringLiteral("space"), QStringLiteral("kind"),
                                     QStringLiteral("text")}) {
            check(entry.contains(field),
                  "A stored line is missing " + field.toStdString() + ".");
        }
    }
}

void checkNewChat(ChatApp &app) {
    SpikeWindow &window = *app.window;
    check(!timelineTexts(window).isEmpty(), "There is nothing to clear.");
    window.findChild<QPushButton *>(QStringLiteral("spaceChatCircleButton"))->click();
    check(timelineTexts(window).isEmpty(), "A new chat did not clear the timeline.");
    check(window.assistantStore().entries(window.space()).empty(),
          "A new chat did not clear the stored conversation.");
    // The notes the assistant created stay: a new chat is not a cleanup.
    check(!window.listNotes().isEmpty(), "A new chat removed the notes.");
}

void checkDisabledAgent(ChatApp &app, FakeTransport &transport) {
    SpikeWindow &window = *app.window;
    app.session->setAgentEnabled(false);
    check(!window.agentAllowed(), "The assistant is still allowed with the agent switched off.");
    const int before = transport.requests.size();
    sendPrompt(window, QStringLiteral("Bist du da?"));
    check(transport.requests.size() == before, "A request was sent with the agent switched off.");
    check(window.assistantStore().entries(window.space()).empty(),
          "A refused send was recorded anyway.");
    app.session->setAgentEnabled(true);
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    try {
        ChatApp app;
        SpikeWindow &window = *app.window;
        checkPanelExists(window);

        // The connection is set directly: saving it through the panel would put
        // a key into the Keychain, which this test has no business doing.
        ChatConnection connection;
        connection.endpoint = ChatProviderPresets::endpoint(ChatProvider::openAI);
        connection.model = QStringLiteral("gpt-test");
        window.assistantStore().setConnection(connection);
        window.assistantStore().setApiKey(QStringLiteral("sk-test"));

        FakeTransport transport;
        check(window.assistantRunner() != nullptr, "The window has no assistant runner.");
        window.assistantRunner()->setTransport(transport.handler());

        checkNoteTools(app, transport);
        checkSpaceIsolation(app);
        checkInventory(app);
        checkApprovalRefusal(app, transport);
        checkPersistence(app);
        checkNewChat(app);
        checkDisabledAgent(app, transport);
    } catch (const std::exception &error) {
        std::cerr << "SPACE CHAT UI FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "SPACE CHAT UI PASS\n";
    return 0;
}
