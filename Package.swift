// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "LiveWall",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "LiveWall",
            path: "Sources/LiveWall",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "LiveWallTests",
            dependencies: ["LiveWall"],
            path: "Tests/LiveWallTests",
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
