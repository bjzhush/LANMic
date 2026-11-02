import AVFoundation
import AppKit
import Foundation
import NetMicProtocol
import ServiceManagement
import SenderCore
import os

private let logger = Logger(subsystem: "com.netmic.sender", category: "sender")

/// Owns the running Capture + Server and publishes their state to the menu.
///
/// Settings live in UserDefaults (domain com.netmic.sender). The menu sets
/// DeviceUID and AlwaysOn; the rest are for `defaults write`:
///   Name (String, "M4 Mic"), Port (Int, 47810), Token (32 hex digits),
///   GainDb (Float, 0), Channel (Int, 0; -1 = mix), IOFrames (Int, 240), StopGrace (Double, 5).
@MainActor
final class SenderController: ObservableObject {
    @Published private(set) var devices: [InputDevice] = []
    @Published private(set) var selectedUID: String?
    @Published private(set) var selectedName: String?
    @Published private(set) var alwaysOn: Bool
    @Published private(set) var launchAtLogin = false
    @Published private(set) var capturing = false
    @Published private(set) var subscribers: [String] = []
    @Published private(set) var levelDb: Float = -120
    @Published private(set) var error: String?
    @Published private(set) var needsMicrophoneAccess = false

    let serviceName: String
    let port: UInt16

    private let defaults = UserDefaults.standard
    private var server: Server?
    private var capture: Capture?
    private var serverDone: DispatchSemaphore?
    private var pollTimer: Timer?
    private let pollQueue = DispatchQueue(label: "netmic.app.poll")

    init() {
        logSink = { logger.log("\($0, privacy: .public)") }
        serviceName = defaults.string(forKey: "Name") ?? "M4 Mic"
        port = UInt16(exactly: defaults.integer(forKey: "Port")).flatMap { $0 > 0 ? $0 : nil } ?? NetMic.defaultPort
        alwaysOn = defaults.bool(forKey: "AlwaysOn")
        selectedUID = defaults.string(forKey: "DeviceUID")
        selectedName = defaults.string(forKey: "DeviceName")
        refreshLaunchAtLogin()
        refreshDevices()
        if selectedUID == nil, let d = defaultInputDevice() { remember(d) }

        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                               queue: nil) { [weak self] _ in
            MainActor.assumeIsolated { self?.shutdown() }
        }
        requestMicrophoneThenStart()
    }

    // MARK: - menu actions

    var statusText: String {
        if let error { return error }
        guard selectedUID != nil else { return "请选择麦克风" }
        if !subscribers.isEmpty {
            return subscribers.count == 1 ? "正在发送给 \(subscribers[0])" : "正在发送给 \(subscribers.count) 个接收端"
        }
        if capturing { return alwaysOn ? "常开采集中，等待接收端" : "接收端已离开，即将停止采集" }
        return "待机：M2 上有应用使用 “\(serviceName)” 时自动开始"
    }

    func refreshDevices() {
        devices = listInputDevices()
        if let uid = selectedUID, let d = devices.first(where: { $0.uid == uid }), d.name != selectedName {
            remember(d)
        }
    }

    func select(uid: String) {
        guard uid != selectedUID, let d = devices.first(where: { $0.uid == uid }) else { return }
        remember(d)
        restart()
    }

    func setAlwaysOn(_ on: Bool) {
        alwaysOn = on
        defaults.set(on, forKey: "AlwaysOn")
        server?.alwaysOn = on
    }

    func setLaunchAtLogin(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            log("login item: \(error)")
        }
        refreshLaunchAtLogin()
        if on && SMAppService.mainApp.status == .requiresApproval { SMAppService.openSystemSettingsLoginItems() }
    }

    func openMicrophoneSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone") {
            NSWorkspace.shared.open(url)
        }
    }

    func quit() {
        shutdown()
        NSApp.terminate(nil)
    }

    // MARK: - running

    private func remember(_ d: InputDevice) {
        selectedUID = d.uid
        selectedName = d.name
        defaults.set(d.uid, forKey: "DeviceUID")
        defaults.set(d.name, forKey: "DeviceName")
    }

    private func refreshLaunchAtLogin() {
        launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    private func requestMicrophoneThenStart() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            restart()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { granted in
                Task { @MainActor in
                    if granted { self.restart() } else { self.microphoneDenied() }
                }
            }
        default:
            microphoneDenied()
        }
    }

    private func microphoneDenied() {
        needsMicrophoneAccess = true
        error = "没有麦克风权限。打开设置允许 NetMic Sender 后重新启动本应用。"
    }

    /// Stops the current server (if any) and starts a new one for the selected device.
    private func restart() {
        stopServer()
        guard !needsMicrophoneAccess, let uid = selectedUID else { return }
        let token = defaults.string(forKey: "Token").flatMap { Token(hex: $0) } ?? .zero
        let channel = defaults.object(forKey: "Channel") as? Int ?? 0
        let ioFrames = defaults.object(forKey: "IOFrames") as? Int ?? 240
        let capture = Capture(options: .init(deviceQuery: uid, channel: Int32(channel),
                                             gainDb: defaults.float(forKey: "GainDb"), ioFrames: UInt32(max(0, ioFrames))))
        capture.log = { log($0) }
        let stopGrace = defaults.object(forKey: "StopGrace") as? Double ?? 5
        do {
            let server = try Server(options: .init(port: port, name: serviceName, token: token, alwaysOn: alwaysOn,
                                                   stopGrace: stopGrace), capture: capture)
            let done = DispatchSemaphore(value: 0)
            let thread = Thread {
                server.run()
                done.signal()
            }
            thread.name = "netmic.server"
            thread.start()
            self.server = server
            self.capture = capture
            serverDone = done
            error = nil
        } catch {
            capture.shutdown()
            self.error = "无法监听 UDP \(port)：\(error)。命令行版 netmic-sender 是否还在运行？"
        }
    }

    private func stopServer() {
        guard let server else { return }
        server.stop()
        _ = serverDone?.wait(timeout: .now() + 2)  // run() returns within ~100 ms and releases the mic
        self.server = nil
        capture = nil
        serverDone = nil
        capturing = false
        subscribers = []
        levelDb = -120
    }

    private func shutdown() {
        pollTimer?.invalidate()
        stopServer()
    }

    private func poll() {
        guard let server, let capture else { return }
        pollQueue.async {
            // takeLevel waits on the capture queue, which can be busy starting the engine.
            let status = server.status()
            let level = capture.takeLevel()
            Task { @MainActor in
                guard self.server === server else { return }
                self.capturing = status.capturing
                self.subscribers = status.subscribers
                self.levelDb = status.capturing ? level.peakDb : -120
            }
        }
    }
}

private func defaultInputDevice() -> InputDevice? {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultInputDevice,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var id = AudioDeviceID(0)
    var size = UInt32(MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id) == noErr
    else { return nil }
    return listInputDevices().first { $0.id == id }
}
