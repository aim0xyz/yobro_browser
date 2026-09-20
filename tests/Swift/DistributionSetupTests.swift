import XCTest
import WebKit
@testable import YOBRO

final class DistributionSetupTests: XCTestCase {
    @MainActor
    func testBrowserAccessRequiresExplicitConsentAndPersistsPause() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", root.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: root)
        }
        let model = BrowserModel(root: root)
        XCTAssertFalse(model.agentEnabled)
        _ = try await model.handle(["command": "status"])
        do { _ = try await model.handle(["command": "tabs"]); XCTFail("Fresh profile must reject access") } catch {}
        model.agentEnabled = true
        XCTAssertTrue(AgentAccessPermission.load(home: root).allowed)
        _ = try await model.handle(["command": "tabs"])
        model.pauseAgentWorkspace()
        XCTAssertFalse(AgentAccessPermission.load(home: root).allowed)
    }

    func testConnectionStoresOnlySocketBasenameAndPrivatePermissions() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let socket = root.appendingPathComponent("p-\(UUID().uuidString).sock")
        try AgentConnectionBinding.save(client: .claude, socket: socket)
        XCTAssertEqual(AgentConnectionBinding.read(client: .claude, root: root)?.socketName, socket.lastPathComponent)
        XCTAssertNil(AgentConnectionBinding.read(client: .codex, root: root))
        let file = AgentConnectionBinding.file(client: .claude, root: root)
        let object = try JSONSerialization.jsonObject(with: Data(contentsOf: file)) as! [String: String]
        XCTAssertEqual(Set(object.keys), ["socketName"])
        XCTAssertFalse(try String(contentsOf: file).contains(root.path))
        let permissions = try FileManager.default.attributesOfItem(atPath: file.path)[.posixPermissions] as! NSNumber
        XCTAssertEqual(permissions.intValue, 0o600)
    }

    func testNewAndCorruptAgentPermissionFailClosed() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        XCTAssertFalse(AgentAccessPermission.load(home: root).allowed)
        let file = root.appendingPathComponent("agent-access.json")
        try Data("bad data".utf8).write(to: file)
        XCTAssertFalse(AgentAccessPermission.load(home: root).allowed)
        try JSONEncoder().encode(AgentAccessPermission(allowed: true)).write(to: file)
        XCTAssertTrue(AgentAccessPermission.load(home: root).allowed)
    }

    @MainActor
    func testVPNShipsEmptyAndMissingLegacyConfigurationDoesNotFailOpen() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let empty = ManagedVPNStore(home: root)
        XCTAssertTrue(empty.locations.isEmpty)
        XCTAssertFalse(empty.isActive)
        try Data("{\"location\":\"old-location\"}".utf8).write(to: root.appendingPathComponent("managed-vpn.json"))
        let previous = ManagedVPNStore(home: root)
        await previous.restore(to: .nonPersistent())
        XCTAssertTrue(previous.isActive)
        XCTAssertNotNil(previous.isolationFailure)
        XCTAssertFalse(previous.isConnected)
    }
}
