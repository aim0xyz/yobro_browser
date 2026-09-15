import SwiftUI
import WebKit
import AppKit

struct InstalledExtension: Codable, Identifiable {
    var id: UUID
    var name: String
    var version: String
    var file: String
    var enabled: Bool
    var permissions: [String]
    var sites: [String]
    var storeID: String? = nil
}

@MainActor
final class ExtensionStore: ObservableObject {
    @Published var entries: [InstalledExtension] = []
    @Published var errors: [UUID: String] = [:]
    @Published var message: String?
    @Published var busy = false
    @Published var pending: InstalledExtension?
    @Published var pendingWarnings: [String] = []
    let directory: URL
    private var runtimeObject: AnyObject?
    private var candidateObject: AnyObject?
    var supported: Bool { if #available(macOS 15.4, *) { return true }; return false }
    @available(macOS 15.4, *) var runtime: ExtensionRuntime { runtimeObject as! ExtensionRuntime }

    init(home: URL, websiteDataStore: WKWebsiteDataStore? = nil, profileIdentifier: UUID? = nil) {
        directory = home.appendingPathComponent("Extensions", isDirectory: true)
        if let data = try? Data(contentsOf: directory.appendingPathComponent("installed.json")) {
            do { entries = try JSONDecoder().decode([InstalledExtension].self, from: data) }
            catch { message = L("Erweiterungsliste konnte nicht gelesen werden: \(error.localizedDescription)", "Could not read extensions: \(error.localizedDescription)") }
        }
        if #available(macOS 15.4, *) { runtimeObject = ExtensionRuntime(websiteDataStore: websiteDataStore, profileIdentifier: profileIdentifier) }
    }
    func configure(_ configuration: WKWebViewConfiguration) {
        if #available(macOS 15.4, *) { configuration.webExtensionController = runtime.controller }
    }
    func start(owner: BrowserModel) async {
        guard #available(macOS 15.4, *) else { return }
        runtime.owner = owner
        runtime.controller.didOpenWindow(runtime)
        for entry in entries where entry.enabled {
            do { try await runtime.load(entry, from: directory.appendingPathComponent(entry.file)) }
            catch { errors[entry.id] = error.localizedDescription }
        }
    }
    func save() throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try JSONEncoder().encode(entries).write(to: directory.appendingPathComponent("installed.json"), options: .atomic)
    }
    func choose() {
        guard supported, !busy else { return }
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = true
        panel.message = L("Erweiterungsordner mit manifest.json oder eine ZIP-/CRX-Datei wählen.")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await prepare(url) }
    }
    func prepare(_ url: URL) async {
        guard #available(macOS 15.4, *), !busy else { return }
        busy = true; defer { busy = false }
        cancelPending()
        let isDirectory = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
        let id = UUID()
        let file = id.uuidString + (isDirectory ? "" : ".zip")
        let target = directory.appendingPathComponent(file, isDirectory: isDirectory)
        do {
            guard !directory.standardizedFileURL.path.hasPrefix(url.standardizedFileURL.path + "/"), directory.standardizedFileURL != url.standardizedFileURL else { throw YOBROError.message(L("Bitte nur den einzelnen Erweiterungsordner auswählen.")) }
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            if url.pathExtension.lowercased() == "crx" {
                guard (try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0) <= 200 * 1024 * 1024 else { throw YOBROError.message(L("Das Paket ist größer als 200 MB.")) }
                let data = try Data(contentsOf: url)
                try ExtensionPackage.zipPayload(data).write(to: target, options: .atomic)
            } else {
                guard isDirectory || url.pathExtension.lowercased() == "zip" else { throw YOBROError.message(L("Bitte einen Erweiterungsordner, ZIP oder CRX auswählen.")) }
                // A copied extension must not retain links into unrelated local folders.
                if isDirectory {
                    let enumerator = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.isSymbolicLinkKey])
                    while let child = enumerator?.nextObject() as? URL {
                        if try child.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink == true { throw YOBROError.message(L("Der Erweiterungsordner enthält symbolische Links. Bitte eine eigenständige Kopie verwenden.")) }
                    }
                }
                try FileManager.default.copyItem(at: url, to: target)
            }
            let ext = try await WKWebExtension(resourceBaseURL: target)
            guard ext.supportsManifestVersion(ext.manifestVersion) else { throw YOBROError.message(L("Diese Manifest-Version wird von WebKit nicht unterstützt.")) }
            let required = ext.manifest["permissions"] as? [String] ?? []
            if required.contains("proxy") {
                throw YOBROError.message(L("Diese Erweiterung benötigt die Chrome-Proxy-API (proxy). YoBro unterstützt diese API noch nicht; VPN-Verbindungen dieser Erweiterung funktionieren deshalb nicht. Das betrifft auch NordVPN. Für den VPN-Schutz kann die separate macOS-App des Anbieters verwendet werden."))
            }
            pendingWarnings = ext.errors.map(\.localizedDescription)
            let implemented = Set(ext.requestedPermissions.map(\.rawValue))
            let unavailable = required.filter { !implemented.contains($0) && !$0.contains("://") && $0 != "<all_urls>" }
            if !unavailable.isEmpty { pendingWarnings.append(L("Von WebKit nicht unterstützte Berechtigungen: ") + unavailable.joined(separator: ", ") + L(". Zugehörige Funktionen können ausfallen.")) }
            let extensionName = ext.displayName ?? L("Erweiterung", "Extension")
            if extensionName.localizedCaseInsensitiveContains("phantom") {
                pendingWarnings.insert(
                    L("Eingeschränkt kompatibel – Wallet-Funktionen sind in YoBro noch nicht verifiziert. Nicht unterstützte Chrome-APIs können Verbindung, Popup oder Signaturanfragen beeinträchtigen.",
                      "Limited compatibility — wallet features have not yet been verified in YoBro. Unsupported Chrome APIs may affect connection, popups, or signing requests."),
                    at: 0
                )
            }
            let permissions = ext.requestedPermissions.map(\.rawValue).sorted()
            let sites = ext.allRequestedMatchPatterns.map(\.string).sorted()
            candidateObject = ext
            pending = InstalledExtension(id: id, name: extensionName, version: ext.version ?? "", file: file, enabled: true, permissions: permissions, sites: sites)
        } catch {
            try? FileManager.default.removeItem(at: target)
            message = L("Installation nicht möglich: \(error.localizedDescription)", "Could not install: \(error.localizedDescription)")
        }
    }
    func installPending() async {
        guard #available(macOS 15.4, *), let entry = pending, let ext = candidateObject as? WKWebExtension, !busy else { return }
        busy = true; defer { busy = false }
        message = L("\(entry.name) wird gestartet …", "Starting \(entry.name) …")
        do {
            try await runtime.load(entry, extension: ext)
            entries.append(entry)
            do { try save() } catch { entries.removeAll { $0.id == entry.id }; try? runtime.unload(entry.id); throw error }
            pending = nil; candidateObject = nil; pendingWarnings = []
            message = L("\(entry.name) installiert. Bereits offene Webseiten gegebenenfalls neu laden.", "\(entry.name) installed. You may need to reload open websites.")
        } catch {
            try? runtime.unload(entry.id)
            message = L("Installation von \(entry.name) fehlgeschlagen: \(error.localizedDescription)", "Could not install \(entry.name): \(error.localizedDescription)")
        }
    }
    func cancelPending() {
        if let pending { try? FileManager.default.removeItem(at: directory.appendingPathComponent(pending.file)) }
        pending = nil; candidateObject = nil; pendingWarnings = []
    }
    func toggle(_ entry: InstalledExtension) async {
        guard #available(macOS 15.4, *), let i = entries.firstIndex(where: { $0.id == entry.id }), !busy else { return }
        busy = true; defer { busy = false }
        do {
            if entry.enabled { try runtime.unload(entry.id) }
            else { try await runtime.load(entry, from: directory.appendingPathComponent(entry.file)) }
            entries[i].enabled.toggle()
            do { try save() } catch {
                entries[i].enabled = entry.enabled
                if entry.enabled { try? await runtime.load(entry, from: directory.appendingPathComponent(entry.file)) }
                else { try? runtime.unload(entry.id) }
                throw error
            }
            errors[entry.id] = nil
        } catch { errors[entry.id] = error.localizedDescription }
    }
    func remove(_ entry: InstalledExtension) {
        guard #available(macOS 15.4, *) else { return }
        do {
            try runtime.unload(entry.id)
            let previous = entries
            entries.removeAll { $0.id == entry.id }
            do { try save() } catch { entries = previous; throw error }
            try FileManager.default.removeItem(at: directory.appendingPathComponent(entry.file))
            errors[entry.id] = nil
        } catch { message = error.localizedDescription }
    }
    func perform(_ entry: InstalledExtension, tab: BrowserTab?) {
        if #available(macOS 15.4, *), let context = runtime.contexts[entry.id] {
            if let tab { context.userGesturePerformed(in: tab) }
            context.performAction(for: tab)
        }
    }
}

