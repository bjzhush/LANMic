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

## 阶段 1：网络链路 —— 已完成，待人工验收

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
