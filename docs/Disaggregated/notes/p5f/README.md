# P5f — 一切状态上 wire（2026-09-20 收官）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 目标：两个角色之间除 `SEG_CMD` / `SEG_STAGE` / `SEG_REPLY` / `SEG_EVENT` / caps 快照 / 控制帧之外零交换，判据由人读审计改为**机器**（双块演练：`inproc` 下两个角色各自一份 `PipeInputs`）。计划与设计见 [`P5F-WIRE-COMPLETENESS.md`](P5F-WIRE-COMPLETENESS.md)（2026-09-24 从上层移入）。
- 包：f0 普查、f1 双块、fc 控制帧、fe Espryt 字段、fm Magma record consumers、fs 静态世代 / 角色 / liveness、fr registry、fv 反向回调。
- 结果：字段归属 **41 RECORD_SUPPLIED / 6 APPLIER_DERIVED / 0 BARRIER_PULLED / 16 FATAL**；strict / dualblock marker 表均空；双后端逐帧 `rsp=0`；unit 2310、GPU 1377 零失败；G1 恒等；Redmi 六个 clean-boot 臂 60 PASS / 6 skip / 0 failed。验收代码 WSL `cfca93c7`（Windows `c42577a4`）。收口 [`close-report.md`](close-report.md)、审查 [`close-review.md`](close-review.md)、设备 [`device-report.md`](device-report.md)。
- 之后同目录还记了两件事：**Magma run-ahead**（`194382c9`，[`magma-runahead.md`](magma-runahead.md)）与 Magma inproc 实际游戏修复 / RD32 性能修复；以及 **CI / inproc 功能补齐**（2026-09-21 收官，`e1bf3677`，交接 [`HANDOFF-CI-INPROC-20260920.md`](HANDOFF-CI-INPROC-20260920.md)）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5f** 一切状态上 wire
- **状态**：✅ **已收官（2026-09-20）**；验收代码 Windows `c42577a4` / WSL `cfca93c7885f`
- **落地什么 / 范围**：f0 普查、f1 双块、fc 控制帧、fe Espryt 字段、fm Magma record consumers、fs 静态世代/角色及 liveness、fr registry、fv 反向回调均已合并。字段归属 **41 RECORD_SUPPLIED / 6 APPLIER_DERIVED / 0 BARRIER_PULLED / 16 FATAL**；strict / dualblock 预期 marker 集合均空。控制帧与静态量工作已完成，不再列为 P6 待实现项
- **验收门 / 证据**：unit **2310 = 2300 PASS + 10 skip**；GPU **1377** 零失败；ordinary / strict split **180 = 176 PASS + 4 skip**；双块 **212 = 206 PASS + 6 精确具名 skip**；双后端 RSP 门 **2 PASS / 0 skip**、逐帧 rsp=0。G1 `.text` 10806051、symbols 27815 恒等且 0/0/0/0；G2 3016 同名；G14 3646→3744 无删名。逐包与收官修复均有 red-once；Claude 异族终审 P1/P2 已修；Redmi 六 clean-boot 臂 **60 PASS / 6 skip / 0 failed**、36 个窗口 rsp=0。详见 [`close-report`](close-report.md)

## 状态注记（原 `ROADMAP.md` 页首）

> **P5f 后续 Magma run-ahead 已完成**（`194382c9`，2026-09-20）：主机真实排队、credit、负控及 Vulkan 同步验证通过，Redmi 同包 Magma 开/关与 GLES 三臂 FCL 世界实测通过。见 [报告](magma-runahead.md)。P6 / P7 全阶段状态不因此改变。

## 里程碑记录（原 `ROADMAP.md`）

- **P5f 出口（2026-09-20）**：双块和 strict 空棘轮、逐帧 rsp=0、零 BARRIER_PULLED、主机全门、异模型族审查修复和 Redmi 六 clean-boot 臂全部完成；P6 前提解除，实施未开始。
- **CI / inproc 收尾出口（2026-09-21）**：`feat/disaggregated` 主产物拆分编译 + inproc 主验收全绿——unit 2328 零失败、`integration-gpu` 双 transport 各 1455 零失败、retrace 77 例 monolith 77/77 且 inproc 的唯一间歇项（`bsl-esc-menu-854` DirectVulkan，lavapipe 固有 texture-handle 注册停顿越过旧 30 s barrier 预算）在预算修正为 120 s 后隔离 10/10 绿、18 项 CI 门全绿、双 ABI APK 与 AVD 生命周期验证通过；合入头 `e1bf3677`，最终矩阵以远端 CI 为准。