enum ExtensionPackage {
    static func zipPayload(_ data: Data) throws -> Data {
        func uint(_ offset: Int) throws -> Int {
            guard offset + 4 <= data.count else { throw YOBROError.message(L("Unvollständige CRX-Datei.")) }
            return (0..<4).reduce(0) { $0 | Int(data[offset + $1]) << ($1 * 8) }
        }
        guard data.count >= 12, data.prefix(4) == Data("Cr24".utf8) else { throw YOBROError.message(L("Keine gültige CRX-Datei.")) }
        let version = try uint(4)
        let offset: Int
        switch version {
        case 2: offset = try 16 + uint(8) + uint(12)
        case 3: offset = try 12 + uint(8)
        default: throw YOBROError.message(L("Diese CRX-Version wird nicht unterstützt."))
        }
        guard offset <= data.count - 4, data[offset..<offset+4] == Data([0x50, 0x4b, 0x03, 0x04]) else { throw YOBROError.message(L("Das CRX enthält kein gültiges ZIP-Archiv.")) }
        return Data(data.dropFirst(offset))
    }
}

@available(macOS 15.4, *)
@MainActor
final class ExtensionRuntime: NSObject, WKWebExtensionControllerDelegate, WKWebExtensionWindow {
    let controller: WKWebExtensionController
    weak var owner: BrowserModel?
    var contexts: [UUID: WKWebExtensionContext] = [:]
    private var backgroundLoadTasks: [UUID: Task<Void, Never>] = [:]
    private var popupWindows: [NSWindow] = []
    private var actionPopover: NSPopover?
    private var actionPopoverEventMonitor: Any?
    private var actionPopoverCloseObserver: NSObjectProtocol?
    private var applicationResignObserver: NSObjectProtocol?
    init(websiteDataStore: WKWebsiteDataStore? = nil, profileIdentifier: UUID? = nil) {
        let configuration: WKWebExtensionController.Configuration
        if let websiteDataStore, !websiteDataStore.isPersistent { configuration = .nonPersistent() }
        else if let profileIdentifier { configuration = .init(identifier: profileIdentifier) }
        else { configuration = .default() }
        if let websiteDataStore {
            configuration.defaultWebsiteDataStore = websiteDataStore
            let webViewConfiguration = WKWebViewConfiguration()
            webViewConfiguration.websiteDataStore = websiteDataStore
            configuration.webViewConfiguration = webViewConfiguration
        }
        controller = WKWebExtensionController(configuration: configuration)
        super.init(); controller.delegate = self
    }
    func load(_ entry: InstalledExtension, from url: URL) async throws { try await load(entry, extension: WKWebExtension(resourceBaseURL: url)) }
    func load(_ entry: InstalledExtension, extension ext: WKWebExtension) async throws {
        let context = WKWebExtensionContext(for: ext)
        context.uniqueIdentifier = entry.id.uuidString
        context.baseURL = URL(string: "webkit-extension://\(entry.id.uuidString.lowercased())/")!
        context.unsupportedAPIs = ["bookmarks", "history", "downloads", "runtime.connectNative", "runtime.sendNativeMessage", "sessions", "topSites"]
        for permission in ext.requestedPermissions where entry.permissions.contains(permission.rawValue) { context.setPermissionStatus(.grantedExplicitly, for: permission) }
        for pattern in ext.allRequestedMatchPatterns where entry.sites.contains(pattern.string) { context.setPermissionStatus(.grantedExplicitly, for: pattern) }
        // Register the context first so installation is complete independently
        // of a large on-demand MV3 worker's startup time. YOBRO still warms the
        // worker below because message listeners need it immediately.
        try controller.load(context)
        contexts[entry.id] = context
        if ext.hasBackgroundContent {
            let required = Set(ext.manifest["permissions"] as? [String] ?? [])
            let needsDeferredBackgroundStart = !required.isDisjoint(with: ["identity", "sidePanel"])
            if needsDeferredBackgroundStart {
                backgroundLoadTasks[entry.id]?.cancel()
                backgroundLoadTasks[entry.id] = Task { @MainActor [weak self] in
                    defer { self?.backgroundLoadTasks[entry.id] = nil }
                    do { try await context.loadBackgroundContent() }
                    catch {
                        guard !Task.isCancelled else { return }
                        self?.owner?.extensions.errors[entry.id] = L(
                            "Hintergrunddienst konnte nicht gestartet werden: \(error.localizedDescription)",
                            "Could not start background service: \(error.localizedDescription)"
                        )
                    }
                }
            } else {
                try await context.loadBackgroundContent()
            }
        }
    }
    func unload(_ id: UUID) throws {
        backgroundLoadTasks[id]?.cancel()
        backgroundLoadTasks[id] = nil
        if let context = contexts[id] { try controller.unload(context); contexts[id] = nil }
    }
    func tabs(for context: WKWebExtensionContext) -> [any WKWebExtensionTab] { owner?.tabs ?? [] }
    func activeTab(for context: WKWebExtensionContext) -> (any WKWebExtensionTab)? { owner?.active }
    func webExtensionController(_ controller: WKWebExtensionController, openWindowsFor context: WKWebExtensionContext) -> [any WKWebExtensionWindow] { [self] }
    func webExtensionController(_ controller: WKWebExtensionController, focusedWindowFor context: WKWebExtensionContext) -> (any WKWebExtensionWindow)? { self }
    func webExtensionController(_ controller: WKWebExtensionController, openNewTabUsing configuration: WKWebExtension.TabConfiguration, for context: WKWebExtensionContext, completionHandler: @escaping ((any WKWebExtensionTab)?, Error?) -> Void) {
        guard let owner else { completionHandler(nil, YOBROError.message(L("Browser geschlossen", "Browser closed"))); return }
        let previous = owner.activeID
        let isExtensionPage = configuration.url.map { $0.scheme == context.baseURL.scheme && $0.host == context.baseURL.host } ?? false
        // WebKit requires a context-specific configuration for extension-origin pages.
        // A normal browser WKWebView cancels these navigations with NSURLErrorResourceUnavailable.
        // BrowserTab copies this template and supplies a tab-local user-content
        // controller so YOBRO message handlers are never registered twice.
        let tab = owner.newTab(webViewConfiguration: isExtensionPage ? context.webViewConfiguration : nil)
        if let url = configuration.url {
            guard ["http", "https"].contains(url.scheme ?? "") || context.baseURL.host == url.host && context.baseURL.scheme == url.scheme else {
                owner.closeTab(tab.id); completionHandler(nil, YOBROError.message(L("Nicht unterstützte Adresse"))); return
            }
            tab.webView.load(URLRequest(url: url))
        }
        tab.pinned = configuration.shouldBePinned
        if !configuration.shouldBeActive, let previous { owner.select(previous) }
        completionHandler(tab, nil)
    }
    func webExtensionController(_ controller: WKWebExtensionController, presentActionPopup action: WKWebExtension.Action, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) {
        guard let popover = action.popupPopover, let view = owner?.active?.webView, view.window != nil else { completionHandler(YOBROError.message(L("Bitte zuerst einen Browser-Tab öffnen."))); return }
        dismissActionPopover()
        // A transient NSPopover is dismissed when SwiftUI presents the folder
        // preview popover on hover. Keep the extension popup under our control
        // so pointer-only previews do not count as an outside interaction.
        popover.behavior = .applicationDefined
        popover.show(relativeTo: NSRect(x: max(0, view.bounds.width - 44), y: view.bounds.height - 1, width: 1, height: 1), of: view, preferredEdge: .minY)
        trackActionPopover(popover)
        completionHandler(nil)
    }
    private func trackActionPopover(_ popover: NSPopover) {
        actionPopover = popover
        owner?.extensionActionPopupPresented = true
        actionPopoverCloseObserver = NotificationCenter.default.addObserver(forName: NSPopover.didCloseNotification, object: popover, queue: .main) { [weak self, weak popover] _ in
            MainActor.assumeIsolated {
                guard let self, self.actionPopover === popover else { return }
                self.clearActionPopoverTracking()
            }
        }
        applicationResignObserver = NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.dismissActionPopover() }
        }
        actionPopoverEventMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .keyDown]) { [weak self, weak popover] event in
            guard let self else { return event }
            if event.type == .keyDown && event.keyCode == 53 {
                self.dismissActionPopover()
                return nil
            }
            if event.type != .keyDown, event.window !== popover?.contentViewController?.view.window {
                self.dismissActionPopover()
            }
            return event
        }
    }
    private func dismissActionPopover() {
        let popover = actionPopover
        clearActionPopoverTracking()
        popover?.close()
    }
    private func clearActionPopoverTracking() {
        if let actionPopoverEventMonitor { NSEvent.removeMonitor(actionPopoverEventMonitor) }
        if let actionPopoverCloseObserver { NotificationCenter.default.removeObserver(actionPopoverCloseObserver) }
        if let applicationResignObserver { NotificationCenter.default.removeObserver(applicationResignObserver) }
        actionPopoverEventMonitor = nil
        actionPopoverCloseObserver = nil
        applicationResignObserver = nil
        actionPopover = nil
        owner?.extensionActionPopupPresented = false
    }
    func webExtensionController(_ controller: WKWebExtensionController, openOptionsPageFor context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) {
        guard let url = context.optionsPageURL, let config = context.webViewConfiguration else { completionHandler(YOBROError.message(L("Keine Einstellungsseite verfügbar."))); return }
        let view = WKWebView(frame: NSRect(x: 0, y: 0, width: 760, height: 600), configuration: config)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = context.webExtension.displayName ?? L("Erweiterung", "Extension"); window.contentView = view
        window.isReleasedWhenClosed = false; popupWindows.removeAll { !$0.isVisible }; popupWindows.append(window)
        view.load(URLRequest(url: url)); window.center(); window.makeKeyAndOrderFront(nil); completionHandler(nil)
    }
    private func requestApproval(_ context: WKWebExtensionContext, details: [String], tab: (any WKWebExtensionTab)?, completion: @escaping (Bool) -> Void) {
        guard let owner, let tabID = (tab as? BrowserTab)?.id ?? owner.activeID else { completion(false); return }
        owner.presentWebPageDialog(
            tabID: tabID,
            title: L("Zusätzlicher Zugriff für \(context.webExtension.displayName ?? "Erweiterung")?", "Additional access for \(context.webExtension.displayName ?? "Extension")?"),
            message: details.sorted().joined(separator: "\n"),
            kind: .confirm,
            completion: completion
        )
    }
    func webExtensionController(_ controller: WKWebExtensionController, promptForPermissions permissions: Set<WKWebExtension.Permission>, in tab: (any WKWebExtensionTab)?, for context: WKWebExtensionContext, completionHandler: @escaping (Set<WKWebExtension.Permission>, Date?) -> Void) {
        requestApproval(context, details: permissions.map(\.rawValue), tab: tab) { completionHandler($0 ? permissions : [], nil) }
    }
    func webExtensionController(_ controller: WKWebExtensionController, promptForPermissionMatchPatterns patterns: Set<WKWebExtension.MatchPattern>, in tab: (any WKWebExtensionTab)?, for context: WKWebExtensionContext, completionHandler: @escaping (Set<WKWebExtension.MatchPattern>, Date?) -> Void) {
        requestApproval(context, details: patterns.map(\.string), tab: tab) { completionHandler($0 ? patterns : [], nil) }
    }
    func webExtensionController(_ controller: WKWebExtensionController, promptForPermissionToAccess urls: Set<URL>, in tab: (any WKWebExtensionTab)?, for context: WKWebExtensionContext, completionHandler: @escaping (Set<URL>, Date?) -> Void) {
        requestApproval(context, details: urls.map(\.absoluteString), tab: tab) { completionHandler($0 ? urls : [], nil) }
    }
}

