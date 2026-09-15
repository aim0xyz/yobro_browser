import SwiftUI
import WebKit
import Network
import Security

enum SpaceProxyType: String, Codable, CaseIterable, Identifiable {
    case socks5 = "SOCKS5"
    case http = "HTTP CONNECT"
    case https = "HTTPS / TLS"
    var id: String { rawValue }
}

struct SpaceProxyConfig: Codable, Equatable, Identifiable {
    var id: String { host + ":" + String(port) }
    var enabled = true
    var label = ""
    var type: SpaceProxyType = .socks5
    var host = ""
    var port = 1080
    var username = ""
    var password = ""
    var matchDomains: [String] = []
    var excludedDomains: [String] = ["localhost", "127.0.0.1", "[::1]"]

    var displayLabel: String {
        if !label.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return label.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        let clean = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if clean.isEmpty { return L("Kein Proxy", "No proxy") }
        return "\(clean):\(port)"
    }

    func cleanHost() -> String {
        host.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://", with: "")
            .replacingOccurrences(of: "http://", with: "")
            .replacingOccurrences(of: "socks5://", with: "")
            .replacingOccurrences(of: "socks5h://", with: "")
            .replacingOccurrences(of: "/", with: "")
    }

    func validate() throws {
        let cleaned = cleanHost()
        guard !cleaned.isEmpty else {
            throw YOBROError.message(L("Bitte einen Server-Hostnamen oder eine IP-Adresse eingeben.", "Please enter a server host name or IP address."))
        }
        guard (1...65535).contains(port) else {
            throw YOBROError.message(L("Bitte einen gültigen Port zwischen 1 und 65535 eingeben.", "Please enter a valid port between 1 and 65535."))
        }
    }

    @available(macOS 14.0, *)
    func makeNetworkProxyConfiguration() throws -> ProxyConfiguration {
        try validate()
        let cleaned = cleanHost()
        guard let portEndpoint = NWEndpoint.Port(rawValue: UInt16(port)) else {
            throw YOBROError.message(L("Ungültiger Port.", "Invalid port."))
        }
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(cleaned), port: portEndpoint)
        var config: ProxyConfiguration
        switch type {
        case .socks5:
            config = ProxyConfiguration(socksv5Proxy: endpoint)
        case .http:
            config = ProxyConfiguration(httpCONNECTProxy: endpoint)
        case .https:
            config = ProxyConfiguration(httpCONNECTProxy: endpoint, tlsOptions: NWProtocolTLS.Options())
        }
        let user = username.trimmingCharacters(in: .whitespacesAndNewlines)
        if !user.isEmpty {
            config.applyCredential(username: user, password: password)
        }
        let matched = matchDomains.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        if !matched.isEmpty {
            config.matchDomains = matched
        }
        let excluded = excludedDomains.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        if !excluded.isEmpty {
            config.excludedDomains = excluded
        }
        return config
    }
}

enum ProxySecrets {
    static func service(home: URL) -> String { "local.yobro.proxy." + home.path }

