import Foundation
import CoreAudio
import AudioBridgeC

enum AudioEngineError: Error, LocalizedError {
    case bridgeCreateFailed
    case bridgeStartFailed(OSStatus)

    var errorDescription: String? {
        switch self {
        case .bridgeCreateFailed:         return "Failed to create audio bridge"
        case .bridgeStartFailed(let s):   return "Failed to start audio bridge (err \(s))"
        }
    }
}

// AudioEngine wraps the C AudioBridge and owns its lifecycle.
// All methods must be called from the main actor (via AppStateStore).
final class AudioEngine {
    private var bridge: AudioBridgeRef?

    func start(selection: DeviceSelection) async throws {
        precondition(bridge == nil, "AudioEngine already started — call stop() first")
        guard let b = AudioBridgeCreate(selection.primaryOutputID,
                                        selection.secondaryOutputID) else {
            throw AudioEngineError.bridgeCreateFailed
        }
        let status = AudioBridgeStart(b)
        guard status == noErr else {
            AudioBridgeDestroy(b)
            throw AudioEngineError.bridgeStartFailed(status)
        }
        bridge = b
    }

    func stop() async {
        guard let b = bridge else { return }
        AudioBridgeStop(b)
        AudioBridgeDestroy(b)
        bridge = nil
    }

    func restart(selection: DeviceSelection) async throws {
        await stop()
        try await start(selection: selection)
    }

    func setVolumes(primary: Float, secondary: Float) {
        guard let b = bridge else { return }
        AudioBridgeSetVolumes(b, primary, secondary)
    }

    func setDelays(primarySeconds: Float, secondarySeconds: Float) {
        guard let b = bridge else { return }
        let sampleRate: Float = 44100.0
        let pFrames = UInt32(max(0, primarySeconds * sampleRate))
        let sFrames = UInt32(max(0, secondarySeconds * sampleRate))
        AudioBridgeSetDelays(b, pFrames, sFrames)
    }
}
