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
    var title: String { label.isEmpty ? address : label }
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
    var references = ""
    var id: String { "\(accountID):\(folder):\(validity):\(uid)" }
    /// The display name of `From`, or its address when there is none.
    var senderName: String { MailAddress.name(sender) }
    var senderAddress: String { MailAddress.address(sender) }
}

enum MailAddress {
    /// `Name <a@b>` → `Name`; a bare address stays as it is.
    static func name(_ value: String) -> String {
        guard let open = value.lastIndex(of: "<") else { return value.trimmingCharacters(in: .whitespaces) }
        let name = value[..<open].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
        return name.isEmpty ? address(value) : name
    }
    static func address(_ value: String) -> String {
        guard let open = value.lastIndex(of: "<"), let close = value.lastIndex(of: ">"), open < close else {
            return value.trimmingCharacters(in: .whitespaces).lowercased()
        }
        return value[value.index(after: open)..<close].trimmingCharacters(in: .whitespaces).lowercased()
    }
}

struct MailAttachment: Identifiable, Decodable { let id: String; let name: String; let size: Int; let mime: String }
struct MailBody {
    let text: String
    var html = ""
    let attachments: [MailAttachment]
    /// Whether the HTML refers to images on other servers, which stay blocked until allowed.
    var hasRemoteImages: Bool {
        html.range(of: #"(?i)(src|background)\s*=\s*["']?\s*(https?:)?//|url\(\s*["']?\s*(https?:)?//"#, options: .regularExpression) != nil
    }
}
struct MailFolder: Identifiable, Decodable {
    let path: String; let title: String; let delimiter: String; let selectable: Bool; var unread: Int; let role: String?
    var id: String { path }
    var displayTitle: String { path.uppercased() == "INBOX" ? L("Posteingang", "Inbox") : title }
    var isTrash: Bool { role == "trash" || ["trash", "papierkorb", "gelöscht", "gelöschte elemente", "deleted items"].contains(title.lowercased()) }
    var symbol: String {
        if path.uppercased() == "INBOX" { return "tray" }
        switch role {
        case "sent": return "paperplane"
        case "trash": return "trash"
        case "drafts": return "doc"
        case "junk": return "xmark.bin"
        case "archive": return "archivebox"
        default: return isTrash ? "trash" : "folder"
        }
    }
}

/// The newest inbox message already seen, so a later refresh can tell what is new.
struct MailInboxMark: Codable, Equatable {
    var validity: String
    var uid: UInt64
}

struct MailPreferences: Codable {
    var notifications = false
    var inboxMarks: [String: MailInboxMark] = [:]
    /// Load images from other servers in every message.
    var remoteImages = false
    /// Senders whose images always load, by lowercased address.
    var trustedImageSenders: Set<String> = []

