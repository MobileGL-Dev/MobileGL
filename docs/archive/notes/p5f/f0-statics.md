# P5f f0 普查（只读）：进程级静态量中语义属于 per-context / per-session 的成员

> 基线：`feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`，其后提交仅动文档）。
> 范围：`MobileGL/` 下 `MG_Backend`（DirectGLES / DirectVulkan / MGPipe）、`MG_Impl`、`MG_Pipe`、
> `MG_Remote`、`MG_State`。方法：`g_`/`s_` 前缀可变全局、函数级 `static`、`inline X& = *new X()`
> 单例、命名空间级非常量变量的全面 grep，再逐族读代码确认键控方式与重置时机。
> 本报告只读产生；未修改任何代码、未构建、未跑测试。

## 0. 判定框架

P5f 关心的是「换一个地址空间还成立吗」的一个变体：**这个静态量的语义寿命跟谁走**。

| 分类 | 含义 |
|---|---|
| **【A】P5f 必须按角色+世代分区** | 语义属于某个 GL context / session，且当前的重置时机挂在错误的钟上（或不挂任何钟），f1 双块演练下必须暴露 |
| **【B】进程级即可** | 拆进程后每个进程自然各有一份，且持有者在 spawn 后唯一（server 独有 / client 独有），或纯诊断/配置 |
| **【C】已是 per-context / per-session** | 已有世代键控自失效或已有 per-session 重置机制，只需 f1 确认 |
| **【D】未知** | 只读普查不足以判定，标出待 f1/后续包回答的问题 |

一条贯穿全报告的事实先记住：**拆进程后 Espryt/Magma 后端代码只在 server 进程运行，MG_Impl 只在
client 进程运行**（P5F-WIRE-COMPLETENESS.md §1.4：「MG_Impl 根本不在 server 进程里」）。所以
`MG_Backend` 下的静态量天然只需 server 一份、`MG_Impl` 下的天然只需 client 一份——**单看拆进程，
绝大多数成员是安全的**。真正的缺陷集中在两类：(1) 重置时机挂错钟（挂 server 自己的钟，而语义属于
client context 世代）；(2) 静态量里存着 client 对象的指针/SharedPtr。

## 1. 已知成员的核实与重置时机

### S1 `g_syncedRenderStateParameters` 家族 —— 【A】

- 证据：`MG_Backend/DirectGLES/DirectGLES.cpp:4584-4617`，同族 `g_syncedRenderStateVersion`、
  `g_hasSyncedRenderState`、`g_syncedBackendViewport`、`g_syncedBackendScissorBox`、
  `g_syncedSrgbFramebufferWrites`、`g_forceFullRenderStateResync`、`g_syncedColorMaskAlphaWidenMask`、
  `g_dualSourceDeclinedBlendStates`。
- 语义：「后端 ES context 上当前已推下去的渲染状态」的影子，diff 对象是**前端**
  `RenderStateParameters` 的版本与字节（`DirectGLES.cpp:4629-4660`）。即 per-(client context ×
  server ES context) 的乘积状态。
- **重置时机**：`RenderStateImpl::InvalidateSyncedRenderState()`（`DirectGLES.cpp:4618-4623`，置
  `g_forceFullRenderStateResync=true` 并清空四个影子）。调用点两个：
  (a) `DirectGLES::MakeCurrent()` 尾部（`DirectGLES.cpp:15375`）——每次 backend make-current；
  (b) viewport-routing 回放收尾（`DirectGLES.cpp:7930`）。注意 `DestroyEGLContext()`
  （`DirectGLES.cpp:15896-15912`）的清理清单**不含**它——靠「新 context 必经 MakeCurrent」兜底。
- 今天为何能工作：inproc 单进程，ES context 与 client context 同在；ID-67 裁定「相同 tuple 的
  make-current 跳过 native bind」，但 `BackendObject_DirectGLES.cpp:40-42` 的注释写明不同 virtual
  context 仍进 `MakeCurrent`，七个缓存在那里统一失效。
