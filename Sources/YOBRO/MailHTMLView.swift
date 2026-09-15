import SwiftUI
import WebKit

struct MailHTMLView: NSViewRepresentable {
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
        let policy = "default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src data: \(remoteImages ? "https: http:" : ""); font-src data:; frame-src 'none'; object-src 'none'; connect-src 'none'; form-action 'none'; base-uri 'none'"
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
        let document = Self.document(html, remoteImages: remoteImages)
        guard context.coordinator.last != document else { return }
        context.coordinator.last = document
        view.loadHTMLString(document, baseURL: nil)
    }
    final class Coordinator: NSObject, WKNavigationDelegate {
        var last = ""
        let openURL: (URL) -> Void
        init(openURL: @escaping (URL) -> Void) { self.openURL = openURL }
        func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if action.navigationType == .linkActivated, let url = action.request.url, ["http", "https"].contains(url.scheme ?? "") { openURL(url); decisionHandler(.cancel); return }
            decisionHandler(action.request.url?.absoluteString == "about:blank" ? .allow : .cancel)
        }
    }
}

struct MailUnreadDot: View {
    var body: some View { Circle().fill(Color(red: 0.96, green: 0.36, blue: 0.25)).frame(width: 7, height: 7).accessibilityLabel(L("Ungelesene Nachrichten", "Unread messages")) }
}

struct MailSidebarButton: View {
    @ObservedObject var store: MailStore
    let active: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 11) {
                Image(systemName: active ? "envelope.open.fill" : "envelope").font(.system(size: 18)).foregroundStyle(moss)
                Text(L("E-Mail")).font(.system(size: 12, weight: .medium))
                Spacer(minLength: 8)
                if store.hasUnread { MailUnreadDot() }
                Image(systemName: "chevron.right").font(.system(size: 10, weight: .medium)).foregroundStyle(ink.opacity(0.4))
            }.padding(.horizontal, 14).frame(maxWidth: .infinity).frame(height: 44)
                .background(active ? moss.opacity(0.15) : YOBROTheme.surface.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
        }.buttonStyle(YOBROButtonStyle()).help(L("YoBro Mail · Alle Postfächer"))
    }
}
