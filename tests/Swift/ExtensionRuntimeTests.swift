import XCTest
import WebKit
@testable import YOBRO

final class ExtensionRuntimeTests: XCTestCase {
    @MainActor
    func testBundledUpgradePreservesDisabledStateAndIdentity() async throws {
        guard #available(macOS 15.6, *) else { throw XCTSkip("Needs WebKit 18.6") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let store = ExtensionStore(home: home, websiteDataStore: .nonPersistent())
        let old = InstalledExtension(id: ExtensionStore.bundledBlockerID, name: "uBlock Origin Lite", version: "2026.914.1325", file: "previous.zip", enabled: false, permissions: ["storage"], sites: ["<all_urls>"])
        store.entries = [old]
        try store.save()
        try Data(old.version.utf8).write(to: store.directory.appendingPathComponent("bundled-blocker-installed"))
        await store.installBundledBlockerIfNeeded()
        let updated = try XCTUnwrap(store.entries.first, store.message ?? "No entry")
        XCTAssertEqual(updated.id, old.id)
        XCTAssertEqual(updated.version, "2026.914.1325.1")
        XCTAssertFalse(updated.enabled)
        XCTAssertEqual(updated.permissions, old.permissions)
        XCTAssertTrue(store.runtime.contexts.isEmpty)
        XCTAssertTrue(FileManager.default.fileExists(atPath: store.directory.appendingPathComponent(updated.file).path))
        let restored = ExtensionStore(home: home, websiteDataStore: .nonPersistent())
        XCTAssertEqual(restored.entries.first?.version, updated.version)
        XCTAssertFalse(try XCTUnwrap(restored.entries.first).enabled)
    }

