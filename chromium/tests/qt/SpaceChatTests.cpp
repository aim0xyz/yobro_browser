#include "spike/SpaceChat.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::AgentApiKeySecrets;
using yobro::spike::ChatConnection;
using yobro::spike::ChatProvider;
using yobro::spike::ChatProviderPresets;
using yobro::spike::SpaceChatEntry;
using yobro::spike::SpaceChatHost;
using yobro::spike::SpaceChatProtocol;
using yobro::spike::SpaceChatRunner;
using yobro::spike::SpaceChatStore;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

QJsonObject parse(const QByteArray &data) { return QJsonDocument::fromJson(data).object(); }

/// Spins the event loop until `ready` turns true or the budget runs out.
void settle(const std::function<bool()> &ready, int milliseconds = 5000) {
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (ready()) loop.quit(); });
    poll.start(5);
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    if (!ready()) loop.exec();
    check(ready(), "The asynchronous step did not finish in time.");
}

// MARK: - Presets and connection

void checkPresets() {
    check(ChatProviderPresets::all().size() == 4, "There are not four providers.");
    check(ChatProviderPresets::endpoint(ChatProvider::openRouter)
              == QStringLiteral("https://openrouter.ai/api/v1/chat/completions"),
          "The OpenRouter endpoint changed.");
    check(ChatProviderPresets::endpoint(ChatProvider::anthropic)
              == QStringLiteral("https://api.anthropic.com/v1/messages"),
          "The Anthropic endpoint changed.");
    check(ChatProviderPresets::endpoint(ChatProvider::openAI)
              == QStringLiteral("https://api.openai.com/v1/chat/completions"),
          "The OpenAI endpoint changed.");
    check(ChatProviderPresets::endpoint(ChatProvider::custom).isEmpty(),
          "A custom provider must not carry a preset endpoint.");
    check(ChatProviderPresets::defaultModel(ChatProvider::openRouter) == QStringLiteral("openrouter/auto"),
          "The OpenRouter default model changed.");

    for (const ChatProvider provider : ChatProviderPresets::all()) {
        if (provider == ChatProvider::custom) continue;
        check(ChatProviderPresets::matching(ChatProviderPresets::endpoint(provider)) == provider,
              "A preset endpoint was not recognised again.");
    }
    // Anything unknown is custom, never silently one of the presets.
    check(ChatProviderPresets::matching(QStringLiteral("https://example.invalid/v1")) == ChatProvider::custom,
          "An unknown endpoint was not treated as custom.");
    check(ChatProviderPresets::matching(QString()) == ChatProvider::custom,
          "An empty endpoint was not treated as custom.");
    check(!ChatProviderPresets::title(ChatProvider::custom).isEmpty(), "The custom provider has no title.");
}

/// The model list must stay in step with the WebKit build, otherwise one shell
/// offers a model the other does not know.
void checkAnthropicModelParity() {
    QFile source(QStringLiteral(YOBRO_WEBKIT_SPACE_CHAT_SOURCE));
    check(source.open(QIODevice::ReadOnly), "The WebKit SpaceChat source could not be read.");
    const QString swift = QString::fromUtf8(source.readAll());
    const int start = swift.indexOf(QStringLiteral("static let anthropicModels"));
    check(start >= 0, "The WebKit model list was not found.");
    const int open = swift.indexOf(QLatin1Char('['), start);
    const int close = swift.indexOf(QLatin1Char(']'), open);
    check(open > 0 && close > open, "The WebKit model list is not a literal array.");

    QStringList expected;
    for (const QString &piece : swift.mid(open + 1, close - open - 1).split(QLatin1Char(','))) {
        const QString trimmed = piece.trimmed();
        if (trimmed.size() < 2) continue;
        expected.append(trimmed.mid(1, trimmed.size() - 2));
    }
    check(!expected.isEmpty(), "No models were parsed from the WebKit source.");
    check(ChatProviderPresets::anthropicModels() == expected,
          "The Anthropic model list differs from the WebKit build: "
          + ChatProviderPresets::anthropicModels().join(QLatin1Char(',')).toStdString()
          + " vs " + expected.join(QLatin1Char(',')).toStdString());
}

void checkConnectionUrl() {
    const auto accepted = [](const QString &endpoint, const QString &model) {
        ChatConnection connection;
        connection.endpoint = endpoint;
        connection.model = model;
        QString problem;
        const bool ok = connection.url(&problem).has_value();
        if (ok) check(problem.isEmpty(), "An accepted endpoint still reported a problem.");
        else check(!problem.isEmpty(), "A refused endpoint reported no reason.");
        return ok;
    };

    check(accepted(QStringLiteral("https://api.openai.com/v1/chat/completions"), QStringLiteral("gpt")),
          "A plain HTTPS endpoint was refused.");
    // HTTP is only tolerated for a local model server.
    check(accepted(QStringLiteral("http://localhost:1234/v1/chat/completions"), QStringLiteral("m")),
          "A loopback HTTP endpoint was refused.");
    check(accepted(QStringLiteral("http://127.0.0.1:1234/v1"), QStringLiteral("m")),
          "The loopback address was refused.");

    for (const auto &item : QList<QPair<QString, QString>>{
             {QString(), QStringLiteral("m")},                                  // nothing entered
             {QStringLiteral("https://api.example/v1"), QString()},             // no model
             {QStringLiteral("https://api.example/v1"), QStringLiteral("   ")}, // blank model
             {QStringLiteral("http://api.example/v1"), QStringLiteral("m")},    // plain HTTP, remote
             {QStringLiteral("ftp://api.example/v1"), QStringLiteral("m")},
             {QStringLiteral("file:///tmp/x"), QStringLiteral("m")},
             {QStringLiteral("https://user:secret@api.example/v1"), QStringLiteral("m")},
             {QStringLiteral("https://api.example/v1?key=secret"), QStringLiteral("m")},
             {QStringLiteral("https://api.example/v1#frag"), QStringLiteral("m")},
             {QStringLiteral("https:///v1"), QStringLiteral("m")},              // no host
         }) {
        check(!accepted(item.first, item.second),
              "An unsafe endpoint was accepted: " + item.first.toStdString());
    }
}

void checkWebUrl() {
    check(SpaceChatProtocol::webUrl(QStringLiteral("https://example.com/a")).has_value(),
          "An HTTPS page URL was refused.");
    check(SpaceChatProtocol::webUrl(QStringLiteral("http://example.com/a")).has_value(),
          "An HTTP page URL was refused.");
    for (const QString &raw : {
             QString(),
             QStringLiteral("file:///etc/passwd"),
             QStringLiteral("javascript:alert(1)"),
             QStringLiteral("data:text/html,<b>x"),
             QStringLiteral("about:blank"),
             QStringLiteral("https://user:secret@example.com/"),
             QStringLiteral("https:///nohost"),
         }) {
        check(!SpaceChatProtocol::webUrl(raw).has_value(),
              "A URL the agent must not open was accepted: " + raw.toStdString());
    }
}

