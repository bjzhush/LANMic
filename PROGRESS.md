# NetMic 进度记录

规格：`netmic-plan.md`。仓库根目录即本目录（`LANMic/`，对应规格中的 `netmic/`）。

## 环境

| 机器 | 系统 | 工具链 |
|---|---|---|
| M2（开发机兼接收端，Mac mini M2） | macOS 27.0 (26A428) | 仅 Command Line Tools：Swift 6.4 (swiftlang-6.4.0.34.1)、Apple clang 21.0.0；无 Xcode；cmake 4.4.5（Homebrew） |
| M4（发送端） | macOS 15.7.3 (24G419) | 只运行 netmic-sender |

**与规格的差异**：规格假定在 M4 上开发、用 `deploy-m2.sh` 推到 M2；实际开发机是 M2（2026-11-01 用户指出，此前记录误写为 M4）。
因此驱动可在 M2 本机直接安装，需要拷贝的是 `netmic-sender` 到 M4。网络上只能 M4 → M2 发起 ssh，反向不行。
部署变更（2026-11-02 用户同意）：删除 `deploy-m2.sh`；驱动在 M2 本机用 `scripts/install-driver.sh` 安装（sudo 由用户执行）；
M4 用一条 `scp` 从 M2 拉取 `netmic-sender`（见 README）。

### 环境问题

1. **SwiftPM 无法解析任何 Package.swift**（`swift build` / `swift test` 链接 manifest 失败）。
   原因：CLT 目录中残留了 Swift 5.10（2024-02）的
   `/Library/Developer/CommandLineTools/usr/lib/swift/pm/ManifestAPI/PackageDescription.swiftmodule/{arm64,x86_64}-apple-macos.private.swiftinterface`，
   编译器优先使用它们，与 6.4 的 `libPackageDescription.dylib` 符号不匹配。
   已验证：复制一份去掉这两个文件的 ManifestAPI，manifest 可正常编译运行。
   修复需 sudo（移走这两个文件，或重装 CLT），由用户执行。
2. ~~cmake 未安装~~ 用户已安装（4.4.5）。阶段 0 的 `scripts/test-all.sh` 在没有 cmake 时会退回直接用 clang++ 编译测试；阶段 2 构建驱动必须有 cmake。
3. 无 XCTest（只有 CLT）→ Swift 测试使用 swift-testing（`import Testing`），CLT 自带。
4. 用户已移走问题 1 中的两个文件（2026-11-01），SwiftPM 恢复正常。
5. 只有 CLT 时，默认构建系统（swiftbuild）从干净目录构建测试会报 `plugin for module 'TestingMacros' not found`，表现为 `error: fatalError`
   （增量构建有时能过，容易误判）。解决：`swift test -Xswiftc -plugin-path -Xswiftc $(xcode-select -p)/usr/lib/swift/host/plugins/testing`，
   `scripts/test-all.sh` 已自动加上。

## 阶段 0：骨架与纯逻辑 —— 已完成，用户验收通过（2026-11-01）

完成内容：
- 仓库结构、`.gitignore`、`docs/protocol.md`。
- `shared/netmic_protocol.h`：常量、packed 包头（static_assert 24 字节）、显式小端编解码、负载校验、ANNOUNCE / PCM 辅助函数。全部 inline、无分配。
- `shared/test_vectors.txt` + `scripts/gen-test-vectors.py`：用 Python `struct` 独立生成的参考向量（合法包 8 个、非法包 13 个、ANNOUNCE 3 个、PCM 1 个）。C++ 与 Swift 两套测试都读这一个文件，确保字节级一致。
- Swift Package `sender/`：`NetMicProtocol` 模块（编解码、Token、Announce、PCM 饱和转换、私有地址判断），`netmic-sender` / `netmic-probe` 为占位入口（阶段 1 实现）。
- `driver/src/JitterBuffer.{h,cpp}`：无锁 SPSC 抖动缓冲 + PI 漂移补偿（见下方设计说明）。
- `driver/tests/`：无依赖的小测试框架；协议向量测试 7 个，JitterBuffer 测试 20 个。
- `driver/CMakeLists.txt`：`netmic_core` 静态库 + `netmic_tests`。插件 bundle 与 libASPL submodule 留到阶段 2 再加。
- `scripts/test-all.sh`：跑 Swift + C++ 测试；`TSAN=1` 时额外用 ThreadSanitizer 跑并发压力测试。

测试结果（2026-11-01，M2）：
- C++：27/27 通过（CMake 构建）。ThreadSanitizer 下并发压力测试无报告。
- Swift：12/12 通过（修好 CLT 后，`scripts/test-all.sh` 用 `swift test` 从干净目录和增量两种方式各跑一次，均通过）。

漂移仿真结果（300 s，0–4 ms 均匀网络抖动 + 乱序，512 帧 IO，目标 20 ms）：

