import XCTest
import SwiftUI
import WebKit
@testable import YOBRO

final class TabHibernationTests: XCTestCase {
    override func setUp() {
        super.setUp()
        UserDefaults.standard.removeObject(forKey: "YOBRO.autoSuspendInactiveTabs")
    }

    override func tearDown() {
        UserDefaults.standard.removeObject(forKey: "YOBRO.autoSuspendInactiveTabs")
        super.tearDown()
    }

    /// Hibernation only releases pages that were actually loaded, so tests
    /// commit a local document instead of waiting on the network.
    @MainActor
    private func loadedTab(_ model: BrowserModel, _ address: String) async throws -> BrowserTab {
        let tab = model.newTab()
        tab.webView.loadHTMLString("<html><body>\(address)</body></html>", baseURL: URL(string: address))
        for _ in 0..<100 where tab.webView.isLoading || tab.webView.url == nil || tab.loading {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertFalse(tab.url.isEmpty)
        return tab
    }

    @MainActor
    func testInactiveTabSuspendsAfterInactivityTimeout() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let activeTab = try await loadedTab(model, "https://active.example/")
        let inactiveTab = try await loadedTab(model, "https://background.example/")
        model.select(activeTab.id)

        // Simulate 1 hour of inactivity on the background tab
        inactiveTab.lastActiveAt = Date().addingTimeInterval(-3600)
        activeTab.lastActiveAt = Date().addingTimeInterval(-3600)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)

        XCTAssertTrue(suspended.contains(inactiveTab.id))
        XCTAssertTrue(inactiveTab.isSuspended)
        XCTAssertFalse(activeTab.isSuspended, "Active tab must never be suspended")
    }

    @MainActor
    func testActiveAndSplitTabsAreNeverSuspended() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let leftTab = try await loadedTab(model, "https://left.example/")
        let rightTab = try await loadedTab(model, "https://right.example/")
        model.pairTabs(rightTab.id, with: leftTab.id)
        model.select(leftTab.id)

        leftTab.lastActiveAt = Date().addingTimeInterval(-7200)
        rightTab.lastActiveAt = Date().addingTimeInterval(-7200)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)

        XCTAssertFalse(suspended.contains(leftTab.id))
        XCTAssertFalse(suspended.contains(rightTab.id))
        XCTAssertFalse(leftTab.isSuspended)
        XCTAssertFalse(rightTab.isSuspended)
    }

    @MainActor
    func testAgentTabsAreNeverSuspended() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let agentTab = model.newAgentTab(url: "https://agent.example")
        let mainTab = model.newTab(url: "https://main.example")
        model.select(mainTab.id)

        agentTab.lastActiveAt = Date().addingTimeInterval(-7200)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)

        XCTAssertFalse(suspended.contains(agentTab.id))
        XCTAssertFalse(agentTab.isSuspended)
    }

    @MainActor
    func testSelectingSuspendedTabResumesIt() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let currentTab = try await loadedTab(model, "https://current.example/")
        let tabToSuspend = try await loadedTab(model, "https://hibernate.example/")
        model.select(currentTab.id)

        tabToSuspend.lastActiveAt = Date().addingTimeInterval(-3600)
        await model.suspendInactiveTabs(olderThan: 1800)
        XCTAssertTrue(tabToSuspend.isSuspended)

        let beforeResume = Date()
        model.select(tabToSuspend.id)

        XCTAssertFalse(tabToSuspend.isSuspended, "Selecting suspended tab must resume it")
        XCTAssertEqual(model.activeID, tabToSuspend.id)
        XCTAssertGreaterThanOrEqual(tabToSuspend.lastActiveAt, beforeResume.addingTimeInterval(-1))
    }

    @MainActor
    func testDisabledAutoSuspendPreventsHibernation() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        // The switch is flipped in another profile's settings; this model's
        // own copy of the value is stale.
        let otherProfile = BrowserModel(root: home.appendingPathComponent("other"))
        otherProfile.autoSuspendInactiveTabs = false

        let activeTab = try await loadedTab(model, "https://active.example/")
        let inactiveTab = try await loadedTab(model, "https://background.example/")
        model.select(activeTab.id)

        inactiveTab.lastActiveAt = Date().addingTimeInterval(-7200)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)
        XCTAssertTrue(suspended.isEmpty)
        XCTAssertFalse(inactiveTab.isSuspended)
    }

    /// Writes a session with one tab carrying `state`, as an earlier run would.
    @MainActor
    private func restoredModel(home: URL, state: Data) throws -> (BrowserModel, BrowserTab) {
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        let saved = StoredTab(id: UUID(), title: "Saved", url: "https://saved.example.invalid/", space: "Studio", pinned: false, interactionState: state)
        let session = StoredSession(tabs: [saved], activeID: nil, space: "Studio", closedTabs: nil)
        try JSONEncoder().encode(session).write(to: home.appendingPathComponent("session.json"))
        let model = BrowserModel(root: home)
        return (model, try XCTUnwrap(model.tabs.first { $0.id == saved.id }))
    }

    @MainActor
    private func savedState(_ home: URL, _ id: UUID) throws -> Data? {
        let session = try JSONDecoder().decode(StoredSession.self, from: Data(contentsOf: home.appendingPathComponent("session.json")))
        return session.tabs.first { $0.id == id }?.interactionState
    }

    @MainActor
    func testUnshownRestoredTabKeepsItsHistory() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let state = Data("back-forward list".utf8)
        let (model, tab) = try restoredModel(home: home, state: state)

        XCTAssertTrue(model.persistSession())
        XCTAssertEqual(try savedState(home, tab.id), state, "The first save must not replace the history")

        tab.suspend(forced: true)
        XCTAssertTrue(tab.isSuspended)
        XCTAssertNil(tab.webView.url, "A never-loaded tab starts no blank document")
        XCTAssertTrue(model.persistSession())
        XCTAssertEqual(try savedState(home, tab.id), state)
    }

    @MainActor
    func testNavigatingASuspendedTabKeepsTheNewAddress() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let (model, tab) = try restoredModel(home: home, state: Data("old history".utf8))
        tab.suspend(forced: true)
        try tab.navigate("https://next.example.invalid/")
        XCTAssertFalse(tab.isSuspended)
        model.select(tab.id)
        XCTAssertFalse(tab.isSuspended)
        XCTAssertEqual(tab.url, "https://next.example.invalid/")
        XCTAssertEqual(tab.webView.url?.absoluteString, "https://next.example.invalid/")
    }

    @MainActor
    func testSelectedTabIsNoLongerAHibernationCandidate() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let current = try await loadedTab(model, "https://current.example/")
        let background = try await loadedTab(model, "https://background.example/")
        model.select(current.id)
        background.lastActiveAt = Date().addingTimeInterval(-3600)
        XCTAssertTrue(model.isHibernationCandidate(background, timeout: 1800))
        model.select(background.id)
        XCTAssertFalse(model.isHibernationCandidate(background, timeout: 1800))
        let blank = model.newTab()
        blank.lastActiveAt = Date().addingTimeInterval(-3600)
        model.select(current.id)
        XCTAssertFalse(model.isHibernationCandidate(blank, timeout: 1800), "Nothing loaded, nothing to release")
    }

    @MainActor
    func testRepeatedCrashStopsReloading() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab()
        tab.url = "https://crash.example.invalid/"
        tab.webViewWebContentProcessDidTerminate(tab.webView)
        XCTAssertNil(tab.failure, "The first crash reloads")
        tab.webViewWebContentProcessDidTerminate(tab.webView)
        XCTAssertNotNil(tab.failure, "A second crash within a minute shows the error")
    }
}
