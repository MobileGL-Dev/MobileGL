## 4. 接口设计：MGPipe

### 4.1 文件布局与单一真相源

```
MobileGL/MG_Pipe/                      # client 与 server 都 include；不链接 MG_State，不链接 MG_Impl
    PipeCalls.def                      # X-macro：调用目录的唯一真相源，一行一个调用
    MGPipe.h                           # 由 .def 生成的两张函数表 + 手写 payload 声明
    MGPipeTypes.h                      # 全部 payload POD（trivially copyable，逐个 static_assert）
    MGPipeHandles.h                    # MGPipeHandle、MGPipeKind、保留 handle、slot 分配契约
    MGPipeHostSpan.h                   # 唯一一个"形状随传输而变"的访问器（§4.5.7）
    MGPipeCallbacks.h                  # 反向通道（事件/回复）的函数表，见 §7
    generated/PipeTables.inc           # G1：两张函数表
    generated/PipeThunks.inc           # G2：monolith 直调 thunk
    generated/PipeWire.inc             # G3：wire 记录 + static_assert + 运行期边界检查 + applier switch
    generated/PipeVerify.inc           # G4：逐字段影子比对器
    generated/PipeFilled.inc           # G5：written-once 位图与 poison 断言
    generated/PipeCoverage.inc         # G6：477 读点 → MGPipe 调用的映射表（CI 重生成 + git diff --exit-code）
MobileGL/MG_Impl/Pipe/
    Tracker.{h,cpp}                    # st_validate_state 类比物
    SlotAllocator.{h,cpp}  CsoCache.{h,cpp}
    HostResolve.cpp                    # 客户端数组界限 / 索引扫描 / restart 重写 / indirect count 解析
    CompositeResolver.cpp              # program pipeline 合成体的 handle 生命周期
MobileGL/MG_Backend/MGPipe/
    PipeInputs.h                       # backend 私有的"被推送状态"块（迁移载体，§6.2）
    MGPipeImpl_DirectGLES.cpp          # 用 Espryt 的函数填 MGPipeContext
    MGPipeImpl_DirectVulkan.cpp        # 用 Magma 的函数填 MGPipeContext
MobileGL/MG_Remote/                    # 传输，继承 PLAN.md §13（删掉 Server/ReplicaContext.*）
    Server/PipeApplier.cpp  Server/PipeObjectTables.{h,cpp}
scripts/gen_pipe.py                    # 跑 G1..G6
```

`PipeCalls.def` 一行一个调用，**六个生成器**消费它：

```cpp
// MG_Pipe/PipeCalls.def   —  X(Name, PayloadStruct, Class, Flags)
//   Class : kScreen | kCtxCso | kCtxState | kCtxObject | kCtxVerb | kCtxQuery
//   Flags : kNone | kNeedsAck | kHasBlob | kVarTail | kHostSpan | kReplySlot | kOptional
#define MGP_CALL_LIST(X)                                                          \
  /* ---- screen ---- */                                                          \
  X(GetCaps,             MGPCaps,             kScreen,   kReplySlot)              \
  X(ResourceCreate,      MGPResourceDesc,     kScreen,   kNone)                   \
  X(ResourceRespecify,   MGPResourceDesc,     kScreen,   kNone)                   \
  X(ResourceDestroy,     MGPHandleOnly,       kScreen,   kNone)                   \
  X(MapPersistent,       MGPHandleOnly,       kScreen,   kReplySlot|kOptional)    \
  /* ---- CSO ---- */                                                             \
  X(CreateRenderState,   MGPRenderStateDesc,  kCtxCso,   kHasBlob)                \
  X(BindRenderState,     MGPBindRenderState,  kCtxCso,   kNone)                   \
  /* ---- state ---- */                                                           \
  X(SetFramebufferState, MGPFramebufferState, kCtxState, kNone)                   \
  X(SetSamplerViews,     MGPSamplerViews,     kCtxState, kVarTail)                \
  /* ---- verb ---- */                                                            \
  X(DrawVbo,             MGPDrawInfo,         kCtxVerb,  kHostSpan|kVarTail)      \
  X(ResourceSubData,     MGPSubData,          kCtxObject,kHasBlob)                \
  X(RenderbufferStorage, MGPRbStorage,        kCtxObject,kNeedsAck)               \
  /* … 共约 72 项，完整目录见 §4.4 与 part4 的速查表 … */
```

| 生成器 | 产物 | 替代/新增 |
|---|---|---|
| **G1** | `struct MGPipeScreen { … };` / `struct MGPipeContext { void (*DrawVbo)(const MGPDrawInfo*, …); … };` | 替代今天手写的 `GLFunctionsTable` |
| **G2** | monolith thunk：`inline void MGP_DrawVbo(const MGPDrawInfo* p){ gPipeCtx.DrawVbo(p); }` — 一次**已经在付**的间接调用 | 替代 `gBackendFunctionsTable.GL.*`（89 个 MG_Impl 站点改名即可） |
| **G3** | wire 记录结构 + 每种一条 `static_assert(sizeof==N)` + applier 分发前的运行期边界检查 → `Fatal{ProtocolCorruption}` | 继承并扩展 `PLAN.md` §6.3 的 `Records.def` 机制到**全部**调用 |
| **G4** | `MOBILEGL_PIPE_VERIFY` 的逐字段比对器：对每个 `kCtxState`/`kCtxCso` 调用生成 `Compare(pushed, snapshot, &firstDivergentFieldName)` | **新增**：这是每份候选设计都被判缺失的语义绊线 |
| **G5** | `PipeInputs::m_filledMask` 的位定义 + 读未填字段时的 `Fatal{UnmigratedPipeInput, "<field>"}` | **新增**：完整性的**运行期/构建期**绊线，不只是测试绊线 |
| **G6** | 477 行读点清单 → MGPipe 调用的映射，CI 重生成并 `git diff --exit-code`，0 UNMAPPED | 改造自 `Feat/CS-Delta-IPC` 的 `extract_backend_read_inventory.py`（删掉制造"0 UNMAPPED"的前缀兜底规则） |

**G4 与 G5 从同一份 `.def` 生成，因此不可能与调用目录漂移。** 这是把"完整性只有测试绊线、没有构建绊线"这条历史评审结论一次性关掉的机制。

**接口表用函数指针 struct，不用虚基类。** 三条本仓库自己的理由：(1) 边界今天**就是**函数指针 struct，装在 `MG_Backend/Init.cpp:44` 的唯一 hook 点上；(2) `nullptr` 项**已经**表示"未实现，前端回退"，写进 `BackendObject.h:212-215` 与 `:265-269`，而 DirectVulkan 确实留空 8 项——**一个 null `set_*` 恰好就是"这个子系统还没迁移，继续拉取"**，这是 strangler 的原生表达，纯虚类只能用说谎的 stub override 来模拟；(3) `MG_Test` 已经会替换这张表做 mock。稀有的 EGL/caps 面继续留在 `pActiveBackendObject` 的虚函数上（20 个方法，`BackendObject.h:543-568, 597-598`），它们本来就罕见。

