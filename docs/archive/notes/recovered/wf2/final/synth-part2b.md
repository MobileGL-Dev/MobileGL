## 5. 前端 state tracker

### 5.1 推送发生在哪里——本设计里最容易做错的一个决定

**不在 GL setter 里。** `glEnable(GL_BLEND)` 绝不调 `bind_render_state`。Blaze3D 每个 batch 都用 `glEnable/glDisable(GL_BLEND)` 包住，代码自己标注它是最热的路径（`DirectGLES.cpp:2044-2046`）。天真的 per-setter 推送把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表——**严格慢于今天，而 trace 语料会在一天之内说出来**。

**在 validate 时刻。** 就是 gallium 的 `st_validate_state` 形状：

```cpp
// MG_Impl/Pipe/Tracker.h
class MGPipeTracker {
public:
    void ValidateForDraw(const MGPValidateHint&);   // 20 个 GL draw 入口调用
    void ValidateForDispatch();                     // glDispatchCompute*
    void ValidateForClear(GLbitfield);              // 只 framebuffer + blend/colormask
    void ValidateForBlitOrCopy();                   // 只 framebuffer + pack state
private:
    Uint64 m_dirty;                                 // MGPIPE_NEW_* 位
    Uint64 m_lastPushed[kGroupCount];               // 每组上次推送的（加宽后的）版本
};
```

`MG_Impl` 里 89 个表调用站点中大约 40 个是 verb，validate 就插在这 40 个之前。**只有资源 mutation 在 GL 调用时刻推送**——这恰恰是 `BufferBackendOps` 今天的做法（`BufferObject.h:70-71`："在 GL 调用时刻分发，就在 shadow 拷贝刚更新之后"）。

**稳态成本 = 一次 64 位 dirty word 测试 + N 次 `set_*`/`bind_*` 间接调用，N = 变脏的组数（稳态 1-3；因为 CSO 内容寻址，重复状态常常 N=0）。** 这严格少于它替换掉的东西：124 / 169 次 accessor 调用 + 版本比较 + 一次 ~1.2KB 的三段 memcmp + `CurrentUnitBindingsEpoch` 的逐 unit owner 走查 + Magma 的两次有损版本求和。

### 5.2 dirty bits：全部来自已有计数器，`MG_State` 零新增记账

MobileGL 已经有 dirty 底盘，只是拼写成"由 backend 轮询的版本计数器"。tracker 只是把极性反过来。

| dirty 位 | 现有计数器 | 位置 |
|---|---|---|
| `NEW_RENDER_STATE` / `NEW_PIPELINE_STATE` | `m_version` / `m_pipelineStateVersion`（`Uint16`） | `RenderState.h:522, 529`；bump 点 `RenderState.cpp:311` |
| `NEW_VERTEX_ELEMENTS` | `VertexArrayObject::GetConfigVersion()`（`Uint32`） | `VertexArrayObject.h:155` |
| `NEW_VERTEX_BUFFERS` | 逐属性 `VertexAttributeVersion{Format, Buffer, Switch}` | `:66-70, 150-151` |
| `NEW_INDEX_BUFFER` | 索引 slot `GetVersion()`（**回绕 `Uint16`**）+ 绑定对象身份 | `MG_Util/Types.h:197` |
| `NEW_FRAMEBUFFER` | `GetObjectVersion()`（`Uint16`）+ `GetAllFramebufferAttachmentVersions()`（`Array<Uint16,40>`）+ slot 版本 | `FramebufferObject.h:145, 149, 171, 183` |
| `NEW_SAMPLER_VIEWS` | `GetContentVersion`（`Uint64`）+ `GetShapeVersion` + `GetTextureParamsVersion`（`Uint16`）+ `GetTextureBindGeneration()` + `GetSamplingResolutionGeneration()` | `TextureObject.h:61-71, 203-214`；`Core.h:130, 136` |
| `NEW_SAMPLERS` | `SamplerObject::GetVersion()`（`Uint16`） | `SamplerObject.h:532, 551` |
| `NEW_SHADER_IMAGES` | `ImageTextureBinding::Version` | `TextureState.h:24, 34` |
| `NEW_SHADER` | `GetLinkVersion()` + `GetImageUnitVersion()` | `ProgramObject.h:844, 906` |
| `NEW_SHADER_BINDINGS` | `GetBackendStateVersion()`、`GetBlockBindingVersion()`、`GetUniformWriteSetVersion()` | `:841, 1064, 652` |
| `NEW_GLOBAL_CONSTANTS` | `GetUBOContentVersion()`（`~0u` 跳过回绕） | `:791-794` |
| `NEW_CONST_BUFFERS` / `NEW_SHADER_BUFFERS` / `NEW_SO_TARGETS` | slot 版本 + `BufferObject::GetChangeSerial()`（`Uint64`） | `BufferObject.h:211` |
| `NEW_PATCH_STATE` | patch 三字段，用 `BitwiseEqual` 比较（NaN 合法，`DirectGLES.cpp:2807-2814`） | `RenderState.h:241, 247, 248` |
| `NEW_PIXEL_PACK` | `PixelStoreParameters` | `RenderState.h:190-199` |

