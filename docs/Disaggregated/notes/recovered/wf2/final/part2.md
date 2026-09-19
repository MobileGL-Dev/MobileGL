## 4. 接口设计：MGPipe

### 4.1 文件布局与单一真相源

```
MobileGL/MG_Pipe/                      # client 与 server 都 include；不链接 MG_State，不链接 MG_Impl
    PipeCalls.def                      # X-macro：调用目录的唯一真相源，一行一个调用
    MGPipe.h                           # 由 .def 生成的两张函数表 + 手写 payload 声明
    MGPipeTypes.h                      # 全部 payload POD（trivially copyable，逐个 static_assert）
    MGPipeValueTypes.h                 # ★v2 新增：无依赖的共享值类型（见 §4.7.2）
    MGPipeHandles.h                    # MGPipeHandle、MGPipeKind、保留 handle、slot 分配契约
    MGPipeHostSpan.h                   # 唯一一个"形状随传输而变"的访问器（§4.5.7）
    MGPipeCallbacks.h                  # 反向通道（事件/回复）的函数表，见 §7
    MGPipeRenderStateSpans.{h,cpp}     # ★v2 新增：pipeline/dynamic 划分的唯一定义（§4.5.2）
    generated/PipeTables.inc           # G1：两张函数表
    generated/PipeThunks.inc           # G2：monolith 直调 thunk
    generated/PipeWire.inc             # G3：wire 记录 + static_assert + 运行期边界检查 + applier switch
    generated/PipeVerify.inc           # G4：逐字段影子比对器
    generated/PipeFilled.inc           # G5：written-once 位图与 poison 断言（**逐 verb 世代**）
    generated/PipeCoverage.inc         # G6：477 读点 → MGPipe 调用的映射表
    generated/PipeSpanTable.inc        # ★G7：render-state 的 pipeline/dynamic chunk 表 + setter 一致性测试
MobileGL/MG_Impl/Pipe/
    Tracker.{h,cpp}                    # st_validate_state 类比物（含从 backend 搬来的 ~175 行去抖/解析）
    SlotAllocator.{h,cpp}  CsoCache.{h,cpp}
    HostResolve.cpp                    # 客户端数组界限 / 索引扫描 / indirect count 解析
    CompositeResolver.cpp              # program pipeline 合成体的 handle 生命周期
MobileGL/MG_Backend/MGPipe/
    PipeInputs.h                       # backend 私有的"被推送状态"块（迁移载体，§6.2）
    MGPipeImpl_DirectGLES.cpp          # 用 Espryt 的函数填 MGPipeContext
    MGPipeImpl_DirectVulkan.cpp        # 用 Magma 的函数填 MGPipeContext
MobileGL/MG_Remote/                    # 传输，继承 PLAN.md §13（删掉 Server/ReplicaContext.*）
    Server/PipeApplier.cpp  Server/PipeObjectTables.{h,cpp}  Server/IndexHostMirror.{h,cpp}
scripts/gen_pipe.py                    # 跑 G1..G7
scripts/gen_pipe_dirty_surface.py      # ★v2：MG_Impl mutator → 聚合世代 的覆盖生成器（推论 4）
scripts/check_doc_citations.py         # ★v2：docs/**.md 的 file:line 必须解析到存在的行
```

`PipeCalls.def` 一行一个调用，**七个生成器**消费它：

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
  X(SetDynamicState,     MGPDynamicState,     kCtxState, kHasBlob)                \
  X(SetFramebufferState, MGPFramebufferState, kCtxState, kNone)                   \
  X(SetSamplerViews,     MGPSamplerViews,     kCtxState, kVarTail)                \
  X(SetTextureParams,    MGPTextureParams,    kCtxObject,kNone)                   \
  X(SetShaderBuffers,    MGPShaderBuffers,    kCtxState, kVarTail|kHostSpan)      \
  /* ---- verb ---- */                                                            \
  X(DrawVbo,             MGPDrawInfo,         kCtxVerb,  kHostSpan|kVarTail)      \
  X(ResourceSubData,     MGPSubData,          kCtxObject,kHasBlob|kVarTail)       \
  X(RenderbufferStorage, MGPRbStorage,        kCtxObject,kNeedsAck)               \
  /* … 共约 74 项，完整目录见 §4.4 与 part4 的速查表 … */
```

| 生成器 | 产物 | 替代/新增 |
|---|---|---|
| **G1** | `struct MGPipeScreen { … };` / `struct MGPipeContext { void (*DrawVbo)(const MGPDrawInfo*, …); … };` | 替代今天手写的 `GLFunctionsTable` |
| **G2** | monolith thunk：`inline void MGP_DrawVbo(const MGPDrawInfo* p){ gPipeCtx.DrawVbo(p); }` | 替代 `gBackendFunctionsTable.GL.*`（~93 个 MG_Impl 站点改名即可） |
| **G3** | wire 记录结构 + 每种一条 `static_assert(sizeof==N)` + applier 分发前的运行期边界检查 → `Fatal{ProtocolCorruption}` | 继承并扩展 `PLAN.md` §6.3 的 `Records.def` 机制到**全部**调用 |
| **G4** | `MOBILEGL_PIPE_VERIFY` 的逐字段比对器 | **新增**：每份候选设计都被判缺失的语义绊线 |
| **G5** | `PipeInputs::m_filledGen[]` 的位/世代定义 + 读未填字段时的 `Fatal{UnmigratedPipeInput, "<field>"}` | **新增**（v2：由"位图"升级为"**逐 verb 世代**"，见 §6.2.2） |
| **G6** | 477 行读点清单 → MGPipe 调用的映射，CI 重生成并 `git diff --exit-code`，0 UNMAPPED | 改造自 `Feat/CS-Delta-IPC` 的 `extract_backend_read_inventory.py` |
| **G7（v2 新增）** | `RenderStateParameters` 的 pipeline/dynamic chunk 表 + **一个遍历每个 `RenderState` public setter、断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变` 的 `MG_Test`** | **新增**：D-B1 拒绝三 CSO 时点名要求、v1 却没给自己的完整性绊线 |

**G4、G5、G7 与调用目录从同一份 `.def`/同一张 chunk 表生成，因此不可能漂移。**

**接口表用函数指针 struct，不用虚基类。** 三条本仓库自己的理由：(1) 边界今天**就是**函数指针 struct，装在 `MG_Backend/Init.cpp:44` 的唯一 hook 点上；(2) `nullptr` 项**已经**表示"未实现，前端回退"（`BackendObject.h:212-215`、`:265-269`），DirectVulkan 确实留空 8 项——**一个 null `set_*` 恰好就是"这个子系统还没迁移，继续拉取"**，纯虚类只能用说谎的 stub override 来模拟；(3) `MG_Test` 已经会替换这张表做 mock。稀有的 EGL/caps 面继续留在 `pActiveBackendObject` 的虚函数上。

### 4.2 对象模型

#### 4.2.1 Handle

```cpp
enum class MGPipeKind : Uint8 {
    Buffer=1, Texture, Renderbuffer, Framebuffer, Xfb,
    RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso,
    Fence, Query, Context
};
struct MGPipeHandle { Uint32 slot; Uint32 gen; };   // 8 B，POD，按值走寄存器对
```

- **slot 稠密、按 kind 分配**，把 server 的对象表从哈希表变成**数组**；`SlotAllocator` 是 free-list + 高水位，与 `IndexGenerator` 无关（后者的 LIFO 复用正是问题本身）。
- **`gen` 只在 slot 复用时 ++**，不是每次 respecify。`{slot, gen}` 在同一 slot 被复用 2³² 次之前唯一；文档写明上界，debug 断言它。
- **GL name 只在 `resource_create` 的 payload 里出现一次，纯诊断**，永不做身份、永不进 memo 键或 content hash。
- **`GetLifetimeId()` 留在 client 侧**作为 tracker 自己的身份，不过线；client 维护 `lifetimeId → slot`。
- **保留 handle**：`{0,0}` = null；`{slot=0, gen=1, kind=Framebuffer}` = 默认帧缓冲（退役 `DirectGLES.cpp:1917, 2838, 2867, 9675` 四处 `pDefaultFramebufferInfo->defaultFBO` 身份比较）；`ShaderCso` 的高 1/16 slot 段保留给 **program pipeline 合成体**（§5.6）。

#### 4.2.2 两种 generation，严格分开

| | 拥有者 | 回答什么 | 是否过线 |
|---|---|---|---|
| **身份**（`MGPipeHandle::gen`） | client | "还是同一个 GL 对象吗？" | 是 |
| **`MGGen`**（server 纪元） | **server** | "**我自己**是不是重铸了驱动对象 / 冲了自己的缓存？" | **client→server 永不；server→client 只以纹理拉取请求的形式出现**（§7.5） |

**接口规范条款：任何 MGPipe 调用都不得要求 client 提供或知晓 `MGGen`。** 反过来也是规范：**client 侧的版本计数器永远不是新鲜度的唯一证明**——每一个回绕的 `Uint16`（§2.6）在过线时要么加宽到 32 位、要么与 `{slot, gen}` 同行。

#### 4.2.3 CSO vs 可变对象

| 类别 | 形态 | 因为 backend 今天就是这么缓存的 |
|---|---|---|
| `VertexElementsCso` | `create/bind/delete` | `VertexInputStateFactory::m_cache`，键正是那组字段的 content hash（`VertexInputStateFactory.cpp:19-50`） |
| `SamplerCso` | `create/bind/delete` | `VkSamplerManager::m_samplers`；Espryt 的 `BackendSamplerObject`（`Managers.h:1808-1824`） |
| `SamplerViewCso` | `create/delete` + 由 `set_sampler_views` 绑定 | `TextureResource::{perMipViews, …, storageImageViews}`（`VkTextureManager.h:173-370`）；Espryt 的 `SyncTextureViewToBackend`（`Managers.cpp:3616-3707`） |
| `ShaderCso` | `create/bind/delete` + **server 侧惰性特化**（D-B2） | `ProgramFactory::m_cache`；`BackendProgramObjectImpl` |
| `RenderStateCso` | `create/bind/delete`，**身份 = pipeline 子集**（D-B1 v2） | Espryt 的值镜像 + 单 `Uint16` 早退 + 三段 memcmp；Magma 的 `ComputePipelineStateHash` |
| Buffer / Texture / Renderbuffer | `create` / `respecify` / `subdata` / `destroy` | `GLESBufferResource`、`BackendTextureObject`、`VkBufferResource`、`TextureResource` |
| Framebuffer / Xfb | per-context 身份 + `set_*` payload | `BackendFramebufferObject`、`m_xfbCounterSlotByObject` |

**CSO 在 client 侧内容寻址**（Mesa `cso_context`/`cso_cache` 先例）：每类一张 `ska::flat_hash_map<Uint64 xxHash, MGPipeHandle>`，容量上限（render-state 64、vertex-elements 1024、sampler 256、sampler-view 4096、shader 跟随 `ProgramObject` 生命周期），LRU 淘汰时发 `delete_*_state`。**收益**：两个不同 program 设置了相同状态时 server 侧**零状态转换**。

**任何 `create_*` 都不返回 server 铸造的 handle。** 这是对 gallium 的**有意偏离**（D1），也是这份目录能在**零创建 round trip** 下远程化的根本原因。`BackendSyncHandle`/`BackendQueryHandle = void*`（`BackendObject.h:110, 115`）随之变成 `MGPipeHandle`。

### 4.3 `MGPipeScreen` 与 `MGPipeContext`

| `MGPipeScreen`（share group） | `MGPipeContext` |
|---|---|
| caps、format 能力表、renderer 字符串；buffer / texture / renderbuffer / sampler / shader 的对象命名空间；fence | 全部 `set_*`、全部 CSO 绑定、VAO / FBO / XFB 对象 / query 的命名空间、命令流、present |

v1 只有一个 screen、一个 context、一条 flow。**但两张表从第一天就分开**，因为事后拆分意味着给每个记录种类重新编号。两处必须重新归类的事实：`GetTextureBindGeneration()` 与 `GetSamplingResolutionGeneration()`（`Core.h:130, 136`）是**绑定**（context）事实却住在 share-group 作用域的 `TextureState` 里；`GetTextureContextId()`（`:143`）直接**就是** context handle。

### 4.4 完整调用目录

#### 4.4.1 `MGPipeScreen`（14 项）

