# MGPipe 设计与架构

> 本文只写**已决定**的设计，每条决定附一行理由。落地状态：P0–P5c 已落地（代码头 `b88e8487`）；标 **P6+** 的是后续形状，阶段号见 `ROADMAP.md`；实测数字见 `MEASUREMENTS.md`；P5 / P5b / P5c 的 wire 契约原文在 `MobileGL/MG_Remote/CONTRACT-P5.md`、`CONTRACT-P5B.md`、`CONTRACT-P5C.md`。

## 1. 边界

### 1.1 一句话

`MG_Backend` 已经是一台贴着目标 API 的状态机（Espryt：逐字节渲染状态镜像、twin registry、三条 persistent ring；Magma：`SetupDrawSnapshot`、pipeline memo、五个 `Vk*Manager`）。它缺的不是状态，而是一份"我被告知了什么"的显式声明。MGPipe 就是那份声明：前端在每条 verb 之前把变化**推**过去，后端不再拉 `MG_State::pGLContext`；server 进程因此只装 `MG_Backend` + MGPipe 对象表，不链接 `MG_State`、`MG_Impl`、glslang。

接口是从两个后端自己维护的关键结构反推出来的（`SetupDrawSnapshot` 字段并集 → `set_*`；`DrawTextureSyncKeys` → sampler view 三件；`ResolvedDrawBuffers` / `ResolvedVertexBindings` → vertex elements 三件；`g_syncedRenderStateParameters` → render-state CSO；`UnpackStagingBlock` → `MGPSubData` 的 region 形状；`BufferBackendOps` 七个 hook → `resource_*` 全族）。gallium 是目的地（词汇可读、可迁移），不是推导前提。

### 1.2 两张函数指针表

`MGPipeScreen`（share-group 作用域：caps、resource、persistent map、fence）与 `MGPipeContext`（其余全部），由 `PipeCalls.def` 经 G1 生成（`MG_Pipe/generated/PipeTables.inc`）。

- 函数指针 struct 而非虚基类：边界今天就是函数指针 struct（`gBackendFunctionsTable`），**null 项已经表示"未实现，前端回退"**，`MG_Test` 已用替换整张表的方式 mock 后端。
- 两张表从第一天分开：事后拆分意味着给记录重新编号。v1 只有一个 screen、一个 context、一条 flow。
- EGL 生命周期 8 项与 caps 面留在 `pActiveBackendObject` 的虚函数上（罕见路径）。

### 1.3 三种形态，一份后端

| 形态 | 表里装的是什么 | 用途 |
|---|---|---|
| `monolith`（默认） | backend 自己的函数；回调直调 `MG_State`；`MGHostSpan.Ptr` 指向 client shadow | 出货 |
| `inproc` | 发射器 → 同进程第二个线程上的 applier | CI 形态；同时就是 monolith 的**渲染线程**（把 `PrepareForDraw` 与驱动调用搬离 GL 线程） |
| `spawn`（P6） | 发射器 → SPSC shm ring → 另一个进程的 applier → 同一批 backend 函数 | 两进程出货形态 |

唯一 hook 点是 `MG_Backend::Init()` 里一个 `#if MOBILEGL_BUILD_DISAGGREGATED` 分支：`MG_Config::Transport != Monolith` 时装 `MG_Remote::BackendObject_Remote`，否则走今天的 `switch`。`MG_Impl` 的边界调用点零 `#ifdef`。

## 2. 对象模型

### 2.1 句柄 = `{slot, gen}`（`MG_Pipe/MGPipeHandles.h`）

- 8 字节 POD，按值走寄存器对；**client 铸造，server 永不返回句柄** → 整份目录零创建 round trip（对 gallium 的偏离 D1）。
- slot 稠密、按 kind 分配（free list + 高水位），server 对象表是数组不是哈希表；`IndexGenerator` 的 LIFO 名字复用正是句柄要关掉的问题。
- `gen` 只在 slot 复用时 ++，不在 respecify 时 ++；同一 slot 复用 2³² 次才回绕，debug 分配器断言回绕。
- kind：`Buffer, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context`。
- 保留句柄：`{0,0}` = null；`{0,1}` of `Framebuffer` = 默认帧缓冲（退役 Espryt 的 `defaultFBO` 身份比较）；`ShaderCso` slot 空间的高段保留给 program pipeline 合成体（`kMGPipeShaderCsoCompositeSlotBase`）。
- GL name 只以 `GlNameForDiag` 出现在 `MGPResourceDesc` 里，永不做身份、永不进 memo 键或 content hash；`GetLifetimeId()` 留在 client 作 tracker 自己的身份，client 维护 `lifetimeId → slot`。

### 2.2 两种世代，严格分开

| | 拥有者 | 回答 | 过线 |
|---|---|---|---|
| `MGPipeHandle::Gen` | client | "还是同一个 GL 对象吗？" | 是 |
| `MGGen`（`g_bufferMutationEpoch`、`m_textureImageEpoch` 等后端纪元） | server | "我自己是否重铸了驱动对象？" | **永不**；server→client 只以纹理拉取请求出现（§8.4） |

规范：任何 MGPipe 调用不得要求 client 提供或知晓 `MGGen`；client 的回绕 `Uint16` 版本计数器永远不是新鲜度的唯一证明——过线时要么加宽、要么与 `{slot, gen}` 同行。

### 2.3 CSO 与可变对象

| 类别 | 形态 | 对应后端已有缓存 |
|---|---|---|
| `VertexElementsCso` | create/bind/delete | `VertexInputStateFactory::m_cache`（Magma）；Espryt 逐 VAO twin |
| `SamplerCso` | create/delete + `bind_sampler_states` | `VkSamplerManager`、`BackendSamplerObject` |
| `SamplerViewCso` | create/delete + `set_sampler_views` | `TextureResource::perMipViews`、`SyncTextureViewToBackend` |
| `ShaderCso` | create/bind/delete + server 侧惰性特化 | `ProgramFactory::m_cache`、`BackendProgramObjectImpl` |
| `RenderStateCso` | create/bind/delete，身份 = pipeline 子集 | Espryt 值镜像；Magma `ComputePipelineStateHash` |
| Buffer / Texture / Renderbuffer | create / respecify / subdata / destroy | 各自 twin |
| Framebuffer / Xfb | per-context 身份 + `set_*` payload | `BackendFramebufferObject`、`m_xfbCounterSlotByObject` |

CSO 在 client 侧内容寻址（Mesa `cso_cache` 先例）：每类一张 `flat_hash_map<xxHash, MGPipeHandle>`，LRU 淘汰时发 `delete_*`；render-state 容量 64（`kMGPipeCsoCacheCapacity`，P13 重调）、sampler state 256。两条已落地的偏离：

- **D-G1（P3a）**：Espryt 的 vertex-elements CSO **按 VAO 身份寻址**（一 VAO 一句柄，配置变化在同一句柄上重发）——Espryt 的 twin 持有驱动 VAO 名与 scratch buffer，两个格式相同的 VAO 不能共享；1024 项的内容寻址是 P7（Magma）在同一组调用之上加的。
- **D-F2（P4a）**：Espryt 的 sampler **view** 按纹理对象身份寻址（Espryt 没有可共享的驱动侧视图对象）；sampler **state** 内容寻址并带**引用计数**——`MGPTextureParams::BuiltinSampler` 指着的项不允许被 LRU 挤掉（超容铸造计入 `OverCapacityMints`），否则一条标准记录会指向已换代的句柄。

## 3. 调用目录

### 3.1 单一真相源

`MobileGL/MG_Pipe/PipeCalls.def`：一行一个调用 `X(Name, PayloadStruct, Class, Flags)`。**线上 opcode 就是行的位置**（1-based），新调用只能**追加**、退役的调用保留槽位。`MGP_CALL_LIST_DOCUMENTED_COUNT = 76`（P5b 契约追加了 5 条 verb），由 `MG_Test/Pipe/PipeCatalogueTest.cpp` 钉住。

生成器（产物提交进树，CI `pipe-gates` 重生成并 `git diff --exit-code`）：

| | 产物 | 内容 |
|---|---|---|
| G1 | `PipeTables.inc` | 两张函数指针表 |
| G2 | `PipeThunks.inc` | monolith 直调 thunk `MGP_<Name>()`，`MG_Impl` 的边界站点逐名改到它上面 |
| G3 | `PipeWire.inc` | wire 记录 + 每种一条尺寸 `static_assert` + applier 分发前的运行期边界检查 → `Fatal{ProtocolCorruption}` |
| G4 | `PipeVerify.inc` | `MOBILEGL_PIPE_VERIFY` 的逐字段比对器（字段表 `PipeFields.def`；浮点按位比较） |
| G5 | `PipeFilled.inc` | `PipeInputs` 字段 id（63 个，`static_assert`）与逐 verb 世代 poison |
| G6 | `PipeCoverage.inc` | 后端读点清单 → MGPipe 调用的映射（`Coverage.def`），**0 UNMAPPED** 是门 |
| G7 | `PipeSpanTable.inc` | render-state pipeline 子集的成员名表；chunk 表与 setter 一致性测试在 `RenderStateSpansTest` |
| G8 | `PipeFieldOwnership.inc`（`gen_pipe_field_ownership.py`） | 每个 `PipeInputs` 字段恰好属于 `RECORD-SUPPLIED / APPLIER-DERIVED / BARRIER-PULLED / FATAL` 之一，不在任何一类 = 构建失败；目前 33 / 2 / 36 / 4。从 `MG_Backend/MGPipe/PipeInputs.h` include，**不从 `MG_Pipe/MGPipe.h`**（后者在 pull 构建的 include 闭包里，G1 不允许符号位移） |

另有 `PipeFillPoints.inc`（`FillPoints.def`，83 条 `MGP_FILL` 覆盖 69 个 verb）与 `DirtySurface.def`（`gen_pipe_dirty_surface.py`，§5.2）。

**G1 的 codegen 规则**：capability gate 若替换的是指针表达式，必须保留其**表达式形状**（init-statement / short-circuit / pointer），不能只保留真假值——`MGL_BACKEND_SLOT_CAP` 与 pointer-valued `MGL_BACKEND_SLOT_PTR_CAP` / `_PTR_LOCAL` 分开，后者在 pull 展开回原 slot expression，否则 pull `.text` 会漂移。

### 3.2 分组与计数

| Class | 条 | 内容 |
|---|---|---|
| `kScreen` | 11 | `GetCaps`(R)、`ResourceCreate/Respecify/Destroy`、`MapPersistent`(R,O)/`UnmapPersistent`(O)、`FenceCreate/Status(R)/Wait(R)/Destroy`、`FenceWaitServer` |
| `kCtxQuery` | 8 | `QueryCreate/Begin/End/Available(R)/Result(R)/Destroy`、`QueryTimestamp`(R)、`QueryCounter` |
| `kCtxCso` | 13 | create/delete × {render state, vertex elements, sampler, sampler view, shader} + bind × {render state, vertex elements, shader} |
| `kCtxState` | 17 | `SetDynamicState`(B)、`SetFramebufferState`、`SetVertexBuffers`(V)、`SetIndexBuffer`、`SetIndirectBuffers`、`SetSamplerViews`(V)、`BindSamplerStates`(V)、`SetShaderImages`(V)、`SetShaderBuffers`(V,H)、`SetStreamOutputTargets`(V)、`SetGlobalConstants`(B)、`SetVertexAttribDefaults`(V)、`SetPixelPackState`、`SetPatchState`、`SetDrawProgram`、`SetDispatchProgram`、迁移期临时的 `SetResidualValueState`(B) |
| `kCtxObject` | 9 | `SetTextureParams`、`ResourceSubData`(B,V)、`BufferSubDataResident`(B,O)、`ResourceSubDataComplete`、`ResourceFlushRange`、`ResourceReadback`(R)、`ResourceCopyRegion`、`GenerateMipmap`、`GetTextureImage`(R) |
| `kCtxVerb` | 18 | `Blit`、`Clear`、`ReadPixels`(R)、`DrawVbo`(H,V)、`LaunchGrid`、`MemoryBarrier`、`Begin/End/Pause/ResumeStreamOutput`、`Flush`、`Present`、`SetSwapInterval`(O)；P5b 追加 `BindShaderImage`、`PatchParameter`、`BindStreamOutput`、`SetStorageBlockBinding`、`CopyFramebufferToTexture` |

Flags：`kNeedsAck`、`kHasBlob`(B，payload 拥有一个 `MGPBlobRef` 成员，不弱于此)、`kVarTail`(V)、`kHostSpan`(H)、`kReplySlot`(R，答进 `MGPReplySlot`)、`kOptional`(O，后端表里可为 null：Magma 不注册 `BufferSubDataResident` 与 `SetSwapInterval`)。

- 20 个 draw 入口塌成 `DrawVbo` 一条，`MGPDrawRange[]` 是 `MultiDraw*` 族的形状；`Clear` 一条判别式合并 `glClear` + `glClearBuffer*` + `glClearNamedFramebuffer*`。
- `SetSamplerViews` / `BindSamplerStates` **没有 stage 维度**：MobileGL 的纹理单元空间是合并的（192 个单元一个数组），stage 只在目标 API 需要时由 server 从反射归档推导。
- `SetTextureParams` 按资源寻址、与 sampler view 分开（D10）：只作 attachment / image 单元 / copy 端点的纹理没有 sampler view，但 Espryt 对 attachment 也同步纹理参数。P4a 用白盒断言（`MG_IntegrationTest/Harness/PipeApplyPeek`）证明这个缺口此前确实是坏的。
- `SetIndexBuffer` 独立于 VAO 配置版本（D5）；`SetGlobalConstants` 只覆盖默认 uniform block（D6）。

**显式不移植**：`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv`（后两项已从 `GLFunctionsTable` 删除；`GL_MAX_COMPUTE_WORK_GROUP_*` 进 `MGPCaps`）、`ShaderStorageBlockBinding` 的反射折叠、`set_pixel_unpack_state`（前端已在 `glTexImage` 时解析压缩格式、强制默认 unpack）、压缩格式概念、`pipe_transfer`。

### 3.3 能力位（`MGPCapBit`）

`kCapViewportArray`、`kCapFloat64VertexAttrib`、`kCapResidentSubData`、`kCapCpuXfbPrimitiveAccounting`、`kCapTimerQuery`、`kCapOcclusionQuery`、`kCapXfbPrimitivesQuery`、`kCapNeedsHostIndexBytes`（server 做 restart 重写 / multi-draw 展平，split 下开启索引宿主镜像，§10.3）、`kCapNeedsHostUboBytes`（server 把具名 UBO 打进自己的 ring）、`kCapBackendOwnsXfbCapture`（P5b）。`CallMask` 取代"槽位是否为 null"这个隐式能力探测。

