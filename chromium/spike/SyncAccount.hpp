#pragma once

#include "spike/BrowserSync.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace yobro::spike {

/// Where the account and sync endpoints live.
///
/// Resolution order, as in the WebKit build: environment variables, then
/// `supabase.json` next to the profiles, then the built-in project. An override
/// must be HTTPS with a host, because an access token is attached to every
/// request made against it.
struct SupabaseProject {
    QUrl url;
    QString publishableKey;

    [[nodiscard]] static SupabaseProject resolve(const std::filesystem::path &root);
    /// True when the URL may carry an access token.
    [[nodiscard]] static bool isUsableUrl(const QUrl &candidate);
};

/// A signed-in session. The tokens live in the Keychain, never in a JSON file.
struct SupabaseAuthSession {
    QString accessToken;
    QString refreshToken;
    QString userId;
    QString email;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SupabaseAuthSession fromJson(const QJsonObject &object);
};

/// The account tokens and the sync key in the macOS Keychain.
///
/// A separate service name from the WebKit build's `xyz.aimo.yobro.sync`: the
/// two shells are separate installations, and neither should be able to read the
/// other's tokens. The sync key travels between devices as a recovery code, which
/// is what makes a shared Keychain entry unnecessary.
class SyncSecrets {
public:
    explicit SyncSecrets(std::filesystem::path profileDirectory);

    [[nodiscard]] bool available() const;
    bool storeSession(const SupabaseAuthSession &session);
    [[nodiscard]] std::optional<SupabaseAuthSession> readSession() const;
    bool removeSession();

    bool storeKey(const QByteArray &key);
    [[nodiscard]] QByteArray readKey() const;
    bool removeKey();

    [[nodiscard]] const std::string &service() const { return service_; }

private:
    bool store(const QString &account, const QByteArray &value);
    [[nodiscard]] QByteArray read(const QString &account) const;
    bool remove(const QString &account);

    std::string service_;
};

/// One stored row of the backend.
struct SyncRecord {
    QString profileId;
    qint64 revision = 0;
    qint64 modifiedAt = 0;
    EncryptedSyncPayload payload;
};

/// The browser talks only to this. Replacing the backend later means a new
/// implementation, not a browser rewrite.
class SyncService {
public:
    virtual ~SyncService() = default;

    /// Answers with the stored record, or with nothing when there is none.
    virtual void fetch(
        const QString &profileId,
        std::function<void(QString problem, std::optional<SyncRecord>)> done
    ) = 0;
    /// Stores a payload. `expectedRevision` below zero means "there is no row
    /// yet", which lets the backend reject a blind overwrite.
    virtual void push(
        const QString &profileId,
        qint64 expectedRevision,
        const EncryptedSyncPayload &payload,
        std::function<void(QString problem, std::optional<SyncRecord>)> done
    ) = 0;
};

/// Signing in and out. Kept separate from the sync engine so account handling and
/// token storage stay independent, as in the WebKit build.
class AuthClient {
public:
    virtual ~AuthClient() = default;

    using Answer = std::function<void(QString problem, std::optional<SupabaseAuthSession>)>;
    virtual void signIn(const QString &email, const QString &password, Answer done) = 0;
    /// A project with e-mail confirmation answers without a session; that is not
    /// a failure, and the caller has to say so rather than look signed in.
    virtual void signUp(const QString &email, const QString &password, Answer done) = 0;
    virtual void signOut(const QString &accessToken) = 0;
};

/// The Supabase REST adapter for both halves.
class SupabaseClient : public SyncService, public AuthClient {
public:
    explicit SupabaseClient(SupabaseProject project, QObject *context);

    void fetch(
        const QString &profileId,
        std::function<void(QString problem, std::optional<SyncRecord>)> done
    ) override;
    void push(
        const QString &profileId,
        qint64 expectedRevision,
        const EncryptedSyncPayload &payload,
        std::function<void(QString problem, std::optional<SyncRecord>)> done
    ) override;

    void signIn(const QString &email, const QString &password, Answer done) override;
    void signUp(const QString &email, const QString &password, Answer done) override;
    void signOut(const QString &accessToken) override;

    /// The session the REST calls authenticate with.
    void setSession(const SupabaseAuthSession &session);

    /// The URL for one REST path, or an empty URL when it would leave the
    /// configured project's own host.
    [[nodiscard]] QUrl endpoint(const QString &path) const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

/// Account state, the sync key and the pull-merge-push cycle.
class SyncController : public QObject {
    Q_OBJECT

public:
    /// Reads the browser's current state.
    using SnapshotReader = std::function<SyncSnapshot()>;
    /// Writes a merged snapshot back into the browser. Returns a problem, or an
    /// empty string.
    using SnapshotWriter = std::function<QString(const SyncSnapshot &, SyncTabResolution)>;
    using Completion = std::function<void(QString problem)>;

    SyncController(
        std::filesystem::path profileDirectory,
        SnapshotReader reader,
        SnapshotWriter writer,
        QObject *parent = nullptr
    );

    /// Replaces the backend and the account client, for tests.
    void setService(SyncService *service);
    void setAuthClient(AuthClient *client);

    [[nodiscard]] bool signedIn() const { return session_.has_value(); }
    [[nodiscard]] QString email() const;
    [[nodiscard]] const QString &status() const { return status_; }
    [[nodiscard]] bool busy() const { return busy_; }
    /// The profile's own id in the backend, created once and then kept.
    [[nodiscard]] const QString &profileId() const { return profileId_; }
    /// The recovery code for the current key, or empty when there is none.
    [[nodiscard]] QString recoveryCode() const;
    /// True when a key exists, which is what sealing needs.
    [[nodiscard]] bool hasKey() const { return key_.size() == SyncCipher::keyBytes; }

    /// Signs in and adopts, or creates, the sync key.
    ///
    /// An empty recovery code means "make a new key" on a fresh account and
    /// "use the one already stored" otherwise. A given code is only adopted when
    /// it actually opens what the backend holds, so a typo cannot orphan the
    /// data that is already there.
    void signIn(const QString &email, const QString &password, const QString &recoveryCode, Completion done);
    void signUp(const QString &email, const QString &password, Completion done);
    void signOut();

    /// Pull, merge, push. The tab set follows whichever side is newer, and only
    /// when the local side is not a fresh, empty profile.
    void syncNow(Completion done);

Q_SIGNALS:
    void changed();

private:
    void setStatus(const QString &text);
    void persist();
    void pushMerged(const SyncSnapshot &snapshot, qint64 expectedRevision, Completion done);

    std::filesystem::path directory_;
    SnapshotReader reader_;
    SnapshotWriter writer_;
    SyncSecrets secrets_;
    SyncService *service_ = nullptr;
    AuthClient *auth_ = nullptr;
    std::unique_ptr<SupabaseClient> supabase_;
    std::optional<SupabaseAuthSession> session_;
    QByteArray key_;
    QString profileId_;
    QString status_;
    /// The revision last seen, so a push can be refused when another device got
    /// there first.
    qint64 revision_ = -1;
    bool busy_ = false;
};

} // namespace yobro::spike
