# f0-reverse-channel — P5f §2.6 反向通道逐站点普查（只读）

> 基线 `feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`）。路径在 `MobileGL/` 下。
> 本文件为只读普查，未改任何代码、未发起构建、未跑测试。对应 P5f 计划 §2.6
> （`docs/Disaggregated/P5F-WIRE-COMPLETENESS.md:200-204`）与 P5c 审计表 R1–R5 / B4 行
> （`docs/Disaggregated/ROADMAP.md:61-66`）。
>
> **行号漂移提醒**：任务与 `ROADMAP.md:61`（B4 行）引用的 XFB scatter 旧行号
> （`DirectGLES.cpp:950/1060/1077/1081/1172/1185`）在当前 HEAD 已落在
> `SyncVaoAttributeBuffersByHandle/ByRecord`（VBO memo）区域，与 XFB 无关。XFB 机制现位于
> `DirectGLES.cpp:1508-2072`（`XfbImpl` 命名空间），scatter 本体在 `:1726-1839`，
> 非 scatter 回读臂在 `:1602-1684`。下文一律以当前 HEAD 行号为准。

---

## 0 总览：十个回调，四类有接线

`MGPipeCallbacks` 共 10 个成员（`MG_Pipe/MGPipeCallbacks.h:27-51`，个数有 static_assert 把守）。
逐个数 producer / consumer / 事件种类：

| 回调 | EventKind | server producer | client consumer | 状态 |
|---|---|---|---|---|
| `OnBufferWriteback` | `kEventBufferWriteback`（`EventRing.h:49`） | `ServerSession.cpp:264-292` | `ClientSession.cpp:266-291` → `ResourceTracker.h:575-638` | **已接线** |
| `OnGpuWritten` | `kEventGpuWritten`（`EventRing.h:50`） | `ServerSession.cpp:294-312` | `ClientSession.cpp:292-314` → `ResourceTracker.h:645-667` | **已接线** |
| `OnSurfaceChanged` | `kEventSurfaceChanged`（`EventRing.h:51`） | `ServerSession.cpp:314-330` | `ClientSession.cpp:315-329`（→ `:211-252`） | **已接线** |
| （`OnGlError`） | `kEventGlError`（`EventRing.h:52`，P5c 新增） | **不经回调表**：`PipeInputs::RecordError` 直调 `ServerSession::PostGlError` | `ClientSession.cpp:330-365` | **已接线，但绕过回调表**（见 §5） |
| `OnTextureWriteback` | 无 | 无 | 无 | 零接线 |
| `OnTexturePullRequest` | 无 | 无 | 无 | 零接线（终止符见 §7） |
| `OnMipLevelsGenerated` | 无 | 无 | 无 | 零接线（见 §6） |
| `OnXfbScatterReady` | 无 | 无 | 无 | 零接线（见 §3） |
| `OnCapsInvalidated` | 无 | 无 | 无 | 零接线，由 R-12 caps 重发布取代（`CapsMirror.cpp:78-80`） |
| `OnLog` | 无 | 无 | 无 | 零接线 |

安装/卸载：`ServerSession::Accept` 只装三个（`ServerSession.cpp:538-542`），`Close` 对称卸载
（`:622-632`）；双重安装是 `Fatal{RoleViolation,"callback-double-install"}`（`:337-347`）。
consumer 侧按名直调、绝不回走 `gMGPipeCallbacks`（`ClientSession.cpp:203-206` 注释，防
layer-2 反调）。这套所有制关系本身**没有跨进程问题**，P5f 无需动。

---

## 1 XFB scatter（Espryt）——字节路径已迁移，对象身份仍指回 client

这是 B4 行的本体。当前 HEAD 的传输臂形状（P5c 已落）：

