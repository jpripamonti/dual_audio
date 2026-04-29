import SwiftUI
import CoreAudio

struct WaveformBarsIcon: View {
    var isActive: Bool
    var isPulsing: Bool
    var size: CGFloat = 36

    private let barCount = 9
    private let envelope: [CGFloat] = (0..<9).map { i in
        sin(CGFloat(i) / 8 * .pi)
    }

    var body: some View {
        let barW = size * 0.09
        let gap  = size * 0.035
        let totalW = CGFloat(barCount) * barW + CGFloat(barCount - 1) * gap

        Canvas { ctx, canvasSize in
            let cx = canvasSize.width / 2
            let cy = canvasSize.height / 2
            let startX = cx - totalW / 2
            let maxH = canvasSize.height * 0.88
            let minH = canvasSize.height * 0.18

            var path = Path()
            for i in 0..<barCount {
                let h = minH + (maxH - minH) * envelope[i]
                let x = startX + CGFloat(i) * (barW + gap)
                let y = cy - h / 2
                let r = barW / 2
                path.addRoundedRect(in: CGRect(x: x, y: y, width: barW, height: h),
                                    cornerSize: CGSize(width: r, height: r))
            }

            let gradient = GraphicsContext.Shading.linearGradient(
                Gradient(colors: isActive ? [.cyan, .indigo] : [.blue, .purple]),
                startPoint: CGPoint(x: 0, y: 0),
                endPoint: CGPoint(x: canvasSize.width, y: 0)
            )
            ctx.fill(path, with: gradient)
        }
        .frame(width: size, height: size)
        .shadow(color: (isActive ? Color.cyan : Color.blue).opacity(0.35), radius: 8, x: 0, y: 4)
        .scaleEffect(isActive && isPulsing ? 1.05 : 1.0)
        .animation(.easeInOut(duration: 2.0).repeatForever(autoreverses: true), value: isPulsing)
    }
}

struct ContentView: View {
    @StateObject private var store = AppStateStore()

    @State private var primaryID: AudioDeviceID?   = nil
    @State private var secondaryID: AudioDeviceID? = nil

    private var selectionIsValid: Bool {
        guard let p = primaryID else { return false }
        if let s = secondaryID { return s != p }
        return true
    }
    
    @State private var isPulsing = false