## 本目录

| 文件 | 内容 |
|---|---|
| [`close-report.md`](close-report.md) | P5f 收口报告 |
| [`close-review.md`](close-review.md) | P5f 阶段末跨模型族审查 |
| [`device-report.md`](device-report.md) | P5f Redmi 设备门 |
| [`f0-dualblock-recon.md`](f0-dualblock-recon.md) | P5f / f0 — f1 双块机制设计侦察（只读普查） |
| [`f0-egl.md`](f0-egl.md) | f0-egl — P5f §2.3 EGL / surface 控制面普查（只读） |
| [`f0-env.md`](f0-env.md) | P5f / f0 — 构建与门禁机制侦察（实现代理 cheatsheet） |
| [`f0-fields.md`](f0-fields.md) | f0 普查 · §2.1 BARRIER_PULLED 15 字段逐字段载体判定 |
| [`f0-magma.md`](f0-magma.md) | f0 普查 · §2.2 Magma（DirectVulkan）跨角色读写 |
| [`f0-registry.md`](f0-registry.md) | f0 普查子项：§2.5 registry 重键普查（`MGPipeFrontendKeyedRegistryScope`） |
| [`f0-reverse-channel.md`](f0-reverse-channel.md) | f0-reverse-channel — P5f §2.6 反向通道逐站点普查（只读） |
| [`f0-statics.md`](f0-statics.md) | P5f f0 普查（只读）：进程级静态量中语义属于 per-context / per-session 的成员 |
| [`f1-report.md`](f1-report.md) | P5f / f1 — 双块演练机制：落地报告 |
| [`fc-contract-draft.md`](fc-contract-draft.md) | fc — 控制面帧契约草稿（P5f 包 fc；CONTRACT-P5F.md 本体留给集成收口） |
| [`fc-report.md`](fc-report.md) | P5f / fc — EGL/surface 控制面帧化：落地报告 |
| [`fcl-e2e-report.md`](fcl-e2e-report.md) | P5f FCL / Minecraft 端到端补测 |
| [`fe-report.md`](fe-report.md) | P5f / fe — Espryt 字段退役报告 |
| [`fm-report.md`](fm-report.md) | P5f fm — Magma 跨角色读写退役 |
| [`fr-report.md`](fr-report.md) | P5f / fr — registry 身份边收口 |
| [`fs-report.md`](fs-report.md) | P5f fs — 静态状态世代与 server liveness |
| [`fv-report.md`](fv-report.md) | P5f fv — 反向通道归属与遗留对象边界 |
| [`HANDOFF-CI-INPROC-20260920.md`](HANDOFF-CI-INPROC-20260920.md) | CI / inproc 功能补齐交接（2026-09-20，用户要求停止） |
| [`HANDOFF-P5F.md`](HANDOFF-P5F.md) | P5f 交接：已全部收官 |
| [`magma-inproc-fix.md`](magma-inproc-fix.md) | Magma inproc 实际游戏修复 |
| [`magma-rd32-performance-fix.json`](magma-rd32-performance-fix.json) | 脚本 / 数据 |
| [`magma-rd32-performance-fix.md`](magma-rd32-performance-fix.md) | Magma RD32 inproc 性能修复 |
| [`magma-runahead.md`](magma-runahead.md) | Magma run-ahead |
| [`P5F-WIRE-COMPLETENESS.md`](P5F-WIRE-COMPLETENESS.md) | P5f — 一切状态上 wire：跨进程前的最后一次归零 |
| [`rd32-fourway.json`](rd32-fourway.json) | 脚本 / 数据 |
| [`rd32-fourway.md`](rd32-fourway.md) | Redmi RD32 四组合性能记录 |
| [`wave2-report.md`](wave2-report.md) | P5f 波次 2 集成报告 |
