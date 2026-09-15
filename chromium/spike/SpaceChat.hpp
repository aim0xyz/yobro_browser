#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace yobro::spike {

/// The chat providers the WebKit build offers, in the same order.
enum class ChatProvider { openRouter, anthropic, openAI, custom };

/// Presets mirroring `ChatProviderPreset` in `Sources/YOBRO/SpaceChat.swift`.
struct ChatProviderPresets {
    [[nodiscard]] static std::vector<ChatProvider> all();
    [[nodiscard]] static QString title(ChatProvider provider);
    [[nodiscard]] static QString endpoint(ChatProvider provider);
    [[nodiscard]] static QString defaultModel(ChatProvider provider);
    /// Any endpoint that is not one of the known presets counts as custom.
    [[nodiscard]] static ChatProvider matching(const QString &endpoint);
    [[nodiscard]] static QStringList anthropicModels();
};

/// Endpoint plus model, persisted as `chat-connection.json`.
struct ChatConnection {
    QString endpoint;
    QString model;

    /// Returns the endpoint only when it is safe to send an API key to.
    ///
    /// HTTPS is required, except for loopback hosts during development. A
    /// user or password in the URL, a query, or a fragment is rejected so a
    /// key never ends up in a log or a referrer. `problem` receives a
    /// localised explanation when the endpoint is refused.
    [[nodiscard]] std::optional<QUrl> url(QString *problem = nullptr) const;
};

/// One line of a Space conversation, persisted as `space-chat.json`.
///
/// `kind` is one of `user`, `assistant`, `context`, `action` or `error`, the
/// same vocabulary the WebKit build uses, so a future sync can carry both.
struct SpaceChatEntry {
    QString id;
    /// ISO 8601 with offset, as written by the WebKit build.
    QString date;
    QString space;
    QString kind;
    QString text;
    bool saved = false;
};

/// The API key for one endpoint, stored in the macOS Keychain.
///
/// The key never reaches `space-chat.json`, the timeline, or a log. The
/// service name carries the profile directory, so profiles do not share keys
/// and the WebKit build's own items stay untouched.
class AgentApiKeySecrets {
public:
    explicit AgentApiKeySecrets(std::filesystem::path profileDirectory);

    [[nodiscard]] bool available() const;
    /// An empty key removes the item instead of storing an empty secret.
    bool store(const QString &key, const QString &endpoint);
    [[nodiscard]] QString read(const QString &endpoint) const;
    bool remove(const QString &endpoint);

    [[nodiscard]] const std::string &service() const { return service_; }

private:
    std::string service_;
};

/// Message building and response parsing for both wire formats.
///
/// OpenAI-compatible providers (OpenRouter, OpenAI, custom) and Anthropic use
/// different shapes. Everything internal uses the OpenAI shape and Anthropic
/// requests are converted on the way out and back, exactly as in the WebKit
/// build, so the tool loop does not need to know the provider.
struct SpaceChatProtocol {
    /// The tool declarations offered to the model, the same set as the WebKit
    /// build's: notes, tabs, an approved page open and the built-in mail.
    [[nodiscard]] static QJsonArray tools();
    [[nodiscard]] static QStringList toolNames();

    /// The system message, including the tab and note inventory of the Space.
    [[nodiscard]] static QJsonObject systemMessage(const QString &space, const QJsonArray &inventory);

    [[nodiscard]] static QJsonObject requestBody(
        ChatProvider provider,
        const QString &model,
        const QJsonArray &messages
    );

    /// Normalises a provider response into an OpenAI-style assistant message.
    [[nodiscard]] static std::optional<QJsonObject> providerMessage(
        const QJsonObject &response,
        ChatProvider provider
    );

    /// A localised failure line, preferring the provider's own message.
    [[nodiscard]] static QString providerError(const QByteArray &data, int status);

    /// Accepts plain http(s) URLs without credentials, for `open_url`.
    [[nodiscard]] static std::optional<QUrl> webUrl(const QString &raw);

    /// Model output and page content are cut to this many characters before
    /// they are stored or sent onwards.
    static constexpr int textLimit = 24000;
    /// Note content is cut to this many characters.
    static constexpr int noteLimit = 100000;
    /// A single answer may request at most this many tool calls.
    static constexpr int callsPerTurn = 8;
    /// The loop stops after this many model round trips.
    static constexpr int maxRounds = 8;
};

/// Conversation storage, drafts, staged context and the connection settings.
class SpaceChatStore {
public:
    explicit SpaceChatStore(std::filesystem::path profileDirectory);

    [[nodiscard]] const std::vector<SpaceChatEntry> &entries() const { return entries_; }
    [[nodiscard]] std::vector<SpaceChatEntry> entries(const QString &space) const;
    void append(const QString &kind, const QString &text, const QString &space);
    /// Keeps a line when a Space is renamed, so history is not orphaned.
    void renameSpace(const QString &oldName, const QString &newName);
    /// Marks a line as worth keeping, or unmarks it.
    bool toggleSaved(const QString &id);
    /// Clears one Space's conversation, its draft and its staged context.
    void newChat(const QString &space);

