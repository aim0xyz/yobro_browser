import Foundation
import CryptoKit

/// Backend-neutral representation of the browser state that is safe to sync.
/// Website sessions, cookies, passwords, mail accounts and favicon bytes never
/// enter this object.
struct BrowserSyncSnapshot: Codable, Equatable {
    static let currentVersion = 1

    var version = currentVersion
    var profileID: UUID
    var modifiedAt: Date
    var tabs: [StoredTab]
    var activeID: UUID?
    var currentSpace: String
    var closedTabs: [StoredTab]
    var spaces: [String]
    var folders: [TabFolder]
    var splitPairs: [StoredSplit]
    var bookmarks: [BookmarkEntry]
    var history: [HistoryEntry]
    var spaceIcons: [String: String]? = nil

    init(profileID: UUID, modifiedAt: Date = Date(), tabs: [StoredTab], activeID: UUID?, currentSpace: String,
         closedTabs: [StoredTab], spaces: [String], folders: [TabFolder], splitPairs: [StoredSplit],
         bookmarks: [BookmarkEntry], history: [HistoryEntry], spaceIcons: [String: String]? = nil) {
        self.profileID = profileID
        self.modifiedAt = modifiedAt
        self.tabs = tabs.map(Self.withoutLocalArtwork)
        self.activeID = activeID
        self.currentSpace = currentSpace
        self.closedTabs = closedTabs.map(Self.withoutLocalArtwork)
        self.spaces = spaces
        self.folders = folders
        self.splitPairs = splitPairs
        self.bookmarks = bookmarks
        self.history = Self.sanitizedHistory(history)
        self.spaceIcons = spaceIcons
    }

    var isInitialEmpty: Bool {
        tabs.isEmpty && closedTabs.isEmpty && folders.isEmpty && splitPairs.isEmpty && bookmarks.isEmpty && history.isEmpty
    }

    private static func withoutLocalArtwork(_ tab: StoredTab) -> StoredTab {
        var copy = tab
        copy.faviconData = nil
        return copy
    }

    private static func sanitizedHistory(_ entries: [HistoryEntry]) -> [HistoryEntry] {
        entries.compactMap { entry in
            guard let url = URL(string: entry.url), ["https", "http"].contains(url.scheme?.lowercased() ?? ""), url.host != nil else { return nil }
            var copy = entry
            var components = URLComponents(url: url, resolvingAgainstBaseURL: false)
            components?.fragment = nil
            if url.host?.lowercased() == "accounts.google.com" || url.path.localizedCaseInsensitiveContains("signin_prompt") {
                components?.path = "/"
                components?.query = nil
            }
            guard let safeURL = components?.url?.absoluteString else { return nil }
            copy.url = safeURL
            return copy
        }
    }
}

struct EncryptedSyncPayload: Codable, Equatable {
    var algorithm = "ChaChaPoly"
    var formatVersion = BrowserSyncSnapshot.currentVersion
    var ciphertext: Data
}

enum BrowserSyncCipher {
    static let keyByteCount = 32

    static func makeKey() -> Data {
        Data(SymmetricKey(size: .bits256).withUnsafeBytes(Array.init))
    }

    static func seal(_ snapshot: BrowserSyncSnapshot, keyData: Data) throws -> EncryptedSyncPayload {
        guard keyData.count == keyByteCount else { throw YOBROError.message(L("Ungültiger Sync-Schlüssel.", "Invalid sync key.")) }
        let encoded = try JSONEncoder.sync.encode(snapshot)
        let box = try ChaChaPoly.seal(encoded, using: SymmetricKey(data: keyData))
        return EncryptedSyncPayload(ciphertext: box.combined)
    }

    static func open(_ payload: EncryptedSyncPayload, keyData: Data) throws -> BrowserSyncSnapshot {
        guard payload.algorithm == "ChaChaPoly", payload.formatVersion <= BrowserSyncSnapshot.currentVersion,
              keyData.count == keyByteCount else { throw YOBROError.message(L("Nicht unterstützte Sync-Daten.", "Unsupported sync data.")) }
        let box = try ChaChaPoly.SealedBox(combined: payload.ciphertext)
        return try JSONDecoder.sync.decode(BrowserSyncSnapshot.self, from: ChaChaPoly.open(box, using: SymmetricKey(data: keyData)))
    }
}

