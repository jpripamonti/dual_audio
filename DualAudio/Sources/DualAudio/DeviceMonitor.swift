import Foundation
import CoreAudio
import AppKit

protocol DeviceMonitorDelegate: AnyObject {
    func deviceListDidChange()
    func systemDidWake()
}

final class DeviceMonitor {
    weak var delegate: DeviceMonitorDelegate?
    
    init() {
        startHardwareListener()
        startWakeListener()
    }
    
    deinit {
        stopHardwareListener()
        stopWakeListener()
    }
    
    private func startHardwareListener() {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        AudioObjectAddPropertyListener(AudioObjectID(kAudioObjectSystemObject), &addr, hardwareListenerCallback, selfPtr)
    }
    
    private func stopHardwareListener() {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        AudioObjectRemovePropertyListener(AudioObjectID(kAudioObjectSystemObject), &addr, hardwareListenerCallback, selfPtr)
    }
    
    private func startWakeListener() {
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.delegate?.systemDidWake()
        }
    }
    
    private func stopWakeListener() {
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }
}

private func hardwareListenerCallback(
    _ inObjectID: AudioObjectID,
    _ inNumberAddresses: UInt32,
    _ inAddresses: UnsafePointer<AudioObjectPropertyAddress>,
    _ inClientData: UnsafeMutableRawPointer?
) -> OSStatus {
    guard let clientData = inClientData else { return noErr }
    let monitor = Unmanaged<DeviceMonitor>.fromOpaque(clientData).takeUnretainedValue()
    
    DispatchQueue.main.async {
        monitor.delegate?.deviceListDidChange()
    }
    
    return noErr
}
