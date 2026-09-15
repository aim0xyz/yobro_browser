import Foundation
import WebKit

/// Per-website zoom levels.
///
/// YOBRO previously had no zoom for web pages at all: `pageZoom` was only
/// reachable through the extension API, and ⌘+/⌘-/⌘0 were unbound. Levels are
/// remembered per host, the way other browsers do it, so a site that needs a
/// larger scale keeps it on the next visit.
@MainActor
final class PageZoomStore: ObservableObject {
    /// The familiar browser zoom ladder.
    static let levels: [Double] = [0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0]
    static let standard: Double = 1.0

    @Published private(set) var levelsByHost: [String: Double] = [:]
    private let file: URL
    private var saveTask: Task<Void, Never>?

    init(home: URL) {
        file = home.appendingPathComponent("page-zoom.json")
        if let saved = PersistedState.load([String: Double].self, at: file).value {
            levelsByHost = saved.filter { Self.levels.contains($0.value) }
        }
    }

    /// Zoom applies per host, ignoring `www.` so both spellings share a level.
    static func key(for url: URL?) -> String? {
        guard let host = url?.host?.lowercased(), !host.isEmpty else { return nil }
        return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
    }

    func level(for url: URL?) -> Double {
        guard let key = Self.key(for: url) else { return Self.standard }
        return levelsByHost[key] ?? Self.standard
    }

    /// Returns the level actually applied, or `nil` when the URL has no host.
    @discardableResult
    func step(_ direction: Int, for url: URL?) -> Double? {
        guard let key = Self.key(for: url) else { return nil }
        let current = levelsByHost[key] ?? Self.standard
        let index = Self.levels.firstIndex(of: current) ?? Self.levels.firstIndex(of: Self.standard) ?? 0
        let next = Self.levels[min(max(0, index + direction), Self.levels.count - 1)]
        store(next, key: key)
        return next
    }

    @discardableResult
    func reset(for url: URL?) -> Double? {
        guard let key = Self.key(for: url) else { return nil }
        store(Self.standard, key: key)
        return Self.standard
    }

    private func store(_ value: Double, key: String) {
        // The default needs no entry; dropping it keeps the file small and makes
        // "is anything customised here" a simple lookup.
        if value == Self.standard { levelsByHost.removeValue(forKey: key) }
        else { levelsByHost[key] = value }
        save()
    }

    private func save() {
        saveTask?.cancel()
        let snapshot = levelsByHost
        let destination = file
        saveTask = Task {
            // Stepping through zoom levels fires several changes in a row.
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard !Task.isCancelled else { return }
            try? JSONEncoder().encode(snapshot).write(to: destination, options: .atomic)
        }
    }
}
