import XCTest
import AppKit
import WebKit
@testable import YOBRO

@MainActor
final class SplitFocusTests: XCTestCase {
    func testPageClicksTargetReloadWithoutReorderingOrTakingAgentFocus() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: home)
        }
        let server = Process()
        server.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        server.arguments = ["python3", "-u", "-c", """
        from http.server import HTTPServer, BaseHTTPRequestHandler
        counts = {}
        class H(BaseHTTPRequestHandler):
            def do_GET(self):
                counts[self.path] = counts.get(self.path, 0) + 1
                body = ('<html><head><title>' + self.path + ':' + str(counts[self.path]) + '</title></head><body><input id="field" value="unchanged"><p>Click this page</p></body></html>').encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *args): pass
        server = HTTPServer(('127.0.0.1', 0), H)
        print(server.server_port, flush=True)
        server.serve_forever()
        """]
        let output = Pipe(); server.standardOutput = output; server.standardError = FileHandle.nullDevice
        try server.run()
        defer { server.terminate() }
        var line = Data()
        while true {
            let byte = output.fileHandleForReading.readData(ofLength: 1)
            if byte.isEmpty || byte == Data([10]) { break }
            line.append(byte)
        }
        let port = try XCTUnwrap(Int(String(decoding: line, as: UTF8.self)))
        let model = BrowserModel(root: home)
        await model.webStartupTask?.value
        model.showSidebar = true
        let left = model.newTab(url: "http://127.0.0.1:\(port)/left")
        let right = model.newTab(url: "http://127.0.0.1:\(port)/right")
        XCTAssertTrue(model.pairTabs(right.id, with: left.id))
        let originalPair = model.splitPairs
        let content = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        left.webView.frame = NSRect(x: 0, y: 0, width: 400, height: 400)
        right.webView.frame = NSRect(x: 400, y: 0, width: 400, height: 400)
        content.addSubview(left.webView); content.addSubview(right.webView)
        let window = NSWindow(contentRect: content.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = content; window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        try await expectTitle("/left:1", in: left.webView)
        try await expectTitle("/right:1", in: right.webView)
        let rightView = try XCTUnwrap(right.webView as? AppearanceWebView)
        let event = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDown, location: NSPoint(x: 600, y: 200), modifierFlags: [], timestamp: 0, windowNumber: window.windowNumber, context: nil, eventNumber: 1, clickCount: 1, pressure: 1))
        NSApp.sendEvent(event)
        XCTAssertEqual(model.activeID, right.id, "A real page click must update the command target")
        XCTAssertEqual(model.splitID, left.id)
        XCTAssertEqual(model.splitPairs, originalPair, "Changing focus must not swap panes")
        model.active?.webView.reload() // The same target used by the Command-R menu action.
        try await expectTitle("/right:2", in: right.webView)
        XCTAssertEqual(left.webView.title, "/left:1")
        let leftEvent = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDown, location: NSPoint(x: 200, y: 200), modifierFlags: [], timestamp: 0, windowNumber: window.windowNumber, context: nil, eventNumber: 2, clickCount: 1, pressure: 1))
        NSApp.sendEvent(leftEvent)
        XCTAssertEqual(model.activeID, left.id)
        model.active?.webView.reload()
        try await expectTitle("/left:2", in: left.webView)
        XCTAssertEqual(right.webView.title, "/right:2")
        rightView.agentControlled = true
        rightView.activatePane(for: event)
        XCTAssertEqual(model.activeID, left.id, "An agent-controlled pane must not take the user's command target")
        rightView.agentControlled = false
        let overlay = NSView(frame: right.webView.frame)
        content.addSubview(overlay)
        rightView.activatePane(for: event)
        XCTAssertEqual(model.activeID, left.id, "Clicks on an overlay must not activate the page underneath")
    }

    private func expectTitle(_ title: String, in view: WKWebView) async throws {
        for _ in 0..<100 {
            if view.title == title && !view.isLoading { return }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTFail("Expected \(title), got \(view.title ?? "nil")")
    }
}
