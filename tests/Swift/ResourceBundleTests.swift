import XCTest
@testable import YOBRO

final class ResourceBundleTests: XCTestCase {
    func testResourcesResolveWithoutGeneratedAccessor() {
        XCTAssertNotNil(YOBROResources.bundle.url(forResource: "English", withExtension: "json"))
        XCTAssertNotNil(YOBROResources.bundle.url(forResource: "AgentBridge", withExtension: "js"))
    }

    /// `Bundle.module` traps when SwiftPM's build directory is gone, which
    /// crashed released builds at launch. All lookups go through YOBROResources.
    func testAppSourcesNeverUseGeneratedBundleAccessor() throws {
        let sources = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Sources/YOBRO")
        let files = try FileManager.default.contentsOfDirectory(at: sources, includingPropertiesForKeys: nil)
            .filter { $0.pathExtension == "swift" }
        XCTAssertFalse(files.isEmpty)
        for file in files {
            let code = try String(contentsOf: file, encoding: .utf8)
                .components(separatedBy: .newlines)
                .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
            XCTAssertFalse(code.contains { $0.contains("Bundle.module") }, file.lastPathComponent)
        }
    }
}