    private enum CodingKeys: String, CodingKey { case notifications, inboxMarks, knownInbox, remoteImages, remoteImagesApproved, trustedImageSenders }
    init() {}
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        notifications = try values.decodeIfPresent(Bool.self, forKey: .notifications) ?? false
        inboxMarks = try values.decodeIfPresent([String: MailInboxMark].self, forKey: .inboxMarks) ?? [:]
        // Older versions kept every inbox message ID ever seen; only the newest counts.
        if inboxMarks.isEmpty, let legacy = try values.decodeIfPresent([String: Set<String>].self, forKey: .knownInbox) {
            inboxMarks = Self.marks(fromLegacy: legacy)
        }
        let approved = try values.decodeIfPresent(Bool.self, forKey: .remoteImagesApproved) ?? false
        remoteImages = (try values.decodeIfPresent(Bool.self, forKey: .remoteImages) ?? false) && approved
        trustedImageSenders = try values.decodeIfPresent(Set<String>.self, forKey: .trustedImageSenders) ?? []
    }
    func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(notifications, forKey: .notifications)
        try values.encode(inboxMarks, forKey: .inboxMarks)
        try values.encode(remoteImages, forKey: .remoteImages)
        try values.encode(remoteImages, forKey: .remoteImagesApproved)
        try values.encode(trustedImageSenders, forKey: .trustedImageSenders)
    }

    static func marks(fromLegacy legacy: [String: Set<String>]) -> [String: MailInboxMark] {
        var marks: [String: MailInboxMark] = [:]
        for (account, ids) in legacy {
            // IDs read "<account>:INBOX:<validity>:<uid>".
            for id in ids {
                let parts = id.split(separator: ":", omittingEmptySubsequences: false)
                guard parts.count >= 4, let uid = UInt64(parts[parts.count - 1]) else { continue }
                let validity = String(parts[parts.count - 2])
                if let mark = marks[account], mark.validity == validity, mark.uid >= uid { continue }
                if let mark = marks[account], mark.validity != validity { continue }
                marks[account] = MailInboxMark(validity: validity, uid: uid)
            }
        }
        return marks
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
    var errorDescription: String? { L("Mail-Zugriff gesperrt. Bitte dieses Postfach über „Mail entsperren“ freigeben.", "Mail access is locked. Allow this mailbox with “Unlock mail”.") }
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
        guard status == errSecSuccess, let data = result as? Data, let value = String(data: data, encoding: .utf8) else {
            throw YOBROError.message(L("Zugangsdaten nicht verfügbar. Konto bearbeiten und erneut verbinden.", "Credentials are unavailable. Edit the account and connect again."))
        }
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
        guard let script = Bundle.main.url(forResource: "MailWorker", withExtension: "py") ?? YOBROResources.bundle.url(forResource: "MailWorker", withExtension: "py") else {
            throw YOBROError.message(L("Mail-Komponente fehlt.", "The mail component is missing."))
        }
        let output = try await Task.detached(priority: .userInitiated) { () throws -> Data in
            let candidates = ["/usr/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3"]
            guard let runtime = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else {
                throw YOBROError.message(L("Für YoBro Mail wird Python 3 benötigt. Bitte Python 3 installieren.", "YoBro Mail needs Python 3. Please install Python 3."))
            }
            let process = Process()
            process.environment = ProcessInfo.processInfo.environment.merging(["YOBRO_LANGUAGE": YOBROLanguage.identifier]) { _, language in language }
            process.executableURL = URL(fileURLWithPath: runtime); process.arguments = [script.path]
            let input = Pipe(), output = Pipe()
            process.standardInput = input; process.standardOutput = output; process.standardError = FileHandle.nullDevice
            try process.run()
            let timeout = DispatchWorkItem { if process.isRunning { process.terminate() } }
            DispatchQueue.global().asyncAfter(deadline: .now() + 90, execute: timeout)
            defer { timeout.cancel() }
            try input.fileHandleForWriting.write(contentsOf: payload); try input.fileHandleForWriting.close()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else {
                throw YOBROError.message(action == "send"
                    ? L("Versandstatus unklar. Vor erneutem Senden beim Anbieter prüfen, ob die Nachricht angekommen ist.", "Delivery status is unclear. Check with your provider before sending again.")
                    : L("Mail-Verbindung beendet oder Zeitlimit erreicht.", "The mail connection ended or timed out."))
            }
            return data
        }.value
        guard let response = try JSONSerialization.jsonObject(with: output) as? [String: Any] else { throw YOBROError.message(L("Ungültige Mail-Antwort.", "Invalid mail response.")) }
        guard response["ok"] as? Bool == true else { throw YOBROError.message(response["error"] as? String ?? L("Mail-Verbindung fehlgeschlagen.", "Mail connection failed.")) }
        return response["result"] as? [String: Any] ?? [:]
    }
}

