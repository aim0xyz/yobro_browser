import Foundation
import Darwin
import WebKit
import AppKit

// A user-private Unix socket. No TCP listener, browser extension, CDP, or screen access.
// Immutable connection configuration; all browser state is accessed on MainActor.
final class ControlBridge: @unchecked Sendable {
    private let descriptor: Int32
    private let path: String
    private let handler: @MainActor ([String: Any]) async throws -> [String: Any]
    private let queue = DispatchQueue(label: "yobro.control.accept", qos: .utility)

    init(path: String, handler: @escaping @MainActor ([String: Any]) async throws -> [String: Any]) throws {
        self.path = path; self.handler = handler
        guard path.utf8.count < 104 else { throw YOBROError.message("Socket-Pfad zu lang. YOBRO_HOME kürzer setzen.") }
        descriptor = socket(AF_UNIX, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw YOBROError.message("Socket konnte nicht erstellt werden.") }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        withUnsafeMutableBytes(of: &address.sun_path) { buffer in
            buffer.copyBytes(from: Array(path.utf8) + [0])
        }
        // Do not steal a socket from another running instance.
        let probe = socket(AF_UNIX, SOCK_STREAM, 0)
        let existing = withUnsafePointer(to: &address) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(probe, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        Darwin.close(probe)
        if existing == 0 { Darwin.close(descriptor); throw YOBROError.message("Eine andere YoBro-Instanz nutzt bereits diese Schnittstelle.") }
        unlink(path)
        let bound = withUnsafePointer(to: &address) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(descriptor, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard bound == 0 else { Darwin.close(descriptor); throw YOBROError.message("Socket konnte nicht gebunden werden: \(String(cString: strerror(errno)))") }
        chmod(path, 0o600)
        guard listen(descriptor, 8) == 0 else { Darwin.close(descriptor); unlink(path); throw YOBROError.message("Socket konnte nicht gestartet werden.") }
        let fd = descriptor
        queue.async { [weak self] in
            while true {
                let client = accept(fd, nil, nil)
                if client < 0 { break }
                guard let self else { Darwin.close(client); break }
                var uid: uid_t = 0; var gid: gid_t = 0
                guard getpeereid(client, &uid, &gid) == 0, uid == getuid() else { Darwin.close(client); continue }
                var timeout = timeval(tv_sec: 10, tv_usec: 0)
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
                var noSignal: Int32 = 1
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size))
                DispatchQueue.global(qos: .utility).async { self.receive(client) }
            }
        }
    }

    private func receive(_ client: Int32) {
        var data = Data()
        var bytes = [UInt8](repeating: 0, count: 4096)
        while data.count < 1_048_576 {
            let count = Darwin.read(client, &bytes, bytes.count)
            if count <= 0 { Darwin.close(client); return }
            data.append(contentsOf: bytes.prefix(count))
            if data.contains(10) { break }
        }
        guard let end = data.firstIndex(of: 10), let request = (try? JSONSerialization.jsonObject(with: data[..<end])) as? [String: Any] else {
            respond(client, ["ok": false, "error": "Invalid JSON request or request too large."]); return
        }
        Task { @MainActor in
            let response: [String: Any]
            do { response = ["ok": true, "result": try await handler(request)] }
            catch { response = ["ok": false, "error": error.localizedDescription] }
            DispatchQueue.global(qos: .utility).async { self.respond(client, response) }
        }
    }
    private func respond(_ client: Int32, _ response: [String: Any]) {
        defer { Darwin.close(client) }
        guard var data = try? JSONSerialization.data(withJSONObject: response, options: [.sortedKeys]) else { return }
        data.append(10)
        data.withUnsafeBytes { raw in
            var sent = 0
            while sent < raw.count {
                let count = Darwin.write(client, raw.baseAddress!.advanced(by: sent), raw.count - sent)
                if count <= 0 { break }
                sent += count
            }
        }
    }
    deinit { Darwin.close(descriptor); unlink(path) }
}

