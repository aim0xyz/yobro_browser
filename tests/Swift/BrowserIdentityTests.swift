import XCTest
import WebKit
@testable import YOBRO

final class BrowserIdentityTests: XCTestCase {
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