// MARK: - Tool declarations

void checkTools() {
    const QStringList names = SpaceChatProtocol::toolNames();
    const QStringList expected{
        QStringLiteral("list_notes"), QStringLiteral("create_note"), QStringLiteral("read_note"),
        QStringLiteral("write_note"), QStringLiteral("read_mail"), QStringLiteral("read_mail_message"),
        QStringLiteral("read_tab"), QStringLiteral("open_url"),
    };
    check(names == expected, "The tool list changed: " + names.join(QLatin1Char(',')).toStdString());

    for (const QJsonValue &value : SpaceChatProtocol::tools()) {
        const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
        const QJsonObject parameters = function.value(QStringLiteral("parameters")).toObject();
        check(value.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("function"),
              "A tool is not declared as a function.");
        check(!function.value(QStringLiteral("description")).toString().isEmpty(),
              "A tool has no description.");
        // Unknown arguments must be rejected by the schema, not silently kept.
        check(parameters.value(QStringLiteral("additionalProperties")).toBool(true) == false,
              "A tool schema allows extra properties.");
    }

    const auto schemaFor = [](const QString &name) {
        for (const QJsonValue &value : SpaceChatProtocol::tools()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")).toString() == name)
                return function.value(QStringLiteral("parameters")).toObject();
        }
        fail("A declared tool disappeared: " + name.toStdString());
    };
    check(schemaFor(QStringLiteral("write_note")).value(QStringLiteral("required")).toArray().size() == 3,
          "write_note does not require id, content and mode.");
    check(schemaFor(QStringLiteral("write_note")).value(QStringLiteral("properties")).toObject()
              .value(QStringLiteral("mode")).toObject().value(QStringLiteral("enum")).toArray().size() == 2,
          "write_note does not restrict the mode to append and replace.");
    check(schemaFor(QStringLiteral("list_notes")).value(QStringLiteral("properties")).toObject().isEmpty(),
          "list_notes takes arguments it should not.");
    // A mail read must not be able to ask for the whole mailbox.
    const QJsonObject mailLimit = schemaFor(QStringLiteral("read_mail"))
        .value(QStringLiteral("properties")).toObject().value(QStringLiteral("limit")).toObject();
    check(mailLimit.value(QStringLiteral("maximum")).toInt() == 50
              && mailLimit.value(QStringLiteral("minimum")).toInt() == 1,
          "read_mail does not restrict how many messages it may ask for.");
}

void checkSystemMessage() {
    QJsonObject tab;
    tab.insert(QStringLiteral("id"), QStringLiteral("tab-1"));
    tab.insert(QStringLiteral("kind"), QStringLiteral("TAB"));
    tab.insert(QStringLiteral("title"), QStringLiteral("Wetter"));
    tab.insert(QStringLiteral("url"), QStringLiteral("https://example.com/"));
    QJsonObject note;
    note.insert(QStringLiteral("id"), QStringLiteral("note-1"));
    note.insert(QStringLiteral("kind"), QStringLiteral("NOTE"));
    note.insert(QStringLiteral("title"), QStringLiteral("Einkauf"));
    note.insert(QStringLiteral("url"), QString());

    const QJsonObject message = SpaceChatProtocol::systemMessage(QStringLiteral("Arbeit"), {tab, note});
    check(message.value(QStringLiteral("role")).toString() == QStringLiteral("system"),
          "The system message has the wrong role.");
    const QString text = message.value(QStringLiteral("content")).toString();
    check(text.contains(QStringLiteral("Space Arbeit")), "The Space is not named in the system message.");
    check(text.contains(QStringLiteral("tab-1 TAB Wetter https://example.com/")),
          "The tab inventory is missing.");
    check(text.contains(QStringLiteral("note-1 NOTE Einkauf")), "The note inventory is missing.");
    // Page and note text is data, and the model has to be told so.
    check(text.contains(QStringLiteral("untrusted data")), "The untrusted-data rule is missing.");
    check(text.contains(QStringLiteral("requires user approval")), "The approval rule is missing.");
    for (const QString &name : SpaceChatProtocol::toolNames())
        check(text.contains(name), "A tool is not mentioned in the system message: " + name.toStdString());
}

// MARK: - Wire formats

QJsonObject assistantWithCall(const QString &name, const QString &arguments) {
    QJsonObject function;
    function.insert(QStringLiteral("name"), name);
    function.insert(QStringLiteral("arguments"), arguments);
    QJsonObject call;
    call.insert(QStringLiteral("id"), QStringLiteral("call-1"));
    call.insert(QStringLiteral("type"), QStringLiteral("function"));
    call.insert(QStringLiteral("function"), function);
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    message.insert(QStringLiteral("content"), QStringLiteral("Ich sehe nach."));
    message.insert(QStringLiteral("tool_calls"), QJsonArray{call});
    return message;
}

void checkOpenAiBody() {
    QJsonArray messages{SpaceChatProtocol::systemMessage(QStringLiteral("A"), {})};
    const QJsonObject body = SpaceChatProtocol::requestBody(
        ChatProvider::openAI, QStringLiteral("gpt"), messages);
    check(body.value(QStringLiteral("model")).toString() == QStringLiteral("gpt"), "The model is missing.");
    check(body.value(QStringLiteral("messages")).toArray().size() == 1, "The messages were not passed on.");
    check(body.value(QStringLiteral("tools")).toArray().size() == SpaceChatProtocol::toolNames().size(),
          "The tools were not declared.");
    check(!body.contains(QStringLiteral("max_tokens")),
          "An OpenAI request must not carry the Anthropic token field.");
}