    private static func query(space: String, home: URL) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service(home: home),
         kSecAttrAccount as String: space]
    }

    static func store(_ password: String, space: String, home: URL) throws {
        let data = Data(password.utf8)
        let status = SecItemUpdate(query(space: space, home: home) as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var item = query(space: space, home: home)
            item[kSecValueData as String] = data
            item[kSecAttrLabel as String] = "YoBro Proxy · \(space)"
            item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            let added = SecItemAdd(item as CFDictionary, nil)
            guard added == errSecSuccess else { throw keychainError(added) }
        } else if status != errSecSuccess {
            throw keychainError(status)
        }
    }

    static func read(space: String, home: URL) throws -> String? {
        var item = query(space: space, home: home)
        item[kSecReturnData as String] = true
        item[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(item as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = result as? Data, let password = String(data: data, encoding: .utf8) else {
            throw keychainError(status)
        }
        return password
    }

    static func remove(space: String, home: URL) throws {
        let status = SecItemDelete(query(space: space, home: home) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw keychainError(status) }
    }

    private static func keychainError(_ status: OSStatus) -> YOBROError {
        .message(SecCopyErrorMessageString(status, nil) as String? ?? L("Schlüsselbundfehler", "Keychain error"))
    }
}

@MainActor
final class SpaceProxyStore: ObservableObject {
    @Published var configs: [String: SpaceProxyConfig] = [:]
    @Published var activeError: String?
    let home: URL
    private var file: URL { home.appendingPathComponent("space-proxies.json") }

    init(home: URL) {
        self.home = home
        if let data = try? Data(contentsOf: file),
           let saved = try? JSONDecoder().decode([String: SpaceProxyConfig].self, from: data) {
            configs = saved
            migrateStoredConfigurations()
        }
    }

    func config(for space: String) -> SpaceProxyConfig? {
        configs[space]
    }

    func set(_ config: SpaceProxyConfig?, for space: String) throws {
        if let config {
            if !config.password.isEmpty { try ProxySecrets.store(config.password, space: space, home: home) }
            var metadata = config
            metadata.password = ""
            configs[space] = metadata
        } else {
            try ProxySecrets.remove(space: space, home: home)
            configs.removeValue(forKey: space)
        }
        try persist()
        activeError = nil
    }

    func rename(from oldSpace: String, to newSpace: String) throws {
        if let config = configs[oldSpace] {
            if let password = try ProxySecrets.read(space: oldSpace, home: home) {
                try ProxySecrets.store(password, space: newSpace, home: home)
                try ProxySecrets.remove(space: oldSpace, home: home)
            }
            configs.removeValue(forKey: oldSpace)
            configs[newSpace] = config
            try persist()
        }
    }

    func resolvedConfig(_ config: SpaceProxyConfig, for space: String) throws -> SpaceProxyConfig {
        var resolved = config
        if resolved.password.isEmpty { resolved.password = try ProxySecrets.read(space: space, home: home) ?? "" }
        return resolved
    }

    func testConnection(_ config: SpaceProxyConfig, for space: String) async throws {
        let resolved = try resolvedConfig(config, for: space)
        let proxy = try resolved.makeNetworkProxyConfiguration()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        configuration.timeoutIntervalForResource = 20
        configuration.proxyConfigurations = [proxy]
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        var request = URLRequest(url: URL(string: "https://example.com/")!)
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        let (_, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse, (200...499).contains(http.statusCode) else {
            throw YOBROError.message(L("Der Proxy hat keine gültige Web-Antwort geliefert.", "The proxy did not return a valid web response."))
        }
    }

    private func persist() throws {
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try JSONEncoder().encode(configs).write(to: file, options: [.atomic, .completeFileProtection])
    }

    private func migrateStoredConfigurations() {
        var changed = false
        for space in Array(configs.keys) {
            guard var config = configs[space] else { continue }
            if !config.password.isEmpty {
                do {
                    try ProxySecrets.store(config.password, space: space, home: home)
                    config.password = ""
                    changed = true
                } catch {
                    activeError = error.localizedDescription
                    continue
                }
            }
            configs[space] = config
        }
        if changed {
            do { try persist() }
            catch { activeError = error.localizedDescription }
        }
    }

    /// Non-nil while this space is configured for a proxy that could not be
    /// applied. `BrowserModel` refuses to navigate in that state: clearing the
    /// proxy configuration alone would send the traffic out directly, which is
    /// the opposite of what the setting promises.
    @Published private(set) var isolationFailure: String?

    func apply(to store: WKWebsiteDataStore, for space: String) {
        if #available(macOS 14.0, *) {
            if let config = configs[space], config.enabled {
                do {
                    let proxy = try resolvedConfig(config, for: space).makeNetworkProxyConfiguration()
                    store.proxyConfigurations = [proxy]
                    activeError = nil
                    isolationFailure = nil
                } catch {
                    store.proxyConfigurations = []
                    activeError = error.localizedDescription
                    isolationFailure = L("Der Proxy für „\(space)“ konnte nicht aktiviert werden: \(error.localizedDescription)",
                                         "The proxy for “\(space)” could not be activated: \(error.localizedDescription)")
                }
            } else {
                store.proxyConfigurations = []
                isolationFailure = nil
            }
        }
    }
}
