import XCTest
import SwiftUI
@testable import YOBRO

final class AgentWorkspaceTests: XCTestCase {
    @MainActor
    func testSpaceSwitchStaysResponsiveWithAgentSidebarOpenAndClosed() async throws {
        for sidebarOpen in [false, true] {
            let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            let model = BrowserModel(root: home)
            model.agentEnabled = true
            defer { try? FileManager.default.removeItem(at: home) }
            let original = model.space
            _ = model.newTab(space: "Studio")
            model.switchSpace(original)
            _ = model.newAgentTab()
            model.showAgent = sidebarOpen
            let view = NSHostingView(rootView: BrowserShell(model: model))
            view.frame = NSRect(x: 0, y: 0, width: 1100, height: 720)
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .resizable], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            window.contentView = view
            window.orderFront(nil)
            defer { window.close() }
            view.layoutSubtreeIfNeeded()

            let started = ContinuousClock.now
            model.switchSpace("Studio")
            view.layoutSubtreeIfNeeded()
            XCTAssertLessThan(started.duration(to: .now), .seconds(1))
            XCTAssertEqual(model.space, "Studio")
            XCTAssertTrue(model.agentTabIDs.isEmpty)
            XCTAssertFalse(model.agentWorkspaceVisible)
            XCTAssertEqual(model.showAgent, sidebarOpen)
            await Task.yield()
        }
    }
    @MainActor
    func testAgentWebViewIsDetachedWhileAnotherAppIsActive() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.agentEnabled = true
        let agent = model.newAgentTab()

        model.updateAgentWorkspacePresentation(appIsActive: false)
        XCTAssertFalse(model.agentWorkspaceVisible)
        XCTAssertEqual(model.agentTab?.id, agent.id, "Backgrounding must not end the agent session")
        XCTAssertTrue(model.agentTabIDs.contains(agent.id))

        model.updateAgentWorkspacePresentation(appIsActive: true)
        XCTAssertTrue(model.agentWorkspaceVisible)
        XCTAssertEqual(model.agentTab?.id, agent.id)
    }

    @MainActor
    func testAgentIsolationAndPause() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: home)
        }
        let model = BrowserModel()
        model.agentEnabled = true
        let user = model.newTab()
        _ = try await model.handle(["command": "status"])
        XCTAssertFalse(model.agentWorkspaceVisible)
        _ = try await model.handle(["command": "tabs"])
        XCTAssertFalse(model.agentWorkspaceVisible, "Listing tabs alone must not take over the right split")
        _ = try await model.handle(["command": "new"])
        let agent = try XCTUnwrap(model.agentTab)
        XCTAssertTrue(model.agentWorkspaceVisible)
        XCTAssertEqual(model.activeID, user.id)
        XCTAssertFalse(agent.webView.acceptsFirstResponder)
        XCTAssertFalse(model.visibleTabs.contains { $0.id == agent.id })
        model.select(agent.id)
        XCTAssertEqual(model.activeID, user.id)
        for command in ["close", "focus", "reload", "pin"] {
            do {
                _ = try await model.handle(["command": command, "tab": user.id.uuidString])
                XCTFail("User tab must be protected: \(command)")
            } catch {}
        }
        let next = model.newTab()
        XCTAssertEqual(model.agentTabID, agent.id)
        _ = try await model.handle(["command": "focus", "tab": agent.id.uuidString])
        XCTAssertEqual(model.activeID, next.id)
        XCTAssertFalse(model.pairTabs(agent.id, with: next.id))
        XCTAssertTrue(model.pairTabs(user.id, with: next.id))
        user.webView.loadHTMLString("<html><body>User secondary</body></html>", baseURL: URL(string: "https://user-secondary.invalid"))
        next.webView.loadHTMLString("<html><body>User active</body></html>", baseURL: URL(string: "https://user-active.invalid"))
        agent.webView.loadHTMLString("<html><body>Agent</body></html>", baseURL: URL(string: "https://agent.invalid"))
        try await Task.sleep(for: .milliseconds(200))
        // XCTest's host is not an active GUI application; model the foreground
        // state before SwiftUI builds the hierarchy under test.
        model.updateAgentWorkspacePresentation(appIsActive: true)
        let view = NSHostingView(rootView: BrowserShell(model: model))
        view.frame = NSRect(x: 0, y: 0, width: 1400, height: 850)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(300))
        view.layoutSubtreeIfNeeded()
        let userRect = next.webView.convert(next.webView.bounds, to: view)
        let agentRect = agent.webView.convert(agent.webView.bounds, to: view)
        XCTAssertGreaterThanOrEqual(agentRect.minX, userRect.maxX - 2)
        XCTAssertGreaterThan(userRect.width, 280)
        XCTAssertGreaterThan(agentRect.width, 280)
        XCTAssertNil(user.webView.window, "A second user split must stay hidden while the agent owns the right side")
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: "/tmp/YOBRO-agent-layout.png"))
        agent.webView.loadHTMLString("<html><body><label>Name<input id='name'></label><button onclick=\"document.getElementById('out').textContent=document.getElementById('name').value\">Apply</button><p id='out'>Ready</p></body></html>", baseURL: URL(string: "https://fixture.invalid"))
        try await Task.sleep(for: .milliseconds(300))
        let snapshot = try await model.handle(["command": "read"])
        let page = try XCTUnwrap(snapshot["page"] as? [String: Any])
        let elements = try XCTUnwrap(page["elements"] as? [[String: Any]])
        let input = try XCTUnwrap(elements.first { $0["tag"] as? String == "input" }?["ref"] as? String)
        let button = try XCTUnwrap(elements.first { $0["tag"] as? String == "button" }?["ref"] as? String)
        let document = try XCTUnwrap(page["document"] as? String)
        window.makeFirstResponder(next.webView)
        _ = try await model.handle(["command": "fill", "ref": input, "document": document, "value": "Parallel works"])
        _ = try await model.handle(["command": "click", "ref": button, "document": document])
        let updated = try await model.handle(["command": "read"])
        XCTAssertTrue(((updated["page"] as? [String: Any])?["text"] as? String)?.contains("Parallel works") == true)
        XCTAssertEqual(model.activeID, next.id)
        XCTAssertFalse(window.firstResponder === agent.webView)
        model.pauseAgentWorkspace()
        XCTAssertFalse(model.agentEnabled)
        XCTAssertFalse(model.agentWorkspaceVisible)
        XCTAssertTrue(model.agentTabIDs.isEmpty)
        XCTAssertEqual(model.activeID, next.id)
        try await Task.sleep(for: .milliseconds(200))
        view.layoutSubtreeIfNeeded()
        XCTAssertNotNil(user.webView.window, "The user's previous two-tab split should return after the agent leaves")
        XCTAssertFalse(model.tabs.contains { $0.id == agent.id }, "Agent-owned tabs must not become user tabs after the session")
        do { _ = try await model.handle(["command": "new"]); XCTFail("Paused") } catch {}
        model.agentEnabled = true
        _ = try await model.handle(["command": "new"])
        model.switchSpace("Studio")
        XCTAssertFalse(model.agentEnabled)
        XCTAssertFalse(model.agentWorkspaceVisible)
    }
}
