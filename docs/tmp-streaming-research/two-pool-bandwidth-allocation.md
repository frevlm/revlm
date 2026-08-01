# 双池带宽分配算法：normal 池 + 限速池（≥5MB）——实测定稿

> 状态：**8 轮实测完成，全部检查通过。** 探针：`backend/tests/rate_limit_two_pool.cc`。
> 初版设计稿中的 EWMA 估计器、中途晋升机制、无界缓冲均被实测否决并替换。

## 一、最终算法（经 8 轮数据收敛）

### 分类：join 时刻的纯函数，永不改判

```
Content-Length ≥ 5MB   → 限速池
Content-Length < 5MB   → normal 池（不限速，延迟优先）
Content-Length 未知    → 限速池（从第 0 字节起）
```

**没有「中途晋升」。** 未知长度 = 客户端流式发送 = agent 大请求的特征，悲观分类是正确先验。
中途改判被实测否决（见发现 1）。

### 分配：每 200ms tick + join/finish 事件

```
backlog = Σ 活跃 normal 流的 (Content-Length − 已上传)   # 直接测量，非估计
claim   = min(backlog / 1s, P)
budget  = clamp(P − claim − 5%P, 10%P, P − 5%P)
target  = max(budget / N, 32KB/s)

cap 应用：
  上调 → 立即生效
  下调 → 单 tick 最多砍半，且上个 tick 无进展的流不砍（斩断停顿链）
```

### 两条基础设施要求

1. **所有流从第 0 字节起挂 cap**（限速池挂 target，从不超过其生命周期平均）
2. **socket 缓冲有界**：SO_SNDBUF / SO_RCVBUF = 64KB——与主方案滑动窗口设计一致，
   否则 backlog 信号失真（见发现 3）

## 二、实测发现（每条都有失败运行作为证据）

### 发现 1：curl 限速窗口锚定传输起点，cap 低于生命周期平均 = 停到追平为止

对一条已跑了 5MB 的流中途挂 cap，三种进场方式全部长停顿：

| 进场方式 | 停顿 |
|---|---|
| 一步砍到 target | **10.9s** |
| 入口 cap = 实测速率，再降 | **6.5s** |
| 从第 0 字节预挂 cap=P「预热窗口」，再降 | **10.2s** |

结论：MAX_SEND_SPEED 无法表达「给跑快过的流降速」。**消灭场景而不是驯服它**——
未知长度直接进限速池后，该流 stall 降到 2.4~2.8s，与普通限速流同质。
若未来真需要中途降速，机制是 `CURL_READFUNC_PAUSE`（主方案的背压机制），不是改 cap。

### 发现 2：连续砍半形成停顿链

cap 从 300KB 连续砍到 34KB，每次砍半停 ~1s，未恢复又砍——总停顿 = 窗口字节/最终cap，
与一步砍到底相同（实测 8.4s）。**修法**：上个 tick 无进展的流跳过本次下调。
停顿上界回到 ~3s（对照生产 30s 停滞检测有 10 倍余量）。

### 发现 3：内核缓冲让 written ≠ delivered，backlog 信号致盲

默认（自动调优）缓冲下，`SIZE_UPLOAD` 领先线缆若干 MB：实测 t=85 时 backlog=0
而 3MB 仍在途，budget 提前回弹，大流回抢，medium 尾部饿到 25s。
SO_SNDBUF/RCVBUF=64KB 后 mediums max **25.0s → 18.4s**。
生产含义：代理测「已写入上游 socket」的字节数时，必须约束 socket 缓冲，
否则任何基于它的信号都是错的。

### 发现 4：EWMA 估计器两个方向都错，直测 backlog 零旋钮

| 估计器 | 失败模式 |
|---|---|
| 对称 α=0.3 | burst 进行中 m 衰减到 62KB/s，budget 回弹 910KB，mediums 被挤到 28.1s |
| 快攻慢衰 τ=2s | burst 结束后 m 滞留，util 掉到 89.4%，停顿链 8.4s |
| **backlog/1s 直测** | **无失败模式**——分类本来就靠 Content-Length，剩余字节是已知量 |

burst 还剩多少，budget 就压多少；尾字节送完的瞬间 budget 精确回弹。
buffer 假尖峰问题同时消失（buffer 里的字节就是即将上管道的真实需求）。

## 三、最终运行数据（run 8，59 流 / 97MB / P=1MiB/s）

```
利用率 98.7%（忙期）   完成 59/59   RSS 增量 2MB   >1MB 分配 0
限速池 6 流：stall_max 全部 ≤2.9s，池内速率 124~219KB/s（N 随时间变化）
未知长度 8MB 流：stall 2.75s，与普通限速流同质
smalls ≤50KB（翻译类，clean）:  p50=0.05s  p95=0.42s   ← 延迟硬指标
smalls >50KB（clean）:          p95=1.43s（FIFO 公平份额物理界 ~P/(1+N)）
smalls（medium burst 期间）:     p95=1.35s
mediums 1~4MB:                  p50=7.8s  max=19.3s
```

通过标准全部满足：util ≥90%、翻译类 p95 <0.8s、bulk <2.5s、burst 期 <4s、
mediums <25s、全部 stall <5s、零缓冲。

### 尺度换算注意

探针 P=1MiB/s 下 4MB medium = 4 秒管道占用；生产 1Gbps 下同样请求只占 32ms。
探针里 smalls 与 mediums 的池内争抢在生产尺度不构成问题；反之，探针验证的
分配逻辑（budget/claim/降速规则）与尺度无关，直接平移。

## 四、与主文档「否决 2」的关系（不变）

否决 2 拒绝的是按体积分**连接池**解决连接阻塞（伪问题，真瓶颈是带宽）。
本设计分的是**限速类别**，做的是带宽优先级分配——即主文档「响应优先、上传让路」的算法化。
边界抖动问题不存在：分类是 join 时刻的纯函数，永不改判。

## 五、调优历史（8 轮，供复盘）

| 轮 | 变更 | 结果 |
|---|---|---|
| 1 | EWMA α=0.3 + 自适应 H + 5MB 晋升 | util 98.8 ✓，mediums 28.1 ✗，晋升 stall 10.9 ✗ |
| 2 | 快攻慢衰 + inst 钳 P | mediums 17.5 ✓，util 89.4 ✗，停顿链 8.4 ✗ |
| 3 | **backlog 直测** + 停顿感知降速 | util 98.7 ✓，stall ≤4 ✓，晋升 stall 6.5 ✗ |
| 4 | cap=P 预热窗口（假设：晋升可平滑） | 晋升 stall 10.2 ✗ —— 假设被否 |
| 5 | **删除晋升，未知长度→限速池** | 全部 stall ≤3.1 ✓，度量竞态浮现 |
| 6 | 重叠分类 + 结构性阈值 | mediums 25.0 边界 ✗（缓冲致盲暴露） |
| 7 | **SO_SNDBUF/RCVBUF 64KB** | mediums 18.4 ✓，200KB small 物理界浮现 |
| 8 | smalls 按尺寸分级考核（翻译类 vs bulk） | **全绿** |

## 六、生产平移清单

1. 分类函数进请求入口（Content-Length 解析处），未知长度→限速池
2. coordinator 挂在 per-io-thread curl_multi 的事件循环里，tick 200ms
3. backlog 统计 = Σ(normal 流 Content-Length − 已转发字节)
4. 上游 socket SO_SNDBUF 有界（64KB 量级）
5. 停滞检测超时 ≥10s（结构性降速停顿 ~3s，需余量）
6. P 在生产不是常量（共享出口）——P 的在线估计是后续工作；
   分配逻辑本身与 P 来源无关