    var body: some View {
        VStack(spacing: 0) {
            header
            
            ScrollView {
                VStack(spacing: 20) {
                    routingSection
                    
                    if primaryID != nil {
                        controlsSection
                    }
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
            }
            
            bottomBar
        }
        .frame(width: 480, height: 580)
        .background(.quaternary.opacity(0.5))
        .background(.ultraThinMaterial)
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: primaryID)
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: secondaryID)
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: store.status)
        .onChange(of: primaryID)   { syncSelection() }
        .onChange(of: secondaryID) { syncSelection() }
        .onAppear { isPulsing = true }
    }

    // MARK: – Subviews

    private var header: some View {
        HStack {
            WaveformBarsIcon(isActive: store.status.isActive, isPulsing: isPulsing)
            
            VStack(alignment: .leading, spacing: 2) {
                Text("Dual Audio")
                    .font(.title2)
                    .fontWeight(.bold)
                Text("Route audio to multiple devices")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button(action: sendFeedback) {
                Image(systemName: "envelope")
            }
            .buttonStyle(.borderless)
            .help("Send feedback")

            Button(action: { store.refreshDevices() }) {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Refresh device list")
        }
        .padding(24)
    }

    private func sendFeedback() {
        let appVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let build      = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
        let os         = ProcessInfo.processInfo.operatingSystemVersionString
        let body = """


        ---
        App: Dual Audio \(appVersion) (\(build))
        macOS: \(os)
        """
        var comps = URLComponents()
        comps.scheme = "mailto"
        comps.path   = "jpripamonti.it@gmail.com"
        comps.queryItems = [
            URLQueryItem(name: "subject", value: "Dual Audio feedback"),
            URLQueryItem(name: "body",    value: body),
        ]
        if let url = comps.url {
            NSWorkspace.shared.open(url)
        }
    }

    private var routingSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Play on")
                .font(.headline)
                .foregroundStyle(.secondary)

            devicePicker(selection: $primaryID, placeholder: "Choose a device…")
            devicePicker(selection: $secondaryID, placeholder: "Choose another (optional)")

            if let p = primaryID, let s = secondaryID, p == s {
                Text("Pick two different devices.")
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    private func devicePicker(selection: Binding<AudioDeviceID?>,
                              placeholder: String) -> some View {
        Picker("", selection: selection) {
            Text(placeholder).tag(Optional<AudioDeviceID>(nil))
            ForEach(store.availableDevices) { device in
                Text(device.name).tag(Optional(device.id))
            }
        }
        .labelsHidden()
        .pickerStyle(.menu)
        .controlSize(.large)
        .disabled(store.status.isActive)
    }

    private var controlsSection: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Volume")
                .font(.headline)
                .foregroundStyle(.secondary)

            volumeSlider(for: primaryID, value: $store.primaryVolume)

            if secondaryID != nil {
                volumeSlider(for: secondaryID, value: $store.secondaryVolume)
            }

            if secondaryID != nil {
                DisclosureGroup("Advanced") {
                    syncSlider.padding(.top, 8)
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .padding(.top, 4)
            }
        }
    }

    private func volumeSlider(for deviceID: AudioDeviceID?, value: Binding<Float>) -> some View {
        let name = store.availableDevices.first(where: { $0.id == deviceID })?.name ?? "Device"
        return VStack(alignment: .leading, spacing: 6) {
            Text(name)
                .font(.subheadline)
                .fontWeight(.medium)
                .lineLimit(1)
            HStack(spacing: 12) {
                Image(systemName: "speaker.fill")
                    .foregroundStyle(.tertiary)
                Slider(value: value, in: 0...2.0)
                Image(systemName: "speaker.wave.3.fill")
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private var syncSlider: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("If audio is out of sync between devices, adjust here.")
                .font(.caption)
                .foregroundStyle(.secondary)

            HStack {
                Text(String(format: "%.0f ms", store.syncOffset * 1000))
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                Spacer()
            }

            Slider(value: $store.syncOffset, in: -1.0...1.0)

            HStack {
                Text("Delay first")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("Delay second")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var bottomBar: some View {
        HStack {
            statusBadge
            
            Spacer()
            
            if let err = store.lastError {
                Text(err)
                    .foregroundStyle(.red)
                    .font(.caption)
                    .lineLimit(1)
            }
            
            actionButtons
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 16)
        .background(.ultraThinMaterial)
        .overlay(Divider(), alignment: .top)
    }

    private var statusBadge: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(store.status.color)
                .frame(width: 8, height: 8)
                .shadow(color: store.status.color.opacity(0.5), radius: isPulsing && store.status.isActive ? 4 : 0)
                .scaleEffect(isPulsing && store.status.isActive ? 1.2 : 1.0)
                .opacity(isPulsing && store.status.isActive ? 0.7 : 1.0)
                .animation(store.status.isActive ? .easeInOut(duration: 1.0).repeatForever(autoreverses: true) : .default, value: isPulsing)
                
            Text(store.status.rawValue)
                .font(.subheadline)
                .fontWeight(.medium)
                .foregroundStyle(store.status == .failed ? .red : .primary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(8)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color(NSColor.separatorColor), lineWidth: 1)
        )
    }

    private var actionButtons: some View {
        HStack(spacing: 12) {
            if store.status.isActive {
                Button(action: { store.stopEngine() }) {
                    Label("Stop", systemImage: "stop.fill")
                }
                .buttonStyle(.bordered)
                .controlSize(.large)

                if store.status == .failed || store.status == .degraded {
                    Button(action: { Task { await store.reconnect() } }) {
                        Label("Reconnect", systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                }
            } else {
                Button(action: { Task { await store.startEngine() } }) {
                    Label("Start", systemImage: "play.fill")
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(!selectionIsValid)
            }
        }
    }

    // MARK: – Helpers

    private func syncSelection() {
        guard let p = primaryID else {
            store.selection = nil
            return
        }
        let s = secondaryID ?? AudioDeviceID(kAudioObjectUnknown)
        store.selection = DeviceSelection(primaryOutputID: p, secondaryOutputID: s)
    }
}
