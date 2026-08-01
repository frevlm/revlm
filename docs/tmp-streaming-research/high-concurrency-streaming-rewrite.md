# 高并发流式代理改造方案

> 目标负载：单实例 10000+ 并发，全流式 SSE，两类流量混合
> 相关实测：[上游上传行为](./upstream-upload-behavior.md)、[SSE 尺寸上限](./sse-event-size-limits.md)
> 状态：**方案确定，代码未修改**

## 目标负载

| | A 类：沉浸式翻译 | B 类：AI Agent |
|---|---|---|
| body | 1~5 KB | 随对话轮次单调增长，可达数百 MB |
| 数量 | 极多，突发（一页数十~数百并发） | 较少但每条昂贵 |
| 流时长 | 1~3 秒 | 数十秒~数分钟 |
| 瓶颈 | 并发连接数、每请求固定开销 | body 生命周期、带宽 |

两类共用同一端口，合计 10000+ 并发，基本全走 SSE。

**关键性质：B 类的 body 体积是连续增长的**——同一会话每轮重发全部上下文，
从几 KB 长到几百 MB。**任何按体积分类的机制（分连接池、分限流通道）都会在
会话中途抖动，且规模化后 agent 流量会全部落到「大」那一侧。**

## 一、现有问题（按 10000 并发下的撞墙顺序）

### 1. 计费行锁：单用户吞吐与并发数无关

`backend/src/users/users.cpp:177`

```sql
SELECT balance_usd FROM users WHERE id=? FOR UPDATE
```

沉浸式翻译单用户瞬发数百请求，**全部串行在同一行锁上**。单用户吞吐
≈ 1/锁持有时间，HTTP 层优化到极致也无效。

`FOR UPDATE` 保证的是「绝不透支一分钱」，而单次翻译请求成本约万分之一美元——
**用独占行锁串行化换取业务不关心的精度。**

### 2. 每请求 4~5 次阻塞 MySQL，连接池 64

| 调用点 | 查询内容 | 数据变更频率 |
|---|---|---|
| `tokens.cpp:223` | token → user/channel_group（JOIN） | 小时级 |
| `users.cpp:156` | 余额 `find<User>` | 每请求变 |
| `channel_groups.cpp:105` | 渠道组 + `fill_channels` | 小时级 |
| `users.cpp:177` | 扣费（带行锁） | 每请求变 |
| Request::commit | 用量 INSERT | 每请求写 |

**前三项查的都是配置数据，却每请求查一遍。**`config.hpp:22` 池上限 64，
10000 并发要 40000+ 次往返抢 64 条连接。

### 3. HTTP 层并发上限 = 32（8 核机器）

cpp-httplib 0.50.1 **不是** thread-per-connection，是有界线程池：

```cpp
// httplib.h:161-169
#define CPPHTTPLIB_THREAD_POOL_COUNT ((std::max)(8u, hardware_concurrency() - 1))
#define CPPHTTPLIB_THREAD_POOL_MAX_COUNT (CPPHTTPLIB_THREAD_POOL_COUNT * 4)
```

一条 SSE 流从 `set_chunked_content_provider` 进入到 `sink.done()` 退出，
**独占一个池线程数分钟**。8 核 → 上限 32 条并发流。

第 33 个请求起进入 `jobs_` 队列，而 `max_queued_requests_` 默认 0 = 无限
（`httplib.h:10371`）——**客户端收不到任何错误，静默挂起。**

该上限是按「请求数十毫秒结束」标定的，附带
`CPPHTTPLIB_THREAD_POOL_IDLE_TIMEOUT 3`（动态线程空闲 3 秒退出）——
**库的设计意图不支持线程被长期占用。** 调大线程池是对抗设计意图：
4000 线程 × 8 MB 栈 = 32 GB 虚存，且常见容器 `pids.max` 为 4096。

### 4. 每请求重建上游连接

`upstream.cpp:396` `make_client()` 每请求 `make_unique<httplib::Client>`，
用完销毁。每次付一次完整 TLS 握手（跨区域 100~300 ms）。

沉浸式翻译每页数十请求，每个都重建连接。10000 并发则需 10000 条上游连接，
上游按 key 限连接数会直接拒绝。

### 5. 流式桥接层：无背压、每流一裸线程、空转唤醒

`upstream.cpp:458-515` 的 `StreamBridgeState` 把 httplib 的 push 回调转成
pull 接口，代价：