不存在 `kCapPrimitiveRestart` / `kCapMultiDraw*` 一类"归属开关"（D-B7）：**multi-draw 分档与 restart 重写永远由 server 拥有；client 在 caps 说需要时提供索引字节。** `MGPCaps` = `DynamicBackendParameters` 整块 + `CallMask` + 两个 blob（format 能力表、renderer 字符串），握手后一次快照，取代 40 个 `pActiveBackendObject->` 站点与 89 个 caps 读点。

## 4. 记录与 payload 约定（`MG_Pipe/MGPipeTypes.h`）

- 每个 payload 是平坦 POD、显式 padding、`static_assert` 平凡可复制与精确尺寸；**永不含指针**。
- `MGPBlobRef{Offset, Size, Seg}`（24 B）指向 blob 区：monolith 下 `Seg == kMGHostSpanSegNone`；split 下 Seg 命名传输段。
- `MGHostSpan`（32 B）是唯一形状随传输而变的东西：monolith 下 `Ptr` 指向 shadow 或应用内存；split 下 `Ptr == nullptr`、字节在 `SEG_STAGE`，或 `Seg == kMGHostSpanSegFromServerIndexMirror`（P8）。它只进变长尾（`DrawVbo` 的用户索引、`SetShaderBuffers` 的具名 UBO 字节），永不内联进定长 payload——VBO 路径不为它付字节。
- 变长记录（`kVarTail`）= 定长前缀 + 自描述长度的内联尾巴；`kHasBlob` 记录额外校验 `BlobRef` 落在其声明的段内。`SEG_CMD` 是对端并发写入的区域，运行期违反一律 `Fatal{ProtocolCorruption}`。
- wire 记录头 `MGPWireRecHeader{Op:u16, Flags:u16, Size:u32}`（8 B），Size 含头、8 字节倍数；**没有逐记录序号字段**——seq 就是记录序数（producer `m_emitSeq++` / consumer `m_applySeq++`）。`Flags` 是 ring 封帧，永不是 call flags（R-17）。
- 单条记录上界 = ring 容量的一半（`RingProducer::MaxRecordBytes()`），超过的记录被具名拒绝（`Fatal{RingOverrun}`）——**记录本身永不分块**（R-10；实测 `maxrec` 见 `MEASUREMENTS.md` §6.3）。**内容侧已分块**：会超出 `SEG_STAGE` 的 blob 由发射侧按 stage chunk 预算 `MGPipeStageChunkBytes()`（`clamp(segment/4, 4096, segment)`，默认 8 MiB）切成多条记录——buffer 范围走查（`PipeFill.cpp`）与纹理一级的整宽 slab（`TextureEmit.h`，服务端 `StagedTextureStore::AdoptRun` 拼回整级）；单片仍超容量或该 record 类型未接入分片时仍是 `Fatal{RingOverrun, "SEG_STAGE"}` 兜底。
- `MGPSubData` 的 buffer 半边：`Target == Buffer` 时目的字节范围搭在 `UnionBox.X/W` 上，`MGPipeSetSubDataBufferRange()` 是唯一拼写；单条记录上限 offset 2³¹−1 / size 2³²−1。

### 4.1 关键 payload

| payload | 尺寸 | 要点 |
|---|---|---|
| `MGPResourceDesc` | 88 | buffer / 全部纹理 target / renderbuffer 一个判别式 create/respecify 形状；`BindMask` 的 `ELEMENT_ARRAY` 位是索引镜像的开关；`ImageBindableHint` 预防性分配 image-bindable 存储；`ViewOf` 是纹理视图的存储属主；`BufferForTexBuffer/BufOffset/BufSize` 实时解析；`HasDefinedContent` 陈述的是上一次 respecify 当时的事实 |
| `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` | 48 / 12 / 32 | §5.3 |
| `MGPVertexElements` | 40 | blob 带解析后的 `MGPVertexAttribWire[]` **和** `MGPVertexBindingPointWire[]`（记录自洽：一条不描述自己 blob 的记录让边界门无法收口；代价按配置变化付一次；裁掉第二个视图是 P13 的重调项）；`IsLong` 与 `Type == Float64` 分开携带 |
| `MGPSamplerDesc` | 32 | `SamplerParameters` 逐字节过线**含 `borderColorForm`** |
| `MGPSamplerView` / `MGPTextureParams` | 36 / 40 | view 只带视图限制；纹理参数挂在纹理对象上。**D-E1**：`MGPTextureParams::BuiltinSampler` 是该纹理自带 `SamplerObject` 对应的 sampler CSO 句柄（从内容寻址 cache 取，**不允许为空**——空句柄是协议损坏）+ `SamplerResync` 位 |
| `MGPProgramDesc` | 192 | 逐 stage SPIR-V blob ×6 + 反射归档 blob + `StageMask`/`GlobalUboSize`/`ReservedNumSamplesOffset`，§7 |
| `MGPFramebufferState` | 304 | 8 color + depth + stencil + **client 解析后的 `ReadSurface`**；`MGPSurface::InternalFormat` 与 `TextureTarget` 内联；`ContentHash` 既是 render-pass memo 键也是发射抑制器；`Target` 字节按绑定目标发（`Draw` / `Read` / `Both` / **`Named = 3`**），applier 按 framebuffer 句柄存成每对象一张表外加两个"当前绑定"句柄——否则 `BlitNamedFramebuffer` / `ClearNamedFramebuffer*` 会打进一个从未收到附件的 FBO |
| `MGPSubData` / `MGPSubRegion` | 72 / 40 | §6 |
| `MGPDrawInfo` / `MGPDrawRange` / `MGPDrawIndirect` | 56 / 12 / 40 | `Flags` 门控 `MinIndex/MaxIndex`（只在 client-memory 数组路径算）与 `XfbCpuCapturedVertices`；`NumDraws` 个 `MGPDrawRange` 在变长尾；用户索引的 `MGHostSpan` 只在 `kDrawHasUserIndices` 时进变长尾；indirect 的 `DrawCount` 由 client 解析 |
| `MGPShaderBuffers` / `MGPBufferRange` | 32 / 24 | `kCapNeedsHostUboBytes` 下 Uniform 类带第二个变长尾 `MGHostSpan[HostSpanCount]` |
| `MGPPixelPackState` / `MGPPatchState` / `MGPClear` / `MGPGlobalConstants` / `MGPSubDataComplete` | 28 / 40 / 48 / 40 / 24 | 只有 PACK 方向（D5）；patch 同时是 shader variant 输入；clear 五种判别式；`(ShaderCso, Version)` 键控；纹理拉取的正向终止符，可携带零个 region |
| `ResidualValueBlock` | 8 | 迁移期 Track V 载体，§9.4 |

每条 `kVarTail` 的 `set_*` 都带 `ContentHash`——hash 未变就不发（§5.4）。

## 5. 前端 state tracker（`MG_Impl/Pipe/Tracker`）

### 5.1 推送发生在 verb 之前的 validate 时刻，不在 GL setter 里

Blaze3D 每个 batch 用 `glEnable/glDisable(GL_BLEND)` 包住，per-setter 推送会把每次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，严格慢于今天。正确形态是 gallium `st_validate_state`。

**九个** validate 类（`FillPoints.def`）：`kDraw`、`kDispatch`、`kClear`、`kBlitOrCopy`、`kTextureOp`、`kReadback`、`kXfbSpan`、`kProgramOp`、`kQuery`。落地形态是唯一的 `MGPipeValidateForVerb(MGPipeVerb)`（`MG_Impl/Pipe/PipeFill.h`），恰好是每次经函数指针表调用之前的一条语句、位于每个提前返回之后，verb 到类的映射由生成表回答。九个而不是四个，因为约 48 个非 draw 表项自己就读 `pGLContext`。

**只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）。

### 5.2 dirty 位：值类零新增记账，对象类新增 5 个聚合世代

| dirty 位 | 类 | 快门来源 |
|---|---|---|
| `NEW_RENDER_STATE` / `NEW_PIPELINE_STATE` | 值 | `m_version` / `m_pipelineStateVersion` |
| `NEW_PIXEL_PACK`、`NEW_PATCH_STATE`（`BitwiseEqual`，NaN 合法）、`NEW_VERTEX_ATTRIB_DEFAULTS`、`NEW_VERTEX_ELEMENTS` | 值 | 既有计数器 |
| `NEW_SHADER`、`NEW_SHADER_BINDINGS`、`NEW_GLOBAL_CONSTANTS` | 值 | link / image-unit / backend-state / block-binding / uniform-write-set / UBO-content 版本 |
| `NEW_VERTEX_BUFFERS` | 对象 | `VertexArrayState::m_anyVaoAttributeGeneration` → 命中后走 32 属性前缀 |
| `NEW_INDEX_BUFFER` | 对象 | 索引 slot 版本 + 绑定对象 `{slot,gen}` |
| `NEW_FRAMEBUFFER` | 对象 | `FramebufferState::m_anyAttachmentGeneration` + 对象/slot 版本 → 重算 `ContentHash` |
| `NEW_SAMPLER_VIEWS`、`NEW_SAMPLERS`、`NEW_SHADER_IMAGES` | 对象 | `TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration` + bind / sampling-resolution generation → `GetMaxTouchedUnit()` 前缀、重算集合 hash |
| `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` | 对象 | `BufferState::m_anyBufferChangeGeneration` → `GetTouchedBindPointCount()` 前缀 |

五个聚合世代落在既有 bump 点上，把对象类组的快门降成一次 `Uint64` 比较；对象类不能靠轮询逐对象版本（没有聚合能回答"有没有哪张已绑定纹理动了"）。完整性由 `scripts/gen_pipe_dirty_surface.py` 保证：枚举每个 mutator → 必须 bump 的聚合世代，未映射或映射行点名一个已不存在的 mutator 即失败（`--check` + `--self-test` 都是 CI 门）。**快门看不见自己的主体**是 P4a 踩过三次的坑（`glBindSampler`、SSO 下 `GetCurrentProgram()`、`glBindImageTexture` 只换 level）——新增字段必须带"记录字段 → setter → 快门"一行，且优先混入已有世代（新增计数器会撑大 pull 对象，G1 不允许）。

三个回绕 `Uint16` 在 tracker 边界加宽（`m_lastPushed[]` 是 tracker 自己的字段）；回绕在 tracker 本地无害（多一次重推，永不漏推）。

### 5.3 渲染状态：整块 blob 过线，身份只取 pipeline 子集，动态状态单独走（D-B1）

```
create_render_state(cso, MGPBlobRef pipelineSubsetChunks)      // 只带 pipeline 子集
bind_render_state(cso, Uint16 version, Uint16 pipelineVersion)  // 稳态 12 B
set_dynamic_state(MGPBlobRef dynamicChunks, Uint16 version)     // 只带动态子集的变化 chunk
```