@available(macOS 15.4, *)
extension BrowserTab: WKWebExtensionTab {
    func window(for context: WKWebExtensionContext) -> (any WKWebExtensionWindow)? { owner?.extensions.runtime }
    func indexInWindow(for context: WKWebExtensionContext) -> Int { owner?.tabs.firstIndex { $0.id == id } ?? 0 }
    func webView(for context: WKWebExtensionContext) -> WKWebView? { webView }
    func title(for context: WKWebExtensionContext) -> String? { title }
    func url(for context: WKWebExtensionContext) -> URL? { webView.url }
    func isLoadingComplete(for context: WKWebExtensionContext) -> Bool { !loading }
    func isPinned(for context: WKWebExtensionContext) -> Bool { pinned }
    func isSelected(for context: WKWebExtensionContext) -> Bool { owner?.activeID == id }
    func size(for context: WKWebExtensionContext) -> CGSize { webView.bounds.size }
    func zoomFactor(for context: WKWebExtensionContext) -> Double { webView.pageZoom }
    func setZoomFactor(_ zoomFactor: Double, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { webView.pageZoom = zoomFactor; completionHandler(nil) }
    func activate(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { owner?.select(id); completionHandler(nil) }
    func close(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { owner?.closeTab(id); completionHandler(nil) }
    func setPinned(_ pinned: Bool, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { self.pinned = pinned; owner?.save(); completionHandler(nil) }
    func loadURL(_ url: URL, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) {
        if url.scheme == context.baseURL.scheme && url.host == context.baseURL.host { webView.load(URLRequest(url: url)); completionHandler(nil); return }
        do { try navigate(url.absoluteString); completionHandler(nil) } catch { completionHandler(error) }
    }
    func reload(fromOrigin: Bool, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { if fromOrigin { webView.reloadFromOrigin() } else { webView.reload() }; completionHandler(nil) }
    func goBack(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { webView.goBack(); completionHandler(nil) }
    func goForward(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { webView.goForward(); completionHandler(nil) }
}