- **每条流额外一个裸 `std::thread`**（不在池内，无上限）。32 条流 = 32 池线程 + 32 裸线程
- `chunks` 是 `std::deque<std::string>`，**无容量上限**。上游快客户端慢时无界增长
- `:510` 是 `cv.wait_for(lock, 100ms)` **无条件超时**，非 predicate 等待。
  每流每 100 ms 空转唤醒一次

每个 SSE 字节被拷贝 4 次：`chunks.emplace_back` → `memcpy` 到 `buffer[8192]`
→ `SseReader::line_` 逐字节 `push_back` → `pending_send.append`。

按 [SSE 实测](./sse-event-size-limits.md)，一次请求约 1.3 万个 delta 事件，
其中绝大多数是纯转发（不含 `usage`，无需 `json::parse`）。

### 6. `idle_timeout` 在 httplib 路径上是死代码

`upstream.cpp:614` 硬编码 `response.stream.poll_fd = -1`，导致
`gateway.cpp:1298` 的 `poll_readable` 分支整体不执行。

上游停发时靠 httplib 自身 read timeout 触发 `worker_error`，
**`pump.idle_timeout` 永不置 true，永远归类为 `upstream_error`**。
计费与日志中这两者的区分是假的。

### 7. body 上限校验发生在读完之后

`http_dispatch.cpp:184` `parsed.content_length = req.body.size()`——
**整个 body 已进内存后才检查大小**，等于未检查。

同一份 body 同时存在三份拷贝：`ProxyRequest.http.body`、httplib 的
`req.body`、`UpstreamPreparedRequest.body`。

### 8. 生产路径上的原始 HTTP 字符串往返

`http_dispatch.cpp:211`

```cpp
.raw_request = inject_request_metadata(build_raw_http_request(req), client_ip),
```

已解析的 `Request` 结构体重新序列化成 HTTP 文本，再由
`api_authenticated_user` 重新解析。**每请求两次多余的序列化+解析。**

### 9. SSRF 校验存在 DNS rebinding 窗口

`assert_resolved_addresses_allowed` 解析并校验一次，httplib 内部**再解析一次**
去连接。两次解析之间 DNS 应答可被替换。

### 10. SSE 单行上限余量仅 1.6 倍

见 [SSE 尺寸上限](./sse-event-size-limits.md)。超限后果不止断流：
`message_delta` 到不了 → `finalize()` 不被调用 → **该请求计费丢失**。

### 11. 部署侧：ingress 体积限制

`charts/revlm/values.yaml:142` `proxy-body-size: "32m"`。

（`proxy-buffering: off` 与 `proxy-request-buffering: off` 已正确配置——
后者若开启，nginx 会先将 body 落盘再转发，后端零缓冲工作全部失效。）

### 12. 配置项声明了未实现的能力

`values.yaml:123` 注释称「Redis 为空时应用只启用单 Pod 本地并发限制」，
但 `redis_addr` 仅在 `config.cpp:80` 被解析，**全代码库无任何使用点**。
并发限制功能不存在。

## 二、被否决的方案及原因

推演过程中有多个方案被实测或推理否决，记录以免重复投入。

### 否决 1：调大 httplib 线程池

看似一行改动即可把并发上限从 32 提到 2000：

```cpp
server->new_task_queue = [] { return new httplib::ThreadPool(512, 2048); };
```

**否决原因：**

- 4000 线程（含桥接裸线程）× 8 MB 栈 = 32 GB 虚存；容器 `pids.max` 常见 4096
- `upstream.cpp:510` 的 100 ms 空转唤醒 × 2000 流 = 20000 次无效唤醒/秒
- 每 chunk 两次 futex + 四次拷贝，2000 流 × 450 事件/秒 ≈ 90 万事件/秒
- `httplib.h:10377` 动态线程创建与 `cleanup_finished_threads()` 均在全局
  单锁内，配合 `set_keep_alive_max_count(1)` 会串行化 accept 路径

**结论：与库的设计意图对抗，非解决方案。**

### 否决 2：按 body 体积分连接池

原提案：大请求走独占 HTTP/1.1 连接，小请求共享 HTTP/2 多路复用连接。
理由是「200 MB 上传会阻塞同连接上的小请求」。

**否决原因：**