extension BrowserModel {
    private func target(_ request: [String: Any]) throws -> BrowserTab {
        if let id = request["tab"] as? String {
            guard let tab = tabs.first(where: { $0.id.uuidString.lowercased() == id.lowercased() }) else { throw YOBROError.message("Unbekannter Tab.") }
            guard agentTabIDs.contains(tab.id) else { throw YOBROError.message("This is a user tab. Open its URL with new to work in the right agent pane.") }
            agentTabID = tab.id
            return tab
        }
        if let agentTab { return agentTab }
        return newAgentTab()
    }
    func tabInfo(_ tab: BrowserTab) -> [String: Any] {
        ["id": tab.id.uuidString.lowercased(), "title": tab.title, "url": tab.url, "space": tab.space,
         "owner": agentTabIDs.contains(tab.id) ? "agent" : "user", "agentActive": tab.id == agentTabID, "pinned": tab.pinned, "active": tab.id == activeID, "loading": tab.webView.isLoading,
         "error": tab.error as Any? ?? NSNull()]
    }
    func waitForLoad(_ tab: BrowserTab) async throws {
        let deadline = Date().addingTimeInterval(20)
        try await Task.sleep(nanoseconds: 150_000_000)
        while tab.webView.isLoading && Date() < deadline { try await Task.sleep(nanoseconds: 100_000_000) }
        guard agentEnabled, isProfileActive else {
            throw YOBROError.message("Agent operation was paused.")
        }
        if tab.webView.isLoading { throw YOBROError.message("Seite lädt noch. Später erneut mit read prüfen.") }
        if let error = tab.error { throw YOBROError.message(error) }
    }
    func evaluate(_ script: String, tab: BrowserTab) async throws -> Any {
        try await withCheckedThrowingContinuation { continuation in
            tab.webView.evaluateJavaScript(script, in: nil, in: .defaultClient) { result in
                switch result {
                case .success(let value): continuation.resume(returning: value)
                case .failure(let error):
                    let detail = (error as NSError).userInfo["WKJavaScriptExceptionMessage"] as? String
                    continuation.resume(throwing: detail.map { YOBROError.message($0) } ?? error)
                }
            }
        }
    }
    func prepare(_ tab: BrowserTab) async throws {
        guard !tab.url.isEmpty else { throw YOBROError.message("Dieser Tab zeigt die Startseite. Zuerst eine URL öffnen.") }
        guard let resource = Bundle.main.url(forResource: "AgentBridge", withExtension: "js")
            ?? Bundle.module.url(forResource: "AgentBridge", withExtension: "js") else { throw YOBROError.message("AgentBridge.js fehlt.") }
        _ = try await evaluate(String(contentsOf: resource), tab: tab)
    }
    func readTab(_ tab: BrowserTab) async throws -> [String: Any] {
        try await waitForLoad(tab)
        try await prepare(tab)
        let value = try await evaluate("globalThis.__yobro.snapshot()", tab: tab)
        record("read", tab.webView.url?.host ?? "Seite gelesen", in: tab.space)
        return ["tab": tabInfo(tab), "page": value]
    }
    func actOnTab(_ tab: BrowserTab, action: String, ref: String, document: String, value: String? = nil, key: String? = nil) async throws -> [String: Any] {
        try await prepare(tab)
        guard agentEnabled, isProfileActive else { throw YOBROError.message("Agent operation was paused.") }
        let args: [String: Any] = [
            "action": action,
            "ref": ref,
            "document": document,
            "value": value ?? "",
            "key": key ?? "Enter"
        ]
        let json = String(data: try JSONSerialization.data(withJSONObject: args), encoding: .utf8)!
        let result = try await evaluate("globalThis.__yobro.act(\(json))", tab: tab)
        record(action, "\(ref) · \(tab.webView.url?.host ?? "Seite")", in: tab.space)
        return ["tab": tabInfo(tab), "action": result, "next": "Read again to verify the outcome; pages may update asynchronously."]
    }
    func scrollTab(_ tab: BrowserTab, amount: Int) async throws -> [String: Any] {
        let clamped = min(5000, max(-5000, amount))
        _ = try await evaluate("window.scrollBy(0, \(clamped)); true", tab: tab)
        record("scroll", tab.webView.url?.host ?? tab.title, in: tab.space)
        return tabInfo(tab)
    }
    func navigateTab(_ tab: BrowserTab, url: URL) async throws -> [String: Any] {
        if agentTabIDs.contains(tab.id) {
            agentTabID = tab.id
            presentAgentWorkspaceIfAppropriate()
        }
        try tab.navigate(url.absoluteString)
        try await waitForLoad(tab)
        record("open", tab.webView.url?.host ?? tab.title, in: tab.space)
        return tabInfo(tab)
    }
    func handle(_ request: [String: Any]) async throws -> [String: Any] {
        guard isProfileActive else { throw YOBROError.message(L("Dieses Profil ist nicht aktiv. Wechsle im Browser zu diesem Profil.", "This profile is inactive. Switch to it in the browser.")) }
        let command = request["command"] as? String ?? "status"
        if command == "status" {
            var blocker: [String: Any] = [:]
            if #available(macOS 15.4, *) { blocker = extensions.runtime.blockerReadinessSummary }
            return ["browser": "YoBro", "version": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development", "engine": "WebKit", "blocker": blocker, "protocol": 2, "enabled": agentEnabled, "agentPaneVisible": agentWorkspaceVisible, "agentSessionActive": agentSessionActive, "agentUsageActive": agentUsageActive, "agentAction": agentAction as Any? ?? NSNull(),
                    "socket": controlSocketURL.path, "profile": profileName,
                    "libraryAccess": bridgeLibraryAccess,
                    "commands": ["status", "tabs", "open", "new", "focus", "close", "read", "click", "fill", "press", "scroll", "back", "forward", "reload", "pin", "split", "find", "history", "downloads", "download", "cancel-download", "duplicate", "end"]]
        }
        guard agentEnabled else { throw YOBROError.message("Agentenzugriff ist im Browser pausiert.") }
        if command == "tabs" { agentSessionActive = true; return ["tabs": tabs.map(tabInfo), "space": space] }
        if command == "end" {
            endAgentWorkspace()
            return ["ended": true]
        }
        guard agentAction == nil else { throw YOBROError.message("Agent is busy. Retry after the current command completes.") }
        if ["space", "panel", "restore", "move"].contains(command) {
            throw YOBROError.message("This command changes the user workspace and is unavailable in agent mode.")
        }
        agentSessionActive = true
        agentAction = command
        defer { agentAction = nil }
        let generation = agentGeneration
        if !["history", "downloads", "cancel-download"].contains(command) {
            updateAgentWorkspacePresentation(appIsActive: NSApp.isActive)
        }
        // These two return stored data without touching a tab, so they are the
        // only commands that can quietly exfiltrate in a single request. Any
        // process running as the same user can reach this socket, so they stay
        // closed until the user opts in.
        if ["history", "downloads"].contains(command) {
            guard bridgeLibraryAccess else {
                throw YOBROError.message(L("Verlauf und Downloads sind für die Agentenschnittstelle gesperrt. Aktiviere den Zugriff in den Einstellungen unter „Datenschutz & Werbung“.",
                                           "History and downloads are blocked for the agent interface. Enable access in Settings under “Privacy & ads”."))
            }
        }
        if command == "history" {
            let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .iso8601
            let limit = min(200, max(1, request["limit"] as? Int ?? 50))
            let data = try encoder.encode(Array(matchingHistory(request["query"] as? String ?? "").prefix(limit)))
            return ["history": try JSONSerialization.jsonObject(with: data)]
        }
        if command == "downloads" {
            let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .iso8601
            return ["downloads": try JSONSerialization.jsonObject(with: encoder.encode(downloads.entries)), "directory": downloads.directory.path]
        }
        if command == "cancel-download" {
            guard let value = request["id"] as? String, let id = UUID(uuidString: value), let item = downloads.entries.first(where: { $0.id == id }) else { throw YOBROError.message("Unbekannter Download.") }
            guard item.state == "downloading" else { throw YOBROError.message("Download ist nicht aktiv.") }
            downloads.cancel(id); record(command, item.name); return ["cancelRequested": value]
        }
        if command == "new" {
            let tab = newAgentTab()
            if let url = request["url"] as? String { try tab.navigate(url); try await waitForLoad(tab) }
            record(command, tab.webView.url?.host ?? L("Neuer Tab")); return tabInfo(tab)
        }
        if command == "split" { let tab = agentTab ?? newAgentTab(); return ["split": tab.id.uuidString.lowercased()] }
        // Only enabled for local development verification, never in normal launches.
        if command == "capture-window", ProcessInfo.processInfo.environment["YOBRO_DEV_CAPTURE"] == "1" {
            let window = NSApp.windows.compactMap(\.attachedSheet).first
                ?? NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil })
            guard let view = window?.contentView,
                  let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { throw YOBROError.message("Kein Fenster.") }
            view.cacheDisplay(in: view.bounds, to: bitmap)
            let destination = home.appendingPathComponent("window.png")
            try bitmap.representation(using: .png, properties: [:])?.write(to: destination)
            return ["path": destination.path]
        }
        let tab = try target(request)
        switch command {
        case "find":
            guard let query = request["query"] as? String else { throw YOBROError.message("Suchtext fehlt.") }
            try await waitForLoad(tab)
            tab.showFind = !query.isEmpty
            let found = await tab.find(query, backwards: request["backwards"] as? Bool ?? false)
            if let error = tab.findError { throw YOBROError.message(error) }
            record(command, tab.webView.url?.host ?? "Seitensuche")
            return ["tab": tabInfo(tab), "found": found, "query": query]
        case "duplicate":
            let duplicate = newAgentTab(url: tab.url)
            record(command, tab.title); return tabInfo(duplicate)
        case "download":
            guard let value = request["url"] as? String, let url = URL(string: value), ["https", "http"].contains(url.scheme?.lowercased() ?? ""), url.host != nil else { throw YOBROError.message("Gültige HTTP- oder HTTPS-Downloadadresse erforderlich.") }
            let download = await tab.webView.startDownload(using: URLRequest(url: url))
            downloads.track(download)
            record(command, url.host ?? "Datei")
            return ["started": true, "next": "Use downloads to check progress and retrieve the final file path."]
        case "focus": agentTabID = tab.id
        case "close": closeTab(tab.id)
        case "open":
            guard let rawURL = request["url"] as? String, let url = URL(string: rawURL) else { throw YOBROError.message("URL fehlt.") }
            return try await navigateTab(tab, url: url)
        case "back": tab.webView.goBack(); try await waitForLoad(tab)
        case "forward": tab.webView.goForward(); try await waitForLoad(tab)
        case "reload": tab.error = nil; tab.webView.reload(); try await waitForLoad(tab)
        case "pin": tab.pinned.toggle(); save()
        case "read":
            return try await readTab(tab)
        case "click", "fill", "press":
            guard let reference = request["ref"] as? String else {
                throw YOBROError.message("ref aus einem aktuellen read ist erforderlich.")
            }
            let document = request["document"] as? String ?? ""
            if command == "fill" && request["value"] as? String == nil { throw YOBROError.message("Wert fehlt.") }
            guard agentTabIDs.contains(tab.id), generation == agentGeneration else { throw YOBROError.message("Agent operation was paused.") }
            return try await actOnTab(tab, action: command, ref: reference, document: document, value: request["value"] as? String, key: request["key"] as? String)
        case "scroll":
            return try await scrollTab(tab, amount: request["amount"] as? Int ?? 600)
        default: throw YOBROError.message("Unbekannter Befehl: \(command)")
        }
        record(command, tab.webView.url?.host ?? tab.title, in: tab.space)
        return tabInfo(tab)
    }
}