- P5f 处置建议：拆进程后只有 server 持有、重置挂在 server 自己的 MakeCurrent 上，跨进程方向成立；
  但 (1) f1 双块下必须把它归入 server 角色的块，使 push-monolith 臂与 server 臂不共享；(2) 待验证点：
  server ES context 换代（`g_backendContextGeneration` 在 `DirectGLES.cpp:15912` 自增）而 client
  tuple 不变时，`MakeCurrent` 是否仍被调用——若是则靠 :15375 兜住，若否则此家族缺一条挂
  `g_backendContextGeneration` 的失效线。**该点只读无法定论，标为待 f1 双块回答。**

### S2 `ScopedDefaultUnpackState::s_synced` 家族 —— 【A】（全清单里最弱的一个）

- 证据：`MG_Backend/DirectGLES/Managers.cpp:6258-6325`。类内 `static inline` 成员：
  `s_synced`（:6319）、`s_alignment/s_rowLength/s_skipRows/s_skipPixels/s_imageHeight/s_skipImages`
  （:6320-6325）。
- 语义：后端 ES context 的 GL_UNPACK_* 像素存储状态影子（纹理上传路径的 save/restore）。
- **重置时机：没有。** `s_synced` 在 `EnsureShadowSynced()`（:6283-6300）里一次性置 true 后**永不
  清除**；`grep` 确认它不在 `DestroyEGLContext()`（`DirectGLES.cpp:15896-15912`）的清理清单里，
  也不在任何 `Invalidate*` 里（`PixelStoreImpl::InvalidatePackStateCache`
  （`Managers.cpp:11565-11567`）只动 PACK 侧的 `g_packState`）。
- 今天为何能工作：两层巧合——(a) 本类是唯一写者且析构恒恢复到 resting default (4,0,0,0,0,0)
  （类注释 :6250-6257）；(b) 新 ES context 的 GL 默认值恰好等于该 resting default，故 ES context
  销毁重建后影子「恰好仍对」。CONTRACT-P5C.md:538-539 已把它与 S1 一起记为 P6 的债，P5f §2.4 收回。
- P5f 处置建议：把 `s_synced=false` 挂进 `DestroyEGLContext` 的清理清单（或挂
  `g_backendContextGeneration` 比较），这是「进程级静态无世代重置」的最小确定样本；f1 双块下归入
  server 块。

## 2. 新发现

### S3 后端 memo/影子大家族：键控 (client 上下文 id/serial, `g_backendContextGeneration`)，自失效 —— 【C】

Espryt 的 per-draw memo 几乎全部自带双键：前端侧键（`ContextId`/`ContextSerial`/内容哈希）加
`g_backendContextGeneration`（`DirectGLES.cpp:15912` 处自增）。全部成员（grep 命中的完整枚举）：

| 成员 | 位置 | 键 / 重置 |
|---|---|---|
| `g_observedUnitBindings` 等 5 个 | DirectGLES.cpp:2767-2771 | (contextId, bind generation, maxUnit) 三重门，见 :2773-2782 注释 |
| `g_unitTextureSyncListByHandle*` 7 个 | DirectGLES.cpp:3002-3008 | 记录臂：(ContextSerial, ViewsSerial, g_backendContextGeneration)，:3372/:3404 |
| `g_unitTextureSyncList*` 7 个 | DirectGLES.cpp:3010-3016 | 前端臂：(ContextId, epoch, generation)，:3420/:3464 |
| `g_fboTextureSyncList*` 8 个 | DirectGLES.cpp:3046-3066 | {Fbo, ContentHash, ContextSerial, g_backendContextGeneration}，:3522/:3550 |
| `g_fboAttachmentSyncList*` 6 个 | DirectGLES.cpp:3073-3078 | 同上，:3231/:3236 |
| `g_readFboAttachmentSyncList*` 6 个 | DirectGLES.cpp:3082-3087 | :3249/:3254 |
| `g_readFboTextureSyncList*` 8 个 | DirectGLES.cpp:3182-3189 | :3299/:3330 |
| `g_imageSweep*` 7 个 | DirectGLES.cpp:4013-4024 | 记录臂三 serial + generation，:4074-4094；前端臂 :4088 |
| `g_unitSamplerWalk*` 5 个 | DirectGLES.cpp:6627-6631 | (contextId, epoch, maxUnit, generation) + 行 memcmp，:6640/:6770 |
| `g_resolvedTextureBindingMemos` + cursor | DirectGLES.cpp:6859-6860 | 每 program 4 槽 round-robin memo，键含 lifetime id（:6921-6940） |
| `g_fboSyncedSerials` / `g_fboRecordsTrusted` | DirectGLES.cpp:4137 / :4146 | `InvalidateFramebufferHandleArmMemos()`（:4161-4164），挂 `InvalidateFramebufferBindingCache`（Managers.cpp:9726）→ MakeCurrent（:15367）与 DestroyEGLContext（:15903-15905） |
| broadcast memo 7 个 | DirectGLES.cpp:5324-5350 | `PrgramImpl::InvalidateBroadcastMemo()`（:5358），挂 MakeCurrent（:15364）与 DestroyEGLContext（:15909） |
| `g_currentDrawFrontendProgram`/`g_currentDrawBackendProgram`/`g_currentDrawProgramHandle` | DirectGLES.cpp:5300-5313 | 每次 PrepareForDraw/Compute 顶部重写（:5291-5298 注释） |
| `g_passes`/`g_passCount`/`g_routedProgram` | DirectGLES.cpp:7779-7780 | 每次 replay 收尾清零（:7915-7924） |

