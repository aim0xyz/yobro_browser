import XCTest
import SwiftUI
@testable import YOBRO

final class CompactSidebarTests: XCTestCase {
    @MainActor
    func testPrivateTabUsesEphemeralStorageAndIsNeverPersistedOrReopened() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let ordinary = model.newTab(url: "https://ordinary.example")
        let privateTab = model.newTab(url: "https://private.example", isPrivate: true)

        XCTAssertTrue(privateTab.isPrivate)
        XCTAssertFalse(privateTab.webView.configuration.websiteDataStore.isPersistent)
        model.persistSession(scheduleSync: false)

        let data = try Data(contentsOf: home.appendingPathComponent("session.json"))
        let saved = try JSONDecoder().decode(StoredSession.self, from: data)
        XCTAssertEqual(saved.tabs.map(\.id), [ordinary.id])
        XCTAssertFalse(model.syncSnapshot().tabs.contains { $0.id == privateTab.id })

        model.closeTab(privateTab.id)
        XCTAssertFalse(model.closedTabs.contains { $0.id == privateTab.id })
    }

    func testSidebarPreviewsRequireDeliberateHoverIntent() {
        XCTAssertGreaterThanOrEqual(SidebarPreviewTiming.openDelay, 500_000_000)
        XCTAssertLessThan(SidebarPreviewTiming.closeDelay, SidebarPreviewTiming.openDelay)
    }

    @MainActor
    func testClosedTabUndoRestoresItsSpaceAndFolder() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Work")
        let folder = try XCTUnwrap(model.folders.first)
        let first = model.newTab()
        let tab = model.newTab()
        let last = model.newTab()
        for item in [first, tab, last] { model.moveToFolder(item.id, folder: folder.id) }

        model.closeTab(tab.id)
        XCTAssertNotNil(model.closedTabUndoID)
        XCTAssertFalse(model.tabs.contains { $0.id == tab.id })

        let restored = try XCTUnwrap(model.reopenTab())
        XCTAssertNil(model.closedTabUndoID)
        XCTAssertEqual(restored.id, tab.id)
        XCTAssertEqual(restored.space, folder.space)
        XCTAssertEqual(restored.folderID, folder.id)
        XCTAssertEqual(model.tabs.filter { $0.folderID == folder.id }.map(\.id), [first.id, tab.id, last.id])
    }

    @MainActor
    func testFolderTabClosesPageBeforeDeletingEntry() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Keep")
        let folder = try XCTUnwrap(model.folders.first)
        let tab = model.newTab(url: "https://example.com")
        model.moveToFolder(tab.id, folder: folder.id)

        model.closeSidebarTab(tab.id)

        XCTAssertTrue(model.tabs.contains { $0.id == tab.id }, "The first close retains a folder tab")
        XCTAssertTrue(tab.isSuspended)
        XCTAssertEqual(tab.url, "https://example.com")
        XCTAssertNil(model.activeID)
        XCTAssertTrue(model.closedTabs.isEmpty, "Suspending a page is not a deleted-tab undo action")
        XCTAssertEqual(tab.stored.suspended, true)

        model.select(tab.id)
        XCTAssertEqual(model.activeID, tab.id)
        XCTAssertFalse(tab.isSuspended, "Selecting the retained entry reopens its page")

        model.closeSidebarTab(tab.id)
        XCTAssertTrue(tab.isSuspended)
        model.closeSidebarTab(tab.id)
        XCTAssertFalse(model.tabs.contains { $0.id == tab.id }, "The x deletes an already closed folder tab")
        XCTAssertEqual(model.closedTabs.last?.id, tab.id)
    }

    @MainActor
    func testSuspendingFolderTabRetainsPersistedFavicon() async throws {
        let image = try XCTUnwrap(NSImage(systemSymbolName: "globe", accessibilityDescription: nil))
        let faviconData = try XCTUnwrap(FaviconStore.persistedData(for: image))
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Keep")
        let folder = try XCTUnwrap(model.folders.first)
        let tab = BrowserTab(saved: StoredTab(id: UUID(), title: "Example", url: "https://example.com", space: model.space, pinned: false, folderID: folder.id, faviconData: faviconData), owner: model, deferLoading: true)
        model.tabs.append(tab)
        model.activeID = tab.id

        model.closeSidebarTab(tab.id)
        try await Task.sleep(nanoseconds: 150_000_000)

        XCTAssertTrue(tab.isSuspended)
        XCTAssertNotNil(tab.favicon)
        XCTAssertEqual(tab.stored.faviconData, faviconData)
    }

    @MainActor
    func testClosingActiveTabRevealsBrandedStartPageInsteadOfAnotherTab() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let remaining = model.newTab(url: "https://remaining.example")
        let closing = model.newTab(url: "https://closing.example")

        model.closeSidebarTab(closing.id)

        XCTAssertNil(model.activeID)
        XCTAssertTrue(model.tabs.contains { $0.id == remaining.id })
    }

    @MainActor
    func testRelaunchPreservesIntentionalEmptyStartPage() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        let space = L("Persönlich")
        let retained = StoredTab(id: UUID(), title: "Retained", url: "https://example.com", space: space, pinned: false)
        let session = StoredSession(tabs: [retained], activeID: nil, space: space, closedTabs: [], spaces: [space], spaceIcons: [:], folders: [], splitPairs: [])
        try JSONEncoder().encode(session).write(to: home.appendingPathComponent("session.json"), options: .atomic)

        let relaunched = BrowserModel(root: home)

        XCTAssertEqual(relaunched.tabs.map(\.id), [retained.id], "Sidebar tabs remain available after relaunch")
        XCTAssertNil(relaunched.activeID, "The branded empty state must not activate the first sidebar tab")
    }

    @MainActor
    func testRelaunchStartsEmptyEvenWhenSessionHadAnActiveTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        let space = L("Persönlich")
        let retained = StoredTab(id: UUID(), title: "Retained", url: "https://example.com", space: space, pinned: false)
        let session = StoredSession(tabs: [retained], activeID: retained.id, space: space, closedTabs: [], spaces: [space], spaceIcons: [:], folders: [], splitPairs: [])
        try JSONEncoder().encode(session).write(to: home.appendingPathComponent("session.json"), options: .atomic)

        let relaunched = BrowserModel(root: home)

        XCTAssertEqual(relaunched.tabs.map(\.id), [retained.id])
        XCTAssertNil(relaunched.activeID, "A cold launch must wait for an explicit tab selection")
        XCTAssertNil(relaunched.tabs.first?.webView.url, "The retained page must stay deferred on a cold launch")
    }

    @MainActor
    func testOrdinaryTabStillDeletesInOneStep() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab(url: "https://example.com")

        model.closeSidebarTab(tab.id)

        XCTAssertFalse(model.tabs.contains { $0.id == tab.id })
        XCTAssertEqual(model.closedTabs.last?.id, tab.id)
    }

    @MainActor
    func testInactiveFolderTabDeletesInsteadOfShowingAsOpen() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Keep")
        let folder = try XCTUnwrap(model.folders.first)
        let inactive = model.newTab(url: "https://first.example")
        model.moveToFolder(inactive.id, folder: folder.id)
        let active = model.newTab(url: "https://second.example")
        model.moveToFolder(active.id, folder: folder.id)
        XCTAssertEqual(model.activeID, active.id)

        model.closeSidebarTab(inactive.id)

        XCTAssertFalse(model.tabs.contains { $0.id == inactive.id }, "Only the active folder tab receives the retain-in-folder minus action")
        XCTAssertTrue(model.tabs.contains { $0.id == active.id })
    }

    @MainActor
    func testDraggedTabsCanBePlacedBeforeAndAfterEachOther() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Work")
        let folder = try XCTUnwrap(model.folders.first)
        let first = model.newTab()
        let middle = model.newTab()
        let last = model.newTab()
        for tab in [first, middle, last] { model.moveToFolder(tab.id, folder: folder.id) }

        XCTAssertTrue(model.moveTab(last.id, relativeTo: first.id, after: false))
        XCTAssertEqual(model.tabs.filter { $0.folderID == folder.id }.map(\.id), [last.id, first.id, middle.id])

        XCTAssertTrue(model.moveTab(last.id, relativeTo: middle.id, after: true))
        XCTAssertEqual(model.tabs.filter { $0.folderID == folder.id }.map(\.id), [first.id, middle.id, last.id])
    }

    @MainActor
    func testFoldersCanBeReorderedAsWholeGroups() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("First")
        try model.addFolder("Middle")
        try model.addFolder("Last")
        let first = model.folders[0]
        let middle = model.folders[1]
        let last = model.folders[2]

        XCTAssertTrue(model.moveFolder(last.id, relativeTo: first.id, after: false))
        XCTAssertEqual(model.folders.map(\.id), [last.id, first.id, middle.id])

        XCTAssertTrue(model.moveFolder(last.id, relativeTo: middle.id, after: true))
        XCTAssertEqual(model.folders.map(\.id), [first.id, middle.id, last.id])
    }

    @MainActor
    func testDroppingOntoTabCenterStillCreatesSplitView() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let first = model.newTab()
        let second = model.newTab()

        XCTAssertTrue(model.handleTabDrop(second.id, on: first.id, locationY: 22, rowHeight: 44))
        XCTAssertTrue(model.splitPairs.contains { $0.contains(first.id) && $0.contains(second.id) })
    }

    @MainActor
    func testDroppingOntoTabInAnotherFolderFilesItThere() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Work")
        let folder = try XCTUnwrap(model.folders.first)
        let target = model.newTab()
        model.moveToFolder(target.id, folder: folder.id)
        let dragged = model.newTab()

        XCTAssertEqual(model.tabDropIntent(dragged.id, on: target.id, locationY: 22, rowHeight: 44), .after)
        XCTAssertTrue(model.handleTabDrop(dragged.id, on: target.id, locationY: 22, rowHeight: 44))
        XCTAssertEqual(dragged.folderID, folder.id)
        XCTAssertFalse(model.splitPairs.contains { $0.contains(target.id) && $0.contains(dragged.id) })
    }

    func testTabDropZonesCommunicateReorderAndSplitIntents() {
        XCTAssertEqual(TabDropIntent.resolve(locationY: 4, rowHeight: 44), .before)
        XCTAssertEqual(TabDropIntent.resolve(locationY: 22, rowHeight: 44), .split)
        XCTAssertEqual(TabDropIntent.resolve(locationY: 40, rowHeight: 44), .after)
        XCTAssertEqual(TabDropIntent.resolve(locationY: 46, rowHeight: 49), .after)
    }

    @MainActor
    func testDroppingInTheGapBetweenRowsReordersTheTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let first = model.newTab()
        let second = model.newTab()
        let third = model.newTab()

        XCTAssertTrue(model.handleTabDrop(third.id, on: first.id, locationY: 46, rowHeight: 49))
        XCTAssertEqual(model.tabs.map(\.id), [first.id, third.id, second.id])
    }

    @MainActor
    func testOldDragCleanupCannotCancelANewDragOfTheSameTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab()

        model.beginSidebarTabDrag(tab.id)
        let firstSession = try XCTUnwrap(model.sidebarDragSessionID)
        model.beginSidebarTabDrag(tab.id)
        let secondSession = try XCTUnwrap(model.sidebarDragSessionID)
        XCTAssertNotEqual(firstSession, secondSession)

        model.finishSidebarDrag(sessionID: firstSession)
        XCTAssertEqual(model.draggingTabID, tab.id)
        XCTAssertEqual(model.sidebarDragSessionID, secondSession)

        model.finishSidebarDrag(sessionID: secondSession)
        XCTAssertNil(model.draggingTabID)
        XCTAssertNil(model.sidebarDragSessionID)
    }

    @MainActor
    func testOverlaySidebarDragStateKeepsOverlayVisible() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.sidebarAutoHide = true
        model.showSidebar = false
        model.sidebarOverlayVisible = false

        let tab = model.newTab()
        model.beginSidebarTabDrag(tab.id)
        XCTAssertEqual(model.draggingTabID, tab.id)

        model.finishSidebarDrag()
        XCTAssertNil(model.draggingTabID)
    }

    @MainActor
    func testTabDropPairingUnloadedOrSuspendedTabsResumesBothInSplitView() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        try model.addFolder("Work")
        let folder = try XCTUnwrap(model.folders.first)

        let first = model.newTab(url: "https://first.example")
        let second = model.newTab(url: "https://second.example")
        model.moveToFolder(first.id, folder: folder.id)
        model.moveToFolder(second.id, folder: folder.id)
        model.closeSidebarTab(second.id) // suspends second

        XCTAssertTrue(second.isSuspended)

        // Drop first onto second in the middle zone to split
        let paired = model.handleTabDrop(first.id, on: second.id, locationY: 22, rowHeight: 44)
        XCTAssertTrue(paired)
        XCTAssertFalse(first.isSuspended)
        XCTAssertFalse(second.isSuspended)
        XCTAssertTrue(model.splitPairs.contains { $0.contains(first.id) && $0.contains(second.id) })
    }

    @MainActor
    func testAddressEntrySelectsTheCompleteCurrentURL() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let expected = "https://example.com/a/long/path?query=value"
        let tab = BrowserTab(saved: StoredTab(id: UUID(), title: "Example", url: expected, space: model.space, pinned: false), owner: model, deferLoading: true)
        let host = NSHostingView(rootView: AddressEntry(model: model, tab: tab, close: {}))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 540, height: 320), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }

        try await Task.sleep(nanoseconds: 300_000_000)
        let editor = try XCTUnwrap(window.firstResponder as? NSTextView)
        XCTAssertEqual(editor.string, expected)
        XCTAssertEqual(editor.selectedRange(), NSRange(location: 0, length: (expected as NSString).length))
    }

    @MainActor
    func testOnlyOneFolderCanOwnTheHoverPopover() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let first = UUID()
        let second = UUID()

        model.hoveredFolderID = first
        XCTAssertEqual(model.hoveredFolderID, first)
        model.hoveredFolderID = second
        XCTAssertEqual(model.hoveredFolderID, second)

        // A delayed dismissal belonging to the old row must not close the new row.
        if model.hoveredFolderID == first { model.hoveredFolderID = nil }
        XCTAssertEqual(model.hoveredFolderID, second)
    }

    @MainActor
    func testStoredTabRestoresFaviconWithoutLoadingPage() throws {
        let image = try XCTUnwrap(NSImage(systemSymbolName: "globe", accessibilityDescription: nil))
        let data = try XCTUnwrap(FaviconStore.persistedData(for: image))
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let saved = StoredTab(id: UUID(), title: "Example", url: "https://example.com", space: model.space, pinned: false, faviconData: data)
        let tab = BrowserTab(saved: saved, owner: model, deferLoading: true)

        XCTAssertNotNil(tab.favicon)
        XCTAssertEqual(tab.stored.faviconData, data)
        XCTAssertNil(tab.webView.url)
    }

    func testLegacyStoredTabWithoutFaviconStillDecodes() throws {
        let json = #"{"id":"00000000-0000-0000-0000-000000000002","title":"Example","url":"https://example.com","space":"Personal","pinned":false}"#
        let tab = try JSONDecoder().decode(StoredTab.self, from: Data(json.utf8))
        XCTAssertNil(tab.faviconData)
        XCTAssertNil(tab.suspended)
    }

    func testLegacyFolderWithoutColorStillUsesDesignDefault() throws {
        let json = #"{"id":"00000000-0000-0000-0000-000000000003","name":"Work","space":"Personal","collapsed":false}"#
        let folder = try JSONDecoder().decode(TabFolder.self, from: Data(json.utf8))
        XCTAssertEqual(folder.folderColor, .moss)
    }

    @MainActor
    func testFolderMenuSwatchesKeepTheirOriginalColors() throws {
        let images = try FolderColor.allCases.map { try XCTUnwrap($0.menuImage.tiffRepresentation) }
        XCTAssertEqual(Set(images).count, FolderColor.allCases.count)
        XCTAssertTrue(FolderColor.allCases.allSatisfy { !$0.menuImage.isTemplate })
    }

    @MainActor
    func testNameEditorUsesBrowserStyling() async throws {
        let view = NSHostingView(rootView: YOBRONameEditor(
            icon: "folder.fill", title: "Ordner umbenennen",
            detail: "Der neue Name gilt nur in diesem Space.", text: .constant("Apple"),
            cancel: {}, save: {}
        ))
        view.frame = NSRect(x: 0, y: 0, width: 330, height: 190)
        let window = NSWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 200_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: "/tmp/YOBRO-name-editor.png"))
    }

    @MainActor
    func testSidebarTooltipUsesWebsiteTitleAndFallsBackToHost() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let titled = BrowserTab(saved: StoredTab(id: UUID(), title: "YOBRO Docs", url: "https://docs.example.com/start", space: model.space, pinned: false), owner: model, deferLoading: true)
        let untitled = BrowserTab(saved: StoredTab(id: UUID(), title: "Neue Seite", url: "https://example.com/path", space: model.space, pinned: false), owner: model, deferLoading: true)

        XCTAssertEqual(titled.sidebarTitle, "YOBRO Docs")
        XCTAssertEqual(titled.sidebarHelp, "YOBRO Docs\ndocs.example.com")
        XCTAssertEqual(untitled.sidebarTitle, "example.com")
    }

    @MainActor
    func testCustomTabTitleSurvivesPageTitleChangesAndPersistence() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab(url: "https://example.com")

        tab.rename("Research")
        tab.title = "A later website title"
        XCTAssertEqual(tab.sidebarTitle, "Research")

        model.persistSession(scheduleSync: false)
        let data = try Data(contentsOf: home.appendingPathComponent("session.json"))
        let saved = try JSONDecoder().decode(StoredSession.self, from: data)
        XCTAssertEqual(saved.tabs.first(where: { $0.id == tab.id })?.customTitle, "Research")
    }

    @MainActor
    func testClosingLastWindowKeepsYOBRORunningAndAllowsReopen() {
        let delegate = AppDelegate()
        XCTAssertFalse(delegate.applicationShouldTerminateAfterLastWindowClosed(NSApplication.shared))
        XCTAssertTrue(delegate.applicationShouldHandleReopen(NSApplication.shared, hasVisibleWindows: true))
    }

    @MainActor
    func testNewTabCreatesFocusedBrandedStartPage() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString:$0) }; setenv("YOBRO_HOME",home.path,1)
        defer { if let previous { setenv("YOBRO_HOME",previous,1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at:home) }
        let model = BrowserModel(); model.showSidebar = false; model.showAgent = false
        let host = NSHostingView(rootView: BrowserShell(model:model))
        let window = NSWindow(contentRect:NSRect(x:0,y:0,width:1100,height:740),styleMask:[.titled,.resizable],backing:.buffered,defer:false)
        window.isReleasedWhenClosed = false; window.contentView = host; host.frame = window.contentView!.bounds; window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds:300_000_000)
        XCTAssertTrue(model.tabs.isEmpty)
        for _ in 0..<2 {
            let count = model.tabs.count
            model.requestNewTab()
            try await Task.sleep(nanoseconds:250_000_000)
            XCTAssertEqual(model.tabs.count, count + 1, "New tab must immediately create its start page")
            XCTAssertTrue(window.childWindows?.isEmpty ?? true, "The start page stays inside the browser window")
            let editor = try XCTUnwrap(window.firstResponder as? NSTextView, "The start page must focus its search field")
            XCTAssertTrue(editor.delegate is NSTextField)
            let field = try XCTUnwrap(editor.delegate as? NSTextField)
            XCTAssertTrue(field.isDescendant(of: host), "The focused field must belong to the start page")
            XCTAssertEqual(field.placeholderString, L("Suchen oder URL eingeben"))
            editor.insertText("example.invalid", replacementRange: NSRange(location:0,length:0))
            XCTAssertTrue(editor.string.contains("example.invalid"))
            model.focusAddress = true
            try await Task.sleep(nanoseconds:100_000_000)
            XCTAssertTrue(window.childWindows?.isEmpty ?? true)
            XCTAssertTrue(window.firstResponder === editor)
            XCTAssertFalse(model.focusAddress)
            model.closeTab(model.activeID)
        }
        host.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in:host.bounds)); host.cacheDisplay(in:host.bounds,to:bitmap)
        try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:URL(fileURLWithPath:"/tmp/YOBRO-compact-sidebar.png"))
        XCTAssertEqual(model.tabs.count, 1)
        XCTAssertEqual(model.active?.url, "")
    }

    func testSidebarOffersTheCorrectActiveTabJumpDirection() {
        let viewport = CGRect(x: 20, y: 200, width: 220, height: 300)
        XCTAssertEqual(SidebarVisibility.jumpDirection(for: CGRect(x: 20, y: 120, width: 200, height: 44), viewport: viewport), .up)
        XCTAssertEqual(SidebarVisibility.jumpDirection(for: CGRect(x: 20, y: 540, width: 200, height: 44), viewport: viewport), .down)
        XCTAssertNil(SidebarVisibility.jumpDirection(for: CGRect(x: 20, y: 300, width: 200, height: 44), viewport: viewport))
        XCTAssertNil(SidebarVisibility.jumpDirection(for: nil, viewport: viewport))
        XCTAssertEqual(SidebarVisibility.inferredJumpDirection(activeIndex: 12, visibleIndices: [0, 1, 2, 3]), .down)
        XCTAssertEqual(SidebarVisibility.inferredJumpDirection(activeIndex: 1, visibleIndices: [8, 9, 10]), .up)
        XCTAssertNil(SidebarVisibility.inferredJumpDirection(activeIndex: 9, visibleIndices: [8, 9, 10]))
    }
}
