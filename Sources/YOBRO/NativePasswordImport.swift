import Foundation
import Security
import CommonCrypto

struct NativePasswordResult: Decodable {
    var passwords: [ImportPassword] = []
    var warnings: [String] = []
    var needsPrimaryPassword: Bool? = nil
}

struct EncryptedBrowserPassword: Decodable {
    let url: String
    let username: String
    let encrypted: Data
}

/// Reads only after the user requests password transfer. macOS owns all Keychain
/// authorization prompts; denial stops the operation. No ACLs are changed.
enum NativePasswordImport {
    static func load(browser: String, profile: String, primaryPassword: String = "") async throws -> NativePasswordResult {
        if browser == "Firefox" {
            return try await ImportWorker.call(["action": "firefox_passwords", "path": profile, "primaryPassword": primaryPassword], as: NativePasswordResult.self)
        }
        if browser == "Safari" {
            return try await Task.detached(priority: .userInitiated) { try safariPasswords() }.value
        }
        guard ["Chrome", "Brave", "Arc"].contains(browser) else { throw YOBROError.message(L("Dieser Browser wird nicht unterstützt.", "This browser is not supported.")) }
        let rows = try await ImportWorker.call(["action": "chromium_passwords", "path": profile], as: [EncryptedBrowserPassword].self)
        guard !rows.isEmpty else { return NativePasswordResult() }
        return try await Task.detached(priority: .userInitiated) {
            let secret = try safeStorage(browser: browser)
            let key = try deriveKey(secret)
            var result = NativePasswordResult()
            var unsupported = 0
            for row in rows {
                do {
                    let password = try decrypt(row.encrypted, key: key)
                    if !password.isEmpty { result.passwords.append(ImportPassword(url: row.url, username: row.username, password: password)) }
                } catch { unsupported += 1 }
            }
            if unsupported > 0 {
                result.warnings.append(L("\(unsupported) Passwörter konnten nicht entschlüsselt werden. Bitte diese Konten über einen CSV-Export übernehmen.", "Could not decrypt \(unsupported) passwords. Transfer these accounts using a CSV export."))
            }
            return result
        }.value
    }

    private static func safeStorage(browser: String) throws -> Data {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "\(browser) Safe Storage", kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else {
            if status == errSecUserCanceled || status == errSecAuthFailed || status == errSecInteractionNotAllowed {
                throw YOBROError.message(L("Passwortzugriff nicht freigegeben. Du kannst es erneut versuchen oder eine CSV-Datei verwenden.", "Password access was not granted. Try again or use a CSV file."))
            }
            throw YOBROError.message(L("Der Schlüsselbund-Eintrag für \(browser) ist nicht verfügbar. Öffne den Quellbrowser auf diesem Mac oder verwende einen CSV-Export.", "The Keychain entry for \(browser) is unavailable. Open the source browser on this Mac or use a CSV export."))
        }
        return data
    }

    static func deriveKey(_ password: Data) throws -> Data {
        let salt = Array("saltysalt".utf8)
        var output = [UInt8](repeating: 0, count: kCCKeySizeAES128)
        let status = password.withUnsafeBytes { bytes in
            CCKeyDerivationPBKDF(CCPBKDFAlgorithm(kCCPBKDF2), bytes.baseAddress?.assumingMemoryBound(to: CChar.self), password.count,
                                salt, salt.count, CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA1), 1003, &output, output.count)
        }
        guard status == kCCSuccess else { throw YOBROError.message(L("Passwortschlüssel konnte nicht gelesen werden.", "Could not read the password key.")) }
        return Data(output)
    }

    static func decrypt(_ encrypted: Data, key: Data) throws -> String {
        // Chromium macOS v10: PBKDF2-SHA1 / AES-128-CBC / PKCS#7.
        // Unknown formats are never treated as plaintext.
        guard key.count == kCCKeySizeAES128, encrypted.starts(with: Data("v10".utf8)),
              encrypted.count > 3, (encrypted.count - 3) % kCCBlockSizeAES128 == 0 else {
            throw YOBROError.message(L("Unbekanntes Passwortformat.", "Unknown password format."))
        }
        let payload = Data(encrypted.dropFirst(3))
        let iv = [UInt8](repeating: 0x20, count: kCCBlockSizeAES128)
        var output = [UInt8](repeating: 0, count: payload.count + kCCBlockSizeAES128)
        let capacity = output.count
        var length = 0
        let status = key.withUnsafeBytes { keyBytes in
            payload.withUnsafeBytes { bytes in
                CCCrypt(CCOperation(kCCDecrypt), CCAlgorithm(kCCAlgorithmAES), CCOptions(kCCOptionPKCS7Padding),
                        keyBytes.baseAddress, key.count, iv, bytes.baseAddress, payload.count, &output, capacity, &length)
            }
        }
        guard status == kCCSuccess, let text = String(bytes: output.prefix(length), encoding: .utf8) else {
            throw YOBROError.message(L("Passwort konnte nicht entschlüsselt werden.", "Could not decrypt password."))
        }
        return text
    }

    private static func safariPasswords() throws -> NativePasswordResult {
        var result = NativePasswordResult()
        // Local HTTP(S) credentials use the shared macOS Internet Password store.
        // Protected iCloud/Passwords-app access groups are not accessible to YOBRO.
        for proto in [kSecAttrProtocolHTTP, kSecAttrProtocolHTTPS] {
            let query: [String: Any] = [kSecClass as String: kSecClassInternetPassword,
                kSecAttrProtocol as String: proto, kSecMatchLimit as String: kSecMatchLimitAll,
                kSecReturnAttributes as String: true, kSecReturnPersistentRef as String: true]
            var matches: CFTypeRef?
            let status = SecItemCopyMatching(query as CFDictionary, &matches)
            if status == errSecItemNotFound { continue }
            guard status == errSecSuccess else { throw YOBROError.message(L("macOS hat den Zugriff auf Web-Passwörter nicht freigegeben. Verwende den Export aus der App „Passwörter“.", "macOS did not grant access to web passwords. Use an export from the Passwords app.")) }
            for item in matches as? [[String: Any]] ?? [] {
                guard let reference = item[kSecValuePersistentRef as String] as? Data,
                      let host = item[kSecAttrServer as String] as? String, !host.isEmpty else { continue }
                var components = URLComponents()
                components.scheme = proto == kSecAttrProtocolHTTPS ? "https" : "http"
                components.host = host
                if let port = item[kSecAttrPort as String] as? Int, port > 0 { components.port = port }
                guard let url = components.url else { continue }
                let lookup: [String: Any] = [kSecValuePersistentRef as String: reference, kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
                var value: CFTypeRef?
                let code = SecItemCopyMatching(lookup as CFDictionary, &value)
                guard code == errSecSuccess else {
                    throw YOBROError.message(L("Passwortzugriff abgebrochen oder nicht freigegeben. Verwende bei Bedarf einen CSV-Export.", "Password access was cancelled or not granted. Use a CSV export if needed."))
                }
                if let data = value as? Data, let password = String(data: data, encoding: .utf8), !password.isEmpty {
                    result.passwords.append(ImportPassword(url: url.absoluteString, username: item[kSecAttrAccount as String] as? String ?? "", password: password))
                }
            }
        }
        result.warnings.append(L("Nur zugängliche lokale macOS-Web-Passwörter wurden gelesen. Für iCloud und weitere Safari-Passwörter: App „Passwörter“ → Ablage → Alle Passwörter exportieren.", "Only accessible local macOS web passwords were read. For iCloud and other Safari passwords: Passwords app → File → Export All Passwords."))
        return result
    }
}
