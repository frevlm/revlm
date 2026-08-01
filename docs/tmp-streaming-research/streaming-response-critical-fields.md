# 流式 Response Body 关键信息分布

## 问题

代理层需要从流式响应中提取计费相关字段（input_tokens, output_tokens, cache_*, model, service_tier）。如果使用固定大小环形缓冲区只保留最近 N 个 chunk 或最后 N 字节，能否保证所有关键信息都被读到？

结论：**不能。** OpenAI 和 Anthropic 将关键元数据分布在了流的两端。

---

## OpenAI Chat Completions Streaming (SSE)

### Chunk 结构

每个 chunk 的 JSON：

```json
{
  "id": "chatcmpl-xxx",
  "object": "chat.completion.chunk",
  "created": 1694268190,
  "model": "gpt-4o-mini",
  "system_fingerprint": "fp_xxx",
  "choices": [{
    "index": 0,
    "delta": {...},
    "finish_reason": null,
    "logprobs": null
  }],
  "usage": null
}
```

### delta vs message

- 非流式响应使用 `choices[0].message`（完整内容）
- 流式响应使用 `choices[0].delta`（增量内容）
- `message` 字段在流式 chunk 中不存在

### Chunk 序列（文本场景，`include_usage: true`）

| 序号 | delta | finish_reason | usage |
|------|-------|---------------|-------|
| 1 | `{role:"assistant", content:""}` | null | null |
| 2..N-2 | `{content:"片段文本"}` | null | null |
| N-1 | `{}` | `"stop"` | null |
| N | 不存在（choices 为空数组） | 不存在 | `{prompt_tokens, completion_tokens, total_tokens, ...}` |
| 终止 | `data: [DONE]` | | |

### 关键事实

1. **`finish_reason` 在中间 chunk 为 null，仅在倒数第 1-2 个 chunk 变非 null。**
2. **`usage` 仅在设置 `stream_options: {include_usage: true}` 时才出现。** 默认不出现。
3. **usage 仅在最后 1 个 chunk（choices 为空数组）。** 官方文档明确警告：
   > "If the stream is interrupted or cancelled, you may not receive the final usage chunk which contains the total token usage for the request."
4. **`model` 字段在每个 chunk 顶层都有**，不依赖尾部。
5. tool_calls 场景中 delta 还有 `tool_calls` 数组（含 id/function.name/function.arguments），跨多个 chunk 增量传输。

### `service_tier` 出现位置

- 最后一个 chunk 的 usage 对象中
- 可能也在中间某些 chunk 的顶层（取决于 provider 实现）

> 来源：
> - https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events
> - https://platform.openai.com/docs/guides/streaming-responses?api-mode=chat

---

## Anthropic Messages Streaming (SSE)

### 事件顺序

```
message_start → content_block_start → content_block_delta × N
→ content_block_stop → message_delta → message_stop
```

中间可能穿插 `ping` 事件。

### 各事件字段（官方 SSE 示例验证）

#### message_start（第 1 个事件）

```json
{
  "type": "message_start",
  "message": {
    "id": "msg_...",
    "type": "message",
    "role": "assistant",
    "model": "claude-opus-5",
    "content": [],
    "stop_reason": null,
    "stop_sequence": null,
    "usage": {
      "input_tokens": 25,
      "output_tokens": 1
    }
  }
}
```

**携带 `input_tokens`、`output_tokens`（初值）、`model`、空 `content`。** cache 相关字段（`cache_creation_input_tokens`, `cache_read_input_tokens`）在 web search 等场景出现。

#### content_block_start

```json
{"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}
```

#### content_block_delta

```json
{"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Hello"}}
```

**仅三个字段：`delta`、`index`、`type`。无 usage、无 stop_reason。**

delta 子类型：`text_delta`、`thinking_delta`、`input_json_delta`、`signature_delta`、`citations_delta`。

#### content_block_stop

```json
{"type": "content_block_stop", "index": 0}
```

#### message_delta

```json
{
  "type": "message_delta",
  "delta": {
    "stop_reason": "end_turn",
    "stop_sequence": null
  },
  "usage": {
    "output_tokens": 15
  }
}
```

**携带 `stop_reason`、`stop_sequence`、`usage.output_tokens`。** 是 usage 累积终值的来源。

官方文档声明：
> "The token counts shown in the `usage` field of the `message_delta` event are *cumulative*."

#### message_stop

```json
{"type": "message_stop"}
```

**仅一个 `type` 字段。零 payload。** 纯粹的流终止信号。

### usage 字段的"最后写覆盖"语义

官方 SDK 源码（`_messages.py`）的 `accumulate_event()`：

```python
# message_start — 创建初始快照
if current_snapshot is None:
    if event.type == "message_start":
        return ParsedMessage.construct(**event.message.to_dict())

# message_delta — 覆盖
elif event.type == "message_delta":
    current_snapshot.usage.output_tokens = event.usage.output_tokens   # 无条件覆盖
    if event.usage.input_tokens is not None:                           # 条件覆盖
        current_snapshot.usage.input_tokens = event.usage.input_tokens
    # ... 其他字段同样条件覆盖
```

**不是取 max，是"最后写覆盖"。** `message_delta` 有就覆盖，没有就保留 `message_start` 的值。

### 实际数据：message_delta 不一定包含 input_tokens

基础文本请求：
```
message_start:  usage: {input_tokens: 25, output_tokens: 1}
message_delta:  usage: {output_tokens: 15}               ← 只有 output_tokens
```

Web search 请求：
```
message_start:  usage: {input_tokens: 2679, output_tokens: 3}
message_delta:  usage: {input_tokens: 10682, output_tokens: 510, ...}
                ← 全量覆盖
```

**结论：`message_delta` 的字段是可选的。`input_tokens` 的最终值可能来自 `message_start`。**

