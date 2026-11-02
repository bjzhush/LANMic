// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "NetMic",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "netmic-sender", targets: ["netmic-sender"]),
        .executable(name: "netmic-probe", targets: ["netmic-probe"]),
        .executable(name: "NetMicSender", targets: ["NetMicSenderApp"]),
    ],
    targets: [
        .target(name: "NetMicProtocol"),
        // UDP socket, receive statistics: shared by sender and probe.
        .target(name: "NetMicKit", dependencies: ["NetMicProtocol"]),
        // Real-time-safe C helpers for the capture callback.
        .target(name: "CNetMicSupport"),
        // Capture + UDP server: shared by the command-line sender and the menu-bar app.
        .target(name: "SenderCore", dependencies: ["NetMicProtocol", "NetMicKit", "CNetMicSupport"]),
        .executableTarget(name: "netmic-sender", dependencies: ["SenderCore", "NetMicProtocol"]),
        // Menu-bar app (scripts/build-app.sh wraps it into NetMicSender.app).
        .executableTarget(name: "NetMicSenderApp", dependencies: ["SenderCore", "NetMicProtocol", "NetMicKit"]),
        .executableTarget(name: "netmic-probe", dependencies: ["NetMicProtocol", "NetMicKit"]),
        .testTarget(name: "NetMicProtocolTests", dependencies: ["NetMicProtocol"]),
        .testTarget(name: "SenderCoreTests", dependencies: ["SenderCore", "NetMicKit", "NetMicProtocol"]),
        .testTarget(name: "NetMicKitTests", dependencies: ["NetMicKit", "NetMicProtocol", "CNetMicSupport"]),
    ],
    swiftLanguageModes: [.v5]
)
