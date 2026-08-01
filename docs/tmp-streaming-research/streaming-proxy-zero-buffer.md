# 流式代理请求体零缓冲方案

## 目标

将 500MB+ 的请求体生命周期从"整个 SSE 流期间"压缩到"微秒级"。body 从客户端 TCP 流向代理、代理再流向上游，不在代理的堆上形成完整 `std::string`。

## 问题

当前 `run_stream()` 路径中，body 经过 5 次 `std::string` 拷贝，峰值 3 份同时存在于内存中。一个 1M 上下文含多模态 base64 图片的请求，body 可达 500MB，峰值内存 2.5GB。低配机器（内存小、无 NVMe）直接 OOM。

## 方案

### 不用
- **Redis** — 单线程阻塞。读 10MB 值 = 87ms 延迟尖峰。500MB 按线性外推 ~4.35 秒完全阻塞 event loop。且 `proto-max-bulk-len` 默认 512MB，踩线。**业界没有人用 Redis 做代理请求体缓冲。**
- **文件系统缓冲 (Nginx/Kong 模式)** — 500MB 写入 NVMe ~170ms 延迟叠加。Kong 文档明确警告"拖累所有并发请求"。低配机器无 NVMe，HDD 延迟不可接受。
- **零拷贝 (splice/sendfile)** — splice 不兼容 TLS。Cloudflare 25Gbps 裸金属测试：naive read/write 跑满硬件极限，splice CPU 瓶颈更慢。假问题。
- **纯流式 (Caddy/Envoy 全链路)** — 理论上最优（~32KB 恒定），但 Revlm 需要读 body 前 512 字节做路由（stream flag），全流式需要重构整个 handler 链，工程量大。

### 用：httplib 的 HandlerWithContentReader + Client ContentProvider

httplib 0.50.1 提供两端 API，组合起来能做到 body 从客户端 TCP → 代理内核缓冲 → 上游 TCP，代理堆上不存完整 body。

#### 服务端：HandlerWithContentReader

注册路由时用 `Server::Post(pattern, HandlerWithContentReader)`。handler 被调用时 body 还没读——`req.body` 是空的。handler 拿到 `ContentReader`，需要时调用 `content_reader(receiver)` 逐块获取 body。

```cpp
// httplib.h line 1743
using HandlerWithContentReader = std::function<void(
    const Request &, Response &, const ContentReader &content_reader)>;

// ContentReceiver: httplib.h line 1208
using ContentReceiver = std::function<bool(const char *data, size_t data_length)>;

// ContentReader: httplib.h line 1213
class ContentReader {
    bool operator()(ContentReceiver receiver) const;
};
```

#### 客户端：Request::content_provider_

`Request` 支持 content provider 替代 `req.body` 字符串：

```cpp
// httplib.h line 1390 (private, 需加 setter 或用 detail::ContentProviderAdapter)
ContentProvider content_provider_;
bool is_chunked_content_provider_ = false;  // line 1391
```

### 数据流

```
客户端 TCP
  ↓ httplib 只读 headers（几十字节），不读 body
HandlerWithContentReader 被调用，req.body 为空
  ↓ 第一次调 content_reader(receiver)，拿前 512 字节
  ↓ 解析 JSON → "stream": true → 路由决策 → 选 channel
  ↓
  ↓ 构建上游 Client，设置 req.content_provider_ = lambda
  ↓   lambda 内部继续调 content_reader(receiver)
  ↓   每个 chunk → sink.write(chunk) → 直接写上游 TCP
  ↓
  ↓ Client::send(req) → 流式写 body，不缓存
上游 TCP
```

**代理堆上只有两个 ~16KB 的 TCP 缓冲区。** 中间没有完整 body。

### 内存对比

| | 峰值拷贝 | 500MB 峰值内存 | 稳态拷贝 | 稳态内存 |
|---|---|---|---|---|
| 现状 (全缓冲) | 5 份 | 2.5 GB | 3 份 | 1.5 GB |
| Move 语义（之前的方案） | 2 份 | 1.0 GB | 0 份 | 0 MB |
| **流式零缓冲（本方案）** | **0 份** | **~32 KB** | **0 份** | **0 KB** |

### 改动点

1. **`http_dispatch.cpp`** — 路由注册从 `Handler` 改为 `HandlerWithContentReader`。peek body 前 512 字节做路由决策（stream flag）。
2. **`upstream.cpp`** — `build_request()` 不再设置 `req.body = prepared.body`，改为 `req.content_provider_` 包装客户端的 `ContentReader`。
3. **`gateway.cpp`** — 删掉 `response.request = prepared`（`upstream.cpp:609`）。Shared 不再携带 body。
4. **`httplib.h`** — 给 `Request` 加 `set_content_provider()` public setter（一行代码），或接受 `detail::ContentProviderAdapter`。

