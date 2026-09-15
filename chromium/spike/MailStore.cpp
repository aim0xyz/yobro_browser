#include "spike/MailStore.hpp"

#include "spike/Localization.hpp"
#include "spike/MailWorker.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace yobro::spike {
namespace {

const QString kInbox = QStringLiteral("INBOX");
const QString kIdKey = QStringLiteral("id");
const QString kFolderKey = QStringLiteral("folder");
const QString kValidityKey = QStringLiteral("validity");
const QString kUidKey = QStringLiteral("uid");
const QString kTargetKey = QStringLiteral("target");
const QString kWarningKey = QStringLiteral("warning");

/// The two mailbox names the store recognises as Trash by title, matching the
/// WebKit build and the helper script.
const QStringList &trashTitles() {
    static const QStringList titles{
        QStringLiteral("trash"), QStringLiteral("deleted"), QStringLiteral("deleted items"),
        QStringLiteral("papierkorb"), QStringLiteral("gelöscht"), QStringLiteral("gelöschte elemente"),
    };
    return titles;
}

QString hostProblem(const QString &host) {
    const QUrl probe(QStringLiteral("https://") + host);
    if (host.trimmed().isEmpty() || host.contains(QLatin1Char('/')) || host.contains(QLatin1Char(' '))
        || probe.host().isEmpty() || probe.host() != host.toLower())
        return L(QStringLiteral("Servername ohne Schema und ohne Pfad eintragen."),
                 QStringLiteral("Enter the server name without a scheme and without a path."));
    return {};
}

#if defined(__APPLE__)
/// Owns a CoreFoundation reference and releases it on scope exit.
template <typename Ref>
class CFHandle {
public:
    explicit CFHandle(Ref ref = nullptr) : ref_(ref) {}
    CFHandle(const CFHandle &) = delete;
    CFHandle &operator=(const CFHandle &) = delete;
    ~CFHandle() { if (ref_) CFRelease(ref_); }

    [[nodiscard]] Ref get() const { return ref_; }
    Ref *address() { return &ref_; }

private:
    Ref ref_ = nullptr;
};

CFStringRef makeString(const std::string &value) {
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(value.data()),
        static_cast<CFIndex>(value.size()),
        kCFStringEncodingUTF8,
        false
    );
}

CFMutableDictionaryRef makeQuery(const std::string &service, const std::string &account) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFHandle<CFStringRef> serviceRef(makeString(service));
    CFDictionarySetValue(query, kSecAttrService, serviceRef.get());
    CFHandle<CFStringRef> accountRef(makeString(account));
    CFDictionarySetValue(query, kSecAttrAccount, accountRef.get());
    return query;
}
#endif

} // namespace

// MARK: - Account

QJsonObject MailAccount::toJson() const {
    QJsonObject object;
    object.insert(kIdKey, id);
    object.insert(QStringLiteral("label"), label);
    object.insert(QStringLiteral("address"), address);
    object.insert(QStringLiteral("username"), username);
    object.insert(QStringLiteral("imapHost"), imapHost);
    object.insert(QStringLiteral("imapPort"), imapPort);
    object.insert(QStringLiteral("smtpHost"), smtpHost);
    object.insert(QStringLiteral("smtpPort"), smtpPort);
    object.insert(QStringLiteral("smtpSecurity"), smtpSecurity);
    if (!smtpUsername.isEmpty()) object.insert(QStringLiteral("smtpUsername"), smtpUsername);
    return object;
}

MailAccount MailAccount::fromJson(const QJsonObject &object) {
    MailAccount account;
    account.id = object.value(kIdKey).toString();
    account.label = object.value(QStringLiteral("label")).toString();
    account.address = object.value(QStringLiteral("address")).toString();
    account.username = object.value(QStringLiteral("username")).toString();
    account.imapHost = object.value(QStringLiteral("imapHost")).toString();
    account.imapPort = object.value(QStringLiteral("imapPort")).toInt(993);
    account.smtpHost = object.value(QStringLiteral("smtpHost")).toString();
    account.smtpPort = object.value(QStringLiteral("smtpPort")).toInt(465);
    account.smtpSecurity = object.value(QStringLiteral("smtpSecurity")).toString(QStringLiteral("tls"));
    account.smtpUsername = object.value(QStringLiteral("smtpUsername")).toString();
    return account;
}

QString MailAccount::validationProblem() const {
    if (id.isEmpty())
        return L(QStringLiteral("Dem Konto fehlt eine Kennung."), QStringLiteral("The account has no id."));
    if (!address.contains(QLatin1Char('@')) || address.startsWith(QLatin1Char('@'))
        || address.endsWith(QLatin1Char('@')))
        return L(QStringLiteral("Bitte eine gültige E-Mail-Adresse eingeben."),
                 QStringLiteral("Please enter a valid email address."));
    if (username.trimmed().isEmpty())
        return L(QStringLiteral("Benutzername eintragen."), QStringLiteral("Enter a user name."));
    if (const QString problem = hostProblem(imapHost); !problem.isEmpty()) return problem;
    if (const QString problem = hostProblem(smtpHost); !problem.isEmpty()) return problem;
    if (imapPort < 1 || imapPort > 65535 || smtpPort < 1 || smtpPort > 65535)
        return L(QStringLiteral("Port zwischen 1 und 65535 eintragen."),
                 QStringLiteral("Enter a port between 1 and 65535."));
    // Anything else would mean sending over an unencrypted connection.
    if (smtpSecurity != QStringLiteral("tls") && smtpSecurity != QStringLiteral("starttls"))
        return L(QStringLiteral("Für den Versand ist TLS oder STARTTLS nötig."),
                 QStringLiteral("Sending needs TLS or STARTTLS."));
    return {};
}

