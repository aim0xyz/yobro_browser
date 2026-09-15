#include "spike/SyncAccount.hpp"

#include "spike/Localization.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrlQuery>
#include <QUuid>

#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace yobro::spike {
namespace {

/// The project the app ships with, overridable for a test or self-hosted setup.
const QString kFallbackUrl = QStringLiteral("https://bbjiumzwufgakgijixwr.supabase.co");
const QString kFallbackKey = QStringLiteral("sb_publishable_VyAQG0hfzFI6d9rgznC1_g_Bu6NWESW");

const QString kSessionAccount = QStringLiteral("session");
const QString kKeyAccount = QStringLiteral("key");

QString unexpectedAnswer() {
    return L(QStringLiteral("Unerwartete Antwort des Kontodienstes."),
             QStringLiteral("Unexpected answer from the account service."));
}

#if defined(__APPLE__)
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

// MARK: - Project

bool SupabaseProject::isUsableUrl(const QUrl &candidate) {
    // An access token is attached to every request, so plain HTTP or a URL
    // without a host is refused outright.
    return candidate.isValid() && candidate.scheme().toLower() == QStringLiteral("https")
        && !candidate.host().isEmpty() && candidate.userName().isEmpty()
        && candidate.password().isEmpty();
}

SupabaseProject SupabaseProject::resolve(const std::filesystem::path &root) {
    QString url = qEnvironmentVariable("YOBRO_SUPABASE_URL");
    QString key = qEnvironmentVariable("YOBRO_SUPABASE_PUBLISHABLE_KEY");

    if (url.isEmpty() || key.isEmpty()) {
        QFile file(QString::fromStdString((root / "supabase.json").string()));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject stored = QJsonDocument::fromJson(file.readAll()).object();
            if (url.isEmpty()) url = stored.value(QStringLiteral("url")).toString();
            if (key.isEmpty()) key = stored.value(QStringLiteral("publishableKey")).toString();
        }
    }

    const QUrl parsed(url);
    if (!isUsableUrl(parsed) || key.trimmed().isEmpty())
        return {QUrl(kFallbackUrl), kFallbackKey};
    return {parsed, key};
}

// MARK: - Session

bool SupabaseAuthSession::isValid() const {
    return !accessToken.isEmpty() && !userId.isEmpty();
}

QJsonObject SupabaseAuthSession::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("accessToken"), accessToken);
    object.insert(QStringLiteral("refreshToken"), refreshToken);
    object.insert(QStringLiteral("userId"), userId);
    object.insert(QStringLiteral("email"), email);
    return object;
}

SupabaseAuthSession SupabaseAuthSession::fromJson(const QJsonObject &object) {
    SupabaseAuthSession session;
    session.accessToken = object.value(QStringLiteral("accessToken")).toString();
    session.refreshToken = object.value(QStringLiteral("refreshToken")).toString();
    session.userId = object.value(QStringLiteral("userId")).toString();
    session.email = object.value(QStringLiteral("email")).toString();
    return session;
}

// MARK: - Keychain

SyncSecrets::SyncSecrets(std::filesystem::path profileDirectory)
    : service_("YOBRO.Chromium.Sync." + profileDirectory.lexically_normal().string()) {}

#if defined(__APPLE__)

bool SyncSecrets::available() const {
    return true;
}

bool SyncSecrets::store(const QString &account, const QByteArray &value) {
    if (account.isEmpty() || value.isEmpty()) return false;
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, account.toStdString()));
    CFHandle<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(value.constData()),
        static_cast<CFIndex>(value.size())
    ));
    if (SecItemCopyMatching(query.get(), nullptr) == errSecSuccess) {
        CFHandle<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(update.get(), kSecValueData, data.get());
        return SecItemUpdate(query.get(), update.get()) == errSecSuccess;
    }
    CFDictionarySetValue(query.get(), kSecValueData, data.get());
    // Sync has to work right after a restart, so this is readable after the
    // first unlock rather than only while the screen is unlocked.
    CFDictionarySetValue(query.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    return SecItemAdd(query.get(), nullptr) == errSecSuccess;
}