| 调用 | payload | 取代 |
|---|---|---|
| `get_caps(MGPCaps* out)` | `DynamicBackendParameters`（`BackendObject.h:302-522`，~90 标量，平坦 POD）+ `RendererInfo` + `FormatCapabilityCache`（`:88-99`）+ `callMask` | 40 个 `pActiveBackendObject->` 站点、89 个 caps 读点 |
| `resource_create(h, const MGPResourceDesc*)` | §4.5.1 | buffer/texture/renderbuffer 的创建 |
| `resource_respecify(h, const MGPResourceDesc*)` | 同上 | `BufferBackendOps::Respecify`（`BufferObject.h:80`）泛化 |
| `resource_destroy(h)` | handle | `OnDestroy`（`:101`）+ **两个 `WeakPtr` GC 扫描** |
| `map_persistent(h) → MGPMapResult` / `unmap_persistent(h)` | — | `AcquirePersistentMap`（`:112`）。**改造期不碰**（D-B4） |
| `fence_create/status/wait/destroy` | handle (+timeout) | `FenceSync`…`GetSyncStatus`（`:220-224`）。两值契约（`:243-249`）**逐字保留** |
| `query_create/begin/end/available/result/destroy` | handle + kind | `BackendObject.h:230-256` |
| EGL 生命周期 8 项 | `BackendObject.h:548-559` | 原样保留为虚函数（罕见） |

**`callMask` 取代"槽位是否为 null"这个隐式能力探测**（`GL_Query.cpp:471, 545, 768`）。**v2 修订的能力位集**（v1 的五个 emulation 归属位按 D-B7 删除）：
`kCapViewportArray`、`kCapFloat64VertexAttrib`、`kCapResidentSubData`、`kCapCpuXfbPrimitiveAccounting`、`kCapTimerQuery`、`kCapOcclusionQuery`、`kCapXfbPrimitivesQuery`、**`kCapNeedsHostIndexBytes`**（server 侧的 restart 重写/multi-draw 展平需要索引宿主字节 → split 下开启索引宿主镜像，D-B7）、**`kCapNeedsHostUboBytes`**（server 侧要把具名 UBO 打进自己的 ring → 需要 `set_shader_buffers` 的 host payload，D-B8）。
**删除**：`kCapPrimitiveRestart`、`kCapPrimitiveRestartFixedIndex`、`kCapMultiDraw`、`kCapMultiDrawIndirect`、`kCapMultiDrawIndirectCount`——它们表达的"归属开关"不可表达（D-B7）。

#### 4.4.2 `MGPipeContext` — CSO（15 项）

`create/bind/delete` × { `render_state`, `vertex_elements`, `sampler`, `sampler_view`, `shader` }。payload 见 §4.5.2-4.5.5。

#### 4.4.3 `MGPipeContext` — `set_*`（17 项，v2 从 14 增至 17）

| 调用 | 取代的拉取点 |
|---|---|
| `set_dynamic_state(MGPBlobRef chunks, Uint16 version)` **（v2 新增）** | 渲染状态里 `m_pipelineStateVersion` 不覆盖的那一半（viewport / scissor / depth range / blend color / line width / polygon offset / stencil ref+write mask / clear values / sample coverage / hints / point-size 族）。**这条让 `glViewport` 不再铸造新 CSO**（D-B1） |
| `set_framebuffer_state` | `GetFramebufferBindingSlot` ×19、`GetAllAttachmentObjects`、`GetDrawBuffers`、`GetReadBuffer`、4 处 `pDefaultFramebufferInfo` |
| `set_vertex_buffers(start, count, const MGPVertexBuffer*)` | VAO binding-point 走查 |
| `set_index_buffer(const MGPIndexBuffer*)` | `GetIndexBufferBindingSlot`；**独立调用**——VAO config version 不是它的超集（D5） |
| `set_indirect_buffers(drawIndirect, parameter)` | `GetBufferBindingSlot(DrawIndirect/Parameter)` |
| `set_sampler_views(start, count, const MGPBoundView*)` **（v2：删掉 stage 形参）** | `GetTextureUnitObject` ×19、`GetActiveTextureUnit` ×8、`GetTextureBindGeneration` ×5。**client 侧已解析**（§5.5） |
| `bind_sampler_states(start, count, const MGPipeHandle*)` **（v2：删掉 stage 形参）** | `TextureUnit.h:394` |
| `set_texture_params(res, const MGPTextureParams*)` **（v2 新增）** | base/max level、swizzle、depth-stencil mode、LOD 钳。**必须独立于 sampler view**，见下 |
| `set_shader_images(start, count, const MGPImageView*)` | `GetImageTextureBinding` ×14；**退役 `ImageUnitFormatsStillMatch`**（`Managers.cpp:6545-6573`） |
| `set_shader_buffers(cls, start, count, const MGPBufferRange*, writableMask)` **（v2：Uniform 类的 range 可带 `MGHostSpan payload`）** | `GetBufferBindingPoint` ×19、`GetTouchedBufferBindingPointCount` ×2。`cls` ∈ {Uniform, ShaderStorage, AtomicCounter}。**payload 由 `kCapNeedsHostUboBytes` 门控**（D-B8） |
| `set_stream_output_targets(count, const MGPBufferRange*, const Uint32* offsets, Uint64 generation)` | XFB 绑定走查 |
| `set_global_constants(shaderCso, MGPBlobRef, Uint32 version)` | `MapUBO`/`GetUBOData`/`GetUBOSize`/`GetUBOContentVersion`（§4.6 D6）。**只覆盖默认 uniform block** |
| `set_vertex_attrib_defaults(Uint32 mask, const MGPAttribValue*)` | `GetCurrentVertexAttribute` ×2；float/int/uint 视图由 `ClassifyVertexAttribType`（`Core.h:51`）在 client 侧解析 |
| `set_pixel_pack_state(const PixelStoreParameters*)` | 6 个 PACK 读点。**没有 unpack 对应项**（§4.6 D5） |
| `set_patch_state(Uint32 vertices, const Float outer[4], const Float inner[2])` | `GetPatchVertices`/`…OuterLevel`/`…InnerLevel` ×6。**同时是 shader variant 输入** |
| `set_draw_program(shaderCso)` / `set_dispatch_program(shaderCso)` | `GetProgramForDraw` ×7、`GetProgramForDispatch` ×3。含 composite（§5.6） |

**为什么删掉 `stage` 形参（v2）**：MobileGL 的纹理单元空间是**合并的**，不是分 stage 的——`TextureState::m_textureUnits` 是 `Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS>` 且 `MAX_TEXTURE_IMAGE_UNITS = 192`（`TextureState.h:41, 128`），每 stage 的 32 只是一个**广告数字**（`:46`）；`TextureUnit` 本身是 `Array<BindingSlot<ITextureObject>, TextureTargetCount>` 加一个 sampler（`TextureUnit.h:20, 24-25`）；两个 backend 都按合并单元绑定（`g_boundTexturesCache[192][TargetCount]`）。同一个合并单元可以被两个 stage 采样。加 stage 维度会逼 client 要么按 stage 复制 view、要么发明一个 GL 未定义的 stage 归属，而 server 还得把它塌回去。**stage 只在目标 API 真正需要时出现（Magma 的描述符 stage flags），由 server 从反射归档推导。**

**为什么纹理参数不能只挂在 sampler view 上（v2）**：Espryt 对**每个 touched 单元绑定**与**每个 draw-FBO attachment 纹理**都调 `SyncTextureParamsToBackend`（`DirectGLES.cpp:1548-1560` 单元表、`:1580-1601` attachment 表），而 `RequireImageBindableStorage` 会置 `m_forceTextureParamsResync`，正是因为通道加宽后的载体需要一个前端 params 版本**不会移动**的 swizzle 覆盖（`Managers.cpp:2815-2821`）。一张**只作 FBO attachment**、**只作 image 单元绑定**、或**只作 `glCopyImageSubData` 端点**的纹理**没有 sampler view**，它的 `glTexParameter` 状态在 v1 的映射里没有载体。所以：**base/max level、swizzle、depth-stencil mode、LOD 钳挂在 `set_texture_params(res, …)` 上；`MGPSamplerView` 只带"视图限制"（min/num level、min/num layer、别名格式）。** 这同时让 `glTextureView` 保持它真正的身份——一个有自己参数、自己能当 FBO attachment、自己能当 `glTexSubImage` 目标的**真纹理对象**（`TextureObjectView.cpp:281, 290`）——而不是被降格成"普通 view CSO"。

**迁移期额外一项（显式临时）**：`set_residual_value_state(MGPBlobRef)`，见 §6.3。

#### 4.4.4 `MGPipeContext` — transfer（12 项）

`resource_subdata`（buffer + texture 同一形状，**带步长的多 region 描述符**，§4.5.6）、`resource_flush_range(h, Range1D, Flags<BufferMappingAccessBit>)`（携带应用**真实**的 access flags，`BufferObject.h:94-96`）、`resource_readback(h, off, size, MGPReplySlot)`、`resource_copy_region`、`blit`、`clear`（一条，判别式合并今天的 `Clear` + 4 个 `ClearBuffer*` + 4 个 `ClearNamedFramebuffer*`）、`generate_mipmap(h, target, const MGPMipPlan*)`、`read_pixels(const MGPReadbackInfo*, MGPReplySlot)`、`get_texture_image(...)`、`buffer_subdata_resident(h, off, MGPBlobRef)`（**可为 null**）。

**`buffer_subdata_resident` 的 per-backend 可选性必须被接口允许。** Espryt 注册它、Magma 故意不注册（`VkBufferManager.cpp:104-111`），差别是 `glBufferSubData` 在活的 coherent map 上的排序语义（`BufferObject.h:84-92` 的 Minecraft 撕裂 postmortem）。表现为 `kCapResidentSubData` 位 + null 项。

#### 4.4.5 `MGPipeContext` — 命令（10 项）

```cpp
void draw_vbo (const MGPDrawInfo*, Uint32 drawIdOffset,
               const MGPDrawIndirect*, const MGPDrawRange*, Uint numDraws);
void launch_grid(const MGPGridInfo*);
void memory_barrier(GLbitfield bits, Bool byRegion);
void begin_stream_output(GLenum primitiveMode);
void end_stream_output(const MGPXfbAccounting*);
void pause_stream_output();  void resume_stream_output();
void flush(Uint32 flags);
void present(Uint64 frameSerial);  void set_swap_interval(Int interval);   // 后者可 null（Magma）
```

**今天 20 个 draw 入口塌成 `draw_vbo` 一条**，`MGPDrawRange[]` **就是** `MultiDraw*` 族今天的形状（gallium 的 `pipe_draw_start_count_bias`）。

#### 4.4.6 显式删除、不移植的项

- `GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`（`BackendObject.h:195-197`）。只有 `GL_COMPUTE_WORK_GROUP_SIZE`（`DirectVulkan.cpp:790-795`）是真后端答案，进 `MGPCaps`。
- `ShaderStorageBlockBinding`（`:207-208`）→ 折进 `MGPProgramDesc` 的反射归档。
- **总规则：server 不回答任何 client 能自己回答的问题；剩下的每个 server 查询都是 async-with-handle，绝不阻塞。**

### 4.5 关键 payload

#### 4.5.1 `MGPResourceDesc`（判别式，三种 GL 存储类合一）

```cpp
struct MGPResourceDesc {
    Uint8  target;            // Buffer | Tex1D..TexCubeArray | Tex2DMS.. | Renderbuffer | TexBuffer
    Uint8  storageKind;       // Mipmap | Buffer   (== TextureStorageType, TextureEnum.h:61-64)
    Uint16 bindMask;          // VERTEX|INDEX|CONSTANT|SHADER_BUFFER|INDIRECT|SAMPLER|SHADER_IMAGE|
                              //   RENDER_TARGET|DEPTH_STENCIL|STREAM_OUTPUT|ATOMIC|ELEMENT_ARRAY
    Uint32 internalFormat;    // 已在前端解析为非压缩后备
    Uint32 width, height, depth;
    Uint16 arrayLayers, levels, samples;
    Uint8  fixedSampleLocations, immutable;
    Uint32 usage;             // BufferUsage
    Uint32 storageFlags;      // glBufferStorage flags
    Uint8  hasDefinedContent;  // NULL-data respecify 之后为 false，BufferObject.h:216
    Uint8  imageBindableHint;  // client 侧 everImageBound，预防性分配（§7.5(a)）
    Uint8  glNameForDiag[2];   // 仅诊断
    MGPipeHandle viewOf;       // 纹理视图的存储属主（GetViewStorageOwner，TextureObject.h:100）
    MGPipeHandle bufferForTexBuffer;  Uint64 bufOffset, bufSize;   // kWholeBuffer = ~0，实时解析
};
```

`bindMask` 里的 **`ELEMENT_ARRAY` 位是 D-B7 的开关**：server 见到它且 `kCapNeedsHostIndexBytes` 为真时，把该资源纳入索引宿主镜像。

**Renderbuffer 保持独立类**：自己的 format-capability target 索引（`BackendObject.h:85`）、自己的 `ComponentSizes` 上报（`RenderbufferObject.h:37-43`）、自己的 twin（`Managers.h:1838`）。

#### 4.5.2 渲染状态：`MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState`（D-B1 v2）

