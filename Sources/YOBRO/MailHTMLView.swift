import SwiftUI
import WebKit

struct MailHTMLView: NSViewRepresentable {
    /// Identifies the message; the page reloads only when it or `remoteImages` changes.
    let key: String
    let html: String
    let remoteImages: Bool
    let openURL: (URL) -> Void
    func makeCoordinator() -> Coordinator { Coordinator(openURL: openURL) }
    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.defaultWebpagePreferences.allowsContentJavaScript = false
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator
        view.allowsMagnification = true
        return view
    }
    static func document(_ html: String, remoteImages: Bool) -> String {
        let policy = "default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src data: \(remoteImages ? "https:" : ""); font-src data:; frame-src 'none'; object-src 'none'; connect-src 'none'; form-action 'none'; base-uri 'none'"
        // Protocol-relative image URLs occur in newsletters but have no origin
        // when loaded via loadHTMLString. Resolve them without enabling scripts.
        let resolved = html.replacingOccurrences(of: "src=\"//", with: "src=\"https://", options: .caseInsensitive)
            .replacingOccurrences(of: "src='//", with: "src='https://", options: .caseInsensitive)
        // Keep this override after the message markup. Some newsletters set
        // overflow:hidden on html/body, which otherwise disables wheel scrolling.
        let overrides = "html,body{color-scheme:light;overflow-x:auto!important;overflow-y:auto!important;height:auto!important;min-height:100%!important}body{margin:20px!important;background:#fff;color:#222;font:14px -apple-system,sans-serif;overflow-wrap:anywhere}img{max-width:100%!important;height:auto}table{max-width:100%}pre{white-space:pre-wrap}"
        return "<!doctype html><html><head><meta http-equiv=\"Content-Security-Policy\" content=\"\(policy)\"><meta name=\"viewport\" content=\"width=device-width\"></head><body>\(resolved)<style id=\"yobro-mail-overrides\">\(overrides)</style></body></html>"
    }
    func updateNSView(_ view: WKWebView, context: Context) {
        context.coordinator.openURL = openURL
        let identity = "\(key)|\(remoteImages)"
        guard context.coordinator.loaded != identity else { return }
        context.coordinator.loaded = identity
        view.loadHTMLString(Self.document(html, remoteImages: remoteImages), baseURL: nil)
    }
    final class Coordinator: NSObject, WKNavigationDelegate {
        var loaded = ""
        var openURL: (URL) -> Void
        init(openURL: @escaping (URL) -> Void) { self.openURL = openURL }
        func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if action.navigationType == .linkActivated,
               let url = action.request.url,
               ["http", "https"].contains(url.scheme?.lowercased() ?? "") {
                // Opening a tab hides Mail and tears this WKWebView down. Finish
                // the navigation callback first; mutating the view hierarchy from
                // inside it can leave WebKit with a blank page.
                decisionHandler(.cancel)
                DispatchQueue.main.async { [openURL] in openURL(url) }
                return
            }
            decisionHandler(action.request.url?.absoluteString == "about:blank" ? .allow : .cancel)
        }
    }
}

struct MailUnreadDot: View {
    var body: some View {
        Circle().fill(Color(red: 0.96, green: 0.36, blue: 0.25)).frame(width: 7, height: 7)
            .accessibilityLabel(L("Ungelesene Nachrichten", "Unread messages"))
    }
}

/// The envelope beside the assistant, in the sidebar and the tab strip.
struct MailButton: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var mail: MailStore
    var size: CGFloat = 26

    var body: some View {
        Button { model.presentMail(!model.showMail) } label: {
            Image(systemName: model.showMail ? "envelope.open.fill" : "envelope")
                .overlay(alignment: .topTrailing) {
                    if mail.hasUnread { Circle().fill(brandOrange).frame(width: 6, height: 6).offset(x: 3, y: -2) }
                }
        }
        .buttonStyle(ChromeButtonStyle(size: size))
        .yobroIconHelp(model.showMail ? L("Mail schließen · ⇧⌘M", "Close mail · ⇧⌘M") : L("Mail öffnen · ⇧⌘M", "Open mail · ⇧⌘M"), label: L("Mail", "Mail"))
        .accessibilityValue(mail.unreadCount > 0 ? L("\(mail.unreadCount) ungelesen", "\(mail.unreadCount) unread") : "")
        .accessibilityAddTraits(model.showMail ? .isSelected : [])
        .accessibilityIdentifier("mail-quick-access")
    }
}
