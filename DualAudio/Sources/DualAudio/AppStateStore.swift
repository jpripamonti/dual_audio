import SwiftUI
import CoreAudio
import AppKit

enum RuntimeStatus: String {
    case idle       = "Idle"
    case starting   = "Starting…"
    case running    = "Running"
    case degraded   = "Degraded"
    case recovering = "Reconnecting…"
    case failed     = "Failed"

    var isActive: Bool { self == .running || self == .degraded || self == .recovering }
    var color: Color {
        switch self {
        case .idle:       return .secondary
        case .starting:   return .yellow
        case .running:    return .green
        case .degraded:   return .orange
        case .recovering: return .yellow
        case .failed:     return .red
        }
    }
}

@MainActor
final class AppStateStore: ObservableObject, DeviceMonitorDelegate {
    @Published var status: RuntimeStatus = .idle
    @Published var selection: DeviceSelection?
    @Published var availableDevices: [AudioOutputDevice] = []
    @Published var lastError: String?

    @Published var primaryVolume: Float = 1.0 { didSet { updateEngineVolumes() } }
    @Published var secondaryVolume: Float = 1.0 { didSet { updateEngineVolumes() } }
    @Published var syncOffset: Float = 0.0 { didSet { updateEngineDelays() } }

    let engine        = AudioEngine()
    let outputManager = OutputManager()
    
    private let deviceMonitor = DeviceMonitor()
    private let recoveryManager = RecoveryManager()

    private var savedDefaultOutputID: AudioDeviceID?

    init() {
        deviceMonitor.delegate = self
        recoveryManager.checkDelegate = self
        refreshDevices()

        NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.restoreSystemOutput()
            }
        }
    }

    func refreshDevices() {
        availableDevices = outputManager.availableOutputs()
    }

    func startEngine() async {
        guard let sel = selection else { return }
        status    = .starting
        lastError = nil
        do {
            try await engine.start(selection: sel)
            switchSystemOutputToDualAudio()
            status = .running
            updateEngineVolumes()
            updateEngineDelays()
        } catch {
            status    = .failed
            lastError = error.localizedDescription
        }
    }

    func stopEngine() {
        restoreSystemOutput()
        Task { await engine.stop() }
        status = .idle
    }

    private func switchSystemOutputToDualAudio() {
        guard let dualID = outputManager.findDevice(named: "Dual Audio") else { return }
        let current = outputManager.currentDefaultOutput()
        if current != dualID && savedDefaultOutputID == nil {
            savedDefaultOutputID = current
        }
        outputManager.setDefaultOutput(dualID)
    }

    private func restoreSystemOutput() {
        guard let saved = savedDefaultOutputID else { return }
        outputManager.setDefaultOutput(saved)
        savedDefaultOutputID = nil
    }

    func reconnect() async {
        await engine.stop()
        status = .idle
        await startEngine()
    }
    
    private func updateEngineVolumes() {
        if status.isActive {
            engine.setVolumes(primary: primaryVolume, secondary: secondaryVolume)
        }
    }

    private func updateEngineDelays() {
        if status.isActive {
            let pDelay = syncOffset < 0 ? abs(syncOffset) : 0
            let sDelay = syncOffset > 0 ? syncOffset : 0
            engine.setDelays(primarySeconds: pDelay, secondarySeconds: sDelay)
        }
    }

    // MARK: - DeviceMonitorDelegate
    
    nonisolated func deviceListDidChange() {
        Task { @MainActor in
            self.refreshDevices()
            self.checkForMissingDevices()
        }
    }
    
    nonisolated func systemDidWake() {
        Task { @MainActor in
            self.recoveryManager.recover(from: "System Wake")
        }
    }
    
    private func checkForMissingDevices() {
        guard let sel = selection, status.isActive else { return }
        let ids = Set(availableDevices.map(\.id))
        
        let primaryMissing = !ids.contains(sel.primaryOutputID)
        let secondaryMissing = sel.hasSecondary && !ids.contains(sel.secondaryOutputID)
        
        if primaryMissing || secondaryMissing {
            recoveryManager.recover(from: "Device Disconnected")
        }
    }
}