- **非 scatter 臂** `ReadbackCapturedRanges`（`DirectGLES.cpp:1602-1684`）：transport 下
  （`:1616-1656`）按 handle 解析 server 资源（`:1622-1623`）、map 的是 **ES 驱动 buffer**
  （`:1626-1629`）、字节经 `OnBufferWriteback` 事件回传（`:1638-1643`），serial/epoch 的
  序按 Ops_H_Readback 同款规则（`:1650-1654`）。monolith 臂（`:1658-1680`）仍直接
  `target.buffer->WritebackFromBackend`（`:1675`），被 transport 分支的 `continue` 隔开。
- **scatter 臂** `ScatterCapturedRecords`（`:1726-1839`）：transport 下预捕获字节读的是
  **server 自己的 staged shadow**（`:1776-1794`，含 `RequireStagedCoverage` 覆盖断言
  `:1789-1790`），拼好的范围同样经 `OnBufferWriteback` 回传（`:1807-1819`），不再读改写
  client shadow。monolith 臂 `target.buffer->MappedData()` / `WritebackFromBackend`
  （`:1794`/`:1826`）保留。

**仍触碰 / 依赖 client 内存的残余**（都在 transport 臂上，逐条列）：

1. **`scatterProgram` 是 client `ProgramObject` 的 `SharedPtr`，并在 apply 线程上直接调它的
   方法。** 来源：`MGB_CTX->GetTransformFeedbackProgram()`（`:1871`，BARRIER_PULLED 字段，
   `FieldOwnership.def:106` 记 "P3b/P4b (Espryt), P7 (Magma)"），存进
   `XfbObjectState::scatterProgram`（`:1538`、`:1925`）。scatter 时读
   `GetTransformFeedbackPackedStride()`（`:1731`）、`GetTransformFeedbackStride()`（`:1766`）、
   `GetTransformFeedbackVaryings()`（`:1796`）——**这些都是对 client 对象体的读**，
   字节不走 client shadow 了，但 layout 元数据仍从 client 对象上现取。
   - 今天为何能工作：同地址空间 + barrier/run-ahead 窗口内对象存活（P5e 的 `ReleaseFillPins`
     机制钉住 O 类 SharedPtr，`ClientSession.cpp:1073-1082`）。
   - P5f 处置：这是 §2.1 `GetTransformFeedbackProgram` 字段退役（P3b/P4b 债）与 fv 的交集——
     scatter 所需的三个 layout 答案应随 XFB span 记录上 wire（值类，可推导），server 侧只留
     快照。**分类：P5f（fv，依赖 §2.1 字段的载体判定先行）。**
2. **`XfbCaptureTarget::buffer` 持有 client `BufferObject` 的 `SharedPtr` 跨整个 span**
   （`:1511`，填充于 `StartPendingTransformFeedback` 的 `:1890-1901`，后者同时读
   BARRIER_PULLED 的 `GetBufferBindingPoint`，`:1891`）。transport 臂只取
   `HandleOfBuffer(target.buffer.get())`（`:1622`/`:1779`）——**用 client 裸地址当 registry
   键**，落在具名豁免 `MGPipeFrontendKeyedRegistryScope` 内（`Managers.cpp:3063`）。
   - 今天为何能工作：同地址空间下地址即身份；scope 把债记名。
   - P5f 处置：targets 快照应改持 `{handle, range}` 纯值，地址键探针随 §2.5 registry 重键
     （包 `fr`）一起退。**分类：P5f（fr ∩ fv；本身不是字节读写，但跨进程后"client 地址"无意义）。**
3. **`MGB_CTX->GetTransformFeedbackCapturedVertices()`（`:1733`）是 RECORD_SUPPLIED**
   （`PipeFieldOwnership.inc:85`，emitter `SetContextValues`，`PipeFill.cpp:3064`），
   **不是债**，列出以免后人重复普查。

另外 XFB 整条机制读 `GetBufferBindingPoint`（BARRIER_PULLED，`FieldOwnership.def:80`）的退役
属 §2.1 字段包，fv 不应重复记账，只登记依赖关系。

## 2 `OnBufferWriteback` —— 已接线，blobref 约定闭环（无 P5f 债）

