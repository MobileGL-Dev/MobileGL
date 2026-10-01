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

## S1 已落地形状（context 身份上 wire）

上面第 1、2 条是 P14a 的目标形状；S1 只落地了「身份能跨线、server 能归属」这一层，实际形状如下，后续切片按此续建。

**data plane：`bind_context`，opcode 84**（`MG_Pipe/PipeCalls.def`，`kScreen` 族）。

| 项 | 值 |
|---|---|
| payload | `MGPBindContext{ Uint64 ClientContextToken }`（8 B，`MGP_ASSERT_POD`） |
| flags / wait | `kNone` / `kWaitNone`（apply 不读客户端内存，记录的 ring 位置就是它的全部次序） |
| apply 入口 | 无 `MGPipeApply*`；与 `applier_reset`/`object_death` 同为 control record，只到 `WireVerbSink` |
| monolith 生产者 | 无（G1 不变） |
| sink | `ServerVerbSink::OnBindContext`（`MG_Remote/Server/PipeApplier.cpp`） |

**token 位宽：两端都是 `Uint64`。** 记录里是 `ClientContextToken`，控制帧里是 `SurfaceOp.context`；同宽是刻意的，否则必有一端要截断。`0` 在两处都表示「无 context」（release），也正是一个从未 bind 过的 session 已有的答案。token 由 `EGLState::CreateContext` 铸造（`m_nextClientContextToken` 从 1 起、稠密、不复用），随 `ContextObject` 存放，`EGLState::CurrentContextClientToken()` / `GetContextClientToken()` 读出。

**control plane：`SurfaceOpKind.CreateContext = 12` / `DestroyContext = 13`**（`protocol.fbs`），不是两个新的 CtrlMsg——context 的创建/销毁/make-current 与 surface 是同一个调用者、同一个线程、同一条阻塞 frame channel，另开一条只会多一个次序会打架的地方。`SurfaceOp` 追加 `shareGroupToken: ulong` 与 `contextFlags: uint`（保留扩展位，今天恒 0）。`MOBILEGL_PROTOCOL_CONTROL_REVISION` 5 → 6，`scripts/ci/protocol_revision_pins.json` 同步加行。

- `CreateContext`：`context` = 新 context 的 token，`shareGroupToken` = 它并入的 share group（`shareCtx` 的族，或新建），`contextFlags` = 保留位。`display`/`surface` 为 0（server 的 display 就是 session 自己的）。
- `DestroyContext`：`context` = 要销的 token；server 销表并在它当前 bind 时解绑。
- 两者都用既有的 `SurfaceReply` 回答（ok + refusal），S1 建的是占位 runtime，没有别的可回。

**`bind_context` 与 MakeCurrent 控制帧的关系：不合并，各管一件事。**

- `MakeCurrent` 控制帧仍是 **native tuple 的 op**：它让 server 的 apply thread 决定要不要真的 `eglMakeCurrent`，并参与 `m_haveCurrentTuple` 的去重（ID-54/ID-67 的口径一个字没动）。它的 `context` 字段与 `bind_context` 的 token 是同一个值，但那是「同一个身份」，不是「同一条通路」。
- `bind_context` 是 **ring 上的归属记录**：它告诉 server「这条 ring 上从今往后的记录属于哪个 context」。它必须在 ring 上，因为 GL 记录就在 ring 上，而控制帧与 ring 之间没有全局次序可依赖。
- 发射点只有一个：`EGLImpl::MakeCurrent` 在切换成功之后、返回之前调 `EmitContextBinding`（`MG_Impl/EGLImpl/EGLImpl.cpp`）。这给出「先于后续 GL 记录」的构造性保证——app 线程在 `eglMakeCurrent` 返回前不可能发出新 context 的任何记录，而 ring 是 FIFO。token 未变则不发（identical make-current 是 EGL 合法 no-op）。
- `DestroyContext` 帧在客户端 `EGLState::DestroyContext` 接受之后才发，所以 EGL 自己拒掉的那种销毁（context 仍被某线程 current）不会到达 server，两端不会因为「token 悄悄消失」而对不上。

**与第 1 条的取舍差异（明确记录）**：S1 走的是「在边沿宣告身份」而不是「在 71 个 context 族记录的 header 与 payload 之间插 `Uint32 ContextSlot`」。代价是每个 make-current 一条记录（而不是每条记录 4 B），收益是 payload 布局与 `MGPWireRec_*` 尺寸全部不动、`kRecHasContext` 位与 batch 边界的生成器改动全部不必；单 context 的既有形状（token 0 / 从未 bind 过）零改动即兼容。多线程交错下的逐记录身份若将来确有需要，再按第 1 条补 slot——那时两者可以并存，slot 为 0 仍读当前 token。

**server 端**（`MG_Remote/Server/ServerSession.{h,cpp}`）：每 session 一张 `{token → ContextRuntime}`（`ContextRuntime{ Token, ShareGroupToken, Flags }`，S1 里是占位，S4 给它装 native context）加一个当前 token（atomic，0 = 无）。`CreateContext`/`DestroyContext` 控制帧经 `ServerLoop::ApplySurfaceControlFrame` 的 `CreateContext`/`DestroyContext` 两个 arm 建表/销表；`bind_context` 经 codec 到 `ServerVerbSink::OnBindContext` 切当前 token。写入全部发生在 apply thread，表因此不需要锁。`bind_context` 指向一个本 session 从未创建的 token 是 `Fatal{ProtocolCorruption, "BindContext.ClientContextToken"}`（0 例外，永远合法）；表的存在正是为了让这条判定有依据。

