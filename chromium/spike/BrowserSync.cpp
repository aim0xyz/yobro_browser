#include "spike/BrowserSync.hpp"

#include "spike/ChaCha20Poly1305.hpp"
#include "spike/Localization.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace yobro::spike {
namespace {

const QString kId = QStringLiteral("id");
const QString kUrl = QStringLiteral("url");
const QString kTitle = QStringLiteral("title");
const QString kSpace = QStringLiteral("space");
const QString kFolder = QStringLiteral("folder");
const QString kPinned = QStringLiteral("pinned");
const QString kOrder = QStringLiteral("order");
const QString kNote = QStringLiteral("note");
const QString kNoteHtml = QStringLiteral("noteHtml");
const QString kName = QStringLiteral("name");
const QString kColor = QStringLiteral("color");
const QString kDate = QStringLiteral("date");
const QString kVisits = QStringLiteral("visits");

template <typename Item>
QJsonArray toArray(const std::vector<Item> &items) {
    QJsonArray array;
    for (const Item &item : items) array.append(item.toJson());
    return array;
}

template <typename Item>
std::vector<Item> fromArray(const QJsonValue &value) {
    std::vector<Item> items;
    for (const QJsonValue &entry : value.toArray()) items.push_back(Item::fromJson(entry.toObject()));
    return items;
}

} // namespace

// MARK: - Value types

QJsonObject SyncTab::toJson() const {
    QJsonObject object;
    object.insert(kId, id);
    object.insert(kUrl, url);
    object.insert(kTitle, title);
    object.insert(kSpace, space);
    object.insert(kFolder, folder);
    object.insert(kPinned, pinned);
    object.insert(kOrder, order);
    object.insert(kNote, note);
    if (note) object.insert(kNoteHtml, noteHtml);
    return object;
}

SyncTab SyncTab::fromJson(const QJsonObject &object) {
    SyncTab tab;
    tab.id = object.value(kId).toString();
    tab.url = object.value(kUrl).toString();
    tab.title = object.value(kTitle).toString();
    tab.space = object.value(kSpace).toString();
    tab.folder = object.value(kFolder).toString();
    tab.pinned = object.value(kPinned).toBool(false);
    tab.order = object.value(kOrder).toInt(0);
    tab.note = object.value(kNote).toBool(false);
    tab.noteHtml = object.value(kNoteHtml).toString();
    return tab;
}

QJsonObject SyncFolder::toJson() const {
    QJsonObject object;
    object.insert(kId, id);
    object.insert(kName, name);
    object.insert(kSpace, space);
    object.insert(kColor, color);
    return object;
}

SyncFolder SyncFolder::fromJson(const QJsonObject &object) {
    SyncFolder folder;
    folder.id = object.value(kId).toString();
    folder.name = object.value(kName).toString();
    folder.space = object.value(kSpace).toString();
    folder.color = object.value(kColor).toString();
    return folder;
}

QJsonObject SyncBookmark::toJson() const {
    QJsonObject object;
    object.insert(kTitle, title);
    object.insert(kUrl, url);
    object.insert(kFolder, folder);
    return object;
}

SyncBookmark SyncBookmark::fromJson(const QJsonObject &object) {
    SyncBookmark bookmark;
    bookmark.title = object.value(kTitle).toString();
    bookmark.url = object.value(kUrl).toString();
    bookmark.folder = object.value(kFolder).toString();
    return bookmark;
}

QJsonObject SyncHistoryEntry::toJson() const {
    QJsonObject object;
    object.insert(kUrl, url);
    object.insert(kTitle, title);
    object.insert(kDate, static_cast<double>(date));
    object.insert(kVisits, static_cast<double>(visits));
    return object;
}

SyncHistoryEntry SyncHistoryEntry::fromJson(const QJsonObject &object) {
    SyncHistoryEntry entry;
    entry.url = object.value(kUrl).toString();
    entry.title = object.value(kTitle).toString();
    entry.date = static_cast<qint64>(object.value(kDate).toDouble());
    entry.visits = static_cast<qint64>(object.value(kVisits).toDouble(1));
    return entry;
}

// MARK: - Snapshot

bool SyncSnapshot::isInitialEmpty() const {
    return tabs.empty() && closedTabs.empty() && folders.empty() && bookmarks.empty()
        && history.empty();
}