void checkAnthropicBody() {
    QJsonObject toolResult;
    toolResult.insert(QStringLiteral("role"), QStringLiteral("tool"));
    toolResult.insert(QStringLiteral("tool_call_id"), QStringLiteral("call-1"));
    toolResult.insert(QStringLiteral("content"), QStringLiteral("{\"notes\":[]}"));

    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), QStringLiteral("Was steht in meinen Notizen?"));

    const QJsonArray messages{
        SpaceChatProtocol::systemMessage(QStringLiteral("Arbeit"), {}),
        user,
        assistantWithCall(QStringLiteral("list_notes"), QStringLiteral("{}")),
        toolResult,
    };
    const QJsonObject body = SpaceChatProtocol::requestBody(
        ChatProvider::anthropic, QStringLiteral("claude-sonnet-4-6"), messages);

    check(body.value(QStringLiteral("max_tokens")).toInt() == 4096, "The token budget is missing.");
    check(body.value(QStringLiteral("system")).toString().contains(QStringLiteral("Space Arbeit")),
          "The system prompt was not lifted out of the message list.");
    const QJsonArray converted = body.value(QStringLiteral("messages")).toArray();
    check(converted.size() == 3, "The system message was not removed from the message list.");

    check(converted.at(0).toObject().value(QStringLiteral("role")).toString() == QStringLiteral("user"),
          "The user turn was lost.");

    const QJsonObject assistant = converted.at(1).toObject();
    check(assistant.value(QStringLiteral("role")).toString() == QStringLiteral("assistant"),
          "The assistant turn was lost.");
    const QJsonArray blocks = assistant.value(QStringLiteral("content")).toArray();
    check(blocks.size() == 2, "The assistant turn was not split into a text and a tool block.");
    check(blocks.at(0).toObject().value(QStringLiteral("type")).toString() == QStringLiteral("text"),
          "The assistant text block is missing.");
    const QJsonObject use = blocks.at(1).toObject();
    check(use.value(QStringLiteral("type")).toString() == QStringLiteral("tool_use"), "The tool_use block is missing.");
    check(use.value(QStringLiteral("name")).toString() == QStringLiteral("list_notes"), "The tool name was lost.");
    check(use.value(QStringLiteral("input")).isObject(),
          "The arguments were not converted from a string to an object.");

    // A tool answer becomes a user turn carrying a tool_result block.
    const QJsonObject result = converted.at(2).toObject();
    check(result.value(QStringLiteral("role")).toString() == QStringLiteral("user"),
          "A tool result must be sent as a user turn.");
    const QJsonObject resultBlock = result.value(QStringLiteral("content")).toArray().first().toObject();
    check(resultBlock.value(QStringLiteral("type")).toString() == QStringLiteral("tool_result"),
          "The tool_result block is missing.");
    check(resultBlock.value(QStringLiteral("tool_use_id")).toString() == QStringLiteral("call-1"),
          "The tool result was not tied back to its call.");

    const QJsonArray tools = body.value(QStringLiteral("tools")).toArray();
    check(tools.size() == SpaceChatProtocol::toolNames().size(), "Not every tool was converted.");
    check(tools.first().toObject().contains(QStringLiteral("input_schema")),
          "Anthropic tools need an input_schema, not parameters.");
}

void checkProviderMessage() {
    const QByteArray openAi = QByteArrayLiteral(
        R"({"choices":[{"message":{"role":"assistant","content":"Hallo"}}]})");
    const auto message = SpaceChatProtocol::providerMessage(parse(openAi), ChatProvider::openAI);
    check(message.has_value() && message->value(QStringLiteral("content")).toString() == QStringLiteral("Hallo"),
          "An OpenAI answer was not read.");

    const QByteArray anthropic = QByteArrayLiteral(R"({"content":[
        {"type":"text","text":"Ich lese die Notiz."},
        {"type":"tool_use","id":"tu-1","name":"read_note","input":{"id":"note-1"}}
    ]})");
    const auto converted = SpaceChatProtocol::providerMessage(parse(anthropic), ChatProvider::anthropic);
    check(converted.has_value(), "An Anthropic answer was not read.");
    check(converted->value(QStringLiteral("content")).toString() == QStringLiteral("Ich lese die Notiz."),
          "The Anthropic text was lost.");
    const QJsonArray calls = converted->value(QStringLiteral("tool_calls")).toArray();
    check(calls.size() == 1, "The Anthropic tool call was lost.");
    const QJsonObject function = calls.first().toObject().value(QStringLiteral("function")).toObject();
    check(function.value(QStringLiteral("name")).toString() == QStringLiteral("read_note"),
          "The Anthropic tool name was lost.");
    check(parse(function.value(QStringLiteral("arguments")).toString().toUtf8())
              .value(QStringLiteral("id")).toString() == QStringLiteral("note-1"),
          "The Anthropic tool input was not turned back into an argument string.");

    // Malformed answers must be reported, never guessed at.
    check(!SpaceChatProtocol::providerMessage({}, ChatProvider::openAI).has_value(),
          "An empty OpenAI answer was accepted.");
    check(!SpaceChatProtocol::providerMessage(parse(QByteArrayLiteral(R"({"choices":[]})")),
                                              ChatProvider::openAI).has_value(),
          "An OpenAI answer without a choice was accepted.");
    check(!SpaceChatProtocol::providerMessage(parse(QByteArrayLiteral(R"({"choices":[{}]})")),
                                              ChatProvider::openAI).has_value(),
          "An OpenAI choice without a message was accepted.");
    check(!SpaceChatProtocol::providerMessage(parse(QByteArrayLiteral(R"({"content":"text"})")),
                                              ChatProvider::anthropic).has_value(),
          "An Anthropic answer without content blocks was accepted.");
}

void checkProviderError() {
    const QString detailed = SpaceChatProtocol::providerError(
        QByteArrayLiteral(R"({"error":{"message":"insufficient credits"}})"), 402);
    check(detailed.contains(QStringLiteral("insufficient credits")),
          "The provider's own message was dropped.");

    const QString plain = SpaceChatProtocol::providerError(QByteArrayLiteral("<html>bad gateway</html>"), 502);
    check(plain.contains(QStringLiteral("502")), "The status code is missing from the fallback message.");
    check(!plain.contains(QStringLiteral("html")), "A non-JSON body was echoed into the message.");

    // A very long provider message must not flood the timeline.
    const QString flood = QString(2000, QLatin1Char('x'));
    QJsonObject error;
    error.insert(QStringLiteral("message"), flood);
    QJsonObject root;
    root.insert(QStringLiteral("error"), error);
    const QString clipped = SpaceChatProtocol::providerError(
        QJsonDocument(root).toJson(QJsonDocument::Compact), 500);
    check(clipped.size() < 600, "A long provider message was not cut short.");
}

// MARK: - Store

