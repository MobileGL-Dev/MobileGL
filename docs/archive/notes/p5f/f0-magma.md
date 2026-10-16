# f0 普查 · §2.2 Magma（DirectVulkan）跨角色读写

> 基线：`feat/disaggregated @ 8b68b92c`（代码头 25fba0d5 之后的提交只动文档）。行号均以该 HEAD 为准。
> 范围：`MobileGL/MG_Backend/DirectVulkan/**` 一侧所有直接读前端对象 / 读 client 镜像 / 写 client 存储的站点。
> 只读普查：本文件是唯一产物，未改任何代码、未构建、未跑测试。

## 0. 总判断

1. **Magma 在 transport 下的 draw/dispatch 路径今天根本跑不通**，这不是推断而是守卫的直接后果：顶点/索引/UBO/SSBO/indirect 缓冲的同步都汇到 `VkBufferManager::AcquireResidentSlice` / `AcquireStreamedSlice`，两者第一动作就是 `bufferObject->SyncPersistentMappedRange()`（VkBufferManager.cpp:657、:708），而该方法在 transport + apply 线程下无条件 `Fatal{RoleViolation, "buffer-legacy-arm"}`（MG_State/GLState/BufferState/BufferObject.cpp:415-440，守卫条件 `PushIsArmed() && OnServerRole()`，PersistentMapTracker.cpp:526-535）。CI 侧与之一致：`integration-magma-split` 只有两条 `DirectVulkan.Split.NamedBlit.*`，且**预期红**（在 Clear 的 `GetFramebufferBindingSlot` 读处就 abort，MobileGL/MG_IntegrationTest/CMakeLists.txt:2411-2437，.github/workflows/test.yml:1353-1381）。所以"今天为何能工作"对本目录绝大多数站点的诚实回答是：**monolith / pull 臂下同线程直接成立；inproc split 下只有不设缓冲的 verb（Clear/Blit/GenerateMipmap 等）能走到，凡是 draw 带缓冲即具名 Fatal**。lockstep（Magma 永不发布 `kCapRunAheadApply`，CONTRACT-P5E.md:68、§6.1）保证每条记录设障，使未加守卫的 BARRIER_PULLED 读在非 strict 下作为 admitted pull 计数（`rsp`）而非 Fatal——但 guard 类站点不吃这一套，任何 strictness 下都 abort。
2. **T5 属实且已被 P5c tx 处理**：transport 下生成 mip 改写 server 的 staged shadow，不再写 client 的 level 存储；但留了两处具名残余（见 §2）。
3. **C1 四处 caps 读属实已改走 server backend**（见 §3）。
4. 对象类 BARRIER_PULLED 字段的 Magma 读点全部在 draw/dispatch/blit/readback/mipmap/XFB 路径上，逐字段清单见 §4。其中 8 行的退役标签是 "P5e (Espryt unbarriered), P7 (Magma)"，第 9 个 `GetTextureObject` 标的是裸 "P7"（Espryt 半边已在 P5c tx 退役），合计 9 个 Magma 相关行——与任务书的"9 个"一致。
5. 除字段读外，Magma 还有三类结构性跨角色耦合：前端地址键控的 twin registry / memo（§5）、server 在 apply 线程上创建与销毁前端类型对象（§6）、以及对前端 BufferObject 的直接方法调用（§7）。

## 1. 术语与机制前提（本次普查用到的）

- `MGB_CTX` 在 push 构建下 = `&MG_Pipe::gPipeInputs`（MG_Pipe/PipeInputsSwitch.h:17-21）；在 pull 构建下 = `MG_State::pGLContext`。Magma 全树的对象访问器读都经此宏。
- lockstep：Magma 臂的 `CallMask` 不含 `kCapRunAheadApply`，每条记录都设障，apply 时 client 停在 `WaitForApplied`。
- 双层守卫（均在 `MOBILEGL_BUILD_DISAGGREGATED` 下）：`Fatal{RoleViolation, "MGPipeSlots"}`（MG_Impl/Pipe/SlotAllocator.cpp:32-70，挂在 Acquire/FindByLifetimeId/Free，:330/:317/:339）；`Fatal{RoleViolation, "buffer-legacy-arm"}`（BufferObject.cpp:47-66，挂在 MappedData :992 / ShadowAllocationBytes :1002 / GetChangeSerial :1025 / HasDefinedContent :1034 / IsMapped :1049，以及 SyncPersistentMappedRange :434-437）；`Fatal{RoleViolation, "texture-legacy-arm"}`（MipmapStorage.cpp:30-60，被 `MGPipeTextureLegacyArmScope` 豁免）。
- Magma 的具名豁免 scope：`MagmaP7AllocatorDebtScope`（MG_Impl/Pipe/SlotAllocator.h:223-248，P5e ruling 12 改名后专指 Magma 的四笔 apply 线程分配器债，且按后端种类 keyed——Espryt 借用即 Fatal，SlotAllocator.cpp:47-55）。

