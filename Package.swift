// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "DualAudio",
    platforms: [.macOS(.v14)],
    targets: [
        // C layer: AudioOutputUnit + shared memory bridge
        .target(
            name: "AudioBridgeC",
            path: "AudioBridge",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioUnit"),
                .linkedFramework("AudioToolbox"),
            ]
        ),

        // SwiftUI app
        .executableTarget(
            name: "DualAudio",
            dependencies: ["AudioBridgeC"],
            path: "DualAudio/Sources/DualAudio",
            exclude: ["Info.plist"],
            swiftSettings: [
                .unsafeFlags(["-parse-as-library"])
            ]
        ),
    ]
)