- 真正阻塞小请求的是**网卡带宽**，不是连接。分池未解决该问题
- **body 体积是连续的，无分界线可划**（B 类上下文逐轮增长）
- 规模化后 agent 流量全部落到「大」侧，等于退回每请求一连接

### 否决 3：读 body 前若干字节判断 `stream` 标志

`docs/tmp-streaming-research/streaming-proxy-zero-buffer.md` 提出「读前 512
字节做路由决策」。

**否决原因：JSON 键顺序不保证。**
`{"model":…,"messages":[<数百 MB>],"stream":true}` 中 `stream` 位于 body 末尾，
前 512 字节看不到它。

**替代方案更简单：不解析请求体，改看上游响应 `Content-Type`。**
`gateway.cpp:1099` 的 `is_sse_content_type()` 已在做此判断（用于处理
「请求流式但上游返回 JSON」），将其作为唯一判据即可。
`pr.is_stream` 由响应事后回填，计费不受影响。

**收益：请求体从头到尾无需任何字节进入代理内存。**

### 否决 4：上传限速（第一轮否决，后经实测推翻）

推理：限速会拉长请求总时长，若上游存在「总时长」超时则必然触发，
且失败时已消耗完整带宽，把「可能成功」变成「一定失败且更贵」。

**实测推翻：** 见[上游上传行为](./upstream-upload-behavior.md)——
5 KB/s 持续 206 秒未被断开，上游超时全为空闲型。且客户端弱网慢上传本就是
必须支持的场景，限速的风险上界不超过它。

**限速方案恢复可用。**

### 否决 5：大请求排队 / 名额机制

否决 4 成立时的替代方案：不减速，减同时在跑的大请求数量，让排队发生在
上游连接建立之前（此时无任何超时在计时）。

**否决原因：** 否决 4 被实测推翻后此方案失去必要性。且实测显示渠道 body
上限为 20 MB 量级，按最慢 0.72 MB/s 计单请求最长约 28 秒，不构成需要排队
的资源占用。

### 否决 6：`Expect: 100-continue` 提前拒绝

**实测否决：** HTTP/3 不支持该机制；协商到 h2 时虽在 1.0 s 收到
`100 Continue`，但上游仍将 64 MB 全部读完，11.8 s 才回 413。
`Content-Length` 已声明体积，上游不据此提前拒绝。

### 否决 7：代理侧预设 body 体积上限

实测得该渠道上限为 20 MB（21 MB 起 413），一度考虑在代理侧提前拦截以避免
白传。

**否决原因：该值由渠道 CDN 配置决定，非协议常量。** 代理侧硬编码任何数值，
都会在渠道调整配置后变成错误的拒绝。**超限应透传上游的 413。**

代价是超限请求会白传完整 body（实测 64 MB 白传 12~64 秒）。这是用带宽换取
「不猜上游限额」的取舍。

### 否决 8：内核零拷贝转发（splice/sendfile）

理论上可让字节完全不进用户态内存。

**否决原因：** 上游为 HTTPS，字节必须在本进程内加密，无法绕过用户态。
64 KB 滑动窗口已是下限。

### 否决 9：Boost.Beast 作为上游客户端

**实测否决：** 上游默认协商 HTTP/3（13 次实测中 12 次），Beast 既不支持
h2 也不支持 h3。而 h2 多路复用是 10000 并发下控制上游连接数的必要手段。

### 否决 10：为新 server 库仿制 `BufferStream` 测试适配层

14 个测试文件依赖 `handle_http_request(string) -> string`，其实现继承
`httplib::Server` 调用其 protected `process_request`，并使用
`httplib::detail::BufferStream`（`http_dispatch.cpp:263-275, 615`）。

**否决原因：** 为新库仿制同类适配层是把同一个设计错误搬一遍。测试应直接
调用 handler 层，输入输出均为结构体。

## 三、最终方案

### 架构

```
                    ┌──────────── io_context (N = 核数) ────────────┐
                    │                                              │
  客户端 ──h1.1───► │  Beast acceptor 协程                          │
  (ingress,         │    └─ 每连接一协程                            │
   buffering off)   │        request_parser<buffer_body>            │
                    │        无体积上限 + async_read_some           │
                    │              │                               │
                    │      热路径：全内存，零阻塞 DB                 │
                    │        token LRU / 配置快照 / 余额预留         │
                    │              │                               │
                    │              ▼                               │
                    │  每 io 线程一个 curl_multi（无跨线程共享）      │
                    │    单一连接池，锁定 HTTP/2 多路复用            │
                    │              │                               │
                    │   READFUNCTION ◄── PAUSE/CONT 背压            │
                    │   WRITEFUNCTION ──► SSE 增量转发 ──► 客户端    │
                    └────────────────┬─────────────────────────────┘
                                     │ 无锁队列（非阻塞）
                                     ▼
                              BatchWriter 单线程
                                200 ms / 128 条批量落库
```