## 2. T5（P5c 审计行，ROADMAP.md:57）核验：属实，已迁移，但有残余

**原状**（ROADMAP 记的行号已漂移）：`EnsureGenerateMipmapStorageAllocated` 现在位于 `Renderer/VulkanRenderer.cpp:1590-1637`（非 :1562-1590），其中 `texture.AllocateStorage(...)` :1632、`texture.MarkStorageDirty(...)` :1633 直接写前端 `TextureObjectMipmap` 的 level 存储。调用点在 `GenerateMipmap`（def :11378，非 :11317）。

**现状（P5c tx 已落地）**：`GenerateMipmap` 在 transport 下改走 `EnsureGenerateMipmapShadowAllocated`（:1655-1682），把生成的链定义在 server 的 `StagedTextureStore` 上、以 twin 地址为键（`KeyForTwinAddress`，:1670）；monolith 臂保持原样（:11464-11474 的三元）。**跨进程：成立。**

**残余两处，均已被代码注释自认**：
- 形状读：`EnsureGenerateMipmapShadowAllocated(*resource, baseMipLevel, texture->GetUploadTargets())`（:11470）——upload-target 列表仍读前端对象。注释 :1653-1654 明说："The upload-target list still comes from the frontend object - a shape READ, the P7 registry's residual"。
- `GenerateMipmap` 头部仍经 `MGB_CTX->GetTextureUnitObject(...)`（:11401，BARRIER_PULLED）取得前端纹理，并把它传给 `m_textureManager->NeedsMipChainGrowth(*texture)`（:11420）与 `SyncTextureAndGetDescriptor(*texture)`（:11442、:11477）——后者进入 §5.1 的前端地址键控同步，读 client mip shadow（见 §5.2 的 texture-legacy-arm）。

**P5f 处置建议**：T5 本体无需再做；残余随 §4 的 `GetTextureUnitObject` / §5.1 的纹理 twin 迁移一起消失（upload-target 列表可随纹理描述符/记录供给）。

## 3. C1（ROADMAP.md:67）核验：四处全部属实，已改走 server backend

四处均在 `MG_Config::Transport != Monolith` 时读 `MG_Remote::Server::ServerLoopInstance().Backend()->GetDynamicParameters()`（server 自己的 backend），monolith 臂保持 `pActiveBackendObject`：

| # | 站点（当前 HEAD） | 函数 |
|---|---|---|
| 1 | `DirectVulkan.cpp:714-733`（split 臂 :723-728） | `ShaderStorageBlockBinding`，`MaxShaderStorageBufferBindings` 越界检查 |
| 2 | `Renderer/VulkanRenderer.cpp:676-712`（split 臂 :682-693） | `ApplyLineWidthState`，aliased line-width clamp |
| 3 | `Renderer/VertexInputStateFactory.cpp:252-264` | `narrowFloat64Arrays` 判定（`SupportsFloat64VertexAttributes`） |
| 4 | `Renderer/VertexInputStateFactory.cpp:515-527` | `ToVkVertexFormat` 的 Long 臂同一 flag 判定 |

（ROADMAP 记的 :713-715 / :667-676 / :249-250 / :502-503 均略有漂移，指向相同代码。）`DirectVulkan.cpp:20` 起的 `#if MOBILEGL_BUILD_DISAGGREGATED` include 即为此。**跨进程：成立**（server 读自己的 backend 对象）。Espryt 回落臂（`DirectGLES/Utils.cpp:50`）不在本普查范围。