- 整块：`RenderStateParameters` 是平凡可复制 POD，Espryt 做 head/blend/tail 三段 memcmp，**字段顺序承重**；拆成三个 CSO 要手工维护 ~150 字段划分表且无绊线。
- 子集身份：整块内容寻址会让 `glViewport`/`glScissor`/`glClearColor` 每次铸造新 CSO、冲掉 server 的 pipeline memo（`RenderState.h` 记录的那次回归）。
- 划分只写在一处：`MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` 的 chunk 表（16 个 `constexpr` 边界，两半交替：**7 个 pipeline chunk 共 396 B + 8 个 dynamic chunk 共 772 B = 1168**，总数与划分完整性是 `static_assert`）+ `MGPipeComputePipelineSubsetHash()`，client 与两个后端共用。G7 的 `RenderStateSpansTest` 遍历每个 setter，断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变`。
- server 侧：每 context 一份 working `RenderStateParameters`，`bind` 与 `set_dynamic_state` 各把自己的 chunk 散射进去；**Espryt 的 `SyncRenderState` 一行不动**；Magma 的 pipeline memo 键是 `cso.slot`。
- client 取值顺序：`m_pipelineStateVersion` 未变 → 复用上一个 CSO handle，零哈希；变了 → 对 396 B 子集算 xxHash → map 探测 → 命中发 12 B bind，未命中发 create 再 bind；`m_version` 变而子集未变 → 只发 `set_dynamic_state`。
- P2 补了三个此前无存储的 capability：`FramebufferSrgb`、`DepthClamp`、`TextureCubeMapSeamless`，落在 `ColorMasks` 与 `ClearColor` 之间的 3 字节空洞，`sizeof` 仍 1168、既有成员一个没挪位。

### 5.4 验证不变式、合并与抑制器

规范（D-B3）：**一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处从它此刻持有的全部已推送状态惰性特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间没有顺序要求。** 退役 Espryt 的 fragColor 重推导 workaround、`g_broadcastMemo*` 与 `ImageUnitFormatsStillMatch` 的机制是惰性特化，不是调用顺序。

`create_shader_state` 从编译池的终止 continuation 发出（不是从 draw），SPIR-V 在首个用到它的 draw 之前到达 server。

合并规则：整块结构优于逐字段；高水位标记留在 tracker 走查里、直接是 `count` 实参；只发 program 解析过的集合；**集合 hash 抑制器**——每条 `kVarTail` `set_*` 在 client 算已解析集合的 xxHash，未变不发（从后端搬到 client 的 ~175 行去抖；MC 26.2 每次纹理单元切换都重绑同一个 sampler）。索引绑定范围在 validate 时刻实时解析。

### 5.5 sampler view 在 client 侧解析

GL 是每 unit 每 target 各一个绑定；shader 看见哪一个取决于 sampler uniform 类型、mipmap 完备性与 `IsUndefinedDefaultTexture`。gallium 的"每槽一个 view"就是解析后的形态，解析留在 client 并带自己的 memo。两处后端特定后处理留在 server：Espryt 的 raw-depth-fetch sampler 替换、Magma 的 feedback-loop 检测。

### 5.6 生命周期、共享组、composite program

- `resource_create` 在前端对象构造时发，存储由 `resource_respecify` 惰性定义；`resource_destroy` 在析构时发。顺序约束由 payload 表达：view 先于存储属主销毁（`ViewOf` + server keep-alive）、FBO attachment 钉住纹理、buffer texture 钉住 buffer。死亡在 wire delete 与 slot free 之间转发给每个 emitter（六个死亡 helper 只释放 slot 曾导致 UAF）。
- 共享组：v1 一个 screen、一个 context、一条 flow；`eglMakeCurrent` 是 flow 所有权转移。
- program pipeline 合成体（`MG_Impl/Pipe/CompositeResolver.h`）：`GLContext::GetProgramForDraw()` 完全在前端合成，tracker 推**一个** handle，slot 从 `ShaderCso` 保留段分配，pipeline cache 淘汰时释放 slot、`gen++`、发 `delete_shader_state`。释放有两条路且必须恰好一次：合成体被换掉时由 resolver 释放，程序对象死亡时由 `~ProgramObject` 释放（D-H7）。**记忆按 `(ContextId, 管线 GL 名)` 键控**——resolver 是进程级单例而 GL 名是每上下文的。

### 5.7 emulation 的归属

规则：**驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering。** 只有三个"读前端字节的纯 CPU 变换"下放到 client。

| emulation | 归属 | 过线的是什么 |
|---|---|---|
| client 顶点数组 | client | 字节（`MGHostSpan`），永不是指针 |
| 最大索引扫描（`TryComputeMaxIndexFromHostBytes`） | client | `MGPDrawInfo::MinIndex/MaxIndex`（flag 门控） |
| client 索引数组 | client | 变长尾里的 `MGHostSpan` |
| `*IndirectCount` 计数解析 | client | 解析后的 `MGPDrawRange[]` |
| primitive-restart 重写（整 EBO） | **server** | 零线上流量：从索引宿主镜像读（§10.3） |
| multi-draw 五档分档 + 展平 | **server** | 同上 |
| viewport-array N 遍回放 | server | 无新增（16 组 viewport/scissor/depth-range 已在渲染状态里） |
| fp64 顶点窄化 | server | 原始字节；`IsLong` 与 `Type` 分开过线 |
| image-bindable 存储加宽/拆分 | server | 正向 `ImageBindableHint`；反向纹理拉取 + 终止符 |
| 生成 mipmap 的前端存储 | 拆开：client 分配 level 存储，server 生成 | `MGPMipPlan`；`OnMipLevelsGenerated` 只带形状 |
| CopyImage shadow 镜像 | client | 只回"拷贝成功" |
| XFB CPU 图元计数 / scatter 的 read-modify-write | client | `XfbCpuCapturedVertices`、`MGPXfbAccounting`；§8.5 |
| 压缩纹理 / pixel unpack 规整 | client | 无 |

**陈旧索引纪律是逐站点表**，client 侧扫描/解析之前的 reconcile 必须逐字复现 monolith 的集合：最大索引扫描（EBO 源）= `SyncPersistentMappedRange()` **+** `SyncGpuWrites()`；`*IndirectCount` 计数解析 = **只** `SyncPersistentMappedRange()`（monolith 今天就只做这一个）；client 顶点数组范围计算与 client 指针源扫描 = 无；server 侧重写 / 展平从镜像读，GPU 写者可见性由 `OnGpuWritten` 收窄集在 server 本地判定。门：`ClientArrayAfterComputeWriteScenario`（去掉等待必须看到几何缺失）；`create-indirect` fixture 上 `roundtrips-per-frame` 必须读零（P8）。

## 6. 纹理 subdata 与 dirty 归属

- `glTexSubImage*` 根本不调后端表：全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做（96-rect 级联合并、union-box 回退、unpack ring 可用时塌成一个 box）——Mali 按**作业数**给上传计价，~100 个精灵 rect 对一个 union box 实测 +6 ms/frame。逐 `glTexSubImage` 发一条记录会精确复现那个形状。
- 因此 client 在自己的 `MipmapStorage` rect 模型里累积，在**下一个 validate / flush 点**把合并后的形状作为**一条** `ResourceSubData` 发出，**同时携带 union box 与 region 列表，由 server 选上传形状**（决策留在付 GPU 代价的那一侧；`MEASUREMENTS.md` §1.3：vanilla 世界同样 185 次发射，Espryt 整 box 635 KB/帧、Magma rect 40 KB）。
- `MGPSubRegion` 显式携带 `SrcRowStride/SrcSliceStride`，`MGPSubData::SourceIsVerbatimLevelShadow` 显式回答原来由指针比较回答的问题；Espryt 的上传路径改为从描述符取步长。
- **dirty 归属反转**：client 保留 rect 模型、维护发射游标、发射后清自己的标志，server 从不碰 client 的标志（`MG_Impl` 里没有任何 `IsStorageDirty` 调用点）。**P4a 落地的是排水列表**——client 在 validate 点把每个脏的 `(存储属主, 上传目标, level)` 发成一条 `resource_subdata`；按存储属主键控的发射游标与 view 索引重映射是 P3b/P4b 的。两条规矩：**清 dirty 要等 applier 的 acceptance**（被拒的记录必须让 level 留在脏表里）；**服务端自己的重新变脏是服务端的事**（twin 直接重新武装 applier 记录里的待上传项）。
- 后端在 shadow 里写字节的两处——CPU 回退生成 mip 与 `glCopyImageSubData` 目的地镜像——分别由 `OnTextureWriteback` 与"CopyImage 镜像搬到 client"处理。Unpack PBO 完全在 client 解析；`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client。
- `TextureUploadShapeScenario`（形状金标）已建好但只记录不设门，等 Mali 侧的帧时增量才升级成门。

## 7. Shader state = SPIR-V + 反射归档

- `CreateShaderState` 的 payload 是逐 stage SPIR-V + 反射归档（`LinkArtifacts` + `SpirvArtifacts` 全结构体），**不是源码**。glslang 全在 client，SPIRV-Cross 全在 server，文件级切割；没有 `MOBILEGL_IPC_PROGRAM` 开关、没有 server 侧 compile pool。
- 归档机制：`Visit()` + `sizeof` 绊线。序列化器在树里（`MG_Pipe/ProgramArtifactsCodec.{h,cpp}`），**monolith 只在 verify 构建里调它**（D-H3：制品不在 monolith 过线，编解码不在热路径上）。未完：`ProgramArtifacts.h` 的 libc++/NDK 尺寸钉仍是惰性的（`MGL_ARTIFACT_SIZES_LIBCXX_PINNED` 未定义），记在 P3b/P4b。
- P0.5 把反射类型抽到 `MG_State/GLState/ProgramState/ProgramArtifacts.h`、值类型抽到 `MG_Pipe/MGPipeValueTypes.h`（不 include `MG_State/GLState`），CI `-H` 闭包断言把关；没有这一步 P7 的 `nm -D | grep glslang` 判据不可达。
- server 侧惰性特化（D-B2）：后端 program 还依赖 8 个额外输入（draw FBO 的 clamp mask、fragColor 广播数、storage-block 绑定签名、atomic counter 集、活的 image 格式、patch 参数；Magma 另加 Y-flip 高度与 XFB 布局），`create_shader_state` 发布**制品**，server 在 verb 时刻从已推送状态特化——正是两个后端今天的做法。
- 后端 link/compile 失败不需要同步返回：`GL_LINK_STATUS` 永不撤回，同步查询由 client 从 `ProgramObject` 回答，失败以 `OnLog` ≥ERROR 无损呈现。
- Magma 的两个内部 shader（blit、depth-mipmap）烘焙成签进树的 SPIR-V，`MG_Test` 重跑树内 glslang 逐字节比对守新鲜度（`MOBILEGL_BAKED_INTERNAL_SHADERS`，P7）。

## 8. 反向通道

### 8.1 `MGPipeCallbacks`（`MG_Pipe/MGPipeCallbacks.h`）

十个具名回调 + 一个正向终止符（`ResourceSubDataComplete`），取代今天 95 个调用点 / 17 个方法直接 poke 前端对象。gallium 没有这些词汇（Mesa 里两者共享地址空间），具名化是有意偏离（D8）。monolith 下直调，split 下是 `SEG_EVENT` 上的记录。

| 回调 | 取代 |
|---|---|
| `OnGlError(code)` | 6 处 `RecordError`；**必须对命令流有序**（`glGetError` 本身永远本地） |
| `OnGpuWritten(res, ranges[])` | 6 处 `MarkGpuWritten`：client 在每个 draw/dispatch 发射点保守自建 pending 集，这是**收窄**通道 |
| `OnBufferWriteback(res, offset, bytes)` | PBO 回读、XFB 捕获；按操作级批处理；必须与 epoch bump 有序 |
| `OnTextureWriteback(res, box, bytes)` | CPU 回退生成 mip 的纹素（唯一生产者） |
| `OnTexturePullRequest(res, target, firstLevel, levelCount, pullSerial)` | §8.4 |
| `OnMipLevelsGenerated(res, base, count)` | 只带形状，与 monolith 的 `EnsureGenerateMipmapStorageAllocated` 行为一致 |
| `OnSurfaceChanged(info)` | `SwapchainObject` 写 `pDefaultFramebufferInfo` 的分层倒置；client 自己合成 default-FB 对象 |
| `OnCapsInvalidated()` | 2 处 `InvalidateCompileEnv` |
| `OnLog(level, text)` | ≤WARN 有损，≥ERROR 无损 + 速率限制 |

95 个写回点的其余归属：`MarkStorageDirty` 大多是 server 本地记账；后端凭空造的前端对象（Magma 占位纹理、swapchain default-FB 占位）→ server 原生；`SetBackendStateMemo` 直接删除；`SetBackendHashMemo/AuxMemo` → server 侧 per-slot 字段。**D-K（P3a）**：`PipeResource::m_backend` / `SetBackendResource` / `ReleaseBackend` / `BackendBufferResource` 在 push 下只是不再被写，真删会移动 pull 构建里 `sizeof(BufferObject)`（G1 破坏），随 pull 路径在 P13 退役。

### 8.2 有序性是正确性要求

每一次 `WritebackFromBackend` 后面都紧跟 `BumpBufferMutationEpoch()`，否则 server 的 draw-clean memo 会在 epoch 背后变陈旧——split 里这是反向通道上的排序规则：写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**

### 8.3 错误、ack 与日志

- 纹理分配的 OOM 在 monolith 里就已推迟到 sync 时刻，拆分不改变可观察行为，不同步 ack。**唯一允许同步 ack 的入口是 `glBufferStorage`**；`glRenderbufferStorage*` 不 ack（41 个 trace fixture 里 OOM 探测惯用法出现 0 次，语料里的成功性检查是 `glCheckFramebufferStatus`，client 本地作答）。
- `ResourceRespecify` 携带 `kNeedsAck` 并带逐记录谓词 `MGPipeResourceRespecifyNeedsAck(desc)`（`Immutable != 0` **且是 buffer 目标**，D-A2——`glTexStorage*` 的 `Immutable` 同样是 1，不收窄每次不可变纹理分配都会变成同步往返）；另有**逐 level 作用域**（`MGPRespecifiedLevel` 尾；不带 level 时 `glTexImage2D(level 1)` 会把已接受的第 0 级待上传一起丢掉）与**元数据更新**（描述符逐项相同时只换 `BindMask`/`ImageBindableHint`，不 ack、不清待上传）。
- 其余错误一律晚到，走有序的 `OnGlError`。`OnLog` ≤WARN 有损（覆盖最旧 + `eventDropped`）；≥ERROR 无损，加入触发 `eventRingFull` + 停止 apply 的语义事件集，每秒速率限制器。

### 8.4 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素，三个原因会要求重发已发过的 level：`RequireImageBindableStorage` 的 re-dirty、整格式再生、view 源重铸。四条缓解：(1) **预防主因**：`ResourceCreate/Respecify` 一直携带 `ImageBindableHint`；(2) **拉取异步**：server 发 `OnTexturePullRequest` 并把 twin 标 not-ready，阻塞的是 apply 线程不是应用线程；(3) 有上限的保留默认关（`MOBILEGL_PIPE_TEXEL_RETAIN_MB=0`，`MipmapStorage` 保有每 level 完整 CPU 影子，缓存买的是延迟不是正确性）；(4) **显式终止符** `ResourceSubDataComplete`，可携带零个 region（内容只来自渲染 / GPU mip 的 level，client 没有字节；server 带着"已分配但为空"的存储继续）。真实语料上重铸拉取率见 `ROADMAP.md` 开放问题 2（780 个窗口 2 次）。P5b 下这条路径是 `Fatal{UnmigratedEmulation,"texture-remint-pull"}`（三条 trace 的首阻塞），P9 落地。

### 8.5 XFB scatter 留在 server（本节在 P5c/P5f 之后重写；P3b/P4b espryt D1 更正文本）

Espryt 的 `ScatterCapturedRecords` 是 read-modify-write：`gl_SkipComponents` 的空洞必须保留目的缓冲原本的内容，所以补丁循环需要「捕获前的字节」。本节原来写的是**搬到 client**——server 把紧密打包的 scratch 经 `OnBufferWriteback` 推给 client，用 `OnXfbScatterReady` 告知布局，client 跑补丁循环再以 `ResourceSubData` 重发。**树上走的是相反的一条路**，这一段按树更正：

- **捕获前的字节是 server 自己的 staged shadow**（rule C：拆分后权威影子归 server，`GLESBufferResource::hostBytes`），不是 client 的 `MappedData()`——后者在 apply 线程上是 `Fatal{RoleViolation, "buffer-legacy-arm"}`（CONTRACT-P5C §3.8）。所以 server 有它需要的一切：影子、反射归档里的 varying/stride（`ProgramArchive->Link`），补丁循环原地跑完。
- **回程只有一条 `OnBufferWriteback`**，载的是**补好的整段**，不是 scratch。`OnXfbScatterReady` 因此是零生产者、零消费者、无 EventKind 的死声明，P3b/P4b espryt D1 slice 3 删除，`kMGPipeCallbackCount` 10 → 9。
- **该回写按 SEG_EVENT 容量的四分之一切片**（`MGPipeBufferWritebackSliceBytes`，espryt D1 slice 2）：字节内联在 SEG_EVENT 记录里，而环形缓冲拒绝任何超过容量一半的记录，所以整段投递在 >128 KiB 时是 `Fatal{EventRingOverflow}`。切片按序投递，最后一片落地即蕴含之前每一片。
- **孤儿目标（`HasDefinedContent == 0`）是合法目标**（espryt D1 slice 1）：它没有可读的捕获前字节是应用自己声明的，零填充后照常散射并上传，与 monolith 从新鲜 `MappedData()` 读到的逐字节相同。

不新增停顿类。

## 9. 后端状态机改造

### 9.1 原样不动的东西

Espryt：三条 persistent-mapped ring 与 `PersistentRing` 算法、buffer pool、fallback-repack 路径、`m_backendColorSlots` 置换表、scratch FBO 及驱动侧影子、全部驱动绑定影子、Adreno / Mali workaround、SPIRV-Cross 会话与 ESSL 重写、驱动 POST 自检、restart 重写与 multi-draw 五档。Magma：`VulkanRenderer` 全部 memo 与 scratch、`PipelineFactory`、`ProgramFactory`、`UniformManager`、五个 `Vk*Manager`、`FrameContext`、`SwapchainObject`、`DynamicStateShadow`、`VertexInputStateFactory` 的 cache 本体、**D18 的节点式容器纪律**。从"不动"里移出的一项：Espryt 的 sub-rect 上传判定与跨步计算（§6）。

**字节一致门 G5**（`scripts/p3a_untouched_regions.sh`：pool / 延迟释放 / ring / flush 梯十一函数；`scripts/p4a_untouched_regions.sh`：depth-stencil 采样模拟与格式 caveat 两族 17 区 / 3 文件）：这些是"被推送的记录改变了输入、但算法本身不许动"的那一类，字节一致是唯一能证明这点的门；两份脚本都对固定 pin 比较，re-pin 必须同时改两份。仍在 `MG_Backend` 里的前端类型：`g_rawDepthFetchSamplerState`（Espryt 自己为原始深度取样铸的 `SamplerObject`，client 从没见过它），原生化归 P3b/P4b。