### 4.2 对象模型

#### 4.2.1 Handle

```cpp
// MG_Pipe/MGPipeHandles.h
enum class MGPipeKind : Uint8 {
    Buffer=1, Texture, Renderbuffer, Framebuffer, Xfb,
    RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso,
    Fence, Query, Context
};
struct MGPipeHandle {          // 8 B，POD，可哈希，按值传寄存器对
    Uint32 slot;               // 按 kind 稠密分配（client 侧 SlotAllocator）
    Uint32 gen;                // slot 复用时 ++；client 侧维护
};
```

- **slot 稠密、按 kind 分配**。这一条是把 server 的对象表从哈希表变成**数组**的唯一途径，也是删掉 `TwinLookupMemo` 的斐波那契散列 + owner 相等性检查的原因。`SlotAllocator` 是一个 free-list + 高水位，与 `IndexGenerator` 无关（后者的 LIFO 复用正是问题本身）。
- **`gen` 只在 slot 复用时 ++，不是每次 respecify 都 ++**。`{slot, gen}` 在**同一个 slot 被复用 2³² 次**之前唯一。文档写明这条上界，debug 构建断言它。
- **GL name 只在 `resource_create` 的 payload 里出现一次，纯诊断用途**，永不做身份、永不进任何 memo 键、永不进任何 content hash。
- **`GetLifetimeId()` 留在 client 侧**作为 tracker 自己的身份（它已经是永不复用的），不过线。client 侧维护 `lifetimeId → slot` 映射。
- **保留 handle**：`{0,0}` = null；`{slot=0, gen=1, kind=Framebuffer}` = 默认帧缓冲（这一条退役 `DirectGLES.cpp:1917, 2838, 2867, 9675` 四处对 `pDefaultFramebufferInfo->defaultFBO` 的身份比较）；`ShaderCso` 的高 1/16 slot 段保留给 **program pipeline 合成体**（§5.6），因为它们没有 GL name。

#### 4.2.2 两种 generation，严格分开

| | 拥有者 | 回答什么 | 是否过线 |
|---|---|---|---|
| **身份**（`MGPipeHandle::gen`） | client | "还是同一个 GL 对象吗？" | 是，随每个 handle |
| **`MGGen`**（server 纪元） | **server** | "**我自己**是不是重铸了驱动对象 / 冲了自己的缓存？" | **client→server 永不；server→client 只以纹理拉取请求的形式出现**（§7.5） |

`MGGen` 是 §0.3 推论 3 那 12 个计数器的 wire 名字。**接口规范条款：任何 MGPipe 调用都不得要求 client 提供或知晓 `MGGen`。** 这条是"server 拥有自己的状态机"的可执行定义。反过来也是规范：**client 侧的版本计数器永远不是新鲜度的唯一证明**——每一个回绕的 `Uint16`（§2.6）在过线时要么被加宽到 32 位、要么与 `{slot, gen}` 同行。

#### 4.2.3 CSO vs 可变对象

| 类别 | 形态 | 因为 backend 今天就是这么缓存的 |
|---|---|---|
| `VertexElementsCso` | `create/bind/delete` | `VertexInputStateFactory::m_cache`，键正是 `VertexAttribute` 那组字段的 content hash（`VertexInputStateFactory.cpp:19-50`） |
| `SamplerCso` | `create/bind/delete` | `VkSamplerManager::m_samplers`（`VkSamplerManager.h:104-117`）；Espryt 的 `BackendSamplerObject`（`Managers.h:1808-1824`） |
| `SamplerViewCso` | `create/delete` + 由 `set_sampler_views` 绑定 | `TextureResource::{perMipViews, perMipSampledViews, attachmentViews, alternateSampledViews, storageImageViews}`（`VkTextureManager.h:173-370`）；Espryt 的 `SyncTextureViewToBackend`（`Managers.cpp:3616-3707`） |
| `ShaderCso` | `create/bind/delete` + **server 侧惰性特化**（D-B2） | `ProgramFactory::m_cache`（`ProgramFactory.h:614-642`）；`BackendProgramObjectImpl`（`Managers.h:1473-1725`） |
| `RenderStateCso` | `create/bind/delete`，**payload 是整块 blob**（D-B1） | Espryt 的值镜像 + 三段 memcmp；Magma 的 `ComputePipelineStateHash` 单次 bulk fetch |
| Buffer / Texture / Renderbuffer | `create` / `respecify` / `subdata` / `destroy`（可变，带存储） | `GLESBufferResource`、`BackendTextureObject`、`VkBufferResource`、`TextureResource` |
| Framebuffer / Xfb | per-context 身份 + `set_*` payload（不带存储） | `BackendFramebufferObject`、`m_xfbCounterSlotByObject` |

**CSO 在 client 侧内容寻址**（Mesa `cso_context`/`cso_cache` 的先例）：`MG_Impl/Pipe/CsoCache` 每类一张 `ska::flat_hash_map<Uint64 xxHash, MGPipeHandle>`，容量上限（blend/DS/rasterizer 合并后的 render-state 64、vertex-elements 1024、sampler 256、sampler-view 4096、shader 不设上限跟随 `ProgramObject` 生命周期），LRU 淘汰时发 `delete_*_state`。**内容寻址的一个直接收益**：两个不同 program 设置了相同的 blend 状态时，server 侧**零状态转换**。

**任何 `create_*` 都不返回 server 铸造的 handle。** 这是对 gallium 的**有意偏离**（gallium 的 `create_*_state` 返回 driver 指针），也是这份目录能在**零创建 round trip** 下远程化的根本原因。今天的 `BackendSyncHandle`/`BackendQueryHandle = void*`（`BackendObject.h:110, 115`）随之变成 `MGPipeHandle`——fence 与 query 需要的唯一改动。

### 4.3 `MGPipeScreen` 与 `MGPipeContext`

GL 的 share-group 规则本身就是分界，而 MobileGL 的容器归属**已经**吻合：

| `MGPipeScreen`（share group） | `MGPipeContext` |
|---|---|
| caps、format 能力表、renderer 字符串；buffer / texture / renderbuffer / sampler / shader 的对象命名空间；fence | 全部 `set_*`、全部 CSO 绑定、VAO / FBO / XFB 对象 / query 的命名空间、命令流、present |

