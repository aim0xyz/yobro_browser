import XCTest
@testable import YOBRO

@MainActor
final class MailStoreTests: XCTestCase {
    private var homes: [URL] = []

    override func tearDown() {
        for home in homes {
            try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: home.path)
            try? FileManager.default.removeItem(at: home)
        }
        homes = []
        super.tearDown()
    }

    private func makeHome() throws -> URL {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        homes.append(home)
        return home
    }

    private func item(_ account: UUID, uid: String = "7", unread: Bool = true, sender: String = "Alice Example <Alice@Example.org>",
                      folder: String = "INBOX", references: String = "") -> MailItem {
        MailItem(accountID: account, folder: folder, validity: "42", uid: uid, subject: "Plan", sender: sender, recipient: "me@example.org",
                 replyTo: sender, date: Date(timeIntervalSince1970: 1_790_000_000), unread: unread, messageID: "<m\(uid)@example.org>", references: references)
    }

    func testOpeningMailFromTheEmptyStartPage() throws {
        let model = BrowserModel(root: try makeHome())
        XCTAssertTrue(model.tabs.isEmpty)
        model.presentMail()
        XCTAssertTrue(model.showMail)
        XCTAssertTrue(model.tabs.isEmpty)
        // Opening a tab leaves Mail.
        model.newTab()
        XCTAssertFalse(model.showMail)
    }

    func testSenderNameAndAddress() {
        let account = UUID()
        XCTAssertEqual(item(account).senderName, "Alice Example")
        XCTAssertEqual(item(account).senderAddress, "alice@example.org")
        XCTAssertEqual(item(account, sender: "\"Bob, Jr.\" <bob@example.org>").senderName, "Bob, Jr.")
        XCTAssertEqual(item(account, sender: "carol@example.org").senderName, "carol@example.org")
        XCTAssertEqual(item(account, sender: "<dave@example.org>").senderName, "dave@example.org")
    }

    func testOnlyMessagesNewerThanTheMarkAreAnnounced() {
        let account = UUID()
        let loaded = [item(account, uid: "5"), item(account, uid: "9"), item(account, uid: "10", unread: false)]
        // No mark yet: the first refresh only sets the baseline.
        XCTAssertTrue(MailStore.newInboxItems(loaded, mark: nil, validity: "42").isEmpty)
        // A renumbered mailbox is a new baseline, not a flood of alerts.
        XCTAssertTrue(MailStore.newInboxItems(loaded, mark: MailInboxMark(validity: "41", uid: 1), validity: "42").isEmpty)
        XCTAssertEqual(MailStore.newInboxItems(loaded, mark: MailInboxMark(validity: "42", uid: 6), validity: "42").map(\.uid), ["9"])
        // An inbox that was empty announces whatever arrives.
        XCTAssertEqual(MailStore.newInboxItems(loaded, mark: MailInboxMark(validity: "42", uid: 0), validity: "42").map(\.uid), ["5", "9"])
    }

    func testLegacyInboxHistoryShrinksToTheNewestMessage() async throws {
        let home = try makeHome()
        let account = UUID()
        let ids = (1...300).map { "\(account.uuidString):INBOX:42:\($0)" }
        try JSONSerialization.data(withJSONObject: ["notifications": false, "knownInbox": [account.uuidString: ids]])
            .write(to: home.appendingPathComponent("mail-preferences.json"))
        var mailAccount = MailAccount(address: "me@example.org"); mailAccount.id = account
        let store = MailStore(home: home, request: { _, _, _ in ["validity": "42", "messages": [["uid": "301", "unread": true]], "total": 1] })
        store.accounts = [mailAccount]
        try await store.fetchFolder(mailAccount, folder: "INBOX", limit: 100)
        let saved = try String(contentsOf: home.appendingPathComponent("mail-preferences.json"), encoding: .utf8)
        XCTAssertFalse(saved.contains("knownInbox"))
        let preferences = try JSONDecoder().decode(MailPreferences.self, from: Data(saved.utf8))
        XCTAssertEqual(preferences.inboxMarks[account.uuidString], MailInboxMark(validity: "42", uid: 301))
    }

    func testRemoteImagesAreAllowedPerMessageOrPerSender() throws {
        let home = try makeHome()
        let account = UUID()
        let store = MailStore(home: home)
        let first = item(account, uid: "1"), second = item(account, uid: "2")
        let stranger = item(account, uid: "3", sender: "news@other.example")
        XCTAssertFalse(store.remoteImagesEnabled)
        XCTAssertFalse(store.remoteImagesAllowed(for: first))

        store.allowRemoteImages(for: first, always: false)
        XCTAssertTrue(store.remoteImagesAllowed(for: first))
        XCTAssertFalse(store.remoteImagesAllowed(for: second), "Allowing one message must not allow others")
        XCTAssertFalse(MailStore(home: home).remoteImagesAllowed(for: first), "A one-time allowance does not outlive the session")

        store.allowRemoteImages(for: second, always: true)
        let reopened = MailStore(home: home)
        XCTAssertTrue(reopened.remoteImagesAllowed(for: first), "Trusting a sender covers all of its messages")
        XCTAssertFalse(reopened.remoteImagesAllowed(for: stranger))
        reopened.forgetTrustedImageSenders()
        XCTAssertFalse(MailStore(home: home).remoteImagesAllowed(for: first))
    }

    func testLegacyRemoteImagePreferenceRequiresFreshOptIn() throws {
        let home = try makeHome()
        try Data(#"{"remoteImages":true}"#.utf8).write(to: home.appendingPathComponent("mail-preferences.json"))
        XCTAssertFalse(MailStore(home: home).remoteImagesEnabled)
        try Data(#"{"remoteImages":true,"remoteImagesApproved":true}"#.utf8).write(to: home.appendingPathComponent("mail-preferences.json"))
        XCTAssertTrue(MailStore(home: home).remoteImagesEnabled)
    }

    func testRemoteImageDetection() {
        XCTAssertTrue(MailBody(text: "", html: #"<img src="https://t.example/p.gif">"#, attachments: []).hasRemoteImages)
        XCTAssertTrue(MailBody(text: "", html: "<img src='//cdn.example/a.png'>", attachments: []).hasRemoteImages)
        XCTAssertTrue(MailBody(text: "", html: #"<td style="background:url(https://x.example/b.png)">"#, attachments: []).hasRemoteImages)
        XCTAssertFalse(MailBody(text: "", html: #"<img src="data:image/png;base64,AAAA"><a href="https://example.org">link</a>"#, attachments: []).hasRemoteImages)
    }

    func testReplyQuotesTheMessageAndKeepsTheThread() throws {
        let store = MailStore(home: try makeHome())
        let account = UUID()
        let original = item(account, references: "<root@example.org>")
        store.items = [original]
        store.bodies[original.id] = MailBody(text: "Line one\nLine two", attachments: [])
        store.reply(original)
        XCTAssertTrue(store.composerPresented)
        XCTAssertEqual(store.draftSubject, "Re: Plan")
        XCTAssertEqual(store.draftReplyID, "<m7@example.org>")
        XCTAssertEqual(store.draftReferences, "<root@example.org>")
        XCTAssertTrue(store.draftText.hasPrefix("\n\n"), "The reply starts above the quote")
        XCTAssertTrue(store.draftText.contains("Alice Example"))
        XCTAssertTrue(store.draftText.hasSuffix("> Line one\n> Line two"))
        XCTAssertEqual(store.draftTo, "Alice Example <Alice@Example.org>")
        // Without Reply-To, the answer goes to the sender.
        let bare = MailItem(accountID: account, validity: "42", uid: "8", subject: "Hi", sender: "bob@example.org", recipient: "", replyTo: "",
                            date: Date(), unread: false, messageID: "")
        store.reply(bare)
        XCTAssertEqual(store.draftTo, "bob@example.org")
    }

    func testSendPassesCopyAndThreadToTheTransport() async throws {
        let home = try makeHome()
        let account = MailAccount(label: "Test", address: "sender@example.invalid")
        var payload: [String: Any] = [:]
        let store = MailStore(home: home, request: { _, _, extra in payload = extra; return ["sent": true, "refused": []] })
        store.accounts = [account]
        store.draftAccount = account.id
        store.draftTo = "a@example.invalid"; store.draftCc = "b@example.invalid"
        store.draftText = "Hi"; store.draftReplyID = "<m@example.invalid>"; store.draftReferences = "<root@example.invalid>"
        try await store.send()
        XCTAssertEqual(payload["cc"] as? String, "b@example.invalid")
        XCTAssertEqual(payload["references"] as? String, "<root@example.invalid>")
        XCTAssertEqual(store.draftCc, "")
        XCTAssertEqual(store.draftReferences, "")
    }

    func testMarkUnreadUpdatesListAndFolderCount() async throws {
        let account = MailAccount(address: "me@example.org")
        var requests: [String] = []
        let store = MailStore(home: try makeHome(), request: { action, _, _ in requests.append(action); return [:] })
        store.accounts = [account]
        let read = item(account.id, unread: false)
        store.items = [read]
        store.folders[account.id] = try JSONDecoder().decode([MailFolder].self, from: Data(#"[{"path":"INBOX","title":"INBOX","delimiter":"/","selectable":true,"unread":2,"role":null}]"#.utf8))
        store.selectedID = read.id
        await store.markUnread(read)
        XCTAssertEqual(requests, ["unseen"])
        XCTAssertTrue(store.items[0].unread)
        XCTAssertEqual(store.folders[account.id]?.first?.unread, 3)
        XCTAssertNil(store.selectedID, "Leaving the message keeps it unread")
    }

    func testEditingWithoutPasswordKeepsTheSavedOne() async throws {
        let home = try makeHome()
        var account = MailAccount(label: "Old", address: "me@example.org", username: "me@example.org", imapHost: "imap.example.org", smtpHost: "smtp.example.org")
        var checked: [String] = []
        let store = MailStore(home: home, check: { _, password in checked.append(password) })
        store.accounts = [account]
        try store.persist([account])
        // The session cache stands in for the Keychain entry.
        MailSecrets.session.set("saved-secret", id: account.id)
        defer { MailSecrets.session.remove(account.id) }
        account.label = "New"
        try await store.connect(account, password: "")
        XCTAssertEqual(checked, ["saved-secret"])
        XCTAssertEqual(store.accounts.map(\.label), ["New"])
        XCTAssertEqual(MailStore(home: home).accounts.map(\.label), ["New"])
    }

    func testNewAccountNeedsAPassword() async throws {
        let store = MailStore(home: try makeHome(), check: { _, _ in XCTFail("Must not connect without a password") })
        do {
            try await store.connect(MailAccount(address: "me@example.org", imapHost: "imap.example.org", smtpHost: "smtp.example.org"), password: "")
            XCTFail("Expected an error")
        } catch {}
        XCTAssertTrue(store.accounts.isEmpty)
    }

    func testRemovingAnAccountKeepsItWhenTheListCannotBeSaved() throws {
        let home = try makeHome()
        let account = MailAccount(address: "me@example.org")
        let store = MailStore(home: home)
        try store.persist([account])
        store.accounts = [account]
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: home.path)
        XCTAssertThrowsError(try store.disconnect(account))
        XCTAssertEqual(store.accounts, [account])
    }

    func testPollingDoesNotKeepAClosedProfilesStoreAlive() async throws {
        var store: MailStore? = MailStore(home: try makeHome())
        weak let released = store
        store?.startPolling()
        try await Task.sleep(nanoseconds: 50_000_000)
        store = nil
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertNil(released)
    }
}