| 漂移 | 末 100 s 平均修正 | 收敛时间（误差 < 1 ms） | 30 s 后最大深度误差 | 修正比例最大波动 | 欠载/过载 |
|---|---|---|---|---|---|
| +100 ppm | +99.95 ppm | 17.0 s | 17.4 帧 | ±198 ppm | 0 / 0 |
| −100 ppm | −101.44 ppm | 16.0 s | 14.6 帧 | ±149 ppm | 0 / 0 |
| +300 ppm | +299.44 ppm | 18.2 s | 19.5 帧 | ±208 ppm | 0 / 0 |
| −300 ppm | −301.85 ppm | 16.0 s | 22.0 帧 | ±236 ppm | 0 / 0 |

输出 440 Hz 正弦的相邻采样最大跳变 0.0288（理论值 0.0288），没有咔哒声。

### JitterBuffer 设计说明 / 决策

- **坐标**：内部使用单调递增的 int64 帧坐标；写端按 stream 记录锚点（`anchorSample_ → anchorBase_`），用相对锚点的差值计算位置，任意 64 位 samplePos 都不会溢出。
- **环形槽为 `std::atomic<float>`**（arm64 上就是普通 ldr/str）。这样迟到包可以安全回填读端已可见的零填充空洞（规格要求“比读位置旧的才丢弃”），不存在数据竞争。
- **重置由写端执行**（streamId 变化、包超出窗口、`requestReset()`）：先递增 `generation_`，再把读写位置都挪到 `旧写位置 + 容量`。读端用 CAS 推进读位置并复查 generation，与重置竞争的那次读取输出静音并丢弃。新基址总比任何旧读位置大，CAS 不会有 ABA 问题。
- **IO 停止时读端不动**：写端写满一个容量后会重新锚定而不是覆盖；IO 恢复后靠过载规则直接跳到“最新 − 目标深度”。
- **开始播放条件**：深度 ≥ 目标且 > 本次 IO 帧数，避免目标小于 IO 缓冲时反复欠载。
- **PI 参数**：Kp = 1.04e-5 /帧，Ki = 1.30e-6 /(帧·s)，对应 ωn ≈ 0.25 rad/s、ζ ≈ 1，修正限幅 ±1000 ppm，积分抗饱和。
- **与规格的偏差**：深度平滑时间常数用 **2 s**（规格写“约 1 秒”）。1 s 时包粒度（240 帧）和抖动带来的测量噪声会让修正比例波动约 ±270 ppm；2 s 时降到约 ±150–240 ppm，收敛时间仍在 16–18 s，满足 10–30 s 的要求。±300 ppm 的音高变化约 0.005 个半音，听不出来。参数在 `JitterBufferConfig` 中可调。
- 欠载 / 过载 / 重置时都保留积分项（它就是两台机器时钟偏差的估计值）。

## 阶段 1：网络链路 —— 已完成，用户验收通过（2026-11-02）

完成内容：
- `netmic-sender`：设备枚举（`--list-devices`）、按名称子串或 UID 选设备、只设置 inputNode 的 `CurrentDevice`（不改系统默认输入）、
  `--channel N|mix`、`--gain-db`、`--token`、按需启停（订阅 3 s 过期、`--stop-grace`）、`--always-on`、配置变化（拔插/采样率）自动重建并换 streamId、
  设备丢失时每秒重试、`-v` 每 5 s 打印订阅者 / 发包数 / 电平。默认只接受私有地址段的请求（`--allow-any-source` 可关闭）。
- `netmic-sender --tone <Hz>`（规格外新增）：用 −12 dBFS 正弦代替麦克风，按主机时钟节拍发送。用于无人说话 / 无麦克风时测试网络链路，阶段 2–3 也会用到。
- `netmic-probe`：`--discover`、`--record N --out x.wav`、`--stats N`；统计收包、丢包（按 seq）、乱序、重复、RFC 3550 抖动、漂移 ppm（samplePos 对到达时间做最小二乘，需 ≥10 s 数据）。Ctrl-C 提前结束时照样写文件、发 UNSUBSCRIBE。
- 广播发现同时发往 `255.255.255.255` 和每个网卡的子网广播地址（多网卡时受限广播只走主网卡）。
- 新增模块：`NetMicKit`（UDP socket、接收统计，sender / probe 共用），`CNetMicSupport`（C 写的无锁环形缓冲，供采集实时回调使用）。新增 11 个单元测试（漂移估计、抖动、丢包/乱序/重复、声道选择/混合、溢出、回环收发）。
- `scripts/build-all.sh`。（原有 `scripts/deploy-m2.sh`，因开发机就是 M2 已删除，见「环境」。）

设计变更（相对规格 4.1）：
- **采集不用 `installTap`，改用 `AVAudioSinkNode`**（仍是 AVAudioEngine）。Apple 文档写明 installTap 的缓冲范围是 100–400 ms，超出 50 ms 的延迟预算；sink node 按设备 IO 周期（约 10 ms）回调。
  实时回调里只做降为单声道并写入 C 环形缓冲（无分配、无锁、无 Swift 运行时）；打包线程每 2 ms 取一次数据，做采样率转换（仅当设备不是 48 kHz 时用 AVAudioConverter）、增益、Int16、凑 240 帧发包。
  因此混合 / 选声道在重采样之前完成，转换器只处理单声道。