```cpp
// MG_Pipe/MGPipeRenderStateSpans.h —— 划分的唯一定义
struct MGPStateChunk { Uint16 offset, length; };
extern const MGPStateChunk kPipelineChunks[];   // G7 生成，来源 = VulkanRenderer.cpp:4826-4906 的字段表
extern const MGPStateChunk kDynamicChunks[];    // 补集
Uint64 MGPipeComputePipelineSubsetHash(const RenderStateParameters&);   // client 与两个 backend 共用

struct MGPRenderStateDesc {          // create：只带 pipeline 子集的 chunk 字节
    MGPipeHandle cso;
    Uint32       chunkMask;          // 未命中时可只发变化的 chunk；全新 CSO 为全 1
    MGPipeHandle baseCso;            // 增量基（chunkMask 非全 1 时有效）
    MGPBlobRef   blob;
};
struct MGPBindRenderState {          // bind：稳态 12 B
    MGPipeHandle cso;  Uint16 version;  Uint16 pipelineVersion;
};
struct MGPDynamicState {             // 动态子集，只发变化的 chunk
    Uint32     chunkMask;
    Uint16     version;  Uint16 pad;
    MGPBlobRef blob;
};
```

**server 侧模型**：每 context 一份 working `RenderStateParameters`（~1.2KB）。`bind_render_state` 把 CSO 的 chunk 散射进去；`set_dynamic_state` 把动态 chunk 散射进去。**Espryt 的 `SyncRenderState` 拿到的仍是 `const RenderStateParameters&`，693 行函数体、单 `Uint16` 早退、三段 memcmp、`g_syncedColorMaskAlphaWidenMask`、dual-source decline 一行不动。** Magma 的 pipeline memo 键是 `cso.slot`，`glViewport` 不再冲掉它；动态尾巴仍走 `ApplyDynamicDrawStateTail` 的两级门。

**两套 span 划分并存，互不干扰，各有绊线：**

| 划分 | 用途 | 定义在哪 | 绊线 |
|---|---|---|---|
| head / blend / tail（`DirectGLES.cpp:2038-2047`，按 `offsetof(BlendStates)`、`offsetof(LogicOp)`） | Espryt **驱动侧**增量 | `DirectGLES.cpp` 原地，**不动** | 已有：`static_assert(is_trivially_copyable_v)`；`RenderState.h:359-368` 的字段顺序注释 |
| pipeline / dynamic | **线上传输与 CSO 身份** | `MGPipeRenderStateSpans.cpp`，G7 生成 | **G7 的 setter 一致性测试**：遍历每个 `RenderState` public setter，断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变` |

**client 侧的取值顺序（热路径，必须照此实现）：**
1. `m_pipelineStateVersion` 未变 → **复用上一个 CSO handle，零哈希**；
2. 变了 → 对 pipeline 子集算 xxHash（~25-30 字，正是 Magma 今天在算的那个）→ CSO map 探测 → 命中发 12 B `bind_render_state`，未命中发变化 chunk 的 `create_render_state` 再 bind；
3. `m_version` 变而 pipeline 子集未变 → 只发 `set_dynamic_state` 的变化 chunk（~200 B）。

**性能诚实注记**：Blaze3D 的 `glEnable/glDisable(GL_BLEND)` 走 `SET_CAPABILITY`（`RenderState.cpp:312`）→ `BumpVersions()`，所以每次都进第 2 步。交替的两个状态命中两个交替的 CSO，不重发 blob。对比今天：Espryt 1.2KB×3 段 memcmp + Magma ~30 字哈希。**净变便宜但差距不大**，因此 **P2 必须带一个专门的 enable/draw/disable/draw 微基准**（MC batch 速率，两台设备）。

#### 4.5.3 `MGPVertexElements`

携带**两个视图，缺一不可**：解析后的 `VertexAttribute[32]`（`VertexArrayObject.h:17-53`）**和** `VertexBufferBindingPoint`（`:58-64`，初始 stride 是 **16** 不是 0，`:61-62`）。`VertexArrayObject.h:22-29` 记录了合并它们的代价：pointer 调用的 stride 0 被解析成 element size，而 binding-model 的 stride 0 意味着每个顶点读**同一个** element，塌成一个害了 `KHR-GL43.vertex_attrib_binding.basic-input-case7/8`。`IsLong` 与 `Type == Float64` **分开携带**（`:34-39`）。**仅供查询的 `LegacyStride`/`LegacyPointer`（`:51-52`）留在 client。**

#### 4.5.4 `SamplerParameters` 与 `MGPSamplerView` / `MGPTextureParams`

`SamplerParameters`（**`SamplerObject.h:72-96`**，v1 误引为 `:468-492`）**逐字节原样过线，包括 `borderColorForm`**（**`:66-70`**）：`:60-65` 明说没有它 backend 无法在 `glSamplerParameterIiv` 与 `fv` 之间、或在 `VkBorderColor` 家族之间选择，因为三种表示（`borderColor`/`borderColorI`/`borderColorUI`，`:93-95`）**永远都被数值填满**。`SamplerObject::BumpVersion()`（`:151`，`m_version` 在 `:155`）**同时**bump context 级 sampling-resolution generation，因为 MIN_FILTER 决定是否读 mip 链 → 决定 mipmap 完备性 → 决定 backend 到底绑不绑这张纹理。

```cpp
struct MGPTextureParams {            // ★v2：per-texture-object，与 view 无关
    MGPipeHandle res;
    Uint16 baseLevel, maxLevel;
    Uint8  swizzle[4];
    Uint8  depthStencilMode, pad[3];
    Float  minLod, maxLod, lodBias;
    Uint8  forceResync;              // 对应 m_forceTextureParamsResync（Managers.cpp:2815-2821）
};
struct MGPSamplerView {              // = pipe_sampler_view，**只带视图限制**
    MGPipeHandle cso, texture;
    Uint32 internalFormat;           // 别名格式（glTextureView）
    Uint8  target, pad[3];
    Uint16 minLevel, numLevels, minLayer, numLayers;
    Uint16 samples;  Uint8 fixedSampleLocations, pad2;
};
```

`GetViewStorageOwner()`（`TextureObject.h:96-100`，一个 `SharedPtr`，且**它自己永远不是 view**）变成 `resource_create` 的 `viewOf` + server 侧 keep-alive。

#### 4.5.5 `MGPProgramDesc`（`create_shader_state` 的 payload）

```cpp
struct MGPProgramDesc {
    MGPipeHandle cso;
    Uint32     stageMask;            // == GetLinkedShaderStages()
    MGPBlobRef spirv[6];             // GetGeneratedSpirv()，逐 stage
    MGPBlobRef reflection;           // Visit() 归档的 LinkArtifacts + SpirvArtifacts（全结构体）
    Uint32     globalUboSize;
    Uint32     reservedNumSamplesOffset;
    Uint8      spirvStatus, nativeFloat64, pointSizeDemoted, enableSpirvValidation;
};
```

**v2 前置条件（P0.5）：反射类型必须先搬出 `ProgramObject.h`。** `TypeFacts`（`ProgramObject.h:44`）、`ResourceReflection`（`:76`）、`XfbVarying`（`:1146`）、`LinkArtifacts`（`:1210`）、`SpirvArtifacts`（`:1409`）今天全部声明在 `ProgramObject.h` 里，而该文件 `:11` include `ShaderObject.h`（→ `ShaderCompileTask.h` → glslang；`ShaderObject.h:146` 返回 `SharedPtr<glslang::TShader>`）、`:14` include `SpvcSession.h`（→ `spirv_reflect.h`）。**server 要反序列化进这些类型就必须 include 被门禁止的头。** P0.5 把它们抽到：

```
MG_State/GLState/ProgramState/ProgramArtifacts.h    # 只 include <Includes.h> 与容器/向量类型
```

更新 7 个 includer（`ProgramFactory.h`、`UniformManager.cpp`、`VulkanRenderer.cpp`、`ProgramInterface.cpp`、`ProgramLinkTask.h`、`ProgramObject.h`、`ProgramTranslationCache.h`），并加 CI 断言：**`ProgramArtifacts.h` 的 `-H` 传递 include 闭包里不得出现 glslang / SPIRV-Cross / spirv_reflect 任何头**。没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。

反射归档**序列化整个结构体**，机制沿用 `PLAN.md` §6.9 的 `Visit()` + `sizeof` 绊线，但**用途改变**：不再是"分歧预言机"（没有可分歧的对象），而是**schema 完整性绊线**：

```cpp
template <class Ar> void Visit(Ar& ar, LinkArtifacts& a) { ar(a.writtenUniformLocationBits, /*…全字段…*/); }
static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
              "新字段请加进 Visit() 并 bump MGL_LINKARTIFACTS_SIZE");
```

归档必须覆盖：四个 `ResourceReflection`（各带 `TypeFacts`）、`uniformSamplerOrImageUnitIndex`（`:1298`）、`uniformBlockBinding`（`:1314`）、`shaderStorageBlockBinding`（按名字，`:1325`）、`explicitOpaqueUniformBindings`（`:1303`）、`xfbVaryings`/`xfbStrides`/`xfbPackedStride`/`xfbNeedsScatteredCapture`（`:1357-1394`）、`computeLocalSize`、GS/TCS/TES 事实（`:1373-1388`）、`usesReservedNumSamples`（`:1345`）、`uniformOffsets`（`:1416`）。

**`XfbVarying`（`:1146-1171`）必须带两套拼写**：GL 名字（Espryt 的 ESSL 驱动侧捕获列表）**和** `blockInstanceName`/`blockName`/`blockMemberIndex`/`blockMemberElement`（`:1163-1170`）。

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
    Uint64 contentHash;              // client 计算；server 的 render-pass memo 键 + **client 侧发射抑制器**
};
```

1. **`readSurface` 是 client 解析后的表面**，按结构消灭 read-buffer-shared-FBO 缺陷类。
2. **`internalFormat` 内联**，四个跨对象 mask（`Managers.cpp:5616-5619`）在 `set_framebuffer_state` 内部零查表推出。
3. **`contentHash` 有两个用途**（v2 强调第二个）：server 的 memo 键（取代 D7 四元组与 D15 三元组）**以及 client 的发射抑制器**——hash 未变就不发这条记录，这是 §2.5 里那 ~175 行去抖搬到 client 后的载体。**同一模式必须推广到每一条 `kVarTail` 的 `set_*`**（`set_sampler_views`、`bind_sampler_states`、`set_shader_images`、`set_shader_buffers`），否则 26.2 的冗余 `glBindSampler` 会让每个 batch 重发一条变长记录。

```cpp
struct MGPSubRegion {                // ★v2：形状照抄已存在的 UnpackStagingBlock（Managers.cpp:4340-4390）
    Int32  x, y, z;                  // 目标 box 原点（level 坐标系）
    Uint32 w, h, d;
    Uint64 srcOffset;                // blob 内偏移
    Uint32 srcRowStride;             // 源行距（字节）；0 = 紧密（= w * bpp）
    Uint32 srcSliceStride;           // 源片距（字节）；0 = 紧密
};
struct MGPSubData {
    MGPipeHandle res;
    Uint16 target, level;
    Uint8  sourceIsVerbatimLevelShadow;  // ★ 取代 backend 里的 `uploadData == mipData` 指针比较
    Uint8  pad[3];
    MGPBox unionBox;                 // union box（server 可选它）
    Uint32 regionCount;              // MGPSubRegion[] 在变长尾（server 可选它们）
    MGPBlobRef blob;
};
```

**同时携带 union box 与 region 列表，由 server 选上传形状。** 这不是冗余：Mali 按**作业数**给纹理上传计价，实测 ~100 个精灵 rect 对一个 union box 是 **+6 ms/frame**（`Managers.cpp:4386-4390`）。client 按 `MipmapStorage::GetDirtyRects` 的语义产生区域形状（96-rect 级联合并 + `summedArea*4 >= unionArea*3` 回退，`MipmapStorage.cpp:300-305`），**决策留在付 GPU 代价的那一侧**。

**v2 关键修正：sub-rect 上传不能再靠指针比较判定。** 今天 `Managers.cpp:4278-4283` 用 `uploadData == mipData` 判"上传源就是整 level shadow"，随后 `:4288-4293` 与 `rectShadowPtr`（`:4321-4326`）用 `levelRowBytes`/`levelSliceBytes` 跨步进**整 level**。在 split 下这个前提不成立：client 若发整 level 就毁掉带宽收益并与 §0.4 的零副本主张矛盾；若发紧密区域则 `uploadData == mipData` 为假，静默退回整 level 上传；若什么都不发就需要 server 侧整 level 镜像——那就是 replica 的 `MipmapStorage`。
**修正**：`MGPSubRegion` 显式携带源步长，`sourceIsVerbatimLevelShadow` 显式携带原来那个指针比较回答的语义问题（"这批字节是未经转换的 level shadow 吗"）。`Managers.cpp:4274-4326` 相应改为**从描述符**取步长而不是从指针算，`UNPACK_ROW_LENGTH` 从 `srcRowStride/bpp` 设。
**注意树里已经有这个形状**：unpack ring 路径的 `UnpackStagingBlock`（`Managers.cpp:4340-4390`）就是 `{src, rowBytes, rows, slices, srcRowStride, srcSliceStride, offset}`，且注释明说 ring 路径把区域**紧密重打包**、因此完全不发 `glPixelStorei`。所以 split 的自然形态就是"永远走紧密重打包 + 描述符"，与 ring 路径同构。
**这项工作从 v1 的"原地不动"移出，计入子系统 5 的天数**（§6.4），并加一个 Mali 设备门发布 box-vs-rect 作业数与帧时增量。