**S1 的 token 只有写入者，没有读者**：server 端目前没有任何 apply 分支读 `CurrentContextToken()`（`grep` 可证：除 `ServerSession` 自己的访问器外无人调用），所以整条渲染路径的行为与 S1 之前逐字相同——这一层先把「归属」记下来，读它做事是 S4（native context）和 P14b/c 的事。没有 `bind_context` 的 session 行为同样不变：`ServerLoopTest` 的 MakeCurrent/epoch/liveness 用例（`MG_Test/Wire/ServerLoopTest.cpp:1780-1941`）仍全部通过，它们走的正是这条路径。

**S1 的验收证据**（本次提交的测试）：`PipeCatalogue.LateArrivalsAreAppendedWithoutRenumbering` 钉 opcode 84 与 `kNone`/`kScreen`/`kWaitNone`；`SurfaceControlFrameTest.TheContextLifecycleFramedOpsAreCreateContextAndDestroyContext` 钉 12/13 的值与 `shareGroupToken`/`contextFlags` 的往返；`SessionHandshakeTest.ARevisionFivePeerIsRefusedByNameAtRevisionSix` 钉 revision bump；`FuzzArm2/PeerLatchSite/.../BindContextUnknownToken` 是 data plane 的端到端控制——真实 `--serve` supervisor 上，一个伪造的、指向本 session 从未创建过的 token 的 `bind_context` 让 session 按名 latch（`exit=75`），且同一 supervisor 的下一个 session 正常渲染。

## 前端（MG_State / MG_Impl）

1. **拆 GLContext**。`MG_State/GLState/Core.h:77-698` 的 `GLContext` 拆成 `ShareGroupState`（buffer/texture/renderbuffer/sampler/shader/program/sync 的对象 store + 名称分配）与 `GLContext`（bindings、error、viewport/scissor、render state、VAO/FBO/TF/pipeline/query store、texture unit 绑定、current program）。容器对象（VAO/FBO/TF/pipeline/query）**不共享**，留在 per-context；这是 GL 4.6 §5 的口径，不要为省事把 FBO 放进 share group。
2. **per-EGLContext 实例化**。`EGLState::CreateContext` 建 `GLContext`，`shareCtx != EGL_NO_CONTEXT` 时挂到对方 `ShareGroupState`（首次共享时把对方的对象 store 迁移进新 share group，双方同指）。`eglMakeCurrent` 切 TLS 的 (session→) GLContext 指针；`eglDestroyContext` 只在 owner 不存在时允许（沿用 `MG_State/EGLState/Core.cpp:714-718`），销毁 context 不销毁 share group，最后一个 context 离开时 share group 对象全删。
3. **Tracker 判据改显式身份**。`Pipe/Tracker.h:306-309` 的指针比较换成 (session, contextSlot)；换 context 的首个 verb 重推完整状态的语义不变。`MGPipeTrackerInstance()`（`Tracker.h:956-963`）从单例改为 per-context（tracker 缓存的是 per-context 推送快门）。
4. **eglMakeCurrent 流程**：`EGLImpl::MakeCurrent` 除调 `backendObject->MakeEGLCurrent` 外，先切 TLS GLContext 再发 `SurfaceOp::MakeCurrent{contextSlot}`；client 线程后续的 context 族记录都带这个 slot。

## S3 已落地形状（前端 per-context / share group）

上面「前端」一节是目标形状；S3 落地了第 1、2 条（第 3、4 条的 tracker/slot 部分未做），实际形状如下。

**`ShareGroupState`（`MG_State/GLState/ShareGroupState.{h,cpp}`，新）** 持有一个 share group 的全部对象注册表与名字生成器：

| 共享（ShareGroupState） | 每 context（GLContext） |
|---|---|
| buffer / texture / renderbuffer 的对象表与 `IndexGenerator`；sampler 对象表（`SamplerState` 整体）；program+shader 表与共用名字生成器（`ProgramState` 整体）；`token`（S1 的 `ShareGroupToken`） | error 列表；buffer binding slot 与 indexed binding point、touched 高水位、各 aggregate generation；texture unit / image unit 数组、active unit、bind/sampling generation、per-target 默认纹理（name 0）；renderbuffer binding slot；VAO/FBO/TF/pipeline/query；`current program` |

`BufferState`/`TextureState`/`RenderbufferState` 因此只剩 binding 侧：删除走「先查 share group 的对象、再解本 context 的绑定、最后 free 名字」，即删对象是全局的、解绑定是逐 context 的（GL 4.6 core 6.1/8.1）。`ProgramState` 的 `UseProgram`/`glDeleteProgram`/`ReleaseShaderNameIfOrphaned` 现在把调用方的 current program 当参数传入，current program 本身落在 `GLContext`。`GLContext` 由 `GLContext(SharedPtr<ShareGroupState>)` 构造，`GLContext()` 建一个只属于它的 share group（测试与进程默认 context 用）。

**`pGLContext` 改成 TLS**：`extern thread_local SharedPtr<GLState::GLContext> pGLContext`（`GLState/Core.h`）。`EGLState::MakeCurrent` 成功后把它指向该 context 的 `GLContext`，`ReleaseThread`/`eglTerminate`/释放路径恢复线程进入 EGL 之前的那个值（`ThreadCurrentState::RestoreThreadGLState`）——所以「一个 context 同时只被一个线程拥有」的既有语义（`m_contextOwners`，EGL_BAD_ACCESS）一个字没动，owner 也不进 TLS：context 跨线程移交后状态仍在它自己的 `GLContext` 里。