**producer（后端侧，3 处）**，全部以 `Seg = kMGHostSpanSegNone` + host 地址形态交出：
`Managers.cpp:2451-2455`（`Ops_H_Readback`，readback verb 的字节）、
`DirectGLES.cpp:1638-1643`（XFB 非 scatter 臂）、`DirectGLES.cpp:1809-1814`（XFB scatter 臂）。
三处都带"无通道则 MGLOG_E_ONCE 丢弃"的显式降级（如 `:1644-1648`），不是静默。

**producer（会话侧）** `ServerOnBufferWriteback`（`ServerSession.cpp:264-292`）：
入参 `Seg != kMGHostSpanSegNone` 即 `Fatal{ProtocolCorruption,"OnBufferWriteback.Seg"}`
（`:266-276`）——**后端边界只接受 host span，段拷贝是这个函数自己的活**；字节 memcpy 进
SEG_EVENT 记录内联尾（`:285-290`）。满环经 `ReserveEventOrBlock`（`:211-262`）：
run-ahead 服务器park 等 client 排空（CONTRACT-P5E §2.6 流控），lockstep 服务器
`Fatal{EventRingOverflow}`。整 buffer 大于半环的切片在 client 侧发起处完成
（`BufferObject.cpp:593-612`，`BufferWritebackSliceBytes`）。

**consumer** `DrainEventRing`（`ClientSession.cpp:266-291`）：在此构造
`Seg = kSegEvent` + `OffsetInSegment` 的 blobref（`:282-285`），交给
`MGPipeClientOnBufferWriteback`（`ResourceTracker.h:575-638`）——三臂校验齐备：
kSegEvent 经 client 自己的 SegmentTable 界检解析（`:596-617`）、transport 下 host span 即
Fatal（`:584-593`）、其他 Seg 即 Fatal（`:618-624`）。写 client shadow 的是 GL 线程上的
`WritebackFromBackend`（`:636-637`），归属正确。

- 今天为何能工作：约定是**双向 Fatal 执法**的，不只是文档；wire 形态（`Seg=kSegEvent`）
  在跨越点被强制。
- P5f 处置：**无**。剩余课题（批处理、epoch 排序、`eventRingFull`/`eventDropped` 的丢弃
  策略）都是 P9 的，`EventRing.h:14-16` 与 `CONTRACT-P5C.md §4.4` 已记名。

## 3 `OnXfbScatterReady` —— 声明存在，全仓零接线

仅有声明（`MGPipeCallbacks.h:50`，注释写明设计意图：server 交回 packed scratch、
**client 自己 scatter**，`:48-49`）。grep 全仓无 producer、无 consumer、无对应 EventKind；
`CONTRACT-P5C.md:536` 把它与 `OnTexturePullRequest` 并列记给 P9。

- 今天为何能工作：P5c 选的是另一条路——scatter 留在 server，读 server staged shadow、
  结果走 `OnBufferWriteback`（§1）。两条路在语义上互斥地覆盖同一个需求。
- P5f 处置：**无**。是否改走"client scatter"是纯设计/异步化选择（P9 的题材，
  `ROADMAP.md:34` P3b/P4b 行也记了"XFB scatter 搬到 client"）。若 P9 采纳，§1 的
  残余 1/2 自然消解（layout 与 targets 都回到 client 侧）。**分类：P9。**

## 4 `OnGpuWritten` —— Espryt 已净，Magma 有一处响亮但真实的直捅兜底

**Espryt**：`MarkBufferGpuWritten`（`Managers.cpp:3072-3103`）在资源族臂上只发事件
（`:3101-3102`，单条 `kMGPipeWholeBuffer` 全量 range——收窄是 P8/P9 的，
`ResourceTracker.h:646-656` 有 assert 把守形状）。它在 transport 下的三个原调用点已被
P5e 删除或门控：`MarkShaderStorageBuffersGpuWritten` 整体早退（`DirectGLES.cpp:845`，
注释 `:828-844` 说明 client 的 `MarkShaderStorageBindings` 已覆盖同集）；
`SyncAtomicCounterBuffers` 的记录臂不标（`:918-924`）；
`MarkWritableImageBufferTexturesGpuWritten` transport 下整函数 return（`:3884-3887`）。
client 侧保守集在 `MG_Remote/Client/GpuWritePending.{h,cpp}`（GL 线程自标，窄化通道反向
运行）。**Espryt 无 P5f 债。**