std::optional<QString> SyncSnapshot::sanitizedHistoryUrl(const QString &raw) {
    const QUrl parsed(raw);
    if (!parsed.isValid() || parsed.host().isEmpty()) return std::nullopt;
    const QString scheme = parsed.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) return std::nullopt;

    QUrl clean = parsed;
    // A fragment is never worth syncing and can carry tokens of its own.
    clean.setFragment(QString());
    const bool googleSignIn = parsed.host().toLower() == QStringLiteral("accounts.google.com")
        || parsed.path().contains(QStringLiteral("signin_prompt"), Qt::CaseInsensitive);
    if (googleSignIn) {
        // These addresses carry one-time sign-in tokens in the path and query.
        clean.setPath(QStringLiteral("/"));
        clean.setQuery(QString());
    }
    const QString result = clean.toString();
    if (result.isEmpty()) return std::nullopt;
    return result;
}

void SyncSnapshot::sanitize() {
    std::vector<SyncHistoryEntry> cleaned;
    cleaned.reserve(history.size());
    for (const SyncHistoryEntry &entry : history) {
        const std::optional<QString> url = sanitizedHistoryUrl(entry.url);
        if (!url) continue;
        SyncHistoryEntry copy = entry;
        copy.url = *url;
        cleaned.push_back(std::move(copy));
    }
    history = std::move(cleaned);

    // Icons of spaces that no longer exist are dropped rather than carried.
    QMap<QString, QString> icons;
    for (auto entry = spaceIcons.begin(); entry != spaceIcons.end(); ++entry)
        if (spaces.contains(entry.key())) icons.insert(entry.key(), entry.value());
    spaceIcons = icons;
}

QString SyncSnapshot::validationProblem() const {
    const QString invalid = L(QStringLiteral("Die Cloud-Daten sind ungültig."),
                              QStringLiteral("The cloud data is invalid."));
    if (version > currentVersion) return invalid;
    if (spaces.isEmpty()) return invalid;
    if (!spaces.contains(currentSpace)) return invalid;
    if (tabs.size() > static_cast<std::size_t>(maximumTabs)) return invalid;
    if (history.size() > static_cast<std::size_t>(maximumHistory)) return invalid;
    if (bookmarks.size() > static_cast<std::size_t>(maximumBookmarks)) return invalid;
    for (const SyncTab &tab : tabs)
        if (!spaces.contains(tab.space)) return invalid;
    return {};
}

QJsonObject SyncSnapshot::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("version"), version);
    object.insert(QStringLiteral("profileId"), profileId);
    object.insert(QStringLiteral("modifiedAt"), static_cast<double>(modifiedAt));
    object.insert(QStringLiteral("tabs"), toArray(tabs));
    object.insert(QStringLiteral("activeId"), activeId);
    object.insert(QStringLiteral("currentSpace"), currentSpace);
    object.insert(QStringLiteral("closedTabs"), toArray(closedTabs));
    object.insert(QStringLiteral("spaces"), QJsonArray::fromStringList(spaces));
    object.insert(QStringLiteral("folders"), toArray(folders));
    object.insert(QStringLiteral("bookmarks"), toArray(bookmarks));
    object.insert(QStringLiteral("history"), toArray(history));
    QJsonObject icons;
    for (auto entry = spaceIcons.begin(); entry != spaceIcons.end(); ++entry)
        icons.insert(entry.key(), entry.value());
    object.insert(QStringLiteral("spaceIcons"), icons);
    return object;
}

