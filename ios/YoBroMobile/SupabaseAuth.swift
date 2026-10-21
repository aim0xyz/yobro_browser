import Foundation
import Security
import CryptoKit

#if os(macOS)
import AppKit
#endif

struct SupabaseAuthSession: Codable, Sendable {
    var userID: UUID
    var email: String
    var accessToken: String
    var refreshToken: String
    var expiresAt: Date
}

/// The QR payload contains no account credential. `bootstrapSecret` expires in
/// ten minutes and can be redeemed once; `syncKey` only decrypts this profile.
struct DevicePairingCode: Codable, Equatable, Identifiable {
    let version: Int
    let profileID: UUID
    let bootstrapSecret: String
    let syncKey: String
    var id: String { bootstrapSecret }

    func encoded() throws -> String {
        let data = try JSONEncoder().encode(self)
        return "yobro-pair:" + data.base64EncodedString()
    }

    static func decode(_ value: String) throws -> Self {
        let raw = value.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "yobro-pair:", with: "")
        guard let data = Data(base64Encoded: raw), let result = try? JSONDecoder().decode(Self.self, from: data),
              result.version == 1, result.profileID.uuidString.isEmpty == false,
              Data(base64Encoded: result.bootstrapSecret) != nil,
              Data(base64Encoded: result.syncKey)?.count == 32 else {
            throw YOBROError.message(L("Dieser QR-Code ist ungültig.", "This QR code is invalid."))
        }
        return result
    }

    static func secret() -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess else {
            preconditionFailure("Secure random generator unavailable")
        }
        return Data(bytes).base64EncodedString()
    }
    static func hash(_ secret: String) -> String {
        let digest = SHA256.hash(data: Data(secret.utf8))
        return digest.map { String(format: "%02x", $0) }.joined()
    }
}

struct PairedBrowserDevice: Codable, Identifiable {
    let id: String
    let label: String
    let platform: String
    let last_used_at: Date
}

/// Optional override of the sync backend, read from `supabase.json`.
struct SupabaseProjectConfiguration: Codable {
    var url: String
    var publishableKey: String
}

struct SupabaseAuthClient: Sendable {
    /// The built-in project.
    ///
    /// A Supabase publishable key is designed to ship inside the client and is
    /// not a secret: anyone can read it out of the binary. What actually protects
    /// the data are the project's row level security policies. It is overridable
    /// so a separate test project, or a self-hosted one, can be used without
    /// rebuilding the app.
    private static let fallbackProjectURL = "https://bbjiumzwufgakgijixwr.supabase.co"
    private static let fallbackPublishableKey = "sb_publishable_VyAQG0hfzFI6d9rgznC1_g_Bu6NWESW"

    /// Resolution order: environment variables, then `supabase.json` in the
    /// application support folder, then the built-in project.
    private static let resolved: (url: URL, key: String) = {
        let environment = ProcessInfo.processInfo.environment
        var url = environment["YOBRO_SUPABASE_URL"]
        var key = environment["YOBRO_SUPABASE_KEY"]
        if url == nil || key == nil, let file = configurationFile,
           let stored = PersistedState.load(SupabaseProjectConfiguration.self, at: file).value {
            url = url ?? stored.url
            key = key ?? stored.publishableKey
        }
        // An override must be HTTPS with a host: access tokens are attached to
        // these requests, so a malformed value must never be used.
        guard let candidate = url, let parsed = URL(string: candidate),
              parsed.scheme?.lowercased() == "https", parsed.host != nil,
              let candidateKey = key, !candidateKey.isEmpty else {
            return (URL(string: fallbackProjectURL)!, fallbackPublishableKey)
        }
        return (parsed, candidateKey)
    }()

