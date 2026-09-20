import XCTest
import WebKit
import UIKit
import SwiftUI
@testable import YoBroMobile

final class MobileTests: XCTestCase {
    @MainActor func testWebKitAcceptsBlockRules() async throws {
        let rules = try await WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "mobile.test", encodedContentRuleList: MobileProtection.rules)
        XCTAssertNotNil(rules)
        try await WKContentRuleListStore.default().removeContentRuleList(forIdentifier: "mobile.test")
    }
    func testSearchEncodesQueryWithoutChangingMeaning() {
        let url = MobileAddress.resolve("café & tabs")!
        XCTAssertEqual(URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first?.value, "café & tabs")
    }
    func testNavigationRejectsExecutableAndLocalSchemes() {
        for value in ["javascript:alert(1)", "file:///etc/passwd", "data:text/html,test", "yobro:auth"] {
            XCTAssertNil(MobileAddress.resolve(value))
        }
        XCTAssertEqual(MobileAddress.resolve("example.com/path")?.absoluteString, "https://example.com/path")
        XCTAssertNil(MobileAddress.resolve("  "))
    }
    func testStateRoundTripKeepsTabsAndNotes() throws {
        var state = MobileSnapshot()
        state.selected = state.tabs[0].id
        state.notes = [MobileNote(title: "Recherche", text: "Entwurf", source: "https://example.com")]
        let restored = try JSONDecoder().decode(MobileSnapshot.self, from: JSONEncoder().encode(state))
        XCTAssertEqual(restored.selected, restored.tabs[0].id)
        XCTAssertEqual(restored.notes[0].text, "Entwurf")
    }
    func testBlockRulesOnlyApplyToThirdParties() throws {
        let rules = try JSONSerialization.jsonObject(with: Data(MobileProtection.rules.utf8)) as! [[String: Any]]
        XCTAssertEqual(rules.count, MobileProtection.domains.count)
        for rule in rules {
            let trigger = rule["trigger"] as! [String: Any]
            XCTAssertEqual(trigger["load-type"] as? [String], ["third-party"])
            let regex = try NSRegularExpression(pattern: trigger["url-filter"] as! String)
            let safe = "https://notdoubleclick.net/script.js"
            XCTAssertEqual(regex.numberOfMatches(in: safe, range: NSRange(safe.startIndex..., in: safe)), 0)
        }
    }
}

