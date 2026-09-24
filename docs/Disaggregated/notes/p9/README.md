# P9 — 反向通道（待排，P6.5 之后）

> 尚未开工。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- 异步 reply 语义，建立在 P6.5 的消息式 reply 之上；2 MiB reply cap 与 chunked readback（ID-47）；PACK-PBO 回读 fire-and-forget（ID-57）；武装剩余回调（`OnTextureWriteback`、`OnTexturePullRequest`、`OnMipLevelsGenerated`、`OnLog` 分级）；`OnGlError` 有序；纹理拉取缓解 + 终止符；XFB 命名空间（`DeleteTransformFeedback`）。
- 门：回读 / XFB 场景在 split 下绿；`TextureRemintPullScenario`；拉取计数逐 trace 发布；两条故障注入。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P9** 反向通道
- **状态**：待排，**P6.5 之后**（2026-09-22 重定范围）
- **落地什么 / 范围**：**异步 reply 语义**，机制建立在 P6.5 的消息式 reply 之上（不再造 `SEG_REPLY` slot 池——池今天就有，stream 链路会删它）；2 MiB reply cap 与 chunked readback（契约 §10.2.3 从 P6 改判至此，ID-47）；PACK-PBO 回读 fire-and-forget + client 侧 `MarkGpuWritten`（ID-57，P6 未做）；十个回调里未武装的四个：`OnTextureWriteback`、`OnTexturePullRequest`、`OnMipLevelsGenerated`、`OnLog` 分级（`OnXfbScatterReady` 是零生产者、零消费者、无 EventKind 的死声明——随 P3b/P4b 删除，`kMGPipeCallbackCount` 10→9；`ClientSession.cpp:429`；`OnBufferWriteback` / `OnGpuWritten` / `OnSurfaceChanged` / `OnGlError` / `OnCapsInvalidated` 已武装）；`OnGpuWritten` 收窄；`OnBufferWriteback` 批处理 + epoch 排序；`OnGlError` 有序（CONTRACT-P5C §4.2）；纹理拉取四条缓解 + 终止符；XFB 命名空间（`DeleteTransformFeedback` 的 server 侧泄漏——按裁定无行、最后两个 class-C 之一）。~~`SEG_EVENT` 溢出策略~~ 机制已由 P5e 落地（`ARCHITECTURE.md` §11.7），剩余的"不排空对端杀宿主"归 Ph
- **验收门 / 证据**：回读 / XFB 场景在 split 下绿；`TextureRemintPullScenario`（含无解用例）；拉取计数逐 trace 发布；两条故障注入
