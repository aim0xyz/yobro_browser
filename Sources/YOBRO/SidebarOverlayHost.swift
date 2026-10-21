import SwiftUI
import AppKit

/// Give the overlay its own native drag destination coordinate space, just like
/// the docked sidebar. A translated overlay in the web content's hosting view
/// can leave SwiftUI's registered drag regions behind its displayed rows.
struct SidebarOverlayHost<Content: View>: NSViewRepresentable {
    let visible: Bool
    let content: Content
    init(visible: Bool, @ViewBuilder content: () -> Content) {
        self.visible = visible
        self.content = content()
    }
    func makeCoordinator() -> SidebarOverlayTransition { SidebarOverlayTransition(visible: visible) }
    func makeNSView(context: Context) -> NSHostingView<Content> {
        let host = NSHostingView(rootView: content)
        host.sizingOptions = []
        // The shell already covers the full-size titlebar. A second safe-area
        // inset here would shift the overlay toolbar below the traffic lights.
        host.safeAreaRegions = []
        host.wantsLayer = true
        host.isHidden = !visible
        return host
    }
    func updateNSView(_ host: NSHostingView<Content>, context: Context) {
        host.rootView = content
        context.coordinator.update(host, visible: visible,
                                   reduceMotion: NSWorkspace.shared.accessibilityDisplayShouldReduceMotion)
    }
}

/// Slides the hidden sidebar in from the left edge and back out. The host is
/// only hidden once the slide-out finishes, and only if no newer change
/// asked for it again in the meantime.
@MainActor
final class SidebarOverlayTransition {
    private(set) var lastVisible: Bool
    private var generation = 0

    init(visible: Bool) { lastVisible = visible }

    func update(_ view: NSView, visible: Bool, reduceMotion: Bool) {
        guard visible != lastVisible else { return }
        lastVisible = visible
        generation &+= 1
        let current = generation
        let offscreen = CATransform3DMakeTranslation(-(Chrome.sidebarWidth + 24), 0, 0)
        if reduceMotion {
            view.layer?.transform = CATransform3DIdentity
            view.alphaValue = 1
            view.isHidden = !visible
            return
        }
        if visible && view.isHidden {
            view.alphaValue = 0
            view.layer?.transform = offscreen
            view.isHidden = false
        }
        // A reveal during the slide-out continues from where the overlay is.
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            context.timingFunction = CAMediaTimingFunction(name: .easeOut)
            context.allowsImplicitAnimation = true
            view.alphaValue = visible ? 1 : 0
            view.layer?.transform = visible ? CATransform3DIdentity : offscreen
        } completionHandler: { [weak self, weak view] in
            MainActor.assumeIsolated {
                guard let self, let view, !visible, self.generation == current, !self.lastVisible else { return }
                view.isHidden = true
            }
        }
    }
}