QByteArray SyncSecrets::read(const QString &account) const {
    if (account.isEmpty()) return {};
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, account.toStdString()));
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFHandle<CFDataRef> data;
    if (SecItemCopyMatching(query.get(), reinterpret_cast<CFTypeRef *>(data.address())) != errSecSuccess
        || !data.get())
        return {};
    return QByteArray(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data.get())),
        static_cast<int>(CFDataGetLength(data.get()))
    );
}

bool SyncSecrets::remove(const QString &account) {
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_, account.toStdString()));
    const OSStatus status = SecItemDelete(query.get());
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else

bool SyncSecrets::available() const { return false; }
bool SyncSecrets::store(const QString &, const QByteArray &) { return false; }
QByteArray SyncSecrets::read(const QString &) const { return {}; }
bool SyncSecrets::remove(const QString &) { return false; }

#endif

bool SyncSecrets::storeSession(const SupabaseAuthSession &session) {
    return store(kSessionAccount, QJsonDocument(session.toJson()).toJson(QJsonDocument::Compact));
}

std::optional<SupabaseAuthSession> SyncSecrets::readSession() const {
    const QByteArray stored = read(kSessionAccount);
    if (stored.isEmpty()) return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(stored);
    if (!document.isObject()) return std::nullopt;
    const SupabaseAuthSession session = SupabaseAuthSession::fromJson(document.object());
    if (!session.isValid()) return std::nullopt;
    return session;
}

bool SyncSecrets::removeSession() {
    return remove(kSessionAccount);
}

bool SyncSecrets::storeKey(const QByteArray &key) {
    if (key.size() != SyncCipher::keyBytes) return false;
    return store(kKeyAccount, key);
}

QByteArray SyncSecrets::readKey() const {
    const QByteArray key = read(kKeyAccount);
    return key.size() == SyncCipher::keyBytes ? key : QByteArray();
}

bool SyncSecrets::removeKey() {
    return remove(kKeyAccount);
}

// MARK: - Supabase REST

class SupabaseClient::Impl {
public:
    SupabaseProject project;
    SupabaseAuthSession session;
    QNetworkAccessManager *network = nullptr;
};

SupabaseClient::SupabaseClient(SupabaseProject project, QObject *context)
    : impl_(std::make_shared<Impl>()) {
    impl_->project = std::move(project);
    impl_->network = new QNetworkAccessManager(context);
    // A redirect could carry the access token to a host nobody approved.
    impl_->network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

void SupabaseClient::setSession(const SupabaseAuthSession &session) {
    impl_->session = session;
}

QUrl SupabaseClient::endpoint(const QString &path) const {
    QUrl url = impl_->project.url;
    QString base = url.path();
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    url.setPath(base + path);
    // Matching the configured host exactly is tighter than a suffix check and
    // keeps working for a self-hosted project.
    if (!SupabaseProject::isUsableUrl(url) || url.host() != impl_->project.url.host()) return {};
    return url;
}

namespace {

/// Reads the message a Supabase error carries, or falls back to the status.
QString supabaseProblem(int status, const QByteArray &body) {
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    for (const QString &field : {QStringLiteral("msg"), QStringLiteral("message"),
                                 QStringLiteral("error_description"), QStringLiteral("hint"),
                                 QStringLiteral("error")}) {
        const QString value = root.value(field).toString();
        if (!value.trimmed().isEmpty()) return value.left(300);
    }
    return L(QStringLiteral("Sync-Serverfehler ("), QStringLiteral("Sync server error ("))
        + QString::number(status) + QStringLiteral(").");
}

} // namespace

void SupabaseClient::fetch(
    const QString &profileId,
    std::function<void(QString problem, std::optional<SyncRecord>)> done
) {
    QUrl url = endpoint(QStringLiteral("rest/v1/browser_sync"));
    if (url.isEmpty()) {
        done(L(QStringLiteral("Ungültige Supabase-Adresse."), QStringLiteral("Invalid Supabase URL.")), {});
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("select"),
                       QStringLiteral("user_id,profile_id,revision,payload,updated_at"));
    query.addQueryItem(QStringLiteral("profile_id"), QStringLiteral("eq.") + profileId.toLower());
    url.setQuery(query);

    QNetworkRequest request{url};
    request.setTransferTimeout(30'000);
    request.setRawHeader("apikey", impl_->project.publishableKey.toUtf8());
    request.setRawHeader("Authorization", "Bearer " + impl_->session.accessToken.toUtf8());
    auto impl = impl_;
    QNetworkReply *reply = impl_->network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, impl, done]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (status < 200 || status >= 300) {
            done(supabaseProblem(status, body), {});
            return;
        }
        const QJsonArray rows = QJsonDocument::fromJson(body).array();
        if (rows.isEmpty()) {
            done({}, std::nullopt);
            return;
        }
        const QJsonObject row = rows.first().toObject();
        // A row belonging to another account must never be opened.
        if (row.value(QStringLiteral("user_id")).toString() != impl->session.userId) {
            done(L(QStringLiteral("Ungültige Sync-Antwort."), QStringLiteral("Invalid sync response.")), {});
            return;
        }
        const QByteArray envelope = QByteArray::fromBase64(
            row.value(QStringLiteral("payload")).toString().toUtf8(),
            QByteArray::AbortOnBase64DecodingErrors
        );
        const QJsonDocument document = QJsonDocument::fromJson(envelope);
        if (!document.isObject()) {
            done(L(QStringLiteral("Ungültige Sync-Antwort."), QStringLiteral("Invalid sync response.")), {});
            return;
        }
        SyncRecord record;
        record.profileId = row.value(QStringLiteral("profile_id")).toString();
        record.revision = static_cast<qint64>(row.value(QStringLiteral("revision")).toDouble());
        record.modifiedAt = QDateTime::fromString(
            row.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
        record.payload = EncryptedSyncPayload::fromJson(document.object());
        done({}, record);
    });
}