v1 只有一个 screen、一个 context、一条 flow（`shareCtx` 是死状态，§1.2）。**但两张表从第一天就分开**，因为事后拆分意味着给每个记录种类重新编号。两处必须重新归类的事实：`GetTextureBindGeneration()` 与 `GetSamplingResolutionGeneration()`（`Core.h:130, 136`）是**绑定**（context）事实却住在 share-group 作用域的 `TextureState` 里；`GetTextureContextId()`（`:143`）已经存在正是为了在 context 重建时区分它们——它直接**就是** context handle。

### 4.4 完整调用目录

> 完整速查表（含 payload 大小与 flags）在 part4 的附录。这里给出结构与每项的来源/取代关系。

#### 4.4.1 `MGPipeScreen`（14 项）

| 调用 | payload | 取代 |
|---|---|---|
| `get_caps(MGPCaps* out)` | `DynamicBackendParameters`（`BackendObject.h:302-522`，~90 标量，平坦 POD）+ `RendererInfo` + `FormatCapabilityCache`（`:88-99`，14 个能力位 × 全部 internalformat × 12 target 的稠密表 + 每格 `SampleCounts`）+ `callMask` | 40 个 `pActiveBackendObject->` 站点、89 个 caps 读点 |
| `resource_create(h, const MGPResourceDesc*)` | §4.5.1 | buffer/texture/renderbuffer 的创建 |
| `resource_respecify(h, const MGPResourceDesc*)` | 同上 | `BufferBackendOps::Respecify`（`BufferObject.h:80`）泛化；server 自己按 busy 跟踪决定 swap-vs-in-place |
| `resource_destroy(h)` | handle | `OnDestroy`（`:101`）+ **两个 `WeakPtr` GC 扫描**（`Managers.h:353-390`、`VkTextureManager.cpp:1694-1720`）。server 的 pooling / 延迟释放（`Managers.cpp:1271-1300`、`VkBufferManager::OnResourceDestroyed`）原样保留 |
| `map_persistent(h) → MGPMapResult` / `unmap_persistent(h)` | — | `AcquirePersistentMap`（`:112`）。**改造期不碰**（D-B4） |
| `fence_create/status/wait/destroy` | handle (+timeout) | `FenceSync`…`GetSyncStatus`（`:220-224`）。两值契约（`:243-249` "false = 还没好，留着 handle"）**逐字保留** |
| `query_create/begin/end/available/result/destroy` | handle + kind | `BackendObject.h:230-256` |
| EGL 生命周期 8 项 | 见 `BackendObject.h:548-559` | 原样保留为虚函数（罕见） |

**`callMask` 取代"槽位是否为 null"这个隐式能力探测**（`GL_Query.cpp:471, 545, 768` 拿 `BeginOcclusionQuery != nullptr` 当能力位）。它同时是 emulation 归属的判据（§5.7）：`kCapPrimitiveRestart`、`kCapPrimitiveRestartFixedIndex`、`kCapMultiDraw`、`kCapMultiDrawIndirect`、`kCapMultiDrawIndirectCount`、`kCapViewportArray`、`kCapFloat64VertexAttrib`、`kCapResidentSubData`、`kCapCpuXfbPrimitiveAccounting`、`kCapNeedsHostIndexBytes`、`kCapTimerQuery`、`kCapOcclusionQuery`、`kCapXfbPrimitivesQuery`。

#### 4.4.2 `MGPipeContext` — CSO（15 项）

`create/bind/delete` × { `render_state`, `vertex_elements`, `sampler`, `sampler_view`, `shader` }。payload 见 §4.5.2-4.5.5。

#### 4.4.3 `MGPipeContext` — `set_*`（14 项）

| 调用 | 取代的拉取点 |
|---|---|
| `set_framebuffer_state` | `GetFramebufferBindingSlot` ×19、`GetAllAttachmentObjects`、`GetDrawBuffers`、`GetReadBuffer`、4 处 `pDefaultFramebufferInfo` |
| `set_vertex_buffers(start, count, const MGPVertexBuffer*)` | VAO binding-point 走查 |
| `set_index_buffer(const MGPIndexBuffer*)` | `GetIndexBufferBindingSlot`；**独立调用**——VAO config version 不是它的超集（D5） |
| `set_indirect_buffers(drawIndirect, parameter)` | `GetBufferBindingSlot(DrawIndirect/Parameter)` |
| `set_sampler_views(stage, start, count, const MGPBoundView*)` | `GetTextureUnitObject` ×19、`GetActiveTextureUnit` ×8、`GetTextureBindGeneration` ×5。**client 侧已解析**（§5.5） |
| `bind_sampler_states(stage, start, count, const MGPipeHandle*)` | `TextureUnit.h:394` |
| `set_shader_images(start, count, const MGPImageView*)` | `GetImageTextureBinding` ×14；**这一条退役 `ImageUnitFormatsStillMatch`**（`Managers.cpp:6545-6573`） |
| `set_shader_buffers(cls, start, count, const MGPBufferRange*, writableMask)` | `GetBufferBindingPoint` ×19、`GetTouchedBufferBindingPointCount` ×2。`cls` ∈ {Uniform, ShaderStorage, AtomicCounter} |
| `set_stream_output_targets(count, const MGPBufferRange*, const Uint32* offsets, Uint64 generation)` | XFB 绑定走查 |
| `set_global_constants(shaderCso, MGPBlobRef, Uint32 version)` | `MapUBO`/`GetUBOData`/`GetUBOSize`/`GetUBOContentVersion`（§4.6 D6） |
| `set_vertex_attrib_defaults(Uint32 mask, const MGPAttribValue*)` | `GetCurrentVertexAttribute` ×2；float/int/uint 视图由 `ClassifyVertexAttribType`（`Core.h:51`）在 client 侧解析 |
| `set_pixel_pack_state(const PixelStoreParameters*)` | 6 个 PACK 读点。**没有 unpack 对应项**（§4.6 D5） |
| `set_patch_state(Uint32 vertices, const Float outer[4], const Float inner[2])` | `GetPatchVertices`/`…OuterLevel`/`…InnerLevel` ×6。**同时是 shader variant 输入**（`RenderState.h:243-249`） |
| `set_draw_program(shaderCso)` / `set_dispatch_program(shaderCso)` | `GetProgramForDraw` ×7、`GetProgramForDispatch` ×3。含 composite（§5.6） |

**迁移期额外一项（显式临时）**：`set_residual_value_state(MGPBlobRef)`，见 §6.3。它带一个**编译错误式退役绊线**。

#### 4.4.4 `MGPipeContext` — transfer（12 项）

