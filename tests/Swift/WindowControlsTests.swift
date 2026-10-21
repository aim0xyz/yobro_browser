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
                XCTAssertEqual(rect.minX, 18 + Double(index) * 22, accuracy: 1)
            }
        }
    }

    /// Presses the close button through the transparent row and releases the
    /// mouse at `release` (window coordinates; the button centre if nil).
    /// AppKit's own button tracking ignores synthesized events once a window
    /// is on screen, so stand-in buttons track the press the same way.
    @MainActor
    private func clickCloseButton(releasingAt release: NSPoint? = nil, declining: Bool = false) throws -> Int {
        _ = NSApplication.shared
        let window = StubButtonWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 200),
                                      styleMask: [.borderless], backing: .buffered, defer: false)
        let titlebar = NSView()
        for type in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            let button = TrackingWindowButton(frame: NSRect(x: 0, y: 0, width: 14, height: 14))
            button.declines = declining
            titlebar.addSubview(button)
            window.stubs[type] = button
        }
        let view = WindowControlsView(frame: NSRect(x: 0, y: 0, width: 60, height: 32))
        window.isReleasedWhenClosed = false
        window.contentView = view
        window.orderFront(nil)
        defer { window.close() }

        view.layoutSubtreeIfNeeded()
        let close = try XCTUnwrap(window.standardWindowButton(.closeButton))
        XCTAssertTrue(close.superview === view)
        let spy = WindowButtonActionSpy()
        close.target = spy
        close.action = #selector(WindowButtonActionSpy.clicked(_:))
        let center = view.convert(NSPoint(x: close.frame.midX, y: close.frame.midY), to: nil)
        func mouse(_ type: NSEvent.EventType, at location: NSPoint) throws -> NSEvent {
            try XCTUnwrap(NSEvent.mouseEvent(with: type, location: location, modifierFlags: [], timestamp: 0,
                                             windowNumber: window.windowNumber, context: nil, eventNumber: 1,
                                             clickCount: 1, pressure: type == .leftMouseDown ? 1 : 0))
        }

        XCTAssertTrue(view.hitTest(view.convert(center, from: nil)) === view)
        window.postEvent(try mouse(.leftMouseUp, at: release ?? center), atStart: false)
        view.mouseDown(with: try mouse(.leftMouseDown, at: center))
        // Drain a release the button left unread.
        _ = NSApp.nextEvent(matching: .leftMouseUp, until: Date(), inMode: .default, dequeue: true)
        return spy.clickCount
    }

    @MainActor
    func testTransparentWindowButtonForwardsClickToNativeAction() throws {
        XCTAssertEqual(try clickCloseButton(), 1)
    }

    @MainActor
    func testReleasingOutsideWindowButtonCancelsClick() throws {
        XCTAssertEqual(try clickCloseButton(releasingAt: NSPoint(x: 220, y: 120)), 0)
    }

    @MainActor
    func testButtonDecliningPressDoesNotBounceBackForever() throws {
        XCTAssertEqual(try clickCloseButton(declining: true), 0)
    }

    @MainActor
    func testDoubleClickOnEmptyChromeFollowsSystemSetting() async throws {
        let key = "AppleActionOnDoubleClick"
        defer { UserDefaults.standard.removeObject(forKey: key) }
        let screen = try XCTUnwrap(NSScreen.main).visibleFrame
        let host = NSHostingView(rootView: WindowDragArea())
        let window = NSWindow(contentRect: NSRect(x: screen.minX + 80, y: screen.minY + 80, width: 360, height: 240),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { window.close() }
        host.layoutSubtreeIfNeeded()
        func find(_ view: NSView) -> WindowDragView? {
            view as? WindowDragView ?? view.subviews.lazy.compactMap(find).first
        }
        let area = try XCTUnwrap(find(host))
        XCTAssertTrue(area.mouseDownCanMoveWindow)
        let doubleClick = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDown, location: NSPoint(x: 100, y: 100), modifierFlags: [],
                                                           timestamp: 0, windowNumber: window.windowNumber, context: nil,
                                                           eventNumber: 1, clickCount: 2, pressure: 1))

        UserDefaults.standard.set("None", forKey: key)
        let frame = window.frame
        area.mouseDown(with: doubleClick)
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(window.frame, frame)
        XCTAssertFalse(window.isZoomed)

        UserDefaults.standard.set("Maximize", forKey: key)
        area.mouseDown(with: doubleClick)
        for _ in 0..<40 where !window.isZoomed { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertTrue(window.isZoomed)
    }

    @MainActor
    func testHiddenSidebarSwitchesInstantlyWhenMotionIsReduced() {
        let view = NSView(frame: NSRect(x: 0, y: 0, width: Chrome.sidebarWidth, height: 400))
        view.wantsLayer = true
        let transition = SidebarOverlayTransition(visible: true)
        transition.update(view, visible: false, reduceMotion: true)
        XCTAssertTrue(view.isHidden)
        transition.update(view, visible: true, reduceMotion: true)
        XCTAssertFalse(view.isHidden)
        XCTAssertEqual(view.alphaValue, 1)
    }

    @MainActor
    func testHiddenSidebarRevealedDuringSlideOutStaysVisible() async throws {
        let view = NSView(frame: NSRect(x: 0, y: 0, width: Chrome.sidebarWidth, height: 400))
        let window = NSWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView?.addSubview(view)
        view.wantsLayer = true
        window.orderFront(nil)
        defer { window.close() }
        let transition = SidebarOverlayTransition(visible: true)

        transition.update(view, visible: false, reduceMotion: false)
        XCTAssertFalse(view.isHidden, "The overlay stays mounted while it slides out")
        transition.update(view, visible: true, reduceMotion: false)
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertFalse(view.isHidden)
        XCTAssertEqual(view.alphaValue, 1)
        XCTAssertTrue(CATransform3DIsIdentity(try XCTUnwrap(view.layer).transform))

        transition.update(view, visible: false, reduceMotion: false)
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertTrue(view.isHidden)
        transition.update(view, visible: true, reduceMotion: false)
        XCTAssertFalse(view.isHidden)
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertTrue(CATransform3DIsIdentity(try XCTUnwrap(view.layer).transform))
    }
}

/// Tracks a press like NSButton: the action fires only when the mouse is
/// released inside. A declining button passes the press up the responder
/// chain, as NSControl does for events it does not handle.
private final class TrackingWindowButton: NSButton {
    var declines = false
    override func mouseDown(with event: NSEvent) {
        if declines { nextResponder?.mouseDown(with: event); return }
        guard let release = NSApp.nextEvent(matching: .leftMouseUp, until: Date().addingTimeInterval(1),
                                            inMode: .eventTracking, dequeue: true) else { return }
        if bounds.contains(convert(release.locationInWindow, from: nil)) { sendAction(action, to: target) }
    }
}

private final class StubButtonWindow: NSWindow {
    var stubs: [NSWindow.ButtonType: NSButton] = [:]
    override func standardWindowButton(_ type: NSWindow.ButtonType) -> NSButton? { stubs[type] }
}

private final class WindowButtonActionSpy: NSObject {
    private(set) var clickCount = 0
    @objc func clicked(_ sender: Any?) { clickCount += 1 }
}
