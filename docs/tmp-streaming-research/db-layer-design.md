# 数据库层去阻塞:设计

> 对应主方案「三、7」与「六、执行顺序」第 2 步。状态:设计稿,代码未修改。
> 目标一句话:**热路径零 DB 访问**——配置读走快照,余额走原子变量,写入走批量。

## 结构

两个新文件对,MySQL 仍是唯一事实源,内存全部是可重建的派生缓存:

```
热路径(io 线程)                        后台
─────────────────────                  ─────────────────────
认证/路由 → Snapshot 原子读             Snapshot 重建线程(周期 + 管理端触发)
余额闸门 → available > 0 否则 402
计费     → available.fetch_sub(usd)
         → 队列 enqueue  ──────────►   BatchWriter 单线程,200ms/128 条批量落库
```

## 1. Snapshot(`store/snapshot.{hpp,cpp}`)

```cpp
struct Snapshot {
    std::unordered_map<std::string, SnapshotToken> tokens; // token_hash → {user_id, token_id, group_id}
    std::unordered_map<long long, ChannelGroup> groups;    // channels 已填充
};
// 全局一个 std::atomic<std::shared_ptr<const Snapshot>>
```

- **全量重建**,4 条批量 SELECT(users / user_tokens / channel_groups / members JOIN channels),
  毫秒级。不做增量——全量替换没有中间态。
- 触发:本 pod 管理端写路径调 `invalidate()`(复用 `routing_rebuild_debounce_ms` 防抖);
  跨 pod 靠 5 s 周期重建;启动时同步建一次,失败即启动失败。
- **未命中回退**:新 token 在重建间隙必须可用——快照查不到就走现有
  `resolve_token_channel_group_by_raw_token` 单条 DB 查询,代价与今天相同。

替换点:`token_api.cpp:38` 认证、`gateway.cpp:662` 取渠道组。管理端 API 不动,继续直读 DB。

## 2. 余额 + 批量落库(`store/batch_writer.{hpp,cpp}`)

余额就是一张 map,一个用户一个原子数(µUSD = 1e-6 USD,对齐现有 `%.6f` 精度):

```cpp
std::unordered_map<long long, std::atomic<int64_t>> available_;  // shared_mutex 只保护 map 结构
```

- **准入**(替换 `paygo_balance_gate`):`available > 0` 放行,否则 402。语义与今天相同。
- **计费**(替换 `debit_user_balance_usd` 行锁):`available.fetch_sub(usd_micro)` + 入队。
  透支上界 = 刷盘窗口 × 单用户燃烧速率,主方案已声明接受。
- **重建同步**:快照重建时顺带读了 `balance_usd`,对无未刷盘扣费的用户直接覆盖
  `available = db 余额`——管理端充值由此生效(最迟 5 s)。

BatchWriter 单线程,mutex 双缓冲队列(10k QPS 下临界区占单核 0.05%,不做 lock-free),
128 条或 200 ms 触发,一个事务:

```sql
UPDATE users SET balance_usd = balance_usd - ? WHERE id = ?;   -- 按用户合并,相对扣减,无行锁
INSERT INTO requests (…) VALUES (…),(…),…;                      -- 多行一条
INSERT INTO request_totals (…) VALUES (…) AS new                -- 按 (user,token,date) 预合并
  ON DUPLICATE KEY UPDATE requests = requests + new.requests, …; -- 取代 apply_total 的每请求 SELECT+UPDATE
```

DB 写 QPS 10000 → 数十,`db_max_open_conns` 64 → 8。

失败处理:事务整批重试(指数退避);DB 持续不可用时队列封顶 10 万条,超限丢最旧并响亮记日志
——余额已扣,丢的是明细不是护栏;优雅退出时 drain 后最后一次 flush;崩溃丢失 ≤ 一个刷盘窗口。

删除:`debit_user_balance_usd`、`Request::commit`、`apply_total` 热路径用法、
`http_dispatch.cpp:282` 进程内 id 计数器。

## 3. 迁移(独立、零破坏、先行合并)

```
0011  ALTER TABLE requests MODIFY id BIGINT NOT NULL AUTO_INCREMENT;
      -- 顺带修在产 bug:id 计数器重启归零,commit 的存在性检查静默丢行
0012  CREATE UNIQUE INDEX idx_user_tokens_hash ON user_tokens (token_hash);
      CREATE INDEX idx_requests_user ON requests (user_id, id);
      CREATE INDEX idx_requests_time ON requests (time);
      CREATE INDEX idx_requests_token ON requests (token_id);
      -- 现状全库无任何二级索引,token 认证是全表扫描
```

## 4. 行为变更(落地 PR 需声明)

1. 计费与 usage 查询有 ≤200 ms 延迟;崩溃丢失一个窗口。
2. 余额展示走内存值(`available`),比 DB 新一个窗口。
3. 现状「期末余额不足则整条记录不落库」(`commit_proxy_usage:165`)移除:
   用量永远记账,允许有界透支——这是修复,不是退化。

## 5. 顺序

1. 迁移 0011/0012。
2. Snapshot,切认证/路由读侧。
3. 余额 map + BatchWriter,切计费,删死代码。
4. 压测确认后连接池收口 64 → 8。

每步独立回滚。测试:余额 map 并发守恒单测;BatchWriter 用 `flush_now()` 钩子驱动断言;
现有集成测试在计费断言前加一次 `flush_now()`,其余不动。
