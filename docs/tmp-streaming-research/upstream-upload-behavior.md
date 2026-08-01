# 上游上传行为实测：速率、限速容忍度、body 上限

> 范围：`backend/src/proxy/upstream.cpp` 的上游请求发送路径
> 探针：`backend/tests/upload_rate_probe.cc`
> 被测渠道：RightCode Deepseek（`https://www.rightapi.ai/deepseek/anthropic/messages`）

## 动机

流式改造要回答三个问题，此前全靠推测：

1. 上游能接受多快的上传？→ 决定大 body 请求的耗时量级
2. 主动限速会不会被上游误判成连接断开/超时？→ 决定限速方案是否可行
3. body 超限时上游是提前拒还是读完才拒？→ 决定要不要在代理侧提前拦截

## 探针方法

body 是**截断的 JSON**——合法前缀 + 未闭合的 `content` 字符串：

```
{"model":"deepseek-v4-pro","max_tokens":1,"stream":false,
 "messages":[{"role":"user","content":"AAAA…（填充到 Content-Length）
```

上游必须读完 `Content-Length` 声明的全部字节才能发现 JSON 非法 → 返回 400。
**请求从不进入模型，13 次实测零 token 成本。**

填充字节在 `CURLOPT_READFUNCTION` 里实时生成，进程内从不存在完整 body——
这本身就是滑动窗口方案的可行性验证。

## 实测结果

### 1. 限速可行：上游没有「总时长」超时

| 限速 | body | 实际耗时 | 上游响应 |
|---|---|---|---|
| 不限 | 4 MB | 5.5 s | 400 |
| 100 KB/s | 4 MB | 33.5 s | 400 |
| 20 KB/s | 2 MB | 103.7 s | 400 |
| **5 KB/s** | 1 MB | **206.7 s** | **400（未断开）** |

5 KB/s 持续 3 分半，上游完整收下。**上游超时只看「多久没有字节到达」，
不看「总共花了多久」。**

与 cc-switch 自身配置一致（`proxy_config` 表）：
`streaming_first_byte_timeout=90`、`streaming_idle_timeout=180`、
`non_streaming_timeout=600`——三者全是空闲型，无总时长项。

结论：**客户端弱网慢上传本来就是必须支持的场景，主动限速的风险上界不超过它。**

### 2. body 上限 = 20 MB（该渠道）

夹逼所得：

| body | 上游响应 |
|---|---|
| 4 / 8 / 16 / 20 MB | 400（读完才判，说明放行） |
| **21 MB** | **413** |
| 22 / 24 / 32 / 64 MB | 413 |

413 由 Cloudflare 返回（响应体含 `cdn-cgi/content` 链接），非上游应用层。
**该值属渠道/CDN 配置，不同渠道不同，不是协议常量。**

### 3. 超限时上游读完才拒，且无法提前得知

三种 HTTP 版本组合下 `Expect: 100-continue` 均无效：

| 请求设置 | 实际协商 | `100 Continue` | 最终 413 | 白传 |
|---|---|---|---|---|
| 默认 | HTTP/3 | 不支持 | 43.97 s | 64 MB |
| 强制 h2 | HTTP/3 | 不支持 | 43.97 s | 64 MB |
| 强制 h1.1 | HTTP/2 | **1.00 s** | **11.81 s** | 64 MB |

拿到 `100 Continue` 的那次也没用：Cloudflare 在 1.0 s 放行，然后把 64 MB
全部数完，11.8 s 才回 413。**`Content-Length` 头里已声明 64 MB，上游仍不据此
提前拒绝。**

因此「收到 4xx 立刻掐断上传」这个措施救不了超限场景——4xx 到达时上传已结束。
（该措施对上游中途因其他原因报错仍有价值，但不解决体积超限。）

### 4. 上传速率 0.72 ~ 5.42 MB/s

同一渠道同一 body 大小下波动 7 倍，随本地上行链路变化，未观察到上游侧限速。
20 MB 按最慢档 0.72 MB/s 计约 28 秒。

### 5. 上游默认协商 HTTP/3

13 次请求中除强制 h1.1 落到 h2 的一次外，全部经 alt-svc 升级到 HTTP/3。

这排除了 Boost.Beast 作为上游客户端——它既不支持 h2 也不支持 h3。

## 对设计的影响

| 实测结论 | 设计决定 |
|---|---|
| 上游无总时长超时，5 KB/s 可存活 | 上传限速可用于出口保护 |
| 超限读完才拒，无法提前得知 | 「提前问一声再传」方案作废 |
| body 上限由渠道/CDN 决定，非常量 | 代理侧不设固定上限，超限透传上游的 413 |
| 上游默认 h3，h2 可强制 | 上游客户端需支持 h2/h3 → 排除 Beast |
| 20 MB 上限下大 body 最多 28 秒 | 大请求排队/名额机制无必要 |

## 已知代价

不在代理侧设上限意味着：超过渠道上限的请求会白传完整 body 才收到 413。
按实测，64 MB 白传 12~64 秒。这是**用带宽换取「不猜上游限额」**的取舍——
代理侧硬编码任何数值都会在渠道换 CDN 配置时变成错误的拒绝。

若后续要优化，可行方向是从上游返回的 413 中学习该渠道的实际上限并缓存，
而非预设常量。本文档不含该设计。

## 复现方式

```bash
cd backend/tests
clang++ -std=c++20 -O2 upload_rate_probe.cc -lcurl -o upload_rate_probe

export REVLM_PROBE_URL='<渠道 messages 端点>'
export REVLM_PROBE_TOKEN='<渠道 token>'

./upload_rate_probe --size-mb 20                      # 找体积上限
./upload_rate_probe --size-mb 2 --rate-kbps 20        # 测限速容忍度
./upload_rate_probe --size-mb 64 --http 11 --expect-100 --abort-on-response
```

参数：`--size-mb` body 体积、`--rate-kbps` 限速（0=不限）、`--http` 强制版本
（11/2/3）、`--expect-100` 启用 100-continue、`--abort-on-response` 收到 4xx
立刻掐断上传。

输出含 `t_100` / `t_final_status` 两个时间戳——**区分二者是必要的**，
`100 Continue` 早到会被误读成「上游提前拒绝」。