extension JSONEncoder {
    fileprivate static var sync: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .millisecondsSince1970
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }
}

extension JSONDecoder {
    fileprivate static var sync: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .millisecondsSince1970
        return decoder
    }
}

struct RemoteSyncRecord: Equatable {
    var profileID: UUID
    var revision: Int64
    var modifiedAt: Date
    var payload: EncryptedSyncPayload
}

protocol BrowserSyncService: Sendable {
    func fetch(profileID: UUID) async throws -> RemoteSyncRecord?
    func push(profileID: UUID, expectedRevision: Int64?, payload: EncryptedSyncPayload) async throws -> RemoteSyncRecord
}

/// The browser talks only to this coordinator. Replacing Supabase with another
/// backend later requires a new BrowserSyncService, not a browser rewrite.
actor BrowserSyncCoordinator {
    private let service: any BrowserSyncService
    private let keyData: Data

    init(service: any BrowserSyncService, keyData: Data) {
        self.service = service
        self.keyData = keyData
    }

    func pull(profileID: UUID) async throws -> (BrowserSyncSnapshot, Int64)? {
        guard let remote = try await service.fetch(profileID: profileID) else { return nil }
        return (try BrowserSyncCipher.open(remote.payload, keyData: keyData), remote.revision)
    }

    func push(_ snapshot: BrowserSyncSnapshot, expectedRevision: Int64?) async throws -> RemoteSyncRecord {
        let payload = try BrowserSyncCipher.seal(snapshot, keyData: keyData)
        return try await service.push(profileID: snapshot.profileID, expectedRevision: expectedRevision, payload: payload)
    }
}

extension BrowserModel {
    func syncSnapshot(profileID: UUID = LocalBrowserProfile.originalID, now: Date? = nil) -> BrowserSyncSnapshot {
        let syncTabs = tabs.filter { !$0.isPrivate && !agentTabIDs.contains($0.id) }
        let syncIDs = Set(syncTabs.map(\.id))
        return BrowserSyncSnapshot(
            profileID: profileID,
            modifiedAt: now ?? syncModifiedAt,
            // Interaction state is device-local WebKit data. Uploading it would
            // bloat the payload and push one Mac's scroll offsets to another.
            tabs: syncTabs.map { $0.stored.withoutInteractionState },
            activeID: activeID.flatMap { syncIDs.contains($0) ? $0 : nil },
            currentSpace: space,
            closedTabs: closedTabs.map(\.withoutInteractionState),
            spaces: spaces,
            folders: folders,
            splitPairs: splitPairs.filter { syncIDs.contains($0.first) && syncIDs.contains($0.second) },
            bookmarks: bookmarks,
            history: history,
            spaceIcons: spaceIcons
        )
    }


    /// What should happen to the set of open tabs when a remote snapshot arrives.
    enum SyncTabResolution {
        /// The remote device's tab set replaces the local one.
        case adoptRemote
        /// Keep the local tabs; only the archives are merged.
        case keepLocal
    }

