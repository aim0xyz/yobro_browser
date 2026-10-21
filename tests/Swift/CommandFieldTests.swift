import XCTest
@testable import YOBRO

final class CommandFieldTests: XCTestCase {
    @MainActor
    func testVisitedHostIsOfferedAsTopHitAboveTheSearch() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.history = [HistoryEntry(title: "GitHub", url: "https://github.com/", visits: 3)]

        let suggestions = model.commandSuggestions("git")
        XCTAssertEqual(suggestions.first?.action, .go("https://github.com/"))
        XCTAssertEqual(suggestions.dropFirst().first?.action, .go("git"))
        XCTAssertEqual(suggestions.dropFirst().first?.detail, L("Mit \(SearchEngine.current.name) suchen", "Search with \(SearchEngine.current.name)"))
        XCTAssertEqual(suggestions.filter { $0.action == .go("https://github.com/") }.count, 1, "The top hit is not repeated under History")

        XCTAssertEqual(model.commandSuggestions("g").first?.action, .go("g"), "One letter is too little for a top hit")
        XCTAssertEqual(model.commandSuggestions("github.com").first?.action, .go("github.com"), "A complete host is opened as typed")

        let open = BrowserTab(saved: StoredTab(id: UUID(), title: "GitHub", url: "https://github.com/", space: model.space, pinned: false), owner: model, deferLoading: true)
        model.tabs.append(open)
        let switching = model.commandSuggestions("git")
        XCTAssertEqual(switching.first?.action, .tab(open.id), "An open page is switched to, not opened twice")
        XCTAssertEqual(switching.filter { $0.tabID == open.id }.count, 1)
    }

    @MainActor
    func testShortcutsOnABlankTabFocusItsFieldInsteadOfOpeningAnother() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.newTab()
        let focused = expectation(forNotification: Notification.Name("YOBRO.focusInlineCommandField"), object: model)
        focused.expectedFulfillmentCount = 2

        model.openCommandBar(.newTab)
        model.openCommandBar(.current)

        XCTAssertNil(model.commandBar)
        wait(for: [focused], timeout: 1)
    }

    @MainActor
    func testAddressYoBroCannotOpenLeavesNoTabBehind() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let page = model.newTab(url: "https://example.com")

        model.perform(CommandSuggestion(id: "go", section: "", title: "file:///a.pdf", detail: "", symbol: "globe", action: .go("file:///a.pdf")), mode: .newTab)

        XCTAssertEqual(model.tabs.count, 1)
        XCTAssertEqual(model.activeID, page.id)
        XCTAssertNotNil(model.notice)
    }

    @MainActor
    func testOpeningTheAddressFieldClosesThePeek() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        model.newTab(url: "https://example.com")
        model.peek(try XCTUnwrap(URL(string: "https://example.org")))
        XCTAssertNotNil(model.peekTab)

        model.openCommandBar(.current)

        XCTAssertNil(model.peekTab)
        XCTAssertEqual(model.commandBar, .current)
    }
}
