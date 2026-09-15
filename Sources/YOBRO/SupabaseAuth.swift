import Foundation
import Security
import AppKit

struct SupabaseAuthSession: Codable, Sendable {
    var userID: UUID
    var email: String
    var accessToken: String
    var refreshToken: String
    var expiresAt: Date
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

    func session(fromAuthURL url: URL) async throws -> (SupabaseAuthSession, isRecovery: Bool) {
        guard url.scheme?.lowercased() == "yobro", url.host?.lowercased() == "auth" else {
            throw YOBROError.message(L("Ungültiger Anmeldelink.", "Invalid authentication link."))
        }
        let parameters = Self.authParameters(url)
        if let error = parameters["error_description"] ?? parameters["error"] {
            throw YOBROError.message(error.replacingOccurrences(of: "+", with: " "))
        }
        guard let access = parameters["access_token"], let refresh = parameters["refresh_token"] else {
            throw YOBROError.message(L("Der Anmeldelink ist unvollständig oder abgelaufen.", "The authentication link is incomplete or expired."))
        }
        let user = try await currentUser(accessToken: access)
        let seconds = Double(parameters["expires_in"] ?? "3600") ?? 3600
        return (SupabaseAuthSession(userID: user.id, email: user.email ?? "", accessToken: access, refreshToken: refresh,
                                    expiresAt: Date().addingTimeInterval(seconds)),
                parameters["type"] == "recovery" || url.path == "/reset-password")
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

@MainActor
final class BrowserSyncStore: ObservableObject {
    @Published private(set) var signedIn = false
    @Published private(set) var email = ""
    @Published private(set) var busy = false
    @Published private(set) var status = L("Nicht verbunden", "Not connected")
    @Published private(set) var recoveryCode: String?
    @Published private(set) var needsNewPassword = false

    private struct Metadata: Codable { var revision: Int64? }
    private let profileID: UUID
    private let home: URL
    private let auth = SupabaseAuthClient()
    private var session: SupabaseAuthSession?
    private var keyData: Data?
    private var revision: Int64?
    private var scheduled: Task<Void, Never>?
    private var applyingRemote = false
    private var syncing = false

    private var sessionAccount: String { "\(home.path)|\(profileID.uuidString)|session" }
    private var keyAccount: String { "\(home.path)|\(profileID.uuidString)|encryption-key" }
    private var metadataURL: URL { home.appendingPathComponent("sync-metadata.json") }

    init(home: URL, profileID: UUID) {
        self.home = home; self.profileID = profileID
        if let data = BrowserSyncSecrets.read(account: sessionAccount), let value = try? JSONDecoder().decode(SupabaseAuthSession.self, from: data) {
            session = value; email = value.email
        }
        keyData = BrowserSyncSecrets.read(account: keyAccount)
        signedIn = session != nil && keyData != nil
        if signedIn { status = L("Verbunden", "Connected") }
        if let data = try? Data(contentsOf: metadataURL) { revision = (try? JSONDecoder().decode(Metadata.self, from: data))?.revision }
    }

    func signIn(email: String, password: String, recoveryCode: String, model: BrowserModel) async {
        await connect(email: email, password: password, recoveryCode: recoveryCode, model: model)
    }

    func signUp(email: String, password: String, model: BrowserModel) async {
        guard !busy else { return }; busy = true; defer { busy = false }
        do {
            if let value = try await auth.signUp(email: email, password: password) {
                try saveSession(value); try ensureKey(from: "")
                signedIn = true
                recoveryCode = encodeKey(keyData!)
                status = L("Konto erstellt und verbunden. Wiederherstellungscode sicher aufbewahren.", "Account created and connected. Store the recovery code safely.")
                await synchronize(model)
            } else {
                self.email = email
                status = L("Bestätige die E-Mail und melde dich danach an.", "Confirm the email, then sign in.")
            }
        } catch { status = error.localizedDescription }
    }

    func requestPasswordReset(email: String) async {
        guard !busy else { return }
        let address = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !address.isEmpty else {
            status = L("Gib zuerst deine E-Mail-Adresse ein.", "Enter your email address first.")
            return
        }
        busy = true; defer { busy = false }
        do {
            try await auth.requestPasswordReset(email: address)
            self.email = address
            status = L("Falls ein Konto existiert, wurde eine E-Mail zum Zurücksetzen gesendet.", "If an account exists, a password reset email was sent.")
        } catch { status = error.localizedDescription }
    }

    func resendConfirmation(email: String) async {
        guard !busy else { return }
        let address = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !address.isEmpty else {
            status = L("Gib zuerst deine E-Mail-Adresse ein.", "Enter your email address first.")
            return
        }
        busy = true; defer { busy = false }
        do {
            try await auth.resendConfirmation(email: address)
            self.email = address
            status = L("Bestätigungs-E-Mail erneut gesendet.", "Confirmation email sent again.")
        } catch { status = error.localizedDescription }
    }

    func handleAuthURL(_ url: URL, model: BrowserModel) async {
        do {
            let (value, recovery) = try await auth.session(fromAuthURL: url)
            try saveSession(value)
            if recovery {
                signedIn = false; needsNewPassword = true
                status = L("Wähle jetzt ein neues Passwort.", "Choose a new password now.")
            } else {
                try ensureKey(from: "")
                signedIn = true; needsNewPassword = false
                recoveryCode = encodeKey(keyData!)
                status = L("E-Mail bestätigt. Dein Konto ist verbunden.", "Email confirmed. Your account is connected.")
                await synchronize(model)
            }
            model.settingsSection = L("Sync", "Sync"); model.showSettings = true
            NSApp.activate(ignoringOtherApps: true)
        } catch {
            status = error.localizedDescription
            model.settingsSection = L("Sync", "Sync"); model.showSettings = true
        }
    }

    func updatePassword(_ password: String, model: BrowserModel) async {
        guard password.count >= 8, let session else {
            status = L("Das neue Passwort muss mindestens 8 Zeichen haben.", "The new password must be at least 8 characters.")
            return
        }
        busy = true; defer { busy = false }
        do {
            try await auth.updatePassword(password, accessToken: session.accessToken)
            try ensureKey(from: "")
            signedIn = true; needsNewPassword = false
            if revision == nil { recoveryCode = encodeKey(keyData!) }
            status = L("Passwort geändert. Dein Konto ist wieder verbunden.", "Password changed. Your account is connected again.")
            await synchronize(model)
        } catch { status = error.localizedDescription }
    }

    private func connect(email: String, password: String, recoveryCode: String, model: BrowserModel) async {
        guard !busy else { return }; busy = true; defer { busy = false }
        do {
            let value = try await auth.signIn(email: email, password: password)
            try saveSession(value)
            let service = service(for: value)
            let remote = try await service.fetch(profileID: profileID)
            if keyData == nil && remote != nil && recoveryCode.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                signedIn = false
                status = L("Gib den Wiederherstellungscode deines ersten Geräts ein.", "Enter the recovery code from your first device.")
                return
            }
            if keyData == nil, let remote {
                let candidate = try decodeKey(recoveryCode)
                _ = try BrowserSyncCipher.open(remote.payload, keyData: candidate)
                try BrowserSyncSecrets.store(candidate, account: keyAccount); keyData = candidate
            } else {
                try ensureKey(from: recoveryCode)
            }
            signedIn = true
            if remote == nil { self.recoveryCode = encodeKey(keyData!) }
            await synchronize(model, prefetched: remote)
        } catch { status = error.localizedDescription }
    }

    func restoreAndSync(_ model: BrowserModel) async {
        guard signedIn else { return }
        await synchronize(model)
    }

    func schedule(_ model: BrowserModel) {
        guard signedIn, !applyingRemote else { return }
        scheduled?.cancel()
        scheduled = Task { [weak self, weak model] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard !Task.isCancelled, let self, let model else { return }
            await self.synchronize(model)
        }
    }

    func syncNow(_ model: BrowserModel) async { await synchronize(model) }

    func signOut() async {
        scheduled?.cancel()
        if let session { await auth.signOut(accessToken: session.accessToken) }
        BrowserSyncSecrets.remove(account: sessionAccount)
        session = nil; signedIn = false; email = ""; revision = nil; recoveryCode = nil; needsNewPassword = false
        try? FileManager.default.removeItem(at: metadataURL)
        status = L("Abgemeldet. Der Verschlüsselungsschlüssel bleibt sicher auf diesem Mac.", "Signed out. The encryption key remains safely on this Mac.")
    }

    func revealRecoveryCode() {
        guard let keyData else { return }
        recoveryCode = encodeKey(keyData)
    }

    private func synchronize(_ model: BrowserModel, prefetched: RemoteSyncRecord? = nil) async {
        guard !syncing, var activeSession = session, let keyData else { return }
        syncing = true
        let wasBusy = busy
        if !wasBusy { busy = true }
        defer { syncing = false; if !wasBusy { busy = false } }
        do {
            if activeSession.expiresAt.timeIntervalSinceNow < 60 {
                activeSession = try await auth.refresh(activeSession); try saveSession(activeSession)
            }
            let coordinator = BrowserSyncCoordinator(service: service(for: activeSession), keyData: keyData)
            let remote = if let prefetched { (try BrowserSyncCipher.open(prefetched.payload, keyData: keyData), prefetched.revision) }
                         else { try await coordinator.pull(profileID: profileID) }
            let local = model.syncSnapshot(profileID: profileID)
            if let (remoteSnapshot, remoteRevision) = remote {
                if revision != remoteRevision {
                    // Archives are always merged; only the open tabs follow one
                    // side, so a losing comparison can no longer erase history or
                    // bookmarks.
                    let resolution: BrowserModel.SyncTabResolution =
                        local.isInitialEmpty || Self.remoteIsNewer(remoteSnapshot, than: local) ? .adoptRemote : .keepLocal
                    applyingRemote = true
                    defer { applyingRemote = false }
                    try model.mergeSyncSnapshot(remoteSnapshot, tabs: resolution)
                    // Push the union back so the other device receives it too.
                    let pushed = try await coordinator.push(model.syncSnapshot(profileID: profileID), expectedRevision: remoteRevision)
                    revision = pushed.revision
                    try saveMetadata()
                    status = L("Cloud-Daten zusammengeführt · \(Date().formatted(date: .omitted, time: .shortened))", "Cloud data merged · \(Date().formatted(date: .omitted, time: .shortened))")
                    return
                }
                let pushed = try await coordinator.push(local, expectedRevision: remoteRevision)
                revision = pushed.revision
            } else {
                let pushed = try await coordinator.push(local, expectedRevision: nil)
                revision = pushed.revision
            }
            try saveMetadata()
            status = L("Synchronisiert · \(Date().formatted(date: .omitted, time: .shortened))", "Synced · \(Date().formatted(date: .omitted, time: .shortened))")
        } catch {
            applyingRemote = false
            status = L("Sync fehlgeschlagen: ", "Sync failed: ") + error.localizedDescription
        }
    }

    /// Decides whose open tabs win, using the two client clocks.
    ///
    /// A timestamp noticeably in the future is not trusted: otherwise a Mac whose
    /// clock runs ahead would win every comparison forever, and its tab set would
    /// permanently override every other device.
    nonisolated static func remoteIsNewer(_ remote: BrowserSyncSnapshot, than local: BrowserSyncSnapshot, now: Date = Date()) -> Bool {
        let tolerance: TimeInterval = 300
        guard remote.modifiedAt <= now.addingTimeInterval(tolerance) else { return false }
        return remote.modifiedAt > local.modifiedAt
    }

    private func service(for session: SupabaseAuthSession) -> SupabaseSyncService {
        SupabaseSyncService(configuration: .init(projectURL: SupabaseAuthClient.projectURL, publishableKey: SupabaseAuthClient.publishableKey,
                                                  userID: session.userID, accessToken: session.accessToken))
    }

    private func saveSession(_ value: SupabaseAuthSession) throws {
        try BrowserSyncSecrets.store(JSONEncoder().encode(value), account: sessionAccount)
        session = value; email = value.email
    }

    private func ensureKey(from code: String) throws {
        if keyData != nil { return }
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)
        let value: Data
        if trimmed.isEmpty { value = BrowserSyncCipher.makeKey() }
        else { value = try decodeKey(trimmed) }
        try BrowserSyncSecrets.store(value, account: keyAccount); keyData = value
    }

    private func decodeKey(_ code: String) throws -> Data {
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)
        let base64 = trimmed.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/") + String(repeating: "=", count: (4 - trimmed.count % 4) % 4)
        guard let decoded = Data(base64Encoded: base64), decoded.count == BrowserSyncCipher.keyByteCount else {
            throw YOBROError.message(L("Der Wiederherstellungscode ist ungültig.", "The recovery code is invalid."))
        }
        return decoded
    }

    private func encodeKey(_ data: Data) -> String {
        data.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
    }

    private func saveMetadata() throws {
        try JSONEncoder().encode(Metadata(revision: revision)).write(to: metadataURL, options: [.atomic, .completeFileProtection])
    }
}
