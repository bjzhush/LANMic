import AppKit
import SwiftUI

/// Menu-bar version of netmic-sender: same capture and protocol (SenderCore),
/// with the device, always-on mode and launch at login chosen in the menu.
@main
struct NetMicSenderApp: App {
    @StateObject private var controller = SenderController()

    var body: some Scene {
        MenuBarExtra {
            MenuView(controller: controller)
        } label: {
            Image(systemName: controller.capturing ? "mic.fill" : "mic")
        }
        .menuBarExtraStyle(.window)
    }
}

struct MenuView: View {
    @ObservedObject var controller: SenderController

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("NetMic 发送端").font(.headline)

            Text(controller.statusText)
                .foregroundStyle(controller.error == nil ? Color.secondary : Color.red)
                .fixedSize(horizontal: false, vertical: true)
            if controller.needsMicrophoneAccess {
                Button("打开“隐私与安全性 → 麦克风”设置") { controller.openMicrophoneSettings() }
            }

            LevelMeter(db: controller.levelDb, active: controller.capturing)

            Picker("麦克风", selection: Binding(get: { controller.selectedUID ?? "" },
                                               set: { controller.select(uid: $0) })) {
                if controller.selectedUID == nil { Text("请选择").tag("") }
                ForEach(controller.devices, id: \.uid) { d in
                    Text(d.name).tag(d.uid)
                }
                if let uid = controller.selectedUID, !controller.devices.contains(where: { $0.uid == uid }) {
                    Text("（未连接）\(controller.selectedName ?? uid)").tag(uid)
                }
            }

            Toggle(isOn: Binding(get: { controller.alwaysOn }, set: { controller.setAlwaysOn($0) })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("常开采集")
                    Text("听写不会吞掉第一个字，但麦克风指示灯一直亮").font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle("登录时启动", isOn: Binding(get: { controller.launchAtLogin },
                                           set: { controller.setLaunchAtLogin($0) }))

            Divider()
            HStack {
                Text("名称 “\(controller.serviceName)” · UDP \(String(controller.port))")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("退出") { controller.quit() }
            }
        }
        .padding(14)
        .frame(width: 320)
        .onAppear {
            controller.refreshDevices()
            controller.setMenuVisible(true)
        }
        .onDisappear { controller.setMenuVisible(false) }
    }
}

/// Peak-ish level bar, -60…0 dBFS.
struct LevelMeter: View {
    let db: Float
    let active: Bool

    var body: some View {
        HStack(spacing: 8) {
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3).fill(Color.secondary.opacity(0.2))
                    RoundedRectangle(cornerRadius: 3)
                        .fill(db > -6 ? Color.orange : Color.green)
                        .frame(width: geo.size.width * CGFloat(fraction))
                }
            }
            .frame(height: 8)
            Text(active ? String(format: "%.0f dB", max(db, -99)) : "—")
                .font(.caption.monospacedDigit())
                .frame(width: 48, alignment: .trailing)
        }
    }

    private var fraction: Float { active ? min(1, max(0, (db + 60) / 60)) : 0 }
}
