# 大并发 + 大请求 + 限速压力测试方案

> 验证改写方案（`high-concurrency-streaming-rewrite.md`）中令牌桶上传限速的可行性：
> 50 并发混合流量（2 × 10 MB 大请求 + 48 × 5 KB 小请求），聚合出口限速 5 Mbit/s，
> 代理零缓冲转发。

## 一、测试负载

| | 大请求（模拟 Agent） | 小请求（模拟翻译） |
|---|---|---|
| 数量 | 2 | 48 |
| body | 10 MB | 5 KB |
| 占比 | 99.9% 字节量 | 0.1% 字节量 |

```
总数据量：2 × 10 MB + 48 × 5 KB ≈ 20.24 MB
限速：    5 Mbit/s = 625 KB/s
理论耗时：20.24 MB / 625 KB/s ≈ 32.4 秒
```

**核心验证点**：大请求不饿死小请求（48 个小的必须在大块传完前完成），令牌桶
聚合出口准确，代理全程零缓冲。

## 二、测试目标

| 验证点 | 具体问题 |
|--------|---------|
| **令牌桶限速准确** | 聚合出口 ≈ 5 Mbit/s（允许 sleep 精度导致的 5~10% 误差） |
| **零缓冲** | 代理 RSS 增量 < 20 MB，代理侧 >1 MB 分配 = 0 |
| **公平性** | 48 个小请求不被 2 个大请求饿死；小请求完成时间远早于大请求 |
| **无死锁** | 50 条连接全部完成，无 TCP 零窗口卡死 |
| **数据完整性** | 每条连接 echo 字节数 == 发送字节数 |

## 三、架构

```
┌─ Test Harness（单进程）────────────────────────────────────┐
│                                                            │
│  50 个线程，同时 POST 到代理 :15810                          │
│                                                            │
│  2 × big (10 MB)  ──┐                                     │
│  48 × small (5 KB) ─┤                                     │
│                      │  TCP loopback                       │
│                      ▼                                     │
│            ┌─────────────────┐                             │
│            │ RateLimitProxy   │                            │
│            │   :15810         │                            │
│            │                  │                             │
│            │ ContentReader    │  ← req.body 始终为空        │
│            │   │              │                             │
│            │   ▼              │                             │
│            │ TokenBucket      │  全局单桶                   │
│            │  5 Mbit/s        │  capacity=256 KB            │
│            │  refill 每 8ms   │                             │
│            │   │              │                             │
│            │   ▼              │                             │
│            │ ContentProvider  │── POST ──▶ Echo Server      │
│            │ sub-chunk 64 KB  │              :15811         │
│            └─────────────────┘            流式读取回显字节数  │
└────────────────────────────────────────────────────────────┘
```

**全在一个进程内，三个 httplib Server/Client，loopback。**
不限速方式：不用 `tc`/`pfctl`——令牌桶在代理 ContentReader→ContentProvider
路径上精确控速。

## 四、核心组件

### 4.1 Echo Server（上游，端口 15811）

- `HandlerWithContentReader` 流式读 body，累计字节数
- 返回 `ECHO:<字节数>`，客户端验证完整性
- 不做任何限速——背压全由代理令牌桶产生

### 4.2 令牌桶 `TokenBucket`

```
capacity: 256 KB（最大突发，≈ 0.4 秒积攒量）
rate:     625 KB/s = 5 Mbit/s
refill:   按需计算（基于 elapsed time，无需定时器）
```

**全局单桶**，所有连接共享。每连接每次最多消费 64 KB（一个子块），消费后必须
重新争用令牌。令牌不足→`sleep 10ms` 后重试，对应生产路径 `CURL_READFUNC_PAUSE`。

反饿死检测：每条连接记录上次获令牌时间戳，>5 s 未获则打印告警。

### 4.3 RateLimitProxy（代理，端口 15810）

- `HandlerWithContentReader`——**req.body 为空**
- 流式桥接：
  1. peek 首 512 字节（模拟元数据检测，实际仅统计）
  2. 每读到一个 chunk，向 TokenBucket 申领令牌
  3. 令牌足够 → 分成 64 KB 子块写 `DataSink`
  4. 令牌不足 → sleep 10ms 后重试
  5. 全部转发完 → `sink.done()`
- 上游回包后计算 SSE 事件数（用 production `SseReader` 等效逻辑），
  代理自身不保留响应内容

### 4.4 Test Harness

50 个 `std::thread`，主线程收集结果：

```cpp
struct Task {
    int id;                    // 0..49
    size_t body_size;          // 10 MB (id<2) or 5 KB (id≥2)
    std::string body;          // 确定性 pattern，基于 id
    uint64_t body_hash;        // 滚动哈希，验证完整性
};
```