    [[nodiscard]] QString draft(const QString &space) const;
    void setDraft(const QString &space, const QString &text);
    /// Reads and clears the staged context, which is sent exactly once.
    QString takeContext(const QString &space);
    void setContext(const QString &space, const QString &text);

    [[nodiscard]] const ChatConnection &connection() const { return connection_; }
    void setConnection(const ChatConnection &value);
    /// Fills endpoint and model from a preset and loads that endpoint's key.
    void selectProvider(ChatProvider provider);

    [[nodiscard]] const QString &apiKey() const { return apiKey_; }
    void setApiKey(const QString &value) { apiKey_ = value; }
    /// Validates the endpoint, writes the key to the Keychain and persists.
    bool saveConnection(QString *problem);
    /// Forgets the stored key for the current endpoint.
    bool forgetApiKey(QString *problem);

    /// True when `space-chat.json` could not be read. Nothing is written in
    /// that case, so a damaged file is never overwritten with a fresh one.
    [[nodiscard]] bool loadFailed() const { return loadFailed_; }
    [[nodiscard]] const QString &problem() const { return problem_; }

    [[nodiscard]] const AgentApiKeySecrets &secrets() const { return secrets_; }

private:
    void persist();

    std::filesystem::path directory_;
    AgentApiKeySecrets secrets_;
    std::vector<SpaceChatEntry> entries_;
    std::vector<std::pair<QString, QString>> drafts_;
    std::vector<std::pair<QString, QString>> contexts_;
    ChatConnection connection_;
    QString apiKey_;
    QString problem_;
    bool loadFailed_ = false;
    unsigned long long nextId_ = 1;
};

/// Everything the tool loop needs from the browser window.
///
/// Kept abstract so the loop can be driven by a test double without a window,
/// a profile or a network connection.
class SpaceChatHost {
public:
    virtual ~SpaceChatHost() = default;

    /// The Space the run belongs to. A change aborts the run.
    [[nodiscard]] virtual QString space() const = 0;
    /// False when the profile is inactive or the agent switch is off.
    [[nodiscard]] virtual bool agentAllowed() const = 0;
    /// `{id, kind, title, url}` for every tab and note of the Space.
    [[nodiscard]] virtual QJsonArray inventory() const = 0;

    [[nodiscard]] virtual QJsonArray listNotes() const = 0;
    /// Returns the new note id.
    virtual QString createNote(const QString &title, const QString &content) = 0;
    [[nodiscard]] virtual std::optional<QJsonObject> readNote(const QString &id) const = 0;
    /// `mode` is `append` or `replace`.
    virtual bool writeNote(const QString &id, const QString &content, const QString &mode) = 0;

    /// Opens the built-in mail screen and answers with recent inbox metadata.
    /// Answers with nothing when no mailbox is set up.
    virtual void readMail(int limit, std::function<void(std::optional<QJsonObject>)> done) = 0;
    /// Opens one message from the built-in mail screen and answers with it.
    virtual void readMailMessage(
        const QString &id,
        std::function<void(std::optional<QJsonObject>)> done
    ) = 0;

    /// Reads a tab through the agent bridge. Answers with the snapshot JSON, or
    /// nothing when the tab is not part of this Space.
    virtual void readTab(const QString &id, std::function<void(std::optional<QJsonObject>)> done) = 0;
    /// Asks the user whether the URL may be opened, then answers with the new
    /// tab id when it was, or nothing when the user declined.
    virtual void requestOpenUrl(const QUrl &url, std::function<void(std::optional<QString>)> done) = 0;
};

/// Drives one conversation turn: request, tool calls, request again.
class SpaceChatRunner : public QObject {
    Q_OBJECT

public:
    /// Sends a request body to an endpoint and reports status plus payload.
    /// A status of 0 means the request never reached the server.
    using Transport = std::function<void(
        const QUrl &endpoint,
        const QJsonObject &headers,
        const QByteArray &body,
        std::function<void(int status, QByteArray data)> done
    )>;

    explicit SpaceChatRunner(SpaceChatStore &store, QObject *parent = nullptr);
    ~SpaceChatRunner() override;

    /// Replaces the network transport, for tests.
    void setTransport(Transport transport);

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] const QString &runningSpace() const { return runningSpace_; }

    /// Starts a turn from the Space's draft. Does nothing when a run is
    /// already active, the draft is empty, the agent is not allowed, the
    /// conversation file is damaged or the endpoint is refused.
    void send(SpaceChatHost &host);
    /// Aborts the current run and records that it was stopped.
    void cancel();

Q_SIGNALS:
    void changed();
    void failed(const QString &message);
    void finished();

private:
    class Run;

    void step(const std::shared_ptr<Run> &run);
    void handleResponse(const std::shared_ptr<Run> &run, int status, const QByteArray &data);
    void runCalls(const std::shared_ptr<Run> &run, int index);
    void finish(const std::shared_ptr<Run> &run);
    void fail(const std::shared_ptr<Run> &run, const QString &message);
    [[nodiscard]] bool alive(const std::shared_ptr<Run> &run) const;

    SpaceChatStore &store_;
    Transport transport_;
    std::shared_ptr<Run> run_;
    QString runningSpace_;
    unsigned long long token_ = 0;
    bool running_ = false;
};

} // namespace yobro::spike
