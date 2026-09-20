import SwiftUI
import AppKit

enum YOBROTooltipPlacement {
    case topTrailing, trailing
}

extension View {
    /// App-owned hover help, including controls inside popovers and scroll views.
    func yobroHelp(_ text: String, placement: YOBROTooltipPlacement = .topTrailing) -> some View {
        background {
            GeometryReader { geometry in
                YOBROTooltipAnchor(text: text, placement: placement)
                    .frame(width: geometry.size.width, height: geometry.size.height)
            }
        }
            .accessibilityHint(Text(text))
    }
}

private struct YOBROTooltipAnchor: NSViewRepresentable {
    let text: String
    let placement: YOBROTooltipPlacement
    func makeNSView(context: Context) -> YOBROTooltipAnchorView { YOBROTooltipAnchorView() }
    func updateNSView(_ view: YOBROTooltipAnchorView, context: Context) {
        if view.text != text || view.placement != placement {
            YOBROTooltipPresenter.shared.dismiss(owner: view)
            view.text = text
            view.placement = placement
        }
    }
    static func dismantleNSView(_ view: YOBROTooltipAnchorView, coordinator: ()) {
        YOBROTooltipPresenter.shared.dismiss(owner: view)
    }
}

final class YOBROTooltipAnchorView: NSView {
    var text = ""
    var placement: YOBROTooltipPlacement = .topTrailing
    private var area: NSTrackingArea?
    // On unclipped AppKit views, visibleRect can extend to the entire parent.
    // Hovering and positioning must stay inside this individual control's bounds.
    var helpRect: NSRect { bounds.intersection(visibleRect) }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let area { removeTrackingArea(area) }
        let next = NSTrackingArea(rect: helpRect, options: [.mouseEnteredAndExited, .activeInActiveApp], owner: self)
        addTrackingArea(next)
        area = next
    }
    override func mouseEntered(with event: NSEvent) { YOBROTooltipPresenter.shared.schedule(self) }
    override func mouseExited(with event: NSEvent) { YOBROTooltipPresenter.shared.dismiss(owner: self) }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { YOBROTooltipPresenter.shared.dismiss(owner: self) }
    }
}

struct YOBROTooltipCard: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(ink)
            .lineSpacing(3)
            .lineLimit(8)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 12).padding(.vertical, 9)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(paper, in: RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(YOBROTheme.border.opacity(0.75), lineWidth: 1))
            .padding(1)
    }
}

@MainActor
final class YOBROTooltipPresenter {
    static let shared = YOBROTooltipPresenter()
    private weak var owner: YOBROTooltipAnchorView?
    private var pending: Task<Void, Never>?
    private(set) var panel: NSPanel?
    private var eventMonitor: Any?
    private var inactiveObserver: NSObjectProtocol?

    init() {
        inactiveObserver = NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.dismiss() }
        }
    }

    func schedule(_ view: YOBROTooltipAnchorView) {
        guard !view.text.isEmpty else { return }
        // Nested help regions prefer the smaller control (e.g. a tab's close button).
        if let current = owner, current !== view, current.window === view.window,
           current.bounds.width * current.bounds.height < view.bounds.width * view.bounds.height,
           current.helpRect.contains(current.convert(current.window?.mouseLocationOutsideOfEventStream ?? .zero, from: nil)) { return }
        dismiss()
        owner = view
        monitorDismissalEvents()
        pending = Task { @MainActor [weak self, weak view] in
            do { try await Task.sleep(for: .milliseconds(450)) } catch { return }
            guard let self, let view, self.owner === view, let window = view.window,
                  window.isVisible, !view.isHiddenOrHasHiddenAncestor,
                  view.helpRect.contains(view.convert(window.mouseLocationOutsideOfEventStream, from: nil)) else { return }
            self.show(view)
        }
    }

    func show(_ view: YOBROTooltipAnchorView) {
        guard let window = view.window, let screen = window.screen else { return }
        dismiss()
        owner = view
        let longest = view.text.components(separatedBy: .newlines).map {
            ($0 as NSString).size(withAttributes: [.font: NSFont.systemFont(ofSize: 11, weight: .medium)]).width
        }.max() ?? 80
        let width = min(310, max(70, ceil(longest) + 26))
        let content = NSHostingView(rootView: YOBROTooltipCard(text: view.text).frame(width: width))
        content.appearance = view.effectiveAppearance
        let size = content.fittingSize
        // Keep sizing stable once the card's fitting size is known.
        content.sizingOptions = []
        content.frame = NSRect(origin: .zero, size: size)
        let anchor = window.convertToScreen(view.convert(view.helpRect, to: nil))
        let frame = Self.position(size: size, anchor: anchor, screen: screen.visibleFrame, placement: view.placement)
        let tooltip = NSPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        tooltip.isReleasedWhenClosed = false
        tooltip.backgroundColor = .clear
        tooltip.isOpaque = false
        tooltip.hasShadow = true
        tooltip.ignoresMouseEvents = true
        tooltip.hidesOnDeactivate = true
        tooltip.contentView = content
        tooltip.appearance = view.effectiveAppearance
        panel = tooltip
        window.addChildWindow(tooltip, ordered: .above)
        // AppKit inherits the parent level when adding a child; set this afterwards.
        tooltip.level = NSWindow.Level(rawValue: max(NSWindow.Level.popUpMenu.rawValue, window.level.rawValue + 1))
        tooltip.orderFront(nil)
        tooltip.setFrame(frame, display: true)
        monitorDismissalEvents()
    }

    private func monitorDismissalEvents() {
        eventMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .keyDown, .scrollWheel]) { [weak self] event in
            MainActor.assumeIsolated { self?.dismiss() }
            return event
        }
    }

    static func position(size: NSSize, anchor: NSRect, screen: NSRect, placement: YOBROTooltipPlacement = .topTrailing) -> NSRect {
        let safe = screen.insetBy(dx: 8, dy: 8)
        let width = min(size.width, safe.width), height = min(size.height, safe.height)
        // Overlap the source edge slightly so the card belongs to that control.
        var x = anchor.maxX - 12
        if x + width > safe.maxX { x = anchor.minX - width + 12 }
        x = min(max(x, safe.minX), safe.maxX - width)
        var y: CGFloat
        switch placement {
        case .topTrailing:
            y = anchor.maxY - 8
            if y + height > safe.maxY { y = anchor.minY - height + 8 }
        case .trailing:
            y = anchor.midY - height / 2
        }
        y = min(max(y, safe.minY), safe.maxY - height)
        return NSRect(x: x, y: y, width: width, height: height)
    }

    func dismiss(owner view: YOBROTooltipAnchorView? = nil) {
        if let view, owner !== view { return }
        pending?.cancel()
        pending = nil
        if let eventMonitor { NSEvent.removeMonitor(eventMonitor); self.eventMonitor = nil }
        if let panel {
            panel.parent?.removeChildWindow(panel)
            panel.orderOut(nil)
        }
        panel = nil
        owner = nil
    }
}
