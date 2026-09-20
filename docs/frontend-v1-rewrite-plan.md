# 控制台前端重写工单

本页是把 `frontend/` 对齐到 [CONTEXT.md](../CONTEXT.md) 与 ADR 0010–0013 的执行计划。它描述要做什么和按什么顺序做，不描述如何实现。

结论先行：控制台前端没有组件层，也没有数据层。它有 87 个 `.tsx`、8708 行非 story 组件代码、226 个 `useState`、一份 1868 行的样式表，以及三份手工同步的第三方依赖清单。"没有设计系统"只是表症；根因是**服务器状态被当成组件本地状态**，以及**第三方运行时不在构建图里**。这不是重构，是重写运行时边界、数据层和视图层三层。

## 目标规模

| 目标 | 现状 | 重写后 |
|---|---|---|
| 非 story `.tsx` 总行数 | 8708 | ~4200 |
| `pages/admin/ChannelsPage.tsx` | 1097 | ~80（页面只做路由装配） |
| `pages/TokensPage.tsx` | 895 | ~80 |
| `useState` 总数 | 226 | ~70（只剩真正的 UI 局部状态） |
| 组件内直接 `await` API 的文件 | 13 | 0 |
| `src/index.css` | 1868 | ~400 |
| `!important` | 73 | ~6 |
| 按 DOM id 操作 modal 的调用点 | 19 | 0 |
| 第三方依赖清单份数 | 3（`index.html`、`.storybook/preview-head.html`、隐含的隔离渲染环境） | 1（`package.json`） |
| UI 原语组件 | 0 | 9 |

## 判断依据

四条互相独立的架构缺陷，按严重度排列。

**一是服务器状态被当成组件本地状态。** `ChannelsPage` 一个组件持有 30 余个 `useState`，其中 `editName` / `editType` / `editGroups` / `editBaseURL` / `editKey` / `editStatus` / `editPriority` / `editPriceMultiplier` / `editConfigJSON` 是把一个 `Channel` 对象砸成九个独立字符串，再用 `useEffect` 手工摊开、手工收回。该文件 8 个 `useEffect` 里大半是维持这种一致性的胶水。全站 13 个文件在组件里直接 `await` API：没有缓存、没有请求去重、没有失效策略，每次打开 modal 重新拉一次。

**二是状态有三个真相源。** URL `searchParams`（`useAdminSelectionParam`）、React state、以及 DOM 与全局实例（`window.bootstrap` 的 Modal 实例、`window.Chart` 的图表实例）。`components/modal.ts` 按字符串 id 取 DOM 再模拟点击 `data-bs-dismiss` 按钮，19 处调用。所有"特殊情况"分支都是从这三者不同步长出来的补丁。

**三是第三方运行时不在构建图里。** 详见 [ADR 0011](adr/0011-third-party-frontend-deps-enter-the-build-graph.md)。附带发现：flatpickr（72K）在两份 HTML 里被加载，但没有任何 `.ts`/`.tsx` 调用它，`index.css` 里还留着 25 行 `.flatpickr-*` 覆盖规则。纯死代码。

## 阶段

阶段之间是依赖关系。每个阶段结束时 `npm run typecheck`、`npm run lint`、`npm run build` 必须是绿的。

顺序为 P0 → P1 → P2 → P3 → P4 → P5。

P0 必须最先，不是因为它最痛，而是因为它决定别的能不能做：第三方依赖不进构建图，任何隔离渲染环境就永远要手抄一份清单。

P1 在 P2 之前。先修数据结构，组件形态自己会浮现；反过来先做组件，会照着现在这套散件 state 的形状去设计 prop，做完还得再改一遍。

### P0 运行时边界

两条并行、互不冲突的改动。

- 第三方依赖进 `package.json`：`bootstrap@5.3.2`、`chart.js@4.5.1`、`@popperjs/core@2.11.8`。新增 `src/runtime/vendor.ts` 作为唯一的第三方 JS 入口。字体与 RemixIcon 子集是本项目自建资产而非第三方包，移入 `src/assets/` 进构建图，不装 npm 包（上游完整包会把 8K 的图标子集换成 100K+）。删除 `public/vendor/`、`index.html` 的全部第三方标签、`.storybook/preview-head.html`。卸载 flatpickr。
- `src/index.css` 引入级联层，删除死规则与冗余规则。注意 `!important` 声明的层序是反的：见 ADR 0011「级联层的方向陷阱」。

### P1 数据层

新增 `src/data/`，每个资源一个模块（`channels.ts` / `tokens.ts` / `usage.ts` / `users.ts`），对外只暴露 `useChannels()` / `useUpdateChannel()` 这类 hook。`src/api/` 保持不变——它是纯传输层，只管 URL 与类型，这个分工是对的。

组件内不再出现 `useEffect` + `await api.*`。表单状态改为单个草稿对象加提交时 diff，不再是 N 个字符串 state。

先迁 `DashboardPage`（199 行，最小）验证形态，再迁 `ChannelsPage`（1097 行，收益最大）。

### P2 UI 原语层

新增 `src/ui/`：`Button` `Card` `Input` `Select` `Table` `Badge` `Alert` `Modal` `Dropdown`。见 [ADR 0013](adr/0013-ui-primitives-own-class-names.md)。

先做 `Button` 与 `Card`，覆盖 174 处调用点的大头。`Modal` 改为受控组件，`open` 是 prop 而非 DOM id，`components/modal.ts` 随之删除。

此阶段 Bootstrap 降级为过渡垫片：原语内部仍可拼 `bs` class，但外部只见 `<Button variant="primary" size="sm">`。

图表调用点在此阶段一并从 `window.Chart` 改为直接 `import`，`src/runtime/vendor.ts` 的全局桥随之拆除，Chart.js 转为按需加载（`vendor` chunk 预计再降约 200 kB）。

### P3 拆页面

新增 `src/features/`，页面逻辑按领域搬入（`channels/` `tokens/` `usage/` `plugins/` `users/`）。`src/pages/` 只剩路由装配，每个文件目标 80 行以内。

### P4 断奶

原语内部改为纯 token CSS，卸载 Bootstrap CSS（308K）与 `bootstrap` 包。`src/index.css` 收敛为 token 定义加少量全局重置。P0 引入的 `utility-overrides` 层随 Bootstrap 一起删除。

### P5 组件目录

到这一步才有真组件与真变体，`.stories.tsx` 从页面快照改为组件变体网格，设计系统同步成为可能。这是结果，不是目标。

## 不做的事

- 不引入 CSS-in-JS 或 Tailwind。见 ADR 0013「考虑过的方案」。
- 不在 P0 顺手升级 Bootstrap（5.3.2 → 5.3.8）。重构期间混入视觉漂移无法归因。