没有 EGL context 的线程落到**进程默认 context**（`ProcessDefaultGLContext()`，`MG_State::Init()` 建，构造时 `pGLContext` 的初值就是它）——这正是拆分前「全局唯一 context」的语义，所以单 context 应用与所有直接操作 `pGLContext` 的既有测试行为不变。

**每个 `eglCreateContext` 都建独立 `GLContext`**（`ContextObject::GLStateObject`）；`shareCtx != EGL_NO_CONTEXT` 时 `GLContext` 挂到对方 `ShareGroupState`，否则新建一个并把 S1 铸的 `ShareGroupToken` 记在里面。这里不需要「迁移对方 store」：group 在第一个 context 诞生时就存在，后来者直接并入。`eglDestroyContext` 丢掉 `ContextObject`，共享指针归零才销毁 `GLContext` / `ShareGroupState`。

**默认帧缓冲（明确的偏差）**：GL framebuffer 0 今天仍是**进程一个对象**（`MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo`，后端按它判断「这是默认 FBO」）。它在 `MG_Impl::Init()` 建好之后经 `MG_State::SetDefaultFramebuffer` 交给 MG_State，此后每个新建 `GLContext` 构造时把它装成自己的 name 0 并绑到 Draw/Read（`GLContext::InstallDefaultFramebuffer`）。也就是说：每个 context 有**自己的 FramebufferState 表项与绑定**（`glBindFramebuffer` 互不影响），但那个对象暂时是共用的。真正的 per-context 默认帧缓冲属于「后端 per-context native tuple」那一期（见「后端」§1），在那之前若各 context 各建一份，后端的 `pDefaultFramebufferInfo` 身份判定会对第二个 context 失效。

**monolith 与 split 走同一份结构**：两端共用一个 `MG_State`，`eglCreateContext`/`eglMakeCurrent`/`eglDestroyContext` 都先落这套前端状态，`MOBILEGL_PIPE_PUSH=0` 与 `=1` 的区别只是后端从哪儿读状态（`MGB_CTX` 是 `pGLContext` 还是 `gPipeInputs`）。`MGB_CTX_IDENTITY` 因此天然按 context 变化，post-S3 的 tracker 只需照旧比较（改成显式 slot 是后续切片）。

**验收证据**（本次）：`MG_Test/State/ContextStateOwnershipTest.cpp` 六个用例——不共享的两个 context 名字空间独立（同名不同对象、绑定不互见）；共享对看到同一对象而各自持有绑定；`eglMakeCurrent` 切换后 error/viewport/FBO binding 各自独立且切回来还在；`eglDestroyContext` 后另一个 context 对象与绑定不受影响；释放后线程回到进入 EGL 前的 GL 状态；无 current context 的线程落到进程默认 context。

## server（MG_Remote/Server）

1. **session registry 取代单例**。`ServerSessionInstance`/`ServerLoopInstance` 改为 `ServerProcess` 持有的 session 表（slot → {transport, ServerSession, ServerLoop, PipeApplier, apply thread}）。in-process server（`ServerMain.cpp:1593-1651` 的 `mobilegl_server_serve_inprocess`）从「单 active + RefuseBusy」改为接受至上限（默认 8，可配）；`--serve` 的 unix 路径同样改为同进程多 session，**移除 fork-worker 分发**（`ServerMain.cpp:1566-1584`），`--max-sessions` 语义改为同进程上限。TCP 的 `TcpSupervisor` 单 active（`:1220-1223`）同样让位。
2. **apply thread per session**。每 session 一个 apply thread；`g_applyThreadKey`（`ServerLoop.h:129`）改为 per-session 成员，`OnApplyThread()` 语义不变（同 thread 比较）。session 之间顺序调度由线程天然完成，不需要全局锁；native driver 本来就支持多线程各自 current 不同 context。
3. **g_applier per session**。`PipeApply.cpp:484` 的进程级 `MGPipeApplierState` 改为 session 持有；`MGPipeApplierReset` 的 role guard（`PipeApply.cpp:1474-1481`）按本 session 的 apply thread 判定。`PipeApply.h:558-617` 已有的 share-group 对象记录 / working-state 二分是地基：对象记录表按 share group 分桶，working state 按 context 分桶，reset 只清本 context 的 working state。**（S5 已落地，形状见下面「S5 已落地形状」：这一条最终不是「per session 一个 applier」，而是 per {session, context} 一个 applier、其对象记录挂到 {session, shareGroup} 的组上。）**
4. **反向路由按 session + context**。`ReverseCallbackOwner`（`ServerSession.cpp:408-418`）的 `Active()` 单活性改为「按发起线程/显式 session 参数解析」；事件头加 `ContextSlot`（见 wire 3）。`gMGPipeCallbacks`（`MGPipeCallbacks.h:77`）从进程级变量改为 per-session 表，Accept 时挂到 session 而非全局。
5. **session 模式闩保留在 session 层**。`SessionSurfaceMode`/`SurfaceModeMismatch`（`ServerLoop.h:199-206`、`ServerLoop.cpp:1539-1544`）口径不变：compositor session 闩 OnScreen、app session 闩 Offscreen；统一 server 同时持有两种 session 是合法形状，不许把闩提升到进程级。

### S2 已落地形状（同进程多 session）

上面第 1、2、4、5 条是 P14c 的目标形状；本切片落地的是「归属拆开、路由按线程」这一层，实际形状如下。