## 4. 九个 "P7 (Magma)" BARRIER_PULLED 字段的 Magma 读点

字段归属以 `MG_Pipe/FieldOwnership.def:53-107`（非 sticky 行）与 :178-192（sticky forward 行）为准。"draw 路径"= 经 `SetupDraw`(:6801) / `TrySetupDrawFastPath`(:6375) / `GetOrCreatePipeline`(:5250) 到达；"dispatch 路径"= 经 `DispatchCompute`(:7419) / `DispatchComputeIndirect`(:7472) 到达。

### 4.1 `GetBoundVertexArray` — 6 站，全部在 draw 路径

- `DirectVulkan.cpp:805`（`BuildClosedLineLoopIndices`，LINE_LOOP 重写）：读出 VAO 的索引缓冲绑定后 **`indexBufferShared->SyncPersistentMappedRange()` + `MappedData()`**（:814-815）——后者在 transport 下是 `Fatal{buffer-legacy-arm}`。即 Magma 的 LINE_LOOP indexed draw 在 split 下今天是具名 Fatal，不是静默错。
- `DirectVulkan.cpp:929`（`MultiDrawElementsImpl` :913）：只读 EBO 绑定存在性以决定逐条回放。
- `VulkanRenderer.cpp:6446`（`TrySetupDrawFastPath`）、`:6852`（`SetupDraw`）：每次 draw 的对象读；fast path 还有 handle 臂（`ResolveVaoHandle`，:6452-6458，`MagmaPipeTrackHArmIsHandles`），但 handle 由 `vao.GetLifetimeId()` 推出——仍以前端对象为源。
- `VulkanRenderer.cpp:12550`（`MultiDrawElementsIndirectCount` :12528）、`:12651`（`MultiDrawElementsIndirect` :12629）。

**今天为何能工作**：同地址空间 + 每条记录设障；值是 client 残余填充的 `SharedPtr<VertexArrayObject>`（PipeFill.cpp:1902-1905 一带，见 FieldOwnership.def:68-71 的引文）。**跨进程：不成立**（前端堆 SharedPtr）。

### 4.2 `GetBufferBindingPoint` / `GetBufferBindingPointCount` — 3 + 2 站，draw 与 dispatch 都到

- `UniformManager.cpp:1205` + `:1200`（`ResolveStorageBufferDescriptor` :1157，SSBO/原子计数器绑定解析，draw/dispatch 共享）。
- `UniformManager.cpp:2035` + `:2030`（`ResolveUniformBufferPayload` :1979，UBO 绑定解析，draw/dispatch 共享）。
- `VulkanRenderer.cpp:11764`（`BeginXfbCaptureForDraw` :11714，`BufferTarget::TransformFeedback`，XFB 捕获，draw 路径）。

三处取到绑定点后都继续 `bindingPoint.GetBoundObject()` 拿前端 `BufferObject`，再走 §7 的 buffer legacy arm。**跨进程：不成立**（`BindingSlotRange1D` 前端基址指针，PipeFill.cpp:2591-2592 的 "four raw bases" 同类）。`GetBufferBindingPointCount` 是 sticky forward（FieldOwnership.def:147-148），退役标签 "P7/P13"。

### 4.3 `GetFramebufferBindingSlot` — 13 站

- draw 路径：`VulkanRenderer.cpp:3158`（`GetShaderTransformFlags`，判 default FBO 决定 Y-flip 编译位）、`:5510`、`:5730`（`GetOrCreatePipeline`，深度/模板附件门控与 render-pass 相容性）、`:6360`（`GetBaseTransformFlagsRaw` 的 MOBILEGL_ASSERT——**注意：INFO 级别下断言被编译掉，这站不总是执行**）、`:6465`（fast path）、`:6845`（`SetupDraw`）、`UniformManager.cpp:560`（`ResolveSamplerDescriptor` 的 feedback-loop 诊断）。
- 其余 verb：`VulkanRenderer.cpp:7648`（`Clear`——**这就是 integration-magma-split 预期红的那一对 `GetFramebufferBindingSlot@Clear`，CMakeLists.txt:2411**）、`:8016`（`QueueClearBufferPayload`）、`:9024`/`:9025`（`BlitFramebuffer` 的 bound 臂；具名臂见 §5.4）、`:9660`（`CopyTexSubImage2D`，Read 靶）、`:10371`（`ReadPixels`，Read 靶）。

