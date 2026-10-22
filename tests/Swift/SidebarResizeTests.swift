import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

@MainActor
final class SidebarResizeTests: XCTestCase {
    override func tearDown() {
        UserDefaults.standard.removeObject(forKey: "YOBRO.sidebarWidth")
        UserDefaults.standard.removeObject(forKey: "YOBRO.showSidebar")
        super.tearDown()
    }

    func testSidebarWidthIsClampedAndRemembered() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        XCTAssertEqual(model.sidebarWidth, Chrome.sidebarWidth)
        model.setSidebarWidth(331.6)
        XCTAssertEqual(model.sidebarWidth, 332)
        XCTAssertEqual(BrowserModel(root: home).sidebarWidth, 332, "The width is remembered")
        model.setSidebarWidth(40)
        XCTAssertEqual(model.sidebarWidth, BrowserModel.sidebarWidths.lowerBound)
        model.setSidebarWidth(2000)
        XCTAssertEqual(model.sidebarWidth, BrowserModel.sidebarWidths.upperBound)
        UserDefaults.standard.set(9999.0, forKey: "YOBRO.sidebarWidth")
        XCTAssertEqual(BrowserModel(root: home).sidebarWidth, BrowserModel.sidebarWidths.upperBound, "A stored value outside the range is clamped")
    }

    func testStripTabZonesReorderAtTheEdgesAndSplitInTheMiddle() {
        // The strip reads the horizontal position with the same zones as a row.
        XCTAssertEqual(TabDropIntent.resolve(locationY: 10, rowHeight: 120), .before)
        XCTAssertEqual(TabDropIntent.resolve(locationY: 60, rowHeight: 120), .split)
        XCTAssertEqual(TabDropIntent.resolve(locationY: 110, rowHeight: 120), .after)
    }

    func testPageFollowsTheSidebarWidthAndFloatingFreesTheSpace() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }; setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(); model.showAgent = false; model.showSidebar = true
        let tab = model.newTab()
        let origin = URL(string: "https://layout.test/")!
        tab.url = origin.absoluteString
        tab.webView.loadHTMLString("<html><body>Layout</body></html>", baseURL: origin)
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1200, height: 760), styleMask: [.titled, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        for width in [CGFloat(200), 360] {
            model.setSidebarWidth(width)
            try await Task.sleep(nanoseconds: 350_000_000)
            host.layoutSubtreeIfNeeded()
            XCTAssertEqual(tab.webView.bounds.width, 1200 - width - Chrome.inset, accuracy: 2, "sidebar \(width)")
        }
        // Floating: the page takes the whole width; the sidebar keeps its size for the overlay.
        model.showSidebar = false
        try await Task.sleep(nanoseconds: 350_000_000)
        host.layoutSubtreeIfNeeded()
        XCTAssertEqual(tab.webView.bounds.width, 1200 - 2 * Chrome.inset, accuracy: 2)
        XCTAssertEqual(model.sidebarWidth, 360)
        // The page steps aside for the whole floating sidebar, however wide:
        // a fixed 260 pt let the page take presses on a wide sidebar's rows.
        model.sidebarOverlayVisible = true
        let pageHost = try XCTUnwrap(tab.webView.superview)
        XCTAssertNil(tab.webView.hitTest(pageHost.convert(NSPoint(x: 350, y: 300), from: nil)))
        XCTAssertNotNil(tab.webView.hitTest(pageHost.convert(NSPoint(x: 600, y: 300), from: nil)))
    }
}
