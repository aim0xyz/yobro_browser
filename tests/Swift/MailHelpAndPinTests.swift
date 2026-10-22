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
}
