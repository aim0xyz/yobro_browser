import XCTest
import WebKit
import Network
@testable import YOBRO

final class SpaceProxyTests: XCTestCase {
    @MainActor
    func testProxyPasswordIsStoredOnlyInKeychain() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer {
            try? ProxySecrets.remove(space: "Personal", home: home)
            try? FileManager.default.removeItem(at: home)
        }
        let store = SpaceProxyStore(home: home)
        let config = SpaceProxyConfig(
            enabled: false,
            label: "Private proxy",
            type: .socks5,
            host: "proxy.example.com",
            port: 1080,
            username: "service-user",
            password: "fixture-secret"
        )

        try store.set(config, for: "Personal")

        let file = home.appendingPathComponent("space-proxies.json")
        let persisted = try String(contentsOf: file, encoding: .utf8)
        XCTAssertFalse(persisted.contains("fixture-secret"))
        XCTAssertEqual(store.config(for: "Personal")?.password, "")
        XCTAssertEqual(try store.resolvedConfig(store.config(for: "Personal")!, for: "Personal").password, "fixture-secret")
    }

    func testProxyConfigurationValidation() {
        let config = SpaceProxyConfig(enabled: true, host: "  socks5://us-ny.proxy.example.com/ ", port: 1080)
        XCTAssertNoThrow(try config.validate())
        XCTAssertEqual(config.cleanHost(), "us-ny.proxy.example.com")

        let invalidHost = SpaceProxyConfig(enabled: true, host: "   ", port: 1080)
        XCTAssertThrowsError(try invalidHost.validate())

        let invalidPortZero = SpaceProxyConfig(enabled: true, host: "127.0.0.1", port: 0)
        XCTAssertThrowsError(try invalidPortZero.validate())

        let invalidPortTooHigh = SpaceProxyConfig(enabled: true, host: "127.0.0.1", port: 70000)
        XCTAssertThrowsError(try invalidPortTooHigh.validate())
    }

    func testProxyConfigurationNetworkObject() throws {
        guard #available(macOS 14.0, *) else { return }

        let socks = SpaceProxyConfig(
            enabled: true,
            label: "NordVPN East",
            type: .socks5,
            host: "socks5://us123.nordvpn.com",
            port: 1080,
            username: "vpnuser",
            password: "vpnpassword",
            matchDomains: ["example.com"],
            excludedDomains: ["localhost", "internal.local"]
        )

        let nwSocks = try socks.makeNetworkProxyConfiguration()
        XCTAssertNotNil(nwSocks)
        XCTAssertEqual(socks.displayLabel, "NordVPN East")

        let http = SpaceProxyConfig(
            enabled: true,
            label: "HTTP Proxy",
            type: .http,
            host: "proxy.company.com",
            port: 8080
        )
        let nwHttp = try http.makeNetworkProxyConfiguration()
        XCTAssertNotNil(nwHttp)
    }

    @MainActor
    func testSpaceProxyStorePersistenceAndRename() throws {
        let tempHome = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: tempHome) }

        let store = SpaceProxyStore(home: tempHome)
        let config = SpaceProxyConfig(
            enabled: true,
            label: "Mullvad Zürich",
            type: .socks5,
            host: "ch-zrh.socks.mullvad.net",
            port: 1080,
            username: "user123",
            password: "pass456"
        )

        try store.set(config, for: "Studio")
        XCTAssertEqual(store.config(for: "Studio")?.host, "ch-zrh.socks.mullvad.net")

        // Reload from disk
        let reloaded = SpaceProxyStore(home: tempHome)
        XCTAssertEqual(reloaded.config(for: "Studio")?.host, "ch-zrh.socks.mullvad.net")
        XCTAssertEqual(reloaded.config(for: "Studio")?.username, "user123")

        // Rename Space
        try reloaded.rename(from: "Studio", to: "Research")
        XCTAssertNil(reloaded.config(for: "Studio"))
        XCTAssertEqual(reloaded.config(for: "Research")?.host, "ch-zrh.socks.mullvad.net")

        // Reload again to verify rename persistence
        let reloadedAfterRename = SpaceProxyStore(home: tempHome)
        XCTAssertNil(reloadedAfterRename.config(for: "Studio"))
        XCTAssertEqual(reloadedAfterRename.config(for: "Research")?.host, "ch-zrh.socks.mullvad.net")

        // Remove
        try reloadedAfterRename.set(nil, for: "Research")
        XCTAssertNil(reloadedAfterRename.config(for: "Research"))
    }

    @MainActor
    func testSpaceProxyApplicationToWebsiteDataStore() throws {
        guard #available(macOS 14.0, *) else { return }

        let tempHome = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: tempHome) }

        let store = SpaceProxyStore(home: tempHome)
        let dataStore = WKWebsiteDataStore.nonPersistent()

        let usProxy = SpaceProxyConfig(
            enabled: true,
            label: "US NordVPN",
            type: .socks5,
            host: "us.socks.nordvpn.com",
            port: 1080
        )
        try store.set(usProxy, for: "US Space")

        // Apply proxy for "US Space"
        store.apply(to: dataStore, for: "US Space")
        XCTAssertEqual(dataStore.proxyConfigurations.count, 1)

        // Switch to "Default Space" (no proxy)
        store.apply(to: dataStore, for: "Default Space")
        XCTAssertEqual(dataStore.proxyConfigurations.count, 0)

        // Disable proxy in "US Space" and apply
        var disabled = usProxy
        disabled.enabled = false
        try store.set(disabled, for: "US Space")
        store.apply(to: dataStore, for: "US Space")
        XCTAssertEqual(dataStore.proxyConfigurations.count, 0)
    }

    @MainActor
    func testBrowserModelSpaceSwitchUpdatesProxyLive() throws {
        guard #available(macOS 14.0, *) else { return }

        let tempHome = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: tempHome) }

        let browser = BrowserModel(root: tempHome)
        let proxy = SpaceProxyConfig(
            enabled: true,
            label: "Lokal Tor/Shadowsocks",
            type: .socks5,
            host: "127.0.0.1",
            port: 9050
        )
        try browser.proxies.set(proxy, for: "Studio")

        // Current space is "Persönlich", no proxy
        XCTAssertEqual(browser.websiteDataStore.proxyConfigurations.count, 0)

        // Switch to "Studio" space -> proxy should be applied automatically
        browser.switchSpace("Studio")
        XCTAssertEqual(browser.space, "Studio")
        XCTAssertEqual(browser.websiteDataStore.proxyConfigurations.count, 1)

        // Switch back to "Persönlich" -> proxy should be removed automatically
        browser.switchSpace(browser.spaces[0])
        XCTAssertEqual(browser.websiteDataStore.proxyConfigurations.count, 0)
    }
}
