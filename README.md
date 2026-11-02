# NetMic

在局域网内把一台 Mac（M4）的麦克风变成另一台 Mac（M2）上的系统级输入设备 “M4 Mic”。

开发规格见 `netmic-plan.md`，进度见 `PROGRESS.md`，线协议见 `docs/protocol.md`。

## 开发

```bash
brew install cmake          # 构建驱动需要
scripts/test-all.sh         # Swift + C++ 单元测试（TSAN=1 额外跑 ThreadSanitizer）
```

完整的构建、安装、使用说明将在最后阶段补全。
