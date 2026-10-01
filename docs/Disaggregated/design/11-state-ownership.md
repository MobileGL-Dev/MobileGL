# 状态归属：session / context / share group / thread current（P14 设计）

> 为 anland 内置统一 server 而做。动因与否定项见 [`../notes/anland/handoff-legacy-mobilegl-unified.md`](../notes/anland/handoff-legacy-mobilegl-unified.md)：不许用「每应用 fork 一个渲染进程」掩盖全局状态归属。本文行号对 `2f64ede8`（+ `4156ca6e` 文档提交），代码移动后以符号名检索。

## 结论

今天整条栈的正确性建立在一个等式上：**一个进程 = 一个 server = 一个 session = 一个 context = 一个 share group**。`ServerLoopInstance`/`ServerSessionInstance`/`g_applyThreadKey`/`pGLContext`/`pEGLContext`/`g_Display`/`g_Context`/`g_Surface`/`g_applier`/`gMGPipeContext` 全是单实例。`2f64ede8` 的 `--serve --max-sessions` 用 fork 把等式搬进每个 worker 进程，绕开而不是解决归属；anland 统一 server 要求同进程多 session，等式必须在代码里真正拆开。

拆成四层归属：

| 层 | 拥有什么 | 今天的错误位置 |
|---|---|---|
| **Session** | SHM rings/segments、staged stores、reply/event 路由、failure、对象 handle 空间、PipeApplier、apply thread、backend runtime 绑定 | `ServerSessionInstance()`/`ServerLoopInstance()`/`g_applier` 单例（`ServerSession.cpp:535`、`ServerLoop.cpp:1191`、`PipeApply.cpp:484`） |
| **Context** | 独立 GLContext：bindings/render state/errors、VAO/FBO/TF/pipeline/query（容器对象不共享）、cache/scratch/owner/generation、native EGLContext | 唯一 `pGLContext`（`MG_State/GLState/Core.cpp:1565`）；`eglCreateContext` 不建 GL 状态（`MG_State/EGLState/Core.cpp:625-700`）；后端唯一 `g_Context`（`DirectGLES.cpp:16184`） |
| **Share group** | buffer/texture/renderbuffer/sampler/shader/program/sync 的对象 registry 与名称分配；后端 twin/native 对象映射 | 混在唯一 `pGLContext` 各 State 成员里（`MG_State/GLState/Core.h:610-692`）；`shareCtx` 只存不用（`MG_State/EGLState/Core.cpp:650`） |
| **Thread current** | TLS 指向 (session, context)；owner 关系不进 TLS，context 可跨线程移交；apply 显式设置 runtime scope | `m_threadCurrents`/`m_contextOwners` 已有雏形但绑死单实例表（`MG_State/EGLState/Core.h:283-284`）；`g_applyThreadKey` 进程级（`ServerLoop.h:129`） |

关闭一个 session 不得破坏另一个；同名 handle 在不同 session/share group 互不干扰；shared contexts 看到同一份共享对象。

## wire 变更

1. **逐记录 context 身份**。`MGPWireRecHeader` 8 B（`generated/PipeWire.inc:35-65`）不含身份；`MGPApplierReset::ContextSerial` 只做断言（`MGPipeTypes.h:1953-1960`）。设计：context 族记录（`PipeTables.inc:32-45` 的 71 个）在 header 与 payload 之间插入 `Uint32 ContextSlot`（flags 加 `kRecHasContext` 位，生成器在 `PipeCalls.def` 的族声明处统一加，不逐 verb 手改）。`ContextSlot=0` 保留给「session 默认 context」，兼容只有一个 context 的旧形状。screen 族记录操作的对象 handle 本身属 share group，server 经「记录所属 context → share group」解析，不另加字段。
   - 为什么不用「每 context 一条 ring」：ring 是 session 的资源/信用单位，每 context 一条会把 swap pacing、反向事件、segment 生命周期全部复制一份；逐记录身份 4 B 解决同一问题。
   - 交错正确性：多线程 client 的记录在一条 ring 上交错，apply 以记录自带的 context 为准，不得依赖「最后一个 MakeCurrent」。不可拆分批次（一次 emit 的多记录）同 context 才许合并，生成器在 batch 边界断言。