必须真改的 `MG_State` 类型内部用法（都在 Magma，P7）：占位纹理（~120 行木偶戏 → 原生 `VkImage`）、两个内部 shader 烘焙（§7）。

### 9.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充 + poison 世代（P1）

```cpp
// MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，不按读点组织
struct PipeInputs {
    const RenderStateParameters& GetRenderStateParameters() const;   // 类型与后端今天读到的完全一致
    // … 每个后端真正用到的 GLContext 方法一个访问器（Espryt 32 / Magma 56）
#if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED
    Uint64 m_filledGen[kFieldCount];   // 逐字段"上次填充的 verb 序号"
    Uint64 m_currentVerbSerial;
#endif
};
#if MOBILEGL_PIPE_PUSH
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

| 阶段 | 改什么 | 证明 |
|---|---|---|
| A 别名（P1） | 机械替换 `MG_State::pGLContext->` → `MGB_CTX->`（277 处）+ 58 行非箭头用法；逐 verb 类填充点填 `gPipeInputs` | `nm --defined-only` 不变；`.text` 差异可逐行归因 |
| B 推送（P2） | tracker 填 `gPipeInputs`，填充器按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | `MOBILEGL_PIPE_VERIFY=1`：tracker 再填一份快照版，G4 比对器逐字段每 draw 比一次 |
| C 句柄化（P3a–P4a、P7） | `SharedPtr<前端对象>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§13） |

poison 是**逐 verb 世代**不是位图：每次 verb 递增 `m_currentVerbSerial`，字段被填时记下序号，读取时断言相等（跨 verb 有效的字段显式标 sticky）。读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`。纯度门 grep 的是 `pGLContext` 不是 `pGLContext->`。`FillPoints.def` 里 8 条静态过近似的填充行有意保留（能退役它们的证据只能是动态的：`MOBILEGL_PIPE_POISON_OMIT` 跑遍完整 CTS caselist，P3a 之后未做）。

### 9.3 Track V / Track H

Track V（值类型：render state、pixel store、capability 位、stencil/colormask/depthmask/scissor/patch/attrib 默认值、Magma 标量 getter，约 B 类读点的 55%）：机械。Track H（对象类型：167 个 `SharedPtr<MG_State…>` 点）：真活。读点分类（静态）：探测变化 ~12%、翻译输入 ~74%、瞬时参数 / 身份键 / 数据字节其余——74% 是翻译输入，"bump 一个版本让 server 自己拉"行不通，值本身必须过去。

### 9.4 残余值块

P2 发一个**显式临时**调用 `SetResidualValueState(MGPBlobRef)`，payload `ResidualValueBlock`。三条纪律：退役是编译错误（`MGL_RESIDUAL_BLOCK_SIZE` 只降不升，P2 已从 1248 棘轮到 **8**——只剩 `Uint64 CapabilityBits`，P13 变成 `static_assert(sizeof == 0)`）；布局逐成员 `offsetof` 断言且 split 下逐字段序列化；`MOBILEGL_PIPE_STATS` 单独计一类字节。

### 9.5 21 条身份 memo 的重键

统一事实：每个进入 memo 键的版本计数器要么是回绕 `Uint16`，要么根本不会被它害怕的那个 mutation bump；身份比较是堵回绕洞的补丁。`{slot, gen}` + 显式 destroy 让 **11 条直接删除**（registry 的同址 `weak_ptr` + GC ×6、`TwinLookupMemo` ×3 + `OwnerEquals`、`SetBackendStateMemo`、`VkTextureManager::TextureIdentity` 存活探测、`ConvertedVertexStreamKey` 的 `sourcePin`……），**2 条** server 删除但去抖搬到 client（§5.4），**7 条重键**成更便宜的比较（`StampSyncedFBO` 四元组 → `ContentHash` + server 私有 `attachmentRemintEpoch`；`ResolvedTextureBindingMemo` 9 键 → `(shaderCso.slot, viewSetSerial)`；`SetupDrawSnapshot` 的 ~14 探测字段 → 三个 handle + 两个 server 纪元 + dirty mask；`VertexInputStateFactory::ComputeHash` 里的 lifetimeId → `gen` 混进 server 侧每个 content hash），**1 条**（D18）原样不动。P2 付了 11 条删除与 2 条重键，P3a 付了 buffer/VAO 的重键（`MEASUREMENTS.md` §4.3）。

### 9.6 A/B 与口径收窄

`MOBILEGL_PIPE_PUSH` 子系统位图在阶段 B 是真正的旧-vs-新 A/B；阶段 C 之后不是——位清零时后端仍跑重键后的 memo 代码。对策：**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）保留 pre-handle 臂活在同一个 `PipeInputs` 接口之下，随 pull 路径在 P13 退役。

- buffer 家族的 pre-handle 臂是 `Ops_*` 表加 `g_glesBufferBackendOps`，无条件编译；VAO twin 与 P4a 六种 twin 的前端读取臂在 `MOBILEGL_PIPE_LEGACY_MEMOS` 之下，位清零 + `LEGACY_MEMOS=0` 是没有任何臂的配置，arm resolver **响亮地**报 `Fatal{PipeLegacyMemosDisabled}` 而不是静默。
- 位依赖两侧都拒：服务端在各族 arm resolver 里拒绝并跑旧臂，客户端在 `PipeFill.cpp` 的族门里**根本不发射**——只在服务端拒会出现"客户端已按 acceptance 清了 dirty、服务端却走旧臂"的丢上传。同一族门上挂着**消费者条件**：没有任何后端注册 `MGPipeResourceOps` 时 P4a 四族一条不发（Magma 就是这种情形，P7 之前）。
- 退役的 twin 成员（VAO twin 的五个同步 memo、`g_pendingFetchBaseInstance` 与 `ScopedFetchBaseInstance`、D-K 那组）都**仍在 pull 构建里编译**——真删会移走 pull 符号或改 `sizeof`，G1 的 0/0/0/0 就是这条的度量。

P13：删 `SnapshotFromGLContext()` 的非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**保留 `MOBILEGL_PIPE_VERIFY`**（D-B5，verify 构建永不出货）；三道纯度门在非 verify 构建上转绿。

## 10. server 侧

### 10.1 对象表与 applier

- `MG_Remote/Server/PipeApplier`：解码 → 更新对象表与 `PipeInputs` → 调后端函数指针。server 不持有任何 buffer 的完整副本、不持有纹素、不持有前端对象图；debug 断言任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界。`InProcessTransport` 走与 spawn **完全相同**的 G3 编解码路径，只在门铃/拷贝机制上不同。
- 每 context 一份 working `RenderStateParameters`（§5.3）。

### 10.2 monolith 侧的净收益

即使 IPC 永不上线：复用地址 ABA 一整类不可表达；FBO → program 排序 hazard 消失；`SwapchainObject` 写 `MG_Impl` 的分层倒置消失；一次 glslang 编译离开启动路径；`inproc` = 渲染线程；`MG_Test` 的 mock 后端变成 MGPipe recorder（§13.3）。monolith 净代码量是**增加**的，所以 monolith 论据是逐线程 CPU 数字，不是删除行数。

### 10.3 索引宿主镜像（`Server/IndexHostMirror`，P8）

覆盖 `BindMask & ELEMENT_ARRAY` 的资源，且仅当 `kCapNeedsHostIndexBytes`。由 server 本来就要收的 `ResourceCreate/Respecify/SubData` 流增量维护：零额外线上流量、零 round trip；GPU 写者对镜像的影响由 `OnGpuWritten` 收窄集在 server 本地判定。预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），超预算时该 buffer 退化为逐 draw 经 `MGHostSpan` 传送（计入 `index-bytes-shipped`）。必须是它：`kMaxRestartRewriteBytes` = 64 MiB 是默认 `SEG_STAGE` 的两倍。它是本设计里唯一的"数据副本"。

## 11. 传输与数据面（`MobileGL/MG_Remote/`）

### 11.1 段

| 段 | id | 拥有者 | 默认 | 内容 |
|---|---|---|---|---|
| `SEG_CMD` | 1 | client（server 只读） | 8 MiB，2 的幂 | `RingControl`（4 KiB 页）+ POD 记录 + ≤4 KiB 内联负载 |
| `SEG_STAGE` | 2 | client | 32 MiB | bulk 字节：buffer sub-data、纹理紧密重打包区域、UBO scratch、client 顶点/索引/indirect 数组、multi-draw 参数块、具名 UBO host payload、persistent-map 脏块 |
| `SEG_REPLY` | 3 | server（client 只读） | 16 MiB，8 个 2 MiB slot（ID-47） | readback 像素、buffer writeback、acceptance 答案 |
| `SEG_EVENT` | 4 | server | 256 KiB SPSC ring | 十个回调的事件 + `EvQueryResult/EvFenceSignaled/EvReadbackDone` |
| `SEG_SHADOW[n]` | 5 | client | 每对象 ≥256 KiB（Phase 2） | 零拷贝 buffer/texture shadow |
| `SEG_ADOPT[n]` | 6 | server（client RW） | 每 buffer ≥16 MiB（P11） | 应用直写 GPU 内存 |

`SEG_STAGE` 默认 32 MiB 曾装不下 P5b 目标负载的单次 128 MiB 上传；**内容侧分块已落地（2026-09-20）**——buffer 范围走查（`1e7c372e`）与纹理整宽 slab（`9469d48e`）按 `MGPipeStageChunkBytes()`（默认 segment/4 = 8 MiB）切成多条记录，默认 32 MiB 即可容纳，历史普查 / 设备的显式 `MOBILEGL_IPC_STAGE_MB=256` profile 不再是这两条路径的必需；未接入分片的 record 类型与专用 carrier 仍是 P8 的设计（`ROADMAP.md` 开放问题 11）。

创建（`ShmSegment`）：Android `ASharedMemory_create`（API 26）；桌面 Linux `memfd_create`；其他 POSIX `shm_open`；Windows `CreateFileMappingW`。传递：POSIX `SCM_RIGHTS`（`FdPassing`，专用 `AF_UNIX SOCK_DGRAM` socketpair）；Windows 段名走 `SegmentRef`。不进 `SEG_STAGE` 的：restart 重写的整 EBO 与 multi-draw 展平的索引流（走索引镜像）。`SEG_SHADOW` 块的退休：释放的块进 pending 链表，`appliedSeq`（借入 GPU 时间线的 slot 用 `retiredSeq`）越过最后一条引用它的记录后才归还。

### 11.2 `RingControl`（`Ring.h`）

一页 4 KiB，每个争用组各占一条 cache line。**P6 包 `lk` 按写者重组过这一页**（`CONTRACT-P6.md` §8.3）：producer 线 `cmdHead` + `submittedSeq`（producer 写、诊断量，`Ring.h` 的 "THE FIVE WATERMARKS" 段写明无人等待它）；consumer 线 `cmdAppliedTail / cmdRetiredTail`；`SEG_STAGE` 独立三元组（P5 起故意全零，见 `Ring.h`）；**四个 consumer 写的水位收成具名成员 `Progress`**（`Transport::LinkProgress`，`ILink.h`）——`appliedSeq`（释放 `*AppliedTail`）/ `retiredSeq`（释放 staging）/ `completedFrameSerial`（释放 `*RetiredTail` 与 `SEG_ADOPT`）/ `presentAckSerial`，访问写作 `control.Progress.appliedSeq`；最后是 `serverEpoch`、`ringGeneration`、`consumerParked`/`producerParked`、`eventRingFull`、`eventDropped`。**两个 event 标志刻意留在最后这一组**：`eventRingFull` 有两个写者（server `store(1)`、client 每次 drain 一次 `exchange(0)`），放进 `Progress` 会把 client 的 RMW 落到 client 自己自旋读的那条线上。两个 tail 是必须的：P11 之后 server 会**借用** ring slot 而不是再拷一次。游标是单调字节计数、2 的幂掩码、永不重置。

记录头 `RingRecordHeader{kind, flags, size}`，kind 0 保留给 wrap 填充；`RingProducer::Reserve` 在记录会跨 wrap 边界时自动发 pad 记录；`RingConsumer::Pop` 拒绝不可能的头并置 corrupt → `Fatal{ProtocolCorruption}`；`HardDrainRing` 只在两侧静默且 ring 全空时 bump generation。

### 11.3 双向 doorbell（`Doorbell.h`）

- client → server：consumer 自旋 → 置 `consumerParked=1` → 阻塞；producer release-store `cmdHead` 之后仅当 `consumerParked` 时敲。
- server → client：client 在**任何**等待先自旋 `MOBILEGL_IPC_SPIN_US`（默认 50 µs）→ 置 `producerParked=1` → 阻塞；server 在 release-store 任何 watermark 之后仅当 `producerParked` 时敲。没有第二个方向，每处 client 等待都退化成跨进程自旋一条 cache line。
- 两个实现：`CondVarDoorbell`（`inproc`，带 `Kill()` 死亡态让 `Shutdown` 能 join 一个 parked 的等待者）与 `SocketDoorbell`（`spawn`，一字节；对端关闭时 `POLLIN|POLLHUP` + `recv()==0` 是死亡检测）。丢失唤醒窗口由两个 `seq_cst` fence 关闭。

### 11.4 控制面（`protocol.fbs`、`Framing.h`、`ITransport.h`）

- 一份 schema，两种用法：热路径 → FlatBuffers `struct` 直接进 ring（即 G3 生成的记录，与 `MGPipeTypes.h` 的 POD 逐条 `static_assert` 对齐）；罕见/变长/需演进 → `table` 走 CTRL socket（`CtrlMsg`：`Hello`、`Welcome`、`CapsSnapshot`、`SurfaceOp/SurfaceReply`、`ResyncRequest/Done`、`AuxRequest`、`Fatal`、`LogLine`），`file_identifier "MGLC"`，union tag 只追加。
- `protocol_generated.h` 提交进树，`scripts/gen_protocol.py` 再生成（只用 `MOBILEGL_FLATC_EXECUTABLE` 或 pinned submodule 构建的 flatc），CI `flatc-check` 重生成并 diff；**codegen 绝不进默认构建图**。
- 封帧 `[u32 'MGLF'][u32 len][payload]`，64 MiB 上限，**读时校验**：坏 magic / 超长长度立即 latch 失败并报 `MOBILEGL_ERR_PROTOCOL_MISMATCH`；接收缓冲不足返回所需大小并保留消息。
- `ITransport`：`SendFrame / ReceiveFrame / PeekFrameSize / ShareFd / ReceiveFd / Shutdown / Role`；热路径完全绕过它。`WireLog.h` 是 `Transport/` 唯一的日志入口（纯度门 A）。`mg_protocol_base.h`：纯 C 的结果码 / span / `ShmRegion` 词汇，structSize-first 版本纪律。

### 11.5 WAR 危害、拷贝账与背压

Phase 1（P5–P8）：GL 调用时刻把字节拷进 ring slot，slot 到 `stageAppliedTail` 越过它为止不可变，危害按构造消除；代价一次 memcpy。Phase 2（shadow-in-shm，零拷贝）：≥256 KiB 的 shadow 分配在 `SEG_SHADOW`，`ResourceSubData` 只带 `{seg, offset, size}`，WAR 用 per-shadow 64 KiB 块发送水位；必须整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹。

| 路径 | monolith | Phase 1 | Phase 2 |
|---|---|---|---|
| `glBufferSubData` → shadow store | 2 | 3 | 2 |
| `glBufferSubData` → adopted store（P11） | 2 | 2 | 2 |
| `glMapBufferRange(WRITE)`+unmap | 3 | 4 | 3 |
| persistent coherent map 推送（§12） | 0 | 1/发射点 | 1/发射点（精确块） |
| `glTexSubImage` | 2 | 2 | 2 |
| 全局 UBO / draw | 1 | 2 | 1 |
| adopted ≥16 MiB（P11） | 0 | 0 | 0 |

server 没有第二份 `BufferObject`，不存在"staging → server 侧 shadow"这次中间拷贝。分配与背压：逐字移植 `PersistentRing`；分配失败升级：扩容 → 对最老未 retire 批次有界等待（走 `producerParked` doorbell）→ 硬 `Drain` + `ringGeneration` bump；硬 drain 后恢复便宜：tracker 把全部 dirty 位置为"必须重推"。P5 落地形状：staging 与 command ring 的等待都以未退休发布记录的真实退休为条件（`ringwaits` 只计真正阻塞的分配），超大单记录仍拒绝。

### 11.6 publish、序号与 credit

- 不设"records ≥ 64 KiB"一类阈值。规则：每条记录（或每 8–16 条摊销）release-store `cmdHead`，仅当 `consumerParked` 时敲门铃。
- 显式门铃点：`present`、任何 `kNeedsAck` 请求、`eglMakeCurrent`、`glFlush`、`SEG_STAGE` 余量 < 1/4、**轮询类入口**（`glClientWaitSync` 任意 timeout、`glGetSynciv(GL_SYNC_STATUS)`、`glGetQueryObject*(AVAILABLE|NO_WAIT)`）；带 `GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 publish。饥饿升级：同一 handle 连续 N 次（`MOBILEGL_IPC_POLL_ESCALATE`，默认 64）本地回答"未就绪"而 watermark 毫无移动 → 升级为一次阻塞 round trip（P10）。
- `glFlush` 保持纯 no-op。**`glFinish` 在 P5e 之后不再是**：wire 上仍然没有它的记录，但"至此发出的命令都已完成"是一条 run-ahead 队列能违背的承诺，所以它变成 `WaitForApplyToCatchUp`——等到 `appliedSeq` 追上最后一条发布的记录，再排空反向通道（`CONTRACT-P5E.md` §2.5）。lockstep 下它照旧什么都不做，因为那里 client 本来就把自己发出的每条命令都等完了。
- seq = 记录序数；两个互相独立的窗口：字节 credit 与 present credit（`presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT` 时 `eglSwapBuffers` 阻塞）。server 不发 credit 消息，consumer 每 64 条记录更新一次 `appliedSeq`。
- **P5e：等待规则由生成的 `WaitClass` 列决定，不再由 call class 决定。**`MGPipeWaitClassFor(op)`（`PipeCalls.def` 的第五列）把每一行分成 `kWaitReply` / `kWaitPresent` / `kWaitApplied` / `kWaitNone`，`EmitAndWaitTails` 在 `RunAheadArmed()` 为真时照这一列走：`kWaitNone` 发布即返回，`kWaitApplied` 等一次 apply 再放掉 fill 的四个 O 类 pin，`kWaitPresent` 的等待已经在 `EmitPresent` 编码之前付过（下条）。它**替换**而不是叠加 `MOBILEGL_IPC_BATCH_WAITS` 的跳过规则：batch 读的是 call class，这一列读的是"这条记录的 apply 会读什么"，而 server 侧的 `MGPipeBarriered` 读的是后者——两条规则并存就是线两端对"client 现在是否被 park 着"给出不同答案。
- **强制等待点**（§2.5）：`MapBuffer(READ)` / `GetBufferSubData` / `CopyBufferSubData` 源走 `SyncGpuWrites`（reply 行，不变）；`glFinish` 如上；每一个 `Server*` EGL forwarder 之前等一次——control mailbox 是在两批 drain **之间**泵的，make-current 落在两条 run-ahead 记录中间不是画错一个像素而是全错。`ServerSwapEGLBuffers` 不在此列：present 就是那次 swap。`glGetError` 的放宽也在这里具名：run-ahead verb 的 `kEventGlError` 在下一个排空点被看到，即最多晚一个 present credit。

