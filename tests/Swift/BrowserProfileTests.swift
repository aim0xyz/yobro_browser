import XCTest
import WebKit
import Security
import SwiftUI
@testable import YOBRO

@MainActor
final class BrowserProfileTests: XCTestCase {
    private func isolated(_ body: (URL) async throws -> Void) async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", root.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: root)
        }
        try await body(root)
    }

    func testCreateSwitchRenameAndRestorePreservesOriginal() async throws {
        try await isolated { root in
            let profiles = BrowserProfiles(root: root)
            let original = profiles.browser
            let id = profiles.active.id
            XCTAssertEqual(original.home, root)
            original.newTab(url: "https://original.invalid")
            _ = try original.mergeImport(bookmarks: [ImportLink(title: "Original", url: "https://original.invalid")], history: [])
            try profiles.create(name: "Work", symbol: "briefcase.fill")
            let workID = profiles.active.id
            let work = profiles.browser
            XCTAssertNotEqual(work.home, original.home)
            XCTAssertTrue(work.bookmarks.isEmpty)
            XCTAssertFalse(work.tabs.contains { $0.url == "https://original.invalid" })
            XCTAssertFalse(work.agentEnabled)
            XCTAssertFalse(original.isProfileActive)
            do { _ = try await original.handle(["command": "tabs"]); XCTFail("Inactive profile must reject control") } catch {}
            let status = try await work.handle(["command": "status"])
            XCTAssertEqual(status["socket"] as? String, work.controlSocketURL.path)
            work.newTab(url: "https://work.invalid")
            try profiles.edit(name: "Studio", symbol: "sparkles")
            try profiles.switchTo(id)
            XCTAssertTrue(profiles.browser === original)
            XCTAssertEqual(original.bookmarks.count, 1)
            try profiles.switchTo(workID)
            XCTAssertTrue(profiles.browser === work)
            profiles.persistAll()
            let restored = BrowserProfiles(root: root)
            XCTAssertEqual(restored.active.name, "Studio")
            XCTAssertEqual(restored.active.id, workID)
            XCTAssertTrue(restored.browser.tabs.contains { $0.url == "https://work.invalid" })
            XCTAssertThrowsError(try profiles.create(name: " studio ", symbol: "leaf.fill"))
            XCTAssertThrowsError(try profiles.create(name: "  ", symbol: "leaf.fill"))
            try await Task.sleep(nanoseconds: 150_000_000)
        }
    }

    func testCookieAndPasswordIsolation() async throws {
        try await isolated { root in
            let profiles = BrowserProfiles(root: root)
            let original = profiles.browser
            let firstID = profiles.active.id
            let cookie = HTTPCookie(properties: [.domain: "profile-test.invalid", .path: "/", .name: "session", .value: "personal"])!
            await original.websiteDataStore.httpCookieStore.setCookie(cookie)
            let password = ImportPassword(url: "https://profile-test.invalid", username: "synthetic", password: "synthetic-only")
            defer { SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: PasswordVault.service(home: original.home)] as CFDictionary) }
            XCTAssertTrue(try PasswordVault.store(password, home: original.home, replace: false))
            try profiles.create(name: "Test", symbol: "flask.fill")
            let other = profiles.browser
            let cookies = await other.websiteDataStore.httpCookieStore.allCookies()
            XCTAssertTrue(cookies.isEmpty)
            XCTAssertTrue(try PasswordVault.entries(home: other.home).isEmpty)
            if #available(macOS 15.4, *) {
                XCTAssertTrue(other.extensions.runtime.controller.configuration.defaultWebsiteDataStore === other.websiteDataStore)
            }
            try profiles.switchTo(firstID)
            let restoredCookies = await profiles.browser.websiteDataStore.httpCookieStore.allCookies()
            XCTAssertEqual(restoredCookies.first?.value, "personal")
            XCTAssertEqual(try PasswordVault.entries(home: original.home).first?.password, password.password)
            try await Task.sleep(nanoseconds: 150_000_000)
        }
    }

    func testCorruptRegistryIsNotOverwritten() async throws {
        try await isolated { root in
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let file = root.appendingPathComponent("profiles.json")
            let invalid = Data("broken registry".utf8)
            try invalid.write(to: file)
            let profiles = BrowserProfiles(root: root)
            XCTAssertNotNil(profiles.browser.notice)
            XCTAssertThrowsError(try profiles.create(name: "Work", symbol: "briefcase.fill"))
            XCTAssertEqual(try Data(contentsOf: file), invalid)
            try await Task.sleep(nanoseconds: 150_000_000)
        }
    }

    func testProductionProfileUsesStableWebKitIdentifier() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        unsetenv("YOBRO_HOME")
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) }
            try? FileManager.default.removeItem(at: root)
        }
        let profile = LocalBrowserProfile(id: UUID(), name: "Synthetic", symbol: "flask.fill")
        var model: BrowserModel? = BrowserModel(profile: profile, root: root)
        XCTAssertEqual(model?.websiteDataStore.identifier, profile.id)
        XCTAssertEqual(model?.websiteDataStore.isPersistent, true)
        if #available(macOS 15.4, *) {
            XCTAssertEqual(model?.extensions.runtime.controller.configuration.identifier, profile.id)
        }
        let cookie = HTTPCookie(properties: [.domain: "profile-persistence.invalid", .path: "/", .name: "session", .value: "synthetic", .expires: Date().addingTimeInterval(3600)])!
        await model!.websiteDataStore.httpCookieStore.setCookie(cookie)
        try await Task.sleep(nanoseconds: 200_000_000)
        model = nil
        model = BrowserModel(profile: profile, root: root)
        let cookies = await model!.websiteDataStore.httpCookieStore.allCookies()
        XCTAssertEqual(cookies.first(where: { $0.name == "session" })?.value, "synthetic")
        try await Task.sleep(nanoseconds: 150_000_000)
        model = nil
        try await Task.sleep(nanoseconds: 100_000_000)
        try await WKWebsiteDataStore.remove(forIdentifier: profile.id)
    }
}
