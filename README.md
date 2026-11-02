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

## 当前可用（阶段 3：驱动通过网络接收 M4 的声音）

```bash
scripts/build-all.sh             # 构建 driver/build/NetMic.driver（ad-hoc 签名）
scripts/test-all.sh              # 含端到端测试：本机 sender --tone → 进程内加载的驱动，不需要 sudo
scripts/install-driver.sh        # M2：安装并重启 coreaudiod（sudo，声音会中断几秒）
scripts/install-driver.sh --set SenderAddress=192.168.1.4   # 安装时临时改配置（这里是不用广播、直连 M4）
scripts/uninstall-driver.sh      # M2：卸载并重启 coreaudiod
scripts/logs.sh [-v]             # 驱动日志（-v 含 libASPL 逐操作跟踪）
```

配置在 `driver/Info.plist.in` 的 `NetMicConfig` 字典里（DeviceName、SenderAddress、Port、TargetLatencyMs、Channels、Token），改完重新构建并安装；或者安装时用 `--set 键=值` 临时覆盖。

### 网络：建议有线

两台 Mac 之间建议用有线网络。实测有线局域网下 20 ms 缓冲（默认 `TargetLatencyMs=20`）一小时只有 1 次欠载；
Wi-Fi 的包会成批到达并且会丢包，同样的缓冲会断断续续。

- 驱动通过广播在所有网卡上找发送端：有线断开而 Wi-Fi 还在时，会自动改走 Wi-Fi；当前路径 5 s 收不到音频时会重新广播，这时有线通了就会回到有线
  （实测有线恢复后约 30 s 切回）。
- 一定要用 Wi-Fi 时，把缓冲加大，延迟会相应增加约 40 ms：

  ```bash
  scripts/install-driver.sh --set TargetLatencyMs=60
  ```

  缓冲只能吸收到达时间的抖动，Wi-Fi 上真正丢掉的包仍然会变成短暂的静音。

完整的构建、安装、使用说明将在最后阶段补全。