@MainActor
final class MailStore: ObservableObject {
    nonisolated static let pollingIntervalNanoseconds: UInt64 = 5 * 60 * 1_000_000_000
    static var pollingMinutes: Int { Int(pollingIntervalNanoseconds / 60_000_000_000) }
    @Published var accounts: [MailAccount] = []
    @Published var items: [MailItem] = []
    @Published var folders: [UUID: [MailFolder]] = [:]
    @Published var selectedFolder = "INBOX"
    @Published var folderLoading = false
    @Published var lockedAccounts: Set<UUID> = []
    @Published var unlockingAccount: UUID?
    @Published var notificationsEnabled = false
    @Published var remoteImagesEnabled = false
    /// Messages whose images the user allowed for this session.
    @Published var remoteImageMessages: Set<String> = []
    @Published var trustedImageSenders: Set<String> = []
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
    @Published var draftCc = ""
    @Published var draftSubject = ""
    @Published var draftText = ""
    @Published var draftRTF: Data?
    @Published var draftReplyID = ""
    @Published var draftReferences = ""
    @Published var composerPresented = false
    private let file: URL

    typealias Request = (String, MailAccount, [String: Any]) async throws -> [String: Any]
    typealias Check = (MailAccount, String) async throws -> Void
    private let request: Request
    private let check: Check
    init(home: URL, request: @escaping Request = { action, account, extra in
        try await MailTransport.run(action: action, account: account, password: MailSecrets.read(account.id), extra: extra)
    }, check: @escaping Check = { account, password in
        _ = try await MailTransport.run(action: "test", account: account, password: password)
    }) {
        self.request = request
        self.check = check
        file = home.appendingPathComponent("mail-accounts.json")
        preferenceFile = home.appendingPathComponent("mail-preferences.json")
        if let data = try? Data(contentsOf: preferenceFile), let value = try? JSONDecoder().decode(MailPreferences.self, from: data) {
            preferences = value
            notificationsEnabled = value.notifications
            remoteImagesEnabled = value.remoteImages
            trustedImageSenders = value.trustedImageSenders
        }
        // A damaged account list must not be replaced by an empty one: the next
        // `persist` would drop every configured mailbox.
        let stored = PersistedState.load([MailAccount].self, at: file)
        if let saved = stored.value { accounts = saved }
        if let problem = stored.problem { notice = problem }
    }

    var filtered: [MailItem] {
        items.filter {
            (selectedAccount == nil ? $0.folder == "INBOX" : $0.accountID == selectedAccount && $0.folder == selectedFolder)
                && (!unreadOnly || $0.unread || $0.id == selectedID)
                && (search.isEmpty || "\($0.subject) \($0.sender)".localizedCaseInsensitiveContains(search))
        }
        .sorted { $0.date > $1.date }
    }
    var selected: MailItem? { items.first { $0.id == selectedID } }
    var unreadCount: Int {
        accounts.reduce(0) { total, account in
            if let known = folders[account.id]?.first(where: { $0.path.uppercased() == "INBOX" }) { return total + known.unread }
            return total + items.filter { $0.accountID == account.id && $0.folder == "INBOX" && $0.unread }.count
        }
    }
    func persist(_ value: [MailAccount]) throws { try JSONEncoder().encode(value).write(to: file, options: [.atomic, .completeFileProtection]) }