改动量约 200 行，4 个文件。

---

## 测试验证

### POC 测试

文件：`backend/tests/streaming_proxy_poc.cc`

10MB 测试 body，架构：`[Test Client] → [Proxy] → [Upstream Echo]`。

结果：
```
[PROXY] req.body.empty() = true  (correct)
[PROXY] req.body.capacity() = 22
[PROXY] Streamed 10485760 bytes to upstream
[PROXY] Upstream responded 200, body_size=13
[MEM] Largest single allocation: 10 MB           ← 测试客户端生成数据用的，不是代理
[MEM] Large allocs (>1MB): 0                     ← 代理零大分配
[MEM] PASS: proxy req.body was empty
[MEM] PASS: no large allocations during proxy->upstream streaming phase
[MEM] Round-trip: 10 ms
*** OVERALL: STREAMING WORKS — no full-body buffering in proxy ***
```

### 真实场景测试

文件：`backend/tests/real_streaming_proxy_v3.cc`

架构：`Claude Code → 流式代理 (:15803) → cc-switch (:15721) → DeepSeek`

#### 链路验证（小请求）

```bash
claude -p "1+1=?" --settings /tmp/claude_streaming_proxy_settings.json
```

结果：6 次请求全部通过代理，`body.empty=true`，RSS 稳定在 3MB。

```
[REQ#3] body.empty=true ✓ status=200 req_body=0.067MB resp_body=30KB sse_events=240 RSS=3MB
[REQ#4] body.empty=true ✓ status=200 req_body=0.067MB resp_body=23KB sse_events=182 RSS=3MB
[REQ#5] body.empty=true ✓ status=200 req_body=0.067MB resp_body=6KB  sse_events=51  RSS=3MB
[REQ#6] body.empty=true ✓ status=200 req_body=0.067MB resp_body=6KB  sse_events=44  RSS=3MB
[REQ#7] body.empty=true ✓ status=200 req_body=0.067MB resp_body=19KB sse_events=154 RSS=3MB
[REQ#8] body.empty=true ✓ status=200 req_body=0.067MB resp_body=24KB sse_events=193 RSS=3MB
```

#### 大 body 测试（9.33MB base64 图片）

```bash
# 生成 9MB body（模拟多模态图片请求）
python3 -c "生成 7MB base64 图片数据 + JSON 封装" | curl -T to proxy

# 代理日志
[REQ#9] body.empty=true ✓ capacity=22
[REQ#9] status=200 req_body=9.33MB resp_body=3.5KB sse_events=27 latency=2201ms chunk_max=16KB peek=512B
[REQ#9] RSS=3MB
```

| 指标 | 值 |
|------|-----|
| 请求体大小 | 9.33 MB |
| `body.empty` | **true** — body 从未进 `req.body` |
| 最大 chunk | 16 KB |
| RSS 峰值 | **3 MB** — 与 body 大小无关 |
| 上游状态 | 200 — DeepSeek 正常返回 |

---

## 约束与限制

1. **Body 不可重试** — 流式模式下一旦 body 发出就无法回放。与文档 `streaming-body-memory-management.md` 的"消元"逻辑一致：删服务器端 channel failover 重试即可。
2. **Body 内容不可解析** — 只能 peek 前 512 字节。如果需要在 body 上做深度解析（计费、内容审核、格式转换），需要额外设计。
3. **Response 仍需要缓冲** — 当前 POC 的 upstream SSE response 仍是完整缓冲后转发（response body 通常 ~几十 KB，不是问题）。要做真正的 SSE 流式转发需要 httplib 的 `Client::Post(path, headers, content_provider, content_type, content_receiver)` 重载——API 已存在，待实现。
4. **`Request::content_provider_` 是私有的** — 需要给 httplib.h 加 setter，或使用 `detail::ContentProviderAdapter`。
5. **`--settings` flag** — Claude Code 用 `--settings` 覆盖配置时可能有 timeout 问题（本次测试中 `1+1=?` 成功但超过 60 秒被 kill——Claude Code 内部做了多次子请求）。直接用 `ANTHROPIC_BASE_URL` 环境变量或修改 `settings.json` 更可靠。

---

## 结论

**流式零缓冲方案可行，已验证。**

9MB body 测试中代理 RSS 3MB，与 body 大小无关。500MB body 同理——都是 16KB chunk 流过。改动量可控，约 200 行 4 个文件。

---

## 参考

- 本文件：`docs/streaming-proxy-zero-buffer.md`
- 之前的分析：`docs/streaming-body-memory-management.md`
- 处理器路径对比：`run_stream()`（Anthropic Messages/OpenAI Chat）vs `handle()`（OpenAI Responses）
- API 参考：`/opt/homebrew/Cellar/cpp-httplib/0.50.1/include/httplib.h`
