import XCTest
import SwiftUI
import Combine
@testable import YOBRO

final class MailHelpAndPinTests: XCTestCase {
    func testProviderHelpRecognition() {
        XCTAssertEqual(MailPasswordProvider.detect(provider:"Automatisch",account:MailAccount(address:"test@gmail.com")), .google)
        XCTAssertEqual(MailPasswordProvider.detect(provider:"Automatisch",account:MailAccount(address:"test@me.com")), .apple)
        XCTAssertEqual(MailPasswordProvider.detect(provider:"Manuell",account:MailAccount(address:"test@custom.invalid",imapHost:"imap.gmail.com")), .google)
        XCTAssertNil(MailPasswordProvider.detect(provider:"Spacemail",account:MailAccount(address:"test@gmail.com")))
        XCTAssertNil(MailPasswordProvider.detect(provider:"Automatisch",account:MailAccount(address:"test@example.invalid")))
    }
    @MainActor
    func testOpeningMailFromTheEmptyStartPage() {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        XCTAssertTrue(model.tabs.isEmpty)

        model.presentMail()

        XCTAssertTrue(model.showMail)
        XCTAssertTrue(model.tabs.isEmpty)
    }
    @MainActor
    func testPinPublishesWithoutSelectingAnotherTab() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString:$0) }; setenv("YOBRO_HOME",home.path,1)
        defer { if let previous { setenv("YOBRO_HOME",previous,1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at:home) }
        let model = BrowserModel()
        let current = model.newTab()
        let selected = model.activeID
        var changes = 0
        let observation = model.objectWillChange.sink { changes += 1 }
        defer { observation.cancel() }
        current.pinned = true
        XCTAssertGreaterThan(changes, 0)
        XCTAssertEqual(model.visibleTabs.filter(\.pinned).map(\.id), [current.id])
        let afterPin = changes
        current.pinned = false
        XCTAssertGreaterThan(changes, afterPin)
        XCTAssertTrue(model.visibleTabs.filter(\.pinned).isEmpty)
        XCTAssertEqual(model.activeID, selected)
    }
    @MainActor
    func testHelpLayout() async throws {
        let view = NSHostingView(rootView: VStack(spacing:20) {
            MailPasswordHelp(provider:.google)
            MailPasswordHelp(provider:.apple)
        }.padding(20).frame(width: 540, height: 620).background(YOBROTheme.chromeTop).foregroundStyle(ink))
        view.frame = NSRect(x:0,y:0,width:540,height:620)
        let window = NSWindow(contentRect:view.frame,styleMask:[.titled],backing:.buffered,defer:false)
        window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds:200_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in:view.bounds)); view.cacheDisplay(in:view.bounds,to:bitmap)
        try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:URL(fileURLWithPath:"/tmp/YOBRO-mail-password-help.png"))
    }
}
