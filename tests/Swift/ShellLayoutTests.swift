import XCTest
import SwiftUI
@testable import YOBRO

final class ShellLayoutTests: XCTestCase {
    func testOnlyPlainHTTPToPublicHostsIsMarkedInsecure() {
        XCTAssertTrue(PageSecurity.isInsecure(url: "http://example.com"))
        XCTAssertTrue(PageSecurity.isInsecure(url: "HTTP://Example.com/path?q=1"))
        for url in ["https://example.com", "http://localhost:3000", "http://192.168.1.2", "http://foo.local/",
                    "", "about:blank", ExtensionCatalog.internalURL] {
            XCTAssertFalse(PageSecurity.isInsecure(url: url), url)
        }
    }

    @MainActor
    func testInsecureIndicatorFollowsTheTab() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let model = BrowserModel(root: home)
        defer { try? FileManager.default.removeItem(at: home) }
        let tab = model.newTab()
        XCTAssertFalse(PageSecurity.isInsecure(nil))
        XCTAssertFalse(PageSecurity.isInsecure(tab), "A blank tab has no address to judge")
        tab.url = "http://example.com/"
        XCTAssertTrue(PageSecurity.isInsecure(tab))
        tab.url = "https://example.com/"
        XCTAssertFalse(PageSecurity.isInsecure(tab))
        tab.url = ExtensionCatalog.internalURL
        XCTAssertFalse(PageSecurity.isInsecure(tab))
    }

    @MainActor
    func testHiddenTabStripFloatsOverThePage() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer {
            UserDefaults.standard.removeObject(forKey: "YOBRO.tabsOnTop")
            UserDefaults.standard.removeObject(forKey: "YOBRO.showSidebar")
            try? FileManager.default.removeItem(at: home)
        }
        let model = BrowserModel(root: home); model.showAgent = false
        model.tabsOnTop = true; model.showSidebar = false
        let tab = model.newTab()
        let origin = URL(string: "https://layout.test/")!
        tab.url = origin.absoluteString
        tab.webView.loadHTMLString("<html><body>Hidden strip</body></html>", baseURL: origin)
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 700), styleMask: [.titled, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        for revealed in [false, true, false] {
            model.sidebarOverlayVisible = revealed
            try await Task.sleep(nanoseconds: 400_000_000)
            host.layoutSubtreeIfNeeded()
            let rect = tab.webView.convert(tab.webView.bounds, to: host)
            let top = host.isFlipped ? rect.minY : host.bounds.height - rect.maxY
            // Inset, the page band (36) and its hairline; the strip never pushes the page.
            XCTAssertEqual(top, Chrome.inset + 36 + 1, accuracy: 2, "revealed: \(revealed)")
            XCTAssertEqual(tab.webView.bounds.width, 1100 - 2 * Chrome.inset, accuracy: 2)
        }
    }

    func testPinDropsGoBeforeOrAfterAndNeverSplit() {
        XCTAssertEqual(PinDropIntent.resolve(locationX: 10, width: 50), .before)
        XCTAssertEqual(PinDropIntent.resolve(locationX: 40, width: 50), .after)
        XCTAssertEqual(PinDropIntent.resolve(locationX: 25, width: 50), .after)
        for width in [36.0, 45, 50, 60] {
            for x in stride(from: -5.0, through: width + 5, by: 0.5) {
                XCTAssertNotEqual(PinDropIntent.resolve(locationX: x, width: width), .split)
            }
        }
    }
}
