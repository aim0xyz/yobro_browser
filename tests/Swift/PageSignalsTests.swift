import XCTest
import WebKit
@testable import YOBRO

@MainActor
final class PageSignalsTests: XCTestCase {
    private var windows: [NSWindow] = []
    private var homes: [URL] = []

    override func tearDown() async throws {
        windows.forEach { $0.close() }
        windows.removeAll()
        homes.forEach { try? FileManager.default.removeItem(at: $0) }
        homes.removeAll()
    }

    /// A tab-like web view, set up through the same path as every browser tab.
    private func makeWebView(_ html: String, configure: (WKWebViewConfiguration) -> Void = { _ in }) async throws -> AppearanceWebView {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        homes.append(home)
        let config = WKWebViewConfiguration()
        configure(config)
        WebAppearance(home: home).install(on: config.userContentController)
        let web = AppearanceWebView(frame: NSRect(x: 0, y: 0, width: 800, height: 600), configuration: config)
        let window = NSWindow(contentRect: web.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = web
        window.orderFront(nil)
        windows.append(window)
        _ = web.signals
        web.loadHTMLString(html, baseURL: URL(string: "https://example.com/"))
        try await waitUntil {
            (try? await web.evaluateJavaScript("location.href === 'https://example.com/' && document.readyState === 'complete'")) as? Bool == true
        }
        return web
    }

    private func waitUntil(timeout: TimeInterval = 5, _ condition: () async -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if await condition() { return }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTFail("Timed out")
    }

    func testFocusedLinkIsReportedAndClearedButSyntheticHoverIsIgnored() async throws {
        let web = try await makeWebView("<p><a id=link href='/next?page=2'>Next</a> <button id=other>Other</button></p>")
        // A page cannot fake a hover to put an address in the status pill.
        _ = try await web.evaluateJavaScript("document.getElementById('link').dispatchEvent(new MouseEvent('mouseover', {bubbles: true})); true")
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(web.signals.hoveredLink, "")

        _ = try await web.evaluateJavaScript("document.getElementById('link').focus(); true")
        try await waitUntil { web.signals.hoveredLink == "https://example.com/next?page=2" }
        _ = try await web.evaluateJavaScript("document.getElementById('other').focus(); true")
        try await waitUntil { web.signals.hoveredLink.isEmpty }

        // Leaving the page clears a link that is still focused.
        _ = try await web.evaluateJavaScript("document.getElementById('link').focus(); true")
        try await waitUntil { !web.signals.hoveredLink.isEmpty }
        web.loadHTMLString("<p>Next page</p>", baseURL: URL(string: "https://example.com/next"))
        try await waitUntil { web.signals.hoveredLink.isEmpty }
    }

    func testMutedPlaybackIsSilentAndUnmutedPlaybackIsAudible() async throws {
        let web = try await makeWebView("<video id=v loop muted playsinline></video>") { config in
            config.mediaTypesRequiringUserActionForPlayback = []
        }
        // One second of 8-bit silence, so the test makes no sound.
        _ = try await web.evaluateJavaScript("""
            const samples = 8000, bytes = new Uint8Array(44 + samples);
            const view = new DataView(bytes.buffer);
            const text = (offset, value) => [...value].forEach((c, i) => bytes[offset + i] = c.charCodeAt(0));
            text(0, 'RIFF'); view.setUint32(4, 36 + samples, true); text(8, 'WAVEfmt ');
            view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
            view.setUint32(24, 8000, true); view.setUint32(28, 8000, true); view.setUint16(32, 1, true); view.setUint16(34, 8, true);
            text(36, 'data'); view.setUint32(40, samples, true); bytes.fill(128, 44);
            const video = document.getElementById('v');
            video.src = 'data:audio/wav;base64,' + btoa(String.fromCharCode(...bytes));
            video.play();
            true
            """)
        try await waitUntil { (try? await web.evaluateJavaScript("!document.getElementById('v').paused && document.getElementById('v').currentTime > 0")) as? Bool == true }
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertFalse(web.signals.audible)

        _ = try await web.evaluateJavaScript("document.getElementById('v').muted = false; true")
        try await waitUntil { web.signals.audible }
        _ = try await web.evaluateJavaScript("document.getElementById('v').pause(); true")
        try await waitUntil { !web.signals.audible }
    }

    func testMessagesAreValidatedAndOnlyReachBrowserWebViews() {
        let handler = PageSignalsHandler.shared
        let web = AppearanceWebView(frame: .zero, configuration: WKWebViewConfiguration())
        // Neither a foreign web view nor a missing one has signals to update.
        handler.route(["kind": "link", "href": "https://example.com/"], from: WKWebView())
        handler.route(["kind": "link", "href": "https://example.com/"], from: nil)
        XCTAssertEqual(web.signals.hoveredLink, "")

        handler.route(["kind": "link", "href": "https://example.com/a"], from: web)
        XCTAssertEqual(web.signals.hoveredLink, "https://example.com/a")
        handler.route(["kind": "link", "href": 42], from: web)
        handler.route("https://example.com/b", from: web)
        XCTAssertEqual(web.signals.hoveredLink, "https://example.com/a")
        handler.route(["kind": "link", "href": "JavaScript:alert(1)"], from: web)
        XCTAssertEqual(web.signals.hoveredLink, "")
        handler.route(["kind": "link", "href": "https://example.com/" + String(repeating: "x", count: 5000)], from: web)
        XCTAssertEqual(web.signals.hoveredLink.count, PageSignalsHandler.linkLimit)

        handler.route(["kind": "audio", "doc": "", "audible": true], from: web)
        handler.route(["kind": "audio", "doc": "frame", "audible": "yes"], from: web)
        XCTAssertFalse(web.signals.audible)
        handler.route(["kind": "audio", "doc": "main", "audible": true], from: web)
        handler.route(["kind": "audio", "doc": "frame", "audible": true], from: web)
        handler.route(["kind": "audio", "doc": "main", "audible": false], from: web)
        XCTAssertTrue(web.signals.audible)
        handler.route(["kind": "audio", "doc": "frame", "audible": false], from: web)
        XCTAssertFalse(web.signals.audible)
    }

    func testInstallingTwiceKeepsOneScriptAndHandler() {
        let controller = WKUserContentController()
        PageSignalsScript.install(on: controller)
        PageSignalsScript.install(on: controller)
        XCTAssertEqual(controller.userScripts.filter { $0.source.contains("yobroSignals") }.count, 1)
    }
}