### 11.7 事件回传与溢出（P9 定策略，P5e 落地）

`SEG_EVENT` 承载十个回调加回读完成通知。client 排空点：`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`glGetSynciv`、`eglSwapBuffers`、`glMapBuffer*`/`glGetBufferSubData`/`glCopyBufferSubData`，以及每一次等待循环的每一轮。溢出策略（修一个双向死锁）：`EvLogLine` ≤WARN 有损；语义承载事件无损——ring 满时 server 置 `eventRingFull=1`、**在记录边界停止 apply**、敲反向门铃，client 排空后清标志并敲正向门铃。故障注入：client 被 credit 阻塞时灌满 `SEG_EVENT`；日志洪泛下注入一次 link 失败，那行 ERROR 必须出现且两侧恢复。

**P5e 把上面这段从计划变成代码（`CONTRACT-P5E.md` §2.6）。**`Fatal{EventRingOverflow}` 的前提是"client 每个 verb 都排空，所以 256 KiB 满环是没有工作负载会产生的突发"；run-ahead 的 client 在自己的等待点排空，稳态上相隔一个 present，满环因此是普通积压，为它中止等于把反向通道的容量变成工作负载上限。现在 producer 改为阻塞：发布已在环里的部分、敲 client 门铃、park 在自己的 bell 上等标志清掉、重试。两条具名拒绝留下，而且是**不可能**而非**繁忙**的情形——单条事件大于半个环（再怎么排空也放不下），以及 30 秒都等不到排空（对端不在跑）。不发布 `kCapRunAheadApply` 的 server 原样保留 P5C 的 Fatal。
死锁论证要两半才成立，所以两半都在：`EventRingConsumer::Drained()` **清标志的同时敲门铃**（清了不敲就是正向那条"先 publish 再敲"专门在防的丢唤醒），而 `SessionProducer::WaitForAppliedOrEventBacklog` / `WaitForPresentAckOrEventBacklog` 让 client 的每一次 park 都能被 `eventRingFull` 打断——于是"等着 client 排空的 server" 总能叫醒那个 client。`ServerLoop` 的 park 条件因此多一条：环满时等待中的记录**还不是**工作，因为 apply 它只会让 producer 停在半路；Stop 与 Control 仍然优先，拆机不被一个不排空的 client 拖住。

### 11.8 fence 与无 present 负载（P10）

fence 完成度必须来自**真的逐 fence 退休**，不是 present 水位（DirectGLES 的 `g_completedFrameSerial` 只在 `Present()` 里前进，帧中 fence 会退化成帧计数推断——`DirectVulkan.cpp` 写明这是被修掉的 bug）。无 present 循环下 `retiredTail` 会饿死：DirectGLES 的 server 加**非 present fence tick**（距上次 `Present` 超过 8 ms 或每 4096 条已 apply 记录插一个 `glFenceSync`）；P8 加一个无 present 的 split 用例。P5b 已把五条 sync 槽搬上 apply 线程（§17.5）。

### 11.9 传输栈的两根轴（P6.5，2026-09-22 定）

终局：client 与 server 可以在不同机器、不同 OS / 架构上，经 TCP 连接；同机 `spawn` 保留。为此传输栈拆成**两根独立可选、握手协商、可混搭**的轴，上层只见两个接口：

| 轴 | 接口 | 实现 | 选项 |
|---|---|---|---|
| **控制面** | `ITransport`（`Transport/ITransport.h`，只承载 FlatBuffers 控制帧，**不再承载描述符传递**） | `SocketTransport` | `fork`（继承 fd，今天的 spawn）、`unix:<path>`（sm 的独立启动）、`tcp://host:port`（跨机；同机也可用） |
| **数据面** | `ILink`（`Transport/ILink.h`，热路径：记录预留 / 提交、watermark、reply、event、span 解析） | `ShmLink`（共享段 + `RingControl` 页 + 门铃）、`StreamLink`（分块封帧、watermark 变消息、reply 变自带 seq 的消息、发送窗口） | `shm`、`stream`、`auto` |

规则：

- **配对在会话建立时选定一次，之后不变**（缝是 setup-time 的，`CONTRACT-P6.md` §8.2）。`Hello` / `Welcome` 里的 `LinkTerms{dataPlane, delivery, wireForm, maxReplyBytes, cmdWindowBytes, stageWindowBytes}` 由 **server 陈述**尺寸、client 请求可被钳制。
- **共享段的交付与控制面解耦**：`ShmLink` 自带一个 AF_UNIX aux 汇合名，在 `Welcome` 里公布，`SCM_RIGHTS` 只走它。于是同机上"控制面 TCP + 数据面共享段"是合法配对，控制面走什么与数据面走什么互不牵连；`ITransport` 收窄为纯控制帧（`auxFd = -1` 已是"这条链不传 fd"的诚实答案）。
- **`auto` 的判据是段实际交付成功**——client 连 aux、收到 fd、`Adopt` 通过 `fstat` 校验——而不是看对端地址是不是 loopback。回落到 `stream` 必须具名进日志与 stats 行（`ARM` 证明的形状，ID-124），不能静默降级。
- **上层零链路种类分支**：`SessionProducer` / `SessionConsumer` / `ServerLoop` / `PipeWireEncoder`、`ClientSession.cpp` 的 `kSegEvent` 挂载、`ResourceTracker.h` 的 writeback 解析都只经 `ILink`；`ShmLink` / `StreamLink` / `RingControl` / `ReplySlotPool` / `EventRingConsumer` / `m_shm.` 只允许出现在 `Transport/` 与其测试（lk 的 grep 门加宽）。角色 / 拓扑谓词（`MGPipeSplitActive()`、`MGPipeBlocksAreDistinct()`、`MGPipeBackendIsLocal()`）回答的是"谁在哪个进程"，不是"用的什么链路"。
- **门铃归数据面**：共享段上是 futex / condvar（同内核跨进程可用），stream 上是消息本身；控制面不参与热路径。
- **跨机意味着两端是不同二进制**：`wireFingerprint`（`PipeFields.def` 派生的逐成员布局摘要 + 目录摘要 + 字节序 / 指针宽度）永远比较，`buildFingerprint` 只在 `Dial == Fork` 下比较；不一致是 `Refuse`，不是 abort。
- 配置面（命名待 P6.5 定）：`MOBILEGL_TRANSPORT` 仍是**拓扑**（monolith / inproc / spawn，G1 相关）；新增 `MOBILEGL_IPC_CONTROL=fork|unix:<path>|tcp://host:port`（取代 `MOBILEGL_IPC_SERVER_PATH` 的角色）与 `MOBILEGL_IPC_DATA=auto|shm|stream`。
- **代价要量**：`ILink::ReserveRecord` 在 VD32 下是 ~850–3550 次/帧的虚调用，客户端线程已 ~94% 占用；`lk` 的门对 inproc 结果与逐线程 CPU 逐字不变，P6.5 再加 2×2 矩阵车道（{unix, tcp} × {shm, stream}）与混搭对 (unix + shm) 的逐线程 CPU 差。

TCP 上的两个数量级差别（RTT 毫秒级、带宽 30–100 MB/s）使 §13.1 的每一条 `kWaitReply` 行和 §14 的 `SEG_STAGE` 预算从"记录项"变成"可行性数"；见 `ROADMAP.md` P6.5 行的必测数与开放问题 19–23。

## 12. persistent map 与 ≥16 MiB 采纳

`AcquirePersistentMap` 是永久的地址空间捐赠（返回 host-visible coherent 指针；≥16 MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到，实测 MC 26.3 p99 163→21 ms、40→115 fps、省 ~400 MB）。**整个 monolith 改造期一动不动**（D-B4）。

三档，由运行时 POST 探针选择，spike B 已在两台设备上给出答案（`MEASUREMENTS.md` §1.2）：

| 档 | 形态 | 实测 |
|---|---|---|
| **T0 — server 导入 client 分配**（P11 主攻） | client 分配 `AHardwareBuffer` BLOB，socket 交接；server 以 `VK_ANDROID_external_memory_android_hardware_buffer`（Magma）或 `EGL_ANDROID_get_native_client_buffer` + `glBufferStorageExternalEXT`（Espryt）导入 | 唯一在两台设备、两个后端上都成立的完整读写档 |
| T1 — server 导出自己的映射 | `VK_KHR_external_memory_fd` opaque fd | 只有 Adreno 的 Vulkan 路径可用；每次存储定义一次 round trip |
| T3 — host pointer 导入 | `VK_EXT_external_memory_host` | Adreno 无扩展；Mali 只读 |
| T2 — 拒绝（永久正确回退） | `AcquirePersistentMap` 返回 `nullptr` | 此档下 client 侧推送强制；**P5 split 使用的档**（`MOBILEGL_IPC_ADOPT_TIER=2`） |

`map-persistent-roundtrips`（`mpr`）数的是**每一次 `MapPersistent` 发射，铸成还是拒绝都算**——按"真正发生的往返"定义在 monolith 下按构造恒为 0，永远红不了；按获取尝试计数两种模式下数字相同、在 monolith 下就可断言（门：`StorageBufferRegrowScenario`、`LargeArenaAdoptionScenario`）。

**client 侧 persistent map 推送三件套**（T2 档强制，P5 落地）：(1) 不做 map/unmap 命令对，`ResourceRespecify/SubData` 的 payload 带 `hasLiveHostWrites` 位；(2) 块粒度脏块推送：tracker 维护 `m_livePersistentMaps`，在每个 validate 点对本次操作可达的每个这类 buffer 按 `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`（默认 64）切块发送——Phase 1 保守版（整个 mapped span 当脏），Phase 2 精确版（shadow-in-shm 的块脏位）；(3) 门 `PersistentCoherentMapScenario`（map PERSISTENT|WRITE|COHERENT、写、不做任何其它 GL 调用、draw、readback 校验）。R-1（§17.1）的 apply-role producer guard 保证 server 不会重入 client 的 persistent-map producer。`MOBILEGL_COHERENT_AS_FLUSH` 在拆分模式下照常生效。

## 13. 回读、roundtrip 清单与验证