`resource_subdata`（buffer + texture 同一形状，**同时携带 union box 与 rect 列表**，§4.5.6）、`resource_flush_range(h, Range1D, Flags<BufferMappingAccessBit>)`（携带应用**真实**的 access flags，`BufferObject.h:94-96`）、`resource_readback(h, off, size, MGPReplySlot)`、`resource_copy_region`、`blit`、`clear`（一条，判别式合并今天的 `Clear` + 4 个 `ClearBuffer*` + 4 个 `ClearNamedFramebuffer*`）、`generate_mipmap(h, target, const MGPMipPlan*)`、`read_pixels(const MGPReadbackInfo*, MGPReplySlot)`、`get_texture_image(...)`、`buffer_subdata_resident(h, off, MGPBlobRef)`（**可为 null**，见下）。

**`buffer_subdata_resident` 的 per-backend 可选性必须被接口允许，不能抹平。** Espryt 注册它、Magma 故意不注册（`VkBufferManager.cpp:104-111`），差别是 `glBufferSubData` 在活的 coherent map 上的排序语义（`BufferObject.h:84-92` 的 Minecraft 撕裂 postmortem）。表现为 `kCapResidentSubData` 位 + null 项。**统一强制的接口会静默改变 Magma 的行为并发布一个 Minecraft chunk 撕裂。**

#### 4.4.5 `MGPipeContext` — 命令（4 + 4 + 2 项）

```cpp
void draw_vbo (const MGPDrawInfo*, Uint32 drawIdOffset,
               const MGPDrawIndirect*, const MGPDrawRange*, Uint numDraws);
void launch_grid(const MGPGridInfo*);      // + indirect 变体字段
void memory_barrier(GLbitfield bits, Bool byRegion);
void begin_stream_output(GLenum primitiveMode);
void end_stream_output(const MGPXfbAccounting*);
void pause_stream_output();  void resume_stream_output();
void flush(Uint32 flags);                  // GL_SYNC_FLUSH_COMMANDS_BIT 语义
void present(Uint64 frameSerial);  void set_swap_interval(Int interval);   // 后者可 null（Magma）
```

**今天 20 个 draw 入口塌成 `draw_vbo` 一条**——这正是 gallium 的合并，而 `MGPDrawRange[]` **就是** `MultiDraw*` 族今天的形状（gallium 的 `pipe_draw_start_count_bias`）。

#### 4.4.6 显式删除、不移植的项

- `GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`（`BackendObject.h:195-197`）。`DirectGLES.cpp:7264-7386` 的 15 个 case 完全不碰 GL（占 Espryt 124 个读点里的 14 个）；Magma 同形（`DirectVulkan.cpp:685, 759`），只有 `GL_COMPUTE_WORK_GROUP_SIZE`（`:790-795`）是真后端答案，进 `MGPCaps`。
- `ShaderStorageBlockBinding`（`:207-208`）→ 折进 `MGPProgramDesc` 的反射归档（`LinkArtifacts::shaderStorageBlockBinding` 按**名字**索引，`ProgramObject.h:1325`）。它自己的注释已经说 program interface 由前端回答。
- **总规则：server 不回答任何 client 能自己回答的问题；剩下的每个 server 查询都是 async-with-handle，绝不阻塞。**

### 4.5 关键 payload

#### 4.5.1 `MGPResourceDesc`（判别式，三种 GL 存储类合一）

```cpp
struct MGPResourceDesc {
    Uint8  target;            // Buffer | Tex1D..TexCubeArray | Tex2DMS.. | Renderbuffer | TexBuffer
    Uint8  storageKind;       // Mipmap | Buffer   (== TextureStorageType, TextureEnum.h:61-64)
    Uint16 bindMask;          // VERTEX|INDEX|CONSTANT|SHADER_BUFFER|INDIRECT|SAMPLER|SHADER_IMAGE|
                              //   RENDER_TARGET|DEPTH_STENCIL|STREAM_OUTPUT|ATOMIC   (== PIPE_BIND_*)
    Uint32 internalFormat;    // GL internalformat，已在前端解析为非压缩后备
    Uint32 width, height, depth;
    Uint16 arrayLayers, levels, samples;
    Uint8  fixedSampleLocations, immutable;
    Uint32 usage;             // BufferUsage
    Uint32 storageFlags;      // glBufferStorage flags
    Uint8  hasDefinedContent;  // NULL-data respecify（orphaning 惯用法）之后为 false，BufferObject.h:216
    Uint8  imageBindableHint;  // client 侧 everImageBound，预防性分配（§7.5(a)）
    Uint8  glNameForDiag[2];   // 仅诊断
    MGPipeHandle viewOf;       // 纹理视图的存储属主（GetViewStorageOwner，TextureObject.h:100）
    MGPipeHandle bufferForTexBuffer;  Uint64 bufOffset, bufSize;   // kWholeBuffer = ~0，实时解析
};
```

**Renderbuffer 保持独立类，不折进纹理。** 它有自己的 format-capability target 索引（`BackendObject.h:85`）、自己的 `ComponentSizes` 上报（`RenderbufferObject.h:37-43`）、自己的 twin（`Managers.h:1838`）。合并零收益且丢掉一张独立能力表。`bindMask` 让它在**绑定语义**上与纹理统一，这就够了。

#### 4.5.2 `MGPRenderStateDesc` / `MGPBindRenderState`（D-B1）

```cpp
struct MGPRenderStateDesc {          // create：整块，每个唯一状态一次
    MGPipeHandle cso;
    Uint8  spanMask;                 // bit0=head, bit1=blend[], bit2=tail；未命中时只发变化段
    MGPipeHandle baseCso;            // 增量基（spanMask != 0b111 时有效）
    MGPBlobRef   blob;               // RenderStateParameters 的对应段
};
struct MGPBindRenderState {          // bind：稳态 12 B
    MGPipeHandle cso;  Uint16 version;  Uint16 pipelineVersion;
};
```
段划分**与 `DirectGLES.cpp:2038-2047` 现有的 head/blend/tail 完全一致**，所以两侧对"段"的定义按构造相同。两个版本号都过线（`RenderState.h:522` / `:529`）。

#### 4.5.3 `MGPVertexElements`

携带**两个视图，缺一不可**：解析后的 `VertexAttribute[32]`（`VertexArrayObject.h:17-53`）**和** `VertexBufferBindingPoint`（`:58-64`，注意初始 stride 是 **16** 而不是 0，`:61-62`）。`VertexArrayObject.h:22-29` 记录了合并它们的代价：pointer 调用的 stride 0 被解析成 element size，而 binding-model 的 stride 0 意味着每个顶点读**同一个** element，塌成一个害了 `KHR-GL43.vertex_attrib_binding.basic-input-case7/8`。`IsLong` 与 `Type == Float64` **分开携带**（`:34-39`：`glVertexAttribFormat(GL_DOUBLE)` 读 double 转 float，`glVertexAttribLFormat` 保留 64 位，`GL_VERTEX_ATTRIB_ARRAY_LONG` 上报这个区别）。**仅供查询的 `LegacyStride`/`LegacyPointer`（`:51-52`）留在 client，永不过线。**

