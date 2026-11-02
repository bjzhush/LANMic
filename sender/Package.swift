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
        .executableTarget(name: "netmic-sender", dependencies: ["NetMicProtocol"]),
        .executableTarget(name: "netmic-probe", dependencies: ["NetMicProtocol"]),
        .testTarget(name: "NetMicProtocolTests", dependencies: ["NetMicProtocol"]),
    ],
    swiftLanguageModes: [.v5]
)