### 13.1 稳态零 roundtrip 与不可避免的阻塞点

零 round trip：全部 draw/clear/blit/copy/dispatch/barrier/XFB 跨度/bind/CSO/`set_*`/上传/`present`；全部 caps 站点；`glGetError`/`glFinish`/`glFlush`；fence 与 query 的创建及非阻塞轮询；`glGetTexImage`（DirectGLES）；`glReadPixels` → pack PBO（fire-and-forget + client 侧 `MarkGpuWritten`，P6+）；`glEndTransformFeedback`；`eglSwapBuffers`；`*IndirectCount`；restart/multi-draw。**TCP 终局（2026-09-22）**：每一条 `kWaitReply` 行在跨机链路上就是一次 RTT——`ResourceCreate`、`SetTextureParams`、纹理半边的 `ResourceSubData` 都在此列（`CONTRACT-P5E.md` §2.5）——所以逐帧 `kWaitReply` 次数是 P6.5 的必测数，与本节的 roundtrip 计数器同源；MC 加载区块那一阵的几百次创建在 1–5 ms 一次下是可感知的卡顿，要么减少这一列，要么让它们异步（P9）。

不可避免（全部罕见）：握手一次；surface 生命周期与首次 `MakeCurrent`+`InitCapabilities` 每 surface 至多一次；`glReadPixels` → 客户内存（像素进 `SEG_REPLY`）；`glGetTexImage`（DirectVulkan）；GPU-write pending 的 buffer 首次 CPU 读；`glClientWaitSync(timeout>0)`、`GL_QUERY_RESULT` 未完成、`glBeginConditionalRender`（谓词只解析一次）；`glBufferStorage` 的 ack；`MapPersistent`（仅 T1）；纹理拉取（§8.4）；client 侧索引扫描当源 EBO 在 pending 集里；ring/stage 耗尽与 present credit。

验收措辞：在全部 trace 用例上发布逐用例的 roundtrip 计数器、纹理拉取计数器、索引镜像字节数；零 timeout 轮询循环必须在有界时间内退出。

### 13.2 五部分验证门（取代 monolith 的字节一致门）

1. **接口纯度三道门**：**A 门 include 图**（disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除，`scripts/check_include_closure.py`）；**B 门符号** `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门** `grep -c 'pGLContext' MG_Backend/` == 0。外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。
2. **语义影子比对 `MOBILEGL_PIPE_VERIFY=1`**——决定性的一条：两套状态模型活在同一地址空间，tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 比对器逐字段每 draw 比对，打印第一个分歧字段与 draw 序号。第三种 CI 模式（`build-linux-verify` / `integration-verify` / `retrace-verify`），~5–10× 慢，永不出货。**保留模式**：消费即清的组（纹理 dirty rect）verify 时保留清除前的集合并比对发射出去的 `(UnionBox, RegionCount, Regions[])`。活过 P13。
3. **行为 A/B**：trace 语料在 `{monolith-pull, monolith-push, split}` 下 SSIM ≥ 0.99；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Split.`（DirectVulkan 同）之间逐名相同（G2）；测试名只增不删（G14）；单元全绿；CTS 逐后端 conformance 在 0.5 pp 内（只在五个架构边界与合并 `dev` 之前跑完整 caselist）。`TextureUploadShapeScenario` 把逐纹理逐帧的上传形状录金标比对——+6 ms 悬崖由形状相等把关，SSIM 对它完全不敏感。
4. **性能**：Redmi `2f7cbe2e` reboot-clean、同热窗口、配对 A/B（`devices/pin-verification-2026-09-07.md`），trace replay `--benchmark` 逐帧 JSON（`frameTimesMs[]` + `frameCpuTimesMs[]`）；**指标是逐线程 CPU 时间**，p50 与 p99；tracker 每 draw 的绝对 ns（`DriverBench` T1/T2）与 Blaze3D blend-toggle 微基准单列；关掉 CSO 内容寻址的负面对照。**口径（用户 2026-09-08 起）：对着 pull 臂记录，不作阻塞门**；专门的优化阶段排在路线图之后。
5. **覆盖 + poison + 句柄纪律**：G6 重生成 0 UNMAPPED；`gen_pipe_dirty_surface.py` 0 未映射 mutator；G8 字段归属完备；逐 verb 世代 poison；G7 setter 一致性；`ResidualValueBlock` 棘轮。

两条幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空；**G1**：pull 构建的符号集与 `.text` 字节对 P3a 起的基线恒等（`scripts/symbol_report.py --threshold 0`，每阶段 0 增 / 0 删 / 0 resize / 0 重命名）。**每个门必须能因它存在的理由变红**：每个门都带阴性对照并真跑过一次红（R-16）；公共 GL 看不见的改白盒断言（`PipeApplyPeek`、`PipeSlotPeek`、`BackendCapsPeek`）；`ctest -V` 做拒绝普查是假零（console sink 在发布配置里被编译掉，必须逐用例读自己的日志文件）。

### 13.3 长期语义门：MGPipe recorder（P13）

`MG_Test` 的 mock 后端变成 MGPipe recorder：在一组 fixture 上录下每 draw 的已推送状态，后续构建对比录像。它不依赖 `MG_State`，也给 `tools/trace_replay` 一种记录**已解析**状态的录制格式。它只覆盖推送内容，不覆盖后端对它的解释。

## 14. Present、线程与帧节奏

- `eglSwapBuffers` → `present{frameSerial}` → publish + 敲门铃 → 返回，除非超出 credit。**`present` 与 `eglSwapBuffers` 严格 1:1**：两个后端的帧边界排空只在 `Present` 内发生，批量会饿死它们。
- **`MOBILEGL_IPC_PRESENT_CREDIT` 默认 1**（P10 提出，P5e 落地，range 1..8）：延迟叠加，server 的 `Present` 末尾已在 `vkWaitForFences` 上等 2–3 帧，credit 2 就是端到端 4–5 帧；只有实测吞吐收益能抵掉延迟代价才调高。Magma 从不注册 `SetSwapInterval`，IPC credit 是它唯一的显式限帧器。
- **P5e：credit 是 run-ahead 的唯一稳态背压，字节不是**（`CONTRACT-P5E.md` §2.4）。`MGPPresent::FrameSerial` 从"0，由 server 盖自己的帧数"改成**由 client 铸造、1 起算**——一个 credit 只能在付账方能推进的 id 空间里计量，而付账方是 client；`ServerVerbSink::OnPresent` 在 `Present()` 返回之后调 `ServerSession::ReturnPresentCredit(FrameSerial)`，一次 swap 一个 credit（那个函数自 v1 写下以来一直没有生产调用者，这是它的第一个）。credit 1 = client 在 server 应用并交换第 N 帧时发布第 N+1 帧的记录：一帧重叠，最多一帧附加延迟。等待发生在**编码之前**——先编码再 park 会把一次 SEG_CMD 预留持有整整一帧 server 时间。run-ahead server 上 `FrameSerial == 0` 是 `Fatal{ProtocolCorruption, "Present.FrameSerial"}`。
- 预算（~850 draw + ~0.36 MB pmap/帧）：SEG_CMD 每帧 ~70 KB 对 8 MiB，SEG_STAGE ~1 MB/帧对 32 MiB，SEG_REPLY 不变。跨机 TCP 上这 ~1 MB/帧对的是 30–100 MB/s 的链路而不是内存，60 fps 即 60 MB/s，贴着 Wi-Fi 上限（`ROADMAP.md` 开放问题 19、20）。`WaitForCmdSpace` 与 stage bell 是安全网而不是节奏器，两者退出时都排空 SEG_EVENT。stats 行与 wire ledger 多一个 `credit-waits`：credit 1 下它是 0 就说明 server 从来不在帧的关键路径上，接近帧数就说明它一直是。
- 线程——client：**v1 不加线程**，编码在 GL 线程上直接写 ring；外来线程的 sync/query 读从 `RingControl` 无锁回答，必须发射的少数取 `ctrlMutex` 走 `AuxRequest`（SPSC ring 不允许第二个 producer）。server：`mgl-srv-io`（封帧、`SCM_RIGHTS`、doorbell、CTRL RPC）、`mgl-srv-apply`（**终身持有原生 context**，`MakeCurrent` 的缓存失效风暴变启动期一次性）。
- **核心放置**：全库无亲和性控制。规则：报总 CPU 工作量差（client tracker + encode + decode + server apply vs monolith `PrepareForDraw`）；复用 `ShaderCompilePool` 的大核探测把 `mgl-srv-apply` 绑到大核（`MOBILEGL_IPC_SERVER_AFFINITY`，默认 auto）。
- 拆机顺序：publish + server 排空并 ack → 停 apply 线程 → 关 transport → client 排空 compile pool → `MobileGL::Destroy()` → 释放 sync/query handle（P5 落地形状见 §17.3）。

## 15. 进程、EGL 与平台（P6 / P12）

### 15.1 启动与握手

- server 定位：`MOBILEGL_IPC_SERVER_PATH`（主要）→ `dladdr(&MobileGL::Initialize)` 同目录的 `libMobileGLServer.so`（兜底；集成测试静态链接 `MobileGL_s`、trace replay 的可执行文件不在库目录）。
- **端点种类（P6.5 ct，2026-09-22）**：`fork`（继承 fd，本节其余各条描述的形状）、`unix:<path>`（sm 的独立启动，`SocketTransport::Listen` / `ConnectTo`，两条连接）、`tcp://host:port`（跨机；同机也可用，此时数据面仍可协商为共享段，§11.9）。TCP keepalive / 心跳失败**由传输层报告为挂断**，进 dl 的 device-lost 闩——它不是 apply 超时，dl 的"绝不取自超时"不因此松动。
- 启动：`socketpair(AF_UNIX, SOCK_STREAM)` + `fork`/`execve`，fd 3 = socket。无文件系统 socket 路径、无 abstract namespace。
- **子进程强制 monolith**（修无界 fork 链）：spawn 时构造显式 envp 剔除 `MOBILEGL_TRANSPORT` 与全部 `MOBILEGL_IPC_*`；`mobilegl_server_main` 在到达 `Init()` 之前把 `MG_Config::Transport` 硬置为 `Monolith`。两条都做。`MG_Test/Wire` 测试：进程树只多出恰好一个子进程。
- `Hello{abi, backendType, buildFingerprint, configBlob}` → `Welcome{四个段}`。`configBlob` 转发 client 解析好的 `MG_Config::Features`；`buildFingerprint`（git hash + `PipeCalls.def` hash）不匹配 → `Fatal{AbiMismatch}`；segment 尺寸尚未混入 fingerprint（ID-47 债）。
- `mobilegl_server_main` 声明为 `extern "C" __attribute__((visibility("default")))`（非 Debug 构建设了 hidden visibility）。

### 15.2 Android（spike A 已证，`MEASUREMENTS.md` §1.1）

- 交付链：APK 唯一可 exec 的位置是 `lib/<abi>/`，server 以 `add_executable` + `PREFIX "lib"/SUFFIX ".so"` 构建（真 PIE），`RUNTIME_OUTPUT_DIRECTORY` 指到 AGP 收集原生产物的目录（`MOBILEGL_BUILD_SERVER_SPIKE`）。**两台设备上都已证明**：从 `untrusted_app` 进程 `fork`+`execve` 子进程落在同一域、exit 0、零 avc denial。
- `fork`+`execve` 而非 `posix_spawn`：bionic 从 API 28 才声明后者，minSdk 26。fork 与 execve 之间只做 async-signal-safe 调用。应用进程的 stdout/stderr 是 `/dev/null`：子进程用 marker 文件证明自己活过，exec 被拒的 errno 经 close-on-exec pipe 回传。
- 生产 server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`。一份共享库、两个角色。
- minSdk 26 没有公开 NDK API 能扁平化 `ANativeWindow`。**P5–P11 验证路径无窗口**（pbuffer / `AImageReader`）。~~P12 生产路径：Java `Surface` → Messenger/AIDL → `MobileGLServerService` → JNI `ANativeWindow_fromSurface`~~——按重审 §6 删掉（client 的窗口不跨进程，Rule H）。
- **P12 上屏路径（2026-09-24，子集，[`notes/p12/INTEGRATOR-DECISIONS-P12.md`](notes/p12/INTEGRATOR-DECISIONS-P12.md)）**：窗口是 **server 自己的**。`MobileGLDisplayActivity`（`android:process=":mglwin"`）持有全屏 SurfaceView，并在**自己的进程里**用一个线程跑 TCP server（`mobilegl_server_serve_inprocess`，即 `--serve` 的线程形状：会话串行、会话间重置 latch、后端按进程钉住）；`ServerDisplay` 以 acquire/release/几何上调三个钩子持有该窗口，apply 线程以**租约**使用它。client 设 `MOBILEGL_IPC_SURFACE=server` 后**无头**：`eglCreateWindowSurface(NULL)` 发一帧 `WindowKind::ServerOwned`（token 0，控制修订 3），不发 `SetWindowHandle`，reply 带回窗口真实尺寸；client 的 resize 变成对 server 窗口的几何请求（`setFixedSize` / 0/0 = `setSizeFromLayout`），同样以真实尺寸应答。离屏 service（`MobileGLServerService`，fork-per-session）保留，**一台设备同一时刻一个 server**，一个会话一种表面模式（`SurfaceModeMismatch` 具名拒绝）；无显示的 server 对 ServerOwned 具名拒绝（`NoServerDisplay`）。
- **失窗与会话边界（P12）**：`surfaceDestroyed` 有界（3 s）阻塞，直到 apply 线程——在批内每条记录之前也检查——放掉后端表面并以 `Fatal{ServerWindowLost}` 闩会话，之后才返回；server 继续监听。进程内的会话结束手动做进程退出原本白给的清理：Espryt 丢弃全部 twin 及指向它们的纹理/采样器单元影子、忘掉 swap interval，Magma 丢弃进程全局 renderer；进程内 server 停止时不再 drain 仍在推流的 client 的队列；日志只转发当前会话自己线程的行，且不在日志互斥下发送。
- `HeadlessGL` 的 fork 预检会 fork 一个子进程跑完整 EGL bring-up 然后 `_exit`——拆分模式下那个子进程会 spawn 一个孤儿 server。规则：server 的 EOF 检测**即时且无条件退出**；client 的 socket fd 设成 `_exit` 会确定性关闭的形态；握手有界重试。P6 验收。
- 通用 env 透传 `--env K=V`（`run_android_retrace_local.py` → intent extra → `trace_replay_core.cpp` 在加载库前 `setenv`）已接进 retrace 通道。

### 15.3 Linux / Windows / 崩溃

- Linux/X11：`Window` 是 XID，`nativeToken:u64` 直接送；Wayland 维持不支持；WSL/CI 永不开窗（`EGL_PLATFORM=surfaceless`）。Windows：`HWND` 进 `nativeToken`，Vulkan 可行，WGL/ANGLE 对外进程 HWND 不受支持 → headless only；transport 默认 named pipe。Windows 机器不是正确性门。macOS 不拆分。**2026-09-22**：TCP 终局下 Windows / 桌面 client 成为真实形态，`pipe:` / `unix:` 的 Windows 具名拒绝由 TCP 替代；"不是正确性门"与"macOS 不拆分"在 client 平台范围（`ROADMAP.md` 开放问题 22）定下前维持。
- server 死：client 读到 EOF/EPIPE → device-lost 闩锁（GL 调用 no-op、`eglSwapBuffers` 返回 `EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus` 返回 `GL_UNKNOWN_CONTEXT_RESET`）；`MOBILEGL_IPC_RESPAWN=1` 时重启并全量重推（默认关）。client 死：server 读到 EOF → 立即销毁原生 context 并退出；`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作最后保险。