#### 4.5.4 `SamplerParameters` 与 `MGPSamplerView`

`SamplerParameters`（`SamplerObject.h:468-492`）**逐字节原样过线，包括 `borderColorForm`**（`:462-466`）：`:455-461` 明说没有它 backend 无法在 `glSamplerParameterIiv` 与 `fv` 之间、或在 `VkBorderColor` 家族之间选择，因为三种表示（`borderColor`/`borderColorI`/`borderColorUI`）**永远都被数值填满**。

```cpp
struct MGPSamplerView {              // = pipe_sampler_view
    MGPipeHandle cso, texture;
    Uint32 internalFormat;           // 别名格式（glTextureView）
    Uint8  target, depthStencilMode, pad[2];
    Uint16 minLevel, numLevels, minLayer, numLayers;
    Uint8  swizzle[4];
    Uint16 samples;  Uint8 fixedSampleLocations, pad2;
};
```
**`glTextureView` 因此从"ES name 重铸特殊路径"（`Managers.cpp:3616-3707`）变成一个普通 view CSO**，这对两个 backend 都是净简化。真正需要保留的区别是 `GetViewStorageOwner()`（`TextureObject.h:96-100`，一个 `SharedPtr`，且**它自己永远不是 view**）——它变成 `resource_create` 的 `viewOf` + server 侧 keep-alive。

#### 4.5.5 `MGPProgramDesc`（`create_shader_state` 的 payload）

```cpp
struct MGPProgramDesc {
    MGPipeHandle cso;
    Uint32     stageMask;            // == GetLinkedShaderStages()
    MGPBlobRef spirv[6];             // GetGeneratedSpirv()，逐 stage，顺序与 GetLinkedShaderSnapshot() 一致
    MGPBlobRef reflection;           // Visit() 归档的 LinkArtifacts + SpirvArtifacts（全结构体）
    Uint32     globalUboSize;        // GetUBOSize()
    Uint32     reservedNumSamplesOffset;
    Uint8      spirvStatus, nativeFloat64, pointSizeDemoted, enableSpirvValidation;
};
```

反射归档**序列化整个结构体，而不是 backend 今天读的那 ~40 个字段**——这样 backend 新增一次 read 永不需要改协议。机制沿用 `PLAN.md` §6.9 的 `Visit()` + `sizeof` 绊线，但**用途改变**：它不再是"分歧预言机"（没有可分歧的对象），而是**schema 完整性绊线**：

```cpp
// MG_State/GLState/ProgramState/ProgramArtifactsArchive.h
template <class Ar> void Visit(Ar& ar, LinkArtifacts& a) { ar(a.writtenUniformLocationBits, /*…全字段…*/); }
static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
              "新字段请加进 Visit() 并 bump MGL_LINKARTIFACTS_SIZE");
```

归档必须覆盖：`uniformReflection`/`blockReflection`/`pipeInputReflection`/`pipeOutputReflection`（`ProgramObject.h:1210-1395`，各带 `TypeFacts`）、`uniformSamplerOrImageUnitIndex`（`:1298`）、`uniformBlockBinding`（`:1314`）、`shaderStorageBlockBinding`（按名字，`:1325`）、`explicitOpaqueUniformBindings`（`:1303`）、`xfbVaryings`/`xfbStrides`/`xfbPackedStride`/`xfbNeedsScatteredCapture`（`:1357-1394`）、`computeLocalSize`、GS/TCS/TES 事实（`:1373-1388`）、`usesReservedNumSamples`（`:1345`）、`uniformOffsets`（`:1416`）。

**`XfbVarying`（`:1146-1171`）必须带两套拼写**：GL 名字（供 Espryt 的 ESSL 驱动侧捕获列表）**和** `blockInstanceName`/`blockName`/`blockMemberIndex`/`blockMemberElement`（`:1163-1170`），因为 SPIR-V 装饰的是 block 的实例变量与成员索引，无法装饰单个数组元素。

#### 4.5.6 `MGPFramebufferState` 与 `MGPSubData`

```cpp
struct MGPSurface {                  // = pipe_surface
    MGPipeHandle res;
    Uint32 internalFormat;           // 内联！让四个跨对象 mask 在推送时刻零查表推出
    Uint8  kind;                     // Texture | Renderbuffer | None
    Uint8  layered;  Uint16 level;
    Uint32 layer;    Uint16 uploadTarget;  Uint16 pad;
};
struct MGPFramebufferState {
    MGPipeHandle fbo;                // {0,1} = 默认帧缓冲
    MGPSurface   color[8], depth, stencil;
    MGPSurface   readSurface;        // *** client 侧已解析的读表面，不是索引 ***
    Int8   drawBuffers[8];           // attachment 索引，-1 = NONE
    Uint16 width, height, layers, samples;
    Uint8  fixedSampleLocations, isDefault, complete, pad;
    Uint64 contentHash;              // client 计算；server 的 render-pass memo 键
};
```

三点值得单独说：

1. **`readSurface` 是 client 解析后的表面，不是 `glReadBuffer` 的索引。** 这一条**按结构消灭** read-buffer-shared-FBO 缺陷类（同一个 FBO 既做 draw 又做 read 时跳过 read-buffer 同步，导致回读全取 attachment 0）。
2. **`internalFormat` 内联在每个 `MGPSurface` 里**，所以四个跨对象 mask（`g_snormFallbackClampOutputMask`、`g_unormFallbackClampOutputMask`、`g_alphaWidenedDrawBufferMask`、`g_integerColorDrawBufferMask`，`Managers.cpp:5616-5619`）在 `set_framebuffer_state` 内部零查表推出。配合 D-B3 的固定顺序，`DirectGLES.cpp:2712-2732` 的 fragColor 重推导 workaround 与 `g_broadcastMemo*` 一起删除。
3. **`contentHash` 是全表最有价值的一个字段**：它用一次 64 位比较取代 D7 的四元组同步戳与 D15 的 `(pointer, lifetimeId, Uint16 version)` 三元组，由一个本来就在跟踪全部输入的 client 计算。attachment enum 到 Color31（`FramebufferObject.h:65-66`）而 `MAX_DRAW_BUFFERS` 是 8（`:106`）——surface 数组是 8，其余由 `drawBuffers[]` 的解析结果承载。`ARB_framebuffer_no_attachments` 的默认值是独立字段（`:134-138`）。

