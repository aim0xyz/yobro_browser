import XCTest
import SwiftUI
@testable import YOBRO

final class WindowControlsTests: XCTestCase {
    @MainActor
    func testWindowButtonsStayInTopNavigationRow() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let model = BrowserModel(root: home)
        defer { try? FileManager.default.removeItem(at: home) }
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1360, height: 880),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { window.close() }
        for (width, sidebar) in [(1360.0, true), (960.0, false), (1100.0, true)] {
            model.showSidebar = sidebar
            window.setContentSize(NSSize(width: width, height: 700))
            try await Task.sleep(nanoseconds: 400_000_000)
            host.layoutSubtreeIfNeeded()
            for (index, type) in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton].enumerated() {
                let button = try XCTUnwrap(window.standardWindowButton(type))
                XCTAssertTrue(button.superview is WindowControlsView)
                XCTAssertLessThan(button.alphaValue, 0.01)
                let rect = button.convert(button.bounds, to: host)
                let topCenter = host.isFlipped ? rect.midY : host.bounds.height - rect.midY
                XCTAssertEqual(topCenter, 26, accuracy: 1)
                XCTAssertEqual(rect.minX, (sidebar ? 17 : 10) + Double(index) * 22, accuracy: 1)
            }
        }
    }

    @MainActor
    func testTransparentWindowButtonForwardsClickToNativeAction() throws {
        let view = WindowControlsView(frame: NSRect(x: 0, y: 0, width: 60, height: 32))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 200),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = view
        window.orderFront(nil)
        defer { window.close() }

        view.layoutSubtreeIfNeeded()
        let close = try XCTUnwrap(window.standardWindowButton(.closeButton))
        let spy = WindowButtonActionSpy()
        close.target = spy
        close.action = #selector(WindowButtonActionSpy.clicked(_:))
        let center = view.convert(NSPoint(x: close.frame.midX, y: close.frame.midY), to: nil)
        let event = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDown,
                                                     location: center,
                                                     modifierFlags: [],
                                                     timestamp: 0,
                                                     windowNumber: window.windowNumber,
                                                     context: nil,
                                                     eventNumber: 1,
                                                     clickCount: 1,
                                                     pressure: 1))

        XCTAssertTrue(view.hitTest(view.convert(center, from: nil)) === view)
        view.mouseDown(with: event)
        XCTAssertEqual(spy.clickCount, 1)
    }
}

private final class WindowButtonActionSpy: NSObject {
    private(set) var clickCount = 0
    @objc func clicked(_ sender: Any?) { clickCount += 1 }
}