每个线程：
1. 生成 body
2. `httplib::Client` POST 到 `:15810`
3. 记录 `t_start`, `t_end`, `status`, `response_body`
4. 超时：connect 10s, read/write 300s

## 五、测量指标

### 5.1 吞吐量

| 指标 | 方式 |
|------|------|
| 聚合有效吞吐 | `total_bytes / wall_time` |
| 每秒吞吐序列 | 令牌桶内部每秒 `try_consume` 成功字节数 |
| 每连接吞吐 | `body_size / (t_end - t_start)` |

### 5.2 内存

| 指标 | 方式 |
|------|------|
| 峰值 RSS | `getrusage` 开始/峰值/结束 |
| >1 MB 大块分配 | 重载 `operator new`，按 phase 追踪 |
| 代理 `req.body` 是否为空 | 每个请求检查 `req.body.empty()` |

### 5.3 公平性

| 指标 | 计算 |
|------|------|
| 小请求完成时间分布 | P50 / P95 / max（48 个） |
| 大请求完成时间 | 各自 t_end |
| 小请求是否全部早于大请求 | `max(t_small) < min(t_big)` |
| 饿死告警次数 | >5s 未获令牌的连接数 |

### 5.4 正确性

| 指标 | 通过条件 |
|------|---------|
| HTTP 状态 | 全部 200 |
| 回显校验 | 每条 `ECHO:<size>` == 实际发送字节数 |
| 无超时 | `result.error() == Success` × 50 |

## 六、预期结果与通过标准

```
理论耗时：20.24 MB / 625 KB/s ≈ 32.4 s（sleep 精度损失后约 33~36 s）
```

| 指标 | 通过阈值 |
|------|---------|
| 聚合吞吐 | 0.5 ~ 0.625 MB/s（允许 sleep 精度损失） |
| 代理 RSS 增量 | < 20 MB |
| 代理 >1 MB alloc | 0 |
| 全部完成 | 50/50，字节数一致 |
| 小请求完成 | 48 个全部在 2 个大请求完成之前 |
| 无死锁 | 无 >5s 饿死告警 |

## 七、文件与构建

```
backend/tests/rate_limit_stress.cc    # 约 400 行
```

```sh
g++ -std=c++17 -I/opt/homebrew/include \
    -O2 -lpthread \
    backend/tests/rate_limit_stress.cc \
    -o backend/tests/rate_limit_stress

# 运行（约 35 秒）
./backend/tests/rate_limit_stress
```

输出：

```
=== Rate Limit Stress Test ===
Concurrency: 50 (2 big × 10MB + 48 small × 5KB), Limit: 5 Mbit/s
[ 2.0s]  done: 15/50  rate=4.8Mbps  RSS=3MB
[ 5.0s]  done: 25/50  rate=5.1Mbps  RSS=4MB
[10.0s]  done: 40/50  rate=4.9Mbps  RSS=4MB
[15.0s]  done: 48/50  rate=5.0Mbps  RSS=5MB   <-- 48 small done
[33.0s]  done: 50/50  rate=5.0Mbps  RSS=6MB   <-- 2 big done

=== RESULTS ===
Completed: 50/50, Errors: 0
Wall time: 33.2 s (theory: 32.4 s)
Aggregate rate: 4.88 Mbit/s (97.6% of limit)

Small requests (48):
  completion: all within 0.5~8.0 s
Big requests (2):
  completion: 33.0s, 33.2s

RSS delta: 6 MB (req.body always empty: yes)
Starvation alerts: 0
Data integrity: 50/50 verified

*** ALL CHECKS PASSED ***
```

## 八、设计取舍

### 做

- 全局单令牌桶 + sleep 模拟 PAUSE
- httplib 三个 Server/Client 单进程 loopback
- 混合 body 体积模拟真实 A/B 流量比例
- `operator new` 重载追踪 >1 MB 分配

### 不做

- 不用 `tc`/`pfctl` 网络层限速（需 root，测的是 TCP 栈不是令牌桶）
- 不测 SSE 响应路径（与限速正交，`sse_size_probe` 已覆盖）
- 不做 per-connection 子配额（是生产调优参数，测试验证全局桶 + 自然公平）

### 与生产路径差异

| 方面 | 探针 | 生产 |
|------|------|------|
| 限速等待 | `sleep 10ms` | `curl_easy_pause(CURLPAUSE_CONT)` |
| 上游连接 | 每请求新建 httplib Client | curl 连接池 h2 复用 |
| 缓冲 | 64 KB 子块硬编码 | 公共缓冲池借用归还 |
| Server | httplib | Beast + Asio |

差异不改变令牌桶逻辑的有效性——consume/refill 是纯计算，与 I/O 层无关。
sleep 精度导致吞吐略低于理论值是预期内的，非方案缺陷。
