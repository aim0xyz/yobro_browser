import XCTest
@testable import YOBRO

final class SupabaseAuthTests: XCTestCase {
    func testDisplayNameKeepsExistingBundleIdentity() throws {
        let plist = try XCTUnwrap(NSDictionary(contentsOf: URL(fileURLWithPath: "Resources/Info.plist")))
        XCTAssertEqual(plist["CFBundleName"] as? String, "YoBro")
        XCTAssertEqual(plist["CFBundleDisplayName"] as? String, "YoBro")
        XCTAssertEqual(plist["CFBundleIdentifier"] as? String, "local.yobro.browser")
        XCTAssertEqual(plist["CFBundleExecutable"] as? String, "YOBRO")
    }

    func testAuthCallbackReadsFragmentTokens() throws {
        let url = try XCTUnwrap(URL(string: "yobro://auth/reset-password#access_token=access%2Evalue&refresh_token=refresh%2Evalue&expires_in=3600&type=recovery"))
        let values = SupabaseAuthClient.authParameters(url)
        XCTAssertEqual(values["access_token"], "access.value")
        XCTAssertEqual(values["refresh_token"], "refresh.value")
        XCTAssertEqual(values["type"], "recovery")
    }

    func testInfoPlistRegistersYOBROScheme() throws {
        let plist = try XCTUnwrap(NSDictionary(contentsOf: URL(fileURLWithPath: "Resources/Info.plist")))
        let types = try XCTUnwrap(plist["CFBundleURLTypes"] as? [[String: Any]])
        XCTAssertTrue(types.contains { ($0["CFBundleURLSchemes"] as? [String])?.contains("yobro") == true })
    }

    func testInfoPlistRegistersWebSchemes() throws {
        let plist = try XCTUnwrap(NSDictionary(contentsOf: URL(fileURLWithPath: "Resources/Info.plist")))
        let types = try XCTUnwrap(plist["CFBundleURLTypes"] as? [[String: Any]])
        let schemes = types.flatMap { $0["CFBundleURLSchemes"] as? [String] ?? [] }
        XCTAssertTrue(schemes.contains("http"))
        XCTAssertTrue(schemes.contains("https"))
    }

    func testEmailFlowsUseHTTPSBridge() {
        XCTAssertEqual(SupabaseAuthClient.callbackURL, "https://yobro.aimoxyz.xyz/auth/callback")
        XCTAssertEqual(SupabaseAuthClient.recoveryURL, "https://yobro.aimoxyz.xyz/auth/reset-password")
    }
}