void SupabaseClient::push(
    const QString &profileId,
    qint64 expectedRevision,
    const EncryptedSyncPayload &payload,
    std::function<void(QString problem, std::optional<SyncRecord>)> done
) {
    const QUrl url = endpoint(QStringLiteral("rest/v1/rpc/push_browser_sync"));
    if (url.isEmpty()) {
        done(L(QStringLiteral("Ungültige Supabase-Adresse."), QStringLiteral("Invalid Supabase URL.")), {});
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("p_profile_id"), profileId.toLower());
    if (expectedRevision < 0) body.insert(QStringLiteral("p_expected_revision"), QJsonValue());
    else body.insert(QStringLiteral("p_expected_revision"), static_cast<double>(expectedRevision));
    body.insert(QStringLiteral("p_payload"),
                QString::fromUtf8(QJsonDocument(payload.toJson()).toJson(QJsonDocument::Compact).toBase64()));

    QNetworkRequest request{url};
    request.setTransferTimeout(30'000);
    request.setRawHeader("apikey", impl_->project.publishableKey.toUtf8());
    request.setRawHeader("Authorization", "Bearer " + impl_->session.accessToken.toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    auto impl = impl_;
    QNetworkReply *reply =
        impl_->network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, impl, done]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray answer = reply->readAll();
        reply->deleteLater();
        if (status < 200 || status >= 300) {
            done(supabaseProblem(status, answer), {});
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(answer);
        const QJsonObject row = document.isArray() ? document.array().first().toObject()
                                                   : document.object();
        if (row.isEmpty()) {
            done(L(QStringLiteral("Leere Sync-Antwort."), QStringLiteral("Empty sync response.")), {});
            return;
        }
        SyncRecord record;
        record.profileId = row.value(QStringLiteral("profile_id")).toString();
        record.revision = static_cast<qint64>(row.value(QStringLiteral("revision")).toDouble());
        record.modifiedAt = QDateTime::fromString(
            row.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
        done({}, record);
    });
}

namespace {

/// Turns an auth answer into a session, or explains why it is not one.
void readAuthAnswer(
    int status,
    const QByteArray &body,
    const AuthClient::Answer &done
) {
    if (status < 200 || status >= 300) {
        done(supabaseProblem(status, body), std::nullopt);
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QString token = root.value(QStringLiteral("access_token")).toString();
    if (token.isEmpty()) {
        // A project with e-mail confirmation answers without a token. That is
        // not a failure, and the caller must not look signed in.
        done({}, std::nullopt);
        return;
    }
    SupabaseAuthSession session;
    session.accessToken = token;
    session.refreshToken = root.value(QStringLiteral("refresh_token")).toString();
    const QJsonObject user = root.value(QStringLiteral("user")).toObject();
    session.userId = user.value(QStringLiteral("id")).toString();
    session.email = user.value(QStringLiteral("email")).toString();
    if (!session.isValid()) {
        done(unexpectedAnswer(), std::nullopt);
        return;
    }
    done({}, session);
}

} // namespace

void SupabaseClient::signIn(const QString &email, const QString &password, Answer done) {
    const QUrl url = endpoint(QStringLiteral("auth/v1/token?grant_type=password"));
    if (url.isEmpty()) {
        done(L(QStringLiteral("Ungültige Auth-Adresse."), QStringLiteral("Invalid authentication URL.")),
             std::nullopt);
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("email"), email);
    body.insert(QStringLiteral("password"), password);

    QNetworkRequest request{url};
    request.setTransferTimeout(30'000);
    request.setRawHeader("apikey", impl_->project.publishableKey.toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    QNetworkReply *reply =
        impl_->network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        readAuthAnswer(status, body, done);
    });
}

void SupabaseClient::signUp(const QString &email, const QString &password, Answer done) {
    const QUrl url = endpoint(QStringLiteral("auth/v1/signup"));
    if (url.isEmpty()) {
        done(L(QStringLiteral("Ungültige Auth-Adresse."), QStringLiteral("Invalid authentication URL.")),
             std::nullopt);
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("email"), email);
    body.insert(QStringLiteral("password"), password);

    QNetworkRequest request{url};
    request.setTransferTimeout(30'000);
    request.setRawHeader("apikey", impl_->project.publishableKey.toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    QNetworkReply *reply =
        impl_->network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        readAuthAnswer(status, body, done);
    });
}

void SupabaseClient::signOut(const QString &accessToken) {
    const QUrl url = endpoint(QStringLiteral("auth/v1/logout"));
    if (url.isEmpty() || accessToken.isEmpty()) return;
    QNetworkRequest request{url};
    request.setTransferTimeout(15'000);
    request.setRawHeader("apikey", impl_->project.publishableKey.toUtf8());
    request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    QNetworkReply *reply = impl_->network->post(request, QByteArray());
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply]() { reply->deleteLater(); });
}

// MARK: - Controller

SyncController::SyncController(
    std::filesystem::path profileDirectory,
    SnapshotReader reader,
    SnapshotWriter writer,
    QObject *parent
)
    : QObject(parent),
      directory_(std::move(profileDirectory)),
      reader_(std::move(reader)),
      writer_(std::move(writer)),
      secrets_(directory_) {
    QFile file(QString::fromStdString((directory_ / "sync.json").string()));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        profileId_ = root.value(QStringLiteral("profileId")).toString();
        revision_ = static_cast<qint64>(root.value(QStringLiteral("revision")).toDouble(-1));
    }
    // The backend keys its rows by this id, so it is created once and kept.
    if (profileId_.isEmpty()) {
        profileId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        persist();
    }

    session_ = secrets_.readSession();
    key_ = secrets_.readKey();
    supabase_ = std::make_unique<SupabaseClient>(SupabaseProject::resolve(directory_.parent_path()), this);
    if (session_) supabase_->setSession(*session_);
    service_ = supabase_.get();
    auth_ = supabase_.get();
    if (signedIn() && hasKey())
        status_ = L(QStringLiteral("Verbunden"), QStringLiteral("Connected"));
}

