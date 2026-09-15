import AppKit
import SwiftUI

/// Keep AppKit's actual window buttons, including their native actions and menus,
/// in the same layout row as the browser navigation.
struct WindowControls: NSViewRepresentable {
    func makeNSView(context: Context) -> WindowControlsView { WindowControlsView() }
    func updateNSView(_ view: WindowControlsView, context: Context) { view.attachButtons() }
    static func dismantleNSView(_ view: WindowControlsView, coordinator: ()) { view.restoreButtons() }
}

final class WindowControlsView: NSView {
    private var originals: [(button: NSButton, parent: NSView, frame: NSRect, alpha: CGFloat)] = []
    private let buttonArtwork = WindowButtonAppearance()
    private var mouseMonitor: Any?
    private var previouslyAcceptedMouseMoved = false
    private var hoverArea: NSTrackingArea?
    private var observers: [NSObjectProtocol] = []
    override var intrinsicContentSize: NSSize { NSSize(width: 60, height: 32) }

    override func hitTest(_ point: NSPoint) -> NSView? {
        // The native buttons are intentionally almost transparent so their
        // original titlebar artwork does not overlap our stable custom paint.
        // AppKit excludes views at such a low alpha from normal hit testing,
        // therefore claim the three button regions here and forward clicks in
        // mouseDown(with:) below.
        if originals.contains(where: { $0.button.frame.contains(point) }) { return self }
        return super.hitTest(point)
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard let entry = originals.first(where: { $0.button.frame.contains(point) }),
              entry.button.isEnabled else {
            super.mouseDown(with: event)
            return
        }
        entry.button.performClick(nil)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { restoreButtons(); return }
        attachButtons()
        guard observers.isEmpty, let window else { return }
        previouslyAcceptedMouseMoved = window.acceptsMouseMovedEvents
        window.acceptsMouseMovedEvents = true
        mouseMonitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown, .leftMouseUp, .flagsChanged]) { [weak self] event in
            guard let self else { return event }
            let location = event.window === self.window ? event.locationInWindow : self.window?.mouseLocationOutsideOfEventStream ?? .zero
            let point = self.convert(location, from: nil)
            self.buttonArtwork.hovering = self.originals.reduce(NSRect.null) { $0.union($1.button.frame) }.contains(point)
            self.buttonArtwork.needsDisplay = true
            return event
        }
        for name in [NSWindow.didResizeNotification, NSWindow.didExitFullScreenNotification, NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                DispatchQueue.main.async { self?.attachButtons(); self?.layoutButtons() }
            })
        }
        observers.append(NotificationCenter.default.addObserver(forName: NSWindow.willEnterFullScreenNotification, object: window, queue: .main) { [weak self] _ in
            self?.returnToTitlebar()
        })
    }

    func attachButtons() {
        guard originals.isEmpty, let window, !window.styleMask.contains(.fullScreen) else { return }
        for type in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            guard let button = window.standardWindowButton(type), let parent = button.superview, parent !== self else { continue }
            originals.append((button, parent, button.frame, button.alphaValue))
            addSubview(button)
            // AppKit occasionally repositions standard window buttons in window
            // coordinates even after they have been embedded in this SwiftUI row.
            // Keep the native controls as invisible hit targets and draw their
            // appearance once, in our stable local coordinate system.
            button.alphaValue = 0.001
        }
        wantsLayer = true
        buttonArtwork.wantsLayer = true
        buttonArtwork.layer?.zPosition = 10
        addSubview(buttonArtwork, positioned: .above, relativeTo: nil)
        layoutButtons()
    }

    override func layout() { super.layout(); layoutButtons() }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let hoverArea { removeTrackingArea(hoverArea) }
        let hoverRect = originals.reduce(NSRect.null) { $0.union($1.button.frame) }
        let area = NSTrackingArea(rect: hoverRect.isNull ? .zero : hoverRect, options: [.mouseEnteredAndExited, .activeAlways], owner: self)
        addTrackingArea(area)
        hoverArea = area
    }

    override func mouseEntered(with event: NSEvent) {
        buttonArtwork.hovering = true
        buttonArtwork.needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        buttonArtwork.hovering = false
        buttonArtwork.needsDisplay = true
    }

    func layoutButtons() {
        buttonArtwork.frame = bounds
        for (index, entry) in originals.enumerated() {
            entry.button.setFrameOrigin(NSPoint(x: CGFloat(index) * 22, y: (bounds.height - entry.button.frame.height) / 2))
        }
        buttonArtwork.buttonFrames = originals.map { $0.button.frame }
        updateTrackingAreas()
        buttonArtwork.needsDisplay = true
    }

    private func returnToTitlebar() {
        for entry in originals {
            entry.parent.addSubview(entry.button)
            entry.button.frame = entry.frame
            entry.button.alphaValue = entry.alpha
        }
        originals.removeAll()
        buttonArtwork.removeFromSuperview()
    }

    func restoreButtons() {
        if let mouseMonitor { NSEvent.removeMonitor(mouseMonitor); self.mouseMonitor = nil }
        window?.acceptsMouseMovedEvents = previouslyAcceptedMouseMoved
        returnToTitlebar()
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
    }
}

