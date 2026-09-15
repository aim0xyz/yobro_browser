import SwiftUI
import WebKit
import AppKit
import Security
import UniformTypeIdentifiers

struct BookmarkEntry: Codable, Identifiable, Equatable {
    var id = UUID()
    var title: String
    var url: String
    var folder: String
}
struct ImportProfile: Decodable, Identifiable { let id: String; let browser: String; let name: String; let path: String }
struct ImportLink: Decodable { var title: String; var url: String; var folder: String?; var timestamp: Double?; var visits: Int?; var pinned: Bool?; var space: String?; var tabFolderID: String? }
struct ImportSpace: Decodable { let sourceID: String; let name: String }
struct ImportTabFolder: Decodable { let sourceID: String; let name: String; let space: String }
struct ImportPassword: Codable { let url: String; let username: String; let password: String }
struct ImportCookie: Decodable { let name: String; let value: String; let domain: String; let path: String; let expires: Double; let secure: Bool; let httpOnly: Bool; var sameSite: String? = nil }
struct ImportPreview: Decodable {
    var bookmarks: [ImportLink] = []; var history: [ImportLink] = []; var tabs: [ImportLink] = []
    var spaces: [ImportSpace] = []; var tabFolders: [ImportTabFolder] = []
    var passwords: [ImportPassword] = []; var cookies: [ImportCookie] = []; var warnings: [String] = []
    var availability: [String: String]? = nil
    var detectedPasswords: Int? = nil
    var count: Int { bookmarks.count + history.count + tabs.count + passwords.count + cookies.count }
    mutating func append(_ other: Self) {
        bookmarks += other.bookmarks; history += other.history; tabs += other.tabs
        spaces += other.spaces; tabFolders += other.tabFolders
        passwords += other.passwords; cookies += other.cookies; warnings += other.warnings
    }
    func count(_ kind: ImportKind) -> Int {
        switch kind { case .bookmarks: return bookmarks.count; case .history: return history.count; case .tabs: return tabs.count; case .passwords: return passwords.count; case .cookies: return cookies.count }
    }
}
enum ImportKind: String, CaseIterable, Identifiable {
    case bookmarks, history, tabs, passwords, cookies
    var id: String { rawValue }
    var title: String {
        switch self { case .bookmarks: return L("Lesezeichen"); case .history: return L("Verlauf"); case .tabs: return L("Offene Tabs"); case .passwords: return L("Passwörter"); case .cookies: return "Cookies / Logins" }
    }
    var format: String {
        switch self { case .bookmarks: return L("Lesezeichen-HTML, Chromium-JSON oder Safari-Plist"); case .history: return L("JSON mit URL, Titel und Unix-Zeitstempel; auch Chrome-Takeout"); case .tabs: return L("URL-Liste (TXT), Tab-JSON oder Firefox JSONLZ4"); case .passwords: return L("Passwort-CSV aus Safari, Arc, Brave, Chrome oder Firefox"); case .cookies: return L("Netscape-Cookie-TXT oder Cookie-JSON; Logins können trotzdem eine neue Anmeldung verlangen") }
    }
}

// Worker output is kept in memory; imported secrets are never written into logs or YOBRO JSON files.
enum ImportWorker {
    /// Absolute interpreter paths, preferring the system Python.
    ///
    /// This worker receives plaintext secrets: the Firefox primary password goes
    /// in and decrypted logins come back. Resolving the interpreter through
    /// `/usr/bin/env python3` meant an inherited `PATH` decided which binary saw
    /// them, so a planted `python3` earlier in `PATH` would have received every
    /// password.
    private static let interpreters = ["/usr/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3"]