> 来源：
> - https://platform.claude.com/docs/en/api/messages-streaming（Anthropic 官方 streaming 文档，含完整 SSE 示例）
> - https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/types/raw_message_start_event.py
> - https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/lib/streaming/_messages.py

---

## Revlm 需要从流式响应中提取的字段

### `AnthropicsMessages::finalize()` — 在 `handle_sse_event` 中调用

| 字段 | 来源事件 | 在流中的位置 |
|------|---------|------------|
| `input_tokens` | `message_start.usage`（初值），`message_delta.usage`（可选覆盖） | **第 1 个事件** + 最后第 2 个事件 |
| `output_tokens` | `message_delta.usage`（无条件覆盖） | 倒数第 2 个事件 |
| `cache_read_tokens` | `message_start.usage`（初值），`message_delta.usage`（可选覆盖） | 第 1 个事件 |
| `cache_creation_1h_tokens` | `message_start.usage` | 第 1 个事件 |
| `cache_creation_5m_tokens` | `message_start.usage` | 第 1 个事件 |
| `model` | `message_start.message.model` | **第 1 个事件** |
| `service_tier` | `message_start` 或 `message_delta` | 第 1 个或倒数第 2 个 |

### `OpenaiChatCompletion::finalize()` — 在 `handle_sse_event` 中调用

| 字段 | 来源事件 | 在流中的位置 |
|------|---------|------------|
| `prompt_tokens` | 最后 chunk 的 `usage` | **最后 1 个 chunk** |
| `completion_tokens` | 最后 chunk 的 `usage` | 最后 1 个 chunk |
| `cached_tokens` | 最后 chunk 的 `usage.prompt_tokens_details` | 最后 1 个 chunk |
| `cache_write_tokens` | 最后 chunk 的 `usage.prompt_tokens_details` | 最后 1 个 chunk |
| `model` | 每个 chunk 顶层 `model` | 每个 chunk（不丢） |
| `service_tier` | 最后 chunk 或中间 chunks | 尾部 |

### `handle_sse_event` 还检测

- `type == "message_stop"` — Anthropic 完成信号（倒数第 1 个事件）
- `data == "[DONE]"` — OpenAI 完成信号（最后一行）
- `type == "response.completed"` — OpenAI Responses API 完成信号

---

## 环形缓冲区的结构性缺陷

### 问题：关键数据分布在流的两端

```
Anthropic 流：
  [message_start] ← input_tokens, cache, model
  [content_block_delta × 1000] ← 纯文本增量，无任何元数据
  [content_block_delta × 1000] ← 继续纯文本...
  ...
  [message_delta] ← output_tokens (累积), stop_reason
  [message_stop]  ← 零 payload

OpenAI 流：
  [chunk 1] ← role, model
  [chunk 2..N-2] ← 纯 content 增量
  [chunk N-1] ← finish_reason
  [chunk N] ← usage
  [DONE]
```

### 丢失场景

| 场景 | Anthropic | OpenAI |
|------|-----------|--------|
| 环形缓冲保留尾部 | ❌ `input_tokens`, `model`, cache 丢失 | ✅ usage 在尾部 |
| 环形缓冲保留头部 | ✅ `input_tokens` 不丢 | ❌ `usage`, `finish_reason` 丢失 |
| 流中断/取消 | 所有未发事件丢失 | 最后 chunk 可能不到达* |

* OpenAI 官方明确警告的中断丢失场景。

### 核心结论

**任何"只保留一部分"的策略都会丢失一端的关键数据。** 两端距离 = 整个流。滑动窗口要同时覆盖头尾 = 全缓冲 = 问题没解决。

---

## new-api（one-api fork）的做法

1. **逐事件转发，不缓冲**：使用无缓冲 Go channel 做生产者-消费者背压。
2. **最后一个 chunk 被故意保留（held back）**：用于提取 usage 和 response_id 后处理。
3. **`bufio.Scanner`** 逐行读取 SSE，缓冲区上限 128MB（这是 Scanner 的 max token size，不是累积缓冲）。
4. **除最后一个外的所有 chunk 立即转发给客户端。**

---

## 正确方案：逐事件增量解析

不是保留 chunk 事后解析，而是**事件到达时立刻提取所需字段，然后丢弃原始数据**。

1. **Anthropic `message_start`** → 解析 `input_tokens`、`model`、cache。丢弃 JSON。
2. **Anthropic `message_delta`** → 解析 `output_tokens`、`stop_reason`。覆盖已有字段。丢弃 JSON。
3. **OpenAI 每个 chunk** → 检查 `usage` 是否非 null。非 null → 解析所有 token 数。丢弃 JSON。
4. **中间 `content_block_delta` / content chunk** → 纯转发，不存。

内存 = O(单个事件)，不是 O(整个流)。

> 来源：
> - DeepWiki new-api streaming docs: https://deepwiki.com/QuantumNous/new-api/2.5-streaming-and-websocket
> - one-api 源码: https://gitea.nightsoil.cn/ChatGPT/one-api/src/commit/c880b4a9/relay/channel/openai/main.go

---

## 参考资料

- [OpenAI Streaming Events Reference](https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events)
- [OpenAI Streaming Guide](https://platform.openai.com/docs/guides/streaming-responses?api-mode=chat)
- [Anthropic Messages Streaming](https://platform.claude.com/docs/en/api/messages-streaming)
- [Anthropic Python SDK — RawMessageStartEvent](https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/types/raw_message_start_event.py)
- [Anthropic Python SDK — streaming accumulation](https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/lib/streaming/_messages.py)
- [one-api OpenAI relay 源码](https://gitea.nightsoil.cn/ChatGPT/one-api/src/commit/c880b4a9/relay/channel/openai/main.go)
