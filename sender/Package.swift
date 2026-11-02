// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "NetMic",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "netmic-sender", targets: ["netmic-sender"]),
        .executable(name: "netmic-probe", targets: ["netmic-probe"]),
    ],
    targets: [
        .target(name: "NetMicProtocol"),
        // UDP socket, receive statistics: shared by sender and probe.
        .target(name: "NetMicKit", dependencies: ["NetMicProtocol"]),
        // Real-time-safe C helpers for the capture callback.
        .target(name: "CNetMicSupport"),
        .executableTarget(name: "netmic-sender", dependencies: ["NetMicProtocol", "NetMicKit", "CNetMicSupport"]),
        .executableTarget(name: "netmic-probe", dependencies: ["NetMicProtocol", "NetMicKit"]),
        .testTarget(name: "NetMicProtocolTests", dependencies: ["NetMicProtocol"]),
        .testTarget(name: "NetMicKitTests", dependencies: ["NetMicKit", "NetMicProtocol", "CNetMicSupport"]),
    ],
    swiftLanguageModes: [.v5]
)