本机测试（M2，`--tone 440`，回环 127.0.0.1）：
- 单播 / 广播发现都正常；订阅后 7 ms 收到第一个音频包；stop-grace 到时停止发送。
- 录 3 s：601 包，丢包 0，峰值 −12.04 dBFS，频率 440 Hz，相邻采样最大跳变 = 理论值（无断续）。
- 统计 15 s：3001 包，丢包 0，抖动 0.5 ms。
- **未测：真实麦克风采集路径**。开发机（M2）没有接任何输入设备（`system_profiler SPAudioDataType` 只有扬声器），`--list-devices` 为空。需在人工验收中用外接麦克风验证。

人工验收记录（2026-11-02）：
- M2 系统版本：macOS 27.0 (26A428)
- M4 系统版本：macOS 15.7.3 (24G419)；麦克风 UGREEN USB MIC-CM769（48 kHz，2 ch，取 channel 0）
- 发现：正常。M4 有两块网卡在网，`192.168.1.4` 和 `192.168.6.112` 都应答。
- 首次验收发现 bug：采集每 ~0.18 s 重启一次（stream 不断变化）。原因：在 inputNode 的 AudioUnit 上设置
  `kAudioOutputUnitProperty_CurrentDevice` 后，约 65 ms 会收到一次 `AVAudioEngineConfigurationChange`，
  旧代码收到即重启，形成循环。修复（`39c41b2`）：通知合并 300 ms；引擎仍在运行且设备、格式未变则忽略；
  重启间隔 ≥ 1 s；日志打印状态。M4 上确认出现 `configuration change ignored`，之后不再重启。
- 录音 10 s（`--record`）：回放正常，声音清楚（用户确认）。
- `--stats 300`（M2 ← M4，`--sender 192.168.1.4`）：
  `packets 60001  lost 0 (0.000%)  reordered 0  dup 0  jitter 5.29 ms  drift +0.1 ppm`。
  - 抖动 5.3 ms 比回环（0.5 ms）高，主要是发送端成对发包：sink node 按 ~10 ms 的设备 IO 周期回调，
    一次凑出两个 240 帧包连发。20 ms 的抖动缓冲目标足够吸收。
  - 漂移 +0.1 ppm，小于预期的几 ppm；估计代码已复查，阶段 3 驱动的 PI 环运行时再对照。
- 无订阅者后自动释放麦克风：sender `-v` 日志显示 `subscribers 0 | capture off`，发包数不再增加。
  （`capture off` 与 `capture: stopped` 日志在同一处设置；用户未在滚动的日志里找到该行，也未观察到菜单栏橙色指示点。）

## 阶段 2：最小虚拟设备（不接网络）—— 已完成，用户验收通过（2026-11-02）

内容：
- libASPL 以 git submodule 引入（`driver/third_party/libASPL`，固定在 tag v3.1.2），CMake `ExternalProject` 构建为静态库。
  按约定先读了 README 与 `examples/SinewaveDevice`、`NetcatDevice`，以实际 API 为准。
- `driver/src/`：
  - `Plugin.cpp`：CFPlugIn 工厂 `NetMicEntryPoint`（工厂 UUID `2F6F90F6-0E39-466F-BF8E-04323C6F80E3`）。
  - `NetMicDevice.{h,cpp}`：设备 "M4 Mic"（制造商 NetMic，UID `com.netmic.device.m4mic`，Virtual，48 kHz，
    只有 1 个输入流，Float32 interleaved，带音量 / 静音控件），Latency 报告为目标缓冲帧数（默认 960）。
    IO 回调只调用 `ToneGenerator`（无分配、无锁、无日志）；`OnStartIO` / `OnStopIO` 只改一个原子量（阶段 3 给网络线程用）。
  - `ToneGenerator`：440 Hz、−12 dBFS 正弦，相位由设备采样时间算出，所以各客户端、各 IO 周期之间连续。
  - `Config.{h,cpp}`：读取 bundle Info.plist 里的 `NetMicConfig`，每个键单独校验，非法值回落到默认并打 os_log 错误。
  - `Log.h`：os_log，subsystem `com.netmic.driver`；category `driver` 是驱动自己的日志，`aspl` 是 libASPL 的逐操作跟踪（debug 级）。
- `driver/Info.plist.in`：CFPlugIn 键、`AudioServerPlugIn_Network = true`、`sandboxSafe`、`NetMicConfig` 默认值。
- 构建产物 `driver/build/NetMic.driver`，构建后自动 ad-hoc 签名（环境变量 `CODESIGN_IDENTITY` 可改用证书）。
- 脚本：`scripts/install-driver.sh`、`uninstall-driver.sh`（都在 M2 本机运行，内部用 sudo）、`logs.sh [-v]`。
  安装 / 卸载后会等待并检查设备是否出现 / 消失。