### 1. 滑动窗口 body 转发

内存中只保留当前一小段（64 KB），转发后立即复用：

```
上游可接收 → 从客户端 socket 读一块 → 交给上游 → 归还缓冲 → 读下一块
```

**内存占用与 body 体积完全无关**——不存在「完整 body」这个对象。

**窗口推进节奏必须由上游写就绪驱动，不能由客户端驱动。** 上游未就绪则不读，
数据积压在内核接收缓冲，满后 TCP 自动收缩窗口，反压传回客户端。
**背压机制由 TCP 提供，不自建队列。**

curl 的 `READFUNCTION` 天然是 pull 语义：无数据可交时返回
`CURL_READFUNC_PAUSE`，客户端数据到达后 `curl_easy_pause(CURLPAUSE_CONT)` 恢复。

### 2. 窗口缓冲池，不随连接分配

单请求两块 64 KB 看似很小，但 10000 并发相乘即 1.2 GB。

**缓冲改为公共池，借用即还：**

```
等待该连接可读（此时占 0 字节缓冲）
  → 可读 → 从池借一块 → 读入 → 转发上游 → 立即归还
```

正在等待客户端发送下一段的连接不占缓冲。按 1 Gbps 出口反推，同一瞬间真正在
搬运字节的连接为数十条量级，**池给 64 块 × 64 KB = 4 MB 即可支撑 10000 并发**。

### 3. 滑动窗口的代价：只能前进一次

三项现有功能因此必须移除（详见「四、行为变更」）：换渠道重试、body 改写重发、
从请求体解析 `stream` 字段。

### 4. 上游客户端：libcurl，单一连接池，锁定 h2

选 libcurl 而非手写 nghttp2：

| 能力 | libcurl | 裸 nghttp2 |
|---|---|---|
| ALPN h2 协商 + h1 回落 | 自带 | 自行实现 |
| 连接池与复用 | 自带 | 自行实现 |
| h2 流控 / HPACK / GOAWAY / 重连 | 自带 | 自行维护状态机 |

**每 io 线程一个 `curl_multi` 及独立连接池，不跨线程共享 handle，零锁。**
这也绕开了 httplib `Client` 的线程亲和限制（`socket_requests_are_from_thread_`）。

**显式锁定 h2**：h2 多路复用（单连接数百 streams）足以把 10000 并发压到
40~100 条上游连接。h3 的增益主要在弱网，服务端到上游一般为良好链路，
锁 h2 少一项不确定性（且需确认部署镜像的 curl 是否编入 h3 支持）。

### 5. 上传限速：出口保护

实测确认可行（5 KB/s 存活 206 秒）。**用途是出口紧张时保护 SSE 响应，
不是防止大 body 阻塞小请求**（后者由否决 2、7 说明不成立）。

机制即「控制读取节奏」，无需额外组件：令牌桶不足时返回 PAUSE，定时器补足后
CONT 恢复。两层配额——每请求一层（防单请求吃满出口）、全局一层（留出余量）。

**限速仅允许短暂让路（毫秒级），不允许持续压制。** 持续降速会拉长总时长；
虽然实测该渠道无总时长超时，但不应把这一性质当作所有渠道的保证。

### 6. 出口带宽是真实上界

按 [SSE 实测](./sse-event-size-limits.md) 的事件尺寸（`thinking_delta` 175 B、
`text_delta` 150 B）与典型输出速率估算：

```
单条流 ≈ 60 事件/秒 × 175 B ≈ 10 KB/s
10000 条同时输出 ≈ 100 MB/s ≈ 800 Mbps
```

**10000 条流全速输出时，仅响应即占满 1 Gbps 出口。** 实际不会全部同时输出
（A 类 1~3 秒结束，B 类有思考间隔），打三到五折为 250~400 Mbps，但与大 body
上传属同一量级，不可忽略。