    /// Merges a remote snapshot into the local state.
    ///
    /// This used to assign every collection wholesale, which made synchronizing
    /// destructive: whichever device the timestamp comparison picked erased the
    /// other's entire history, bookmarks, spaces and folders, with no undo. Those
    /// collections only ever grow, so they are unioned instead and no side can
    /// lose an entry. Only the open tabs are a momentary state, and they still
    /// follow whichever side the caller judged newer.
    ///
    /// The merge is idempotent: running it twice on the same data changes
    /// nothing, which is what lets the merged result be pushed straight back.
    func mergeSyncSnapshot(_ snapshot: BrowserSyncSnapshot, tabs resolution: SyncTabResolution) throws {
        guard snapshot.version <= BrowserSyncSnapshot.currentVersion, !snapshot.spaces.isEmpty,
              snapshot.tabs.count <= 5_000, snapshot.history.count <= 100_000, snapshot.bookmarks.count <= 50_000,
              snapshot.spaces.contains(snapshot.currentSpace), snapshot.tabs.allSatisfy({ snapshot.spaces.contains($0.space) }) else {
            throw YOBROError.message(L("Die Cloud-Daten sind ungültig.", "The cloud data is invalid."))
        }

        for name in snapshot.spaces where !spaces.contains(name) { spaces.append(name) }
        // Existing local icons win; the remote only fills gaps.
        var icons = spaceIcons
        for (name, symbol) in snapshot.spaceIcons ?? [:] where icons[name] == nil { icons[name] = symbol }
        spaceIcons = icons.filter { spaces.contains($0.key) && SpaceIcon(rawValue: $0.value) != nil }

        let knownFolders = Set(folders.map(\.id))
        for folder in snapshot.folders where !knownFolders.contains(folder.id) && spaces.contains(folder.space) {
            folders.append(folder)
        }

        var bookmarkKeys = Set(bookmarks.map { "\($0.folder)|\($0.url)" })
        for entry in snapshot.bookmarks where bookmarkKeys.insert("\(entry.folder)|\(entry.url)").inserted {
            bookmarks.append(entry)
        }

        history = Self.mergedHistory(history, snapshot.history)

        let knownClosed = Set(closedTabs.map(\.id))
        for tab in snapshot.closedTabs where !knownClosed.contains(tab.id) { closedTabs.append(tab) }
        if closedTabs.count > 20 { closedTabs = Array(closedTabs.suffix(20)) }

        if resolution == .adoptRemote { adoptRemoteTabs(snapshot) }

        // The local state is now the union of both sides, so it is newer than
        // either input and the caller can push it back.
        syncModifiedAt = Date()
        try JSONEncoder().encode(bookmarks).write(to: home.appendingPathComponent("bookmarks.json"), options: [.atomic, .completeFileProtection])
        try JSONEncoder().encode(history).write(to: home.appendingPathComponent("history.json"), options: [.atomic, .completeFileProtection])
        persistSession(scheduleSync: false, updateTimestamp: false)
        for tab in tabs { tab.restoreFaviconIfNeeded() }
    }

    /// Merges two history lists by sanitized URL.
    ///
    /// Visit counts take the maximum rather than the sum. Summing looks right at
    /// first but is not idempotent: because the merged result is pushed back, the
    /// next sync would add the counts to themselves and inflate them on every
    /// round.
    nonisolated static func mergedHistory(_ local: [HistoryEntry], _ remote: [HistoryEntry]) -> [HistoryEntry] {
        var byURL: [String: HistoryEntry] = [:]
        for entry in local + remote {
            guard let url = URL(string: entry.url), let safeURL = sanitizedHistoryURL(url) else { continue }
            var candidate = entry
            candidate.url = safeURL
            guard let existing = byURL[safeURL] else { byURL[safeURL] = candidate; continue }
            var merged = existing
            merged.visits = max(existing.visits, candidate.visits)
            if candidate.date > existing.date {
                merged.date = candidate.date
                merged.title = candidate.title
            }
            byURL[safeURL] = merged
        }
        return byURL.values.sorted { $0.date > $1.date }
    }

    private func adoptRemoteTabs(_ snapshot: BrowserSyncSnapshot) {
        // Private and agent tabs are never synced, so they must survive.
        let preserved = tabs.filter { $0.isPrivate || agentTabIDs.contains($0.id) }
        let preservedIDs = Set(preserved.map(\.id))
        let preservedActiveID = activeID.flatMap { preservedIDs.contains($0) ? $0 : nil }
        // `stopLoading()` alone left the replaced WebViews playing media and kept
        // their observers alive; `discard()` is what closing a tab uses.
        for tab in tabs where !preservedIDs.contains(tab.id) { tab.discard() }
        tabs = snapshot.tabs.map { BrowserTab(saved: $0, owner: self, deferLoading: true) } + preserved
        splitPairs = snapshot.splitPairs
        space = spaces.contains(snapshot.currentSpace) ? snapshot.currentSpace : space
        if isColdStartRestore && activeID == nil {
            activeID = nil
        } else {
            activeID = preservedActiveID
                ?? (tabs.contains(where: { $0.id == snapshot.activeID }) ? snapshot.activeID : tabs.first(where: { $0.space == space })?.id)
        }
        splitID = activeID.flatMap { id in splitPairs.first(where: { $0.contains(id) })?.other(id) }
    }
}