void checkStore(const QString &root) {
    const QString directory = root + QStringLiteral("/store");
    check(QDir().mkpath(directory), "Could not create the store directory.");

    {
        SpaceChatStore store(directory.toStdString());
        check(store.entries().empty(), "A fresh store is not empty.");
        check(!store.loadFailed(), "A fresh store reported a load failure.");

        store.append(QStringLiteral("user"), QStringLiteral("Frage"), QStringLiteral("Arbeit"));
        store.append(QStringLiteral("assistant"), QStringLiteral("Antwort"), QStringLiteral("Arbeit"));
        store.append(QStringLiteral("user"), QStringLiteral("Privat"), QStringLiteral("Freizeit"));
        check(store.entries(QStringLiteral("Arbeit")).size() == 2, "The Space filter does not work.");
        check(store.entries(QStringLiteral("Freizeit")).size() == 1, "The Space filter does not work.");

        // Drafts survive a Space switch; staged context is sent exactly once.
        store.setDraft(QStringLiteral("Arbeit"), QStringLiteral("Entwurf"));
        check(store.draft(QStringLiteral("Arbeit")) == QStringLiteral("Entwurf"), "A draft was not kept.");
        check(store.draft(QStringLiteral("Freizeit")).isEmpty(), "Drafts leaked between Spaces.");
        store.setContext(QStringLiteral("Arbeit"), QStringLiteral("Kontext"));
        check(store.takeContext(QStringLiteral("Arbeit")) == QStringLiteral("Kontext"), "Context was not staged.");
        check(store.takeContext(QStringLiteral("Arbeit")).isEmpty(), "Staged context was sent twice.");

        ChatConnection connection;
        connection.endpoint = ChatProviderPresets::endpoint(ChatProvider::openAI);
        connection.model = QStringLiteral("gpt-test");
        store.setConnection(connection);
        store.append(QStringLiteral("action"), QStringLiteral("Notiz"), QStringLiteral("Arbeit"));
    }

    {
        SpaceChatStore store(directory.toStdString());
        check(store.entries().size() == 4, "The conversation was not persisted.");
        check(store.connection().model == QStringLiteral("gpt-test"), "The connection was not persisted.");
        check(!store.entries().front().id.isEmpty(), "A restored line has no id.");
        check(!store.entries().front().date.isEmpty(), "A restored line has no timestamp.");
        // The API key belongs in the Keychain, never in the JSON file.
        QFile file(directory + QStringLiteral("/space-chat.json"));
        check(file.open(QIODevice::ReadOnly), "The conversation file is missing.");
        check(!QString::fromUtf8(file.readAll()).contains(QStringLiteral("apiKey")),
              "The conversation file has a field for the key.");

        check(store.toggleSaved(store.entries().front().id), "A line could not be marked.");
        check(store.entries().front().saved, "The mark was not applied.");
        check(!store.toggleSaved(QStringLiteral("does-not-exist")), "An unknown id was accepted.");

        store.renameSpace(QStringLiteral("Arbeit"), QStringLiteral("Büro"));
        check(store.entries(QStringLiteral("Arbeit")).empty(), "Lines stayed under the old Space name.");
        check(store.entries(QStringLiteral("Büro")).size() == 3, "Lines were lost during the rename.");

        store.newChat(QStringLiteral("Büro"));
        check(store.entries(QStringLiteral("Büro")).empty(), "A new chat did not clear the Space.");
        check(store.entries(QStringLiteral("Freizeit")).size() == 1, "A new chat cleared another Space.");
    }

    // A damaged conversation must never be overwritten with an empty one.
    const QString broken = root + QStringLiteral("/broken");
    check(QDir().mkpath(broken), "Could not create the damaged-store directory.");
    const QByteArray garbage = QByteArrayLiteral("{ this is not json");
    {
        QFile file(broken + QStringLiteral("/space-chat.json"));
        check(file.open(QIODevice::WriteOnly), "Could not write the damaged file.");
        file.write(garbage);
    }
    {
        SpaceChatStore store(broken.toStdString());
        check(store.loadFailed(), "A damaged conversation was not detected.");
        check(!store.problem().isEmpty(), "A damaged conversation was not reported.");
        store.append(QStringLiteral("user"), QStringLiteral("x"), QStringLiteral("A"));
    }
    QFile still(broken + QStringLiteral("/space-chat.json"));
    check(still.open(QIODevice::ReadOnly), "The damaged file disappeared.");
    check(still.readAll() == garbage, "The damaged conversation file was overwritten.");
}

void checkKeychain(const QString &root) {
    const QString directory = root + QStringLiteral("/keys");
    AgentApiKeySecrets secrets(directory.toStdString());
    if (!secrets.available()) return;
    check(secrets.service().find("AgentApiKey") != std::string::npos,
          "The Keychain service name does not name its purpose.");
    check(secrets.service().find(directory.toStdString()) != std::string::npos,
          "The Keychain service name is not profile specific.");

    const QString endpoint = QStringLiteral("https://api.example.invalid/v1");
    secrets.remove(endpoint);
    check(secrets.read(endpoint).isEmpty(), "A key existed before it was stored.");
    check(secrets.store(QStringLiteral("sk-test-1"), endpoint), "A key could not be stored.");
    check(secrets.read(endpoint) == QStringLiteral("sk-test-1"), "A stored key was not read back.");
    check(secrets.store(QStringLiteral("sk-test-2"), endpoint), "A key could not be replaced.");
    check(secrets.read(endpoint) == QStringLiteral("sk-test-2"), "A replaced key was not read back.");
    // An empty key removes the item instead of storing an empty secret.
    check(secrets.store(QStringLiteral("  "), endpoint), "An empty key was not handled.");
    check(secrets.read(endpoint).isEmpty(), "An empty key did not remove the entry.");
    check(secrets.remove(endpoint), "Removing an absent key failed.");
    check(!secrets.store(QStringLiteral("sk"), QString()), "A key was stored without an endpoint.");
}

// MARK: - Runner

/// A window stand-in: notes and tabs live in memory, approvals are scripted.
class FakeHost final : public SpaceChatHost {
public:
    struct Note {
        QString id;
        QString title;
        QString content;
    };

    QString currentSpace = QStringLiteral("Arbeit");
    bool allowed = true;
    std::vector<Note> notes;
    QJsonArray tabs;
    std::optional<QJsonObject> snapshot;
    bool approve = true;
    bool deferAsync = false;
    QStringList approvalRequests;
    QStringList readRequests;
    int created = 0;

    [[nodiscard]] QString space() const override { return currentSpace; }
    [[nodiscard]] bool agentAllowed() const override { return allowed; }
    [[nodiscard]] QJsonArray inventory() const override { return tabs; }

    [[nodiscard]] QJsonArray listNotes() const override {
        QJsonArray list;
        for (const Note &note : notes) {
            QJsonObject item;
            item.insert(QStringLiteral("id"), note.id);
            item.insert(QStringLiteral("title"), note.title);
            list.append(item);
        }
        return list;
    }

    QString createNote(const QString &title, const QString &content) override {
        const QString id = QStringLiteral("note-") + QString::number(++created);
        notes.push_back({id, title, content});
        return id;
    }

