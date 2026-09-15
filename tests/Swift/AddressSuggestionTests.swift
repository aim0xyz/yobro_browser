import XCTest
@testable import YOBRO

final class AddressSuggestionTests: XCTestCase {
    @MainActor
    func testSensitiveSignInHistoryIsReducedAndMerged() throws {
        let entries = [
            HistoryEntry(title: "YouTube", url: "https://accounts.google.com/v3/signin/challenge/pwd?TL=secret&continue=https://youtube.com", visits: 2),
            HistoryEntry(title: "Google", url: "https://accounts.google.com/v3/signin/challenge/selection?token=secret", visits: 3)
        ]
        let sanitized = BrowserModel.sanitizedHistory(entries)
        XCTAssertEqual(sanitized.count, 1)
        XCTAssertEqual(sanitized[0].url, "https://accounts.google.com/")
        XCTAssertEqual(sanitized[0].visits, 5)
        XCTAssertFalse(sanitized[0].url.contains("secret"))
    }

    @MainActor
    func testAddressSuggestionsPreferFrequentHomepagePrefixMatch() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let browser = BrowserModel(root: home)
        browser.history = [
            HistoryEntry(title: "A video", url: "https://www.youtube.com/watch?v=123", visits: 2),
            HistoryEntry(title: "YouTube", url: "https://www.youtube.com/", visits: 10),
            HistoryEntry(title: "Yesterday", url: "https://example.com/yesterday", visits: 20)
        ]

        let suggestions = browser.addressSuggestions("y")

        XCTAssertEqual(suggestions.first?.url, "https://www.youtube.com/")
        XCTAssertFalse(suggestions.contains { $0.url.contains("accounts.google.com") })
    }

    @MainActor
    func testAddressSuggestionsBalanceMatchQualityAndHostFrequency() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let browser = BrowserModel(root: home)
        browser.history = [
            HistoryEntry(title: "Example archive", url: "https://rare.test/example", visits: 1),
            HistoryEntry(title: "Example", url: "https://example.com/", visits: 12),
            HistoryEntry(title: "Example docs", url: "https://example.com/docs", visits: 8)
        ]

        let suggestions = browser.addressSuggestions("exa")

        XCTAssertEqual(suggestions.first?.url, "https://example.com/")
    }
}