SyncSnapshot SyncSnapshot::fromJson(const QJsonObject &object) {
    SyncSnapshot snapshot;
    snapshot.version = object.value(QStringLiteral("version")).toInt(currentVersion);
    snapshot.profileId = object.value(QStringLiteral("profileId")).toString();
    snapshot.modifiedAt = static_cast<qint64>(object.value(QStringLiteral("modifiedAt")).toDouble());
    snapshot.tabs = fromArray<SyncTab>(object.value(QStringLiteral("tabs")));
    snapshot.activeId = object.value(QStringLiteral("activeId")).toString();
    snapshot.currentSpace = object.value(QStringLiteral("currentSpace")).toString();
    snapshot.closedTabs = fromArray<SyncTab>(object.value(QStringLiteral("closedTabs")));
    for (const QJsonValue &value : object.value(QStringLiteral("spaces")).toArray())
        snapshot.spaces.append(value.toString());
    snapshot.folders = fromArray<SyncFolder>(object.value(QStringLiteral("folders")));
    snapshot.bookmarks = fromArray<SyncBookmark>(object.value(QStringLiteral("bookmarks")));
    snapshot.history = fromArray<SyncHistoryEntry>(object.value(QStringLiteral("history")));
    const QJsonObject icons = object.value(QStringLiteral("spaceIcons")).toObject();
    for (auto entry = icons.begin(); entry != icons.end(); ++entry)
        snapshot.spaceIcons.insert(entry.key(), entry.value().toString());
    return snapshot;
}

// MARK: - Envelope

QJsonObject EncryptedSyncPayload::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("algorithm"), algorithm);
    object.insert(QStringLiteral("formatVersion"), formatVersion);
    object.insert(QStringLiteral("ciphertext"), QString::fromUtf8(ciphertext.toBase64()));
    return object;
}

EncryptedSyncPayload EncryptedSyncPayload::fromJson(const QJsonObject &object) {
    EncryptedSyncPayload payload;
    payload.algorithm = object.value(QStringLiteral("algorithm")).toString();
    payload.formatVersion = object.value(QStringLiteral("formatVersion")).toInt(0);
    payload.ciphertext = QByteArray::fromBase64(
        object.value(QStringLiteral("ciphertext")).toString().toUtf8(),
        QByteArray::AbortOnBase64DecodingErrors
    );
    return payload;
}

// MARK: - Cipher

QByteArray SyncCipher::makeKey() {
    return ChaCha20Poly1305::randomKey();
}

QString SyncCipher::encodeKey(const QByteArray &key) {
    if (key.size() != keyBytes) return {};
    // Grouped in blocks of eight so it can be read out loud and typed again.
    const QString hex = QString::fromUtf8(key.toHex()).toUpper();
    QStringList groups;
    for (int offset = 0; offset < hex.size(); offset += 8) groups.append(hex.mid(offset, 8));
    return groups.join(QLatin1Char('-'));
}

std::optional<QByteArray> SyncCipher::decodeKey(const QString &code) {
    QString compact = code.trimmed();
    compact.remove(QLatin1Char('-'));
    compact.remove(QLatin1Char(' '));
    if (compact.size() != keyBytes * 2) return std::nullopt;
    // A stray character would silently shorten the key, so it is checked here.
    for (const QChar character : compact)
        if (!std::isxdigit(character.toLatin1())) return std::nullopt;
    const QByteArray key = QByteArray::fromHex(compact.toUtf8());
    if (key.size() != keyBytes) return std::nullopt;
    return key;
}

std::optional<EncryptedSyncPayload> SyncCipher::seal(const SyncSnapshot &snapshot, const QByteArray &key) {
    if (key.size() != keyBytes) return std::nullopt;
    const QByteArray encoded = QJsonDocument(snapshot.toJson()).toJson(QJsonDocument::Compact);
    const QByteArray sealed = ChaCha20Poly1305::seal(key, ChaCha20Poly1305::randomNonce(), encoded);
    if (sealed.isEmpty()) return std::nullopt;
    EncryptedSyncPayload payload;
    payload.ciphertext = sealed;
    return payload;
}

