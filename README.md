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
sender/.build/release/netmic-sender --list-devices     # M4：列出输入设备
sender/.build/release/netmic-sender --device "<名字>" -v
sender/.build/release/netmic-sender --tone 440 -v      # 无麦克风时用测试音
scripts/deploy-m2.sh user@m2-host                      # 拷 netmic-probe 到 M2 的 ~/netmic/
~/netmic/netmic-probe --discover | --record 10 --out test.wav | --stats 300   # M2
```

完整的构建、安装、使用说明将在最后阶段补全。
