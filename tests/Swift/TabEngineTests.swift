import XCTest
import Combine
import WebKit
@testable import YOBRO

@MainActor
final class TabEngineTests: XCTestCase {
    private var home: URL!
    private var previousHome: String?

    override func setUp() async throws {
        home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        previousHome = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
    }

    override func tearDown() async throws {
        if let previousHome { setenv("YOBRO_HOME", previousHome, 1) } else { unsetenv("YOBRO_HOME") }
        UserDefaults.standard.removeObject(forKey: "YOBRO.tabsOnTop")
        try? FileManager.default.removeItem(at: home)
    }

    private func makeModel() -> BrowserModel { BrowserModel(root: home) }

    private func load(_ tab: BrowserTab, _ html: String, at address: String) async throws {
        tab.webView.loadHTMLString(html, baseURL: URL(string: address))
        for _ in 0..<100 where tab.webView.isLoading || tab.webView.url == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        try await Task.sleep(nanoseconds: 100_000_000)
    }

    // MARK: Private tabs and zoom

    func testPrivateZoomStaysOutOfThePageZoomStore() async throws {
        let model = makeModel()
        let tab = model.newTab(isPrivate: true)
        try await load(tab, "<html><body>Zoom</body></html>", at: "https://zoom.test/")
        XCTAssertNotNil(tab.webView.url)
        tab.changeZoom(1)
        XCTAssertEqual(tab.webView.pageZoom, 1.1, accuracy: 0.001)
        XCTAssertTrue(model.pageZoom.levelsByHost.isEmpty)
        tab.applyStoredZoom()
        XCTAssertEqual(tab.webView.pageZoom, 1.1, accuracy: 0.001, "A commit keeps the private level")
        try await Task.sleep(nanoseconds: 450_000_000)
        XCTAssertFalse(FileManager.default.fileExists(atPath: home.appendingPathComponent("page-zoom.json").path))
        tab.changeZoom(0)
        XCTAssertEqual(tab.webView.pageZoom, PageZoomStore.standard)
    }

    func testZoomStepsFromTheNearestRung() {
        XCTAssertEqual(PageZoomStore.stepped(from: 1.07, 1), 1.25)
        XCTAssertEqual(PageZoomStore.stepped(from: 1.07, -1), 1.0)
        XCTAssertEqual(PageZoomStore.stepped(from: 1.0, 1), 1.1)
        XCTAssertEqual(PageZoomStore.stepped(from: 9, 1), PageZoomStore.levels.last)
        XCTAssertEqual(PageZoomStore.stepped(from: 0.1, -1), PageZoomStore.levels.first)
    }

    // MARK: Order and Spaces

    func testNumberShortcutsFollowTheVisibleOrder() {
        let model = makeModel()
        let a = model.newTab(), b = model.newTab(), c = model.newTab()
        c.pinned = true
        for onTop in [false, true] {
            model.tabsOnTop = onTop
            model.select(b.id)
            model.selectVisibleTab(at: 0)
            XCTAssertEqual(model.activeID, c.id, "Pins come first (tabs on top: \(onTop))")
            model.selectVisibleTab(at: 1)
            XCTAssertEqual(model.activeID, a.id)
            model.selectVisibleTab(at: 8)
            XCTAssertEqual(model.activeID, b.id, "⌘9 picks the last tab")
        }
    }

    func testCollapsedFolderOnlyContributesItsActiveTab() throws {
        let model = makeModel()
        try model.addFolder("Work")
        let folder = model.folders[0].id
        let inside = model.newTab(), sibling = model.newTab(), loose = model.newTab()
        model.moveToFolder(inside.id, folder: folder)
        model.moveToFolder(sibling.id, folder: folder)
        model.folders[0].collapsed = true
        model.select(loose.id)
        XCTAssertEqual(model.displayOrderedTabs.map(\.id), [loose.id])
        model.select(inside.id)
        XCTAssertEqual(model.displayOrderedTabs.map(\.id), [inside.id, loose.id])
        model.folders[0].collapsed = false
        XCTAssertEqual(model.displayOrderedTabs.map(\.id), [inside.id, sibling.id, loose.id])
        model.tabsOnTop = true
        XCTAssertEqual(model.displayOrderedTabs.map(\.id), [inside.id, sibling.id, loose.id], "The top strip uses array order")
    }

