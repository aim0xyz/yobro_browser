import XCTest
@testable import YOBRO

final class DownloadSettingsTests: XCTestCase {
    @MainActor
    func testDownloadDirectoryPersistsAndCanBeReset() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let selected = root.appendingPathComponent("My Downloads")
        let store = DownloadStore(home: root, isolated: true)
        XCTAssertTrue(store.usesDefaultDirectory)
        XCTAssertTrue(try store.setDirectory(selected))
        XCTAssertEqual(store.directory.path, selected.standardizedFileURL.path)
        XCTAssertTrue(FileManager.default.fileExists(atPath: selected.path))

        let restored = DownloadStore(home: root, isolated: true)
        XCTAssertEqual(restored.directory.path, selected.standardizedFileURL.path)
        XCTAssertFalse(restored.usesDefaultDirectory)
        XCTAssertTrue(try restored.resetDirectory())
        XCTAssertEqual(restored.directory.path, root.appendingPathComponent("Downloads").standardizedFileURL.path)
    }

    @MainActor
    func testDownloadDirectoryRejectsAFile() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("not-a-folder")
        try Data().write(to: file)

        let store = DownloadStore(home: root, isolated: true)
        XCTAssertThrowsError(try store.setDirectory(file))
        XCTAssertTrue(store.usesDefaultDirectory)
    }
}
