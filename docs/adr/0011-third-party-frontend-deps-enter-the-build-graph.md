# 第三方前端依赖走 npm 与构建图，不走 HTML 标签

状态：accepted

Bootstrap、Chart.js 及其对等依赖 Popper 作为 `package.json` 依赖安装并锁定精确版本，由 `src/runtime/vendor.ts` 单点 `import`。`index.html` 的 `<head>` 不再包含任何第三方 `<link>` 或 `<script>`。第三方 CSS 由 `src/index.css` 用 `@import … layer(vendor)` 单点引入。

字体子集与 RemixIcon 子集不适用本条：它们是本项目自建的裁剪资产，移入 `src/assets/` 进构建图即可，不装上游 npm 包。

## 背景

对齐之前，第三方运行时从 `public/vendor/` 经 `index.html` 的裸标签加载，`.storybook/preview-head.html` 手抄了同一份清单，文件头注释写着"改动 `../index.html` 的 head/脚本时，同步改这里"。

## 理由

**一份手工同步的清单会漂移，两份必然漂移。** 注释是补丁不是机制。任何新的隔离渲染环境都要抄第三份。

**不在构建图里的依赖不受版本约束。** `public/vendor/` 下的文件版本只写在文件头注释里：Bootstrap v5.3.2、Chart.js v4.5.1、flatpickr v4.6.13。没有锁文件、没有完整性校验、没有升级路径，替换一个文件不会在任何检查里留下痕迹。

**死依赖只有在构建图里才发现得了。** flatpickr 被两份 HTML 加载了 72K 的 JS 与 CSS，没有任何 `.ts`/`.tsx` 调用它，`index.css` 还留着 25 行 `.flatpickr-*` 覆盖规则。裸标签没有引用关系可查，因此它活了下来。进构建图后，"没有 import" 就是"可以删"。

## 代价

**全局变量的过渡期。** 现有调用点通过 `window.bootstrap`（`components/modal.ts` 与 Bootstrap 的 `data-bs-*` 声明式 data-api）和 `window.Chart`（两处图表）取用这些库。`src/runtime/vendor.ts` 显式把它们挂上 `window`，这是刻意保留的桥。本阶段的目标是把依赖搬进构建图且不改变任何运行时行为；调用点在 UI 原语层落地时改为直接 `import`，届时这座桥拆除。

**级联层的方向陷阱。** 把 Bootstrap 放进 `layer(vendor)`、应用样式放进 `layer(app)`，对普通声明是 `app` 胜出（这正是能去掉大量 `!important` 的原因），但对 `!important` 声明层序是**反的**，`vendor` 胜出。Bootstrap 的工具类（`.bg-light`、`.rounded-pill`、`.text-*` 等）按设计全部带 `!important`，因此覆盖工具类的应用规则必须放进一个**声明在 `vendor` 之前**的层。层序因此是 `@layer utility-overrides, vendor, app;`。这一点不直觉，必须写在样式表里，否则下一个人会把它"整理"成坏的。

同时确认：Bootstrap 5.3.2 的语义工具类本身已经全部走 `--bs-*` 变量（`.text-primary{color:rgba(var(--bs-primary-rgb),var(--bs-text-opacity))!important}`），因此 `index.css` 里那批"强制走 CSS 变量"的工具类重声明是冗余的，其中 `.text-primary` 的重声明还丢掉了 `--bs-text-opacity`，是负收益。随本决策一并删除。

**首屏字节的重新分配。** 裸标签时代 Bootstrap 与 Chart.js 是两个独立 `<script defer>`，与应用代码分开缓存。进构建图后它们默认并入主 chunk，因此 `vite.config.ts` 显式切出 `vendor`（Bootstrap + Popper + Chart.js）与 `react` 两个 chunk，恢复独立缓存边界。当前产物：`vendor` 283 kB（gzip 93 kB）、`react` 232 kB（gzip 74 kB）、应用入口 59.6 kB（gzip 21 kB）。

其中 Chart.js 仍是**急切**加载的，因为图表调用点还在读 `window.Chart`，而这座桥必须在组件渲染前就位。只有使用量页面和渠道详情需要图表。调用点改为直接 `import` 之后（UI 原语层阶段），Chart.js 随之改为按需加载，`vendor` chunk 再降约 200 kB。这笔账记在这里，不在本阶段解决——本阶段的约束是不改变运行时行为。

## 考虑过的方案

- **保留裸标签，加一个校验脚本比对两份 HTML**：用测试维护本不该存在的重复，且不解决版本锁定与死依赖发现。
- **把 RemixIcon 与字体也换成上游 npm 包**：会把 8K 的图标子集换成完整包的 100K+ woff2，为了形式统一付真实体积，不划算。
- **在 P0 顺手升级 Bootstrap 到 5.3.8**：重构期间混入视觉漂移，出问题无法归因是搬迁还是升级。版本锁死在原有的 5.3.2。