std::optional<SyncSnapshot> SyncCipher::open(const EncryptedSyncPayload &payload, const QByteArray &key) {
    if (payload.algorithm != QStringLiteral("ChaChaPoly")) return std::nullopt;
    // A payload from a newer schema is refused rather than half-understood.
    if (payload.formatVersion <= 0 || payload.formatVersion > SyncSnapshot::currentVersion)
        return std::nullopt;
    if (key.size() != keyBytes) return std::nullopt;
    const std::optional<QByteArray> plain = ChaCha20Poly1305::open(key, payload.ciphertext);
    if (!plain) return std::nullopt;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(*plain, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return SyncSnapshot::fromJson(document.object());
}

// MARK: - Merge

std::vector<SyncHistoryEntry> SyncMerge::mergedHistory(
    const std::vector<SyncHistoryEntry> &local,
    const std::vector<SyncHistoryEntry> &remote
) {
    std::vector<SyncHistoryEntry> merged;
    QMap<QString, std::size_t> byUrl;
    const auto absorb = [&merged, &byUrl](const SyncHistoryEntry &entry) {
        const std::optional<QString> url = SyncSnapshot::sanitizedHistoryUrl(entry.url);
        if (!url) return;
        SyncHistoryEntry candidate = entry;
        candidate.url = *url;
        const auto found = byUrl.constFind(*url);
        if (found == byUrl.constEnd()) {
            byUrl.insert(*url, merged.size());
            merged.push_back(std::move(candidate));
            return;
        }
        SyncHistoryEntry &existing = merged.at(*found);
        existing.visits = std::max(existing.visits, candidate.visits);
        if (candidate.date > existing.date) {
            existing.date = candidate.date;
            existing.title = candidate.title;
        }
    };
    for (const SyncHistoryEntry &entry : local) absorb(entry);
    for (const SyncHistoryEntry &entry : remote) absorb(entry);
    std::sort(merged.begin(), merged.end(), [](const SyncHistoryEntry &left, const SyncHistoryEntry &right) {
        return left.date > right.date;
    });
    return merged;
}

SyncMerge::Result SyncMerge::merge(
    const SyncSnapshot &local,
    const SyncSnapshot &remote,
    SyncTabResolution resolution
) {
    Result result;
    if (const QString problem = remote.validationProblem(); !problem.isEmpty()) {
        result.problem = problem;
        return result;
    }

    SyncSnapshot merged = local;
    for (const QString &name : remote.spaces)
        if (!merged.spaces.contains(name)) merged.spaces.append(name);

    // Local icons win; the remote only fills gaps.
    for (auto entry = remote.spaceIcons.begin(); entry != remote.spaceIcons.end(); ++entry)
        if (!merged.spaceIcons.contains(entry.key())) merged.spaceIcons.insert(entry.key(), entry.value());

    QSet<QString> knownFolders;
    for (const SyncFolder &folder : merged.folders) knownFolders.insert(folder.id);
    for (const SyncFolder &folder : remote.folders) {
        if (knownFolders.contains(folder.id) || !merged.spaces.contains(folder.space)) continue;
        knownFolders.insert(folder.id);
        merged.folders.push_back(folder);
    }

    QSet<QString> bookmarkKeys;
    for (const SyncBookmark &bookmark : merged.bookmarks)
        bookmarkKeys.insert(bookmark.folder + QLatin1Char('|') + bookmark.url);
    for (const SyncBookmark &bookmark : remote.bookmarks) {
        const QString key = bookmark.folder + QLatin1Char('|') + bookmark.url;
        if (bookmarkKeys.contains(key)) continue;
        bookmarkKeys.insert(key);
        merged.bookmarks.push_back(bookmark);
    }

    merged.history = mergedHistory(local.history, remote.history);

    QSet<QString> knownClosed;
    for (const SyncTab &tab : merged.closedTabs) knownClosed.insert(tab.id);
    for (const SyncTab &tab : remote.closedTabs) {
        if (knownClosed.contains(tab.id)) continue;
        knownClosed.insert(tab.id);
        merged.closedTabs.push_back(tab);
    }
    if (merged.closedTabs.size() > static_cast<std::size_t>(SyncSnapshot::maximumClosedTabs)) {
        merged.closedTabs.erase(
            merged.closedTabs.begin(),
            merged.closedTabs.end() - SyncSnapshot::maximumClosedTabs
        );
    }

    if (resolution == SyncTabResolution::adoptRemote) {
        merged.tabs = remote.tabs;
        if (merged.spaces.contains(remote.currentSpace)) merged.currentSpace = remote.currentSpace;
        // The active tab has to be one that actually exists afterwards.
        merged.activeId.clear();
        for (const SyncTab &tab : merged.tabs)
            if (tab.id == remote.activeId) merged.activeId = remote.activeId;
    }

    merged.version = SyncSnapshot::currentVersion;
    merged.sanitize();
    if (const QString problem = merged.validationProblem(); !problem.isEmpty()) {
        result.problem = problem;
        return result;
    }
    result.snapshot = std::move(merged);
    return result;
}

} // namespace yobro::spike