```cpp
struct MGPSubData {
    MGPipeHandle res;
    Uint16 target, level;
    MGPBox box;                       // union box
    Uint32 rectCount;                 // rect 列表在变长尾
    MGPBlobRef blob;
};
```
**同时携带 union box 与 rect 列表，由 server 选上传形状。** 这不是冗余：Mali 按**作业数**给纹理上传计价，本项目实测 ~100 个精灵 rect 对一个 union box 的差是 **+6 ms/frame**（`Managers.cpp:4311-4319`）。client 按 `MipmapStorage::GetDirtyRects` 的语义产生区域形状（96-rect 级联合并 + `summedArea*4 >= unionArea*3` 回退，`MipmapStorage.cpp:300-305`），**决策留在付 GPU 代价的那一侧**。

#### 4.5.7 `MGPDrawInfo` 与 `MGHostSpan`

```cpp
struct MGPDrawInfo {                  // = pipe_draw_info，GL 有对应概念的字段逐一对齐
    Uint32 mode;
    Uint8  indexSize;                 // 0 = arrays，否则 1/2/4
    Uint8  flags;                     // kHasUserIndices | kPrimitiveRestart | kRestartRewritten |
                                      //   kXfbCountUnknown | kIndicesAreClient
    Uint16 pad;
    Uint32 instanceCount, startInstance;
    Uint32 restartIndex;
    Uint32 minIndex, maxIndex;        // client 计算（gallium 有这两个字段）；~0 = 未知
    MGPipeHandle indexResource;
    MGHostSpan   userIndices;         // = pipe_draw_info::index 的 union
    Uint64 xfbCpuCapturedVertices;    // GetTransformFeedbackCapturedVertices()，给 scatter 路径定容量
};
struct MGPDrawRange { Uint32 start, count; Int32 indexBias; };   // = pipe_draw_start_count_bias
```

**`MGHostSpan` 是整份接口里唯一一个"形状随传输而变"的东西**，它把 backend 今天在 draw 时刻解引用**客户端字节**的四个类统一成一个访问器：

```cpp
struct MGHostSpan {                   // 32 B
    const void* ptr;                  // monolith：指向前端 shadow / 应用内存。split：nullptr
    Uint64      size;
    Uint32      seg;                  // split：SEG_STAGE id
    Uint32      pad;
    Uint64      offset;               // split：段内偏移
};
inline const void* MGPipeHostBytes(const MGHostSpan&);   // 一次分支，每次使用解析一次
```

| 消费者 | 今天的站点 | monolith 填法 | split 填法 |
|---|---|---|---|
| client 顶点数组 | `Managers.cpp:2500-2592`（每 draw 每属性上传 `(first+count-1)*stride+elementSize`）、`VulkanRenderer.cpp:3737` | `ptr = attrib.Offset` | tracker 暂存同样的范围进 `SEG_STAGE` |
| client 索引数组 | `DirectGLES.cpp:4425-4442`、`VulkanRenderer.cpp:3418-3433` | `ptr = indices` | 暂存 `count*indexSize` |
| indirect / parameter 命令块 | `DirectGLES.cpp:4666-4695`（`MultiDrawElementsIndirectCount` 从 `parameterBuffer->MappedData()` 读实际 draw 数）、`:4768-4793`、`VulkanRenderer.cpp:12045` | `ptr` 指向 shadow | tracker **解析出计数**并暂存解析后的命令块 |
| restart 重写 / multi-draw 展平的索引字节 | `DirectGLES.cpp:4412-4415`、`MultiDraw.cpp:498-540`、`VulkanRenderer.cpp:4159` | `ptr` 指向 shadow | 暂存，或 client 已重写（§5.7） |

**monolith 代价为零**（一次指针加载），而且它顺带消灭了"backend 在 draw 中途回头调前端来 reconcile"这一整类：20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 全部是"我马上要读你的字节，先把 shadow 对齐"，现在改由 **tracker 在填 span 之前**做同样的 reconcile。这在 monolith 里也是严格改进。

**`kCapNeedsHostIndexBytes`** 存在的理由正是这条 monolith/split 的不对称：monolith 里 span 是一个免费指针，split 里是一次拷贝。让一个从不需要宿主索引字节的 backend 不付这笔钱。

### 4.6 与 gallium 的对应与偏离（八条，逐条记名）

| # | gallium | MGPipe | 理由（证据） |
|---|---|---|---|
| **D1** | `create_*_state` 返回 driver 指针 | **调用方提供 handle**，create 返回 void | 零创建 round trip；handle 是稠密 slot，让 server 对象表变成数组；退役全部 D 类指针 memo |
| **D2** | `pipe_screen::get_param(cap)`、`is_format_supported(...)` 逐项查询 | **一个 `MGPCaps` POD + 一张稠密 format 表** | `DynamicBackendParameters`（`BackendObject.h:302-522`）与 `FormatCapabilityCache`（`:88-99`）本来就是平坦结构，逐项 RPC 等于每次 `glGetInternalformativ` 一次 round trip |
| **D3** | CSO 切分是 D3D10 时代的：polygon offset、stencil write mask 在 rasterizer/depth-stencil CSO **内部** | **CSO 边界跟 Vulkan 动态状态走**：depth bias、stencil ref/write mask、line width、min sample shading、blend color、viewport、scissor、depth range 是 `set_*` 而非 CSO 字段 | `RenderState.h:523-528` 记录了共用一个版本号导致 `glViewport` 冲掉 pipeline memo **和** draw 快路径；`m_pipelineStateVersion`（`:529`）恰好是 CSO 相关子集；Magma 的 `DynamicStateShadow`（`VulkanRenderer.cpp:277-340`）与 `ApplyDynamicDrawStateTail`（`:5871`）已经这么切 |
| **D3b** | 三个独立 CSO：blend / depth_stencil / rasterizer | **一个 `RenderStateCso`，payload 是整块 blob** | D-B1 的全部理由：`is_trivially_copyable_v` 断言（`DirectGLES.cpp:2035`）、三段 memcmp（`:2042-2047`）、**字段顺序承重**（`RenderState.h:359-368`）、两个 backend 都按 span/bulk 消费、拆分需手工维护 ~150 字段划分表且无完整性绊线 |
| **D4** | `transfer_map`/`transfer_unmap`（scoped） | **`resource_subdata` 推送 + `map_persistent`（永久地址空间捐赠）** | `AcquirePersistentMap`（`BufferObject.h:102-118`）把指针交给**应用**，成为唯一真相源，≥16MiB 自动走到（`:226-228`）。实测 p99 163→21ms、40→115fps。不是 scoped map |
| **D5** | driver 看得见压缩格式与 pixel-unpack 状态 | **两者在 MGPipe 里都不存在** | 前端在 `glTexImage` 时把压缩 internalformat 解析成非压缩后备（`GL_Texture.cpp:298-306`）；`ScopedDefaultUnpackState`（`Managers.cpp:2888-2910`）强制 unpack 默认值，因为 `PixelStoreProcessor` 已经规整过。**只有 PACK 方向过线** |
| **D6** | 默认 uniform block = `constant_buffer 0` | **独立入口 `set_global_constants(shaderCso, bytes, version)`** | `SpirvArtifacts::globalUboScratch`（`ProgramObject.h:1418`）是 link **phase B** 产出的 CPU 数组，布局由**优化后**的 SPIR-V 决定（`:1400-1408` 说明它无法从 glslang 反射提出来，因为 spirv-opt 就地跑且能删掉整个 block）。它没有 GL name、没有 `BufferObject`、没有 `PipeResource`。`~0u` 跳过回绕的规则保留（`:792`，那是 backend 的"从未上传"哨兵） |
| **D7** | `pipe_shader_state` = tokens → 完成的 handle | **handle + server 侧惰性特化**，variant 键取自**已推送**状态 | D-B2 的 8 个输入。这其实**就是** gallium：Mesa 的 `st_variant` 也按已绑定状态键控（`st_get_vp_variant`） |
| **D8** | `pipe_context::flush` + fence 是唯一反向通道 | **`MGPipeCallbacks`**：9 个具名回复/事件（§7） | gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求、default-FB 几何这些词汇。把它们具名化好过藏在 95 个写回点里 |
| **D9** | `set_viewport_states(start_slot, num)` | **float 数组 + 独立的 `writtenMask`** | viewport 是 **float** 不是 int（`RenderState.h:229-237`：`KHR-GL43.viewport_array.viewport_api` 用 `==` 无容差比较）；scissor 必须单独带 `ScissorBoxWrittenMask`（`:363`），因为矩形本身回答不了"应用说过话没有"——`ScissorBoxes` 初值全零而 `glScissor(0,0,0,0)` 是合法 GL、意思是"拒绝每个片元"，把空框反转成"接受一切"被 `KHR-GL43.viewport_array.scissor_zero_dimension` 抓到（`:352-362`） |

