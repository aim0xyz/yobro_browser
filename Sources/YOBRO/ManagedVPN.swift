import Foundation
import Network
import Security
import WebKit

struct ManagedVPNLocation: Identifiable, Equatable, Codable {
    let id: String
    let countryCode: String
    let city: String
    let flag: String
    let expectedExitIP: String

    var title: String { "\(flag) \(L(countryCode == "CA" ? "Kanada" : countryCode, countryCode == "CA" ? "Canada" : countryCode))" }
    var subtitle: String { city }
}

enum ManagedVPNStatus: Equatable {
    case disconnected
    case connecting
    case connected(ManagedVPNLocation)
    case failed(String)
}

enum ManagedVPNSecrets {
    static func service(home: URL) -> String { "local.yobro.managed-vpn." + home.path }
    private static func account(locationID: String) -> String { "\(locationID).yobro-app-v3" }

    static func store(_ key: String, locationID: String, home: URL) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service(home: home),
            kSecAttrAccount as String: account(locationID: locationID)
        ]
        let attributes: [String: Any] = [kSecValueData as String: Data(key.utf8)]
        let updated = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if updated == errSecItemNotFound {
            var item = query
            item.merge(attributes) { _, new in new }
            item[kSecAttrLabel as String] = "YoBro VPN · \(locationID)"
            item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            let added = SecItemAdd(item as CFDictionary, nil)
            guard added == errSecSuccess else { throw keychainError(added) }
        } else if updated != errSecSuccess {
            throw keychainError(updated)
        }
    }

    static func read(locationID: String, home: URL) throws -> String {
        if ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil ||
            CommandLine.arguments.first?.contains("xctest") == true ||
            NSClassFromString("XCTestCase") != nil {
            throw YOBROError.message("Managed VPN credentials are unavailable in tests")
        }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service(home: home),
            kSecAttrAccount as String: account(locationID: locationID),
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data,
              let key = String(data: data, encoding: .utf8), !key.isEmpty else {
            if status == errSecItemNotFound {
                throw YOBROError.message(L("Der VPN-Zugang ist auf diesem Gerät noch nicht eingerichtet.", "VPN access has not been provisioned on this device."))
            }
            throw keychainError(status)
        }
        return key
    }

    private static func keychainError(_ status: OSStatus) -> YOBROError {
        .message(SecCopyErrorMessageString(status, nil) as String? ?? L("Schlüsselbundfehler", "Keychain error"))
    }
}

@MainActor
final class ManagedVPNStore: ObservableObject {
    static let localHost = "127.0.0.1"
    static let localPort: UInt16 = 17890

    // Provisioned on the user's device, never compiled into a distributable app.
    @Published private(set) var locations: [ManagedVPNLocation]
    let home: URL
    @Published private(set) var status: ManagedVPNStatus = .disconnected
    @Published private(set) var selectedLocationID: String?
    private var process: Process?
    private var generation = UUID()
    private var selectionFile: URL { home.appendingPathComponent("managed-vpn.json") }

    var isActive: Bool { selectedLocationID != nil }
    var isConnected: Bool {
        if case .connected = status { return true }
        return false
    }

    /// Non-nil while a location is selected but traffic is not actually going
    /// through it. A failed `restore` used to leave the selection in place while
    /// neither the VPN nor the space proxy was applied, so the browser quietly
    /// went out over the direct connection.
    var isolationFailure: String? {
        guard isActive, !isConnected else { return nil }
        let detail: String
        if case .failed(let message) = status { detail = message }
        else { detail = L("Die Verbindung wird noch aufgebaut.", "The connection is still being established.") }
        return L("Das VPN ist ausgewählt, aber nicht verbunden: \(detail)",
                 "The VPN is selected but not connected: \(detail)")
    }
    var connectedLocation: ManagedVPNLocation? {
        if case .connected(let location) = status { return location }
        return nil
    }

    init(home: URL) {
        self.home = home
        locations = (try? Data(contentsOf: home.appendingPathComponent("managed-vpn-locations.json")))
            .flatMap { try? JSONDecoder().decode([ManagedVPNLocation].self, from: $0) } ?? []
        if let data = try? Data(contentsOf: selectionFile),
           let saved = try? JSONDecoder().decode([String: String].self, from: data),
           let id = saved["location"] {
            selectedLocationID = id
        }
    }

    func restore(to dataStore: WKWebsiteDataStore) async {
        guard let selectedLocationID else { return }
        guard let location = locations.first(where: { $0.id == selectedLocationID }) else {
            status = .failed(L("Die lokale VPN-Konfiguration fehlt. Richte sie erneut ein oder schalte das VPN ausdrücklich aus.", "Local VPN configuration is missing. Set it up again or explicitly turn the VPN off."))
            return
        }
        do { try await connect(location, to: dataStore) }
        catch { status = .failed(error.localizedDescription) }
    }

