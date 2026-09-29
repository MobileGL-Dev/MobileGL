# P10：sync / query / present 节奏

2026-09-29。依据 `docs/Disaggregated/notes/p10/`（计划与范围重划 [`PLAN-P10.md`](../../docs/Disaggregated/notes/p10/PLAN-P10.md)、裁定 `INTEGRATOR-DECISIONS-P10.md`）。本文只记实现所需的协议与行为；验证结果以 notes 里的运行证据为准。

## 范围

fence 轮询不再是往返（§1）；最后一个 class-C 与 split 覆盖（§2）；credit 与往返的测量记录（§3）。路线图原文里已做完或前提已变的项（client 铸造 query handle、present 1:1、credit 默认 1、无 present 负载的 `SEG_STAGE` 饥饿）见计划 §1。

## 1. fence 轮询从 server 的报告作答（A）

### 线上格式

- `EventKind` 追加 `kEventFenceSignaled = 5`，载荷 `EventFenceSignaledHead { EventHandle Fence; }`（8 字节）：server 看到该 client fence 已 signaled，每个 fence 至多一条。opcode 与控制协议修订号不变。

### server

- `ServerVerbSink` 按**创建顺序**记下尚未报告的 fence（`FenceCreate` 入队；被删的出队时跳过）。`ReportSignaledFences(flush)` 从最老的一个开始查，查到第一个未完成的就停（同一 context 的 GPU 按提交顺序完成），每个已完成的发一条 `kEventFenceSignaled` 并立即发布。
- `OnPresent` 在归还 present credit **之前**调用一次（状态查询，不提交）：credit 放行的 client 会立刻去等一两帧前的 fence，报告必须先于放行。
- apply 线程在**每个 drain 批次之后**调用（后端表 `GL.GetSyncStatus`，不提交）；**将要空闲**且还有未报告的 fence 时先调一次 `flush=true`——最老的 pending fence 改用 `GL.ClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, 0)`，这是两个后端都会提交所持命令的唯一后端无关方式——仍有未报告的就**限时停车 1 ms**（`kFenceIdlePollMs`）后再查，否则照旧无限期停车。`SEG_EVENT` 满时不报告（与既有"环满不 apply"同一规矩）。
- 与后端无关：两个后端都实现 `GL.FenceSync / GetSyncStatus / ClientWaitSync`。没有原生 fence 的（`FenceSync` 为空或返回空）视为已 signaled。

### client

- 事件排空时记下已报告的 fence（`{slot → gen}`，独立互斥锁：排空发生在持有 `g_fenceMutex` 的发射函数内部）；往返答"已 signaled"的也记下；`glDeleteSync` 时忘掉。
- `glClientWaitSync` / `glGetSynciv(GL_SYNC_STATUS)` 先做一次**轮询入口**（`ClientSession::PollEntry`：刷出已发布的记录、排空反向通道），然后：
  - 已知 signaled → 本地答 `GL_ALREADY_SIGNALED` / `GL_SIGNALED`；
  - 未知且调用不能等（timeout = 0、`glGetSynciv`）→ 本地答 `GL_TIMEOUT_EXPIRED` / `GL_UNSIGNALED`；同一 fence 连续第 N 次（`MOBILEGL_IPC_POLL_ESCALATE`，默认 64）改为一次真往返，计数归零；
  - 未知且调用可以等（timeout > 0）→ 真往返，语义不变。
- `MOBILEGL_IPC_POLL_ESCALATE=0`：每次轮询都往返，即 P10 之前的行为（A/B 臂）。设备丢失后一律走往返臂（其 DECLINED 答复是既有的 no-op）。
- 计数：`ReadFencePollCounters()`（本地作答、升级、往返、server 报告），经 `SplitRuntimePeek` 给车道。

### 门

- `FencePollScenario`（两后端 × 单进程 / Split / Spawn / Tcp）：零超时轮询循环有界结束（带 / 不带 flush 位，后者仅 split）；已 signaled 的 fence 轮询不产生回复槽记录；只轮询不发记录时，空闲 server 仍会报告后来完成的 fence（无升级）。单元控制 `RemoteClientControls.AnUnreportedFencePollCrossesOnlyEveryPollEscalateTimes` 钉住"第 N 次才过线"。
- red-once：`POLL_ESCALATE=0` → 200 条回复槽记录 / 200 次往返；去掉空闲复查 → Espryt 需 1 次升级、Magma 升级 15 次仍等不到（Magma 的 fence 不会自己提交）；client 关掉本地作答 → 单元控制 exit 163。

### 测量

`PacedFrameFenceWaitsBench`（手动开启，不设门）的配对结果在 [`A-FENCE-POLL.md`](../../docs/Disaggregated/notes/p10/A-FENCE-POLL.md)：Magma 走 tcp 时每帧耗时降 37%、fence 往返为 0；Espryt 在主机 llvmpipe 上没有收益，因为只有阻塞等待才能让 fence 可见。在 Adreno 830 上两后端都是 0 次往返、598 / 598 次本地作答（[`C-MEASUREMENTS.md`](../../docs/Disaggregated/notes/p10/C-MEASUREMENTS.md) §1）。

## 2. 最后一个 class-C 与 split 覆盖（B）

- 发射表的 `SetSwapInterval` 槽 = server 转发器（与 `eglSwapInterval` 经 `SurfaceControlOp::SetSwapInterval` 的同一路）；`SetSwapInterval_Unmigrated` 与 `BackendObject_Remote` 的覆盖删除。class C 为空：69 已实现 / 0 未迁移。单元控制：`ServerLoopTest.TheEmitTableSwapIntervalSlotCrossesAsOneDispatchedFrame`（经表调用，恰好一个控制帧到达）。
- caps mirror 采纳 server 快照时，`GL_KHR_parallel_shader_compile` 按 **client** 的 `MOBILEGL_ASYNC_SHADER_COMPILE` 重算：编译池与 `GL_MAX_SHADER_COMPILER_THREADS_KHR` 都在 client。red-once：去掉重算 → 两后端 Tcp 的 `AsyncOff` 扩展串用例红。
- 登记（全部进门）：`AsyncCompileScenario`（`AsyncOn.` / `AsyncOff.` / `OptimisticShaderStatus.`）与 `XfbPrimitiveQueryScenario` × 两后端 × Split / Spawn / Tcp；`PrimitivesGeneratedNoXfbScenario` 只在 Magma（`PrimGen.` 三臂，`PrimGenReroute.` 仅 Split / Spawn：tcp 的共享 server 读不到改道开关）。red-once：client 查询结果加一 → 101 条里 49 条红（全部 XfbQuery / PrimGen / PrimGenReroute），同名 monolith 条目仍绿。

## 3. 测量（只记录）

见 [`C-MEASUREMENTS.md`](../../docs/Disaggregated/notes/p10/C-MEASUREMENTS.md)，包括真机 fence bench、设备上 TCP loopback 的 `PRESENT_CREDIT` 1/2/3、以及主机 retrace 往返普查。在 loopback 上 credit 没有噪声以外的影响（rd12 51.4 / 51.6 / 52.4 fps），默认值保持 1。

## 不变量

- G1：新代码全在 `MOBILEGL_BUILD_DISAGGREGATED` 下（`MG_Remote`、`Config` 的 IPC 段）。
- `EventKind` 只追加；控制协议修订号不变（3）。