**没有 `pipe_transfer`、没有 `set_pixel_unpack_state`、没有压缩格式概念、renderbuffer 不折进纹理。**

### 4.7 覆盖论证

#### 4.7.1 对 477 读点分类的逐类映射

`../MobileGL-CS/docs/CS_Refactor/backend_read_inventory.md`（`Feat/CS-Delta-IPC` 生成）把 477 个 backend 读点分成 19 个 delta 类、0 UNMAPPED。它的**行**已经过时（`TwinLookupMemo` 族、buffer mutation epoch、`DrawTextureSyncKeys`、`SetupDrawSnapshot`/`VaoDrawMemo`、三条 persistent ring 都晚于它），但它的**分类学是现成的覆盖清单**。逐类映射：

| delta 类 | n | 满足它的 MGPipe 调用 | 残余 |
|---|---|---|---|
| handle 化（wire 句柄） | 167 | 每个命名对象的调用签名里的 `MGPipeHandle` | — |
| RenderStateBlob | 99 | `create/bind_render_state` | — |
| ObjectBind:Texture / Sampler | 33 | `set_sampler_views` + `bind_sampler_states` | — |
| ObjectBind:Buffer（array/element/indirect/parameter） | 29 | `set_vertex_buffers` / `set_index_buffer` / `set_indirect_buffers` | — |
| ObjectBind:BufferRange（UBO/SSBO/AC/XFB） | 24 | `set_shader_buffers` / `set_stream_output_targets` | — |
| FboAttach + DrawBuffers + ReadBuffer | 19 | `set_framebuffer_state` | — |
| Buffer ops delta | 17 | `resource_create/respecify/destroy`、`resource_subdata`、`buffer_subdata_resident`、`resource_flush_range`、`resource_readback`、`map_persistent` | — |
| XfbOp | 15 | `set_stream_output_targets` + `*_stream_output` | — |
| ObjectBind:Image | 14 | `set_shader_images` | — |
| ObjectBind:VAO | 12 | `bind_vertex_elements_state` + `set_vertex_buffers` + `set_index_buffer` | — |
| ObjectBind:Program | 10 | `set_draw_program` / `set_dispatch_program` | — |
| TexParam / SamplerParam | 9 | `create_sampler_view`（base/max level、swizzle、dsMode）+ `create_sampler_state` | — |
| Texture state（dirty level/rect） | 7 | `resource_subdata` | **归属反转**（§7.3） |
| PixelStoreBlob | 6 | `set_pixel_pack_state` | unpack **删除** |
| client-resolved（error queue） | 6 | **删除**：`RecordError` → `on_gl_error` 回调（§7） | — |
| ProgramPublish | 3 | `create_shader_state` | — |
| client-resolved（validation） | 3 | **删除**：client 自答 | — |
| CurrentAttrib | 2 | `set_vertex_attrib_defaults` | — |
| client-resolved（compile env） | 2 | **删除**：`on_caps_invalidated` | — |
| Patch 参数 | — | `set_patch_state` | 同时是 variant 输入 |
| 条件渲染 | — | **client 解析，永不过线** | `Core.h:387-391`：谓词只在 `glBeginConditionalRender` 解析一次 |
| XFB CPU 计数 | — | **纯 client**；`MGPDrawInfo::xfbCpuCapturedVertices` + `MGPCaps` 的 `kCapCpuXfbPrimitiveAccounting` | — |
| backend 重铸纪元 | — | **无 client 对应物**：`MGGen`，server 私有 | — |

那 1997 个前端 getter 站点不是第二个面：89 个纯版本读**根本不过线**（推送即信号），72 个数据字节读全部落在 §5.7 与 `MGHostSpan`，38 个 `GetLifetimeId()` 变成 handle，其余是 backend 现在自己持有的 POD 记录的字段读。

#### 4.7.2 覆盖论证不是这张表，是这条门

上表是**声明**，不是证明。证明是机械的：

