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

    func testDeleteRemovesProfileDataAndReturnsToOriginal() async throws {
        try await isolated { root in
            let profiles = BrowserProfiles(root: root)
            try profiles.create(name: "Temporary", symbol: "leaf.fill")
            let id = profiles.active.id
            let model = profiles.browser
            let home = model.home
            let services = [PasswordVault.service(home: home), AgentAPIKeySecrets.service(home: home)]
            defer { for service in services { SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service] as CFDictionary) } }
            XCTAssertTrue(FileManager.default.fileExists(atPath: home.path))
            XCTAssertTrue(try PasswordVault.store(ImportPassword(url: "https://profile-delete.invalid", username: "synthetic", password: "synthetic-only"), home: home, replace: false))
            try AgentAPIKeySecrets.store("synthetic-key", endpoint: "https://agent.invalid", home: home)
            try AgentConnectionBinding.save(client: .codex, socket: model.controlSocketURL)
            try AgentConnectionBinding.save(client: .claude, socket: root.appendingPathComponent("control.sock"))

            try profiles.delete(id)
            XCTAssertEqual(profiles.active.id, LocalBrowserProfile.originalID)
            XCTAssertTrue(profiles.browser.isProfileActive)
            XCTAssertFalse(model.isProfileActive)
            XCTAssertFalse(profiles.registry.profiles.contains { $0.id == id })
            let saved = try JSONDecoder().decode(BrowserProfiles.Registry.self, from: Data(contentsOf: root.appendingPathComponent("profiles.json")))
            XCTAssertFalse(saved.profiles.contains { $0.id == id })
            XCTAssertFalse(FileManager.default.fileExists(atPath: home.path))
            for service in services {
                let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                            kSecReturnAttributes as String: true, kSecMatchLimit as String: kSecMatchLimitAll]
                XCTAssertEqual(SecItemCopyMatching(query as CFDictionary, nil), errSecItemNotFound)
            }
            XCTAssertNil(AgentConnectionBinding.read(client: .codex, root: root), "The deleted profile's agent link must go")
            XCTAssertNotNil(AgentConnectionBinding.read(client: .claude, root: root), "Other profiles keep their links")
            XCTAssertThrowsError(try profiles.delete(LocalBrowserProfile.originalID))
            try await Task.sleep(nanoseconds: 150_000_000)
        }
    }

    func testSwitchingProfilesCarriesAppWideLayoutPreferences() async throws {
        try await isolated { root in
            let defaults = UserDefaults.standard
            let keys = ["YOBRO.tabsOnTop", "YOBRO.showSidebar", "YOBRO.autoSuspendInactiveTabs"]
            let previous = keys.map { defaults.object(forKey: $0) }
            defer {
                for (key, value) in zip(keys, previous) {
                    if let value { defaults.set(value, forKey: key) } else { defaults.removeObject(forKey: key) }
                }
            }
            let profiles = BrowserProfiles(root: root)
            let first = profiles.browser
            let firstID = profiles.active.id
            try profiles.create(name: "Second", symbol: "moon.fill")
            let second = profiles.browser
            let secondID = profiles.active.id
            try profiles.switchTo(firstID)
            first.tabsOnTop.toggle()
            first.showSidebar.toggle()
            first.autoSuspendInactiveTabs.toggle()
            XCTAssertNotEqual(second.tabsOnTop, first.tabsOnTop)
            try profiles.switchTo(secondID)
            XCTAssertTrue(profiles.browser === second)
            XCTAssertEqual(second.tabsOnTop, first.tabsOnTop)
            XCTAssertEqual(second.showSidebar, first.showSidebar)
            XCTAssertEqual(second.autoSuspendInactiveTabs, first.autoSuspendInactiveTabs)
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
        // A real profile installs the bundled blocker on startup, which holds
        // the data store. Under a loaded test run that takes longer than any
        // fixed delay, so wait for it instead.
        await model!.extensionStartup?.value
        try await release(&model)
        model = BrowserModel(profile: profile, root: root)
        let cookies = await model!.websiteDataStore.httpCookieStore.allCookies()
        XCTAssertEqual(cookies.first(where: { $0.name == "session" })?.value, "synthetic")
        await model!.extensionStartup?.value
        try await release(&model)
        // WebKit lets go of the store asynchronously once its last user is gone.
        for attempt in 1...20 {
            do { try await WKWebsiteDataStore.remove(forIdentifier: profile.id); break }
            catch where attempt < 20 { try await Task.sleep(nanoseconds: 150_000_000) }
        }
    }

    /// Drops the model and fails if anything still keeps it alive.
    private func release(_ model: inout BrowserModel?) async throws {
        weak var released = model
        model = nil
        for _ in 0..<40 where released != nil { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertNil(released, "The browser model must be released with its profile")
    }
}