    func testAdjacentTabWrapsInBothDirections() {
        let model = makeModel()
        let a = model.newTab(), b = model.newTab(), c = model.newTab()
        XCTAssertEqual(model.activeID, c.id)
        model.selectAdjacentTab(1)
        XCTAssertEqual(model.activeID, a.id)
        model.selectAdjacentTab(-1)
        XCTAssertEqual(model.activeID, c.id)
        model.selectAdjacentTab(-1)
        XCTAssertEqual(model.activeID, b.id)
        model.activeID = nil
        model.selectAdjacentTab(1)
        XCTAssertEqual(model.activeID, a.id, "From the start page, next is the first tab")
        model.activeID = nil
        model.selectAdjacentTab(-1)
        XCTAssertEqual(model.activeID, c.id, "and previous is the last")
    }

    func testCycleSpaceWraps() {
        let model = makeModel()
        let first = model.space
        model.cycleSpace(1)
        XCTAssertEqual(model.space, model.spaces[1])
        model.cycleSpace(1)
        XCTAssertEqual(model.space, first)
        model.cycleSpace(-1)
        XCTAssertEqual(model.space, model.spaces[1])
    }

    func testSpaceReturnsToItsLastTab() throws {
        let model = makeModel()
        let first = model.space, second = model.spaces[1]
        let a = model.newTab(), _ = model.newTab(), c = model.newTab()
        model.select(c.id)
        // Most recent by date is A, so only the remembered tab explains C.
        a.lastActiveAt = Date().addingTimeInterval(60)
        model.switchSpace(second)
        XCTAssertEqual(model.active?.space, second)
        model.switchSpace(first)
        XCTAssertEqual(model.activeID, c.id)

        model.persistSession()
        let restored = makeModel()
        XCTAssertEqual(restored.lastActiveBySpace[first], c.id)
        XCTAssertNil(restored.activeID, "A relaunch still opens on the start page")
    }

    func testUnloadedFolderTabStaysClosedAcrossSpaceSwitches() throws {
        let model = makeModel()
        let first = model.space
        try model.addFolder("Reading")
        let tab = model.newTab()
        model.moveToFolder(tab.id, folder: model.folders[0].id)
        model.select(tab.id)
        model.closeSidebarTab(tab.id)
        XCTAssertTrue(tab.isSuspended)
        XCTAssertNil(model.activeID)
        model.switchSpace(model.spaces[1])
        model.switchSpace(first)
        XCTAssertNil(model.activeID, "The start page shows instead of reopening the page")
        XCTAssertTrue(tab.isSuspended)
    }

    func testSwitchingToTheCurrentSpaceKeepsTheTab() {
        let model = makeModel()
        _ = model.newTab()
        let second = model.newTab()
        model.select(second.id)
        model.switchSpace(model.space)
        XCTAssertEqual(model.activeID, second.id)
    }

    // MARK: Closing

    func testPinnedTabClosesPageBeforeEntry() {
        let model = makeModel()
        let pin = model.newTab()
        pin.pinned = true
        model.closeSidebarTab(pin.id)
        XCTAssertTrue(model.tabs.contains { $0.id == pin.id })
        XCTAssertTrue(pin.isSuspended)
        XCTAssertTrue(pin.pinned)
        XCTAssertNil(model.activeID)
        XCTAssertTrue(model.closedTabs.isEmpty)
        model.select(pin.id)
        XCTAssertFalse(pin.isSuspended)
        model.closeTab(pin.id)
        XCTAssertFalse(model.tabs.contains { $0.id == pin.id })
    }

    func testBatchCloseUndoesInOneStep() {
        let model = makeModel()
        let pin = model.newTab()
        pin.pinned = true
        let loose = (0..<25).map { _ in model.newTab() }
        let last = model.newTab()
        model.select(loose[3].id)
        model.closeTabs(loose.map(\.id), revealStartPage: false)
        XCTAssertEqual(model.tabs.map(\.id), [pin.id, last.id])
        XCTAssertEqual(model.closedTabUndoCount, 25)
        XCTAssertNotNil(model.closedTabUndoID)
        XCTAssertNotNil(model.activeID, "A replacement is selected")

        model.undoClosedTabs()
        XCTAssertEqual(model.tabs.map(\.id), [pin.id] + loose.map(\.id) + [last.id])
        XCTAssertEqual(model.activeID, loose[3].id)
        XCTAssertNil(model.closedTabUndoID)
        XCTAssertEqual(model.closedTabUndoCount, 1)
        XCTAssertTrue(model.closedTabs.isEmpty)
    }

