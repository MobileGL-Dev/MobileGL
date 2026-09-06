# MGPipe 设计与架构

> 本文描述**已决定**的设计。每条决定附一行理由；数字凡有实测的取实测（见 `MEASUREMENTS.md`）。落地状态以 `feat/disaggregated@458ccde1` 为准：标注"P0 已落地"的是树里的代码，其余是后续阶段要实现的形状（阶段号见 `ROADMAP.md`）。

## 1. 边界

### 1.1 一句话

`MG_Backend` 已经是一台贴着目标 API 的状态机（Espryt 有逐字节的渲染状态镜像、6 个 twin registry、三条 persistent ring；Magma 有 `SetupDrawSnapshot`、pipeline memo、5 个 `Vk*Manager`）。它缺的不是状态，而是一份"我被告知了什么"的显式声明。MGPipe 就是那份声明：前端在每条 verb 之前把变化**推**过去，后端不再拉 `MG_State::pGLContext`。server 进程因此只装 `MG_Backend` + MGPipe 对象表，不链接 `MG_State`、`MG_Impl`、glslang。

接口不是从 gallium 自顶向下设计的，而是从两个后端自己维护的关键结构反推出来的：`SetupDrawSnapshot` 的字段并集 → `set_*` 组；`DrawTextureSyncKeys` → `set_sampler_views`+`create_sampler_view`+`set_texture_params`；`ResolvedDrawBuffers`/`ResolvedVertexBindings` → vertex elements 三件；`g_syncedRenderStateParameters` → render-state CSO；`UnpackStagingBlock` → `MGPSubData` 的 region 形状；`BufferBackendOps`（7 个 hook，注释自称 `pipe_context` 类比）→ `resource_*` 全族。gallium 是目的地（词汇可读、可迁移），不是推导前提；与 gallium 的十条偏离见 §3.5。

### 1.2 两张函数指针表

`MGPipeScreen`（share-group 作用域：caps、resource、persistent map、fence）与 `MGPipeContext`（其余全部：query 命名空间、CSO、`set_*`、对象操作、verb），由 `PipeCalls.def` 经 G1 生成（`MG_Pipe/generated/PipeTables.inc`）。**P0 已落地。**

- 函数指针 struct 而非虚基类：边界今天就是函数指针 struct（`gBackendFunctionsTable`）；**null 项已经表示"未实现，前端回退"**，正好就是"这个子系统还没迁移，继续拉取"；`MG_Test` 已用替换整张表的方式 mock 后端。
- 两张表从第一天分开：事后拆分意味着给记录重新编号。v1 只有一个 screen、一个 context、一条 flow（`pGLContext` 是进程全局，share group 全库无人读取）。
- EGL 生命周期 8 项与 caps 面留在 `pActiveBackendObject` 的虚函数上（罕见路径）。

### 1.3 三种形态，一份后端

| 形态 | 表里装的是什么 | 用途 |
|---|---|---|
| `monolith`（默认） | backend 自己的函数；`MGPipeCallbacks` 是对 `MG_State` 的直调；`MGHostSpan.Ptr` 指向 client shadow（零新增拷贝） | 出货 |
| `inproc` | 发射器 → 同进程第二个线程上的 applier | CI 形态；同时就是 monolith 的**渲染线程**（把 `PrepareForDraw` 与驱动调用搬离 GL 线程，是本项目手上最大的单一 CPU 杠杆） |
| `spawn` | 发射器 → SPSC shm ring → 另一个进程的 applier → 同一批 backend 函数 | 两进程出货形态 |

唯一 hook 点是 `MG_Backend::Init()`（`MG_Backend/Init.cpp`）里一个 `#if MOBILEGL_BUILD_DISAGGREGATED` 分支：`MG_Config::Transport != Monolith` 时装 `MG_Remote::BackendObject_Remote`，否则走今天的 `switch`。下游 `MG_Impl` 的边界调用点零 `#ifdef`。（分支在 P5 落地；P0 的 `Init.cpp` 尚未含它。）

## 2. 对象模型

### 2.1 句柄 = `{slot, gen}`（P0 已落地，`MG_Pipe/MGPipeHandles.h`）

- 8 字节 POD，按值走寄存器对；**client 铸造，server 永不返回句柄** → 整份目录零创建 round trip（对 gallium 的偏离 D1）。
- slot 稠密、**按 kind 分配**（free list + 高水位），server 对象表是数组而非哈希表。与 `IndexGenerator` 无关——后者的 LIFO 名字复用正是句柄要关掉的问题。
- `gen` 只在 slot 复用时 ++，不在 respecify 时 ++；同一 slot 复用 2³² 次才回绕（1000 fps 逐帧复用约 50 天），debug 分配器断言回绕。
- kind：`Buffer, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context`。
- 保留句柄：`{0,0}` = null；`{0,1}` of `Framebuffer` = 默认帧缓冲（退役 Espryt 四处 `pDefaultFramebufferInfo->defaultFBO` 身份比较）；`ShaderCso` slot 空间的高 1/16 保留给 program pipeline 合成体（`MobileGL/MG_Pipe/MGPipeHandles.h:88-90`）。
- GL name 只以 `GlNameForDiag` 出现在 `MGPResourceDesc` 里，永不做身份、永不进 memo 键或 content hash；`GetLifetimeId()` 留在 client 作 tracker 自己的身份，client 维护 `lifetimeId → slot`。

### 2.2 两种世代，严格分开

| | 拥有者 | 回答 | 过线 |
|---|---|---|---|
| `MGPipeHandle::Gen` | client | "还是同一个 GL 对象吗？" | 是 |
| `MGGen`（`g_bufferMutationEpoch`、`m_textureImageEpoch`、`m_cacheStructureEpoch` 等 12 个后端纪元） | server | "我自己是否重铸了驱动对象？" | **永不**；server→client 只以纹理拉取请求出现（§8.4） |

规范：任何 MGPipe 调用不得要求 client 提供或知晓 `MGGen`；反过来，client 的回绕 `Uint16` 版本计数器永远不是新鲜度的唯一证明——过线时要么加宽、要么与 `{slot, gen}` 同行。

### 2.3 CSO 与可变对象

| 类别 | 形态 | 对应后端已有缓存 |
|---|---|---|
| `VertexElementsCso` | create/bind/delete | `VertexInputStateFactory::m_cache` |
| `SamplerCso` | create/delete + `bind_sampler_states` | `VkSamplerManager::m_samplers`、`BackendSamplerObject` |
| `SamplerViewCso` | create/delete + `set_sampler_views` | `TextureResource::{perMipViews,…}`、`SyncTextureViewToBackend` |
| `ShaderCso` | create/bind/delete + server 侧惰性特化 | `ProgramFactory::m_cache`、`BackendProgramObjectImpl` |
| `RenderStateCso` | create/bind/delete，身份 = pipeline 子集 | Espryt 值镜像；Magma `ComputePipelineStateHash` |
| Buffer / Texture / Renderbuffer | create / respecify / subdata / destroy | 各自 twin |
| Framebuffer / Xfb | per-context 身份 + `set_*` payload | `BackendFramebufferObject`、`m_xfbCounterSlotByObject` |

CSO 在 client 侧内容寻址（Mesa `cso_cache` 先例）：每类一张 `ska::flat_hash_map<xxHash, MGPipeHandle>`，容量上限 render-state 64 / vertex-elements 1024 / sampler 256 / sampler-view 4096 / shader 跟随 `ProgramObject` 生命周期，LRU 淘汰时发 `delete_*`。两个不同 program 设置了相同状态时 server 零状态转换。

## 3. 调用目录（P0 已落地）

### 3.1 单一真相源

`MobileGL/MG_Pipe/PipeCalls.def`：一行一个调用 `X(Name, PayloadStruct, Class, Flags)`。**线上 opcode 就是行在文件里的位置**（1-based），所以目录必须是唯一记录的集合，新调用只能**追加**到文件末尾、退役的调用保留槽位。`MGP_CALL_LIST_DOCUMENTED_COUNT = 71`（`MobileGL/MG_Pipe/PipeCalls.def:69`）由 `MG_Test/Pipe/PipeCatalogueTest.cpp` 钉住。

七个生成器（`scripts/gen_pipe.py`，产物提交进树，CI `pipe-gates` 重生成并 `git diff --exit-code`）：

| | 产物 | 内容 |
|---|---|---|
| G1 | `PipeTables.inc` | 两张函数指针表 |
| G2 | `PipeThunks.inc` | monolith 直调 thunk `MGP_<Name>()`，`MG_Impl` 的约 93 个 `gBackendFunctionsTable.GL.*` 站点逐名改到它上面 |
| G3 | `PipeWire.inc` | wire 记录 + 每种一条尺寸 `static_assert` + applier 分发前的运行期边界检查 → `Fatal{ProtocolCorruption}` |
| G4 | `PipeVerify.inc` | `MOBILEGL_PIPE_VERIFY` 的逐字段比对器（字段表来自 `PipeFields.def`；浮点按位比较，NaN patch level 不会误报） |
| G5 | `PipeFilled.inc` | `PipeInputs` 字段 id（61 个）与逐 verb 世代 poison |
| G6 | `PipeCoverage.inc` | 477 行后端读点清单 → MGPipe 调用的映射（`Coverage.def` 手工维护一半）：299 → 调用、5 client 自答、6 反向通道、167 结构性句柄、**0 UNMAPPED** |
| G7 | `PipeSpanTable.inc` | render-state pipeline 子集的成员名表（24 个，取自 `ComputePipelineStateHash` 今天哈希的字段，`scripts/gen_pipe.py:67-92`）；带 `offsetof` 的 chunk 表与 setter 一致性测试在 P2 |

### 3.2 分组与计数

| Class | 条 | 内容 |
|---|---|---|
| `kScreen` | 11 | `GetCaps`(R)、`ResourceCreate/Respecify/Destroy`、`MapPersistent`(R,O)/`UnmapPersistent`(O)、`FenceCreate/Status(R)/Wait(R)/Destroy`、追加的 `FenceWaitServer`（`glWaitSync`，GPU 侧等待） |
| `kCtxQuery` | 8 | `QueryCreate/Begin/End/Available(R)/Result(R)/Destroy`、追加的 `QueryTimestamp`(R)（`glGetInteger64v(GL_TIMESTAMP)`）与 `QueryCounter`（`glQueryCounter`） |
| `kCtxCso` | 13 | create/delete × {render state, vertex elements, sampler, sampler view, shader} + bind × {render state, vertex elements, shader}；sampler 与 sampler view 的绑定是下一组的批量调用 |
| `kCtxState` | 17 | `SetDynamicState`(B)、`SetFramebufferState`、`SetVertexBuffers`(V)、`SetIndexBuffer`、`SetIndirectBuffers`、`SetSamplerViews`(V)、`BindSamplerStates`(V)、`SetShaderImages`(V)、`SetShaderBuffers`(V,H)、`SetStreamOutputTargets`(V)、`SetGlobalConstants`(B)、`SetVertexAttribDefaults`(V)、`SetPixelPackState`、`SetPatchState`、`SetDrawProgram`、`SetDispatchProgram`、迁移期临时的 `SetResidualValueState`(B) |
| `kCtxObject` | 9 | 按资源寻址：`SetTextureParams`、`ResourceSubData`(B,V)、`BufferSubDataResident`(B,O)、`ResourceSubDataComplete`、`ResourceFlushRange`、`ResourceReadback`(R)、`ResourceCopyRegion`、`GenerateMipmap`、`GetTextureImage`(R) |
| `kCtxVerb` | 13 | 按上下文寻址：`Blit`、`Clear`、`ReadPixels`(R)、`DrawVbo`(H,V)、`LaunchGrid`、`MemoryBarrier`、`Begin/End/Pause/ResumeStreamOutput`、`Flush`、`Present`、`SetSwapInterval`(O) |

Flags：`kNeedsAck`（调用方等 server 确认；目录里目前无条目携带，见 §8.3）、`kHasBlob`(B)、`kVarTail`(V)、`kHostSpan`(H)、`kReplySlot`(R，答进 `MGPReplySlot`，永不阻塞)、`kOptional`(O，后端表里可为 null：Magma 故意不注册 `BufferSubDataResident` 与 `SetSwapInterval`)。