**每 session 一个 runtime，singleton 变成 fallback**（`MG_Remote/Server/SessionRuntime.h`）。`SessionRuntime` 持有该 session 的 `ServerSession`、`ServerLoop`、`PipeInputs` 块，加一个 latch domain 与一个 segment resolver 绑定；`ThreadSessionScope` 把它挂到 **该 session 的两个线程**上——控制线程（`ServerMain::RunSession` 自己的线程，supervisor 里每 session 一条 `std::thread`）与 apply 线程（`mgl-srv-apply`，`ApplyThreadMain` 第一件事就是开同一个 scope）。`ServerSessionInstance()` / `ServerLoopInstance()` / `ServerSession::Active()` 因此先问「本线程属于哪个 session」，只有**不属于任何 session 的线程**才拿进程级单例。这一条让 ~900 个 `gPipeInputs` / `MGB_CTX` / `ServerSession::Active()` 调用点一行不改就有正确归属。inproc 的「同进程 client+server」形状刻意不开 scope（`ClientSession` 直接调单例），所以那个形状的 verb barrier、单块 PipeInputs 与逐字行为完全不变。

**gPipeInputs 是每线程解析的引用**（`MG_Backend/MGPipe/PipeInputs.h`）。`gPipeInputs` 从「一个进程级对象」变成 `thread_local` 引用，经 `g_pipeInputsThreadResolver` 这个由 server 层装一次的探针解析到本线程 session 的块，无 session 时落到 `gPipeInputsDefault`（即原对象）。**为什么是这个形状**：`MGB_CTX` 是 `(&gPipeInputs)`，要让 ~380 个点位不动而目标随线程变，只有「目标随线程变的引用」可行。代价是每个访问点一次 TLS 地址计算（不是每个字段，且无 guard：每线程第一次使用时绑定一次），这是本切片的已知成本。

**apply 线程身份改成 TLS 一位，进程级那个字改成计数。** `OnApplyThread()` 的答案现在只可能是「本线程是不是 apply 线程」，所以它是 `thread_local Bool`（零初始化，无 guard、无原子）；`Detail::g_applyThreadKey` 保留原名与类型但装的是**本进程活着的 apply 线程数**——`MG_Backend/DirectGLES/Managers.cpp`（本切片文件集之外）读这个 `!= 0` 问「有没有 apply 线程在跑」，计数正好回答那个问题，热路径没有人再读它。

**latch 是每 session 一个 domain**（`MG_Remote/FatalFunnel.*`）。原来的四个进程级静态变量变成按 domain 存的 `LatchState`，线程绑定一个 domain（`ThreadSessionScope`），不绑定就是 domain 0（进程默认 = 老行为）。理由不是整洁而是正确性：`SessionLatched()` 的三个读者都在 session 自己的线程上（apply 循环、`DrainRing`、`RunSession` 的控制循环），一个进程共用一个 latch 时 session A 的坏字节能停掉 B 的 apply 线程；`ResetSessionLatch()` 也有镜像问题（一个 session 的 reap 会清掉活着的那个的 latch）。现在 A 的关闭/故障都到不了 B。

**segment resolver 是每线程的表 + 进程级的 hook**（`MG_Remote/Wire/PipeWireCodec.*`）。`gMGPipeSegmentResolver` 仍是一个进程级函数指针（装一次，不再置空），但 thunk 先查**调用线程绑定的表**，其次才是进程级 fallback。同一线程上出现第二张表仍然是 `Fatal{...already installed...}`（既有 `APartiallyZeroUploadBoxIsFatalRatherThanASilentRace` 与 `ASecondProcessResolverIsFatalRatherThanASilentRace` 的口径不变）；不同线程上的第二个 session 不再是错误。

**全局回调表：装一次、按线程路由、最后一个 session 关才卸。** `gMGPipeCallbacks` 的五个条目仍是进程级函数指针（每个 session 装的是同一批函数），`Close()` 只有在 `ServerSessionCount()` 归零时才清；`g_sessionOwner` 的 CAS 抢占对**有 runtime 的 session 不再生效**（共表是合法的），只对「进程就是一个 session」的形状（inproc client+server 角色、`ServerLoopTest` 的 `FvSession` 及其对等对象）保留原样的 `Fatal{RoleViolation,"callback-double-install"}`。

**surface mode 仍是 D4 的 per-session 闩**（`ServerLoop::m_surfaceMode`）。本切片一度引入 `SetDeclaredSurfaceMode`，让 `RunSession` 按**进程自己的** `MOBILEGL_IPC_SURFACE` 给 session 声明模式——**已撤掉**，两条理由：(a) 进程级的环境变量说不了统一 server 同时持有的多个 session（§4 第 5 条要的是口径不变、闩不许提升到进程级），(b) 它对 dial 进来的 client 是错的——pre-started server 不继承 client 的 env（`ServerOwnedSurfaceTest` 的 TCP 形状），于是 session 被声明成 offscreen，ServerOwned 的建面先拿 `SurfaceModeMismatch`，永远走不到「本 server 没有 display」那条具名拒绝。**而且它不需要**：`m_surfaceMode` 是 `ServerLoop` 的成员，每个 session 有自己的 loop，两个 session 各自闩各自的模式——「每 session 独立 surface mode」正是这个形状，不需要任何声明。真正想在第一个 surface 之前声明模式的形状（P14d 的 KWin on-screen + Konsole offscreen 同一个 server）必须**让模式上 wire**（Hello 或一条 SurfaceOp），那是 P14d 的账；在那之前，谁先建 surface 谁闩定，是每个 session 各自的答案。**`ServerDisplay` 的 lease 天然多路复用**：holder 是 loop 指针，`AcquireFor` 只会把窗口租给一个 session，第二个 on-screen session 拿到的是**具名拒绝** `NoServerWindow`（日志里点名 LeasedElsewhere），不再是注释里那句「sessions are sequential: a bug if seen」；offscreen session 完全不取 lease，可以任意多。

