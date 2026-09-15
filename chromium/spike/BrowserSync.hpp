#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace yobro::spike {

/// One synced tab or note. Favicon bytes are never part of this: they are local
/// artwork that would bloat every payload for no gain.
struct SyncTab {
    QString id;
    QString url;
    QString title;
    QString space;
    QString folder;
    bool pinned = false;
    int order = 0;
    /// True for a note tab; `url` is then empty and `noteHtml` carries the text.
    bool note = false;
    QString noteHtml;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncTab fromJson(const QJsonObject &object);
};

struct SyncFolder {
    QString id;
    QString name;
    QString space;
    QString color;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncFolder fromJson(const QJsonObject &object);
};

struct SyncBookmark {
    QString title;
    QString url;
    QString folder;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncBookmark fromJson(const QJsonObject &object);
};

struct SyncHistoryEntry {
    QString url;
    QString title;
    /// Milliseconds since the epoch, as in the WebKit build's sync encoder.
    qint64 date = 0;
    qint64 visits = 1;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncHistoryEntry fromJson(const QJsonObject &object);
};

/// The part of the browser state that is safe to sync.
///
/// Website sessions, cookies, passwords, mail accounts and favicon bytes never
/// enter this object, exactly as in `BrowserSyncSnapshot`.
struct SyncSnapshot {
    /// This shell's schema. The WebKit build declares 1 and refuses anything
    /// higher, so it will decline these payloads cleanly instead of misreading
    /// a tab list whose shape it does not know.
    static constexpr int currentVersion = 2;
    /// The limits a snapshot has to stay within to be accepted, matching the
    /// WebKit build's numbers.
    static constexpr int maximumTabs = 5000;
    static constexpr int maximumHistory = 100000;
    static constexpr int maximumBookmarks = 50000;
    static constexpr int maximumClosedTabs = 20;

    int version = currentVersion;
    QString profileId;
    /// Milliseconds since the epoch.
    qint64 modifiedAt = 0;
    std::vector<SyncTab> tabs;
    QString activeId;
    QString currentSpace;
    std::vector<SyncTab> closedTabs;
    QStringList spaces;
    std::vector<SyncFolder> folders;
    std::vector<SyncBookmark> bookmarks;
    std::vector<SyncHistoryEntry> history;
    QMap<QString, QString> spaceIcons;

    /// Nothing worth keeping, so a first sync may take the other side wholesale.
    [[nodiscard]] bool isInitialEmpty() const;
    /// Empty when the snapshot may be used, otherwise why it may not.
    [[nodiscard]] QString validationProblem() const;

    /// Drops what must not travel and normalises history URLs. Always run before
    /// a snapshot is sealed.
    void sanitize();

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncSnapshot fromJson(const QJsonObject &object);

    /// The canonical form of a history URL, or nothing when it must not travel.
    ///
    /// The fragment is always dropped, and a Google sign-in address is reduced
    /// to its host: those URLs carry one-time tokens in their path and query.
    [[nodiscard]] static std::optional<QString> sanitizedHistoryUrl(const QString &raw);
};

/// The sealed envelope stored in the backend.
struct EncryptedSyncPayload {
    QString algorithm = QStringLiteral("ChaChaPoly");
    int formatVersion = SyncSnapshot::currentVersion;
    /// Nonce, ciphertext and tag, in that order.
    QByteArray ciphertext;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static EncryptedSyncPayload fromJson(const QJsonObject &object);
};

/// Sealing and opening sync payloads.
class SyncCipher {
public:
    static constexpr int keyBytes = 32;

    [[nodiscard]] static QByteArray makeKey();
    /// A key as the user sees it, for writing down and typing on another device.
    [[nodiscard]] static QString encodeKey(const QByteArray &key);
    /// Reads a recovery code back. Returns nothing when it is not a key.
    [[nodiscard]] static std::optional<QByteArray> decodeKey(const QString &code);

    [[nodiscard]] static std::optional<EncryptedSyncPayload> seal(
        const SyncSnapshot &snapshot,
        const QByteArray &key
    );
    /// Returns nothing when the payload is not ours, the key is wrong or the
    /// contents were tampered with.
    [[nodiscard]] static std::optional<SyncSnapshot> open(
        const EncryptedSyncPayload &payload,
        const QByteArray &key
    );
};

/// What should happen to the set of open tabs when a remote snapshot arrives.
enum class SyncTabResolution {
    /// The other device's tab set replaces the local one.
    adoptRemote,
    /// Keep the local tabs; only the archives are merged.
    keepLocal,
};

/// Merging two snapshots.
///
/// Assigning collections wholesale would make syncing destructive: whichever
/// side a timestamp comparison picked would erase the other's history,
/// bookmarks, spaces and folders with no undo. Those only ever grow, so they are
/// unioned and no side can lose an entry. Only the open tabs are a momentary
/// state and follow the caller's decision.
///
/// The merge is idempotent, which is what allows the result to be pushed back.
class SyncMerge {
public:
    struct Result {
        SyncSnapshot snapshot;
        /// Empty when the merge went through.
        QString problem;
    };

    [[nodiscard]] static Result merge(
        const SyncSnapshot &local,
        const SyncSnapshot &remote,
        SyncTabResolution resolution
    );

    /// Merges two history lists by sanitized URL.
    ///
    /// Visit counts take the maximum rather than the sum. Summing looks right
    /// but is not idempotent: because the merged result is pushed back, the next
    /// round would add the counts to themselves.
    [[nodiscard]] static std::vector<SyncHistoryEntry> mergedHistory(
        const std::vector<SyncHistoryEntry> &local,
        const std::vector<SyncHistoryEntry> &remote
    );
};

} // namespace yobro::spike
