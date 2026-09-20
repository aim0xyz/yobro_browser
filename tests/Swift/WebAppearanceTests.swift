import XCTest
import WebKit
import AppKit
@testable import YOBRO

@MainActor
final class WebAppearanceTests: XCTestCase {
    func testNativeWebsiteFollowsWindowAppearanceWithoutConversion() async throws {
        let web = AppearanceWebView(frame:NSRect(x:0,y:0,width:600,height:400))
        let window = NSWindow(contentRect:web.frame,styleMask:[.borderless],backing:.buffered,defer:false)
        window.isReleasedWhenClosed = false; window.contentView = web; window.orderFront(nil)
        defer { window.close() }
        web.loadHTMLString("<style>body{background:white}@media(prefers-color-scheme:dark){body{background:rgb(20,20,20)}}</style><body>Native theme</body>",baseURL:nil)
        for dark in [true,false,true] {
            window.appearance = NSAppearance(named:dark ? .darkAqua : .aqua)
            try await Task.sleep(nanoseconds:300_000_000)
            let actual = try await web.evaluateJavaScript("matchMedia('(prefers-color-scheme: dark)').matches") as? Bool
            XCTAssertEqual(actual,dark)
            let color = try await web.evaluateJavaScript("getComputedStyle(document.body).backgroundColor") as? String
            XCTAssertEqual(color,dark ? "rgb(20, 20, 20)" : "rgb(255, 255, 255)")
        }
    }
    func testPreferencesPersist() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let appearance = WebAppearance(home: home)
        XCTAssertFalse(appearance.preferences.enabled)
        appearance.setEnabled(true); appearance.setExcluded("EXAMPLE.COM", true)
        let restored = WebAppearance(home: home)
        XCTAssertTrue(restored.preferences.enabled)
        XCTAssertTrue(restored.preferences.excludedHosts.contains("example.com"))
    }

    func testRenderingAndLiveSystemChanges() async throws {
        _ = NSApplication.shared
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let appearance = WebAppearance(home: home)
        appearance.setEnabled(true)
        let config = WKWebViewConfiguration()
        appearance.install(on: config.userContentController)
        let view = AppearanceWebView(frame: NSRect(x: 0, y: 0, width: 900, height: 700), configuration: config)
        view.appearanceChanged = { [weak view] in if let view { appearance.update(view) } }
        view.appearance = NSAppearance(named: .darkAqua)
        let window = NSWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = view
        window.orderFront(nil)
        defer { window.orderOut(nil) }
        func js(_ script: String) async throws -> Any? {
            try await withCheckedThrowingContinuation { continuation in
                view.evaluateJavaScript(script, in: nil, in: WebAppearance.world) { result in continuation.resume(with: result) }
            }
        }
        func settle() async throws { try await Task.sleep(nanoseconds: 900_000_000) }
        view.loadHTMLString("<html><body style='background:white;color:black'><h1>Light page</h1><input value='Hello'><img id='photo' src='data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22/%3E'></body></html>", baseURL: URL(string: "https://example.com"))
        for _ in 0..<30 {
            try await Task.sleep(nanoseconds: 100_000_000)
            if (try? await js("Boolean(window.__yobroAppearance)")) as? Bool == true { break }
        }
        try await settle()
        let result1 = try await js("window.__yobroAppearance.status().state") as? String
        XCTAssertEqual(result1, "converted")
        let dark = try await js("getComputedStyle(document.body).backgroundColor") as? String
        XCTAssertNotEqual(dark, "rgb(255, 255, 255)")
        let result2 = try await js("getComputedStyle(document.querySelector('#photo')).filter") as? String
        XCTAssertEqual(result2, "none")
        _ = try await js("document.body.insertAdjacentHTML('beforeend', '<section id=dynamic style=\"background:white;color:black;padding:24px\">Später geladener Inhalt</section>');")
        try await settle()
        let dynamicColor = try await js("getComputedStyle(document.querySelector('#dynamic')).backgroundColor") as? String
        XCTAssertNotEqual(dynamicColor, "rgb(255, 255, 255)")
        let inputReadable = try await js("getComputedStyle(document.querySelector('input')).color !== getComputedStyle(document.querySelector('input')).backgroundColor") as? Bool
        XCTAssertEqual(inputReadable, true)
        appearance.setEnabled(false); appearance.update(view); try await settle()
        let result3 = try await js("getComputedStyle(document.body).backgroundColor") as? String
        XCTAssertEqual(result3, "rgb(255, 255, 255)")
        appearance.setEnabled(true); appearance.update(view); try await settle()
        view.appearance = NSAppearance(named: .aqua); try await settle()
        let result4 = try await js("window.__yobroAppearance.status().state") as? String
        XCTAssertEqual(result4, "original")
        let result5 = try await js("getComputedStyle(document.body).backgroundColor") as? String
        XCTAssertEqual(result5, "rgb(255, 255, 255)")
        view.appearance = NSAppearance(named: .darkAqua); try await settle()
        let result6 = try await js("window.__yobroAppearance.status().state") as? String
        XCTAssertEqual(result6, "converted")
        appearance.setExcluded("example.com", true); appearance.update(view); try await settle()
        let result7 = try await js("window.__yobroAppearance.status().state") as? String
        XCTAssertEqual(result7, "original")
        appearance.setExcluded("example.com", false); appearance.update(view); try await settle()
        // A site's native switch must take precedence, even after YOBRO has converted it.
        _ = try await js("document.head.insertAdjacentHTML('beforeend', '<style>body.native{background:#151515!important;color:#eeeeee!important}</style>'); document.body.className='native';")
        try await settle()
        let result8 = try await js("window.__yobroAppearance.status().state") as? String
        XCTAssertEqual(result8, "native")
        let result9 = try await js("getComputedStyle(document.body).backgroundColor") as? String
        XCTAssertEqual(result9, "rgb(21, 21, 21)")
    }

    func testAppearanceWebViewHitTestYieldsOnlyWhenOverlayOrDragIsActive() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer {
            UserDefaults.standard.removeObject(forKey: "YOBRO.sidebarAutoHide")
            try? FileManager.default.removeItem(at: home)
        }
        let model = BrowserModel(root: home)
        model.showSidebar = false
        model.sidebarAutoHide = true
        model.sidebarOverlayVisible = false

        let tab = model.newTab()
        let web = tab.webView as! AppearanceWebView
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700), styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = web
        window.orderFront(nil)
        defer { window.orderOut(nil) }

        let pointInsideSidebarX = NSPoint(x: 100, y: 350)
        XCTAssertNotNil(web.hitTest(pointInsideSidebarX))

        model.sidebarOverlayVisible = true
        XCTAssertNil(web.hitTest(pointInsideSidebarX))

        model.sidebarOverlayVisible = false
        XCTAssertNotNil(web.hitTest(pointInsideSidebarX))

        model.beginSidebarTabDrag(tab.id)
        XCTAssertNil(web.hitTest(pointInsideSidebarX))

        model.finishSidebarDrag(sessionID: model.sidebarDragSessionID)
        XCTAssertNotNil(web.hitTest(pointInsideSidebarX))
    }
}
