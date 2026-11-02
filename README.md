# NetMic

在局域网内把一台 Mac（M4）的麦克风变成另一台 Mac（M2）上的系统级输入设备 “M4 Mic”。

开发规格见 `netmic-plan.md`，进度见 `PROGRESS.md`，线协议见 `docs/protocol.md`。

## 开发

```bash
brew install cmake          # 构建驱动需要
scripts/test-all.sh         # Swift + C++ 单元测试（TSAN=1 额外跑 ThreadSanitizer）
```

## 当前可用（阶段 1）

```bash
scripts/build-all.sh                                   # release 构建
# 开发和构建都在 M2 上。M4 只运行 netmic-sender，从 M2 拉取（只能 M4 → M2 发起 ssh）：
scp zs@192.168.1.2:/Users/zs/Dev/Claude/LANMic/sender/.build/release/netmic-sender ~/netmic/   # 在 M4 上

~/netmic/netmic-sender --list-devices                  # M4：列出输入设备
~/netmic/netmic-sender --device "<名字>" -v              # M4：在本机终端运行（麦克风权限弹窗）
~/netmic/netmic-sender --tone 440 -v                   # M4：无麦克风时用测试音
sender/.build/release/netmic-probe --discover | --record 10 --out test.wav | --stats 300   # M2
```

## 当前可用（阶段 2：驱动只输出 440 Hz 测试音，不接网络）

```bash
scripts/build-all.sh             # 构建 driver/build/NetMic.driver（ad-hoc 签名）
scripts/test-all.sh              # 含驱动冒烟测试：进程内加载 bundle，不需要 sudo
scripts/install-driver.sh        # M2：安装并重启 coreaudiod（sudo，声音会中断几秒）
scripts/uninstall-driver.sh      # M2：卸载并重启 coreaudiod
scripts/logs.sh [-v]             # 驱动日志（-v 含 libASPL 逐操作跟踪）
```

配置在 `driver/Info.plist.in` 的 `NetMicConfig` 字典里（DeviceName、SenderAddress、Port、TargetLatencyMs、Channels、Token），改完重新构建并安装。

完整的构建、安装、使用说明将在最后阶段补全。