    [[nodiscard]] std::optional<QJsonObject> readNote(const QString &id) const override {
        for (const Note &note : notes) {
            if (note.id != id) continue;
            QJsonObject payload;
            payload.insert(QStringLiteral("id"), note.id);
            payload.insert(QStringLiteral("title"), note.title);
            payload.insert(QStringLiteral("content"), note.content);
            return payload;
        }
        return std::nullopt;
    }

    bool writeNote(const QString &id, const QString &content, const QString &mode) override {
        for (Note &note : notes) {
            if (note.id != id) continue;
            note.content = mode == QStringLiteral("replace") || note.content.isEmpty()
                ? content
                : note.content + QLatin1Char('\n') + content;
            return true;
        }
        return false;
    }

    std::optional<QJsonObject> inbox;
    std::optional<QJsonObject> mailMessage;
    QList<int> mailLimits;
    QStringList mailMessageRequests;

    void readMail(int limit, std::function<void(std::optional<QJsonObject>)> done) override {
        mailLimits.append(limit);
        auto answer = inbox;
        if (deferAsync) QTimer::singleShot(0, [done, answer]() { done(answer); });
        else done(answer);
    }

    void readMailMessage(const QString &id, std::function<void(std::optional<QJsonObject>)> done) override {
        mailMessageRequests.append(id);
        auto answer = mailMessage;
        if (deferAsync) QTimer::singleShot(0, [done, answer]() { done(answer); });
        else done(answer);
    }

    void readTab(const QString &id, std::function<void(std::optional<QJsonObject>)> done) override {
        readRequests.append(id);
        auto answer = snapshot;
        if (deferAsync) QTimer::singleShot(0, [done, answer]() { done(answer); });
        else done(answer);
    }

    void requestOpenUrl(const QUrl &url, std::function<void(std::optional<QString>)> done) override {
        approvalRequests.append(url.toString());
        std::optional<QString> tab;
        if (approve) tab = QStringLiteral("agent-tab-1");
        if (deferAsync) QTimer::singleShot(0, [done, tab]() { done(tab); });
        else done(tab);
    }
};

/// Answers queued replies in order and records what was sent.
class FakeTransport {
public:
    std::deque<QPair<int, QByteArray>> replies;
    QList<QJsonObject> requests;
    QList<QJsonObject> headers;
    bool deferAsync = false;
    std::function<void()> pending;

    SpaceChatRunner::Transport handler() {
        return [this](const QUrl &, const QJsonObject &sentHeaders, const QByteArray &body,
                      std::function<void(int, QByteArray)> done) {
            requests.append(parse(body));
            headers.append(sentHeaders);
            QPair<int, QByteArray> reply{200, QByteArrayLiteral(R"({"choices":[{"message":{"content":"ok"}}]})")};
            if (!replies.empty()) {
                reply = replies.front();
                replies.pop_front();
            }
            if (deferAsync) pending = [done, reply]() { done(reply.first, reply.second); };
            else done(reply.first, reply.second);
        };
    }
};

