# 当前阶段：P10 已收官（2026-09-29）；下一阶段 P11 / P8 待排

> **更新 2026-09-29**。这一页只有摘要；裁定明细、测量与证据在 [`notes/p10/README.md`](notes/p10/README.md)（计划与范围重划 [`PLAN-P10.md`](notes/p10/PLAN-P10.md)）。上一阶段 P9 同日收官，见 [`notes/p9/README.md`](notes/p9/README.md)。

## 目标

split 下的同步对象、查询与帧节奏不再逐次往返：fence 轮询不再等 server，最后一个手写 class-C 清掉，credit 在带延迟的链路上有配对数据。

## 完成情况

| 项 | 状态 |
|---|---|
| A：`glClientWaitSync` / `glGetSynciv` 由 server 报告本地作答 | ✅ 两后端；`kEventFenceSignaled`（EventKind 5），present 时在归还 credit 前也报告；`MOBILEGL_IPC_POLL_ESCALATE`（64，0 = A/B 对照）；零超时轮询有界退出 |
| A 的收益 | 主机 Magma·tcp 每帧 −37%、fence 往返 598 → 0；Adreno 830 上两后端都是 0 次往返、598 / 598 次本地作答；主机 Espryt（llvmpipe）无收益，属于驱动行为 |
| B：最后一个 class-C、split 覆盖 | ✅ `SetSwapInterval` 槽改为转发，class C 清零；`GL_KHR_parallel_shader_compile` 由 client 决定；AsyncCompile / XfbPrimitiveQuery / PrimitivesGeneratedNoXfb 在 split 各臂进门 |
| C：`PRESENT_CREDIT` 1/2/3 | 设备 tcp loopback 在噪声内（rd12 51.4 / 51.6 / 52.4 fps），默认保持 1；跨机 +26% 仍以 P12 为准 |
| 主机门与单进程构建二进制不变（G1） | ✅ 符号增 0 减 0，`.text` `0xa52203`；全部车道绿（数字见裁定 ID-P10-12） |

## 收官（2026-09-29）

- 契约 [`MG_Remote/CONTRACT-P10.md`](../../MobileGL/MG_Remote/CONTRACT-P10.md)；`EventKind` 追加 1 个；控制协议修订号不变。
- 转入 [`notes/DEBTS.md`](notes/DEBTS.md)：fp64 宣告按 server 环境（同类的 parallel-compile 已修）；loopback 上的传输停车（性能）；Magma 不支持 `eglSwapInterval`（dev，已开独立任务）。
- 收官审查沿用 P12 的做法：不派 agent / Codex，契约断言由本人对照代码核过，G1 同机复核。

## 下一步

按 [`ROADMAP.md`](ROADMAP.md)：P11（persistent map 与 ≥ 16 MiB 采纳，同机臂专属；前置开放问题 3）；P8（monolith 跑道）；P6.5 残余与 P3b/P4b 余项并行。

**阻塞**：没有需要决策的事项。
