# 流式代理改造调研（临时文档）

> **状态：方案已落地（2026-07-31 止）。** 主文档的 12 项问题中，滑窗 body 转发、
> 上传限速池（two-pool）、SSE 零拷贝转发、缓冲池、四段超时、快照 + BatchWriter
> 已全部接进生产代码；本目录保留实测数据供复盘，结论已可合并进 `docs/reference/`。

## 阅读顺序

| 文档 | 内容 | 可信度 |
|---|---|---|
| [high-concurrency-streaming-rewrite.md](./high-concurrency-streaming-rewrite.md) | **主文档**：10000+ 并发方案、12 项现有问题、10 条被否决路径及依据、改动清单、执行顺序 | 当前结论 |
| [db-layer-design.md](./db-layer-design.md) | 主文档「三、7」的详细设计：Snapshot / BalanceLedger / BatchWriter、迁移、执行顺序 | 设计稿 |
| [upstream-upload-behavior.md](./upstream-upload-behavior.md) | 上游实测：上传速率、限速容忍度、body 上限、HTTP 版本协商 | 实测数据 |
| [bbr-rate-limit-algorithm.md](./bbr-rate-limit-algorithm.md) | 限速池实测：公平份额 + libcurl 内建限速；手搓 BBR 否决记录；catch-up stall 约束 | 实测数据 |
| [two-pool-bandwidth-allocation.md](./two-pool-bandwidth-allocation.md) | 双池带宽分配定稿：backlog 直测、分类纯函数、缓冲有界；EWMA/晋升被实测否决 | 实测数据 |
| [rate-limit-stress-test-plan.md](./rate-limit-stress-test-plan.md) | 限速压测方案（全局桶 50 并发混合流量） | 方案 |
| [sse-event-size-limits.md](./sse-event-size-limits.md) | SSE 单行/单事件尺寸实测，`kMaxSseLineBytes` 余量仅 1.6× | 实测数据 |
| [streaming-body-memory-management.md](./streaming-body-memory-management.md) | 早期调研：body 拷贝路径分析 | ⚠️ 部分作废 |
| [streaming-proxy-zero-buffer.md](./streaming-proxy-zero-buffer.md) | 早期调研：零缓冲 POC 设计 | ⚠️ 部分作废 |
| [streaming-response-critical-fields.md](./streaming-response-critical-fields.md) | 早期调研：SSE 关键字段与计费依赖 | 基本有效 |

## 前三份早期文档中已作废的结论

主文档「二、被否决的方案」有完整依据，此处仅列索引：

1. **「读 body 前 512 字节判断 `stream` 标志」** —— JSON 键顺序不保证，
   `stream` 可能位于数百 MB 之后。改为看上游响应 `Content-Type`
2. **「`Request::content_provider_` 是私有的，需给 httplib.h 打补丁」** ——
   该成员在 httplib 0.50.1 中实为 public（`httplib.h:1390`），无需改库
3. **基于 4 MB body 上限的内存估算** —— 实际目标负载 body 可达数百 MB，
   且渠道上限实测为 20 MB 量级，估算前提不成立
4. **把 body 内存列为首要瓶颈** —— 实测撞墙顺序中它排第四，
   前三位是计费行锁、DB 连接池、HTTP 线程池上限 32

## 探针

| 探针 | 用途 |
|---|---|
| `backend/tests/upload_rate_probe.cc` | 上传速率 / 限速容忍度 / body 上限（截断 JSON，零 token 成本） |
| `backend/tests/sse_size_probe.cc` | SSE 单行/单事件尺寸分布 |
| `backend/tests/rate_limit_stress.cc` | 全局令牌桶：50 并发混合流量限速压测 |
| `backend/tests/rate_limit_curl_pool.cc` | 公平份额限速池：25 条错峰流 + curl 内建限速 |
| `backend/tests/rate_limit_two_pool.cc` | 双池带宽分配：59 流错峰、接收端管道模拟、backlog 直测 |
| `backend/tests/streaming_proxy_poc.cc`、`real_streaming_proxy*.cc` | 零缓冲转发 POC |

复现方式见各文档末节。探针均只统计字节数与事件类型，不落盘消息内容。
