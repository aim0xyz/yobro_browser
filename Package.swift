// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "YOBRO",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "YOBRO", targets: ["YOBRO"])],
    targets: [
        .executableTarget(name: "YOBRO", resources: [.process("Resources")]),
        .testTarget(name: "YOBROTests", dependencies: ["YOBRO"], path: "tests/Swift")
    ]
)