> **接口纯度门。** 在 `MOBILEGL_PIPE_PUSH=all` 构建里，`MG_Backend` 被编译时 `MG_State/GLState/Core.h` **不可用**、`MG_State::pGLContext` **未声明**。任何接口没满足的读都是一次**指名文件与行号的编译错误**。strangler 结束时：`grep -c 'pGLContext' MG_Backend/` == 0（**注意 grep 的是 `pGLContext` 不是 `pGLContext->`**，因为还有 58 行非箭头用法，§2.4）；`MG_Backend` 只允许 include 一张共享**值**头白名单（`RenderState.h` 的 `RenderStateParameters`、`SamplerObject.h` 的 `SamplerParameters`、`RenderState.h` 的 `PixelStoreParameters`、`VertexArrayObject.h` 的 `VertexAttribute`、纹理/格式枚举），今天是 **50 行 include / 18 个不同头文件**；`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空。

**这比生成一张 477 行的清单严格得多：它禁止那次读，而不是给它编目，而且它不会过期。** 那份 inventory 仍然保留，作为 tracker 侧的覆盖检查表（G6 生成，CI `git diff --exit-code`，0 UNMAPPED）。

#### 4.7.3 21 条 D 类身份 memo 的重键表

| # | 今天的键 | 守什么 | MGPipe | 净效果 |
|---|---|---|---|---|
| D1 | `StateBackendObjectRegistry` 用裸 `StateObject*` + 同址 `weak_ptr`（`Managers.h:282-325`）×6 | 分配器地址复用；**也是唯一的删除信号** | 按 slot 索引的数组 + `gen` 比较；显式 `resource_destroy` | GC（1024/64 阈值）**删除** ×6 |
| D2 | `TwinLookupMemo` ×3 + `OwnerEquals`（`DirectGLES.cpp:62-131`） | 复用堆地址命中 memo 槽 | **删除**——数组下标**就是**查表 | ~75 行 + 140KiB |
| D3 | `UnitTextureSyncEntry` + `PairingsIntact`（`:1441-1481`） | 不移动任何计数器的 slot 交换（DSA by-name） | **结构性删除**：`set_sampler_views` 就是信号 | ~115 行 |
| D4 | `IsBufferDrawClean` 身份优先比较（`Managers.cpp:1436`） | respecify 交给前端一个**新**资源 | server 拥有资源表；`gen` 比较；`GetChangeSerial()`（`Uint64`，不回绕）继续过线 | 简化 |
| D5 | `ResolvedDrawBuffers::iboFrontend`（`Managers.h:711-716`） | 索引 slot 重绑而无 epoch/config 移动 | `set_index_buffer` 是独立调用 | 结构性 |
| D6 | `m_syncedIndexBufferObject` 陪一个回绕 `Uint16`（`:775-780`） | 版本回绕后换了个 buffer | `{slot, gen}` 比较，不回绕 | 结构性 |
| D7 | `StampSyncedFBO` 四元组（`DirectGLES.cpp:1856-1901`）；`packed_pixels` postmortem `:2815-2827` | 版本回绕 + backend 侧纹理重铸 | `MGPFramebufferState::contentHash` + server 私有 `attachmentRemintEpoch`（`MGGen`） | 一次 64 位比较 |
| D8 | `g_fboTextureSyncList`（`:1580-1601`） | 同 D3，针对 attachment | **删除**：`set_framebuffer_state` 是信号 | — |
| D9 | `ResolvedTextureBindingMemo`：9 个键 + 驱动绑定影子的 `memcmp`（`:3218-3291`） | 任何未枚举的写者扰动某个 unit | `(shaderCso.slot, viewSetSerial)` 两字比较；`viewSetSerial` 由 server 在 `set_sampler_views` **内部** ++ | 更便宜；自校验性质保留（server 的绑定缓存**就是**它当年比对的那份影子） |
| D10 | `UnitSamplerLookupMemo` 的 `WeakPtr` owner 测试（`:3105-3125`） | 死 sampler 复活 | 数组下标 | 删除 |
| D11 | `VertexInputStateFactory::ComputeHash` 混入 `GetLifetimeId()`（`:38-49`） | 复用 buffer 地址重现整个 content hash（XFB 捕获拿回死 VAO 的顶点数据） | CSO handle **就是**身份；`gen` **混进** server 侧每个 content hash 而不只是比较 | 删除一整类 |
| D12 | `SetBackendStateMemo(&entry, evictionEpoch)`：**前端 VAO 里存后端堆裸指针**（`VertexInputStateFactory.cpp:78`） | table 淘汰 | **直接删除，不翻译**。无 wire 对应物，也不需要；Magma 自己的 `VaoDrawMemo` 已经演示了替代品 | — |
| D13 | `VaoDrawMemo` 槽（`VulkanRenderer.h:1230-1245`，注释："两条声明的防线都失守，因为都归约到 content hash，而 content hash 的 buffer 身份分量本身就是复用的堆地址"） | ABA | CSO handle | 2 字 |
| D14 | `SetupDrawSnapshot` 的三组 `(ptr, lifetimeId, version)` + **有损的** `sampledContentSum`/`sampledParamsSum`（`.cpp:6249-6250`） | 一切 | 三个 handle + 两个 server 纪元 + dirty mask | ~14 个探测字段 → 1 次比较；**顺带消灭一类哈希碰撞**（求和会碰撞） |
| D15 | `m_rpFast*`（`VkRenderPassManager.h:305-320`） | ABA | `contentHash` + `MGGen` | 1 次比较 |
| D16 | `VkTextureManager::TextureIdentity{ptr, lifetimeId}` + `pGLContext->GetTextureObject(name)` 存活探测（`VkTextureManager.cpp:806-819`） | 名字复用 / 删了但仍被 FBO 引用 / 默认纹理 | `{slot, gen}` + 显式 destroy | 三种被记录的失效模式**一起消失** |
| D17 | `VkClearManager::TextureIdentity`（`VkClearManager.h:76-83`） | ABA | `{slot, gen}` | — |
| D18 | 纹理/renderbuffer 资源用**节点式** `std::unordered_map`（postmortem `VkRenderPassManager.h:375-397`） | 扩表搬迁使缓存的 `Resource*` 失效（`BlitFramebuffer` 静默停在 "layout undefined"） | **UNCHANGED。** 接口对此零约束；这是 server 内部分配纪律。slot 数组在插入下稳定，实际更好——但**这条 postmortem 注释必须逐字带进 review checklist** | 保留 |
| D19 | `ProgramFactory::m_cacheStructureEpoch` | 守 `SetupDrawSnapshot` 里的 server 内部裸指针 | **UNCHANGED**（`MGGen` 族） | 保留 |
| D20 | `ConvertedVertexStreamKey` + **纯为防地址复用**持有的 `SharedPtr sourcePin`（`VulkanRenderer.h:1124-1127`） | ABA | server 拥有资源；`changeSerial` 过线 | **pin 删除** |
| D21 | `m_xfbCounterSlotByObject[GetBoundTransformFeedbackName()]`（`VulkanRenderer.cpp:11136-11146`） | **什么都没守——这是一个活的潜伏 bug**：删了又重建的 XFB 对象会继承前任的计数槽，配上匹配的 `m_xfbLastSeenGeneration` 会**恢复**一次本该重启的捕获 | XFB 对象 handle | **顺带修一个 bug**。这一条值得先独立落到 `dev` |

**总计：12 条直接删除，8 条重键成比它替代的东西更便宜的比较，1 条（D18）原样不动。** 这就是"身份键 memo 不可移植"这条历史反对意见的具体回答——它们不是**以指针形式**可移植，接口的职责正是给它们一个可移植的键，而树里的证据已经把那个键是什么讲清楚了。
