# NetMic 线协议 v1

实现：`shared/netmic_protocol.h`（C，driver 使用）与 `sender/Sources/NetMicProtocol`（Swift）。
两者都用 `shared/test_vectors.txt`（由 `scripts/gen-test-vectors.py` 独立生成）做字节级一致性测试。

## 传输

- UDP / IPv4。发送端（M4）监听 **47810**（可配置）。
- 接收端使用临时端口；发送端把 ANNOUNCE / AUDIO 回发到请求包的源地址与端口。
- 所有多字节字段 **小端**。

## 包头（24 字节）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | u32 | magic | `0x43494D4E`，线上字节为 ASCII `NMIC` |
| 4 | u8 | version | `1` |
| 5 | u8 | type | 见下表 |
| 6 | u16 | payloadBytes | 包头之后的负载长度，必须等于数据报长度 − 24 |
| 8 | u32 | seq | 每种类型各自递增 |
| 12 | u32 | streamId | 发送端每次（重新）开始采集时随机生成；接收端发现变化时重置缓冲 |
| 16 | u64 | samplePos | AUDIO：本包第一个采样帧在该 stream 内的序号；其他类型为 0 |

## 包类型

| type | 名称 | 方向 | 负载 |
|---|---|---|---|
| 1 | DISCOVER | 接收端 → 广播 `255.255.255.255:47810` 或配置的单播地址 | `token[16]` |
| 2 | ANNOUNCE | 发送端 → 接收端（单播） | `sampleRate u32, channels u8, format u8 (1=Int16), framesPerPacket u16, nameLen u8, name[nameLen]` (UTF-8) |
| 3 | SUBSCRIBE | 接收端 → 发送端 | `token[16]`；IO 运行期间每 500 ms 一次，兼作心跳 |
| 4 | UNSUBSCRIBE | 接收端 → 发送端 | `token[16]` |
| 5 | AUDIO | 发送端 → 订阅者 | Int16 LE PCM，单声道，240 帧（5 ms @ 48 kHz）= 480 字节 |

## 校验规则（两端实现一致，按此顺序）

| 条件 | 错误 |
|---|---|
| 数据报 < 24 字节 | `short` |
| magic 不符 | `magic` |
| version ≠ 1 | `version` |
| 剩余字节 < payloadBytes | `short` |
| 剩余字节 > payloadBytes | `length` |
| type 不在 1–5 | `type` |
| DISCOVER / SUBSCRIBE / UNSUBSCRIBE 负载 ≠ 16 字节 | `length` |
| ANNOUNCE 负载 < 9 或 ≠ 9 + nameLen | `length` |
| AUDIO 负载为 0 或奇数字节 | `length` |

接收端对 AUDIO 不强制 240 帧，任何偶数字节长度都接受（单个数据报 ≤ 1500 字节）。

## 语义

- **token**：16 字节预共享密钥，全 0 表示不校验。发送端只接受 token 匹配的 DISCOVER / SUBSCRIBE / UNSUBSCRIBE。
- **来源限制**：发送端默认只接受私有地址段：10/8、172.16/12、192.168/16、169.254/16、127/8。
- **订阅过期**：发送端 3 秒内未收到某订阅者的 SUBSCRIBE 即移除该订阅者。
- **发现**：接收端每 1 秒发一次 DISCOVER，收到 ANNOUNCE 后锁定发送端；5 秒收不到发送端任何包则回到发现状态。
- **码率**：768 kbps 音频 + 包头 ≈ 800 kbps，200 包/秒。