QString MailAccount::displayLabel() const {
    return label.trimmed().isEmpty() ? address : label;
}

// MARK: - Folder

MailFolder MailFolder::fromJson(const QJsonObject &object) {
    MailFolder folder;
    folder.path = object.value(QStringLiteral("path")).toString();
    folder.title = object.value(QStringLiteral("title")).toString();
    folder.delimiter = object.value(QStringLiteral("delimiter")).toString();
    folder.role = object.value(QStringLiteral("role")).toString();
    folder.selectable = object.value(QStringLiteral("selectable")).toBool(true);
    folder.unread = object.value(QStringLiteral("unread")).toInt(0);
    return folder;
}

QString MailFolder::displayTitle() const {
    if (path.toUpper() == kInbox) return L(QStringLiteral("Posteingang"), QStringLiteral("Inbox"));
    return title.isEmpty() ? path : title;
}

bool MailFolder::isTrash() const {
    return role == QStringLiteral("trash") || trashTitles().contains(title.toLower());
}

// MARK: - Message

QString MailMessage::id() const {
    return accountId + QLatin1Char(':') + folder + QLatin1Char(':') + validity + QLatin1Char(':') + uid;
}

MailMessage MailMessage::fromJson(
    const QJsonObject &row,
    const QString &accountId,
    const QString &folder,
    const QString &validity
) {
    MailMessage message;
    message.accountId = accountId;
    message.folder = folder;
    message.validity = validity;
    message.uid = row.value(kUidKey).toString();
    message.subject = row.value(QStringLiteral("subject")).toString();
    message.sender = row.value(QStringLiteral("sender")).toString();
    message.recipient = row.value(QStringLiteral("to")).toString();
    message.replyTo = row.value(QStringLiteral("replyTo")).toString();
    message.messageId = row.value(QStringLiteral("messageID")).toString();
    message.date = static_cast<qint64>(row.value(QStringLiteral("date")).toDouble());
    message.unread = row.value(QStringLiteral("unread")).toBool(false);
    return message;
}

bool MailDraft::isEmpty() const {
    return to.trimmed().isEmpty() && subject.trimmed().isEmpty() && text.trimmed().isEmpty();
}

// MARK: - Keychain

MailSecrets::MailSecrets(std::filesystem::path profileDirectory)
    : service_("YOBRO.Chromium.Mail." + profileDirectory.lexically_normal().string()) {}

#if defined(__APPLE__)

bool MailSecrets::available() const {
    return true;
}

bool MailSecrets::store(const QString &accountId, const QString &password) {
    if (accountId.isEmpty() || password.isEmpty()) return false;
    const QByteArray secret = password.toUtf8();
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, accountId.toStdString()));
    CFHandle<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(secret.constData()),
        static_cast<CFIndex>(secret.size())
    ));
    if (SecItemCopyMatching(query.get(), nullptr) == errSecSuccess) {
        CFHandle<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(update.get(), kSecValueData, data.get());
        return SecItemUpdate(query.get(), update.get()) == errSecSuccess;
    }
    CFDictionarySetValue(query.get(), kSecValueData, data.get());
    // The password stays on this device and is readable only while unlocked.
    CFDictionarySetValue(query.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    return SecItemAdd(query.get(), nullptr) == errSecSuccess;
}

QString MailSecrets::read(const QString &accountId) const {
    if (accountId.isEmpty()) return {};
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, accountId.toStdString()));
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFHandle<CFDataRef> data;
    if (SecItemCopyMatching(query.get(), reinterpret_cast<CFTypeRef *>(data.address())) != errSecSuccess
        || !data.get())
        return {};
    return QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data.get())),
        static_cast<int>(CFDataGetLength(data.get()))
    );
}

bool MailSecrets::remove(const QString &accountId) {
    if (accountId.isEmpty()) return false;
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, accountId.toStdString()));
    const OSStatus status = SecItemDelete(query.get());
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else

bool MailSecrets::available() const { return false; }
bool MailSecrets::store(const QString &, const QString &) { return false; }
QString MailSecrets::read(const QString &) const { return {}; }
bool MailSecrets::remove(const QString &) { return false; }

#endif

// MARK: - Store

