# NetMic：局域网远程麦克风（M4 → M2 虚拟输入设备）开发方案

> 本文档是交给 Claude Code（CLI）执行的开发规格书。请按「阶段」顺序推进，每个阶段完成后停下来，让用户完成该阶段的「人工验收」再进入下一阶段。

---

## 0. 背景与目标

- **M4**（Mac mini M4，用户面前）：连接了外接麦克风。
- **M2**（Mac mini M2，约 10 米外，有线局域网连接）：需要使用 M4 的麦克风。
- **目标**：在 M2 上出现一个名为 **"M4 Mic"** 的系统级音频输入设备。系统听写、QuickTime、Zoom、浏览器等任何应用都能像使用本地硬件麦克风一样选择它；它的声音实时来自 M4 的外接麦克风。

### 非目标（v1 不做）
- 不做加密（仅限家庭局域网使用）。
- 不支持多个发送端同时接入，不做反向（M2 → M4）音频。
- 只支持 48 kHz，不针对 Wi-Fi 做优化（两台机器是有线连接）。
- 不做 App Store 分发，不做公证。

### 关键指标
- 端到端延迟 ≤ 50 ms（目标 20–40 ms）。
- 有线局域网下连续运行 1 小时无可闻爆音或断续。
- 没有任何 M2 上的应用在使用 "M4 Mic" 时，M4 停止采集（M4 菜单栏的麦克风橙色指示灯熄灭）。

---

## 1. 总体架构

```
┌──────────────── M4 ────────────────┐            ┌──────────────────────── M2 ─────────────────────────┐
│                                    │            │                                                     │
│ 外接麦克风                          │            │  Core Audio Driver Service（系统进程，沙盒）          │
│    │                               │            │   └─ NetMic.driver（AudioServerPlugIn）              │
│    ▼                               │   UDP      │        ├─ 网络线程：发现 / 订阅心跳 / 收包            │
│ netmic-sender (Swift)              │ ◄────────► │        ├─ 抖动缓冲 + 时钟漂移补偿（环形缓冲区）        │
│  ├─ AVAudioEngine 采集指定设备       │  47810     │        └─ IO 线程：向客户端提供 Float32 采样          │
│  ├─ 转换为 48k / mono / Int16       │            │                    │                                │
│  ├─ 每 5 ms 打一个 UDP 包            │            │                    ▼                                │
│  └─ 订阅者管理（按需启停采集）        │            │   设备 "M4 Mic" → 听写 / QuickTime / Zoom …          │
└────────────────────────────────────┘            └─────────────────────────────────────────────────────┘
```

设计要点：
1. **网络收包直接在 HAL 插件内完成**。苹果允许 AudioServerPlugIn 在 Info.plist 中声明 `AudioServerPlugIn_Network = true` 来使用网络，开源项目 roc-vad 也是在插件内使用 socket 的。这样 M2 上只需要安装一个 `.driver`，不需要额外的守护进程。
2. **按需采集**：插件在 `StartIO` 时开始向 M4 发送订阅心跳，在 `StopIO` 时停止。M4 收不到心跳后自动停止采集。
3. **发现机制用 UDP 广播，不用 Bonjour**。插件运行在受限沙盒中，广播只依赖纯 socket，最稳妥。同时支持在配置中写死 M4 的 IP 作为兜底。

---

## 2. 仓库结构