**Magma**：三个站点经 `MGPipeAnnounceBufferGpuWritten` 走回调——
`UniformManager.cpp:1082`（storage texel buffer，Access != READ_ONLY）、
`UniformManager.cpp:1244`（storage block）、`VulkanRenderer.cpp:11778`（XFB capture）。
但 helper 本体（`ResourceTracker.h:710-743`）有两个跨进程不成立的点：

1. handle 来自 **`MGPipeSlots().FindByLifetimeId`（client 分配器探针）**，套在
   `MagmaP7AllocatorDebtScope` 具名豁免里（`:717-730`）——P7 债，记名在册。
2. **handle 缺失时的兜底是直捅 client 对象**（`:732-739`：MGLOG_E_ONCE 之后
   `bufferObject->MarkGpuWritten()`——apply 线程写 client 内存）。注释说这是"响亮而非
   静默"的故意选择；在 P5f 双块演练下这一支会成为真实违规。

- 今天为何能工作：同地址空间；Magma 跑 lockstep（不发布 `kCapRunAheadApply`），
  client 停在 barrier 后，直捅恰好安全——ID-132 的那类掩盖。
- P5f 处置：**分类 P5f（fm/fv）**。把兜底从"直捅"改成具名 Fatal 或携带性降级
  （例如记 pending 由 client 侧保守集兜底——反正 GpuWritePending 已按绑定走查标过同集，
  这个兜底在语义上是冗余保险而非正确性来源）；lifetime-id 探针随 Magma 的 §2.2/fm 一并退。

## 5 `OnGlError` —— 已接线，但绕过了回调表；顺序保证是"下一个 drain 点"

**路径不经 `gMGPipeCallbacks.OnGlError`**：该成员声明于 `MGPipeCallbacks.h:29`，
**全仓零安装、零调用**。实际路径是 `PipeInputs::RecordError`（`PipeFill.cpp:2164-2186`）
在 transport 下直接调 `ServerSessionInstance().PostGlError`（`:2175`），后者产
`kEventGlError`（`ServerSession.cpp:709-739`，消息内联、NUL 随带、producer 侧截断到
1024，`EventRing.h:108-115`）。consumer `ClientSession.cpp:330-365` 校验
`MessageBytes != 0` 且 NUL 在位（否则 `Fatal{ProtocolCorruption,"kEventGlError"}`），
在 GL 线程写 client 自己的 `pGLContext->RecordError`（`:353-357`）。

**顺序保证的现状**（任务点名项）：所有 producer 都在 apply 线程上、共用同一个 SEG_EVENT
环，所以 **错误事件与 writeback/gpu-written/surface 事件之间保 FIFO**；client 侧的
保证是"**下一个 drain 点观测到**"——保住的是同线程的 error-then-read 程序序，
不保跨 verb 交错。这是写明并接受的局面，不是漏洞：`PipeFill.cpp:2168-2173`、
`ClientSession.cpp:350-352`、`CONTRACT-P5C.md §4.2`（表格第 4 行，`:281`）、
run-ahead 下的松弛在 `CONTRACT-P5E.md:181`。`FieldOwnership.def:164` 的 `RecordError`
行仍挂 BARRIER_PULLED "P9"（`MGP_STICKY_FORWARD_PULL` 宏仍执行、计数仍走 `rsp`），
有序化退役排在 P9。

- 今天为何能工作：错误队列是 client 状态，写它发生在 GL 线程 drain 时（归属正确）；
  序的保证靠 drain 点分布（§8）而非锁。
