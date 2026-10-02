import XCTest
@testable import YOBRO

final class BroWorkspaceTests: XCTestCase {
    @MainActor
    func testFolderReadingStaysWithinAddedRootAndSkipsSecrets() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let folder = home.appendingPathComponent("project")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        try Data("Project instructions".utf8).write(to: folder.appendingPathComponent("AGENTS.md"))
        try Data("secret".utf8).write(to: folder.appendingPathComponent(".env"))
        for name in [".envrc", "auth.json", "secrets.yaml", "access_token.json"] {
            try Data("secret".utf8).write(to: folder.appendingPathComponent(name))
        }
        let store = BroWorkspaceStore(home: home)
        try store.add(folder)
        let id = try XCTUnwrap(store.folders.first?.id)
        XCTAssertTrue(try store.list(id).contains("AGENTS.md"))
        XCTAssertFalse(try store.list(id).contains(".env"))
        XCTAssertEqual(try store.read(id, path: "AGENTS.md"), "Project instructions")
        XCTAssertThrowsError(try store.read(id, path: ".env"))
        for name in [".envrc", "auth.json", "secrets.yaml", "access_token.json"] {
            XCTAssertFalse(try store.list(id).contains(name))
            XCTAssertThrowsError(try store.read(id, path: name))
        }
        XCTAssertThrowsError(try store.read(id, path: "../outside"))
        XCTAssertEqual(BroWorkspaceStore(home: home).folders.first?.path, folder.path)
    }

    func testOnlyKnownAppStoreHQCommandsAreAccepted() throws {
        XCTAssertFalse(try BroASCCommand(["app", "Huggletails"]).requiresApproval)
        XCTAssertTrue(try BroASCCommand(["new-version", "Huggletails", "--version", "1.4.0", "--platform", "IOS"]).requiresApproval)
        XCTAssertThrowsError(try BroASCCommand(["submit", "Huggletails"]))
        XCTAssertThrowsError(try BroASCCommand(["new-version", "Huggletails", "--version", "1.4.0"]))
        XCTAssertThrowsError(try BroASCCommand(["app"]))
    }
}
