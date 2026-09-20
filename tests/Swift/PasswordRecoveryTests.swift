import XCTest
import Foundation
import SwiftUI
@testable import YOBRO

final class RecoveryURLProtocol: URLProtocol {
    static var respond: ((URLRequest) throws -> (Int, Data))?
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        do {
            let (code, body) = try Self.respond!(request)
            client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: code, httpVersion: nil, headerFields: ["Content-Type": "application/json"])!, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: body); client?.urlProtocolDidFinishLoading(self)
        } catch { client?.urlProtocol(self, didFailWithError: error) }
    }
    override func stopLoading() {}
}

@MainActor
final class PasswordRecoveryTests: XCTestCase {
    private func auth() -> SupabaseAuthClient {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [RecoveryURLProtocol.self]
        return SupabaseAuthClient(urlSession: URLSession(configuration: configuration))
    }
    private let user = UUID(uuidString: "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")!
    private var userJSON: Data { Data("{\"id\":\"\(user)\",\"email\":\"fixture@example.invalid\"}".utf8) }

    func testResetLinkSwitchesAlreadyOpenSettingsToRecoveryForm() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.settingsSection = L("Allgemein", "General")
        let host = NSHostingView(rootView: BrowserSettings(model: model, section: model.settingsSection))
        host.frame = NSRect(x: 0, y: 0, width: 860, height: 650)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 100_000_000)
        await model.sync.handleAuthURL(URL(string: "yobro://auth/reset-password")!, model: model)
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertTrue(model.sync.needsNewPassword)
        XCTAssertEqual(model.settingsSection, L("Sync", "Sync"))
        host.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: "/tmp/YOBRO-reset-mac.png"))
    }

    func testOnlyOwnedAuthRoutesAreAccepted() {
        for value in ["yobro://auth/reset-password/", "https://yobro.aimoxyz.xyz/auth/reset-password#type=recovery", "https://yobro.lol/auth/callback?type=recovery"] {
            XCTAssertTrue(SupabaseAuthClient.isRecoveryURL(URL(string: value)!))
        }
        for value in ["https://yobro.lol.evil.invalid/auth/reset-password", "http://yobro.lol/auth/reset-password", "https://yobro.lol:9443/auth/reset-password", "yobro://other/reset-password", "https://yobro.lol/anything", "https://user@yobro.lol/auth/reset-password"] {
            XCTAssertFalse(SupabaseAuthClient.isAuthURL(URL(string: value)!))
        }
    }

    func testRecoveryUpdatesPasswordWithoutStartingSyncOrGeneratingKey() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home); RecoveryURLProtocol.respond = nil }
        let model = BrowserModel(root: home)
        let store = BrowserSyncStore(home: home, profileID: UUID(), auth: auth())
        var methods: [String] = []
        let body = userJSON
        RecoveryURLProtocol.respond = { request in
            XCTAssertEqual(request.url?.path, "/auth/v1/user")
            XCTAssertEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer fixture-access")
            methods.append(request.httpMethod ?? "")
            return (200, body)
        }
        model.settingsSection = L("Allgemein", "General"); model.showSettings = true
        await store.handleAuthURL(URL(string: "https://yobro.aimoxyz.xyz/auth/reset-password/#access_token=fixture-access&refresh_token=fixture-refresh&type=recovery")!, model: model)
        XCTAssertTrue(store.needsNewPassword)
        XCTAssertTrue(store.canUpdatePassword)
        XCTAssertFalse(store.signedIn)
        XCTAssertEqual(model.settingsSection, L("Sync", "Sync"))
        XCTAssertNil(store.recoveryCode)
        await store.updatePassword("Fixture-password-2026", model: model)
        XCTAssertEqual(methods, ["GET", "PUT"])
        XCTAssertFalse(store.needsNewPassword)
        XCTAssertFalse(store.signedIn)
        XCTAssertNil(store.recoveryCode, "Resetting a password must not replace the encrypted sync key")
    }

    func testExpiredAndIncompleteLinksStayInResetFlow() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home); RecoveryURLProtocol.respond = nil }
        let model = BrowserModel(root: home)
        let store = BrowserSyncStore(home: home, profileID: UUID(), auth: auth())
        RecoveryURLProtocol.respond = { _ in (401, Data("{\"msg\":\"expired\"}".utf8)) }
        await store.handleAuthURL(URL(string: "yobro://auth/reset-password#access_token=expired&refresh_token=expired")!, model: model)
        XCTAssertTrue(store.needsNewPassword)
        XCTAssertFalse(store.canUpdatePassword)
        XCTAssertFalse(store.busy)
        store.cancelPasswordRecovery()
        XCTAssertFalse(store.needsNewPassword)
        await store.handleAuthURL(URL(string: "yobro://auth/reset-password")!, model: model)
        XCTAssertTrue(store.needsNewPassword)
        XCTAssertFalse(store.canUpdatePassword)
    }

    func testTokenHashLinkIsVerifiedWithRecoveryType() async throws {
        defer { RecoveryURLProtocol.respond = nil }
        let response = Data("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",\"expires_in\":3600,\"user\":{\"id\":\"\(user)\",\"email\":\"fixture@example.invalid\"}}".utf8)
        RecoveryURLProtocol.respond = { request in
            XCTAssertEqual(request.url?.path, "/auth/v1/verify")
            XCTAssertEqual(request.httpMethod, "POST")
            return (200, response)
        }
        let (_, recovery) = try await auth().session(fromAuthURL: URL(string: "yobro://auth/reset-password?token_hash=fixture-hash&type=recovery")!)
        XCTAssertTrue(recovery)
    }
}
