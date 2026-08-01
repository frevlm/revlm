# revlm ↔ 本地 cc-switch 端到端实测记录 (2026-07-31)

## 拓扑
```
curl 客户端 → revlm Beast (:8080, /v1/*) → cc-switch (127.0.0.1:15721, Anthropic SSE) → opencode.ai → DeepSeek
revlm httplib (:8081, admin) 并行运行
```

## 可复现步骤
1. `backend/tests/seed_ccswitch_test.sh` — 建渠道组/渠道/token/余额(渠道 base_url=http://127.0.0.1:15721)
2. `REVLM_DB_DSN='root:root@tcp(127.0.0.1:3306)/revlm?...' REVLM_ADDR=:8081 REVLM_BEAST_ADDR=0.0.0.0:8080 REVLM_PROXY_UPSTREAM_TIMEOUT_SECONDS=120 ./build/backend/revlm`
3. curl 冒烟见脚本输出。

## 环境事实
- cc-switch 本地代理只支持 **HTTP/1.1**(openresty,无 ALPN h2)——设计文档假设的 h2 多路复用无法对 127.0.0.1:15721 验证
- cc-switch 接受 revlm 规范模型名(claude-haiku-4-5-20251001 等),内部映射到 deepseek-v4-flash;上游响应里 model 变成 `deepseek/deepseek-v4-flash`
- cc-switch 对 4MB+ body 回 100 Continue 后 400(读完全量再拒);1MB 正常
- cc-switch 有真实上游限流(测试连发 ~100 请求后 429 一阵)
- `upstream_channel_allows_private_target(base_url)` 放行 127.0.0.1,本地渠道不需要改 SSRF

## 实测通过(修复后)
- 流式 SSE 完整(message_start→…→message_delta→message_stop,usage 事件解析)
- 非流式 JSON 透传(content-type: application/json,body 完整)
- 20 并发流 + 10 并发非流式全完成(上游不限流时)
- 401 错误 token、429/400 上游错误透传
- 计费:requests 落库(input/output tokens、is_stream、model),BatchWriter 刷盘
- 16MB body → RSS 32→144 MB(**body 全量进内存,滑动窗口未接上游**)

## 已修 bug(工作树内)
1. `curl_multi_pool.cpp` `stream_read` — 排空队列而 worker 未结束时误返 -1 → SSE 泵提前断流(核心 bug)
2. `curl_multi_pool.cpp` `stream_headerfn`/`execute_stream` — headers 数据竞争(写侧无锁) → 响应头间歇为空
3. `curl_multi_pool.cpp` `stream_headerfn` — 1xx(100 Continue)被当最终状态 → 大 body 被拒时透传 100
4. `gateway.cpp` `apply_upstream_gateway_stream` — headers 参数与 `std::move(upstream)` 别名 → headers/Content-Type 丢失;调用点先抽独立 vector + 按值传参
5. `gateway.cpp` `SseReader::drain_partial` — 末尾无换行的残留行被丢弃 → 非流式 JSON 空响应
6. `gateway.cpp` `set_stream_correlation_headers` — 在 `set_chunked_provider` 写头之后调用 → X-Response-Id 永不上线;前移
7. `migrations/0012` — TEXT 列(token_hash/time)建索引缺 key length → MySQL 拒绝,服务起不来;加前缀

## 已知未修(对应设计文档缺口)
- **单总超时 30s**:`CURLOPT_TIMEOUT = proxy_upstream_timeout_seconds` 同时当 connect/total。cc-switch 处理 16MB 需 30.5s → 被 30s 总超时杀死(实测 502)。设计文档"四段超时"未落地
- **滑动窗口上传未接**:beast 读 body 全量累积进 `body_accum`(O(body) 内存,实测 16MB→+112MB);上游请求 `initial_body_chunk = 整个 body`,`read_body` 无调用者
- **流式连接零复用**:每条流在 worker 线程 `curl_multi_init()` 新建 multi → 每请求一条新上游连接(TIME_WAIT),与设计"每 io 线程一个 curl_multi + h2 多路复用"相悖
- **每条 SSE 事件都 json::parse**:设计只要求 usage 事件解析
- **`poll_fd` 硬编码 -1**:inter-event idle 超时死代码
- **每流阻塞一个 io 线程**:`set_chunked_provider` 同步跑完整流
- **客户端写无超时**:慢客户端永久占 io 线程
- **计费模型错位**:上游响应 model 被 `finalize()` 覆盖为 `deepseek/deepseek-v4-flash` → `channel->find_model()` 找不到 → usd=0;`requests.endpoint` 恒 NULL

## 相关代码位置
- 请求流:`beast_server.cpp` handle_connection → dispatch_v1_request → `run_messages_stream`(anthropics_messages.cpp) → `Gateway::run_stream`(gateway.cpp:740)
- 上游流:`open_scheduled_upstream_stream`(gateway.cpp:224) → `default_upstream_http_stream_transport`(upstream.cpp:371) → `CurlMultiPool::execute_stream`(curl_multi_pool.cpp:668)
- SSE 泵:`pump_gateway_stream`(gateway.cpp:1190) + SseReader(gateway.cpp:427)