- 测试：新增 7 个单元测试（配置解析 5 个、测试音 2 个），C++ 共 34 个。
  新增 `netmic_plugin_smoke`：在进程内加载 NetMic.driver，按 HAL 的顺序调用工厂 → Initialize → 属性查询 →
  AddDeviceClient → StartIO → 100 个 IO 周期 → StopIO，检查设备名、UID、制造商、传输类型、延迟、采样率、
  流方向和格式，以及输出是连续的 −12 dBFS 440 Hz 正弦。不需要 sudo，也不碰 coreaudiod，`test-all.sh` 会跑它。

与规格的差异：
- 规格要求"先在 M4 本机安装测试，再部署到 M2"。开发机就是 M2，所以直接在 M2 安装（见「环境」）。
- libASPL 的 IO 入口（`Device::DoIOOperation` 等）内部持有一个 `std::mutex`（ioMutex_），只与 StartIO / StopIO
  等控制操作竞争。这是 libASPL 自身的设计（roc-vad 同样如此），我们自己的 IO 代码不加锁。

踩到的问题：
- 生成的 `Info.plist` 原本放在 `driver/build/` 根目录，CoreFoundation 会把 `build/` 当成同目录测试程序的主 bundle，
  导致 `CFBundleCreate(build/NetMic.driver)` 返回空。改为生成到 `build/plist/Info.plist`。
- zsh 里 `log` 是内建命令，查看系统日志要写 `/usr/bin/log`（脚本里已这样写）。

本机检查结果（2026-11-02，M2，未安装到系统）：
- 单元测试 34 个、冒烟测试 3 个全部通过；TSan 并发测试通过。
- bundle：`Mach-O 64-bit bundle arm64`，minos 13.0，只依赖 CoreFoundation / CoreAudio / libc++ / libSystem，
  `codesign --verify --strict` 通过（adhoc），`plutil -lint` 通过。
- 冒烟测试时 os_log 正常输出 `loading: device "M4 Mic", 1 ch, target 20 ms ...`，无配置警告。

人工验收记录（2026-11-02，M2）：
- 安装：`install-driver.sh` 成功，「系统设置 → 声音 → 输入」出现 "M4 Mic"。
- QuickTime 选 M4 Mic 录 10 s：回放全程是稳定的嘟声，没有其他声音（用户确认）。
- 卸载：`uninstall-driver.sh` 成功，M4 Mic 从输入列表消失。

## 阶段 3：接入网络 —— 已完成，用户验收通过（2026-11-02）

内容：
- `NetReceiver`（`driver/src/NetReceiver.{h,cpp}`）：插件加载时创建的常驻网络线程，是 JitterBuffer 唯一的写者。
  - socket 绑定临时端口并开 `SO_BROADCAST`；`poll` 50 ms 超时，每次把队列里的包全部读完。
  - IO 未运行时不发任何包，收到的包读出后丢弃。
  - IO 运行时：没配置 `SenderAddress` 就每 1 s 广播 DISCOVER（同时发往 255.255.255.255 和各网卡的子网广播地址），
    收到格式正确（48 kHz / 1 ch / Int16 / 240 帧）的 ANNOUNCE 后锁定该发送端；之后每 500 ms 发 SUBSCRIBE（带 Token）。
    只接受来自已锁定发送端（IP + 端口都要对）的 AUDIO。发现来的发送端 5 s 没声音就丢弃并重新发现；
    配置写死的发送端只记一次警告并继续订阅。发现到的发送端在 IO 停止后仍保留，下次开始录音时立即订阅。
  - IO 停止时发两次 UNSUBSCRIBE 并清空缓冲；IO 开始时也先清空。
  - 每 10 s（仅 IO 运行时）和 IO 停止时用 os_log 输出统计：收包、丢包（按 seq）、乱序、被忽略的包、
    缓冲状态和深度、欠载、过载、补零帧数、迟到包、重置次数。
- `StartIO` / `StopIO` 只调用 `NetReceiver::setActive()`（一个原子量），不在回调里做任何网络操作。
- `SampleTimeCache`（`driver/src/SampleTimeCache.h`）：HAL 每个 IO 周期会为每个客户端各调一次 ReadInput，
  时间戳相同；JitterBuffer 只能有一个读者，每帧只能消费一次。缓存按设备采样时间索引，同一段时间只从
  JitterBuffer 取一次，各客户端拿到相同数据；设备时间跳变时重新开始。实时安全（预分配 16384 帧）。
- `JitterBuffer::clear()`：写端立即清空（原 `requestReset()` 要等下一次 write 才生效，会让重新开始录音时
  先播出上次残留的旧数据）。
