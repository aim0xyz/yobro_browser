import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

final class WorkspaceFeatureTests: XCTestCase {
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
    func testOAuthPopupKeepsWebKitConfigurationAndClosesItsTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let configuration = WKWebViewConfiguration()
        let dataStore = WKWebsiteDataStore.nonPersistent()
        let contentController = WKUserContentController()
        configuration.websiteDataStore = dataStore
        configuration.userContentController = contentController

        let opener = model.newTab()
        let other = model.newTab()
        let popup = model.newTab(space: model.space, webViewConfiguration: configuration, useConfigurationDirectly: true)
        popup.openerID = opener.id
        model.select(other.id); model.select(popup.id)

        XCTAssertTrue(popup.webView.configuration.websiteDataStore === dataStore)
        XCTAssertTrue(popup.webView.configuration.userContentController === contentController)
        popup.webViewDidClose(popup.webView)
        XCTAssertFalse(model.tabs.contains { $0.id == popup.id })
        XCTAssertEqual(model.activeID, opener.id, "Closing itself returns to the page that opened it")
        XCTAssertTrue(model.closedTabs.isEmpty, "A self-closed popup is not offered for reopening")
        XCTAssertNil(model.closedTabUndoID)
    }

    @MainActor
    func testScriptPopupRemembersItsOpener() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let opener = model.newTab()
        opener.webView.loadHTMLString("<html><body>Opener</body></html>", baseURL: URL(string: "https://popup-opener.test/"))
        for _ in 0..<60 where opener.webView.isLoading || opener.webView.url == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        _ = model.newTab()
        model.select(opener.id)
        _ = try? await opener.webView.evaluateJavaScript("window.open('about:blank'); true")
        for _ in 0..<40 where model.tabs.count < 3 { try await Task.sleep(nanoseconds: 50_000_000) }
        let popup = try XCTUnwrap(model.tabs.last)
        XCTAssertEqual(popup.openerID, opener.id)
        XCTAssertEqual(model.activeID, popup.id)
        _ = try? await popup.webView.evaluateJavaScript("window.close(); true")
        for _ in 0..<40 where model.tabs.contains(where: { $0.id == popup.id }) { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertFalse(model.tabs.contains { $0.id == popup.id })
        XCTAssertEqual(model.activeID, opener.id)
        XCTAssertTrue(model.closedTabs.isEmpty)
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
}
