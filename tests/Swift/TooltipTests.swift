import XCTest
import SwiftUI
@testable import YOBRO

@MainActor
final class TooltipTests: XCTestCase {
    func testTooltipStaysInsideScreenAtEdges() {
        let screen = NSRect(x: -1440, y: 0, width: 1440, height: 900)
        let safe = screen.insetBy(dx: 8, dy: 8)
        for origin in [NSPoint(x: -1430, y: 10), NSPoint(x: -20, y: 10), NSPoint(x: -1000, y: 800)] {
            let anchor = NSRect(origin: origin, size: NSSize(width: 40, height: 40))
            let frame = YOBROTooltipPresenter.position(size: NSSize(width: 260, height: 60), anchor: anchor, screen: screen)
            XCTAssertTrue(safe.contains(frame))
        }
    }

    func testButtonHelpOverlapsTopRightCornerAndTabHelpStaysOnRow() {
        let screen = NSRect(x: 0, y: 0, width: 1440, height: 900)
        let button = NSRect(x: 100, y: 300, width: 44, height: 44)
        let size = NSSize(width: 200, height: 40)
        let overlay = YOBROTooltipPresenter.position(size: size, anchor: button, screen: screen)
        XCTAssertEqual(overlay.minX, button.maxX - 12)
        XCTAssertEqual(overlay.minY, button.maxY - 8)
        XCTAssertTrue(overlay.intersects(button))
        let tab = NSRect(x: 16, y: 450, width: 228, height: 40)
        let tabOverlay = YOBROTooltipPresenter.position(size: size, anchor: tab, screen: screen, placement: .trailing)
        XCTAssertEqual(tabOverlay.minX, tab.maxX - 12)
        XCTAssertEqual(tabOverlay.midY, tab.midY)
    }

    func testHelpAnchorUsesButtonBoundsInsteadOfContainerBounds() async throws {
        _ = NSApplication.shared
        let view = NSHostingView(rootView:
            Button {} label: { Color.clear.frame(width: 44, height: 44) }
                .buttonStyle(.plain).yobroHelp("Button")
                .padding(20).frame(width: 300, height: 200, alignment: .topLeading))
        view.frame = NSRect(x: 0, y: 0, width: 300, height: 200)
        let window = NSWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = view
        defer { window.close() }
        view.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(100))
        view.layoutSubtreeIfNeeded()
        func find(_ view: NSView) -> YOBROTooltipAnchorView? {
            if let anchor = view as? YOBROTooltipAnchorView { return anchor }
            return view.subviews.lazy.compactMap(find).first
        }
        let anchor = try XCTUnwrap(find(view))
        XCTAssertEqual(anchor.bounds.width, 44, accuracy: 1)
        XCTAssertEqual(anchor.bounds.height, 44, accuracy: 1)
    }

    func testFloatingHelpDoesNotInterceptClicksAndDetachesOnDismiss() throws {
        _ = NSApplication.shared
        let screen = try XCTUnwrap(NSScreen.main)
        let window = NSWindow(contentRect: NSRect(x: screen.visibleFrame.minX + 100, y: screen.visibleFrame.minY + 100, width: 300, height: 200), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let anchor = YOBROTooltipAnchorView(frame: NSRect(x: 20, y: 50, width: 44, height: 44))
        anchor.text = "Neuer Tab · ⌘T"
        window.contentView?.addSubview(anchor)
        defer { YOBROTooltipPresenter.shared.dismiss(); window.close() }
        let responder = window.firstResponder
        YOBROTooltipPresenter.shared.show(anchor)
        let panel = try XCTUnwrap(YOBROTooltipPresenter.shared.panel)
        XCTAssertTrue(panel.ignoresMouseEvents)
        XCTAssertTrue(panel.parent === window)
        XCTAssertGreaterThan(panel.level.rawValue, window.level.rawValue)
        XCTAssertTrue(window.firstResponder === responder)
        XCTAssertGreaterThan(panel.frame.height, 25)
        XCTAssertLessThan(panel.frame.height, 100)
        XCTAssertNil(anchor.hitTest(.zero))
        YOBROTooltipPresenter.shared.dismiss(owner: anchor)
        XCTAssertNil(YOBROTooltipPresenter.shared.panel)
        XCTAssertNil(panel.parent)
        XCTAssertFalse(panel.isVisible)
    }

    func testRealTooltipWindowFollowsEachRowsScreenPosition() async throws {
        _ = NSApplication.shared
        let screen = try XCTUnwrap(NSScreen.main)
        let window = NSWindow(contentRect: NSRect(x: screen.visibleFrame.minX + 100, y: screen.visibleFrame.minY + 100, width: 400, height: 500), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        defer { YOBROTooltipPresenter.shared.dismiss(); window.close() }
        for y: CGFloat in [50, 230, 400] {
            let anchor = YOBROTooltipAnchorView(frame: NSRect(x: 20, y: y, width: 200, height: 44))
            anchor.text = "Tab at \(y)"
            anchor.placement = .trailing
            window.contentView?.addSubview(anchor)
            let expected = window.convertToScreen(anchor.convert(anchor.bounds, to: nil))
            YOBROTooltipPresenter.shared.show(anchor)
            try await Task.sleep(for: .milliseconds(100))
            let panel = try XCTUnwrap(YOBROTooltipPresenter.shared.panel)
            XCTAssertEqual(panel.frame.midY, expected.midY, accuracy: 1)
            XCTAssertEqual(panel.frame.minX, expected.maxX - 12, accuracy: 1)
            anchor.removeFromSuperview()
        }
    }

    func testControlCenterTooltipTracksEachOfTwelveButtons() async throws {
        _ = NSApplication.shared
        let screen = try XCTUnwrap(NSScreen.main)
        let window = NSWindow(contentRect: NSRect(x: screen.visibleFrame.minX + 100, y: screen.visibleFrame.minY + 100, width: 300, height: 350), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        defer { YOBROTooltipPresenter.shared.dismiss(); window.close() }
        for row in 0..<3 {
            for column in 0..<4 {
                let anchor = YOBROTooltipAnchorView(frame: NSRect(x: 16 + column * 62, y: 60 + row * 60, width: 54, height: 44))
                anchor.text = "Action \(row),\(column)"
                window.contentView?.addSubview(anchor)
                anchor.updateTrackingAreas()
                XCTAssertEqual(anchor.trackingAreas.first?.rect, anchor.bounds)
                let expected = window.convertToScreen(anchor.convert(anchor.bounds, to: nil))
                YOBROTooltipPresenter.shared.show(anchor)
                try await Task.sleep(for: .milliseconds(30))
                let panel = try XCTUnwrap(YOBROTooltipPresenter.shared.panel)
                XCTAssertEqual(panel.frame.minX, expected.maxX - 12, accuracy: 1)
                XCTAssertEqual(panel.frame.minY, expected.maxY - 8, accuracy: 1)
                anchor.removeFromSuperview()
            }
        }
    }

    func testLightAndDarkTooltipCardsRender() throws {
        _ = NSApplication.shared
        for dark in [false, true] {
            let view = NSHostingView(rootView: YOBROTooltipCard(text: "Seite vergrößern · ⌘+\nYOBRO — Dokumentation").frame(width: 260))
            view.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            view.frame = NSRect(origin: .zero, size: view.fittingSize)
            view.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
            view.cacheDisplay(in: view.bounds, to: bitmap)
            let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
            try data.write(to: URL(fileURLWithPath: "/tmp/yobro-tooltip-\(dark ? "dark" : "light").png"))
        }
    }
}