- 本阶段关闭漂移补偿（`driftCompensation = false`），阶段 4 再开。阶段 1 实测 M4→M2 漂移约 0.1 ppm。
- 阶段 2 的测试音 `ToneGenerator` 已删除。
- `install-driver.sh --set Key=Value`：安装时临时改 NetMicConfig（复制、修改、重新 ad-hoc 签名），
  不用改源码重新构建。例如广播被拦时 `--set SenderAddress=192.168.1.4`。

测试：
- C++ 单元测试 42 个：新增 NetReceiver 4 个（本机 UDP 假发送端：未激活不发包；订阅 + Token + 心跳间隔 +
  收包写入缓冲 + 丢包计数 + 取消订阅后清空；忽略其他来源和未激活时的包；广播发现 → 忽略错误格式 →
  ANNOUNCE → 订阅 → 5 s 静默后重新发现）、SampleTimeCache 5 个、JitterBuffer::clear 1 个。
- ThreadSanitizer：JitterBuffer 并发压力测试（加入了随机 clear）和 NetReceiver / SampleTimeCache 测试均无报告。
- `scripts/e2e-local.sh`（已加入 `test-all.sh`）：真实 `netmic-sender --tone 440` 跑在本机回环端口，
  构建好的 NetMic.driver 复制一份改成 `SenderAddress=127.0.0.1`，在进程内按 HAL 顺序加载，两个客户端、
  按真实时间节拍跑 4 s 的 IO 周期。结果：两个客户端数据一致；最后 2 s 峰值 −12.04 dBFS、2 s 内 880 个周期、
  相邻采样最大跳变等于正弦理论值（无断续）；sender 日志里看到订阅、取消订阅和 stop-grace 后停止。
  驱动统计：790 包，丢包 0，欠载 0，过载 0，缓冲深度 19.7 ms（目标 20 ms）。
- 顺带验证了广播发现跨机器可用：一次默认配置的冒烟测试（普通进程，不在沙盒里）自动发现了 M4（192.168.1.4），
  1 s 收 153 包，丢包 0。所以 `test-all.sh` 改为只跑本机回环的端到端，避免测试时打开 M4 的麦克风。

尚未验证：
- 驱动在 coreaudiod 的沙盒里能否收发 UDP、能否广播（规格第 7 节的风险）。日志会在加载时打出
  `network: socket ready ...`，失败会有 `socket()/bind()/sendto failed` 错误；广播被拦时用
  `install-driver.sh --set SenderAddress=<M4 的 IP>` 退回单播。

与规格的差异：
- 规格要求"先在 M4 本机闭环（SenderAddress = 127.0.0.1），再部署到 M2"。开发机是 M2，本机闭环用
  `e2e-local.sh` 在进程内完成；装进 coreaudiod 后直接用广播发现连 M4。

人工验收记录（2026-11-02，M2，默认配置 = 广播发现）：
- **沙盒风险已排除**：驱动在 `Core-Audio-Driver-Service.helper` 里正常创建 socket、广播 DISCOVER，
  IO 开始后 3 ms 找到 M4（`found sender "M4 Mic" 192.168.1.4:47810`）。
- QuickTime 录音：用户通过屏幕共享操作 M2，听不到 M2 的声音（屏幕共享默认不传音频），所以由我分析录音文件
  `~/Desktop/test.m4a`：6.4 s，两段说话，浊音帧基频中位数 145 Hz，峰值 −5.4 dBFS 无削波，底噪约 −81 dBFS，
  说话段内未检测到掉音。
- 听写：麦克风来源设为 M4 Mic，在备忘录里听写中文，识别正常（用户确认）。
- 自动停止 / 恢复：结束 QuickTime 和听写后 M4 sender 出现 `capture: stopped`，再开始听写时自动 `capture: started`（用户确认）。
  驱动日志对应地出现 `IO stopped, unsubscribed` 和 `IO started, subscribing`（复用已发现的发送端，不再广播）。
- 驱动统计：约 2 分钟内 21772 包，**丢包 0**，过载 0；**欠载 7 次**（约每 20 s 一次），缓冲深度在 12–26 ms 之间波动。
  - 原因：目标缓冲 20 ms，而 HAL 每次读取 512 帧（10.7 ms），留给到达抖动的余量只有约 9 ms；
    发送端每 ~10 ms 成对发包，加上网络抖动（阶段 1 测得 RFC 3550 抖动 5.3 ms），偶尔晚到超过余量即欠载。
    每次欠载会补约 20 ms 静音。
  - 处理：放到阶段 4（验收要求"除故障期间外没有欠载"），先实测到达时间分布，再定缓冲目标
    （规格允许端到端 ≤ 50 ms）。

## 阶段 4：漂移补偿与健壮性 —— 已完成，用户验收通过（2026-11-02）

### 欠载原因与缓冲目标（实测）
工具：`netmic-probe --trace` 记录每个 AUDIO 包的到达时间；`driver/build/netmic_replay` 用真实到达时间驱动
JitterBuffer，模拟 HAL 每 10.67 ms 读 512 帧（16 个读相位，合计相当于 80 分钟），比较不同目标下的欠载。

