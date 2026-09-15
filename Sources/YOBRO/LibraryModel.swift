import SwiftUI
import WebKit
import AppKit

enum LibrarySection: String, Identifiable, CaseIterable {
    case history = "Verlauf", bookmarks = "Lesezeichen", downloads = "Downloads"

    var symbol: String {
        switch self {
        case .history: return "clock.arrow.circlepath"
        case .bookmarks: return "bookmark"
        case .downloads: return "arrow.down.circle"
        }
    }
    var id: String { rawValue }
}

struct HistoryEntry: Codable, Identifiable, Equatable, Sendable {
    var id = UUID()
    var title: String
    var url: String
    var date = Date()
    var visits = 1
}

struct DownloadEntry: Codable, Identifiable {
    var id = UUID()
    var name = L("Download wird vorbereitet …")
    var source: String
    var date = Date()
    var state = "downloading"
    var received: Int64 = 0
    var expected: Int64 = 0
    var path: String?
    var error: String?
    var fraction: Double { expected > 0 ? min(1, max(0, Double(received) / Double(expected))) : 0 }
    var stateLabel: String {
        switch state {
        case "completed": return L("Abgeschlossen")
        case "cancelled": return L("Abgebrochen")
        case "failed": return L("Fehlgeschlagen")
        case "interrupted": return L("Durch Neustart unterbrochen")
        default: return L("Wird geladen")
        }
    }
}

@MainActor
final class DownloadStore: NSObject, ObservableObject, WKDownloadDelegate {
    @Published var entries: [DownloadEntry] = []
    @Published private(set) var directory: URL
    private let storage: URL
    private let preferencesURL: URL
    private let defaultDirectory: URL
    private var tasks: [UUID: WKDownload] = [:]
    private var observations: [UUID: NSKeyValueObservation] = [:]