读到的都是前端 `BindingSlot<FramebufferObject>` 指针再 `GetBoundObject()`。**跨进程：不成立**。

### 4.4 `GetImageTextureBinding` — 4 站，draw 与 dispatch 都到

- `UniformManager.cpp:1021`（`ResolveStorageTexelBufferDescriptor` :991）、`:1312`（`ResolveStorageImageDescriptor` :1276）、`:1867`（`CollectStorageImageTextures` :1818）、`:1955`（`CollectSamplerImageFeedback` :1900，sampler/image 子资源重叠危害探测）。

返回前端 `ImageTextureBinding` 基址后的元素。**跨进程：不成立**。

### 4.5 `GetTextureUnitObject` — 8 站（与 FieldOwnership.def:90 的 "8 Magma sites" 吻合）

- `UniformManager.cpp:511`（`ResolveSamplerDescriptor`，随后 `textureUnit.GetSamplerObject()` :512 取前端 SamplerObject）、`:787`（`ProgramSamplesOnlySingleLevelTextures`）、`:826`（`ResolveSamplerTexture`）、`:852`（`ResolveSamplerTextureRaw`）、`:1679`（`ResolveSampledBinding`）。
- `VulkanRenderer.cpp:9652`（`CopyTexSubImage2D`）、`:11157`（`GetTexImage`——class C，split 下 client 侧已具名拒绝，EmitTables.cpp:1673-1675）、`:11401`（`GenerateMipmap`）。

指针指进 client 的纹理单元数组（PipeInputs.h:669 的解引用 + PipeFill.cpp:274 的裸指针填充）。**跨进程：不成立**。

### 4.6 `GetProgramForDraw` / `GetProgramForDispatch` — 3 + 2 站

- draw：`VulkanRenderer.cpp:6384`（`TrySetupDrawFastPath`）、`:6815`、`:6853`（`SetupDraw`）。
- dispatch：`VulkanRenderer.cpp:7424`（`DispatchCompute`）、`:7476`（`DispatchComputeIndirect`）。（FieldOwnership.def:97 注里写的 :7327 已漂移。）

拿到前端 `SharedPtr<ProgramObject>` 后整个 program 内容都从前端对象读：`ProgramFactory::ComputeHash`/`GetOrCreateProgram` 读 `program.GetGeneratedSpirv()`（ProgramFactory.cpp:2469-2473、:3472）；`ResolveUniformBufferPayload` 读 `program.GetUBOData()/GetUBOSize()`（UniformManager.cpp:1992-1993，具名 UBO host payload，ROADMAP.md:142 D-B8 的已知形状）、`GetUniformBlockBinding`/`GetUniformBlockName`/`GetUBOSizeAt`（:2028/:2033/:2063）。**跨进程：不成立**。Espryt 在 P5e pa 的证明（"记录早就带着逐 link 的 ProgramArchive"，ROADMAP.md:31）对 Magma 尚未接线——Magma 的 program 记录载体是否存在，本次未核实到（**未知**，fm 包第一天要回答）。

### 4.7 `GetTransformFeedbackProgram` — 2 站，draw 路径

- `VulkanRenderer.cpp:11725`（`BeginXfbCaptureForDraw`）、`:11824`（`EndXfbCaptureForDraw`）。前端 `SharedPtr<ProgramObject>`。**跨进程：不成立**。配套读还有 `IsTransformFeedbackActive/Paused`（:11716/:11722，值类，记录供给，成立）、`GetBoundTransformFeedbackLifetimeId`（:11667，值类）、`GetTransformFeedbackGeneration`（:11801，值类）、`HasOpenTransformFeedbackSpan`（:11684，sticky forward，"P7/P9"——前端 XFB span 表查询，**不成立**）。

### 4.8 `GetTextureObject` — 1 站