```
netmic/
├── README.md                     # 构建、安装、使用说明（最后阶段完善）
├── PROGRESS.md                   # Claude Code 维护：各阶段进度、遇到的问题、决策记录
├── docs/protocol.md              # 线协议说明（从本文第 3 节整理）
├── shared/
│   └── netmic_protocol.h         # C 头文件：包头结构、常量；sender（Swift）和 driver（C++）共用
├── sender/                       # Swift Package（运行在 M4）
│   ├── Package.swift
│   └── Sources/
│       ├── NetMicProtocol/       # 包编解码（Swift 实现，与 shared 头文件保持一致）
│       ├── netmic-sender/        # 发送端 CLI
│       └── netmic-probe/         # 接收端测试 CLI（运行在 M2，阶段 1 用来验证网络）
│   └── Tests/
├── driver/                       # CMake 工程（运行在 M2）
│   ├── CMakeLists.txt
│   ├── third_party/libASPL/      # git submodule
│   ├── src/
│   │   ├── Plugin.cpp            # 入口：CFPlugIn factory → libASPL Driver
│   │   ├── NetMicDevice.{h,cpp}  # 设备/流/IO handler
│   │   ├── NetReceiver.{h,cpp}   # 网络线程：发现、订阅、收包
│   │   ├── JitterBuffer.{h,cpp}  # 无锁 SPSC 环形缓冲 + 漂移补偿重采样
│   │   └── Config.{h,cpp}        # 从 bundle Info.plist 读取配置
│   ├── Info.plist.in
│   └── tests/                    # JitterBuffer 等纯逻辑的单元测试（不依赖 Core Audio）
└── scripts/
    ├── build-all.sh
    ├── install-driver.sh         # 本机安装 + 重启 coreaudiod
    ├── uninstall-driver.sh
    ├── deploy-m2.sh              # 从 M4 构建后 scp 到 M2 并远程安装（ssh -t，需用户输入 sudo 密码）
    └── logs.sh                   # 查看驱动日志
```

**开发机**：在 M4 上运行 Claude Code。两台机器都是 Apple Silicon（arm64），在 M4 上构建出的 driver 可以直接拷到 M2 使用。

**本机闭环调试**：驱动也可以先装在 M4 本机，配置 `SenderAddress = 127.0.0.1`，和本机的 sender 跑通完整链路，确认稳定后再部署到 M2。