**三个回绕的 `Uint16` 在 tracker 边界加宽。** `BindingSlot::m_version`、`FramebufferObject::m_objectVersion`、`SamplerObject::m_version` 今天只有配合裸指针身份比较才正确（`packed_pixels` postmortem，`DirectGLES.cpp:2815-2827`）。`m_lastPushed[]` 是 tracker 自己的字段而不是 `MG_State` 的字段，所以加宽到 `Uint32`/`Uint64` **不需要改 `MG_State` 一行**；同时 handle 与它同行过线。**回绕在 tracker 本地是无害的**（一次回绕造成一次多余的重推，永远不会造成一次漏推），因为 tracker 还额外保存推送内容的 hash 用于 CSO 查表。

### 5.3 每命令 validate 与固定顺序

```
1  set_framebuffer_state            ← 必须先于 2 和 4
2  create_shader_state（link 时刻发，不是 draw 时刻） / set_draw_program
3  set_sampler_views / bind_sampler_states / set_shader_images / set_shader_buffers / set_global_constants
4  create_render_state（未命中时）/ bind_render_state
5  bind_vertex_elements_state / set_vertex_buffers / set_index_buffer / set_vertex_attrib_defaults
6  set_patch_state / set_stream_output_targets
7  draw_vbo
```

**framebuffer 严格第一是承重的**（D-B3）：四个跨对象 mask 从 attachment 格式推出（`Managers.cpp:5616-5619`），被渲染状态推送（`DirectGLES.cpp:2014`）与 program 陈旧性判定（`:2769-2770`）消费。今天 Espryt 靠在 `SyncCurrentProgram` 内部**自己重新推导**广播数来绕开（`:2712-2732`，注释：否则用陈旧计数编译出的 program 要迟一个 draw 才重链）。固定推送顺序把这个 hazard 变成免费属性，那段 workaround 与 `g_broadcastMemo*` **一起删掉**。同理 `ImageUnitFormatsStillMatch`（`Managers.cpp:6545-6573`，注释明说"不可表达为单调版本"）由 `set_shader_images` 直接告知。

`create_shader_state` **从编译池的终止 continuation 发出**（`JobNode.h:109-123`），不是从 draw 发出——这样 SPIR-V 在用到它的第一个 draw 之前就到达 server。这是 monolith 拿不到的异步收益，也是把阻塞的 `JoinLinkAndSpirv()` 移出 server draw path 的那一步。

### 5.4 合并：保留代码库已经发现的三条

1. **整块结构优于逐字段。** Magma 的 `ComputePipelineStateHash`（`VulkanRenderer.cpp:4818-4826`）已经把 ~17 次 accessor 调用换成一次 bulk fetch；Espryt 的三段 memcmp 同理。tracker 发**变化的段**（`spanMask`），段划分与现有 memcmp 完全一致。
2. **高水位标记。** `BufferState::TouchBindPoint` / `GetTouchedBindPointCount`（`BufferState.h:49-62`，每 target 84 个绑定点，理由 `:23-28`）与 `TextureState::NoteUnitTouched` / `GetMaxTouchedUnit`（`Core.h:123-126`，192 个单元）**必须留在 tracker 的走查里**——没有它们，每次 validate 都要走 84×4 个绑定点和 192 个纹理单元。它们直接就是 `set_shader_buffers` / `set_sampler_views` 的 `count` 实参。
3. **只发 program 解析过的集合。** `set_sampler_views` 只推 shader 真正读的那些单元，用 `LinkArtifacts::uniformSamplerOrImageUnitIndex`（`ProgramObject.h:1298`）——这是 client 侧数据，所以是纯 client 收益。两个 backend 今天已经在算这个（`ResolveAndBindUnitTextures`，`DirectGLES.cpp:2973`；`UniformManager::CollectSampledTextures`）。

**索引绑定的范围必须在 validate 时刻实时解析，不是在 bind 时刻快照。** `BindingSlotRange1D::GetRange()` 对整 buffer 绑定返回 `Range1D(0, object->GetSize())` 而不是绑定时的快照，因为 `glBindBufferBase` 之后再 `glBufferData` 是普通应用代码，只有 `glBindBufferRange` 才钉死一个窗口。

### 5.5 sampler view 在 client 侧解析

GL 是**每个 unit 每个 target 各一个绑定**（`TextureUnit.h:393-394`；`TextureState::m_textureUnits` 是 `Array<TextureUnit, 192>` **按值**存放，`TextureState.h:128`，而每 stage 广告上限是 32，`:46`），shader 看见哪一个取决于 sampler uniform 的声明类型、mipmap 完备性（`IsMipmapCompleteForFilter`，`TextureObject.h:309`；`SamplesAsIncompleteTexture`，`:315`）和 `IsUndefinedDefaultTexture`（`:329-332`）。**gallium 的"每槽一个 view"就是解析后的形态。**

**解析留在 client。** 把这本 GL 完备性规则书推给 server，就是在 server 里重建前端——正是用户否掉的那个冗余。

**两处 backend 特定的后处理留在 server**，并作用在已解析的集合上：Espryt 的 raw-depth-fetch sampler 替换（`DirectGLES.cpp:3540-3546`）与 Magma 的 feedback-loop 检测（对着 draw FBO，`UniformManager.cpp:554`）。两者都可以从已推送的 `set_framebuffer_state` + view 集合判定，所以是普通的 server 侧 lowering。