- `VkTextureManager.cpp:810`（`SyncTextureAndGetDescriptor` 内）：按 GL name 查前端纹理对象表以注册 alive 追踪；查不到就 `texture.weak_from_this()`（:821）——把前端对象的 control block 存进 server 侧 map。sticky forward，退役标签裸 "P7"。**跨进程：不成立**。

### 4.9 相邻但非 "P7 (Magma)" 的字段读（列出以免漏网）

- `GetBufferBindingSlot`（"P8/P9/P13"，15 靶合一数组）13 站：`DirectVulkan.cpp:283`（`ResolveIndirectCommandBytes`，DrawIndirect）、`:407`（`MultiDrawArraysIndirect`）、`:470`（`MultiDrawArraysIndirectCount`，Parameter）、`:538`（`DrawElementsIndirect`）、`:591`（`DrawArraysIndirect`）；`VulkanRenderer.cpp:7520`（`DispatchComputeIndirect`，DispatchIndirect）、`:11126`（`GetTexImage`，PixelPack）、`:12560`/`:12566`、`:12661`、`:12731`（multi-draw indirect 各站）；`UniformManager.cpp:910`（`ResolveTexelBufferDescriptor`）、`:1053`（`ResolveStorageTexelBufferDescriptor`——这两站是 `textureBuffer->GetBufferBindingSlot()`，前端 TextureBuffer 对象自身的方法）。indirect 路径取到前端 buffer 后普遍跟 `SyncPersistentMappedRange()`（如 DirectVulkan.cpp:285、VR:7525）——transport 下具名 Fatal。
- `GetTransformFeedbackPausedPrimitiveCounter`（FATAL 行）：`DirectVulkan.cpp:1256`（`GetQueryResult64` :1228）、`:1303`（`BeginXfbPrimitivesQuery` :1294）。两个入口都是 class C（EmitTables.cpp:1690-1693），split 下 client 侧先拒绝，**今天不可达**；行为一致，归 §2.7，不归 P5f。
- `GetPixelStoreParameters(false)`（pack 半，APPLIER_DERIVED）：`VulkanRenderer.cpp:11127`。**跨进程成立**。注意 FieldOwnership.def:208 注里写的 ":10980" 已漂移（该行现在是 MOBILEGL_ASSERT），真身在 :11127——文档漂移，供 fm 顺手修注。

## 5. 前端地址 / 前端身份键控的 twin registry 与 memo

### 5.1 `VkTextureManager`（纹理 twin）

- `TextureIdentity{ ITextureObject* texture, Uint64 lifetimeId }`（VkTextureManager.h:126-145），`m_textureResources` / `m_aliveObjects` 均以此为主键；hash 混入前端对象地址（:144）。`m_syncedTextureMemo[8]` 直接以 `const ITextureObject*` 比较（:708-715，"A dead-then-reused texture address cannot false-hit: the new object carries a new lifetime id"）。
- `SyncTexture`（VkTextureManager.cpp:1745-1796+）整体裹在 `MGPipeTextureLegacyArmScope`（:1755）里——注释 :1748-1754 自认："Magma's texture sync still reads - and clears - the CLIENT's mip shadow: the dirty scan below, and UploadDirtyMipLevels' texel reads / region reads / MarkStorageDirty(false) clears"。**这是 §2.2 意义上最大的一块：读 client 纹素字节 + 写（清脏）client 存储**，guard 是 texture-legacy-arm，scope 内豁免。
- `ToStorageMipLevel/ToStorageArrayLayer(ITextureObject*)`（VkTextureManager.h:113-126）全树散布，读前端 view 父链。

**跨进程：不成立**。P5c 只迁了 Espryt 的 sync（走 StagedTextureStore 句柄键），Magma 的 server 侧 sync 是 P7 的活。

### 5.2 `VkBufferManager`（缓冲 twin）

- twin 指针直接**写进前端对象**：`bufferObject->SetBackendResource(resource)`（VkBufferManager.cpp:305、:601），读出对称（:291、:299）。`GetBackendResource/SetBackendResource` 是前端 BufferObject 上的 mutable 槽——server 写 client 内存。
- `AcquirePersistentMap`（:598-646）seed 时读 `bufferObject.MappedData()`（:631）。
- `AcquireResidentSlice`（:649-699）/ `AcquireStreamedSlice`（:701-）：每 draw 调用，含 `SyncPersistentMappedRange()`（:657/:708）、`GetSize()`、`GetChangeSerial()`（:726）、`MappedData()` 整缓冲上传（:682/:768）。transport 下第一行即 `Fatal{buffer-legacy-arm}`（见 §0.1）。