    private static var interpreter: String? {
        interpreters.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    /// A fixed environment for the same reason: no inherited `PATH`, and no
    /// `PYTHONPATH`/`PYTHONHOME` that could inject modules into the worker.
    private static var environment: [String: String] {
        var value = ProcessInfo.processInfo.environment
        value["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        value["PYTHONPATH"] = nil
        value["PYTHONHOME"] = nil
        value["PYTHONSTARTUP"] = nil
        value["YOBRO_LANGUAGE"] = YOBROLanguage.identifier
        return value
    }

    static func call<T: Decodable>(_ request: [String: Any], as: T.Type) async throws -> T {
        let input = try JSONSerialization.data(withJSONObject: request)
        guard let script = Bundle.module.url(forResource: "BrowserImport", withExtension: "py") else { throw YOBROError.message(L("Importmodul fehlt.")) }
        guard let python = interpreter else {
            throw YOBROError.message(L("Für den Import wird Python 3 benötigt. Installiere die Xcode-Befehlszeilenwerkzeuge oder Python 3.",
                                       "The import needs Python 3. Install the Xcode command line tools or Python 3."))
        }
        let environment = Self.environment
        let data = try await Task.detached(priority: .userInitiated) {
            let process = Process(); process.environment = environment; process.executableURL = URL(fileURLWithPath: python); process.arguments = [script.path]
            let stdin = Pipe(), stdout = Pipe(), stderr = Pipe()
            process.standardInput = stdin; process.standardOutput = stdout; process.standardError = stderr
            try process.run()
            stdin.fileHandleForWriting.write(input); try stdin.fileHandleForWriting.close()
            let output = stdout.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
            guard process.terminationStatus == 0, !output.isEmpty else { throw YOBROError.message(L("Import konnte nicht gestartet werden. Python 3 wird benötigt.")) }
            return output
        }.value
        let envelope = try JSONDecoder().decode(WorkerEnvelope<T>.self, from: data)
        guard let result = envelope.result, envelope.ok else { throw YOBROError.message(envelope.error ?? L("Import fehlgeschlagen.")) }
        return result
    }
    private struct WorkerEnvelope<T: Decodable>: Decodable { let ok: Bool; let result: T?; let error: String? }
}

@MainActor
final class BrowserImportStore: ObservableObject {
    @Published var profiles: [ImportProfile] = []
    @Published var browser = "Safari"
    @Published var profileID = ""
    @Published var selected: Set<ImportKind> = [.bookmarks, .history]
    @Published var preview = ImportPreview()
    @Published var busy = false
    @Published var message: String?
    @Published var replaceCookies = false
    @Published var replacePasswords = false
    @Published var passwordMessage: String?
    @Published var needsPrimaryPassword = false
    @Published var passwordsLoaded = false
    @Published var completed = false
    @Published var files: [ImportKind: String] = [:]
    private var filePreviews: [ImportKind: ImportPreview] = [:]
    private var nativePasswords: [ImportPassword] = []
    private var basePreview = ImportPreview()
    private var nativeWarnings: [String] = []
    private var revision = UUID()
    var matchingProfiles: [ImportProfile] { profiles.filter { $0.browser == browser } }
    var selectedCount: Int { selected.reduce(0) { $0 + preview.count($1) } }
    var canUnlockPasswords: Bool { browser == "Safari" || !profileID.isEmpty }

    private func rebuildPreview() {
        preview = basePreview
        for kind in ImportKind.allCases {
            if let extra = filePreviews[kind] { preview.append(extra) }
        }
        preview.passwords += nativePasswords
        preview.warnings += nativeWarnings
    }

    func discover() async {
        guard !busy, profiles.isEmpty else { return }
        busy = true
        do {
            var request: [String: Any] = ["action": "profiles"]
            if let isolated = ProcessInfo.processInfo.environment["YOBRO_HOME"] { request["home"] = isolated }
            profiles = try await ImportWorker.call(request, as: [ImportProfile].self)
            profileID = matchingProfiles.first?.id ?? ""
        } catch { message = error.localizedDescription }
        busy = false
        await readProfile()
    }

    func resetSource() {
        revision = UUID(); profileID = matchingProfiles.first?.id ?? ""
        clearPreview(); message = nil
        Task { await readProfile() }
    }

    private func clearPreview() {
        preview = ImportPreview(); basePreview = ImportPreview(); filePreviews = [:]
        files = [:]; nativePasswords = []; nativeWarnings = []
        passwordsLoaded = false; passwordMessage = nil; needsPrimaryPassword = false; completed = false
    }

    func selectProfile(_ value: String) {
        guard !busy, profileID != value else { return }
        profileID = value; revision = UUID(); clearPreview()
        Task { await readProfile() }
    }

    func chooseFolder() {
        guard !busy else { return }
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true
        panel.message = L("Profilordner von \(browser) wählen.", "Choose the profile folder for \(browser).")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let profile = ImportProfile(id: url.path, browser: browser, name: url.lastPathComponent, path: url.path)
        profiles.removeAll { $0.id == profile.id }; profiles.append(profile)
        selectProfile(profile.id)
    }

    func readProfile() async {
        guard let profile = profiles.first(where: { $0.id == profileID }), !busy else { return }
        let token = UUID(); revision = token
        busy = true; message = nil; completed = false; defer { busy = false }
        do {
            let result = try await ImportWorker.call(["action": "profile", "browser": profile.browser, "path": profile.path, "kinds": ImportKind.allCases.map(\.rawValue)], as: ImportPreview.self)
            if revision == token { basePreview = result; rebuildPreview() }
        } catch { if revision == token { message = error.localizedDescription } }
    }

    func chooseFile(_ kind: ImportKind) {
        guard !busy else { return }
        let panel = NSOpenPanel(); panel.message = kind.format
        panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        if kind == .passwords { panel.allowedContentTypes = [.commaSeparatedText, .plainText] }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await loadFile(kind, url: url) }
    }

    func loadFile(_ kind: ImportKind, url: URL) async {
        guard !busy else { return }
        busy = true; completed = false; defer { busy = false }
        do {
            let value = try await ImportWorker.call(["action": "file", "kind": kind.rawValue, "path": url.path], as: ImportPreview.self)
            filePreviews[kind] = value; files[kind] = url.lastPathComponent
            rebuildPreview(); selected.insert(kind); message = nil
        } catch { message = error.localizedDescription }
    }

    func unlockPasswords(primaryPassword: String = "") async {
        guard !busy, canUnlockPasswords else { return }
        busy = true; passwordMessage = nil; needsPrimaryPassword = false; completed = false
        defer { busy = false }
        do {
            let result = try await NativePasswordImport.load(browser: browser, profile: profileID, primaryPassword: primaryPassword)
            if result.needsPrimaryPassword == true {
                needsPrimaryPassword = true
                passwordMessage = L("Firefox benötigt dein Hauptpasswort.", "Firefox requires your Primary Password.")
                return
            }
            nativePasswords = result.passwords; nativeWarnings = result.warnings
            passwordsLoaded = true; selected.insert(.passwords); rebuildPreview()
            passwordMessage = result.passwords.isEmpty
                ? L("Keine direkt zugänglichen Passwörter gefunden. Wähle ein anderes Profil oder nutze den CSV-Export.", "No directly accessible passwords found. Choose another profile or use a CSV export.")
                : L("\(result.passwords.count) Passwörter bereit zur Übernahme.", "\(result.passwords.count) passwords ready to transfer.")
        } catch { passwordMessage = error.localizedDescription }
    }
    func apply(to model: BrowserModel) async {
        guard !busy, selectedCount > 0 else { return }
        busy = true; completed = false; defer { busy = false }
        var summary: [String] = []; var failures: [String] = []
        if selected.contains(.bookmarks) || selected.contains(.history) {
            do {
                let counts = try model.mergeImport(bookmarks: selected.contains(.bookmarks) ? preview.bookmarks : [], history: selected.contains(.history) ? preview.history : [])
                summary.append(L("\(counts.0) neue Lesezeichen, \(counts.1) neue Verlaufsadressen", "\(counts.0) new bookmarks, \(counts.1) new history URLs"))
            } catch { failures.append(error.localizedDescription) }
        }
        if selected.contains(.passwords) {
            var count = 0, skipped = 0
            for password in preview.passwords {
                do { if try PasswordVault.store(password, home: model.home, replace: replacePasswords) { count += 1 } else { skipped += 1 } }
                catch { failures.append(L("Passwort konnte nicht im Schlüsselbund gespeichert werden: \(error.localizedDescription)", "Could not save password in Keychain: \(error.localizedDescription)")); break }
            }
            summary.append(L("\(count) Passwörter gespeichert, \(skipped) bestehende übersprungen", "\(count) passwords saved, \(skipped) existing passwords skipped"))
        }
        if selected.contains(.cookies) {
            let store = model.websiteDataStore.httpCookieStore
            let existing = await store.allCookies()
            var keys = Set(existing.map { "\($0.domain)|\($0.path)|\($0.name)" }); var count = 0, skipped = 0
            for item in preview.cookies {
                guard !item.name.isEmpty, !item.domain.isEmpty, item.path.hasPrefix("/"), item.expires == 0 || item.expires > Date().timeIntervalSince1970 else { skipped += 1; continue }
                var properties: [HTTPCookiePropertyKey: Any] = [.name: item.name, .value: item.value, .domain: item.domain, .path: item.path, .secure: item.secure ? "TRUE" : "FALSE"]
                if item.expires > 0 { properties[.expires] = Date(timeIntervalSince1970: item.expires) }
                if item.httpOnly { properties[HTTPCookiePropertyKey("HttpOnly")] = "TRUE" }
                if let sameSite = item.sameSite, ["lax", "strict", "none"].contains(sameSite) { properties[.sameSitePolicy] = sameSite }
                guard let cookie = HTTPCookie(properties: properties) else { skipped += 1; continue }
                let key = "\(cookie.domain)|\(cookie.path)|\(cookie.name)"
                if keys.contains(key) && !replaceCookies { skipped += 1; continue }
                await store.setCookie(cookie); keys.insert(key); count += 1
            }
            summary.append(L("\(count) Cookies importiert, \(skipped) übersprungen", "\(count) cookies imported, \(skipped) skipped"))
        }
        if selected.contains(.tabs) {
            let counts = model.mergeImportedTabs(preview.tabs, spaces: preview.spaces, folders: preview.tabFolders)
            summary.append(L("\(counts.tabs) Tabs, \(counts.spaces) Spaces und \(counts.folders) Ordner hinzugefügt", "\(counts.tabs) tabs, \(counts.spaces) spaces, and \(counts.folders) folders added"))
        }
        message = (failures.isEmpty ? L("Import abgeschlossen. ") : L("Import teilweise abgeschlossen. ")) + summary.joined(separator: " · ") + (failures.isEmpty ? "" : "\n" + failures.joined(separator: "\n"))
        if failures.isEmpty { clearPreview(); completed = true }
    }
}

extension BrowserModel {
    func mergeImportedTabs(_ incoming: [ImportLink], spaces incomingSpaces: [ImportSpace], folders incomingFolders: [ImportTabFolder]) -> (tabs: Int, spaces: Int, folders: Int) {
        func cleaned(_ value: String, fallback: String, limit: Int) -> String {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            return String((trimmed.isEmpty ? fallback : trimmed).prefix(limit))
        }
        func existingName(_ candidate: String, in values: [String]) -> String? {
            values.first { $0.localizedCaseInsensitiveCompare(candidate) == .orderedSame }
        }
        func uniqueName(_ candidate: String, in values: [String], limit: Int) -> String {
            if let existing = existingName(candidate, in: values) { return existing }
            var suffix = 2
            while true {
                let addition = " \(suffix)"
                let base = String(candidate.prefix(max(1, limit - addition.count)))
                let value = suffix == 2 ? candidate : base + addition
                if existingName(value, in: values) == nil { return value }
                suffix += 1
            }
        }

        var spaceMap: [String: String] = [:]
        var addedSpaces = 0
        for incomingSpace in incomingSpaces {
            let candidate = cleaned(incomingSpace.name, fallback: L("Arc Space"), limit: 40)
            let resolved: String
            if let existing = existingName(candidate, in: spaces) { resolved = existing }
            else {
                resolved = uniqueName(candidate, in: spaces + Array(spaceMap.values), limit: 40)
                spaces.append(resolved); addedSpaces += 1
            }
            spaceMap[incomingSpace.sourceID] = resolved
        }

        var folderMap: [String: UUID] = [:]
        var addedFolders = 0
        for incomingFolder in incomingFolders {
            let targetSpace = spaceMap[incomingFolder.space] ?? space
            let name = cleaned(incomingFolder.name, fallback: L("Arc-Ordner", "Arc folder"), limit: 60)
            if let existing = folders.first(where: { $0.space == targetSpace && $0.name.localizedCaseInsensitiveCompare(name) == .orderedSame }) {
                folderMap[incomingFolder.sourceID] = existing.id
            } else {
                let folder = TabFolder(name: name, space: targetSpace)
                folders.append(folder); folderMap[incomingFolder.sourceID] = folder.id; addedFolders += 1
            }
        }

        var addresses = Set(tabs.map { "\($0.space)\u{0}\($0.url)" })
        var addedTabs = 0
        // Restored tabs load only on selection to avoid issuing hundreds of requests on import.
        for entry in incoming {
            let targetSpace = entry.space.flatMap { spaceMap[$0] } ?? space
            guard addresses.insert("\(targetSpace)\u{0}\(entry.url)").inserted else { continue }
            let saved = StoredTab(id: UUID(), title: entry.title, url: entry.url, space: targetSpace, pinned: entry.pinned ?? false, folderID: entry.tabFolderID.flatMap { folderMap[$0] })
            let tab = BrowserTab(saved: saved, owner: self, deferLoading: true)
            tabs.append(tab)
            if #available(macOS 15.4, *) { extensions.runtime.controller.didOpenTab(tab) }
            addedTabs += 1
        }
        persistSession()
        return (addedTabs, addedSpaces, addedFolders)
    }