    @MainActor
    func testActionPanelIsArrowlessAndStaysAtTopWhenResized() async throws {
        guard #available(macOS 15.4, *) else { throw XCTSkip("Needs macOS 15.4") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        try #"{"manifest_version":3,"name":"Popup fixture","version":"1","action":{"default_popup":"popup.html"}}"#.write(to: home.appendingPathComponent("manifest.json"), atomically: true, encoding: .utf8)
        try "<html><body style='width:300px;height:200px;margin:0'>Popup fixture</body></html>".write(to: home.appendingPathComponent("popup.html"), atomically: true, encoding: .utf8)
        let ext = try await WKWebExtension(resourceBaseURL: home)
        let context = WKWebExtensionContext(for: ext)
        let controller = WKWebExtensionController(configuration: .nonPersistent())
        try controller.load(context)
        defer { try? controller.unload(context) }
        let action = try XCTUnwrap(context.action(for: nil))
        let window = NSWindow(contentRect: NSRect(x: 100, y: 100, width: 900, height: 650), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let anchor = NSView(frame: NSRect(x: 0, y: 0, width: 900, height: 650))
        window.contentView = anchor
        window.orderFront(nil)
        defer { window.close() }
        let surface = try XCTUnwrap(ExtensionActionPanel(action: action, anchor: anchor))
        var closed = false
        surface.onDismiss = { closed = true }
        surface.show()
        defer { surface.dismiss() }
        for _ in 0..<30 {
            if (try? await action.popupWebView?.evaluateJavaScript("document.body.innerText")) as? String == "Popup fixture" { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let text = try await action.popupWebView?.evaluateJavaScript("document.body.innerText") as? String
        XCTAssertEqual(text, "Popup fixture")
        try await Task.sleep(nanoseconds: 300_000_000)
        let panel = try XCTUnwrap(window.childWindows?.first)
        XCTAssertEqual(panel.styleMask, .borderless)
        let page = window.convertToScreen(anchor.bounds).intersection(try XCTUnwrap(window.screen).visibleFrame)
        XCTAssertEqual(panel.frame.maxY, page.maxY - 12, accuracy: 1)
        XCTAssertEqual(panel.frame.maxX, page.maxX - 12, accuracy: 1)
        _ = try await action.popupWebView?.evaluateJavaScript("document.body.style.width='400px';document.body.style.height='350px'")
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertEqual(panel.frame.width, 400, accuracy: 1)
        XCTAssertEqual(panel.frame.maxY, page.maxY - 12, accuracy: 1)
        XCTAssertEqual(panel.frame.maxX, page.maxX - 12, accuracy: 1)
        surface.dismiss()
        XCTAssertTrue(closed)
        XCTAssertFalse(panel.isVisible)
        XCTAssertTrue(window.childWindows?.isEmpty ?? true)
        // WebKit closes the page asynchronously; keep its controller alive
        // until those IPC callbacks have drained before tearing down the fixture.
        try await Task.sleep(nanoseconds: 500_000_000)
    }

    @MainActor
    func testBundledSafariBlockerLoadsAndRespectsRemoval() async throws {
        guard #available(macOS 15.6, *) else { throw XCTSkip("Needs WebKit 18.6") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let store = ExtensionStore(home: home, websiteDataStore: .nonPersistent())
        await store.installBundledBlockerIfNeeded()
        let entry = try XCTUnwrap(store.entries.first, store.message ?? "Missing bundled blocker")
        XCTAssertEqual(entry.id, ExtensionStore.bundledBlockerID)
        XCTAssertTrue(store.runtime.contexts[entry.id]?.isLoaded == true)
        await store.toggle(entry)
        await store.installBundledBlockerIfNeeded()
        XCTAssertFalse(store.entries[0].enabled)
        await store.toggle(store.entries[0])
        XCTAssertTrue(store.entries[0].enabled, store.errors[entry.id] ?? "Re-enable failed")
        let restarted = try XCTUnwrap(store.runtime.contexts[entry.id])
        XCTAssertEqual(restarted.baseURL.scheme, "webkit-extension")
        try await store.runtime.waitForBundledBlocker(restarted)
        XCTAssertTrue(restarted.hasInjectedContent(for: URL(string: "https://www.youtube.com/watch?v=fixture")!),
                      "YouTube must have registered script filters after a restart, not only network rules")
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        store.configure(config)
        let page = WKWebView(frame: .zero, configuration: config)
        page.loadHTMLString("<html><body><script>window.ytInitialPlayerResponse = {adPlacements: [1], playerAds: [1], adSlots: [1], videoDetails: {title: 'fixture'}};</script></body></html>", baseURL: URL(string: "https://www.youtube.com/"))
        for _ in 0..<50 {
            if (try? await page.evaluateJavaScript("window.ytInitialPlayerResponse?.videoDetails?.title")) as? String == "fixture" { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let fixtureTitle = try await page.evaluateJavaScript("window.ytInitialPlayerResponse?.videoDetails?.title") as? String
        XCTAssertEqual(fixtureTitle, "fixture")
        let hasAds = try await page.evaluateJavaScript("Boolean(window.ytInitialPlayerResponse?.adPlacements?.length)") as? Bool
        XCTAssertEqual(hasAds, false, "YouTube scriptlets must actually intercept page-world player data")
        page.stopLoading()
        store.remove(store.entries[0])
        await store.installBundledBlockerIfNeeded()
        XCTAssertTrue(store.entries.isEmpty, "An explicitly removed default must not reinstall itself")
        await store.restoreBundledBlocker()
        XCTAssertEqual(store.entries.first?.id, ExtensionStore.bundledBlockerID)
        XCTAssertTrue(store.entries.first?.enabled == true)
        XCTAssertTrue(store.runtime.contexts[ExtensionStore.bundledBlockerID]?.isLoaded == true)
        XCTAssertFalse(store.busy)
    }

    @MainActor
    func testInstallInjectDisableAndRestore() async throws {
        guard #available(macOS 15.4, *) else { throw XCTSkip("Needs macOS 15.4") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let source = home.appendingPathComponent("fixture")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let manifest = """
        {"manifest_version":3,"name":"YOBRO Test Extension","version":"1.0","description":"Synthetic extension integration test","permissions":["storage","tabs"],"background":{"service_worker":"background.js"},"host_permissions":["http://127.0.0.1/*"],"content_scripts":[{"matches":["http://127.0.0.1/*"],"js":["content.js"],"run_at":"document_end"}],"action":{"default_popup":"popup.html"}}
        """
        try manifest.write(to: source.appendingPathComponent("manifest.json"), atomically: true, encoding: .utf8)
        try "browser.runtime.sendMessage({test:true}).then(v => document.body.setAttribute('data-yobro-extension', v));".write(to: source.appendingPathComponent("content.js"), atomically: true, encoding: .utf8)
        try "<html><body>Test popup</body></html>".write(to: source.appendingPathComponent("popup.html"), atomically: true, encoding: .utf8)
        try "<html><head><title>Extension review</title></head><body>Extension review</body></html>".write(to: source.appendingPathComponent("review.html"), atomically: true, encoding: .utf8)
        try "browser.runtime.onMessage.addListener((message,sender,reply) => { Promise.all([browser.storage.local.set({test:'ok'}),browser.tabs.query({active:true,currentWindow:true})]).then(async ([_,tabs]) => { const data=await browser.storage.local.get('test'); if (!tabs.some(t => t.url.startsWith('http://127.0.0.1:'))) { reply('tabs-missing'); return; } await browser.tabs.create({url:browser.runtime.getURL('review.html?first')}); await browser.tabs.create({url:browser.runtime.getURL('review.html?second')}); reply(data.test); }); return true; });".write(to: source.appendingPathComponent("background.js"), atomically: true, encoding: .utf8)
        let previousHome = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer { if let previousHome { setenv("YOBRO_HOME", previousHome, 1) } else { unsetenv("YOBRO_HOME") } }
        let model = BrowserModel()
        let server = Process(); server.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        server.arguments = ["python3", "-u", "-c", "from http.server import HTTPServer,BaseHTTPRequestHandler\nclass H(BaseHTTPRequestHandler):\n def do_GET(self):\n  body=b'<html><body>Fixture</body></html>';self.send_response(200);self.send_header('Content-Type','text/html');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)\n def log_message(self,*args): pass\ns=HTTPServer(('127.0.0.1',0),H);print(s.server_port,flush=True);s.serve_forever()"]
        let output = Pipe(); server.standardOutput = output; server.standardError = FileHandle.nullDevice
        try server.run(); defer { server.terminate() }
        var line = Data()
        while true { let byte = output.fileHandleForReading.readData(ofLength: 1); if byte.isEmpty || byte == Data([10]) { break }; line.append(byte) }
        let port = try XCTUnwrap(Int(String(decoding: line, as: UTF8.self)))
        let store = model.extensions
        await store.start(owner: model)
        await store.prepare(source)
        let pending = try XCTUnwrap(store.pending, store.message ?? "No pending extension")
        XCTAssertEqual(pending.name, "YOBRO Test Extension")
        XCTAssertTrue(store.runtime.contexts.isEmpty, "Preparing must not execute the extension")
        await store.installPending()
        let context = try XCTUnwrap(store.runtime.contexts[pending.id], store.message ?? "Not installed")
        XCTAssertTrue(context.isLoaded)
        context.hasAccessToPrivateData = true // The test uses an ephemeral WKWebsiteDataStore.
        XCTAssertTrue(context.action(for: nil)?.presentsPopup == true)
        let webView = model.newTab(url: "http://127.0.0.1:\(port)/").webView
        var observed: String?
        for _ in 0..<50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            observed = try? await webView.evaluateJavaScript("document.body.getAttribute('data-yobro-extension')") as? String
            if observed == "ok" { break }
        }
        XCTAssertEqual(observed, "ok", "Content scripts and storage must actually execute: \(context.errors)")
        var reviews: [BrowserTab] = []
        for _ in 0..<50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            reviews = model.tabs.filter { $0.webView.url?.path == "/review.html" }
            if reviews.count == 2, reviews.allSatisfy({ $0.webView.title == "Extension review" }) { break }
        }
        XCTAssertEqual(reviews.count, 2, "Multiple browser.tabs.create calls must not reuse YOBRO's script message handlers")
        XCTAssertTrue(reviews.allSatisfy { $0.webView.title == "Extension review" }, "browser.tabs.create must open extension-owned pages with the context-specific WKWebView configuration")
        XCTAssertTrue(reviews.allSatisfy { $0.error == nil })
        await store.toggle(store.entries[0])
        XCTAssertFalse(context.isLoaded)
        XCTAssertFalse(store.entries[0].enabled)
        await store.toggle(store.entries[0])
        XCTAssertTrue(store.runtime.contexts[pending.id]?.isLoaded == true)
        let restored = ExtensionStore(home: home)
        XCTAssertEqual(restored.entries.first?.id, pending.id)
        store.remove(store.entries[0])
        XCTAssertTrue(store.entries.isEmpty)
    }
}