**跨进程：不成立**；且今天是 guarded Fatal，不是静默读。

### 5.3 draw 路径上的地址键控小缓存

- `ConvertedVertexStreamKey.buffer = sourceBufferShared.get()`（VulkanRenderer.cpp:4106-4116）+ `GetChangeSerial()`（:4107）+ `MappedData()` 转换读（:4127）——`UploadAndBindVertexStreams` 内，前端 buffer 地址做 memo 键 + 读 client 字节。
- `UploadAndBindIndexBuffer` 的 restart-index 替换臂：`indexBufferShared->SyncGpuWrites()` + `MappedData()` + `GetSize()`（:4421-4423）——`SyncGpuWrites` 在 transport 下有反向通道臂（BufferObject.cpp:571-590 起），但那是 client 线程语义；apply 线程调它的行为本次未逐行走通（**未知**，大概率在 `MappedData()` 的 guard 处先 Fatal）。
- `SetupDrawSnapshot`（VR:6388-6470 一带）以 `program.GetLifetimeId()`、前端 VAO/FBO 的地址与 lifetime id 做键（:6446-6468）。
- `UniformManager.h` 的 memo 族（`SamplerResolveMemo` :418 起、`FastRebindMemo` :363、`m_globalUboMemo` :392、`m_descriptorReuseMemo` :341）以 binding + program lifetime id / 版本号做键——键本身是值，但值全部来自前端对象读出。
- `ProgramFactory::ProgramLookupCache.program` 存前端 `ProgramObject*`（ProgramFactory.h:582-587）。
- `g_programResourceCaches`（DirectVulkan.cpp:103-108）以 **GL name** 为键、lifetime id 校验（:166-189）——`GetShaderStorageBlockIndex/Binding`（:313/:332，client 侧查询路径）与 `ShaderStorageBlockBinding`（:714，P5b 的 `set_storage_block_binding` 记录会经 sink 在 apply 线程到达）都会进 `TryGetDirectVulkanProgram`（:274-280），后者调 sticky forward `ValidateProgramName` + `GetProgramObject`（"P9" 行）——**server 按名字探前端名字表，跨进程不成立**；GL-name 键控的 cache 本身是 server 内存，成立。

### 5.4 具名 blit 端点解析（P5c G6 / P5e ruling 12）

- `VulkanRenderer.cpp:8974-9022`：split 臂从 applier 的 verb 句柄工作区取 `ReadFbo/DrawFbo` 句柄（:8989-8991），随后 `resolveEndpoint` lambda 里**直接拼写 `MG_State::pGLContext`**（:8997、:9004、:9005）与 `MG_Pipe::MGPipeSlots()`（:8999、:9003），全程裹 `MagmaP7AllocatorDebtScope`（:8995）。这是 DirectVulkan 全树仅有的 `pGLContext` 与 `MGPipeSlots()` 直接拼写。**跨进程：不成立**（前端 FBO 池 + client 分配器）。P7 的退役方式注释已写明（:8983-8984："P7 retires it by carrying the object identity in the record"）。
- 附带发现：**ARCHITECTURE.md:460 的 C 门原文是 `grep -c 'pGLContext' MG_Backend/ == 0`，当前 HEAD 实测非零**——`VulkanRenderer.cpp` 3 处代码命中（:8997/:9004/:9005）+ `MGPipe/PipeInputs.h:822` 一处注释。该门是否仍在 CI 以 ==0 执行、还是已改为白名单，本次未核实（**未知**）；若仍按字面执行则门已破，值得在 f1 之前确认。

### 5.5 `MagmaPipeIdentityTables`（MagmaPipeArms.h:370-529）