## 16. 构建布局

```
MobileGL/MG_Pipe/                  永远进构建（monolith 的架构）
  PipeCalls.def PipeFields.def Coverage.def FieldOwnership.def FillPoints.def DirtySurface.def
  MGPipe.h MGPipeTypes.h MGPipeValueTypes.h MGPipeHandles.h MGPipeHostSpan.h MGPipeCallbacks.h
  MGPipeRenderStateSpans.{h,cpp} PipeApply.{h,cpp} PipeRoute.{h,cpp} PipeMutation.h PipeInputsSwitch.h
  generated/ PipeTables PipeThunks PipeWire PipeVerify PipeFilled PipeCoverage PipeSpanTable PipeFieldOwnership PipeFillPoints (.inc)
MobileGL/MG_Impl/Pipe/             Tracker.h PipeFill.{h,cpp} SlotAllocator CsoCache CompositeResolver ResourceTracker
                                   SetHashSuppressor {VertexInput,Framebuffer,Texture,Sampler,Image,Program}Emit.h
MobileGL/MG_Backend/MGPipe/        PipeInputs.{h,cpp}
MobileGL/MG_Remote/                仅 MOBILEGL_BUILD_DISAGGREGATED
  CONTRACT-P5.md CONTRACT-P5B.md CapsCodec.{h,cpp}
  Protocol/  protocol.fbs generated/protocol_generated.h mg_protocol_base.h
  Transport/ ITransport InProcessTransport Framing Ring SessionRings ReplySlot EventRing RoleMemory
             ShmSegment(+Posix/Win32) FdPassing Doorbell WireLog          SocketTransport [P6]
  Wire/      PipeWireCodec.{h,cpp}（记录编解码、WireVerbSink）
  Client/    BackendObject_Remote ClientSession EmitTables WireTables SlotCaps CapsMirror
             PersistentMapTracker GpuWritePending
  Server/    PipeApplier（ServerVerbSink）ServerLoop ServerSession StagedShadow   IndexHostMirror ServerMain [P6/P8]
```

- CMake option：`MOBILEGL_BUILD_DISAGGREGATED`（默认 OFF）追加 `MG_Remote/**` 并定义 `-DMOBILEGL_BUILD_DISAGGREGATED=1`；OFF 时 `MG_Config::Transport` 是 `constexpr Monolith`。`MOBILEGL_BUILD_DISAGGREGATED_INPROC` 隐含前者并加角色隔离：MGPipe 让需要角色分身的进程全局从四个降到两个（pipe 表与 `pActiveBackendObject`），两个角色通过 apply thread 与控制 mailbox 分开，而不是给 1494 个 `pGLContext->` 读点加 TLS。`3rdparty/flatbuffers/include` 缺失时把 option 强制回 OFF。
- `MobileGLServer`（P6）：桌面 `add_executable` 链接 `MobileGL_s`；Android 改名 `lib*.so` 链接共享 `MobileGL`。
- `MOBILEGL_TRANSPORT = monolith | inproc | spawn | unix:<path> | pipe:<name>`（`ConfigLoader.cpp` 解析；P5 只接受前两个，其余具名拒绝并回落 monolith），免费换来 ctest `ENVIRONMENT` 变体、trace-replay 的 `setenv` 块、FCL 用户 env、plugin APK 的开关表。**split build 不设 `MOBILEGL_TRANSPORT=inproc` 时是 monolith control arm**。
- 测试接线陷阱：ctest `ENVIRONMENT` 是替换而非追加、`;` 必须转义，必须用 `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` 构造；`add_trace_replay_test` 加 `SPLIT` 后缀并 `-DTRACE_TRANSPORT=`；每条 `DirectGLES.Split.*` 条目带独立 `MOBILEGL_LOG_FILE_PATH`。
- CI（`.github/workflows/test.yml`）：`pipe-gates`（G1–G8 重生成 + diff、生成器 self-test、符号报告门、`MG_Backend`/`MG_State` 禁止 stdio 插桩、dirty-surface、字段归属、R-16 阴性对照 smoke、G5 两族、文档引用 lint）；`flatc-check`；`include-graph-check`；`monolith-symbol-report`；`build-linux` / `build-linux-verify` / `build-linux-split`；`integration` / `integration-verify` / `integration-split`（含 `scripts/ci/split_negative_controls.sh` 的 E1/E3(a)/E2 硬门，broad inproc 车道只记录普查）；`retrace` / `retrace-verify` / `retrace-split`。`apk.yml` 构建 pull / push / split 三份 APK 并在 AVD 上 retrace。两份 workflow 里的 `feat/disaggregated` 触发器是 **TEMPORARY**，合入 dev 前移除。

## 17. P5 / P5b 落地形状：lockstep `inproc`、reply、apply-thread server 与 verb 迁移

### 17.1 verb barrier 与诚实的同地址空间传输

P5 的 `inproc` 是真第二线程，但还是 **lockstep**：每条 class-B verb 发射后 client 的 `EmitAndWait` 等到 `appliedSeq == emitSeq`；`MOBILEGL_IPC_VERB_BARRIER=1` 默认开启。理由不是吞吐，而是 BARRIER-PULLED 字段（G8 表里目前 36 行）尚无记录载体；在这些字段退役前让两线程同时跑，server 会读到 client 的"未来值"（R-1）。barrier 是逐族可退役对象，不是 P6 transport 的要求；它的设备代价（barrier tax）见 `MEASUREMENTS.md` §7.4。

同一地址空间不得成为旁路（R-2）：encoder 把 `MGHostSpan::Ptr` 恒写成 `nullptr`，内容 blob 必须带真实 `SEG_STAGE` offset / 非零 size；decoder 对四种形状分别 `Fatal{ProtocolCorruption}`。`MapPersistent` 在 split 恒 decline；`MOBILEGL_IPC_AUDIT=1` 在 retire 后把 staging 填 `0xDD`，让跨 applier 返回持针的实现下一次读取时可见地失败（R-11：任何 widened read 先过 `RequireStagedCoverageForPendingRanges`，否则 `Fatal{StageSnapshotTooNarrow}`）。apply 角色不得进入 client 的 persistent-map producer（r1）。

reply mailbox 以**记录序号作为 slot id**（R-3）：slot header 是 `{Seq, Status, Size}`，acceptance 的 Bool 答案与 `MapPersistent` decline 都在既有 verb wait 内读取；`Status=ERROR` 一律 `Fatal{ReplyError}`，未知 status 是 `Fatal{ReplyStatusInvalid}`；读到 reply 之前先由 `appliedSeq` 证明该记录已离开 applier（R-5）。P9 才把这套同步 mailbox 推广成异步池。等待预算：普通 verb / 容量等待 120 s（`Fatal{BarrierTimeout}`，`ClientSession.cpp` 的 `kBarrierTimeoutMs`）；`ClientWaitSync` 的 applied/reply 预算 = ceil(timeout ns / 1e6) + 120 000 ms，有限 chunk 避开溢出。这条看门狗只判**死锁**（记录永不退役），不是性能门：lavapipe/llvmpipe 在 texture-handle 注册（`vkCreateImageView` → `llvmpipe_register_texture`）上可合法阻塞 apply 线程 29.3–39.7 s，故预算从原 30 s 提到 120 s——monolith 走同一路径本就没有这个上限。

### 17.2 71 槽的三类、caps 与 tight readback

client 表（`Client/EmitTables.cpp`）把后端函数表的 71 个槽分成三类，`static_assert` 钉住 **A = 2**（getter 从 caps mirror 本地回答）/ **B = 54**（发射：P5 的 `Clear`、`DrawArrays`、`ReadPixels`、`Blit`、`Present` + P5b 的 d1 19、i1 7、t2 6、f1 11、`BlitNamedFramebuffer`、sync 5）/ **C = 15**（`Fatal{UnmigratedVerb}` 具名拒绝，永不回落到 monolith applier——R-4）。applier 侧 33 条走生成路由表 + 4 条 escape，`PipeCatalogueTest` 钉 37；生成路由的完备性覆盖值调用与 `&MGPipeApply*` 地址取用（R-17）。

能力存活只读 `CapsMirror` 中的 `MGPCaps::CallMask`（R-8）；server 的 consumer mask 由 backend 类型显式设置并以实际 op table 校验，**永不**从进程级 `MGPipeGetResourceOps()` 推导。成功的 `MakeEGLCurrent` / `InitCapabilities` 重新发布 snapshot，client 按 generation 采纳（R-12）；相同 (dpy, draw, read, ctx) tuple 的重复 make-current 不重发（ID-67）。

`ReadPixels` 在线上恒为 **tight**：server 临时设 neutral pack，只向 reply 写 `width*height*bytesPerPixel`；client 用自己持有的 pack state scatter（含 `PACK_SWAP_BYTES` 的 component / packed-word swap，r1）。绑定 PACK PBO 时在 client 侧 `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}`；reply 单槽 payload 上限 `2 MiB - 16`，更大 readback 的 carrier 留给 P6+（ID-47/57）。

### 17.3 server 角色、shadow 与退出

`ServerLoop` 的 `mgl-srv-apply` 是 native context 的终身 owner。`ServerMakeEGLCurrent` 对 tuple **只 bind 一次**，client release 只记账、绝不让 apply thread native-unbind（ID-54）。罕见 EGL 操作经 caller-serialised one-slot control mailbox 进 apply thread；十二个 `Server*` forwarder 是唯一缝。有 backend 却无 thread 时 `Fatal{ApplyThreadNotRunning}`，不允许回落到 app thread。framebuffer death 的驱动调用也经 mailbox 上 apply thread（r1）。

`PipeApplier::ApplyOne` 依次 stamp verb、decode/apply、清 stamp；只有 `SessionConsumer::ApplyOne` 每条记录把 `appliedSeq` 加一。`StagedShadowStore` 按 resource twin 复制并合并**精确覆盖范围**；`Ops_H_SubData` / flush / respecify 只把 server-owned copy 交给后端；有传输时 `liveHostBase()` 不得回落到 client 的 `MappedData()`（ID-50/52）。

退出顺序：`ShutdownSplitRoles` 先让 client publish 并 bounded-drain，再 shutdown transport / 唤醒 wait，随后 bounded-join apply thread；线程在仍持 context 时 detach decoder、销毁 private backend，最后才销毁 emitter 与 transport。

### 17.4 lane 隔离与阶段边界

每条 `DirectGLES.Split.*` entry 有独立 `MOBILEGL_LOG_FILE_PATH`；E1/E3 控制从被选 entry 的私有文件取 Fatal，且把"选中条目被跳过"判为失败（ID-53/62）。`integration-split` 是缩减路径的硬门；broad inproc `integration-gpu` 车道只记录普查、不设门（ID-65）。

### 17.5 P5b：class-C verb 迁移

- **顺序由动态普查决定**（ID-68）：79 个 trace × 集成车道在 inproc 下的首阻塞统计（`~/w7/notes/p6/census-classC.md`），所有 Minecraft trace 首阻塞 = `DrawElements` → Minecraft 优先，四包并行（d1 draws、i1 image/compute、t2 XFB/tess、f1 clear/copy/mip）+ 尾包（sync、具名 blit、GLES mip）。
- **一次迁移 = 一个发射器 + 一个 sink 体**：c0b 先给 25 个已测量槽铺 wire 记录（`Wire/PipeWireCodec` 的行）、`WireVerbSink` 分派与 `ServerVerbSink` 具名 Fatal stub；包只在 `EmitTables.cpp` 把槽从 C 挪到 B、在 `PipeApplier.cpp` 填 sink 体（apply 线程、只从记录与 server 状态取输入）。这些槽到达的是 sink，不是 `MGPipeApply*`。
- **规则 D**：记录把 GL 调用逐字带在句柄旁边（draw 的 mode/count/type/offset/instance/base 字段、clear 的值类与 drawbuffer 下标、named-framebuffer 的句柄形式），后端保留它的 barrier-pulled 读；未测量的形式**按名拒绝**（`+RENDERBUFFER`、`+UNBOUND` 等）而不是猜。
- **draw 家族**：19 个索引 / 实例 / multi-draw / indirect 槽全部下沉到 `draw_vbo`；用户索引 span 的 shape/extent 门在 encoder / decoder / sink 三端共用（单 range、宽度 1/2/4、`Uint64(Count) × IndexSize ≤ Size`）。
- **sync**：`FenceSync` / `ClientWaitSync` / `GetSyncStatus` / `WaitSync` / `DeleteSync` 五条现有 opcode 接线，client 铸造 Fence 句柄，wire 只过 `{slot,gen}`，native fence 归 apply 线程；顺带修掉 `MGL_BACKEND_SLOT_PTR_LOCAL` 对 split 恒返回 nullptr。
- **具名 blit**：`BlitNamedFramebuffer` 经作用域化 client-shadow read/draw 绑定发布，降为现有 bound backend 调用，退出恢复公开绑定。**GLES mip storage**：前端先定义并发布层级，server 只验证 applier descriptor 的 Levels/extent；registry 身份解析只 Find 不 mint。
- **P5b 留下的 inproc 依赖**：具名 blit 的 scoped client binding + barrier；mip descriptor 的 barrier-held registry 查询；FBO death 的 inproc mailbox。三者跨地址空间都不成立——它们只是 §17.6 清单里的三行。

### 17.6 `inproc` 仍经共享地址空间的访问，与 P5c 的形状（已落地，头 `b88e8487`）

P5 / P5b 的 wire 只覆盖 verb 记录、`SEG_STAGE` blob、reply 与 caps 快照。对 `a79a0af6` 的只读静态审计（`ROADMAP.md` "P5c 计划"，报告 `~/w7/notes/p5c/p5c-audit-v1.md`）列出 59 处仍靠 verb barrier 与同一地址空间才正确的直接访问，最重的四类：**纹理纹素**虽已过 `SEG_STAGE` 但 applier 丢掉指针、Espryt 回读 client 的 `MipmapStorage`（整个纹理家族不在 `FieldOwnership.def`，`rsp` / strict / audit 都看不见）；**反向通道**是 apply 线程直接调进 client `MG_State`（`OnBufferWriteback` 传裸指针，`SEG_EVENT` 已铺好但零 producer，十个回调只装了两个）；**server 用前端 `GetLifetimeId()` 去 client 的 slot 分配器查找甚至铸造句柄**，而记录里其实已带句柄；**Magma** 直接读 client 的 caps 镜像、直接 `MarkGpuWritten`、直接写 client 的 mip 存储。