QByteArray openAiReply(const QString &content, const QJsonArray &calls = {}) {
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

QJsonObject toolCall(const QString &id, const QString &name, const QJsonObject &arguments) {
    QJsonObject function;
    function.insert(QStringLiteral("name"), name);
    function.insert(QStringLiteral("arguments"),
                    QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
    QJsonObject call;
    call.insert(QStringLiteral("id"), id);
    call.insert(QStringLiteral("type"), QStringLiteral("function"));
    call.insert(QStringLiteral("function"), function);
    return call;
}

/// Prepares a store with a usable connection and a draft in the Space.
std::unique_ptr<SpaceChatStore> readyStore(const QString &directory, const QString &prompt) {
    check(QDir().mkpath(directory), "Could not create a runner store directory.");
    auto store = std::make_unique<SpaceChatStore>(directory.toStdString());
    ChatConnection connection;
    connection.endpoint = ChatProviderPresets::endpoint(ChatProvider::openAI);
    connection.model = QStringLiteral("gpt-test");
    store->setConnection(connection);
    store->setApiKey(QStringLiteral("sk-test"));
    store->setDraft(QStringLiteral("Arbeit"), prompt);
    return store;
}

QStringList kindsOf(const SpaceChatStore &store, const QString &space) {
    QStringList kinds;
    for (const SpaceChatEntry &entry : store.entries(space)) kinds.append(entry.kind);
    return kinds;
}

void checkRunnerRefusals(const QString &root) {
    // No draft: nothing is sent and nothing is recorded.
    {
        auto store = readyStore(root + QStringLiteral("/run-empty"), QString());
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(transport.requests.isEmpty(), "An empty draft still sent a request.");
        check(store->entries().empty(), "An empty draft was recorded.");
    }

    // Agent switched off: the prompt stays in the draft.
    {
        auto store = readyStore(root + QStringLiteral("/run-off"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        runner.setTransport(transport.handler());
        FakeHost host;
        host.allowed = false;
        runner.send(host);
        check(transport.requests.isEmpty(), "A disabled agent still sent a request.");
        check(store->draft(QStringLiteral("Arbeit")) == QStringLiteral("Hallo"),
              "A refused send lost the draft.");
    }

    // A refused endpoint is reported and nothing is sent.
    {
        const QString directory = root + QStringLiteral("/run-endpoint");
        check(QDir().mkpath(directory), "Could not create the endpoint store directory.");
        SpaceChatStore store(directory.toStdString());
        ChatConnection connection;
        connection.endpoint = QStringLiteral("http://api.example/v1");
        connection.model = QStringLiteral("m");
        store.setConnection(connection);
        store.setDraft(QStringLiteral("Arbeit"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(store);
        FakeTransport transport;
        runner.setTransport(transport.handler());
        QString reported;
        QObject::connect(&runner, &SpaceChatRunner::failed, [&reported](const QString &message) {
            reported = message;
        });
        FakeHost host;
        runner.send(host);
        check(transport.requests.isEmpty(), "A plain HTTP endpoint still sent a request.");
        check(!reported.isEmpty(), "A refused endpoint was not reported.");
        check(store.entries().empty(), "A refused endpoint was recorded as a conversation line.");
    }

    // A damaged conversation blocks sending, so nothing is appended to it.
    {
        const QString directory = root + QStringLiteral("/run-damaged");
        check(QDir().mkpath(directory), "Could not create the damaged runner directory.");
        {
            QFile file(directory + QStringLiteral("/space-chat.json"));
            check(file.open(QIODevice::WriteOnly), "Could not write the damaged file.");
            file.write(QByteArrayLiteral("nope"));
        }
        SpaceChatStore store(directory.toStdString());
        ChatConnection connection;
        connection.endpoint = ChatProviderPresets::endpoint(ChatProvider::openAI);
        connection.model = QStringLiteral("gpt-test");
        store.setConnection(connection);
        store.setDraft(QStringLiteral("Arbeit"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(store);
        FakeTransport transport;
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(transport.requests.isEmpty(), "A damaged conversation still sent a request.");
    }
}

void checkRunnerConversation(const QString &root) {
    auto store = readyStore(root + QStringLiteral("/run-notes"), QStringLiteral("Schreib mir eine Notiz."));
    store->setContext(QStringLiteral("Arbeit"), QStringLiteral("Kontext aus einer Mail"));
    SpaceChatRunner runner(*store);
    FakeTransport transport;
    transport.replies.push_back({200, openAiReply(QStringLiteral("Ich lege sie an."), QJsonArray{
        toolCall(QStringLiteral("c1"), QStringLiteral("create_note"), QJsonObject{
            {QStringLiteral("title"), QStringLiteral("Einkauf")},
            {QStringLiteral("content"), QStringLiteral("Milch")},
        }),
    })});
    transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
        toolCall(QStringLiteral("c2"), QStringLiteral("write_note"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("note-1")},
            {QStringLiteral("content"), QStringLiteral("Brot")},
            {QStringLiteral("mode"), QStringLiteral("append")},
        }),
        toolCall(QStringLiteral("c3"), QStringLiteral("read_note"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("note-1")},
        }),
    })});
    transport.replies.push_back({200, openAiReply(QStringLiteral("Fertig: Milch und Brot."))});
    runner.setTransport(transport.handler());

    FakeHost host;
    int finished = 0;
    QObject::connect(&runner, &SpaceChatRunner::finished, [&finished]() { ++finished; });
    runner.send(host);

    check(finished == 1, "The run did not finish exactly once.");
    check(!runner.running(), "The runner stayed busy after finishing.");
    check(transport.requests.size() == 3, "Expected three model round trips.");

    // The staged context is sent as its own line, right after the question.
    check(kindsOf(*store, QStringLiteral("Arbeit")) == QStringList({
              QStringLiteral("user"), QStringLiteral("context"),
              QStringLiteral("assistant"), QStringLiteral("assistant"),
          }),
          "The recorded conversation is wrong: "
          + kindsOf(*store, QStringLiteral("Arbeit")).join(QLatin1Char(',')).toStdString());
    check(store->draft(QStringLiteral("Arbeit")).isEmpty(), "The draft was not cleared.");
    check(store->takeContext(QStringLiteral("Arbeit")).isEmpty(), "The context was not consumed.");

    check(host.notes.size() == 1, "The note was not created.");
    check(host.notes.front().title == QStringLiteral("Einkauf"), "The note title was lost.");
    check(host.notes.front().content == QStringLiteral("Milch\nBrot"),
          "The append mode did not keep the earlier content: " + host.notes.front().content.toStdString());

    // Every tool call has to be answered, in order, before the next request.
    const QJsonArray second = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
    check(second.last().toObject().value(QStringLiteral("role")).toString() == QStringLiteral("tool"),
          "The first tool answer was not sent back.");
    check(second.last().toObject().value(QStringLiteral("tool_call_id")).toString() == QStringLiteral("c1"),
          "The tool answer was not tied to its call.");
    const QJsonArray third = transport.requests.at(2).value(QStringLiteral("messages")).toArray();
    QStringList answered;
    for (const QJsonValue &value : third) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
            answered.append(message.value(QStringLiteral("tool_call_id")).toString());
    }
    check(answered == QStringList({QStringLiteral("c1"), QStringLiteral("c2"), QStringLiteral("c3")}),
          "Not every tool call was answered in order: " + answered.join(QLatin1Char(',')).toStdString());
    check(third.last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("Brot")),
          "The note content did not reach the model.");

    // The key travels in the header, never in the body.
    check(transport.headers.first().value(QStringLiteral("Authorization")).toString()
              == QStringLiteral("Bearer sk-test"),
          "The API key was not sent as a bearer token.");
    check(!QString::fromUtf8(QJsonDocument(transport.requests.first()).toJson()).contains(QStringLiteral("sk-test")),
          "The API key ended up in the request body.");
}

void checkRunnerApproval(const QString &root) {
    // Approved: a tab is opened and the model is told which one.
    {
        auto store = readyStore(root + QStringLiteral("/run-open"), QStringLiteral("Öffne die Seite."));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({200, openAiReply(QStringLiteral("Ich frage nach."), QJsonArray{
            toolCall(QStringLiteral("o1"), QStringLiteral("open_url"), QJsonObject{
                {QStringLiteral("url"), QStringLiteral("https://example.com/a")},
            }),
        })});
        transport.replies.push_back({200, openAiReply(QStringLiteral("Geöffnet."))});
        runner.setTransport(transport.handler());

        FakeHost host;
        runner.send(host);
        check(host.approvalRequests == QStringList({QStringLiteral("https://example.com/a")}),
              "The user was not asked before opening a page.");
        const QStringList kinds = kindsOf(*store, QStringLiteral("Arbeit"));
        check(kinds.count(QStringLiteral("action")) == 2,
              "The request and the result were not both recorded: " + kinds.join(QLatin1Char(',')).toStdString());
        const QJsonArray messages = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
        check(messages.last().toObject().value(QStringLiteral("content")).toString()
                  .contains(QStringLiteral("agent-tab-1")),
              "The model was not told which tab was opened.");
    }

    // Declined: nothing is opened and the model is told not to retry.
    {
        auto store = readyStore(root + QStringLiteral("/run-declined"), QStringLiteral("Öffne die Seite."));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
            toolCall(QStringLiteral("o1"), QStringLiteral("open_url"), QJsonObject{
                {QStringLiteral("url"), QStringLiteral("https://example.com/a")},
            }),
        })});
        transport.replies.push_back({200, openAiReply(QStringLiteral("Verstanden."))});
        runner.setTransport(transport.handler());

        FakeHost host;
        host.approve = false;
        runner.send(host);
        const QJsonArray messages = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
        check(messages.last().toObject().value(QStringLiteral("content")).toString()
                  .contains(QStringLiteral("declined")),
              "The refusal was not passed on to the model.");
    }

    // A URL the agent must not open is refused without ever asking the user.
    {
        auto store = readyStore(root + QStringLiteral("/run-badurl"), QStringLiteral("Öffne das."));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
            toolCall(QStringLiteral("o1"), QStringLiteral("open_url"), QJsonObject{
                {QStringLiteral("url"), QStringLiteral("file:///etc/passwd")},
            }),
        })});
        transport.replies.push_back({200, openAiReply(QStringLiteral("Geht nicht."))});
        runner.setTransport(transport.handler());

        FakeHost host;
        runner.send(host);
        check(host.approvalRequests.isEmpty(), "A local file URL reached the approval dialog.");
        const QJsonArray messages = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
        check(messages.last().toObject().value(QStringLiteral("content")).toString()
                  .contains(QStringLiteral("unavailable")),
              "A refused URL was not reported back as unavailable.");
    }
}