    func testBatchCloseKeepsMoreThanTheUsualLimit() {
        let model = makeModel()
        let loose = (0..<60).map { _ in model.newTab() }
        model.closeTabs(loose.map(\.id), revealStartPage: true)
        XCTAssertEqual(model.closedTabs.count, 60)
        XCTAssertEqual(model.closedTabUndoCount, 60)
        XCTAssertNil(model.activeID)
    }

    func testCloseOtherTabsKeepsPinsFoldersAndTheChosenTab() throws {
        let model = makeModel()
        try model.addFolder("Keep")
        let pin = model.newTab()
        pin.pinned = true
        let filed = model.newTab()
        model.moveToFolder(filed.id, folder: model.folders[0].id)
        _ = model.newTab()
        let kept = model.newTab()
        let active = model.newTab()
        model.select(active.id)
        model.closeOtherTabs(except: kept.id)
        XCTAssertEqual(Set(model.tabs.map(\.id)), [pin.id, filed.id, kept.id])
        XCTAssertEqual(model.activeID, kept.id)
        XCTAssertEqual(model.closedTabUndoCount, 2)
        model.undoClosedTabs()
        XCTAssertEqual(model.tabs.count, 5)
        XCTAssertEqual(model.activeID, active.id)
    }

    func testClosingAPopupReturnsToItsOpener() {
        let model = makeModel()
        let opener = model.newTab()
        let other = model.newTab()
        let popup = model.newTab()
        popup.openerID = opener.id
        model.select(other.id)
        model.select(popup.id)
        model.closeTab(popup.id)
        XCTAssertEqual(model.activeID, opener.id, "Not the more recently used tab")
    }

    func testClosingASplitPaneShowsItsPartner() {
        let model = makeModel()
        let left = model.newTab(), right = model.newTab(), _ = model.newTab()
        model.pairTabs(right.id, with: left.id)
        model.select(left.id)
        model.closeTab(left.id)
        XCTAssertEqual(model.activeID, right.id)
    }

    func testAddressesAreValidatedBeforeUse() throws {
        XCTAssertThrowsError(try BrowserTab.resolveAddress("file:///a.pdf"))
        XCTAssertThrowsError(try BrowserTab.resolveAddress("ftp://h/x"))
        XCTAssertEqual(try BrowserTab.resolveAddress("example.com"), "https://example.com")
        XCTAssertEqual(try BrowserTab.resolveAddress("localhost:3000"), "http://localhost:3000")
        XCTAssertNil(try BrowserTab.resolveAddress("   "))
        XCTAssertNil(try BrowserTab.resolveAddress(ExtensionCatalog.internalURL))
    }

    func testSplitNeverBorrowsATabFromAnotherFolder() throws {
        let model = makeModel()
        try model.addFolder("A")
        try model.addFolder("B")
        let other = model.newTab()
        model.moveToFolder(other.id, folder: model.folders[1].id)
        let anchor = model.newTab()
        model.moveToFolder(anchor.id, folder: model.folders[0].id)
        model.select(anchor.id)
        model.toggleSplit()
        let partner = try XCTUnwrap(model.splitID)
        XCTAssertNotEqual(partner, other.id)
        XCTAssertEqual(model.tabs.count, 3)
        XCTAssertEqual(other.folderID, model.folders[1].id)
        XCTAssertEqual(model.tabs.first { $0.id == partner }?.folderID, model.folders[0].id)
    }

    func testFolderMoveRedrawsTheTabList() throws {
        let model = makeModel()
        try model.addFolder("Inbox")
        let tab = model.newTab()
        var changes = 0
        let subscription = model.objectWillChange.sink { changes += 1 }
        defer { subscription.cancel() }
        tab.folderID = model.folders[0].id
        XCTAssertEqual(changes, 1)
        tab.folderID = model.folders[0].id
        XCTAssertEqual(changes, 1, "An unchanged folder sends nothing")
    }

    // MARK: Toasts and model hooks

