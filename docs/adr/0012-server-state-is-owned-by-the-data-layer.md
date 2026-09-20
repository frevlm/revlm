# 服务器状态归数据层所有，组件不持有远端数据

状态：accepted

远端数据的缓存、请求去重、失效与重取由 `src/data/` 下的资源模块统一持有，对外只暴露 `useChannels()` / `useUpdateChannel()` 这类 hook。组件里不出现 `useEffect` + `await api.*`，也不把一个远端对象拆成多个 `useState`。

`src/api/` 保持为纯传输层，只负责 URL、方法与类型，不含缓存与状态。这个分工不变。

## 背景

对齐之前，13 个组件文件在自己体内直接 `await` API，各自重写 `data` / `loading` / `err` 三件套。全站 226 个 `useState`。`pages/admin/ChannelsPage.tsx` 一个组件持有 30 余个 `useState`，其中九个是同一个 `Channel` 对象被拆成的独立字符串：

```
const [editName, setEditName]       = useState('');
const [editType, setEditType]       = useState('');
const [editGroups, setEditGroups]   = useState('');
const [editBaseURL, setEditBaseURL] = useState('');
…
```

该文件 8 个 `useEffect` 里大半是把远端对象摊开到这些字段、再收回成请求体的胶水。

## 理由

**数据结构错了，代码量是结果。** 服务器有一个对象，客户端把它砸成散件后必须写代码维持一致性。改回单个草稿对象、提交时 diff 出 patch，九个 state 变一个，配套的 `useEffect` 一起消失。这不是少写几行，是消除了一整类"忘记同步某个字段"的缺陷。

**没有缓存层就没有失效策略。** 现状下每次打开 modal 重新拉一次、每个卡片各自拉一次、修改后靠手工调用刷新函数。哪些数据在一次变更后过期，这个知识散落在每个调用点，因此必然有调用点漏掉。

**分层让页面可被隔离渲染。** 组件不再自己发请求之后，渲染一个组件只需要喂它数据，不需要准备网络环境。这是后续组件目录成立的前提。

## 代价

引入 TanStack Query 作为缓存层，约 13KB。这是本次重写唯一新增的运行时依赖。

迁移期间新旧两套取数方式共存。按 `DashboardPage`（199 行）先验证形态、`ChannelsPage`（1097 行）后取收益的顺序推进，共存窗口尽量短。

## 考虑过的方案

- **react-router v7 的 loader / action 数据路由**：零新增依赖，仓库已经在用 react-router 7，能消掉 `useEffect` 取数。但失效粒度是整个路由级 revalidate，而控制台存在轮询图表、同页多张卡片各自刷新、modal 内独立取数这三类需求，用 loader 表达需要绕。且迁移到数据路由本身要重写 `App.tsx` 的路由结构，代价并不更小。
- **自己写一个极小的 `useResource` hook**：起点简单，但缓存键、去重、失效、轮询这些需求会一条条长出来，最终写出一个更差的缓存库。
- **上一个全局状态容器（Redux 等）把远端数据放进 store**：把服务器状态当客户端状态管，需要手写全部同步逻辑，正是当前问题的放大版。