Magma 自己的 {slot, gen} mint，按 lifetime id 把前端 VAO/Buffer 映到 server 侧 slot 表（`HandleOf` :515-518）；`VertexInputStateFactory::VaoBackendMemos` 以该 handle 为键（VertexInputStateFactory.h:149-171）。表本身是 server 私有内存，**成立**；但 lifetime id 的获取仍靠读前端对象（`vao.GetLifetimeId()`），身份源是 §4.1 的字段读—— mint 干净、水源脏。P5f 无需动表，动的是喂它的字段。

## 6. server 在 apply 线程上创建 / 销毁前端类型对象

- 隐藏 blit 资源：`VulkanRenderer.cpp:4475-4491`（`ShaderObject`/`ProgramObject`）、`:4527-4540`（两个 `SamplerObject`）；隐藏 depth-mipmap 资源：`:4567-4617`。销毁走 `ShutdownBlitResources`/`ShutdownDepthMipmapResources`（:4544-4562、:4634-4643），已裹 `MagmaP7AllocatorDebtScope`（:4559、:4640）——注释自认析构会触发 client death helper 的 lifetime-id 探测。注释 :4552-4553 明说："P7 gives these resources storage that is not a frontend object"。
- UniformManager 占位纹理：`MakePlaceholderTextureObject`（UniformManager.cpp:167-189，`GetFallbackTexture` :1410/:1435、`GetFallbackMultisampleTexture` :1451/:1497-1499 调用），以及 `m_unboundStorageImageTextures`（UniformManager.h:317）。**每构造一个纹理对象都会 `MGPipeMintTextureHandle` + `MGPipeEmitTextureResourceCreate`**（TextureObject.cpp:143-170，push 构建下无条件 mint）——apply 线程上的 `MGPipeSlots().Acquire` 无 scope 即 `Fatal{RoleViolation, "MGPipeSlots"}`（SlotAllocator.cpp:330、:47-59）。也就是说 **Magma 的占位纹理在 split 下首次需要时即具名 Fatal**（与 ROADMAP.md:127 "占位纹理原生化 → P7" 互证）。ProgramObject/ShaderObject/SamplerObject 的构造不 mint（grep 无命中），故隐藏 program/sampler 的创建本身今天不触发守卫，只有析构的探测被 scope 兜住。

**跨进程：不成立**（server 进程里没有 MG_Impl/MG_State 的 client 语义，构造出的对象也不该存在）。P5f 处置：随 P7 的"内部 shader 烘焙 / 占位纹理原生化"换成 server 原生资源；f1 双块演习时这些点会以 Fatal 形式自己报出来。

## 7. 对前端 BufferObject 的直接方法调用（写 client 存储的边）

- `bufferObject->MarkGpuWritten()` 直写臂已按 P5c ev 改掉：三处都是 `#if MOBILEGL_PIPE_PUSH → MGPipeAnnounceBufferGpuWritten(...) / #else 直写` 双臂（UniformManager.cpp:1077-1085、:1241-1247；VulkanRenderer.cpp:11774-11781）。announce 内部经 `MagmaP7AllocatorDebtScope` 做 client 分配器的 lifetime-id 反查（ResourceTracker.h:710-743，:728 是"第四笔债"），找到句柄则走 `OnGpuWritten` → SEG_EVENT；找不到句柄则**回退直写 client 对象**（:738）。**跨进程：announce 主臂成立；回退臂不成立；scope 内的分配器探测不成立**。
- `bufferObject->EnsureGpuResidentStorage()`（UniformManager.cpp:1077、:1239；VulkanRenderer.cpp:11774）：transport 下早退 `return false`（BufferObject.cpp:877-880，R-6 的第二道门），但早退之前已读 `m_resource.IsGpuResident()` / `m_isMapped`（:856、:866）——**无守卫的 client 内存读**，今天靠同地址空间成立，跨进程不成立。
- `indexBufferShared->SyncGpuWrites()`（VR:4421）：见 §5.3。

## 8. 写 client 存储 / 读 client 镜像的其他站点

