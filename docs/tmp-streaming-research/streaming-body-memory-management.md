# 流式代理请求体内存管理

## 问题

AI API 代理在流式（SSE）模式下，客户端发来的请求体（body）通常在 1KB～1MB+（含图片等多模态输入）。body 是完整缓存在内存中还是流式转发？如果是缓存，它在整个 SSE 流生命周期中存活多久？

## Deep Research 结论

### 业界存在两种截然不同的策略

**策略 A：全缓冲（以 aiproxy 为代表）**

> source: https://github.com/labring/aiproxy (core/common/body.go, core/controller/relay-controller.go)

aiproxy 在请求到达时将完整 body 读入 `[]byte` 缓冲区，用 `reusableRequestBody` 包装替换 `req.Body`——该包装的 `Close()` 是空方法，**从不释放底层内存**。全量代码搜索 `ReleaseBody`/`ClearBody`/`FreeBody`/`ResetBody`/`DropBody` 结果为零。

这样做的核心驱动力是**服务器端重试**：同一个 body 需要被多次消费（每个 channel 尝试一次）。但代价是 body 在整个 SSE 流生命周期内不释放。

同时 aiproxy 的 stream fake 插件在响应侧也将所有 SSE chunk 累积到内存中（`bytes.Buffer`），构成**双端全缓冲**架构。

**策略 B：流式传输（以 Caddy、Envoy 为代表）**

> source: https://github.com/caddyserver/caddy/pull/1314
> source: https://www.envoyproxy.io/docs/envoy/v1.21.2/faq/configuration/flow_control

Caddy PR #1314 的核心陈述：

> "If only one upstream is defined we don't need to buffer the body. We can directly stream to the upstream host, which reduces memory usage as well as latency."

实测数据：100MB body 流式分配 ~582KB vs 缓冲分配 ~269MB，**内存节省约 460 倍**。

Envoy 官方文档明确声明：在所有 L7 filter 均为流式的路由上「将以流式方式代理任意大小的 body」。

**关键约束：缓冲是不可逆的单向门**

> source: https://github.com/caddyserver/caddy/pull/1314

> "You can't decide to buffer the body *after* you've already consumed it by proxying it to the first upstream host."

一旦数据流式发送到第一个上游，就无法事后恢复未保存的字节用于重试。IETF HTTP 工作组在 2026 年仍在讨论该问题的标准化——Ben Schwartz 承认 "unbounded buffering in the proxy would be dangerous."

**mitmproxy 的洞察：事件钩子决定缓冲行为**

> source: https://docs.mitmproxy.org/stable/api/events.html

在 `response` 事件中处理意味着整个响应体已被缓冲，客户端在此之前不会收到任何字节。流式必须在 `responseheaders` 事件中启用 `flow.response.stream = True`——因为 `responseheaders` 触发时 body 为空，而 `response` 触发时缓冲已完成。

### LiteLLM 的流式 chunk 反模式

> source: https://github.com/BerriAI/litellm/pull/22346

LiteLLM 曾对每个流式 chunk 无条件调用 `model_copy()` 创建完整副本后再追加到列表，导致大量不必要的内存分配。优化后仅对包含 usage 数据的最终 chunk 执行复制，非 usage chunk 直接追加（零复制开销）。

### 总结

| 策略 | 代表项目 | body 存活时间 | 内存占用 | 是否支持重试 |
|------|---------|-------------|---------|------------|
| 全缓冲 | aiproxy | 整个 SSE 流生命周期 | O(body_size) | ✅ |
| 流式传输 | Caddy, Envoy | 发完即释放 | ~32KB 缓冲区 | ❌（但客户端可重试） |

---

## 对 Revlm 的影响

### 当前问题

Revlm 的流式路径中，body 存在多份副本。以 `Gateway::run_stream()` 为例：

1. `ProxyRequest::http.body` —— handler 入口拷贝，`run_stream()` 内 clear 释放
2. `httplib::Request::body` —— worker thread lambda 捕获，存活到流结束（worker join）
3. `Shared::upstream.request.body`（`UpstreamPreparedRequest` 内）——存活到 chunked provider 完成

其中副本 #2 和 #3 都是完整 body，活到流结束。#3 在成功路径上从未被访问（泵送过程只用 `stream.read`/`close`/`poll_fd`），是数据结构绑定导致的死数据。

### 好品味的方向

**1. 删除服务器端重试**

客户端失败后自然会重发请求，服务器端不需要替客户端做重试。删除 `Gateway::handle()` 和 `Gateway::run_stream()` 中的 do-while channel failover 循环后，body 不再需要为"下一次尝试"而保留。`Gateway::handle()` 从 200+ 行缩到 ~50 行。

**2. 拆开 `UpstreamStreamResponse`**

```cpp
// Before: 两个生命周期绑在一个 struct 里
struct UpstreamStreamResponse {
    UpstreamPreparedRequest request; // 请求阶段，发完即死
    int status_code;
    vector<UpstreamHeader> headers;
    string initial_body;
    UpstreamReadHandle stream;       // 流阶段，需要长活
};

// After: 分离生命周期
struct UpstreamStreamSession {
    int status_code;
    string content_type;
    string initial_body;
    UpstreamReadHandle stream;       // 只有流相关的东西
};
```

body 在函数栈上，`make_upstream` 之后不再被任何人引用，栈展开自动释放。不需要手动 `clear()`。

**3. Shared 中不保留 body**

chunked provider 的 `shared_ptr<Shared>` 只携带 `UpstreamStreamSession` + `ProxyRequest`（计费用）+ `Gateway`。body 不在其中。

### 不是优化，是消元

这一切不是在优化 body 的释放时机，而是**消除需要持有 body 的原因**。去掉服务器端重试 → body 不用保留给重试 → 数据结构自然不需要携带 body → body 自动提前释放。没有打补丁，没有手动 `clear()`，是数据结构本身保证的正确性。

---

## 参考资料

- [labring/aiproxy](https://github.com/labring/aiproxy) — 全缓冲策略的完整实现（Go）
- [Caddy PR #1314](https://github.com/caddyserver/caddy/pull/1314) — 流式代理设计原则（Go）
- [Envoy Flow Control FAQ](https://www.envoyproxy.io/docs/envoy/v1.21.2/faq/configuration/flow_control) — L7 流式过滤器架构
- [mitmproxy Events API](https://docs.mitmproxy.org/stable/api/events.html) — 事件生命周期与缓冲的关系
- [LiteLLM PR #22346](https://github.com/BerriAI/litellm/pull/22346) — 流式 chunk model_copy 优化