两次 5 分钟记录（M4 UGREEN 麦克风 → M2，有线局域网）：

| | 旧 sender（设备 IO 512 帧 = 10.7 ms） | 新 sender（`--io-frames 240` = 5 ms） |
|---|---|---|
| 包间隔分布 | 一半 0 ms（背靠背），一半 10–13 ms：成批发出 | 集中在 4–6 ms |
| RFC 3550 抖动 | 5.32 ms | 0.59 ms |
| 晚到（相对最早可能到达）p50 / p99 / p99.9 / p99.99 / max | 6.6 / 12.5 / 13.9 / 19.9 / 29.8 ms | 0.8 / 2.1 / 4.9 / 9.4 / 13.0 ms |
| 丢包 | 0 | 0 |
| 漂移 | +1.2 ppm | +0.4 ppm |

回放结果（新的余量语义，漂移补偿开）：旧 sender 余量 20 ms 时欠载 8 次、25 ms 1 次、30 ms 0 次；
新 sender 余量 10 ms 时 20 次、**15 ms 及以上 0 次**。PI 控制器平均 |ratio| 约 64 ppm（深度随读周期呈锯齿，
被当成误差），对应音高变化约 0.0001 半音，听不出来。

结论与改动：
1. 欠载的主因是**发送端成批发包**（M4 麦克风 IO 512 帧），不是网络。sender 新增 `--io-frames`（默认 240），
   通过 `kAudioDevicePropertyBufferFrameSize`（每进程设置）把麦克风 IO 设成 5 ms；M4 上确认生效（`IO 240 frames`）。
   打包线程轮询从 2 ms 改为 1 ms。
2. 原来的目标深度没有考虑每次读取量：目标 20 ms、一次读 10.7 ms，只剩 9 ms 余量；客户端 IO 缓冲比目标还大时
   （例如 4096 帧）会每个周期都欠载。**`targetFrames` / `TargetLatencyMs` 的含义改为"读取量之上的抗抖动余量"**，
   实际目标深度 = 余量 + 本次读取帧数，所以任何 IO 大小都有同样的余量。日志里的 depth 也按余量报告，应接近目标。
   默认值仍为 20 ms（比实测需要的 15 ms 多 5 ms 余地）。估算端到端延迟约 30–35 ms
   （M4 IO 5 ms + 打包 ≤1 ms + 网络 <1 ms + 缓冲约 20 ms + 半个读周期 + HAL）。
3. 驱动开启漂移补偿（阶段 0 实现的 PI 重采样）。

### 健壮性改动
- sender 采集看门狗：应在采集但 2 s 没有数据时，重建采集（休眠唤醒、拔掉麦克风时 AVAudioEngine 可能不发
  配置变更通知就停了）；设备不在时沿用每 1 s 重试，同一错误只记一次日志。
- 驱动日志：每 10 s 的统计带增量（`lost (+n)`、`underruns (+n)`、`overruns (+n)`）和重采样比 ppm；
  欠载 / 过载发生时（最多每秒一行）单独记 `buffer: ... underrun(s)`，便于和网络事件对照。
- 已有、无需改动的恢复路径：发送端重启 → 新 streamId → 缓冲重置；发现来的发送端 5 s 无声 → 丢弃并重新广播发现；
  sendto 失败按每 100 次记一次日志；socket 绑定 INADDR_ANY，网卡变化不影响。

### 测试
- C++ 43 个（新增：大 IO 读取不欠载；场景测试改为余量语义）、TSan、端到端全部通过。

### 人工验收：60 分钟录音 + 故障场景（2026-11-02 01:40–02:41，M2 QuickTime 录 M4 Mic）
分析方法：M2 驱动日志（权威）+ `driver/build/netmic_find_gaps`（新增工具：把录音里整段精确为 0 的 5 ms 窗口
当作驱动没数据的时刻；真实麦克风总有底噪，不会出现精确 0）。录音 3643 s，从 01:40:11 开始，两者时间吻合。

| 时间 | 场景 | 录音 | 驱动日志 | 结果 |
|---|---|---|---|---|
| 01:40:46 | A 杀掉 sender，约 10 s 后重启 | 静音 13.4 s | 5 s 无声 → 重新广播 → 01:40:59 重新订阅 | 自动恢复 |
| 01:41:29 | C M4 休眠 / 唤醒 | 静音 22.6 s | 同上，01:41:52 恢复 | 自动恢复 |
| 01:42:52 | B 关 M4 Wi-Fi、停用有线 | 静音 24.6 s | 没有发送端 | 自动恢复（见下） |
| 01:43:17–01:43:45 | B 网络恢复过程中 | **约 28 s 断断续续** | 经 Wi-Fi 找到 M4（192.168.6.112）；丢 295 包，欠载约 120、过载约 70 | 能通但质量差 |
| 01:43:45 | 用户在 M4 上用语音输入法占用麦克风 | 静音 29.2 s | sender 回应发现但不发音频，5 s 一轮共 5 轮，01:44:11 恢复 | 自动恢复 |
| 01:44:11–02:40:54 | 正常运行 56 分钟 | 无长于 100 ms 的空洞 | 欠载 1 次（02:36:28），过载 0，丢包 0，深度 19.8–19.9 ms，ratio ±100 ppm | 通过 |