2. **EGL 生命周期上 wire**。`eglCreateContext`/`eglDestroyContext` 今天是纯前端（`EGLImpl.cpp:327-333`、`:428-434`）。`SurfaceOpKind`（`protocol.fbs:240-267`）新增 `CreateContext{contextSlot, shareWithSlot, api, version, flags}` 与 `DestroyContext{contextSlot}`；`MakeCurrent` 的 `context` 字段从「恒 0/忽略」变成真实 slot。frontend 仍在本地建 EGL 元数据，wire 只负责让 server 建对应 native context 与 share group。
3. **反向事件带 context**。`EventFenceSignaledHead`（`ServerSession.cpp:1324-1338`）、GL error、surface-changed 事件头加 `ContextSlot`（surface 事件已有 surface handle，context 便于 client 直接派发）。fence handle 属 context 的 share group，client 按 (slot, gen) 原样核对。
4. **wireFingerprint 随以上变更 bump**；两端同 commit + 显式 `-DMOBILEGL_BUILD_STAMP=<commit>` 重建，不接受 drift（交接文档「构建产物」节）。

## 前端（MG_State / MG_Impl）

1. **拆 GLContext**。`MG_State/GLState/Core.h:77-698` 的 `GLContext` 拆成 `ShareGroupState`（buffer/texture/renderbuffer/sampler/shader/program/sync 的对象 store + 名称分配）与 `GLContext`（bindings、error、viewport/scissor、render state、VAO/FBO/TF/pipeline/query store、texture unit 绑定、current program）。容器对象（VAO/FBO/TF/pipeline/query）**不共享**，留在 per-context；这是 GL 4.6 §5 的口径，不要为省事把 FBO 放进 share group。
2. **per-EGLContext 实例化**。`EGLState::CreateContext` 建 `GLContext`，`shareCtx != EGL_NO_CONTEXT` 时挂到对方 `ShareGroupState`（首次共享时把对方的对象 store 迁移进新 share group，双方同指）。`eglMakeCurrent` 切 TLS 的 (session→) GLContext 指针；`eglDestroyContext` 只在 owner 不存在时允许（沿用 `MG_State/EGLState/Core.cpp:714-718`），销毁 context 不销毁 share group，最后一个 context 离开时 share group 对象全删。
3. **Tracker 判据改显式身份**。`Pipe/Tracker.h:306-309` 的指针比较换成 (session, contextSlot)；换 context 的首个 verb 重推完整状态的语义不变。`MGPipeTrackerInstance()`（`Tracker.h:956-963`）从单例改为 per-context（tracker 缓存的是 per-context 推送快门）。
4. **eglMakeCurrent 流程**：`EGLImpl::MakeCurrent` 除调 `backendObject->MakeEGLCurrent` 外，先切 TLS GLContext 再发 `SurfaceOp::MakeCurrent{contextSlot}`；client 线程后续的 context 族记录都带这个 slot。

## server（MG_Remote/Server）

1. **session registry 取代单例**。`ServerSessionInstance`/`ServerLoopInstance` 改为 `ServerProcess` 持有的 session 表（slot → {transport, ServerSession, ServerLoop, PipeApplier, apply thread}）。in-process server（`ServerMain.cpp:1593-1651` 的 `mobilegl_server_serve_inprocess`）从「单 active + RefuseBusy」改为接受至上限（默认 8，可配）；`--serve` 的 unix 路径同样改为同进程多 session，**移除 fork-worker 分发**（`ServerMain.cpp:1566-1584`），`--max-sessions` 语义改为同进程上限。TCP 的 `TcpSupervisor` 单 active（`:1220-1223`）同样让位。
2. **apply thread per session**。每 session 一个 apply thread；`g_applyThreadKey`（`ServerLoop.h:129`）改为 per-session 成员，`OnApplyThread()` 语义不变（同 thread 比较）。session 之间顺序调度由线程天然完成，不需要全局锁；native driver 本来就支持多线程各自 current 不同 context。
3. **g_applier per session**。`PipeApply.cpp:484` 的进程级 `MGPipeApplierState` 改为 session 持有；`MGPipeApplierReset` 的 role guard（`PipeApply.cpp:1474-1481`）按本 session 的 apply thread 判定。`PipeApply.h:558-617` 已有的 share-group 对象记录 / working-state 二分是地基：对象记录表按 share group 分桶，working state 按 context 分桶，reset 只清本 context 的 working state。
4. **反向路由按 session + context**。`ReverseCallbackOwner`（`ServerSession.cpp:408-418`）的 `Active()` 单活性改为「按发起线程/显式 session 参数解析」；事件头加 `ContextSlot`（见 wire 3）。`gMGPipeCallbacks`（`MGPipeCallbacks.h:77`）从进程级变量改为 per-session 表，Accept 时挂到 session 而非全局。
5. **session 模式闩保留在 session 层**。`SessionSurfaceMode`/`SurfaceModeMismatch`（`ServerLoop.h:199-206`、`ServerLoop.cpp:1539-1544`）口径不变：compositor session 闩 OnScreen、app session 闩 Offscreen；统一 server 同时持有两种 session 是合法形状，不许把闩提升到进程级。