**接入层**：`UnixInProcessSupervisor`（`mobilegl_server_serve_inprocess` 的 unix 分支）从「一次一个 + 第二个 Refuse{Busy} one session is already active」改成**并发至多 `MOBILEGL_IPC_INPROC_MAX_SESSIONS`（默认 16）**，每个 session 一条控制线程，超限时用 fork 路径同一句话 `Refuse{Busy} the session limit is reached`；embedded ABI 的签名不变。**没动的三处**：`--serve --max-sessions` 的 fork worker 路径（进程隔离仍是可选形态，`UnixSupervisor.ConcurrentSharedMemoryClientsHaveSeparateWorkersAndAnExitedSlotIsReused` 与 `...RequiresAnExplicitLimit` 照旧）、TCP 的 in-process 形状（仍是一 session 一次，`InProcessServer.AnAuthenticatedSecondHelloWhileASessionIsLiveIsRefusedBusy` 照旧）、`TcpSupervisor` 的 fork 形状。与本文 §4 第 1 条写的「默认 8」差一个数：本切片取 16，且上限是环境变量而非新 ABI 参数。

**验收证据**（本切片新增）：`MG_Test/Wire/MultiSessionTest.cpp` 三个用例——`MultiSessionFixture.TwoShmSessionsInOneProcessAreServedAtOnce`（同进程两个 SHM session 并发：两个 Welcome 同 pid、日志里两条 `in-process unix session #N started`、各自 clear 各自读回自己的颜色）、`MultiSessionFixture.SameNamesInTwoSessionsDoNotCollide`（同一 client context token 与同一 render state handle 在两个 session 里都被接受——名字是 session 内的名字）、`MultiSessionFixture.ClosingOneSessionLeavesTheOtherServing`（A 关掉并 reap 之后 B 继续 apply 三条记录，进程级 resolver/回调表没被 A 卸掉）。既有套件不回归：本切片点名的 `InProcessServerTest`、`ServerDisplayTest`、`UnixSupervisor`、`SupervisorChildren`、`ServerLoopTest`/`ServerLoopEglTest`/`ServerLoopLatchTest`、`EmbeddedServerTest`、`PeerLatchTest`（含 `PeerLatch.SiteMap`，其 FatalFunnel 的 check 正则已跟着 domain 改写更新）、`ServerOwnedSurfaceTest`、`SessionHandshakeTest`、`SurfaceControlFrameTest` 全绿（host ctest 里剩下的失败只有 `3rdparty/glslang` 那个 dirty submodule 造成的编译/链接期基线失败，与本切片无关）。

**修正（`HealthySessionWithColor` 的颜色解包）**：本切片把 `P12ServerRig.h` 的绿色 clear 参数化时，一度把 `rgba` 按 `A<<24|B<<16|G<<8|R` 的逆序解包（把最高字节当 R），于是 `0xFF00FF00`（绿）被清成 `0x00FF00FF`（品红），`InProcessServer.SequentialSessions...` 的读回断言因此挂掉。正确的解包就是读回自己的字节序——`PeerReport::pixel` 是 GL_RGBA/GL_UNSIGNED_BYTE 三元组的 little-endian `Uint32`，故 `R = rgba & 0xFF … A = rgba >> 24`。**生产路径（clear → read_pixels）一个字节没动**：`PeerLatchTest` 用同一绿色的字面量 `{0,1,0,1}` 且断言同一个 `0xFF00FF00`，它的对应用例全程是绿的，就是这条结论的控制。红色 `0xFF0000FF` 在字节反转下是回文，所以当时只有绿色 session 失败——这不是两个 bug。

**本切片明确没做（下一层的账）**：DirectGLES 的 `g_Display/g_Config/g_Context/g_Surface` 与六个 twin registry 仍是进程级（P14b 的 per-context native tuple），所以两个并发 session 的**原生上下文并不隔离**：一个 session 的 teardown 仍会拆掉进程的 EGL context，本切片的渲染验收因此只在两个 session 都活着时做，关掉一个之后的证据是记录级的（记录照常 apply 并有回包）。`MGPipeServerSetContextLive` / `MGPipeServerSetOwnedWindow` 与 `PipeInputs.cpp` 同属进程级，不在本切片的文件范围内。`MG_Util::PipeStats::Init/Shutdown` 仍是进程级，多 session 下是遥测噪声不是正确性。`SessionFail`（未武装时的真 Fatal）仍会带走整个进程——那是 `--serve` fork 形状存在的理由。

## 后端（DirectGLES）

1. **native tuple per context**。`g_Display` 保留进程级（native display 一次 initialize 合理）；`g_Context`/`g_Surface`/`g_Config`（`DirectGLES.cpp:16183-16186`）改为 per-(session, contextSlot) 表，native context 按 share group 用 `eglCreateContext(share=…)` 建。
2. **拆掉无条件 teardown**。`InitDisplayAndContext` 首句 `DestroyEGLContext()`（`DirectGLES.cpp:16569`）与 `BackendObject_DirectGLES` 建 surface 前的 `DestroyEGLContext()+ResetEGLRuntimeState()`（`BackendObject_DirectGLES.cpp:972-982`、`:1007-1014`）改为：目标 context 已存在则复用其 native context，仅换绑 surface；pbuffer→window→pbuffer 不丢资源。`DestroyEGLContext` 只拆指定 context；最后一个 context 走了才 `eglTerminate`。
3. **Managers 按 share group / context 分表**。六个 twin registry（`Managers.cpp:6895`、`:11035`、`:12835`、`:13188`、`:16255`、`:16580`）按 share group 实例化；`g_activeTextureUnit`/`g_boundTexturesCache`/sampler cache（`Managers.cpp:11031-11032` 等）按 context 实例化；`DropEveryTwinForEndedServerSession`（`Managers.cpp:425-443`）只清本 session。`g_backendContextGeneration` 改为 per-context 世代。
4. **DirectVulkan 同步审视**。Magma 的 per-device/per-queue 结构也要按 session 分；本阶段先保证不编译退化，多 session 下 Magma 的验收单列。