- 今天 20 个 draw 入口塌成 `DrawVbo` 一条，`MGPDrawRange[]` 就是 `MultiDraw*` 族今天的形状；`Clear` 一条判别式合并 `glClear` + 4 个 `glClearBuffer*` + 4 个 `glClearNamedFramebuffer*`。
- `SetSamplerViews` / `BindSamplerStates` **没有 stage 维度**：MobileGL 的纹理单元空间是合并的（`TextureState::m_textureUnits` 是 192 个单元的一个数组，每 stage 32 只是广告数字），同一单元可被两个 stage 采样；stage 只在目标 API 需要时由 server 从反射归档推导。
- `SetTextureParams` 按资源寻址、与 sampler view 分开（D10）：只作 FBO attachment / image 单元 / `glCopyImageSubData` 端点的纹理没有 sampler view，但 Espryt 对 attachment 也同步纹理参数，且 `RequireImageBindableStorage` 需要在前端参数版本不动时强制重同步。
- `SetIndexBuffer` 独立于 VAO 配置版本（D5）：索引 slot 重绑不移动 VAO config version。
- `SetGlobalConstants` 只覆盖默认 uniform block（D6）：`globalUboScratch` 是 link phase B 的 CPU 数组，没有 GL name、没有 `BufferObject`。

**显式不移植**：`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv`（后两项 P0 已从 `GLFunctionsTable` 删除，`50815a23`；唯一属于后端的带下标答案 `GL_MAX_COMPUTE_WORK_GROUP_COUNT/SIZE` 进 `MGPCaps`，`e8ee7b1a`；`GL_COMPUTE_WORK_GROUP_SIZE` 是前端反射查询）、`ShaderStorageBlockBinding`（折进反射归档）、`set_pixel_unpack_state`（不存在：前端已在 `glTexImage` 时解析压缩格式、强制默认 unpack）、压缩格式概念、`pipe_transfer`。

### 3.3 能力位（`MGPCapBit`）

`kCapViewportArray`、`kCapFloat64VertexAttrib`、`kCapResidentSubData`、`kCapCpuXfbPrimitiveAccounting`、`kCapTimerQuery`、`kCapOcclusionQuery`、`kCapXfbPrimitivesQuery`、`kCapNeedsHostIndexBytes`（server 做 restart 重写 / multi-draw 展平，split 下开启索引宿主镜像，§10.3）、`kCapNeedsHostUboBytes`（server 把具名 UBO 打进自己的 ring，需要 `SetShaderBuffers` 的 host payload）。`CallMask` 取代"槽位是否为 null"这个隐式能力探测。

不存在 `kCapPrimitiveRestart` / `kCapMultiDraw*` 一类"归属开关"（D-B7）：`ResolveTierForBatch` 逐 batch 用 `programReadsDrawID`（转译后 ESSL 的性质，只存在于 server）选档，两个后端都做 restart 重写，所以这类归属不可用 cap 表达。规则一句话：**multi-draw 分档与 restart 重写永远由 server 拥有；client 在 caps 说需要时提供索引字节。**

一个待转显式能力位的现有陷阱：`GL_Drawing.cpp` 把 `EndTransformFeedback` 槽位的非空当作"后端按 GL 顶点序捕获"来跳过 `FixupGsStripCaptureOrder`。MGPipe 下改为显式 `kCapDriverOrderedXfbCapture` 一类的位（P8/P9）。

`MGPCaps` = `DynamicBackendParameters`（整块包含，~90 个标量含六个 compute 限制）+ `CallMask` + 两个 blob（format 能力表、renderer 字符串），握手后一次快照，取代 40 个 `pActiveBackendObject->` 站点与 89 个 caps 读点。

## 4. 记录与 payload 约定（P0 已落地，`MG_Pipe/MGPipeTypes.h`）

- 每个 payload 是平坦 POD、显式 padding、`static_assert` 平凡可复制与**精确尺寸**；**永不含指针**。
- `MGPBlobRef{Offset, Size, Seg}`（24 B）指向 blob 区：monolith 下 `Seg == kMGHostSpanSegNone`、Offset 是调用方 staging arena 内地址；split 下 Seg 命名传输段。
- `MGHostSpan`（32 B，`MG_Pipe/MGPipeHostSpan.h`）是整份接口里**唯一形状随传输而变**的东西：monolith 下 `Ptr` 指向 shadow 或应用内存；split 下 `Ptr == nullptr`、字节在 `Seg/Offset` 命名的 `SEG_STAGE`，或 `Seg == kMGHostSpanSegFromServerIndexMirror`（`MobileGL/MG_Pipe/MGPipeHostSpan.h:26`）表示"字节已在你那边的索引镜像里"。`MGPipeHostBytes()` 是一次可预测分支；split 解析器 `gMGPipeSegmentResolver` 由 `MG_Remote` 安装。它只进变长尾（`DrawVbo` 的用户索引、`SetShaderBuffers` 的具名 UBO 字节），永不内联进定长 payload——VBO 路径（MC/Sodium 的全部 draw）不为它付字节。
- 变长记录（`kVarTail`）= 定长前缀 + 自描述长度的内联尾巴；`kHasBlob` 记录额外校验 `BlobRef` 落在其声明的段内。运行期边界纪律：`SEG_CMD` 是对端并发写入的区域，`static_assert` 管不到运行期损坏，违反一律 `Fatal{ProtocolCorruption}`。
- wire 记录头 `MGPWireRecHeader{Op:u16, Flags:u16, Size:u32}`（8 B），Size 含头、8 字节倍数；**没有逐记录序号字段**——seq 就是记录序数（producer `m_emitSeq++` / consumer `m_applySeq++`）。
- **分块上界 = ring 容量的一半**（`RingProducer::MaxRecordBytes()`）：这是每个 head 偏移都能放下的最大记录（wrap pad 最多花 total−8 字节），超过它的 payload（大 `ResourceSubData`、`CreateShaderState` 归档）由发射器切成多条；ring 对更大的记录直接拒绝（nullptr + `MGLOG_E`）而不是让 producer 等一个永远不够的空闲量。
- `MGPSubData` 的 buffer 半边：`Target == Buffer` 时没有 level 与 box，目的字节范围搭在 `UnionBox.X`（offset）与 `UnionBox.W`（size）上，`MGPipeSetSubDataBufferRange()` 是唯一拼写；单条记录上限 offset 2³¹−1 / size 2³²−1，越界由发射器拆分。

### 4.1 关键 payload

| payload | 尺寸 | 要点 |
|---|---|---|
| `MGPResourceDesc` | 88 | buffer / 全部纹理 target / renderbuffer 一个判别式 create/respecify 形状；`BindMask` 的 `ELEMENT_ARRAY` 位是索引镜像的开关；`ImageBindableHint` 预防性分配 image-bindable 存储；`ViewOf` 是纹理视图的存储属主（server 侧 keep-alive）；`BufferForTexBuffer/BufOffset/BufSize` 实时解析（`kMGPipeWholeBuffer = ~0`）。Renderbuffer 保持独立类（自己的 format-capability target、`ComponentSizes`、twin） |
| `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` | 48 / **12** / 32 | §5.3 |
| `MGPVertexElements` | 40 | blob 同时带解析后的 `VertexAttribute[]` **和** `VertexBufferBindingPoint[]`，缺一不可（pointer 调用的 stride 0 = element size，binding 模型的 stride 0 = 每顶点读同一 element）；`IsLong` 与 `Type == Float64` 分开携带；仅供查询的 `LegacyStride/LegacyPointer` 留在 client |
| `MGPSamplerDesc` | 32 | `SamplerParameters` 逐字节过线**含 `borderColorForm`**（三种 border color 表示永远都被数值填满，没有它后端无法在 `Iiv`/`fv` 或 `VkBorderColor` 家族间选择） |
| `MGPSamplerView` / `MGPTextureParams` | 36 / 32 | view 只带视图限制（min/num level、min/num layer、别名格式）；纹理参数（base/max level、swizzle、depth-stencil mode、LOD 钳、`ForceResync`）挂在纹理对象上 |
| `MGPProgramDesc` | 192 | 逐 stage SPIR-V blob ×6 + 反射归档 blob + `StageMask`/`GlobalUboSize`/`ReservedNumSamplesOffset` + 四个状态字节，§7 |
| `MGPFramebufferState` | 304 | 8 color + depth + stencil + **client 解析后的 `ReadSurface`**（按结构消灭 read-buffer-shared-FBO 缺陷类）；`MGPSurface::InternalFormat` 内联（四个跨对象 mask 推送时零查表）；`ContentHash` 既是 server 的 render-pass memo 键也是 client 的发射抑制器 |
| `MGPSubData` / `MGPSubRegion` | 72 / 40 | §6 |
| `MGPDrawInfo` / `MGPDrawRange` / `MGPDrawIndirect` | **56** / 12 / 40 | `Flags` 门控 `MinIndex/MaxIndex`（只在 client-memory 数组路径算）与 `XfbCpuCapturedVertices`（只在 XFB scatter 路径读）——不是每 draw 都算；`NumDraws` 个 `MGPDrawRange` 在变长尾；用户索引的 `MGHostSpan` 只在 `kDrawHasUserIndices` 时进变长尾；indirect 的 `DrawCount` 由 client 解析，server 永不读 indirect 命令块来数 draw |
| `MGPShaderBuffers` / `MGPBufferRange` | 32 / 24 | range 不内联 host span；`kCapNeedsHostUboBytes` 下 Uniform 类带第二个变长尾 `MGHostSpan[HostSpanCount]`，与 range 数组下标对齐 |
| `MGPPixelPackState` | 28 | 只有 PACK 方向（D5） |
| `MGPPatchState` | 40 | 同时是 shader variant 输入 |
| `MGPClear` | 48 | Whole / Color / Depth / Stencil / DepthStencil 判别式 |
| `MGPGlobalConstants` | 40 | `(ShaderCso, Version)` 键控，每 program 每帧至多一次 |
| `MGPSubDataComplete` | 24 | 纹理拉取的正向终止符，可携带零个 region |
| `ResidualValueBlock` | **1248** | 迁移期 Track V 载体，§9.4 |

每条 `kVarTail` 的 `set_*`（`SetVertexBuffers`、`SetSamplerViews`、`BindSamplerStates`、`SetShaderImages`、`SetShaderBuffers`、`SetStreamOutputTargets`）都带 `ContentHash`——与 `MGPFramebufferState` 同一模式，hash 未变就不发（§5.4）。

## 5. 前端 state tracker（`MG_Impl/Pipe/Tracker`，P2 起）

### 5.1 推送发生在 verb 之前的 validate 时刻，不在 GL setter 里

Blaze3D 每个 batch 用 `glEnable/glDisable(GL_BLEND)` 包住（Espryt 代码自己标它为最热路径），per-setter 推送会把每次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，严格慢于今天。正确形态是 gallium `st_validate_state`。

八个 validate 入口，由 `PipeCalls.def` 的 `kCtxVerb`/`kCtxObject` 条目生成：`ValidateForDraw`（20 个 draw 入口）、`ValidateForDispatch`、`ValidateForClear`、`ValidateForBlitOrCopy`、`ValidateForTextureOp`（GenerateMipmap / CopyTex* / BindImageTexture）、`ValidateForReadback`、`ValidateForXfbSpan`、`ValidateForQuery`。八个而不是四个，因为 `MG_Impl` 用到的 70 个表项里只有约 22 个是 draw/dispatch，其余 ~48 个（clear、blit、copy、回读、barrier、XFB 跨度、query/sync）很多自己就读 `pGLContext`。

**只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）。

### 5.2 dirty 位：值类零新增记账，对象类新增 5 个聚合世代

| dirty 位 | 类 | 快门来源 |
|---|---|---|
| `NEW_RENDER_STATE` / `NEW_PIPELINE_STATE` | 值 | `m_version` / `m_pipelineStateVersion` |
| `NEW_PIXEL_PACK`、`NEW_PATCH_STATE`（`BitwiseEqual`，NaN 合法）、`NEW_VERTEX_ATTRIB_DEFAULTS`、`NEW_VERTEX_ELEMENTS`（VAO config version） | 值 | 既有计数器 |
| `NEW_SHADER`、`NEW_SHADER_BINDINGS`、`NEW_GLOBAL_CONSTANTS` | 值 | link/image-unit/backend-state/block-binding/uniform-write-set/UBO-content 版本 |
| `NEW_VERTEX_BUFFERS` | 对象 | **`VertexArrayState::m_anyVaoAttributeGeneration`**（新增）→ 命中后走 32 属性前缀 |
| `NEW_INDEX_BUFFER` | 对象 | 索引 slot 版本 + 绑定对象 `{slot,gen}` |
| `NEW_FRAMEBUFFER` | 对象 | **`FramebufferState::m_anyAttachmentGeneration`**（新增）+ 对象/slot 版本 → 重算 `ContentHash` |
| `NEW_SAMPLER_VIEWS`、`NEW_SAMPLERS`、`NEW_SHADER_IMAGES` | 对象 | **`TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration`**（新增）+ bind/sampling-resolution generation → 走 `GetMaxTouchedUnit()` 前缀、重算集合 hash |
| `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` | 对象 | **`BufferState::m_anyBufferChangeGeneration`**（新增）→ 走 `GetTouchedBindPointCount()` 前缀 |