final class MobileSyncTests: XCTestCase {
    private let profile = UUID()
    private var remote: [String: Any] {
        ["version": 1, "profileID": profile.uuidString, "modifiedAt": 1000, "tabs": [["id": UUID().uuidString, "title": "Desktop", "url": "https://example.com", "space": "Work", "pinned": true]], "spaces": ["Work"], "currentSpace": "Work", "bookmarks": [], "history": [], "closedTabs": [], "folders": [], "splitPairs": [], "futureField": ["keep": true]]
    }
    func testCipherInteroperabilityAndWrongKeyRejected() throws {
        let key = Data(repeating: 5, count: 32)
        let payload = try MobileSyncCodec.seal(remote, key: key)
        let opened = try MobileSyncCodec.open(payload, key: key, profileID: profile)
        XCTAssertEqual(opened["currentSpace"] as? String, "Work")
        XCTAssertThrowsError(try MobileSyncCodec.open(payload, key: Data(repeating: 6, count: 32), profileID: profile))
        XCTAssertThrowsError(try MobileSyncCodec.open(payload, key: key, profileID: UUID()))
    }
    func testMergePreservesDesktopAndUnknownFields() {
        let initial = remote
        var local = MobileSnapshot()
        local.tabs = [MobileTab(title: "Mobile", url: "https://mobile.example")]
        local.notes = [MobileNote(title: "Gedanken", text: "Text")]
        let merged = MobileSyncCodec.merged(initial, local: local)
        XCTAssertEqual((merged["tabs"] as? [[String: Any]])?.count, 3)
        XCTAssertEqual(merged["currentSpace"] as? String, "Work")
        XCTAssertEqual((merged["futureField"] as? [String: Bool])?["keep"], true)
        XCTAssertEqual((merged["tabs"] as? [[String: Any]])?.first?["pinned"] as? Bool, true)
        let repeated = MobileSyncCodec.merged(merged, local: local)
        XCTAssertEqual((repeated["tabs"] as? [[String: Any]])?.count, 3)
    }
    func testImportIsIdempotentAndRejectsUnsafeURLs() {
        var data = remote
        var tabs = data["tabs"] as! [[String: Any]]
        tabs.append(["id": UUID().uuidString, "url": "javascript:alert(1)", "title": "Unsafe"])
        tabs.append(["id": UUID().uuidString, "kind": "note", "title": "Note", "noteContent": "Hello"])
        data["tabs"] = tabs
        let state = MobileSyncCodec.importing(data, into: MobileSnapshot())
        XCTAssertEqual(state.tabs.count, 2)
        XCTAssertEqual(state.notes.count, 1)
        let again = MobileSyncCodec.importing(data, into: state)
        XCTAssertEqual(again.tabs.count, state.tabs.count)
        XCTAssertEqual(again.notes.count, state.notes.count)
    }
    func testImportKeepsSpacesAndGroupsTabsIntoFolders() {
        let folderID = UUID()
        let tabID = UUID()
        var data = remote
        data["folders"] = [["id": folderID.uuidString, "name": "Lesen", "space": "Work", "collapsed": false, "color": "moss"]]
        data["tabs"] = [["id": tabID.uuidString, "title": "Grouped", "url": "https://example.com", "space": "Work", "folderID": folderID.uuidString]]
        let state = MobileSyncCodec.importing(data, into: MobileSnapshot())
        XCTAssertEqual(state.spaces, ["Work"])
        XCTAssertEqual(state.selectedSpace, "Work")
        XCTAssertEqual(state.folders?.first?.name, "Lesen")
        XCTAssertEqual(state.tabs.first(where: { $0.sourceID == tabID })?.folderID, folderID)
    }
    func testRecoveryKeyURLSafeEncoding() throws {
        let key = Data(repeating: 255, count: 32)
        let code = key.base64EncodedString().replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
        XCTAssertEqual(try MobileSyncCodec.key(from: code), key)
        XCTAssertThrowsError(try MobileSyncCodec.key(from: "invalid"))
    }
    @MainActor func testDialogCompletionRunsExactlyOnce() {
        var calls = 0
        let dialog = MobileDialog(tabID: UUID(), origin: "example.com", message: "Confirm?", kind: .confirm) { _ in calls += 1 }
        dialog.finish(nil); dialog.finish("yes")
        XCTAssertEqual(calls, 1)
    }
    @MainActor func testDownloadFilenameCannotEscapeDirectory() {
        XCTAssertEqual(MobileDownloads.safeName("../../secret.txt"), "secret.txt")
        XCTAssertEqual(MobileDownloads.safeName(".."), "Download")
    }
    @MainActor func testBundledUBlockActuallyStarts() async throws {
        guard #available(iOS 18.6, *) else { throw XCTSkip("Requires iOS 18.6") }
        let runtime = MobileExtensions(userID: UUID(), dataStore: .default())
        try await runtime.start()
        XCTAssertNotNil(runtime.context)
        XCTAssertEqual(runtime.readiness["ready"] as? Bool, true)
        let ruleCount = (runtime.readiness["rulesets"] as? Int ?? 0) + (runtime.readiness["dynamicRules"] as? Int ?? 0) + (runtime.readiness["sessionRules"] as? Int ?? 0)
        XCTAssertGreaterThan(ruleCount, 0, "uBlock must install actual filtering rules")
        try runtime.stop()
        XCTAssertNil(runtime.context)
    }
}

final class MobileGuestTests: XCTestCase {
    @MainActor func testGuestRestoresTabsAndNotesWithoutAuthentication() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let account = MobileAccount()
        XCTAssertTrue(account.isGuest)
        let first = MobileBrowser(userID: account.browserProfileID, home: home)
        first.state.tabs = [MobileTab(title: "Local", url: "https://example.com")]
        first.state.selected = first.state.tabs[0].id
        first.state.notes = [MobileNote(title: "Offline", text: "Kein Konto erforderlich")]
        first.save()
        let reopenedAccount = MobileAccount()
        XCTAssertEqual(reopenedAccount.browserProfileID, account.browserProfileID)
        let reopened = MobileBrowser(userID: reopenedAccount.browserProfileID, home: home)
        XCTAssertEqual(reopened.state.tabs.first?.url, "https://example.com")
        XCTAssertEqual(reopened.state.notes.first?.text, "Kein Konto erforderlich")
        XCTAssertNil(reopenedAccount.session)
    }
    @MainActor func testGuestCannotRequestAuthenticatedSyncSession() async {
        let account = MobileAccount()
        do { _ = try await account.validSession(); XCTFail("Guest must not receive cloud credentials") }
        catch { XCTAssertNil(account.session); XCTAssertFalse(account.busy) }
    }
    @MainActor func testSharedPaletteResolvesBothAppearances() {
        for (style, expected) in [(UIUserInterfaceStyle.light, UInt32(0xF9F8F4)), (.dark, UInt32(0x191E1B))] {
            let color = UIColor(YOBROTheme.page).resolvedColor(with: UITraitCollection(userInterfaceStyle: style))
            var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
            XCTAssertTrue(color.getRed(&r, green: &g, blue: &b, alpha: &a))
            XCTAssertEqual(r, CGFloat((expected >> 16) & 255) / 255, accuracy: 0.005)
            XCTAssertEqual(g, CGFloat((expected >> 8) & 255) / 255, accuracy: 0.005)
            XCTAssertEqual(b, CGFloat(expected & 255) / 255, accuracy: 0.005)
        }
    }
}