    init(home: URL, isolated: Bool) {
        storage = home.appendingPathComponent("downloads.json")
        preferencesURL = home.appendingPathComponent("download-preferences.json")
        defaultDirectory = isolated ? home.appendingPathComponent("Downloads")
            : FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask)[0].appendingPathComponent("YOBRO")
        if let data = try? Data(contentsOf: preferencesURL),
           let preferences = try? JSONDecoder().decode(DownloadPreferences.self, from: data),
           !preferences.directory.isEmpty {
            directory = URL(fileURLWithPath: preferences.directory, isDirectory: true).standardizedFileURL
        } else {
            directory = defaultDirectory
        }
        super.init()
        if let data = try? Data(contentsOf: storage), let saved = try? JSONDecoder().decode([DownloadEntry].self, from: data) {
            entries = saved.map { item in
                var item = item
                if item.state == "downloading" { item.state = "interrupted"; try? FileManager.default.removeItem(at: partial(item.id)) }
                return item
            }
        }
    }
    var activeCount: Int { entries.filter { $0.state == "downloading" }.count }
    var activeEntries: [DownloadEntry] { entries.filter { $0.state == "downloading" } }
    var usesDefaultDirectory: Bool { directory.standardizedFileURL.path == defaultDirectory.standardizedFileURL.path }

    @discardableResult
    func setDirectory(_ url: URL) throws -> Bool {
        guard activeCount == 0 else { return false }
        let selected = url.standardizedFileURL
        var isDirectory: ObjCBool = false
        if FileManager.default.fileExists(atPath: selected.path, isDirectory: &isDirectory) {
            guard isDirectory.boolValue else { throw YOBROError.message(L("Bitte wähle einen Ordner aus.", "Please choose a folder.")) }
        } else {
            try FileManager.default.createDirectory(at: selected, withIntermediateDirectories: true)
        }
        try persistPreferences(selected)
        directory = selected
        return true
    }

    @discardableResult
    func resetDirectory() throws -> Bool { try setDirectory(defaultDirectory) }

    private func persistPreferences(_ selected: URL) throws {
        try JSONEncoder().encode(DownloadPreferences(directory: selected.path)).write(to: preferencesURL, options: .atomic)
    }
    private func partial(_ id: UUID) -> URL { directory.appendingPathComponent(".\(id.uuidString).yobro-part") }
    private func identifier(_ download: WKDownload) -> UUID? { tasks.first { $0.value === download }?.key }
    private func update(_ id: UUID, _ body: (inout DownloadEntry) -> Void) {
        guard let index = entries.firstIndex(where: { $0.id == id }) else { return }
        body(&entries[index])
    }
    private func persist() {
        do { try JSONEncoder().encode(entries).write(to: storage, options: .atomic) }
        catch { NSLog("YoBro download history could not be saved: %@", error.localizedDescription) }
    }
    func track(_ download: WKDownload) {
        guard identifier(download) == nil else { return }
        let entry = DownloadEntry(source: download.originalRequest?.url?.absoluteString ?? "")
        entries.insert(entry, at: 0); tasks[entry.id] = download
        download.delegate = self
        observations[entry.id] = download.progress.observe(\.completedUnitCount, options: [.new]) { [weak self] progress, _ in
            let received = progress.completedUnitCount, expected = progress.totalUnitCount
            Task { @MainActor in self?.update(entry.id) { $0.received = received; $0.expected = expected } }
        }
        persist()
    }
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String, completionHandler: @escaping (URL?) -> Void) {
        guard let id = identifier(download), entries.first(where: { $0.id == id })?.state == "downloading" else { completionHandler(nil); return }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            var name = URL(fileURLWithPath: suggestedFilename).lastPathComponent
            name = String(name.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) }).trimmingCharacters(in: .whitespacesAndNewlines)
            if name.isEmpty || name == "." || name == ".." { name = "Download" }
            if name.hasPrefix(".") { name = "Download" + name }
            name = String(name.prefix(180))
            let candidate = URL(fileURLWithPath: name)
            let stem = candidate.deletingPathExtension().lastPathComponent
            let ext = candidate.pathExtension
            var destination = directory.appendingPathComponent(name), number = 1
            while FileManager.default.fileExists(atPath: destination.path) || entries.contains(where: { $0.path == destination.path }) {
                destination = directory.appendingPathComponent("\(stem) (\(number))\(ext.isEmpty ? "" : "." + ext)"); number += 1
            }
            update(id) { $0.name = destination.lastPathComponent; $0.path = destination.path; $0.expected = response.expectedContentLength }
            persist(); completionHandler(partial(id))
        } catch { finish(id, state: "failed", error: error.localizedDescription); completionHandler(nil) }
    }
    func downloadDidFinish(_ download: WKDownload) {
        guard let id = identifier(download), let item = entries.first(where: { $0.id == id }), item.state == "downloading", let path = item.path else { return }
        do {
            // Never replace an existing file, including one created after destination selection.
            try FileManager.default.moveItem(at: partial(id), to: URL(fileURLWithPath: path))
            let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size] as? NSNumber)?.int64Value ?? item.received
            update(id) { $0.received = size; $0.expected = size }
            finish(id, state: "completed")
        } catch { finish(id, state: "failed", error: error.localizedDescription) }
    }
    func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) {
        guard let id = identifier(download) else { return }
        finish(id, state: "failed", error: error.localizedDescription)
    }
    private func finish(_ id: UUID, state: String, error: String? = nil) {
        update(id) { $0.state = state; $0.error = error }
        tasks[id] = nil; observations[id] = nil
        if state != "completed" { try? FileManager.default.removeItem(at: partial(id)) }
        persist()
    }
    func cancel(_ id: UUID) {
        guard let download = tasks[id] else { return }
        update(id) { $0.state = "cancelled" }
        download.cancel { [weak self] _ in self?.finish(id, state: "cancelled") }
    }
    func reveal(_ item: DownloadEntry) {
        guard item.state == "completed", let path = item.path else { return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }
}

private struct DownloadPreferences: Codable {
    let directory: String
}

extension BrowserModel {
    nonisolated static func sanitizedHistoryURL(_ url: URL) -> String? {
        guard ["https", "http"].contains(url.scheme?.lowercased() ?? ""), let host = url.host?.lowercased() else { return nil }
        var components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        components?.fragment = nil
        if host == "accounts.google.com" || url.path.localizedCaseInsensitiveContains("signin_prompt") {
            components?.path = "/"
            components?.query = nil
        }
        return components?.url?.absoluteString
    }

    static func sanitizedHistory(_ entries: [HistoryEntry]) -> [HistoryEntry] {
        var merged: [String: HistoryEntry] = [:]
        for var entry in entries {
            guard let url = URL(string: entry.url), let safeURL = sanitizedHistoryURL(url) else { continue }
            entry.url = safeURL
            if var existing = merged[safeURL] {
                existing.visits += entry.visits
                if entry.date > existing.date { existing.date = entry.date; existing.title = entry.title }
                merged[safeURL] = existing
            } else {
                merged[safeURL] = entry
            }
        }
        return merged.values.sorted { $0.date > $1.date }
    }

