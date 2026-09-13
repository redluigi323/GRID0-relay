// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Grid0Relay",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "Grid0Relay", targets: ["Grid0Relay"])
    ],
    targets: [
        .executableTarget(
            name: "Grid0Relay",
            path: "Sources/Grid0Relay"
        )
    ],
    swiftLanguageModes: [.v5]
)