- 今天为何能工作：双键俱备且失效方向保守（键不符即重建）；`MGPipeApplierReset()` 在 context 切换
  时清空 applier 的记录表，令 `ContextSerial`/记录哈希侧的键同时移动（DirectGLES.cpp:1022-1029
  的注释即此论证）。
- P5f 处置建议：**进程级即可，不新增机制**。spawn 后只有 server 跑 Espryt；记录臂的键全部来自
  applier 自己的 serial（APPLIER_DERIVED），不依赖 client 内存。f1 双块只需验证 client 角色下无人
  读写它们（双块会把这种访问变成具名红）。

### S4 纯 server-ES-context 状态（绑定影子 / scratch / 池 / 环 / EGL 句柄）—— 【B】

描述的是 server 自己的 ES context 或 renderer，不携带任何 client 身份。重置统一挂在
`DestroyEGLContext()`（`DirectGLES.cpp:15896-15912`，依次调 `BufferImpl::OnBackendContextDestroyed`
（Managers.cpp:3146）、`XfbImpl::OnBackendContextDestroyed`（DirectGLES.cpp:2067-2074）、
`MultiDrawImpl::OnBackendContextDestroyed`（MultiDraw.cpp:1086）、`OnRestartSubstitutionContextDestroyed`、
`ScratchFBOImpl::OnBackendContextDestroyed`（Managers.cpp:11509）、`ReleasePackedWordScratchTexture`
（DirectGLES.cpp:14150-14155））与 `MakeCurrent()` 的 Invalidate 序列（:15363-15375）：

- DirectGLES.cpp：`g_Display/g_Context/g_Surface/g_Config`（:14834-14837，后端唯一 ES context
  句柄）、`g_requestedSwapInterval`（:15229，per-surface，surface 重建时重放）、
  `g_backendContextOwnerThread`（:15289）、`g_syncContextGeneration`（:15295）、frame fence 环
  （:15304-15312）、`g_eglVerifiedFrameSerial/g_eglVerifiedContextGeneration`（:15415-15416）、
  resolve scratch（:9462-9467，自含世代守卫 :9488）、`ReplicateBlitImpl`（:9777-9789，守卫 :9851）、
  `DepthStencilSamplingReadImpl`（:12876-12911，守卫 :12941）、packed-word scratch
  （:14101-14103）、restart scratch（:8395-8396）、`g_writableImageBufferUnits/g_writableImageBufferUnitCount`
  （:3605-3607）与 `g_imageUnitHighWaterMark`（:3633，自恢复语义见 :3600-3604 注释）。
- Managers.cpp：`g_backendContextGeneration`（:68）、绑定影子（:811-822、:3777-3791、
  :4790-4791、:9578-9582 `g_activeTextureUnit`/`g_boundTexturesCache`、:11520-11521 pack 影子）、
  `g_bufferContextGeneration`（:826）、deferred releases（:852-857）、buffer pool（:873-875）、
  三条 persistent ring（:1017-1039）、`g_backendBufferResources`（:2873）、`g_bufferBackendIdGeneration`
  （:3108）、`g_backendSamplerViews`（:4649）、`g_pendingFetchBaseInstance`（:4850）、alpha/integer
  mask（:9929-9930）、`g_attachmentBackendIdGeneration`（:11277）、scratch FBO（:11283-11287）、
  program 影子（:11571-11574）。