**结论：1 Gbps 单实例承载 10000 流已贴近物理上限，只能靠多实例分摊
（`values.yaml` 已配 `maxReplicas: 6`），软件层无解。** 限速机制的真实价值
是让「出口紧张时谁先让」成为可控决策——建议响应优先、上传让路。

### 7. 数据库层去阻塞

**配置快照**：`shared_ptr<const Snapshot>` 持有全部 channel_group / channel /
model / token 映射。读侧原子 load，零锁零拷贝；admin 改配置时重建整份快照
原子替换。

**余额预留**：`unordered_map<user_id, atomic<int64_t>>`（微美分），请求路径
`fetch_sub`，为负则 402。无锁、无 DB。

**BatchWriter 单线程**：200 ms 或 128 条触发。同一用户窗口内多次扣费合并为
一条 `UPDATE users SET balance_usd = balance_usd - ?`（相对扣减，无需行锁
语义）；`requests` 表批量 INSERT。**DB 写入 QPS 从 10000 降到约 80，
连接池 64 → 8。**

透支上界 = 刷盘窗口 × 单用户燃烧速率，有界可证。`values.yaml:152` 已配
`upstream-hash-by: $binary_remote_addr`，同一客户端 IP 恒定落同一 pod，
进程内 per-user 状态可行。需严格零透支时可将预留移至 Redis 计数器。

### 8. 四段超时

现状 `proxy_upstream_timeout_seconds = 30` 同时充当 connect / read / write
超时（`upstream.cpp:405-407`）。拆分为：

| 阶段 | 语义 | 建议值 |
|---|---|---|
| connect | TCP + TLS 握手 | 5 s |
| upload stall | **停滞**检测，非总时长 | 10 KB/s 持续 30 s |
| response header | 首字节等待 | 120 s |
| inter-event idle | SSE 事件间隔 | 60 s |

**upload 一项必须是停滞超时**——大 body 传输 10 分钟只要未停即为健康。

curl 提供 `LOW_SPEED_LIMIT/TIME`，且有真实 socket，`poll_fd` 不再是 -1，
`idle_timeout` 不再是死代码。

### 9. SSE 零拷贝转发

在 curl `WRITEFUNCTION` 的缓冲上原地切分：**仅含 `usage` 的事件执行
`json::parse`，其余以 `string_view` 直接 `async_write`。**

按实测，一次请求约 1.3 万事件中绝大多数为纯转发，带 `usage` 的
`message_start` / `message_delta` 仅 2 个且 ≤399 B。

单行上限提至 1 MiB / 单事件 2 MiB（依据见 [SSE 尺寸上限](./sse-event-size-limits.md)）。

保留 `gateway.cpp:431` 的 `kFlushBytes = 1024` 攒批策略：150 B 事件单独发送时
TLS 记录开销约 20%，攒至 1 KB 降到 3%；10000 流 × 60 事件/秒逐个发送为
60 万次系统调用/秒，攒批后降至约 8 万次。

### 10. body 体积校验改为流式计数

移除「读完后检查」。若保留可配上限则边读边计数、超限立即中断连接；
**默认不设代理侧上限，超限由上游 413 透传**（见否决 7）。

`config.hpp:19-20` 的 `int` 应改为 `long long`。

### 11. 部署侧

```yaml
# charts/revlm/values.yaml:142
nginx.ingress.kubernetes.io/proxy-body-size: "0"   # 32m → 0（不限）
```

其余三项 nginx 注解已正确。

### 12. 顺带修复

- **SSRF DNS rebinding**：自行解析一次并校验，用 `CURLOPT_RESOLVE` 钉住已校验
  的 IP，消除二次解析窗口
- **原始 HTTP 字符串往返**：`build_raw_http_request` 整条链路删除，改传结构体

## 四、行为变更（需决策）

三项现有功能与滑动窗口不兼容，**必须移除**：

### 1. 服务端换渠道重试

`gateway.cpp:685-726`、`1058-1212` 两处 do-while 循环。

- **物理不可能**：body 流式发出后无法回放，无论体积多大
- 10000 并发下串行重试全部渠道会把局部故障放大为雪崩（N 倍上游压力 ×
  N 倍占用时长）
- 客户端（翻译插件、agent）自身均有重试

移除后 `Gateway::handle` 从 200+ 行降至约 50 行，
`UpstreamStreamResponse::request`（`upstream.hpp:56`，成功路径上从不读取的
死数据）一并消失。

