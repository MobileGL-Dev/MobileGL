# P9 — 反向通道（已收官，2026-09-29）

> **2026-09-29 收官**。协议与行为：`MobileGL/MG_Remote/CONTRACT-P9.md`；裁定 [`INTEGRATOR-DECISIONS-P9.md`](INTEGRATOR-DECISIONS-P9.md)（ID-P9-1..12）；包报告 [`W1-PACK-BUFFER-READBACK.md`](W1-PACK-BUFFER-READBACK.md)、[`W2-REMINT.md`](W2-REMINT.md)；范围重划与交接 [`HANDOFF-P9.md`](HANDOFF-P9.md)。
>
> | 包 | 结果 |
> |---|---|
> | W1 PACK-PBO 回读 | 读进 pack buffer 走 op 82/83，client 不等、标 GPU 写过；server 经 buffer 写入口落行（两后端）；UNPACK 源在 split 下先同步；F1 |
> | W2 纹理重铸 | 不需要拉取协议：Espryt 读回驱动上存在的层 + store，Magma GPU→GPU；修"只定义未上传的层"的 Fatal；`OnTexturePullRequest` 删；`TextureRemintPullScenario` 14 例 × 两后端 × 四臂 |
> | W3 日志分级 | 转发带级别、队列 + 发送线程：≤WARN 有损计数、ERROR 无损限速、FATAL 有界；`OnLog` 删；F2 |
> | W4 事件量 | 不做：全语料 0 丢弃、0 阻塞（ID-P9-7） |
> | W5 `OnGlError` 有序 | 保持 P5e 放宽，写进契约 §3 |
>
> 门（集成头）：G1 符号增 0 减 0、`.text` 0xa52203；fatal census 79、link ratchet 171；unit、split / spawn / tcp、三条 Magma 双进程车道、integration-gpu 两臂全绿（数字见 ID-P9-12）；verify 构建三车道全绿；主机 retrace 230 / 236（6 项为宿主 llvmpipe JIT 崩溃，monolith 同挂）。未做真机。
>
> 下面是该阶段在路线图上的原始范围与出口门（2026-09-24 从 `ROADMAP.md` 移入，原文照录）。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- 异步 reply 语义，建立在 P6.5 的消息式 reply 之上；2 MiB reply cap 与 chunked readback（ID-47）；PACK-PBO 回读 fire-and-forget（ID-57）；武装剩余回调（`OnTextureWriteback`、`OnTexturePullRequest`、`OnMipLevelsGenerated`、`OnLog` 分级）；`OnGlError` 有序；纹理拉取缓解 + 终止符；XFB 命名空间（`DeleteTransformFeedback`）。
- 门：回读 / XFB 场景在 split 下绿；`TextureRemintPullScenario`；拉取计数逐 trace 发布；两条故障注入。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P9** 反向通道
- **状态**：待排，**P6.5 之后**（2026-09-22 重定范围）
- **落地什么 / 范围**：**异步 reply 语义**，机制建立在 P6.5 的消息式 reply 之上（不再造 `SEG_REPLY` slot 池——池今天就有，stream 链路会删它）；2 MiB reply cap 与 chunked readback（契约 §10.2.3 从 P6 改判至此，ID-47）；PACK-PBO 回读 fire-and-forget + client 侧 `MarkGpuWritten`（ID-57，P6 未做）；十个回调里未武装的四个：`OnTextureWriteback`、`OnTexturePullRequest`、`OnMipLevelsGenerated`、`OnLog` 分级（`OnXfbScatterReady` 是零生产者、零消费者、无 EventKind 的死声明——随 P3b/P4b 删除，`kMGPipeCallbackCount` 10→9；`ClientSession.cpp:429`；`OnBufferWriteback` / `OnGpuWritten` / `OnSurfaceChanged` / `OnGlError` / `OnCapsInvalidated` 已武装）；`OnGpuWritten` 收窄；`OnBufferWriteback` 批处理 + epoch 排序；`OnGlError` 有序（CONTRACT-P5C §4.2）；纹理拉取四条缓解 + 终止符；XFB 命名空间（`DeleteTransformFeedback` 的 server 侧泄漏——按裁定无行、最后两个 class-C 之一）。~~`SEG_EVENT` 溢出策略~~ 机制已由 P5e 落地（`ARCHITECTURE.md` §11.7），剩余的"不排空对端杀宿主"归 Ph
- **验收门 / 证据**：回读 / XFB 场景在 split 下绿；`TextureRemintPullScenario`（含无解用例）；拉取计数逐 trace 发布；两条故障注入