- MultiDraw.cpp：scratch 组（:329-344）、tier 解析（:464-466）、`g_announcedTiers`（:483）。
- BackendObject_DirectGLES.cpp:54-58：native bind 影子，注释自述「Process-wide like the native
  state it mirrors; under split exactly one DirectGLES object exists (the server's)」。
- 处置建议：进程级即可；唯一待办是 f1 双块下确认 client 角色不触碰（EsprytSlotTablesEnabled 等
  latch 已在拆分臂下拒绝）。

### S5 `TwinLookupMemo` 三件套 + `g_fbSlotCache*`（legacy 臂）—— 【B】，含一条 D

- 证据：DirectGLES.cpp:143-152（`g_vaoTwinLookupMemo`/`g_programTwinLookupMemo`/`g_fboTwinLookupMemo`，
  仅 `MOBILEGL_PIPE_LEGACY_MEMOS` 编译）、:167-168（`g_fbSlotCacheContext`/`g_fbSlotCache`，pull 或
  legacy 臂编译）。
- 语义：以前端对象地址为键的 memo——**per-client-context 语义**，因为键是 client 堆地址。
- 今天为何能工作：memo 注释（:88-102）论证 registry 条目跨 ES context 存活、弱指针防地址复用；
  `g_fbSlotCache` 以 context 裸指针为键、指针比较即失效（:155-163）。
- P5f 处置建议：拆进程后 server 进程没有 client 对象，这两条臂在 server 上根本不编译/不被选中
  （句柄臂取代）；进程级即可。但留一条【D】：push-monolith 构建下它们仍在运行，若 f1 双块让
  monolith 臂与 server 臂同进程共存，这两个 memo 属于 client 角色块——f1 的归属判定要覆盖它们。

### S6 XfbImpl 家族：map 里存 client 对象的 SharedPtr —— 【A】

- 证据：`g_xfbObjects`（`DirectGLES.cpp:1548`）、`g_currentXfbName`/`g_currentXfbState`
  （:1549/:1558）、`g_scatterBufferId/g_scatterBufferSize`（:1545-1546）。`XfbObjectState` 持有
  `SharedPtr<MG_State::GLState::BufferObject> buffer`（:1511）与
  `SharedPtr<MG_State::GLState::ProgramObject> scatterProgram`（:1538）。
- 语义：按前端 XFB 对象名键控的捕获状态——per-client-context 语义，且**值里嵌着 client 对象指针**。
- 重置时机：`XfbImpl::OnBackendContextDestroyed()`（:2067-2074）清表——只随 server ES context
  销毁走，**不随 client context 世代走**。
- 今天为何能工作：同地址空间 + lockstep；P5c 已把 XFB scatter 的读改到 server staged shadow，但
  这个 map 的 SharedPtr 成员本身仍在。
- P5f 处置建议：此项同时落在 §2.6（反向通道触碰 client 内存）的 fv 包里；从静态量角度它必须
  按角色+世代分区，且成员里的 `SharedPtr` 要在 spawn 前换成句柄或 server 侧对象——与 T5 同类的
  跨角色持指针。

### S7 `g_rawDepthFetchSamplerState`/`g_rawDepthFetchSamplerBackend` —— 【D】

- 证据：DirectGLES.cpp:72-73；惰性构造于 `GetRawDepthFetchSampler()`（:301-313），**无重置点**。
- 语义：raw-depth-fetch 替代的 server 自建 frontend SamplerObject 及其 backend twin（P4a D-F3 具名
  保留，:284-300 注释）。
- 今天为何能工作：内容是不可变常量（构造即定型），`SyncToBackend` 每次调用重同步。
- P5f 处置建议：跨进程后 server 自建自持有，方向成立；但 `BackendSamplerObject` 内部是否持有
  driver sampler id（若是，则 ES context 换代后失效无人清）只读未确认——**未知**，留给 fs 包顺手
  回答（一行 grep `BackendSamplerObject` 的成员即可）。

### S8 `g_anyProgramRoutesViewportIndex` —— 【B】，附注

- 证据：Managers.cpp:87；置位点 :14028（program link），**只升不降，无重置**。
- 语义：「本进程编译过的 program 里是否有任何一个路由 viewport index」的 latch。
- 今天为何能工作：方向保守——latch 卡在 true 只会多做 viewport routing 检查，不会错画面。
- P5f 处置建议：进程级即可（per-server-进程的程序集合属性）；不需修，仅记录它是「无重置 latch」
  家族的一员。

### S9 `g_bufferMutationEpoch` —— 【C】（已被角色守卫钉在 server 侧）

- 证据：Managers.cpp:834；bump 点全在 `Ops_*Tracked` 与 DirectGLES 的 6 处
  （DirectGLES.cpp:1654/:1679/:1828/:12828/:14393/:14808）；P5e 已加
  `Fatal{RoleViolation, "CurrentBufferMutationEpoch"}` 守卫（Managers.cpp:2853）把 GL 线程 bump
  钉死（仅 bring-up 例外，:2811-2830 注释），并有 `g_bufferMutationEpochBumpsOffTheApplyThread`
  （:2803）计数供测试断言为零。
- 语义：memo 干净探针的跳过时钟；语义上「谁的 buffer 变了」是 client 事件，但 P5e 已裁定 bump 只
  发生在 apply 线程（server 角色）。
- P5f 处置建议：已是角色正确；进程级即可。f1 双块下它归入 server 块。

### S10 Magma（DirectVulkan）—— 【B】

- `g_rendererGeneration`（DirectVulkan.cpp:44）：per-renderer 世代，renderer 重建时
  `BumpRendererGeneration()`（:51-53）；server 独有。
- `g_activeBufferManager`（Renderer/VkBufferManager.cpp:56）：当前 renderer 的 manager 指针；
  server 独有。
- `g_dynamicStateShadow`（Renderer/VulkanRenderer.cpp:409）：Vulkan 动态状态影子，per-renderer。
- `s_warnedHalfTessellatedPipeline`（PipelineFactory.cpp:528）、`s_warnedInstanceIndexUnsupported`
  （ProgramFactory.cpp:2635）：log-once，进程级。
- 注：VulkanRenderer.h:718-730 的 `s_vkResetQueryPool` 等是**类成员**（无 `static`），不在普查范围。
- 处置建议：进程级即可。Magma 臂的 lockstep 掩盖问题属 §2.2（fm 包），静态量本身无跨角色语义。

### S11 MG_Impl/Pipe 的 client 侧单例群（16 个 Instance() 泄漏单例 + PipeFill 静态）—— 【B】

- 证据：`MG_Impl/Pipe/` 下 `CompositeResolver.h:266`、`CsoCache.h:202`、`FramebufferEmit.h:656`、
  `ImageEmit.h:194`、`PipeFill.cpp:1711`（publication latch）、`ProgramEmit.h:725`、
  `ResourceTracker.h:550`、`SamplerEmit.h:522`（cso cache）/`:679`（opaque units）/`:1082`、
  `SetHashSuppressor.h:130`、`ShaderBufferEmit.h:273`、`SlotAllocator.cpp:433`、`TextureEmit.h:1344`、
  `Tracker.h:932`、`VertexInputEmit.h:441`；`CsoCache.h:129` `s_hashForTest`（测试钩子）。
- PipeFill.cpp 的命名空间静态：`g_lastFillWasBarriered`（:486）、`g_omission` 三件套（:509-511，
  verify 旋钮）、`g_snapshot`/`g_readScratch`（:565-566，verify 双臂）、`g_verify`（:591）、
  `g_deferredDestroy*`（:1166-1168）、`g_fillPlan`（:2822）、`g_attribDefaultRepairs`（:2942）、
  `g_attribDefaultLastHeader`（:2949）、`g_residualDue`（:3152）、verify 探针 `probe`（:2664）。
- 语义：per-client-context/session 的发射与追踪状态。
- 重置时机：make-current 边沿（`tracker.FreshlyPrimed()`，`PipeFill.cpp:2654-2667`；该处同时是
  `applier_reset` 记录的 producer，并做 client 侧的 CsoCache / hash suppressor / vertex-input
  emitter 重置，CONTRACT-P5C §5.1）。
- 今天为何能工作：单 client、单 context/session（P5c 裁定每 session 恰一个 context）。
- P5f 处置建议：进程级即可——spawn 后它们只存在于 client 进程；f1 双块时归入 client 块。多
  context 是 P6+ 的形状，`MGPApplierReset::ContextSerial` 的注释（CONTRACT-P5C §1）已写明。

### S12 `MG_Impl/GLImpl` 的 query/sync 注册表 —— 【B】，含一条多 context 语义债

- 证据：`GL_Query.cpp:55-66`（`g_queryObjectsMutex`/`g_liveQueryObjects`/`g_nextQueryId` 与四个
  active-query id + pipeline-statistics map）、`GL_Sync.cpp:34-37`（`g_syncObjectsMutex`/
  `g_liveSyncObjects`）。
- 语义：GL 对象（query/sync）按规范是 per-context 的，这里存成进程级注册表。
- 今天为何能工作：单 context/单 session；mutex 防多 JVM 线程迁移（:50-54 注释）。
- P5f 处置建议：拆进程后天然在 client 进程，进程级即可；但「语义 per-context、存储 per-process」
  这一事实记入 P6+ 多 context 债（与 S11 同一形状）。

### S13 MG_Remote 会话单例与跨角色标量 —— 混合

- `ClientSession::g_active`（Client/ClientSession.cpp:48）、`ServerSession::g_active`
  （Server/ServerSession.cpp:349）、`ServerLoop` 单例（Server/ServerLoop.cpp:774）：每进程一个
  session 是设计本身——【B】。
- `g_applyThreadInsideApplier`（Client/ClientSession.cpp:118）：**inproc 下 server 写、client 读的
  跨角色共享原子**，语义 per-session。CONTRACT-P5C §6（A1）已具名其为 barrier 簿记。拆进程后
  client 那份恒 false，但它是 wait-rule 机制的一部分而非状态内容——【A】：f1 双块/角色分区时必须
  按角色拆开，否则会漏掉「client 读自己那份永远 false」的假绿。同址的 `thread_local
  g_inBarrierWait`（:117）是 client 线程本地，【B】。
- `WireTables.cpp:91-104`（`g_emitted`/`g_declined`/`g_clientTablesUninstalled`）、
  `WireTables.cpp:642`（`g_applierResetContextSerial`，per-session 计数器，client 侧）、
  `EmitTables.cpp:91-96`（drop 旋钮与 `g_presentOrdinal`/`g_publishedMaxRecordBytes`）、
  `EmitTables.cpp:1543`（`g_fenceMutex`）、`EmitTables.cpp:711`（发射 scratch）、
  `CapsMirror.cpp:34-40`（hook + 计数器）、`CapsMirror.cpp:169`（单例）、
  `GpuWritePending.cpp:171`（producer marks）、`PersistentMapTracker.cpp:85-136`（SIGSEGV 追踪）：
  全部 client 侧，【B】。
- `PipeWireCodec.cpp:347` `g_processResolverTable`：本进程段表指针，每进程各一份即正确，【B】；
  同文件 :355 的 `t_activeDecoder` 是 thread_local，已是 per-thread。
- `ServerLoop.h:126` `g_applyThreadKey`：server 线程身份戳，【B】。
- `StagedShadowStore` 单例（Managers.cpp:1160-1165）：server 侧暂存影子，注释自述「ONE PER
  PROCESS」，【B】。
- Transport 的 `g_spinItersPerUs`（Doorbell.cpp:37）、`g_shmCounter`（ShmSegmentPosix.cpp:47）、
  `g_sectionCounter`（ShmSegmentWin32.cpp:36）：进程级校准/命名计数器，【B】。

### S14 MG_Pipe 核心 —— 已知项定位

- `gPipeInputs`（MG_Backend/MGPipe/PipeInputs.h:835）：P5f §1 的主对象，双块机制（§4）就是为它造
  的；不重述。
- `g_applier`（MG_Pipe/PipeApply.cpp:433）：server-private（CONTRACT-P5C §5.1 裁定），重置经
  `applier_reset` 记录（opcode 77，`MGPipeApplierReset()`，`PipeApply.cpp:1202-1300`）——【C】
  已是 per-session。
- `PipeRoute.cpp:63-69`（`g_arm`/`g_monolithScreen`/`g_monolithContext`/`g_monolithEscapes`）：
  路由表与配置，【B】。
- `PipeInputs.cpp:55` `g_residualPulls`：诊断计数器（rsp），server 侧读，【B】。

### S15 MG_State —— 【B】

- 七个 lifetime-id 分配器（BufferObject.cpp:31、RenderbufferObject.cpp:25、
  FramebufferObject.cpp:20、ProgramObject.cpp:27、SamplerObject.cpp:20、TextureObject.cpp:26、
  VertexArrayObject.cpp:18）与 `s_nextTextureStateContextId`（TextureState.cpp:24）：client 侧单调
  分配器；spawn 后只存在于 client 进程，句柄经记录过线，进程级即可。
- `g_stateObjectDeathOps`（StateObjectDeathNotice.h:52）、`g_bufferBackendOps`
  （BufferObject.cpp:79）：进程级函数表，【B】。
- `g_textureLegacyArmScopeDepth`（MipmapStorage.cpp:112）、`g_magmaP7AllocatorDebtScopeDepth`
  （SlotAllocator.cpp:118）、`g_frontendKeyedRegistryScopeDepth`（SlotAllocator.cpp:143）：P5c 具名
  豁免 scope 的深度计数，守卫簿记而非状态，【B】。
- `ProgramSpirvTask.cpp:35-36`：log-once atomic_flag，【B】。
- `MG_Impl/NSOpenGLImpl/NSOpenGLImpl.cpp:32-33`：macOS IMP 缓存，与拆分无关，【B】。

## 3. 汇总

| 分类 | 条目 |
|---|---|
| 【A】P5f 必须按角色+世代分区 | S1（g_syncedRenderStateParameters 家族）、S2（s_synced 家族）、S6（XfbImpl）、S13 中的 `g_applyThreadInsideApplier` |
| 【B】进程级即可 | S4、S5（附一条 D）、S8、S10、S11、S12（附多 context 债）、S13 大部、S14 的 PipeRoute/rsp、S15 |
| 【C】已是 per-context/per-session | S3（自失效双键）、S9（角色守卫已钉）、S14 的 `g_applier` |
| 【D】未知 | S7（BackendSamplerObject 是否持 driver id）；S1 的「ES context 换代且 tuple 不变时 MakeCurrent 是否仍被调用」 |

**最重要的三条结论**：

1. **两个已知成员里，`ScopedDefaultUnpackState::s_synced` 比 `g_syncedRenderStateParameters` 弱得多**：
   前者没有任何重置点（`Managers.cpp:6319` 一次置位永不清，不在 `DestroyEGLContext` 清单里），正确性
   靠「新 ES context 的 GL 默认值恰好等于影子值」的巧合；后者有 `MakeCurrent()`（DirectGLES.cpp:15375）
   的每次强制全量重推兜底，但兜底链条上「tuple 相同而 ES context 换代」一格只读无法确认（D 项）。
2. **Espryt 的 per-draw memo 大家族（S3）已经自带 (client 键, `g_backendContextGeneration`) 双键
   自失效，是本普查中最大的一类，且全部不需要新机制**——f1 双块对它们只需证明 client 角色不触碰。
   P5f §2.4 的「完整清单未知」可以收口为：真正缺世代钟的只有 S2、S6、S7 三个小点。
3. **S6（`g_xfbObjects` 持有 client 对象 `SharedPtr`，DirectGLES.cpp:1511/:1538/:1548）是本次普查
   新发现的、唯一一个「进程级 map 的值里嵌 client 指针」的成员**，重置只挂 server ES context 销毁
   （:2067-2074），同时属于 §2.6 fv 包的范畴——两个包的清单里都应有它。

### 附：普查方法的可复现性

```
grep -rn -E '^\s*(inline\s+)?(static\s+)?[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*[ \*&](g_|s_)[A-Za-z_0-9]+\s*(=[^=]|\[|\{|;)' \
  MG_Backend MG_Impl MG_Pipe MG_Remote MG_State --include='*.cpp' --include='*.h'
# 加上 '= *new ' / 'Instance()' 单例模式的第二轮 grep；全部命中均已人工过目并归入上表。
# 3rdparty/ 子模块不在范围内（其 git 报错为既有噪音）。
```