- P5f 处置：**本体分类 P9**（有序化、`OnGlError` 回调成员的正式接线）。P5f 只需登记一个
  不对称事实：**错误事件不走回调表，因此 §4.1 的双重安装守卫与"十个成员"的记账方式
  盖不住它**；若 fv 收口时顺手把 `gMGPipeCallbacks.OnGlError` 也接到
  `ServerSession::PostGlError`（或显式标注该成员永不经表），可消除"将来有人调
  `OnGlError` 而静默空转"的坑。这是可选的整洁项，不是跨进程缺陷。

## 6 `OnTextureWriteback` / `OnMipLevelsGenerated` —— 零接线，且今天的 Espryt 不需要它们

全仓零 producer / consumer / EventKind。核查它们"本该服务"的路径在 transport 下的现状：

- `glGenerateMipmap`（Espryt）：transport 下走记录臂（`DirectGLES.cpp:12093-12117` →
  `GenerateMipmapByRecord :12037-12081`），纹理由 `VerbMipRes` 句柄解析，存储形状由
  **前端自己分配**、server 侧只做描述符核验（`EnsureGenerateMipmapStorageDescribed`，
  `:11040`起）——这正是 `OnMipLevelsGenerated` 注释里"SHAPE ONLY, client owns the CPU
  shadow"（`MGPipeCallbacks.h:39-41`）想要的形态，只是由"前端先行分配"实现而非事件通知。
- 唯一真正需要 texel 回流的是 RGB16F/RGB32F 的 CPU filter
  （`GenerateThreeChannelFloatMipmapOnCpu` 读改写 client level shadow）：transport 下已被
  具名拒绝 `MGPipeUnmigratedEmulation("generate-mipmap-cpu-filter")`（`:12051-12056`），
  记录保持 barriered（`:12030-12036` 注释）。**它就是 `OnTexturePullRequest` /
  `OnTextureWriteback` 的未来用户**，`CONTRACT-P5B.md:255/:379` 也这么记。
- Magma 侧 mip 直写 client level 存储是 T5（`ROADMAP` §2.2 引用
  `VulkanRenderer.cpp:1562-1590` 一带），属包 `fm`，此处只登记交叉。

- P5f 处置：**无**。**分类：P9**（纹理拉取与 texel 回写通道的建设）；T5 归 fm。

## 7 `OnTexturePullRequest` 与终止符 `ResourceSubDataComplete` —— 整条未建

零 producer。终止符 `ResourceSubDataComplete` 有 wire 位（opcode 50，
`PipeCalls.def:229`、`PipeWire.inc:122`）但 **applier 是显式空 stub**：
`PipeWireCodec.cpp:2070-2073`（"There is no client producer and no applier; the
reverse channel is P7/P9"，`return false`）。`PipeCatalogueTest.cpp:1425` 注释同样记为
later phase。

- 今天为何能工作：缩减路径上没有纹理重铸（recast）场景；唯一会需要的 CPU filter 已被
  具名拒绝（§6）。
- P5f 处置：**无**。**分类：P9**（`ROADMAP.md:37` P9 行"纹理拉取四条缓解 + 终止符"）。

## 8 排空点清单（顺序保证的现实基础，P5f 不需要动）

`kEventGlError` 与所有事件的"下一个 drain 点"具体是（全部在 GL 线程）：
`ClientSession.cpp:170`（事件积压唤醒的二级退出）、`:908`（SEG_CMD 空间等待出口）、
`:1071`（`EmitAndWait` barrier 之后——主排空点，注释 `:1066-1070`）、`:1206`
（`WaitForApplyToCatchUp`，run-ahead 强制对齐）、`:1256`/`:1269`（present credit 等待）、
`:1456`（公共 `DrainEvents`）；另有第二类：阻塞式 EGL 生命周期 RPC 返回处
（`CONTRACT-P5C.md §4.1`，`:263-272`）。每个等待出口都排空是 P5e 流控死锁论证的支柱
（`:1202-1206`）。**这些是全值的拆分机语义，跨进程成立，P5f 无处置。**

