import SwiftUI
import Security
import AppKit
import UserNotifications

struct MailAccount: Codable, Identifiable, Hashable {
    var id = UUID()
    var label = ""
    var address = ""
    var username = ""
    var imapHost = ""
    var imapPort = 993
    var smtpHost = ""
    var smtpPort = 465
    var smtpSecurity = "tls"
    var smtpUsername: String?
}

struct MailItem: Identifiable, Hashable {
    let accountID: UUID
    var folder = "INBOX"
    let validity: String
    let uid: String
    let subject: String
    let sender: String
    let recipient: String
    let replyTo: String
    let date: Date
    var unread: Bool
    let messageID: String
    var id: String { "\(accountID):\(folder):\(validity):\(uid)" }
}

struct MailAttachment: Identifiable, Decodable { let id: String; let name: String; let size: Int; let mime: String }
struct MailBody { let text: String; var html = ""; let attachments: [MailAttachment] }
struct MailFolder: Identifiable, Decodable {
    let path: String; let title: String; let delimiter: String; let selectable: Bool; var unread: Int; let role: String?
    var id: String { path }
    var displayTitle: String { path.uppercased() == "INBOX" ? "Posteingang" : title }
    var isTrash: Bool { role == "trash" || ["trash", "papierkorb", "gelöscht", "gelöschte elemente", "deleted items"].contains(title.lowercased()) }
}
struct MailPreferences: Codable {
    var notifications = false
    var knownInbox: [String: Set<String>] = [:]
    var remoteImages = true

    private enum CodingKeys: String, CodingKey { case notifications, knownInbox, remoteImages }
    init() {}
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        notifications = try values.decodeIfPresent(Bool.self, forKey: .notifications) ?? false
        knownInbox = try values.decodeIfPresent([String: Set<String>].self, forKey: .knownInbox) ?? [:]
        remoteImages = try values.decodeIfPresent(Bool.self, forKey: .remoteImages) ?? true
    }
}

final class MailCredentialSession {
    private var values: [UUID: Result<String, Error>] = [:]
    private let lock = NSRecursiveLock()
    func read(_ id: UUID, load: () throws -> String) throws -> String {
        lock.lock(); defer { lock.unlock() }
        if let cached = values[id] { return try cached.get() }
        let result = Result(catching: load); values[id] = result
        return try result.get()
    }
    func set(_ value: String, id: UUID) { lock.lock(); defer { lock.unlock() }; values[id] = .success(value) }
    func remove(_ id: UUID) { lock.lock(); defer { lock.unlock() }; values[id] = nil }
    func retryFailures() {
        lock.lock(); defer { lock.unlock() }
        values = values.filter { if case .success = $0.value { return true }; return false }
    }
}

enum MailSecretError: LocalizedError {
    case accessRequired
    var errorDescription: String? { L("Mail-Zugriff gesperrt. Bitte dieses Postfach über „Mail entsperren“ freigeben.") }
}