    func recordVisit(_ tab: BrowserTab) {
        guard !tab.isPrivate else { return }
        guard let url = tab.webView.url, ["https", "http"].contains(url.scheme ?? "") else { return }
        guard let safeURL = Self.sanitizedHistoryURL(url) else { return }
        var item = history.first { $0.url == safeURL } ?? HistoryEntry(title: tab.title, url: safeURL, visits: 0)
        item.title = tab.webView.title?.isEmpty == false ? tab.webView.title! : url.host ?? tab.title
        item.date = Date(); item.visits += 1
        history.removeAll { $0.url == item.url }; history.insert(item, at: 0)
        // Imported history remains available; no implicit truncation on the next visit.
        saveLibrary()
    }
    func matchingHistory(_ query: String) -> [HistoryEntry] {
        let query = query.trimmingCharacters(in: .whitespacesAndNewlines)
        return history.filter { query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) || $0.url.localizedCaseInsensitiveContains(query) }
    }
    func addressSuggestions(_ query: String, limit: Int = 6) -> [HistoryEntry] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !needle.isEmpty else { return [] }
        let candidates = history.compactMap { entry -> (HistoryEntry, URL, String)? in
            guard let url = URL(string: entry.url), let rawHost = url.host?.lowercased(), rawHost != "accounts.google.com" else { return nil }
            let host = rawHost.hasPrefix("www.") ? String(rawHost.dropFirst(4)) : rawHost
            let title = entry.title.lowercased()
            guard title.contains(needle) || entry.url.lowercased().contains(needle) || host.contains(needle) else { return nil }
            return (entry, url, host)
        }
        let hostVisits = Dictionary(grouping: history.compactMap { entry -> (String, Int)? in
            guard let rawHost = URL(string: entry.url)?.host?.lowercased(), rawHost != "accounts.google.com" else { return nil }
            return (rawHost.hasPrefix("www.") ? String(rawHost.dropFirst(4)) : rawHost, entry.visits)
        }, by: \.0).mapValues { $0.reduce(0) { $0 + $1.1 } }
        let now = Date()

        func score(_ candidate: (HistoryEntry, URL, String)) -> Int {
            let (entry, url, host) = candidate
            let title = entry.title.lowercased()
            let words = title.split { !$0.isLetter && !$0.isNumber }.map(String.init)
            let pathParts = url.pathComponents.map { $0.lowercased() }
            let match: Int
            if host == needle { match = 1_200 }
            else if host.hasPrefix(needle) { match = 900 }
            else if title.hasPrefix(needle) { match = 760 }
            else if words.contains(where: { $0.hasPrefix(needle) }) { match = 700 }
            else if host.contains(needle) { match = 560 }
            else if pathParts.contains(where: { $0.hasPrefix(needle) }) { match = 500 }
            else if title.contains(needle) { match = 440 }
            else { match = 350 }

            // A logarithmic popularity boost lets frequently used pages win close
            // matches without allowing a weak URL substring to swamp a good match.
            let popularity = min(260, Int(log2(Double(max(1, hostVisits[host] ?? entry.visits)) + 1) * 42))
            let isHomepage = url.path.isEmpty || url.path == "/"
            let homepage = isHomepage ? 80 : 0
            let ageInDays = max(0, now.timeIntervalSince(entry.date) / 86_400)
            let recency = max(0, 45 - Int(ageInDays))
            return match + popularity + homepage + recency
        }

        return candidates.sorted { lhs, rhs in
            let lhsScore = score(lhs), rhsScore = score(rhs)
            if lhsScore != rhsScore { return lhsScore > rhsScore }
            if lhs.0.visits != rhs.0.visits { return lhs.0.visits > rhs.0.visits }
            return lhs.0.date > rhs.0.date
        }.prefix(max(0, limit)).map(\.0)
    }
    func updateVisitTitle(_ tab: BrowserTab) {
        guard !tab.isPrivate else { return }
        guard let title = tab.webView.title, !title.isEmpty,
              let url = tab.webView.url, let safeURL = Self.sanitizedHistoryURL(url),
              let index = history.firstIndex(where: { $0.url == safeURL }), history[index].title != title else { return }
        history[index].title = title; saveLibrary()
    }
    func clearHistory() { history = []; flushLibrary() }

    /// Schedules a history write.
    ///
    /// This runs on every finished navigation. Encoding and writing the whole
    /// list synchronously on the main actor made each page load stall for longer
    /// and longer as the history grew, so bursts are coalesced and the file work
    /// happens off the main actor.
    func saveLibrary() {
        libraryTask?.cancel()
        libraryTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 400_000_000)
            guard !Task.isCancelled, let self else { return }
            flushLibrary()
        }
    }

    /// Writes the history immediately. Used when the change must not be lost,
    /// for example after the user clears it.
    ///
    /// - Parameter waitForCompletion: write inline instead of on a background
    ///   task. Required while the app terminates, because a detached task is not
    ///   guaranteed to finish once the process is going away.
    func flushLibrary(waitForCompletion: Bool = false) {
        libraryTask?.cancel()
        libraryTask = nil
        markSyncChanged()
        let snapshot = history
        let destination = home.appendingPathComponent("history.json")
        if waitForCompletion {
            notice = Self.writeHistory(snapshot, to: destination) ?? notice
            return
        }
        // This task is main-actor isolated, so only Sendable values cross into
        // the detached write and the result is reported back here.
        Task { [weak self] in
            let failure = await Self.writeHistoryOffMainActor(snapshot, to: destination)
            if let failure { self?.notice = failure }
        }
    }

    /// Returns a message on failure, `nil` on success.
    private nonisolated static func writeHistory(_ snapshot: [HistoryEntry], to destination: URL) -> String? {
        do {
            try JSONEncoder().encode(snapshot).write(to: destination, options: .atomic)
            return nil
        } catch {
            return L("Verlauf konnte nicht gespeichert werden: \(error.localizedDescription)",
                     "History could not be saved: \(error.localizedDescription)")
        }
    }

    private nonisolated static func writeHistoryOffMainActor(_ snapshot: [HistoryEntry], to destination: URL) async -> String? {
        await Task.detached(priority: .utility) { writeHistory(snapshot, to: destination) }.value
    }
    @discardableResult
    func reopenTab() -> BrowserTab? {
        guard var saved = closedTabs.popLast() else { return nil }
        clearClosedTabUndo()
        let closedIndex = saved.closedIndex
        saved.closedIndex = nil
        if tabs.contains(where: { $0.id == saved.id }) { saved.id = UUID() }
        let tab = BrowserTab(saved: saved, owner: self)
        if let closedIndex { tabs.insert(tab, at: min(max(0, closedIndex), tabs.count)) }
        else { tabs.append(tab) }
        if #available(macOS 15.4, *) { extensions.runtime.controller.didOpenTab(tab) }
        select(tab.id); save(); return tab
    }
    func moveTab(_ id: UUID, relativeTo target: UUID, after: Bool) -> Bool {
        guard id != target, let source = tabs.first(where: { $0.id == id }), let destination = tabs.first(where: { $0.id == target }), source.space == destination.space else { return false }
        tabs.removeAll { $0.id == id }
        guard let targetIndex = tabs.firstIndex(where: { $0.id == target }) else { return false }
        source.pinned = destination.pinned
        source.folderID = destination.folderID
        tabs.insert(source, at: targetIndex + (after ? 1 : 0)); save(); return true
    }
    func tabDropIntent(_ id: UUID, on target: UUID, locationY: CGFloat, rowHeight: CGFloat) -> TabDropIntent {
        let intent = TabDropIntent.resolve(locationY: locationY, rowHeight: rowHeight)
        guard intent == .split,
              let source = tabs.first(where: { $0.id == id }),
              let destination = tabs.first(where: { $0.id == target }),
              destination.folderID != nil,
              source.folderID != destination.folderID else { return intent }
        // Dropping on a tab inside another folder should file the dragged tab
        // beside it; split view remains available within the same folder.
        return .after
    }
    func handleTabDrop(_ id: UUID, on target: UUID, locationY: CGFloat, rowHeight: CGFloat) -> Bool {
        switch tabDropIntent(id, on: target, locationY: locationY, rowHeight: rowHeight) {
        case .before: return moveTab(id, relativeTo: target, after: false)
        case .split: return pairTabs(id, with: target)
        case .after: return moveTab(id, relativeTo: target, after: true)
        }
    }
}