- `RecordError`（写前端错误队列）：`VulkanRenderer.cpp:1367`、`:1371`（`RecordClearBufferError`/`RecordTextureCopyError` 帮手）、`:1427`（`RecordUnsupportedFramebufferError`）、`DirectVulkan.cpp:735`（`ShaderStorageBlockBinding`）。transport 下 `PipeInputs::RecordError` 改走 `ServerSessionInstance().PostGlError` → SEG_EVENT `kEventGlError`（PipeFill.cpp:2164-2178），client 在 drain 点入账。**跨进程：成立**（排序是 P9 的已知债）。
- `InvalidateCompileEnv`（写前端编译环境）：`BackendObject_DirectVulkan.cpp:390`（`InitCapabilities`）、`:788`（`ApplyVulkanCapabilitiesForTesting`）。transport 下 `PipeInputs::InvalidateCompileEnv` 已删除为空操作（PipeFill.cpp:2143-2156，失效改由 caps 重发布完成）。**跨进程：成立**。
- `SwapchainObject` 默认 FBO 附件写：transport 下走 `OnSurfaceChanged` → SEG_EVENT（SwapchainObject.cpp:323-339），monolith 臂仍直写 `pDefaultFramebufferInfo` 的 `AllocateStorage/SetInternalFormat`（:342-358）。**跨进程：成立**（P5c ev / R3 已处理，ROADMAP.md:64）。
- `GetTextureImage`（VR:11162-11209+）：入口签名直接吃前端 `SharedPtr<ITextureObject>`（DirectVulkan.h:101 声明、DirectVulkan.cpp:768 入口），内部 `SyncTextureAndGetDescriptor(*textureObject)`（:11175）+ 无 GPU 存储时回退 `MG_Impl::GLImpl::CopyTextureImageToClientOrPBO_State`（:11205）——直接调 MG_Impl 函数读 client shadow 写 client 目标缓冲。但两个 GL 入口都是 class C（EmitTables.cpp:1673-1675），split 下 client 先拒绝，**今天不可达**；若 P9 要放开，这是整条臂的跨进程形状问题，先记着。

## 9. 值类读取（非债，列出备查）

以下 accessor 在 DirectVulkan 下被读、但属 RECORD_SUPPLIED / APPLIER_DERIVED，跨进程成立，不需 P5f 动作：`IsCapabilityEnabled(Indexed)`、`GetRenderStateParameters(Version)`、`GetPipelineStateVersion`、`GetStencilState`、`GetColorMaskIndexed`、`GetDepthMask`、`GetBlendColor/Func/Equation`、`GetScissorBox`、`GetViewportIndexed`、`GetDepthRangeIndexed`、`GetClearColor/Depth/Stencil`、`GetActiveTextureUnit`、`GetCurrentVertexAttribute`、`GetPatch*`、`GetPolygon*`、`GetLineWidth`、`GetLogicOp`、`GetCullFaceMode`、`GetDepthFunc`、`GetMinSampleShadingValue`、`GetClampReadColor`、`GetProvokingVertexMode`（VR:13229）、`IsTransformFeedbackActive/Paused`、`GetTransformFeedbackGeneration`、`GetBoundTransformFeedbackLifetimeId`，以及三 shutter `GetTextureBindGeneration`/`GetSamplingResolutionGeneration`（VR:6477/:6659/:6907/:6913/:7031/:7050/:7361/:7387 等，P5c rv 起 APPLIER_DERIVED，server 自答）。`g_dynamicStateShadow`（VR:290-409）是纯 server 侧 memo，成立。

## 10. 未知 / 留给 fm 的问题

1. Magma 的 program 内容（SPIR-V、UBO 初值）是否已有记录载体（类似 Espryt 的 ProgramArchive）——本普查未在 wire schema 侧核实；若没有，4.6 是 fm 最大的一块。
2. `SyncGpuWrites` 从 apply 线程在 transport 下的实际行为（BufferObject.cpp:590 起的臂是否假设 GL 线程）。
3. ARCHITECTURE.md:460 的 C 门是否仍以 ==0 执行（当前实测非零，见 §5.4）。
4. indirect draw 无绑定缓冲（client 指针）时，记录在 wire 上如何携带命令字节、server 端 `ResolveIndirectCommandBytes` 的 client 指针解引用在严格双块下的归类（指针随记录过来，值无意义——大概率走 client-memory 升级的具名拒绝，待 f1 的红来确认）。
5. `GetTexImage`/`GetTextureImage` 若 P9 放开，VR:11162 起的整条前端对象签名链需要重新设计（§8）。
