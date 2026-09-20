import XCTest
@testable import YOBRO

@MainActor
final class AgentActivityTests: XCTestCase {
    func testSocketUsageShowsActivityWithoutSplitAndEndsExplicitly() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        _ = try await model.handle(["command": "status"])
        XCTAssertFalse(model.agentUsageActive, "An availability check alone is not an agent session")
        _ = try await model.handle(["command": "tabs"])
        XCTAssertTrue(model.agentUsageActive)
        XCTAssertNil(model.agentTab)
        XCTAssertFalse(model.agentWorkspaceVisible)
        model.updateAgentWorkspacePresentation(appIsActive: false)
        XCTAssertTrue(model.agentUsageActive, "Backgrounding must not hide the activity indicator")
        _ = try await model.handle(["command": "end"])
        XCTAssertFalse(model.agentUsageActive)
    }

    func testClosingLastTabDoesNotEndSessionAndPauseClearsIt() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        let tab = model.newAgentTab()
        model.updateAgentWorkspacePresentation(appIsActive: false)
        XCTAssertFalse(model.agentWorkspaceVisible)
        XCTAssertTrue(model.agentUsageActive)
        model.closeTab(tab.id)
        XCTAssertTrue(model.agentUsageActive)
        model.pauseAgentWorkspace()
        XCTAssertFalse(model.agentUsageActive)
        model.agentEnabled = true
        XCTAssertFalse(model.agentUsageActive, "Re-enabling access must not restore stale activity")
    }

    func testBuiltInChatIndicatesActivityWithoutBrowserTab() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        model.chat.running = true
        XCTAssertTrue(model.agentUsageActive)
        XCTAssertNil(model.agentTab)
        model.chat.running = false
        XCTAssertFalse(model.agentUsageActive)
    }
}