MailStore::MailStore(std::filesystem::path profileDirectory, QObject *parent)
    : QObject(parent), directory_(std::move(profileDirectory)), secrets_(directory_) {
    const QString accountsPath = QString::fromStdString((directory_ / "mail-accounts.json").string());
    QFile accountsFile(accountsPath);
    if (accountsFile.exists()) {
        if (!accountsFile.open(QIODevice::ReadOnly)) {
            loadFailed_ = true;
            notice_ = L(QStringLiteral("Die Kontenliste konnte nicht gelesen werden."),
                        QStringLiteral("The account list could not be read."));
        } else {
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(accountsFile.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isArray()) {
                loadFailed_ = true;
                notice_ = L(QStringLiteral("Die Kontenliste ist beschädigt und wird nicht überschrieben."),
                            QStringLiteral("The account list is damaged and will not be overwritten."));
            } else {
                for (const QJsonValue &value : document.array()) {
                    MailAccount account = MailAccount::fromJson(value.toObject());
                    if (account.id.isEmpty()) continue;
                    accounts_.push_back(std::move(account));
                }
            }
        }
    }

    QFile preferencesFile(QString::fromStdString((directory_ / "mail-preferences.json").string()));
    if (preferencesFile.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(preferencesFile.readAll()).object();
        preferences_.notifications = root.value(QStringLiteral("notifications")).toBool(false);
        preferences_.remoteImages = root.value(QStringLiteral("remoteImages")).toBool(true);
        const QJsonObject known = root.value(QStringLiteral("knownInbox")).toObject();
        for (auto iterator = known.begin(); iterator != known.end(); ++iterator) {
            QStringList ids;
            for (const QJsonValue &value : iterator.value().toArray()) ids.append(value.toString());
            preferences_.knownInbox.emplace_back(iterator.key(), ids);
        }
    }

    transport_ = [this](
        const QString &action,
        const MailAccount &account,
        const QJsonObject &extra,
        std::function<void(QString, QJsonObject)> done
    ) {
        const QString password = secrets_.read(account.id);
        if (password.isEmpty()) {
            done(L(QStringLiteral("Zugangsdaten nicht verfügbar. Konto bearbeiten und erneut verbinden."),
                   QStringLiteral("Credentials are not available. Edit the account and connect again.")),
                 {});
            return;
        }
        QJsonObject request = extra;
        request.insert(QStringLiteral("action"), action);
        request.insert(QStringLiteral("password"), password);
        request.insert(QStringLiteral("account"), account.toJson());
        MailWorker::callAsync(request, 90'000, this, [done](MailWorker::Result result) {
            done(result.problem, result.value);
        });
    };

    verifier_ = [this](
        const MailAccount &candidate,
        const QString &password,
        std::function<void(QString)> done
    ) {
        QJsonObject request;
        request.insert(QStringLiteral("action"), QStringLiteral("test"));
        request.insert(QStringLiteral("password"), password);
        request.insert(QStringLiteral("account"), candidate.toJson());
        MailWorker::callAsync(request, 60'000, this, [done](MailWorker::Result result) {
            done(result.problem);
        });
    };
}

void MailStore::setTransport(Transport transport) {
    transport_ = std::move(transport);
}

void MailStore::setVerifier(Verifier verifier) {
    verifier_ = std::move(verifier);
}

void MailStore::run(
    const QString &action,
    const MailAccount &account,
    const QJsonObject &extra,
    std::function<void(QString problem, QJsonObject result)> done
) {
    transport_(action, account, extra, std::move(done));
}

MailStore::Completion MailStore::track(Completion done) {
    ++busy_;
    Q_EMIT changed();
    return [this, done = std::move(done)](QString problem) {
        if (busy_ > 0) --busy_;
        Q_EMIT changed();
        if (done) done(std::move(problem));
    };
}

std::optional<MailAccount> MailStore::account(const QString &id) const {
    for (const MailAccount &account : accounts_)
        if (account.id == id) return account;
    return std::nullopt;
}

const std::vector<MailFolder> &MailStore::folders(const QString &accountId) const {
    static const std::vector<MailFolder> empty;
    for (const auto &entry : folders_)
        if (entry.first == accountId) return entry.second;
    return empty;
}

std::vector<MailMessage> MailStore::messages(const QString &accountId, const QString &folder) const {
    std::vector<MailMessage> result;
    for (const MailMessage &message : messages_)
        if (message.accountId == accountId && message.folder == folder) result.push_back(message);
    std::sort(result.begin(), result.end(), [](const MailMessage &left, const MailMessage &right) {
        return left.date > right.date;
    });
    return result;
}

std::vector<MailMessage> MailStore::visibleMessages() const {
    std::vector<MailMessage> result;
    for (const MailMessage &message : messages_) {
        // Without a chosen account the inboxes of every mailbox are shown together.
        if (selectedAccount_.isEmpty()) {
            if (message.folder.toUpper() != kInbox) continue;
        } else if (message.accountId != selectedAccount_ || message.folder != selectedFolder_) {
            continue;
        }
        // The open message stays visible even when it is no longer unread, so
        // reading it does not make it disappear under the cursor.
        if (unreadOnly_ && !message.unread && message.id() != selectedMessage_) continue;
        if (!search_.isEmpty()) {
            const QString haystack = message.subject + QLatin1Char(' ') + message.sender;
            if (!haystack.contains(search_, Qt::CaseInsensitive)) continue;
        }
        result.push_back(message);
    }
    std::sort(result.begin(), result.end(), [](const MailMessage &left, const MailMessage &right) {
        return left.date > right.date;
    });
    return result;
}

