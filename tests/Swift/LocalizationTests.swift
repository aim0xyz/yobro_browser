import XCTest
@testable import YOBRO

final class LocalizationTests: XCTestCase {
    func testPrimarySystemLanguageDeterminesBrowserLanguage() {
        for languages in [["de"], ["de-DE"], ["de-AT", "en"], ["de_CH"], ["DE-de"]] {
            XCTAssertEqual(YOBROLanguage.identifier(for: languages), "de")
        }
        for languages in [["en"], ["fr-FR", "de-DE"], ["ja-JP"], ["es"], []] {
            XCTAssertEqual(YOBROLanguage.identifier(for: languages), "en")
        }
    }

    func testEnglishResourceIsBundledAndCoreStringsAreTranslated() {
        XCTAssertEqual(YOBROLanguage.english["Neuer Tab"], "New tab")
        XCTAssertEqual(YOBROLanguage.english["Einstellungen"], "Settings")
        XCTAssertEqual(YOBROLanguage.english["Weniger Suchen.\nMehr Entdecken."], "Less searching.\nMore discovering.")
        XCTAssertGreaterThan(YOBROLanguage.english.count, 300)
    }
}
