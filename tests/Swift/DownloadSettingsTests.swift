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

    @MainActor
    func testPrivateDownloadsAreListedButNeverSaved() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = DownloadStore(home: root, isolated: true)
        let ordinary = DownloadEntry(source: "https://example.com/report.pdf", state: "completed")
        let secret = DownloadEntry(source: "https://private.example/secret.pdf", state: "completed")

        store.register(ordinary, ephemeral: false)
        store.register(secret, ephemeral: true)

        XCTAssertEqual(store.entries.map(\.id), [secret.id, ordinary.id])
        XCTAssertEqual(store.persistentEntries.map(\.id), [ordinary.id])
        let saved = try JSONDecoder().decode([DownloadEntry].self, from: Data(contentsOf: root.appendingPathComponent("downloads.json")))
        XCTAssertEqual(saved.map(\.id), [ordinary.id])
        XCTAssertEqual(DownloadStore(home: root, isolated: true).entries.map(\.id), [ordinary.id])
    }

    @MainActor
    func testRemovingAndClearingKeepRunningDownloads() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = DownloadStore(home: root, isolated: true)
        let running = DownloadEntry(source: "https://example.com/large.zip")
        let done = DownloadEntry(source: "https://example.com/done.zip", state: "completed")
        let failed = DownloadEntry(source: "https://example.com/failed.zip", state: "failed")
        let hidden = DownloadEntry(source: "https://private.example/a.zip", state: "cancelled")
        for entry in [running, done, failed] { store.register(entry, ephemeral: false) }
        store.register(hidden, ephemeral: true)

        store.remove(running.id)
        XCTAssertTrue(store.entries.contains { $0.id == running.id }, "A running download has to be cancelled first")
        store.remove(failed.id)
        XCTAssertFalse(store.entries.contains { $0.id == failed.id })

        store.clearFinished()
        XCTAssertEqual(store.entries.map(\.id), [running.id])
        XCTAssertEqual(store.persistentEntries.map(\.id), [running.id])
        let saved = try JSONDecoder().decode([DownloadEntry].self, from: Data(contentsOf: root.appendingPathComponent("downloads.json")))
        XCTAssertEqual(saved.map(\.id), [running.id])
    }
}