- coreaudiod 没有崩溃，没有重装驱动；`IO stopped` 时统计：746208 包、丢包 295（全部在 Wi-Fi 那 30 s）。
- 场景 D（拔插 USB 麦克风）这次没有单独区分出来；语音输入法抢占麦克风同样验证了"sender 在、采集中断、
  恢复后自动续上"这条路径。
- 录音里 01:44 之后零星的单个 5 ms 精确静音窗口（每分钟 0–4 个）对应时刻驱动没有任何欠载，是 AAC 编码
  在极安静段的产物。

发现的问题和处理（用户同意）：
1. **网络卡顿后的突发让缓冲长时间偏深**。02:36:28 网络停顿约 70 ms 后一次性到达：欠载一次（约 10 ms 静音），
   重新缓冲后深度到 60 ms（超目标 40 ms），PI 修正上限 1000 ppm 只能每秒追 1 ms，用了约 40 s 才回到 20 ms，
   期间多了最多 40 ms 延迟。
   处理：JitterBuffer 新增"追赶"（`catchUpMarginFrames` = 15 ms，`catchUpSec` = 0.5 s，仅在漂移补偿开时）：
   深度连续 0.5 s 都比目标高 15 ms 以上时，一次丢掉这段时间里始终多出来的部分（取最小超出量，不会丢到目标以下），
   并把这段时间里累积的 PI 积分还原（那是突发，不是漂移）。60 ms 的硬过载阈值保留。实时线程里只是几次算术，
   不分配不加锁。日志的统计行和事件行增加 `catch-ups`。
   - 新测试 `jb_burst_after_stall_catches_up`：60 ms 停顿后突发，峰值超目标 +39 ms（与现场一致），
     有追赶 1.9 s 回到 ±2 ms 内、追赶 1 次、过载 0；关掉追赶要 47 s。
   - 漂移测试（0/±100/±300 ppm、0–4 ms 抖动）检查追赶次数为 0；两段 5 分钟真实到达记录回放（`netmic_replay`
     新增 catch-ups 列）在 15/20/60 ms 余量下追赶都是 0 次——正常抖动不会触发。
2. **Wi-Fi 上质量差**：丢包无法靠缓冲解决，到达也成批。处理：不改代码，README 说明建议有线；必须用 Wi-Fi 时
   `install-driver.sh --set TargetLatencyMs=60`。

测试：C++ 44 个、TSan（JitterBuffer 并发、NetReceiver）、端到端全部通过。

追赶改动未重新做 60 分钟录音（由单元测试和真实到达记录回放验证）；安装新驱动后日志里能看到
`catch-ups 0 (+0)`，网络卡顿时会出现 `buffer: ... catch-up(s)`。

安装追赶版驱动后复查（2026-11-02 08:14，用户确认日志里 `catch-ups 0`）：开录 43 s 时出现 1 次欠载
（08:15:50，又一次短暂网络停顿，丢包 0），重新缓冲后深度 +10 ms（低于 15 ms 追赶阈值，由 PI 修正，
约 20 s 回到 19.9 ms）；之后稳定。拔插 USB 麦克风（场景 D）没有单独测试。


## 阶段 5：易用性 —— 已完成，用户验收通过（2026-11-02）

### 菜单栏 App（NetMic Sender）
- 发送端代码拆成库 `SenderCore`（AudioDevices、Capture、Server、ToneSource），命令行 `netmic-sender` 和 App 共用，
  行为一致。为 App 新增：`Server.stop()`（run() 约 100 ms 内返回，释放麦克风和端口）、运行中可改的 `alwaysOn`、
  `status()`（订阅者、是否在采集、发包数）；`Capture.shutdown()`（同步停采集和看门狗）；`logSink`（App 里写到
  os_log，subsystem `com.netmic.sender`）。
- App：SwiftUI `MenuBarExtra`（window 样式），`LSUIElement`。图标采集时为实心麦克风。菜单内容：状态、峰值电平条、
  麦克风选择（按 UID 保存；拔掉的设备显示“未连接”，插回后由 Capture 的重试自动续上）、常开采集、登录时启动
  （`SMAppService.mainApp`）、退出。其他参数（Name、GainDb、Channel、Token、StopGrace、IOFrames、Port）走
  `defaults write com.netmic.sender`。
- 权限：Info.plist 含 `NSMicrophoneUsageDescription`、`NSLocalNetworkUsageDescription`；启动时请求麦克风权限，
  被拒时菜单显示红字和打开设置的按钮。端口被占用（命令行版还开着）时显示错误。