enum MailSecrets {
    static let session = MailCredentialSession()
    static func query(_ id: UUID) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "local.yobro.mail", kSecAttrAccount as String: id.uuidString]
    }
    static func store(_ password: String, id: UUID) throws {
        let data = Data(password.utf8)
        let status = SecItemUpdate(query(id) as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var item = query(id); item[kSecValueData as String] = data
            item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            let added = SecItemAdd(item as CFDictionary, nil)
            guard added == errSecSuccess else { throw YOBROError.message(L("Schlüsselbund: \(added)", "Keychain: \(added)")) }
        } else if status != errSecSuccess { throw YOBROError.message(L("Schlüsselbund: \(status)", "Keychain: \(status)")) }
        session.set(password, id: id)
    }
    static func read(_ id: UUID) throws -> String { try session.read(id) { try readKeychain(id) } }
    static func unlock(_ id: UUID) throws {
        let password = try readKeychain(id, interactive: true)
        session.set(password, id: id)
    }
    // These existing mail items live in the legacy login Keychain. Its UI policy
    // is process-wide, so keep the synchronous operation short and restore it.
    static func withoutAuthenticationUI<T>(_ operation: () throws -> T) throws -> T {
        var previous: DarwinBoolean = true
        guard SecKeychainGetUserInteractionAllowed(&previous) == errSecSuccess,
              SecKeychainSetUserInteractionAllowed(false) == errSecSuccess else { throw MailSecretError.accessRequired }
        defer { SecKeychainSetUserInteractionAllowed(previous.boolValue) }
        return try operation()
    }
    private static func readKeychain(_ id: UUID, interactive: Bool = false) throws -> String {
        if !interactive { return try withoutAuthenticationUI { try readKeychain(id, interactive: true) } }
        var item = query(id); item[kSecReturnData as String] = true; item[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(item as CFDictionary, &result)
        if [errSecInteractionNotAllowed, errSecAuthFailed, errSecUserCanceled].contains(status) { throw MailSecretError.accessRequired }
        guard status == errSecSuccess, let data = result as? Data, let value = String(data: data, encoding: .utf8) else { throw YOBROError.message(L("Zugangsdaten nicht verfügbar. Konto bearbeiten und erneut verbinden.")) }
        return value
    }
    static func remove(_ id: UUID) throws {
        let status = SecItemDelete(query(id) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw YOBROError.message(L("Schlüsselbund: \(status)", "Keychain: \(status)")) }
        session.remove(id)
    }
}

enum MailTransport {
    static func run(action: String, account: MailAccount, password: String, extra: [String: Any] = [:]) async throws -> [String: Any] {
        var request = extra
        request["action"] = action; request["password"] = password
        request["account"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(account))
        let payload = try JSONSerialization.data(withJSONObject: request)
        guard let script = Bundle.main.url(forResource: "MailWorker", withExtension: "py") ?? Bundle.module.url(forResource: "MailWorker", withExtension: "py") else { throw YOBROError.message("Mail-Komponente fehlt.") }
        let output = try await Task.detached(priority: .userInitiated) { () throws -> Data in
            let candidates = ["/usr/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3"]
            guard let runtime = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else { throw YOBROError.message(L("Für die Mail-Vorschau wird Python 3 benötigt. Bitte Python 3 installieren.")) }
            let process = Process(); process.environment = ProcessInfo.processInfo.environment.merging(["YOBRO_LANGUAGE": YOBROLanguage.identifier]) { _, language in language }; process.executableURL = URL(fileURLWithPath: runtime); process.arguments = [script.path]
            let input = Pipe(), output = Pipe()
            process.standardInput = input; process.standardOutput = output; process.standardError = FileHandle.nullDevice
            try process.run()
            let timeout = DispatchWorkItem { if process.isRunning { process.terminate() } }
            DispatchQueue.global().asyncAfter(deadline: .now() + 90, execute: timeout)
            defer { timeout.cancel() }
            try input.fileHandleForWriting.write(contentsOf: payload); try input.fileHandleForWriting.close()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else { throw YOBROError.message(action == "send" ? L("Versandstatus unklar. Vor erneutem Senden beim Anbieter prüfen, ob die Nachricht angekommen ist.") : L("Mail-Verbindung beendet oder Zeitlimit erreicht.")) }
            return data
        }.value
        guard let response = try JSONSerialization.jsonObject(with: output) as? [String: Any] else { throw YOBROError.message(L("Ungültige Mail-Antwort.")) }
        guard response["ok"] as? Bool == true else { throw YOBROError.message(response["error"] as? String ?? "Mail-Verbindung fehlgeschlagen.") }
        return response["result"] as? [String: Any] ?? [:]
    }
}