void SyncController::setService(SyncService *service) {
    service_ = service;
}

void SyncController::setAuthClient(AuthClient *client) {
    auth_ = client;
}

QString SyncController::email() const {
    return session_ ? session_->email : QString();
}

QString SyncController::recoveryCode() const {
    return hasKey() ? SyncCipher::encodeKey(key_) : QString();
}

void SyncController::setStatus(const QString &text) {
    status_ = text;
    Q_EMIT changed();
}

void SyncController::persist() {
    std::error_code code;
    std::filesystem::create_directories(directory_, code);
    QJsonObject root;
    root.insert(QStringLiteral("profileId"), profileId_);
    root.insert(QStringLiteral("revision"), static_cast<double>(revision_));
    QSaveFile file(QString::fromStdString((directory_ / "sync.json").string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

void SyncController::signIn(
    const QString &email,
    const QString &password,
    const QString &recoveryCode,
    Completion done
) {
    if (busy_) {
        done(L(QStringLiteral("Bitte den laufenden Vorgang abwarten."),
               QStringLiteral("Please wait for the running operation.")));
        return;
    }
    if (email.trimmed().isEmpty() || password.isEmpty()) {
        done(L(QStringLiteral("E-Mail und Passwort eingeben."), QStringLiteral("Enter email and password.")));
        return;
    }
    // A recovery code is either a key or a typo; it is never guessed at.
    std::optional<QByteArray> offered;
    if (!recoveryCode.trimmed().isEmpty()) {
        offered = SyncCipher::decodeKey(recoveryCode);
        if (!offered) {
            done(L(QStringLiteral("Der Wiederherstellungscode ist ungültig."),
                   QStringLiteral("The recovery code is invalid.")));
            return;
        }
    }

    busy_ = true;
    setStatus(L(QStringLiteral("Anmeldung läuft …"), QStringLiteral("Signing in …")));
    auth_->signIn(email.trimmed(), password,
                  [this, offered, done](QString problem, std::optional<SupabaseAuthSession> session) {
        busy_ = false;
        if (!problem.isEmpty()) {
            setStatus(problem);
            done(problem);
            return;
        }
        if (!session) {
            const QString message = L(
                QStringLiteral("Bitte zuerst die Anmeldung per E-Mail bestätigen."),
                QStringLiteral("Please confirm the sign-in by email first.")
            );
            setStatus(message);
            done(message);
            return;
        }
        session_ = session;
        supabase_->setSession(*session);
        if (!secrets_.storeSession(*session)) {
            const QString message = L(
                QStringLiteral("Die Anmeldung konnte nicht im Schlüsselbund gespeichert werden."),
                QStringLiteral("The sign-in could not be stored in the Keychain.")
            );
            setStatus(message);
            done(message);
            return;
        }

        // The key decides what can be read, so it is settled before any sync.
        if (offered) {
            key_ = *offered;
            if (!secrets_.storeKey(key_)) {
                const QString message = L(
                    QStringLiteral("Der Sync-Schlüssel konnte nicht gespeichert werden."),
                    QStringLiteral("The sync key could not be stored.")
                );
                setStatus(message);
                done(message);
                return;
            }
        } else if (!hasKey()) {
            key_ = SyncCipher::makeKey();
            if (!secrets_.storeKey(key_)) {
                const QString message = L(
                    QStringLiteral("Der Sync-Schlüssel konnte nicht gespeichert werden."),
                    QStringLiteral("The sync key could not be stored.")
                );
                setStatus(message);
                done(message);
                return;
            }
        }
        setStatus(L(QStringLiteral("Verbunden"), QStringLiteral("Connected")));
        done({});
    });
}

void SyncController::signUp(const QString &email, const QString &password, Completion done) {
    if (busy_) {
        done(L(QStringLiteral("Bitte den laufenden Vorgang abwarten."),
               QStringLiteral("Please wait for the running operation.")));
        return;
    }
    if (email.trimmed().isEmpty() || password.size() < 8) {
        done(L(QStringLiteral("E-Mail und ein Passwort mit mindestens acht Zeichen eingeben."),
               QStringLiteral("Enter an email and a password of at least eight characters.")));
        return;
    }
    busy_ = true;
    setStatus(L(QStringLiteral("Konto wird angelegt …"), QStringLiteral("Creating the account …")));
    auth_->signUp(email.trimmed(), password,
                  [this, done](QString problem, std::optional<SupabaseAuthSession> session) {
        busy_ = false;
        if (!problem.isEmpty()) {
            setStatus(problem);
            done(problem);
            return;
        }
        if (!session) {
            const QString message = L(
                QStringLiteral("Konto angelegt. Bitte die E-Mail bestätigen und dann anmelden."),
                QStringLiteral("Account created. Please confirm the email and then sign in.")
            );
            setStatus(message);
            done({});
            return;
        }
        session_ = session;
        supabase_->setSession(*session);
        (void)secrets_.storeSession(*session);
        if (!hasKey()) {
            key_ = SyncCipher::makeKey();
            (void)secrets_.storeKey(key_);
        }
        setStatus(L(QStringLiteral("Verbunden"), QStringLiteral("Connected")));
        done({});
    });
}

void SyncController::signOut() {
    if (session_ && auth_) auth_->signOut(session_->accessToken);
    (void)secrets_.removeSession();
    session_.reset();
    // The key stays: it is what makes the data on the server readable again, and
    // the recovery code is the only other copy.
    revision_ = -1;
    persist();
    setStatus(L(QStringLiteral("Abgemeldet"), QStringLiteral("Signed out")));
}

void SyncController::syncNow(Completion done) {
    if (busy_) {
        done(L(QStringLiteral("Bitte den laufenden Vorgang abwarten."),
               QStringLiteral("Please wait for the running operation.")));
        return;
    }
    if (!signedIn()) {
        done(L(QStringLiteral("Nicht angemeldet."), QStringLiteral("Not signed in.")));
        return;
    }
    if (!hasKey()) {
        done(L(QStringLiteral("Es fehlt der Sync-Schlüssel."), QStringLiteral("The sync key is missing.")));
        return;
    }

    busy_ = true;
    setStatus(L(QStringLiteral("Wird abgeglichen …"), QStringLiteral("Synchronising …")));
    service_->fetch(profileId_, [this, done](QString problem, std::optional<SyncRecord> record) {
        if (!problem.isEmpty()) {
            busy_ = false;
            setStatus(problem);
            done(problem);
            return;
        }

        SyncSnapshot local = reader_();
        local.profileId = profileId_;
        local.version = SyncSnapshot::currentVersion;
        local.sanitize();

        if (!record) {
            // Nothing there yet, so the local state becomes the first revision.
            pushMerged(local, -1, done);
            return;
        }

        const std::optional<SyncSnapshot> remote = SyncCipher::open(record->payload, key_);
        if (!remote) {
            busy_ = false;
            const QString message = L(
                QStringLiteral("Die Cloud-Daten passen nicht zu diesem Schlüssel. Wiederherstellungscode prüfen."),
                QStringLiteral("The cloud data does not match this key. Check the recovery code.")
            );
            setStatus(message);
            done(message);
            return;
        }

        // The tab set is a momentary state and follows whichever side is newer.
        // A fresh, empty profile always adopts, so a new device does not wipe
        // the tabs of an old one.
        const SyncTabResolution resolution = local.isInitialEmpty() || remote->modifiedAt > local.modifiedAt
            ? SyncTabResolution::adoptRemote
            : SyncTabResolution::keepLocal;
        const SyncMerge::Result merged = SyncMerge::merge(local, *remote, resolution);
        if (!merged.problem.isEmpty()) {
            busy_ = false;
            setStatus(merged.problem);
            done(merged.problem);
            return;
        }
        if (const QString writeProblem = writer_(merged.snapshot, resolution); !writeProblem.isEmpty()) {
            busy_ = false;
            setStatus(writeProblem);
            done(writeProblem);
            return;
        }
        pushMerged(merged.snapshot, record->revision, done);
    });
}

void SyncController::pushMerged(const SyncSnapshot &snapshot, qint64 expectedRevision, Completion done) {
    SyncSnapshot outgoing = snapshot;
    // The result is the union of both sides, so it is newer than either input.
    outgoing.modifiedAt = QDateTime::currentMSecsSinceEpoch();
    const std::optional<EncryptedSyncPayload> payload = SyncCipher::seal(outgoing, key_);
    if (!payload) {
        busy_ = false;
        const QString message = L(QStringLiteral("Die Daten konnten nicht verschlüsselt werden."),
                                  QStringLiteral("The data could not be encrypted."));
        setStatus(message);
        done(message);
        return;
    }
    service_->push(profileId_, expectedRevision, *payload,
                   [this, done](QString problem, std::optional<SyncRecord> record) {
        busy_ = false;
        if (!problem.isEmpty()) {
            setStatus(problem);
            done(problem);
            return;
        }
        if (record) {
            revision_ = record->revision;
            persist();
        }
        setStatus(L(QStringLiteral("Abgeglichen"), QStringLiteral("Synchronised")));
        done({});
    });
}

} // namespace yobro::spike
