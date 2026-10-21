import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

@MainActor
final class PeekAndTopTabsTests: XCTestCase {
    func testPeekStaysOutOfTheSidebarUntilKept() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let page = model.newTab(url: "https://page.example.invalid/")
        model.peek(URL(string: "https://peek.example.invalid/")!)
        let peek = try XCTUnwrap(model.peekTab)
        XCTAssertFalse(model.tabs.contains { $0.id == peek.id })
        model.persistSession()
        let saved = try JSONDecoder().decode(StoredSession.self, from: Data(contentsOf: home.appendingPathComponent("session.json")))
        XCTAssertFalse(saved.tabs.contains { $0.id == peek.id })

        model.keepPeek(split: true)
        XCTAssertNil(model.peekTab)
        XCTAssertTrue(model.tabs.contains { $0.id == peek.id })
        XCTAssertTrue(model.splitPairs.contains { $0.contains(page.id) && $0.contains(peek.id) })

        model.peek(URL(string: "https://another.example.invalid/")!)
        let discarded = try XCTUnwrap(model.peekTab)
        model.switchSpace(model.spaces[1])
        XCTAssertNil(model.peekTab, "Changing Space closes the preview")
        XCTAssertFalse(model.tabs.contains { $0.id == discarded.id })
    }

    func testShiftClickOnALinkOpensPeek() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab()
        tab.url = "https://fixture.invalid/"
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 600, height: 400), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = tab.webView; window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        tab.webView.loadHTMLString("<html><body style='margin:0'><a id='link' href='https://peek.example.invalid/article' style='display:block;width:200px;height:60px'>Article</a></body></html>", baseURL: URL(string: "https://fixture.invalid/"))
        for _ in 0..<60 where tab.webView.isLoading || tab.webView.url == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        try await Task.sleep(nanoseconds: 200_000_000)
        // Scripted clicks lose their modifier keys in WebKit, so send real
        // mouse events with Shift held, as a person would.
        let web = tab.webView
        let point = web.convert(NSPoint(x: 100, y: web.isFlipped ? 30 : web.bounds.height - 30), to: nil)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: .shift, timestamp: ProcessInfo.processInfo.systemUptime,
                                                         windowNumber: window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1))
            // The test host is not the active app, so the window is never
            // key and would swallow a first click; deliver to the view.
            let target = try XCTUnwrap(window.contentView?.hitTest(point))
            if type == .leftMouseDown { target.mouseDown(with: event) } else { target.mouseUp(with: event) }
        }
        for _ in 0..<40 where model.peekTab == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertEqual(model.peekTab?.url, "https://peek.example.invalid/article")
        XCTAssertEqual(model.tabs.count, 1, "A preview never adds a tab by itself")
        model.closePeek()
    }

    func testTabsOnTopReplaceTheSidebar() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }; setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            UserDefaults.standard.removeObject(forKey: "YOBRO.tabsOnTop")
            UserDefaults.standard.removeObject(forKey: "YOBRO.showSidebar")
            try? FileManager.default.removeItem(at: home)
        }
        let model = BrowserModel(); model.showAgent = false; model.showSidebar = true
        model.tabsOnTop = true
        let tab = model.newTab()
        let origin = URL(string: "https://layout.test/")!
        tab.url = origin.absoluteString
        tab.webView.loadHTMLString("<html><body>Top tabs</body></html>", baseURL: origin)
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1200, height: 760), styleMask: [.titled, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 500_000_000)
        host.layoutSubtreeIfNeeded()
        let rect = tab.webView.convert(tab.webView.bounds, to: host)
        let top = host.isFlipped ? rect.minY : host.bounds.height - rect.maxY
        // Strip (6 + 28), gap (6), band (36) and its hairline sit above the page.
        XCTAssertEqual(top, 6 + 28 + 6 + 36 + 1, accuracy: 2)
        XCTAssertEqual(rect.minX, Chrome.inset, accuracy: 2, "No sidebar beside the page")
        XCTAssertEqual(tab.webView.bounds.width, 1200 - 2 * Chrome.inset, accuracy: 2)
    }
}