#### 4.5.7 `MGPDrawInfo` 与 `MGHostSpan`

```cpp
struct MGPDrawInfo {                  // = pipe_draw_info
    Uint32 mode;
    Uint8  indexSize;                 // 0 = arrays，否则 1/2/4
    Uint8  flags;                     // kHasUserIndices | kPrimitiveRestart | kIndicesAreClient |
                                      //   kHasIndexRange | kHasXfbCount
    Uint16 pad;
    Uint32 instanceCount, startInstance;
    Uint32 restartIndex;
    MGPipeHandle indexResource;
    // 以下三项**由 flags 门控**，只在有消费者时才计算与携带（v2）
    Uint32 minIndex, maxIndex;        // kHasIndexRange；client 计算，~0 = 未知
    Uint64 xfbCpuCapturedVertices;    // kHasXfbCount；GetTransformFeedbackCapturedVertices()
    MGHostSpan userIndices;           // kHasUserIndices；否则不进变长尾
};
struct MGPDrawRange { Uint32 start, count; Int32 indexBias; };   // = pipe_draw_start_count_bias
```

**v2 成本诚实化**：今天的 `DrawArrays(GLenum, GLint, GLsizei)` 是三个寄存器实参（`BackendObject.h:117`）。替换成一个 ~48 B 的固定头（含 handle）加按需的变长尾。`minIndex/maxIndex` 今天**只**在 client-memory 数组路径算（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3407-3470`，用于 `:3599`），`xfbCpuCapturedVertices` 今天**只**在 XFB scatter 路径读（`DirectGLES.cpp:900`）——所以两者由 `flags` 门控，**不是每 draw 都算**。`userIndices` 的 32 B `MGHostSpan` **移出固定头进变长尾**，让 VBO 路径（MC/Sodium 的全部 draw）不为它付字节。**每 draw payload 字节数进 P0 的计数器直方图**（`cmd-records` 是逐帧的，这里要逐 draw 的分布，它才是 `SEG_CMD` 的定尺依据）。

**`MGHostSpan` 是整份接口里唯一一个"形状随传输而变"的东西**：

```cpp
struct MGHostSpan {                   // 32 B
    const void* ptr;                  // monolith：指向前端 shadow / 应用内存。split：nullptr
    Uint64      size;
    Uint32      seg;                  // split：SEG_STAGE id，或 kFromServerIndexMirror
    Uint32      pad;
    Uint64      offset;
};
inline const void* MGPipeHostBytes(const MGHostSpan&);   // 一次可预测分支
```

**v2 修订的消费者表**（与 §5.8 一致，解决 v1 §4.5.7 与 §5.8 互相矛盾的问题）：

| 消费者 | 今天的站点 | 归属 | monolith 填法 | split 填法 |
|---|---|---|---|---|
| client 顶点数组 | `Managers.cpp:2500-2592`、`VulkanRenderer.cpp:3737` | **client 供字节** | `ptr = attrib.Offset` | tracker 暂存同样范围进 `SEG_STAGE` |
| client 索引数组 | `DirectGLES.cpp:4425-4442`、`VulkanRenderer.cpp:3418-3433` | **client 供字节** | `ptr = indices` | 暂存 `count*indexSize` |
| indirect / parameter 命令块 | `DirectGLES.cpp:4655-4695`、`:4768-4793`、`VulkanRenderer.cpp:12045` | **client 解析计数** | `ptr` 指向 shadow | tracker **解析出计数**并发解析后的 `MGPDrawRange[]`（几十字节） |
| **restart 重写 / multi-draw 展平的索引字节** | `DirectGLES.cpp:4412-4415`、`MultiDraw.cpp:498-540`、`VulkanRenderer.cpp:4159` | **server 拥有变换**（D-B7） | `ptr` 指向前端 shadow | `seg = kFromServerIndexMirror`：**server 从自己的索引宿主镜像取**，零线上流量；镜像超预算时退化为 client 逐 draw 暂存并计数 |

**monolith 代价**：一次可预测分支 + 变长尾里的 32 B（仅 `kHasUserIndices` 时）。它顺带消灭"backend 在 draw 中途回头调前端 reconcile"的大部分：20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 里，凡消费者搬到 client 的那些改由 **tracker 在填 span 之前**做同一次 reconcile（**逐站点对照见 §5.8.1，不是一条笼统规则**）。

### 4.6 与 gallium 的对应与偏离（十条，逐条记名）

| # | gallium | MGPipe | 理由（证据） |
|---|---|---|---|
| **D1** | `create_*_state` 返回 driver 指针 | **调用方提供 handle** | 零创建 round trip；handle 是稠密 slot；退役全部 D 类指针 memo |
| **D2** | `get_param(cap)`、`is_format_supported(...)` 逐项查询 | **一个 `MGPCaps` POD + 一张稠密 format 表** | `DynamicBackendParameters` 与 `FormatCapabilityCache` 本来就是平坦结构 |
| **D3** | CSO 切分是 D3D10 时代的 | **CSO 边界跟 Vulkan 动态状态走** | `RenderState.h:519-528` 记录共用一个版本号让 `glViewport` 冲掉 pipeline memo **和** draw 快路径；`m_pipelineStateVersion`（`:529`）恰好是 CSO 相关子集；Magma 的 `DynamicStateShadow` 与 `ApplyDynamicDrawStateTail` 已经这么切 |
| **D3b（v2 重写）** | 三个独立 CSO：blend / depth_stencil / rasterizer | **一个 `RenderStateCso`，传输是整块 chunk，身份是 pipeline 子集，动态子集走 `set_dynamic_state`** | 整块的理由：`is_trivially_copyable_v` 断言（`DirectGLES.cpp:2035`）、三段 memcmp（`:2038-2047`）、**字段顺序承重**（`RenderState.h:359-368`）、两个 backend 都按 span/bulk 消费。子集身份的理由：整块内容寻址会让 `glViewport` 铸造新 CSO 并冲掉 pipeline memo——即 D3 要防的那次回归。完整性由 G7 的 setter 一致性测试保证 |
| **D4** | `transfer_map`/`transfer_unmap`（scoped） | **`resource_subdata` 推送 + `map_persistent`（永久地址空间捐赠）** | `AcquirePersistentMap`（`BufferObject.h:102-118`）把指针交给**应用**；≥16MiB 自动走到（`:226-228`）。实测 p99 163→21ms |
| **D5** | driver 看得见压缩格式与 pixel-unpack 状态 | **两者都不存在** | 前端在 `glTexImage` 时解析压缩 internalformat（`GL_Texture.cpp:298-306`）；`ScopedDefaultUnpackState`（`Managers.cpp:2888-2910`）强制 unpack 默认值。**只有 PACK 方向过线** |
| **D6** | 默认 uniform block = `constant_buffer 0` | **独立入口 `set_global_constants`** | `SpirvArtifacts::globalUboScratch`（`ProgramObject.h:1418`）是 link **phase B** 产出的 CPU 数组，布局由**优化后**的 SPIR-V 决定（`:1400-1408`）。它没有 GL name、没有 `BufferObject`、没有 `PipeResource` |
| **D7** | `pipe_shader_state` = tokens → 完成的 handle | **handle + server 侧惰性特化**，variant 键取自**已推送**状态 | D-B2 的 8 个输入。这其实**就是** gallium（Mesa 的 `st_variant` 也按已绑定状态键控） |
| **D8** | `pipe_context::flush` + fence 是唯一反向通道 | **`MGPipeCallbacks`**：10 个具名回复/事件（§7） | gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求/终止、default-FB 几何这些词汇 |
| **D9** | `set_viewport_states(start_slot, num)` | **float 数组 + 独立的 `writtenMask`** | viewport 是 **float**（`RenderState.h:229-237`：`KHR-GL43.viewport_array.viewport_api` 用 `==` 无容差）；scissor 必须单独带 `ScissorBoxWrittenMask`（`:363`），因为 `glScissor(0,0,0,0)` 是合法 GL、意思是"拒绝每个片元"（`:352-362`） |
| **D10（v2 新增）** | 纹理参数（swizzle / base-max level / dsMode）住在 `pipe_sampler_view` 里 | **`set_texture_params(res, …)` 独立，`MGPSamplerView` 只带视图限制** | 一张只作 FBO attachment / image 单元 / CopyImage 端点的纹理没有 sampler view，但 Espryt 对 attachment 也调 `SyncTextureParamsToBackend`（`DirectGLES.cpp:1580-1601`），且 `RequireImageBindableStorage` 要在前端 params 版本不动的情况下强制重同步（`Managers.cpp:2815-2821`） |

**没有 `pipe_transfer`、没有 `set_pixel_unpack_state`、没有压缩格式概念、renderbuffer 不折进纹理、`set_sampler_views` 没有 stage 维度。**

### 4.7 覆盖论证

#### 4.7.1 对 477 读点分类的逐类映射

| delta 类 | n | 满足它的 MGPipe 调用 | 残余 |
|---|---|---|---|
| handle 化（wire 句柄） | 167 | 每个命名对象的调用签名里的 `MGPipeHandle` | — |
| RenderStateBlob | 99 | `create/bind_render_state` + `set_dynamic_state` | — |
| ObjectBind:Texture / Sampler | 33 | `set_sampler_views` + `bind_sampler_states` | — |
| ObjectBind:Buffer | 29 | `set_vertex_buffers` / `set_index_buffer` / `set_indirect_buffers` | — |
| ObjectBind:BufferRange | 24 | `set_shader_buffers` / `set_stream_output_targets` | **Uniform 类另带 host payload**（D-B8） |
| FboAttach + DrawBuffers + ReadBuffer | 19 | `set_framebuffer_state` | — |
| Buffer ops delta | 17 | `resource_*` 全族 | — |
| XfbOp | 15 | `set_stream_output_targets` + `*_stream_output` | — |
| ObjectBind:Image | 14 | `set_shader_images` | — |
| ObjectBind:VAO | 12 | `bind_vertex_elements_state` + `set_vertex_buffers` + `set_index_buffer` | — |
| ObjectBind:Program | 10 | `set_draw_program` / `set_dispatch_program` | — |
| TexParam / SamplerParam | 9 | **`set_texture_params`** + `create_sampler_state` + `create_sampler_view` | **v2 修正归属**（D10） |
| Texture state（dirty level/rect） | 7 | `resource_subdata`（带步长描述符） | **归属反转**（§7.3） |
| PixelStoreBlob | 6 | `set_pixel_pack_state` | unpack **删除** |
| client-resolved（error queue） | 6 | `on_gl_error` 回调（§7） | — |
| ProgramPublish | 3 | `create_shader_state` | 依赖 P0.5 |
| client-resolved（validation） | 3 | client 自答 | — |
| CurrentAttrib | 2 | `set_vertex_attrib_defaults` | — |
| client-resolved（compile env） | 2 | `on_caps_invalidated` | — |
| Patch 参数 | — | `set_patch_state` | 同时是 variant 输入 |
| 条件渲染 | — | **client 解析，永不过线** | `Core.h:387-391` |
| XFB CPU 计数 | — | **纯 client**；`MGPDrawInfo::xfbCpuCapturedVertices`（flag 门控） | — |
| backend 重铸纪元 | — | **无 client 对应物**：`MGGen`，server 私有 | — |

那 1997 个前端 getter 站点不是第二个面：89 个纯版本读**根本不过线**，72 个数据字节读全部落在 §5.7/§5.8 与 `MGHostSpan`，38 个 `GetLifetimeId()` 变成 handle。

#### 4.7.2 覆盖论证不是这张表，是这三道门（v2：从两道增至三道）

上表是**声明**。证明是机械的：

**门 A —— include 图门（v2 新增，取代 v1 单靠 `nm` 的那半）。**
v1 说 `MG_Backend` 只允许 include "一张共享**值**头白名单（`RenderState.h` 的 `RenderStateParameters`、`SamplerObject.h` 的 `SamplerParameters`、…）"。**实测这张白名单不是叶子集**：`RenderState.h:12` include `FramebufferState/FramebufferObject.h`，后者 `:12-13` 再 include `TextureState/TextureObject.h` 与 `RenderbufferState/RenderbufferObject.h`；依赖是结构性的——`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 给两个数组定长（`RenderState.h:263, 273`）。所以"把 `RenderStateParameters` 交给纯净的 `MG_Backend`"会把整张 framebuffer/texture/renderbuffer 类图一起拖进来。**而 `nm --undefined-only` 看不见这个**：只 include 而不调用其成员函数的类不产生未定义符号，门可以在 include 图完全耦合的情况下为绿。
**修正**：P0.5 交付 `MG_Pipe/MGPipeValueTypes.h`——把 `MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute` 与相关枚举搬进去，**它不 include `MG_State/GLState` 的任何东西**；`RenderState.h`/`SamplerObject.h`/`VertexArrayObject.h` 反过来 include 它。门变成：

