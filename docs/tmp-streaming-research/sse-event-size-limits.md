# SSE 单行/单事件尺寸上限

> 范围：`backend/src/proxy/gateway.cpp` 的 `SseReader`，影响全部流式代理链路（messages、chat/completions、responses）

## 动机

`SseReader` 有两个硬上限，超过即判上游错误、中断流：

```cpp
constexpr size_t kMaxSseLineBytes  = 64 * 1024;    // 单行
constexpr size_t kMaxSseEventBytes = 256 * 1024;   // 单事件
```

实测发现 **64 KiB 的单行上限余量只剩 1.6 倍**，正在逼近真实故障。

## 实测数据

用探针（`backend/tests/sse_size_probe.cc`）串在 Claude Code 与真实 Anthropic
上游之间，复刻 `SseReader` 的行/事件切分口径，共 6 发请求。

### signature_delta 随 thinking 量单调增长

| thinking budget | 最大单行 | 64 KiB 余量 |
|---|---|---|
| 3,000 | 492 B | 133× |
| 3,000（另一发） | 1,568 B | 41.8× |
| 30,000 | 17,136 B | 3.8× |
| 20,000（长推理） | **41,796 B (40.8 KiB)** | **1.6×** |

### 其他事件类型都极小

| 事件类型 | 最大字节 | 出现次数 |
|---|---|---|
| `signature_delta` | **41,824** | ×4 |
| `message_start` | 399 | ×7 |
| `message_delta` | 270 | ×6 |
| `thinking_delta` | 175 | ×6,838 |
| `text_delta` | 150 | ×7,905 |
| `content_block_stop` | 72 | ×10 |

风险完全集中在 `signature_delta`：它是一整个 base64 加密签名，作为**单个不可
分割的 `data:` 行**发送，长度与 thinking 内容总量正相关，且由 Anthropic 侧格式
决定，调用方无法预测上界。`thinking_delta` 是小片段（≤175 B），不是风险点。

真正的问题是 `kMaxSseLineBytes`，不是 `kMaxSseEventBytes`——单事件 ≈ 单行 + 28 B。

## 故障后果

超限时 `SseReader::consume` 置 `ok_ = false` 并返回 false，上层置
`upstream_error = true`：

- 客户端流在中段被切断，已发出的内容无法回退
- `message_delta` 永远到不了 → `finalize()` 不被调用 → **该请求计费丢失**

## 隐藏放大

`line_` 是 `std::string`，逐字节 `push_back`，超限检查在 push_back **之后**，
最后一次倍增扩容已经发生。libc++ 实测容量序列：

```
… → 32831 → 65663 → 131327
```

40.8 KiB 的行实际占 65,663 B 容量；卡在 64 KiB 边界触发检查时容量已涨到
131,327 B——**标称上限 64 KiB，实际内存 128 KiB**。

## 结论：单行上限应提到 1 MiB（待实施）

```cpp
// 现状（backend/src/proxy/gateway.cpp:429）
constexpr size_t kMaxSseLineBytes  = 64 * 1024;
constexpr size_t kMaxSseEventBytes = 256 * 1024;

// 建议
constexpr size_t kMaxSseLineBytes  = 1024 * 1024;   // 1 MiB
constexpr size_t kMaxSseEventBytes = 2048 * 1024;   // 2 MiB
```

依据：实测最大 40.8 KiB，1 MiB 给出 25× 余量，覆盖更大 thinking budget 的增长
空间；同时仍是常数上界，不构成无界内存暴露。单事件上限保持为单行的 2 倍。

> 本文档仅记录实测结论与建议值，代码尚未修改。

### 为什么不需要流式 JSON 解析器

`signature_delta` 的结构里**没有 `usage` 字段**：

```json
{"type":"content_block_delta","index":0,
 "delta":{"type":"signature_delta","signature":"<40KB base64>"}}
```

`contains_usage_object` 对它返回 false，所以它从不需要被 `json::parse`，只需
原样转发。带 `usage` 的事件（`message_start` / `message_delta`）实测 ≤399 B。

因此这里不需要把三个 `finalize()` 重写为 SAX 状态机。若将来要进一步收紧，
正确方向是在 `SseReader` 层区分「需解析事件」与「仅转发事件」：前者上限可收到
4 KiB，后者边读边写不进 `line_` 缓冲。这比 SAX 方案更便宜且效果更好——SAX
仍需缓冲整行才能判断事件类型。

## 复现方式

```bash
cd backend/tests
c++ -std=c++20 -O2 -I/opt/homebrew/include sse_size_probe.cc -lpthread -o sse_size_probe
./sse_size_probe        # 监听 :15805，转发到 cc-switch :15721
```

探针只统计字节数与事件类型，不落盘任何消息内容。用 `thinking.budget_tokens`
越大的请求施压，`signature_delta` 越大。