    func mergeImport(bookmarks incoming: [ImportLink], history visits: [ImportLink]) throws -> (Int, Int) {
        var bookmarks = self.bookmarks, history = self.history
        var bookmarkKeys = Set(bookmarks.map { "\($0.folder)|\($0.url)" })
        var addedBookmarks = 0, addedHistory = 0
        for entry in incoming {
            let folder = entry.folder ?? L("Importiert")
            if bookmarkKeys.insert("\(folder)|\(entry.url)").inserted { bookmarks.append(BookmarkEntry(title: entry.title, url: entry.url, folder: folder)); addedBookmarks += 1 }
        }
        var indices = Dictionary(history.enumerated().map { ($1.url, $0) }, uniquingKeysWith: { a, _ in a })
        for entry in visits {
            let date = Date(timeIntervalSince1970: entry.timestamp ?? 0)
            if let index = indices[entry.url] {
                history[index].visits = max(history[index].visits, entry.visits ?? 1)
                if date > history[index].date { history[index].date = date; history[index].title = entry.title }
            } else { indices[entry.url] = history.count; history.append(HistoryEntry(title: entry.title, url: entry.url, date: date, visits: entry.visits ?? 1)); addedHistory += 1 }
        }
        history.sort { $0.date > $1.date }
        // Persist each selected category before publishing it. Existing entries are never cleared.
        if !incoming.isEmpty { try JSONEncoder().encode(bookmarks).write(to: home.appendingPathComponent("bookmarks.json"), options: .atomic); self.bookmarks = bookmarks }
        if !visits.isEmpty { try JSONEncoder().encode(history).write(to: home.appendingPathComponent("history.json"), options: .atomic); self.history = history }
        if !incoming.isEmpty || !visits.isEmpty { markSyncChanged() }
        return (addedBookmarks, addedHistory)
    }
}

enum PasswordVault {
    static func service(home: URL) -> String { "YOBRO.Passwords." + home.path }
    static func store(_ entry: ImportPassword, home: URL, replace: Bool) throws -> Bool {
        guard let url = URL(string: entry.url), let host = url.host, ["http", "https"].contains(url.scheme ?? "") else { return false }
        let account = "\(url.scheme!)://\(host):\(url.port ?? (url.scheme == "https" ? 443 : 80))|\(entry.username)"
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service(home: home), kSecAttrAccount as String: account]
        // Every other secret in YOBRO (mail, proxy, VPN, sync) is stored
        // ThisDeviceOnly. Without this the login items were the only ones that
        // could travel into a backup or to another Mac.
        let value: [String: Any] = [
            kSecValueData as String: try JSONEncoder().encode(entry),
            kSecAttrLabel as String: "YoBro · \(host) · \(entry.username)",
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        ]
        let status = SecItemAdd(query.merging(value) { _, new in new } as CFDictionary, nil)
        if status == errSecDuplicateItem {
            guard replace else { return false }
            let updated = SecItemUpdate(query as CFDictionary, value as CFDictionary)
            guard updated == errSecSuccess else { throw YOBROError.message(SecCopyErrorMessageString(updated, nil) as String? ?? L("Schlüsselbundfehler")) }
        } else if status != errSecSuccess { throw YOBROError.message(SecCopyErrorMessageString(status, nil) as String? ?? L("Schlüsselbundfehler")) }
        return true
    }
    static func entries(home: URL, origin: String? = nil) throws -> [ImportPassword] {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service(home: home)]
        let query = base.merging([kSecReturnAttributes as String: true, kSecMatchLimit as String: kSecMatchLimitAll]) { _, new in new }
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return [] }
        guard status == errSecSuccess else { throw YOBROError.message(SecCopyErrorMessageString(status, nil) as String? ?? L("Schlüsselbundfehler")) }
        return try (result as? [[String: Any]] ?? []).compactMap { attributes in
            guard let account = attributes[kSecAttrAccount as String] as? String else { return nil }
            if let origin {
                let storedOrigin = account.components(separatedBy: "|").first.flatMap(URL.init(string:)).flatMap(LoginAutofill.origin)
                guard storedOrigin == origin else { return nil }
            }
            let lookup = base.merging([kSecAttrAccount as String: account, kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]) { _, new in new }
            var value: CFTypeRef?
            let code = SecItemCopyMatching(lookup as CFDictionary, &value)
            guard code == errSecSuccess, let data = value as? Data else { throw YOBROError.message(SecCopyErrorMessageString(code, nil) as String? ?? L("Schlüsselbundfehler")) }
            return try JSONDecoder().decode(ImportPassword.self, from: data)
        }
    }
}
