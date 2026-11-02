# NetMic 进度记录

规格：`netmic-plan.md`。仓库根目录即本目录（`LANMic/`，对应规格中的 `netmic/`）。

## 环境

| 机器 | 系统 | 工具链 |
|---|---|---|
| M4（开发机，Mac mini M4） | macOS 27.0 (26A428) | 仅 Command Line Tools：Swift 6.4 (swiftlang-6.4.0.34.1)、Apple clang 21.0.0；无 Xcode；**cmake 未安装** |
| M2 | 待记录（`sw_vers`） | — |

### 环境问题

1. **SwiftPM 无法解析任何 Package.swift**（`swift build` / `swift test` 链接 manifest 失败）。
   原因：CLT 目录中残留了 Swift 5.10（2024-02）的
   `/Library/Developer/CommandLineTools/usr/lib/swift/pm/ManifestAPI/PackageDescription.swiftmodule/{arm64,x86_64}-apple-macos.private.swiftinterface`，
   编译器优先使用它们，与 6.4 的 `libPackageDescription.dylib` 符号不匹配。
   已验证：复制一份去掉这两个文件的 ManifestAPI，manifest 可正常编译运行。
   修复需 sudo（移走这两个文件，或重装 CLT），由用户执行。
2. **cmake 未安装**：`brew install cmake`。阶段 0 的 `scripts/test-all.sh` 在没有 cmake 时会退回直接用 clang++ 编译测试；阶段 2 构建驱动必须有 cmake。
3. 无 XCTest（只有 CLT）→ Swift 测试使用 swift-testing（`import Testing`），CLT 自带。
4. 用户已移走问题 1 中的两个文件（2026-11-01），SwiftPM 恢复正常。
5. 只有 CLT 时，默认构建系统（swiftbuild）从干净目录构建测试会报 `plugin for module 'TestingMacros' not found`，表现为 `error: fatalError`
   （增量构建有时能过，容易误判）。解决：`swift test -Xswiftc -plugin-path -Xswiftc $(xcode-select -p)/usr/lib/swift/host/plugins/testing`，
   `scripts/test-all.sh` 已自动加上。

## 阶段 0：骨架与纯逻辑 —— 已完成，待验收

完成内容：
- 仓库结构、`.gitignore`、`docs/protocol.md`。
- `shared/netmic_protocol.h`：常量、packed 包头（static_assert 24 字节）、显式小端编解码、负载校验、ANNOUNCE / PCM 辅助函数。全部 inline、无分配。
- `shared/test_vectors.txt` + `scripts/gen-test-vectors.py`：用 Python `struct` 独立生成的参考向量（合法包 8 个、非法包 13 个、ANNOUNCE 3 个、PCM 1 个）。C++ 与 Swift 两套测试都读这一个文件，确保字节级一致。
- Swift Package `sender/`：`NetMicProtocol` 模块（编解码、Token、Announce、PCM 饱和转换、私有地址判断），`netmic-sender` / `netmic-probe` 为占位入口（阶段 1 实现）。
- `driver/src/JitterBuffer.{h,cpp}`：无锁 SPSC 抖动缓冲 + PI 漂移补偿（见下方设计说明）。
- `driver/tests/`：无依赖的小测试框架；协议向量测试 7 个，JitterBuffer 测试 20 个。
- `driver/CMakeLists.txt`：`netmic_core` 静态库 + `netmic_tests`。插件 bundle 与 libASPL submodule 留到阶段 2 再加。
- `scripts/test-all.sh`：跑 Swift + C++ 测试；`TSAN=1` 时额外用 ThreadSanitizer 跑并发压力测试。

测试结果（2026-11-01，M4）：
- C++：27/27 通过（clang++ 直接编译，cmake 缺失）。ThreadSanitizer 下并发压力测试无报告。
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

## 阶段 1：网络链路 —— 未开始