/// A non-interactive paint layer gives the relocated native buttons a hover
/// region independent of AppKit's original titlebar tracking rectangles.
private final class WindowButtonAppearance: NSView {
    var hovering = false
    var buttonFrames: [NSRect] = []
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func draw(_ dirtyRect: NSRect) {
        let colors: [NSColor] = [NSColor(srgbRed: 1, green: 0.37, blue: 0.35, alpha: 1),
                                NSColor(srgbRed: 1, green: 0.74, blue: 0.18, alpha: 1),
                                NSColor(srgbRed: 0.16, green: 0.79, blue: 0.26, alpha: 1)]
        for (index, frame) in buttonFrames.enumerated() {
            let circle = NSBezierPath(ovalIn: frame.insetBy(dx: 0.5, dy: 0.5))
            let active = window?.isKeyWindow == true || hovering
            (active ? colors[index] : NSColor.quaternaryLabelColor).setFill()
            circle.fill()
            NSColor.black.withAlphaComponent(0.12).setStroke()
            circle.lineWidth = 0.5
            circle.stroke()
            guard hovering else { continue }
            let center = NSPoint(x: frame.midX, y: frame.midY)
            let glyph = NSBezierPath()
            glyph.lineWidth = 1.1
            glyph.lineCapStyle = .round
            NSColor.black.withAlphaComponent(0.65).setStroke()
            NSColor.black.withAlphaComponent(0.65).setFill()
            if index == 0 {
                glyph.move(to: NSPoint(x: center.x - 2.5, y: center.y - 2.5))
                glyph.line(to: NSPoint(x: center.x + 2.5, y: center.y + 2.5))
                glyph.move(to: NSPoint(x: center.x - 2.5, y: center.y + 2.5))
                glyph.line(to: NSPoint(x: center.x + 2.5, y: center.y - 2.5))
                glyph.stroke()
            } else if index == 1 {
                glyph.move(to: NSPoint(x: center.x - 3, y: center.y))
                glyph.line(to: NSPoint(x: center.x + 3, y: center.y))
                glyph.stroke()
            } else {
                glyph.move(to: NSPoint(x: center.x - 3, y: center.y + 3))
                glyph.line(to: NSPoint(x: center.x + 1, y: center.y + 3))
                glyph.line(to: NSPoint(x: center.x - 3, y: center.y - 1))
                glyph.close()
                glyph.move(to: NSPoint(x: center.x + 3, y: center.y - 3))
                glyph.line(to: NSPoint(x: center.x - 1, y: center.y - 3))
                glyph.line(to: NSPoint(x: center.x + 3, y: center.y + 1))
                glyph.close()
                glyph.fill()
            }
        }
    }
}