@MainActor
final class MailStore: ObservableObject {
    nonisolated static let pollingIntervalNanoseconds: UInt64 = 5 * 60 * 1_000_000_000
    @Published var accounts: [MailAccount] = []
    @Published var items: [MailItem] = []
    @Published var folders: [UUID: [MailFolder]] = [:]
    @Published var selectedFolder = "INBOX"
    @Published var folderLoading = false
    @Published var lockedAccounts: Set<UUID> = []
    @Published var unlockingAccount: UUID?
    @Published var notificationsEnabled = false
    @Published var remoteImagesEnabled = true
    @Published var attachmentDownloads: Set<String> = []
    @Published var folderTotals: [String: Int] = [:]
    private var preferences = MailPreferences()
    private var preferenceFile: URL
    private var polling: Task<Void, Never>?
    private var readIDs: Set<String> = []
    private var loadingIDs: Set<String> = []
    private var folderRevision = UUID()
    private var limit = 100
    var notificationOpened: ((UUID) -> Void)?
    @Published var bodies: [String: MailBody] = [:]
    @Published var errors: [UUID: String] = [:]
    @Published var refreshing = false
    @Published var connecting = false
    @Published var sending = false
    @Published var organizing = false
    @Published var selectedAccount: UUID?
    @Published var selectedID: String?
    @Published var search = ""
    @Published var unreadOnly = false
    @Published var lastRefresh: Date?
    @Published var notice: String?
    @Published var draftAccount: UUID?
    @Published var draftTo = ""
    @Published var draftSubject = ""
    @Published var draftText = ""
    @Published var draftRTF: Data?
    @Published var draftReplyID = ""
    private let file: URL

