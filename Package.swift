// swift-tools-version: 5.9
import PackageDescription
import Foundation

// The tests stay on the development Mac and are not in the public repository.
let testsFolder = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("tests/Swift").path
let tests: [Target] = FileManager.default.fileExists(atPath: testsFolder)
    ? [.testTarget(name: "YOBROTests", dependencies: ["YOBRO"], path: "tests/Swift")]
    : []

let package = Package(
    name: "YOBRO",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "YOBRO", targets: ["YOBRO"]), .executable(name: "yobro-mcp", targets: ["YOBROMCP"])],
    dependencies: [.package(url: "https://github.com/sparkle-project/Sparkle", from: "2.7.0")],
    targets: [
        .executableTarget(name: "YOBRO", dependencies: [.product(name: "Sparkle", package: "Sparkle")], resources: [.process("Resources")]),
        .executableTarget(name: "YOBROMCP", resources: [.process("Resources")])
    ] + tests
)
