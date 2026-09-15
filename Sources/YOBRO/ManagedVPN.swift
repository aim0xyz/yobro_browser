import Foundation
import Network
import Security
import WebKit

struct ManagedVPNLocation: Identifiable, Equatable {
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

    let locations = [
        ManagedVPNLocation(id: "ca-montreal", countryCode: "CA", city: "Montréal", flag: "🇨🇦", expectedExitIP: "15.175.21.24")
    ]
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
        if let data = try? Data(contentsOf: selectionFile),
           let saved = try? JSONDecoder().decode([String: String].self, from: data),
           let id = saved["location"], locations.contains(where: { $0.id == id }) {
            selectedLocationID = id
        } else if let location = locations.first,
                  let bootstrapPath = ProcessInfo.processInfo.environment["YOBRO_VPN_BOOTSTRAP_FIFO"] {
            let bootstrapURL = URL(fileURLWithPath: bootstrapPath)
            defer { try? FileManager.default.removeItem(at: bootstrapURL) }
            do {
                let handle = try FileHandle(forReadingFrom: bootstrapURL)
                let data = try handle.readToEnd() ?? Data()
                try handle.close()
                guard let key = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
                      key.hasPrefix("ss://") else {
                    throw YOBROError.message("Invalid Outline bootstrap payload")
                }
                try ManagedVPNSecrets.store(key, locationID: location.id, home: home)
                selectedLocationID = location.id
                try? "STORED".write(toFile: "/private/tmp/yobro-vpn-bootstrap.status", atomically: true, encoding: .utf8)
            } catch {
                try? "ERROR \(error.localizedDescription)".write(toFile: "/private/tmp/yobro-vpn-bootstrap.status", atomically: true, encoding: .utf8)
            }
        }
    }

    func restore(to dataStore: WKWebsiteDataStore) async {
        guard let selectedLocationID,
              let location = locations.first(where: { $0.id == selectedLocationID }) else { return }
        do { try await connect(location, to: dataStore) }
        catch { status = .failed(error.localizedDescription) }
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
            throw YOBROError.message(L("Der Server antwortet, aber nicht mit der erwarteten kanadischen IP.", "The server responded without the expected Canadian IP."))
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
