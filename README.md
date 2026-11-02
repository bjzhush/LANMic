# NetMic

NetMic 在局域网里把一台 Mac 的麦克风变成另一台 Mac 的系统输入设备。

- **发送端（M4）**：插着 USB 麦克风的 Mac。运行 NetMic Sender（菜单栏 App）或命令行 `netmic-sender`。
- **接收端（M2）**：装 NetMic.driver（Core Audio 插件）。系统里会多出一个输入设备 **“M4 Mic”**，QuickTime、听写、
  会议软件等任何应用都可以直接选它。

只有 M2 上有应用在用 “M4 Mic” 时，M4 才会打开麦克风；用完 5 秒后自动关掉。
有线局域网实测端到端延迟约 30–35 ms。

开发规格见 `netmic-plan.md`，开发记录见 `PROGRESS.md`，线协议见 `docs/protocol.md`。

## 要求

- 两台 Mac 都是 macOS 13 或更新，Apple 芯片；在同一个局域网里，**建议有线**（见[网络](#网络建议有线)）。
- 在一台 Mac 上构建（本项目在 M2 上构建）：Xcode 或 Command Line Tools（`xcode-select --install`），
  以及 cmake（`brew install cmake`）。
- 驱动和 App 都是 ad-hoc 签名，只能在自己的机器上使用，不能分发。

## 从零开始安装

下面的命令都在仓库根目录执行。

### 1. 构建（M2）

```bash
git submodule update --init       # libASPL
scripts/build-all.sh              # 驱动 driver/build/NetMic.driver，命令行 sender/probe
scripts/build-app.sh              # 菜单栏 App：sender/build/NetMicSender.app
scripts/test-all.sh               # 可选：全部测试，不需要 sudo
```

### 2. 安装驱动（M2）

```bash
scripts/install-driver.sh
```

会要 sudo 密码。脚本把驱动复制到 `/Library/Audio/Plug-Ins/HAL/` 并重启 coreaudiod，**这台 Mac 的所有声音会中断几秒**。
看到 `OK: "M4 Mic" is present.` 就装好了，「系统设置 → 声音 → 输入」里会出现 “M4 Mic”。

### 3. 安装发送端（M4）

把 App 从 M2 拷到 M4（在 **M4** 上执行，把地址换成 M2 的 IP）：

```bash
scp -r zs@192.168.1.2:Dev/Claude/LANMic/sender/build/NetMicSender.app /Applications/
open /Applications/NetMicSender.app
```

第一次运行：

1. 系统会问“是否允许 NetMic Sender 访问麦克风”，选允许。
2. 第一次有 M2 来连接时，系统会问“是否允许查找本地网络上的设备”，选允许（macOS 15 起）。
3. 菜单栏出现麦克风图标。点开选择麦克风（默认是系统当前的输入设备）。
4. 想开机自动运行就打开“登录时启动”。

### 4. 使用（M2）

在任何应用的麦克风设置里选 “M4 Mic”。M4 菜单栏的图标会变成实心，状态显示“正在发送给 …”。

## 菜单栏 App

| 项目 | 说明 |
|---|---|
| 状态 | 待机 / 正在发送给某个接收端 / 常开采集中 / 错误提示 |
| 电平条 | 采集中时显示麦克风峰值电平（dBFS），用来确认麦克风有声音 |
| 麦克风 | 选择采集哪个输入设备；不会改动 M4 的系统默认输入。拔掉的设备显示为“（未连接）”，插回后自动继续 |
| 常开采集 | 不管有没有接收端都开着麦克风。M4 的麦克风冷启动要 100–300 ms，听写可能吞掉第一个字；常开就没有这个问题，代价是麦克风指示灯一直亮 |
| 登录时启动 | 用 `SMAppService` 注册为登录项；如果系统要求确认，会自动打开「登录项」设置 |

其他设置用 `defaults` 改，改完退出 App 再打开：

```bash
defaults write com.netmic.sender Name "M4 Mic"       # 发现时报给接收端的名字
defaults write com.netmic.sender GainDb -float 6     # 增益（dB）
defaults write com.netmic.sender Channel -int -1     # 多声道麦克风：0 = 第一声道（默认），-1 = 各声道平均
defaults write com.netmic.sender Token 00112233445566778899aabbccddeeff   # 与驱动的 Token 一致才响应
defaults write com.netmic.sender StopGrace -float 60 # 最后一个接收端离开后多少秒关麦克风（默认 5）
```

App 的日志：`log stream --predicate 'subsystem == "com.netmic.sender"'`。

## 命令行发送端（可选）

不想用 App 时，M4 上也可以在终端里运行命令行版（麦克风权限会归到「终端」）：

```bash
scp zs@192.168.1.2:Dev/Claude/LANMic/sender/.build/release/netmic-sender ~/netmic/   # 在 M4 上
~/netmic/netmic-sender --list-devices
~/netmic/netmic-sender --device "USB MIC" -v         # 名字的一部分或 UID
~/netmic/netmic-sender --tone 440 -v                 # 不用麦克风，发测试音
~/netmic/netmic-sender --help                        # 其他选项：--always-on、--gain-db、--token ……
```

App 和命令行版都监听 UDP 47810，**同一时间只能运行一个**。

## 驱动配置（M2）

配置在驱动 bundle 的 Info.plist 里（`driver/Info.plist.in` 的 `NetMicConfig`）。插件只能读自己 bundle 里的文件，
所以改配置要重新安装。最方便的是安装时用 `--set` 覆盖：

```bash
scripts/install-driver.sh --set TargetLatencyMs=40 --set SenderAddress=192.168.1.4
```

| 键 | 默认 | 说明 |
|---|---|---|
| `DeviceName` | M4 Mic | 系统里显示的设备名 |
| `SenderAddress` | 空 | 空 = 广播自动发现；填 IP = 直连这台发送端（广播被路由器拦掉时用） |
| `Port` | 47810 | 发送端端口 |
| `TargetLatencyMs` | 20 | 抗抖动缓冲（5–200 ms）。越大越不容易断，延迟也越大 |
| `Channels` | 1 | 设为 2 时把单声道复制成立体声（个别只认立体声的应用） |
| `Token` | 空 | 32 位十六进制；不为空时只和 Token 相同的发送端通信 |

## 网络：建议有线

有线局域网下，默认 20 ms 缓冲在一小时的测试里只有 1 次欠载（约 10 ms 静音）。Wi-Fi 的包会成批到达，还会丢包，
同样的缓冲会断断续续。

- 驱动通过广播在所有网卡上找发送端：有线断开而 Wi-Fi 还在时，会自动改走 Wi-Fi；当前路径 5 s 收不到音频时会重新广播，
  这时有线通了就会回到有线（实测有线恢复后约 30 s 切回）。
- 一定要用 Wi-Fi 时，把缓冲加大，延迟会相应增加约 40 ms：

  ```bash
  scripts/install-driver.sh --set TargetLatencyMs=60
  ```

  缓冲只能吸收到达时间的抖动，Wi-Fi 上真正丢掉的包仍然会变成短暂的静音。

## 常见问题

**“M4 Mic” 有了，但录到的是静音。**
1. M4 上 App 在运行吗？菜单里的状态是不是“正在发送给 …”？还在“待机”说明 M2 没连上，往下看。
2. 在 M2 上看驱动日志：`scripts/logs.sh`。
   - 一直是 `discovering` 没有 `found sender`：M4 没有回应。先看 M4「系统设置 → 隐私与安全性 → 本地网络」里
     NetMic Sender 是否打开（被拒时 App 发不出任何包）；再检查两台在同一网段，或者直接指定地址
     `scripts/install-driver.sh --set SenderAddress=<M4 的 IP>`。
   - 反复出现 `found sender` 和 `silent for 5 s`：M4 回应了但没有音频，一般是 M4 的麦克风被别的程序占用
     或者没插好，看 App 的状态和电平条。
3. App 状态是“正在发送”但电平条不动：麦克风选错了，或者 M4 上麦克风权限被拒（菜单里会有红字和打开设置的按钮）。

**声音偶尔有很短的断续。**
`scripts/logs.sh` 里的 `buffer: ... underrun(s)` 每出现一次就是一次断续。偶尔一两次是网络瞬间卡顿；频繁出现时把
`TargetLatencyMs` 调大（30、40……），用 Wi-Fi 的话见上一节。每 10 秒一行的统计里 `lost` 是丢包数，`depth` 应该接近目标值。

**听写吞掉第一个字。**
打开菜单里的“常开采集”。或者把 `StopGrace` 调大（例如 300 秒），说完话几分钟内再用就不需要重新打开麦克风。

**重新构建 App 后又问麦克风权限。**
ad-hoc 签名每次构建都不同，系统把它当成新 App，允许一次即可。旧的条目可以在「隐私与安全性 → 麦克风」里删掉。

**App 显示“无法监听 UDP 47810”。**
命令行版 `netmic-sender` 还开着（或者开了两个 App）。关掉其中一个后，退出 App 再打开。

**“登录时启动”打不开。**
App 需要放在「应用程序」文件夹里。打开后如果系统提示需要批准，到「系统设置 → 通用 → 登录项」里允许 NetMic Sender。

**M2 的声音全部没了 / coreaudiod 出问题。**
先卸载驱动（下一节），声音会恢复。日志在 `scripts/logs.sh -v`，崩溃报告在「控制台 → 崩溃报告」里找 `coreaudiod`
或 `Core-Audio-Driver-Service`。

## 卸载

M2：

```bash
scripts/uninstall-driver.sh       # 删除驱动并重启 coreaudiod（sudo），"M4 Mic" 消失
```

M4：

1. 菜单里关掉“登录时启动”，点“退出”。
2. 把 `/Applications/NetMicSender.app` 拖到废纸篓。
3. 可选：`defaults delete com.netmic.sender` 删掉设置；「隐私与安全性」的“麦克风”“本地网络”里删掉 NetMic Sender。
4. 用过命令行版的话删掉 `~/netmic/`。

## 开发

```bash
scripts/test-all.sh               # Swift + C++ 单元测试 + 本机端到端（TSAN=1 再跑 ThreadSanitizer）
scripts/logs.sh [-v]              # 驱动日志（-v 含 libASPL 逐操作跟踪）
sender/.build/release/netmic-probe --discover | --record 10 --out test.wav | --stats 300 | --trace t.csv
driver/build/netmic_replay t.csv  # 用真实到达时间回放抖动缓冲，选缓冲目标
driver/build/netmic_find_gaps rec.wav   # 找录音里驱动没有数据的空洞（afconvert 先转成 16 位 WAV）
```

目录：`shared/`（协议常量和测试向量）、`sender/`（Swift：协议、发送端核心、命令行、菜单栏 App、probe）、
`driver/`（C++：libASPL 插件、抖动缓冲、网络接收、测试和工具）、`scripts/`、`docs/`。
