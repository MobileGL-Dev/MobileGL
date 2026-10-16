# P10 计划：sync / query / present 节奏（重划范围）

> 2026-09-29，基于 `feat/disaggregated@de33d401`（P9 收官头）。路线图行（原文在 [`README.md`](README.md)）写于 09-22，与树的现状逐项核对如下；**先读 §1**。

## 1. 路线图每一项的现状

| 路线图写的 | 现状（核对） | 处置 |
|---|---|---|
| 轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE` | **没做**：`glClientWaitSync` / `glGetSynciv` 都是 `kWaitReply`（`FenceWait` op 9、`FenceStatus` op 8），每次都要等 server 把它之前**所有**记录 apply 完再回复——run-ahead 下等于一次全管线同步。FCL 里的 Minecraft 稳态每帧约 1 次（P12 验收 A）；TCP 上再加一个 RTT。旋钮 0 处 | **A**（核心） |
| DirectGLES fence 完成度逐 fence 退休；非 present fence tick | 线上**不依赖**帧水位：server 每批 apply 后 `RetireThrough(appliedSeq)`（`ServerLoop.cpp`），`SEG_STAGE` 不会在无 present 负载里饿死。帧水位只喂 Espryt 自己的 buffer 池回收，与 monolith 同 | 不单独做；A 需要"空闲时也检查 fence"，那一点就是线上的非 present tick |
| `present` 1:1、credit 默认 1、client 铸造 query handle | 已是 | 关闭 |
| `PRESENT_CREDIT > 1` 从未量过 | 跨机 Wi-Fi 已量（P12：rd12 稳态 1→2 +26%、2→3 +4%，[`../p12/CROSSHOST-ACCEPTANCE.md`](../p12/CROSSHOST-ACCEPTANCE.md)）；**TCP loopback 未量**（[`../perf-five-arm-20260928`](../perf-five-arm-20260928/README.md) 建议 #2 "未实测，待 A/B"） | **C** |
| roundtrip 计数器与输入延迟直方图 | 部分已有：`P65LinkMetrics` 逐 op 回包等待 + 回复延迟直方图（P12）；trace 稳态回包等待 0（五臂报告）。缺：present credit 等待的直方图、逐 trace 的往返普查记录 | **C**（记录）；直方图若缺则补 |
| `SetSwapInterval`（最后一个手写 class-C） | `eglSwapInterval` 实际走 `BackendObject_Remote::SetEGLSwapInterval` → 控制帧 `SurfaceControlOp::SetSwapInterval`，跨进程可用；发射表槽 `SetSwapInterval_Unmigrated` 是永远到不了的具名 Fatal | **B**：槽改为同一转发，class-C 清零 |
| 门：query / XFB / `AsyncCompile` 场景在 split 下绿 | `SyncWire`、四个 XFB 场景已登记 split；**`AsyncCompileScenario`、`XfbPrimitiveQueryScenario`、`PrimitivesGeneratedNoXfbScenario` 未登记任何 split 臂** | **B** |
| 门：零 timeout 轮询有界退出 | 今天靠每次往返保证进度；A 之后由升级规则保证 | **A** 的门 |

## 2. 工作包

### A — fence 轮询本地作答（核心）

- **server**：apply 线程在每个 drain 批次之后、以及空闲等待时（有未报告的 fence 就限时停车，默认 1 ms 再查）按创建顺序用后端表 `GL.GetSyncStatus` 查尚未报告的 fence；新近 signaled 的发一条 `kEventFenceSignaled{handle}`（EventKind 只追加）。后端无关（两个后端都实现 `GL.FenceSync/GetSyncStatus/ClientWaitSync`）。
- **client**：排空事件时记下已 signaled 的 fence；`glClientWaitSync` / `glGetSynciv(GL_SYNC_STATUS)`：已知 signaled → 本地答 `GL_ALREADY_SIGNALED` / `GL_SIGNALED`；未知且 timeout = 0 → 先 publish + 敲门铃（轮询入口是门铃点；`GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 flush），排空事件，仍未知则本地答 `GL_TIMEOUT_EXPIRED` / `GL_UNSIGNALED`；同一 fence 连续 N 次（`MOBILEGL_IPC_POLL_ESCALATE`，默认 64；0 = 关闭本地作答，即 A/B 控制）无进展 → 升级成一次真往返；timeout > 0 且未知 → 真往返（语义不变）。
- **门**：零 timeout 轮询循环在 split / spawn / tcp × 两后端有界退出；轮询不产生回复槽记录（计数门，red-once = `POLL_ESCALATE=0`）；`SyncWireScenario` 不变绿；停车的 server 仍会报告后来才 signaled 的 fence（red-once = 去掉空闲检查）。

### B — split 覆盖与最后一个 class-C

- `AsyncCompileScenario`、`XfbPrimitiveQueryScenario`、`PrimitivesGeneratedNoXfbScenario` 登记到两个后端的 Split / Spawn / Tcp 臂，红的修（两后端都修）。
- `SetSwapInterval` 发射表槽改为转发，删 `SetSwapInterval_Unmigrated`。

### C — 测量（A 落地之后）

- 红米 `2f7cbe2e`：`PRESENT_CREDIT` 1 / 2 / 3 × {spawn+tcp localhost, WSL → 手机 Wi-Fi}，rd12 与 openra，每档 ≥ 3 次交错；`POLL_ESCALATE` 0 / 64 的配对。只记录、不设门（性能只记录，用户 09-08）。
- 主机 retrace 语料 `MOBILEGL_PIPE_STATS=1`：逐 trace 的稳态回包等待按 op 列表，draw / state / upload 路径应为 0。

## 3. 门与规矩

同 P9（[`../p9/HANDOFF-P9.md`](../p9/HANDOFF-P9.md) §5–§6）：`~/w7/notes/p9/gate.sh` 全套 + verify 构建三车道；G1 同机比较；改 wire 按规矩（EventKind / opcode 只追加）；每个新门 red-once；设备只用红米；不拉 LFS；提交一行、无署名尾注。
