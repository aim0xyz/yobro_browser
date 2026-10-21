import XCTest
import SwiftUI
@testable import YOBRO

final class SettingsDesignTests: XCTestCase {
    @MainActor
    func testSettingsRenderInLightAndDarkAppearance() async throws {
        let sections = [L("Allgemein", "General"), L("Agenten", "Agents"), L("Datenschutz & Werbung", "Privacy & ads"), L("Downloads", "Downloads"),
                        L("Erweiterungen"), L("Daten importieren"), L("Lesezeichen"), L("Passwörter")]
        for (name, appearance) in [("light", NSAppearance.Name.aqua), ("dark", .darkAqua)] {
            let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            // Keeps the import page from scanning this Mac's real browser profiles.
            let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
            setenv("YOBRO_HOME", home.path, 1)
            defer {
                if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
                try? FileManager.default.removeItem(at: home)
            }
            let model = BrowserModel(root: home)
            for (index, section) in sections.enumerated() {
                let host = NSHostingView(rootView: BrowserSettings(model: model, section: section))
                host.appearance = NSAppearance(named: appearance)
                host.frame = NSRect(x: 0, y: 0, width: 860, height: 650)
                let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
                window.isReleasedWhenClosed = false
                window.contentView = host
                window.orderFront(nil)
                try await Task.sleep(nanoseconds: 250_000_000)
                host.layoutSubtreeIfNeeded()
                let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                host.cacheDisplay(in: host.bounds, to: bitmap)
                let data = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                XCTAssertGreaterThan(data.count, 10_000, "\(section) in \(name) mode")
                try data.write(to: URL(fileURLWithPath: index == 0 ? "/tmp/YOBRO-settings-\(name).png" : "/tmp/YOBRO-settings-\(name)-\(index).png"))
                window.close()
            }
        }
    }

    @MainActor
    func testDownloadSettingsRender() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let host = NSHostingView(rootView: BrowserSettings(model: model, section: L("Downloads", "Downloads")))
        host.frame = NSRect(x: 0, y: 0, width: 860, height: 650)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        try await Task.sleep(nanoseconds: 250_000_000)
        host.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        XCTAssertGreaterThan(try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).count, 10_000)
        window.close()
    }
}