private final class MobileRecoveryProtocol: URLProtocol {
    static var respond: ((URLRequest) -> (Int, Data))?
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let (code, data) = Self.respond!(request)
        client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: code, httpVersion: nil, headerFields: ["Content-Type": "application/json"])!, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data); client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

@MainActor
final class MobilePasswordRecoveryTests: XCTestCase {
    private func account() -> MobileAccount {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [MobileRecoveryProtocol.self]
        return MobileAccount(auth: SupabaseAuthClient(urlSession: URLSession(configuration: config)))
    }

    func testRecoveryShowsFormAndSavesPasswordWithoutSwitchingBrowserProfile() async {
        defer { MobileRecoveryProtocol.respond = nil }
        let account = account()
        let profile = account.browserProfileID
        var methods: [String] = []
        MobileRecoveryProtocol.respond = { request in
            methods.append(request.httpMethod ?? "")
            XCTAssertEqual(request.url?.path, "/auth/v1/user")
            XCTAssertEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer fixture-access")
            return (200, Data("{\"id\":\"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\",\"email\":\"fixture@example.invalid\"}".utf8))
        }
        await account.handle(URL(string: "https://yobro.aimoxyz.xyz/auth/reset-password/#access_token=fixture-access&refresh_token=fixture-refresh&type=recovery")!)
        XCTAssertTrue(account.needsNewPassword)
        XCTAssertTrue(account.canUpdatePassword)
        XCTAssertNil(account.session)
        XCTAssertEqual(account.browserProfileID, profile)
        await account.changePassword("Fixture-password-2026")
        XCTAssertEqual(methods, ["GET", "PUT"])
        XCTAssertTrue(account.passwordRecoveryCompleted)
        XCTAssertFalse(account.needsNewPassword)
        XCTAssertNil(account.session)
        XCTAssertEqual(account.browserProfileID, profile)
    }

    func testIncompleteLinkFormRenders() async throws {
        let account = account()
        await account.handle(URL(string: "yobro://auth/reset-password")!)
        let host = UIHostingController(rootView: MobilePasswordReset().environmentObject(account))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 402, height: 874))
        window.rootViewController = host; window.makeKeyAndVisible()
        defer { window.isHidden = true }
        try await Task.sleep(nanoseconds: 300_000_000)
        host.view.layoutIfNeeded()
        let image = UIGraphicsImageRenderer(bounds: host.view.bounds).image { _ in host.view.drawHierarchy(in: host.view.bounds, afterScreenUpdates: true) }
        let attachment = XCTAttachment(image: image); attachment.name = "Password recovery form"; attachment.lifetime = .keepAlways; add(attachment)
        XCTAssertTrue(account.needsNewPassword)
        XCTAssertNotNil(account.error)
    }

    func testExpiredAndIncompleteLinksShowRecoveryErrorInsteadOfBrowser() async {
        defer { MobileRecoveryProtocol.respond = nil }
        let account = account()
        MobileRecoveryProtocol.respond = { _ in (401, Data("{\"msg\":\"expired\"}".utf8)) }
        await account.handle(URL(string: "yobro://auth/reset-password#access_token=expired&refresh_token=expired")!)
        XCTAssertTrue(account.needsNewPassword)
        XCTAssertFalse(account.canUpdatePassword)
        XCTAssertNotNil(account.error)
        account.cancelPasswordRecovery()
        XCTAssertFalse(account.needsNewPassword)
        await account.handle(URL(string: "yobro://auth/reset-password")!)
        XCTAssertTrue(account.needsNewPassword)
        XCTAssertNotNil(account.error)
    }
}
