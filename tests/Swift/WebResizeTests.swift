import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

final class WebResizeTests: XCTestCase {
    @MainActor
    func testSidebarAndSplitKeepWebsitesInsideWindow() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }; setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(); model.showAgent = false
        let first = model.newTab()
        // TabContent renders NewTabPage for a tab without a URL, so the WebView
        // would never enter the view hierarchy and every measured frame would be
        // zero. Give the fixture a URL and load the markup against that origin.
        let origin = URL(string: "https://layout.test/")
        first.url = origin!.absoluteString
        first.webView.loadHTMLString("<html><body style='margin:0'><div style='width:100%'>Test</div></body></html>", baseURL: origin)
        let host = NSHostingView(rootView: BrowserShell(model:model))
        let window = NSWindow(contentRect:NSRect(x:0,y:0,width:1360,height:880),styleMask:[.titled,.resizable,.fullSizeContentView],backing:.buffered,defer:false)
        window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds:400_000_000)
        var wide: CGFloat = 0
        for (width, sidebar) in [(1360.0,true),(960.0,true),(960.0,false)] {
            model.showSidebar = sidebar; window.setContentSize(NSSize(width:width,height:700)); host.layoutSubtreeIfNeeded()
            try await Task.sleep(nanoseconds:350_000_000)
            let rect = first.webView.convert(first.webView.bounds, to:host)
            XCTAssertEqual(rect.minY, 10, accuracy: 2)
            XCTAssertEqual(rect.maxY, host.bounds.height - 10, accuracy: 2)
            XCTAssertLessThanOrEqual(rect.maxX, host.bounds.width + 2)
            XCTAssertEqual(first.webView.bounds.width, width - (sidebar ? 248 : 80) - 20, accuracy:2)
            if width == 1360 { wide = first.webView.bounds.width } else { XCTAssertLessThan(first.webView.bounds.width, wide) }
        }
        let second = model.newTab()
        second.url = origin!.absoluteString
        second.webView.loadHTMLString("<html><body>Second</body></html>", baseURL: origin)
        model.pairTabs(second.id, with:first.id)
        try await Task.sleep(nanoseconds:400_000_000)
        for tab in [first,second] {
            let rect = tab.webView.convert(tab.webView.bounds,to:host)
            XCTAssertLessThanOrEqual(rect.maxX, host.bounds.width + 2)
            XCTAssertLessThan(tab.webView.bounds.width, 600)
            XCTAssertGreaterThan(tab.webView.bounds.width, 180)
        }
    }
    @MainActor
    func testViewportTracksAvailableWidthAndHeight() async throws {
        let web = WKWebView(frame: .zero)
        let host = NSHostingView(rootView: WebSurface(webView: web))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 700), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        web.loadHTMLString("<html><head><style>body{margin:0}main{width:100%;height:1500px}@media(max-width:600px){body{--compact:yes}}</style></head><body><main>Responsive fixture</main></body></html>", baseURL: nil)
        for _ in 0..<50 { if !web.isLoading, web.url != nil { break }; try await Task.sleep(nanoseconds: 50_000_000) }
        for size in [NSSize(width:1100,height:700), NSSize(width:450,height:500), NSSize(width:900,height:650)] {
            window.setContentSize(size); host.layoutSubtreeIfNeeded()
            try await Task.sleep(nanoseconds: 200_000_000)
            let width = try await web.evaluateJavaScript("window.innerWidth") as! Double
            let height = try await web.evaluateJavaScript("window.innerHeight") as! Double
            XCTAssertEqual(web.bounds.width, size.width, accuracy: 2)
            XCTAssertEqual(width, size.width, accuracy: 2)
            XCTAssertEqual(height, size.height, accuracy: 2)
            let compact = try await web.evaluateJavaScript("matchMedia('(max-width:600px)').matches") as! Bool
            XCTAssertEqual(compact, size.width <= 600)
        }
    }
}
