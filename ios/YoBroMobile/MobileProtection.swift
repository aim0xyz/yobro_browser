import Foundation
import NetworkExtension
import Security

// Deliberately a small native blocklist, not an implementation of uBlock Origin.
enum MobileProtection {
    static let domains = ["doubleclick.net", "googlesyndication.com", "googleadservices.com", "google-analytics.com", "adnxs.com", "criteo.com", "taboola.com", "outbrain.com", "hotjar.com", "scorecardresearch.com"]
    static var rules: String {
        let entries: [[String: Any]] = domains.map { domain in
            ["trigger": ["url-filter": "^https?://([^/]+\\.)?" + domain.replacingOccurrences(of: ".", with: "\\.") + "[:/]", "load-type": ["third-party"]], "action": ["type": "block"]]
        }
        return String(data: try! JSONSerialization.data(withJSONObject: entries), encoding: .utf8)!
    }
    static func darkScript(enabled: Bool, excludedHosts: [String] = []) -> String {
        let hosts = String(data: try! JSONEncoder().encode(excludedHosts), encoding: .utf8)!
        return "var yobroDarkEnabled = !" + hosts + ".includes(location.hostname.toLowerCase()); " +
        "DarkReader.setFetchMethod((url) => fetch(url, {credentials: 'omit'})); DarkReader." + (enabled ? "disable(); if (yobroDarkEnabled) DarkReader.enable({brightness: 100, contrast: 90, sepia: 0});" : "disable();")
    }
}

@MainActor final class MobileVPN: ObservableObject {
    @Published var server = ""
    @Published var remoteID = ""
    @Published var username = ""
    @Published var password = ""
    @Published var status = "Nicht verbunden"
    @Published var connected = false
    @Published var busy = false
    private let manager = NEVPNManager.shared()
    private var observer: NSObjectProtocol?
    init() {
        observer = NotificationCenter.default.addObserver(forName: .NEVPNStatusDidChange, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.updateStatus() }
        }
    }
    deinit { if let observer { NotificationCenter.default.removeObserver(observer) } }
    func load() async {
        do {
            try await manager.loadFromPreferences()
            if let configuration = manager.protocolConfiguration as? NEVPNProtocolIKEv2 {
                server = configuration.serverAddress ?? ""; remoteID = configuration.remoteIdentifier ?? ""
                username = configuration.username ?? ""
            }
            updateStatus()
        } catch { status = "VPN nicht verfügbar: " + error.localizedDescription }
    }
    func connect() async {
        guard !busy else { return }
        guard !server.trimmingCharacters(in: .whitespaces).isEmpty, !remoteID.isEmpty, !username.isEmpty else {
            status = "Server, Remote-ID und IKEv2-Zugangsdaten fehlen."; return
        }
        busy = true; defer { busy = false }
        do {
            try await manager.loadFromPreferences()
            let reference: Data
            if password.isEmpty {
                guard let saved = manager.protocolConfiguration as? NEVPNProtocolIKEv2,
                      saved.serverAddress == server.trimmingCharacters(in: .whitespacesAndNewlines),
                      saved.remoteIdentifier == remoteID.trimmingCharacters(in: .whitespacesAndNewlines),
                      saved.username == username, let stored = saved.passwordReference else {
                    throw YOBROError.message("Bitte die VPN-Zugangsdaten einmal vollständig eingeben.")
                }
                reference = stored
            } else { reference = try passwordReference() }
            let configuration = NEVPNProtocolIKEv2()
            configuration.serverAddress = server.trimmingCharacters(in: .whitespacesAndNewlines)
            configuration.remoteIdentifier = remoteID.trimmingCharacters(in: .whitespacesAndNewlines)
            configuration.username = username
            configuration.authenticationMethod = .none
            configuration.useExtendedAuthentication = true
            configuration.passwordReference = reference
            configuration.disconnectOnSleep = false
            manager.protocolConfiguration = configuration
            manager.localizedDescription = "YoBro IKEv2"
            manager.isEnabled = true
            try await manager.saveToPreferences()
            try await manager.loadFromPreferences()
            try manager.connection.startVPNTunnel()
            password = ""; updateStatus()
        } catch { status = "VPN-Verbindung fehlgeschlagen: " + error.localizedDescription }
    }
    func disconnect() { manager.connection.stopVPNTunnel(); updateStatus() }
    private func updateStatus() {
        connected = manager.connection.status == .connected
        switch manager.connection.status {
        case .connected: status = "Verbunden"
        case .connecting: status = "Verbindung wird aufgebaut …"
        case .reasserting: status = "Verbindung wird wiederhergestellt …"
        case .disconnecting: status = "Verbindung wird getrennt …"
        case .invalid: status = "Noch kein VPN eingerichtet"
        default: status = "Nicht verbunden"
        }
    }
    private func passwordReference() throws -> Data {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "xyz.aimo.yobro.mobile.vpn", kSecAttrAccount as String: "ikev2"]
        let data = Data(password.utf8)
        let update = SecItemUpdate(query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if update == errSecItemNotFound {
            var entry = query; entry[kSecValueData as String] = data
            entry[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            let result = SecItemAdd(entry as CFDictionary, nil)
            guard result == errSecSuccess else { throw YOBROError.message("VPN-Schlüsselbundfehler: \(result)") }
        } else if update != errSecSuccess { throw YOBROError.message("VPN-Schlüsselbundfehler: \(update)") }
        var lookup = query; lookup[kSecReturnPersistentRef as String] = true
        var result: CFTypeRef?
        let code = SecItemCopyMatching(lookup as CFDictionary, &result)
        guard code == errSecSuccess, let reference = result as? Data else { throw YOBROError.message("VPN-Passwort konnte nicht gespeichert werden.") }
        return reference
    }
}
