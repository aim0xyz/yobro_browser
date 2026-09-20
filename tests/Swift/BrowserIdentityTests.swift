import XCTest
import WebKit
@testable import YOBRO

final class BrowserIdentityTests: XCTestCase {
    func testCatalogIdentityIsStrictlyScoped() {
        XCTAssertNotNil(BrowserIdentity.catalogUserAgent(for: URL(string: "https://chromewebstore.google.com/category/extensions")))
        for value in ["https://example.com", "https://accounts.google.com", "https://chromewebstore.google.com.evil.example", "http://chromewebstore.google.com"] {
            XCTAssertNil(BrowserIdentity.catalogUserAgent(for: URL(string: value)))
        }
    }

    func testBrowserIdentifiesAsInstalledSafari() {
        let configuration = WKWebViewConfiguration()
        BrowserIdentity.configure(configuration)

        XCTAssertEqual(configuration.applicationNameForUserAgent, BrowserIdentity.applicationNameForUserAgent)
        XCTAssertTrue(BrowserIdentity.applicationNameForUserAgent.hasPrefix("Version/"))
        XCTAssertTrue(BrowserIdentity.applicationNameForUserAgent.contains(" Safari/"))
        XCTAssertFalse(BrowserIdentity.safariVersion.isEmpty)
        XCTAssertTrue(configuration.userContentController.userScripts.contains {
            $0.source.contains("credential_picker_container") && $0.injectionTime == .atDocumentStart && $0.isForMainFrameOnly
        })
    }

    func testCompatibilityScriptOnlyTargetsGoogleOneTapPrompt() {
        XCTAssertTrue(BrowserIdentity.webViewCompatibilitySource.contains("#credential_picker_container"))
        XCTAssertTrue(BrowserIdentity.webViewCompatibilitySource.contains("#credential_picker_iframe"))
        XCTAssertFalse(BrowserIdentity.webViewCompatibilitySource.contains(".g_id_signin"))
    }
}