### S4 已落地形状（每 context 原生 tuple + share-group 键控）

上面第 1、2 条是 P14b 的目标形状；S4 落地了「native tuple 按 context 归属、创建第二个 context/surface 不再拆 context、share group 进原生」这一层，实际形状如下。

**native tuple 按 (session, token) 键控**（`MG_Backend/DirectGLES/DirectGLES.cpp` 的 `NativeContextTuple` / `NativeSessionState` 注册表）。`g_Display` 仍是进程级一份、`eglInitialize` 一次，只有最后一个原生 context 走了才 `eglTerminate`（`g_displayUsers` 计数）。`EGLContext`/`EGLConfig` 随 tuple；**原生 surface 按 session 一份**（EGL 里 surface 不属于 context，只有 draw/read 绑定属于），tuple 记自己当前画到哪个 surface。键由 server 层装的探针回答（`SetNativeContextKeyResolver`，`MG_Remote/Server/SessionRuntime.cpp` 经 `ThreadSessionScope` 装一次）：答案是**本线程的 session 身份 + `ServerSession::CurrentContextToken()`**；没有探针的线程（monolith、纯前端进程、unit case、inproc 单进程形状）答案恒为 `{0,0}`——这正是被替换掉的进程单例，所以单 context 世界逐字不变。

**创建不再拆 context**。`InitDisplayAndContext()`（首句无条件 `DestroyEGLContext()`）换成 `EnsureNativeContext(tuple, surfaceBit, window)`：tuple 已有原生 context 就复用，只按需要选 config；没有才建。`BackendObject_DirectGLES::CreateEGL{Window,Pbuffer}Surface` 里的 `DestroyEGLContext()+ResetEGLRuntimeState()` 同样去掉 teardown，只留 base class 的 per-surface bookkeeping。因此 pbuffer→window→pbuffer 只是往 session 的 surface 集合里加/换，tuple 的 `EGLContext` 一字不动——对象（后端 twin 直接活在原生 context 里）自然不丢。`DestroyEGLContext()` 现在是**本 session** 的全量 teardown（`ReleaseEGLResources`/`~BackendObject_DirectGLES` 的语义），单个 context 的销毁是新的 `DestroyNativeContextFor(token)`。

**share group 进原生**（`CreateNativeContextFor(token, shareGroup)`）。`CreateContext` 控制帧到达时，server 为该 token 建原生 context：**同 session 内、同一 shareGroupToken 且已有活 context 的 tuple 的 `EGLContext` 作为 `eglCreateContext` 的 share 参数**，否则 `EGL_NO_CONTEXT`。组内后续 context 因此共享原生对象命名空间。建失败则回滚该 session 表项并把 `ok=false` 回给客户端。

**bind_context 是原生切换点**。MakeCurrent 控制帧到的时刻 `CurrentContextToken()` 还是旧值（S1 刻意让客户端在 make-current 成功后才发绑定记录），所以在那里切原生会绑到刚离开的 context。`ServerVerbSink::OnBindContext`（`MG_Remote/Server/PipeApplier.cpp`）在 `session->BindContext(token)` 之后调 `DirectGLES::MakeNativeContextCurrentForBoundToken()`：按新 token 解析 tuple，与 `t_boundNativeContext` 相同就返回（单 context 的每次绑定零成本），不同才做原生 `eglMakeCurrent`。

**验收证据**（本次新增，`MG_Test/Wire/ServerLoopTest.cpp`，真实 headless EGL）：`ServerLoopEglTest.CreatingASecondContextLeavesTheFirstNativeContextAlive`（同一 session 三个 context token，三个原生 EGLContext 同时活着且互不相同——还原 S4 前的无条件 teardown 时只剩一个）；`ServerLoopEglTest.CreatingAnotherSurfaceKeepsTheSameNativeContext`（建第二个 pbuffer 后 tuple 的 `EGLContext` 句柄不变、原生 context 数仍为 1）。既有集合零回归：本切片点名的 97 个用例全绿（86 基线 + 11 `ServerLoopEglTest` + 上面两个）。

**本切片明确没做（下一层的账）**：

