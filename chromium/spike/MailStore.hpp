#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace yobro::spike {

/// One configured mailbox, persisted as `mail-accounts.json`.
///
/// The password is never part of this: it lives in the Keychain under the
/// account id.
struct MailAccount {
    QString id;
    QString label;
    QString address;
    QString username;
    QString imapHost;
    int imapPort = 993;
    QString smtpHost;
    int smtpPort = 465;
    /// `tls` for implicit TLS, `starttls` for an upgraded connection.
    QString smtpSecurity = QStringLiteral("tls");
    /// Empty means the IMAP user name is used for sending too.
    QString smtpUsername;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static MailAccount fromJson(const QJsonObject &object);
    /// Empty when the account can be used, otherwise the reason it cannot.
    [[nodiscard]] QString validationProblem() const;
    /// The label if it has one, otherwise the address.
    [[nodiscard]] QString displayLabel() const;
};

struct MailFolder {
    QString path;
    QString title;
    QString delimiter;
    QString role;
    bool selectable = true;
    int unread = 0;

    [[nodiscard]] static MailFolder fromJson(const QJsonObject &object);
    /// `INBOX` is shown under its translated name, everything else as reported.
    [[nodiscard]] QString displayTitle() const;
    /// True for the folder deleted messages go to, by role or by name.
    [[nodiscard]] bool isTrash() const;
};

struct MailAttachment {
    QString id;
    QString name;
    QString mime;
    int size = 0;
};

struct MailMessage {
    QString accountId;
    QString folder = QStringLiteral("INBOX");
    /// The mailbox generation. A change means every uid became meaningless.
    QString validity;
    QString uid;
    QString subject;
    QString sender;
    QString recipient;
    QString replyTo;
    QString messageId;
    /// Seconds since the epoch.
    qint64 date = 0;
    bool unread = false;

    /// Stable across a refresh, and different after a mailbox reset.
    [[nodiscard]] QString id() const;
    [[nodiscard]] static MailMessage fromJson(
        const QJsonObject &row,
        const QString &accountId,
        const QString &folder,
        const QString &validity
    );
};

struct MailBody {
    QString text;
    QString html;
    std::vector<MailAttachment> attachments;
};

/// The message being written, kept per account like the WebKit build's draft.
struct MailDraft {
    QString accountId;
    QString to;
    QString subject;
    QString text;
    QString html;
    /// Set when this is a reply, so the provider can thread it.
    QString replyToMessageId;

    [[nodiscard]] bool isEmpty() const;
};

/// Mailbox passwords in the macOS Keychain.
///
/// Deliberately a different service name from the WebKit build's
/// `local.yobro.mail`: the two shells keep separate profiles, and one must not
/// be able to read the other's mailbox passwords.
class MailSecrets {
public:
    explicit MailSecrets(std::filesystem::path profileDirectory);

    [[nodiscard]] bool available() const;
    bool store(const QString &accountId, const QString &password);
    [[nodiscard]] QString read(const QString &accountId) const;
    bool remove(const QString &accountId);

    [[nodiscard]] const std::string &service() const { return service_; }

private:
    std::string service_;
};

/// Remembered mail settings, persisted as `mail-preferences.json`.
struct MailPreferences {
    /// Off by default, like the WebKit build.
    bool notifications = false;
    /// On by default, like the WebKit build.
    bool remoteImages = true;
    /// Per account: the inbox message ids already seen, so only genuinely new
    /// mail is announced.
    std::vector<std::pair<QString, QStringList>> knownInbox;
};

/// Accounts, folders, messages and drafts for the built-in mail client.
class MailStore : public QObject {
    Q_OBJECT

public:
    /// Runs one helper action for one account. Replaceable, so the whole store
    /// can be driven without a mail server.
    using Transport = std::function<void(
        const QString &action,
        const MailAccount &account,
        const QJsonObject &extra,
        std::function<void(QString problem, QJsonObject result)> done
    )>;
    /// Reports a finished operation. An empty string means it worked.
    using Completion = std::function<void(QString problem)>;
    /// Checks an account and a password against the server before anything is
    /// stored. Separate from `Transport` because the password is passed in here
    /// rather than read from the Keychain.
    using Verifier = std::function<void(
        const MailAccount &account,
        const QString &password,
        std::function<void(QString problem)> done
    )>;

    explicit MailStore(std::filesystem::path profileDirectory, QObject *parent = nullptr);

    void setTransport(Transport transport);
    void setVerifier(Verifier verifier);

    [[nodiscard]] const std::vector<MailAccount> &accounts() const { return accounts_; }
    [[nodiscard]] std::optional<MailAccount> account(const QString &id) const;
    [[nodiscard]] const std::vector<MailFolder> &folders(const QString &accountId) const;
    /// Messages of one folder, newest first.
    [[nodiscard]] std::vector<MailMessage> messages(const QString &accountId, const QString &folder) const;
    /// The filtered list the panel shows: search over subject and sender, and an
    /// optional unread-only switch.
    [[nodiscard]] std::vector<MailMessage> visibleMessages() const;
    [[nodiscard]] std::optional<MailMessage> message(const QString &id) const;
    [[nodiscard]] std::optional<MailBody> body(const QString &id) const;
    [[nodiscard]] int folderTotal(const QString &accountId, const QString &folder) const;

