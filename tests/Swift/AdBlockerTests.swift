import XCTest
import WebKit
@testable import YOBRO

@MainActor
final class AdBlockerTests: XCTestCase {
    func testRulesCompileWithWebKit() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let blocker = AdBlocker(home: root)
        let list = try await blocker.compiledRuleList()

        XCTAssertFalse(list.identifier.isEmpty)
        XCTAssertTrue(AdBlocker.ruleJSON.contains("doubleclick"))
        XCTAssertTrue(AdBlocker.ruleJSON.contains("analytics"))
        XCTAssertTrue(AdBlocker.ruleJSON.contains("ytd-ad-slot-renderer"))
        XCTAssertTrue(AdBlocker.ruleJSON.contains("third-party"))
        XCTAssertGreaterThan(AdBlocker.blockedDomains.count, 90)
    }

    func testTrackingParametersAreRemovedWithoutChangingUsefulQueryData() throws {
        let input = try XCTUnwrap(URL(string: "https://shop.example/product?id=42&utm_source=newsletter&fbclid=secret&color=green#details"))
        let cleaned = AdBlocker.removingTrackingParameters(from: input)

        XCTAssertEqual(cleaned.absoluteString, "https://shop.example/product?id=42&color=green#details")
        XCTAssertEqual(AdBlocker.removingTrackingParameters(from: cleaned), cleaned)
    }

    func testAdBlockingIsEnabledByDefaultAndPersists() {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let blocker = AdBlocker(home: root)
        XCTAssertTrue(blocker.enabled)
        XCTAssertTrue(blocker.strictProtection)
        blocker.setEnabled(false, webViews: [])
        blocker.setStrictProtection(false, webViews: [])

        let restored = AdBlocker(home: root)
        XCTAssertFalse(restored.enabled)
        XCTAssertFalse(restored.strictProtection)
    }
}