P5c 的设计决定（六条全部落地，落地形状与偏差以 `MobileGL/MG_Remote/CONTRACT-P5C.md` 为最新权威）：(1) **server 端纹理 staged shadow**——`ApplyTextureUpload` 采纳 `SEG_STAGE` 字节，`SyncMipmapsToBackend` 只读它与描述符，`0xDD` audit 因此覆盖纹理；(2) **`SEG_EVENT` 成为唯一反向通道**——`OnBufferWriteback` 的 `MGPBlobRef` 约定 `Seg = kSegEvent`，`OnGpuWritten` / `OnSurfaceChanged` 走同一 ring，Magma 与 Espryt 共用回调，溢出策略按 §11.7；(3) **sink 与 twin 按记录里的句柄解析**（`GetOrCreate(MGPipeHandle)`），server 永不触 `MGPipeSlots()`；(4) **两条控制记录** `applier_reset`（make-current 边）与 `object_death`（framebuffer 首次有 delete opcode），mailbox 只剩 EGL forwarder 给 P6；(5) **值类 BARRIER-PULLED 行改为每 verb 残余值记录或 server 自答**，对象类行保持 barrier 直到 twin 表落地（P3b/P4b、P7、P8）；(6) **角色守卫**——split 构建有传输时，apply 线程触前端对象表面、GL 线程触 server 状态表面都是 `Fatal{RoleViolation}`；这是 P5c 的出口门，也是 P6 只做传输替换的前提。

落地时的三条结构性补充（契约 §3.1 与 §5.4 的具名豁免）：审计漏了三族无句柄站点——绑定记录 P4b 才发射的 ensure/通告族（`MGPipeReverseAnnouncementScope`）、G6 前端键 twin registry（`MGPipeFrontendKeyedRegistryScope`，P3b/P4b 重键）、Magma 拆除期的隐藏资源；两个 scope 内的只读探测是具名、可 grep、带退役阶段的债，不是守卫的洞。

### 17.7 P5e：client 在 Espryt draw 路径上跑在 apply 前面（权威为 `MobileGL/MG_Remote/CONTRACT-P5E.md`）

P5d 把 `inproc` 变成 client 线程 CPU-bound，帧仍是 `client 工作 +（每 draw）等 apply + 交接`。那个等待存在，是因为 draw 的 apply 仍在解引用 client 拥有的内存：`FieldOwnership.def` 的对象类 BARRIER-PULLED 行，以及两个 P5C 具名豁免 scope 里的前端键 twin registry。**P5e 的一句话：client 发布完就走；server 用记录带的句柄解析每一个对象；它唯一可以读的 client 内存，是一条 client 确实被挡在后面的记录的残余 fill。**

- **barriered 谓词**（§2.1，裁定 3 / ID-83）：`MGPipeBarriered(op, payload, applierState)` = 静态 `WaitClass` 列 ≠ `kWaitNone`，或 XFB span 打开时的 `kCtxVerb`，或带 `kDrawClientArrays` 的 draw。两侧算同一个函数、同一份数据——client 在 emit 时用 `ctx.IsTransformFeedbackActive()`，server 用应用过的 `MGPContextValues` 镜像，而 `set_context_values` 在 ring 上先于 verb，所以两者一致。没有第四条升级；加一条是整合者的裁定加契约里的一行。
- **规则 F**（§0）：**unbarriered 记录的 apply 不读任何 client 内存**——不解引用前端对象、不读 BARRIER-PULLED 行、不探测也不铸造 `MGPipeSlots()`、不持有前端对象的 `SharedPtr`。违反是具名 Fatal，**与 `MOBILEGL_IPC_STRICT_ERRORS` 无关**：那个值按构造就是撕裂或陈旧的，没有"计一笔"的臂。barriered 记录原样保留 P5C 的语义（裁定 4 / ID-84），这正是第一次落地能做小的原因：回读、CopyTex、`GetTexImage`、`set_storage_block_binding`、XFB 都留在 barriered 且拖后。
- **`gPipeInputs` 变成 server 角色内存**（§3）：unbarriered verb 的 validate 只跑第 2 步（tracker 走查）和第 3 步（发射器）——它们产出记录，这正是 unbarriered verb 被允许交给 server 的全部——序号自增、verb 戳、撤回 server 戳与整个第 4 步都不跑。`MGPipeInputUnfreshRead` 的计数器在 unbarriered 记录下变成**无条件中止**，这是把 `integration-split-strict` 从"预期红"变成硬绿门的那一处（§7）。
- **run-ahead 的臂**：`RunAheadArmed()` = `m_barrierArmed && Ipc.RunAhead && Caps().HasCap(kCapRunAheadApply)`，在 `Start` 之后第一次 caps 采纳时**锁存**，此后只能被关掉、不能被打开——已经不等就发布出去的记录收不回来。Magma 的 `CallMask` 永远不带 bit 10（`MGPipeRunAheadCapBitsFor` 是纯函数，`MagmaPipeIdentityTest` 钉住），Espryt 则由 `MG_Backend/Init.cpp` 的 `kMGPipeP5eRunAheadReady` 一个常量把整相位关在惰性里，直到整合提交把它翻过来。
- **对照臂**：`MOBILEGL_IPC_RUN_AHEAD=0` 是 A/B 的**对照**而不是负控制——server 代码、fill 决定、每一条记录两臂都一样，唯一的差别是 client 等不等；`MOBILEGL_IPC_VERB_BARRIER=0` 保留它自己的意思，并且因为它是那个合取式的第一项，它会直接把 run-ahead 关掉。

## 附 A：开关

CMake：

| 选项 | 默认 | 状态 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 已落地 |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | 已落地（P5，隐含前者） |
| `MOBILEGL_BUILD_SERVER_SPIKE` | OFF（仅 Android） | 已落地（spike A，非出货） |
| `MOBILEGL_PIPE_PUSH` | OFF | 已落地（P1；push / verify / split flavour 都打开它） |
| `MOBILEGL_PIPE_VERIFY` | OFF | 已落地（P1；隐含 `PIPE_PUSH`，编译 `SnapshotFromGLContext()` 与 G4 比对器，永不出货，P13 后保留） |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON | 已落地（P2；OFF 时不编译 pre-handle 臂；`PIPE_PUSH=OFF` 强制回 ON） |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 只服务 `flatc-check` |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON | 计划（P7） |

运行时，MGPipe（`MobileGL/Config.h`、`ConfigLoader.cpp`）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | pull 构建 `0`；push 构建 `0x1fff` | 子系统位图（`MG_Pipe/MGPipe.h`，位永不复用）：`0x01` 渲染状态、`0x02` pixel pack、`0x04` patch、`0x08` attrib defaults、`0x10` residual、`0x20` Espryt slots、`0x40` Magma vertex input（以上 P2 = `0x7f`）、`0x80` resources、`0x100` vertex input（P3a = `0x1ff`）、`0x200` framebuffer、`0x400` 纹理资源、`0x800` sampler、`0x1000` program（P4a = `0x1fff`）。依赖（两侧都拒，客户端不发射）：位 8 要 7；位 11 要 10；位 9 要 10；位 10 要 7 和 11。位 63 `kMGPipeBehaviourNoCsoContentAddressing` 是行为对照不是子系统。`0` = 全 pull，只在 `LEGACY_MEMOS` 编进了 pre-handle 臂时才是有效对照 |
| `MOBILEGL_PIPE_VERIFY` / `_VERIFY_FATAL` / `_VERIFY_CORRUPT` / `_POISON_OMIT` | 0 / 1 / 空 / 空 | 逐 draw 逐字段影子比对；首个分歧是否 abort；篡改字段 / 抽掉填充戳记两个阴性对照（verify 构建才有） |
| `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` | 0 | 阴性对照 C：故意打掉句柄身份让 `HandleRecycle` 的 ABA 臂重现旧污染 |
| `MOBILEGL_PIPE_STATS` / `_STATS_PERIOD` / `_STATS_FILE` | 0 / 120 / 空 | 边界计数器（附 B）；每多少帧一条汇总行；teardown JSON（trace app 从不到达 teardown，设备上只有周期行） |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON | 三态读取，只有显式 falsy 才关 |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | 0（0–4096） | 纹理拉取保留 LRU |
| `MOBILEGL_PIPE_INDEX_MIRROR_MB` | 64（0–4096） | 索引宿主镜像预算（P8） |

运行时，传输与 IPC：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | `inproc` 已落地；`spawn` / `unix:` / `pipe:` 是 P6，当前具名拒绝并回落 monolith |
| `MOBILEGL_IPC_RING_MB` | 8 | `SEG_CMD`；单条记录至多一半 |
| `MOBILEGL_IPC_STAGE_MB` | 32 | `SEG_STAGE`；目标负载 profile 显式 256 |
| `MOBILEGL_IPC_SPIN_US` | 50 | park 前自旋；P5d 三轮起自旋按一次性校准的迭代预算走、稳态不读时钟，`0` = 不自旋直接 park |
| `MOBILEGL_IPC_EVENT_WAIT_MS` | 2000（1–600000） | PH-6（ID-P7-2），只由 server 读（`ServerSpawn.cpp` 放行给 spawn 子进程）：run-ahead 下一条反向事件遇到满的 `SEG_EVENT` 时，server 等 client 腾出空间的**整笔预算**（不是每次 park 的）。到期（client 不排空 / 只涓流不腾够）→ `ReverseChannelForfeit{NotDraining\|TooSlow}`；client 已走（bell 已死——shm 上靠 server bell 挂在控制套接字上的死亡见证——或控制流 EOF）→ `{PeerGone}`、会话被 Stop（如控制流收到畸形帧）→ `{Stopped}`，这两种立即结束等待，所以旋钮可以大于 `ServerLoop::Stop()` 的 5000 ms join。之后该事件及其后全部计数丢弃（`eventDropped`），apply 循环按正常 Stop 退出，控制循环每一轮都查 apply 线程是否还在，会话 exit 0，supervisor 照常欢迎下一连接。取代原先 30000 ms × 两轮后的 `Fatal{EventRingOverflow}`。lockstep 臂（无 `kCapRunAheadApply`，即 Magma）不等待，仍是 P5C 的 Fatal |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` | 64 | persistent-map 推送块粒度；`0` 是 E3(a) 阴性对照 |
| `MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS` | 1 | 推送只发内容变了的块（追踪 buffer 走 mprotect 位图，未追踪走内容哈希）；`0` 恢复全范围推送（A/B 对照） |
| `MOBILEGL_IPC_BATCH_WAITS` | 1 | 值类记录（无 reply slot 的 kCtxState/kCtxCso/kCtxObject）发布即返回，barrier 推迟到下一个拉取类 verb；**`generate_mipmap` 例外**（它的 apply 读 `MGB_CTX->GetActiveTextureUnit()`，规则：只有 apply 不读残余填充字段的记录才可免等）；`0` 恢复 R-1 逐条 barrier（`MOBILEGL_PIPE_VERIFY` 强制 0） |
| `MOBILEGL_IPC_ADOPT_TIER` | 2 | `auto/0/1/2`；P5 split 用 emulated（T2） |
| `MOBILEGL_IPC_VERB_BARRIER` | 1 | 每 verb 等 `appliedSeq == emitSeq`；`0` 只作 E1 阴性对照，并且因为它是 `RunAheadArmed()` 合取式的第一项，它同时把 run-ahead 关掉 |
| `MOBILEGL_IPC_RUN_AHEAD` | 1 | P5e：发布完 unbarriered 记录即返回（§11.6 的 `WaitClass` 列）。**只是合取式的一半**——server 不发布 `kCapRunAheadApply` 时它等于 0 并日志一次；`0` 是 **A/B 对照**（两臂 server 代码与记录完全相同，只差 client 等不等），不是阴性对照。`MOBILEGL_PIPE_VERIFY` 强制 0 |
| `MOBILEGL_IPC_PRESENT_CREDIT` | 1 | P5e：client 可以在手的 present 数，range 1..8。credit 1 = 一帧重叠、最多一帧附加延迟；2 是设备测量臂。stats 行的 `credit-waits` 是它真正阻塞的次数 |
| `MOBILEGL_IPC_STRICT_ERRORS` | 0 | BARRIER-PULLED residual input 提升为具名 Fatal |
| `MOBILEGL_IPC_AUDIT` | 0 | retire 后 `0xDD` 填退休 staging |
| `MOBILEGL_IPC_SERVER_AFFINITY` | `auto` | apply 线程亲和性；日志报告内核实际采纳的掩码（Redmi 的内核对 app 线程一律忽略 `sched_setaffinity`，解析为 0xff） |
| `MOBILEGL_IPC_SERVER_PATH` | 空 | P6 消费 |

P6+ 生效：`MOBILEGL_IPC_POLL_ESCALATE`、shadow shm、`MOBILEGL_IPC_RESPAWN`、`MOBILEGL_IPC_IDLE_EXIT_S`（`MOBILEGL_IPC_PRESENT_CREDIT` 已在 P5e 生效，见上表）。显式不设立：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）、`MOBILEGL_IPC_VALIDATE_SERVER`（server 没有 `MG_Impl` 校验器）。

## 附 B：边界计数器（`MobileGL/MG_Util/Metrics/PipeStats.h`）

关闭时每站点一次全局 load + 一条永不命中的分支。字节类：`stage-buffer`、`stage-texture`、`stage-ubo-global`、`stage-ubo-named`（只有 Magma 贡献，D-B8）、`stage-vertex-client`、`stage-index-client`、`stage-indirect-cmd`、`persistent-map-push`（`pmap`）、`residual-value-block`（`resid`）、`cso-blob-bytes`（`csob-blob`）。调用类：`draws`、`accessor-calls`（约 10 个热入口的**静态**计数，是下界——把读点搬走与把工作去掉在它上面长得一样，判性能只看 CPU 时间序列与 memo 门）、`texture-upload-emissions/box/rect/jobs`、`csom`/`csob`（CSO 铸造 / 绑定）、`mpr`、`trp`（纹理重铸拉取）、`rsp`（residual reads，只是有 stamp 读点的下界）、`maxrec`/`ringwraps`/`ringpads`/`ringwaits`（run totals）。六个 memo 门（`ers`/`etl`/`eub`、`mfp`/`mpm`/`mdt`）各计 hit/miss。每 `MOBILEGL_PIPE_STATS_PERIOD` 帧一条 `MGPipe stats:` 汇总行；站点清单——哪些路径**没有**接线——写在 `PipeStats.cpp` 头部，那份清单是契约。逐 dirty 位触发率与每 draw payload 直方图已实现但从未接进汇总行（未测）。
