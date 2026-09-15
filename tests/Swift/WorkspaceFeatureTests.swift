import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

final class WorkspaceFeatureTests: XCTestCase {
    @MainActor
    func testDeletingACompleteLinkRestoresNormalTypingAttributes() {
        let textView = NSTextView()
        textView.isRichText = true
        textView.string = "https://example.com"
        let wholeText = NSRange(location: 0, length: textView.string.utf16.count)
        textView.textStorage?.addAttributes([
            .link: URL(string: "https://example.com")!,
            .foregroundColor: NSColor.linkColor,
            .underlineStyle: NSUnderlineStyle.single.rawValue
        ], range: wholeText)
        textView.typingAttributes[.link] = URL(string: "https://example.com")!
        textView.typingAttributes[.foregroundColor] = NSColor.linkColor
        textView.typingAttributes[.underlineStyle] = NSUnderlineStyle.single.rawValue

        textView.textStorage?.deleteCharacters(in: wholeText)
        textView.setSelectedRange(NSRange(location: 0, length: 0))
        // Automatic link detection sometimes leaves only the visual typing
        // attributes behind after deletion, without the `.link` key itself.
        textView.typingAttributes.removeValue(forKey: .link)
        RichTextEditor.Coordinator.clearOrphanedLinkTypingAttributes(in: textView)

        XCTAssertNil(textView.typingAttributes[.link])
        XCTAssertNil(textView.typingAttributes[.underlineStyle])
        XCTAssertNotEqual(textView.typingAttributes[.foregroundColor] as? NSColor, NSColor.linkColor)
    }
    func item(_ account: UUID, uid: String = "7", validity: String = "42") -> MailItem {
        MailItem(accountID: account, validity: validity, uid: uid, subject: "Deine Reiseunterlagen", sender: "Team <team@example.invalid>", recipient: "test@example.invalid", replyTo: "team@example.invalid", date: Date(), unread: true, messageID: "fixture")
    }
    func testInboxNotificationsOnlyForNewUIDs() {
        let id = UUID(), old = item(UUID())
        let previous = item(id, uid: "100")
        let rows = [item(id, uid: "50"), item(id, uid: "100"), item(id, uid: "101")]
        XCTAssertEqual(MailStore.newInboxItems(rows, known: [previous.id]).map(\.uid), ["101"])
        XCTAssertTrue(MailStore.newInboxItems(rows, known: [old.id]).isEmpty)
        XCTAssertEqual(MailStore.newInboxItems(rows, known: []).count, 3)
        XCTAssertTrue(MailStore.newInboxItems([item(id, validity: "43")], known: [previous.id]).isEmpty)
    }
    @MainActor
    func testReadStatusHTMLAttachmentsAndFolderIdentity() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let account = MailAccount(label: "Test", address: "test@example.invalid")
        var actions: [String] = []
        let store = MailStore(home: home) { action, _, extra in
            actions.append(action)
            XCTAssertEqual(extra["folder"] as? String, "INBOX")
            return ["text": "Reiseunterlagen", "html": "<h1>Reiseunterlagen</h1>", "attachments": [["id":"1.2","name":"ticket.pdf","size":3,"mime":"application/pdf"]], "seen":true]
        }
        let message = item(account.id)
        store.accounts = [account]; store.items = [message]; store.selectedID = message.id; store.unreadOnly = true
        store.folders[account.id] = [MailFolder(path:"INBOX",title:"INBOX",delimiter:"/",selectable:true,unread:1,role:"inbox")]
        try await store.load(message)
        XCTAssertFalse(store.items[0].unread); XCTAssertFalse(store.hasUnread)
        XCTAssertEqual(store.filtered.count, 1)
        XCTAssertEqual(store.bodies[message.id]?.attachments.first?.name, "ticket.pdf")
        XCTAssertTrue(store.bodies[message.id]?.html.contains("<h1>") == true)
        try await store.load(store.items[0]); XCTAssertEqual(actions, ["body"])
        var archive = message; archive.folder = "Archive"
        XCTAssertNotEqual(archive.id, message.id)
    }
    @MainActor
    func testSpacesFoldersAndSplitPersistence() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }; setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel()
        try model.addSpace("Reisen"); try model.addFolder("Planung")
        model.setSpaceIcon(.travel, for: "Reisen")
        model.setFolderColor(.lilac, folder: model.folders[0].id)
        let first = model.newTab(); let second = model.newTab()
        model.moveToFolder(first.id, folder: model.folders[0].id)
        XCTAssertTrue(model.pairTabs(second.id, with: first.id))
        XCTAssertEqual(first.folderID, second.folderID)
        model.select(second.id); XCTAssertEqual(model.splitID, first.id)
        try model.renameSpace("Reisen", to: "Urlaub")
        XCTAssertEqual(first.space, "Urlaub"); XCTAssertEqual(model.folders[0].space, "Urlaub")
        let restored = BrowserModel()
        XCTAssertTrue(restored.spaces.contains("Urlaub")); XCTAssertEqual(restored.splitPairs.count, 1)
        XCTAssertEqual(restored.spaceIcon(for: "Urlaub"), .travel)
        XCTAssertEqual(restored.folders.first?.folderColor, .lilac)
        model.separateSplit(); XCTAssertNil(model.splitID); XCTAssertTrue(model.tabs.contains { $0.id == first.id })
        model.dissolveFolder(model.folders[0].id); XCTAssertNil(first.folderID)
        model.closeTab(second.id); XCTAssertNotNil(model.active)
        XCTAssertThrowsError(try model.addSpace("Urlaub"))
    }

    @MainActor
    func testFolderNotePersistence() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Google")
        let folder = try XCTUnwrap(model.folders.first)
        let note = model.newNote(title: "Google-Notizen", folderID: folder.id)
        note.noteContent = "Ideen für die nächste Kampagne"
        model.persistSession(scheduleSync: false)

        let restored = BrowserModel(root: home)
        let savedNote = try XCTUnwrap(restored.tabs.first(where: { $0.id == note.id }))
        XCTAssertTrue(savedNote.isNote)
        XCTAssertEqual(savedNote.title, "Google-Notizen")
        XCTAssertEqual(savedNote.noteContent, "Ideen für die nächste Kampagne")
        XCTAssertEqual(savedNote.folderID, folder.id)
        XCTAssertEqual(savedNote.space, model.space)
    }

    @MainActor
    func testRichTextNotePersistence() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let note = model.newNote(title: "Formatierte Notiz")
        let rich = NSMutableAttributedString(string: "Wichtige Überschrift")
        rich.addAttribute(.font, value: NSFont.boldSystemFont(ofSize: 24), range: NSRange(location: 0, length: rich.length))
        note.noteContent = rich.string
        note.noteRTF = rich.rtf(from: NSRange(location: 0, length: rich.length), documentAttributes: [:])
        model.persistSession(scheduleSync: false)

        let restored = BrowserModel(root: home)
        let saved = try XCTUnwrap(restored.tabs.first(where: { $0.id == note.id }))
        let restoredRich = try XCTUnwrap(saved.noteRTF.flatMap { NSAttributedString(rtf: $0, documentAttributes: nil) })
        let font = try XCTUnwrap(restoredRich.attribute(.font, at: 0, effectiveRange: nil) as? NSFont)
        XCTAssertTrue(NSFontManager.shared.traits(of: font).contains(.boldFontMask))
        XCTAssertGreaterThanOrEqual(font.pointSize, 23)
    }

    @MainActor
    func testRichMailSendsHTMLAlternative() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let account = MailAccount(label: "Test", address: "sender@example.invalid")
        var sentHTML = ""
        let store = MailStore(home: home) { action, _, payload in
            XCTAssertEqual(action, "send")
            sentHTML = payload["html"] as? String ?? ""
            return ["sent": true, "saved": true, "refused": []]
        }
        store.accounts = [account]
        store.draftAccount = account.id
        store.draftTo = "receiver@example.invalid"
        store.draftSubject = "Link"
        store.draftText = "YOBRO"
        let rich = NSMutableAttributedString(string: "YOBRO")
        rich.addAttribute(.link, value: URL(string: "https://example.com")!, range: NSRange(location: 0, length: rich.length))
        store.draftRTF = rich.rtf(from: NSRange(location: 0, length: rich.length), documentAttributes: [:])

        try await store.send()
        XCTAssertTrue(sentHTML.contains("https://example.com"))
    }
    @MainActor
    func testOAuthPopupKeepsWebKitConfigurationAndClosesItsTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let configuration = WKWebViewConfiguration()
        let dataStore = WKWebsiteDataStore.nonPersistent()
        let contentController = WKUserContentController()
        configuration.websiteDataStore = dataStore
        configuration.userContentController = contentController

        let popup = model.newTab(space: model.space, webViewConfiguration: configuration, useConfigurationDirectly: true)

        XCTAssertTrue(popup.webView.configuration.websiteDataStore === dataStore)
        XCTAssertTrue(popup.webView.configuration.userContentController === contentController)
        popup.webViewDidClose(popup.webView)
        XCTAssertFalse(model.tabs.contains { $0.id == popup.id })
    }
    @MainActor
    func testBrowserShellSplitLayout() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }; setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(); model.showAgent = false
        try model.addSpace("Reisen"); try model.addFolder("Kopenhagen")
        model.setSpaceIcon(.travel, for: "Reisen")
        model.setFolderColor(.sky, folder: model.folders[0].id)
        let first = model.newTab(), second = model.newTab()
        model.moveToFolder(first.id, folder: model.folders[0].id)
        model.pairTabs(second.id, with:first.id)
        let view = NSHostingView(rootView: BrowserShell(model:model))
        view.frame = NSRect(x:0,y:0,width:1360,height:880)
        let window = NSWindow(contentRect:view.frame,styleMask:[.titled,.closable,.resizable,.fullSizeContentView],backing:.buffered,defer:false)
        window.titlebarAppearsTransparent = true; window.titleVisibility = .hidden
        window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds:300_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in:view.bounds)); view.cacheDisplay(in:view.bounds,to:bitmap)
        try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:URL(fileURLWithPath:"/tmp/YOBRO-shell-layout.png"))
    }
    @MainActor
    func testNordVPNPackageWhenProvided() async throws {
        guard let path = ProcessInfo.processInfo.environment["YOBRO_TEST_NORD_PACKAGE"] else { throw XCTSkip("Optional NordVPN package inspection") }
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at:home) }
        let store = ExtensionStore(home:home)
        await store.prepare(URL(fileURLWithPath:path))
        XCTAssertNil(store.pending)
        XCTAssertTrue(store.message?.contains("Chrome-Proxy-API") == true, store.message ?? "Missing reason")
        store.cancelPending()
    }
    func testStoreURLValidation() {
        let id = "abcdefghijklmnopabcdefghijklmnop"
        XCTAssertEqual(ChromeStore.identifier("https://chromewebstore.google.com/detail/test/" + id), id)
        XCTAssertNil(ChromeStore.identifier("https://chromewebstore.google.com.evil.invalid/detail/" + id))
        XCTAssertNil(ChromeStore.identifier("http://chromewebstore.google.com/detail/" + id))
        let url = ChromeStore.downloadURL(id: id, version: "140.0.0.0")
        XCTAssertEqual(URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "x" })?.value, "id=\(id)&installsource=ondemand&uc")
    }
    func testStoreLiveDownloadWhenRequested() async throws {
        guard getenv("YOBRO_TEST_STORE") != nil else { throw XCTSkip("Optional live marketplace check") }
        let file = try await ChromeStore.download(id: "eimadpbcbfnmbkopoojfekhnkhdbieeh")
        defer { try? FileManager.default.removeItem(at: file) }
        XCTAssertGreaterThan(try ExtensionPackage.zipPayload(Data(contentsOf: file)).count, 1000)
    }
    @MainActor
    func testMailHTMLAndLayout() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let account = MailAccount(label: "Privat", address: "test@example.invalid")
        let message = item(account.id)
        let html = "<h1>Deine Reise ist gebucht.</h1><p>Hallo! Hier findest du deine Reiseunterlagen.</p><table style='background:#e7eddf;padding:20px;width:100%'><tr><td><b>Berlin → Kopenhagen</b><br>12. September · 09:30 Uhr</td></tr></table><p>Wir wünschen dir eine gute Reise.</p><script>document.body.dataset.executed='yes'</script>"
        let store = MailStore(home: home) { action, _, _ in
            if action == "folders" { return ["folders": [["path":"INBOX","title":"INBOX","delimiter":"/","selectable":true,"unread":1],["path":"Gesendet","title":"Gesendet","delimiter":"/","selectable":true,"unread":0],["path":"Archiv/Reisen","title":"Archiv/Reisen","delimiter":"/","selectable":true,"unread":0]]] }
            if action == "list" { return ["validity":"42","total":1,"messages":[["uid":"7","subject":message.subject,"sender":message.sender,"unread":true]]] }
            return ["text":"Deine Reise ist gebucht.","html":html,"attachments":[["id":"1.2","name":"Reiseunterlagen.pdf","size":42170,"mime":"application/pdf"]],"seen":true]
        }
        store.accounts = [account]; store.items = [message]; store.selectedAccount = account.id; store.selectedID = message.id
        try await store.load(message)
        let view = NSHostingView(rootView: MailWorkspace(store: store, close: {}))
        view.frame = NSRect(x:0,y:0,width:1100,height:740)
        let window = NSWindow(contentRect:view.frame,styleMask:[.titled],backing:.buffered,defer:false)
        window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 1_000_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in:view.bounds)); view.cacheDisplay(in:view.bounds,to:bitmap)
        try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:URL(fileURLWithPath:"/tmp/YOBRO-mail-layout.png"))
        let document = MailHTMLView.document(html, remoteImages:false)
        XCTAssertTrue(document.contains("script-src 'none'")); XCTAssertFalse(document.contains("img-src data: https:"))
        XCTAssertTrue(document.contains("overflow-y:auto!important"))
        XCTAssertTrue(MailHTMLView.document(html, remoteImages:true).contains("img-src data: https: http:"))
        XCTAssertTrue(MailHTMLView.document("<img src='//cdn.example.test/a.png'>", remoteImages:true).contains("src='https://cdn.example.test/a.png'"))
    }
}