### 5.6 对象生命周期、共享组与 composite pipeline program

#### 5.6.1 生命周期

`resource_create` 在**前端对象构造**时发（lifetimeId 铸造的地方），存储由 `resource_respecify` 惰性定义。`resource_destroy` 在前端对象析构时发。三条今天靠 `SharedPtr` 边表达、接口必须显式声明的顺序约束：

- **view 先于其存储属主销毁**：`ITextureObject::GetViewStorageOwner()`（`TextureObject.h:96-100`）是 `SharedPtr`，正是为了免费拿到 GL 的名字删除语义（`glDeleteTextures(orig)` 之后对象因 view 引用而存活）。→ `MGPResourceDesc::viewOf` + server 侧 keep-alive。
- **FBO attachment 钉住纹理**：`FramebufferAttachmentObject::m_texture`（`FramebufferObject.h:95`）。→ `set_framebuffer_state` 的 surface handle 隐含 server 侧 keep-alive。
- **buffer texture 钉住 buffer，且范围实时解析**：`TextureObjectBuffer::m_bufferBindingSlot`（`TextureObjectBuffer.h:46`），`kWholeBuffer = ~0`（`:28`）使后续 respecify 继续被跟随，`GetBufferRangeSizeInBytes`（`:35-41`）对实时 size 钳制。→ `MGPResourceDesc::{bufferForTexBuffer, bufOffset, bufSize}`。

#### 5.6.2 共享组