## 后端（DirectGLES）

1. **native tuple per context**。`g_Display` 保留进程级（native display 一次 initialize 合理）；`g_Context`/`g_Surface`/`g_Config`（`DirectGLES.cpp:16183-16186`）改为 per-(session, contextSlot) 表，native context 按 share group 用 `eglCreateContext(share=…)` 建。
2. **拆掉无条件 teardown**。`InitDisplayAndContext` 首句 `DestroyEGLContext()`（`DirectGLES.cpp:16569`）与 `BackendObject_DirectGLES` 建 surface 前的 `DestroyEGLContext()+ResetEGLRuntimeState()`（`BackendObject_DirectGLES.cpp:972-982`、`:1007-1014`）改为：目标 context 已存在则复用其 native context，仅换绑 surface；pbuffer→window→pbuffer 不丢资源。`DestroyEGLContext` 只拆指定 context；最后一个 context 走了才 `eglTerminate`。
3. **Managers 按 share group / context 分表**。六个 twin registry（`Managers.cpp:6895`、`:11035`、`:12835`、`:13188`、`:16255`、`:16580`）按 share group 实例化；`g_activeTextureUnit`/`g_boundTexturesCache`/sampler cache（`Managers.cpp:11031-11032` 等）按 context 实例化；`DropEveryTwinForEndedServerSession`（`Managers.cpp:425-443`）只清本 session。`g_backendContextGeneration` 改为 per-context 世代。
4. **DirectVulkan 同步审视**。Magma 的 per-device/per-queue 结构也要按 session 分；本阶段先保证不编译退化，多 session 下 Magma 的验收单列。

## 分期与验收

| 期 | 内容 | 验收 |
|---|---|---|
| P14a | wire context 身份 + 前端 GLContext/ShareGroupState 拆分 + CreateContext/DestroyContext 上 wire | shared contexts 访问同一 buffer/texture；nonshared 同名资源隔离；每 context 的 viewport/FBO/bindings/error 独立；context 跨线程移交后状态保留 |
| P14b | 后端 per-context native tuple + surface 生命周期 | pbuffer→window→pbuffer 资源不丢；Qt offscreen probe + 实窗的 context 序列不丢首窗内容 |
| P14c | server 同进程多 session（registry、per-session applier/apply-thread/反向路由），移除 fork-worker 依赖 | 相同 handle 值的两个 session 隔离且可独立关闭；一个 session 崩不拖死另一个；in-process server 并发两 client 渲染互不串台 |
| P14d | anland 切统一 embedded server | KWin（OnScreen session）+ Konsole（Offscreen session）同 server 同上屏；见交接文档验收链 |

每期都过既有门：host ctest 全量、`split_coverage.py` 三臂、monolith G1 符号不变；wire 变更期两端同 stamp 重构建。monolith 路径（`MOBILEGL_PIPE_PUSH=0`）下多 context 语义同样必须成立——拆分前后端共用一个 `MG_State`。

## 明确不做

- 跨 session 共享 GL 对象（EGL 语义本就不允许跨连接共享；需要时另立 import 设计）。
- per-context ring（见 wire 1 的取舍）。
- 把 FBO/VAO 等容器对象放进 share group。
- 保留 `--serve --max-sessions` 的 fork 分发作为任何验收路径；工具本身可留作调试，但 anland 与多 session 验收不得依赖它。