> **在 disaggregated 配置下编译 `MG_Backend` 时，把 `MG_State/GLState` 从 include 搜索路径里移除**（或对 `-H` 输出断言）。这是唯一一条能因它存在的理由变红的检查。

**门 B —— 符号门。** `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空。保留，作为门 A 的补充（它能抓到通过前置声明+跨 TU 调用绕过 include 图的情况）。

**门 C —— 未声明门。** 在 `MOBILEGL_PIPE_PUSH=all` **且非 verify** 构建里，`MG_State::pGLContext` **未声明**。任何接口没满足的读是一次**指名文件与行号的编译错误**。strangler 结束时 `grep -c 'pGLContext' MG_Backend/` == 0（**grep `pGLContext` 不是 `pGLContext->`**，因为还有 58 行非箭头用法）。**这条门只跑非 verify 构建**（D-B5：verify 构建保留 `SnapshotFromGLContext()`）。

**这三道门比生成一张 477 行的清单严格得多：它们禁止那次读，而不是给它编目，而且不会过期。** 那份 inventory 保留为 tracker 侧覆盖检查表（G6，CI `git diff --exit-code`，0 UNMAPPED）。

#### 4.7.3 21 条 D 类身份 memo 的重键表

| # | 今天的键 | 守什么 | MGPipe | 净效果 |
|---|---|---|---|---|
| D1 | `StateBackendObjectRegistry` 用裸 `StateObject*` + 同址 `weak_ptr`（`Managers.h:282-325`）×6 | 分配器地址复用；**也是唯一的删除信号** | 按 slot 索引的数组 + `gen` 比较；显式 `resource_destroy` | GC（1024/64 阈值）**删除** ×6 |
| D2 | `TwinLookupMemo` ×3 + `OwnerEquals`（`DirectGLES.cpp:62-131`） | 复用堆地址命中 memo 槽 | **删除**——数组下标**就是**查表 | ~75 行 + 140KiB |
| D3 | `UnitTextureSyncEntry` + `PairingsIntact`（`:1441-1481`） | 不移动任何计数器的 slot 交换（DSA by-name） | **server 侧删除**；**去抖搬到 client**（§2.5：`set_sampler_views` 的 client 侧 hash 抑制器，否则冗余 `glBindSampler` 会 per-batch 重发） | server −115 行 / client +~60 行 |
| D4 | `IsBufferDrawClean` 身份优先比较（`Managers.cpp:1436`） | respecify 交给前端一个**新**资源 | server 拥有资源表；`gen` 比较；`GetChangeSerial()`（`Uint64`，不回绕）继续过线 | 简化 |
| D5 | `ResolvedDrawBuffers::iboFrontend`（`Managers.h:711-716`） | 索引 slot 重绑而无 epoch/config 移动 | `set_index_buffer` 是独立调用 | 结构性 |
| D6 | `m_syncedIndexBufferObject` 陪一个回绕 `Uint16`（`:775-780`） | 版本回绕后换了个 buffer | `{slot, gen}` 比较，不回绕 | 结构性 |
| D7 | `StampSyncedFBO` 四元组（`DirectGLES.cpp:1856-1901`）；`packed_pixels` postmortem `:2815-2827` | 版本回绕 + backend 侧纹理重铸 | `MGPFramebufferState::contentHash` + server 私有 `attachmentRemintEpoch`（`MGGen`） | 一次 64 位比较 |
| D8 | `g_fboTextureSyncList`（`:1580-1601`） | 同 D3，针对 attachment | server 侧删除；由 `contentHash` 在 client 侧抑制 | server −20 行 |
| D9 | `ResolvedTextureBindingMemo`：9 个键 + 驱动绑定影子的 `memcmp`（`:3218-3291`） | 任何未枚举的写者扰动某个 unit | `(shaderCso.slot, viewSetSerial)` 两字比较；`viewSetSerial` 由 server 在 `set_sampler_views` **内部** ++。**前提是 client 侧的 hash 抑制器已经挡住冗余推送**，否则这个 serial 每个 batch 都动 | 更便宜（有前提） |
| D10 | `UnitSamplerLookupMemo` 的 `WeakPtr` owner 测试（`:3105-3125`） | 死 sampler 复活 | 数组下标 | 删除 |
| D11 | `VertexInputStateFactory::ComputeHash` 混入 `GetLifetimeId()`（`:38-49`） | 复用 buffer 地址重现整个 content hash | CSO handle **就是**身份；`gen` **混进** server 侧每个 content hash | 删除一整类 |
| D12 | `SetBackendStateMemo(&entry, evictionEpoch)`：**前端 VAO 里存后端堆裸指针**（`VertexInputStateFactory.cpp:78`） | table 淘汰 | **直接删除，不翻译** | — |
| D13 | `VaoDrawMemo` 槽（`VulkanRenderer.h:1230-1245`） | ABA | CSO handle | 2 字 |
| D14 | `SetupDrawSnapshot` 的三组 `(ptr, lifetimeId, version)` + **有损的** `sampledContentSum`/`sampledParamsSum` | 一切 | 三个 handle + 两个 server 纪元 + dirty mask | ~14 个探测字段 → 1 次比较；**顺带消灭一类哈希碰撞** |
| D15 | `m_rpFast*`（`VkRenderPassManager.h:305-320`） | ABA | `contentHash` + `MGGen` | 1 次比较 |
| D16 | `VkTextureManager::TextureIdentity` + `GetTextureObject(name)` 存活探测（`VkTextureManager.cpp:806-819`） | 名字复用 / 删了但仍被 FBO 引用 / 默认纹理 | `{slot, gen}` + 显式 destroy | 三种失效模式一起消失 |
| D17 | `VkClearManager::TextureIdentity`（`VkClearManager.h:76-83`） | ABA | `{slot, gen}` | — |
| D18 | 纹理/renderbuffer 资源用**节点式** `std::unordered_map`（postmortem `VkRenderPassManager.h:375-397`） | 扩表搬迁使缓存的 `Resource*` 失效 | **UNCHANGED。** 接口零约束；这是 server 内部分配纪律。**postmortem 注释必须逐字带进 review checklist** | 保留 |
| D19 | `ProgramFactory::m_cacheStructureEpoch` | 守 server 内部裸指针 | **UNCHANGED**（`MGGen` 族） | 保留 |
| D20 | `ConvertedVertexStreamKey` + **纯为防地址复用**持有的 `SharedPtr sourcePin` | ABA | server 拥有资源；`changeSerial` 过线 | **pin 删除** |
| D21 | `m_xfbCounterSlotByObject[GetBoundTransformFeedbackName()]`（`VulkanRenderer.cpp:11136-11146`） | **什么都没守——活的潜伏 bug** | XFB 对象 handle | **顺带修一个 bug**，先独立落 `dev` |

**总计：11 条直接删除，2 条（D3/D8）server 删除但去抖搬到 client，7 条重键成更便宜的比较，1 条（D18）原样不动。**

---

## 5. 前端 state tracker

### 5.1 推送发生在哪里——本设计里最容易做错的一个决定

**不在 GL setter 里。** `glEnable(GL_BLEND)` 绝不调 `bind_render_state`。Blaze3D 每个 batch 都用它包住，代码自己标注它是最热的路径（`DirectGLES.cpp:2029-2032`）。天真的 per-setter 推送把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表——**严格慢于今天**。

**在 verb 之前的 validate 时刻。**

```cpp
// MG_Impl/Pipe/Tracker.h
class MGPipeTracker {
public:
    // 每一类 verb 一个入口；由 PipeCalls.def 的 kCtxVerb / kCtxObject 条目生成（§6.2.1）
    void ValidateForDraw(const MGPValidateHint&);   // 20 个 GL draw 入口
    void ValidateForDispatch();                     // glDispatchCompute*
    void ValidateForClear(GLbitfield);              // framebuffer + 渲染状态（ClearColor 在其中）
    void ValidateForBlitOrCopy();                   // framebuffer + pack state
    void ValidateForTextureOp(MGPipeHandle res);    // GenerateMipmap / CopyTex* / BindImageTexture
    void ValidateForReadback();                     // ReadPixels / GetTexImage
    void ValidateForXfbSpan();                      // Begin/End/Pause/Resume TransformFeedback
    void ValidateForQuery();                        // query begin/end
private:
    Uint64 m_dirty;
    Uint64 m_lastPushed[kGroupCount];
    Uint64 m_lastSetHash[kVarTailGroupCount];       // ★ kVarTail set_* 的发射抑制器（§2.5）
};
```

**这八个入口不是随手列的**：`MG_Impl` 用到 **70 个不同表项 / ~93 个调用点**，其中只有 ~22 个是 draw/dispatch，其余 ~48 个是纹理操作、回读、blit、clear、XFB 跨度、query——**而它们中很多自己就读 `pGLContext`**（§2.1(a) 列了具体行号）。v1 只给 4 个 validate 入口、只在两处填快照，会让第一个 `glGenerateMipmap`/`glReadPixels` 撞上 poison Fatal，`MOBILEGL_PIPE_VERIFY` 的全绿验收因此不可达。

#### 5.1.1 哪些操作在 GL 调用时刻推送（v2 修正推论 1）

**规则的正确措辞**：

> **只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook（`BufferObject.h:70-71` 自己写着"在 GL 调用时刻分发，就在 shadow 拷贝刚更新之后"）。**纹理 subdata 不在此列。**

理由：`glTexSubImage*` **根本不调 backend 表**（`GL_Texture.cpp` 只有 3 处 `MarkStorageDirtyRegion`），全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做，那里才跑 96-rect 级联合并与 union-box 回退，并在 unpack ring 可用时刻意塌成一个 box（`Managers.cpp:4386-4390`，实测 +6 ms/frame）。逐 `glTexSubImage` 发一条 `resource_subdata` 精确复现那个 ~100 作业的形状。

**因此纹理路径的形态是**：client 在自己的 `MipmapStorage` rect 模型里累积（§7.3 的发射游标），在**下一个 validate / flush 点**把合并后的形状作为**一条** `resource_subdata`（带 union box + region 列表）发出。`MOBILEGL_PIPE_STATS` 必须把逐帧 `resource_subdata` 发射次数单列一类，并在 MC 动画图集 fixture 上设上限。

**稳态成本**：见 §10.2（v2 已按动态口径重写）。

### 5.2 dirty bits：值类零新增记账，对象类新增 5 个聚合世代（推论 4）

| dirty 位 | 类别 | 快门来源 |
|---|---|---|
| `NEW_RENDER_STATE` / `NEW_PIPELINE_STATE` | 值 | `m_version` / `m_pipelineStateVersion`（`RenderState.h:522, 529`；bump 点 `RenderState.cpp:311-312` 等） |
| `NEW_PIXEL_PACK` | 值 | `PixelStoreParameters`（`RenderState.h:190-199`） |
| `NEW_PATCH_STATE` | 值 | patch 三字段，用 `BitwiseEqual` 比较（NaN 合法，`DirectGLES.cpp:2807-2814`） |
| `NEW_VERTEX_ATTRIB_DEFAULTS` | 值 | `GetCurrentVertexAttribute` |
| `NEW_VERTEX_ELEMENTS` | 值 | `VertexArrayObject::GetConfigVersion()`（`Uint32`，`:155`） |
| `NEW_VERTEX_BUFFERS` | **对象** | **`VertexArrayState::m_anyVaoAttributeGeneration`**（新增）→ 命中后走 32 属性前缀 + 逐属性 `VertexAttributeVersion`（`:66-70`） |
| `NEW_INDEX_BUFFER` | **对象** | 索引 slot `GetVersion()`（回绕 `Uint16`）+ 绑定对象 `{slot,gen}` |
| `NEW_FRAMEBUFFER` | **对象** | **`FramebufferState::m_anyAttachmentGeneration`**（新增）+ `GetObjectVersion()` + slot 版本 → 命中后重算 `contentHash` |
| `NEW_SAMPLER_VIEWS` | **对象** | **`TextureState::m_anyTextureContentGeneration` + `m_anyTextureParamsGeneration`**（新增）+ `GetTextureBindGeneration()` + `GetSamplingResolutionGeneration()` → 命中后走 `GetMaxTouchedUnit()` 前缀、重算集合 hash、**hash 未变则不发** |
| `NEW_SAMPLERS` | **对象** | `SamplerObject::GetVersion()`（回绕 `Uint16`，`SamplerObject.h:155`）+ 上面的聚合 |
| `NEW_SHADER_IMAGES` | **对象** | `ImageTextureBinding::Version`（`TextureState.h:24, 34`）+ `m_anyTextureContentGeneration` |
| `NEW_SHADER` | 值 | `GetLinkVersion()` + `GetImageUnitVersion()`（`ProgramObject.h:844, 906`） |
| `NEW_SHADER_BINDINGS` | 值 | `GetBackendStateVersion()`、`GetBlockBindingVersion()`、`GetUniformWriteSetVersion()` |
| `NEW_GLOBAL_CONSTANTS` | 值 | `GetUBOContentVersion()`（`~0u` 跳过回绕，`:791-794`） |
| `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` | **对象** | **`BufferState::m_anyBufferChangeGeneration`**（新增）+ slot 版本 → 命中后走 `GetTouchedBindPointCount()` 前缀 |

**五个新增聚合世代**（`TextureState` 两个、`BufferState`、`VertexArrayState`、`FramebufferState` 各一）**全部落在既有 bump 点上，合计约 20 行**。它们把对象类组的快门从"每 validate 走查 192 个单元 / 84×4 个绑定点 / 32 个属性 / 40 个 attachment"降成一次 `Uint64` 比较；只有快门为真时才走 touched 前缀并重算集合 hash。

**完整性由 `gen_pipe_dirty_surface.py` 保证**（推论 4）：它枚举 `MG_Impl/GLImpl/**` 里每一个会改变某组的 mutator，映射到必须 bump 的聚合世代，CI 重生成 + `git diff --exit-code`，**未映射的 mutator 直接失败**。这是 `PLAN.md` 的 `gen_impl_mutation_surface.py` 的改造版（replay 义务消失、标记义务出现），也是 B-R6 的第四层。

**三个回绕的 `Uint16` 在 tracker 边界加宽。** `m_lastPushed[]` 是 tracker 自己的字段，加宽到 `Uint32`/`Uint64` **不需要改 `MG_State` 一行**；同时 handle 与它同行过线。**回绕在 tracker 本地是无害的**（一次回绕造成一次多余的重推，永不漏推），何况集合 hash 抑制器会把多余重推吞掉。

### 5.3 每命令 validate 的**不变式**（v2：从"固定顺序契约"降级）

**规范条款（D-B3 v2）**：

> 一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间**没有**顺序要求。

**推荐实现顺序**（便于 tracker 的代码组织与 dirty 位遍历，**不是**正确性契约）：

```
1  set_framebuffer_state
2  set_draw_program（create_shader_state 在 link 时刻已发）
3  set_texture_params / set_sampler_views / bind_sampler_states / set_shader_images /
   set_shader_buffers / set_global_constants
4  bind_render_state（未命中时先 create_render_state）/ set_dynamic_state
5  bind_vertex_elements_state / set_vertex_buffers / set_index_buffer / set_vertex_attrib_defaults
6  set_patch_state / set_stream_output_targets
7  draw_vbo
```

**退役 workaround 的机制是惰性特化，不是调用顺序**：`DirectGLES.cpp:2712-2732` 的 fragColor 重推导与 `g_broadcastMemo*` 之所以能删，是因为 server 在 **verb 处**才特化，那时 `set_framebuffer_state` 一定已到；同理 `ImageUnitFormatsStillMatch`（`Managers.cpp:6545-6573`，注释明说"不可表达为单调版本"）由 `set_shader_images` 在 verb 之前告知。**v1 把这归因于"framebuffer 严格第一"，但它自己把 images 排在 program 之后——那个论证站不住，结论仍然成立。**

`create_shader_state` **从编译池的终止 continuation 发出**（`JobNode.h:109-123`），不是从 draw 发出，这样 SPIR-V 在用到它的第一个 draw 之前就到达 server。这是 monolith 拿不到的异步收益。

### 5.4 合并：保留代码库已经发现的三条，加上第四条

1. **整块结构优于逐字段。** Magma 的 `ComputePipelineStateHash`（`VulkanRenderer.cpp:4818-4826`）已经把 ~17 次 accessor 调用换成一次 bulk fetch；Espryt 的三段 memcmp 同理。
2. **高水位标记。** `BufferState::TouchBindPoint` / `GetTouchedBindPointCount`（`BufferState.h:51-62`，每 target 84 个绑定点）与 `TextureState::NoteUnitTouched` / `GetMaxTouchedUnit`（`Core.h:124-126`，192 个单元）**必须留在 tracker 的走查里**，它们直接就是 `set_shader_buffers` / `set_sampler_views` 的 `count` 实参。
3. **只发 program 解析过的集合**，用 `LinkArtifacts::uniformSamplerOrImageUnitIndex`（`ProgramObject.h:1298`）。两个 backend 今天已经在算（`ResolveAndBindUnitTextures`，`DirectGLES.cpp:2973`；`UniformManager::CollectSampledTextures`）。
4. **（v2 新增）集合 hash 抑制器。** 每一条 `kVarTail` 的 `set_*` 在 client 侧算一次已解析集合的 xxHash，与 `m_lastSetHash[]` 比较，**未变就不发**。这是 §2.5 里那 ~175 行去抖搬到 client 后的载体，也是 D9 的前提——没有它，`GetTextureBindGeneration()` 在冗余重绑时的 bump（`DirectGLES.cpp:1414-1420`，26.2 每次纹理单元切换都重绑同一个 sampler）会让每个 batch 重发一条几百字节的变长记录并冲掉 server 的两个 memo。

**索引绑定的范围必须在 validate 时刻实时解析，不是在 bind 时刻快照。** `BindingSlotRange1D::GetRange()` 对整 buffer 绑定返回 `Range1D(0, object->GetSize())`，因为 `glBindBufferBase` 之后再 `glBufferData` 是普通应用代码。

### 5.5 sampler view 在 client 侧解析

GL 是**每个 unit 每个 target 各一个绑定**（`TextureUnit.h:20, 24-25`；`TextureState::m_textureUnits` 是 `Array<TextureUnit, 192>` **按值**存放，`TextureState.h:128`，每 stage 广告上限 32，`:46`），shader 看见哪一个取决于 sampler uniform 的声明类型、mipmap 完备性（`IsMipmapCompleteForFilter`，`TextureObject.h:309`；`SamplesAsIncompleteTexture`，`:315`）和 `IsUndefinedDefaultTexture`（`:329-332`）。**gallium 的"每槽一个 view"就是解析后的形态。**

**解析留在 client**，并且 client 必须为它保留一个自己的 memo（§2.5 的 ~40 行搬迁项），否则每 draw 重跑完备性规则。**合并单元空间，无 stage 维度**（§4.4.3）。

**两处 backend 特定的后处理留在 server**，作用在已解析的集合上：Espryt 的 raw-depth-fetch sampler 替换（`DirectGLES.cpp:3540-3546`）与 Magma 的 feedback-loop 检测（对着 draw FBO，`UniformManager.cpp:554`）。两者都可从已推送的 `set_framebuffer_state` + view 集合判定。

### 5.6 对象生命周期、共享组与 composite pipeline program

#### 5.6.1 生命周期

`resource_create` 在**前端对象构造**时发，存储由 `resource_respecify` 惰性定义。`resource_destroy` 在前端对象析构时发。三条顺序约束：

- **view 先于其存储属主销毁**：`GetViewStorageOwner()`（`TextureObject.h:96-100`）→ `MGPResourceDesc::viewOf` + server 侧 keep-alive。
- **FBO attachment 钉住纹理**（`FramebufferObject.h:95`）→ `set_framebuffer_state` 的 surface handle 隐含 server keep-alive。
- **buffer texture 钉住 buffer，范围实时解析**（`TextureObjectBuffer.h:28, 35-46`）→ `MGPResourceDesc::{bufferForTexBuffer, bufOffset, bufSize}`。

#### 5.6.2 共享组

v1：一个 screen、一个 context、一个扁平 handle 空间、一条 flow。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射——**顺手修今天不取该锁的两个入口**：`ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）。

#### 5.6.3 composite pipeline program：判过死刑的那个反对意见，答案是"什么都不用做"

`GLContext::GetProgramForDraw()`（`Core.cpp:592`）**今天就已经完全在前端**完成合成：join 每个 stage 的 `JoinLinkAndSpirv()`、按 `ComputeDrawProgramSignature()`（`:630`）查 cache、miss 时构造**故意不命名**的 `MakeShared<ProgramObject>(0u)`（`:644`）、挂上每个 stage 被钉住的 linked snapshot、重装捕获 stage 的 XFB varyings、`Link(true)`、缓存、`RefreshCompositeUniforms`。

tracker 调它，拿到 `SharedPtr<ProgramObject>`，推**一个 handle**。合成体没有 GL name，但**有 lifetimeId**，slot 从 `ShaderCso` 的保留高位段分配。生命周期：pipeline cache 淘汰该条目时释放 slot、`gen++`、发 `delete_shader_state`——`CompositeResolver.cpp` 里三行。

**合成体从不过线、从不被重新实现，`PLAN.md` 提议的 `SetReplicaResolvedDrawProgram` 钩子完全不需要。** 副带收益：阻塞的 `JoinLinkAndSpirv()` 彻底离开 server 的 draw path。

### 5.7 program artifacts 与全局 UBO scratch

**`create_shader_state` 的 payload 是 SPIR-V + 全结构体反射归档**（§4.5.5），不是源码。**依赖 P0.5 的头文件抽取。**

**SPIRV-Cross 留在 server**（`TranspileSpirvToEssl`，`Managers.cpp:6575`）：它消费 SPIR-V 加设备事实。**glslang 留在 client。** 这是一次文件级切割。

**全局 UBO scratch 走独立入口**（D6）：`set_global_constants(shaderCso, MGPBlobRef bytes, Uint32 version)`，键 `(shaderCso.slot, uboContentVersion)`，复现 `DirectGLES.cpp:3369-3392` 的"每 program 每帧至多一次"。

**具名 UBO 字节走 `set_shader_buffers` 的 host payload**（D-B8）：`UniformManager::ResolveUniformBufferPayload` 在 `UniformManager.cpp:2022` 调 `SyncPersistentMappedRange()`、`:2052` 读 `MappedData() + rangeStart` 打进 **Magma 自己的 UBO ring**——消费者在 server，搬不走。由 `kCapNeedsHostUboBytes` 门控（Espryt 直接绑给驱动，不需要）。**逐帧字节量进 `stage-ubo-named` 计数器；在 P0 给出数字之前不冻结这个 payload 的形状。**

**backend 侧 program link/compile 失败不需要任何同步返回，也不需要新事件种类。** 实测：`SyncToBackend` 在 `Managers.cpp:8091` link、`:8094` 读 `GL_LINK_STATUS`、`:8095` 折进 `m_backendProgramUsable`、`:8097-8101` 取驱动日志、`:8106` 发 `MGLOG_E`；`Use()` 随后绑 program 0（`:8357`）并 `MGLOG_E_ONCE`（`:8364-8372`）。**没有 GL error、没有 `ProgramObject` 变更、`GL_LINK_STATUS` 永不撤回**（`:7098`、`:7247-7249`、`:6478`、`:7827`）。同步查询由 client 从 `ProgramObject` 回答（`GL_Program.cpp:851` → `ProgramObject.h:913`）。所以 `on_log` 逐字复现它——**但由此推出一条对 `PLAN.md` §7.4 的强制修正，见 §7.4**。

### 5.8 emulation 所需前端数据的显式传递（v2 按 D-B7 重写）

归属规则：**驱动表达不了的变换在 state tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering**。**v1 用 cap 位门控 emulation 归属的做法对 restart 与 multi-draw 不可表达（D-B7），此处收回。**

| emulation | 归属 | 门 | 过线的是什么 |
|---|---|---|---|
| **client 顶点数组**（`Managers.cpp:2500-2592` 把 `attrib.Offset` 当应用裸指针，每 draw 每属性上传 `(first+count-1)*stride+elementSize`；`VulkanRenderer.cpp:3737` 是**唯一无界**的应用指针读） | **client**（它拥有地址空间） | — | **字节，永不是指针**（`MGHostSpan`） |
| **索引扫描**（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3407-3470`，用于 `:3599` 给上一条定界） | **client**（只有它同时持有两个数组） | — | `MGPDrawInfo::minIndex/maxIndex`（`kHasIndexRange` 门控），`~0` = 未知 |
| **client 索引数组** | client | — | `MGPDrawInfo::userIndices`（`kHasUserIndices` 门控） |
| **primitive-restart 重写**（`DirectGLES.cpp:4368-4470` 整 EBO 重写，`kMaxRestartRewriteBytes = 1<<26` = 64 MiB，`:4218`；`VulkanRenderer.cpp:4159-4161`） | **server（v2 改：v1 曾说 client）** | `kCapNeedsHostIndexBytes` → 索引宿主镜像 | **零线上流量**：server 从镜像读。**monolith 行为零变化**，诊断仍落在原线程（开放问题 12 关闭） |
| **multi-draw 分档 + 展平**（`MultiDraw.cpp:282-320` 的 `ResolveTierForBatch` **逐 batch** 在五档里选，输入含 `programReadsDrawID`——**转译出的 ESSL 的性质，只存在于 server**；容量判定 `kMaxFlattenedIndices` `:72` / `kMaxComputeFlattenedIndices` `:82`；自动阶梯 Ext→BaseVertex→MultiIndirect→Indirect→DrawElements `:241-243`，CPU 展平是**回退**） | **server，全部五档**（v2 改） | `kCapNeedsHostIndexBytes` | `draw_vbo(info, indirect, MGPDrawRange[], numDraws)`；索引字节走镜像 |
| **`*IndirectCount` CPU 回退**（`DirectGLES.cpp:4655-4695` 从 `parameterBuffer->MappedData()` 读实际 draw 数） | **client** | — | client 从自己的 shadow 解析计数，发解析后的 `MGPDrawRange[]`（几十字节）。**注意它今天只调 `SyncPersistentMappedRange()`，不调 `SyncGpuWrites()`**（§5.8.1） |
| **viewport-array N 遍回放**（`DirectGLES.cpp:3742-3846`，今天包住 14 个 draw 入口） | **server** | `kCapViewportArray` | 无新增：16 组 viewport/scissor/depth-range 已在渲染状态里 |
| **fp64 顶点窄化**（`Managers.cpp:2518-2557`） | **server**（后端格式决策） | `kCapFloat64VertexAttrib`（`BackendObject.h:487-500` 明说它与 `SupportsShaderFloat64` **独立**） | 原始字节；`IsLong` 与 `Type` 分开过线 |
| **image-bindable 存储加宽/拆分**（`Managers.cpp:2789-2822`、`:4620-4630`） | **server** | — | 正向 `imageBindableHint`；反向 `on_texture_pull_request` + 终止符（§7.5） |
| **生成 mipmap 的前端存储** | **拆开**：client 分配 level 存储，server 生成 | — | `MGPMipPlan`；`on_mip_levels_generated` **只带形状不带字节**（见 §9.1 的说明）；CPU 回退路径的纹素由 `on_texture_writeback` 回来 |
| **CopyImage shadow 镜像**（`DirectGLES.cpp:7065-7140`） | **client** | — | 只回"拷贝成功"。**删掉一整条 server→client 字节通道** |
| **XFB CPU 图元计数**（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`） | **纯 client** | `kCapCpuXfbPrimitiveAccounting` | `MGPDrawInfo::xfbCpuCapturedVertices`（flag 门控）+ `end_stream_output` 的 `MGPXfbAccounting` |
| **XFB scatter 的 read-modify-write**（`DirectGLES.cpp:893-960`） | **client（v2 新增行）** | — | 见 §7.2 的 `on_buffer_writeback` 修正 |
| **压缩纹理 / pixel unpack 规整** | **纯 client** | — | 无 |

#### 5.8.1 陈旧索引纪律——**逐站点**表，不是一条笼统规则（v2 修正）

v1 写"上表里每一次 client 侧扫描/重写，在 monolith 里都紧跟在 `SyncPersistentMappedRange()` + `SyncGpuWrites()` 之后"。**对 `*IndirectCount` 不成立**：`DirectGLES.cpp:4666-4667` **只**调两次 `SyncPersistentMappedRange()`，然后在 `:4690-4694` 直接读 `MappedData()`；**没有 `SyncGpuWrites()`，因此今天没有停等**。而 `SyncGpuWrites` 才是触发 `ReadbackFromGpu`（`BufferObject.cpp:265-274`）的那一条。照 v1 的笼统规则实施，`glMultiDrawElementsIndirectCount` 会平白获得一次 publish-and-wait round trip——而 trace 语料里恰好有 `minecraft-1.21.1-neoforge-create-indirect-in-world`（Create/Flywheel，indirect 与 parameter buffer 每帧被写），于是这会变成一个**逐帧逐 batch 的同步 round trip**，而 §9.2 第 10 行还把它写成"常见情况代价为零"。

**逐站点 reconcile 表（必须逐字复现 monolith 的集合，不多不少）：**

| client 侧动作 | monolith 对应站点 | 必须做的 reconcile |
|---|---|---|
| client 顶点数组范围计算 + 暂存 | `Managers.cpp:2500-2592`（无 buffer，源是应用指针） | **无**（应用内存，无 GPU 写者） |
| 最大索引扫描（EBO 源） | `VulkanRenderer.cpp:3406-3470` 前的 `:3431` | `SyncPersistentMappedRange()` **+** `SyncGpuWrites()` |
| 最大索引扫描（client 索引源） | 同上，client 指针分支 | **无** |
| `*IndirectCount` 计数解析 | `DirectGLES.cpp:4666-4667`、`:4768-4793` | **只** `SyncPersistentMappedRange()`。**不加 `SyncGpuWrites()`** |
| （server 侧）restart 重写 | `DirectGLES.cpp:4412-4413` | server 从镜像读；镜像由 subdata 流维护，**GPU 写者的可见性由 `on_gpu_written` 收窄集驱动**——server 侧本地判定，无 round trip |
| （server 侧）multi-draw 展平 | `MultiDraw.cpp:498-499` | 同上 |

**client 侧需要 reconcile 的那两条的形态**：publish → 等 `appliedSeq` → 排空事件 → 再碰 shadow。跳过它，`maxIndex` 来自陈旧字节，顶点数组被少拷 → 几何缺失，或越界读应用数组。

门：`ClientArrayAfterComputeWriteScenario`（新增），**必须能因它存在的理由变红**。
门：`create-indirect` fixture 上的 `roundtrips-per-frame` 计数器**必须读零**（P8 验收），这是上面那条"不加 `SyncGpuWrites()`"的绊线。

**另注**：monolith 在 `*IndirectCount` 上不调 `SyncGpuWrites()` 本身可能是一个潜在缺口（compute 写的 indirect buffer）。**那是一个独立的 `dev` 问题，拆分不得借机"顺手修"**——那会改变基线并让逐名对比失去意义。列入开放问题。

---

## 6. 后端状态机改造

### 6.1 什么原样不动（先说这个，因为它是"最短可信改造"的依据）

**每一个 ring、pool、arena、quirk、lowering pass 原地不动：**

Espryt：三条 persistent-mapped ring、`PersistentRing` 的分配/背压算法、buffer pool、全部 7 条 fallback-repack 路径（`Managers.cpp:3209-3527`）、`m_backendColorSlots` draw-buffer 置换表、三个 scratch FBO 及其驱动侧 attachment 影子、`PackState`、全部驱动绑定影子、Adreno 的"禁用属性无指针 SIGSEGV" workaround（`Managers.cpp:2371-2380, 2427-2433`）、Mali 的 XFB 捕获丢失 workaround（`DirectGLES.cpp:400-410`）、`ScopedDefaultUnpackState`、SPIRV-Cross 会话与 6 次 post-emission ESSL 重写、驱动 POST 自检族、**restart 重写与 multi-draw 五档**（D-B7）。

Magma：`VulkanRenderer` 全部 memo 与 scratch、`PipelineFactory`、`ProgramFactory`、`UniformManager` 的 ring 与描述符集、五个 `Vk*Manager`、`FrameContext`、`SwapchainObject`、`DynamicStateShadow`、`VertexInputStateFactory` 的 cache **本体**、**以及 D18 的节点式容器纪律**。

**v2 从"原样不动"里移出的一项**：`Managers.cpp:4274-4326` 的 sub-rect 上传判定与跨步计算——它今天靠 `uploadData == mipData` 指针比较与整 level 步长算术，split 下不成立（§4.5.6），必须改成从 `MGPSubRegion` 描述符取步长。**这不是 v1 说的"只把输入从拉取的 shadow 指针换成 `MGPBlobRef`"，是真代码改动，计入子系统 5。**

**唯一两处必须真改的 `MG_State` 类型内部用法**：

1. **Magma 的占位纹理**（`UniformManager.cpp:161-181, 1416-1500, 1624-1634`）：构造真的 `TextureObject2D` / `TextureObject2DMultisample` / `TextureObject2DMultisampleArray`，走 `SetInternalFormat(RGBA8)` / `AllocateStorage({1,1,1},4)` / `UpdateMipmapSubData` / `MarkStorageDirty` / `SetSamples(2)`（VUID-RuntimeSpirv-samples-08726）/ `TruncateMipmapLevels(1)`，**唯一理由**是让"未绑定单元"复用 `SyncTextureAndGetDescriptor(ITextureObject&)` 这个签名。改成 backend 自己分配 `VkImage` + view + descriptor：**~120 行前端对象木偶戏变成 ~60 行直白的 VMA/Vulkan，34 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个随之消失。**
2. **Magma 的两个内部 shader**（`InitializeBlitResources` `VulkanRenderer.cpp:4210-4283`、`InitializeDepthMipmapResources` `:4287-4356`）：**烘焙成 SPIR-V。** 方式：把生成的 SPIR-V、uniform location、UBO 布局作为生成头文件签进树，用一个 `MG_Test` 重跑树内 glslang 对同一批源码字符串并逐字节比对守新鲜度。不用构建期 host glslang target。`uSource` 的描述符绑定本来就由 `ProgramFactory` 自己的 SPIRV-Reflect 走查找到（`:4340-4350`），原样存活。**顺带把一次 glslang 编译从 monolith 启动路径上删掉。**

Espryt 有一个小号同类：`g_rawDepthFetchSamplerState`（`DirectGLES.cpp:166-179`）→ backend 原生 sampler 记录，~40 行。

### 6.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充器 + poison 世代

```cpp
// MG_Backend/MGPipe/PipeInputs.h
namespace MobileGL::MG_Pipe {
struct PipeInputs {
    // 阶段 A：字段类型与 backend 今天读到的**完全一致**
    const RenderStateParameters& GetRenderStateParameters() const;
    Uint16 GetRenderStateParametersVersion() const;
    const MGPVaoRec&             GetBoundVertexArray() const;
    // … 每个 backend 真正用到的 GLContext 方法一个访问器（Espryt 32 个 / Magma 55 个）
#if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED
    Uint64 m_filledGen[kFieldCount];   // ★v2：逐字段"上次填充的 verb 序号"，不是一位
    Uint64 m_currentVerbSerial;
#endif
};
extern PipeInputs gPipeInputs;
}
#if MOBILEGL_PIPE_PUSH
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

**`PipeInputs` 按 memo 键组织，不是按读点组织。** 这是它只有 ~20KB、且字段集在整个迁移期稳定的原因。

#### 6.2.1 三个阶段，其中阶段 A 可证明是**近乎** no-op

| 阶段 | 改什么 | 怎么证明 |
|---|---|---|
| **A — 别名** | 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加手工转换 58 行非箭头用法**（§2.4）。**逐 verb 类填充点**（见下）填 `gPipeInputs`。backend 函数体其余部分不变 | `nm --defined-only` 不变；`.text` size **在可逐行归因的范围内**（**不是**完全相等，见下） |
| **B — 推送** | tracker 填 `gPipeInputs`；填充器仍在，按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | **`MOBILEGL_PIPE_VERIFY=1`**（§10.3-②）：tracker 再填一份快照版，G4 生成的比对器**逐字段**每 draw 比一次 |
| **C — handle 化** | `SharedPtr<FrontendObject>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§10.3）。**注意 A/B 口径在此收窄，见 §6.7** |

**v2 修正 1：填充点必须逐 verb 类，不能只有两处。**
v1 只在 `PrepareForDraw`（`DirectGLES.cpp:2916`）与 `SetupDraw`（`VulkanRenderer.cpp:6371`）顶端填快照。但 `MG_Impl` 用到的 70 个表项里有 ~48 个不是 draw/dispatch，其中多个自己就读 `pGLContext`（`UpdateTextureBindingAtTarget` `:6051-6052`、`PackStateFromContext` `:6129`、`Clear` `:4106/:4165`、`BlitFramebuffer` `:5988-5989`、`GetTexImage` `:9254-9257`、DSA by-name `:4038-4043`、`:7417-7418`），而代码自己说明了这一点（`:1501-1502`："for every non-draw call site (Clear, readbacks)"）。
**做法**：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些 `PipeInputs` 字段"的表，并在 `MG_Impl` 的 ~93 个边界站点上生成对应的 validate/fill 调用。这同时把 poison 从"某个 draw 上炸"升级为"在**需要它的那个 verb** 上炸"。

**v2 修正 2：poison 从"位图"升级为"逐 verb 世代"。**
一个只被上一个 draw 填过的字段，在紧随其后的 `glTexSubImage`/`glReadPixels` 里读到的是**陈旧值**，位图版的 poison 看不见（位已置）。世代版：每次 verb 递增 `m_currentVerbSerial`，字段被填时记下当时的序号，读取时断言 `m_filledGen[f] == m_currentVerbSerial`（对"跨 verb 有效"的字段单独标注为 sticky 并在生成表里显式列出）。**这才让"一个字段在某个 verb 上没被推送"必然是一次 Fatal 而不是一次静默陈旧。**

#### 6.2.2 poison 世代是完整性的运行期绊线

在 debug 与 disaggregated 构建里，读一个当前 verb 未填的非 sticky 字段是 **`Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`**——响亮、精确、不可能渲染过去。P13 之后（`SnapshotFromGLContext()` 只在 verify 构建里）完整性变成**构建期事实**：一个从未被写入的字段就是一个编译器能标出来的字段。

### 6.3 Track V / Track H 与残余值块

- **Track V（值类型）**：`GetRenderStateParameters`、`GetPixelStoreParameters`、`IsCapabilityEnabled(+Indexed)`、`GetStencilState`、`GetColorMaskIndexed`、`GetDepthMask`、`GetScissorBox`、`GetPatchVertices`、`GetCurrentVertexAttribute`、Magma 的 ~22 个标量 getter…… **约占 B 类读点的 55%**。机械，每组 ~1 天。
- **Track H（对象类型）**：167 个 `SharedPtr<MG_State…>` 点。真活。

**Track V 的 55% 不需要逐字段接口条目就能跑起来**，所以 P2 发一个**显式临时**调用 `set_residual_value_state(MGPBlobRef)`：

```cpp
struct ResidualValueBlock {
    RenderStateParameters renderState;   // 直到 create/bind_render_state + set_dynamic_state 落地
    PixelStoreParameters  pack;          // 直到 set_pixel_pack_state 落地
    Uint64 capabilityBits;
    Uint32 patchVertices; Float patchOuter[4], patchInner[2];
    // … 每个阶段变小 …
};
```

**三条硬性纪律：**

1. **退役是一个编译错误。** `static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE)`，常量每阶段**下调**；P13 到 0 之后 `static_assert(sizeof(ResidualValueBlock) == 0, ...)` 一直红到最后一个字段消失。
2. **布局必须逐成员断言，不能只断言 sizeof。** 异质 POD 并集跨编译器/ABI 最容易出 padding 差异，而 monolith 的 verify harness **看不见它**（两侧是同一个 TU）。所以 G3 为每个成员生成 `static_assert(offsetof(...) == N)`，**并且**在 split 下该块**逐字段序列化**而不是整块 memcpy。
3. **只在 P2..P13 之间存在**，`MOBILEGL_PIPE_STATS` 单独计一类字节。

### 6.4 DirectGLES（Espryt）逐子系统

`PrepareForDraw` 的阶段顺序（`DirectGLES.cpp:2916-2975`）：`GetBoundVertexArray` → `ResolveVaoTwin` → `GetProgramForDraw`（**join 编译池**）→ `CaptureDrawTextureSyncKeys` → `SyncNeccessaryBuffers` → `SyncCurrentVAO` → `SyncNeccessaryTextures` → `SyncImageTextureBindingsForDraw` → `MarkWritableImageBufferTexturesGpuWritten`（**改前端**）→ `SyncCurrentFBO` → `SyncCurrentProgram` → `SyncRenderState` → `BindCurrentFBO` → VAO bind → `SyncCurrentVertexAttributeValues` → `BindCurrentTextures` → `BindCurrentProgramWithResources` → `StartPendingTransformFeedback`。

| # | 子系统 | 消除读点 | memo | 写回 | 轨 | 天 | 风险 |
|---|---|---|---|---|---|---|---|
| 0a | `GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 移回 `MG_Impl` | 14 | 0 | 0 | — | 1-2 | 极低（严格 no-op） |
| 0b | handle 基建；6 个 registry → slot 数组；删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / 2 个 GC 扫描 | — | 9 删 | — | — | 5-7 | 低 |
| 1 | **渲染状态**（`DirectGLES.cpp:1962-2654`，693 行） | **4**（`:2007, 2021, 2050, 2133`） | 0 | 0 | V | **3-5** | **低**：693 行函数体、单 `Uint16` 早退、三段 memcmp 全不动 |
| 2 | buffer + 7 个 `BufferBackendOps` | 19 | 3 | 6（+23 处 re-entry 删除） | H | 10-13 | **高**（不碰 `AcquirePersistentMap`） |
| 3 | VAO / vertex elements | 2（+~10 getter） | 4 | **0**（Espryt 不往前端对象写 memo） | H | 7-9 | 中 |
| 4 | framebuffer / renderbuffer | 8 + 4 处 `pDefaultFramebufferInfo` | 4 | 1 | H | 7-9 | 中高 |
| 5 | 纹理 / sampler / image unit / **`set_texture_params`** / **subdata 描述符改造** | 18（+~35 getter） | 8（5 删） | 21 | H | **23-30**（v1 为 20-26，+3-4 为 §4.5.6 的跨步描述符改造） | **高** |
| 6 | program + constant buffer | 16（+~30 getter） | 5 | 0 | H | 14-18 | **高** |
| 7 | XFB（含 **scatter 搬到 client**，§7.2） | 3 | 1 | 2 | H | 5-7 | 中 |
| 8 | emulation + `MGHostSpan` + **索引宿主镜像的 server 侧接口** | ~12 | 0 | 3 | — | 8-11 | 中 |
| 9 | 回读 / pack state | ~10 | 1 | 7 | V+H | 5-7 | 中 |
| 10 | 删 pull 路径 + `MGB_CTX` | — | — | — | — | 4-6 | 低 |
| | **合计** | **124** | ~32 | 28 | | **92-124** | |

**子系统 5 是全表最危险的一处**：它同时压着实测 +6ms/frame 的 box-vs-rects 悬崖（`Managers.cpp:4386-4390`）、7 条 fallback-repack 路径、以及 v2 新增的跨步描述符改造。缓解：`resource_subdata` 同时携带 box 与 region 列表且 **server 选形状**；repack 族本体不动；**子系统 5 拆成两个可独立落地的半**（先 sampler view + sampler + `set_texture_params`，再 image unit + dirty 归属反转 + 跨步描述符），让回归能二分到其中一半。**Mali 设备门必须发布逐帧上传作业数与帧时增量**（不是只有 SSIM）。

### 6.5 DirectVulkan（Magma）逐子系统

| # | 子系统 | 读点 | memo | 写回 | 天 | 风险 |
|---|---|---|---|---|---|---|
| 0a/0b | 同 Espryt；13 个身份缓存重键 | ~10 | 13 | 0 | 5-8 | 低 |
| 1 | **pipeline + 动态状态** | ~55 | 1 | 0 | **3-4** | **低——两个 backend 里最便宜的一次转换** |
| 2 | `SetupDraw` + `TrySetupDrawFastPath`（`:5994`，377 行）+ `SetupDrawSnapshot[4]` | ~48 | 4 | 0 | 10-13 | 高 |
| 3 | `VkBufferManager`（7 个 op 里的 6 个；`ResidentSubData` 保持 null） | ~19 | 2 | 4 | 7-9 | 高 |
| 4 | `VertexInputStateFactory` + `VaoDrawMemo`（**删掉写进前端 VAO 的后端堆裸指针**） | ~6 | 2 | 3 | 2-3 | **低（纯结构性收益）** |
| 5 | `VkTextureManager`（3504 行）+ `VkSamplerManager` + **`set_texture_params`** | ~30 | 3 | 7 | 13-16 | 高 |
| 6 | `UniformManager` 描述符 + **占位纹理原生化** + **具名 UBO host payload**（D-B8） | ~35 | 4 | 6，**且删 ~120 行** | 12-15 | 高 |
| 7 | `VkRenderPassManager` / `VkClearManager` / framebuffer（**保留 D18**） | ~20 | 2 | 0 | 7-9 | 中高 |
| 8 | `ProgramFactory` + **内部 shader 烘焙**（含 4 天烘焙与回归测试） | ~15 | 1 | 2 | 7-9 | 中（构建 lane） |
| 9 | XFB（**顺带修 D21**）+ query + 回读 | ~15 | 2 | 5 | 11-14 | 中 |
| 10 | swapchain / default FBO（`SwapchainObject.cpp:276-330` 的**写**变 `on_surface_changed`） | ~4 | 0 | 7 | 4-5 | 中 |
| 11 | 删 pull 路径 | — | — | — | 4-6 | 低 |
| | **合计** | **169** | ~34 | 42 | **85-111** | |

**Espryt 的子系统 1 与 Magma 的子系统 1 作为一个里程碑一起做**（合计 6-9 天），这样同一个接口调用在两个 backend 上同时被证明。

### 6.6 strangler 顺序（风险最小化）

```
0a  getter 移出（AdvertisedLimitsScenario；严格 no-op）
0b  字节/调用计数器落地  ← 含**动态** accessor 计数与 memo 命中率（§2.3.1）
0c  清工作树 per-draw fprintf
0d  值头与制品头抽取（MGPipeValueTypes.h、ProgramArtifacts.h）+ include 图门  ← P0.5
0e  handle 基建：slot 分配器 + registry 变数组 + 删 TwinLookupMemo/OwnerEquals/g_fbSlotCache/GC
1   渲染状态（两个 backend 一起）+ Magma 子系统 4   ← 机制证明 + 第一片 Track H
2   buffer + BufferBackendOps          ← 泛化已存在的模式；不碰 AcquirePersistentMap
3   VAO / vertex elements
4   framebuffer
5   纹理 / sampler / image unit（拆两半）
6   program + constant buffer
7   XFB + query + 回读                 ← 可与 5/6 并行（第二个工程师）
8   emulation + 索引宿主镜像
9   删 pull 路径；三道纯度门转绿
```

**0b 必须在任何迁移之前**：所有 ring 尺寸、批处理阈值、wire 粒度决策否则都是猜测。**0c 必须在基线之前**：那两处 per-draw `fprintf` 污染每一次测量。**0d 必须在 program 与渲染状态之前**：否则纯度门与 `nm -D | grep glslang` 判据不可达。

### 6.7 A/B：旧路径怎么保留，**以及它的口径在哪里收窄**

```
MOBILEGL_PIPE_PUSH        = <子系统位图>   # 0 = 全 pull；每位一个子系统；含一位关闭 CSO 内容寻址（负面对照）
MOBILEGL_PIPE_VERIFY      = 0|1            # 影子比对（~5-10x 慢，永不出货；P13 之后仍保留）
MOBILEGL_PIPE_STATS       = 0|1            # 字节/调用/roundtrip/纹理拉取/上传形状计数器
MOBILEGL_PIPE_LEGACY_MEMOS= 0|1            # ★v2：编译期开关，保留 registry / TwinLookupMemo 实现
```

在 init 时刻锁存，与 `MOBILEGL_BACKEND_TYPE` 同一套机制（`ConfigLoader.cpp:212-225`），与树里已有的 ~40 个 `MOBILEGL_*` 开关并列。

**v2 必须写明的口径收窄。** v1 说"任何一次提交都能在同一份二进制上按子系统 A/B，设备回归可以二分到'哪个子系统'"。**这在阶段 B（值字段）成立，在阶段 C（handle 化）之后不成立**：stage C 把 `PipeInputs` 的字段**类型**从 `SharedPtr<FrontendObject>` 换成 `MGPipeHandle` + POD 描述符、把 6 个 `StateBackendObjectRegistry` 哈希表换成 slot 数组、删掉 `TwinLookupMemo`×3 与 `OwnerEquals`、把 memo 重键成 `{slot, gen}`。位清零时，`SnapshotFromGLContext()` 仍要从 client 的 slot 表**合成**那个 handle，backend 仍然跑重键后的 memo 代码——**两个分支跑的是同一份新代码**。一个重键 bug（正是 D1/D2/D3/D11/D13 那一类）在两个分支里都在，位图二分不出来。

**对策**：`MOBILEGL_PIPE_LEGACY_MEMOS`（**编译期**开关）在 P3a 与 P4a 期间保留 registry / `TwinLookupMemo` 的实现活在同一个 `PipeInputs` 接口之下，给前两波 handle 化保留一个**真正的**旧-vs-新臂；随 pull 路径一起在 P13 退役。**这条开关的存在期与代价必须写在阶段计划里**（P3a/P4a 各 +1 天维护成本）。

**P13 删除 pull 路径时**：删 `SnapshotFromGLContext()` 的**非 verify** 编译分支、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**`MOBILEGL_PIPE_VERIFY` 连同它需要的 `SnapshotFromGLContext()` 与 `MG_State` include 一起保留**（D-B5）；`static_assert(sizeof(ResidualValueBlock) == 0)` 必须编译通过；三道纯度门（§4.7.2）在**非 verify** 构建上转绿。
