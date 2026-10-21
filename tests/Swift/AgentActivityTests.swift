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

    func testClosingAgentSplitKeepsChatPermission() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        _ = model.newAgentTab()
        model.closeAgentWorkspace()
        XCTAssertTrue(model.agentEnabled)
        XCTAssertNil(model.agentTab)
        XCTAssertFalse(model.agentWorkspaceVisible)
        XCTAssertTrue(AgentAccessPermission.load(home: home).allowed)
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

    func testClosedAgentTabIsNotOfferedForReopening() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        let tab = model.newAgentTab()
        model.closeTab(tab.id)
        XCTAssertTrue(model.closedTabs.isEmpty)
        XCTAssertNil(model.closedTabUndoID)
    }

    func testChatTabsCloseOnSpaceSwitchWithoutRevokingAccess() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        _ = model.newAgentTab(startsExternalSession: false)
        XCTAssertFalse(model.agentSessionActive)
        model.switchSpace(model.spaces[1])
        XCTAssertTrue(model.agentEnabled)
        XCTAssertTrue(AgentAccessPermission.load(home: home).allowed)
        XCTAssertTrue(model.agentTabIDs.isEmpty)
        XCTAssertFalse(model.agentUsageActive)
    }

    func testAgentPagesStayOutOfHistory() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        let tab = model.newAgentTab()
        tab.webView.loadHTMLString("<html><head><title>Agent</title></head><body>Agent</body></html>", baseURL: URL(string: "https://agent-history.test/"))
        for _ in 0..<60 where tab.webView.isLoading || tab.webView.url == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(tab.webView.url?.host, "agent-history.test")
        XCTAssertFalse(model.history.contains { $0.url.contains("agent-history.test") })
    }

    func testPagesCannotMultiplyAgentTabs() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        let opener = model.newAgentTab()
        opener.webView.loadHTMLString("<html><body>Opener</body></html>", baseURL: URL(string: "https://agent-popups.test/"))
        for _ in 0..<60 where opener.webView.isLoading || opener.webView.url == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        for _ in 0..<(BrowserModel.maxAgentTabs + 2) {
            _ = try? await opener.webView.evaluateJavaScript("window.open('about:blank'); true")
        }
        XCTAssertEqual(model.agentTabIDs.count, BrowserModel.maxAgentTabs)
    }

    func testManualStopCancelsCurrentChat() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        model.chat.running = true
        model.stopAgentRun()
        XCTAssertFalse(model.chat.running)
        XCTAssertTrue(model.agentEnabled)
    }
}