- **`g_applier` 没有改成 {shareGroup → applier}**（上面 server 第 3 条）。原因不是时间，是**形状**：`MGPipeApplierState` 把「share group 的对象记录」和「每 context 的 working state」混在一个结构里（`PipeApply.h:515-933`，其自身注释第 101 行已指出该二分），照字面把它按 share group 键控会让**同一 share group 的两个 context 共用 working state**——bindings、current program、unit 集互相覆盖，比不改更错。正确的做法是先按该二分拆结构（对象表进 share group、working state 进 context），那是独立的一刀。今天 `g_applier` 与 `gMGPipeApplierReset` 逐字未动。**（S5 已做，见下节。）**
- **六个 twin registry 与 twin 键 `{shareGroup, slot, gen}` 未改**（`Managers.cpp`）。twin 仍按引用活着的后端 context 复用，多 context 下的正确性依赖「一个原生 context 一个 twin 世代」，这在两个 context 都活着时成立（本次测到的），在跨 context 迁移 twin 的路径上还没有证据。**（S5 仍未做，见 S5 的未做项。）**
- **share group 的驱动级证明未落地**。`eglCreateContext(share=…)` 已接线，但用 headless EGL 直接问 `glIsBuffer` 的探针在本机没有得到可用的控制（同 context 内 `glGenBuffers` 后 `glIsBuffer` 即返回 false），没有作为「跑了个空的绿」收进来。要证到驱动层，需要先弄清该探针，再补一个用例。**（S5 也没弄清：见 S5 的未做项与「为什么没做」。）**
- **Wire 第 1 条的逐记录 `ContextSlot` 未做**，仍是 S1 的边沿宣告 + `bind_context`。
- **`g_backendContextGeneration`/`g_syncContextGeneration` 仍是进程级**：销毁单个 context 会推进全局世代，其他 context 的旧 fence/query 句柄因此读作「已 signal」。安全方向（最坏是多等），但不是每 context 世代，属 P14b/c。
- **ServerLoop 的 MakeCurrent 去重仍是「当前绑定的那一个 tuple」**，不是 per-context 表。理由写在 `ServerLoop.h:560-573`：ID-67 的钉死用例（A→B→A 必须三次原生绑定）定义「不同 tuple」为**当前绑定**的变化，而 EGL 里一个线程同时只有一个 current context，回到 A 确实必须重跑那七条前端 context 失效。「每 context 持有自己的 native triple」由后端的 {session, token} 注册表满足，去重这一层的单位仍是线程的当前绑定。

### S5 已落地形状（applier 按 {session, context} / {session, shareGroup} 键控）

S4 留下的最后一块：`PipeApply.cpp` 的进程级 `g_applier` 现在是一个**每 (session, context) 的注册表**，它的对象记录按 {session, share group} 分桶。

**结构拆分（`MG_Pipe/PipeApply.h`）**。原来一个 `MGPipeApplierState` 按两个寿命切成两个结构：

| `MGPipeObjectRecords`（share group） | `MGPipeWorkingState`（context） |
|---|---|
| `Resources`、`VertexElementsCsos`、`TextureResources`、`RenderbufferResources`、`SamplerCsos`、`SamplerViewCsos`、`ShaderCsos`、`CompositeShaderCsos`、`FramebufferRecords` | `RenderStateCsos`、`BoundRenderStateCso`、`Residual`、`ScatteredChunkBits`、四个 wire 计数器与四个 refusal 计数器、`BoundVertexElements`/`VertexBuffers*`/`IndexBuffer*`、`MapPersistentRoundtrips`、`BoundFramebuffer`/`FramebufferSerial`/`StaleFramebufferRecordLookups`、三个 unit 窗口、三个 program handle、三个 binding-point 窗口、`IsTransformFeedbackActive`、`TextureShutterSerial`/`ContextSerial`、`#if DISAGGREGATED` 的 verb handle 组 |

`RenderStateCsos` 在 working 一侧不是笔误：它今天是**被 `MGPipeApplierReset` 清掉的**（每个 make-current 一份新 CSO 表），照旧规则留在 per-context 那半。`FramebufferRecords` 留在对象一侧也是照旧规则（reset 不清它），且它的槽来自全局分配器，所以一个 share group 装两个 context 的记录是「更宽的表」而不是别名。

`MGPipeApplierState` 现在**继承** `MGPipeWorkingState` 并持有一个 `MGPipeObjectRecords& Objects`，另把九张对象表**按原名字绑成引用成员**（`Resources`、`TextureResources`…）。理由写在头里：MG_Backend / MG_IntegrationTest / MG_Test 里 ~200 个调用点一直把 `st.Resources` 读作「这个对象的 share group 记录」，绑引用让这些读点**一个不改**且按各自 context 所属的 group 正确解析；把键控改动变成改名改动只增风险不增正确性。头文件里可以同时用 `Objects.X`（在**创建**两半的地方显式点出群体）与 `X`（后端一直的拼法），两者是同一张表。

**注册表与解析链（`PipeApply.cpp`）**。三层，和 S2 的 `gPipeInputs`、S4 的 native tuple key 同构：

- `MGPipeApplierKey{ SessionKey, ContextToken }`，由 `MGPipeSetApplierKeyResolver` 装的探针回答；`SessionRuntime.cpp` 在 `InstallSessionRuntimeHooks` 里装（`ThreadSessionScope` 一开就装）。探针答案是**本线程 session 的 `ServerSession*` 地址 + `CurrentContextToken()`**；不是 session 线程的答案 false，拿到进程级 applier —— 这正是被替换掉的单例，所以 monolith / 纯客户端 / inproc 单进程形状 / 单元用例逐字不变。
- `MGPipeApplierRegisterContext(session, token, groupToken)` / `MGPipeApplierUnregisterContext` / `MGPipeApplierReleaseSession`。**注册而不是推断**：`ServerSession::CreateContext` / `DestroyContext` 在 apply 线程上调用它们，所以解析探针永远是「读本 session 已经在维护的当前 token」两字，不必从别的线程去读 session 的 map；`Accept` 与 `~SessionRuntime` 释放整 session。
- **group 键**：`shareGroupToken != 0 ? shareGroupToken : (contextToken | 高位哨兵)`。前端铸的 share group token 与 context token 来自两个计数器，不隔离就会让 group 7 和 context 7 变成一组；`0` 是 S1 注释里「本 context 自己起了一个组」的哨兵（也是 `ServerCreateEGLContext(token, 0, 0)` 这类手工建 context 传的值），落成「自己的组」而不是「和别的 0 一组」。
- **最后一个 context 走才释放该组的对象记录**（`MGPipeApplierReleaseObjectRecords` 的 per-group 版）；一组的另一个 context 还在时记录一条不丢。
- **线程局部缓存 + 世代号**。`MGPipeApplier()` 是后端按状态读调的，所以快路径是「本线程上次的 (session, token) 与注册表世代都对得上就直接返回」；注册表每次变动 `++generation`，缓存指针只有在世代相等时才被解引用 —— 这就是「另一个线程销毁了 context」不会留下悬垂缓存的原因。快路径开销是三个 TLS 读、一次原子读、两次比较；探针为 null（monolith）时是一次函数指针比较。

