import SwiftUI
import WebKit

@MainActor final class MobileDialog: Identifiable {
    enum Kind { case alert, confirm, prompt }
    let id = UUID()
    let tabID: UUID
    let origin: String
    let message: String
    let kind: Kind
    let defaultText: String
    private var completion: ((String?) -> Void)?
    init(tabID: UUID, origin: String, message: String, kind: Kind, defaultText: String = "", completion: @escaping (String?) -> Void) {
        self.tabID = tabID; self.origin = origin; self.message = message; self.kind = kind; self.defaultText = defaultText; self.completion = completion
    }
    func finish(_ value: String?) { let callback = completion; completion = nil; callback?(value) }
}
struct MobileDownload: Codable, Identifiable {
    var id = UUID()
    var name = "Download"
    var file: String?
    var status = "Wird geladen …"
    var complete = false
}
@MainActor final class MobileDownloads: NSObject, ObservableObject, WKDownloadDelegate {
    @Published private(set) var entries: [MobileDownload] = []
    @Published var error: String?
    let directory: URL
    private var active: [ObjectIdentifier: (WKDownload, UUID)] = [:]
    private var stateURL: URL { directory.appendingPathComponent("downloads.json") }
    init(home: URL) {
        directory = home.appendingPathComponent("Downloads", isDirectory: true)
        super.init()
        let loaded = PersistedState.load([MobileDownload].self, at: stateURL)
        entries = loaded.value ?? []; error = loaded.problem
        for i in entries.indices where !entries[i].complete { entries[i].status = "Unterbrochen" }
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
        catch { self.error = error.localizedDescription }
    }
    func begin(_ download: WKDownload) {
        let entry = MobileDownload(); entries.insert(entry, at: 0)
        active[ObjectIdentifier(download)] = (download, entry.id); download.delegate = self; save()
    }
    static func safeName(_ proposed: String) -> String {
        let name = (proposed as NSString).lastPathComponent.replacingOccurrences(of: ":", with: "_")
        return name.isEmpty || name == "." || name == ".." ? "Download" : String(name.prefix(180))
    }
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String, completionHandler: @escaping (URL?) -> Void) {
        guard let (_, id) = active[ObjectIdentifier(download)], let index = entries.firstIndex(where: { $0.id == id }) else { completionHandler(nil); return }
        let name = Self.safeName(suggestedFilename)
        // Separate UUID folder preserves a useful filename without overwriting an existing download.
        let folder = directory.appendingPathComponent(id.uuidString, isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true, attributes: [.protectionKey: FileProtectionType.complete])
            entries[index].name = name; entries[index].file = id.uuidString + "/" + name; save()
            completionHandler(folder.appendingPathComponent(name))
        } catch { entries[index].status = error.localizedDescription; completionHandler(nil); save() }
    }
    func downloadDidFinish(_ download: WKDownload) { finish(download, message: "Abgeschlossen", success: true) }
    func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) { finish(download, message: error.localizedDescription, success: false) }
    private func finish(_ download: WKDownload, message: String, success: Bool) {
        guard let (_, id) = active.removeValue(forKey: ObjectIdentifier(download)), let index = entries.firstIndex(where: { $0.id == id }) else { return }
        entries[index].status = message; entries[index].complete = success
        if !success { removePartial(entries[index]) }; save()
    }
    func cancel(_ id: UUID) {
        guard let pair = active.first(where: { $0.value.1 == id }) else { return }
        pair.value.0.cancel { _ in }
        finish(pair.value.0, message: "Abgebrochen", success: false)
    }
    func cancelAll() { for id in active.values.map({ $0.1 }) { cancel(id) } }
    func fileURL(_ entry: MobileDownload) -> URL? {
        guard entry.complete, let path = entry.file else { return nil }
        let url = directory.appendingPathComponent(path).standardizedFileURL
        guard url.path.hasPrefix(directory.standardizedFileURL.path + "/"), FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
    }
    private func removePartial(_ entry: MobileDownload) {
        try? FileManager.default.removeItem(at: directory.appendingPathComponent(entry.id.uuidString))
    }
    private func save() {
        do { try JSONEncoder().encode(entries).write(to: stateURL, options: [.atomic, .completeFileProtection]) }
        catch { self.error = error.localizedDescription }
    }
}
struct MobileDownloadList: View {
    @ObservedObject var downloads: MobileDownloads
    var body: some View {
        List {
            if let error = downloads.error { Text(error).foregroundStyle(.red) }
            if downloads.entries.isEmpty { ContentUnavailableView("Keine Downloads", systemImage: "arrow.down.circle") }
            ForEach(downloads.entries) { entry in
                HStack {
                    VStack(alignment: .leading) { Text(entry.name); Text(entry.status).font(.caption).foregroundStyle(.secondary) }
                    Spacer()
                    if let url = downloads.fileURL(entry) { ShareLink(item: url) { Image(systemName: "square.and.arrow.up") } }
                    else if entry.status == "Wird geladen …" { Button("Abbrechen") { downloads.cancel(entry.id) } }
                }.listRowBackground(YOBROTheme.surface)
            }
        }.mobileSurface().navigationTitle("Downloads")
    }
}