    func testBookmarkingShowsAToastInsteadOfANotice() async throws {
        let model = makeModel()
        let tab = model.newTab()
        try await load(tab, "<html><head><title>Saved</title></head><body>Saved</body></html>", at: "https://bookmark.test/")
        model.bookmarkActivePage()
        let added = try XCTUnwrap(model.toast)
        XCTAssertEqual(added.icon, "bookmark.fill")
        XCTAssertNil(model.notice)
        model.bookmarkActivePage()
        let duplicate = try XCTUnwrap(model.toast)
        XCTAssertEqual(duplicate.icon, "bookmark")
        XCTAssertNotEqual(duplicate.id, added.id)
        model.dismissToast(added.id)
        XCTAssertEqual(model.toast, duplicate, "A stale timer leaves the newer toast")
        model.dismissToast(duplicate.id)
        XCTAssertNil(model.toast)
        XCTAssertNil(model.notice)
    }

    func testCopyLinkWritesThePasteboard() {
        let model = makeModel()
        let pasteboard = NSPasteboard.general
        let previous = pasteboard.string(forType: .string)
        defer { pasteboard.clearContents(); if let previous { pasteboard.setString(previous, forType: .string) } }
        let tab = model.newTab()
        model.copyLink(tab)
        XCTAssertNil(model.toast, "A blank tab has no link")
        tab.url = "https://copy.test/page?x=1"
        model.copyLink(tab)
        XCTAssertEqual(pasteboard.string(forType: .string), "https://copy.test/page?x=1")
        XCTAssertEqual(model.toast?.icon, "link")
    }

    func testSwitchingToTabsOnTopHidesTheFloatingSidebar() {
        let model = makeModel()
        model.sidebarOverlayVisible = true
        model.tabsOnTop = true
        XCTAssertFalse(model.sidebarOverlayVisible)
    }

    func testFindRequestsFocusEachTime() {
        let model = makeModel()
        let tab = model.newTab()
        tab.requestFind()
        tab.requestFind()
        XCTAssertTrue(tab.showFind)
        XCTAssertEqual(tab.findFocusRequest, 2)
    }

    func testSearchUsesTheChosenEngine() throws {
        let defaults = UserDefaults.standard
        let previous = defaults.object(forKey: SearchEngine.defaultsKey)
        defer { if let previous { defaults.set(previous, forKey: SearchEngine.defaultsKey) } else { defaults.removeObject(forKey: SearchEngine.defaultsKey) } }

        defaults.set("google", forKey: SearchEngine.defaultsKey)
        let google = try XCTUnwrap(URLComponents(string: try XCTUnwrap(BrowserTab.resolveAddress("foo bar"))))
        XCTAssertEqual(google.host, "www.google.com")
        XCTAssertEqual(google.path, "/search")
        XCTAssertEqual(google.queryItems?.first { $0.name == "q" }?.value, "foo bar")

        defaults.set("startpage", forKey: SearchEngine.defaultsKey)
        let startpage = try XCTUnwrap(URLComponents(string: try XCTUnwrap(BrowserTab.resolveAddress("foo bar"))))
        XCTAssertEqual(startpage.host, "www.startpage.com")
        XCTAssertEqual(startpage.queryItems?.first { $0.name == "query" }?.value, "foo bar")

        defaults.set("unknown", forKey: SearchEngine.defaultsKey)
        XCTAssertEqual(SearchEngine.current, .duckDuckGo)
        defaults.removeObject(forKey: SearchEngine.defaultsKey)
        XCTAssertEqual(URL(string: try XCTUnwrap(BrowserTab.resolveAddress("foo bar")))?.host, "duckduckgo.com")
        for engine in SearchEngine.allCases { XCTAssertEqual(engine.url(for: "x").scheme, "https") }
    }

    func testProfilesShareOneUpdater() {
        let first = makeModel()
        let second = BrowserModel(root: home.appendingPathComponent("other"))
        XCTAssertTrue(first.updates === second.updates)
        XCTAssertTrue(first.updates === UpdateService.shared)
    }

    // MARK: Navigation policy

    func testDataFramesRender() async throws {
        let model = makeModel()
        let tab = model.newTab()
        let frame = "data:text/html,%3Cscript%3Eparent.postMessage('framed','*')%3C/script%3E"
        let html = "<html><body><script>addEventListener('message', e => { if (e.data === 'framed') document.body.dataset.frame = e.data })</script><iframe src=\"\(frame)\"></iframe></body></html>"
        try await load(tab, html, at: "https://frames.test/")
        var value: String?
        for _ in 0..<40 {
            value = try? await tab.webView.evaluateJavaScript("document.body.dataset.frame || ''") as? String
            if value == "framed" { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertEqual(value, "framed")
    }
}