    /// The path is derived here rather than via `BrowserModel.defaultHome`, which
    /// is main-actor isolated and performs the legacy folder migration as a side
    /// effect. Reading a configuration file must do neither.
    private static var configurationFile: URL? {
        if let configured = ProcessInfo.processInfo.environment["YOBRO_HOME"] {
            return URL(fileURLWithPath: configured).appendingPathComponent("supabase.json")
        }
        guard let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return nil }
        return support.appendingPathComponent("YOBRO").appendingPathComponent("supabase.json")
    }

    static var projectURL: URL { resolved.url }
    static var publishableKey: String { resolved.key }
    static let callbackURL = "https://yobro.aimoxyz.xyz/auth/callback"
    static let recoveryURL = "https://yobro.aimoxyz.xyz/auth/reset-password"

    static func isAuthURL(_ url: URL) -> Bool {
        let path = url.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        if url.scheme?.lowercased() == "yobro", url.host?.lowercased() == "auth" {
            return ["callback", "reset-password"].contains(path)
        }
        return url.scheme?.lowercased() == "https" && url.user == nil && url.password == nil &&
            (url.port == nil || url.port == 443) &&
            ["yobro.aimoxyz.xyz", "yobro.lol"].contains(url.host?.lowercased() ?? "") &&
            ["auth/callback", "auth/reset-password"].contains(path)
    }

    static func isRecoveryURL(_ url: URL) -> Bool {
        isAuthURL(url) && (url.lastPathComponent == "reset-password" || authParameters(url)["type"] == "recovery")
    }

    private struct User: Decodable { let id: UUID; let email: String? }
    private struct Response: Decodable {
        let access_token: String?
        let refresh_token: String?
        let expires_in: Double?
        let user: User?
    }
    private struct Failure: Decodable { let msg: String?; let message: String?; let error_description: String? }

    var urlSession: URLSession = .shared

    func signIn(email: String, password: String) async throws -> SupabaseAuthSession {
        try await authenticate(path: "auth/v1/token?grant_type=password", body: ["email": email, "password": password])
    }

    func signUp(email: String, password: String) async throws -> SupabaseAuthSession? {
        let response = try await request(path: "auth/v1/signup?redirect_to=\(Self.callbackURL)", body: ["email": email, "password": password])
        guard response.access_token != nil else { return nil }
        return try session(from: response, fallbackEmail: email)
    }

    func refresh(_ session: SupabaseAuthSession) async throws -> SupabaseAuthSession {
        let response = try await request(path: "auth/v1/token?grant_type=refresh_token", body: ["refresh_token": session.refreshToken])
        return try self.session(from: response, fallbackEmail: session.email)
    }

    func requestPasswordReset(email: String) async throws {
        _ = try await request(path: "auth/v1/recover?redirect_to=\(Self.recoveryURL)", body: ["email": email])
    }

    func resendConfirmation(email: String) async throws {
        _ = try await request(path: "auth/v1/resend?redirect_to=\(Self.callbackURL)", body: ["type": "signup", "email": email])
    }

    /// Starts the passwordless flow. `create_user` deliberately remains true:
    /// the first verified code creates the account, while every later one signs
    /// the same person back in.
    func requestEmailOTP(email: String) async throws {
        try await send(path: "auth/v1/otp", body: ["email": email, "create_user": true])
    }

    func verifyEmailOTP(email: String, code: String) async throws -> SupabaseAuthSession {
        let response = try await request(path: "auth/v1/verify", body: ["email": email, "token": code, "type": "email"])
        return try session(from: response, fallbackEmail: email)
    }

    func session(fromAuthURL url: URL) async throws -> (SupabaseAuthSession, isRecovery: Bool) {
        guard Self.isAuthURL(url) else {
            throw YOBROError.message(L("Ungültiger Anmeldelink.", "Invalid authentication link."))
        }
        let parameters = Self.authParameters(url)
        if let error = parameters["error_description"] ?? parameters["error"] {
            throw YOBROError.message(error.replacingOccurrences(of: "+", with: " "))
        }
        if let hash = parameters["token_hash"], !hash.isEmpty {
            let type = Self.isRecoveryURL(url) ? "recovery" : (parameters["type"] ?? "signup")
            guard ["recovery", "signup", "email"].contains(type) else {
                throw YOBROError.message(L("Ungültiger Anmeldelink.", "Invalid authentication link."))
            }
            let response = try await request(path: "auth/v1/verify", body: ["token_hash": hash, "type": type])
            return (try session(from: response, fallbackEmail: ""), type == "recovery")
        }
        guard let access = parameters["access_token"], let refresh = parameters["refresh_token"] else {
            throw YOBROError.message(L("Der Anmeldelink ist unvollständig oder abgelaufen.", "The authentication link is incomplete or expired."))
        }
        let user = try await currentUser(accessToken: access)
        let seconds = Double(parameters["expires_in"] ?? "3600") ?? 3600
        return (SupabaseAuthSession(userID: user.id, email: user.email ?? "", accessToken: access, refreshToken: refresh,
                                    expiresAt: Date().addingTimeInterval(seconds)),
                Self.isRecoveryURL(url))
    }

    func updatePassword(_ password: String, accessToken: String) async throws {
        var request = URLRequest(url: Self.projectURL.appendingPathComponent("auth/v1/user"))
        request.httpMethod = "PUT"
        request.setValue(Self.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(["password": password])
        let (data, response) = try await urlSession.data(for: request)
        try validate(response: response, data: data, fallback: L("Passwort konnte nicht geändert werden.", "Password could not be changed."))
    }

    static func authParameters(_ url: URL) -> [String: String] {
        var result: [String: String] = [:]
        for source in [url.query, url.fragment].compactMap({ $0 }) {
            for item in URLComponents(string: "?" + source)?.queryItems ?? [] where result[item.name] == nil {
                result[item.name] = item.value ?? ""
            }
        }
        return result
    }

    func signOut(accessToken: String) async {
        var request = URLRequest(url: Self.projectURL.appendingPathComponent("auth/v1/logout"))
        request.httpMethod = "POST"
        request.setValue(Self.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        _ = try? await urlSession.data(for: request)
    }

    private func authenticate(path: String, body: [String: String]) async throws -> SupabaseAuthSession {
        let response = try await request(path: path, body: body)
        return try session(from: response, fallbackEmail: body["email"] ?? "")
    }

    private func request(path: String, body: [String: String]) async throws -> Response {
        guard let endpoint = URL(string: path, relativeTo: Self.projectURL.appendingPathComponent("/"))?.absoluteURL else {
            throw YOBROError.message(L("Ungültige Auth-Adresse.", "Invalid authentication URL."))
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue(Self.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(body)
        let (data, response) = try await urlSession.data(for: request)
        try validate(response: response, data: data, fallback: L("Anmeldung fehlgeschlagen.", "Sign-in failed."))
        return try JSONDecoder().decode(Response.self, from: data)
    }

    private func send(path: String, body: [String: Any]) async throws {
        guard let endpoint = URL(string: path, relativeTo: Self.projectURL.appendingPathComponent("/"))?.absoluteURL else {
            throw YOBROError.message(L("Ungültige Auth-Adresse.", "Invalid authentication URL."))
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue(Self.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, response) = try await urlSession.data(for: request)
        try validate(response: response, data: data, fallback: L("Code konnte nicht gesendet werden.", "Could not send code."))
    }

    private func currentUser(accessToken: String) async throws -> User {
        var request = URLRequest(url: Self.projectURL.appendingPathComponent("auth/v1/user"))
        request.setValue(Self.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        let (data, response) = try await urlSession.data(for: request)
        try validate(response: response, data: data, fallback: L("Anmeldedaten konnten nicht geprüft werden.", "Authentication data could not be verified."))
        return try JSONDecoder().decode(User.self, from: data)
    }

    private func validate(response: URLResponse, data: Data, fallback: String) throws {
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            let failure = try? JSONDecoder().decode(Failure.self, from: data)
            throw YOBROError.message(failure?.msg ?? failure?.message ?? failure?.error_description ?? fallback)
        }
    }

    private func session(from response: Response, fallbackEmail: String) throws -> SupabaseAuthSession {
        guard let user = response.user, let access = response.access_token, let refresh = response.refresh_token else {
            throw YOBROError.message(L("Unvollständige Anmeldeantwort.", "Incomplete authentication response."))
        }
        return SupabaseAuthSession(userID: user.id, email: user.email ?? fallbackEmail, accessToken: access,
                                   refreshToken: refresh, expiresAt: Date().addingTimeInterval(response.expires_in ?? 3600))
    }
}

enum BrowserSyncSecrets {
    private static let service = "xyz.aimo.yobro.sync"

    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: account]
    }

    static func store(_ data: Data, account: String) throws {
        let status = SecItemUpdate(query(account) as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var item = query(account)
            item[kSecValueData as String] = data
            item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            let added = SecItemAdd(item as CFDictionary, nil)
            guard added == errSecSuccess else { throw YOBROError.message(L("Schlüsselbundfehler: \(added)", "Keychain error: \(added)")) }
        } else if status != errSecSuccess {
            throw YOBROError.message(L("Schlüsselbundfehler: \(status)", "Keychain error: \(status)"))
        }
    }

    static func read(account: String) -> Data? {
        // XCTest executables have a different code-signing identity than the app.
        // Reusing a developer keychain entry there can open a modal authorization
        // prompt and deadlock the headless suite; tests use their explicit fixtures.
        if ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil ||
            CommandLine.arguments.first?.contains("xctest") == true ||
            NSClassFromString("XCTestCase") != nil { return nil }
        var item = query(account); item[kSecReturnData as String] = true; item[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        return SecItemCopyMatching(item as CFDictionary, &result) == errSecSuccess ? result as? Data : nil
    }

    static func remove(account: String) {
        SecItemDelete(query(account) as CFDictionary)
    }
}

