import XCTest
import WebKit
import Security
import SwiftUI
@testable import YOBRO

final class BrowserImportTests: XCTestCase {
    @MainActor
    func testProfileChangeClearsPreviouslyUnlockedPasswords() {
        let importer = BrowserImportStore()
        importer.profileID = "old"
        importer.passwordsLoaded = true
        importer.preview.passwords = [ImportPassword(url: "https://example.invalid", username: "test", password: "synthetic")]
        importer.needsPrimaryPassword = true
        importer.selectProfile("new")
        XCTAssertTrue(importer.preview.passwords.isEmpty)
        XCTAssertFalse(importer.passwordsLoaded)
        XCTAssertFalse(importer.needsPrimaryPassword)
    }

    func testNativePasswordDecryptRejectsUnknownFormats() {
        XCTAssertThrowsError(try NativePasswordImport.decrypt(Data("v20unsupported".utf8), key: Data(repeating: 0, count: 16)))
        XCTAssertThrowsError(try NativePasswordImport.decrypt(Data("v10short".utf8), key: Data(repeating: 0, count: 16)))
    }

    func testCRXPayloadBoundsAndVersions() throws {
        let zip = Data([0x50,0x4b,0x03,0x04,1,2,3])
        let crx3 = Data([0x43,0x72,0x32,0x34,3,0,0,0,2,0,0,0,0,0]) + zip
        XCTAssertEqual(try ExtensionPackage.zipPayload(crx3), zip)
        let crx2 = Data([0x43,0x72,0x32,0x34,2,0,0,0,1,0,0,0,1,0,0,0,0,0]) + zip
        XCTAssertEqual(try ExtensionPackage.zipPayload(crx2), zip)
        for data in [Data(), Data(crx3.prefix(10)), Data([0x43,0x72,0x32,0x34,3,0,0,0,255,255,255,255])] {
            XCTAssertThrowsError(try ExtensionPackage.zipPayload(data))
        }
    }
    @MainActor
    func testImportMergeIsIdempotentAndKeepsExistingHistory() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel()
        model.history = [HistoryEntry(title: "Existing", url: "https://existing.invalid")]
        let bookmarks = [ImportLink(title: "Test", url: "https://example.invalid", folder: "Work")]
        let history = [ImportLink(title: "Imported", url: "https://example.invalid", timestamp: 1700000000, visits: 4)]
        let counts = try model.mergeImport(bookmarks: bookmarks, history: history)
        XCTAssertEqual(counts.0, 1); XCTAssertEqual(counts.1, 1)
        let repeated = try model.mergeImport(bookmarks: bookmarks, history: history)
        XCTAssertEqual(repeated.0, 0); XCTAssertEqual(repeated.1, 0)
        XCTAssertEqual(model.history.count, 2)
        XCTAssertTrue(model.history.contains { $0.title == "Existing" })
        XCTAssertEqual(try JSONDecoder().decode([BookmarkEntry].self, from: Data(contentsOf: home.appendingPathComponent("bookmarks.json"))).count, 1)
    }
    @MainActor
    func testArcTabImportCreatesSpacesFoldersAndKeepsPlacement() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel()
        let importedSpaces = [ImportSpace(sourceID: "arc-space", name: "Arc Work")]
        let importedFolders = [ImportTabFolder(sourceID: "arc-folder", name: "Projects / Orbit", space: "arc-space")]
        let links = [
            ImportLink(title: "Pinned", url: "https://pinned.invalid", pinned: true, space: "arc-space"),
            ImportLink(title: "Folder", url: "https://folder.invalid", pinned: false, space: "arc-space", tabFolderID: "arc-folder")
        ]
        let first = model.mergeImportedTabs(links, spaces: importedSpaces, folders: importedFolders)
        XCTAssertEqual(first.tabs, 2); XCTAssertEqual(first.spaces, 1); XCTAssertEqual(first.folders, 1)
        let folder = try XCTUnwrap(model.folders.first { $0.space == "Arc Work" })
        XCTAssertTrue(model.tabs.contains { $0.url == "https://pinned.invalid" && $0.space == "Arc Work" && $0.pinned })
        XCTAssertTrue(model.tabs.contains { $0.url == "https://folder.invalid" && $0.folderID == folder.id && !$0.pinned })
        let repeated = model.mergeImportedTabs(links, spaces: importedSpaces, folders: importedFolders)
        XCTAssertEqual(repeated.tabs, 0); XCTAssertEqual(repeated.spaces, 0); XCTAssertEqual(repeated.folders, 0)
    }
    @MainActor
    func testCookieImportPreservesExistingUnlessSelected() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(), importer = BrowserImportStore()
        let original = HTTPCookie(properties: [.domain: "example.invalid", .path: "/", .name: "test", .value: "original"])!
        await model.websiteDataStore.httpCookieStore.setCookie(original)
        importer.selected = [.cookies]
        let incoming = ImportCookie(name: "test", value: "replacement", domain: "example.invalid", path: "/", expires: 0, secure: false, httpOnly: true)
        importer.preview.cookies = [incoming]
        await importer.apply(to: model)
        var cookies = await model.websiteDataStore.httpCookieStore.allCookies()
        XCTAssertEqual(cookies.first?.value, "original")
        importer.preview.cookies = [incoming]; importer.replaceCookies = true
        await importer.apply(to: model)
        cookies = await model.websiteDataStore.httpCookieStore.allCookies()
        XCTAssertEqual(cookies.first?.value, "replacement"); XCTAssertEqual(cookies.first?.isHTTPOnly, true)
    }
    @MainActor
    func testImportWindowLayoutAndWorkerFileRoundTrip() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer { if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel()
        let fixture = home.appendingPathComponent("tabs.txt")
        try "https://example.invalid/one\nfile:///not-allowed\n".write(to: fixture, atomically: true, encoding: .utf8)
        let result = try await ImportWorker.call(["action":"file","kind":"tabs","path":fixture.path], as: ImportPreview.self)
        XCTAssertEqual(result.tabs.count, 1)
        let view = NSHostingView(rootView: BrowserImportView(model: model))
        view.frame = NSRect(x: 0, y: 0, width: 770, height: 660)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = view
        window.orderFront(nil)
        try await Task.sleep(nanoseconds: 200_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
        view.cacheDisplay(in: view.bounds, to: bitmap)
        let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        try data.write(to: URL(fileURLWithPath: "/tmp/YOBRO-import-layout.png"))
        window.close()
    }
    func testPasswordVaultDuplicatePolicy() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { SecItemDelete([kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:PasswordVault.service(home: home)] as CFDictionary) }
        let first = ImportPassword(url: "https://yobro-test.invalid/login", username: "synthetic", password: "first-synthetic")
        let second = ImportPassword(url: first.url, username: first.username, password: "second-synthetic")
        XCTAssertTrue(try PasswordVault.store(first, home: home, replace: false))
        XCTAssertFalse(try PasswordVault.store(second, home: home, replace: false))
        XCTAssertEqual(try PasswordVault.entries(home: home).first?.password, first.password)
        XCTAssertTrue(try PasswordVault.store(second, home: home, replace: true))
        XCTAssertEqual(try PasswordVault.entries(home: home).first?.password, second.password)
    }
}