void checkRunnerTabReading(const QString &root) {
    auto store = readyStore(root + QStringLiteral("/run-tab"), QStringLiteral("Was steht auf der Seite?"));
    SpaceChatRunner runner(*store);
    FakeTransport transport;
    transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
        toolCall(QStringLiteral("t1"), QStringLiteral("read_tab"), QJsonObject{
            {QStringLiteral("tab"), QStringLiteral("tab-1")},
        }),
        toolCall(QStringLiteral("t2"), QStringLiteral("read_tab"), QJsonObject{
            {QStringLiteral("tab"), QStringLiteral("tab-from-another-space")},
        }),
    })});
    transport.replies.push_back({200, openAiReply(QStringLiteral("Dort steht: Hallo."))});
    runner.setTransport(transport.handler());

    FakeHost host;
    // The second read is answered with nothing, standing in for a tab that is
    // not part of this Space.
    QJsonObject page;
    page.insert(QStringLiteral("url"), QStringLiteral("https://example.com/"));
    page.insert(QStringLiteral("text"), QStringLiteral("Hallo"));
    host.snapshot = page;
    host.deferAsync = true;

    runner.send(host);
    settle([&runner]() { return !runner.running(); });

    check(host.readRequests.size() == 2, "Not every read_tab call reached the window.");
    const QJsonArray messages = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
    QStringList answers;
    for (const QJsonValue &value : messages) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
            answers.append(message.value(QStringLiteral("content")).toString());
    }
    check(answers.size() == 2, "Not every read_tab call was answered.");
    check(answers.at(0).contains(QStringLiteral("Hallo")), "The page text did not reach the model.");
}

void checkRunnerMail(const QString &root) {
    auto store = readyStore(root + QStringLiteral("/run-mail"), QStringLiteral("Was ist neu im Postfach?"));
    SpaceChatRunner runner(*store);
    FakeTransport transport;
    transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
        // A limit past the ceiling has to be brought back into range, not passed on.
        toolCall(QStringLiteral("m1"), QStringLiteral("read_mail"), QJsonObject{
            {QStringLiteral("limit"), 500},
        }),
    })});
    transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
        toolCall(QStringLiteral("m2"), QStringLiteral("read_mail_message"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("acc-1:INBOX:v1:11")},
        }),
    })});
    transport.replies.push_back({200, openAiReply(QStringLiteral("Eine Rechnung ist da."))});
    runner.setTransport(transport.handler());

    FakeHost host;
    QJsonObject inbox;
    inbox.insert(QStringLiteral("source"), QStringLiteral("YoBro built-in mail"));
    inbox.insert(QStringLiteral("messages"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("acc-1:INBOX:v1:11")},
        {QStringLiteral("subject"), QStringLiteral("Rechnung")},
    }});
    host.inbox = inbox;
    QJsonObject full;
    full.insert(QStringLiteral("subject"), QStringLiteral("Rechnung"));
    full.insert(QStringLiteral("body"), QStringLiteral("Bitte bis Freitag zahlen."));
    host.mailMessage = full;

    runner.send(host);
    check(host.mailLimits == QList<int>({50}),
          "The mail limit was not brought back into range.");
    check(host.mailMessageRequests == QStringList({QStringLiteral("acc-1:INBOX:v1:11")}),
          "The message id was not passed on.");
    const QJsonArray messages = transport.requests.at(2).value(QStringLiteral("messages")).toArray();
    QStringList answers;
    for (const QJsonValue &value : messages) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool"))
            answers.append(message.value(QStringLiteral("content")).toString());
    }
    check(answers.size() == 2, "Not every mail call was answered.");
    check(answers.at(0).contains(QStringLiteral("Rechnung")), "The inbox list did not reach the model.");
    check(answers.at(1).contains(QStringLiteral("Freitag")), "The message body did not reach the model.");

    // Without a mailbox the model is told so instead of getting an error.
    auto emptyStore = readyStore(root + QStringLiteral("/run-nomail"), QStringLiteral("Neue Mail?"));
    SpaceChatRunner emptyRunner(*emptyStore);
    FakeTransport emptyTransport;
    emptyTransport.replies.push_back({200, openAiReply(QString(), QJsonArray{
        toolCall(QStringLiteral("m1"), QStringLiteral("read_mail"), {}),
    })});
    emptyTransport.replies.push_back({200, openAiReply(QStringLiteral("Kein Postfach."))});
    emptyRunner.setTransport(emptyTransport.handler());
    FakeHost emptyHost;
    emptyRunner.send(emptyHost);
    const QJsonArray emptyMessages =
        emptyTransport.requests.at(1).value(QStringLiteral("messages")).toArray();
    check(emptyMessages.last().toObject().value(QStringLiteral("content")).toString()
              .contains(QStringLiteral("No mail account")),
          "A missing mailbox was not explained to the model.");
    check(emptyHost.mailLimits == QList<int>({20}), "The default mail limit changed.");
}