    /// Verifies and saves an account. Editing an account may leave the password
    /// empty to keep the one in the Keychain.
    func connect(_ account: MailAccount, password: String) async throws {
        guard !connecting && !refreshing && !sending else { throw YOBROError.message(L("Bitte den laufenden Vorgang abwarten.", "Please wait for the current action to finish.")) }
        connecting = true; defer { connecting = false }
        let editing = accounts.contains { $0.id == account.id }
        let secret = password.isEmpty && editing ? try MailSecrets.read(account.id) : password
        guard !secret.isEmpty else { throw YOBROError.message(L("Passwort fehlt.", "Password is missing.")) }
        try await check(account, secret)
        if !password.isEmpty { try MailSecrets.store(password, id: account.id) }
        var updated = accounts
        if let index = updated.firstIndex(where: { $0.id == account.id }) { updated[index] = account } else { updated.append(account) }
        try persist(updated); accounts = updated
        forget(account.id)
    }
    func disconnect(_ account: MailAccount) throws {
        guard !refreshing && !connecting && !sending else { throw YOBROError.message(L("Bitte den laufenden Vorgang abwarten.", "Please wait for the current action to finish.")) }
        // Save the list first: if that fails, the account must keep its password.
        let updated = accounts.filter { $0.id != account.id }
        try persist(updated); accounts = updated
        try MailSecrets.remove(account.id)
        forget(account.id)
        errors[account.id] = nil; lockedAccounts.remove(account.id)
        preferences.inboxMarks[account.id.uuidString] = nil; persistPreferences()
        if selectedAccount == account.id { selectedAccount = nil; selectedFolder = "INBOX"; selectedID = nil }
    }
    /// Drops what was loaded for one account, so nothing stale survives an edit.
    private func forget(_ account: UUID) {
        let removed = Set(items.filter { $0.accountID == account }.map(\.id))
        items.removeAll { $0.accountID == account }
        bodies = bodies.filter { !removed.contains($0.key) }
        folders[account] = nil
        if selectedID.map(removed.contains) == true { selectedID = nil }
    }
    var hasUnread: Bool { accounts.contains { hasUnread($0.id) } }
    func hasUnread(_ account: UUID) -> Bool {
        if let known = folders[account] { return known.contains { $0.unread > 0 } }
        return items.contains { $0.accountID == account && $0.unread }
    }
    func persistPreferences() {
        do { try JSONEncoder().encode(preferences).write(to: preferenceFile, options: .atomic) }
        catch { notice = L("Mail-Einstellungen konnten nicht gespeichert werden.", "Mail settings could not be saved.") }
    }
    func setNotifications(_ enabled: Bool) async {
        if enabled {
            do {
                guard try await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) else {
                    notice = L("Mitteilungen sind in macOS nicht erlaubt. Du kannst YoBro unter Systemeinstellungen → Mitteilungen freigeben.",
                               "Notifications are not allowed in macOS. You can allow YoBro in System Settings → Notifications.")
                    return
                }
            } catch { notice = error.localizedDescription; return }
        } else {
            UNUserNotificationCenter.current().removeAllPendingNotificationRequests()
            UNUserNotificationCenter.current().removeAllDeliveredNotifications()
        }
        notificationsEnabled = enabled; preferences.notifications = enabled; persistPreferences()
    }

    // MARK: Remote images

    func remoteImagesAllowed(for item: MailItem) -> Bool {
        remoteImagesEnabled || remoteImageMessages.contains(item.id) || trustedImageSenders.contains(item.senderAddress)
    }
    /// Loads images for one message, and with `always` for everything from its sender.
    func allowRemoteImages(for item: MailItem, always: Bool) {
        remoteImageMessages.insert(item.id)
        guard always, !item.senderAddress.isEmpty else { return }
        trustedImageSenders.insert(item.senderAddress)
        preferences.trustedImageSenders = trustedImageSenders; persistPreferences()
    }
    func setRemoteImages(_ enabled: Bool) {
        remoteImagesEnabled = enabled; preferences.remoteImages = enabled
        persistPreferences()
    }
    func forgetTrustedImageSenders() {
        trustedImageSenders = []; preferences.trustedImageSenders = []
        persistPreferences()
    }

    // MARK: Loading

    func stopPolling() { polling?.cancel(); polling = nil }

    func startPolling() {
        guard polling == nil else { return }
        polling = Task { [weak self] in
            // The store is held only while refreshing, never during the pause,
            // so a closed profile's store goes away and the loop ends with it.
            while !Task.isCancelled, await Self.poll(self) {
                do { try await Task.sleep(nanoseconds: Self.pollingIntervalNanoseconds) } catch { return }
            }
        }
    }
    private static func poll(_ store: MailStore?) async -> Bool {
        guard let store else { return false }
        if !store.accounts.isEmpty { await store.refresh() }
        return true
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
        guard let account = accounts.first(where: { $0.id == selectedAccount }) else { return }
        limit = min(5000, limit + 100)
        let revision = folderRevision, folder = selectedFolder
        folderLoading = true
        defer { if revision == folderRevision { folderLoading = false } }
        do { try await fetchFolder(account, folder: folder, limit: limit) }
        catch { errors[account.id] = error.localizedDescription }
    }
    func fetchFolder(_ account: MailAccount, folder: String, limit: Int) async throws {
        let response = try await request("list", account, ["folder": folder, "limit": limit])
        guard accounts.contains(where: { $0.id == account.id }) else { return }
        let validity = response["validity"] as? String ?? ""
        let loaded = (response["messages"] as? [[String: Any]] ?? []).compactMap { row -> MailItem? in
            guard let uid = row["uid"] as? String else { return nil }
            var item = MailItem(accountID: account.id, folder: folder, validity: validity, uid: uid, subject: row["subject"] as? String ?? "",
                                sender: row["sender"] as? String ?? "", recipient: row["to"] as? String ?? "", replyTo: row["replyTo"] as? String ?? "",
                                date: Date(timeIntervalSince1970: row["date"] as? Double ?? 0), unread: row["unread"] as? Bool ?? false,
                                messageID: row["messageID"] as? String ?? "", references: row["references"] as? String ?? "")
            if !item.unread { readIDs.remove(item.id) }
            if readIDs.contains(item.id) { item.unread = false }
            return item
        }
        if folder.uppercased() == "INBOX" { await noteInbox(loaded, account: account, validity: validity) }
        items.removeAll { $0.accountID == account.id && $0.folder == folder }; items.append(contentsOf: loaded)
        folderTotals["\(account.id):\(folder)"] = response["total"] as? Int ?? loaded.count
    }

    /// Notifies about unread inbox messages newer than the last refresh. The first
    /// refresh only sets the baseline, and the mark is just the newest UID.
    private func noteInbox(_ loaded: [MailItem], account: MailAccount, validity: String) async {
        let key = account.id.uuidString
        let mark = preferences.inboxMarks[key]
        if notificationsEnabled {
            let new = Self.newInboxItems(loaded, mark: mark, validity: validity)
            if !new.isEmpty {
                let content = UNMutableNotificationContent()
                content.title = account.title
                content.body = new.count == 1 ? "\(new[0].senderName)\n\(new[0].subject)" : L("\(new.count) neue E-Mails", "\(new.count) new emails")
                content.sound = .default; content.userInfo = ["account": key]
                do { try await UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "yobro-mail-" + UUID().uuidString, content: content, trigger: nil)) }
                catch { errors[account.id] = L("Mitteilung konnte nicht angezeigt werden: \(error.localizedDescription)", "Could not show notification: \(error.localizedDescription)") }
            }
        }
        let newest = loaded.compactMap { UInt64($0.uid) }.max() ?? 0
        let updated = MailInboxMark(validity: validity, uid: mark?.validity == validity ? max(mark?.uid ?? 0, newest) : newest)
        if updated != mark { preferences.inboxMarks[key] = updated; persistPreferences() }
    }

    nonisolated static func newInboxItems(_ loaded: [MailItem], mark: MailInboxMark?, validity: String) -> [MailItem] {
        // No mark yet, or the server renumbered the mailbox: set a new baseline.
        guard let mark, mark.validity == validity else { return [] }
        return loaded.filter { $0.unread && (UInt64($0.uid) ?? 0) > mark.uid }
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
            setUnread(false, item)
        } else if let warning = result["warning"] as? String { notice = warning }
    }
    /// Updates the list and the folder count after the server changed a message's Seen flag.
    private func setUnread(_ unread: Bool, _ item: MailItem) {
        guard let index = items.firstIndex(where: { $0.id == item.id }), items[index].unread != unread else { return }
        items[index].unread = unread
        if let folder = folders[item.accountID]?.firstIndex(where: { $0.path == item.folder }) {
            folders[item.accountID]?[folder].unread = max(0, (folders[item.accountID]?[folder].unread ?? 0) + (unread ? 1 : -1))
        }
    }
    func markUnread(_ item: MailItem) async {
        guard let account = accounts.first(where: { $0.id == item.accountID }) else { return }
        do {
            _ = try await request("unseen", account, ["uid": item.uid, "validity": item.validity, "folder": item.folder])
            readIDs.remove(item.id)
            setUnread(true, item)
            // Leave the message, or opening it again would mark it read at once.
            if selectedID == item.id { selectedID = nil }
        } catch { notice = error.localizedDescription }
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
                guard let encoded = result["data"] as? String, let data = Data(base64Encoded: encoded) else {
                    throw YOBROError.message(L("Anhang konnte nicht gelesen werden.", "The attachment could not be read."))
                }
                try data.write(to: url, options: .atomic)
                NSWorkspace.shared.activateFileViewerSelecting([url])
            } catch { notice = error.localizedDescription }
        }
    }

    // MARK: Writing

    func compose() {
        draftAccount = selectedAccount ?? draftAccount ?? accounts.first?.id
        composerPresented = true
    }
    func reply(_ item: MailItem) {
        draftAccount = item.accountID; draftTo = item.replyTo.isEmpty ? item.sender : item.replyTo; draftCc = ""
        draftSubject = item.subject.lowercased().hasPrefix("re:") ? item.subject : "Re: " + item.subject
        draftReplyID = item.messageID; draftReferences = item.references; draftRTF = nil
        draftText = Self.quote(item, body: bodies[item.id])
        composerPresented = true
    }
    /// The original below an empty first line, each line marked with ">".
    static func quote(_ item: MailItem, body: MailBody?) -> String {
        guard let text = body?.text.trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty else { return "" }
        let date = item.date.formatted(date: .abbreviated, time: .shortened)
        let quoted = text.prefix(20_000).split(separator: "\n", omittingEmptySubsequences: false).map { "> " + $0 }.joined(separator: "\n")
        return "\n\n" + L("Am \(date) schrieb \(item.senderName):", "On \(date), \(item.senderName) wrote:") + "\n" + quoted
    }
    func send() async throws {
        guard !sending else { throw YOBROError.message(L("Die Nachricht wird bereits gesendet.", "The message is already being sent.")) }
        guard let account = accounts.first(where: { $0.id == draftAccount }) else { throw YOBROError.message(L("Absenderkonto wählen.", "Choose a sender account.")) }
        sending = true; defer { sending = false }
        var payload: [String: Any] = ["to": draftTo, "cc": draftCc, "subject": draftSubject, "text": draftText, "replyID": draftReplyID, "references": draftReferences]
        if let draftRTF,
           let attributed = NSAttributedString(rtf: draftRTF, documentAttributes: nil),
           let htmlData = try? MailTextColor.contentForExport(attributed).data(from: NSRange(location: 0, length: attributed.length), documentAttributes: [.documentType: NSAttributedString.DocumentType.html]),
           let html = String(data: htmlData, encoding: .utf8) {
            payload["html"] = html
        }
        let result = try await request("send", account, payload)
        let refused = result["refused"] as? [String] ?? []
        draftTo = ""; draftCc = ""; draftSubject = ""; draftText = ""; draftRTF = nil; draftReplyID = ""; draftReferences = ""
        if !refused.isEmpty {
            notice = L("Teilweise versendet. Nicht angenommene Empfänger: ", "Partly sent. Recipients that were refused: ") + refused.joined(separator: ", ")
        } else if let warning = result["warning"] as? String, !warning.isEmpty {
            notice = warning
        } else {
            notice = L("Die Nachricht wurde versendet und in Gesendet gespeichert.", "The message was sent and saved to Sent.")
        }
    }

    // MARK: Organizing

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