v1：一个 screen、一个 context、一个扁平 handle 空间、一条 flow，没有 session id（理由见 §1.2 与 §4.3）。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射——**顺手修今天不取该锁的两个入口**：`ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）。v2 的映射是干净的（每个 share group 一个 client `GLContext`，每个 GL context 一个 `MGPipeContext`），但走独立 PR 带多 context 测试落 `dev`，不在本分支。

#### 5.6.3 composite pipeline program：判过死刑的那个反对意见，答案是"什么都不用做"

`GLContext::GetProgramForDraw()`（`Core.cpp:592`）**今天就已经完全在前端**完成 program pipeline 的合成：join 每个 stage 的 `JoinLinkAndSpirv()`（`:617` 附近的 J1 join 点）、按 `ComputeDrawProgramSignature()`（`:630`，键是每个图形 stage 的 `(lifetimeId, linkVersion)`）查 cache、miss 时构造**故意不命名**的 `MakeShared<ProgramObject>(0u)`（`:644`）、挂上每个 stage 被钉住的 linked snapshot、重装捕获 stage 的 XFB varyings、`Link(true)`、缓存、`RefreshCompositeUniforms`。

tracker 调它，拿到一个 `SharedPtr<ProgramObject>`，推**一个 handle**。合成体没有 GL name，但它**有 lifetimeId**，而它的 slot 从 `ShaderCso` 的保留高位段分配（§4.2.1）。生命周期：pipeline cache 淘汰该条目时（signature 变化或 pipeline 删除）释放 slot、`gen++`、发 `delete_shader_state`——`CompositeResolver.cpp` 里三行。

**合成体从不过线、从不被重新实现，`PLAN.md` §5.7 提议往 `MG_State/GLState/Core.h` 加的 `SetReplicaResolvedDrawProgram` 钩子完全不需要**——pipe 调用**就是**那次解析。副带收益：阻塞的 `JoinLinkAndSpirv()` 彻底离开 server 的 draw path。

### 5.7 program artifacts 与全局 UBO scratch

**`create_shader_state` 的 payload 是 SPIR-V + 全结构体反射归档**（§4.5.5），不是源码。理由是硬约束而非偏好：`ProgramObject.h:11 → ShaderObject.h:12 → ShaderCompileTask.h`，`ShaderObject.h:146` 返回 `SharedPtr<glslang::TShader>`，`ProgramObject.h:14 → SpvcSession.h`——**链接真 `ProgramObject` 就链接 glslang**。所以 `RecProgramLinkOp` 不可能存在，`ProgramPublish` 第一天就上，`nm -D libMobileGLServer.so | grep glslang` 为空是**接口纯度门的一部分**。

**SPIRV-Cross 留在 server**（`TranspileSpirvToEssl`，`Managers.cpp:6575`）：它消费 SPIR-V 加设备事实，两者都在 server 侧。**glslang 留在 client。** 这是一次文件级切割，不是一次审计。

**全局 UBO scratch 走独立入口**（D6）：`set_global_constants(shaderCso, MGPBlobRef bytes, Uint32 version)`，键 `(shaderCso.slot, uboContentVersion)`，复现 `DirectGLES.cpp:3369-3392` 的"每 program 每帧至多一次"。`~0u` 跳过回绕的规则保留（`ProgramObject.h:792`，那是 backend 的"从未上传"哨兵）。

**backend 侧 program link/compile 失败不需要任何同步返回，也不需要新事件种类。** 实测：`SyncToBackend` 在 `Managers.cpp:8091` link、`:8094` 读 `GL_LINK_STATUS`、`:8095` 折进 `m_backendProgramUsable`、`:8097-8101` 取驱动日志、`:8106` 发 `MGLOG_E`；`Use()` 随后绑 program 0（`:8357`）并 `MGLOG_E_ONCE`（`:8364-8372`）。**没有 GL error、没有 `ProgramObject` 变更、`GL_LINK_STATUS` 永不撤回**——代码在四处说明理由（`:7098`、`:7247-7249`、`:6478`、`:7827`："GL 无法撤回它已经报告为 true 的 LINK_STATUS，所以'链接了但画不出来'就是答案"）。同步查询 `glGetProgramiv(GL_LINK_STATUS)` 与 `glGetProgramInfoLog` 完全由 client 从 `ProgramObject` 回答（`GL_Program.cpp:851` → `ProgramObject.h:913`）。所以 `on_log` 逐字复现它——**但由此推出一条对 `PLAN.md` §7.4 的强制修正，见 §7.4**。

### 5.8 emulation 所需前端数据的显式传递

归属规则用 gallium 自己的：**驱动表达不了的变换在 state tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering**，且每一条由一个具名 cap 位门控（这是 Design 1 的贡献，它让归属可审计、可推广到第三个 backend）。

| emulation | 归属 | cap 门 | 过线的是什么 |
|---|---|---|---|
| **client 顶点数组**（`Managers.cpp:2500-2592` 把 `attrib.Offset` 当应用裸指针，每 draw 每属性上传 `(first+count-1)*stride+elementSize`；`VulkanRenderer.cpp:3737` 是**唯一无界**的应用指针读） | **client**（它拥有地址空间） | — | **字节，永不是指针**。范围 client 侧算。gallium 根本没有 client array 概念，Mesa 的 `_mesa_upload_vertices` 就是这么做的 |
| **索引扫描**（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3407-3470`，用于给上一条定界） | **client**（只有它同时持有两个数组） | — | `MGPDrawInfo::minIndex/maxIndex`（gallium 本来就有这两个字段），`~0` = 未知 |
| **client 索引数组** | client | — | `MGPDrawInfo::userIndices`（`MGHostSpan`） |
| **primitive-restart 重写**（`DirectGLES.cpp:4368-4470` 整 EBO 重写，`kMaxRestartRewriteBytes` 上限；`VulkanRenderer.cpp:4159-4161`） | **client**，`!kCapPrimitiveRestart` 时 | `kCapPrimitiveRestart` / `kCapPrimitiveRestartFixedIndex` | 重写后的索引 + `kRestartRewritten` 标志。**"draw 被丢弃"的诊断随之落到应用线程上，比落在 server 上更有用** |
| **multi-draw 展平**（`MultiDraw.cpp:498-540`） | **CPU 档在 client**（`!kCapMultiDraw`）；**compute 档留 server**（`kMaxComputeFlattenedIndices`，`:81-83`，那是驱动 lowering） | `kCapMultiDraw*` | 只在 cap 说需要时才发索引字节 |
| **`*IndirectCount` CPU 回退**（`DirectGLES.cpp:4684-4793` 从 `parameterBuffer->MappedData()` 读实际 draw 数） | **client**，`!kCapMultiDrawIndirectCount` 时 | `kCapMultiDrawIndirectCount` | client 从自己的 shadow 解析出计数，发解析后的 `MGPDrawRange[]` |
| **viewport-array N 遍回放**（`DirectGLES.cpp:3742-3846`，今天包住 14 个 draw 入口） | **server**（它取决于转译出来的 ESSL 有没有被注入路由 gate） | `kCapViewportArray` | 无新增：16 组 viewport/scissor/depth-range 已在 render-state blob 里，decline 输入（XFB active、rasterizer discard）也在 |
| **fp64 顶点窄化**（`Managers.cpp:2518-2557`） | **server**（这是后端格式决策） | `kCapFloat64VertexAttrib`（`BackendObject.h:487-500` 明说它与 `SupportsShaderFloat64` **独立**） | 原始字节；`IsLong` 与 `Type` 分开过线 |
| **image-bindable 存储加宽/拆分**（`Managers.cpp:2789-2822`、`:4620-4630`） | **server**（纯驱动 workaround） | — | 正向：`imageBindableHint` 预防；反向：`on_texture_pull_request`（§7.5） |
| **生成 mipmap 的前端存储** | **拆开**：client 分配 level 存储（它必须回答 `glGetTexImage`），server 生成 | — | `MGPMipPlan`；CPU 回退路径的纹素由 `on_texture_writeback` 回来 |
| **CopyImage shadow 镜像**（`DirectGLES.cpp:7065-7140`） | **client**（给定区域，它可以用自己的两张纹理做同样的拷贝） | — | 只回"拷贝成功"。**删掉一整条 server→client 字节通道** |
| **XFB CPU 图元计数**（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`） | **纯 client** | `kCapCpuXfbPrimitiveAccounting` | `MGPDrawInfo::xfbCpuCapturedVertices` + `end_stream_output` 的 `MGPXfbAccounting`。**`PLAN.md` 的 `RecXfbAccounting`、`MG_Remote::Shared::` helper 族、整个 `gen_impl_mutation_surface.py` 生成器随之删除** |
| **压缩纹理 / pixel unpack 规整** | **纯 client**（`GL_Texture.cpp:298-306`；`PixelStoreProcessor`） | — | 无 |

#### 5.8.1 陈旧索引纪律——这条不做就是几何消失

上表里每一次 client 侧扫描/重写，在 monolith 里都紧跟在 `SyncPersistentMappedRange()` + `SyncGpuWrites()` 之后（`DirectGLES.cpp:4412-4413`、`MultiDraw.cpp:498-499`、`VulkanRenderer.cpp:3431, 4159`），因为 EBO 可能刚被 compute shader 或 XFB 写过。**tracker 必须在完全相同的程序点做同一次 reconcile**：publish → 等 `appliedSeq` → 排空事件 → 再碰 shadow。跳过它，`maxIndex` 就来自陈旧字节，顶点数组被少拷 → 几何缺失/花屏，或越界读应用数组。

门：`ClientArrayAfterComputeWriteScenario`（新增），**必须能因它存在的理由变红**（去掉那次等待就应当看到几何缺失）。

---

## 6. 后端状态机改造

### 6.1 什么原样不动（先说这个，因为它是"最短可信改造"的依据）

**每一个 ring、pool、arena、quirk、lowering pass 原地不动：**

Espryt：三条 persistent-mapped ring（UBO `Managers.h:591-637`、unpack `:639-671`、upload `:673-…`）、`PersistentRing` 的分配/背压算法、buffer pool（64MiB 预算 / 单 buffer 8MiB）、全部 7 条 fallback-repack 路径（`Managers.cpp:3209-3527`）、`m_backendColorSlots` draw-buffer 置换表（`Managers.h:1183-1195`）、三个 scratch FBO 及其驱动侧 attachment 影子、`PackState`、全部驱动绑定影子、Adreno 的"禁用属性无指针 SIGSEGV" workaround（`Managers.cpp:2371-2380, 2427-2433`）、Mali 的 XFB 捕获丢失 workaround（`DirectGLES.cpp:400-410`）、`ScopedDefaultUnpackState`、SPIRV-Cross 会话与 6 次 post-emission ESSL 重写、驱动 POST 自检族。

Magma：`VulkanRenderer` 全部 memo 与 scratch、`PipelineFactory`、`ProgramFactory`、`UniformManager` 的 ring 与描述符集、五个 `Vk*Manager`、`FrameContext`、`SwapchainObject`、`DynamicStateShadow`、`VertexInputStateFactory` 的 cache **本体**、**以及 D18 的节点式容器纪律**。

**接口改变的是 backend 怎么知道这些事实，不是它拿这些事实做什么。** 这就是为什么这是最短的可信改造路径：对多数子系统，函数体不变，只有 accessor 表达式换个地方取值。

**唯一两处必须真改的 `MG_State` 类型内部用法**（`PLAN.md` 把它们计为零成本，两轮评审都点了名）：

1. **Magma 的占位纹理**（`UniformManager.cpp:161-181, 1416-1500, 1624-1634`）：构造真的 `TextureObject2D` / `TextureObject2DMultisample` / `TextureObject2DMultisampleArray`，走 `SetInternalFormat(RGBA8)` / `AllocateStorage({1,1,1},4)` / `UpdateMipmapSubData` / `MarkStorageDirty` / `SetSamples(2)`（VUID-RuntimeSpirv-samples-08726 禁止 MS=1 读 1-sample image）/ `TruncateMipmapLevels(1)`，**唯一理由**是让"未绑定单元"复用 `SyncTextureAndGetDescriptor(ITextureObject&)` 这个签名。改成 backend 自己分配 `VkImage` + view + descriptor：**~120 行前端对象木偶戏变成 ~60 行直白的 VMA/Vulkan，43 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个随之消失。** 这是推送模型**减少**工作量最清楚的一处；上一轮"薄 server 不可行"的判决，是针对一个**拉取**接口下的判决。
2. **Magma 的两个内部 shader**（`InitializeBlitResources` `VulkanRenderer.cpp:4210-4283`、`InitializeDepthMipmapResources` `:4287-4356`）：从字符串字面量构造 `ShaderObject`、`Compile()`、建 `ProgramObject`、`Link(false)`、读 `GetUniformLocation` ×7、断言 `GetUBOSize() > 0`、后面 `MapUBO()`（`:8450-8452`）。**烘焙成 SPIR-V。**
   **方式：把生成的 SPIR-V、uniform location、UBO 布局作为生成头文件签进树里，用一个 `MG_Test` 用例重跑树内 glslang 对同一批源码字符串并逐字节比对来守新鲜度。** 不用构建期 host glslang target——那需要一个 host 工具目标，在 Android NDK 交叉构建、WSL、Windows、macOS 四条 lane 上以四种不同方式坏掉。签进树 + 测试守卫是本仓库对 golden 已经在用的模式。`uSource` 的描述符绑定本来就是由 `ProgramFactory` 自己的 SPIRV-Reflect 走查找到的（`:4340-4350`），原样存活。**顺带把一次 glslang 编译从 monolith 启动路径上删掉。**

Espryt 有一个小号同类：`g_rawDepthFetchSamplerState`，一个独立的 `SamplerObject(0)` 被 `SyncToBackend` 驱动（`DirectGLES.cpp:166-179`）→ 改写成 backend 原生的 sampler 记录，~40 行。

### 6.2 strangler 脚手架：`PipeInputs` + 两个填充器 + poison mask

```cpp
// MG_Backend/MGPipe/PipeInputs.h   （backend 私有；迁移结束后不 include 任何 MG_State 头）
namespace MobileGL::MG_Pipe {
struct PipeInputs {
    // 阶段 A：字段类型与 backend 今天读到的**完全一致**
    const RenderStateParameters& GetRenderStateParameters() const;
    Uint16 GetRenderStateParametersVersion() const;
    const MGPVaoRec&             GetBoundVertexArray() const;
    // … 每个 backend 真正用到的 GLContext 方法一个访问器（Espryt 32 个 / Magma 55 个）
#if MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED
    Uint64 m_filledMask[2];      // G5 生成；读未填字段 = Fatal{UnmigratedPipeInput, "<field>"}
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

**`PipeInputs` 按 memo 键组织，不是按读点组织。** 这是它只有 ~20KB、且字段集在整个迁移期稳定的原因（Design 2 的贡献）。每 draw 实际推送的增量由 §5.4 的两个高水位标记定界。

#### 6.2.1 三个阶段，其中阶段 A 可证明是 no-op

| 阶段 | 改什么 | 怎么证明 |
|---|---|---|
| **A — 别名** | 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加手工转换 58 行非箭头用法**（§2.4，其中 `DirectGLES.cpp:146` 的 `.get()` 裸指针捕获 `sed` 完全抓不到）。`SnapshotFromGLContext()` 在 `PrepareForDraw`（`DirectGLES.cpp:2916`）/ `SetupDraw`（`VulkanRenderer.cpp:6371`）顶端填 `gPipeInputs`。**backend 函数体其余部分不变** | `nm --defined-only` + 剥调试信息后的 `.text` size diff 在 pull 构建里一致 |
| **B — 推送** | tracker 填 `gPipeInputs`；`SnapshotFromGLContext()` 仍在，按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | **`MOBILEGL_PIPE_VERIFY=1`**（§10.3-2）：tracker 再填一份快照版，G4 生成的比对器**逐字段**每 draw 比一次，打印第一个分歧字段名与 draw 序号 |
| **C — handle 化** | `SharedPtr<FrontendObject>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§10.3） |

**迁移粒度是一个 accessor，不是一个子系统。** §6.4/§6.5 的阶段划分只是方便的批次，不是硬边界。

#### 6.2.2 poison mask 是完整性的运行期绊线

`m_filledMask` 由 G5 从 `PipeCalls.def` 生成。在 debug 与 disaggregated 构建里，读一个 tracker 从未推送的字段是 **`Fatal{UnmigratedPipeInput, "GetStencilState"}`，发生在第一个 draw 上**——响亮、精确、不可能渲染过去。迁移结束（P13）删掉 `SnapshotFromGLContext()` 之后，完整性变成**构建期事实**：一个从未被写入的字段就是一个编译器能标出来的字段。

**这就是历次评审所说"reconciler 完整性只有测试绊线、没有构建绊线"的答案**，而它只有在"接口先落在 monolith 里"时才存在。

### 6.3 Track V / Track H 与残余值块

32 个 Espryt accessor 与 55 个 Magma accessor 分两条轨：

- **Track V（值类型）**：`GetRenderStateParameters`、`GetPixelStoreParameters`、`IsCapabilityEnabled(+Indexed)`、`GetStencilState`、`GetColorMaskIndexed`、`GetDepthMask`、`GetScissorBox`、`GetPatchVertices`、`GetCurrentVertexAttribute`、Magma 的 ~22 个标量 getter…… **约占 B 类读点的 55%**。它们**完全不需要重塑形状**：client memcpy 值，server 把自己那份的引用交给 backend。机械，每组 ~1 天。
- **Track H（对象类型）**：167 个 `SharedPtr<MG_State…>` 点。真活：参数类型变 `MGPipeHandle` + `const MGPXRec*`，twin registry 变 slot 数组，memo 重键。

**Track V 的 55% 不需要逐字段接口条目就能跑起来**，所以 P2 发一个**显式临时**调用：

```cpp
void (*set_residual_value_state)(MGPBlobRef);   // 尚未迁移的值类 accessor 的并集，~1.5KiB
struct ResidualValueBlock {
    RenderStateParameters renderState;   // 直到 create/bind_render_state 落地
    PixelStoreParameters  pack;          // 直到 set_pixel_pack_state 落地
    Uint64 capabilityBits;               // 直到能力位折进 render-state CSO
    Uint32 patchVertices; Float patchOuter[4], patchInner[2];
    // … 每个阶段变小 …
};
```

**三条硬性纪律，缺一不可：**

1. **它是显式临时的，退役是一个编译错误。** `static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE)`，常量每阶段**下调**；P13 到 0 之后 `static_assert(sizeof(ResidualValueBlock) == 0, "残余块必须清空才能删除 pull 路径")` 一直红到最后一个字段消失。**本设计里没有任何"临时重复"是没有具名退役绊线的。**
2. **布局必须逐成员断言，不能只断言 sizeof。** 这是一个异质 POD 的并集，跨编译器/ABI 边界最容易出的正是 padding 差异，而 monolith 的 verify harness **看不见它**（monolith 里两侧是同一个 TU）。所以：G3 为块内**每个成员**生成 `static_assert(offsetof(ResidualValueBlock, X) == N)`，**并且**在 split 模式下该块**逐字段序列化**（走 G3 生成的编解码器）而不是整块 memcpy。
3. **它只在 P2..P13 之间存在**，且 `MOBILEGL_PIPE_STATS` 必须把它的字节量单独计一类，让"它还有多大"是一个可见指标。

### 6.4 DirectGLES（Espryt）逐子系统

`PrepareForDraw` 的阶段顺序（`DirectGLES.cpp:2916-2975`，工作树行号）：`GetBoundVertexArray` → `ResolveVaoTwin` → `GetProgramForDraw`（**join 编译池**）→ `CaptureDrawTextureSyncKeys` → `SyncNeccessaryBuffers` → `SyncCurrentVAO` → `SyncNeccessaryTextures` → `SyncImageTextureBindingsForDraw` → `MarkWritableImageBufferTexturesGpuWritten`（**改前端**）→ `SyncCurrentFBO` → `SyncCurrentProgram` → `SyncRenderState` → `BindCurrentFBO` → VAO bind → `SyncCurrentVertexAttributeValues` → `BindCurrentTextures` → `BindCurrentProgramWithResources` → `StartPendingTransformFeedback`。

| # | 子系统 | 消除读点 | memo | 写回 | 轨 | 天 | 风险 |
|---|---|---|---|---|---|---|---|
| 0a | `GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 移回 `MG_Impl` | 14 | 0 | 0 | — | 1-2 | 极低（严格 no-op） |
| 0b | handle 基建；6 个 registry → slot 数组；删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / 2 个 GC 扫描 | — | 9 删 | — | — | 5-7 | 低 |
| 1 | **渲染状态**（`DirectGLES.cpp:1962-2654`，693 行） | **4**（`:2007, 2021, 2050, 2133`） | 0（本来就是值键） | 0 | V | **3-5** | **低**：693 行函数体、三段 memcmp、`g_syncedColorMaskAlphaWidenMask`、dual-source decline 全部不动，只有取值表达式换地方 |
| 2 | buffer + 7 个 `BufferBackendOps` | 19 | 3 | 6（+23 处 re-entry 删除） | H | 10-13 | **高**（但不碰 `AcquirePersistentMap`） |
| 3 | VAO / vertex elements | 2（+~10 getter） | 4 | **0**（Espryt 不往任何前端对象写 memo，已核实） | H | 7-9 | 中 |
| 4 | framebuffer / renderbuffer | 8 + 4 处 `pDefaultFramebufferInfo` | 4 | 1 | H | 7-9 | 中高 |
| 5 | 纹理 / sampler / image unit | 18（+~35 getter） | 8（5 删） | 21 | H | 20-26 | **高** |
| 6 | program + constant buffer | 16（+~30 getter） | 5 | 0 | H | 14-18 | **高** |
| 7 | XFB | 3 | 1 | 2 | H | 5-7 | 中 |
| 8 | emulation + `MGHostSpan` | ~12 | 0 | 3 | — | 8-11 | 中 |
| 9 | 回读 / pack state | ~10 | 1 | 7 | V+H | 5-7 | 中 |
| 10 | 删 pull 路径 + `MGB_CTX` | — | — | — | — | 4-6 | 低 |
| | **合计** | **124** | ~32 | 28 | | **89-120** | |

**子系统 5 是全表最危险的一处**，因为它同时压着实测 +6ms/frame 的 box-vs-rects 悬崖（`Managers.cpp:4311-4319`）与 7 条 fallback-repack 路径（其可行性判定要求 `uploadData == mipData`，即驱动用 `UNPACK_ROW_LENGTH` 跨步直接读前端字节）。缓解：`resource_subdata` **同时**携带 box 与 rect 列表、**server 选形状**（§4.5.6）；repack 族原地不动，只把输入从"拉取的 shadow 指针"换成 `MGPBlobRef`（monolith 里是同一个指针）；子系统 5 拆成两个可独立落地的半（先 sampler view + sampler，再 image unit + dirty 归属反转），让回归能二分到其中一半。

### 6.5 DirectVulkan（Magma）逐子系统

| # | 子系统 | 读点 | memo | 写回 | 天 | 风险 |
|---|---|---|---|---|---|---|
| 0a/0b | 同 Espryt；13 个身份缓存重键 | ~10 | 13 | 0 | 5-8 | 低 |
| 1 | **pipeline + 动态状态**（`ComputePipelineStateHash` `:4818`、`ApplyDynamicDrawStateTail` `:5871`、`GetOrCreatePipeline` `:4948`） | ~55 | 1 | 0 | **3-4** | **低——两个 backend 里最便宜的一次转换** |
| 2 | `SetupDraw` + `TrySetupDrawFastPath`（`:5994`，377 行）+ `SetupDrawSnapshot[4]` | ~48 | 4 | 0 | 10-13 | 高 |
| 3 | `VkBufferManager`（7 个 op 里的 6 个；`ResidentSubData` 保持 null） | ~19 | 2 | 4 | 7-9 | 高 |
| 4 | `VertexInputStateFactory` + `VaoDrawMemo`（**删掉写进前端 VAO 的后端堆裸指针**） | ~6 | 2 | 3 | 2-3 | **低（纯结构性收益）** |
| 5 | `VkTextureManager`（3504 行）+ `VkSamplerManager` | ~30 | 3 | 7 | 13-16 | 高 |
| 6 | `UniformManager` 描述符 + **占位纹理原生化**（§6.1-1） | ~35 | 4 | 6，**且删 ~120 行** | 12-15 | 高 |
| 7 | `VkRenderPassManager` / `VkClearManager` / framebuffer（**保留 D18 的节点式容器**） | ~20 | 2 | 0 | 7-9 | 中高 |
| 8 | `ProgramFactory` + **内部 shader 烘焙**（§6.1-2，含 4 天烘焙与回归测试） | ~15 | 1 | 2 | 7-9 | 中（构建 lane） |
| 9 | XFB（**顺带修 D21**）+ query + 回读 | ~15 | 2 | 5 | 11-14 | 中 |
| 10 | swapchain / default FBO（`SwapchainObject.cpp:276-330` 的**写**变 `on_surface_changed`） | ~4 | 0 | 7 | 4-5 | 中 |
| 11 | 删 pull 路径 | — | — | — | 4-6 | 低 |
| | **合计** | **169** | ~34 | 42 | **85-111** | |

**Espryt 的子系统 1 与 Magma 的子系统 1 作为一个里程碑一起做**（合计 6-9 天），这样同一个接口调用在两个 backend 上同时被证明。它们是两个 backend 里最便宜的转换，而且**它们就是整个机制本身**。

### 6.6 strangler 顺序（风险最小化），每步的门

```
0a  getter 移出（AdvertisedLimitsScenario，6 个测试；严格 no-op）
0b  字节/调用计数器落地  ← 树里今天没有任何 per-frame 字节度量
0c  清工作树 per-draw fprintf（DirectGLES.cpp:290-303、Managers.cpp:875-877）
0d  handle 基建：slot 分配器 + registry 变数组 + 删 TwinLookupMemo/OwnerEquals/g_fbSlotCache/GC
1   渲染状态（两个 backend 一起）      ← 机制证明，最便宜的面
2   buffer + BufferBackendOps          ← 泛化已存在的模式；不碰 AcquirePersistentMap
3   VAO / vertex elements
4   framebuffer（**在纹理之前**）      ← 它发布四个派生 mask
5   纹理 / sampler / image unit        ← 最大；删除量也最大
6   program + constant buffer          ← 需要 4 与 5 才能退役陈旧性判定的 4-6、8-9 条
7   XFB + query + 回读                 ← 与 5/6 并行（第二个工程师）
8   emulation
9   删 pull 路径；纯度门转绿
```

**framebuffer 必须在纹理之前**（D-B3）。**0b 必须在任何迁移之前**：所有 ring 尺寸、批处理阈值、render-state wire 粒度决策否则都是猜测。**0c 必须在基线之前**：那两处 per-draw `fprintf` 污染每一次测量，而 `Feat/CS-Delta-IPC` 的 `d96be9f3` 提交过同类东西，导致该分支上**每一次**测量都跑在每 draw 一次 stderr 写的构建上。

### 6.7 A/B：旧路径怎么保留

```
MOBILEGL_PIPE_PUSH = <子系统位图>     # 0 = 全 pull；每位一个子系统
MOBILEGL_PIPE_VERIFY = 0|1            # 影子比对（~5-10x 慢，永不出货）
MOBILEGL_PIPE_STATS = 0|1             # 字节/调用/roundtrip/纹理拉取计数器
```

在 init 时刻锁存，与 `MOBILEGL_BACKEND_TYPE` 完全同一套机制（`ConfigLoader.cpp:212-225`：一个 `Config` 字段 + 一行 `QueryEnvUint32`），并与树里已有的 ~40 个 `MOBILEGL_*` 开关并列（`Config.h:70-303`，其中已有多个是运行时路径切换：`MOBILEGL_ESPRYT_DISABLE_UPLOAD_RING` `:186-191`、`_UBO_RING` `:177-180`、`MOBILEGL_ESPRYT_MULTIDRAW_MODE` `:237-241`）。

**pull 路径一直编译在里面直到 P13**，所以：

- **任何一次提交都能在同一份二进制上按子系统 A/B**，两个 backend 都行。这比整后端级开关细得多。
- 设备回归可以二分到"哪个子系统"而不只是"哪个提交"。
- trace 工装本来就有 per-case 行为开关的先例（`TRACE_COHERENT_AS_FLUSH` → `--coherent-as-flush`，`run_trace_case.cmake:26-29`），`--pipe-push` 照抄那三行。

**P13 删除 pull 路径时**：删 `SnapshotFromGLContext()`、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`（推送成为唯一路径），**保留 `MOBILEGL_PIPE_VERIFY` 的工装**给后续工作；`static_assert(sizeof(ResidualValueBlock) == 0)` 必须编译通过；纯度门（§4.7.2）转绿。