- 构建：`scripts/build-app.sh` → SwiftPM release 构建 + 组装 `sender/build/NetMicSender.app` + ad-hoc 签名
  （显示名 “NetMic Sender”，最低 macOS 13，已用 vtool 确认 minos 13.0）。M4 用 `scp -r` 从 M2 拉取。
  ad-hoc 签名每次构建不同，重新构建后 M4 会再问一次麦克风权限（README 常见问题里写了）。

### README
重写：要求、从零安装（构建 → 装驱动 → M4 装 App 和授权 → 使用）、菜单说明、`defaults` 设置、命令行版、
驱动配置表、网络（建议有线）、常见问题、两台机器的卸载步骤、开发工具。

### 测试
- 新增 Swift 测试 `ServerLifecycleTests`：ToneSource + Server 跑在线程上，回环订阅收到音频；取消订阅后常开时继续、
  关掉常开后按 stop-grace 停止、再打开常开又开始；stop() 后 run() 1 s 内返回、音源停止；同一端口第二轮能重新绑定。
- Swift 24 个、C++ 44 个、端到端全部通过。
- 在 M2 上启动 App 确认能运行（停在首次麦克风授权）；实际采集和发送在 M4 上人工验收。

人工验收（2026-11-02，M4 运行 NetMic Sender.app，M2 用 “M4 Mic”）：用户确认按 README 步骤安装和使用均正常，无问题。

## 全部阶段完成

剩余已知事项（不阻塞使用）：
- 有线局域网偶尔有一次几十毫秒的网络停顿（60 分钟里 1 次），会造成约 10 ms 静音；原因未查明（可能在网络或 M4 一侧）。
- Wi-Fi 下质量差，只能靠加大 `TargetLatencyMs` 缓解（README 已说明）。

## 部署配置：固定走有线（2026-11-02）

用户决定只走有线网络（192.168.1.*），不再让驱动在有线断开时切到 Wi-Fi（192.168.6.*）。偶发停顿暂不排查。

- M2 上执行 `scripts/install-driver.sh --set SenderAddress=192.168.1.4`（20:06 安装）。已安装驱动的
  `NetMicConfig:SenderAddress` = `192.168.1.4`，其余为默认（20 ms、1 声道、端口 47810、无 Token）。
- 驱动日志确认：`loading: ... sender 192.168.1.4:47810`，不再广播发现；之后两次 IO 收包丢包 0。
- 效果：有线断开时 “M4 Mic” 为静音，不会改走 Wi-Fi；有线恢复后订阅心跳（每 500 ms）自动接上。
- 随后（用户要求）仓库默认也改为固定有线：`driver/Info.plist.in` 的 `SenderAddress` = `192.168.1.4`，
  直接 `scripts/install-driver.sh` 即可，不用再带 `--set`。改回广播发现：`--set SenderAddress=`（已验证 PlistBuddy
  写入空字符串）。代码里 `Config` 的默认值仍为空（广播），只影响 bundle 的默认配置；测试不受影响（e2e 自己改成
  127.0.0.1，NetReceiver 测试用代码构造的 Config），全部通过。M4 有线网卡的 IP 需要在路由器里固定。

## NetMic Sender CPU 优化（2026-11-09）

用户反馈：M4 上 NetMic Sender 在活动监视器 CPU 排前三。在 M2 上用同一份代码测量（`ps` 累计 CPU 时间）：

| 情况 | 优化前 | 优化后 |
|---|---|---|
| 命令行 sender 空闲（对照，同一核心） | 0.05% | — |
| App 空闲（无接收端、菜单关闭） | 2.7% | 0.22% |
| App 采集中（常开，菜单关闭，含 AVAudioEngine IO 线程） | —（估计 ≥4%） | 0.9% |

原因（`sample` 确认）：App 每 0.2 s 轮询一次状态，并且**每次都给 @Published 属性赋值**（即使值没变），
SwiftUI 因此每秒 5 次重绘菜单栏图标（主线程上 NSButtonCell 解析符号图片、自动布局、Core Animation 提交），
菜单关着、没在采集时也一样。另外采集时打包线程每 1 ms 轮询一次环形缓冲区（`--tone` 对照测得约 1.3%）。

改动：
- 只在值变化时赋值；电平变化小于 0.5 dB 不更新。
- 菜单打开时每 0.2 s 轮询并读电平；菜单关闭时每 1 s 只查状态（图标），不读电平（timer 带 tolerance）。
- 打包线程改为由 IO 回调 `DispatchSemaphore.signal()` 唤醒（无分配、无锁），不再 1 ms 轮询；等待带 100 ms 超时，
  只用于响应停止。实测（M2 App 采集 “M4 Mic” → 本机 `netmic-probe --stats 15`）：200 包/秒、丢包 0、抖动 0.04 ms。
- 测试全部通过（Swift 24、C++ 44、端到端）。M4 上需要重新拷贝 App，ad-hoc 签名变了会再问一次麦克风权限。