五个聚合世代全部落在既有 bump 点上（约 20 行），把对象类组的快门从"每 validate 走查 192 单元 / 84×4 绑定点 / 32 属性 / 40 attachment"降成一次 `Uint64` 比较；对象类不能靠轮询逐对象版本（没有聚合能回答"有没有哪张已绑定纹理动了"，这正是 Magma 不得不用有损 `sampledContentSum` 的原因）。

完整性由 `scripts/gen_pipe_dirty_surface.py` 保证：枚举 `MG_Impl/GLImpl` 里每个 mutator → 必须 bump 的聚合世代，CI 重生成 + `git diff --exit-code`，未映射即失败（P1 起成为门）。**实测规模**：926 次 mutator 调用落在 73 个不同 mutator 上，其中 92 次（7 个 mutator，绝大多数 `RecordError`）位于同函数内也会到达后端的"即时发布点"，其余 834 次由紧随其后的 verb 发布——映射表是 73 条目的问题。

三个回绕 `Uint16` 在 tracker 边界加宽（`m_lastPushed[]` 是 tracker 自己的字段，不改 `MG_State`）；回绕在 tracker 本地无害（多一次重推，永不漏推），且被集合 hash 抑制器吞掉。

### 5.3 渲染状态：整块 blob 过线，身份只取 pipeline 子集，动态状态单独走（D-B1）

```
create_render_state(cso, MGPBlobRef pipelineSubsetChunks)      // 只带 pipeline 子集
bind_render_state(cso, Uint16 version, Uint16 pipelineVersion)  // 稳态 12 B
set_dynamic_state(MGPBlobRef dynamicChunks, Uint16 version)     // 只带动态子集的变化 chunk
```

- 整块的理由：`RenderStateParameters` 是平凡可复制 POD，Espryt 自己 `static_assert` 并做 head/blend/tail 三段 memcmp，**字段顺序承重**（`ScissorBoxWrittenMask`、`ClipDistanceEnabledMask` 故意放在 tail 段）；拆成 blend/depth-stencil/rasterizer 三个 CSO 要手工维护 ~150 字段划分表且无绊线。
- 子集身份的理由：整块内容寻址会让 `glViewport`/`glScissor`/`glBlendColor`/`glClearColor` 每次铸造新 CSO、冲掉 server 的 pipeline memo——`RenderState.h` 记录的那次回归。`RenderState.cpp` 里 viewport/scissor/line-width 族只 `++m_version`，`SET_CAPABILITY` 与 pipeline 相关 setter 才 `BumpVersions()`。
- 动态子集：viewport、scissor、depth range、blend color、line width、polygon offset、stencil ref/write mask、clear 值、sample coverage、hints、point-size 族。
- 划分只写在一处：`MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`（P2）的 chunk 表 + `MGPipeComputePipelineSubsetHash()`，从 Magma 的 `ComputePipelineStateHash` 搬来，client 与两个后端共用；G7 的 `MG_Test` 遍历每个 `RenderState` public setter，断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变`。
- server 侧：每 context 一份 working `RenderStateParameters`（1168 B），`bind` 与 `set_dynamic_state` 各把自己的 chunk 散射进去。**Espryt 的 `SyncRenderState`（693 行）拿到的仍是 `const RenderStateParameters&`，单 `Uint16` 早退、三段 memcmp 一行不动**；Magma 的 pipeline memo 键是 `cso.slot`，动态尾巴仍走 `ApplyDynamicDrawStateTail`。Espryt 的 head/blend/tail 划分（驱动侧增量）与 pipeline/dynamic 划分（线上与身份）是两回事，并存、各有绊线。
- client 取值顺序：`m_pipelineStateVersion` 未变 → 复用上一个 CSO handle，零哈希；变了 → 对 pipeline 子集算 xxHash（~25-30 字，Magma 今天就在算）→ CSO map 探测 → 命中发 12 B bind，未命中发变化 chunk 的 create 再 bind；`m_version` 变而子集未变 → 只发 `set_dynamic_state`（~200 B）。
- `FramebufferSrgb` 与 `DepthClamp` 今天**没有存储**（`glEnable` 被静默吞掉且不报错，六个后端读点恒为 false）；chunk 表冻结前要补真存储并把 `FramebufferSrgb` 划进 pipeline 半边（它改变 attachment/blend 的解释）——待拍板，见 `ROADMAP.md`。

### 5.4 验证不变式、合并与抑制器

规范（D-B3）：**一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态惰性特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间没有顺序要求。** 推荐实现顺序（framebuffer → program → 纹理/sampler/image/buffer/global constants → render state/dynamic → vertex elements/buffers/index/attrib defaults → patch/XFB → verb）只是代码组织，不是契约。退役 Espryt 的 fragColor 重推导 workaround、`g_broadcastMemo*` 与 `ImageUnitFormatsStillMatch` 的机制是惰性特化，不是调用顺序。

`create_shader_state` 从编译池的终止 continuation 发出（不是从 draw），SPIR-V 在首个用到它的 draw 之前到达 server——monolith 拿不到的异步收益。

四条合并规则：整块结构优于逐字段；高水位标记（`GetTouchedBindPointCount`、`GetMaxTouchedUnit`）留在 tracker 走查里，直接就是 `count` 实参；只发 program 解析过的集合（`uniformSamplerOrImageUnitIndex`）；**集合 hash 抑制器**——每条 `kVarTail` `set_*` 在 client 算已解析集合的 xxHash，未变不发。最后一条是从后端搬到 client 的 ~175 行去抖（`UnitBindingsSnapshot`/`PairingsIntact`/`g_fboTextureSyncList` 族）的载体：`GetTextureBindGeneration()` 在冗余重绑时也 bump（MC 26.2 每次纹理单元切换都重绑同一个 sampler），没有抑制器每个 batch 都会重发一条几百字节的变长记录并冲掉 server 的两个 memo。

索引绑定范围在 validate 时刻实时解析（`glBindBufferBase` 之后再 `glBufferData` 是普通应用代码）。

### 5.5 sampler view 在 client 侧解析

GL 是每 unit 每 target 各一个绑定；shader 看见哪一个取决于 sampler uniform 类型、mipmap 完备性（`IsMipmapCompleteForFilter`、`SamplesAsIncompleteTexture`）与 `IsUndefinedDefaultTexture`。gallium 的"每槽一个 view"就是解析后的形态，解析留在 client 并带自己的 memo（~40 行搬迁）。两处后端特定后处理留在 server、作用于已解析集合：Espryt 的 raw-depth-fetch sampler 替换、Magma 的 feedback-loop 检测。

### 5.6 生命周期、共享组、composite program

- `resource_create` 在前端对象构造时发，存储由 `resource_respecify` 惰性定义；`resource_destroy` 在析构时发。三条顺序约束由 payload 表达：view 先于存储属主销毁（`ViewOf` + server keep-alive）、FBO attachment 钉住纹理（surface handle 隐含 keep-alive）、buffer texture 钉住 buffer（`BufferForTexBuffer`，范围实时解析）。
- 共享组：v1 一个 screen、一个 context、一条 flow；`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex` 下发射（顺手让 `ReleaseThread` 与 `SwapInterval` 也取该锁）。
- program pipeline 合成体：`GLContext::GetProgramForDraw()` 今天就完全在前端合成（join、签名查 cache、`Link(true)`）。tracker 拿到 `SharedPtr<ProgramObject>` 推**一个** handle，slot 从 `ShaderCso` 保留高位段分配，pipeline cache 淘汰时释放 slot、`gen++`、发 `delete_shader_state`。合成体从不过线，server 不需要任何"解析后的 draw program"钩子；副带收益是阻塞的 `JoinLinkAndSpirv()` 离开 server 的 draw path。

### 5.7 emulation 的归属

规则：**驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering。** 只有三个"读前端字节的纯 CPU 变换"下放到 client。

| emulation | 归属 | 过线的是什么 |
|---|---|---|
| client 顶点数组（`(first+count-1)*stride+elementSize`） | client | 字节（`MGHostSpan`），永不是指针 |
| 最大索引扫描（`TryComputeMaxIndexFromHostBytes`，唯一无界的应用指针读，只有 client 同时持有两个数组） | client | `MGPDrawInfo::MinIndex/MaxIndex`（flag 门控，`~0` = 未知） |
| client 索引数组 | client | 变长尾里的 `MGHostSpan` |
| `*IndirectCount` 计数解析（从 parameter buffer 的 shadow 读实际 draw 数） | client | 解析后的 `MGPDrawRange[]`（几十字节） |
| primitive-restart 重写（整 EBO 重写，`kMaxRestartRewriteBytes` = 64 MiB） | **server** | 零线上流量：从索引宿主镜像读（§10.3） |
| multi-draw 五档分档 + 展平（`ResolveTierForBatch`，CPU 展平是回退） | **server** | 同上 |
| viewport-array N 遍回放 | server | 无新增：16 组 viewport/scissor/depth-range 已在渲染状态里 |
| fp64 顶点窄化 | server | 原始字节；`IsLong` 与 `Type` 分开过线 |
| image-bindable 存储加宽/拆分 | server | 正向 `ImageBindableHint`；反向纹理拉取 + 终止符 |
| 生成 mipmap 的前端存储 | 拆开：client 分配 level 存储，server 生成 | `MGPMipPlan`；`OnMipLevelsGenerated` 只带形状不带字节 |
| CopyImage shadow 镜像 | client | 只回"拷贝成功"，删掉一整条 server→client 字节通道 |
| XFB CPU 图元计数 | client | `XfbCpuCapturedVertices`（flag 门控）+ `EndStreamOutput` 的 `MGPXfbAccounting` |
| XFB scatter 的 read-modify-write | client | §8.5 |
| 压缩纹理 / pixel unpack 规整 | client | 无 |

**陈旧索引纪律是逐站点表，不是一条笼统规则**（client 侧扫描/解析之前要做的 reconcile 必须逐字复现 monolith 的集合）：

| client 侧动作 | 必须做的 reconcile |
|---|---|
| client 顶点数组范围计算 + 暂存 | 无（应用内存，无 GPU 写者） |
| 最大索引扫描（EBO 源） | `SyncPersistentMappedRange()` **+** `SyncGpuWrites()` |
| 最大索引扫描（client 指针源） | 无 |
| `*IndirectCount` 计数解析 | **只** `SyncPersistentMappedRange()`，不加 `SyncGpuWrites()`——monolith 今天就只做这一个，加了会给 Create/Flywheel 的每 batch 平白加一次 publish-and-wait |
| server 侧 restart 重写 / multi-draw 展平 | server 从镜像读；GPU 写者可见性由 `OnGpuWritten` 收窄集在 server 本地判定 |

前两条 client reconcile 的形态：publish → 等 `appliedSeq` → 排空事件 → 再碰 shadow。门：`ClientArrayAfterComputeWriteScenario`（去掉等待必须看到几何缺失）；`create-indirect` fixture 上 `roundtrips-per-frame` 必须读零（P8）。

## 6. 纹理 subdata 与 dirty 归属

- `glTexSubImage*` 根本不调后端表：全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做，那里跑 `MipmapStorage` 的 96-rect 级联合并与 `summedArea*4 >= unionArea*3` 的 union-box 回退，并在 unpack ring 可用时刻意塌成一个 box——Mali 按**作业数**给上传计价，~100 个精灵 rect 对一个 union box 实测 +6 ms/frame。逐 `glTexSubImage` 发一条记录会精确复现那个形状。
- 因此：client 在自己的 `MipmapStorage` rect 模型里累积，在**下一个 validate / flush 点**把合并后的形状作为**一条** `ResourceSubData` 发出。`MOBILEGL_PIPE_STATS` 单列逐帧发射次数与上传作业数（`TextureUploadEmissions/Box/Rect/Jobs`）。
- **同时携带 union box 与 region 列表，由 server 选上传形状**：决策留在付 GPU 代价的那一侧。实测（`MEASUREMENTS.md`）：vanilla 世界同样 185 次发射，Espryt 的整 box 路径每帧 635 KB 纹素、Magma 的 rect 路径 40 KB，16×。
- `MGPSubRegion` 显式携带 `SrcRowStride/SrcSliceStride`，`MGPSubData::SourceIsVerbatimLevelShadow` 显式携带原来由 `uploadData == mipData` 指针比较回答的问题："这批字节是未经转换的 level shadow 吗"。split 下 client 既不发整 level 也不在 server 留整 level 镜像，指针比较不成立；Espryt 的上传路径改为从描述符取步长，`UNPACK_ROW_LENGTH` 从 `SrcRowStride/bpp` 设。形状照抄已存在的 `UnpackStagingBlock`（ring 路径本来就紧密重打包、不发 `glPixelStorei`）。
- **dirty 归属反转**：client 保留 rect 模型、维护一份发射游标、发射后清自己的标志，server 从不碰 client 的标志。安全，因为 `MG_Impl` 里没有任何 `IsStorageDirty/GetStorageDirtyRects/GetStorageDirtyRegion` 调用点（前端从不读自己的 dirty 状态）。逐 level "server 权威位"与纹理 ack 协议因此不必存在。
- 发射游标按**存储属主**键控 `(storageOwnerHandle, ownerUploadTarget, ownerLevel)`：`TextureObjectView` 把 dirty 查询/清除全部转发给属主并做索引重映射，view 与属主共用同一份 dirty 状态。门：通过 view 上传、经属主采样（及反向），跨 draw 边界各一次。
- 后端真正在 shadow 里写字节的两处——CPU 回退生成 mip（RGB16F/RGB32F）与 `glCopyImageSubData` 目的地镜像——分别由 `OnTextureWriteback` 与"CopyImage 镜像搬到 client"处理。
- Unpack PBO 完全在 client 解析；压缩纹理永不到达后端；`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client（今天就是纯前端操作：借一次 `ReadPixels` 进 CPU scratch 再写 shadow），拆分后恰好是一次阻塞 ReadPixels round trip，脏区按普通 subdata 下发。