void checkRunnerLimits(const QString &root) {
    // More tool calls than allowed in one answer stops the run.
    {
        auto store = readyStore(root + QStringLiteral("/run-flood"), QStringLiteral("Mach viel."));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        QJsonArray calls;
        for (int index = 0; index < 9; ++index) {
            calls.append(toolCall(QStringLiteral("c") + QString::number(index),
                                  QStringLiteral("list_notes"), {}));
        }
        transport.replies.push_back({200, openAiReply(QString(), calls)});
        runner.setTransport(transport.handler());
        QString reported;
        QObject::connect(&runner, &SpaceChatRunner::failed, [&reported](const QString &m) { reported = m; });
        FakeHost host;
        runner.send(host);
        check(transport.requests.size() == 1, "The run continued after too many tool calls.");
        check(!reported.isEmpty(), "Too many tool calls were not reported.");
        check(kindsOf(*store, QStringLiteral("Arbeit")).contains(QStringLiteral("error")),
              "The failure was not recorded in the conversation.");
    }

    // A model that only ever calls tools is stopped after the round limit.
    {
        auto store = readyStore(root + QStringLiteral("/run-loop"), QStringLiteral("Dreh dich im Kreis."));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        for (int index = 0; index < 20; ++index) {
            transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
                toolCall(QStringLiteral("c") + QString::number(index), QStringLiteral("list_notes"), {}),
            })});
        }
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(transport.requests.size() == SpaceChatProtocol::maxRounds,
              "The round limit was not enforced: " + std::to_string(transport.requests.size()) + " requests.");
        check(!runner.running(), "The runner did not stop at the round limit.");
        bool stopped = false;
        for (const SpaceChatEntry &entry : store->entries(QStringLiteral("Arbeit")))
            if (entry.kind == QStringLiteral("action")) stopped = true;
        check(stopped, "The round limit was not explained in the conversation.");
    }

    // An HTTP error becomes one recorded failure, not a retry storm.
    {
        auto store = readyStore(root + QStringLiteral("/run-error"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({401, QByteArrayLiteral(R"({"error":{"message":"bad key"}})")});
        runner.setTransport(transport.handler());
        QString reported;
        QObject::connect(&runner, &SpaceChatRunner::failed, [&reported](const QString &m) { reported = m; });
        FakeHost host;
        runner.send(host);
        check(transport.requests.size() == 1, "A failed request was retried.");
        check(reported.contains(QStringLiteral("bad key")), "The provider message was not reported.");
        check(kindsOf(*store, QStringLiteral("Arbeit")).last() == QStringLiteral("error"),
              "The failure was not recorded.");
    }

    // An answer with neither text nor a tool call is a failure, not a silence.
    {
        auto store = readyStore(root + QStringLiteral("/run-silent"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({200, openAiReply(QString())});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(kindsOf(*store, QStringLiteral("Arbeit")).last() == QStringLiteral("error"),
              "An empty answer was accepted as a reply.");
    }

    // A malformed tool call stops the run instead of being guessed at.
    {
        auto store = readyStore(root + QStringLiteral("/run-badcall"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        QJsonObject function;
        function.insert(QStringLiteral("name"), QStringLiteral("list_notes"));
        function.insert(QStringLiteral("arguments"), QStringLiteral("{not json"));
        QJsonObject call;
        call.insert(QStringLiteral("id"), QStringLiteral("c1"));
        call.insert(QStringLiteral("function"), function);
        transport.replies.push_back({200, openAiReply(QString(), QJsonArray{call})});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(transport.requests.size() == 1, "A malformed tool call did not stop the run.");
        check(kindsOf(*store, QStringLiteral("Arbeit")).last() == QStringLiteral("error"),
              "A malformed tool call was not recorded as a failure.");
    }

    // An unknown tool is answered, so the model can correct itself.
    {
        auto store = readyStore(root + QStringLiteral("/run-unknown"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.replies.push_back({200, openAiReply(QString(), QJsonArray{
            toolCall(QStringLiteral("c1"), QStringLiteral("send_mail"), {}),
        })});
        transport.replies.push_back({200, openAiReply(QStringLiteral("Das kann ich nicht."))});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        const QJsonArray messages = transport.requests.at(1).value(QStringLiteral("messages")).toArray();
        check(messages.last().toObject().value(QStringLiteral("content")).toString()
                  .contains(QStringLiteral("unavailable")),
              "An unknown tool was not reported back as unavailable.");
    }
}

void checkRunnerAborts(const QString &root) {
    // Stopping mid-flight ignores the answer that arrives afterwards.
    {
        auto store = readyStore(root + QStringLiteral("/run-cancel"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.deferAsync = true;
        transport.replies.push_back({200, openAiReply(QStringLiteral("Zu spät."))});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        check(runner.running(), "The run did not start.");
        runner.cancel();
        check(!runner.running(), "Stopping did not end the run.");
        check(transport.pending != nullptr, "The pending reply is missing.");
        transport.pending();
        const QStringList kinds = kindsOf(*store, QStringLiteral("Arbeit"));
        check(kinds == QStringList({QStringLiteral("user"), QStringLiteral("action")}),
              "A stopped run still recorded the answer: " + kinds.join(QLatin1Char(',')).toStdString());
    }

    // Switching the Space mid-flight abandons the run silently.
    {
        auto store = readyStore(root + QStringLiteral("/run-space"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.deferAsync = true;
        transport.replies.push_back({200, openAiReply(QStringLiteral("Antwort"))});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        host.currentSpace = QStringLiteral("Freizeit");
        check(transport.pending != nullptr, "The pending reply is missing.");
        transport.pending();
        check(kindsOf(*store, QStringLiteral("Arbeit")) == QStringList({QStringLiteral("user")}),
              "An answer was recorded after the Space changed.");
    }

    // Turning the agent off mid-flight has the same effect.
    {
        auto store = readyStore(root + QStringLiteral("/run-disable"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.deferAsync = true;
        transport.replies.push_back({200, openAiReply(QStringLiteral("Antwort"))});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        host.allowed = false;
        check(transport.pending != nullptr, "The pending reply is missing.");
        transport.pending();
        check(kindsOf(*store, QStringLiteral("Arbeit")) == QStringList({QStringLiteral("user")}),
              "An answer was recorded after the agent was switched off.");
    }

    // A second send while one run is active is ignored.
    {
        auto store = readyStore(root + QStringLiteral("/run-double"), QStringLiteral("Hallo"));
        SpaceChatRunner runner(*store);
        FakeTransport transport;
        transport.deferAsync = true;
        transport.replies.push_back({200, openAiReply(QStringLiteral("Antwort"))});
        runner.setTransport(transport.handler());
        FakeHost host;
        runner.send(host);
        store->setDraft(QStringLiteral("Arbeit"), QStringLiteral("Noch etwas"));
        runner.send(host);
        check(transport.requests.size() == 1, "A second run started while the first was active.");
        check(store->draft(QStringLiteral("Arbeit")) == QStringLiteral("Noch etwas"),
              "The second draft was consumed by the refused send.");
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        checkPresets();
        checkAnthropicModelParity();
        checkConnectionUrl();
        checkWebUrl();
        checkTools();
        checkSystemMessage();
        checkOpenAiBody();
        checkAnthropicBody();
        checkProviderMessage();
        checkProviderError();
        checkStore(work.path());
        checkKeychain(work.path());
        checkRunnerRefusals(work.path());
        checkRunnerConversation(work.path());
        checkRunnerApproval(work.path());
        checkRunnerTabReading(work.path());
        checkRunnerMail(work.path());
        checkRunnerLimits(work.path());
        checkRunnerAborts(work.path());
    } catch (const std::exception &error) {
        std::cerr << "SPACE CHAT FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "SPACE CHAT PASS\n";
    return 0;
}
