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

    @MainActor
    func testInactiveTabSuspendsAfterInactivityTimeout() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let activeTab = model.newTab(url: "https://active.example")
        model.select(activeTab.id)

        let inactiveTab = model.newTab(url: "https://background.example")
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

        let leftTab = model.newTab(url: "https://left.example")
        let rightTab = model.newTab(url: "https://right.example")
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
    func testNotesAndAgentTabsAreNeverSuspended() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let note = model.newNote()
        let agentTab = model.newAgentTab(url: "https://agent.example")
        let mainTab = model.newTab(url: "https://main.example")
        model.select(mainTab.id)

        note.lastActiveAt = Date().addingTimeInterval(-7200)
        agentTab.lastActiveAt = Date().addingTimeInterval(-7200)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)

        XCTAssertFalse(suspended.contains(note.id))
        XCTAssertFalse(suspended.contains(agentTab.id))
        XCTAssertFalse(note.isSuspended)
        XCTAssertFalse(agentTab.isSuspended)
    }

    @MainActor
    func testSelectingSuspendedTabResumesIt() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)

        let currentTab = model.newTab(url: "https://current.example")
        let tabToSuspend = model.newTab(url: "https://hibernate.example")
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
        model.autoSuspendInactiveTabs = false

        let activeTab = model.newTab(url: "https://active.example")
        let inactiveTab = model.newTab(url: "https://background.example")
        model.select(activeTab.id)

        inactiveTab.lastActiveAt = Date().addingTimeInterval(-7200)

        let suspended = await model.suspendInactiveTabs(olderThan: 1800)
        XCTAssertTrue(suspended.isEmpty)
        XCTAssertFalse(inactiveTab.isSuspended)
    }
}