## 9 SEG_EVENT blobref 约定核验结论（任务点名项）

约定（`CONTRACT-P5C.md:54`）：wire 上的 `OnBufferWriteback` blobref 必须
`Seg = kSegEvent` + 段内偏移，**永不**是 host 地址；monolith 下保持 `kMGHostSpanSegNone`。
逐环核验：**约定被所有 producer 遵守，且每一环都有执法**——

1. 后端 → 会话：3 处 producer 全部交 `kMGHostSpanSegNone` host span（§2）；
2. 会话入口：`Seg` 检查是 Fatal（`ServerSession.cpp:266-276`）；
3. wire 形态构造：只有 `DrainEventRing` 一处造 `kSegEvent` blobref（`ClientSession.cpp:282-285`）；
4. consumer 三臂：kSegEvent 界检解析 / host span 在 transport 下 Fatal / 其他 Fatal
   （`ResourceTracker.h:583-624`）。

**无违规、无缺口。**

## 10 汇总

| # | 站点 | 触碰 client 内存？ | 分类 |
|---|---|---|---|
| 1 | XFB scatter 字节路径（Espryt） | 否（P5c 已改走 server shadow + writeback 事件） | 无债 |
| 2 | XFB `scatterProgram`（client ProgramObject 的 SharedPtr，apply 线程直读 layout） | **是（读）** | **P5f（fv，依赖 §2.1 `GetTransformFeedbackProgram` 载体判定）** |
| 3 | XFB `targets` 持 client BufferObject SharedPtr + `HandleOfBuffer` 地址键探针 | 身份依赖（不读字节） | **P5f（fr ∩ fv）** |
| 4 | `OnBufferWriteback` 全链 | 否 | 无债（批处理/排序归 P9） |
| 5 | `OnXfbScatterReady` | —（零接线） | P9 |
| 6 | `OnGpuWritten` Espryt 臂 | 否 | 无债（收窄归 P9） |
| 7 | `OnGpuWritten` Magma 兜底（handle 缺失时直捅 `MarkGpuWritten`） | **是（写，响亮兜底）** | **P5f（fm/fv）** |
| 8 | `OnGlError`（`RecordError` → `kEventGlError`） | 否（GL 线程写 client 队列） | 有序化归 P9；回调表不对称是可登记的小项 |
| 9 | `OnTextureWriteback` / `OnMipLevelsGenerated` | —（零接线；唯一用户被具名拒绝） | P9 |
| 10 | `OnTexturePullRequest` + 终止符 | —（整条未建） | P9 |
| 11 | `OnSurfaceChanged` 全链（Espryt `:14959-14970` / Magma `SwapchainObject.cpp:324-339` → `ClientSession.cpp:211-252`） | 否（GL 线程写 client `pDefaultFramebufferInfo`，R3 归属正确） | 无债 |
| 12 | `OnCapsInvalidated` / `OnLog` | —（零接线；前者由 R-12 重发布取代） | P9/无需 |

**P5f 实收三条**（#2、#3、#7），全部集中在 XFB 对象身份与 Magma 兜底；反向通道的
**字节面在 P5c 之后已经干净**。monolith 臂上的直写点（`DirectGLES.cpp:1675/:1826/:12820/
:14391/:14806`、`Utils.cpp:2398`、`Managers.cpp:2025`）逐一核过：transport 下或被
`#else` 臂隔开，或被 client 侧具名拒绝把门（`EmitTables.cpp:976-980` 的
`UnmigratedVerbFatal("ReadPixels+PACK_BUFFER")`、`PipeWireCodec.cpp:2105-2108` 的
`GetTextureImage` class-C 拒绝），均不可达，不构成 P5f 项（其中 `ReadPixels` 服务器臂对
`GetBufferBindingSlot(PixelPack)` 的残余读，`DirectGLES.cpp:14337`，属 §2.1
`GetBufferBindingSlot` 行 "P9 (readback)"，不在本清单重复记账）。
