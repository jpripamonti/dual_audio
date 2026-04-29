import CoreAudio
import Foundation

struct AudioOutputDevice: Identifiable, Hashable {
    let id: AudioDeviceID
    let name: String
    let uid: String
}

struct DeviceSelection: Equatable {
    let primaryOutputID: AudioDeviceID
    let secondaryOutputID: AudioDeviceID   // kAudioObjectUnknown (0) = none

    var hasSecondary: Bool { secondaryOutputID != kAudioObjectUnknown }
}

struct OutputManager {
    func availableOutputs() -> [AudioOutputDevice] {
        let deviceList: [AudioDeviceID] = allDeviceIDs()
        return deviceList.compactMap { makeDevice($0) }
    }

    // MARK: - Phase 5 stubs

    func open(selection: DeviceSelection) throws {
        let availableIDs = Set(availableOutputs().map(\.id))
        guard availableIDs.contains(selection.primaryOutputID) else {
            throw NSError(domain: "OutputManager", code: 1, userInfo: [NSLocalizedDescriptionKey: "Primary device not available"])
        }
        if selection.hasSecondary {
            guard availableIDs.contains(selection.secondaryOutputID) else {
                throw NSError(domain: "OutputManager", code: 1, userInfo: [NSLocalizedDescriptionKey: "Secondary device not available"])
            }
        }
    }

    func closeAll() {
        // Nothing for now, AudioEngine handles bridge teardown
    }

    // MARK: - System default output

    func findDevice(named name: String) -> AudioDeviceID? {
        availableOutputs().first(where: { $0.name == name })?.id
    }

    func currentDefaultOutput() -> AudioDeviceID? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope:    kAudioObjectPropertyScopeGlobal,
            mElement:  kAudioObjectPropertyElementMain)
        var id: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &addr, 0, nil, &size, &id)
        return status == noErr ? id : nil
    }

    @discardableResult
    func setDefaultOutput(_ id: AudioDeviceID) -> Bool {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope:    kAudioObjectPropertyScopeGlobal,
            mElement:  kAudioObjectPropertyElementMain)
        var deviceID = id
        let size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &addr, 0, nil, size, &deviceID)
        return status == noErr
    }

    func reopenMissingOutput() throws {
        // The C bridge doesn't support dynamically adding/removing units yet.
        // We throw an error to trigger a full engine restart in RecoveryManager.
        throw NSError(domain: "OutputManager", code: 2, userInfo: [NSLocalizedDescriptionKey: "reopenMissingOutput unsupported, triggering restart"])
    }

    // MARK: - Private

    private func allDeviceIDs() -> [AudioDeviceID] {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope:    kAudioObjectPropertyScopeGlobal,
            mElement:  kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                             &addr, 0, nil, &size) == noErr,
              size > 0 else { return [] }

        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var ids = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                         &addr, 0, nil, &size, &ids) == noErr
        else { return [] }
        return ids
    }

    private func makeDevice(_ id: AudioDeviceID) -> AudioOutputDevice? {
        // Only include devices that have output channels
        guard outputChannelCount(id) > 0 else { return nil }
        let name = stringProperty(id, kAudioObjectPropertyName) ?? "Unknown"
        let uid  = stringProperty(id, kAudioDevicePropertyDeviceUID) ?? "\(id)"
        return AudioOutputDevice(id: id, name: name, uid: uid)
    }

    private func outputChannelCount(_ id: AudioDeviceID) -> Int {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope:    kAudioObjectPropertyScopeOutput,
            mElement:  kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr,
              size > 0 else { return 0 }

        let buf = UnsafeMutableRawPointer.allocate(byteCount: Int(size),
                                                    alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { buf.deallocate() }
        let abl = buf.bindMemory(to: AudioBufferList.self, capacity: 1)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, abl) == noErr
        else { return 0 }

        let ptr = UnsafeMutableAudioBufferListPointer(abl)
        return ptr.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    private func stringProperty(_ id: AudioDeviceID, _ selector: AudioObjectPropertySelector) -> String? {
        var addr = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope:    kAudioObjectPropertyScopeGlobal,
            mElement:  kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<CFTypeRef>.size)
        var cfRef: Unmanaged<CFString>? = nil
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &cfRef) == noErr,
              let ref = cfRef
        else { return nil }
        return ref.takeRetainedValue() as String
    }
}
