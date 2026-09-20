import AppKit
import WebKit

/// A custom action surface, without NSPopover's arrow. Keep WebKit's content
/// controller and sizing, but explicitly finish its popup lifecycle on dismissal.
@available(macOS 15.4, *)
@MainActor
final class ExtensionActionPanel {
    private final class Panel: NSPanel {
        override var canBecomeKey: Bool { true }
    }

    private let action: WKWebExtension.Action
    private let popover: NSPopover
    private let panel = Panel(contentRect: .zero, styleMask: [.borderless], backing: .buffered, defer: false)
    private weak var anchor: NSView?
    private var sizeObservation: NSKeyValueObservation?
    private var eventMonitor: Any?
    private var observers: [NSObjectProtocol] = []
    private var dismissed = false
    var onDismiss: (() -> Void)?

    init?(action: WKWebExtension.Action, anchor: NSView) {
        guard anchor.window != nil, let popover = action.popupPopover,
              let controller = popover.contentViewController else { return nil }
        self.action = action
        self.popover = popover
        self.anchor = anchor
        panel.isReleasedWhenClosed = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        // Avoid NSWindow's preferredContentSize handling, which re-centers the
        // window after WebKit resizes. Positioning belongs to this surface.
        panel.contentView = controller.view
        controller.view.wantsLayer = true
        controller.view.layer?.cornerRadius = 14
        controller.view.layer?.masksToBounds = true
    }

    func show() {
        guard let parent = anchor?.window else { dismiss(); return }
        position()
        parent.addChildWindow(panel, ordered: .above)
        panel.makeKeyAndOrderFront(nil)
        sizeObservation = popover.contentViewController?.observe(\.preferredContentSize, options: [.new]) { [weak self] _, _ in
            Task { @MainActor in self?.position() }
        }
        observers.append(NotificationCenter.default.addObserver(forName: NSWindow.didResizeNotification, object: panel, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.position() }
        })
        for name in [NSWindow.didResizeNotification, NSWindow.didMoveNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: parent, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.position() }
            })
        }
        for (name, object) in [(NSApplication.didResignActiveNotification, nil as AnyObject?), (NSWindow.willCloseNotification, parent)] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: object, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.dismiss() }
            })
        }
        eventMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .keyDown]) { [weak self] event in
            guard let self else { return event }
            if event.type == .keyDown && event.keyCode == 53 { self.dismiss(); return nil }
            if event.type != .keyDown && event.window !== self.panel { self.dismiss() }
            return event
        }
    }

    private func position() {
        guard !dismissed, let anchor, let parent = anchor.window else { return }
        let page = parent.convertToScreen(anchor.convert(anchor.bounds, to: nil))
        let available = page.intersection(parent.screen?.visibleFrame ?? page).insetBy(dx: 12, dy: 12)
        guard available.width > 0, available.height > 0 else { return }
        let requested = popover.contentViewController?.preferredContentSize ?? .zero
        let size = NSSize(width: min(requested.width > 0 ? requested.width : 360, available.width),
                          height: min(requested.height > 0 ? requested.height : 480, available.height))
        let frame = NSRect(x: available.maxX - size.width, y: available.maxY - size.height,
                           width: size.width, height: size.height)
        if panel.frame != frame { panel.setFrame(frame, display: true) }
    }

    func dismiss() {
        guard !dismissed else { return }
        dismissed = true
        sizeObservation = nil
        if let eventMonitor { NSEvent.removeMonitor(eventMonitor) }
        eventMonitor = nil
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        panel.parent?.removeChildWindow(panel)
        panel.close()
        panel.contentView = nil
        action.closePopup()
        onDismiss?()
        onDismiss = nil
    }
}