## 7. Shader state = SPIR-V + 反射归档

- `CreateShaderState` 的 payload 是逐 stage SPIR-V + 反射归档（`LinkArtifacts` + `SpirvArtifacts` 全结构体），**不是源码**。"server 从源码重新 link"这条路显式关闭：链接真 `ProgramObject` 就链接 glslang。glslang 全在 client，SPIRV-Cross（`TranspileSpirvToEssl`）全在 server，文件级切割。没有 `MOBILEGL_IPC_PROGRAM` 开关、没有 server 侧 compile pool。
- 归档机制：`Visit()` + `sizeof` 绊线（`static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`），一份字段表服务序列化两个方向。必须覆盖四个 `ResourceReflection`（各带 `TypeFacts`）、`uniformSamplerOrImageUnitIndex`、`uniformBlockBinding`、`shaderStorageBlockBinding`（按名字）、`explicitOpaqueUniformBindings`、`xfbVaryings/xfbStrides/xfbPackedStride/xfbNeedsScatteredCapture`、`computeLocalSize`、GS/TCS/TES 事实、`usesReservedNumSamples`、`uniformOffsets`。`XfbVarying` 带两套拼写（GL 名字 + block 实例/成员/元素）。
- **P0.5 硬前置**：反射类型今天声明在 `ProgramObject.h` 里，而它 include `ShaderObject.h`（→ glslang）与 `SpvcSession.h`（→ spirv_reflect）。P0.5 把 `TypeFacts`、`ResourceReflection`、`XfbVarying`、`LinkArtifacts`、`SpirvArtifacts` 抽到 `MG_State/GLState/ProgramState/ProgramArtifacts.h`（只 include `<Includes.h>` 与容器），8 个 includer 靠类内 `using` 别名零改动，加 CI `-H` 闭包断言（已落地，见 `ROADMAP.md` P0.5 行；`DynamicBackendParameters` 留在 `BackendObject.h`，所以闭包门 A 断言的是 `MGPipeValueTypes.h` 而不是 `MGPipeTypes.h`）。同批抽取 `MG_Pipe/MGPipeValueTypes.h`（`MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint`），它不 include `MG_State/GLState` 任何东西；`MGPipeTypes.h` 今天为此临时 include 了 `BackendObject.h` 与 `RenderState.h`（文件头注明为 P0.5 债务）。没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。
- server 侧惰性特化（D-B2）：后端 program 还依赖 8 个额外输入（draw FBO 的 snorm/unorm clamp mask、fragColor 广播数、storage-block 绑定签名、atomic counter 集、活的 image 格式、patch 参数；Magma 另加 FragCoord-Y-flip 的 default-FB 高度与 XFB 布局），`create_shader_state` 发布**制品**，server 在 verb 时刻从已推送状态特化——正是两个后端今天的做法，也是 gallium `st_variant` 的做法。
- 后端 link/compile 失败不需要同步返回：今天只是一行 `MGLOG_E` 加 bind program 0 的空 draw，`GL_LINK_STATUS` 永不撤回，同步查询由 client 从 `ProgramObject` 回答。`OnLog` 逐字复现——由此要求日志按严重级分级（§8.3）。
- Magma 的两个内部 shader（blit、depth-mipmap）烘焙成签进树的 SPIR-V + uniform location + UBO 布局，用一个 `MG_Test` 重跑树内 glslang 逐字节比对守新鲜度（`MOBILEGL_BAKED_INTERNAL_SHADERS`，P7）；顺带把一次 glslang 编译从 monolith 启动路径上删掉。

## 8. 反向通道

### 8.1 `MGPipeCallbacks`（P0 已落地，`MobileGL/MG_Pipe/MGPipeCallbacks.h:27-51`）

十个具名回调 + 一个正向终止符（`ResourceSubDataComplete`），取代今天 95 个调用点 / 17 个方法直接 poke 前端对象。gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求、default-FB 几何这些词汇（Mesa 里两者共享地址空间），具名化是有意偏离（D8）。monolith 下直调，split 下是 `SEG_EVENT` 上的记录。

| 回调 | 取代 |
|---|---|
| `OnGlError(code)` | 6 处 `RecordError`；**必须对命令流有序**，否则 `glGetError` 答错（`glGetError` 本身永远本地） |
| `OnGpuWritten(res, ranges[])` | 6 处 `MarkGpuWritten`：client 在每个 draw/dispatch 发射点**保守自建** pending 集，这是**收窄**通道 |
| `OnBufferWriteback(res, offset, bytes)` | PBO 回读、XFB 捕获；**按操作级批处理**（今天两处逐行循环绝不能变成每扫描线一次 IPC）；必须与 epoch bump 有序 |
| `OnTextureWriteback(res, box, bytes)` | CPU 回退生成 mip 的纹素（唯一生产者） |
| `OnTexturePullRequest(res, target, firstLevel, levelCount, pullSerial)` | §8.4 |
| `OnMipLevelsGenerated(res, base, count)` | 只带形状：monolith 的 `EnsureGenerateMipmapStorageAllocated` 也只 `AllocateStorage` + `MarkStorageDirty(false)` 不填内容，split 行为一致 |
| `OnSurfaceChanged(info)` | `SwapchainObject` 写 `pDefaultFramebufferInfo` 的分层倒置；client 自己合成 default-FB 对象 |
| `OnCapsInvalidated()` | 2 处 `InvalidateCompileEnv` |
| `OnLog(level, text)` | ≤WARN 有损，≥ERROR 无损 + 速率限制 |
| `OnXfbScatterReady(scratch, packedStride, vertices)` | §8.5 |

