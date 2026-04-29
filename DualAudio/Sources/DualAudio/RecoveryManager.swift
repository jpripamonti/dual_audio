import Foundation
import os.log

@MainActor
final class RecoveryManager {
    weak var checkDelegate: AppStateStore?
    private var isRecovering = false
    private let log = OSLog(subsystem: "com.dualaudio.RecoveryManager", category: "recovery")
    
    func recover(from reason: String) {
        guard let store = checkDelegate, let selection = store.selection else { return }
        guard !isRecovering else { return }
        guard store.status == .running || store.status == .degraded else { return }
        
        isRecovering = true
        store.status = .degraded
        
        os_log("Recovery started. Reason: %{public}@", log: self.log, type: .info, reason)
        
        Task {
            do {
                // Short wait to allow enumeration to settle before retrying
                try await Task.sleep(nanoseconds: 500_000_000)
                store.status = .recovering
                
                // 1. Attempt to reopen the missing output only
                do {
                    try store.outputManager.reopenMissingOutput()
                    store.status = .running
                    os_log("Recovery succeeded: reopened missing output.", log: self.log, type: .info)
                } catch {
                    os_log("Reopen failed, falling back to engine restart. Error: %{public}@", log: self.log, type: .error, error.localizedDescription)
                    // 2. If that fails, restart the entire AudioEngine
                    try await store.engine.restart(selection: selection)
                    store.status = .running
                    os_log("Recovery succeeded: engine restarted.", log: self.log, type: .info)
                }
            } catch {
                store.status = .failed
                store.lastError = "Recovery failed: \(error.localizedDescription)"
                os_log("Recovery failed completely. Error: %{public}@", log: self.log, type: .error, error.localizedDescription)
            }
            isRecovering = false
        }
    }
}