**前端/会话接入**：`EGLImpl::MakeCurrent` 的 `EmitContextBinding` 一字未动 —— S1 的边沿宣告本来就是「这条 ring 往后归谁」的全部信息，S5 只是第一次**读**它。

**验收证据**（本次新增）：

- `MG_Test/Wire/ApplierOwnershipTest.cpp`，5 个用例，直接驱动注册表与解析（无 EGL、无 wire）：两个 nonshared context 同名 handle 是两条记录且互不覆盖（reset 也不丢）；同一 share group 的两个 context **共用对象表而各持 working state**（当前 program、vertex-buffer 窗口不互见，而一张表上的 create 两边可见）；两个 session 用同名 group/context token 不共享任何东西；组的记录活到最后一个 context 才释放、重建的 token 不继承死组；无 session 的线程仍拿到进程级 applier。
  **负控**（必做，否则是空绿）：把测试装的解析探针改成 null（即回到 S5 前的「谁都拿进程级 applier」），5 个用例**全红**；换回即全绿。
- `MultiSessionTest.TwoSessionsInterleaveTwoContextsEachWithoutCrossTalk`：真实 `--serve` 前身（in-process unix supervisor）上两个 session，各自两个 context（同名 token 与同名 group token），**8 轮交错**：每轮先各发一条 `bind_context` 切 context，再跑一整趟 clear + 真读回。绿/红两种颜色互为证人；日志里 0 条 `SessionLatch{`。这条用例同时压的是**生产**解析链（`ThreadSessionScope` 装的探针）与并发（两个 apply 线程同时跑）。
- 既有集合零回归：本切片点名的 97 个用例与 `MultiSessionFixture` 原有的 4 个全绿（含 `ServerLoopEglTest` 的 11 个、`ContextStateOwnershipTest` 6 个）。`SanityTest` 的两个用例因 `MGPipeApplierState` 不再可拷贝改为保存/恢复它真正改动的 working 字段。
- **全量 host ctest 与基线的逐名对比**（不是"看起来差不多"）：先 `git stash` 掉本切片的全部改动、重建、跑一遍全量（`ctest -j 8`，2735 个用例）留下失败名单，再恢复本切片、重建、跑同一遍，两份名单 `comm`：

  | | 数量 |
  |---|---|
  | 基线失败 | 266 |
  | 本切片失败 | 265 |
  | **本切片新增失败** | **0** |
  | 基线失败而本切片不失败 | 1（`ProgramTest.CompileVertex`，glslang 那批的已知抖动） |

  失败名单全部落在 glslang / 着色器编译族（`ProgramTest`、`ProgramInterfaceTest`、`AsyncLinkTest`、`GlslangCaptureProbeTest`、`TranslationCacheTest`…），与 `3rdparty/glslang` 这个 dirty submodule 的既有基线一致。

**S5 明确没做（下一层的账）**：

- **六个 twin registry 与 twin 键仍未改**（`MG_Backend/DirectGLES/Managers.cpp` 的六个 `g_backend*` 静态注册表；键在 `Managers.h:315` 的 `StateBackendObjectRegistry` → `SlotTables.h` 的 `BackendSlotTable<MGPipeHandle{Slot,Gen}>`）。**这是 S5 之后"两个 nonshared context 的同名 handle"剩下的那一半**：applier 里两条记录是对的（有测试），但 twin 层按 `{slot, gen}` 键，两个 context 的同一个 handle 值会解析到同一个 twin；反向也一样，跨 context 复用 twin 的路径没有按 share group 分组。S4 报告里那条 `glIsBuffer` 探针本机仍无可用控制，所以**驱动级的 shared / nonshared 证据仍然是缺的**，本次没有以"跑了个空的绿"收进来（这是本切片最该被下一刀补上的证据）。改形状是把 `BackendSlotTable` 加一个 group 维度或按组实例化那六个注册表，触及 `Managers.cpp` 里全部调用点，不是本轮时间能稳妥做完的一刀。
- **pbuffer→pbuffer→pbuffer 的资源保留**只到 S4 已落地的程度（`ServerLoopEglTest.CreatingAnotherSurfaceKeepsTheSameNativeContext`：换 surface 不动 tuple 的 `EGLContext` 句柄）。真正的"三个 pbuffer 之间来回且资源都在"要 window 半边，本机 headless 验不了；按 S4 的口径只记录。
- **wire 第 1 条的逐记录 `ContextSlot`** 仍未做，仍是 S1 的边沿宣告 + `bind_context`。所以"同一 ring 上两个 context 的记录交错"只在 client 按 make-current 顺序发射时成立（本轮的交错压力用例走的正是这个顺序）。
- **`MGPipeApplierShareGroupCountForTesting`** 是测试观测面，不是生产 API。
- **`MGPipeApplierState` 的九张表名与 `Objects.X` 两种拼法并存**是刻意换取 ~200 个读点不动的代价；改名的机会在 twin registry 一起做的那一刀。
- **`g_backendContextGeneration`/`g_syncContextGeneration`、ServerLoop 的 MakeCurrent 去重**同 S4 一节所列，仍未动。


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