    /// True when any account reports unread mail.
    [[nodiscard]] bool hasUnread() const;
    [[nodiscard]] bool hasUnread(const QString &accountId) const;

    [[nodiscard]] const QString &selectedAccount() const { return selectedAccount_; }
    [[nodiscard]] const QString &selectedFolder() const { return selectedFolder_; }
    [[nodiscard]] const QString &selectedMessage() const { return selectedMessage_; }
    void setSelectedMessage(const QString &id);
    [[nodiscard]] const QString &search() const { return search_; }
    void setSearch(const QString &value);
    [[nodiscard]] bool unreadOnly() const { return unreadOnly_; }
    void setUnreadOnly(bool value);

    [[nodiscard]] const MailPreferences &preferences() const { return preferences_; }
    void setNotifications(bool enabled);
    void setRemoteImages(bool enabled);

    [[nodiscard]] MailDraft &draft() { return draft_; }
    [[nodiscard]] const MailDraft &draft() const { return draft_; }
    /// Prepares a reply, filling recipient, subject and threading id.
    void beginReply(const MailMessage &message);
    void clearDraft();

    [[nodiscard]] bool busy() const { return busy_ > 0; }
    /// The error last reported for an account, or empty.
    [[nodiscard]] QString error(const QString &accountId) const;
    [[nodiscard]] const QString &notice() const { return notice_; }
    void setNotice(const QString &text);
    /// True when `mail-accounts.json` could not be read. Nothing is written in
    /// that state, so a damaged list is never replaced by an empty one.
    [[nodiscard]] bool loadFailed() const { return loadFailed_; }

    /// Checks the account against the server, then stores it and its password.
    void connectAccount(const MailAccount &account, const QString &password, Completion done);
    /// Forgets an account, its password and everything loaded for it.
    QString disconnectAccount(const QString &accountId);

    /// Reloads folders and the inbox of every account.
    void refresh(Completion done);
    /// Switches the visible folder and loads it.
    void selectFolder(const QString &accountId, const QString &folder, Completion done);
    /// Raises the message limit of the visible folder and loads again.
    void loadMore(Completion done);
    /// Loads the body, or only marks the message as read when it is already there.
    void openMessage(const QString &messageId, Completion done);
    /// Sends the draft and clears it on success.
    void sendDraft(Completion done);
    /// Moves messages to the provider's Trash folder.
    void deleteMessages(const QStringList &messageIds, Completion done);
    void moveMessages(const QStringList &messageIds, const QString &target, Completion done);
    /// Fetches one attachment and answers with its bytes.
    void readAttachment(
        const QString &messageId,
        const QString &attachmentId,
        std::function<void(QString problem, QByteArray data)> done
    );

    /// Inbox messages that are unread and newer than everything already known.
    ///
    /// A mailbox seen for the first time announces its unread mail; afterwards
    /// only uids above the highest known one count, so a refresh does not
    /// announce the same message twice.
    [[nodiscard]] static std::vector<MailMessage> newInboxMessages(
        const std::vector<MailMessage> &loaded,
        const QStringList &known
    );

Q_SIGNALS:
    void changed();
    /// New inbox mail was found and notifications are switched on.
    void newMail(const QString &accountId, int count, const QString &sender, const QString &subject);

private:
    void persistAccounts();
    void persistPreferences();
    void applyFolderResponse(const MailAccount &account, const QString &folder, const QJsonObject &result);
    void fetchFolder(const MailAccount &owner, const QString &folder, int limit, Completion done);
    void setError(const QString &accountId, const QString &problem);
    void run(
        const QString &action,
        const MailAccount &account,
        const QJsonObject &extra,
        std::function<void(QString problem, QJsonObject result)> done
    );
    /// Wraps a completion so `busy()` is true for exactly as long as it runs.
    [[nodiscard]] Completion track(Completion done);

    std::filesystem::path directory_;
    MailSecrets secrets_;
    std::vector<MailAccount> accounts_;
    std::vector<std::pair<QString, std::vector<MailFolder>>> folders_;
    std::vector<MailMessage> messages_;
    std::vector<std::pair<QString, MailBody>> bodies_;
    std::vector<std::pair<QString, int>> totals_;
    std::vector<std::pair<QString, QString>> errors_;
    /// Ids that were marked read locally, so a refresh does not undo it.
    QStringList readIds_;
    MailPreferences preferences_;
    MailDraft draft_;
    Transport transport_;
    Verifier verifier_;
    QString selectedAccount_;
    QString selectedFolder_{QStringLiteral("INBOX")};
    QString selectedMessage_;
    QString search_;
    QString notice_;
    int limit_ = 100;
    int busy_ = 0;
    bool unreadOnly_ = false;
    bool loadFailed_ = false;
};

} // namespace yobro::spike