95 个写回点的其余归属：`MarkStorageDirty` 大多是 server 本地记账（零消息）；后端凭空造的前端对象（Magma 占位纹理、swapchain default-FB 占位）→ server 原生；`SetBackendResource` 删除（server 拥有资源表）；`SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）直接删除；`SetBackendHashMemo/AuxMemo` → server 侧 per-slot 字段。20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 按 §5.7 逐站点归属，其中至少一处消费者搬不走：Magma 的 `ResolveUniformBufferPayload` 把具名 UBO 打进自己的 UBO ring → `SetShaderBuffers` 的 host payload（D-B8）。

### 8.2 有序性是正确性要求

每一次 `WritebackFromBackend` 后面都紧跟 `BumpBufferMutationEpoch()`，否则 server 的 draw-clean memo 会在 epoch 背后变陈旧——split 里这变成反向通道上的排序规则：写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**

### 8.3 错误、ack 与日志

- 纹理分配的 OOM 在 monolith 里就已推迟到 sync 时刻（`glTexImage*`/`glTexStorage*` 只 `MarkStorageDirty`，Espryt 惰性分配；连 `glRenderbufferStorage*` 也在 `SyncToBackend` 里惰性做），拆分不改变可观察行为，这批不同步 ack。
- **唯一允许同步 ack 的入口是 `glBufferStorage`（真同步分配）**。`glRenderbufferStorage*` 不 ack：41 个 trace fixture 里 OOM 探测惯用法出现 0 次（9 次调用散在 5 个 fixture，无一在 3 个调用内跟 `glGetError`；语料里的成功性检查是 `glCheckFramebufferStatus`，client 本地作答）。目录里目前没有条目携带 `kNeedsAck`（`ResourceRespecify` 是 `kNone`），标记随 P3a 的 buffer 路径落地。
- 其余错误一律晚到，走有序的 `OnGlError`。
- `OnLog` 分级：≤WARN 有损（覆盖最旧 + `eventDropped` 计数）；≥ERROR 无损，加入触发 `eventRingFull` + 停止 apply 的语义事件集；每秒 ERROR 速率限制器，超限发一条 "N errors suppressed"；`MGLOG_E_ONCE` 的 latch 变 per-server。理由：后端 link 失败只以一行 ERROR 呈现，统一有损会让最有诊断价值的那一行在日志压力下消失。

### 8.4 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素，三个原因会要求重发已发过的 level：`RequireImageBindableStorage` 的 re-dirty、整格式再生、view 源重铸。四条缓解同时上：

1. **预防主因**：client 给纹理打 `everImageBound`，`ResourceCreate/Respecify` 一直携带 `ImageBindableHint`，image-bindable 存储前期分配好。
2. **拉取异步**：server 发 `OnTexturePullRequest` 并把 twin 标 not-ready，client 下次 publish 时重发；阻塞的是 `mgl-srv-apply` 线程不是应用线程。
3. **有上限的保留，默认关**：`MOBILEGL_PIPE_TEXEL_RETAIN_MB` 默认 0——`MipmapStorage` 保有每 level 完整 CPU 影子，拉取总能被服务，缓存买的是延迟不是正确性。只有实测拉取率非平凡才开。
4. **显式终止符**：拉取是 request/response 对，由 `ResourceSubDataComplete(res, target, firstLevel, levelCount, pullSerial)` 终止，**可携带零个 region**——内容只来自渲染、被 `CanMirrorCopyImageShadow` 拒绝的 copy、或 GPU 侧 mip 生成的 level，client 根本没有字节；收到零 region 时 server 带着"已分配但为空"的存储继续（正是 monolith 的行为）并记 `MGLOG_W`。没有终止符 apply 线程会永久 park。

门：`TextureRemintPullScenario`（含无解用例，且在终止符落地前必须是红的）；拉取次数逐 trace 用例发布。本设计从不声称"零 round trip"，它测量并公布。

### 8.5 XFB scatter 搬到 client

Espryt 的 `ScatterCapturedRecords` 是对 client shadow 的 read-modify-write：从应用已有的字节起步，只把捕获到的 varying 补进去（`gl_SkipComponents` 的空洞保留应用原本的内容，`KHR-GL46.transform_feedback.capture_special_interleaved_test` 走到它）。server 没有 `MappedData()`，所以：server 把紧密打包的 scratch 通过 `OnBufferWriteback` 推给 client，用 `OnXfbScatterReady` 告知布局；client 拥有目的 shadow 与反射归档里的 varying/stride，原样跑补丁循环；补好的范围作为普通 `ResourceSubData` 重发并 bump change serial。不新增停顿类。

## 9. 后端状态机改造

### 9.1 原样不动的东西

Espryt：三条 persistent-mapped ring 与 `PersistentRing` 算法、buffer pool、7 条 fallback-repack 路径、`m_backendColorSlots` 置换表、三个 scratch FBO 及驱动侧影子、`PackState`、全部驱动绑定影子、Adreno 禁用属性 SIGSEGV workaround、Mali XFB 捕获丢失 workaround、`ScopedDefaultUnpackState`、SPIRV-Cross 会话与 post-emission ESSL 重写、驱动 POST 自检族、restart 重写与 multi-draw 五档。
Magma：`VulkanRenderer` 全部 memo 与 scratch、`PipelineFactory`、`ProgramFactory`、`UniformManager` 的 ring 与描述符集、五个 `Vk*Manager`、`FrameContext`、`SwapchainObject`、`DynamicStateShadow`、`VertexInputStateFactory` 的 cache 本体、**D18 的节点式容器纪律**（`m_renderbufferResources`/`m_textureResources` 故意用 `std::unordered_map`，调用方跨查表缓存 `Resource*`；postmortem 注释逐字进 review checklist）。

从"不动"里移出的一项：Espryt 的 sub-rect 上传判定与跨步计算（§6，从描述符取步长）。

唯一两处必须真改的 `MG_State` 类型内部用法（都在 Magma）：占位纹理（构造真的 `TextureObject2D*` 只为复用 `SyncTextureAndGetDescriptor(ITextureObject&)` 签名，~120 行木偶戏 → ~60 行原生 `VkImage`+view+descriptor，34 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个随之消失）；两个内部 shader 烘焙（§7）。Espryt 的小号同类：`g_rawDepthFetchSamplerState` → 后端原生 sampler。

### 9.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充 + poison 世代（P1）

```cpp
// MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，不按读点组织（~20 KB，字段集全迁移期稳定）
struct PipeInputs {
    const RenderStateParameters& GetRenderStateParameters() const;   // 阶段 A：类型与后端今天读到的完全一致
    // … 每个后端真正用到的 GLContext 方法一个访问器（Espryt 32 / Magma 55）
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
| A 别名 | 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（293 处）+ 手工转换 58 行非箭头用法（~34 处 `MOBILEGL_ASSERT` 删除、7 处空守卫、3 处三元、`.get()` 裸指针捕获与 `decltype` 别名、14 处 `!= nullptr`、1 处注释）；逐 verb 类填充点填 `gPipeInputs` | `nm --defined-only` 不变；`.text` 差异可逐行归因（空守卫/三元的重写推迟到 P2） |
| B 推送 | tracker 填 `gPipeInputs`，填充器按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | `MOBILEGL_PIPE_VERIFY=1`：tracker 再填一份快照版，G4 比对器逐字段每 draw 比一次 |
| C 句柄化 | `SharedPtr<前端对象>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§13） |

- 填充点逐 verb 类，不只 `PrepareForDraw`/`SetupDraw` 两处：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些字段"的表，在 `MG_Impl` 的 ~93 个边界站点生成 validate/fill 调用。
- poison 是**逐 verb 世代**不是位图：每次 verb 递增 `m_currentVerbSerial`，字段被填时记下序号，读取时断言相等（跨 verb 有效的字段显式标 sticky）。位图看不见"上一个 draw 填过、紧随的 `glTexSubImage` 读到陈旧值"。debug 与 disaggregated 构建里读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`。纯度门 grep 的是 `pGLContext` 不是 `pGLContext->`。

### 9.3 Track V / Track H

- Track V（值类型：`GetRenderStateParameters`、`GetPixelStoreParameters`、capability 位、stencil/colormask/depthmask/scissor/patch/attrib 默认值、Magma ~22 个标量 getter……约 B 类读点的 55%）：机械。
- Track H（对象类型：167 个 `SharedPtr<MG_State…>` 点）：真活。
- 读点分类实测（静态）：A 探测变化 ~35（12%）、B 翻译输入 ~216（74%）、C 瞬时参数 ~4、D 身份/缓存键 ~48（与 B 重叠）、E 数据字节 3、写 8。74% 是 B 类——"bump 一个版本让 server 自己拉"行不通，值本身必须过去。

### 9.4 残余值块

Track V 的 55% 不需要逐字段接口条目就能跑起来，所以 P2 发一个**显式临时**调用 `SetResidualValueState(MGPBlobRef)`，payload `ResidualValueBlock{RenderStateParameters, PixelStoreParameters, CapabilityBits, patch 三字段}`。三条纪律：退役是编译错误（`MGL_RESIDUAL_BLOCK_SIZE` 只降不升，`MobileGL/MG_Pipe/MGPipeTypes.h:535`，P13 变成 `static_assert(sizeof == 0)`）；布局逐成员 `offsetof` 断言且 split 下逐字段序列化（异质 POD 并集的 padding 差异 monolith verify 看不见）；只在 P2..P13 存在，`MOBILEGL_PIPE_STATS` 单独计一类字节（`ResidualValueBlock`，P0 已占位）。

### 9.5 21 条身份 memo 的重键

统一事实：每个进入 memo 键的版本计数器要么是回绕 `Uint16`，要么根本不会被它害怕的那个 mutation bump；身份比较是堵回绕洞的补丁。`{slot, gen}` + 显式 destroy 让 **11 条直接删除**（registry 的同址 `weak_ptr` + GC ×6、`TwinLookupMemo` ×3 + `OwnerEquals`、`UnitSamplerLookupMemo` 的 `WeakPtr` 测试、`SetBackendStateMemo`、`VkTextureManager::TextureIdentity` 存活探测、`ConvertedVertexStreamKey` 的 `sourcePin`……），**2 条** server 删除但去抖搬到 client（§5.4），**7 条重键**成更便宜的比较（`StampSyncedFBO` 四元组 → `ContentHash` + server 私有 `attachmentRemintEpoch`；`ResolvedTextureBindingMemo` 9 键 → `(shaderCso.slot, viewSetSerial)`；`SetupDrawSnapshot` 的 ~14 探测字段与两个有损求和 → 三个 handle + 两个 server 纪元 + dirty mask；`VertexInputStateFactory::ComputeHash` 里的 lifetimeId → `gen` **混进** server 侧每个 content hash），**1 条**（D18）原样不动。两个顺带修掉的潜伏 bug 已先独立落地：`m_xfbCounterSlotByObject` 用裸 GL name 做键（`bd2b4158`）、`RenderbufferObject` 缺 `GetLifetimeId()`（`9c7339b2`）。

### 9.6 A/B 与口径收窄

`MOBILEGL_PIPE_PUSH` 子系统位图（含一位关闭 CSO 内容寻址，负面对照）在阶段 B 是真正的旧-vs-新 A/B；阶段 C 之后不是——位清零时 `SnapshotFromGLContext()` 仍要合成句柄，后端仍跑重键后的 memo 代码，一个重键 bug 两臂都在。对策：**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）在 P3a/P4a 期间保留 registry / `TwinLookupMemo` 实现活在同一个 `PipeInputs` 接口之下，随 pull 路径在 P13 退役（各阶段 +1 天维护）。

P13：删 `SnapshotFromGLContext()` 的非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**保留 `MOBILEGL_PIPE_VERIFY` 连同它需要的 `SnapshotFromGLContext()` 与 `MG_State` include**（D-B5，verify 构建永不出货）；三道纯度门在非 verify 构建上转绿。

## 10. server 侧

### 10.1 对象表与 applier

- `MG_Remote/Server/PipeObjectTables`：按 kind 的 slot 数组，不是对象图；server 不持有任何 buffer 的完整副本、不持有纹素、不持有前端对象图。
- `PipeApplier`：解码 → 更新对象表与 `PipeInputs` → 调后端函数指针。debug 断言：任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界。`InProcessTransport` 走与 spawn **完全相同**的 G3 编解码路径，只在门铃/拷贝机制上不同。
- 每 context 一份 working `RenderStateParameters`（§5.3）。

### 10.2 monolith 侧的净收益

即使 IPC 永不上线：复用地址 ABA 一整类不可表达；FBO → program 排序 hazard 消失；`SwapchainObject` 写 `MG_Impl` 的分层倒置消失；两个潜伏 bug 已修；一次 glslang 编译离开启动路径；`inproc` = 渲染线程；`MG_Test` 的 mock 后端变成 MGPipe recorder（§13.3）。monolith 净代码量是**增加**的（约 +6,650 手写 + 4,000 生成，对 ~372 行真删除），所以 monolith 论据是逐线程 CPU 数字（§13.2-④），不是删除行数。

### 10.3 索引宿主镜像（`MG_Remote/Server/IndexHostMirror`，P8）

- 覆盖：`BindMask & ELEMENT_ARRAY` 的资源，且仅当 `kCapNeedsHostIndexBytes`（split 且 server 需要索引字节做 restart 重写 / multi-draw 展平）。
- 由 server 本来就要收的 `ResourceCreate/Respecify/SubData` 流增量维护：零额外线上流量、零 round trip。GPU 写者对镜像的影响由 `OnGpuWritten` 收窄集在 server 本地判定。
- 预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），逐帧发布 `index-mirror-bytes`；超预算时该 buffer 退化为逐 draw 经 `MGHostSpan` 传送（`Seg` 指向 `SEG_STAGE`），计入 `index-bytes-shipped`。
- 必须是它：`kMaxRestartRewriteBytes` = 64 MiB 是默认 `SEG_STAGE` 的两倍，`kMaxFlattenedIndices` = 1<<24 同量级，逐 draw 塞进 32 MiB 的段既不可行也无必要。它是本设计里唯一的"数据副本"。

## 11. 传输与数据面（骨架 P0 已落地，`MobileGL/MG_Remote/`）

### 11.1 段

| 段 | 拥有者 | 默认 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB，2 的幂 | `RingControl`（4 KiB 页）+ POD 记录 + ≤4 KiB 内联负载 |
| `SEG_STAGE` | client | 32 MiB，上限实测定 | bulk 字节：buffer sub-data、纹理紧密重打包区域、UBO scratch、client 顶点/索引/indirect 数组、multi-draw 参数块、具名 UBO host payload、persistent-map 脏块 |
| `SEG_REPLY` | server（client 只读） | 8 MiB，4 KiB slot | readback 像素、buffer writeback |
| `SEG_EVENT` | server | 256 KiB SPSC ring | 十个回调的事件 + `EvQueryResult/EvFenceSignaled/EvReadbackDone` |
| `SEG_SHADOW[n]` | client | 每对象，≥256 KiB shadow（Phase 2） | 零拷贝 buffer/texture shadow |
| `SEG_ADOPT[n]` | server（client RW） | 每 buffer，≥16 MiB adopted store（P11） | 应用直写 GPU 内存 |

创建（`ShmSegment`）：Android `ASharedMemory_create`（API 26；libc 的 `memfd_create` wrapper 是 API 30）；桌面 Linux `syscall(SYS_memfd_create)`；其他 POSIX `shm_open`+`shm_unlink`；Windows `CreateFileMappingW`(`Local\`)。传递：POSIX `SCM_RIGHTS`（`FdPassing`，专用 `AF_UNIX SOCK_DGRAM` socketpair——消息边界保住 ancillary data 与 payload 不被拆开，sideband ≤256 B）；Windows 段名走 `SegmentRef`。fd 传递在第一个 transport commit 里实现——没有它数据面在唯一重要的平台上一字节过不去。

不进 `SEG_STAGE` 的：restart 重写的整 EBO 与 multi-draw 展平的索引流（走索引镜像）。`SEG_SHADOW` 块的退休规则：释放的块进 pending 链表，`appliedSeq`（借入 GPU 时间线的 slot 用 `retiredSeq`）越过最后一条引用它的记录后才归还 arena。

### 11.2 `RingControl`（`Ring.h`）

一页 4 KiB，每个争用组各占一条 cache line：`SEG_CMD` 游标三元组 `cmdHead / cmdAppliedTail / cmdRetiredTail`；`SEG_STAGE` 独立三元组（`stageHead / stageAppliedTail / stageRetiredTail`——"`SEG_STAGE` 余量 < 1/4"是 publish 触发器，占用率不能从另一个 ring 算出，且 stage slot 的退休条件不同）；三个严格区分的水位 `appliedSeq`（释放 `*AppliedTail`）/ `submittedSeq`（释放 staging）/ `retiredSeq` + `completedFrameSerial`（释放 `*RetiredTail` 与 `SEG_ADOPT`）+ `presentAckSerial`；`serverEpoch`（context 丢失 / server 重启 ++）、`ringGeneration`（硬 drain 后 ++，作废缓存 offset）、`consumerParked`/`producerParked`、`eventRingFull`、`eventDropped`。两个 tail 是必须的：P11 之后 server 会**借用** ring slot 而不是再拷一次，那种 slot 只能在 `completedFrameSerial` 之后回收。游标是单调字节计数、2 的幂掩码、永不重置。

记录头 `RingRecordHeader{kind, flags, size}`，kind 0 保留给 wrap 填充；`RingProducer::Reserve` 在记录会跨 wrap 边界时自动发 pad 记录，保证每条记录连续；`MaxRecordBytes() == Capacity()/2`；`RingConsumer::Pop` 拒绝不可能的头（非 8 对齐、小于头、大于已发布）并置 corrupt → `Fatal{ProtocolCorruption}`；`HardDrainRing` 只在两侧静默且 ring 全空时 bump generation。

### 11.3 双向 doorbell（`Doorbell.h`）

- client → server：consumer 自旋 → 置 `consumerParked=1` → 阻塞；producer release-store `cmdHead` 之后仅当 `consumerParked` 时敲（字节码 `0x01`）。
- server → client：client 在**任何**等待（present credit、`kNeedsAck`、ring/stage 满）先自旋 `MOBILEGL_IPC_SPIN_US`（默认 50 µs）→ 置 `producerParked=1` → 阻塞；server 在 release-store 任何 watermark 之后仅当 `producerParked` 时敲（`0x02`）。没有第二个方向，每处 client 等待都退化成跨进程自旋一条 cache line——手机上一颗大核满频空转一整帧，而全库没有亲和性控制。
- 两个实现，零 futex/eventfd/named-event 平台代码：`CondVarDoorbell`（`inproc`，带 `Kill()` 死亡态让 `Shutdown` 能 join 一个 parked 的等待者）与 `SocketDoorbell`（`spawn`，一字节；`SOCK_STREAM` 端在对端关闭时报 `POLLIN|POLLHUP` + `recv()==0`，这是死亡检测）。
- 丢失唤醒窗口由**两个 `seq_cst` fence** 关闭（等待者置标志 → fence → 再测条件；通知者发布 watermark → fence → 读标志），标志本身的访问是 relaxed。`NotifyIfParked` 的前置条件：watermark 已发布。死亡的 doorbell 让 `Wait` 停止重新 park。

### 11.4 控制面（`protocol.fbs`、`Framing.h`、`ITransport.h`）

- 一份 schema，两种用法：热路径 → FlatBuffers `struct`（定长、无 vtable、只需边界检查）直接进 ring——即 G3 生成的记录，与 `MGPipeTypes.h` 的 POD 逐条 `static_assert` 尺寸/`offsetof` 对齐；罕见/变长/需演进 → `table` 走 CTRL socket。今天 `protocol.fbs` 只含控制面（`MobileGL/MG_Remote/Protocol/protocol.fbs:218-228` 的 `CtrlMsg`：`Hello`、`Welcome`（四个段的 `SegmentRef`）、`CapsSnapshot`、`SurfaceOp/SurfaceReply`、`ResyncRequest/Done`、`AuxRequest`（外来线程的 fence wait / query result / scalar get）、`Fatal`（`ProtocolCorruption/RingOverrun/SegmentMismatch/DeviceLost/ServerCrashed/AbiMismatch`）、`LogLine`），`file_identifier "MGLC"`；union tag 是 wire 值，只追加。
- `protocol_generated.h` 提交进树，`scripts/gen_protocol.py` 再生成（只用 `MOBILEGL_FLATC_EXECUTABLE` 或从 pinned submodule 在仓库外构建一次的 flatc，不用 PATH 上的），CI `flatc-check`（`.github/workflows/test.yml:304`）重生成并 diff。**codegen 绝不进默认构建图**；运行时 header-only。
- 封帧 `[u32 'MGLF'][u32 len][payload]`，64 MiB 上限，**读时校验**：坏 magic / 超长长度立即 latch 失败并报 `MOBILEGL_ERR_PROTOCOL_MISMATCH`（不是静默永久挂起）；接收缓冲不足**返回所需大小并保留消息**（`MOBILEGL_ERR_BUFFER_TOO_SMALL`）。
- `ITransport`：`SendFrame / ReceiveFrame / PeekFrameSize / ShareFd / ReceiveFd / Shutdown / Role`；热路径完全绕过它。`Shutdown` 拆掉整个连接（两端都不能再发，等待者全部解锁，已排队消息仍可读完）。`WireLog.h` 是唯一的日志入口，让 `Transport/` 的头不 include 前端 umbrella（纯度门 A 断言 `-H` 输出）。
- `mg_protocol_base.h`：纯 C、无依赖的结果码 / span / `ShmRegion` / id 词汇，structSize-first 版本纪律（追加 = minor，改动 = major，major 不符是结构化失败）。

### 11.5 WAR 危害、拷贝账与背压

- Phase 1（P5–P8）：GL 调用时刻把字节拷进 ring slot，slot 到 `stageAppliedTail` 越过它为止不可变，危害按构造消除；代价一次 memcpy，`Ops_ResidentSubData` 与 `StageBlocksIntoUnpackRing` 在 monolith 里已经在付。
- Phase 2（shadow-in-shm，零拷贝）：≥256 KiB 的 shadow 分配在 `SEG_SHADOW`（`PipeResource::MapAlignedAllocator` 增加 shm arena，保留 64 B 对齐契约；`MipmapStorage` 的 level vector 同理），`ResourceSubData` 只带 `{seg, offset, size}`。WAR 用 per-shadow 64 KiB 块发送水位：应用写某块而该块上次发送尚未被 `appliedSeq` 覆盖 → 这次写走 `SEG_STAGE`。必须整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹（改容器 allocator 就改了类型，option OFF 时逐字折叠回今天的 allocator）。

| 路径 | monolith | Phase 1 | Phase 2 |
|---|---|---|---|
| `glBufferSubData` → shadow store | 2 | 3 | **2** |
| `glBufferSubData` → adopted store（P11） | 2 | 2 | 2 |
| `glMapBufferRange(WRITE)`+unmap | 3 | 4 | 3 |
| persistent coherent map 推送（§12） | 0 | 1/发射点 | 1/发射点（精确块） |
| `glTexSubImage` | 2 | 2 | 2 |
| 全局 UBO / draw | 1 | 2 | 1 |
| adopted ≥16 MiB（P11 T1/T0） | 0 | 0 | 0 |

server 没有第二份 `BufferObject`，所以不存在"staging → server 侧 shadow"这次中间拷贝。字节计数器装在 wire 两侧，验收看总量。

- 分配与背压：逐字移植 `PersistentRing`（单调 head/tail、2 的幂掩码、frame mark）。分配失败升级：扩容（翻倍）→ 对最老未 retire 批次有界等待（默认 50 ms，走 `producerParked` doorbell）→ 硬 `Drain` + `ringGeneration` bump。硬 drain 后恢复便宜：正向流是自洽的推送流，tracker 把全部 dirty 位置为"必须重推"，下一个 verb 重发完整 `set_*` 集合，纹理侧由发射游标负责，没有"重发未 apply 对象状态"的特殊协议。`SEG_CMD` 与 `SEG_STAGE` 各自独立跑这套升级。

### 11.6 publish、序号与 credit

- 不设"records ≥ 64 KiB"一类阈值（那是一整帧的流水线气泡，且否掉 `inproc` 的全部意义）。规则：每条记录（或每 8–16 条摊销）release-store `cmdHead`，仅当 `consumerParked` 时敲门铃。
- 显式门铃点：`present`、任何 `kNeedsAck` 请求、`eglMakeCurrent`、`glFlush`（刷出不等待）、`SEG_STAGE` 余量 < 1/4、**轮询类入口**（`glClientWaitSync` 任意 timeout、`glGetSynciv(GL_SYNC_STATUS)`、`glGetQueryObject*(AVAILABLE|NO_WAIT)`——否则 `while (glClientWaitSync(s, FLUSH_COMMANDS_BIT, 0) == TIMEOUT_EXPIRED) {}` 永久自旋）；带 `GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 publish。
- 饥饿升级：同一 handle 连续 N 次（`MOBILEGL_IPC_POLL_ESCALATE`，默认 64）本地回答"未就绪"而 watermark 毫无移动 → 升级为一次阻塞 round trip。
- `glFinish`/`glFlush` 保持纯 no-op。
- seq = 记录序数；两个互相独立的窗口：字节 credit（两个 ring 各自占用）与 present credit（`presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT` 时 `eglSwapBuffers` 阻塞）。server 不发 credit 消息：对 `RingControl` release store，consumer 每 64 条记录更新一次 `appliedSeq`，`producerParked` 时敲反向门铃。

### 11.7 事件回传与溢出

`SEG_EVENT` 承载十个回调加回读完成通知。client 排空点：`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`glGetSynciv`、`eglSwapBuffers`、`glMapBuffer*`/`glGetBufferSubData`/`glCopyBufferSubData`，以及**每一次等待循环的每一轮**。溢出策略（修一个双向死锁：client 卡在 present credit、server apply 线程卡在生产事件）：`EvLogLine` ≤WARN 有损；语义承载事件（`EvGpuWritten`、`EvReadbackDone`、`EvFenceSignaled`、writeback、pull request、mip、scatter、`EvGlError`、surface、caps、`EvLogLine ≥ERROR`）无损——ring 满时 server 置 `eventRingFull=1`、**在记录边界停止 apply**、敲反向门铃，client 排空后清标志并敲正向门铃；ERROR 速率限制器。故障注入：client 被 credit 阻塞时灌满 `SEG_EVENT`；日志洪泛下注入一次 link 失败，那行 ERROR 必须出现且两侧恢复。server 侧 `MGLOG` 按流顺序 replay 进 client 日志流（复用 `DeferredLogLine` 机制）。

### 11.8 fence 与无 present 负载

- fence 完成度必须来自**真的逐 fence 退休**，不是 present 水位：DirectGLES 的 `g_completedFrameSerial` 只在 `Present()` 与 `WaitForFrameSerialCompleted` 里前进，帧中 fence 会退化成帧计数推断——`DirectVulkan.cpp` 写明这是被修掉的 bug（MC 1.21.5 的 fence-paced ring 曾因此 native-heap OOM）。规则：`FenceCreate` 转成真实的后端 `FenceSync()`，server 用自己已有的逐 fence 轮询在非 present 时刻也推进并发 `EvFenceSignaled`。
- 无 present 循环（CTS、回读循环、从不 swap 的集成场景）下 `retiredTail` 会饿死、`SEG_STAGE` 填满、每个用例都跑到硬 drain。规则：DirectGLES 的 server 加**非 present fence tick**——距上次 `Present` 超过 8 ms 或每 4096 条已 apply 记录插一个 `glFenceSync` 并轮询 fence ring；ring 占用率与升级次数进计数器；P8 加一个无 present 的 split 用例。

## 12. persistent map 与 ≥16 MiB 采纳

`AcquirePersistentMap` 是永久的地址空间捐赠（返回 host-visible coherent 指针，成为该 buffer 的唯一真相源；≥16 MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到，实测 MC 26.3 p99 163→21 ms、40→115 fps、省 ~400 MB）。**整个 monolith 改造期一动不动**（D-B4），只有 IPC 那一步会打破它。

三档，由运行时 POST 探针选择（本项目"后端限制一律探针判定、不硬编码驱动名"的既定规则），**spike B 已在两台设备上给出答案**（`MEASUREMENTS.md` §2）：

| 档 | 形态 | 实测 |
|---|---|---|
| **T0 — server 导入 client 分配**（P11 主攻） | client 分配 `AHardwareBuffer` BLOB，socket 交接；server 以 `VK_ANDROID_external_memory_android_hardware_buffer`（Magma）或 `EGL_ANDROID_get_native_client_buffer` + `glBufferStorageExternalEXT`（Espryt）导入，两侧 persistent+coherent 映射 | **Adreno 830 与 Mali 都是完整读写往返**，含 GPU 访问与两侧字节校验——唯一在两台设备、两个后端上都成立的档 |
| T1 — server 导出自己的映射 | `VK_KHR_external_memory_fd` opaque fd，client `mmap` + 导入 | 只有 Adreno 的 Vulkan 路径可用；Adreno 的 GLES 导入 `glMapBufferRange` 全部 `GL_INVALID_OPERATION`；Mali 不可导出。**每次存储定义一次 round trip**（不是每 store 一次），`StorageBufferRegrowScenario` 发布 `map-persistent-roundtrips` |
| T3 — host pointer 导入（`VK_EXT_external_memory_host`） | | Adreno 无扩展；Mali 只读（GPU 写对宿主映射不可见） |
| T2 — 拒绝（永久正确回退） | `AcquirePersistentMap` 返回 `nullptr`，前端已在三处容忍 | 此档下 client 侧推送强制 |

`MOBILEGL_IPC_ADOPT_TIER`（`auto`/0/1/2）做负面对照；与 `MOBILEGL_IPC_RESPAWN` 互斥（被采纳的 store 是 server 拥有的内存）。

**client 侧 persistent map 推送三件套**（T2 档强制，P5）：

1. 不做 map/unmap 命令对：server 唯一需要知道的是"这个资源现在有没有活的宿主写入者"（`IsBufferDrawClean` 那一行要表达的东西），所以 `ResourceRespecify/SubData` 的 payload 带一个 `hasLiveHostWrites` 位，零新增记录种类。
2. 块粒度脏块推送：tracker 维护 `m_livePersistentMaps`（persistent+write+非 FlushExplicit+非 GpuResident），在每个 validate 点对本次操作可达的每个这类 buffer（VAO/index/indirect/UBO/SSBO/atomic/XFB target——即后端 20 个 `SyncPersistentMappedRange` 站点的并集）按 `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`（默认 64）切块发送。Phase 1 保守版（整个 mapped span 当脏，按块拆）；Phase 2 精确版（shadow-in-shm 的 64 KiB 块脏位，`memcmp` 先行）。P5 验收记录 `persistent-map-push` 字节量；若保守版在 Create/Flywheel fixture 上不可接受，精确版提前——计划里唯一允许因测量改变阶段顺序的地方。
3. 门从第一天就有：`PersistentCoherentMapScenario`（map PERSISTENT|WRITE|COHERENT、写、不做任何其它 GL 调用、draw、readback 校验）。

`MOBILEGL_COHERENT_AS_FLUSH` 在拆分模式下照常生效：两个带 `coherent_as_flush: true` 的 Create fixture 在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义。

## 13. 回读、roundtrip 清单与验证

### 13.1 稳态零 roundtrip 与不可避免的阻塞点

零 round trip：全部 draw/clear/blit/copy/dispatch/barrier/XFB 跨度/bind/CSO/`set_*`/上传/`present`（单向记录）；全部 caps 站点（握手快照）；`glGetError`/`glFinish`/`glFlush`（本地 / no-op）；fence 与 query 的创建及非阻塞轮询（client 铸造 handle，未命中合法地答"未就绪"）；`glGetTexImage`（DirectGLES，含 GPU 生成的 mip）；`glReadPixels` → pack PBO（fire-and-forget + client 侧 `MarkGpuWritten`，严格优于 monolith 的无条件停等）；`glEndTransformFeedback`（取消无限 fence 等待，对 capture target 置 `MarkGpuWritten`）；`eglSwapBuffers`（只查 credit）；`*IndirectCount`；restart/multi-draw。

不可避免（全部罕见）：握手一次；surface 生命周期与首次 `MakeCurrent`+`InitCapabilities` 每 surface 至多一次；`glReadPixels` → 客户内存（像素进 `SEG_REPLY`，逐行写回循环留在 server 内按操作级批成一段）；`glGetTexImage`（DirectVulkan，对"无 GPU 背书"的 level 回答"请用你自己的 shadow"）；GPU-write pending 的 buffer 首次 CPU 读（monolith 本来就 `glFinish()`；由 `writableMask` 与 `OnGpuWritten` 收窄）；`glClientWaitSync(timeout>0)`、`GL_QUERY_RESULT` 未完成、`glBeginConditionalRender`（谓词只解析一次，之后每个条件 draw 在 client 丢弃，server 永远不需要那个 query）；`glBufferStorage` 的 ack；`MapPersistent`（仅 T1，每次存储定义一次）；纹理拉取（§8.4）；client 侧索引扫描当源 EBO 在 pending 集里；ring/stage 耗尽与 present credit（节奏，非语义）。

验收措辞：在全部 40 个 trace 用例上发布逐用例的 roundtrip 计数器、纹理拉取计数器、索引镜像字节数与 `index-bytes-shipped`；零 timeout 轮询循环必须在有界时间内退出。

### 13.2 五部分验证门（取代 monolith 的字节一致门）

"改前改后 `nm --defined-only` 与 `.text` size 完全相等"的门在本方案里按构造死亡（不存在能让旧字节回来的配置）；替换是：

1. **接口纯度三道门**（只跑非 verify 构建）：**A 门 include 图**——disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h → FramebufferObject.h → TextureObject.h` 正是这种耦合），依赖 P0.5；**B 门符号**——`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门未声明**——`grep -c 'pGLContext' MG_Backend/` == 0。外加 debug 断言"每个后端 memo 键都是 `{slot, gen}`，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑（重键前必须在至少一个后端上是红的）。
2. **语义影子比对 `MOBILEGL_PIPE_VERIFY=1`**——决定性的一条：两套状态模型活在同一地址空间，tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 比对器逐字段、每 draw 比对，打印第一个分歧字段与 draw 序号。抓 tracker 忘推的字段、**dirty 位触发得太少**（危险方向）、两条路径变换不一致的值。第三种 CI 模式，40 个 trace + 全部集成测试，~5–10× 慢，永不出货。逐字段而非 `memcmp`（padding 会 false-DIFFER）。**保留模式**：消费即清的组（纹理 dirty rect）发射后无法重算，verify 时 tracker 保留清除前的集合并比对发射出去的 `(UnionBox, RegionCount, Regions[])`。**活过 P13**。
3. **行为 A/B**：40 个 trace 在 `{monolith-pull, monolith-push, split}` 下 SSIM ≥ 0.99（默认阈值）；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.`（DirectVulkan 同）之间逐名相同；单元测试全绿；CTS 逐后端 conformance 在 0.5 pp 内（行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母）。`TextureUploadShapeScenario` 把逐纹理逐帧的上传形状（box vs N region、作业数）录金标比对——+6 ms 悬崖由形状相等把关，SSIM 对它完全不敏感。逐名功能基线是"P1 出口的重构后 monolith"（P1 出口先用 verify 证明等价于 `81b17c0b`）；`81b17c0b` 只作性能锚点。
4. **monolith 性能不回归**：两台设备 reboot-clean、同热窗口、配对 A/B，`tools/bench.sh` + trace replay `--benchmark` 逐帧 JSON；**指标是逐线程 CPU 时间**，p50 与 p99；**绝对阈值**——tracker 每 draw 的 ns 公布并设上限（真实拉取基线只有每 draw 6.5–9.3 次 accessor，相对噪声阈值会平凡通过）；Blaze3D blend-toggle 微基准单列；关掉 CSO 内容寻址的负面对照。
5. **覆盖 + poison + 句柄纪律**：G6 重生成 0 UNMAPPED；`gen_pipe_dirty_surface.py` 重生成 0 未映射 mutator；逐 verb 世代 poison；G7 setter 一致性测试；`ResidualValueBlock` 的 `offsetof` 断言与 P13 的 `sizeof == 0`。

两条幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中。符号与 `.text` 漂移每阶段作为信息性指标发布。

### 13.3 长期语义门：MGPipe recorder

P13 把 `MG_Test` 的 mock 后端变成 MGPipe recorder：在一组 fixture 上录下每 draw 的已推送状态，后续构建对比录像。它不依赖 `MG_State`，是 P13 之后不靠 verify 构建的语义门，也给 `tools/trace_replay` 一种记录**已解析**状态的、比 apitrace 精确得多的录制格式。它只覆盖推送内容，不覆盖后端对它的解释（split-only 的渲染 bug 仍无 server 侧第二意见）。

## 14. Present、线程与帧节奏

- `eglSwapBuffers` → `present{frameSerial}`（swap interval 搭在同一条记录上）→ publish + 敲门铃 → 返回，除非超出 credit。**`present` 与 `eglSwapBuffers` 严格 1:1**：两个后端的帧边界排空（Magma 四次 `OnFrameBoundary` 老化、`TryDrainFrameTransients`、`BeginFrame`；Espryt 三个 ring 与 `TrimBufferPool` 的 retire）只在 `Present` 内发生，批量会饿死它们。
- **`MOBILEGL_IPC_PRESENT_CREDIT` 默认 1**（可配 1–4）：延迟叠加，`端到端 ≈ client credit + server 帧数 + 驱动深度`；server 的 `Present` 末尾已在 `vkWaitForFences` 上等 2–3 帧，credit 2 就是端到端 4–5 帧（60 Hz 下 66–83 ms）。P10/P12 用 `GetGpuTimestampNs` 与 `--benchmark` 逐帧 JSON 构建输入延迟直方图，只有实测吞吐收益能抵掉延迟代价才调高。
- Magma 从不注册 `SetSwapInterval` 且偏好 `MAILBOX`/`IMMEDIATE`，IPC credit 是它唯一的显式限帧器；若需要 FIFO 作为独立 `dev` 变更。
- 线程——client：**v1 不加线程**，编码在 GL 线程上直接写 ring（前端本就是 per-context 单线程契约）；外来线程的 sync/query 读全部从 `RingControl` 无锁回答，必须发射的少数取 `ctrlMutex` 走 CTRL socket 的 `AuxRequest`（SPSC ring 不允许第二个 producer）；`ShaderCompilePool` 原样在 client；可选 `mgl-client-tx` 凭测量决定。server：`mgl-srv-io`（asio、封帧、`SCM_RIGHTS`、doorbell、CTRL RPC）、`mgl-srv-apply`（**终身持有原生 context**：`g_backendContextOwnerThread` 只写一次，`MakeCurrent` 的缓存失效风暴变启动期一次性，每帧 EGL 复核恒真，off-thread 降级消失）、可选 `mgl-srv-dec`。
- **核心放置**：拆分的全部性能主张押在两半落在两个都快的核上。全库无亲和性控制，server 是独立进程不继承 launcher 的亲和性。规则：报总 CPU 工作量差（client tracker + encode + decode + server apply vs monolith `PrepareForDraw`）；复用 `ShaderCompilePool` 的大核探测把 `mgl-srv-apply` 绑到大核（`MOBILEGL_IPC_SERVER_AFFINITY`，默认 auto，解析出的 mask 打进日志）；每阶段报逐线程 CPU 时间。
- 拆机顺序：publish + server 排空并 ack → 停 apply 线程 → 关 transport → client 排空 compile pool（先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构）→ `MobileGL::Destroy()` → 释放 sync/query handle。

## 15. 进程、EGL 与平台

### 15.1 启动与握手

- server 定位：`MOBILEGL_IPC_SERVER_PATH`（主要）→ `dladdr(&MobileGL::Initialize)` 同目录的 `libMobileGLServer.so`（兜底；不能当主要机制，因为集成测试静态链接 `MobileGL_s`、trace replay 的可执行文件不在库目录）。配套：`MobileGLServer` 的 `RUNTIME_OUTPUT_DIRECTORY` 设为 `$<TARGET_FILE_DIR:MobileGL>`，每条新 ctest `ENVIRONMENT` 与 `add_trace_replay_test` 的 `SPLIT` 分支带 `MOBILEGL_IPC_SERVER_PATH`。
- 启动：`socketpair(AF_UNIX, SOCK_STREAM)` + `fork`/`execve`，fd 3 = socket。无文件系统 socket 路径、无 abstract namespace、Android 上无 SELinux 争议。
- **子进程强制 monolith**（修无界 fork 链——server stub `dlopen(libMobileGL.so)` 后必然走 `MG_Backend::Init()`，继承的 `MOBILEGL_TRANSPORT=spawn` 会再 spawn）：spawn 时构造显式 envp 剔除 `MOBILEGL_TRANSPORT` 与全部 `MOBILEGL_IPC_*`；`mobilegl_server_main` 在到达 `Init()` 之前把 `MG_Config::Transport` 硬置为 `Monolith`。两条都做。`MG_Test/Wire` 测试：spawn 一个 server，进程树只多出恰好一个子进程。
- `Hello{abi, backendType, buildFingerprint, configBlob}` → `Welcome{四个段}`。`configBlob` 转发 client 解析好的 `MG_Config::Features`，两半不可能对 quirk 开关有分歧；`buildFingerprint`（git hash + `PipeCalls.def` hash）不匹配 → 握手期 `Fatal{AbiMismatch}`。
- `mobilegl_server_main` 声明为 `extern "C" __attribute__((visibility("default")))`：非 Debug 构建设了 hidden visibility，而 FCL/plugin 出货的是 RelWithDebInfo，否则 `dlsym` 在设备上静默失败。

### 15.2 Android（spike A 已证）

- 交付链：APK 唯一可 exec 的位置是 `lib/<abi>/`，打包器只收 `lib*.so`，所以 server 以 `add_executable` + `PREFIX "lib"/SUFFIX ".so"` 构建（真 PIE），并把 `RUNTIME_OUTPUT_DIRECTORY` 指到 AGP 收集原生产物的 `CMAKE_LIBRARY_OUTPUT_DIRECTORY`（`CMakeLists.txt:784-808`，`MOBILEGL_BUILD_SERVER_SPIKE`）。**两台设备上都已证明**：从 `TraceReplayActivity` 自身的 `untrusted_app` 进程 `fork`+`execve` `<nativeLibraryDir>/libMobileGLServer.so`，子进程落在同一域、同一 MLS category，exit 0，零 avc denial（`MEASUREMENTS.md` §1）。
- `fork`+`execve` 而非 `posix_spawn`：bionic 从 API 28 才声明后者，minSdk 26（`android-plugin/app/src/trace/cpp/spawn_spike.cpp:63-68`）。fork 与 execve 之间只做 async-signal-safe 的 open/dup2/execve/write/_exit（父进程是多线程 JVM）。
- 应用进程的 stdout/stderr 是 `/dev/null`：子进程用 **marker 文件** 证明自己活过，exec 被拒的 errno 经 close-on-exec pipe 回传（EACCES 与 ENOEXEC 是完全不同的判决）。
- 生产 server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`。一份共享库、两个角色、版本必然匹配（Android 上那份库仍含 glslang/SPIRV-Cross，因为它同时服务 client；B 门检的是 server 侧代码有没有引用它们）。
- minSdk 26 没有公开 NDK API 能扁平化 `ANativeWindow`（`libbinder_ndk`、`ASurfaceControl` 都是 API 29）。**P5–P11 验证路径无窗口**：pbuffer 或 `AImageReader` 的 `ANativeWindow`，trace replay 默认 pbuffer。**P12 生产路径**：Java `Surface`（Parcelable）→ Messenger/AIDL → `MobileGLServerService`（`android:process=":mgl"`）→ JNI `ANativeWindow_fromSurface`（FCLauncher 今天在 `egl_bridge.c` 做的那一次调用）；仓内先例是 `android:process=":bench"` 的 `BenchService`。代价：server 进程多一个 ART（~15–25 MB）。FCL 把游戏 JVM 跑在主进程，第二个进程必须新建。
- `HeadlessGL` 的 fork 预检会 fork 一个子进程跑完整 EGL bring-up 然后 `_exit`——拆分模式下那个子进程会 spawn 一个孤儿 server。规则：server 的 EOF 检测**即时且无条件退出**（亚秒级）；client 的 socket fd 设成 `_exit` 会确定性关闭的形态；就绪握手有界重试。列为 P6 验收。
- 通用 env 透传 `--env K=V`（`run_android_retrace_local.py` → intent extra `mobilegl_env` → `trace_replay_core.cpp` 在加载 `libMobileGL.so` 前 `setenv`）已接进 retrace 通道，取代逐 knob 加 `--es/--ez`。

### 15.3 Linux / Windows / 崩溃

- Linux/X11：`Window` 是 XID，`nativeToken:u64` 直接送，backend 自己 `XOpenDisplay(getenv("DISPLAY"))`；Wayland 维持不支持。WSL/CI 永不开窗：`EGL_PLATFORM=surfaceless` + `EnsureHeadlessPlatform()`。
- Windows：`HWND` 进 `nativeToken`，Vulkan 可行，WGL/ANGLE-DXGI 对外进程 HWND 不受支持 → headless only。transport 默认 named pipe：asio `windows::stream_handle` 要求 overlapped 句柄，所以用 GUID 命名的 `CreateNamedPipeW(FILE_FLAG_OVERLAPPED)` + `CreateFileW(FILE_FLAG_OVERLAPPED)` 造句柄对再继承给 `CreateProcess`；AF_UNIX-everywhere 是可选简化。Windows 机器不是正确性门。macOS 不拆分（`CAMetalLayer` 无跨进程表示）。
- server 死：client 读到 EOF/EPIPE → device-lost 闩锁（GL 调用 no-op、`eglSwapBuffers` 返回 `EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus` 返回 `GL_UNKNOWN_CONTEXT_RESET`）；`MOBILEGL_IPC_RESPAWN=1` 时重启并全量重推（默认关，静默重启会掩盖 bug）。client 死：server 读到 EOF → 立即销毁原生 context 并退出；`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作最后保险。

## 16. 构建布局

```
MobileGL/MG_Pipe/                  永远进构建（monolith 的架构，不在任何 option 之后）         [P0]
MobileGL/MG_Impl/Pipe/             Tracker、SlotAllocator、CsoCache、HostResolve、CompositeResolver  [P2+]
MobileGL/MG_Backend/MGPipe/        PipeInputs.h + MGPipeImpl_DirectGLES/DirectVulkan.cpp             [P1+]
MobileGL/MG_Remote/                仅 MOBILEGL_BUILD_DISAGGREGATED
  Protocol/  protocol.fbs  generated/protocol_generated.h  mg_protocol_base.h                       [P0]
  Transport/ ITransport  InProcessTransport  Framing  Ring  ShmSegment(+Posix/Win32)  FdPassing  Doorbell  WireLog  [P0]
             SocketTransport                                                                        [P6]
  Client/    PipeEmitter  EmitTables  BackendObject_Remote  CapsMirror  ShadowArena  PersistentMapTracker  GpuWritePending  Surface/{X11,Win32,Android,Headless}  [P5+]
  Server/    PipeApplier  PipeObjectTables  IndexHostMirror  ServerLoop  ReplyPool  EventRing  ServerMain  [P5+]
  ServerJni.cpp                                                                                     [P12]
```

- CMake option（`CMakeLists.txt:23`）：`MOBILEGL_BUILD_DISAGGREGATED`（默认 OFF）追加 `MG_Remote/**` 进 `SOURCE_FILES`（`CMakeLists.txt:454-469`）并定义 `-DMOBILEGL_BUILD_DISAGGREGATED=1`；OFF 时 `MG_Config::Transport` 是 `constexpr Monolith`，`Init.cpp` 的分支编译期消失。`3rdparty/flatbuffers/include` 缺失时把 option 强制回 OFF 并 `message(WARNING)`（`CMakeLists.txt:440-451`）。`MobileGL` 与 `MobileGL_s` 都拿到同一份源。`MG_Test/Wire` 只在该 option 下注册（`MobileGL/MG_Test/CMakeLists.txt:93-95`）。
- `MOBILEGL_BUILD_DISAGGREGATED_INPROC`（尚不存在）：CI/调试形态，隐含开启前者，额外加角色隔离 shim。MGPipe 让需要角色分身的进程全局从四个（`pGLContext`、`gBackendFunctionsTable`、`pActiveBackendObject`、`pDefaultFramebufferInfo`）降到**两个**（pipe 表与 `pActiveBackendObject`）：server 角色不再读 `pGLContext`（三道纯度门就是这个断言），`pDefaultFramebufferInfo` 由保留句柄 `{0,1}` + `OnSurfaceChanged` 取代。两个 shim 都不在 GL 热路径的每次访问上——这是 `inproc` 从"成本可疑的实验"变成"可交付形态"的直接原因（Android 上 dlopen 的库无法可靠用 initial-exec TLS，`pGLContext->` 在 `MG_Impl` 有 1494 处）。
- `MobileGLServer`：桌面 `add_executable` 链接 `MobileGL_s`；Android `add_executable` 改名 `lib*.so` 链接共享 `MobileGL`，由 AGP 打进 `jniLibs`。
- `MOBILEGL_TRANSPORT = monolith | inproc | spawn | unix:<path> | pipe:<name>`（P5 起在 `ConfigLoader.cpp` 解析），免费换来 ctest `ENVIRONMENT` 变体、trace-replay 的 `setenv` 块、FCL 用户可编辑 env、plugin APK 的 V2 开关表、`/data/local/tmp` CTS 路径。
- 测试接线陷阱：ctest `ENVIRONMENT` 是替换而非追加、`;` 必须转义、property 覆盖 job env，必须用 `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` 构造；`add_trace_replay_test` 加 `SPLIT` 后缀（否则与同 case+backend 重名）并加 `-DTRACE_TRANSPORT=` 给 `run_trace_case.cmake` 消费。
- CI（`.github/workflows/test.yml:809` `pipe-gates`，P0 已落地）：`gen_pipe.py` 重生成 + diff；`MG_Backend`/`MG_State` 下禁止 stdio 插桩的 grep 门；`gen_pipe_dirty_surface.py --summary`（信息性，P1 成门）；`check_doc_citations.py`（警告级，文档定稿后 `--strict`）。独立 job `flatc-check`。后续：`include-graph-check`（P0.5）、`monolith-symbol-report`。

## 附 A：开关

CMake：

| 选项 | 默认 | 状态 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 已落地 |
| `MOBILEGL_BUILD_SERVER_SPIKE` | OFF（仅 Android） | 已落地（spike A，非出货） |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | 计划（P5） |
| `MOBILEGL_PIPE_VERIFY` | OFF | 计划（P1；构建期开关，编译进 `SnapshotFromGLContext()` 与 G4 比对器，P13 后保留） |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON（P2..P13） | 计划（编译期臂） |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 已落地（只服务 `flatc-check`） |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON（P7+） | 计划 |

运行时，MGPipe（`MobileGL/Config.h:319-358`，`MobileGL/ConfigLoader.cpp:245-256`，P0 已落地）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | 0 | 子系统位图（0 = 全 pull），含一位关闭 CSO 内容寻址；十进制或 `0x` |
| `MOBILEGL_PIPE_VERIFY` | 0 | 逐 draw 逐字段影子比对 |
| `MOBILEGL_PIPE_STATS` | 0 | 边界计数器（§附 B） |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON | 三态读取，只有显式 falsy 才关 |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | 0（0–4096） | 纹理拉取保留 LRU |
| `MOBILEGL_PIPE_INDEX_MIRROR_MB` | 64（0–4096） | 索引宿主镜像预算 |
| `MOBILEGL_PIPE_STATS_PERIOD` | 120（1–10⁶） | 每多少帧一条汇总行 |
| `MOBILEGL_PIPE_STATS_FILE` | 空 | teardown 时的 JSON 转储路径 |

运行时，传输与 IPC（计划，P5+）：`MOBILEGL_TRANSPORT`(monolith)、`MOBILEGL_IPC_SERVER_PATH`、`MOBILEGL_IPC_RING_MB`(8)、`MOBILEGL_IPC_STAGE_MB`(32)、`MOBILEGL_IPC_PRESENT_CREDIT`(1)、`MOBILEGL_IPC_SPIN_US`(50)、`MOBILEGL_IPC_POLL_ESCALATE`(64)、`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`(64)、`MOBILEGL_IPC_ADOPT_TIER`(auto)、`MOBILEGL_IPC_SHADOW_SHM`(1，Phase 2 起)、`MOBILEGL_IPC_INLINE_PAYLOADS`(0，负面对照)、`MOBILEGL_IPC_SERVER_AFFINITY`(auto)、`MOBILEGL_IPC_STRICT_ERRORS`(0)、`MOBILEGL_IPC_AUDIT`(0)、`MOBILEGL_IPC_TRACE`(0)、`MOBILEGL_IPC_ATTACH`、`MOBILEGL_IPC_RESPAWN`(0)、`MOBILEGL_IPC_IDLE_EXIT_S`(30)。显式不设立：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）、`MOBILEGL_IPC_VALIDATE_SERVER`（server 没有 `MG_Impl` 校验器）。既有负面对照开关（`MOBILEGL_ESPRYT_DISABLE_{UBO,UNPACK,UPLOAD}_RING`、`_INVALIDATE_FLUSH`、`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`、`MOBILEGL_COHERENT_AS_FLUSH`）全部保留。

## 附 B：边界计数器（`MobileGL/MG_Util/Metrics/PipeStats.h:46-122`，P0 已落地）

关闭时每站点一次全局 load + 一条永不命中的分支。字节类：`stage-buffer`、`stage-texture`、`stage-ubo-global`、`stage-ubo-named`（只有 Magma 贡献，D-B8 的不对称）、`stage-vertex-client`、`stage-index-client`、`stage-indirect-cmd`（Espryt 独有）、`persistent-map-push`（P0 未接线：monolith 期不存在推送）、`residual-value-block`（占位）。调用类：`draws`、`accessor-calls`（实际执行的 GLContext accessor 次数，在约 10 个热入口做静态计数，是**下界**）、`texture-upload-emissions/box/rect/jobs`。六个 memo 门（`SyncRenderState` 早退、`SyncNeccessaryTextures` 键比较、`CurrentUnitBindingsEpoch` 快门、`TrySetupDrawFastPath`、pipeline memo、`ApplyDynamicDrawStateTail`）各计 hit/miss。每 draw payload 直方图（24 桶）已实现，等第一个发射器接入。每 `MOBILEGL_PIPE_STATS_PERIOD` 帧一条 `MGPipe stats:` 汇总行（`MGLOG_I`），`TRACY_ENABLE` 下逐帧 `TracyPlot`，teardown 时可选 JSON。站点清单——哪些路径**没有**接线——写在 `MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`，那份清单是契约。
