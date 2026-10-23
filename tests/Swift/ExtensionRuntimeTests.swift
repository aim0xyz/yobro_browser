import CryptoKit
import XCTest
import WebKit
@testable import YOBRO

final class ExtensionRuntimeTests: XCTestCase {
    static let bundledBlockerPackage = YOBROResources.bundle.url(forResource: "uBlockOriginLite.safari", withExtension: "zip")

    /// scripts/prepare-bundled-ublock.py writes the package and its notice
    /// together; a hand-edited package or notice must not ship.
    @MainActor
    func testBundledBlockerNoticeMatchesPackage() async throws {
        guard #available(macOS 15.4, *) else { throw XCTSkip("Needs macOS 15.4") }
        let package = try XCTUnwrap(Self.bundledBlockerPackage)
        let noticeURL = try XCTUnwrap(YOBROResources.bundle.url(forResource: "uBlockOriginLite-NOTICE", withExtension: "txt"))
        let notice = try String(contentsOf: noticeURL, encoding: .utf8)
        let digest = SHA256.hash(data: try Data(contentsOf: package)).map { String(format: "%02x", $0) }.joined()
        XCTAssertTrue(notice.contains("Bundled SHA-256: \(digest)\n"), "Regenerate with scripts/prepare-bundled-ublock.py")
        let bundled = try await WKWebExtension(resourceBaseURL: package)
        let version = try XCTUnwrap(bundled.version)
        let upstream = version.split(separator: ".").dropLast().joined(separator: ".")
        XCTAssertTrue(notice.contains("YoBro compatibility version: \(version)\n"))
        XCTAssertTrue(notice.contains("Based on official Safari release: \(upstream)\n"))
        XCTAssertTrue(notice.contains("releases/tag/\(upstream)\n"))
    }

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
        let bundled = try await WKWebExtension(resourceBaseURL: XCTUnwrap(Self.bundledBlockerPackage))
        XCTAssertEqual(updated.id, old.id)
        XCTAssertEqual(updated.version, bundled.version)
        XCTAssertFalse(updated.enabled)
        XCTAssertEqual(updated.permissions, old.permissions)
        XCTAssertTrue(store.runtime.contexts.isEmpty)
        XCTAssertTrue(FileManager.default.fileExists(atPath: store.directory.appendingPathComponent(updated.file).path))
        let restored = ExtensionStore(home: home, websiteDataStore: .nonPersistent())
        XCTAssertEqual(restored.entries.first?.version, updated.version)
        XCTAssertFalse(try XCTUnwrap(restored.entries.first).enabled)
    }

    @MainActor
    func testPrivateTabsAreHiddenFromExtensions() async throws {
        guard #available(macOS 15.4, *) else { throw XCTSkip("Needs macOS 15.4") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let source = home.appendingPathComponent("fixture")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        let previousHome = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previousHome { setenv("YOBRO_HOME", previousHome, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: home)
        }
        try #"{"manifest_version":3,"name":"Tabs probe","version":"1.0"}"#.write(to: source.appendingPathComponent("manifest.json"), atomically: true, encoding: .utf8)
        let context = WKWebExtensionContext(for: try await WKWebExtension(resourceBaseURL: source))
        let model = BrowserModel(root: home)
        await model.extensionStartup?.value
        let runtime = model.extensions.runtime
        let ordinary = model.newTab()
        let hidden = model.newTab(isPrivate: true)
        let later = model.newTab()
        model.select(hidden.id)

        let listed = runtime.tabs(for: context).compactMap { $0 as? BrowserTab }
        XCTAssertEqual(listed.map(\.id), [ordinary.id, later.id])
        XCTAssertNil(runtime.activeTab(for: context))
        XCTAssertEqual(later.indexInWindow(for: context), 1, "Indices skip private tabs")
        model.select(ordinary.id)
        XCTAssertTrue(runtime.activeTab(for: context) as? BrowserTab === ordinary)
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

    /// uBOL answers YouTube's ad-block stall with a reload loop: it tags the
    /// next player request with a strategy, reloads the video, and moves on to
    /// the next strategy only when the tag came back in the player response.
    /// The 2026.914 rules tagged XMLHttpRequest bodies, which YouTube no longer
    /// sends, so the same reload repeated forever: a frozen picture whose time
    /// flipped between 0:00 and the full length. This stalled fake player
    /// serializes its request like YouTube; the loop must give up on its own.
    @MainActor
    func testBundledBlockerYouTubeStallRecoveryTerminates() async throws {
        guard #available(macOS 15.6, *) else { throw XCTSkip("Needs WebKit 18.6") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let store = ExtensionStore(home: home, websiteDataStore: .nonPersistent())
        await store.installBundledBlockerIfNeeded()
        let entry = try XCTUnwrap(store.entries.first, store.message ?? "Missing bundled blocker")
        let context = try XCTUnwrap(store.runtime.contexts[entry.id])
        try await store.runtime.waitForBundledBlocker(context)
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        store.configure(config)
        let page = WKWebView(frame: .zero, configuration: config)
        defer { page.stopLoading() }
        let userAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)"
        page.loadHTMLString("""
            <!doctype html><html><head><script>
            window.ytcfg = {data_: {INNERTUBE_CONTEXT: {client: {clientName: 'WEB', userAgent: '\(userAgent)'}}}};
            </script></head><body><div id="masthead" logo-type="YOUTUBE_LOGO"></div><div id="movie_player"></div>
            <script>if (window.ytcsi) { window.ytcsi.tick('js_ld', null, ''); }</script>
            <script>
            let response = {videoDetails: {videoId: 'fixture'}, playerConfig: {}, playbackTracking: {videostatsPlaybackUrl: {baseUrl: 'https://s.youtube.com/api/stats/playback'}}};
            window.reloads = 0;
            Object.assign(document.getElementById('movie_player'), {
              getPlayerResponse: () => response,
              getStatsForNerds: () => ({debug_info: 'SABR', buffer_health_seconds: '0.00 s', resolution: '0x0'}),
              getProgressState: () => ({current: 0, duration: 212, loaded: 0}),
              getPlayerStateObject: () => ({isBuffering: true}),
              seekTo() {},
              loadVideoById(videoId) {
                window.reloads++;
                const body = JSON.parse(JSON.stringify({videoId, attestationRequest: {},
                  context: {client: window.ytcfg.data_.INNERTUBE_CONTEXT.client},
                  playbackContext: {contentPlaybackContext: {referer: location.href}}}));
                response = {...response, playbackTracking: {videostatsPlaybackUrl: {baseUrl:
                  'https://s.youtube.com/api/stats/playback?referer=' + encodeURIComponent(body.playbackContext.contentPlaybackContext.referer)}}};
              },
            });
            </script></body></html>
            """, baseURL: URL(string: "https://www.youtube.com/watch?v=fixture"))
        // The initial blank document is already "complete"; wait for the fixture itself.
        var loaded = false
        for _ in 0..<100 where !loaded {
            loaded = (try? await page.evaluateJavaScript("document.readyState === 'complete' && typeof window.reloads === 'number'")) as? Bool == true
            if !loaded { try await Task.sleep(nanoseconds: 100_000_000) }
        }
        XCTAssertTrue(loaded, "The YouTube fixture did not load")
        // YouTube reports its ad-block stall through a snackbar message; each
        // DOM change runs the blocker's player check.
        func stallAgain(_ times: Int) async throws -> Int {
            for _ in 0..<times {
                _ = try await page.evaluateJavaScript("new Map().has('onSnackbarMessage'); document.body.append(document.createElement('i')); 0")
                try await Task.sleep(nanoseconds: 50_000_000)
            }
            let reloads = try await page.evaluateJavaScript("window.reloads") as? Int
            return try XCTUnwrap(reloads)
        }
        let reloads = try await stallAgain(12)
        XCTAssertGreaterThan(reloads, 0, "The bundled YouTube scriptlets must run on the fixture")
        let more = try await stallAgain(8)
        XCTAssertEqual(more, reloads, "Each strategy must be tried once; then the video is left alone")
        let agent = try await page.evaluateJavaScript("window.ytcfg.data_.INNERTUBE_CONTEXT.client.userAgent") as? String
        XCTAssertEqual(agent, userAgent, "Giving up must restore YouTube's own client identity")
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
