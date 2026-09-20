import Foundation
import WebKit

enum BrowserIdentity {
    /// The store otherwise redirects Safari/WebKit to /unsupported. This only
    /// enables browsing the catalog; extension APIs still come from WebKit.
    static func catalogUserAgent(for url: URL?) -> String? {
        guard url?.scheme == "https", url?.host?.lowercased() == "chromewebstore.google.com" else { return nil }
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"
    }

    /// Google Identity Services can still paint its One Tap iframe after a
    /// WKWebView has identified itself as Safari, but Google deliberately does
    /// not support completing that flow in web views. The result is a visible,
    /// inert prompt covering the site's own working sign-in controls.
    ///
    /// Keep regular "Sign in with Google" buttons untouched: those start the
    /// normal OAuth popup handled by BrowserTab's WKUIDelegate.
    static let webViewCompatibilitySource = """
    (() => {
      const removeUnsupportedGoogleOneTap = () => {
        document.querySelectorAll('#credential_picker_container, #credential_picker_iframe').forEach((element) => element.remove());
      };
      const beginWatching = () => {
        removeUnsupportedGoogleOneTap();
        if (document.documentElement) {
          new MutationObserver(removeUnsupportedGoogleOneTap).observe(document.documentElement, { childList: true, subtree: true });
        }
      };
      if (document.documentElement) beginWatching();
      else document.addEventListener('DOMContentLoaded', beginWatching, { once: true });
    })();
    """

    static var safariVersion: String {
        if let safari = Bundle(path: "/Applications/Safari.app"),
           let version = safari.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String,
           !version.isEmpty {
            return version
        }

        let version = ProcessInfo.processInfo.operatingSystemVersion
        return "\(version.majorVersion).\(version.minorVersion)"
    }

    static var applicationNameForUserAgent: String {
        "Version/\(safariVersion) Safari/605.1.15"
    }

    static func configure(_ configuration: WKWebViewConfiguration) {
        // A plain WKWebView omits the Version/Safari product tokens. Google then
        // treats the otherwise current system WebKit as an unsupported browser.
        configuration.applicationNameForUserAgent = applicationNameForUserAgent
        configuration.userContentController.addUserScript(WKUserScript(
            source: webViewCompatibilitySource,
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        ))
    }
}
