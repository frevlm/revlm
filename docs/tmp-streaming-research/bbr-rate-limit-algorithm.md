# 上传限速池：公平份额 + libcurl 内建限速（实测）

> 状态：**实测完成**。手搓 BBR 已否决，最终方案为 `cap = total/N` + `CURLOPT_MAX_SEND_SPEED_LARGE`。
> 探针：`backend/tests/rate_limit_stress.cc`（全局单桶）、`backend/tests/rate_limit_curl_pool.cc`（错峰公平份额）

## 结论

1. **限速池不需要自研拥塞控制。** 每流 `cap = total / N`（流 join/finish 时重算并写回
   所有 handle 的 `CURLOPT_MAX_SEND_SPEED_LARGE`），pacing 全部交给 libcurl。
   25 条错峰流实测：利用率 97%（87.7 s vs 理论 85.3 s）、Jain 中位数 0.995、零缓冲。
2. **关键行为发现：curl 的限速按「传输平均速率」执行。** cap 中途调低后，curl 会
   停发数秒等平均值追平新 cap（记 catch-up stall）。第一轮实测中该停顿超过了
   echo 服务器默认 5 s read timeout，流 #0 被 400 掐断（10 MB 只传出 1.29 MB）。
   - **生产含义**：代理/上游侧任何「停滞检测」超时必须大于加流引发的追平停顿
     （秒级，上界 ≈ 旧cap/新cap × 限速窗口）。实测真实上游容忍 206 s 涓流
     （见 [upstream-upload-behavior.md](./upstream-upload-behavior.md)），无风险；
     但主方案「四段超时」中的 upload stall 检测（10 KB/s 持续 30 s）不得改小到 10 s 以下。

## 被否决：手搓 BBR 状态机

原设想：每流复刻 BBR 的 STARTUP/DRAIN/PROBE_BW/PROBE_RTT 状态机 + 8-phase 增益循环，
全局桶水位作拥塞信号。

**否决原因：**

- 完整复刻约 250 行状态机，首次运行 25 流全部卡死（0/25，互相等待令牌），
  排查成本高于收益
- BBR 解决的是「瓶颈带宽未知、需在网络中探测」的问题；本场景瓶颈是**自己设的**
  （总限速已知），唯一要解的是公平分配——`total/N` 一行解决
- 结论：**错误的工具。** 已知带宽下的公平分配不是拥塞控制问题

## 测试 1：全局单桶（`rate_limit_stress.cc`）

50 并发同时到达（2×10MB + 48×5KB），全局令牌桶 5.12 Mbit/s，httplib 代理
ContentReader→ContentProvider 零缓冲桥接。

| 指标 | 结果 |
|---|---|
| 聚合速率 | 5.18 Mbit/s（偏差 +1.2%） |
| 耗时 | 32.8 s（理论 33.2 s） |
| 48 小请求 | 全部 0.4 s 内完成，先于大请求 |
| 代理 req.body | 始终为空；>1MB 分配 = 0 |
| 完整性 | 50/50 |

结论：**同时到达 + 尽力争用场景下，全局桶 + 自然争用已足够公平**（小请求体积小，
争到一次令牌即完成）。

## 测试 2：错峰到达 + 公平份额（`rate_limit_curl_pool.cc`）

### 架构

```
单线程 curl_multi 驱动 25 条错峰流 ──► httplib echo :15831
   │
   ├─ body 在 CURLOPT_READFUNCTION 内生成 —— 全进程无 body 缓冲
   ├─ 限速：CURLOPT_MAX_SEND_SPEED_LARGE（libcurl 内建）
   └─ coordinator：join/finish 事件触发 cap = total/N 写回所有 handle
      （无定时器、无状态机、无自研 pacing）
```

### 负载

4×10MB + 2×5MB + 2×1MB + 17×5KB = 52.08 MB，到达时间 t=0~19.5s 错开，
总限速 5.12 Mbit/s，理论耗时 85.3 s。

### 结果（第二轮，echo read timeout 提到 60 s 后）

| 指标 | 结果 | 判定 |
|---|---|---|
| 完成 | 25/25 | ✅ |
| 耗时 | 87.7 s（理论 85.3 s，ratio 1.03） | ✅ 利用率 97% |
| 聚合速率 | 4.98 Mbit/s（上限 5.12） | ✅ 无超速 |
| Jain 公平指数 | 中位 0.995（82 样本） | ✅ |
| 大流均速 | 1.00 / 1.02 / 1.05 / 1.20 Mb/s（4 条 10MB） | ✅ 均分 |
| tiny 最大时延 | 0.07 s | ✅ 不被大流饿死 |
| RSS 增量 | 0 MB；>1MB 分配 = 0 | ✅ 零缓冲 |
| 最大并发流 | 8 | |

新流 join 时旧流立即被压到新份额（样本可见 `#0: 4.87 → 0.00 → 1.02 Mb/s`），
即用户要求的「新请求进池、旧请求让出」行为——由 curl 平均值限速自然产生，
无需显式通知。

### 第一轮失败记录（有价值）

echo 服务器用 httplib 默认 5 s read timeout 时：流 #0 在 4 次连续 cap 下调后
进入 ~5 s catch-up stall，服务器超时回 400 掐断连接。24/25。
**这不是限速池的 bug，是它暴露了「停滞超时」与「限速追平停顿」的耦合约束**（见结论 2）。

## 复现

```sh
# 测试 1：全局单桶，50 并发同时到达（~33 s）
g++ -std=c++17 -I/opt/homebrew/include -O2 -lpthread \
    backend/tests/rate_limit_stress.cc -o backend/tests/rate_limit_stress
./backend/tests/rate_limit_stress

# 测试 2：错峰 + 公平份额（~88 s）
g++ -std=c++17 -I/opt/homebrew/include -O2 \
    backend/tests/rate_limit_curl_pool.cc -lcurl -lpthread \
    -o backend/tests/rate_limit_curl_pool
./backend/tests/rate_limit_curl_pool
```

注意：接收端（echo/上游）read timeout 必须大于 cap 下调引发的停顿上界，
否则会复现第一轮的 400 断流。