std::optional<MailMessage> MailStore::message(const QString &id) const {
    for (const MailMessage &message : messages_)
        if (message.id() == id) return message;
    return std::nullopt;
}

std::optional<MailBody> MailStore::body(const QString &id) const {
    for (const auto &entry : bodies_)
        if (entry.first == id) return entry.second;
    return std::nullopt;
}

int MailStore::folderTotal(const QString &accountId, const QString &folder) const {
    const QString key = accountId + QLatin1Char(':') + folder;
    for (const auto &entry : totals_)
        if (entry.first == key) return entry.second;
    return 0;
}

bool MailStore::hasUnread() const {
    for (const MailAccount &account : accounts_)
        if (hasUnread(account.id)) return true;
    return false;
}

bool MailStore::hasUnread(const QString &accountId) const {
    const std::vector<MailFolder> &known = folders(accountId);
    if (!known.empty()) {
        for (const MailFolder &folder : known)
            if (folder.unread > 0) return true;
        return false;
    }
    for (const MailMessage &message : messages_)
        if (message.accountId == accountId && message.unread) return true;
    return false;
}

void MailStore::setSelectedMessage(const QString &id) {
    if (selectedMessage_ == id) return;
    selectedMessage_ = id;
    Q_EMIT changed();
}

void MailStore::setSearch(const QString &value) {
    if (search_ == value) return;
    search_ = value;
    Q_EMIT changed();
}

void MailStore::setUnreadOnly(bool value) {
    if (unreadOnly_ == value) return;
    unreadOnly_ = value;
    Q_EMIT changed();
}

void MailStore::setNotifications(bool enabled) {
    if (preferences_.notifications == enabled) return;
    preferences_.notifications = enabled;
    persistPreferences();
    Q_EMIT changed();
}

void MailStore::setRemoteImages(bool enabled) {
    if (preferences_.remoteImages == enabled) return;
    preferences_.remoteImages = enabled;
    persistPreferences();
    Q_EMIT changed();
}

void MailStore::beginReply(const MailMessage &message) {
    draft_ = MailDraft{};
    draft_.accountId = message.accountId;
    // Reply-To is what the sender asked replies to go to; the helper falls back
    // to From when the header is missing.
    draft_.to = message.replyTo.isEmpty() ? message.sender : message.replyTo;
    draft_.subject = message.subject.trimmed().toLower().startsWith(QStringLiteral("re:"))
        ? message.subject
        : QStringLiteral("Re: ") + message.subject;
    draft_.replyToMessageId = message.messageId;
    Q_EMIT changed();
}

void MailStore::clearDraft() {
    draft_ = MailDraft{};
    Q_EMIT changed();
}

QString MailStore::error(const QString &accountId) const {
    for (const auto &entry : errors_)
        if (entry.first == accountId) return entry.second;
    return {};
}

void MailStore::setError(const QString &accountId, const QString &problem) {
    for (auto iterator = errors_.begin(); iterator != errors_.end(); ++iterator) {
        if (iterator->first != accountId) continue;
        if (problem.isEmpty()) errors_.erase(iterator);
        else iterator->second = problem;
        return;
    }
    if (!problem.isEmpty()) errors_.emplace_back(accountId, problem);
}

void MailStore::setNotice(const QString &text) {
    notice_ = text;
    Q_EMIT changed();
}