    func provision(city: String, countryCode: String, expectedExitIP: String, accessKey: String) throws {
        let city = city.trimmingCharacters(in: .whitespacesAndNewlines)
        let country = countryCode.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        let ip = expectedExitIP.trimmingCharacters(in: .whitespacesAndNewlines)
        let key = accessKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !city.isEmpty, city.count <= 60, country.range(of: "^[A-Z]{2}$", options: .regularExpression) != nil,
              IPv4Address(ip) != nil || IPv6Address(ip) != nil,
              key.hasPrefix("ss://"), key.count > 10, key.count < 8192, !key.contains("\n"), !key.contains("\r") else {
            throw YOBROError.message(L("Prüfe Namen, zweistelligen Ländercode, Ausgangs-IP und Outline-Zugangsschlüssel.", "Check the name, two-letter country code, exit IP, and Outline access key."))
        }
        let location = ManagedVPNLocation(id: UUID().uuidString, countryCode: country, city: city, flag: "🌐", expectedExitIP: ip)
        try ManagedVPNSecrets.store(key, locationID: location.id, home: home)
        let updated = locations + [location]
        let file = home.appendingPathComponent("managed-vpn-locations.json")
        try JSONEncoder().encode(updated).write(to: file, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        locations = updated
    }

    func connect(_ location: ManagedVPNLocation, to dataStore: WKWebsiteDataStore) async throws {
        do {
            try await startConnection(location, to: dataStore)
        } catch {
            shutdownProcess()
            status = .failed(error.localizedDescription)
            throw error
        }
    }

    private func startConnection(_ location: ManagedVPNLocation, to dataStore: WKWebsiteDataStore) async throws {
        let key = try ManagedVPNSecrets.read(locationID: location.id, home: home)
        shutdownProcess()
        let currentGeneration = UUID()
        generation = currentGeneration
        status = .connecting

        guard let helper = Bundle.module.url(forResource: "http2transport-yobro", withExtension: nil) else {
            throw YOBROError.message(L("Der Outline-VPN-Helfer fehlt in dieser YoBro-Version.", "The Outline VPN helper is missing from this YoBro build."))
        }
        let input = Pipe()
        let child = Process()
        child.executableURL = helper
        child.standardInput = input
        child.standardOutput = FileHandle.nullDevice
        child.standardError = FileHandle.nullDevice
        try child.run()
        process = child
        input.fileHandleForWriting.write(Data((key + "\n").utf8))
        try? input.fileHandleForWriting.close()

        let proxy = try localProxyConfiguration()
        let verified = try await verify(location: location, proxy: proxy, process: child)
        guard generation == currentGeneration, child.isRunning else {
            throw YOBROError.message(L("Die VPN-Verbindung wurde unterbrochen.", "The VPN connection was interrupted."))
        }
        guard verified else {
            shutdownProcess()
            throw YOBROError.message(L("Der Server antwortet nicht mit dem erwarteten Standort und der erwarteten IP.", "The server did not return the expected location and IP."))
        }
        dataStore.proxyConfigurations = [proxy]
        selectedLocationID = location.id
        try persistSelection()
        status = .connected(location)
    }

    func disconnect(from dataStore: WKWebsiteDataStore) {
        generation = UUID()
        shutdownProcess()
        dataStore.proxyConfigurations = []
        selectedLocationID = nil
        try? FileManager.default.removeItem(at: selectionFile)
        status = .disconnected
    }

    func reapply(to dataStore: WKWebsiteDataStore) {
        guard isConnected, let proxy = try? localProxyConfiguration() else { return }
        dataStore.proxyConfigurations = [proxy]
    }

    func shutdown() { shutdownProcess() }

    private func localProxyConfiguration() throws -> ProxyConfiguration {
        guard let port = NWEndpoint.Port(rawValue: Self.localPort) else {
            throw YOBROError.message(L("Ungültiger lokaler VPN-Port.", "Invalid local VPN port."))
        }
        return ProxyConfiguration(httpCONNECTProxy: .hostPort(host: .init(Self.localHost), port: port))
    }

    /// Total time budget for confirming the tunnel. `BrowserModel.init` awaits
    /// this before it loads the restored tabs, so a broken helper must not be
    /// able to hold the whole browser hostage. Twenty four-second attempts used
    /// to add up to roughly 85 seconds of a blank window.
    private static let verificationBudget: TimeInterval = 12

    private func verify(location: ManagedVPNLocation, proxy: ProxyConfiguration, process: Process) async throws -> Bool {
        var lastError: Error?
        let deadline = Date().addingTimeInterval(Self.verificationBudget)
        while Date() < deadline {
            guard process.isRunning else {
                throw YOBROError.message(L("Der Outline-VPN-Helfer wurde unerwartet beendet.", "The Outline VPN helper exited unexpectedly."))
            }
            let configuration = URLSessionConfiguration.ephemeral
            configuration.timeoutIntervalForRequest = 3
            configuration.timeoutIntervalForResource = 4
            configuration.proxyConfigurations = [proxy]
            let session = URLSession(configuration: configuration)
            defer { session.invalidateAndCancel() }
            do {
                let (data, response) = try await session.data(from: URL(string: "https://www.cloudflare.com/cdn-cgi/trace")!)
                if let http = response as? HTTPURLResponse, http.statusCode == 200,
                   let body = String(data: data, encoding: .utf8),
                   body.contains("loc=\(location.countryCode)"), body.contains("ip=\(location.expectedExitIP)") {
                    return true
                }
            } catch { lastError = error }
            try await Task.sleep(for: .milliseconds(250))
        }
        if let lastError { throw lastError }
        return false
    }

    /// `terminate()` alone only asks the helper to stop. Without waiting, the
    /// process outlived YOBRO and kept the local port bound, so the next launch
    /// could not start the tunnel.
    private func shutdownProcess() {
        if let process, process.isRunning {
            process.terminate()
            let deadline = Date().addingTimeInterval(2)
            while process.isRunning, Date() < deadline { usleep(50_000) }
            if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        }
        process = nil
    }

    private func persistSelection() throws {
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let data = try JSONEncoder().encode(["location": selectedLocationID ?? ""])
        try data.write(to: selectionFile, options: [.atomic, .completeFileProtection])
    }
}
