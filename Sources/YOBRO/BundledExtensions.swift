import Foundation
import WebKit

extension ExtensionStore {
    static let bundledBlockerID = UUID(uuidString: "FC650123-334F-4B10-8C9B-017C5230B010")!

    /// An explicit user action can restore a previously removed bundled blocker.
    func restoreBundledBlocker() async {
        guard !busy, pending == nil, !entries.contains(where: { $0.id == Self.bundledBlockerID }) else { return }
        busy = true
        defer { busy = false }
        do {
            let marker = directory.appendingPathComponent("bundled-blocker-installed")
            if FileManager.default.fileExists(atPath: marker.path) { try FileManager.default.removeItem(at: marker) }
            message = nil
            await installBundledBlockerIfNeeded()
        } catch { message = error.localizedDescription }
    }

    /// Install once and upgrade bundled releases; preserve identity, settings, and opt-outs.
    func installBundledBlockerIfNeeded() async {
        guard !bundledBlockerInstalling else { return }
        bundledBlockerInstalling = true
        let previousBusy = busy
        busy = true
        defer { busy = previousBusy; bundledBlockerInstalling = false }
        guard #available(macOS 15.6, *) else {
            message = L("uBlock Origin Lite benötigt macOS 15.6 oder neuer.", "uBlock Origin Lite requires macOS 15.6 or later.")
            return
        }
        let marker = directory.appendingPathComponent("bundled-blocker-installed")
        let previous = entries.first { $0.id == Self.bundledBlockerID }
        // A marker without an entry means the user explicitly removed it.
        if previous == nil && FileManager.default.fileExists(atPath: marker.path) { return }
        do {
            guard let source = Bundle.module.url(forResource: "uBlockOriginLite.safari", withExtension: "zip") else {
                throw YOBROError.message("Bundled uBlock Origin Lite package is missing.")
            }
            let ext = try await WKWebExtension(resourceBaseURL: source)
            let version = ext.version ?? ""
            if let previous, previous.version.compare(version, options: .numeric) != .orderedAscending { return }
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let file = "uBlockOriginLite-\(version).safari.zip"
            let target = directory.appendingPathComponent(file)
            try Data(contentsOf: source).write(to: target, options: .atomic)
            let installed = try await WKWebExtension(resourceBaseURL: target)
            let entry = InstalledExtension(id: Self.bundledBlockerID, name: installed.displayName ?? "uBlock Origin Lite", version: version,
                file: file, enabled: previous?.enabled ?? true,
                permissions: previous?.permissions ?? installed.requestedPermissions.map(\.rawValue),
                sites: previous?.sites ?? installed.allRequestedMatchPatterns.map(\.string))
            let savedEntries = entries
            let wasLoaded = runtime.contexts[entry.id] != nil
            do {
                if wasLoaded { try runtime.unload(entry.id) }
                if entry.enabled { try await runtime.load(entry, extension: installed) }
                if let index = entries.firstIndex(where: { $0.id == entry.id }) { entries[index] = entry }
                else { entries.append(entry) }
                try save()
            } catch {
                entries = savedEntries
                try? runtime.unload(entry.id)
                if wasLoaded, let previous { try? await runtime.load(previous, from: directory.appendingPathComponent(previous.file)) }
                try? FileManager.default.removeItem(at: target)
                throw error
            }
            try Data(version.utf8).write(to: marker, options: .atomic)
        } catch {
            message = L("uBlock Origin Lite konnte nicht gestartet werden: ", "Could not start uBlock Origin Lite: ") + error.localizedDescription
        }
    }
}

@available(macOS 15.6, *)
extension ExtensionRuntime {
    /// Loading a background document does not await uBOL's asynchronous filter
    /// registration. Its diagnostic message explicitly waits for initialization.
    func waitForBundledBlocker(_ context: WKWebExtensionContext) async throws {
        guard let configuration = context.webViewConfiguration else {
            throw YOBROError.message("uBlock Origin Lite: missing extension configuration")
        }
        let view = WKWebView(frame: .zero, configuration: configuration)
        defer { view.stopLoading() }
        let url = context.baseURL.appendingPathComponent("web_accessible_resources/noop.html")
        view.load(URLRequest(url: url))
        for _ in 0..<50 {
            if !view.isLoading, view.url == url,
               (try? await view.evaluateJavaScript("typeof browser?.runtime?.sendMessage === 'function'")) as? Bool == true {
                let ready = try await view.callAsyncJavaScript("""
                    const { webextFlavor } = await import(browser.runtime.getURL('js/ext.js'));
                    if (webextFlavor !== 'safari') throw new Error('uBlock selected the wrong browser engine');
                    return await Promise.race([
                        browser.runtime.sendMessage({what: 'getRegisteredContentScripts'}).then(async scripts => ({
                            ready: Array.isArray(scripts), flavor: webextFlavor,
                            scripts: scripts?.length ?? 0,
                            sessionRules: (await browser.declarativeNetRequest.getSessionRules()).length,
                            dynamicRules: (await browser.declarativeNetRequest.getDynamicRules()).length
                        })),
                        new Promise((_, reject) => setTimeout(() => reject(new Error('uBlock initialization timed out')), 10000))
                    ]);
                    """, arguments: [:], in: nil, contentWorld: .page)
                guard let summary = ready as? [String: Any], summary["ready"] as? Bool == true else {
                    throw YOBROError.message("uBlock Origin Lite did not confirm filter initialization")
                }
                blockerReadinessSummary = summary
                return
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw YOBROError.message("uBlock Origin Lite readiness page did not load")
    }
}