void MailStore::persistAccounts() {
    // A damaged list is left alone: writing would drop every configured mailbox.
    if (loadFailed_) return;
    std::error_code code;
    std::filesystem::create_directories(directory_, code);
    QJsonArray array;
    for (const MailAccount &account : accounts_) array.append(account.toJson());
    QSaveFile file(QString::fromStdString((directory_ / "mail-accounts.json").string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    file.commit();
}

void MailStore::persistPreferences() {
    std::error_code code;
    std::filesystem::create_directories(directory_, code);
    QJsonObject known;
    for (const auto &entry : preferences_.knownInbox) {
        QJsonArray ids;
        for (const QString &id : entry.second) ids.append(id);
        known.insert(entry.first, ids);
    }
    QJsonObject root;
    root.insert(QStringLiteral("notifications"), preferences_.notifications);
    root.insert(QStringLiteral("remoteImages"), preferences_.remoteImages);
    root.insert(QStringLiteral("knownInbox"), known);
    QSaveFile file(QString::fromStdString((directory_ / "mail-preferences.json").string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

void MailStore::connectAccount(const MailAccount &account, const QString &password, Completion done) {
    Completion finish = track(std::move(done));
    if (loadFailed_) {
        finish(notice_);
        return;
    }
    if (const QString problem = account.validationProblem(); !problem.isEmpty()) {
        finish(problem);
        return;
    }
    if (password.isEmpty()) {
        finish(L(QStringLiteral("Passwort oder App-Passwort eintragen."),
                 QStringLiteral("Enter a password or app password.")));
        return;
    }

    // The password is handed to the check directly and only stored once the
    // server accepted it, so a typo never lands in the Keychain.
    verifier_(account, password, [this, account, password, finish](QString problem) {
        if (!problem.isEmpty()) {
            finish(problem);
            return;
        }
        if (!secrets_.store(account.id, password)) {
            finish(L(QStringLiteral("Das Passwort konnte nicht im Schlüsselbund gespeichert werden."),
                     QStringLiteral("The password could not be stored in the Keychain.")));
            return;
        }
        accounts_.erase(
            std::remove_if(accounts_.begin(), accounts_.end(), [&account](const MailAccount &existing) {
                return existing.id == account.id;
            }),
            accounts_.end()
        );
        accounts_.push_back(account);
        persistAccounts();
        messages_.erase(
            std::remove_if(messages_.begin(), messages_.end(), [&account](const MailMessage &message) {
                return message.accountId == account.id;
            }),
            messages_.end()
        );
        bodies_.clear();
        setError(account.id, {});
        finish({});
    });
}

QString MailStore::disconnectAccount(const QString &accountId) {
    if (busy_ > 0)
        return L(QStringLiteral("Bitte den laufenden Vorgang abwarten."),
                 QStringLiteral("Please wait for the running operation."));
    if (!account(accountId))
        return L(QStringLiteral("Konto nicht gefunden."), QStringLiteral("Account not found."));
    if (!secrets_.remove(accountId))
        return L(QStringLiteral("Das Passwort konnte nicht entfernt werden."),
                 QStringLiteral("The password could not be removed."));

    accounts_.erase(
        std::remove_if(accounts_.begin(), accounts_.end(), [&accountId](const MailAccount &existing) {
            return existing.id == accountId;
        }),
        accounts_.end()
    );
    persistAccounts();
    messages_.erase(
        std::remove_if(messages_.begin(), messages_.end(), [&accountId](const MailMessage &message) {
            return message.accountId == accountId;
        }),
        messages_.end()
    );
    bodies_.clear();
    folders_.erase(
        std::remove_if(folders_.begin(), folders_.end(), [&accountId](const auto &entry) {
            return entry.first == accountId;
        }),
        folders_.end()
    );
    setError(accountId, {});
    if (selectedAccount_ == accountId) {
        selectedAccount_.clear();
        selectedFolder_ = kInbox;
        selectedMessage_.clear();
    }
    if (draft_.accountId == accountId) draft_ = MailDraft{};
    Q_EMIT changed();
    return {};
}

void MailStore::applyFolderResponse(
    const MailAccount &account,
    const QString &folder,
    const QJsonObject &result
) {
    const QString validity = result.value(kValidityKey).toString();
    std::vector<MailMessage> loaded;
    for (const QJsonValue &value : result.value(QStringLiteral("messages")).toArray()) {
        MailMessage message = MailMessage::fromJson(value.toObject(), account.id, folder, validity);
        if (message.uid.isEmpty()) continue;
        // A message the provider now reports as unread really is unread again.
        if (!message.unread) readIds_.removeAll(message.id());
        else if (readIds_.contains(message.id())) message.unread = false;
        loaded.push_back(std::move(message));
    }

    if (folder.toUpper() == kInbox) {
        QStringList ids;
        for (const MailMessage &message : loaded) ids.append(message.id());
        QStringList known;
        for (const auto &entry : preferences_.knownInbox)
            if (entry.first == account.id) known = entry.second;
        if (preferences_.notifications && !known.isEmpty()) {
            const std::vector<MailMessage> fresh = newInboxMessages(loaded, known);
            if (!fresh.empty()) {
                Q_EMIT newMail(
                    account.id,
                    static_cast<int>(fresh.size()),
                    fresh.front().sender,
                    fresh.front().subject
                );
            }
        }
        // Ids of the current mailbox generation are kept, older ones dropped.
        const QString prefix = QStringLiteral(":INBOX:") + validity + QLatin1Char(':');
        QStringList merged = ids;
        for (const QString &id : known)
            if (id.contains(prefix) && !merged.contains(id)) merged.append(id);
        bool replaced = false;
        for (auto &entry : preferences_.knownInbox) {
            if (entry.first != account.id) continue;
            entry.second = merged;
            replaced = true;
        }
        if (!replaced) preferences_.knownInbox.emplace_back(account.id, merged);
        persistPreferences();
    }

    messages_.erase(
        std::remove_if(messages_.begin(), messages_.end(), [&account, &folder](const MailMessage &message) {
            return message.accountId == account.id && message.folder == folder;
        }),
        messages_.end()
    );
    for (MailMessage &message : loaded) messages_.push_back(std::move(message));

    const QString key = account.id + QLatin1Char(':') + folder;
    const int total = result.value(QStringLiteral("total")).toInt(static_cast<int>(loaded.size()));
    bool found = false;
    for (auto &entry : totals_) {
        if (entry.first != key) continue;
        entry.second = total;
        found = true;
    }
    if (!found) totals_.emplace_back(key, total);
}

void MailStore::fetchFolder(const MailAccount &owner, const QString &folder, int limit, Completion done) {
    QJsonObject extra;
    extra.insert(kFolderKey, folder);
    extra.insert(QStringLiteral("limit"), limit);
    const QString accountId = owner.id;
    run(QStringLiteral("list"), owner, extra,
        [this, accountId, folder, done](QString problem, QJsonObject result) {
        const std::optional<MailAccount> current = account(accountId);
        // The account may have been disconnected while the mailbox answered.
        if (!current) {
            if (done) done({});
            return;
        }
        if (!problem.isEmpty()) {
            setError(accountId, problem);
            Q_EMIT changed();
            if (done) done(problem);
            return;
        }
        applyFolderResponse(*current, folder, result);
        setError(accountId, {});
        Q_EMIT changed();
        if (done) done({});
    });
}

void MailStore::refresh(Completion done) {
    Completion finish = track(std::move(done));
    if (accounts_.empty()) {
        finish({});
        return;
    }

    // The accounts are walked one after another so two mailboxes never fight
    // over the same message list.
    auto remaining = std::make_shared<QStringList>();
    for (const MailAccount &account : accounts_) remaining->append(account.id);
    auto lastProblem = std::make_shared<QString>();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, remaining, lastProblem, finish, step]() {
        if (remaining->isEmpty()) {
            // Bodies of messages that are gone must not linger.
            QStringList valid;
            for (const MailMessage &message : messages_) valid.append(message.id());
            bodies_.erase(
                std::remove_if(bodies_.begin(), bodies_.end(), [&valid](const auto &entry) {
                    return !valid.contains(entry.first);
                }),
                bodies_.end()
            );
            Q_EMIT changed();
            finish(*lastProblem);
            step->operator=(nullptr);
            return;
        }
        const QString accountId = remaining->takeFirst();
        const std::optional<MailAccount> current = account(accountId);
        if (!current) {
            (*step)();
            return;
        }
        run(QStringLiteral("folders"), *current, {},
            [this, accountId, remaining, lastProblem, step](QString problem, QJsonObject result) {
            const std::optional<MailAccount> current = account(accountId);
            if (!current) {
                (*step)();
                return;
            }
            if (!problem.isEmpty()) {
                setError(accountId, problem);
                *lastProblem = problem;
                Q_EMIT changed();
                (*step)();
                return;
            }
            std::vector<MailFolder> loaded;
            for (const QJsonValue &value : result.value(QStringLiteral("folders")).toArray())
                loaded.push_back(MailFolder::fromJson(value.toObject()));
            bool replaced = false;
            for (auto &entry : folders_) {
                if (entry.first != accountId) continue;
                entry.second = loaded;
                replaced = true;
            }
            if (!replaced) folders_.emplace_back(accountId, loaded);

            const bool viewing = selectedAccount_ == accountId && selectedFolder_ == kInbox;
            fetchFolder(*current, kInbox, viewing ? limit_ : 100,
                        [this, accountId, remaining, lastProblem, step](QString inboxProblem) {
                if (!inboxProblem.isEmpty()) *lastProblem = inboxProblem;
                const std::optional<MailAccount> current = account(accountId);
                // A second folder is only reloaded when it is the one on screen.
                if (current && selectedAccount_ == accountId && selectedFolder_ != kInbox) {
                    fetchFolder(*current, selectedFolder_, limit_,
                                [lastProblem, step](QString folderProblem) {
                        if (!folderProblem.isEmpty()) *lastProblem = folderProblem;
                        (*step)();
                    });
                    return;
                }
                (*step)();
            });
        });
    };
    (*step)();
}

void MailStore::selectFolder(const QString &accountId, const QString &folder, Completion done) {
    selectedAccount_ = accountId;
    selectedFolder_ = folder.isEmpty() ? kInbox : folder;
    selectedMessage_.clear();
    limit_ = 100;
    Q_EMIT changed();

    const std::optional<MailAccount> current = account(accountId);
    if (!current) {
        if (done) done({});
        return;
    }
    Completion finish = track(std::move(done));
    fetchFolder(*current, selectedFolder_, limit_, finish);
}

void MailStore::loadMore(Completion done) {
    const std::optional<MailAccount> current = account(selectedAccount_);
    if (!current) {
        if (done) done({});
        return;
    }
    // The same ceiling the helper enforces, so the request is never refused for
    // asking too much.
    limit_ = std::min(5000, limit_ + 100);
    Completion finish = track(std::move(done));
    fetchFolder(*current, selectedFolder_, limit_, finish);
}

void MailStore::openMessage(const QString &messageId, Completion done) {
    const std::optional<MailMessage> target = message(messageId);
    if (!target) {
        if (done) done(L(QStringLiteral("Nachricht nicht gefunden."), QStringLiteral("Message not found.")));
        return;
    }
    const std::optional<MailAccount> current = account(target->accountId);
    if (!current) {
        if (done) done(L(QStringLiteral("Konto nicht gefunden."), QStringLiteral("Account not found.")));
        return;
    }
    const bool haveBody = body(messageId).has_value();
    if (haveBody && !target->unread) {
        selectedMessage_ = messageId;
        Q_EMIT changed();
        if (done) done({});
        return;
    }

    selectedMessage_ = messageId;
    Completion finish = track(std::move(done));
    QJsonObject extra;
    extra.insert(kUidKey, target->uid);
    extra.insert(kValidityKey, target->validity);
    extra.insert(kFolderKey, target->folder);
    run(haveBody ? QStringLiteral("seen") : QStringLiteral("body"), *current, extra,
        [this, messageId, finish](QString problem, QJsonObject result) {
        const std::optional<MailMessage> target = message(messageId);
        if (!target) {
            finish({});
            return;
        }
        if (!problem.isEmpty()) {
            setError(target->accountId, problem);
            Q_EMIT changed();
            finish(problem);
            return;
        }
        if (result.contains(QStringLiteral("text"))) {
            MailBody loaded;
            loaded.text = result.value(QStringLiteral("text")).toString();
            loaded.html = result.value(QStringLiteral("html")).toString();
            for (const QJsonValue &value : result.value(QStringLiteral("attachments")).toArray()) {
                const QJsonObject row = value.toObject();
                MailAttachment attachment;
                attachment.id = row.value(kIdKey).toString();
                attachment.name = row.value(QStringLiteral("name")).toString();
                attachment.mime = row.value(QStringLiteral("mime")).toString();
                attachment.size = row.value(QStringLiteral("size")).toInt();
                loaded.attachments.push_back(std::move(attachment));
            }
            bool replaced = false;
            for (auto &entry : bodies_) {
                if (entry.first != messageId) continue;
                entry.second = loaded;
                replaced = true;
            }
            if (!replaced) bodies_.emplace_back(messageId, loaded);
        }

        if (result.value(QStringLiteral("seen")).toBool(false)) {
            if (!readIds_.contains(messageId)) readIds_.append(messageId);
            for (MailMessage &stored : messages_) {
                if (stored.id() != messageId || !stored.unread) continue;
                stored.unread = false;
                for (auto &entry : folders_) {
                    if (entry.first != stored.accountId) continue;
                    for (MailFolder &folder : entry.second)
                        if (folder.path == stored.folder) folder.unread = std::max(0, folder.unread - 1);
                }
            }
        } else if (const QString warning = result.value(kWarningKey).toString(); !warning.isEmpty()) {
            notice_ = warning;
        }
        setError(target->accountId, {});
        Q_EMIT changed();
        finish({});
    });
}

void MailStore::sendDraft(Completion done) {
    const std::optional<MailAccount> current = account(draft_.accountId);
    if (!current) {
        if (done) done(L(QStringLiteral("Absenderkonto wählen."), QStringLiteral("Choose a sending account.")));
        return;
    }
    if (draft_.to.trimmed().isEmpty()) {
        if (done) done(L(QStringLiteral("Empfänger eintragen."), QStringLiteral("Enter a recipient.")));
        return;
    }

    Completion finish = track(std::move(done));
    QJsonObject extra;
    extra.insert(QStringLiteral("to"), draft_.to);
    extra.insert(QStringLiteral("subject"), draft_.subject);
    extra.insert(QStringLiteral("text"), draft_.text);
    extra.insert(QStringLiteral("replyID"), draft_.replyToMessageId);
    if (!draft_.html.isEmpty()) extra.insert(QStringLiteral("html"), draft_.html);
    run(QStringLiteral("send"), *current, extra, [this, finish](QString problem, QJsonObject result) {
        if (!problem.isEmpty()) {
            // The draft is kept so nothing the user wrote is lost.
            notice_ = problem;
            Q_EMIT changed();
            finish(problem);
            return;
        }
        QStringList refused;
        for (const QJsonValue &value : result.value(QStringLiteral("refused")).toArray())
            refused.append(value.toString());
        draft_ = MailDraft{};
        if (!refused.isEmpty()) {
            notice_ = L(QStringLiteral("Teilweise versendet. Nicht angenommene Empfänger: "),
                        QStringLiteral("Partly sent. Refused recipients: "))
                + refused.join(QStringLiteral(", "));
        } else if (const QString warning = result.value(kWarningKey).toString(); !warning.isEmpty()) {
            notice_ = warning;
        } else {
            notice_ = L(QStringLiteral("Die Nachricht wurde versendet und in Gesendet gespeichert."),
                        QStringLiteral("The message was sent and saved to Sent."));
        }
        Q_EMIT changed();
        finish({});
    });
}

void MailStore::deleteMessages(const QStringList &messageIds, Completion done) {
    moveMessages(messageIds, {}, std::move(done));
}

void MailStore::moveMessages(const QStringList &messageIds, const QString &target, Completion done) {
    if (messageIds.isEmpty()) {
        if (done) done({});
        return;
    }
    // Every request covers one account, one folder and one mailbox generation,
    // because the uids only mean anything within that combination.
    const std::optional<MailMessage> first = message(messageIds.first());
    if (!first) {
        if (done) done(L(QStringLiteral("Nachricht nicht gefunden."), QStringLiteral("Message not found.")));
        return;
    }
    const std::optional<MailAccount> current = account(first->accountId);
    if (!current) {
        if (done) done(L(QStringLiteral("Konto nicht gefunden."), QStringLiteral("Account not found.")));
        return;
    }
    QJsonArray uids;
    QStringList affected;
    for (const QString &id : messageIds) {
        const std::optional<MailMessage> item = message(id);
        if (!item) continue;
        if (item->accountId != first->accountId || item->folder != first->folder
            || item->validity != first->validity)
            continue;
        uids.append(item->uid);
        affected.append(id);
    }
    if (uids.isEmpty()) {
        if (done) done(L(QStringLiteral("Ungültige Nachrichtenauswahl."),
                         QStringLiteral("Invalid message selection.")));
        return;
    }

    Completion finish = track(std::move(done));
    QJsonObject extra;
    extra.insert(kFolderKey, first->folder);
    extra.insert(kValidityKey, first->validity);
    extra.insert(QStringLiteral("uids"), uids);
    if (!target.isEmpty()) extra.insert(kTargetKey, target);
    const QString accountId = first->accountId;
    run(target.isEmpty() ? QStringLiteral("delete") : QStringLiteral("move"), *current, extra,
        [this, accountId, affected, finish](QString problem, QJsonObject) {
        if (!problem.isEmpty()) {
            setError(accountId, problem);
            Q_EMIT changed();
            finish(problem);
            return;
        }
        messages_.erase(
            std::remove_if(messages_.begin(), messages_.end(), [&affected](const MailMessage &message) {
                return affected.contains(message.id());
            }),
            messages_.end()
        );
        bodies_.erase(
            std::remove_if(bodies_.begin(), bodies_.end(), [&affected](const auto &entry) {
                return affected.contains(entry.first);
            }),
            bodies_.end()
        );
        if (affected.contains(selectedMessage_)) selectedMessage_.clear();
        setError(accountId, {});
        Q_EMIT changed();
        finish({});
    });
}

void MailStore::readAttachment(
    const QString &messageId,
    const QString &attachmentId,
    std::function<void(QString problem, QByteArray data)> done
) {
    const std::optional<MailMessage> target = message(messageId);
    if (!target) {
        done(L(QStringLiteral("Nachricht nicht gefunden."), QStringLiteral("Message not found.")), {});
        return;
    }
    const std::optional<MailAccount> current = account(target->accountId);
    if (!current) {
        done(L(QStringLiteral("Konto nicht gefunden."), QStringLiteral("Account not found.")), {});
        return;
    }

    Completion finish = track(nullptr);
    QJsonObject extra;
    extra.insert(kUidKey, target->uid);
    extra.insert(kValidityKey, target->validity);
    extra.insert(kFolderKey, target->folder);
    extra.insert(QStringLiteral("attachment"), attachmentId);
    run(QStringLiteral("attachment"), *current, extra,
        [finish, done](QString problem, QJsonObject result) {
        finish({});
        if (!problem.isEmpty()) {
            done(problem, {});
            return;
        }
        const QByteArray data = QByteArray::fromBase64(
            result.value(QStringLiteral("data")).toString().toUtf8(),
            QByteArray::AbortOnBase64DecodingErrors
        );
        if (data.isEmpty()) {
            done(L(QStringLiteral("Anhang konnte nicht gelesen werden."),
                   QStringLiteral("The attachment could not be read.")), {});
            return;
        }
        done({}, data);
    });
}

std::vector<MailMessage> MailStore::newInboxMessages(
    const std::vector<MailMessage> &loaded,
    const QStringList &known
) {
    std::vector<MailMessage> fresh;
    if (loaded.empty()) return fresh;
    const MailMessage &sample = loaded.front();
    const QString prefix = sample.accountId + QLatin1Char(':') + sample.folder + QLatin1Char(':')
        + sample.validity + QLatin1Char(':');

    bool haveHighest = false;
    unsigned long long highest = 0;
    for (const QString &id : known) {
        if (!id.startsWith(prefix)) continue;
        bool valid = false;
        const unsigned long long uid = id.mid(prefix.size()).toULongLong(&valid);
        if (!valid) continue;
        haveHighest = true;
        highest = std::max(highest, uid);
    }
    // A mailbox nobody has looked at yet announces what is unread. A mailbox
    // whose generation changed announces nothing, because every uid is new.
    if (!haveHighest) {
        if (!known.isEmpty()) return fresh;
        for (const MailMessage &message : loaded)
            if (message.unread) fresh.push_back(message);
        return fresh;
    }
    for (const MailMessage &message : loaded) {
        if (!message.unread) continue;
        bool valid = false;
        const unsigned long long uid = message.uid.toULongLong(&valid);
        if (valid && uid > highest) fresh.push_back(message);
    }
    return fresh;
}

} // namespace yobro::spike
