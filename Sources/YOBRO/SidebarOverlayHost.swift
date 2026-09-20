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
    func makeNSView(context: Context) -> NSHostingView<Content> {
        let host = NSHostingView(rootView: content)
        host.sizingOptions = []
        // The shell already covers the full-size titlebar. A second safe-area
        // inset here would shift the overlay toolbar below the traffic lights.
        host.safeAreaRegions = []
        host.isHidden = !visible
        return host
    }
    func updateNSView(_ host: NSHostingView<Content>, context: Context) {
        host.rootView = content
        host.isHidden = !visible
    }
}
