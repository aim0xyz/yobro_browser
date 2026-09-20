import XCTest
import SwiftUI
@testable import YOBRO

final class ExtensionHubTests: XCTestCase {
    func testSearchEscapesOnePathComponentWithoutCountryStorefront() throws {
        let url = try XCTUnwrap(ExtensionCatalog.searchURL("  Übersetzer / 日本?x=#  "))
        XCTAssertEqual(url.host, "chromewebstore.google.com")
        XCTAssertNil(url.query)
        XCTAssertNil(url.fragment)
        XCTAssertTrue(url.absoluteString.contains("%2F"))
        XCTAssertFalse(url.absoluteString.contains("/us/"))
        XCTAssertNil(ExtensionCatalog.searchURL(" \n "))
        XCTAssertFalse(ExtensionCatalog.isInternal("yobro://extensions.evil.example"))
    }

    @MainActor
    func testHubReusesTabAndRestoresWithoutWebNavigation() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.openExtensionsHub()
        let tab = try XCTUnwrap(model.active)
        let count = model.tabs.count
        XCTAssertTrue(tab.isExtensionsHub)
        XCTAssertNil(tab.webView.url)
        model.openExtensionsHub()
        XCTAssertEqual(model.activeID, tab.id)
        XCTAssertEqual(model.tabs.count, count)
        let restored = BrowserTab(saved: tab.stored, owner: model)
        restored.loadPersistedContent()
        XCTAssertTrue(restored.isExtensionsHub)
        XCTAssertNil(restored.webView.url)
        XCTAssertNil(restored.stored.interactionState)
        XCTAssertFalse(restored.loading)
    }

    @MainActor
    func testHubRendersInLightAndDarkAppearance() async throws {
        for (name, appearance) in [("light", NSAppearance.Name.aqua), ("dark", .darkAqua)] {
            let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: home) }
            let model = BrowserModel(root: home)
            let host = NSHostingView(rootView: ExtensionHubPage(model: model))
            host.appearance = NSAppearance(named: appearance)
            host.frame = NSRect(x: 0, y: 0, width: 1000, height: 1000)
            let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            window.contentView = host
            window.orderFront(nil)
            try await Task.sleep(nanoseconds: 350_000_000)
            host.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
            XCTAssertGreaterThan(data.count, 10_000)
            try data.write(to: URL(fileURLWithPath: "/tmp/YOBRO-extensions-\(name).png"))
            window.close()
        }
    }
}