    typealias Request = (String, MailAccount, [String: Any]) async throws -> [String: Any]
    private let request: Request
    init(home: URL, request: @escaping Request = { action, account, extra in
        try await MailTransport.run(action: action, account: account, password: MailSecrets.read(account.id), extra: extra)
    }) {
        self.request = request
        file = home.appendingPathComponent("mail-accounts.json")
        preferenceFile = home.appendingPathComponent("mail-preferences.json")
        if let data = try? Data(contentsOf: preferenceFile), let value = try? JSONDecoder().decode(MailPreferences.self, from: data) {
            preferences = value; notificationsEnabled = value.notifications; remoteImagesEnabled = value.remoteImages
        }
        // A damaged account list must not be replaced by an empty one: the next
        // `persist` would drop every configured mailbox.
        let stored = PersistedState.load([MailAccount].self, at: file)
        if let saved = stored.value { accounts = saved }
        if let problem = stored.problem { notice = problem }
    }
    var filtered: [MailItem] {
        items.filter { (selectedAccount == nil ? $0.folder == "INBOX" : $0.accountID == selectedAccount && $0.folder == selectedFolder) && (!unreadOnly || $0.unread || $0.id == selectedID) && (search.isEmpty || "\($0.subject) \($0.sender)".localizedCaseInsensitiveContains(search)) }
            .sorted { $0.date > $1.date }
    }
    var selected: MailItem? { items.first { $0.id == selectedID } }
    func persist(_ value: [MailAccount]) throws { try JSONEncoder().encode(value).write(to: file, options: [.atomic, .completeFileProtection]) }
    func connect(_ account: MailAccount, password: String) async throws {
        guard !connecting && !refreshing && !sending else { throw YOBROError.message(L("Bitte den laufenden Vorgang abwarten.")) }
        connecting = true; defer { connecting = false }
        _ = try await MailTransport.run(action: "test", account: account, password: password)
        try MailSecrets.store(password, id: account.id)
        var updated = accounts.filter { $0.id != account.id }; updated.append(account)
        try persist(updated); accounts = updated
        items.removeAll { $0.accountID == account.id }; bodies = [:]
    }
    func disconnect(_ account: MailAccount) throws {
        guard !refreshing && !connecting && !sending else { throw YOBROError.message(L("Bitte den laufenden Vorgang abwarten.")) }
        try MailSecrets.remove(account.id)
        let updated = accounts.filter { $0.id != account.id }
        try persist(updated); accounts = updated
        items.removeAll { $0.accountID == account.id }; bodies = [:]; errors[account.id] = nil
        folders[account.id] = nil; lockedAccounts.remove(account.id)
        if selectedAccount == account.id { selectedAccount = nil; selectedFolder = "INBOX"; selectedID = nil }
    }
    var hasUnread: Bool { accounts.contains { hasUnread($0.id) } }
    func hasUnread(_ account: UUID) -> Bool {
        if let known = folders[account] { return known.contains { $0.unread > 0 } }
        return items.contains { $0.accountID == account && $0.unread }
    }
    func persistPreferences() {
        do { try JSONEncoder().encode(preferences).write(to: preferenceFile, options: .atomic) }
        catch { notice = L("Mail-Einstellungen konnten nicht gespeichert werden.") }
    }
    func setNotifications(_ enabled: Bool) async {
        if enabled {
            do {
                guard try await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) else {
                    notice = L("Mitteilungen sind in macOS nicht erlaubt. Du kannst YoBro unter Systemeinstellungen → Mitteilungen freigeben."); return
                }
            } catch { notice = error.localizedDescription; return }
        } else {
            UNUserNotificationCenter.current().removeAllPendingNotificationRequests()
            UNUserNotificationCenter.current().removeAllDeliveredNotifications()
        }
        notificationsEnabled = enabled; preferences.notifications = enabled; persistPreferences()
    }
    func setRemoteImages(_ enabled: Bool) {
        remoteImagesEnabled = enabled; preferences.remoteImages = enabled; persistPreferences()
    }
    func stopPolling() { polling?.cancel(); polling = nil }

    func startPolling() {
        guard polling == nil else { return }
        polling = Task { [weak self] in
            while !Task.isCancelled {
                if let self, !self.accounts.isEmpty { await self.refresh() }
                do { try await Task.sleep(nanoseconds: Self.pollingIntervalNanoseconds) } catch { break }
            }
        }
    }
    func selectFolder(account: UUID?, folder: String = "INBOX") {
        selectedAccount = account; selectedFolder = folder; selectedID = nil; limit = 100
        folderRevision = UUID(); let revision = folderRevision
        guard let account = accounts.first(where: { $0.id == account }) else { return }
        Task {
            folderLoading = true
            defer { if revision == folderRevision { folderLoading = false } }
            do { try await fetchFolder(account, folder: folder, limit: 100) }
            catch { errors[account.id] = error.localizedDescription }
        }
    }
    func loadMore() async {
        limit = min(5000, limit + 100)
        guard let account = accounts.first(where: { $0.id == selectedAccount }) else { return }
        folderLoading = true; defer { folderLoading = false }
        do { try await fetchFolder(account, folder: selectedFolder, limit: limit) }
        catch { errors[account.id] = error.localizedDescription }
    }
    func fetchFolder(_ account: MailAccount, folder: String, limit: Int) async throws {
        let response = try await request("list", account, ["folder": folder, "limit": limit])
        guard accounts.contains(where: { $0.id == account.id }) else { return }
        let validity = response["validity"] as? String ?? ""
        let loaded = (response["messages"] as? [[String: Any]] ?? []).compactMap { row -> MailItem? in
            guard let uid = row["uid"] as? String else { return nil }
            var item = MailItem(accountID: account.id, folder: folder, validity: validity, uid: uid, subject: row["subject"] as? String ?? "", sender: row["sender"] as? String ?? "", recipient: row["to"] as? String ?? "", replyTo: row["replyTo"] as? String ?? "", date: Date(timeIntervalSince1970: row["date"] as? Double ?? 0), unread: row["unread"] as? Bool ?? false, messageID: row["messageID"] as? String ?? "")
            if !item.unread { readIDs.remove(item.id) }
            if readIDs.contains(item.id) { item.unread = false }
            return item
        }
        if folder.uppercased() == "INBOX" {
            let key = account.id.uuidString
            let ids = Set(loaded.map(\.id))
            if let known = preferences.knownInbox[key], notificationsEnabled {
                let new = Self.newInboxItems(loaded, known: known)
                if !new.isEmpty {
                    let content = UNMutableNotificationContent()
                    content.title = account.label.isEmpty ? account.address : account.label
                    content.body = new.count == 1 ? "\(new[0].sender)\n\(new[0].subject)" : L("\(new.count) neue E-Mails", "\(new.count) new emails")
                    content.sound = .default; content.userInfo = ["account": key]
                    do { try await UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "yobro-mail-" + UUID().uuidString, content: content, trigger: nil)) }
                    catch { errors[account.id] = L("Mitteilung konnte nicht angezeigt werden: \(error.localizedDescription)", "Could not show notification: \(error.localizedDescription)") }
                }
            }
            preferences.knownInbox[key] = ids.union((preferences.knownInbox[key] ?? []).filter { $0.contains(":INBOX:\(validity):") }); persistPreferences()
        }
        items.removeAll { $0.accountID == account.id && $0.folder == folder }; items.append(contentsOf: loaded)
        folderTotals["\(account.id):\(folder)"] = response["total"] as? Int ?? loaded.count
    }
    nonisolated static func newInboxItems(_ loaded: [MailItem], known: Set<String>) -> [MailItem] {
        guard let sample = loaded.first else { return [] }
        let prefix = "\(sample.accountID):\(sample.folder):\(sample.validity):"
        let previous = known.filter { $0.hasPrefix(prefix) }.compactMap { UInt64($0.dropFirst(prefix.count)) }.max()
        guard let previous else { return known.isEmpty ? loaded.filter(\.unread) : [] }
        return loaded.filter { $0.unread && (UInt64($0.uid) ?? 0) > previous }
    }
    func unlockMail(_ account: UUID) async {
        guard unlockingAccount == nil, !refreshing else { return }
        unlockingAccount = account
        defer { unlockingAccount = nil }
        do {
            try MailSecrets.unlock(account)
            lockedAccounts.remove(account); errors[account] = nil
            await refresh()
        } catch { errors[account] = error.localizedDescription }
    }
    func refresh() async {
        guard !refreshing && !connecting else { return }
        refreshing = true; defer { refreshing = false; lastRefresh = Date() }
        for account in accounts {
            do {
                let result = try await request("folders", account, [:])
                let data = try JSONSerialization.data(withJSONObject: result["folders"] ?? [])
                folders[account.id] = try JSONDecoder().decode([MailFolder].self, from: data)
                try await fetchFolder(account, folder: "INBOX", limit: selectedAccount == account.id && selectedFolder == "INBOX" ? limit : 100)
                if selectedAccount == account.id, selectedFolder != "INBOX" { try await fetchFolder(account, folder: selectedFolder, limit: limit) }
                errors[account.id] = nil; lockedAccounts.remove(account.id)
            } catch {
                if error is MailSecretError { lockedAccounts.insert(account.id) }
                errors[account.id] = error.localizedDescription
            }
        }
        let valid = Set(items.map(\.id)); bodies = bodies.filter { valid.contains($0.key) }
    }
    func load(_ item: MailItem) async throws {
        guard !loadingIDs.contains(item.id), let account = accounts.first(where: { $0.id == item.accountID }) else { return }
        if bodies[item.id] != nil && !item.unread { return }
        loadingIDs.insert(item.id); defer { loadingIDs.remove(item.id) }
        let result = try await request(bodies[item.id] == nil ? "body" : "seen", account, ["uid": item.uid, "validity": item.validity, "folder": item.folder])
        guard accounts.contains(where: { $0.id == item.accountID }) else { return }
        if let text = result["text"] as? String {
            let data = try JSONSerialization.data(withJSONObject: result["attachments"] ?? [])
            bodies[item.id] = MailBody(text: text, html: result["html"] as? String ?? "", attachments: try JSONDecoder().decode([MailAttachment].self, from: data))
        }
        if result["seen"] as? Bool == true {
            readIDs.insert(item.id)
            if let index = items.firstIndex(where: { $0.id == item.id }), items[index].unread {
                items[index].unread = false
                if let index = folders[item.accountID]?.firstIndex(where: { $0.path == item.folder }) { folders[item.accountID]?[index].unread = max(0, (folders[item.accountID]?[index].unread ?? 0) - 1) }
            }
        } else if let warning = result["warning"] as? String { notice = warning }
    }
    func download(_ attachment: MailAttachment, from item: MailItem) {
        let key = item.id + ":" + attachment.id
        guard !attachmentDownloads.contains(key), let account = accounts.first(where: { $0.id == item.accountID }) else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = URL(fileURLWithPath: attachment.name).lastPathComponent
        guard panel.runModal() == .OK, let url = panel.url else { return }
        attachmentDownloads.insert(key)
        Task {
            defer { attachmentDownloads.remove(key) }
            do {
                let result = try await request("attachment", account, ["uid": item.uid, "validity": item.validity, "folder": item.folder, "attachment": attachment.id])
                guard let encoded = result["data"] as? String, let data = Data(base64Encoded: encoded) else { throw YOBROError.message(L("Anhang konnte nicht gelesen werden.")) }
                try data.write(to: url, options: .atomic)
                NSWorkspace.shared.activateFileViewerSelecting([url])
            } catch { notice = error.localizedDescription }
        }
    }
    func reply(_ item: MailItem) {
        draftAccount = item.accountID; draftTo = item.replyTo
        draftSubject = item.subject.lowercased().hasPrefix("re:") ? item.subject : "Re: " + item.subject
        draftReplyID = item.messageID; draftText = ""; draftRTF = nil
    }
    func send() async throws {
        guard !sending, let account = accounts.first(where: { $0.id == draftAccount }) else { throw YOBROError.message(L("Absenderkonto wählen.")) }
        sending = true; defer { sending = false }
        var payload: [String: Any] = ["to": draftTo, "subject": draftSubject, "text": draftText, "replyID": draftReplyID]
        if let draftRTF,
           let attributed = NSAttributedString(rtf: draftRTF, documentAttributes: nil),
           let htmlData = try? attributed.data(from: NSRange(location: 0, length: attributed.length), documentAttributes: [.documentType: NSAttributedString.DocumentType.html]),
           let html = String(data: htmlData, encoding: .utf8) {
            payload["html"] = html
        }
        let result = try await request("send", account, payload)
        let refused = result["refused"] as? [String] ?? []
        draftTo = ""; draftSubject = ""; draftText = ""; draftRTF = nil; draftReplyID = ""
        if !refused.isEmpty {
            notice = L("Teilweise versendet. Nicht angenommene Empfänger: ") + refused.joined(separator: ", ")
        } else if let warning = result["warning"] as? String, !warning.isEmpty {
            notice = warning
        } else {
            notice = L("Die Nachricht wurde versendet und in Gesendet gespeichert.", "The message was sent and saved to Sent.")
        }
    }

    func delete(_ selectedItems: [MailItem]) async throws {
        try await change(selectedItems, target: nil)
    }

    func move(_ selectedItems: [MailItem], to target: String) async throws {
        try await change(selectedItems, target: target)
    }

    private func change(_ selectedItems: [MailItem], target: String?) async throws {
        guard !organizing, !selectedItems.isEmpty else { return }
        organizing = true; defer { organizing = false }
        let groups = Dictionary(grouping: selectedItems) { "\($0.accountID.uuidString)\u{0}\($0.folder)\u{0}\($0.validity)" }
        var failure: Error?
        for messages in groups.values {
            guard let first = messages.first, let account = accounts.first(where: { $0.id == first.accountID }) else { continue }
            do {
                var extra: [String: Any] = ["folder": first.folder, "validity": first.validity, "uids": messages.map(\.uid)]
                if let target { extra["target"] = target }
                _ = try await request(target == nil ? "delete" : "move", account, extra)
                let changed = Set(messages.map(\.id))
                items.removeAll { changed.contains($0.id) }
                bodies = bodies.filter { !changed.contains($0.key) }
                if selectedID.map(changed.contains) == true { selectedID = nil }
            } catch { failure = error; break }
        }
        await refresh()
        if let failure { throw failure }
    }
}