### 依赖
- Xcode 或 Command Line Tools（Swift 5.9+、clang、C++17）。
- CMake ≥ 3.20（`brew install cmake`）。
- [libASPL](https://github.com/gavv/libASPL)：用于编写 AudioServerPlugIn 的 C++17 库，roc-vad 就是基于它实现的，可以大幅减少样板代码。以 git submodule 方式引入。
- 参考实现（只阅读，不依赖）：
  - roc-vad（https://github.com/roc-streaming/roc-vad）：基于 libASPL 的网络虚拟声卡，最接近本项目。
  - BlackHole（https://github.com/ExistentialAudio/BlackHole）：纯 C 的 HAL 插件。
  - Apple 示例代码 "Creating an Audio Server Driver Plug-in"（NullAudio）。

---

## 3. 线协议（v1）

- 传输：UDP，IPv4。
- 端口：**发送端监听 47810**（可配置）。接收端使用系统分配的临时端口，发送端把音频回发到订阅包的源地址和端口。
- 字节序：**小端**。所有结构体按 packed 方式定义。

### 3.1 包头（24 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | u32 | magic | `0x43494D4E`（内存中依次为 ASCII "NMIC"） |
| 4 | u8 | version | `1` |
| 5 | u8 | type | 见下表 |
| 6 | u16 | payloadBytes | 包头之后的负载长度 |
| 8 | u32 | seq | 每种类型各自递增 |
| 12 | u32 | streamId | 发送端每次（重新）开始采集时随机生成；接收端发现 streamId 变化时重置缓冲区 |
| 16 | u64 | samplePos | AUDIO 包中为本包第一个采样帧在该 stream 内的序号；其他类型填 0 |

### 3.2 包类型

| type | 名称 | 方向 | 负载 |
|---|---|---|---|
| 1 | DISCOVER | 接收端 → 广播 `255.255.255.255:47810`，或发往配置的单播地址 | `token[16]` |
| 2 | ANNOUNCE | 发送端 → 接收端（单播回复） | `sampleRate u32, channels u8, format u8(1=Int16), framesPerPacket u16, nameLen u8, name[nameLen] UTF-8` |
| 3 | SUBSCRIBE | 接收端 → 发送端 | `token[16]`。IO 运行期间每 **500 ms** 发送一次，同时兼作心跳 |
| 4 | UNSUBSCRIBE | 接收端 → 发送端 | `token[16]` |
| 5 | AUDIO | 发送端 → 订阅者 | Int16 LE PCM，单声道，**240 帧（5 ms @ 48 kHz）= 480 字节** |

- `token`：16 字节预共享密钥（默认全 0 表示不校验）。发送端只接受 token 匹配的 DISCOVER / SUBSCRIBE。另外默认只接受来自私有地址段的请求：10/8、172.16/12、192.168/16、169.254/16、127/8。
- 订阅过期：发送端 **3 秒** 内没有收到某个订阅者的 SUBSCRIBE，就移除该订阅者。
- 码率：48000 × 16 bit = 768 kbps，加上包头约 800 kbps，每秒 200 个包。有线局域网完全可以承受。

---

## 4. 组件规格

### 4.1 netmic-sender（M4，Swift CLI）

```
netmic-sender --list-devices
netmic-sender --device <名称子串或UID> [--name "M4 Mic"] [--port 47810]
              [--channel 0|mix] [--gain-db 0] [--token <32位hex>]
              [--always-on] [--stop-grace 5] [-v]
```

功能要求：
1. **设备枚举**：通过 CoreAudio `kAudioHardwarePropertyDevices` 列出所有有输入流的设备（名称、UID、采样率、声道数）。
2. **采集**：
   - 使用 AVAudioEngine。启动前对 `engine.inputNode.audioUnit` 设置 `kAudioOutputUnitProperty_CurrentDevice`，指定麦克风设备，**不要修改 M4 的系统默认输入设备**。
   - 用 `installTap` 获取数据，再用 `AVAudioConverter` 转为 48 kHz、单声道、Float32。默认取第 0 声道，`--channel mix` 时取各声道平均。之后施加增益，转为带饱和截断的 Int16。
   - 用累加器凑满 240 帧发一个包，`samplePos` 单调递增。
   - 监听 `AVAudioEngineConfigurationChange`（设备拔插、采样率变化），收到后自动重建引擎，并生成新的 `streamId`。
3. **网络**：用 BSD socket（或 Network.framework）在 47810 端口收发。DISCOVER 回复 ANNOUNCE；SUBSCRIBE 加入或刷新订阅者；UNSUBSCRIBE 移除订阅者。
4. **按需启停**：有订阅者时启动采集；最后一个订阅者移除 `--stop-grace` 秒后停止采集。`--always-on` 表示常开采集。M4 启动麦克风有 100–300 ms 的冷启动时间，常开模式可以避免听写吞掉第一个字。这是隐私和响应速度之间的取舍，由用户选择。
5. **日志与统计**：`-v` 时每 5 秒打印一次订阅者数量、发包数和采集电平（dBFS）。
6. **权限**：
   - 阶段 1 从「终端」运行。麦克风权限会归属到终端，首次运行会弹窗请求授权。
   - 从终端运行的命令行工具不受 macOS 15 本地网络隐私限制。
   - 阶段 5 打包成 App 时，需要在 Info.plist 中加入 `NSMicrophoneUsageDescription` 和 `NSLocalNetworkUsageDescription`。

### 4.2 netmic-probe（M2，Swift CLI，仅用于测试）

```
netmic-probe --discover                       # 广播发现，打印找到的发送端
netmic-probe --record 10 --out test.wav [--sender <ip>] [--token ...]
netmic-probe --stats 120 [--sender <ip>]      # 只统计，不落盘
```

统计项：收包数、丢包数（按 seq 计算）、乱序数、到达抖动（RFC 3550 算法）、**时钟漂移 ppm**（用 samplePos 和本机 `mach_absolute_time` 做线性回归）。漂移的实测值用来给第 7 节的参数提供依据。

### 4.3 NetMic.driver（M2，AudioServerPlugIn，C++17 + libASPL）

#### 设备属性
| 属性 | 值 |
|---|---|
| 设备名 | 来自配置 `DeviceName`，默认 "M4 Mic" |
| 制造商 | "NetMic" |
| Device UID | `com.netmic.device.m4mic` |
| Transport type | `kAudioDeviceTransportTypeVirtual` |
| 方向 | 只有输入（1 个 input stream，无 output） |
| 采样率 | 仅 48000 Hz |
| 声道 | 配置 `Channels`，默认 1；设为 2 时把单声道复制到双声道（为兼容个别要求立体声的应用） |
| 格式 | Float32 interleaved（linear PCM） |
| Latency | 报告为当前目标缓冲帧数（便于应用做音画同步） |
| IsAlive | 始终为 true。M4 离线时输出静音，设备不消失，避免听写等设置失效 |

#### 配置（bundle Info.plist 里的 `NetMicConfig` 字典）
插件只能读取自身 bundle 内的文件，所以配置写在 Info.plist 中，修改后需要重新安装。

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `DeviceName` | String | "M4 Mic" | |
| `SenderAddress` | String | "" | 为空时使用广播发现；填写 IP 时直接单播（本机调试填 127.0.0.1） |
| `Port` | Integer | 47810 | |
| `TargetLatencyMs` | Integer | 20 | 抖动缓冲目标深度 |
| `Channels` | Integer | 1 | 1 或 2 |
| `Token` | String | "" | 32 位 hex，为空表示全 0 |

Info.plist 中还必须包含：
```xml
<key>AudioServerPlugIn_Network</key><true/>
```
以及标准 CFPlugIn 键：`CFPlugInFactories`、`CFPlugInTypes`（类型 UUID 为 `443ABAB8-E7B3-491A-B985-BEB9187030DB`，即 kAudioServerPlugInTypeUUID）。具体写法参照 libASPL 示例或 roc-vad。

#### 线程模型（务必遵守）
- **IO 线程（实时线程）**：即 libASPL 的 `IORequestHandler::OnReadClientInput` 或等价回调（以 libASPL 实际 API 为准）。
  - **禁止**：内存分配、加锁（包括 `std::mutex`）、系统调用、日志、Objective-C/Swift 运行时、任何可能阻塞的操作。
  - 只从无锁 SPSC 环形缓冲区读取数据并做重采样。数据不足时输出零。
- **网络线程（普通线程）**：在插件初始化时创建，常驻运行。
  - 负责 `recvfrom`（带超时）、发现、订阅心跳和写入环形缓冲区。
  - 订阅心跳只在 IO 运行期间（`StartIO` 计数 > 0）发送。`StartIO` / `StopIO` 只通过原子变量通知网络线程，不在回调内做网络操作。
- 统计值（丢包、欠载次数、当前缓冲深度）用原子变量记录，由网络线程每 10 秒用 `os_log` 输出一次（subsystem `com.netmic.driver`）。

#### 接收逻辑
1. **发现**：未配置 `SenderAddress` 时，每 1 秒广播一次 DISCOVER（socket 需要设置 `SO_BROADCAST`），收到 ANNOUNCE 后锁定该发送端。发送端离线 5 秒后回到发现状态。
2. **AUDIO 包处理**：
   - 校验 magic、version 和 streamId。streamId 变化时**重置**缓冲区。
   - 根据 `samplePos` 计算写入位置：比读位置还旧的包丢弃；有空洞的地方填零（相当于丢包补静音）；超出环形缓冲区窗口时重置。
   - Int16 → Float32 的转换在网络线程完成。
3. **缓冲状态机**：
   - `Buffering`：开始播放或欠载后，累积到 ≥ 目标深度再进入 Playing，期间 IO 线程输出静音。
   - `Playing`：正常读取。
   - 欠载（数据不够本次 IO 读取）：已有数据照常输出，不足部分补零，然后回到 `Buffering`。
   - 过载（深度 > 目标 + 60 ms）：直接把读指针跳到"写位置 − 目标深度"。
   - 环形缓冲容量：500 ms。

### 4.4 时钟漂移补偿

两台机器的音频时钟不同源，典型偏差在几十 ppm。以 50 ppm 为例，每秒偏差约 2.4 个采样，20 ms 的缓冲大约 7 分钟就会被吃空或撑满，所以必须做补偿。

- 在 IO 线程中，对缓冲深度做指数平滑（时间常数约 1 秒），误差 `e = 平滑深度 − 目标深度`。
- 用 PI 控制器计算重采样比 `ratio = 1 + clamp(Kp·e + Ki·∫e, ±1000 ppm)`，按此比例从环形缓冲区以线性插值方式读取。对语音来说线性插值足够，后续可以换成三次插值。
- Kp、Ki 的取值要保证收敛时间在 10–30 秒、无振荡。用单元测试模拟 ±100 ppm 的漂移来调参。
- 降级方案：如果 PI 控制器实现有困难，可以先用"误差超过 ±2 ms 时插入或丢弃 1 个采样"的简单方案过渡，并在 PROGRESS.md 中记录。

---

## 5. 构建、签名、安装

### 构建
```bash
./scripts/build-all.sh          # swift build -c release（sender/probe） + cmake --build（driver）
```
驱动构建产物为 `driver/build/NetMic.driver`（bundle 结构：`Contents/Info.plist`、`Contents/MacOS/NetMic`）。

### 签名
- 默认使用 ad-hoc 签名：`codesign --force --deep --sign - NetMic.driver`。
- 如果 coreaudiod 拒绝加载（日志中出现签名相关错误），改用用户的 Apple Development 证书（在 `scripts` 里通过环境变量 `CODESIGN_IDENTITY` 指定）。

### 安装（install-driver.sh）
```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/NetMic.driver
sudo cp -R driver/build/NetMic.driver /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/NetMic.driver
sudo killall coreaudiod || sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
```
- **不需要重启电脑**。重启 coreaudiod 会让该机器所有声音中断几秒。
- 安装完成后用 `system_profiler SPAudioDataType` 或「音频 MIDI 设置」确认 "M4 Mic" 出现。

### 部署到 M2（deploy-m2.sh）
- 前提：M2 已开启「系统设置 → 通用 → 共享 → 远程登录」。
- 流程：`scp` 拷贝 driver bundle 和 `install-driver.sh` 到 M2，再执行 `ssh -t user@m2 'bash install-driver.sh'`，由用户输入 sudo 密码。
- `netmic-probe` 也用这个脚本一起拷过去。

### 日志
```bash
log stream --predicate 'subsystem == "com.netmic.driver"' --level debug
```
驱动运行在 `Core-Audio-Driver-Service` 进程中（较新的 macOS 版本）。如果驱动崩溃，可以在「控制台」App 的崩溃报告中找到。

---

## 6. 分阶段计划与验收

每个阶段结束时：更新 PROGRESS.md，做一次 git commit，然后把「人工验收」步骤原样列给用户，等用户反馈后再继续。凡是需要说话、听声音、在 M2 上操作的步骤，都需要用户来做。

### 阶段 0：骨架与纯逻辑
- 建立仓库结构、`shared/netmic_protocol.h`、Swift 的 `NetMicProtocol` 模块。
- 实现 `JitterBuffer`（C++，不依赖 Core Audio）并编写单元测试，覆盖：正常、乱序、丢包补零、欠载、过载、streamId 重置、±100 ppm 漂移收敛。
- 实现 Swift 包编解码并编写单元测试。用相同测试向量验证 C++ 和 Swift 的编解码结果一致。
- **验收**：`swift test` 和 C++ 测试全部通过。

### 阶段 1：网络链路（不碰 coreaudiod）
- 完成 `netmic-sender` 和 `netmic-probe`。
- **人工验收**：
  1. M4 运行 `netmic-sender --list-devices`，找到外接麦克风；再运行 `netmic-sender --device "<名字>" -v`。
  2. 把 probe 拷到 M2，运行 `netmic-probe --discover`，应该能找到 "M4 Mic"。
  3. M2 运行 `netmic-probe --record 10 --out test.wav`，同时在 M4 前说话。回放 test.wav，确认音质正常、没有杂音。
  4. 运行 `netmic-probe --stats 300`，记录丢包率（应约为 0）、抖动和漂移 ppm，写进 PROGRESS.md。
  5. probe 结束 `--stop-grace` 秒后，M4 菜单栏的麦克风指示灯应熄灭。

### 阶段 2：最小虚拟设备（不接网络）
- 驱动先只输出 440 Hz 正弦波（-12 dBFS），验证插件的加载、属性和时间戳都正确。
- 先在 **M4 本机**安装测试，通过后再部署到 M2。
- **人工验收**：
  1. 「系统设置 → 声音 → 输入」中出现 "M4 Mic"。
  2. 用 QuickTime 新建音频录制并选择 "M4 Mic"，录 10 秒，回放应听到稳定的正弦音，没有爆音。
  3. 卸载脚本能干净卸载，重启 coreaudiod 后设备消失。

### 阶段 3：接入网络
- 实现 `NetReceiver` 和 `StartIO` / `StopIO` 联动，接上 JitterBuffer（这一阶段可以先不做漂移补偿）。
- 先在 M4 本机闭环（`SenderAddress = 127.0.0.1`），再部署到 M2（广播发现）。
- **人工验收（在 M2 上）**：
  1. QuickTime 用 "M4 Mic" 录音，在 M4 前说话，回放清晰。
  2. 「系统设置 → 键盘 → 听写」中把麦克风来源设为 "M4 Mic"，在备忘录里听写一段中文，识别结果正常。
  3. 关闭 QuickTime 和听写后，M4 的麦克风指示灯在数秒内熄灭；再次开始听写时自动恢复。

### 阶段 4：漂移补偿与健壮性
- 实现 4.4 节的 PI 重采样。
- 健壮性场景：
  - M4 上 sender 被杀掉后重启。
  - 拔掉 M4 网线 10 秒后插回。
  - M4 休眠后唤醒。
  - 拔插外接麦克风。
  - 接收端长时间（> 1 小时）运行。
  
  以上每个场景都要能自动恢复，不需要重装驱动，也不能导致 coreaudiod 崩溃。
- **人工验收**：在 M2 上用 "M4 Mic" 连续录音 60 分钟，期间穿插上述场景。日志中的缓冲深度应稳定在目标附近，除故障期间外没有欠载或过载记录。

### 阶段 5（可选）：易用性
- 把 sender 打包成菜单栏 App（SwiftUI `MenuBarExtra`，`LSUIElement = YES`）：
  - 能选择设备、显示状态和电平、切换常开模式。
  - 用 `SMAppService` 实现登录时启动。
  - App 会单独申请麦克风权限和本地网络权限。
  - 构建方式：SwiftPM 构建后用脚本组装 .app，或者生成一个 Xcode 工程。
- 完善 README：从零开始的安装步骤、常见问题、卸载方法。

---

## 7. 风险与需要尽早验证的点

| 风险 | 验证 / 应对 |
|---|---|
| 插件沙盒内能否发送广播 / 收发 UDP | 阶段 3 一开始先做最小验证（spike）：插件内建 socket，广播一个 DISCOVER 并打日志。广播被拦截时退回单播（要求配置 `SenderAddress`）。如果 socket 完全不可用（概率低，roc-vad 已证明可用），改为 LaunchDaemon 辅助进程 + `AudioServerPlugIn_MachServices` + XPC 共享内存的架构，并先和用户确认 |
| ad-hoc 签名的插件不被加载 | 改用 Apple Development 证书签名 |
| 听写对第一个字有截断（M4 麦克风冷启动） | `--always-on` 选项；或把 stop-grace 加长到几分钟 |
| 插件 bug 导致 coreaudiod 反复崩溃 | 所有指针和长度都要检查；IO 回调里绝不抛异常；`uninstall-driver.sh` 必须随时可用。测试时提醒用户不要在重要通话中进行 |
| 个别应用不认单声道设备 | 配置 `Channels = 2` |
| macOS 版本差异 | 目标 macOS 13+；在 PROGRESS.md 中记录两台机器的实际系统版本（`sw_vers`） |

---

## 8. 给 Claude Code 的工作约定

1. 开始前先运行 `sw_vers`、`xcodebuild -version`（或 `swift --version`）、`cmake --version`，并记录到 PROGRESS.md。缺少依赖时告诉用户安装命令。
2. 严格按阶段推进，**不要跳阶段**。每个阶段结束时停下来，把人工验收步骤给用户。
3. 凡是需要 `sudo` 的操作（安装驱动、重启 coreaudiod），把命令写进脚本，让用户自己执行或确认后执行。
4. 引入 libASPL 前先阅读它的 README 和 examples，以实际 API 为准；本文档提到的类名和回调名只是示意。
5. 发现本文档的设计在实践中不可行时，在 PROGRESS.md 中记录原因和替代方案，先征得用户同意再改变架构。
6. 代码风格：Swift 用 Swift Concurrency 以外的简单线程模型即可（音频回调里不要用 async）；C++ 部分实时路径不使用 STL 容器的动态分配。