### 2. 参数不支持时改写 body 重发

`upstream.cpp:258-291` `rewrite_for_unsupported_parameter_retry`。

`:275` `rewrite_body_field(prepared.body, ...)` 需完整 body 在内存中改写后重发，
与流式转发根本不兼容。

### 3. 从请求体解析 `stream` 字段

`http_dispatch.cpp:490 / 503 / 516` 三处 `parse_json_bool_field(req.body, "stream")`。

改由上游响应 `Content-Type` 判定。**此变更同时修正了现有实现对 JSON 键顺序的
隐含假设**（见否决 3）。

## 五、改动清单

| 模块 | 动作 | 行数 |
|---|---|---|
| `net/io_runtime.{hpp,cpp}` | 新增：io_context + 线程 + 优雅退出 | ~180 |
| `net/curl_pool.{hpp,cpp}` | 新增：per-thread multi ↔ Asio 桥、连接池、PAUSE 背压 | ~550 |
| `net/buffer_pool.{hpp,cpp}` | 新增：公共窗口缓冲池 | ~120 |
| `net/rate_limit.{hpp,cpp}` | 新增：两层令牌桶 | ~100 |
| `server/beast_server.{hpp,cpp}` | 新增：acceptor + 连接协程 + 流式 body 解析 | ~700 |
| `server/http_sink.hpp` | 新增：`ResponseSink` 抽象 | ~130 |
| `store/snapshot.{hpp,cpp}` | 新增：配置快照原子替换 | ~350 |
| `store/batch_writer.{hpp,cpp}` | 新增：余额预留 + 批量落库 | ~400 |
| `proxy/upstream.cpp` | 重写：删 `StreamBridgeState` / `make_client` / 参数重试 | ~550 |
| `proxy/gateway.cpp` | 改：`ResponseSink`、SSE 零拷贝、四段超时、删 failover | ~450 |
| `server/http_dispatch.cpp` | 改：脱 httplib、删原始 HTTP 往返、删 `stream` 解析 | ~350 |
| `users/*`、`channels/*` | 改：走快照 | ~200 |
| 14 个测试文件 | 重写：`handle_http_request(string)->string` 全废 | ~800 |
| `charts/revlm/values.yaml` | `proxy-body-size: 0` | 1 |

约 **4900 行**。移除 cpp-httplib；新增 libcurl（本机 8.7.1，已含 HTTP2）；
Beast / Asio 零新依赖（Boost 1.83 已在依赖中，C++20 已开启）。

## 六、执行顺序

1. **`ResponseSink` 抽象 + 测试改造** —— 不改变行为。当前
   `::httplib::Response` 泄漏进 7 个公开头文件，`gateway.hpp` 一动全量重编。
   此步换不换库都是净收益，且让后续替换成为机械操作。
2. **配置快照 + BatchWriter** —— 与 HTTP 层零耦合，可独立压测。
   **单独落地即可把并发上限从 32 推到 DB 之外。**
3. **上游换 curl** —— 可独立验证 h2 复用连接数、滑动窗口背压、超时分段。
4. **Beast server + 缓冲池 + 限速** —— 此时仅需接管 accept / 解析 / 路由。

**第 2 步必须先于第 4 步验证完成**，否则 server 重写完才会发现瓶颈仍在
`FOR UPDATE` 行锁上。

## 七、遗留问题

1. **数百 MB ~ 1 GB 请求的可行性。** 实测该渠道 20 MB 即 413。架构与 body
   体积无关（64 KB 逐块转发，1 GB 与 1 MB 同等开销），但此类请求在上游侧即被
   拒绝。若必须支持，需在代理侧拆分重组 body（如超大图片改走文件接口换取
   引用），属独立功能，不在本方案范围。
2. **单实例实际需承载的同时输出流数。** 直接决定出口是否足够——10000 条流
   仅响应即需约 800 Mbps。
3. **部署镜像的 curl 是否编入 h3 支持。** 锁定 h2 可回避，但需确认。
4. **`docs/tmp-streaming-research/` 三份文档含已被否决的结论**（前 512 字节
   判断 `stream`、`Request::content_provider_` 需打补丁——该成员在 httplib
   0.50.1 中实为 public，见 `httplib.h:1390`；以及基于 4 MB body 上限的内存
   估算）。建议在本方案落地后清理或标注。
