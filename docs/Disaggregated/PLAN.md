# MobileGL 前后端进程拆分实施计划（MGPipe）

> 状态：设计定稿 v2（2026-09-05，经三视角对抗性评审修订；评审记录见同目录 `REVIEW.md`）。基线 `dev@81b17c0b`；实施分支 `feat/disaggregated`（worktree `../MobileGL-disagg`）。
> 本文是本项目前后端进程拆分的**唯一**实施计划。它定义一份显式的前后端接口 **MGPipe**（gallium 式、句柄寻址、只推不拉），让 `MG_Backend` 拥有自己的状态机，并在此之上把前后端拆到两个进程。传输、数据面、控制面、同步、present、线程、平台与构建（§7-§13）是本文自带的章节，不依赖任何外部文档。
> 全部 `file:line` 引用针对**工作树** `dev@81b17c0b`。工作树有两处未提交的 `fprintf` 插桩，使 `DirectGLES.cpp` 在 ~660 行之后偏移 +11、`Managers.cpp` 在 872 行之后偏移 +3；`MG_State/`、`MG_Impl/`、`MG_Backend/DirectVulkan/` 的行号与 HEAD 一致。
> **v2 修订说明**：v1 里一批继承自调研报告的 `SamplerObject.h` 行号（`:455-492`、`:532-537`、`:551`）指向文件末尾之后——该文件共 160 行。实际位置：`BorderColorForm` 在 `:60-70`、`SamplerParameters` 在 `:72-96`、`GetLifetimeId()` 在 `:141`、`BumpVersion()` 在 `:151`、`m_version` 在 `:155`。**P0 增加一条 CI lint：本目录下所有 `.md` 里的 `file:line` 必须在基线提交上解析到存在的行**（`git show <base>:<path> | wc -l` 比较），防止同类转抄错误再次进入实施规格。

---

## 0. TL;DR、推荐与决策

### 0.1 一句话

**`MG_Backend` 已经是一台贴着目标 API 的状态机；它缺的不是状态，而是一份"我被告知了什么"的显式声明。MGPipe 就是那份声明。** 前端不再让 backend 每 draw 走 293 次 `MG_State::pGLContext->` 把整个 `GLContext` 拉出来，而是在每条命令之前由一个 state tracker 把变化**推**过去；server 进程因此只需要装 `MG_Backend` + MGPipe 的对象表，**不链接 `MG_State`、不链接 `MG_Impl`、不链接 glslang**。

### 0.2 接口不是从 gallium 自顶向下设计的，是从两个 backend 自己维护的关键结构反推出来的

这是本设计与"照抄 gallium"的根本区别，也是完整性论证的来源：

| backend 已有的结构 | 它是什么 | 反推出的接口 |
|---|---|---|
| `SetupDrawSnapshot`（`VulkanRenderer.h:948-1042`，40+ 字段） | Magma 一次 draw 必须钉住的**全部**东西的枚举 | `set_*` 组的并集 |
| `DrawTextureSyncKeys` + `BackendTextureObject::IsDrawSyncClean`（`Managers.h:1003-1020`） | Espryt 纹理"是否还干净"的**全部**输入 | `set_sampler_views` + `create_sampler_view` + `set_texture_params` |
| `ResolvedDrawBuffers`（`Managers.h:697-717`）/ `ResolvedVertexBindings`（`VulkanRenderer.h:1153-1218`） | 顶点输入的完整声明 | `bind_vertex_elements_state` + `set_vertex_buffers` + `set_index_buffer` |
| `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`） | 渲染状态声明，**逐字节** | `create/bind_render_state` + `set_dynamic_state`（见 0.4 D-B1） |
| `UnpackStagingBlock`（`Managers.cpp:4340-4390`，`{src, rowBytes, rows, slices, srcRowStride, srcSliceStride, offset}`） | Espryt 纹理上传的**带步长的源描述符**，已经存在 | `MGPSubData` 的 region 形状 |
| `BufferBackendOps`（`BufferObject.h:76-120`，7 个 hook） | 已经是接口，且注释自称 "the `pipe_context` buffer-op analogue"（`:68`） | `resource_*` 全族 |

把这些结构的**输入集合**推过去，接口就按构造完整。gallium 是**目的地**（同名同形的词汇让形状可读、可迁移），不是**推导前提**。凡 gallium 的词汇与本仓库的证据冲突的地方，本文按证据走，并在 §3.6 逐条记名列出偏离与理由。

### 0.3 四条结构性推论（决定了后面每一节）

**推论 1 — 推送必须发生在 verb 时刻，不是 GL setter 时刻。** Blaze3D 每个 batch 都用 `glEnable/glDisable(GL_BLEND)` 包住，代码自己把它标成最热的路径（`DirectGLES.cpp:2029-2032`：`mc_state_toggle` 干的最热的事）。天真的 per-setter 推送会把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，**严格慢于今天**。正确形态是 gallium 的 `st_validate_state`。
**v2 修订**：v1 把这条写成"只有资源 mutation 在 GL 调用时刻推送——这恰恰是 `BufferBackendOps` 今天的做法"。**这句话对 buffer 成立，对纹理不成立。** 实测：`glTexSubImage*` **根本不调 backend 表**——`MG_Impl/GLImpl/Texture/GL_Texture.cpp` 里只有 3 处 `MarkStorageDirtyRegion`，全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做（`Managers.cpp:4274-4390`），那里才跑 `MipmapStorage` 的 96-rect 级联合并与 `summedArea*4 >= unionArea*3` 回退，并在 unpack ring 可用时**刻意把 rect 列表塌成一个 union box**（`:4386-4390`：`if (BufferImpl::UnpackRingAvailable()) dirtyRectCount = 0;`，注释记录 ~100 个精灵 rect 变成 ~100 个 Mali 作业，实测 **+6 ms/frame**）。若每次 `glTexSubImage` 发一条 `resource_subdata`，就精确复现了那个 ~100 作业的形状。**规则的正确措辞见 §4.1.1。**

**推论 2 — handle 就是身份，而且必须是稠密 slot。** 每个前端对象已经有一个永不复用的 `GetLifetimeId()`（`BufferObject.h:202-208`、`VertexArrayObject.h:110-120`、`FramebufferObject.h:151-158`、`ProgramObject.h:1620`、`TextureObject.h:83`、`SamplerObject.h:141`），它们存在的唯一理由是 GL name 会被 `IndexGenerator::Generate` 从 free list 尾部 LIFO 复用（`MG_Util/Miscellany/IndexGenerator.h:30-42`）、堆地址会被分配器复用。但**单调的 64 位 id 不能索引数组**——如果 wire handle 直接用 lifetimeId，server 侧仍然是一张哈希表，那就只是把指针键换成整数键，并没有删掉查表层。所以 wire handle 是 `{slot: Uint32, gen: Uint32}`，**slot 由 client 按 kind 稠密分配**，`gen` 在 slot 复用时 ++。lifetimeId 留在 client 侧作为 tracker 自己的身份，不过线。这一条才真正把 6 个 `StateBackendObjectRegistry` 哈希表和 13 个 Magma 身份键缓存变成**数组**。

**推论 3 — server 拥有 client 看不见、也永远不该被问的 generation。** 今天有 12 个纯 backend 侧的单调计数器，它们表达的是"**我自己**重新铸造了驱动对象"，与任何前端版本无关：Espryt 的 `g_bufferMutationEpoch`（`Managers.h:397-441`）、`g_bufferBackendIdGeneration`（`:551`）、`g_attachmentBackendIdGeneration`（`:1298`）、`g_backendContextGeneration`；Magma 的 `m_textureImageEpoch`、`m_resourceEraseEpoch`、`m_renderbufferImageEpoch`、`m_sliceEpochCounter`、`m_cacheStructureEpoch`、`m_evictionEpoch`、`m_recordingGeneration`、`m_frameSerial`。本文把它们统称 `MGGen`，**它们永不上线**。"server 拥有自己的状态机"在工程上的确切含义就是这一条：client 绝不是"我的 server 侧状态是否新鲜"的唯一权威。

**推论 4（v2 新增）— dirty 位对值类组可以**轮询**，对对象类组必须**标记**。**
v1 同时主张两件互斥的事：§4.2 说"dirty 位全部来自已有计数器，`MG_State` 零新增记账"，§4.1/§13.2 说稳态是"一次 64 位 dirty word 测试"。对**值类**组（渲染状态、pack、patch、attrib 默认值）两者兼容——一个 `Uint16` 比较就是全部。对**对象类**组不兼容：`NEW_SAMPLER_VIEWS` 在 §4.2 里映射到 `GetContentVersion`/`GetShapeVersion`/`GetTextureParamsVersion`（**逐纹理**）加 `GetTextureBindGeneration()`/`GetSamplingResolutionGeneration()`，没有任何聚合能回答"有没有哪张已绑定纹理的内容动了"。这正是 Magma 不得不用**有损**的 `sampledContentSum`/`sampledParamsSum`（`VulkanRenderer.h:975-1000`）的原因。轮询版本 = 每次 validate 走查 touched 单元，那不是 O(1)，而且是**新增的 client 侧工作**（backend 的 `ResolvedTextureBindingMemo` 今天恰好跳过它）。

**决定**：
- **值类组**：沿用既有计数器，O(1) 比较，`MG_State` 零新增。
- **对象类组**：在 `MG_State` 里**新增 5 个聚合世代计数器**，在既有的 choke point 上 bump，让 tracker 的快门是 O(1)：
  - `TextureState::m_anyTextureContentGeneration`（`ITextureObject::MarkStorageDirtyRegion` / `BumpContentVersion` 里 ++）
  - `TextureState::m_anyTextureParamsGeneration`（`BumpTextureParamsVersion` 里 ++）
  - `BufferState::m_anyBufferChangeGeneration`（`BufferObject::BumpChangeSerial` 里 ++）
  - `VertexArrayState::m_anyVaoAttributeGeneration`（属性/绑定点 setter 里 ++）
  - `FramebufferState::m_anyAttachmentGeneration`（attachment setter 里 ++）
  合计约 **20 行**，全部落在既有的 bump 点上，**不是**枚举 181 个 GL 入口。快门为真时 tracker 才做 touched 前缀走查并重算集合 hash。
- **完整性绊线**：新增 `scripts/gen_pipe_dirty_surface.py`：它枚举 `MG_Impl/GLImpl/**` 里每一个会改变某组的 mutator，映射到必须 bump 的聚合世代，CI 上重生成 + `git diff --exit-code`，**未映射的 mutator 直接失败**。这是 B-R6 的第四层，也是对"reconciler 完整性只有测试绊线"这条历史结论的第二个答案。
- §4.2 的措辞随之改为"**值类零新增记账；对象类新增 5 个聚合世代，换掉 tracker 的逐对象走查**"。§13.2 的稳态成本行同步改写（见 §13.2）。

### 0.4 八个必须先记下来的具体决定（这些是评审里争议最大的点）

**D-B1（v2 重写）：渲染状态用"整块 blob"过线，但 CSO 的**身份**只取 pipeline 相关子集，动态状态单独走。**

v1 写的是"整块 blob + CSO handle，绝不拆成 blend/depth-stencil/rasterizer 三个 CSO"，理由全部成立且保留：`RenderStateParameters`（`RenderState.h:222-370`）是平凡可复制 POD，Espryt 在 `DirectGLES.cpp:2035` 亲自 `static_assert(std::is_trivially_copyable_v<...>)`，紧接着做 head/blend/tail **三段 memcmp**（`:2038-2047`）；`RenderState.h:359-368` 白纸黑字写着 `ScissorBoxWrittenMask` 与 `ClipDistanceEnabledMask` 是**故意**摆在 tail 段里，好让那次 span memcmp 抓到它们；**字段顺序是承重的**；拆成三个 CSO 要手工维护一张 ~150 字段划分表且没有完整性绊线。

**但 v1 同时犯了一个内部矛盾**：它一边在 D3 里说"CSO 边界跟 Vulkan 动态状态走：viewport、scissor、depth range、blend color、line width、depth bias、stencil ref/write mask 是 `set_*` 而非 CSO 字段"，一边把 CSO 的**内容寻址键**定义为**整块**的三段 xxHash。两者不能同真：整块内容寻址意味着 `glViewport`／`glScissor`／`glBlendColor`／`glClearColor`／`glLineWidth`／`glStencilMask`／`glPolygonOffset` 每一次都产生不同的 hash、不同的 CSO handle，于是 (a) 64 项 LRU 在 Iris 光影与阴影级联下颠簸，(b) 每次未命中重发 ~1.2KB，(c) 新 handle 冲掉 server 侧按 CSO 缓存的 pipeline hash——**正是 `RenderState.h:519-528` 记录的那次回归**（"共用一个计数器让 `glViewport` 把下一个 draw 从 pipeline memo **和** draw 快路径上打下来"）。实测确认：`RenderState.cpp` 里 viewport/scissor/line-width 一族的 setter 只做 `++m_version`，`SET_CAPABILITY`（`:312`）与 pipeline 相关 setter 才做 `BumpVersions()`。

**最终形态**：

```
create_render_state(cso, MGPBlobRef pipelineSubsetChunks)   // 只带 pipeline 子集的字节段
bind_render_state(cso, Uint16 version, Uint16 pipelineVersion)   // 稳态 12 B
set_dynamic_state(MGPBlobRef dynamicChunks, Uint16 version)      // 只带动态子集的变化段
```

- server 每 context 持有**一份** working `RenderStateParameters`（~1.2KB）。`bind_render_state` 把 CSO 的 chunk 散射进去，`set_dynamic_state` 把动态 chunk 散射进去。**Espryt 的 `SyncRenderState` 拿到的仍然是一个 `const RenderStateParameters&`，693 行函数体与三段 memcmp 一行不动。**
- Magma 的 pipeline memo 键是 `cso.slot`——**`glViewport` 不再冲掉它**；动态尾巴仍按 `set_dynamic_state` 的 version 走 `ApplyDynamicDrawStateTail` 今天的两级门。
- **划分只写在一个地方**：`MGPipeComputePipelineSubsetHash(const RenderStateParameters&)` 与它的 chunk 表，**从 `VulkanRenderer.cpp:4826-4906` 原样搬进 `MG_Pipe/`**，client 与两个 backend 共用同一个函数。这样"哪些字段属于 pipeline"不再有第二份定义。
- **完整性绊线（这是 v1 拒绝三 CSO 时点名要求、却没给自己的那一条）**：G7 生成一个 `MG_Test`，遍历 `MG_State::GLState::RenderState` 的**每一个 public setter**，用一个不同的值调用它，断言 `pipelineSubsetHash 变了 ⟺ m_pipelineStateVersion 变了`。新加一个 setter 若 `BumpVersions()` 却不在 chunk 表里，这个测试立刻红。
- **两个版本计数器都过线**（`RenderState.h:522` / `:529`），职责不变。
- **两套 span 划分并存，互不干扰**：Espryt 的 head/blend/tail 三段是**驱动侧增量**的划分（不动）；pipeline/dynamic 是**线上与 CSO 身份**的划分（新增）。两者都有各自的绊线。文档必须写清楚它们不是同一件事。
- **热路径成本（诚实版）**：`m_pipelineStateVersion` 未动 → 复用上一个 CSO handle，**零哈希**；动了 → 哈希 pipeline 子集（~25-30 字，正是 Magma 今天已经在算的那个）+ 一次 map 探测。Blaze3D 的 enable/disable 交替会命中两个交替的 CSO，不重发 blob。对比今天：Espryt 1.2KB×3 段 memcmp + Magma ~30 字哈希。**净变便宜，但差距不大**，所以 P2 必须带一个**专门的 enable/draw/disable/draw 微基准**（MC batch 速率）。

**D-B2：`create_shader_state` 不返回一个"做完了的"对象。** backend program 还依赖 8 个额外输入（`DirectGLES.cpp:2766-2818`：draw FBO 的 snorm/unorm fallback clamp mask、由 draw-buffer 数组推出的 fragColor 广播数、storage-block 绑定签名、atomic counter 绑定集、**活的** `glBindImageTexture` 格式、patch 参数；Magma 另加 FragCoord-Y-flip 的 default-FB 高度和 XFB 布局）。接口**明说规则**：`create_shader_state` 发布**制品**，server 在 **verb 时刻**从它已经被推送过的状态**惰性特化**。这正是两个 backend 今天的做法。

**D-B3（v2 重写）：真正承重的不是"framebuffer 第一"，而是"verb 之前状态齐全 + verb 处惰性特化"。**
v1 把 §4.3 的编号顺序（1 framebuffer → 2 program → 3 images → 4 render state → 5 vertex）写成契约，并说这是退役 `ImageUnitFormatsStillMatch`（`Managers.cpp:6545-6573`，注释明说"不可表达为单调版本"）与 fragColor 重推导 workaround（`DirectGLES.cpp:2712-2732`）的机制。**但它自己把 images 排在 program 之后**——所以退役这两条的其实是 **D-B2 的惰性特化**，不是调用顺序。
**规范条款改为**：
> 一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间**没有**顺序要求。

§4.3 的编号列表降级为**推荐实现顺序**（便于 tracker 的代码组织与 dirty 位遍历），不再是正确性契约。收益不变：`DirectGLES.cpp:2712-2732` 的 workaround 与 `g_broadcastMemo*` 照删，因为特化发生在 verb 处、那时 FBO 状态一定已在。

**D-B4：AcquirePersistentMap 在整个改造期一动不动。** 它是**永久的地址空间捐赠**而不是 gallium 的 scoped `transfer_map`：返回一个 host-visible coherent 指针，成为该 buffer 的唯一真相源（`BufferObject.h:102-118`），由 `PipeResource::AdoptPersistentMap`（`PipeResource.h:115`）采纳、经 `MappedData()` 交给应用、≥16MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到（`:226-228`）。实测代价是 MC 26.3 的 p99 163→21ms、40→115fps、省 ~400MB。**它今天就已经是一个"返回指针的显式调用"，因此原样穿过 monolith 改造；只有 IPC 那一步才会打破它。** 改造期不碰，IPC 期按 §7.8 的三档 POST 探针决定，spike B 第一周给答案。绝不允许一个平台未知数挡住 267 天的接口工作。
**v2 补注**：`map_persistent` 的 round trip 是**每次存储定义（respecify）一次**，不是"每 store 生命周期一次"——`TryAdoptLargeStorage` 在存储定义时触发，一个反复扩容的 arena 会付 N 次。`StorageBufferRegrowScenario` 必须发布 `map-persistent-roundtrips` 计数。

**D-B5（v2 修订）：monolith 的字节一致门按构造死亡，这是本方案的成本；但语义门必须活过 P13。**
一个"改前改后 `nm --defined-only` 与剥调试信息后的 `.text` size 完全相等"的 monolith 门在本方案里不成立——**不存在任何配置能让旧字节回来**。替换是**五部分门**（§13.3），其中第 ② 部分（每 draw 逐字段的 pushed-vs-snapshot 影子比对）在语义上**严格强于**任何符号 diff。
**但 v1 的 P13 删掉 `SnapshotFromGLContext()`，而那正是 verify 的参照物来源**——删完之后 verify 无物可比，设计从此没有语义绊线。**修正**：
- `SnapshotFromGLContext()` 与它需要的 `MG_State` include **在 P13 之后继续存在，但整体包在 `#if MOBILEGL_PIPE_VERIFY` 里**；verify 构建**永不出货**。
- 纯度门（`grep -c 'pGLContext' MG_Backend/` == 0、include 白名单、`nm --undefined-only`）**只跑非 verify 构建**，这一点写进门的定义。
- 另外在 P13 交付 §13.4-9 已经勾勒的**录制-金标**模式：把 `MG_Test` 的 mock backend 变成 MGPipe recorder，在一组 fixture 上录下每 draw 的已推送状态，后续构建对比录像。它不依赖 `MG_State`，所以是长期可用的语义门，也是开放问题 11 的答案。

**D-B6：本方案引入一个新的停顿类：server 发起的纹理重铸拉取。** server 不保留纹素字节，所以 `RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）都必须回头向 client 要数据。**三条缓解同时上，不是三选一**，加一个专门的门、一个逐 trace 用例发布的计数器，**以及一个显式的"答不出来"终止符**（§6.5）——因为存在 client **没有**字节可发的 level（纯渲染产生、`CanMirrorCopyImageShadow` 拒绝的 copy 目标、GPU 生成的 mip），没有终止符 apply 线程会永久 park。上一轮 thin-server 设计正是因为把这条一笔带过而被判死。

**D-B7（v2 新增）：restart 重写与 multi-draw 分档**留在 server**，split 下由一份**索引宿主镜像**喂养。**
v1 的 §4.8 把这两条按 `!kCapPrimitiveRestart` / `!kCapMultiDraw` 下放到 client，而 §3.5.7 的表又写"monolith：`ptr` 指向 shadow（server 做）"——**两处互相矛盾**。更根本的是这个划分不可表达：
- `ResolveTierForBatch`（`MultiDraw.cpp:282-320`）**逐 batch**在五档里选，输入包含 `programReadsDrawID`——**转译出的 ESSL 的性质，只存在于 server**——以及 `perSubDrawBaseVertex`、`hasIndexBuffer`、`arbitraryRestart`，并在 `kMaxFlattenedIndices`（`:72`，1<<24）与 `kMaxComputeFlattenedIndices`（`:82`）上做容量判定。自动阶梯是 Ext → BaseVertex → MultiIndirect → Indirect → DrawElements（`:241-243`），CPU 展平的 `DrawElements` 档是**回退**，client 无法预判。
- restart 重写**两个 backend 都做**（`DirectGLES.cpp:4283/4377`、`VulkanRenderer.cpp:3990/4089/4161`），所以 `kCapPrimitiveRestart` 恒为 false，"cap 门控"没有门可控。

**决定**：`kCapPrimitiveRestart` / `kCapPrimitiveRestartFixedIndex` / `kCapMultiDraw` / `kCapMultiDrawIndirect` / `kCapMultiDrawIndirectCount` 作为**归属开关**删除。规则改为一句话：**multi-draw 分档与 restart 重写永远由 server 拥有；client 在 caps 说 server 可能需要时提供索引字节。** 提供方式不是逐 draw 拷贝，而是：

> **`kCapNeedsHostIndexBytes` 开启时，server 为"曾被绑为 `GL_ELEMENT_ARRAY_BUFFER` 的 buffer"维护一份宿主镜像**，由它本来就要收的 `resource_subdata` / `resource_respecify` 流**增量**维护，**零额外线上流量、零 round trip**。预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），逐帧计数；超预算时该 buffer 退化为逐 draw 通过 `MGHostSpan` 传送并计入 `index-bytes-shipped` 计数器。

好处：monolith 行为**零变化**（不搬代码、不改诊断落在哪个线程 → 开放问题 12 关闭）、split 下 restart/multidraw 零 round trip、`kMaxRestartRewriteBytes = 1<<26`（64 MiB，`DirectGLES.cpp:4218`）这种单条记录不再需要塞进 32 MiB 的 `SEG_STAGE`。代价是那份镜像的内存，已计入 §7.9。

**D-B8（v2 新增）：per-draw 的**具名 uniform block 字节**必须有自己的载体。**
v1 §6.2 断言 20 处 `SyncPersistentMappedRange` "作为反向调用彻底消失，因为紧邻它们的 CPU 读全部搬到了 client"。**有一处反例**：`UniformManager::ResolveUniformBufferPayload` 在 `UniformManager.cpp:2022` 调 `SyncPersistentMappedRange()`，随后在 `:2052` 读 `bufferObject->MappedData() + rangeStart`（不足时在 `:2053-2057` 零填充），把具名 UBO 块打进 **Magma 自己的 UBO ring**——消费者在 server，搬不走。而 §3.4.3 的 `set_shader_buffers` 只有 `V` 标志，没有 `kHasBlob`/`MGHostSpan`；`set_global_constants`（D6）只覆盖**默认** uniform block。**结果是每个带具名 UBO 的 Iris/MC draw 都有一条没被承载的数据依赖。**
**决定**：`set_shader_buffers(cls == Uniform, ...)` 的每个 range 增加可选的 `MGHostSpan payload`（`kHostSpan` 标志），由 `kCapNeedsHostUboBytes` 门控（Espryt 不需要——它把具名 UBO 直接绑给驱动）。字节量进 `SEG_STAGE` 的尺寸表（§7.1）与 P0 计数器（`stage-ubo-named`）。**在 P0 计数器给出逐帧字节量之前，不冻结这个 payload 的形状。** 备选（不在本计划内、需独立 `dev` PR + Iris 性能门）：让 Magma 直接描述符绑定常驻 `VkBuffer` 的 range，不再 ring-pack。

### 0.5 推荐

**按下面这条对冲路径起步，在第 43 天做一次真正的 GO/NO-GO：**

先跑 **P0**（卫生、度量、门与骨架，含两个 spike，尤其是 **`TracyPlot` 逐帧字节与调用计数器**——树里今天完全没有 per-frame 字节或调用度量，`MG_Util/Metrics` 只是格式算术，Tracy 只有 zone 无 plot），然后跑 **P0.5 + P1 + P2**。

- **第 ~25 天（P1 出口）— 机制里程碑，零产品风险**：`MOBILEGL_PIPE_VERIFY` 影子比对 harness 在全部 40 个 trace 用例与 367 个集成测试上逐 draw 逐字段证明"推送等价于拉取"。这一天**不**是 GO/NO-GO——它只证明机制，不给性能数字。
- **第 ~42 天（P2 出口）— GO/NO-GO**。

**v2 修订：GO/NO-GO 的口径必须包含一片 Track H，否则它测的不是它要决定的事。**
v1 把 GO/NO-GO 放在"只迁了渲染状态"的时点，而渲染状态恰好是推送**收益最小、v1 的 CSO 设计开销最大**的那个面：Espryt 已经有逐字节镜像 + 单个 `Uint16` 早退（`DirectGLES.cpp:2016-2018`），Magma 已经按 `GetPipelineStateVersion()` 缓存哈希（`:4982-4993`）并双门控动态尾巴（`:5888-5893`）。绿灯不能证明它要担保的事（Track H 的 handle 化在 267 天里划得来），红灯更可能是在指控 CSO 设计而不是推送模型。
**因此 P2 的范围扩大为**：渲染状态 CSO（双后端）**＋ 最便宜的两片 Track H**——Espryt 的 0b handle 基建（`SlotAllocator` + 6 个 registry 变 slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`）与 Magma 的子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，§5.5 自评"低（纯结构性收益）"）。第 43 天你手上会有：

- 逐 draw 逐字段的语义等价证明（P1 交付）；
- 两个 backend 上都已推送的渲染状态，`SyncRenderState` 的 693 行函数体一行未动；
- **Track H 的实测单位成本**（两片，两个 backend 各一）；
- 两台设备上 reboot-clean 配对的**逐线程 CPU 时间**增量，含一个专门的 Blaze3D blend-toggle 微基准；
- 一个**负面对照**：关掉 CSO 内容寻址（`MOBILEGL_PIPE_PUSH` 的一个子位）重跑，把"推送更慢"与"CSO 设计更慢"分开。

**GO/NO-GO 的两个出口，写死在这里：**

- **继续**：第 43 天的逐线程 CPU 增量在两台设备的 p50 与 p99 上都不为负、tracker 每 draw 的绝对 ns 落在预设上限内、Track H 的实测单位成本不超出 §5.4/§5.5 估计的 50%。此时按 §14 的两条跑道推进（monolith 跑道 P3a→P4a→P3b/P4b→P7→P13，IPC 跑道 P5→P6→P8→P13）。
- **收缩为 headless 工装用途或重新评估**：任何一条判据落空时，**不回滚**。P0/P0.5/P1/P2 的产物全部是自洽的 monolith 交付物——handle 基建与 `{slot, gen}` 重键、`MGPipeValueTypes.h` 与 `ProgramArtifacts.h` 的头文件抽取、逐帧字节与调用计数器、`MOBILEGL_PIPE_VERIFY` 影子比对 harness、渲染状态 CSO——它们就地保留在 `dev` 上。MGPipe 本身**收缩为 headless 工装用途**：`MG_Test` 的 mock backend 变成 MGPipe recorder（§13.4-9），给 `tools/trace_replay` 一种比 apitrace 精确得多的、记录**已解析**状态的录制格式；`inproc` 作为渲染线程实验保留在 CI 形态下。IPC 跑道整体搁置，等一个新的判据（例如 §13.2 的 CPU 数字在别的子系统上转正、或产品侧对崩溃隔离提出硬需求）再重新评估。

**沉没成本（诚实版）**：P0（9-11 天）的卫生、度量与骨架无论后续走哪条路都要花；P0.5 的头文件抽取本身就是 monolith 的净收益（它让制品头不再拖 glslang 与 spirv_reflect）。**真正只为 MGPipe 押上的是 P1 + P2 ≈ 28-39 天**，而这 28-39 天在 NO-GO 分支下仍然留下上面那份可用产物。v1 说"只损失 16 天"是按一个与它自己的子系统表矛盾的排期算的。

---

## 1. 目标与非目标

### 1.1 目标

1. **定义并落地一份显式的前后端接口 MGPipe**：句柄寻址、只推不拉、gallium 形状，client 与 server 都只依赖它。
2. **backend 拥有自己的状态机**：`MG_Backend` 在 MGPipe 构建（非 verify）下**不含** `MG_State::pGLContext`，`MG_State` include 收缩到一张共享**值**头文件白名单，server 产物的 `nm --undefined-only` 里没有 `MG_State::GLState::` 符号、没有 glslang 符号。
3. **前后端跑在两个进程**，通过 IPC 通信；client 把状态 reconcile 成推送调用、序列化（FlatBuffers）后发送；server 更新自身状态并调 backend API。
4. **稳态帧零 round trip**（回读 / 阻塞式 query / sync wait / present credit / 分配类错误 ack / 纹理拉取之外，且后者的次数必须**实测发布**而非声称为零）。
5. 两半尽可能互相异步；client 至多领先 server 1 个 present（默认，延迟叠加分析见 §9.1）。
6. 平台特定代码最小化并集中在 `MG_Remote/Transport/` 与 `MG_Remote/Client/Surface*`（§11）。
7. **单进程 Monolith 保持功能与性能不回归**，由五部分门机械验证（§13.3）。注意这**不是**字节级不变——见 D-B5。
8. 所有验收门用**现有测试**：`ctest -L unit`（428 个 `TEST(`）/ `-L integration-gpu`（367 个 `TEST_F`，75 个场景文件）/ `tools/trace_replay`（40 个用例，默认 SSIM ≥ 0.99）/ `tools/cts` / `tools/device_bench`。
9. **接口本身是可独立交付的产物**：即使 IPC 永不上线，`inproc`（同进程第二个 apply 线程）就是 monolith 的渲染线程交付物，且是本项目手上最大的单一 CPU 杠杆。

### 1.2 非目标

- **share-group sessioning 重构。** `eglCreateContext` 的 `shareCtx` 只在 `EGLState/Core.cpp:632` 被校验、`:640` 被存进 `EGLContextState::SharedContext`，**全代码库无人读取**；`pGLContext` 是唯一进程全局（`GLState/Core.cpp:20, 1487`）。v1 = 一条 flow、一个扁平 handle 空间。但**接口头文件从第一天就把 `MGPipeScreen` 与 `MGPipeContext` 分开**（§3.3）。`c7c9e346`/`29d721ef` 那套整体丢弃（理由见 §17 的 DROP 名单）。
- **BFA strict-C-ABI backend 插件 / UtilRuntime C-ABI 化**（理由见 §17 的 DROP 名单）。
- **macOS 拆分**（`CAMetalLayer` 无公开跨进程表示 → monolith only）。
- **Windows 窗口拆分**（headless/pbuffer only，见 §11.5）。
- **把 emulation 层重写到 client。** 只有**三**个"读前端字节的纯 CPU 变换"下放到 client（v1 说五个，D-B7 收回了两个）：client 顶点数组的范围计算、最大索引扫描、`*IndirectCount` 的计数解析。viewport-array 回放、**multi-draw 分档**、**primitive-restart 重写**、fp64 顶点转换、image-bindable 存储加宽等**全部留在 server 作为 lowering pass**，接口只负责把它们的输入表达清楚（含 D-B7 的索引宿主镜像）。
- **在 P13 之前删除 pull 路径。** 旧路径一直编译在里面，任何提交都能用一个 env 位 A/B（**但要注意 §5.7 说明的 A/B 口径在 stage C 之后会收窄**）。

---

## 2. 现状：边界为什么不清楚

### 2.1 今天的边界有七个面（数字按工作树复核）

**(a) `GLFunctionsTable`** — `MG_Backend/BackendObject.h:117-278`。**实测 67 个函数指针 + 1 个 `Bool` 能力位**（`PrefersCpuXfbPrimitiveAccounting`），`GlobalBackendFunctionsTable`（`:279-285`）再加 `Present` 与 `SetSwapInterval` → **全体 69 个函数指针**。
MG_Impl 侧 **~93** 个 `gBackendFunctionsTable.GL.*` 调用点，覆盖 **70 个不同表项**。**null 项已经表示"未实现，前端回退"**，写进头注释（`:212-215` 的 sync 族、`:265-269` 的 XFB 跨度），且 DirectVulkan 确实留空 8 项而 Espryt 填满。三项是错位的前端查询：`GetIntegeri_v`/`GetInteger64i_v`（`:195-196`，`DirectGLES.cpp:7264-7386` 完全不碰 GL）、`GetProgramiv`（`:197`）。**（P0 实测修正）"15 个 case"是错数**：`:7264-7386` 是 `GetIntegeri_v` 的 9 个分支加 `GetInteger64i_v` 的 2 个，共 11 个。**`GetInteger64i_v` 与 `GetProgramiv` 两个表项已在 P0 从 `GLFunctionsTable` 连同两个 backend 的实现一起删除**（提交 "retire the two frontend queries that were never asked"），本节的表项计数是删除前的基线数。

**这 70 个表项里只有约 22 个是 draw/dispatch**（20 个 draw 族 + `DispatchCompute`/`DispatchComputeIndirect`）。**其余 ~48 个是 clear（9）、blit（2）、copy（3）、`GenerateMipmap`、回读（4）、barrier（2）、XFB 跨度（6）、query/sync（~19）、`BindImageTexture`、`PatchParameteri`、`ShaderStorageBlockBinding` 等**，而其中很多**自己就读 `pGLContext`**（例：`UpdateTextureBindingAtTarget` 在 `DirectGLES.cpp:6051-6052` 读 `GetActiveTextureUnit()` + `GetTextureUnitObject()`，被 `CopyTexImage2D`/`CopyTexSubImage2D` 路径命中；`PackStateFromContext` 在 `:6129` 读 `GetPixelStoreParameters(false)`；`Clear` 在 `:4106` 读 `GetRenderStateParameters().ClearColor`、`:4165` 读 draw FBO；`BlitFramebuffer` 在 `:5988-5989` 读两个 FBO slot）。代码自己说明了这一点：`DirectGLES.cpp:1501-1502` 写着无参 `CaptureDrawTextureSyncKeys` 包装存在是"for every non-draw call site (Clear, readbacks)"。
**这是 v1 的一个实质性缺口**：它只在 `PrepareForDraw` 与 `SetupDraw` 两处填快照。修正见 §5.2.1 与 §14 P1。

**(b) `BackendObject` 虚函数** — `BackendObject.h:543-568`，MG_Impl 侧 **40** 个 `pActiveBackendObject->`（其中 35 个是 `GetDynamicParameters()`）。`InitCapabilities()` 懒执行在第一次成功的 `eglMakeCurrent` 内部（`BackendObject.cpp:341-347`），且每次 surface 变更重新武装（`:301`）。

**(c) `BufferBackendOps`** — `BufferObject.h:76-120`，**7 个 hook**，注册入口 `:124`。Espryt 注册 7/7（`Managers.cpp:1338-1346`），Magma 注册 6/7（**故意**不注册 `ResidentSubData`，`VkBufferManager.cpp:104-111`）。**这个面已经是 MGPipe 的三分之一，且注释自称 `pipe_context` 类比。**
**注意它只覆盖 buffer。** 纹理**没有**对应的 GL 调用时刻分发面（推论 1 的 v2 修订）。

**(d) 状态拉取** — `MG_State::pGLContext->` 在 `MG_Backend` 里 **293 次出现 / 290 行**（DirectGLES 124；DirectVulkan 169），**外加 58 行非箭头用法**（见 2.4）。此外还有约 1997 个前端对象 getter 调用点、186 个不同 getter（上界统计）。

**(e) backend → frontend 写回** — 逐名 grep 实测 **95 个调用点 / 17 个方法**：`SyncPersistentMappedRange` 20、`MarkStorageDirty` 18、`AllocateStorage` 8、`WritebackFromBackend` 8、`SetInternalFormat` 7、`SyncGpuWrites` 6、`MarkGpuWritten` 6、`RecordError` 6、`SetBackendResource` 4、`EnsureGpuResidentStorage` 3、`SetBackendHashMemo` 2、`InvalidateCompileEnv` 2、`SetBackendStateMemo` 1、`SetBackendAuxMemo` 1、`UpdateMipmapSubData` 1、`TruncateMipmapLevels` 1、`SetSamples` 1。

**(f) backend 反向进 MG_Impl** — 恰好 6 处：`DirectGLES.cpp:1917, 2838, 2867, 9675`（`pDefaultFramebufferInfo` 身份比较）、`SwapchainObject.cpp:276`（**写**）、`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`，一处真正的分层倒置）。

**(g) MG_Impl 在 table 调用旁做的 `MG_State` mutation** — `EnsureGeneratedMipmapStorageAllocated`（`GL_Texture.cpp:501-544`，调用点 `:6698, 6708`）与 `AccountTransformFeedbackPrimitives`（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`）。**在 MGPipe 里这个面的 replay 义务不存在**（server 没有第二份前端状态可 replay）；但**标记义务**出现（推论 4），由 dirty-surface 生成器覆盖。

**(h) 工作树污染** — `DirectGLES.cpp:640-663` 与 `Managers.cpp:875-877` 的未提交 per-draw `fprintf(stderr)`（后者在 `pendingMutex` 临界区内）。**P0 第一件事就是清掉。**

### 2.2 backend 已有的状态机清单（这就是"server 已经是薄服务端"的实证）

**DirectGLES（Espryt）**
- 6 个 twin registry，全部是 `StateBackendObjectRegistry<StateObject*, {BackendPtr, weak_ptr}>`（模板 `Managers.h:270-390`；实例 `:806`(VAO) `:1123`(Texture) `:1216`(FBO) `:1731`(Program) `:1830`(Sampler) `:1858`(Renderbuffer)），键是**前端裸堆地址**，用同址 `weak_ptr` 防 ABA，GC 阈值 `kGCInterval=1024` draw / `kCreationGCInterval=64` 次创建。
- 三条 persistent-mapped bump ring（UBO `Managers.h:591-637`、纹理 unpack PBO `:639-671`、buffer upload `:673-…`），各自 4MiB 起 → 64MiB 上限；buffer pool 预算 `kMaxPoolBytes = 64MiB`、单 buffer 上限 8MiB（`Managers.cpp:564-565`）。
- 每对象 twin：`GLESBufferResource`（`Managers.h:443-497`）、`BackendVertexArrayObject`（`:675-803`）、`BackendTextureObject`（`:944-1119`）、`BackendFramebufferObject`（`:1140-1213`）、`BackendProgramObjectImpl`（`:1473-1725`）、`BackendSamplerObject`（`:1808-1824`）、`BackendRenderbufferObject`（`:1838-1855`）。
- 完整的渲染状态**值镜像** `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`）+ 单个 `Uint16` 早退门（`:2016-2018`）+ 三段 memcmp（`:2038-2047`）。
- 驱动绑定影子、三个共享 scratch FBO 及其驱动侧 attachment 影子、`PackState`。
- **`UnpackStagingBlock`**（`Managers.cpp:4340-4390`）——一个已经存在的**带步长源描述符**，`MGPSubData` 的 region 直接照抄它的形状（§3.5.6）。

**DirectVulkan（Magma）**
- `VulkanRenderer`：`PipelineMemoEntry m_pipelineMemo[8]`、`SetupDrawSnapshot m_setupDrawSnapshots[4]`（40+ 字段）、`VaoDrawMemo m_vaoDrawMemoTable[2048]`、`ResolvedVertexBindings`、`m_convertedVertexStreams`、`DynamicStateShadow g_dynamicStateShadow`、采样集/LOD/BaseVertex 三个 memo、11 个 per-draw scratch vector。
- 5 个 manager（`VkBufferManager`、`VkTextureManager` 3504 行、`VkRenderPassManager`、`VkSamplerManager`、`VkClearManager`）、3 个 factory、`UniformManager`、`FrameContext`、`SwapchainObject`。

**结论：两个 backend 都已经是完整的、贴着各自 API 的状态机。** 上面**没有一样东西需要删除或重写**——需要改的只是它们**怎么知道**这些事实，以及它们的 memo **用什么做键**。

### 2.3 pull 模型的读点分类：A/B/C/D/E 五类

| 类 | 含义 | DirectGLES | DirectVulkan | 合计 | 占比 |
|---|---|---|---|---|---|
| **A** | 只为**探测变化** | ~21 | ~14 | **~35** | 12% |
| **B** | **翻译输入**，backend 无镜像 | ~88 | ~128 | **~216** | 74% |
| **C** | 瞬时 draw 参数 | ~2 | ~2 | ~4 | 1% |
| **D** | **身份 / 缓存键**（与 B 重叠计） | ~24 | ~24 | ~48 | — |
| **E** | 数据字节（经 `pGLContext` 本身） | 1 | 2 | 3 | 1% |
| **写** | `RecordError` 6 + `InvalidateCompileEnv` 2 | 2 | 6 | 8 | 3% |

**这张表否定了两种直觉方案：**

- **"bump 一个版本让 server 自己拉"行不通。** 只有 12% 是 A 类。74% 是 B 类：值本身必须过去。
- **两个 backend 想要的推送粒度不同，但可以被同一个接口满足。** Espryt 持有逐字节镜像；Magma **没有任何镜像**，它按 `GetPipelineStateVersion()` 缓存一个**值哈希**（`VulkanRenderer.cpp:4982-4993`），然后在 payload 构建器里把 ~40 个字段再读一遍（`:5155-5200`，**仅在 pipeline memo 未命中时**）。整块 blob 同时满足两者。

另一个角度：1997 个前端 getter 站点里，**89 个是纯版本/序号读（A 类）**——推送模型里根本不过线；**72 个是数据字节读（E 类）**，全部在 §4.7/§4.8 处理；**38 个是 `GetLifetimeId()` 身份读（D 类）**，全部变成 handle。

### 2.3.1 v2 新增：把"每 draw 成本"用**动态**口径说清楚

v1 的 §13.2 把今天的每 draw 状态获取写成 "Espryt 124 / Magma 169 次 accessor 调用"。**124/169 是静态调用点数（§2.1(d) 的定义），不是动态每 draw 调用数。** 树里每一处都已经被 memo 门控：

| 路径 | 稳态实际做的事 |
|---|---|
| `SyncRenderState`（`DirectGLES.cpp:2003`） | `:2007` 读一个 `Uint16`，`:2016-2018` 相等即 `return`。**三段 memcmp 只在版本移动后跑。** |
| `SyncNeccessaryTextures`（`:1520`） | 6 值键比较 + `PairingsIntact` + 每条目一次 `IsDrawSyncClean` 字比较；单元走查只在未命中时跑 |
| `CurrentUnitBindingsEpoch`（`:1418-1436`） | 三值快门；owner 走查只在 bind generation 移动后跑 |
| `TrySetupDrawFastPath`（`VulkanRenderer.cpp:5994`） | ~10 次 accessor + ~20 次字比较 |
| `GetOrCreatePipeline`（`:4948`） | `:4982-4993` 只在 `GetPipelineStateVersion()` 移动后重算哈希；`:5155-5200` 的 ~40 次 accessor 走查**只在 pipeline memo 未命中时**跑 |
| `ApplyDynamicDrawStateTail`（`:5871`） | `:5888-5893` 一次版本比较，然后一次 bulk fetch 建值键 |

**所以真实稳态大约是每 backend 每 draw 10-25 次 accessor 调用加几十次字比较，不是 124/169。** 推送模型的优势因此比 v1 声称的**窄得多**，而且它在 §13.2 的对照表必须按动态口径重写（已改）。

**（P0 实测修正）第一个实测数据点：预测成立。** P0 的动态 accessor 计数器在 lavapipe / llvmpipe 上跑 `GuiBatchScenario`（14 帧 / 26 draw），得到**每 draw 动态 accessor 调用数：Espryt 20.65、Magma 15.54**——两者都落在本节预测的 10-25 区间内，且都远低于 124/169 的静态调用点数。**告诫两条**：(a) llvmpipe 上 pipeline memo 是**冷的**（场景太短，未进入真正的稳态命中率），所以这两个数偏**高**而不是偏低，真机稳态只会更靠近区间下沿；(b) **两台设备的数字仍然欠着**（设备锁），第 43 天的 GO/NO-GO 绝对 ns 阈值必须等真机基线，不能拿这组桌面数字定。

**推论**：
1. P0 的计数器交付物**必须包含动态调用计数器**（每 draw 实际执行的 accessor 次数、每个 memo 门的命中/未命中），不只是字节计数器——否则 P2 仍然是在猜。
2. 第 43 天的 GO/NO-GO 阈值必须是一个**绝对数字**（tracker 每 draw 的 ns，两台设备实测），不能只写"落在 monolith-pull 的噪声内"——当真实基线是 20 次调用时，相对噪声阈值会平凡通过。

### 2.4 pull 模型里 293 之外的 58 行：迁移机制必须显式处理的缺口

| 形态 | 数量 | 例子 | 处理 |
|---|---|---|---|
| `MOBILEGL_ASSERT(MG_State::pGLContext, ...)` 真值判定 | ~34 | `DirectVulkan.cpp` 密集区、`UniformManager.cpp` 9 处 | **直接删除**（`Defines.h:114` 在非 debug 下宏为空，所以这批**在 RelWithDebInfo 里本来就不生成代码**）；替换成 §5.2 的 poison mask |
| `if (MG_State::pGLContext)` 空守卫 | 7 | `Managers.cpp:3608`（守 `BackendTextureObject::StampViewSyncKeys` 的三次赋值）、`:3737, 3808, 4663, 8678`、`BackendObject_DirectVulkan.cpp:388, 788` | 删除守卫，改读 `PipeInputs` 字段（永远有效）。**这批会改变 `.text`**（见 §14 P1 验收修正） |
| `MG_State::pGLContext != nullptr ? A : B` 三元 | 3 | `Managers.cpp:7120, 7128, 7131`（patch 参数，在 transpile 路径内） | 由 `set_patch_state` 覆盖，三元塌成直接读。**改变 `.text`** |
| `MG_State::pGLContext.get()` 裸指针捕获 | 1 | `DirectGLES.cpp:146` | **`sed` 完全抓不到**，必须手改。相邻的 `:142` 还有一个 `decltype(MG_State::pGLContext->GetFramebufferBindingSlot(...))` 类型别名，同属此类 |
| `!= nullptr` 条件 | 14 | `VulkanRenderer.cpp:11150, 12649` 等 | 同空守卫 |
| 注释 | 1 | `VertexInputStateFactory.h:133` | 改写措辞 |

**因此：纯度门 grep 的是 `pGLContext`，不是 `pGLContext->`**，且 P1 的机械替换步骤必须把这 58 行列成显式清单逐条转换。

### 2.5 pull 模型为了弥补"没有接口"而付的代价（v2：区分**真删除**与**搬迁**）

v1 把下表全部记作"~550 行删除"。**其中一部分是搬迁，不是删除**，必须分开记账，否则 §13.4 的 monolith 收益被高估。

**真删除（结构性，`{slot, gen}` 与显式 destroy 让它们不可表达）**

| 机制 | 位置 | 行数 |
|---|---|---|
| `TwinLookupMemo` ×3（4096+256+64 槽 ≈ 140KiB）+ `OwnerEquals` | `DirectGLES.cpp:62-131` | ~75 |
| `g_fbSlotCache` + `GetFramebufferBindingSlotFast` | `DirectGLES.cpp:139-155` | ~17 |
| `StateBackendObjectRegistry::CollectGarbage` ×6 | `Managers.h:353-390` | ~40 |
| `m_convertedVertexStreams` 的 `SharedPtr sourcePin` | `VulkanRenderer.h:1124-1127` | ~5 |
| `UniformManager` 的 8 类占位 `TextureObject` 构造 | `UniformManager.cpp:161-181, 1416-1500, 1624-1634` | ~120 |
| `SetupDrawSnapshot` 的 `sampledContentSum`/`sampledParamsSum` 与 ~14 个探测字段 | `VulkanRenderer.h:975-1000` | ~30 |
| `g_broadcastMemo*` + fragColor 重推导 workaround | `DirectGLES.cpp:2669-2732` | ~60 |
| `VkTextureManager::PruneDeadTextures` 的 `WeakPtr::expired()` GC | `VkTextureManager.cpp:1694-1720` | ~25 |
| **小计** | | **~372** |

**搬迁到 client（**不是**净删除）**

| 机制 | 位置 | 行数 | 为什么搬而不是删 |
|---|---|---|---|
| `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` / `CurrentUnitBindingsEpoch` / `UnitTextureSyncEntry` / `PairingsIntact` + 8 个支撑全局 | `DirectGLES.cpp:1372-1489` | ~115 | 它存在的理由是 `GetTextureBindGeneration()` **在冗余重绑时也 bump**（`:1414-1420` 注释：26.2 在每次纹理单元切换前后重绑同一个 sampler）。而 §4.2 恰好把这个计数器列为 `NEW_SAMPLER_VIEWS` 的 dirty 输入。**若 tracker 直接信它，每一次冗余 `glBindSampler` 都会重发一次 `set_sampler_views`——一条 `kVarTail` 变长记录，每 draw 几百字节，且 server 侧 `viewSetSerial` 一动就冲掉解析绑定 memo 与 sampler pass memo。** 这正是那 115 行要防的 per-batch 回归。**去抖必须搬到 client**：tracker 对已解析的 view/image/buffer 集合算 hash，hash 未变则**不发**（`MGPFramebufferState::contentHash` 已经演示了这个模式，这里把它推广到其余 `kVarTail` 的 `set_*`，并且在 client 侧当作**发射抑制器**用，不只是 server 的 memo 键） |
| `g_fboTextureSyncList`（`:1580-1601`） | | ~20 | 同上，针对 attachment；由 `MGPFramebufferState::contentHash` 抑制 |
| `ResolvedTextureBindingMemo` 的完备性解析（`IsMipmapCompleteForFilter` / `SamplesAsIncompleteTexture` / `IsUndefinedDefaultTexture`） | `DirectGLES.cpp:3218-3291` + `TextureObject.h:309/315/329` | ~40 | §4.5 把 view 解析放在 client，所以 client 需要自己的 memo 才不会每 draw 重解析 |
| **小计** | | **~175** |

**净账：monolith 侧真删除 ~372 行；另有 ~175 行从 backend 搬到 `MG_Impl/Pipe/Tracker.cpp`。** §13.4 按这个数字改写。

### 2.6 21 个 D 类身份 memo：它们各自守什么，以及为什么 `{slot, gen}` 能等价替换

统一事实：**每一个进入 memo 键的版本计数器要么是回绕的 `Uint16`，要么根本不会被它真正害怕的那个 mutation bump。** `BindingSlot::m_version`（`MG_Util/Types.h:197`）、`FramebufferObject::m_objectVersion`（`:183`）、`SamplerObject::m_version`（`SamplerObject.h:155`）、`RenderStateParameters` 版本（`RenderState.h:522`）、`TextureObjectBase::m_textureParamsVersion`（`:203`）全部回绕。**身份比较是堵住回绕洞的那块补丁。** 完整的 21 条重键表在 §3.7；这里只点三条最有教育意义的：

- **D3 `UnitTextureSyncEntry` + `PairingsIntact`**（`DirectGLES.cpp:1441-1481`）：注释写明它存在是因为"一次不经过 bind generation 的 slot 交换（DSA by-name 模拟以前就会静默交换一个 slot）会让每个键都匹配，而借来的 slot 指向另一张纹理，replay 于是会**用纹理 B 的前端状态驱动纹理 A 的后端 twin**——用 B 的形状重新指定 A 的后端存储并毁掉 A 的内容"。**这是整份调研里最强的"支持推送接口"的论据**：这一整类 bug 只在"client 能改一个绑定而不移动任何计数器"时才存在。审计义务从"哪些读需要守卫"变成"哪些 mutator 必须发消息"，由 §13.3 的 verify 模式、poison mask 与推论 4 的 dirty-surface 生成器共同强制。（**注意**：这条的**去抖**部分搬到 client，见 §2.5。）
- **D11 `VertexInputStateFactory::ComputeHash`**（`VertexInputStateFactory.cpp:38-49`）：注释是一份 postmortem——"地址会被分配器复用……一个已销毁 buffer 的 GPU 切片被绑给了它的后继者的 draw，这就是一次 transform feedback 捕获拿回一个死 VAO 的顶点数据（0,0,0,1……）的原因"。**所以 `gen` 必须被混进 server 侧的每一个 content hash，而不只是被比较。**
- **D18 `VkRenderPassManager::m_renderbufferResources` / `VkTextureManager::m_textureResources` 用节点式 `std::unordered_map` 而不是本项目开放寻址的 `UnorderedMap`**（postmortem 在 `VkRenderPassManager.h:375-397`）：因为调用方会跨后续查表缓存 `RenderbufferResource*`/`TextureResource*`，一次扩表搬迁曾让 `BlitFramebuffer` 静默停在"source image layout is undefined"。**这一条在重键表里被显式标为 UNCHANGED**，并进 review checklist。

### 2.7 v2 新增：MGPipe **增加**的代码（诚实账）

§2.5 数了删除，v1 没有数新增。永久新增的大致规模：

| 组件 | 估计行数 |
|---|---|
| `MG_Pipe/`（`PipeCalls.def` ~72 行 + `MGPipeTypes.h` ~14 个 POD + handles + host span + callbacks） | ~1,200 |
| 7 个生成器 `scripts/gen_pipe.py`（G1-G7） | ~1,500 |
| 生成产物（`PipeTables.inc`/`PipeThunks.inc`/`PipeWire.inc`/`PipeVerify.inc`/`PipeFilled.inc`/`PipeCoverage.inc`/`PipeSpanTable.inc`） | ~4,000（生成，不手写） |
| `MG_Impl/Pipe/`（Tracker、SlotAllocator、CsoCache、HostResolve、CompositeResolver）**含从 backend 搬来的 ~175 行** | ~2,200 |
| `MG_Backend/MGPipe/`（`PipeInputs.h` + 两个 impl） | ~1,500 |
| `MG_State` 的 5 个聚合世代 + `ProgramArtifacts.h` 抽取 + `MGPipeValueTypes.h` 抽取 | ~250（净新增很小，多为搬移） |
| `MG_Remote/`（emitter、`PipeApplier`、`PipeObjectTables`）——**仅 disaggregated 构建** | ~2,500 |
| **monolith 永久新增（不含 `MG_Remote`）** | **≈ 6,650 手写 + 4,000 生成** |

**所以 monolith 的净行数是增加的，不是减少的。** §13.4 里 "~550 行删除" 不再作为主论据；**主论据是 §13.3-④ 的逐线程 CPU 数字**（每 draw 指令数与 cache line 触达数的减少），而删除清单降级为佐证。B-R2 因此有了一个可证伪的预测而不只是定性主张。

---

## 3. 接口设计：MGPipe

### 3.1 文件布局与单一真相源

```
MobileGL/MG_Pipe/                      # client 与 server 都 include；不链接 MG_State，不链接 MG_Impl
    PipeCalls.def                      # X-macro：调用目录的唯一真相源，一行一个调用
    MGPipe.h                           # 由 .def 生成的两张函数表 + 手写 payload 声明
    MGPipeTypes.h                      # 全部 payload POD（trivially copyable，逐个 static_assert）
    MGPipeValueTypes.h                 # ★v2 新增：无依赖的共享值类型（见 §3.7.2）
    MGPipeHandles.h                    # MGPipeHandle、MGPipeKind、保留 handle、slot 分配契约
    MGPipeHostSpan.h                   # 唯一一个"形状随传输而变"的访问器（§3.5.7）
    MGPipeCallbacks.h                  # 反向通道（事件/回复）的函数表，见 §6
    MGPipeRenderStateSpans.{h,cpp}     # ★v2 新增：pipeline/dynamic 划分的唯一定义（§3.5.2）
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
    PipeInputs.h                       # backend 私有的"被推送状态"块（迁移载体，§5.2）
    MGPipeImpl_DirectGLES.cpp          # 用 Espryt 的函数填 MGPipeContext
    MGPipeImpl_DirectVulkan.cpp        # 用 Magma 的函数填 MGPipeContext
MobileGL/MG_Remote/                    # 传输与 server 侧对象表；完整目录与 CMake 接线见 §13.8
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
  X(RenderbufferStorage, MGPRbStorage,        kCtxObject,kNone)      /*P0：非 ack*/\
  /* … 共 68 项（P0 实测，非"约 74"），完整目录见 §3.4 与附 A 的速查表 … */
```

| 生成器 | 产物 | 替代/新增 |
|---|---|---|
| **G1** | `struct MGPipeScreen { … };` / `struct MGPipeContext { void (*DrawVbo)(const MGPDrawInfo*, …); … };` | 替代今天手写的 `GLFunctionsTable` |
| **G2** | monolith thunk：`inline void MGP_DrawVbo(const MGPDrawInfo* p){ gPipeCtx.DrawVbo(p); }` | 替代 `gBackendFunctionsTable.GL.*`（~93 个 MG_Impl 站点改名即可） |
| **G3** | wire 记录结构 + 每种一条 `static_assert(sizeof==N)` + applier 分发前的运行期边界检查 → `Fatal{ProtocolCorruption}` | 把 §7.3 的记录格式机制扩展到**全部**调用 |
| **G4** | `MOBILEGL_PIPE_VERIFY` 的逐字段比对器 | **新增**：每份候选设计都被判缺失的语义绊线 |
| **G5** | `PipeInputs::m_filledGen[]` 的位/世代定义 + 读未填字段时的 `Fatal{UnmigratedPipeInput, "<field>"}` | **新增**（v2：由"位图"升级为"**逐 verb 世代**"，见 §5.2.2） |
| **G6** | 477 行读点清单 → MGPipe 调用的映射，CI 重生成并 `git diff --exit-code`，0 UNMAPPED | 改造自 `Feat/CS-Delta-IPC` 的 `extract_backend_read_inventory.py` |
| **G7（v2 新增）** | `RenderStateParameters` 的 pipeline/dynamic chunk 表 + **一个遍历每个 `RenderState` public setter、断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变` 的 `MG_Test`** | **新增**：D-B1 拒绝三 CSO 时点名要求、v1 却没给自己的完整性绊线 |

**G4、G5、G7 与调用目录从同一份 `.def`/同一张 chunk 表生成，因此不可能漂移。**

**接口表用函数指针 struct，不用虚基类。** 三条本仓库自己的理由：(1) 边界今天**就是**函数指针 struct，装在 `MG_Backend/Init.cpp:44` 的唯一 hook 点上；(2) `nullptr` 项**已经**表示"未实现，前端回退"（`BackendObject.h:212-215`、`:265-269`），DirectVulkan 确实留空 8 项——**一个 null `set_*` 恰好就是"这个子系统还没迁移，继续拉取"**，纯虚类只能用说谎的 stub override 来模拟；(3) `MG_Test` 已经会替换这张表做 mock。稀有的 EGL/caps 面继续留在 `pActiveBackendObject` 的虚函数上。

### 3.2 对象模型

#### 3.2.1 Handle

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
- **保留 handle**：`{0,0}` = null；`{slot=0, gen=1, kind=Framebuffer}` = 默认帧缓冲（退役 `DirectGLES.cpp:1917, 2838, 2867, 9675` 四处 `pDefaultFramebufferInfo->defaultFBO` 身份比较）；`ShaderCso` 的高 1/16 slot 段保留给 **program pipeline 合成体**（§4.6）。

#### 3.2.2 两种 generation，严格分开

| | 拥有者 | 回答什么 | 是否过线 |
|---|---|---|---|
| **身份**（`MGPipeHandle::gen`） | client | "还是同一个 GL 对象吗？" | 是 |
| **`MGGen`**（server 纪元） | **server** | "**我自己**是不是重铸了驱动对象 / 冲了自己的缓存？" | **client→server 永不；server→client 只以纹理拉取请求的形式出现**（§6.5） |

**接口规范条款：任何 MGPipe 调用都不得要求 client 提供或知晓 `MGGen`。** 反过来也是规范：**client 侧的版本计数器永远不是新鲜度的唯一证明**——每一个回绕的 `Uint16`（§2.6）在过线时要么加宽到 32 位、要么与 `{slot, gen}` 同行。

#### 3.2.3 CSO vs 可变对象

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

### 3.3 `MGPipeScreen` 与 `MGPipeContext`

| `MGPipeScreen`（share group） | `MGPipeContext` |
|---|---|
| caps、format 能力表、renderer 字符串；buffer / texture / renderbuffer / sampler / shader 的对象命名空间；fence | 全部 `set_*`、全部 CSO 绑定、VAO / FBO / XFB 对象 / query 的命名空间、命令流、present |

v1 只有一个 screen、一个 context、一条 flow。**但两张表从第一天就分开**，因为事后拆分意味着给每个记录种类重新编号。两处必须重新归类的事实：`GetTextureBindGeneration()` 与 `GetSamplingResolutionGeneration()`（`Core.h:130, 136`）是**绑定**（context）事实却住在 share-group 作用域的 `TextureState` 里；`GetTextureContextId()`（`:143`）直接**就是** context handle。

### 3.4 完整调用目录

**（P0 实测修正）落地的 `PipeCalls.def` 是 68 条**唯一调用，不是"约 74"。按 `.def` 的 Class 列分组：**screen 10、ctx-query 6、CSO 13、`set_*`（`kCtxState`）17、object（`kCtxObject`）9、verb（`kCtxVerb`）13**。旧数虚高有三个来源，本节各小标题下逐条标出：(1) `bind_sampler_states` 与 `set_sampler_views` 在 CSO 组与 `set_*` 组**各记了一次**；(2) query 族被并进 screen 一起统计，而 §3.3 已经把 query 命名空间**给了 context**；(3) transfer 标 12，正文与速查表实际只列出 11 条。

**为什么这个算术是承重的**：`PipeCalls.def` 是**唯一真相源**，而**线上 opcode 就是一行在文件里的位置**——所以这份目录必须是**唯一记录的集合**，同一个调用在两个组里各出现一次会让 opcode 编号与目录永久错位（且 G3 的 `static_assert` 抓不到，它只校验单条记录的尺寸）。

**`kCtxState` 为什么是 17**：16 个 `set_*` 加上迁移期临时的 `set_residual_value_state`。**`set_texture_params` 不在其中**——它按资源寻址，Class 是 `kCtxObject`。

#### 3.4.1 `MGPipeScreen`（14 项 → **P0 实测 10 项**）

| 调用 | payload | 取代 |
|---|---|---|
| `get_caps(MGPCaps* out)` | `DynamicBackendParameters`（`BackendObject.h:302-522`，~90 标量，平坦 POD）+ `RendererInfo` + `FormatCapabilityCache`（`:88-99`）+ `callMask` | 40 个 `pActiveBackendObject->` 站点、89 个 caps 读点 |
| `resource_create(h, const MGPResourceDesc*)` | §3.5.1 | buffer/texture/renderbuffer 的创建 |
| `resource_respecify(h, const MGPResourceDesc*)` | 同上 | `BufferBackendOps::Respecify`（`BufferObject.h:80`）泛化 |
| `resource_destroy(h)` | handle | `OnDestroy`（`:101`）+ **两个 `WeakPtr` GC 扫描** |
| `map_persistent(h) → MGPMapResult` / `unmap_persistent(h)` | — | `AcquirePersistentMap`（`:112`）。**改造期不碰**（D-B4） |
| `fence_create/status/wait/destroy` | handle (+timeout) | `FenceSync`…`GetSyncStatus`（`:220-224`）。两值契约（`:243-249`）**逐字保留** |
| `query_create/begin/end/available/result/destroy` | handle + kind | `BackendObject.h:230-256` |
| EGL 生命周期 8 项 | `BackendObject.h:548-559` | 原样保留为虚函数（罕见） |

**（P0 实测修正）本表的 query 族 6 项不属于 screen。** §3.3 已把 query 的命名空间划给 context，落地的 `.def` 因此给它们 `kCtxQuery`，独立成组。screen 组是余下的 10 项：`get_caps`、`resource_create`/`_respecify`/`_destroy`、`map_persistent`/`unmap_persistent`、`fence_create`/`_status`/`_wait`/`_destroy`。EGL 生命周期 8 项留在虚函数上，本来就不在 `.def` 里。

**`callMask` 取代"槽位是否为 null"这个隐式能力探测**（`GL_Query.cpp:471, 545, 768`）。**v2 修订的能力位集**（v1 的五个 emulation 归属位按 D-B7 删除）：
`kCapViewportArray`、`kCapFloat64VertexAttrib`、`kCapResidentSubData`、`kCapCpuXfbPrimitiveAccounting`、`kCapTimerQuery`、`kCapOcclusionQuery`、`kCapXfbPrimitivesQuery`、**`kCapNeedsHostIndexBytes`**（server 侧的 restart 重写/multi-draw 展平需要索引宿主字节 → split 下开启索引宿主镜像，D-B7）、**`kCapNeedsHostUboBytes`**（server 侧要把具名 UBO 打进自己的 ring → 需要 `set_shader_buffers` 的 host payload，D-B8）。
**删除**：`kCapPrimitiveRestart`、`kCapPrimitiveRestartFixedIndex`、`kCapMultiDraw`、`kCapMultiDrawIndirect`、`kCapMultiDrawIndirectCount`——它们表达的"归属开关"不可表达（D-B7）。

#### 3.4.2 `MGPipeContext` — CSO（15 项 → **P0 实测 13 项**）

`create/bind/delete` × { `render_state`, `vertex_elements`, `sampler`, `sampler_view`, `shader` }。payload 见 §3.5.2-3.5.5。

**（P0 实测修正）13 而不是 15**：`create`/`delete` × 5 = 10，`bind` 只有 3（`render_state`、`vertex_elements`、`shader`）。sampler 与 sampler view 的绑定**就是**下一节的 `bind_sampler_states` 与 `set_sampler_views`（它们是带 start/count 的批量绑定，不是单条 CSO bind），在两组各记一次是"约 74"里最大的一处重复计数。

#### 3.4.3 `MGPipeContext` — `set_*`（17 项，v2 从 14 增至 17；**P0 实测 `kCtxState` 亦为 17**）

| 调用 | 取代的拉取点 |
|---|---|
| `set_dynamic_state(MGPBlobRef chunks, Uint16 version)` **（v2 新增）** | 渲染状态里 `m_pipelineStateVersion` 不覆盖的那一半（viewport / scissor / depth range / blend color / line width / polygon offset / stencil ref+write mask / clear values / sample coverage / hints / point-size 族）。**这条让 `glViewport` 不再铸造新 CSO**（D-B1） |
| `set_framebuffer_state` | `GetFramebufferBindingSlot` ×19、`GetAllAttachmentObjects`、`GetDrawBuffers`、`GetReadBuffer`、4 处 `pDefaultFramebufferInfo` |
| `set_vertex_buffers(start, count, const MGPVertexBuffer*)` | VAO binding-point 走查 |
| `set_index_buffer(const MGPIndexBuffer*)` | `GetIndexBufferBindingSlot`；**独立调用**——VAO config version 不是它的超集（D5） |
| `set_indirect_buffers(drawIndirect, parameter)` | `GetBufferBindingSlot(DrawIndirect/Parameter)` |
| `set_sampler_views(start, count, const MGPBoundView*)` **（v2：删掉 stage 形参）** | `GetTextureUnitObject` ×19、`GetActiveTextureUnit` ×8、`GetTextureBindGeneration` ×5。**client 侧已解析**（§4.5） |
| `bind_sampler_states(start, count, const MGPipeHandle*)` **（v2：删掉 stage 形参）** | `TextureUnit.h:394` |
| `set_texture_params(res, const MGPTextureParams*)` **（v2 新增）** | base/max level、swizzle、depth-stencil mode、LOD 钳。**必须独立于 sampler view**，见下 |
| `set_shader_images(start, count, const MGPImageView*)` | `GetImageTextureBinding` ×14；**退役 `ImageUnitFormatsStillMatch`**（`Managers.cpp:6545-6573`） |
| `set_shader_buffers(cls, start, count, const MGPBufferRange*, writableMask)` **（v2：Uniform 类的 range 可带 `MGHostSpan payload`）** | `GetBufferBindingPoint` ×19、`GetTouchedBufferBindingPointCount` ×2。`cls` ∈ {Uniform, ShaderStorage, AtomicCounter}。**payload 由 `kCapNeedsHostUboBytes` 门控**（D-B8） |
| `set_stream_output_targets(count, const MGPBufferRange*, const Uint32* offsets, Uint64 generation)` | XFB 绑定走查 |
| `set_global_constants(shaderCso, MGPBlobRef, Uint32 version)` | `MapUBO`/`GetUBOData`/`GetUBOSize`/`GetUBOContentVersion`（§3.6 D6）。**只覆盖默认 uniform block** |
| `set_vertex_attrib_defaults(Uint32 mask, const MGPAttribValue*)` | `GetCurrentVertexAttribute` ×2；float/int/uint 视图由 `ClassifyVertexAttribType`（`Core.h:51`）在 client 侧解析 |
| `set_pixel_pack_state(const PixelStoreParameters*)` | 6 个 PACK 读点。**没有 unpack 对应项**（§3.6 D5） |
| `set_patch_state(Uint32 vertices, const Float outer[4], const Float inner[2])` | `GetPatchVertices`/`…OuterLevel`/`…InnerLevel` ×6。**同时是 shader variant 输入** |
| `set_draw_program(shaderCso)` / `set_dispatch_program(shaderCso)` | `GetProgramForDraw` ×7、`GetProgramForDispatch` ×3。含 composite（§4.6） |

**为什么删掉 `stage` 形参（v2）**：MobileGL 的纹理单元空间是**合并的**，不是分 stage 的——`TextureState::m_textureUnits` 是 `Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS>` 且 `MAX_TEXTURE_IMAGE_UNITS = 192`（`TextureState.h:41, 128`），每 stage 的 32 只是一个**广告数字**（`:46`）；`TextureUnit` 本身是 `Array<BindingSlot<ITextureObject>, TextureTargetCount>` 加一个 sampler（`TextureUnit.h:20, 24-25`）；两个 backend 都按合并单元绑定（`g_boundTexturesCache[192][TargetCount]`）。同一个合并单元可以被两个 stage 采样。加 stage 维度会逼 client 要么按 stage 复制 view、要么发明一个 GL 未定义的 stage 归属，而 server 还得把它塌回去。**stage 只在目标 API 真正需要时出现（Magma 的描述符 stage flags），由 server 从反射归档推导。**

**为什么纹理参数不能只挂在 sampler view 上（v2）**：Espryt 对**每个 touched 单元绑定**与**每个 draw-FBO attachment 纹理**都调 `SyncTextureParamsToBackend`（`DirectGLES.cpp:1548-1560` 单元表、`:1580-1601` attachment 表），而 `RequireImageBindableStorage` 会置 `m_forceTextureParamsResync`，正是因为通道加宽后的载体需要一个前端 params 版本**不会移动**的 swizzle 覆盖（`Managers.cpp:2815-2821`）。一张**只作 FBO attachment**、**只作 image 单元绑定**、或**只作 `glCopyImageSubData` 端点**的纹理**没有 sampler view**，它的 `glTexParameter` 状态在 v1 的映射里没有载体。所以：**base/max level、swizzle、depth-stencil mode、LOD 钳挂在 `set_texture_params(res, …)` 上；`MGPSamplerView` 只带"视图限制"（min/num level、min/num layer、别名格式）。** 这同时让 `glTextureView` 保持它真正的身份——一个有自己参数、自己能当 FBO attachment、自己能当 `glTexSubImage` 目标的**真纹理对象**（`TextureObjectView.cpp:281, 290`）——而不是被降格成"普通 view CSO"。

**迁移期额外一项（显式临时）**：`set_residual_value_state(MGPBlobRef)`，见 §5.3。

**（P0 实测修正）`kCtxState` 的 17 项这样凑出来**：本表 17 行里 `set_texture_params` 被划成 `kCtxObject`（它按资源寻址，见 §3.4.3 上一段"为什么纹理参数不能只挂在 sampler view 上"——它的载体是 `res`，不是 context），剩 16 个 `set_*`，再加迁移期临时的 `set_residual_value_state` = 17。**巧合的是它与本节旧标题同为 17，但成分不同**，改动这张表时别把两者当同一个数。

#### 3.4.4 `MGPipeContext` — transfer（12 项 → **P0 实测正文只有 11 条**）

`resource_subdata`（buffer + texture 同一形状，**带步长的多 region 描述符**，§3.5.6）、`resource_flush_range(h, Range1D, Flags<BufferMappingAccessBit>)`（携带应用**真实**的 access flags，`BufferObject.h:94-96`）、`resource_readback(h, off, size, MGPReplySlot)`、`resource_copy_region`、`blit`、`clear`（一条，判别式合并今天的 `Clear` + 4 个 `ClearBuffer*` + 4 个 `ClearNamedFramebuffer*`）、`generate_mipmap(h, target, const MGPMipPlan*)`、`read_pixels(const MGPReadbackInfo*, MGPReplySlot)`、`get_texture_image(...)`、`buffer_subdata_resident(h, off, MGPBlobRef)`（**可为 null**）。

**`buffer_subdata_resident` 的 per-backend 可选性必须被接口允许。** Espryt 注册它、Magma 故意不注册（`VkBufferManager.cpp:104-111`），差别是 `glBufferSubData` 在活的 coherent map 上的排序语义（`BufferObject.h:84-92` 的 Minecraft 撕裂 postmortem）。表现为 `kCapResidentSubData` 位 + null 项。

**（P0 实测修正）"transfer"在 `.def` 里不是一个 Class。** 标题的 12 是虚数——附 A 的速查表实际列出 11 条。落地的 `.def` 按**寻址方式**给它们分类：按资源寻址的（`resource_subdata`、`renderbuffer_storage`、`set_texture_params` 等）进 `kCtxObject`（该组共 9 项），按上下文寻址的动词（`blit`、`clear`、`read_pixels` 等）进 `kCtxVerb`（该组共 13 项）。**统计时按 Class 数，不要按本节的功能分组数**，否则又会重复计数。

#### 3.4.5 `MGPipeContext` — 命令（10 项；在 `.def` 里与 transfer 的动词合成 `kCtxVerb` 13 项）

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

#### 3.4.6 显式删除、不移植的项

- `GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`（`BackendObject.h:195-197`）。**（P0 实测修正）`GL_COMPUTE_WORK_GROUP_SIZE` 不是后端答案，别把它放进 `MGPCaps`**：`MG_Impl/GLImpl/Program/GL_Program.cpp:928-946` 用 `ProgramObject::GetComputeLocalSize` 自己回答它，没有链接 compute stage 时抛 `INVALID_OPERATION`——它是一个**程序反射查询**，纯 client。真正属于后端、且确实带下标的只有 **`GL_MAX_COMPUTE_WORK_GROUP_COUNT` / `GL_MAX_COMPUTE_WORK_GROUP_SIZE`**（读点 `GL_Getter.cpp:1160` 与 `MG_Util/ShaderTranspiler/CompileEnv.cpp:134-138`），它们以 **compute 限制**的身份进 `MGPCaps`，与 `DynamicBackendParameters` 的其余标量同列。
- **（P0 实测修正）`GetInteger64i_v` 与 `GetProgramiv` 的退役已在 P0 落地**（提交 "retire the two frontend queries that were never asked"）：两个 `GLFunctionsTable` 表项与两个 backend 的实现均已删除。本文其余处（§8.6-3、§14 P0）把它写成待办的地方，读作**已完成**。
- `ShaderStorageBlockBinding`（`:207-208`）→ 折进 `MGPProgramDesc` 的反射归档。
- **总规则：server 不回答任何 client 能自己回答的问题；剩下的每个 server 查询都是 async-with-handle，绝不阻塞。**

### 3.5 关键 payload

#### 3.5.1 `MGPResourceDesc`（判别式，三种 GL 存储类合一）

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
    Uint8  imageBindableHint;  // client 侧 everImageBound，预防性分配（§6.5(a)）
    Uint8  glNameForDiag[2];   // 仅诊断
    MGPipeHandle viewOf;       // 纹理视图的存储属主（GetViewStorageOwner，TextureObject.h:100）
    MGPipeHandle bufferForTexBuffer;  Uint64 bufOffset, bufSize;   // kWholeBuffer = ~0，实时解析
};
```

`bindMask` 里的 **`ELEMENT_ARRAY` 位是 D-B7 的开关**：server 见到它且 `kCapNeedsHostIndexBytes` 为真时，把该资源纳入索引宿主镜像。

**Renderbuffer 保持独立类**：自己的 format-capability target 索引（`BackendObject.h:85`）、自己的 `ComponentSizes` 上报（`RenderbufferObject.h:37-43`）、自己的 twin（`Managers.h:1838`）。

#### 3.5.2 渲染状态：`MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState`（D-B1 v2）

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

#### 3.5.3 `MGPVertexElements`

携带**两个视图，缺一不可**：解析后的 `VertexAttribute[32]`（`VertexArrayObject.h:17-53`）**和** `VertexBufferBindingPoint`（`:58-64`，初始 stride 是 **16** 不是 0，`:61-62`）。`VertexArrayObject.h:22-29` 记录了合并它们的代价：pointer 调用的 stride 0 被解析成 element size，而 binding-model 的 stride 0 意味着每个顶点读**同一个** element，塌成一个害了 `KHR-GL43.vertex_attrib_binding.basic-input-case7/8`。`IsLong` 与 `Type == Float64` **分开携带**（`:34-39`）。**仅供查询的 `LegacyStride`/`LegacyPointer`（`:51-52`）留在 client。**

#### 3.5.4 `SamplerParameters` 与 `MGPSamplerView` / `MGPTextureParams`

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

#### 3.5.5 `MGPProgramDesc`（`create_shader_state` 的 payload）

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

**v2 前置条件（P0.5）：反射类型必须先搬出 `ProgramObject.h`。** `TypeFacts`（`ProgramObject.h:44`）、`ResourceReflection`（`:76`）、`XfbVarying`（`:1146`）、`LinkArtifacts`（`:1210`）、`SpirvArtifacts`（`:1409`）今天全部声明在 `ProgramObject.h` 里，而该文件 `:11` include `ShaderObject.h`（→ `ShaderCompileTask.h` → glslang；`ShaderObject.h:146` 返回 `SharedPtr<glslang::TShader>`）、`:14` include `SpvcSession.h`（→ `spirv_reflect.h`）。**任何链接真 `ProgramObject` 的 server 就链接了整条编译链，而 server 要反序列化进这些类型就必须 include 被门禁止的头。** P0.5 把它们抽到：

```
MG_State/GLState/ProgramState/ProgramArtifacts.h    # 只 include <Includes.h> 与容器/向量类型
```

更新 7 个 includer（`ProgramFactory.h`、`UniformManager.cpp`、`VulkanRenderer.cpp`、`ProgramInterface.cpp`、`ProgramLinkTask.h`、`ProgramObject.h`、`ProgramTranslationCache.h`），并加 CI 断言：**`ProgramArtifacts.h` 的 `-H` 传递 include 闭包里不得出现 glslang / SPIRV-Cross / spirv_reflect 任何头**。没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。

反射归档**序列化整个结构体**，机制是 `Visit()` + `sizeof` 绊线——一份字段表服务序列化的两个方向，加一条尺寸断言；它在本设计里的**用途是 schema 完整性绊线**（没有第二份状态模型可分歧，所以它不是"分歧预言机"）：

```cpp
template <class Ar> void Visit(Ar& ar, LinkArtifacts& a) { ar(a.writtenUniformLocationBits, /*…全字段…*/); }
static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
              "新字段请加进 Visit() 并 bump MGL_LINKARTIFACTS_SIZE");
```

归档必须覆盖：四个 `ResourceReflection`（各带 `TypeFacts`）、`uniformSamplerOrImageUnitIndex`（`:1298`）、`uniformBlockBinding`（`:1314`）、`shaderStorageBlockBinding`（按名字，`:1325`）、`explicitOpaqueUniformBindings`（`:1303`）、`xfbVaryings`/`xfbStrides`/`xfbPackedStride`/`xfbNeedsScatteredCapture`（`:1357-1394`）、`computeLocalSize`、GS/TCS/TES 事实（`:1373-1388`）、`usesReservedNumSamples`（`:1345`）、`uniformOffsets`（`:1416`）。

**`XfbVarying`（`:1146-1171`）必须带两套拼写**：GL 名字（Espryt 的 ESSL 驱动侧捕获列表）**和** `blockInstanceName`/`blockName`/`blockMemberIndex`/`blockMemberElement`（`:1163-1170`）。

**"server 从源码重新 link"这条路被显式关闭。** 既然链接真 `ProgramObject` 就链接 glslang，`create_shader_state` 的 payload 从第一天就是 **SPIR-V + 反射归档**，没有第二档、没有 `MOBILEGL_IPC_PROGRAM` 这类开关，也不存在 server 侧 compile pool。glslang 全在 client，SPIRV-Cross 全在 server（§4.7）。

#### 3.5.6 `MGPFramebufferState` 与 `MGPSubData`

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

**v2 关键修正：sub-rect 上传不能再靠指针比较判定。** 今天 `Managers.cpp:4278-4283` 用 `uploadData == mipData` 判"上传源就是整 level shadow"，随后 `:4288-4293` 与 `rectShadowPtr`（`:4321-4326`）用 `levelRowBytes`/`levelSliceBytes` 跨步进**整 level**。在 split 下这个前提不成立：client 若发整 level 就毁掉带宽收益并与零副本主张矛盾；若发紧密区域则 `uploadData == mipData` 为假，静默退回整 level 上传；若什么都不发就需要 server 侧整 level 镜像——那就是一份重复的 `MipmapStorage`。
**修正**：`MGPSubRegion` 显式携带源步长，`sourceIsVerbatimLevelShadow` 显式携带原来那个指针比较回答的语义问题（"这批字节是未经转换的 level shadow 吗"）。`Managers.cpp:4274-4326` 相应改为**从描述符**取步长而不是从指针算，`UNPACK_ROW_LENGTH` 从 `srcRowStride/bpp` 设。
**注意树里已经有这个形状**：unpack ring 路径的 `UnpackStagingBlock`（`Managers.cpp:4340-4390`）就是 `{src, rowBytes, rows, slices, srcRowStride, srcSliceStride, offset}`，且注释明说 ring 路径把区域**紧密重打包**、因此完全不发 `glPixelStorei`。所以 split 的自然形态就是"永远走紧密重打包 + 描述符"，与 ring 路径同构。
**这项工作从 v1 的"原地不动"移出，计入子系统 5 的天数**（§5.4），并加一个 Mali 设备门发布 box-vs-rect 作业数与帧时增量。

#### 3.5.7 `MGPDrawInfo` 与 `MGHostSpan`

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

**v2 成本诚实化**：今天的 `DrawArrays(GLenum, GLint, GLsizei)` 是三个寄存器实参（`BackendObject.h:117`）。替换成一个 **56 B**（**P0 实测，不是 ~48 B**）的固定头（含 handle）加按需的变长尾。`minIndex/maxIndex` 今天**只**在 client-memory 数组路径算（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3407-3470`，用于 `:3599`），`xfbCpuCapturedVertices` 今天**只**在 XFB scatter 路径读（`DirectGLES.cpp:900`）——所以两者由 `flags` 门控，**不是每 draw 都算**。`userIndices` 的 32 B `MGHostSpan` **移出固定头进变长尾**，让 VBO 路径（MC/Sodium 的全部 draw）不为它付字节。**每 draw payload 字节数进 P0 的计数器直方图**（`cmd-records` 是逐帧的，这里要逐 draw 的分布，它才是 `SEG_CMD` 的定尺依据）。

**（P0 实测修正）定尺用的实测布局**（P0 骨架编译产物的 `sizeof`，64 位 arm64/x86-64 一致；本表取代此前散落各处的估数）：

| 类型 | 实测字节 | 用途 |
|---|---|---|
| `MGPDrawInfo`（固定头） | **56**（此前写 ~48） | 每 draw；`MGHostSpan` 的 **32 B 只在 `kHasUserIndices` 时**进变长尾 |
| `MGHostSpan` | 32 | 见上；不进固定头 |
| `MGPBindRenderState` | **12** | 每次 CSO 绑定 |
| `RenderStateParameters` | **1168**（此前写 ~1.2KB） | server 侧每 context 一份 working 副本；**不整块过线** |
| `ResidualValueBlock` | **1248** | 迁移期 `set_residual_value_state` 的 payload 上界，`static_assert` 逐阶段下调至 0（§5.3、P13） |
| `DynamicBackendParameters` | **328** | `MGPCaps` 的主体 |
| `MGPCaps` | **384** | 握手后一次 |
| `MGPipeScreen` | **80** | 函数指针表（进程内，不过线） |
| `MGPipeContext` | **464** | 同上 |
| `PixelStoreParameters` | **28** | `set_pixel_pack_state` 的整块 payload |

**两条直接后果**：(a) `SEG_CMD` 的定尺按 **56 B 头**算，不是 48——MC 帧 1000-4000 draw 时这是每帧 8-32 KiB 的差额；(b) `ResidualValueBlock` 的 1248 B 是**迁移期每 draw 最坏情况**的额外 payload（`RenderStateParameters` 1168 占了绝大部分），这解释了为什么它的退役绊线要按阶段下调而不是一次性删除。

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

**v2 修订的消费者表**（与 §4.8 一致，解决 v1 §3.5.7 与 §4.8 互相矛盾的问题）：

| 消费者 | 今天的站点 | 归属 | monolith 填法 | split 填法 |
|---|---|---|---|---|
| client 顶点数组 | `Managers.cpp:2500-2592`、`VulkanRenderer.cpp:3737` | **client 供字节** | `ptr = attrib.Offset` | tracker 暂存同样范围进 `SEG_STAGE` |
| client 索引数组 | `DirectGLES.cpp:4425-4442`、`VulkanRenderer.cpp:3418-3433` | **client 供字节** | `ptr = indices` | 暂存 `count*indexSize` |
| indirect / parameter 命令块 | `DirectGLES.cpp:4655-4695`、`:4768-4793`、`VulkanRenderer.cpp:12045` | **client 解析计数** | `ptr` 指向 shadow | tracker **解析出计数**并发解析后的 `MGPDrawRange[]`（几十字节） |
| **restart 重写 / multi-draw 展平的索引字节** | `DirectGLES.cpp:4412-4415`、`MultiDraw.cpp:498-540`、`VulkanRenderer.cpp:4159` | **server 拥有变换**（D-B7） | `ptr` 指向前端 shadow | `seg = kFromServerIndexMirror`：**server 从自己的索引宿主镜像取**，零线上流量；镜像超预算时退化为 client 逐 draw 暂存并计数 |

**monolith 代价**：一次可预测分支 + 变长尾里的 32 B（仅 `kHasUserIndices` 时）。它顺带消灭"backend 在 draw 中途回头调前端 reconcile"的大部分：20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 里，凡消费者搬到 client 的那些改由 **tracker 在填 span 之前**做同一次 reconcile（**逐站点对照见 §4.8.1，不是一条笼统规则**）。

### 3.6 与 gallium 的对应与偏离（十条，逐条记名）

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
| **D8** | `pipe_context::flush` + fence 是唯一反向通道 | **`MGPipeCallbacks`**：10 个具名回复/事件（§6） | gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求/终止、default-FB 几何这些词汇 |
| **D9** | `set_viewport_states(start_slot, num)` | **float 数组 + 独立的 `writtenMask`** | viewport 是 **float**（`RenderState.h:229-237`：`KHR-GL43.viewport_array.viewport_api` 用 `==` 无容差）；scissor 必须单独带 `ScissorBoxWrittenMask`（`:363`），因为 `glScissor(0,0,0,0)` 是合法 GL、意思是"拒绝每个片元"（`:352-362`） |
| **D10（v2 新增）** | 纹理参数（swizzle / base-max level / dsMode）住在 `pipe_sampler_view` 里 | **`set_texture_params(res, …)` 独立，`MGPSamplerView` 只带视图限制** | 一张只作 FBO attachment / image 单元 / CopyImage 端点的纹理没有 sampler view，但 Espryt 对 attachment 也调 `SyncTextureParamsToBackend`（`DirectGLES.cpp:1580-1601`），且 `RequireImageBindableStorage` 要在前端 params 版本不动的情况下强制重同步（`Managers.cpp:2815-2821`） |

**没有 `pipe_transfer`、没有 `set_pixel_unpack_state`、没有压缩格式概念、renderbuffer 不折进纹理、`set_sampler_views` 没有 stage 维度。**

### 3.7 覆盖论证

#### 3.7.1 对 477 读点分类的逐类映射

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
| Texture state（dirty level/rect） | 7 | `resource_subdata`（带步长描述符） | **归属反转**（§6.3） |
| PixelStoreBlob | 6 | `set_pixel_pack_state` | unpack **删除** |
| client-resolved（error queue） | 6 | `on_gl_error` 回调（§6） | — |
| ProgramPublish | 3 | `create_shader_state` | 依赖 P0.5 |
| client-resolved（validation） | 3 | client 自答 | — |
| CurrentAttrib | 2 | `set_vertex_attrib_defaults` | — |
| client-resolved（compile env） | 2 | `on_caps_invalidated` | — |
| Patch 参数 | — | `set_patch_state` | 同时是 variant 输入 |
| 条件渲染 | — | **client 解析，永不过线** | `Core.h:387-391` |
| XFB CPU 计数 | — | **纯 client**；`MGPDrawInfo::xfbCpuCapturedVertices`（flag 门控） | — |
| backend 重铸纪元 | — | **无 client 对应物**：`MGGen`，server 私有 | — |

那 1997 个前端 getter 站点不是第二个面：89 个纯版本读**根本不过线**，72 个数据字节读全部落在 §4.7/§4.8 与 `MGHostSpan`，38 个 `GetLifetimeId()` 变成 handle。

#### 3.7.2 覆盖论证不是这张表，是这三道门（v2：从两道增至三道）

上表是**声明**。证明是机械的：

**门 A —— include 图门（v2 新增，取代 v1 单靠 `nm` 的那半）。**
v1 说 `MG_Backend` 只允许 include "一张共享**值**头白名单（`RenderState.h` 的 `RenderStateParameters`、`SamplerObject.h` 的 `SamplerParameters`、…）"。**实测这张白名单不是叶子集**：`RenderState.h:12` include `FramebufferState/FramebufferObject.h`，后者 `:12-13` 再 include `TextureState/TextureObject.h` 与 `RenderbufferState/RenderbufferObject.h`；依赖是结构性的——`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 给两个数组定长（`RenderState.h:263, 273`）。所以"把 `RenderStateParameters` 交给纯净的 `MG_Backend`"会把整张 framebuffer/texture/renderbuffer 类图一起拖进来。**而 `nm --undefined-only` 看不见这个**：只 include 而不调用其成员函数的类不产生未定义符号，门可以在 include 图完全耦合的情况下为绿。
**修正**：P0.5 交付 `MG_Pipe/MGPipeValueTypes.h`——把 `MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute` 与相关枚举搬进去，**它不 include `MG_State/GLState` 的任何东西**；`RenderState.h`/`SamplerObject.h`/`VertexArrayObject.h` 反过来 include 它。门变成：

> **在 disaggregated 配置下编译 `MG_Backend` 时，把 `MG_State/GLState` 从 include 搜索路径里移除**（或对 `-H` 输出断言）。这是唯一一条能因它存在的理由变红的检查。

**门 B —— 符号门。** `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空。保留，作为门 A 的补充（它能抓到通过前置声明+跨 TU 调用绕过 include 图的情况）。

**门 C —— 未声明门。** 在 `MOBILEGL_PIPE_PUSH=all` **且非 verify** 构建里，`MG_State::pGLContext` **未声明**。任何接口没满足的读是一次**指名文件与行号的编译错误**。strangler 结束时 `grep -c 'pGLContext' MG_Backend/` == 0（**grep `pGLContext` 不是 `pGLContext->`**，因为还有 58 行非箭头用法）。**这条门只跑非 verify 构建**（D-B5：verify 构建保留 `SnapshotFromGLContext()`）。

**这三道门比生成一张 477 行的清单严格得多：它们禁止那次读，而不是给它编目，而且不会过期。** 那份 inventory 保留为 tracker 侧覆盖检查表（G6，CI `git diff --exit-code`，0 UNMAPPED）。

#### 3.7.3 21 条 D 类身份 memo 的重键表

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

## 4. 前端 state tracker

### 4.1 推送发生在哪里——本设计里最容易做错的一个决定

**不在 GL setter 里。** `glEnable(GL_BLEND)` 绝不调 `bind_render_state`。Blaze3D 每个 batch 都用它包住，代码自己标注它是最热的路径（`DirectGLES.cpp:2029-2032`）。天真的 per-setter 推送把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表——**严格慢于今天**。

**在 verb 之前的 validate 时刻。**

```cpp
// MG_Impl/Pipe/Tracker.h
class MGPipeTracker {
public:
    // 每一类 verb 一个入口；由 PipeCalls.def 的 kCtxVerb / kCtxObject 条目生成（§5.2.1）
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

#### 4.1.1 哪些操作在 GL 调用时刻推送（v2 修正推论 1）

**规则的正确措辞**：

> **只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook（`BufferObject.h:70-71` 自己写着"在 GL 调用时刻分发，就在 shadow 拷贝刚更新之后"）。**纹理 subdata 不在此列。**

理由：`glTexSubImage*` **根本不调 backend 表**（`GL_Texture.cpp` 只有 3 处 `MarkStorageDirtyRegion`），全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做，那里才跑 96-rect 级联合并与 union-box 回退，并在 unpack ring 可用时刻意塌成一个 box（`Managers.cpp:4386-4390`，实测 +6 ms/frame）。逐 `glTexSubImage` 发一条 `resource_subdata` 精确复现那个 ~100 作业的形状。

**因此纹理路径的形态是**：client 在自己的 `MipmapStorage` rect 模型里累积（§6.3 的发射游标），在**下一个 validate / flush 点**把合并后的形状作为**一条** `resource_subdata`（带 union box + region 列表）发出。`MOBILEGL_PIPE_STATS` 必须把逐帧 `resource_subdata` 发射次数单列一类，并在 MC 动画图集 fixture 上设上限。

**稳态成本**：见 §13.2（v2 已按动态口径重写）。

### 4.2 dirty bits：值类零新增记账，对象类新增 5 个聚合世代（推论 4）

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

**完整性由 `gen_pipe_dirty_surface.py` 保证**（推论 4）：它枚举 `MG_Impl/GLImpl/**` 里每一个会改变某组的 mutator，映射到必须 bump 的聚合世代，CI 重生成 + `git diff --exit-code`，**未映射的 mutator 直接失败**。这是 B-R6 的第四层。

**（P0 实测修正）这个面到底有多大——已用 `gen_pipe_dirty_surface.py` 量过。** `MG_Impl/GLImpl` 下共 **926 次 `pGLContext` mutator 调用**，但它们只落在 **73 个不同的 mutator** 上。其中 **92 次（7 个不同 mutator，绝大多数是 `RecordError`）位于同时会走到 backend 的函数里**——只有这批需要"在同一个 GL 入口内既改状态又已经发过消息"的顺序推敲；**其余 834 次由紧随其后的 verb 发布**，不需要各自的即时推送。
**结论：P1/P2 的 dirty-surface 映射是一个 73 条目的问题，不是 926 条目的问题**，映射表的规模因此可控（每条目一行"mutator → 必须 bump 的聚合世代"），而 CI 门的成本也是按 73 条计。**调用点数仍要监控**（新增调用点若落在未映射的 mutator 上必须失败），但它不是工作量口径。

**三个回绕的 `Uint16` 在 tracker 边界加宽。** `m_lastPushed[]` 是 tracker 自己的字段，加宽到 `Uint32`/`Uint64` **不需要改 `MG_State` 一行**；同时 handle 与它同行过线。**回绕在 tracker 本地是无害的**（一次回绕造成一次多余的重推，永不漏推），何况集合 hash 抑制器会把多余重推吞掉。

### 4.3 每命令 validate 的**不变式**（v2：从"固定顺序契约"降级）

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

### 4.4 合并：保留代码库已经发现的三条，加上第四条

1. **整块结构优于逐字段。** Magma 的 `ComputePipelineStateHash`（`VulkanRenderer.cpp:4818-4826`）已经把 ~17 次 accessor 调用换成一次 bulk fetch；Espryt 的三段 memcmp 同理。
2. **高水位标记。** `BufferState::TouchBindPoint` / `GetTouchedBindPointCount`（`BufferState.h:51-62`，每 target 84 个绑定点）与 `TextureState::NoteUnitTouched` / `GetMaxTouchedUnit`（`Core.h:124-126`，192 个单元）**必须留在 tracker 的走查里**，它们直接就是 `set_shader_buffers` / `set_sampler_views` 的 `count` 实参。
3. **只发 program 解析过的集合**，用 `LinkArtifacts::uniformSamplerOrImageUnitIndex`（`ProgramObject.h:1298`）。两个 backend 今天已经在算（`ResolveAndBindUnitTextures`，`DirectGLES.cpp:2973`；`UniformManager::CollectSampledTextures`）。
4. **（v2 新增）集合 hash 抑制器。** 每一条 `kVarTail` 的 `set_*` 在 client 侧算一次已解析集合的 xxHash，与 `m_lastSetHash[]` 比较，**未变就不发**。这是 §2.5 里那 ~175 行去抖搬到 client 后的载体，也是 D9 的前提——没有它，`GetTextureBindGeneration()` 在冗余重绑时的 bump（`DirectGLES.cpp:1414-1420`，26.2 每次纹理单元切换都重绑同一个 sampler）会让每个 batch 重发一条几百字节的变长记录并冲掉 server 的两个 memo。

**索引绑定的范围必须在 validate 时刻实时解析，不是在 bind 时刻快照。** `BindingSlotRange1D::GetRange()` 对整 buffer 绑定返回 `Range1D(0, object->GetSize())`，因为 `glBindBufferBase` 之后再 `glBufferData` 是普通应用代码。

### 4.5 sampler view 在 client 侧解析

GL 是**每个 unit 每个 target 各一个绑定**（`TextureUnit.h:20, 24-25`；`TextureState::m_textureUnits` 是 `Array<TextureUnit, 192>` **按值**存放，`TextureState.h:128`，每 stage 广告上限 32，`:46`），shader 看见哪一个取决于 sampler uniform 的声明类型、mipmap 完备性（`IsMipmapCompleteForFilter`，`TextureObject.h:309`；`SamplesAsIncompleteTexture`，`:315`）和 `IsUndefinedDefaultTexture`（`:329-332`）。**gallium 的"每槽一个 view"就是解析后的形态。**

**解析留在 client**，并且 client 必须为它保留一个自己的 memo（§2.5 的 ~40 行搬迁项），否则每 draw 重跑完备性规则。**合并单元空间，无 stage 维度**（§3.4.3）。

**两处 backend 特定的后处理留在 server**，作用在已解析的集合上：Espryt 的 raw-depth-fetch sampler 替换（`DirectGLES.cpp:3540-3546`）与 Magma 的 feedback-loop 检测（对着 draw FBO，`UniformManager.cpp:554`）。两者都可从已推送的 `set_framebuffer_state` + view 集合判定。

### 4.6 对象生命周期、共享组与 composite pipeline program

#### 4.6.1 生命周期

`resource_create` 在**前端对象构造**时发，存储由 `resource_respecify` 惰性定义。`resource_destroy` 在前端对象析构时发。三条顺序约束：

- **view 先于其存储属主销毁**：`GetViewStorageOwner()`（`TextureObject.h:96-100`）→ `MGPResourceDesc::viewOf` + server 侧 keep-alive。
- **FBO attachment 钉住纹理**（`FramebufferObject.h:95`）→ `set_framebuffer_state` 的 surface handle 隐含 server keep-alive。
- **buffer texture 钉住 buffer，范围实时解析**（`TextureObjectBuffer.h:28, 35-46`）→ `MGPResourceDesc::{bufferForTexBuffer, bufOffset, bufSize}`。

#### 4.6.2 共享组

v1：一个 screen、一个 context、一个扁平 handle 空间、一条 flow。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射——**顺手修今天不取该锁的两个入口**：`ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）。

#### 4.6.3 composite pipeline program：判过死刑的那个反对意见，答案是"什么都不用做"

`GLContext::GetProgramForDraw()`（`Core.cpp:592`）**今天就已经完全在前端**完成合成：join 每个 stage 的 `JoinLinkAndSpirv()`、按 `ComputeDrawProgramSignature()`（`:630`）查 cache、miss 时构造**故意不命名**的 `MakeShared<ProgramObject>(0u)`（`:644`）、挂上每个 stage 被钉住的 linked snapshot、重装捕获 stage 的 XFB varyings、`Link(true)`、缓存、`RefreshCompositeUniforms`。

tracker 调它，拿到 `SharedPtr<ProgramObject>`，推**一个 handle**。合成体没有 GL name，但**有 lifetimeId**，slot 从 `ShaderCso` 的保留高位段分配。生命周期：pipeline cache 淘汰该条目时释放 slot、`gen++`、发 `delete_shader_state`——`CompositeResolver.cpp` 里三行。

**合成体从不过线、从不被重新实现，server 侧不需要任何"解析后的 draw program"钩子。** 副带收益：阻塞的 `JoinLinkAndSpirv()` 彻底离开 server 的 draw path。

### 4.7 program artifacts 与全局 UBO scratch

**`create_shader_state` 的 payload 是 SPIR-V + 全结构体反射归档**（§3.5.5），不是源码。**依赖 P0.5 的头文件抽取。**

**SPIRV-Cross 留在 server**（`TranspileSpirvToEssl`，`Managers.cpp:6575`）：它消费 SPIR-V 加设备事实。**glslang 留在 client。** 这是一次文件级切割。

**全局 UBO scratch 走独立入口**（D6）：`set_global_constants(shaderCso, MGPBlobRef bytes, Uint32 version)`，键 `(shaderCso.slot, uboContentVersion)`，复现 `DirectGLES.cpp:3369-3392` 的"每 program 每帧至多一次"。它小、每次 `glUniform*` 变、有版本，字节走 `SEG_STAGE`。

**具名 UBO 字节走 `set_shader_buffers` 的 host payload**（D-B8）：`UniformManager::ResolveUniformBufferPayload` 在 `UniformManager.cpp:2022` 调 `SyncPersistentMappedRange()`、`:2052` 读 `MappedData() + rangeStart` 打进 **Magma 自己的 UBO ring**——消费者在 server，搬不走。由 `kCapNeedsHostUboBytes` 门控（Espryt 直接绑给驱动，不需要）。**逐帧字节量进 `stage-ubo-named` 计数器；在 P0 给出数字之前不冻结这个 payload 的形状。**

**backend 侧 program link/compile 失败不需要任何同步返回，也不需要新事件种类。** 实测：`SyncToBackend` 在 `Managers.cpp:8091` link、`:8094` 读 `GL_LINK_STATUS`、`:8095` 折进 `m_backendProgramUsable`、`:8097-8101` 取驱动日志、`:8106` 发 `MGLOG_E`；`Use()` 随后绑 program 0（`:8357`）并 `MGLOG_E_ONCE`（`:8364-8372`）。**没有 GL error、没有 `ProgramObject` 变更、`GL_LINK_STATUS` 永不撤回**（`:7098`、`:7247-7249`、`:6478`、`:7827`）。同步查询由 client 从 `ProgramObject` 回答（`GL_Program.cpp:851` → `ProgramObject.h:913`）。所以 `on_log` 逐字复现它——**但由此推出一条对事件通道的强制修正，见 §6.4**。

### 4.8 emulation 所需前端数据的显式传递（v2 按 D-B7 重写）

归属规则：**驱动表达不了的变换在 state tracker 里 lowering，硬件/驱动强加的变换在 driver 里 lowering**。**v1 用 cap 位门控 emulation 归属的做法对 restart 与 multi-draw 不可表达（D-B7），此处收回。**

| emulation | 归属 | 门 | 过线的是什么 |
|---|---|---|---|
| **client 顶点数组**（`Managers.cpp:2500-2592` 把 `attrib.Offset` 当应用裸指针，每 draw 每属性上传 `(first+count-1)*stride+elementSize`；`VulkanRenderer.cpp:3737` 是**唯一无界**的应用指针读） | **client**（它拥有地址空间） | — | **字节，永不是指针**（`MGHostSpan`） |
| **索引扫描**（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3407-3470`，用于 `:3599` 给上一条定界） | **client**（只有它同时持有两个数组） | — | `MGPDrawInfo::minIndex/maxIndex`（`kHasIndexRange` 门控），`~0` = 未知 |
| **client 索引数组** | client | — | `MGPDrawInfo::userIndices`（`kHasUserIndices` 门控） |
| **primitive-restart 重写**（`DirectGLES.cpp:4368-4470` 整 EBO 重写，`kMaxRestartRewriteBytes = 1<<26` = 64 MiB，`:4218`；`VulkanRenderer.cpp:4159-4161`） | **server（v2 改：v1 曾说 client）** | `kCapNeedsHostIndexBytes` → 索引宿主镜像 | **零线上流量**：server 从镜像读。**monolith 行为零变化**，诊断仍落在原线程（开放问题 12 关闭） |
| **multi-draw 分档 + 展平**（`MultiDraw.cpp:282-320` 的 `ResolveTierForBatch` **逐 batch** 在五档里选，输入含 `programReadsDrawID`——**转译出的 ESSL 的性质，只存在于 server**；容量判定 `kMaxFlattenedIndices` `:72` / `kMaxComputeFlattenedIndices` `:82`；自动阶梯 Ext→BaseVertex→MultiIndirect→Indirect→DrawElements `:241-243`，CPU 展平是**回退**） | **server，全部五档**（v2 改） | `kCapNeedsHostIndexBytes` | `draw_vbo(info, indirect, MGPDrawRange[], numDraws)`；索引字节走镜像 |
| **`*IndirectCount` CPU 回退**（`DirectGLES.cpp:4655-4695` 从 `parameterBuffer->MappedData()` 读实际 draw 数） | **client** | — | client 从自己的 shadow 解析计数，发解析后的 `MGPDrawRange[]`（几十字节）。**注意它今天只调 `SyncPersistentMappedRange()`，不调 `SyncGpuWrites()`**（§4.8.1） |
| **viewport-array N 遍回放**（`DirectGLES.cpp:3742-3846`，今天包住 14 个 draw 入口） | **server** | `kCapViewportArray` | 无新增：16 组 viewport/scissor/depth-range 已在渲染状态里 |
| **fp64 顶点窄化**（`Managers.cpp:2518-2557`） | **server**（后端格式决策） | `kCapFloat64VertexAttrib`（`BackendObject.h:487-500` 明说它与 `SupportsShaderFloat64` **独立**） | 原始字节；`IsLong` 与 `Type` 分开过线 |
| **image-bindable 存储加宽/拆分**（`Managers.cpp:2789-2822`、`:4620-4630`） | **server** | — | 正向 `imageBindableHint`；反向 `on_texture_pull_request` + 终止符（§6.5） |
| **生成 mipmap 的前端存储** | **拆开**：client 分配 level 存储，server 生成 | — | `MGPMipPlan`；`on_mip_levels_generated` **只带形状不带字节**（见 §12.1 的说明）；CPU 回退路径的纹素由 `on_texture_writeback` 回来 |
| **CopyImage shadow 镜像**（`DirectGLES.cpp:7065-7140`） | **client** | — | 只回"拷贝成功"。**删掉一整条 server→client 字节通道** |
| **XFB CPU 图元计数**（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`） | **纯 client** | `kCapCpuXfbPrimitiveAccounting` | `MGPDrawInfo::xfbCpuCapturedVertices`（flag 门控）+ `end_stream_output` 的 `MGPXfbAccounting` |
| **XFB scatter 的 read-modify-write**（`DirectGLES.cpp:893-960`） | **client（v2 新增行）** | — | 见 §6.2.1 的 `on_buffer_writeback` 修正 |
| **压缩纹理 / pixel unpack 规整** | **纯 client** | — | 无 |

#### 4.8.1 陈旧索引纪律——**逐站点**表，不是一条笼统规则（v2 修正）

v1 写"上表里每一次 client 侧扫描/重写，在 monolith 里都紧跟在 `SyncPersistentMappedRange()` + `SyncGpuWrites()` 之后"。**对 `*IndirectCount` 不成立**：`DirectGLES.cpp:4666-4667` **只**调两次 `SyncPersistentMappedRange()`，然后在 `:4690-4694` 直接读 `MappedData()`；**没有 `SyncGpuWrites()`，因此今天没有停等**。而 `SyncGpuWrites` 才是触发 `ReadbackFromGpu`（`BufferObject.cpp:265-274`）的那一条。照 v1 的笼统规则实施，`glMultiDrawElementsIndirectCount` 会平白获得一次 publish-and-wait round trip——而 trace 语料里恰好有 `minecraft-1.21.1-neoforge-create-indirect-in-world`（Create/Flywheel，indirect 与 parameter buffer 每帧被写），于是这会变成一个**逐帧逐 batch 的同步 round trip**，而 §12.2 第 10 行还把它写成"常见情况代价为零"。

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

## 5. 后端状态机改造

### 5.1 什么原样不动（先说这个，因为它是"最短可信改造"的依据）

**每一个 ring、pool、arena、quirk、lowering pass 原地不动：**

Espryt：三条 persistent-mapped ring、`PersistentRing` 的分配/背压算法、buffer pool、全部 7 条 fallback-repack 路径（`Managers.cpp:3209-3527`）、`m_backendColorSlots` draw-buffer 置换表、三个 scratch FBO 及其驱动侧 attachment 影子、`PackState`、全部驱动绑定影子、Adreno 的"禁用属性无指针 SIGSEGV" workaround（`Managers.cpp:2371-2380, 2427-2433`）、Mali 的 XFB 捕获丢失 workaround（`DirectGLES.cpp:400-410`）、`ScopedDefaultUnpackState`、SPIRV-Cross 会话与 6 次 post-emission ESSL 重写、驱动 POST 自检族、**restart 重写与 multi-draw 五档**（D-B7）。

Magma：`VulkanRenderer` 全部 memo 与 scratch、`PipelineFactory`、`ProgramFactory`、`UniformManager` 的 ring 与描述符集、五个 `Vk*Manager`、`FrameContext`、`SwapchainObject`、`DynamicStateShadow`、`VertexInputStateFactory` 的 cache **本体**、**以及 D18 的节点式容器纪律**。

**v2 从"原样不动"里移出的一项**：`Managers.cpp:4274-4326` 的 sub-rect 上传判定与跨步计算——它今天靠 `uploadData == mipData` 指针比较与整 level 步长算术，split 下不成立（§3.5.6），必须改成从 `MGPSubRegion` 描述符取步长。**这不是 v1 说的"只把输入从拉取的 shadow 指针换成 `MGPBlobRef`"，是真代码改动，计入子系统 5。**

**唯一两处必须真改的 `MG_State` 类型内部用法**：

1. **Magma 的占位纹理**（`UniformManager.cpp:161-181, 1416-1500, 1624-1634`）：构造真的 `TextureObject2D` / `TextureObject2DMultisample` / `TextureObject2DMultisampleArray`，走 `SetInternalFormat(RGBA8)` / `AllocateStorage({1,1,1},4)` / `UpdateMipmapSubData` / `MarkStorageDirty` / `SetSamples(2)`（VUID-RuntimeSpirv-samples-08726）/ `TruncateMipmapLevels(1)`，**唯一理由**是让"未绑定单元"复用 `SyncTextureAndGetDescriptor(ITextureObject&)` 这个签名。改成 backend 自己分配 `VkImage` + view + descriptor：**~120 行前端对象木偶戏变成 ~60 行直白的 VMA/Vulkan，34 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个随之消失。**
2. **Magma 的两个内部 shader**（`InitializeBlitResources` `VulkanRenderer.cpp:4210-4283`、`InitializeDepthMipmapResources` `:4287-4356`）：**烘焙成 SPIR-V。** 方式：把生成的 SPIR-V、uniform location、UBO 布局作为生成头文件签进树，用一个 `MG_Test` 重跑树内 glslang 对同一批源码字符串并逐字节比对守新鲜度。不用构建期 host glslang target。`uSource` 的描述符绑定本来就由 `ProgramFactory` 自己的 SPIRV-Reflect 走查找到（`:4340-4350`），原样存活。**顺带把一次 glslang 编译从 monolith 启动路径上删掉。**

Espryt 有一个小号同类：`g_rawDepthFetchSamplerState`（`DirectGLES.cpp:166-179`）→ backend 原生 sampler 记录，~40 行。

### 5.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充器 + poison 世代

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

#### 5.2.1 三个阶段，其中阶段 A 可证明是**近乎** no-op

| 阶段 | 改什么 | 怎么证明 |
|---|---|---|
| **A — 别名** | 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加手工转换 58 行非箭头用法**（§2.4）。**逐 verb 类填充点**（见下）填 `gPipeInputs`。backend 函数体其余部分不变 | `nm --defined-only` 不变；`.text` size **在可逐行归因的范围内**（**不是**完全相等，见下） |
| **B — 推送** | tracker 填 `gPipeInputs`；填充器仍在，按 `MOBILEGL_PIPE_PUSH` 位图逐字段让位 | **`MOBILEGL_PIPE_VERIFY=1`**（§13.3-②）：tracker 再填一份快照版，G4 生成的比对器**逐字段**每 draw 比一次 |
| **C — handle 化** | `SharedPtr<FrontendObject>` 字段 → `MGPipeHandle` + POD 描述符；memo 重键；写回变回调 | 全套门（§13.3）。**注意 A/B 口径在此收窄，见 §5.7** |

**v2 修正 1：填充点必须逐 verb 类，不能只有两处。**
v1 只在 `PrepareForDraw`（`DirectGLES.cpp:2916`）与 `SetupDraw`（`VulkanRenderer.cpp:6371`）顶端填快照。但 `MG_Impl` 用到的 70 个表项里有 ~48 个不是 draw/dispatch，其中多个自己就读 `pGLContext`（`UpdateTextureBindingAtTarget` `:6051-6052`、`PackStateFromContext` `:6129`、`Clear` `:4106/:4165`、`BlitFramebuffer` `:5988-5989`、`GetTexImage` `:9254-9257`、DSA by-name `:4038-4043`、`:7417-7418`），而代码自己说明了这一点（`:1501-1502`："for every non-draw call site (Clear, readbacks)"）。
**做法**：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些 `PipeInputs` 字段"的表，并在 `MG_Impl` 的 ~93 个边界站点上生成对应的 validate/fill 调用。这同时把 poison 从"某个 draw 上炸"升级为"在**需要它的那个 verb** 上炸"。

**v2 修正 2：poison 从"位图"升级为"逐 verb 世代"。**
一个只被上一个 draw 填过的字段，在紧随其后的 `glTexSubImage`/`glReadPixels` 里读到的是**陈旧值**，位图版的 poison 看不见（位已置）。世代版：每次 verb 递增 `m_currentVerbSerial`，字段被填时记下当时的序号，读取时断言 `m_filledGen[f] == m_currentVerbSerial`（对"跨 verb 有效"的字段单独标注为 sticky 并在生成表里显式列出）。**这才让"一个字段在某个 verb 上没被推送"必然是一次 Fatal 而不是一次静默陈旧。**

#### 5.2.2 poison 世代是完整性的运行期绊线

在 debug 与 disaggregated 构建里，读一个当前 verb 未填的非 sticky 字段是 **`Fatal{UnmigratedPipeInput, "GetStencilState@DrawVbo"}`**——响亮、精确、不可能渲染过去。P13 之后（`SnapshotFromGLContext()` 只在 verify 构建里）完整性变成**构建期事实**：一个从未被写入的字段就是一个编译器能标出来的字段。

### 5.3 Track V / Track H 与残余值块

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

### 5.4 DirectGLES（Espryt）逐子系统

`PrepareForDraw` 的阶段顺序（`DirectGLES.cpp:2916-2975`）：`GetBoundVertexArray` → `ResolveVaoTwin` → `GetProgramForDraw`（**join 编译池**）→ `CaptureDrawTextureSyncKeys` → `SyncNeccessaryBuffers` → `SyncCurrentVAO` → `SyncNeccessaryTextures` → `SyncImageTextureBindingsForDraw` → `MarkWritableImageBufferTexturesGpuWritten`（**改前端**）→ `SyncCurrentFBO` → `SyncCurrentProgram` → `SyncRenderState` → `BindCurrentFBO` → VAO bind → `SyncCurrentVertexAttributeValues` → `BindCurrentTextures` → `BindCurrentProgramWithResources` → `StartPendingTransformFeedback`。

| # | 子系统 | 消除读点 | memo | 写回 | 轨 | 天 | 风险 |
|---|---|---|---|---|---|---|---|
| 0a | `GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 移回 `MG_Impl` | 14 | 0 | 0 | — | 1-2 | 极低（严格 no-op） |
| 0b | handle 基建；6 个 registry → slot 数组；删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / 2 个 GC 扫描 | — | 9 删 | — | — | 5-7 | 低 |
| 1 | **渲染状态**（`DirectGLES.cpp:1962-2654`，693 行） | **4**（`:2007, 2021, 2050, 2133`） | 0 | 0 | V | **3-5** | **低**：693 行函数体、单 `Uint16` 早退、三段 memcmp 全不动 |
| 2 | buffer + 7 个 `BufferBackendOps` | 19 | 3 | 6（+23 处 re-entry 删除） | H | 10-13 | **高**（不碰 `AcquirePersistentMap`） |
| 3 | VAO / vertex elements | 2（+~10 getter） | 4 | **0**（Espryt 不往前端对象写 memo） | H | 7-9 | 中 |
| 4 | framebuffer / renderbuffer | 8 + 4 处 `pDefaultFramebufferInfo` | 4 | 1 | H | 7-9 | 中高 |
| 5 | 纹理 / sampler / image unit / **`set_texture_params`** / **subdata 描述符改造** | 18（+~35 getter） | 8（5 删） | 21 | H | **23-30**（v1 为 20-26，+3-4 为 §3.5.6 的跨步描述符改造） | **高** |
| 6 | program + constant buffer | 16（+~30 getter） | 5 | 0 | H | 14-18 | **高** |
| 7 | XFB（含 **scatter 搬到 client**，§6.2.1） | 3 | 1 | 2 | H | 5-7 | 中 |
| 8 | emulation + `MGHostSpan` + **索引宿主镜像的 server 侧接口** | ~12 | 0 | 3 | — | 8-11 | 中 |
| 9 | 回读 / pack state | ~10 | 1 | 7 | V+H | 5-7 | 中 |
| 10 | 删 pull 路径 + `MGB_CTX` | — | — | — | — | 4-6 | 低 |
| | **合计** | **124** | ~32 | 28 | | **92-124** | |

**子系统 5 是全表最危险的一处**：它同时压着实测 +6ms/frame 的 box-vs-rects 悬崖（`Managers.cpp:4386-4390`）、7 条 fallback-repack 路径、以及 v2 新增的跨步描述符改造。缓解：`resource_subdata` 同时携带 box 与 region 列表且 **server 选形状**；repack 族本体不动；**子系统 5 拆成两个可独立落地的半**（先 sampler view + sampler + `set_texture_params`，再 image unit + dirty 归属反转 + 跨步描述符），让回归能二分到其中一半。**Mali 设备门必须发布逐帧上传作业数与帧时增量**（不是只有 SSIM）。

### 5.5 DirectVulkan（Magma）逐子系统

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

### 5.6 strangler 顺序（风险最小化）

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

### 5.7 A/B：旧路径怎么保留，**以及它的口径在哪里收窄**

```
MOBILEGL_PIPE_PUSH        = <子系统位图>   # 0 = 全 pull；每位一个子系统；含一位关闭 CSO 内容寻址（负面对照）
MOBILEGL_PIPE_VERIFY      = 0|1            # 影子比对（~5-10x 慢，永不出货；P13 之后仍保留）
MOBILEGL_PIPE_STATS       = 0|1            # 字节/调用/roundtrip/纹理拉取/上传形状计数器
MOBILEGL_PIPE_LEGACY_MEMOS= 0|1            # ★v2：编译期开关，保留 registry / TwinLookupMemo 实现
```

在 init 时刻锁存，与 `MOBILEGL_BACKEND_TYPE` 同一套机制（`ConfigLoader.cpp:212-225`），与树里已有的 ~40 个 `MOBILEGL_*` 开关并列。

**v2 必须写明的口径收窄。** v1 说"任何一次提交都能在同一份二进制上按子系统 A/B，设备回归可以二分到'哪个子系统'"。**这在阶段 B（值字段）成立，在阶段 C（handle 化）之后不成立**：stage C 把 `PipeInputs` 的字段**类型**从 `SharedPtr<FrontendObject>` 换成 `MGPipeHandle` + POD 描述符、把 6 个 `StateBackendObjectRegistry` 哈希表换成 slot 数组、删掉 `TwinLookupMemo`×3 与 `OwnerEquals`、把 memo 重键成 `{slot, gen}`。位清零时，`SnapshotFromGLContext()` 仍要从 client 的 slot 表**合成**那个 handle，backend 仍然跑重键后的 memo 代码——**两个分支跑的是同一份新代码**。一个重键 bug（正是 D1/D2/D3/D11/D13 那一类）在两个分支里都在，位图二分不出来。

**对策**：`MOBILEGL_PIPE_LEGACY_MEMOS`（**编译期**开关）在 P3a 与 P4a 期间保留 registry / `TwinLookupMemo` 的实现活在同一个 `PipeInputs` 接口之下，给前两波 handle 化保留一个**真正的**旧-vs-新臂；随 pull 路径一起在 P13 退役。**这条开关的存在期与代价必须写在阶段计划里**（P3a/P4a 各 +1 天维护成本）。

**P13 删除 pull 路径时**：删 `SnapshotFromGLContext()` 的**非 verify** 编译分支、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**`MOBILEGL_PIPE_VERIFY` 连同它需要的 `SnapshotFromGLContext()` 与 `MG_State` include 一起保留**（D-B5）；`static_assert(sizeof(ResidualValueBlock) == 0)` 必须编译通过；三道纯度门（§3.7.2）在**非 verify** 构建上转绿。

---

## 6. backend → frontend 反向通道

这是历次评审对任何薄 backend 设计的中心反对意见，所以逐条处理，**不做概括**。实测：`grep -rnoE "(->|\.)(SetBackendResource|SetBackendHashMemo|SetBackendStateMemo|SetBackendAuxMemo|WritebackFromBackend|MarkGpuWritten|MarkStorageDirty|AllocateStorage|SetInternalFormat|UpdateMipmapSubData|EnsureGpuResidentStorage|SyncPersistentMappedRange|SyncGpuWrites|RecordError|InvalidateCompileEnv|TruncateMipmapLevels|SetSamples)\(" MG_Backend/` = **95 个调用点 / 17 个方法**，外加 6 处 backend 反向进 `MG_Impl`。

### 6.1 `MGPipeCallbacks`：把反向通道具名化（对 gallium 的偏离 D8）

```cpp
// MG_Pipe/MGPipeCallbacks.h  —— context_create 时安装；monolith 里是直调，split 里是记录
struct MGPipeCallbacks {
    void (*on_gl_error)             (Uint32 code);
    void (*on_gpu_written)          (MGPipeHandle res, Uint rangeCount, const MGPRange*);
    void (*on_buffer_writeback)     (MGPipeHandle res, Uint64 off, MGPBlobRef bytes);
    void (*on_texture_writeback)    (MGPipeHandle res, const MGPBox*, MGPBlobRef bytes);
    void (*on_texture_pull_request) (MGPipeHandle res, Uint16 target, Uint16 firstLevel, Uint16 levelCount,
                                     Uint64 pullSerial);
    void (*on_mip_levels_generated) (MGPipeHandle res, Uint16 base, Uint16 count);   // 只带形状，不带字节
    void (*on_surface_changed)      (const MGPSurfaceInfo*);
    void (*on_caps_invalidated)     ();
    void (*on_log)                  (Uint8 level, const char* text);
    void (*on_xfb_scatter_ready)    (MGPipeHandle scratch, Uint64 packedStride, Uint64 vertices);  // ★v2
};
```

配套的**正向终止符**（在 `MGPipeContext` 里，不在 callbacks 里，因为它是 client→server）：

```cpp
// ★v2：拉取请求的显式应答，可以携带零个 region
void (*resource_subdata_complete)(MGPipeHandle res, Uint16 target, Uint16 firstLevel,
                                  Uint16 levelCount, Uint64 pullSerial);
```

gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求/终止、default-FB 几何这些词汇——因为在 Mesa 里 state tracker 与 driver 共享地址空间。**把它们具名化为 10 个回调 + 1 个终止符，好过藏在 95 个 poke 点里。**

### 6.2 95 个写回点的逐族归属

| 族 | n | 变成什么 |
|---|---|---|
| `SyncPersistentMappedRange` | **20** | **v2 修正：不是"全部消失"，而是逐站点归属。** 其中多数紧挨着一次对客户端字节的 CPU 读，而那些读搬到了 client（§4.8），由 **tracker 在填 `MGHostSpan` 之前**做同一次 reconcile（逐站点表见 §4.8.1）。**但至少一处的消费者搬不走**：`UniformManager::ResolveUniformBufferPayload`（`UniformManager.cpp:2022` 同步，`:2052` 读 `MappedData()+rangeStart`，`:2053-2057` 零填充）把具名 UBO 打进 **Magma 自己的 UBO ring**——由 D-B8 的 `set_shader_buffers` host payload 承载，client 在**发射前**做 reconcile。**P1 的交付物包含这 20 处的逐站点归属表**（哪些消失、哪些变 client 发射前 reconcile、哪些需要 host payload），不接受笼统结论 |
| `MarkStorageDirty` | **18** | 16 处是 server 本地记账——**零消息**（dirty 归属反转，§6.3）。2 处 `true`（`Managers.cpp:2813`、`DirectGLES.cpp:6852`）变 `on_texture_pull_request` / `on_texture_writeback` |
| `AllocateStorage` | **8** | 6 处是 **backend 凭空造出来的前端对象**（Magma 的占位纹理、`SwapchainObject` 的 default-FBO 占位，`SwapchainObject.cpp:284, 305, 329`）→ **server 原生，永不上线**；1 处是生成 mip 的 shadow（`DirectGLES.cpp:6261`）→ `on_mip_levels_generated`；1 处是 swapchain 尺寸变更 → `on_surface_changed` |
| `WritebackFromBackend` | **8** | `MGPReplySlot`（回读）+ `on_buffer_writeback`（PBO 回读、XFB 捕获）。**必须按操作级批处理**：其中两处今天在循环里**逐行**写回（`Utils.cpp:2342`、`DirectGLES.cpp:7633`），绝不能变成"每扫描线一次 IPC" |
| `SetInternalFormat` | **7** | 与 `AllocateStorage` 同批 |
| `SyncGpuWrites` | **6** | 同 `SyncPersistentMappedRange`：**逐站点**，见 §4.8.1 |
| `MarkGpuWritten` | **6** | client 在每个 draw/dispatch 发射点**保守自建**，镜像 `DirectGLES.cpp:459-467, 509, 1809` 与 `UniformManager.cpp:1073, 1229`、`VulkanRenderer.cpp:11210` 的输入。`on_gpu_written{res, ranges[]}` 是**收窄**通道 |
| `RecordError` | **6** | `on_gl_error`，**必须对命令流有序**（§6.4） |
| `SetBackendResource` | **4** | **删除。** server 拥有资源表；pooling / 延迟释放原样搬到 server |
| `EnsureGpuResidentStorage` | **3** | server 本地决策 |
| `SetBackendHashMemo` / `SetBackendAuxMemo` | **3** | 纯值 → server 侧 per-slot 字段 |
| `InvalidateCompileEnv` | **2** | `on_caps_invalidated`，低频 |
| `SetBackendStateMemo` | **1** | **直接删除，不翻译**（D12） |
| `UpdateMipmapSubData` / `TruncateMipmapLevels` / `SetSamples` | **3** | 全在 Magma 的占位纹理里 → server 原生 |

**6 处 backend 反向进 `MG_Impl`：** 四处 `pDefaultFramebufferInfo` 身份比较 → 保留 handle `{0,1}` + `MGPFramebufferState::isDefault`；`SwapchainObject.cpp:276-330`（backend **创建** default FBO 的三张 `ITextureObject`）→ `on_surface_changed`，client 自己合成对象——**顺带删掉 monolith 里的一处分层倒置**；`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`）→ `get_texture_image` 返回 **"该 level 无 GPU 背书，请从你自己的 shadow 回答"**（`:10691-10704` 今天测的正是这个条件）。

#### 6.2.1 v2 新增：XFB scatter 是对 client shadow 的 read-modify-write，必须搬到 client

v1 把 8 处 `WritebackFromBackend` 全部归给单向的 server→client 通道。**`ScatterCapturedRecords`（`DirectGLES.cpp:893-960`）不是单向的**：它在 `:928` 做

```cpp
Memcpy(staged.data(), target.buffer->MappedData() + target.start, rangeBytes);
```

——**从应用已有的字节起步**，然后只把捕获到的 varying 补进去，"这样 `gl_SkipComponents` 要求的空洞保留应用原本放在那里的东西——**这正是这个特性的全部意义**"（`:889-892` 的注释；`:880-883` 点名 `KHR-GL46.transform_feedback.capture_special_interleaved_test` 是走到这条路径的用例）。server 没有 `MappedData()`，而 `MGPipeCallbacks` 里也没有反向的 buffer 读。照 v1 实施，要么空洞被清零（一致性破坏），要么需要一次 §12.2 没有列出的、发生在 `glEndTransformFeedback` 上的同步反向读。

**修正（不新增停顿类）**：**scatter 搬到 client。**

1. server 把驱动捕获到的**紧密打包** scratch 字节通过 `on_buffer_writeback(scratchHandle, 0, bytes)` 推给 client，并用 `on_xfb_scatter_ready(scratchHandle, packedStride, vertices)` 告知布局参数；
2. client 拥有目的 shadow，也从反射归档里拥有 `GetTransformFeedbackVaryings()` / `GetTransformFeedbackStride()` / `GetTransformFeedbackPackedStride()`（`ProgramObject.h:1146-1171, 1357-1394`），于是原样跑今天 `:930-939` 的补丁循环；
3. client 把补好的范围当作**普通 `resource_subdata`** 重新发下去（复现今天 `:946-948` 的 `glBufferSubData` 回灌），并 bump 自己的 change serial（复现 `:942` + `BumpBufferMutationEpoch()`）。

副作用：`:906-914` 的"CPU 模型给出 0 顶点 → 整批捕获丢弃"的诊断**落到应用线程**上，比落在 server 上更有用。计入 Espryt 子系统 7（§5.4）。

**（P0 实测修正）一个活的陷阱：`EndTransformFeedback` 槽位的 null 被当成能力位在用。**
`MG_Impl/GLImpl/Drawing/GL_Drawing.cpp:1253-1256` 读的不是这个 hook 的**功能**，而是它的**空与非空**：槽位非空即被解释为"该 backend 按 GL 的顶点序捕获，因此跳过 `FixupGsStripCaptureOrder`"。也就是说**任何**出于别的理由注册了 `EndTransformFeedback` 的 backend，会**静默**丢掉几何阶段的 strip 重排——没有编译错误、没有日志、只有错序的捕获结果。这是 §3.1 那条"null 项表示未实现、前端回退"的惯例被**反向**使用了一次：它在这里表达的是一个正向能力断言。
**MGPipe 下必须转成显式能力位**（例如 `kCapDriverOrderedXfbCapture`，与 §3.4.1 的 `callMask` 同列），由 backend 主动声明，`GL_Drawing.cpp:1253-1256` 改读该位而不是测空。**列为 P8/P9 项**——P8 触到 XFB 动词、P9 触到反向通道与 `on_xfb_scatter_ready`，两处都会重排这段代码；在此之前它是 monolith 上一个真实存在、只是暂时没人踩到的地雷。

### 6.3 纹理 dirty 归属反转

**client** 保留 `MipmapStorage` 的模型（96-rect 级联合并 + `summedArea*4 >= unionArea*3` union-box 回退，`MipmapStorage.cpp:300-305`），维护一份**发射游标**，在发射后清自己的标志。**server 从不碰 client 的标志。**

这是安全的，且已核实：**`MG_Impl` 里没有任何 `IsStorageDirty(` / `GetStorageDirtyRects(` / `GetStorageDirtyRegion(` 调用点**（前端从不读自己的 dirty 状态），而它自己在五处主动清（`GL_Texture.cpp:528, 701, 5547, 5621, 5691`）。**这一条让"server 侧逐 level 权威位 + 纹理 ack 协议"整套机制不必存在。**

**v2 修正 1：发射游标必须按**存储属主**键控，不能按 `(texture, uploadTarget, level)`。**
`TextureObjectView` 把 `IsStorageDirty` / `MapMipmapData` / `MarkStorageDirty` / `MarkStorageDirtyRegion` / `GetStorageDirtyRegion` **全部转发给存储属主的 mipmap 并做索引重映射**（`TextureObjectView.cpp:290-322`；`:281` 直接写属主的数据）。一个 view 与它的属主**共用同一份 dirty 状态**却会各带一个游标：谁先发射谁就清掉了另一个还需要的标志，或者两边都发同一批纹素。
**正确键**：`(storageOwnerHandle, ownerUploadTarget, ownerLevel)`——查询与清除前先经 `GetViewStorageOwner()` 与 view 的 `ToOwnerUploadTarget()` / `ToOwnerLevel()` 映射。
**门**：新增场景，通过 view 上传、经属主采样（以及反向），跨 draw 边界各一次。

**v2 修正 2：`MOBILEGL_PIPE_VERIFY` 需要一个"保留模式"，否则它在最危险的子系统上是瞎的。**
影子比对（§13.3-②）的参照物是"从头重算一次快照"。但发射后 client 已经把 dirty 标志清了，**从头重算无法重建当时的 rect 集合**——于是子系统 5（`resource_subdata` 的 payload）恰恰是 verify 看不见的那一块，而它同时是 §5.4 标注"全表最危险"、押着 +6ms/frame 悬崖与 7 条 repack 路径的那一块。
**修正**：`MOBILEGL_PIPE_VERIFY=1` 时 tracker **保留清除前的 dirty 集合**到本次 draw 结束，G4 比对**发射出去的 `(unionBox, regionCount, regions[])`** 与快照重算的结果。**并且**新增 `TextureUploadShapeScenario`：把逐纹理逐帧的上传形状（box vs N 个 region、作业数）录成金标，与 SSIM 并列比对——**+6ms 悬崖由形状相等把关，不是由 SSIM 把关**（SSIM 对它完全不敏感）。

**上传形状决策留在 server**：`resource_subdata` 同时带 union box 与 region 列表（§3.5.6），Mali 按作业数计价的悬崖在哪一侧付 GPU 代价，决策就留在哪一侧。

### 6.4 反向通道的有序性是正确性要求，不是优化

**`on_buffer_writeback` 必须与 epoch bump 有序。** 今天每一次 `WritebackFromBackend` 后面都紧跟一次 `BumpBufferMutationEpoch()`（`DirectGLES.cpp:834-837, 942, 7625-7629`），否则 server 自己的 draw-clean memo 会在 epoch 背后变陈旧。split 里这变成**反向通道上的一条排序规则**：一次写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 侧应用。**反向通道需要与正向通道相同的有序保证。**

**`on_gl_error` 必须对命令流有序**，否则 `glGetError` 答错。`glGetError` 本身永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`）。

**v2 修正：`kNeedsAck` 只标真正**同步**的分配点，不是"看起来像分配"的 GL 入口。**
v1 把 "`glRenderbufferStorage*`、可能失败的 `glTexImage*`/`glTexStorage*`/`glCopyTexImage*` 形式、`glBufferStorage`" 全标成 `kNeedsAck`，让 OOM 探测惯用法（`allocate; if (glGetError()==GL_OUT_OF_MEMORY) 用更小的重试;`）成立。**实测这批里纹理族根本不调 backend 表**：`MG_Impl/GLImpl/Texture/GL_Texture.cpp` 在 `:2515, 2671, 2755` 只做 `MarkStorageDirty(..., true)`，Espryt 在 sync 时刻才惰性分配；纹理侧的错误上报 `RecordGLError`（`DirectGLES.cpp:6309-6324`）**只有一个调用者**——`glGenerateMipmap`（`:6916`）。连唯一一处真正的同步分配 `glRenderbufferStorage*` 也是在 `BackendRenderbufferObject::SyncToBackend`（`Managers.cpp:8674-8684`）里惰性做的。

**修正后的规则**：
- **纹理分配的 OOM 在 monolith 里就已经推迟到 sync 时刻，拆分不改变任何可观察行为** —— 这批**不标** `kNeedsAck`，并把这条事实写进文档（避免后人以为是遗漏）。
- **（P0 实测修正）`kNeedsAck` 只标一项**：`glBufferStorage`（真同步）。**`glRenderbufferStorage*` 不标**，保持惰性/异步分配。
  **证据**：41 个 trace fixture 里 OOM 探测惯用法出现 **0 次**——全部语料只有 **9 次 `glRenderbufferStorage` 调用、分布在 5 个 fixture**，且**没有一次**在其后 3 个调用之内跟 `glGetError`；语料里真实的成功性检查是 `glCheckFramebufferStatus`，而它本来就在 client 侧作答。因此整条 ack 路径连同它的往返一起省掉，`RenderbufferStorage` 在 `PipeCalls.def` 里的 flags 是 `kNone`。
- 其余错误一律晚到，走有序的 `on_gl_error`。

**对事件通道的强制条款：`on_log` 必须按严重级分级。** §8.4 的朴素策略把**全部**日志行设为有损（覆盖最旧 + `eventDropped`）。但 §4.7 已确认：**backend program link/compile 失败只以一行日志加一次 bind-program-0 的空 draw 呈现**。统一有损策略下，系统里诊断价值最高的那一行会在日志压力下静默消失。

**规则**：`on_log(level ≤ WARN)` 有损；**`on_log(level ≥ ERROR)` 无损**，加入触发 `eventRingFull` + 停止 apply 的语义事件集；再加一个**每秒 ERROR 速率限制器**，超限时发一条显式的 "N errors suppressed"。`MGLOG_E_ONCE` 的 latch 变成 per-server。P9 的故障注入门：日志洪泛下注入一次 link 失败，那行 ERROR 必须出现**且**两侧都恢复。

### 6.5 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素字节，三个原因会要求 client 重发已发过的 level：`RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）。**四条缓解同时上**（v1 是三条，v2 补第 (e) 条终止符），加一个专门的门和一个必须发布的计数器：

**(a) 预防主因。** client 给纹理打 `everImageBound` 标记，`resource_create`/`respecify` 一直携带 `imageBindableHint`，于是 image-bindable 存储在前期就分配好。这把 `RequireImageBindableStorage` 从稳态里彻底移除。

**(b) 拉取是异步的。** server 发 `on_texture_pull_request{res, target, levels[], pullSerial}` 并把那个 twin **标为 not-ready**；client 在下一次 publish 时重发。因为 client 跑在前面，常见情况下字节在 server 到达采样该纹理的 draw 之前就到了；即使没到，**阻塞的是 `mgl-srv-apply` 线程，不是应用线程**。

**(c) 有上限的保留（默认关闭）。** 可选的逐纹理保留位，受一个显式的 LRU 字节预算约束（`MOBILEGL_PIPE_TEXEL_RETAIN_MB`，**v2 把默认从 32 改为 0**）。理由：`MipmapStorage` 保有每个 level 的完整 CPU 影子（`MipmapStorage.h:117` 的 `Vector<Vector<Uint8>> m_data`），所以一次拉取**总是能**从 client 已有的字节服务——保留缓存买的是**延迟**，不是正确性，而它花的是**内存**，恰好是 §7.11 里被逐项预算的那个指标。只有 (d) 的实测拉取率非平凡才开，并拿真预算。

**(d) 门与计数器。** `TextureRemintPullScenario`：同时强制 `RequireImageBindableStorage` 与一次帧中格式再生。**拉取次数逐 trace 用例发布**，与 SSIM 并列。**本设计从不声称"零 round trip"，它测量并公布。**

**(e) v2 新增：显式终止符——因为存在"答不出来"的拉取。**
`RequireImageBindableStorage` 的重放会 re-dirty 每个上传目标的每个 level（`Managers.cpp:2789-2822`），而它自己已经跳过 `GetMipmapByteSize(...) == 0` 的 level（`:2810-2812`）。但还有一类 level：**内容只来自渲染、来自一次 `CanMirrorCopyImageShadow` 拒绝的 `glCopyTexSubImage`（`DirectGLES.cpp:7068-7073`）、或来自 GPU 侧 mip 生成**——client 那里根本没有字节。没有终止符，apply 线程会 park 在一个**永远不会 ready 的 twin** 上。B-R4 与 `TextureRemintPullScenario` 只针对拉取的**频率**，从来没针对**无解的拉取**。
**修正**：
- 拉取是 request/response 对，由 `resource_subdata_complete(res, target, firstLevel, levelCount, pullSerial)` 终止，**它可以携带零个 region**；
- 收到零 region 的应答时，server **带着"已分配但为空"的存储继续**（这正是 monolith 的行为：`EnsureGenerateMipmapStorageAllocated`（`DirectGLES.cpp:6270-6271`）也是 `AllocateStorage` + `MarkStorageDirty(false)`，不填内容），并记一条 `MGLOG_W`；
- **`TextureRemintPullScenario` 必须包含这个无解用例**（一张只被渲染过、随后被 image-bind 的纹理），**且它必须在终止符落地之前是红的**（表现为 apply 线程挂死或超时）。

若在真实语料（MC 与 Iris fixture）上实测拉取率非平凡，(c) 从可选升级为强制并拿到真预算。

---

## 7. 传输与数据面

> 本章与状态模型无关：它规定字节怎么过去、什么时候可以被覆盖、背压怎么升级。§8 规定控制面与同步，§9-§11 规定帧节奏、线程与平台。

### 7.1 段（segment）布局

| 段 | 拥有者 | 默认大小 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB，2 的幂，64B 对齐 | `RingControl`(4KiB) + POD 记录 + ≤4KiB 内联负载 |
| `SEG_STAGE` | client（server 只读） | 32 MiB → 上限由实测定，**不是默认 256 MiB** | bulk 字节：buffer sub-data、纹理区域、UBO scratch、client 顶点/索引/indirect 数组、persistent-map 脏块 |
| `SEG_REPLY` | **server**（client 只读） | 8 MiB，4KiB slot | readback 像素、buffer writeback |
| `SEG_EVENT` | **server**（client 只读） | 256 KiB SPSC ring | `EvQueryResult`/`EvGpuWritten`/`EvGlError`/`EvLogLine`/`EvSurfaceChanged`… |
| `SEG_SHADOW[n]` | client（server 只读） | 每对象，P4.5 起，≥256KiB shadow | 零拷贝 buffer/texture shadow |
| `SEG_ADOPT[n]` | **server**（client RW） | 每 buffer，P11，≥16MiB adopted store | 应用直写 GPU 内存 |

创建：Android `ASharedMemory_create`（API 26，`android/sharedmem.h:78`；libc 的 `memfd_create` wrapper 是 API 30，`sys/mman.h:196`）；桌面 Linux `syscall(SYS_memfd_create, …)`；macOS `shm_open`+`shm_unlink`；Windows `CreateFileMappingW`(`Local\`)。

**传递：POSIX `SCM_RIGHTS`，在第一个 transport commit 里实现**（asio 无 cmsg API → 在 `socket.native_handle()` 上裸 `sendmsg`/`recvmsg`，约 80 行）。`Feat/CS-Delta-IPC` 把它推迟到"P6"（`LocalSocketTransport.h:16-20`，`PollOffer` 里 `out->fd = -1` 硬编码于 `:296`），结果它的数据面在唯一重要的平台上**一个字节都过不去**。**这条是 P0 的第一优先级。**

**`SEG_SHADOW` 块的退休规则**：§7.4 的 64KiB 块发送水位只解决"覆盖一个**活着的** shadow"；它没说怎么**释放**一个 shadow。`glDeleteBuffers` 或 `glBufferData` 重定义会释放/重分配 `SEG_SHADOW` 的 arena 块，而携带 `{segId, offset, size}` 指向该块的记录可能还没被 apply——server 于是读到另一个对象的字节。规则：释放的块进入 pending 链表，只有当 `appliedSeq`（对被借入 GPU 时间线的 slot 是 `retiredSeq`）越过最后一条引用它的记录之后才归还 arena，而不是在对象析构时立即归还。

#### 7.1.1 `SEG_STAGE` 必须额外容纳的六类字节（v2 清单）

MGPipe 让 `SEG_STAGE` 承载了它在纯 delta 模型下不承载的字节，定尺时必须算进去：

1. **client 顶点数组**（`(first+count-1)*stride + elementSize` / 属性 / draw）；
2. **client 索引数组**（`count * indexSize`）；
3. **multi-draw 参数块**（`first[]`/`count[]`/`indices[][]`/`basevertex[]`，`drawcount*4` 级）；
4. **client 解析后的 `*IndirectCount` 命令块**（几十字节）；
5. **具名 UBO 的 host payload**（D-B8，`kCapNeedsHostUboBytes` 下逐 draw 逐块，计数器 `stage-ubo-named`）；
6. **纹理 subdata 的紧密重打包区域**（§3.5.6；今天走 unpack ring 时也已经紧密重打包，所以字节量同阶，但现在过 ring slot）。

**不在此列**（D-B7 解决）：restart 重写的整 EBO（`kMaxRestartRewriteBytes = 1<<26` = 64 MiB，是默认 `SEG_STAGE` 的两倍）与 multi-draw 展平的索引流（`kMaxFlattenedIndices = 1<<24`）——**它们由 server 侧的索引宿主镜像喂养，不过 `SEG_STAGE`**（§7.10）。

上限由 P0 落地的计数器实测定，不用默认值猜。**并且 G3 必须为"单条记录大于段容量"定义明确的分块/降级路径**（大 subdata 分块成多条，而不是一条巨记录）。

### 7.2 RingControl：watermark 是一条共享 cache line，**且带双向 doorbell**

```cpp
// MobileGL/MG_Remote/Transport/Ring.h
struct alignas(4096) RingControl {
    // ---- SEG_CMD 游标 ----
    alignas(64) std::atomic<Uint64> cmdHead;              // producer：累计写入字节
    alignas(64) std::atomic<Uint64> cmdAppliedTail;       // consumer：已解码并拷出的字节
                std::atomic<Uint64> cmdRetiredTail;       // consumer：被借入 GPU 时间线的 slot 已释放
    // ---- SEG_STAGE 游标（独立三元组）----
    alignas(64) std::atomic<Uint64> stageHead;
    alignas(64) std::atomic<Uint64> stageAppliedTail;
                std::atomic<Uint64> stageRetiredTail;
    // ---- 序号 / 帧水位 ----
    alignas(64) std::atomic<Uint64> appliedSeq;           // 已 apply 的记录序号
                std::atomic<Uint64> submittedSeq;         // 已提交给驱动
                std::atomic<Uint64> retiredSeq;           // GPU 已完成
                std::atomic<Uint64> completedFrameSerial;
                std::atomic<Uint64> presentAckSerial;
    // ---- doorbell / 代 ----
    alignas(64) std::atomic<Uint32> serverEpoch;          // context 丢失 / server 重启时 ++
                std::atomic<Uint32> ringGeneration;       // 硬 drain 后 ++，作废缓存 offset
                std::atomic<Uint32> consumerParked;       // server 睡了，producer 要敲门
                std::atomic<Uint32> producerParked;       // client 睡了，server 要敲门
                std::atomic<Uint32> eventRingFull;        // SEG_EVENT 满，server 已停止 apply
                std::atomic<Uint32> eventDropped;         // 被丢弃的有损日志行计数
};
```

**三个 seq 水位严格区分**（混为一谈是经典错误）：`appliedSeq` 释放 `cmdAppliedTail`/`stageAppliedTail`；`submittedSeq` 释放 staging；`retiredSeq`/`completedFrameSerial` 释放 `*RetiredTail` 与 `SEG_ADOPT` 复用。

**两个 tail 是必须的**：`Ops_ResidentSubData` 把字节拷进 `pendingResidentWrites`（`Managers.cpp:1158-1166`），P11 之后 server 会**借用** ring slot 而不是再拷一次——那种 slot 只能在 `completedFrameSerial` 之后回收。单 tail 会在那一天变成保守回收。

**`SEG_STAGE` 必须有自己的游标三元组**：§8.2 把"`SEG_STAGE` 余量 < 1/4"列为 Publish 触发器，而第二个 ring 的占用率无法从第一个 ring 的游标算出；且 stage slot 的退休条件（`retiredSeq`）与 cmd 记录（`appliedSeq`）不同。

#### 7.2a 双向 doorbell

- **client → server**：consumer 自旋 ~200µs → 置 `consumerParked=1` → 在控制 socket 上阻塞读 1 字节；producer 在 release-store `cmdHead` 之后，仅当 `consumerParked` 时写 1 字节（字节码 `0x01 = 'ring advanced'`）。
- **server → client**：client 在**任何**等待里（present credit、`kNeedsAck` 阻塞请求、ring/stage 满的升级等待）先自旋 `MOBILEGL_IPC_SPIN_US`（默认 50µs），再置 `producerParked=1`，然后在同一个 socket 的反向流上阻塞读；server 在 release-store 任何 watermark 之后，仅当 `producerParked` 时写 1 字节（字节码 `0x02 = 'watermark advanced'`）。

没有这一条，每一处 client 等待都退化成跨进程自旋一条共享 cache line：present-credit 等待最长一整帧（60Hz 下 16.6ms），在手机上就是一颗大核满频空转，与 GPU 和游戏 JVM 抢核；§7.5 的"有界 50ms 等待"就是 50ms 自旋。而 MobileGL 全库没有任何亲和性控制（`grep -rn 'sched_setaffinity\|cpu_set_t' MobileGL/` 零命中），无法把它赶到小核上。

`spawn` 模式用 socketpair 的两个方向做 doorbell；`inproc` 模式用一对 `std::condition_variable`（同一套 `producerParked`/`consumerParked` 语义）。**零 futex/eventfd/named-event 平台代码**（asio 已 vendored，`3rdparty/asio/include` 已在主 target 的 include path 上，`CMakeLists.txt:483`）。

### 7.3 记录格式

```cpp
// MobileGL/MG_Remote/Protocol/RecordKinds.h
struct RecHeader { Uint16 kind; Uint16 flags; Uint32 size; };   // 8 B，size 含 header，8 字节倍数
enum RecFlags : Uint16 { kNone=0, kNeedsAck=1<<0, kHasBlob=1<<1, kPad=1<<2, kBorrowSlot=1<<3, kVarTail=1<<4 };
struct BlobRef  { Uint32 seg; Uint32 pad; Uint64 offset; Uint64 size; };  // 24 B
```

**没有 per-record 序号字段**：seq 就是记录序数（producer `m_emitSeq++`，consumer `m_applySeq++`），省 8B/记录并消除一整类失步。

**单一真相源是 `PipeCalls.def`，生成器是 G3**（§3.1）。它对**每一个** MGPipe 调用生成三样东西：

```cpp
// 1) 一条尺寸断言（每种记录一条，不是只对 union 首成员）
static_assert(sizeof(MobileGL::Wire::RecDrawVbo) == 56, "DrawVbo record size drift");

// 2) applier 分发前的运行期边界检查
case RecKind::DrawVbo:
    if (h.size < 56 || h.size > remainingRingBytes || (h.size & 7u))
        return Fatal(FatalCode::ProtocolCorruption, "DrawVbo");
    break;

// 3) applier switch 的一个分支：解码 → 更新对象表 → 调 backend 函数指针
```

**每种一条 `static_assert`** ——修掉正是 `Feat/CS-Delta-IPC` 中过一次的 bug 类（`b50f3348`："旧的 off-by-one 让 applier 误读 TexImage 之后的每一条 state delta"），而它那条只断言 union 首成员的 assert（`ServerCore.cpp:31-33`）永远抓不到中间插入。

**运行期边界纪律**：`SEG_CMD` 是对端并发写入的区域，编译期 `static_assert` 管不到运行期损坏。`kVarTail` 记录额外校验 `定长前缀 + 尾巴自描述长度 == h.size`；`kHasBlob` 记录额外校验 `BlobRef` 落在它声明的段内。违反一律 `Fatal{ProtocolCorruption}`，绝不进入未定义行为。

变长记录（`set_sampler_views` 的 view 数组、`resource_subdata` 的 rect 列表、`draw_vbo` 的 `MGPDrawRange[]` 与 `MGHostSpan`、`set_shader_buffers` 的 range 数组）：`kVarTail` + 定长前缀 + 自描述长度的内联尾巴。

### 7.4 WAR 危害与字节稳定性

**Phase 1 规则（P5-P8）：GL 调用时刻把字节拷进 ring slot。** slot 从写入到 `stageAppliedTail` 越过它为止不可变，client 拿不回它 → **危害按构造消除**。代价是一次 memcpy，而 `Ops_ResidentSubData`（`Managers.cpp:1165`）和 `StageBlocksIntoUnpackRing` 在 monolith 里已经在付同样的钱。

**Phase 2 规则（shadow-in-shm，零拷贝）：** ≥256KiB 的 shadow 分配在 client 拥有的 `SEG_SHADOW` 里——`PipeResource` 的 `MapAlignedAllocator`（`PipeResource.h:33-60`，无状态、25 行、64B 对齐）增加一个 shm arena（保留 `MIN_MAP_BUFFER_ALIGNMENT=64` 契约，`PipeResource.h:28`），`MipmapStorage` 的 level vector 同理。`resource_subdata` 于是只带 `{segId, offset, size}`，**client 侧零拷贝**。
WAR 用 **per-shadow 64KiB 块发送水位**：若应用写入某块而该块最后一次发送尚未 `appliedSeq` 覆盖，这次写走 `SEG_STAGE`。有界、局部、压力下自动退化成 Phase-1 行为。这套块水位同时是 §7.8.1 精确版 persistent-map 推送的脏位来源。

**该改动必须整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹**：`PipeResource` 与 `MipmapStorage` 住在 `MG_State`，不在 `MG_Remote`，而改一个容器的 allocator 就改了类型；不包裹的话 §13.5 的编译期折叠保证不成立。写法是"分配器特化：option OFF 时逐字折叠成今天的 `MapAlignedAllocator`"。

#### 拷贝账（MC pan 一帧约 9MB section mesh + ~1MB UBO scratch）

- monolith 的 `glBufferSubData` → shadow store 是 **2 次**：(1) app→shadow（`BufferObject::UploadSubData` 的 `Memcpy`），(2) shadow→目的地（`FlushPendingRangesNow`：`Memcpy(dst, bufferObject.MappedData()+start, size)` 进 invalidating map，`Managers.cpp:914`；或 `Memcpy(g_uploadRing.store.mappedPtr+ringOffset, ..., size)` 进 upload ring，`Managers.cpp:922`）。
- split Phase 1 是 **3 次**：app→client shadow (1)、client shadow→`SEG_STAGE` (2)、server 的 `FlushPendingRangesNow` ⇒ `SEG_STAGE`→upload ring (3)。
- Phase 2（shadow-in-shm）去掉 (2)，剩 **2 次**——**与 monolith 持平**。

**这是 MGPipe 的一个结构性收益**：server 没有第二份 `BufferObject`/`PipeResource`，所以不存在"staging → server 侧 shadow"这次中间拷贝，也不需要为它设计一种只读采纳模式或 copy-on-write 升级。

| 路径 | monolith | split Phase 1 | Phase 2 |
|---|---|---|---|
| `glBufferSubData` → shadow store | 2 | 3 | **2** |
| `glBufferSubData` → adopted store（P11） | 2 | 2 | 2 |
| `glMapBufferRange(WRITE)`+unmap | 3 | 4 | 3 |
| persistent coherent map 推送（§7.8.1 保守版） | 0 | 1/发射点 | 1/发射点（精确块） |
| `glTexSubImage` | 2 | 2 | 2 |
| 全局 UBO / draw | 1 | 2 | 1 |
| adopted ≥16MiB（P11 T1/T0） | 0 | 0 | 0 |

`TracyPlot` 字节计数器必须**装在 wire 两侧**（client 的 emit 字节 + server 的 apply 字节 + server 的 ring/staging 字节），验收看**总量**，不是只看 client 一侧的数字。

### 7.5 Ring 分配与背压

逐字移植 `PersistentRing`（`Managers.cpp:657-727`、`RingAllocateSlow` `:1891-1970`、`RingOnPresent` `:1975-2016`）：单调 head/tail、2 的幂掩码、frame mark。分配失败升级：**扩容(翻倍) → 对最老未 retire 批次有界等待（默认 50ms，走 §7.2a 的 `producerParked` doorbell，不是自旋） → 硬 `Drain` 请求 + `ringGeneration` bump**。generation bump 上线，防止后续记录引用被回收的 offset。

硬 drain 之后的恢复很便宜，因为 MGPipe 的正向流是自洽的推送流：client 的 tracker 把全部 dirty 位置为"必须重推"，下一个 verb 就会重新发出完整的 `set_*` 集合；纹理侧由 §6.3 的发射游标负责（游标未被清的 rect 仍在 client 手上）。**没有"重发未 apply 的对象状态"这类特殊协议。**

`SEG_CMD` 与 `SEG_STAGE` 各自独立跑这套升级（各有自己的游标三元组）。

### 7.6 纹理

- **Unpack PBO 完全在 client 解析**（`GL_Texture.cpp:1719,1765,1887,1976,2457,2604,2722,4458,6176` 读 `pixelUnpackBufferObject->MappedData() + (SizeT)pixels`，再由 `ProcessTexturePixelsDataUnpack` 紧密重排）。**没有任何纹理像素以 PBO 引用形式过线，server 永远不需要 `GL_PIXEL_UNPACK_BUFFER` 状态。`set_pixel_pack_state` 只用于 PACK 方向**（§3.6 D5）。
- **压缩纹理永不到达任何 backend**（前端在 `glTexImage` 时把压缩 internalformat 解析成非压缩后备，`GL_Texture.cpp:298-306`；`grep -i compress MG_Backend/DirectGLES/*.cpp` 只命中一条注释）。逐字节 `m_compressedData` blob 仅供 `glGetCompressedTexImage`，纯 client 侧，不过线。
- **`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client。** 这两个入口今天就是**纯前端操作**：`CopyTexSubImage{1,2,3}D_State`（`GL_Texture.cpp:3955,3979`）调 `CopyReadFramebufferIntoMipmapRegion`（`:1044-1097`），它借一次 backend `ReadPixels` 进 CPU scratch（`:1079`）、逐行 memcpy 进 mipmap shadow（`:1089-1094`）、`MarkStorageDirty(...,true)`（`:1095`）。拆分后它恰好是**一次阻塞 ReadPixels round trip**，产生的脏区按普通 `resource_subdata` 下发——正确，且不需要任何新命令。`glClearTexImage`（`GL_Texture.cpp:985-1006`）同形。
- **逐 level "server 权威" 位不存在。** dirty 归属反转（§6.3）让 client 始终是纹素的权威；backend 真正在 shadow 里写字节的两处（CPU 生成 mip 路径 `DirectGLES.cpp:6811-6861`、`glCopyImageSubData` 的目的地镜像 `:7144`）分别由 `on_texture_writeback` 与"CopyImage 镜像搬到 client"处理，server 需要重读纹素时走 `on_texture_pull_request` + `resource_subdata_complete`（§6.5）。

### 7.7 回读

| 路径 | monolith | 拆分后 |
|---|---|---|
| `glReadPixels` → 客户内存 | 阻塞 | 一次 round trip，像素放 `SEG_REPLY` slot；**逐行写回循环留在 server 内，按操作级批成一段** |
| `glReadPixels` → pack PBO | **也阻塞**（`DirectGLES.cpp:9189-9205` 把整个 PBO map 回来写 shadow） | **fire-and-forget** + client 侧对该 PBO 置 `MarkGpuWritten`，代价推迟到之后的 map/read。**严格优于 monolith** |
| `glGetTexImage`/`glGetTextureImage` | DirectGLES 从 client shadow 回答 | DirectGLES **零 round trip**（GPU 生成的 level 也是——monolith 那里同样是"已分配但未填充"，§12.1）；DirectVulkan 一次（`get_texture_image` 对"无 GPU 背书"的 level 回答"请用你自己的 shadow"，`VulkanRenderer.cpp:10691-10704`） |
| `glGetBufferSubData` / `glMapBuffer(READ)` on gpuWritePending | 阻塞（`glFinish()`，`Managers.cpp:1246`） | 一次，由 client 侧保守 pending 集合触发，被 `on_gpu_written{ranges}` 收窄 |
| XFB capture writeback | `glEndTransformFeedback` 里无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`） | **不等**，client 对 capture target 置 `MarkGpuWritten`，首次读时付；scatter 由 §6.2.1 的 client 侧路径完成 |
| `glCopyTexSubImage*` | 内含一次同步 ReadPixels | 一次 round trip（保持前端实现不变，§7.6） |

### 7.8 persistent map 与 ≥16MiB 采纳

三档，由**运行时 POST 探针**选择（遵循本项目"后端限制一律探针判定、绝不硬编码驱动名"的既定规则）：

- **T2 — 拒绝（IPC 期默认，永久正确回退）**：`AcquirePersistentMap` 返回 `nullptr`，前端已在三处容忍（`BufferObject.cpp:174, 439-442, 470-472`）。**此档下 §7.8.1 的 client 侧推送是强制的**，否则应用的 coherent persistent 写会丢。
- **T1 — server 导出自己的映射（P11 主攻）**：server 照常铸造 coherent map（`Managers.cpp:988-1058` / `VkBufferManager.cpp:515-563`），经 `VK_KHR_external_memory_fd` / `AHardwareBuffer_sendHandleToUnixSocket`（API 26，`hardware_buffer.h:521`）/ `VK_KHR_external_memory_win32` / `GL_EXT_memory_object_fd` 导出，client `mmap` 后调 `PipeResource::AdoptPersistentMap(base)`。**每次存储定义（respecify）一次 round trip**（v2 修正 v1 的"每 store 生命周期一次"——`TryAdoptLargeStorage` 在存储定义时触发，一个反复扩容的 arena 付 N 次）。`StorageBufferRegrowScenario` 必须发布 `map-persistent-roundtrips`。采纳成功后 §7.8.1 的推送对该 buffer 自动停止（`SyncPersistentMappedRange` 的 `IsGpuResident()` 早退），与 monolith 一致。
- **T0 — server 导入 client 分配**：client 分配 `AHardwareBuffer`/dma-buf，server 以 `GL_EXT_external_buffer`+`glBufferStorageExternalEXT` 或 `VK_EXT_external_memory_host` 导入。理想但可用性未知。

**决策路径**：P0 的 spike B 在第一周给方向（导出 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，client `mmap` 后回读，在两台设备上各跑一次）。若两台都否，P11 从 8 天缩为 2 天的文档与负面对照。**绝不允许一个平台未知数挡住 267 天的接口工作**（D-B4）。

#### 7.8.1 client 侧的 persistent map 推送

**问题**（已在仓库确认）：`BufferObject::SyncPersistentMappedRange()`（`BufferObject.cpp:238-250`）依次早退于 GPU-resident、非 Persistent、非 Write、FlushExplicit、空 range，剩下的情况（**persistent + write + coherent + shadow-backed**）走 `NotifySubData(整个 mapped range)`。它的全部生产调用点都在 `MG_Backend/` 里（20 处）。T2 档下 `AcquireMemoryRange`（`BufferObject.cpp:459-475`）回退到 shadow 并把 `m_resource.Bytes() + range.start` 交给应用——应用之后**不再调任何 GL 函数**就直接写。拆分后没人推，字节丢失。

另外 `IsBufferDrawClean` 里 `if (frontend->IsMapped()) return false;`（`Managers.cpp:1447`，注释："A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push"）也依赖 map 位。

**解法三件套（第 1 条按 MGPipe 收缩，第 2、3 条逐字保留）：**

1. **不需要把 map/unmap 做成一对上线的命令。** server 没有第二份 `BufferObject`，它唯一需要知道的是"这个资源现在有没有活的宿主写入者"——因为那正是 `IsBufferDrawClean` 那一行要表达的东西。所以 `resource_respecify` / `resource_subdata` 的 payload 里带**一个推送的 `hasLiveHostWrites` 位**（由 client 在 map/unmap 时更新），server 的 draw-clean 判定读它。零新增记录种类。
2. **client 侧脏块推送。** tracker 维护 `m_livePersistentMaps`（只装 persistent+write+非-FlushExplicit+非-GpuResident 的 buffer，进出由 map/unmap 入口维护）。在每个 validate 点，对**本次操作可达的**每个这类 buffer（VAO attribute buffer、index buffer、indirect/parameter buffer、UBO/SSBO/atomic binding point、XFB capture target——即 backend 那 20 个 `SyncPersistentMappedRange` 调用点的并集）做**块粒度**发送：把 mapped span 切成 64KiB 块，只发自上次发送以来被改过的块。
   "被改过"的判定：Phase 1 用**保守版**（每个发射点把该 buffer 的整个 mapped span 当脏，但按块拆成多条 `resource_subdata`，让 §7.5 的 range 合并与 ring 复用机制生效）；Phase 2 shadow-in-shm 落地后升级为**精确版**（shadow 住在 client 拥有的 `SEG_SHADOW` 里，用与 WAR 水位同一套 64KiB 块脏位跟踪；块脏位由 `memcmp` 或 mprotect 写屏障提供——先做 `memcmp`，它对 1MB 块是 ~50µs 量级，且只在真正 mapped 的 buffer 上跑）。
   **保守版在持久映射的 chunk arena 上代价可观**（每个可达发射点重传整个 mapped span）。所以 `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`（默认 64）可调，且 **P5 验收必须记录这条路径的字节量**（Tracy 计数器 `persistent-map-push`）。若保守版在 Create/Flywheel fixture 上不可接受，把精确版提前——这是计划里唯一一个允许因测量结果而改变阶段顺序的地方。
3. **门从第一天就有**：`PersistentCoherentMapScenario`（map PERSISTENT|WRITE|COHERENT、写、不做任何其它 GL 调用、draw、readback 校验），列为 P5 验收项。**今天计划里没有任何其它门能抓到这个 bug。**

**与 `MOBILEGL_COHERENT_AS_FLUSH` 的关系**：该开关（`GL_Buffer.cpp:297-305`，默认 false，`Config.h:174` / `ConfigLoader.cpp:185`）把应用请求的 persistent+FLUSH_EXPLICIT 改写成 coherent，从而**制造**上面这个情形。有了三件套，"我们自己改写出来的 coherent map"与"应用自己请求的 coherent map"走同一条正确路径，所以**该开关在拆分模式下照常生效**——这样 `tools/trace_replay/trace_cases.json` 里那两个带 `coherent_as_flush: true` 的用例（`minecraft-1.21.1-neoforge-create-indirect-in-world`、`minecraft-1.21.1-neoforge-create-instancing-in-world`）在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义。若实测保守推送在这两个 fixture 上代价过高，改为"这两个用例在 split 模式下同时关掉该开关，并在报告里标注"，而不是让两侧走不同路径还宣称对比通过。

### 7.9 应用指针（四类，范围全部可算）

| 类 | 范围 | 站点 |
|---|---|---|
| client 顶点数组（仅 DrawArrays 族） | `(first+count-1)*stride + elementSize` | `Managers.cpp:2560`、`VulkanRenderer.cpp:3737` |
| client 索引数组 | `count * indexSize` | `DirectGLES.cpp:4436`、`VulkanRenderer.cpp:4081` |
| client indirect / parameter 块 | `stride*(drawcount-1)+cmdSize` | `DirectGLES.cpp:276`、`DirectVulkan.cpp:303` |
| `MultiDraw*` 参数数组、`ClearBuffer*` value | `drawcount*4`、16B | `DirectVulkan.cpp:963-1057` |

唯一无界的是**索引 draw 下的 client 顶点数组**：索引扫描（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3406-3470`）必须在 **client** 侧跑，只有 client 同时持有两个数组。

**这四类的归属、门控与陈旧索引纪律全部由 §4.8 与 §4.8.1 规定**（`MGHostSpan` 的四行消费者表在 §3.5.7）：字节永远走 `SEG_STAGE`，指针永不过线；`minIndex/maxIndex` 是 flag 门控的 `MGPDrawInfo` 字段；reconcile 是**逐站点**表而不是一条笼统规则（`*IndirectCount` 明确**不**加 `SyncGpuWrites()`）。实现落在 `MG_Impl/Pipe/HostResolve.cpp`，两个 backend 共用。

`draw_vbo` 的 `kIndicesAreClient` 标志由"是否绑定了 element array buffer"决定（`DirectGLES.cpp:4423` vs `:4425-4442`），在 binding 所在的一侧判定。

### 7.10 server 侧索引宿主镜像（D-B7）

`MG_Remote/Server/IndexHostMirror.{h,cpp}`：

- **覆盖范围**：`MGPResourceDesc::bindMask & ELEMENT_ARRAY` 的资源，且仅当 `kCapNeedsHostIndexBytes` 为真（即 split 且 server 侧确实需要索引字节做 restart 重写 / multi-draw 展平）。
- **维护方式**：由 server 本来就要收的 `resource_create` / `resource_respecify` / `resource_subdata` 流**增量**维护。**零额外线上流量、零 round trip。**
- **可见性**：GPU 写者对镜像的影响由 `on_gpu_written` 的收窄集在 server 侧本地判定（server 知道自己提交了什么），不需要问 client。
- **预算**：`MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），逐帧发布 `index-mirror-bytes`。**超预算时该 buffer 退化**为逐 draw 通过 `MGHostSpan` 传送（`seg` 指向 `SEG_STAGE` 而不是 `kFromServerIndexMirror`），并计入 `index-bytes-shipped`。
- **为什么必须是它**：`kMaxRestartRewriteBytes = 1<<26`（64 MiB，`DirectGLES.cpp:4218`）是默认 `SEG_STAGE` 的两倍，`kMaxFlattenedIndices = 1<<24`（`MultiDraw.cpp:72`）同量级；把这些字节逐 draw 塞进 32 MiB 的段既不可行也无必要。

### 7.11 内存预算

| 项 | 字节 | 说明 |
|---|---|---|
| 传输段 | **48.25 MiB** | `SEG_CMD` 8 + `SEG_STAGE` 32 + `SEG_REPLY` 8 + `SEG_EVENT` 0.25 |
| `SEG_STAGE` 额外余量 | **+0～32 MiB** | §7.1.1 的六类新字节实测后定；上限由 P0 计数器给 |
| server 侧**索引宿主镜像**（**仅 split，仅 `kCapNeedsHostIndexBytes`**） | **0～64 MiB（默认上限）** | §7.10；只镜像曾被绑为 ELEMENT_ARRAY 的 buffer，由 subdata 流增量维护，零额外线上流量 |
| 纹素保留 LRU | **默认 0** | `MOBILEGL_PIPE_TEXEL_RETAIN_MB` **默认 0**；只有实测拉取率非平凡才开（§6.5c） |
| POD slot 记录 + CSO 缓存 | ~1-2 MiB | server 侧对象表是数组，不是对象图 |
| **典型（不开索引镜像）** | **≈ +50-60 MiB** | |
| **最坏（镜像满 + stage 余量满）** | **≈ +145 MiB** | |

**诚实注记**：索引宿主镜像是本设计里唯一的"数据副本"，它是把 restart 重写与 multi-draw 分档**留在 server**（D-B7）所付的价钱。它只覆盖索引缓冲、有显式预算与计数器、且超预算时有回退路径（逐 draw 通过 `MGHostSpan` 发送，代价记账）。**server 不持有任何 buffer 的完整副本、不持有任何纹素、不持有前端对象图**——这是"server 拥有自己的状态机"在内存上的直接后果。P5 验收要求**记录两个角色的峰值 RSS**，作为这张表的实测基线。

---

## 8. 控制面与同步

### 8.1 FlatBuffers 用法

**一份 schema `MobileGL/MG_Remote/Protocol/protocol.fbs`，两种用法：**
- **热路径 → FlatBuffers `struct`**（flatc 保证定长布局、无 vtable、无偏移间接、无需 verifier walk，只需边界检查），直接放进 ring：`[RecHeader | struct | 可选变长尾]`。`draw_vbo` 的固定头是 8+**56** = **64B**（**P0 实测修正**：`MGPDrawInfo` 的 `sizeof` 是 56 而不是 48，见 §3.5.7 的实测布局表；对比 table-per-command 的 ~90B 与一次 vtable 遍历）。这正是 `Feat/CS-Delta-IPC` 自己的 plan 第 55 行要求而实现没做的事。
- **罕见/变长/需演进 → FlatBuffers `table`**，走 CTRL socket。

```fbs
namespace MobileGL.Wire;

// ---------- 热路径 struct（进 ring；与 MGPipeTypes.h 的 POD 一一对应）----------
struct PipeHandle  { slot:uint; gen:uint; }
struct BlobRef     { seg:uint; pad:uint; offset:ulong; size:ulong; }
struct HostSpan    { ptr:ulong; size:ulong; seg:uint; pad:uint; offset:ulong; }

struct RecBindRenderState  { cso:PipeHandle; version:ushort; pipelineVersion:ushort; }
struct RecSetDynamicState  { chunkMask:uint; version:ushort; pad:ushort; blob:BlobRef; }
struct RecSetIndexBuffer   { res:PipeHandle; offset:ulong; indexSize:uint; restartIndex:uint; }
struct RecResourceSubData  { res:PipeHandle; target:ushort; level:ushort; flags:uint;
                             box:[uint:6]; regionCount:uint; pad:uint; blob:BlobRef; }  // regions 在变长尾
struct RecDrawVbo          { mode:uint; indexSize:ubyte; flags:ubyte; pad:ushort;
                             instanceCount:uint; startInstance:uint; restartIndex:uint;
                             indexResource:PipeHandle; minIndex:uint; maxIndex:uint;
                             xfbCaptured:ulong; }                    // ranges/HostSpan 在变长尾
struct RecPresent          { frameSerial:ulong; swapInterval:int; pad:uint; }
struct RecRenderbufferStorage { res:PipeHandle; internalFormat:uint; width:int; height:int;
                                samples:int; pad:uint; }
// … 共 68 项（P0 实测），与 PipeCalls.def 逐条对应 …

// ---------- 控制面 table（走 socket）----------
table SegmentRef { id:uint; kind:ubyte; sizeBytes:ulong; name:string; }
table Hello   { abiMajor:uint; abiMinor:uint; buildFingerprint:string; backendType:uint;
                pid:uint; configBlob:[ubyte]; }
table Welcome { abiMajor:uint; abiMinor:uint; serverPid:uint;
                cmdRing:SegmentRef; stageRing:SegmentRef; replyPool:SegmentRef; eventRing:SegmentRef; }
table CapsSnapshot { dynamicParameters:[ubyte];        // DynamicBackendParameters 逐字节
                     rendererInfo:[ubyte]; formatCaps:[ubyte]; extensions:[string];
                     apiVersion:string;
                     maxComputeWorkGroupCount:[int:3]; maxComputeWorkGroupSize:[int:3];
                     callMask:ulong;                   // 远端实际填了 MGPipe 的哪些槽
                     capBits:ulong; }                  // kCapNeedsHostIndexBytes 等
table SurfaceInfo  { width:int; height:int; colorFormat:uint; depthFormat:uint; stencilFormat:uint; }
table SurfaceOp    { seq:ulong; kind:ubyte; display:ulong; surface:ulong; windowKind:ubyte;
                     nativeToken:ulong; width:int; height:int; swapInterval:int; }
table SurfaceReply { seq:ulong; ok:bool; eglMajor:int; eglMinor:int; info:SurfaceInfo; }
table ResyncRequest { serverEpoch:uint; }  table ResyncDone {}
table AuxRequest   { seq:ulong; kind:ubyte; payload:[ubyte]; }   // 外来线程 sync/query
table Fatal   { code:uint; message:string; }
table LogLine { level:ubyte; text:string; }
union CtrlMsg { Hello, Welcome, CapsSnapshot, SurfaceOp, SurfaceReply,
                ResyncRequest, ResyncDone, AuxRequest, Fatal, LogLine }
table CtrlEnvelope { msg:CtrlMsg; }
root_type CtrlEnvelope;
```

**两份定义不可能漂移**：G3 为每条记录生成 `static_assert(sizeof(MobileGL::Wire::Rec*) == sizeof(MGP*))` 与逐成员 `offsetof` 断言，把 fbs `struct` 与 `MGPipeTypes.h` 的 POD 钉在一起（§7.3）。

`protocol_generated.h` **提交进仓库**，由 `scripts/gen_protocol.py` 重新生成（镜像 `tools/trace_replay/CMakeLists.txt:52-69` 驱动 `glproc.py` 的做法）；CI 加 `flatc-check` 步骤重新生成并 `git diff --exit-code`。

**codegen 绝不进默认构建图**：`Feat/CS-Delta-IPC:MobileGL/Protocol/CMakeLists.txt:22-38` 在 `MOBILEGL_FLATC_EXECUTABLE` 未设时 `add_subdirectory(3rdparty/flatbuffers)` 并开 `FLATBUFFERS_BUILD_FLATC ON`——这正是它自称要修的 NDK 陷阱（交叉编译造出 arm64 `flatc` 然后在 host 上执行）。**本计划不复用这一段**：`gen_protocol.py` 是纯开发者/CI 目标，默认构建图里没有 `flatc`，`MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`。FlatBuffers 运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上（用 `nm` 复核 `libMobileGL.so` 链接行没有新增库，不靠断言）。

### 8.2 帧封装与 publish 策略

CTRL socket 封帧：`[u32 'MGLF'][u32 len][payload]`，64MiB 上限，**读时校验**（`Feat/CS-Delta-IPC` 的 `Feed()` 永远返回 OK，坏 magic 变成静默永久挂起，`Framing.h:41-45`；`StartRead` 直接按 wire 长度分配无上限检查，`LocalSocketTransport.cpp:232-236`）。接收缓冲不足时**返回所需大小并保留消息**（那份 transport 会失败且不弹出消息，把流永久卡死）。

#### Publish 触发器

**不设"records ≥ 64KiB"这类阈值。** 按 §7.3 的记录尺寸，64KiB ≈ 1200-2700 条记录，即**一整帧**（MC 帧是 1000-4000 draw）。那意味着 server 在 client 发完整帧之前无法开始工作——这不是异步，是一个整帧的流水线气泡，且在 present credit 之上再加一整帧延迟；它还会在 `inproc` 跑之前就先把 `inproc` 的假设否掉（`inproc` 的全部意义就是让 apply 与 GL 线程重叠，帧粒度 publish 保证零重叠）。而 `SEG_CMD` 是 SPSC ring，"publish" 只是一次 `cmdHead` 的 release store，唯一值得摊销的是门铃写。

**规则**：
- **每条记录（或每 8-16 条，用来摊销 store）release-store `cmdHead`**；仅当 `consumerParked` 时敲门铃。
- 显式门铃点：`present`、任何 `kNeedsAck` 阻塞请求、`eglMakeCurrent`、`glFlush`（**刷出 outbox，不等待**）。
- **`SEG_STAGE` 余量 < 1/4** 时敲门铃（用 `stageHead - stageAppliedTail`）。
- **轮询类入口点也是门铃点（修 livelock）**：`glClientWaitSync`（任意 timeout）、`glGetSynciv(GL_SYNC_STATUS)`、`glGetQueryObject*(GL_QUERY_RESULT_AVAILABLE | GL_QUERY_RESULT_NO_WAIT)`。
  理由：GL 的标准惯用法是 `glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 与 `while (!avail) glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &avail);`。循环里没有别的 GL 调用，若这些入口不 publish，`fence_create` 就永远躺在 ring 里，server 看不到，watermark 不动，循环永久自旋——这是挂死，不是变慢。仓库自己在意这件事：`DirectVulkan.cpp:1158-1160` 写明 "GL_SYNC_FLUSH_COMMANDS_BIT: flush regardless of timeout, so a zero-timeout poll loop makes progress across calls"，而 MG_Impl 无条件把 flags 透传给 backend（`GL_Sync.cpp:96`）。
  **携带 `GL_SYNC_FLUSH_COMMANDS_BIT` 的调用无条件 publish**（spec 要求 flush）。
- **饥饿升级**：同一个 handle 连续 N 次（默认 64，`MOBILEGL_IPC_POLL_ESCALATE`）本地回答 `TIMEOUT_EXPIRED` / "未就绪" 而 watermark 毫无移动时，升级成一次阻塞 round trip，这样一个已经卡住的 server 不会把 client 自旋成死循环。

**`glFinish`/`glFlush` 保持纯 no-op**（`Definitions.cpp:111-112`）——应用唯一的强制停顿手段在 monolith 里免费，拆分后也必须免费。

### 8.3 序号与 credit

seq = 记录序数。**两个互相独立的窗口，绝不是 per-batch 锁步**（`Feat/CS-Delta-IPC` 在 apply 循环里同步发 ack，`ServerCore.cpp:421-429`，是最差的节奏；而且它的 credit 算成 `baseSeq + items.size()`，只有 `baseSeq==0` 时才对）：

- **字节 credit**：`SEG_CMD` 与 `SEG_STAGE` 各自的占用，升级路径见 §7.5。
- **Present credit**：`eglSwapBuffers` 在 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（**默认 1**，见 §9.1）时阻塞。

server 端**不发 credit 消息**：它对 `RingControl` 做 release store，consumer 每 64 条记录更新一次 `appliedSeq`，并在 `producerParked` 时敲反向门铃。

### 8.4 事件回传通道

`SEG_EVENT` 是 server→client 的 SPSC POD ring，承载 §6.1 的十个回调加回读完成通知：`EvQueryResult{handle, available, value}`、`EvFenceSignaled{handle}`、`EvGpuWritten{handle, rangeCount, ranges[]}`、`EvBufferWriteback{handle, offset, BlobRef}`、`EvTextureWriteback{handle, box, BlobRef}`、`EvTexturePullRequest{handle, target, firstLevel, levelCount, pullSerial}`、`EvMipLevelsGenerated{handle, base, count}`、`EvXfbScatterReady{handle, packedStride, vertices}`、`EvReadbackDone{seq, BlobRef}`、`EvGlError{code}`、`EvSurfaceChanged`、`EvCapsInvalidated`、`EvLogLine{level,len,text}`。

#### 排空点

client 在下列位置排空：`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`glGetSynciv`、`eglSwapBuffers`、**`glMapBuffer` / `glMapBufferRange` / `glGetBufferSubData` / `glGetNamedBufferSubData` / `glCopyBufferSubData`**，以及**每一次等待循环的每一轮**（present credit、`kNeedsAck`、ring/stage 满）。最后一条是必须的，见下。

#### 溢出策略（修一个双向死锁）

具体死锁：client 卡在 `eglSwapBuffers` 等 present credit；server 的 apply 线程一边 apply 一边产 `EvLogLine` 与 `EvGpuWritten`；`SEG_EVENT` 满；apply 线程阻塞在生产上；`presentAckSerial` 永不前进；client 永不离开 `eglSwapBuffers`，因而永不排空。两边都死。

**策略**：
1. client **必须**在每个等待循环内排空 `SEG_EVENT`，不只是在入口点边界。
2. **`EvLogLine` 按严重级分级**（§6.4 的强制条款）：`level ≤ WARN` 是**有损**的——覆盖最旧，并累加 `RingControl.eventDropped`（client 在排空时把丢失条数打进日志）；丢一条 INFO/WARN 绝不允许卡住渲染。
3. **语义承载事件无损**：`EvGpuWritten`、`EvReadbackDone`、`EvFenceSignaled`、`EvBufferWriteback`、`EvTextureWriteback`、`EvTexturePullRequest`、`EvMipLevelsGenerated`、`EvXfbScatterReady`、`EvGlError`、`EvSurfaceChanged`、`EvCapsInvalidated`，**以及 `EvLogLine{level ≥ ERROR}`**（因为 backend program link 失败只以一行 ERROR 日志呈现，§4.7）。ring 装不下时 server 置 `RingControl.eventRingFull=1` 并**停止 apply**（停在一条记录的边界上，不是记录中间），敲反向门铃；client 排空后清标志并敲正向门铃。状态因此永远可恢复。
4. **ERROR 速率限制器**：每秒上限，超限时发一条显式的 "N errors suppressed"，避免无损化把 ring 变成死锁源（B-R13）。`MGLOG_E_ONCE` 的 latch 变成 per-server。
5. 故障注入测试：在 client 被 credit 阻塞时灌满 `SEG_EVENT`；以及日志洪泛下注入一次 backend link 失败，那行 ERROR 必须出现**且**两侧都恢复。

server 侧的 `MGLOG` 与延迟诊断按流顺序 replay 进 client 日志流——复用已存在的 `DeferredLogLine`/`ApplyDeferredDiagnostics` 机制（`JobNode.h:26-58,149-158`）。

### 8.5 fence 完成度必须来自真的逐 fence 退休，不是 present 水位

一个诱人的简化是让 `retiredSeq`/`completedFrameSerial` 兜底 fence 语义。**不行。** 在 DirectGLES 上这两个水位**只在 `Present()` 里前进**（`DirectGLES.cpp:10626-10643` 在 `eglSwapBuffers` 之后轮询 4 深 fence ring），或在 `WaitForFrameSerialCompleted`（`:10583-10607`）里。帧中创建的 fence 于是要等到**下一次 present 退休**才报 signalled，即 fence 完成度退化成帧计数推断。`DirectVulkan.cpp:1120-1128` 恰恰写明这是被修掉的 bug：完成度必须"track the GPU itself rather than the frame-count inference; MC 1.21.5's fence-paced ring buffers depend on this to recycle their space instead of growing without bound"，而项目记忆 `magma-mc1215-fence-oom` 记录了它曾导致 native-heap OOM kill。

**规则**：`fence_create` 在 server 侧转成一次**真实的 backend `FenceSync()`**；server 用自己已有的逐 fence 轮询（DirectGLES 有 `WaitForFrameSerialCompleted` 的 fence 选择逻辑 `:10586-10600` 可复用；DirectVulkan 有 `IsSubmitIndexComplete`）在**非 present 时刻**也推进，并发 `EvFenceSignaled{handle}`。client 的本地快路径读的是"由真实逐 fence 退休导出的 handle 水位"，不是 present 水位。

### 8.6 三个应先独立落到 `dev` 的 monolith 修复（可二分、monolith 自身受益）

1. `glEndTransformFeedback` 的无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`）→ 用既有 `MarkGpuWritten`/`SyncGpuWrites` 推迟到首次读。
2. `glDispatchCompute` 的三次 `GetIntegeri_v` 校验查询（`GL_Drawing.cpp:719`）→ 改读 `CompileEnv::maxComputeWorkGroupCount`（`CompileEnv.h:52-54`）。
3. ~~删除 `GetInteger64i_v`/`GetProgramiv` 两个死表项及两个 backend 的实现。~~ **（P0 实测修正）已在 P0 落地**（提交 "retire the two frontend queries that were never asked"）。

（另有两项在 §13.4-5 列出：D21 的 XFB 计数槽重键与 `RenderbufferObject::GetLifetimeId()`，同样先独立落 `dev`。）

---

## 9. Present 与帧节奏

`eglSwapBuffers` → `EGLImpl::SwapBuffers`（`EGLImpl.cpp:162-183`）→ `BackendObject::SwapEGLBuffers`（`BackendObject.cpp:369-398`，其线程归属校验全部对 client 镜像的 EGL 状态求值，**不需要回复**）→ 发 `present{frameSerial}`（swap interval 搭在同一条记录上）→ publish + 敲门铃 → 返回，除非 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`。

**`present` 与应用的 `eglSwapBuffers` 严格 1:1，绝不批量。** Magma 侧四次 `OnFrameBoundary()` 缓存老化、`TryDrainFrameTransients` 和全部四次 `BeginFrame` 只在 `Present` 内发生（`VulkanRenderer.cpp:12765-12904`）；Espryt 侧三个 ring 与 `TrimBufferPool` 在那里 retire（`DirectGLES.cpp:10646-10649`）。批量会饿死这些排空。

### 9.1 延迟是叠加的：credit 默认为 1

一个"credit=2 镜像系统已有预算、因此不引入新的停顿类别"的论证是错的：停顿**类别**确实不新，但**延迟会叠加**：

- server 自己的 `Present` 在返回之前就已经等了 2-3 帧：`VulkanRenderer::Present` 末尾调 `FrameContext::WaitAndAcquireNextImage`，其第一条语句是 `vkWaitForFences(device, 1, &frame.imageInFlightFence, VK_TRUE, timeout)`（`FrameContext.cpp:288-290`）。`presentAckSerial` 因此只能在那次等待完成后才前进。
- 一个被允许领先 2 个 present 的 client，叠在一个自身已领先 GPU 2-3 帧的 server 上 = **端到端 4-5 帧**，60Hz 下 66-83ms，对第一人称游戏不可接受。
- 现有的验收门都看不见它：SSIM 是帧内容比较，`bench.sh` 量的是 FPS，都不是 input-to-photon。

**规则**：`MOBILEGL_IPC_PRESENT_CREDIT` **默认 1**（可配 1-4）。文档里写明叠加公式：`端到端 ≈ client credit + server FIF + 驱动深度`。P10 与 P12 的验收增加**输入延迟测量**：用已有的 `GetGpuTimestampNs` 与 trace-replay `--benchmark` 的逐帧 JSON 构建 "记录发射时刻 → present 完成时刻" 直方图；只有当实测吞吐收益能抵掉实测延迟代价时才调高 credit。

参考基线：`MagmaFramesInFlight = 3` 钳到 `[2, maxImageCount]`（`VulkanRendererConfig.h:14-19`、`VulkanRenderer.cpp:3051-3058`），Espryt 深度 4 的 fence ring 刻意高于驱动的 2-3（`DirectGLES.cpp:10071-10074`）。

### 9.2 swap interval 与 Magma

Swap interval 搭 `present` 记录过去。注意 Magma 从不注册 `SetSwapInterval`（`BackendObject_DirectVulkan.cpp:698` 只注册 `Present`，所以 `set_swap_interval` 在 Magma 上是 null 项）且偏好 `MAILBOX`/`IMMEDIATE`（`SwapchainObject.h:74-79`），因此 **IPC credit 成为 Magma 唯一的显式限帧器** —— 记录在案，P10/P12 在设备上测量输入延迟与帧节奏；若 Magma 需要，把"注册 `SetSwapInterval` 并映射到 FIFO"作为**独立的 `dev` 变更**，不让两套机制同时管节奏。

### 9.3 无 present 循环下的水位饥饿

`retiredTail` 的回收依赖 server 发布准确的 `completedFrameSerial`。DirectVulkan 有 `TryDrainFrameTransients`/`RefreshCompletedSubmits` 可以在非 present 时刻推进，**DirectGLES 没有对应物**：`g_completedFrameSerial` 只在 `Present()` 里（`DirectGLES.cpp:10626-10643`）和 `WaitForFrameSerialCompleted`（`:10583-10607`，且要求存在覆盖目标 serial 的活 fence，slot 被回收时返回 false）前进。在无 present 的负载里——`tools/cts` 的 `run_cts_local.py`、回读循环、从不 swap 的 `MG_IntegrationTest` 场景——一个 fence 都不会被插入，`retiredTail` 永不前进，`SEG_STAGE` 填满，§7.5 的升级路径在每个用例上都跑到硬 drain。那会把一次 CTS run 变成一连串 50ms 等待加整体 drain，并可能被误读成一致性回归。

**规则**：给 DirectGLES 的 server 加**非 present fence tick**——距上次 `Present` 超过阈值（默认 8ms）或每 N 条已 apply 记录（默认 4096）时，插入一个 `glFenceSync` 并轮询 fence ring，复用 `g_frameFenceRing` 机制。同时把 ring 占用率与升级次数打进 Tracy 计数器（P0 交付），让"水位饿死"表现为一个指标而不是一次无法解释的停顿。P8 增加一个无 present 的 split 用例。

---

## 10. 线程模型

### Client
- **v1 不加线程。** 编码在调用方 GL 线程上直接写进 ring。前端本来就是 per-context 单线程契约（`GLContext` 无 mutex；`EGLState::MakeCurrent` 强制一个 owner 线程，`EGLState/Core.cpp:1215-1220`，测试在 `MG_Test/EGLState/EGLStateTest.cpp:39-92`）。
- **flow = per context，不是 per thread。** 今天恰好一个 flow。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射。**顺手修既有漏洞**：`EGLImpl::ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）今天不取该锁而另外三个（`MakeCurrent`/`SwapBuffers`/`DestroySurface`）取。
- **外来线程的 sync/query**：读全部从 `RingControl` 无锁 acquire load 回答（比取 registry mutex 更好）；少数必须发射的（`fence_create`、`query_begin`，以及 §8.2 要求的轮询 publish）取 `ctrlMutex` 并走 CTRL socket 的 out-of-band `AuxRequest` 帧（SPSC ring 不允许第二个 producer）。
- **等待必须能挂起**：所有 client 侧等待（present credit、`kNeedsAck`、ring/stage 满、轮询升级）走 §7.2a 的 `producerParked` + 反向门铃，自旋窗口 `MOBILEGL_IPC_SPIN_US`（默认 50µs）。
- ShaderCompilePool 原样保留在 client（`ShaderCompilePool.h:77-82`，≤4 worker，为 RSS 上限）。glslang 全在 client，`create_shader_state` 从编译池的终止 continuation 发出（§4.3）。
- 可选 `mgl-client-tx` 双缓冲发送线程：**凭测量决定**。在 Tracy 数据出来之前不要预先加线程（会引入拷贝或锁）。

### Server
| 线程 | 职责 |
|---|---|
| `mgl-srv-io` | asio `io_context::run`：封帧读写、`SCM_RIGHTS`、双向 doorbell、CTRL RPC |
| `mgl-srv-apply` | **终身持有原生 EGL/Vulkan context**：消费 ring → 解码 → 更新 MGPipe 对象表与 `PipeInputs` → 调 backend 函数表 |
| `mgl-srv-dec`（可选） | 边界校验/解码前置，凭测量决定 |

因为 context 永不迁移：`g_backendContextOwnerThread`（`DirectGLES.cpp:10052`）只写一次；`DirectGLES::MakeCurrent` 的 8 缓存失效风暴（`:10123-10140`）变成启动期一次性成本；`IsBackendContextCurrentOnThisThread` 的每帧 EGL 复核（`:10195-10228`，动机是 `eglGetCurrentContext` 实测占渲染线程 16%）恒真。DirectGLES 的 off-thread 降级（`FenceSync` 返回 null 等）消失——**保真度提升**。延迟 replay 机制（`Managers.h:458-473` 的 `pendingRespecify`/`pendingRanges`/`pendingResidentWrites`）保留但永不触发。

### 核心放置（是性能主张的前提）

§13.2 说明推送模型把可达性遍历**搬走**而不是翻倍：client 的 tracker 做 O(1) 快门加未命中时的 touched 前缀走查，server 做解码加 backend 调用。**但那仍然是 CPU 工作，只是换了线程**，而且 client 侧新增了 payload 构造与集合 hash。所以拆分的全部性能主张都押在"两半落在两个都快的核上"。

而 MobileGL 全库从不设置亲和性（`grep -rn 'sched_setaffinity\|cpu_set_t\|affinity' MobileGL/` 零命中），server 是 fork/exec 出来的独立进程、不继承 launcher 的亲和性，项目记忆 `pojav-bigcore-affinity-trap` 又记录过 `pojavBigCore=true` 把整个游戏 JVM 加 MobileGL worker 钉死单核、让一整批历史测量作废。若 `mgl-srv-apply` 落到 1.55GHz 小核，它做的工作严格多于 monolith 在 1.96GHz 大核上做的，拆分按构造就是回归，而"帧时在 monolith 10% 内"会以一个没人会正确归因的理由失败。

**规则**：
1. 计划里必须写出**总 CPU 工作量差**（client tracker + encode + decode + server apply  vs  monolith 的 `PrepareForDraw`），不只是单侧成本。
2. 复用 `ShaderCompilePool` 已有的大核探测（`ShaderCompilePool.cpp:73-96` 的 `ReadCpuMaxFrequencyKHz` / `DetectBigCoreCount`）把 `mgl-srv-apply` 绑到大核，开关 `MOBILEGL_IPC_SERVER_AFFINITY`（默认 auto），并把解析出的 mask 打进日志。
3. 每个阶段都必须报**逐线程 CPU 时间**，不只是墙钟帧时，这样"没有收益"的结论能被归因到放置 vs 编码成本。

### 拆机顺序（三条约束）
publish + server 排空并 ack → 停 apply 线程 → 关 transport →（client）排空 compile pool（必须先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构，`ShaderCompilePool.h:106-110`、`Init.cpp:56-62`）→ `MobileGL::Destroy()`（`EGLImpl.cpp:335-338`）→ 释放 sync/query handle（`GL_Sync.cpp:223-226`）。

---

## 11. EGL/窗口与进程生命周期

### 11.1 启动与握手

client 定位 server 的顺序：
1. `MOBILEGL_IPC_SERVER_PATH`（**主要机制**）。
2. `dladdr(&MobileGL::Initialize)` → dirname → `libMobileGLServer.so`（**兜底**）。

把 `dladdr` 当主要机制会让两个桌面验收门都找不到 server：`MG_IntegrationTest/CMakeLists.txt:28-35` 在非 Android 上把 `MGL_ITEST_MOBILEGL_TARGET` 设成 `MobileGL_s`（**静态链接**），`dladdr` 解析到测试可执行文件自身的路径而不是库目录；trace replay 则由 `tools/trace_replay/CMakeLists.txt:285-290` 显式传 `-DMOBILEGL_LIBRARY=$<TARGET_FILE:MobileGL>`，其目录是 MobileGL 的构建输出目录，而 CMake 默认把 `add_executable` 放在定义它的目录的 binary dir。

**配套**：把 `MobileGLServer` 的 `RUNTIME_OUTPUT_DIRECTORY` 设成 `$<TARGET_FILE_DIR:MobileGL>`，并把 `"MOBILEGL_IPC_SERVER_PATH=$<TARGET_FILE:MobileGLServer>"` 加进每一条新的 ctest `ENVIRONMENT`（经 `mgl_itest_join_environment` 与 `${MGL_ITEST_COMMON_ENV}` 合并）以及 `add_trace_replay_test` 的 `SPLIT` 分支。**并复核绝对路径能否活过 CI 的 artifact 搬运**：`.github/workflows/test.yml:174-185` 只重写 `CTestTestfile.cmake` 里的 `cmake` 路径，不重写 `ENVIRONMENT` 值——若不行，改为在测试启动时由 harness 相对 `argv[0]` 解析。

启动方式：`socketpair(AF_UNIX, SOCK_STREAM)` + `fork`/`execve`，fd 3 = socket（Windows 见 §11.5）。**无文件系统 socket 路径、无 abstract namespace、Android 上无 SELinux 争议。**

**子进程必须被强制成 monolith（修无界 fork 链）**：`MG_Config::Transport` 由 `ConfigLoader` 从环境变量读（与 `features.CoherentAsFlush = QueryEnvFlag(...)`（`ConfigLoader.cpp:185`）同形），而 `fork`/`execve` 的子进程会继承 `MOBILEGL_TRANSPORT=spawn`。server stub 里 `dlopen(libMobileGL.so)` + `dlsym("mobilegl_server_main")` 之后必然要起一个真 backend，即走 `MG_Backend::Init()`（`Init.cpp:48-70`）——变量还在，于是它再构造一个 `BackendObject_Remote` 并再 spawn 一次，首次 GL 调用时形成无界 fork 链。
**规则**：(a) spawn 时构造**显式 envp**，剔除 `MOBILEGL_TRANSPORT` 与所有 `MOBILEGL_IPC_*`（只保留 server 真正需要的少数几个，如 `MOBILEGL_BACKEND_TYPE`、日志路径）；(b) `mobilegl_server_main` 在能到达 `MG_Backend::Init()` 之前把 `MG_Config::Transport` 硬置为 `Monolith`。两条都做，任一条单独失效时另一条兜住。P0 增加一个 `MG_Test/Wire` 测试：spawn 一个 server 并断言进程树只多出**恰好一个**子进程。

`Hello{abiVersion, backendType, buildFingerprint, configBlob}` → `Welcome`。`configBlob` 转发 client 解析好的 `MG_Config::Features`，两半不可能对某个 quirk 开关有分歧。`buildFingerprint`（git hash + `PipeCalls.def` 的 hash）不匹配 → 握手期 `Fatal`。

### 11.2 `mobilegl_server_main` 的可见性

`CMakeLists.txt:497-510` 在**非 Debug** 构建上给共享目标设 `C_VISIBILITY_PRESET hidden` / `CXX_VISIBILITY_PRESET hidden` / `VISIBILITY_INLINES_HIDDEN ON`——而 plugin 与 FCL 出货的正是 RelWithDebInfo（`MobileGL/build.gradle` 的 `fordebug` 类型强制 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`）。所以 `dlsym("mobilegl_server_main")` 在 Debug 下能用、在设备上静默失败。

**规则**：入口点声明为
```cpp
extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv);
```
并在 P0 验收里加 `nm -D libMobileGL.so | grep mobilegl_server_main` 断言（与既有的 `nm --defined-only` 门并列）。若哪天 macOS/Windows 也要托管 server，还需同步 `MG_Impl/DyldInterpose/ExportedSymbols.txt` 与 `wgl.def`。

### 11.3 Android

**minSdk 26 没有任何公开 NDK API 能扁平化 `ANativeWindow`**（NDK r27.3 的 `android/native_window.h` 无 parcel 符号；`libbinder_ndk` 是 API 29，`binder_ibinder.h:191`；`ASurfaceControl` 是 API 29，`surface_control.h:67`）。`Feat/CS-Delta-IPC` 的 `nativeBlob` "binder-flattened ANativeWindow"（`protocol.fbs:377-379`）不可实现。

- **P5-P11 验证路径：无窗口。** 两个 PIE ELF。**实测**：从解压出的 nativeLibraryDir exec 在 API 36 上可行（`run-as … libtrace_replay_runner.so` → exit 132 = SIGILL，即 ELF 已被加载进入，而非 `EACCES`；文件 0755 / `u:object_r:apk_data_file:s0` 且无 MLS category，**跨 package 也可**）。`useLegacyPackaging = true` 在 FCL（`../FCL/build.gradle.kts:76-82`）与 plugin（`android-plugin/app/build.gradle.kts:198-203`）都已开。surface 用 pbuffer 或 `AImageReader` 支持的 `ANativeWindow`（`HeadlessGL.cpp:86-131,268-274`），trace replay 默认 pbuffer（`apitrace_glws_egl.cpp:614-618`）。
  **注意实测的域**：上述 SIGILL 证据是经 `run-as` 取得的，即 `runas_app` 域，而不是 trace Activity 所在的 `untrusted_app` 域。**P0 的 Android spike 必须从应用自身进程 spawn 一次**（见 §14 P0）。**（P0 实测修正）不能用 `posix_spawn`**：bionic 从 API 28 才声明它，minSdk 26 下出货的那条臂是 **`fork` + `execve`**；而且应用进程的 stdout/stderr 是 `/dev/null`，子进程要用 **marker 文件**而不是日志来证明自己活过。真机 exec 本身仍待验证（设备锁）。
- **P12 生产路径**：Java `Surface`（Parcelable）→ Messenger/AIDL → `MobileGLServerService`（`android:process=":mgl"`）→ JNI `ANativeWindow_fromSurface(env, surface)`，就是 FCLauncher 今天在 `egl_bridge.c:81` 做的那一次调用。**仓内先例**：`android-plugin` 的 `BenchService` 已在 `android:process=":bench"` 里跑 MobileGL（`BenchService.java:19-77`）。代价：server 进程多一个 ART（~15-25MB）。
- **纠正一条过期笔记**：FCL 把游戏 JVM 跑在**主进程**，不是 `:jvm`（`../FCL/src/main/AndroidManifest.xml:112-121`，`JVMActivity` 没有 `android:process`；`:jvm` 是下载 Service）。第二个进程必须新建。
- **HeadlessGL 的 fork 预检与孤儿 server**：`MG_IntegrationTest/Harness/HeadlessGL.cpp:344-368` 会 fork 一个子进程跑完整 EGL bring-up 然后 `_exit(step)`，注释（`:364-366`）明说这是刻意的——"every atexit handler and static destructor in this address space belongs to the parent's copy of the world"。拆分模式下那个子进程的 bring-up 会走到 `MG_Backend::Init()` 并 spawn 一个 server；`_exit` 不跑任何拆机，那个 server 成为孤儿，活到它发现 EOF 或撞上 `MOBILEGL_IPC_IDLE_EXIT_S`（默认 30s）。父进程随即对同一设备起自己的 server。`HeadlessGL.cpp:585-589` 已经把这种失败模式命名为"a leaked exclusive device, an environment the child did not have"。
  **规则**：server 的 EOF 检测必须**即时且无条件退出**（亚秒级，不靠 30s 看门狗）；client spawn 时把 socket fd 设成 `_exit` 会确定性关闭的形态（不设 `FD_CLOEXEC` 以外的保活）；再加一次**有界重试的就绪握手**，这样残留的预检 server 不会把父进程弄 flaky。这个交互本身列为 P6 验收步骤的一部分，先于任何广度工作。

### 11.4 Linux / X11

`Window` 是 XID，`nativeToken:u64` 直接送。backend 自己 `XOpenDisplay(getenv("DISPLAY"))` 并构造 `VkXlibSurfaceCreateInfoKHR`（`VulkanRenderer.cpp:14486-14521`），只要同 `DISPLAY`/`XAUTHORITY` 就免费。Wayland 今天不支持（`BackendObject.h:529` TODO），维持。
WSL/CI：**永不开窗** —— `EGL_PLATFORM=surfaceless` + `EnsureHeadlessPlatform()`（`HeadlessGL.cpp:160-196`，它存在正是因为一台带 WSLg `DISPLAY` 的工作站曾把这条 lane 弄挂）。

### 11.5 Windows

`HWND` 进 `nativeToken`。Vulkan 可行（`hinstance` 是历史遗留，`VulkanRenderer.cpp:14456-14463`）；**WGL/ANGLE-DXGI 对外进程 HWND 不受支持 → headless only**。

transport：默认 named pipe（asio `windows::stream_handle`）。**"继承句柄就免掉 accept/connect"这句在 asio 上不能直接照搬**：`windows::stream_handle` 的 IOCP 服务要求句柄是 **overlapped** 的，而 `CreatePipe` 造的匿名管道不是。所以句柄对必须这样造：用一个 GUID 唯一命名的 `CreateNamedPipeW(..., FILE_FLAG_OVERLAPPED)` 做 server 端，配一次 `CreateFileW(..., FILE_FLAG_OVERLAPPED)` 做 client 端，然后把 server 端句柄设为可继承并 `CreateProcess` 传下去。

asio 1.38.2 在 Win32 上确实定义了 `ASIO_HAS_LOCAL_SOCKETS`（`3rdparty/asio/asio/include/asio/detail/config.hpp:1085-1092`，只排除 `ASIO_WINDOWS_RUNTIME`，且自带 `sockaddr_un_type` 于 `socket_types.hpp:220`），但其 IOCP `async_accept` 走 `AcceptEx`，AF_UNIX 从不支持它——AF_UNIX-everywhere 是一个**可选简化**，需真编真跑验证，named pipe 是已知可用的默认。

### 11.6 崩溃

- **server 死**：client 读到 EOF/EPIPE → device-lost 闩锁：后续 GL 调用变 no-op、`eglSwapBuffers` 返回 `EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus`（若 robustness 分支落地）返回 `GL_UNKNOWN_CONTEXT_RESET`。`MOBILEGL_IPC_RESPAWN=1` 时重启并让 tracker 把全部 dirty 位置为"必须重推"、对每个活的 handle 重发 `resource_create/respecify` 与全部 CSO（默认关，静默重启会掩盖 bug；且与 `MOBILEGL_IPC_ADOPT_TIER != 2` 互斥，因为被采纳的 store 是 server 拥有的内存，见 §7.8）。
- **client 死**：server 读到 EOF → **立即**销毁原生 context 并退出（不等看门狗）；`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作为 EOF 都收不到时的最后保险。

---

## 12. Roundtrip 清单与稳态零 roundtrip 论证

### 12.1 稳态零 roundtrip 的项

| 类 | roundtrip | 依据 |
|---|---|---|
| 全部 draw、clear、blit、copy、dispatch、barrier、XFB 跨度标记、全部 bind、全部 CSO create/bind、全部 `set_*`、全部 buffer/texture 上传、`present` | **0** | 单向记录；present 只查 credit |
| **全部 89 个 caps 站点** | **0** | 首次 `MakeEGLCurrent` 的一次 `MGPCaps` 快照（`BackendObject.cpp:341-347`，每次 surface 变更重新武装 `:301`）；`callMask` 精确复现 DirectVulkan 少注册的槽位 |
| `glGetError` / `glFinish` / `glFlush` | **0** | 前者永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`），后两者是彻底的 no-op（`Definitions.cpp:111-112`）**且必须继续免费** |
| fence 与 query 的**创建**，以及每一次**非阻塞轮询** | **0** | handle 由 client 铸造；未命中合法地答 `GL_UNSIGNALED`/"未就绪"（`BackendObject.h:210-214`、`:236-241`；前端已遵守，`GL_Query.cpp:302-311`） |
| `glGetTexImage` / `glGetTextureImage`（**DirectGLES**），**包括 GPU 生成的 mip level** | **0** | client shadow 回答（`CopyTextureImageToClientOrPBO_State`，`GL_Texture.cpp:5368-5420`，取用点 `:6460`）。**v2 显式决定**：`on_mip_levels_generated` **只带形状不带字节**，因为 monolith 也是如此——`EnsureGenerateMipmapStorageAllocated`（`DirectGLES.cpp:6243-6274`）对每个新 level 做 `AllocateStorage(...)` + `MarkStorageDirty(..., false)`，**内容留空**。split 因此与 monolith **行为一致**：GPU 生成的 level 在两种模式下都返回已分配但未填充的影子。**只有 CPU 回退生成路径**（RGB16F/RGB32F，`:6811-6861`）产生真纹素，由 `on_texture_writeback` 回来 |
| `glReadPixels` → pack PBO | **0** | fire-and-forget + client 侧 `MarkGpuWritten`。**严格优于 monolith**（`DirectGLES.cpp:9189-9205` 无条件停等） |
| `glEndTransformFeedback` | **0** | 取消无限 fence 等待（`GL_Drawing.cpp:1326-1337`），改为对 capture target 置 `MarkGpuWritten`；scatter 由 §6.2.1 的 client 侧路径完成 |
| `eglSwapBuffers` | **0 次阻塞 round trip**，一次非阻塞 credit 检查 | 只有 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（默认 1）时才阻塞 |
| **`glMultiDrawElementsIndirectCount` / `glMultiDrawArraysIndirectCount`** | **0** | client 从自己的 shadow 解析计数，只做 `SyncPersistentMappedRange()`——**与 monolith 完全相同的 reconcile 集合**（§4.8.1）。**P8 验收要求 `create-indirect` fixture 上该计数器读零** |
| **primitive-restart 重写 / multi-draw 展平** | **0** | server 从索引宿主镜像读（D-B7、§7.10） |

### 12.2 不可避免的阻塞点（全部罕见，逐条给理由与缓解）

| # | 站点 | 为什么不可避免 | 缓解 |
|---|---|---|---|
| 1 | 握手 `Hello`/`Welcome` + 段 fd 传递 | — | 一次 |
| 2 | `InitializeEGLDisplay`、`Create/Resize EGL*Surface`、首次 `MakeEGLCurrent` + `InitCapabilities` | 出参 / 返回 `Bool`；caps 只在那一刻存在 | 每 surface 至多一次；surface 回复顺带 `SurfaceInfo`。`SwapEGLBuffers` 不需要回复（`BackendObject.cpp:365-393` 对 client 镜像的 EGL 状态求值） |
| 3 | `glReadPixels` → 客户内存 | GL 要求返回时字节已就位 | 像素进 `SEG_REPLY` slot；**逐行写回循环留在 server 内，按操作级批成一段** |
| 4 | `glGetTexImage`/`glGetTextureImage`（**DirectVulkan**） | Magma 对只存在于 GPU 的 level 没有 client 可答的 shadow | `get_texture_image` 对"无 GPU 背书"的 level 返回"请从你的 shadow 回答"（`VulkanRenderer.cpp:10691-10704`） |
| 5 | GPU-write pending 的 buffer 首次 CPU 读 | shader 在前端背后写了 store | monolith 里**本来就阻塞**（`Managers.cpp:1246` 的 `glFinish()`；`VkBufferManager.cpp:80-85` → `VulkanRenderer.cpp:9807-9817`）。client 保守 pending 集触发，由 `writableMask` 与 `on_gpu_written{ranges}` 两侧收窄 |
| 6 | `glClientWaitSync(timeout>0)`、`glGetQueryObject*(GL_QUERY_RESULT)` 未完成、`glBeginConditionalRender` | GL 定义即阻塞；`glBeginConditionalRender` 连 `_NO_WAIT` 模式也阻塞（`GL_Query.cpp:705-706`） | 非阻塞兄弟是 0 round trip。条件渲染谓词**只解析一次**（`Core.h:387-391`），之后每个条件 draw 在 client 侧丢弃，**server 永远不需要那个 query 对象** |
| 7 | 分配类入口的 ack | OOM 探测惯用法 | **v2 收窄 +（P0 实测修正）**：**只有 `glBufferStorage`**（真同步）。`glRenderbufferStorage*` 的 OOM 探测惯用法在 41 个 fixture 里出现 0 次（9 次调用 / 5 个 fixture，无一在 3 个调用内跟 `glGetError`；语料里的成功性检查是 `glCheckFramebufferStatus`），故它**不标 `kNeedsAck`**、保持晚到/异步。纹理族在 monolith 里就已经推迟到 sync 时刻，同样**不标**（§6.4） |
| 8 | `map_persistent`（仅 T1 档） | 应用必须拿到一个不再经过任何 API 调用就能写的地址 | **每次存储定义一次**（v2 修正），不是每 store 生命周期一次；`StorageBufferRegrowScenario` 发布计数 |
| 9 | **server 发起的纹理重铸拉取** | server 不保留纹素 | **四条缓解 + 终止符 + 专门的门 + 逐用例发布的计数器**（§6.5）。异步形态下阻塞的是 `mgl-srv-apply` 而非应用线程；零 region 的应答让 server 带着空存储继续，永不永久 park |
| 10 | client 侧索引扫描，当源 EBO 在 pending 集里 | monolith 在**同一位置**调 `SyncGpuWrites()`（`VulkanRenderer.cpp:3431`） | §4.8.1 的逐站点表；**`*IndirectCount` 不在此列**（它今天不调 `SyncGpuWrites()`） |
| 11 | ring/stage 耗尽、present credit | **节奏，非语义** | `PersistentRing` 的升级路径 + `producerParked` doorbell（§7.5、§7.2a） |

### 12.3 论证的形式：测量，不是声称

**验收门措辞**：在**全部 40 个 trace 用例**上发布**逐用例的 roundtrip 计数器、纹理拉取计数器、索引镜像字节数与 `index-bytes-shipped`**。**不做笼统的"零 round trip"声明。** 条件渲染与阻塞 query 的次数按用例列出。

轮询挂死的防护（§8.2 的轮询门铃点与饥饿升级）必须有它自己的门：`glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 必须在有界时间内退出。

---

## 13. Monolith 保留、模式选择与构建布局

### 13.1 接口在进程内就是直调

monolith 模式下 `MGPipeContext` 用 backend 自己的函数填充，`MGPipeCallbacks` 用对 `MG_State` 的直调填充，`MGHostSpan.ptr` 指向 client 自己的 shadow（**零新增拷贝**），`MGPipeHandle` 按值走一对寄存器。split 模式下同一张表换成发射器，applier 反序列化后调**同一批 backend 函数**。**全世界只有一份 backend 实现。**

### 13.2 热路径的间接成本，**动态口径**的诚实版（v2 重写）

v1 这张表把今天的每 draw 状态获取写成 "Espryt 124 / Magma 169 次 accessor 调用"。**那是静态调用点数**（§2.1(d) 的定义），不是动态每 draw 调用数——树里每一处都已被 memo 门控（§2.3.1 逐条列了早退位置）。按动态口径重写：

| | 今天（动态稳态） | 之后（动态稳态） |
|---|---|---|
| 每 verb 的分发 | 1 次间接调用 + 3 个寄存器实参（`DrawArrays`） | 1 次间接调用 + **56 B 固定头**（`MGPDrawInfo`，**P0 实测修正**，此前写 ~48 B）+ 按 flag 的变长尾。**这是一项新增成本，不是持平** |
| 每 draw 的状态获取（值类） | Espryt：1 次 `Uint16` 比较（`DirectGLES.cpp:2016-2018`）早退；未命中时 1.2KB×3 段 memcmp。Magma：1 次版本比较（`:4982`）+ 1 次版本比较（`:5888`）；pipeline memo 未命中时 ~40 次 accessor 走查（`:5155-5200`） | 1 次 `Uint16` 比较；pipeline 版本动了才算 ~25-30 字的子集哈希 + 1 次 map 探测（D-B1）；动态子集动了才发 ~200 B |
| 每 draw 的状态获取（对象类） | Espryt：`SyncNeccessaryTextures` 6 值键 + `PairingsIntact` + 每条目 `IsDrawSyncClean`；`CurrentUnitBindingsEpoch` 三值快门。Magma：`TrySetupDrawFastPath` ~10 次 accessor + ~20 次字比较 + 两次**有损**版本求和（`:6249-6250`） | 5 个聚合世代各 1 次 `Uint64` 比较（推论 4）；命中才走 touched 前缀 + 集合 hash；hash 未变**不发**（§4.4-4） |
| memo 查表 | 对指针位做斐波那契散列的直接映射探测 + owner 相等性（3 次/draw） | 按 slot 的数组下标 |
| 真删除的机制 | — | **~372 行 per-draw 失效发现**（§2.5） |
| 搬到 client 的机制 | — | **~175 行**（去抖 + 完备性解析，§2.5） |

**结论（诚实版）**：推送在稳态**应当**是净减少——省掉三次散列探测、一次 1.2KB 三段 memcmp（换成 ~30 字哈希）、两次有损求和、`CurrentUnitBindingsEpoch` 的 owner 走查；付出 `MGPDrawInfo` 的 payload 构造与集合 hash。**但差距远小于 v1 声称的量级**，而且 §2.7 表明 monolith 的净行数是**增加**的。**所以本设计的 monolith 论据是 §13.3-④ 的逐线程 CPU 数字，不是删除行数。**

两个诚实的告诫：
1. **可达性遍历是搬走了，不是消失了**，头号指标必须是**逐线程 CPU 时间**。
2. **Magma 的 `SetupDrawSnapshot` 快路径命中率在两种模式下会合法地不同**，A/B 比的是**渲染输出与计数器**，永远不是 memo 轨迹。

两个 backend 编进同一个共享库（`CMakeLists.txt:356-383`、`:485`），backend 在 init 时锁存一次（`ConfigLoader.cpp:212-225`），所以去虚化在两种形态下都不可得，也都不需要。**函数指针 struct 而非虚基类**的理由见 §3.1。

### 13.3 替代字节一致门的五部分验证门

**先把成本写在明面上**：一个"改前改后 `nm --defined-only` 与剥调试信息后的 `.text` size 完全相等"的 monolith 门（§13.5 的第四层）在本方案里**按构造死亡**。这是本方案的代价，必须写进设计文档而不是藏起来。

**①（v2 扩为三道）接口纯度门。**
- **门 A（include 图）**：disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（或断言 `-H` 输出）。**这是唯一能因它存在的理由变红的检查**——`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h:12 → FramebufferObject.h:12-13 → TextureObject.h / RenderbufferObject.h` 正是这种耦合，`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 定长（`:263, 273`）。依赖 P0.5 的 `MGPipeValueTypes.h`。
- **门 B（符号）**：`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空。
- **门 C（未声明）**：`grep -c 'pGLContext' MG_Backend/` == 0（grep `pGLContext` 不是 `pGLContext->`）。**三道门都只跑非 verify 构建**（D-B5）。
- **外加**一条 debug 断言"每个 backend memo 键都是 `{slot, gen}` 对，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑——**这个场景在 0e 重键之前必须在至少一个 backend 上是红的**。

**② 语义影子比对（`MOBILEGL_PIPE_VERIFY=1`）——决定性的那一条。**
阶段 B 期间两套状态模型活在同一个地址空间：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 生成的比对器**逐字段**、**每 draw** 与推送版本比对，打印第一个分歧字段名与 draw 序号。抓三种事：(a) tracker 忘了推的字段；(b) **dirty 位触发得太少**——危险的那个方向；(c) 两条路径上被变换得不一样的值。第三种 CI 模式，跑全部 40 个 trace 与 367 个集成测试；~5-10× 慢，永不出货。
**必须逐字段比而不是 `memcmp`**：`DirectGLES.cpp:2029-2033` 明确记录 `RenderStateParameters` 的 memcmp 会因 padding false-DIFFER（无害）但永不 false-match——比对器要零误报。
**v2 修正 A：verify 需要"保留模式"。** 消费即清的组（纹理 dirty rect）在发射后无法从头重算，所以 verify 在纹理 subdata 上是瞎的——而那正是最危险的子系统。`MOBILEGL_PIPE_VERIFY=1` 时 tracker 保留清除前的集合，G4 比对**发射出去的** `(unionBox, regionCount, regions[])`（§6.3）。
**v2 修正 B：verify 活过 P13。** `SnapshotFromGLContext()` 与它的 `MG_State` include 整体包在 `#if MOBILEGL_PIPE_VERIFY` 里保留；纯度门只跑非 verify 构建（D-B5）。P13 另交付**录制-金标**模式（MGPipe recorder，§13.4-9）作为不依赖 `MG_State` 的长期语义门。

**③ 行为 A/B。**
全部 ~40 个 trace 用例（`tools/trace_replay/trace_cases.json`，默认 SSIM 阈值 0.99）在 `{monolith-pull, monolith-push, split}` 三种下同一判定、SSIM ≥ 0.99；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.`（以及 DirectVulkan 对）之间产生**逐名相同**的通过/失败集；428 个单元测试全绿；CTS 逐后端 conformance 在 0.5 个百分点内，按本项目的逐后端表格式上报（行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母）。
**两个 Create fixture 带 `coherent_as_flush: true`**，必须在两种模式下都开着该开关跑（§7.8.1）。
**v2 补充：`TextureUploadShapeScenario`**——上传形状（box vs N region、作业数）录金标比对，因为 SSIM 对 +6ms 悬崖完全不敏感（§6.3）。
**v2 补充：参考构建的定义。** P2 之后 monolith 本身已经变了，所以逐名基线必须明确为**"P1 出口的重构后 monolith"**，而 P1 出口本身要先用 verify 证明重构等价于 `81b17c0b`。**`81b17c0b` 的 monolith 只作为 §13.3-④ 性能对照的锚点，不作为逐名功能基线。**

**④ monolith 性能不回归。**
两台设备（`35d0befa` Adreno 830、`3B159D009VZ00000` Mali），reboot-clean、同热窗口、配对 A/B，用 `tools/bench.sh` + trace replay 的 `--benchmark --benchmark-tail-frames --benchmark-result` 逐帧 JSON。**指标是逐线程 CPU 时间**，monolith-push 在 **p50 与 p99** 上都要落在 monolith-pull 的噪声内。CPU 定频按本项目协议。
**v2 补充三条**：(a) **绝对阈值**——tracker 每 draw 的 ns 必须公布并设上限，因为真实拉取基线只有 10-25 次 accessor（§2.3.1），相对噪声阈值会平凡通过；(b) **Blaze3D blend-toggle 微基准**（enable/draw/disable/draw，MC batch 速率）单列，它是 D-B1 的判据；(c) **负面对照**——关掉 CSO 内容寻址（`MOBILEGL_PIPE_PUSH` 的一位）重跑，把"推送更慢"与"CSO 设计更慢"分开。

**⑤ 覆盖 + poison + handle 纪律。**
`gen_pipe.py` 重生成 477 行 inventory 的 MGPipe 映射列，0 UNMAPPED，`git diff --exit-code`；**`gen_pipe_dirty_surface.py` 重生成 mutator→聚合世代 映射，0 未映射**（推论 4）；`PipeInputs::m_filledGen` 的**逐 verb**世代 poison（§5.2.2）；G7 的 render-state setter 一致性测试；P13 的 `static_assert(sizeof(ResidualValueBlock) == 0)`；`ResidualValueBlock` 的逐成员 `offsetof` 断言。

**两条字节级等式仍然幸存**：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加任何库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中。
**符号与 `.text` 漂移每阶段作为信息性指标发布**——一次无法解释的跳变仍然是一个 smell，只是不再是一条断言。

### 13.4 monolith 侧净收益清单（即使 IPC 永不上线也成立）

1. **~372 行 per-draw 失效发现机制真删除**（§2.5），另有 ~175 行搬到 client。**注意 §2.7：monolith 的净代码量是增加的**（约 +6,650 手写 + 4,000 生成），所以这一条是**佐证**，不是主论据。
2. **复用地址 ABA 一整类不可表达**：D1/D2/D3/D10/D11/D13/D14/D16/D17/D20 全部由 `{slot, gen}` 关闭。
3. **FBO → program 排序 hazard 消失**：`DirectGLES.cpp:2712-2732` 的 fragColor 重推导 workaround 与 `g_broadcastMemo*` 删除（机制是惰性特化，D-B3 v2）。
4. **一处分层倒置消失**：`SwapchainObject.cpp:276-330` 不再往 `MG_Impl` 的 `pDefaultFramebufferInfo` 里写。
5. **两个潜伏 bug 顺带修掉**：D21（`m_xfbCounterSlotByObject` 用裸 GL name 做键，`VulkanRenderer.cpp:11136-11146`）与 `RenderbufferObject` 缺 `GetLifetimeId()`。**两条都先独立落 `dev`。**
6. **一个死能力被暴露**：`CapabilityInput::FramebufferSrgb` 与 `DepthClamp`（`RenderState.h:165, 168`）**没有任何存储**——`SetCapability` 落到 `default: // not supported currently`（`RenderState.cpp:380`），`IsCapabilityEnabled` 返回 `false`（`:428-429`）。**六个 backend 读点今天恒为 false。** **必须在渲染状态 chunk 表冻结之前回答**（它决定 pipeline/dynamic 划分里要不要这个字段）。
   **（P0 实测修正）调查结论 + 待拍板。** 已查明的事实三条：`FramebufferSrgb` 的**六个 backend 读点全部在消费一个编译期常量 `false`**（`IsCapabilityEnabled` 对它的返回值可被常量折叠），`DepthClamp` **一个读点都没有**；`glEnable(GL_FRAMEBUFFER_SRGB)` / `glEnable(GL_DEPTH_CLAMP)` 被**静默吞掉**——落到 `default:` 分支既不存储也**不报 `GL_INVALID_ENUM`**，应用无法察觉；41 个 trace fixture **无一**开启任一项（所以补上真存储不会改动任何既有 fixture 的渲染输出）。
   **调查方给出的建议（决定权在计划所有者）**：在渲染状态 chunk 表冻结**之前**给两者补上真实存储；并把 `FramebufferSrgb` 放进 D-B1 划分的 **pipeline 那一半**——它改变的是 attachment 的解释与 blend 的工作色彩空间，属于会重铸 pipeline 的子集，不是 `set_dynamic_state` 那一半。`DepthClamp` 的归属随实现方式定，可留到补存储时一并拍板。**在拍板之前不要冻结 chunk 表。**
7. **一次 glslang 编译离开 monolith 启动路径**（Magma 的内部 shader 烘焙）。
8. **`inproc` = monolith 的渲染线程**，且只需隔离两个进程全局（§13.6）——本项目手上最大的单一 CPU 杠杆。
9. **`MG_Test` 的 mock backend 顺理成章变成 MGPipe recorder**：`tools/trace_replay` 获得一种比 apitrace 精确得多的 MGPipe 级录制格式（记录的是**已解析**的状态），**而且它是 P13 之后不依赖 `MG_State` 的长期语义门**（D-B5、开放问题 11 的答案）。

### 13.5 三层编译期保证与唯一 hook 点

**从强到弱：**

1. **编译期折叠。** `MOBILEGL_BUILD_DISAGGREGATED`（默认 **OFF**）关闭时 `MobileGL/MG_Remote/**` 不进 `SOURCE_FILES`，`MG_Config::Transport` 是 `constexpr Monolith`，`MG_Backend/Init.cpp` 里的分支在编译期消失。**注意 `MG_Pipe/` 不在这个 option 之后**——它是 monolith 的架构，永远进构建（§13.8）。
2. **唯一 hook 点。** 整个拆分入口是 `MG_Backend/Init.cpp:48-70` 里的一个分支：
```cpp
void Init() {
    MGLOG_D("Initializing MobileGL Backend...");
#if MOBILEGL_BUILD_DISAGGREGATED
    if (MG_Config::Transport != TransportKind::Monolith) {
        pActiveBackendObject = MakeUnique<MG_Remote::BackendObject_Remote>();
    } else
#endif
    switch (MG_Config::ActiveBackendType) { /* 原样不动 */ }
    if (!InitSpecificBackendLibs()) { /* 原样 */ }
    LogBackendInfo();
}
```
`BackendObject_Remote::GetPipeTables()` 返回发射版的 `MGPipeScreen`/`MGPipeContext`，`Initialize()` 负责 spawn/connect。下游的 MG_Impl 边界调用点**零 `#ifdef`**。
3. **shadow-in-shm 的 allocator 改动必须同样包裹。** `PipeResource::MapAlignedAllocator` 与 `MipmapStorage` 的 level vector 住在 `MG_State`，改它们的 allocator 就改了类型；写成"分配器特化，option OFF 时逐字折叠回今天的 `MapAlignedAllocator`"（§7.4）。

**第四层——`nm --defined-only` 与 `.text` size 逐阶段完全相等——在本方案里不成立**（D-B5），由 §13.3 的五部分门取代，只保留两条字节级等式作断言、符号/尺寸漂移作信息性指标。

### 13.6 两个 CMake option 与 `inproc` 的角色隔离

**每一条部署路径都要求出货构建是 ON**：FCL 用户可编辑 env、plugin APK 的 V2 开关表、ctest `ENVIRONMENT` 变体、`/data/local/tmp` CTS 路径。所以 option 必须拆成两个：

- **`MOBILEGL_BUILD_DISAGGREGATED`**（出货形态）：只含 `spawn`/`unix:`/`pipe:`。每进程只有一个 `GLContext`、一份 `gPipeCtx`、一个 `pActiveBackendObject` → 它们**全部保持普通全局**，GL 热路径上没有任何 TLS 与间接。侵入面就是 `MG_Backend/Init.cpp` 里那一个可预测的分支。
- **`MOBILEGL_BUILD_DISAGGREGATED_INPROC`**（CI/调试形态，隐含开启前者）：额外加角色隔离 shim。

**`inproc` 需要隔离的是两个进程全局，不是四个。** 在拉取模型下，同进程同时扮演两个角色需要给 `pGLContext`、`gBackendFunctionsTable`、`pActiveBackendObject`、`pDefaultFramebufferInfo` 四个全局都做角色分身，其中 `pGLContext` 的 shim 坐在全库最热的路径上（`grep -rho 'pGLContext->' MobileGL/MG_Impl | wc -l` = **1494**，加 backend 侧 293），而 Android 上 dlopen 的共享库无法可靠使用 initial-exec TLS，每次访问会退化成一次 `__tls_get_addr` 调用。

MGPipe 把这个数字降到 **2**：

| 全局 | 还需要角色隔离吗 | 为什么 |
|---|---|---|
| `MG_State::pGLContext`（`GLState/Core.h:564` / `Core.cpp:1487`） | **不需要** | server 角色不再读它（三道纯度门就是这个断言）。它只属于 client 角色 |
| `MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo`（`GL_Framebuffer.cpp:3344`） | **不需要** | backend 侧的 4 处身份比较改用保留 handle `{0,1}` + `MGPFramebufferState::isDefault`；`SwapchainObject.cpp:276-330` 的**写**改成 `on_surface_changed`。server 角色不再触碰它 |
| `MG_Backend` 的 pipe 表（今天的 `gBackendFunctionsTable`，MGPipe 下是 `gPipeCtx`/`gPipeScreen`） | **需要** | client 角色要看见发射表，server 角色要看见真 backend 表 |
| `MG_Backend::pActiveBackendObject`（`Init.cpp:53-61`） | **需要** | 同上：EGL/caps 虚函数面 |

两个全局的 shim 只需要 `operator->` / `operator bool` / `get()` / 赋值，而且**都不在 GL 热路径的每次访问上**（pipe 表在每个 MGPipe 调用处取一次，`pActiveBackendObject` 只在 EGL/caps 面）。**这条是 MGPipe 让 `inproc` 从"成本可疑的实验"变成"可交付形态"的直接原因。**

### 13.7 `inproc` 作为产品交付物与运行时选择

`inproc` 不只是测试脚手架：同进程第二个 apply 线程 = monolith 的渲染线程。今天 `PrepareForDraw`（状态调和、VAO/FBO/纹理/program/render-state sync、UBO ring memcpy）加驱动调用全部同步跑在 `glDrawElements` 里；把它们搬到 apply 线程，对 GL 线程 CPU-bound 的应用（本项目的 profiling 史说 Minecraft 就是）是**手上最大的单一杠杆**，且不需要任何 IPC/shm/平台工作。

**`InProcessTransport` 必须走与 spawn 完全相同的 G3 编解码路径**，只在门铃/拷贝机制上不同（§14 P5 的规范条款）。否则 `inproc` 里程碑证明不了 wire 完整性。

`MOBILEGL_TRANSPORT = monolith(默认) | inproc | spawn | unix:<path> | pipe:<name>`，在 `ConfigLoader.cpp` 与既有开关并列解析。这一个选择免费换来：ctest `ENVIRONMENT` 变体、trace-replay 的 `setenv` 块（`trace_replay_core.cpp:134-207`）、FCL 的用户可编辑 env 偏好（`FCLauncher.java:417-430`）、plugin APK 的 V2 开关表（`android-plugin/app/build.gradle.kts:77-103`，由 `.github/scripts/validate-plugin-apks.sh` 校验）、`/data/local/tmp` CTS 路径。**零新增管线。**

保留全部既有负面对照开关（`MOBILEGL_ESPRYT_DISABLE_{UBO,UNPACK,UPLOAD}_RING`、`_INVALIDATE_FLUSH`、`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`、`MOBILEGL_COHERENT_AS_FLUSH`）；新增开关见附 B。

### 13.8 构建布局与测试接线

```
MobileGL/MG_Pipe/                                 # 见 §3.1；**不在任何 option 之后**，永远进构建
MobileGL/MG_Impl/Pipe/                            # tracker、slot 分配器、CSO 缓存、HostResolve、CompositeResolver
MobileGL/MG_Backend/MGPipe/                       # PipeInputs 与两个 backend 的表填充
MobileGL/MG_Remote/                               # 仅 MOBILEGL_BUILD_DISAGGREGATED
  Protocol/  protocol.fbs  protocol_generated.h(提交)  RecordKinds.h
  Transport/ ITransport.h  InProcessTransport.{h,cpp}  SocketTransport.{h,cpp}
             Framing.h  Ring.{h,cpp}  ShmSegment.{h,cpp} ShmSegmentPosix.cpp ShmSegmentWin32.cpp
             FdPassing.{h,cpp}  Doorbell.{h,cpp}
  Client/    PipeEmitter.{h,cpp}  EmitTables.cpp
             BackendObject_Remote.{h,cpp}  CapsMirror.{h,cpp}
             ShadowArena.{h,cpp}  PersistentMapTracker.{h,cpp}  GpuWritePending.{h,cpp}
             Surface/{X11,Win32,Android,Headless}.cpp
  Server/    PipeApplier.cpp  PipeObjectTables.{h,cpp}  IndexHostMirror.{h,cpp}
             ServerLoop.{h,cpp}  ReplyPool.{h,cpp}  EventRing.{h,cpp}  ServerMain.cpp
  ServerJni.cpp                                   # Android，与 DriverPostJni.cpp 并列
scripts/     gen_pipe.py  gen_pipe_dirty_surface.py  gen_protocol.py  check_doc_citations.py
MobileGL/MG_Test/Wire/CMakeLists.txt              # 复制自 MG_Test/Buffer/（27 行）
```

CMake：
- **`MG_Pipe/**` 与 `MG_Impl/Pipe/**` 与 `MG_Backend/MGPipe/**` 无条件进 `SOURCE_FILES`。** 只有 `MG_Remote/**` 在 `MOBILEGL_BUILD_DISAGGREGATED` 之后追加（`CMakeLists.txt:226-419`），因此 `MobileGL`（`:485`）与 `MobileGL_s`（`:552`）都拿到。
- `MobileGLServer`：桌面 `add_executable` 链接 `MobileGL_s`，`RUNTIME_OUTPUT_DIRECTORY` 设为 `$<TARGET_FILE_DIR:MobileGL>`（§11.1）；**Android** `add_executable` + `set_target_properties(MobileGLServer PROPERTIES PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "MobileGLServer")` 并链接**共享**的 `MobileGL`，由 AGP 打进 `jniLibs`。server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`（可见性见 §11.2）。**一份共享库、两个角色，版本必然匹配**（对比 `Feat/CS-Delta-IPC` 的四件必须互相匹配的产物）。
  **AGP 能否打包一个被改名成 `lib*.so` 的 `add_executable`，是 P0 spike A 的验证项之一**（`MobileGL/build.gradle` 没有设 `targets` 列表）。
  **注意**：Android 上那份共享库仍然包含 glslang/SPIRV-Cross/SPIRV-Tools（~43MB），因为它同时服务 client 角色；`nm --undefined-only` 的 glslang 门（§13.3-①B）检的是 **server 侧代码有没有引用它们**，不是产物里有没有这些符号。
- **FlatBuffers**：submodule `3rdparty/flatbuffers` 置于既有的 `if (EXISTS .../flatbuffers/CMakeLists.txt)` 保护下，**去掉 `if (NOT ANDROID)` 一刀切**。因为 `protocol_generated.h` 已提交，**默认构建图里没有 `flatc`，也不 `add_subdirectory(3rdparty/flatbuffers)`**（§8.1）。运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上。
  **第二重 guard**：若 `MOBILEGL_BUILD_DISAGGREGATED=ON` 而 `3rdparty/flatbuffers/include` 不存在，强制把该 option 设回 OFF 并 `message(WARNING ...)`——否则 `MG_Remote/**` 已经进了 `SOURCE_FILES` 而头文件找不到，构建以一个莫名其妙的错误失败（现有的 `EXISTS` 保护只包住 Protocol 子目录）。
  `MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`，经 `MobileGL/build.gradle:17-21` 已在用的 `externalNativeBuild { cmake { arguments } }` 槽传入。
- 测试接线（三个已被文档记录的陷阱要遵守）：
  - `MG_Test/Wire/`（label `unit`）→ 现有 CI `test` job 自动收，**无需改 workflow**。
  - `MG_IntegrationTest/CMakeLists.txt` 每 backend 增加两条 `gtest_discover_tests`（`TEST_PREFIX "DirectGLES.Pipe."` 用于 monolith-push、`"DirectGLES.Split."` 用于拆分，DirectVulkan 同），**必须用 `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` 构造**，并带上 `MOBILEGL_IPC_SERVER_PATH`。陷阱：ctest `ENVIRONMENT` 是**替换而非追加**（`:339-343`）、`;` 必须转义（`:322-332`）、property **覆盖** job env（`test.yml:253-262`）。
  - **trace replay 的 `SPLIT` 接线**：`add_trace_replay_test` 今天把测试命名为 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}`（`tools/trace_replay/CMakeLists.txt:330-332`），加一个 `SPLIT` 参数会与同 case+backend 的现有测试**重名**。改成 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}${SPLIT_SUFFIX}`。另外该测试的命令是 `cmake -P run_trace_case.cmake` 加约 18 个 `-DTRACE_*` 变量，所以还要加 `-DTRACE_TRANSPORT=` 并在 `run_trace_case.cmake` 里消费它——**这两个文件都要列进 P5 的交付物**。
- CI 新增步骤：
  - `pipe-gen-check`：重跑 `gen_pipe.py`（G1-G7）+ `git diff --exit-code`；
  - `dirty-surface-check`：重跑 `gen_pipe_dirty_surface.py` + `git diff --exit-code`，**0 未映射 mutator**；
  - `flatc-check`：重生成 `protocol_generated.h` + `git diff --exit-code`；
  - `include-graph-check`：`MGPipeValueTypes.h` 与 `ProgramArtifacts.h` 的 `-H` 闭包断言（§3.7.2 门 A、§14 P0.5）；
  - `doc-citation-lint`：`check_doc_citations.py`，`docs/**` 里每个 `file:line` 必须在基线提交上解析到存在的行；
  - **一条 grep 门**：禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`；
  - `monolith-symbol-report`：OFF 构建与 ON+monolith 构建的 `nm --defined-only` / `.text` size 对基线，**信息性发布 + 两条幸存等式作断言**（§13.3）。

---

## 14. 分阶段实施计划

> **通用纪律（每个 commit 都适用）**：默认 ALL target 必须能完整构建；禁止提交热路径插桩；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门（其 Vulkan 缺 `vkCreateHeadlessSurfaceEXT`，占该机 567 个基线集成失败中的 423 个）；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议（大核 1.96 / 小核 1.55GHz，GPU 拉满，40°C 门槛）；**每个阶段的出口都跑一次 §13.3 的五部分门**；**每个阶段的性能判据都是逐线程 CPU 时间**，不是墙钟帧时。
> **两条跑道**：P0-P4a、P3b/P4b、P7、P8、P13 是 **monolith 跑道**，每一段都可独立交付、可随时中止且 monolith 严格好于起点；P5、P6、P9-P12 是 **IPC 跑道**。
> **v2 排期修订说明**：v1 的阶段天数与它自己的 §5.4/§5.5 逐子系统表互相矛盾（例如 P3a 给 12 天，而它包含的三行合计 22-29 天，等于"再基线检查点"按构造必然触发；P7 报 48 天下界而同口径是 85-111）。**本节的每个天数都是它所含 §5.4/§5.5 行的求和**，算术在 §14.5 公布。

### P0 — 卫生、度量、门与骨架（9-11 天）

**交付物**
- **清工作树 per-draw `fprintf`**：`DirectGLES.cpp:640-663`、`Managers.cpp:875-877`（后者在 `pendingMutex` 临界区内）。CI 加 grep 门禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`。
- **`TracyPlot` 逐帧计数器，装在边界两侧**，**字节类**：`cmd-records`、`cmd-bytes-per-draw`（**直方图**，`SEG_CMD` 的定尺依据）、`stage-buffer`、`stage-texture`、`stage-vertex-client`、`stage-index-client`、`stage-ubo-global`、`stage-ubo-named`、`persistent-map-push`、`server-ring`、`server-staging`、`residual-value-block`、`index-mirror-bytes`、`index-bytes-shipped`、`texture-pull`；**调用类（v2 新增）**：每 draw 实际执行的 accessor 次数、每个 memo 门（`SyncRenderState` 早退、`SyncNeccessaryTextures` 键比较、`CurrentUnitBindingsEpoch` 快门、`TrySetupDrawFastPath`、pipeline memo、`ApplyDynamicDrawStateTail`）的命中/未命中、`resource_subdata` 发射次数与上传作业数。**没有调用类计数器，P2 的判据仍然是猜**（§2.3.1）。两台设备取基线。
- `MG_Pipe/PipeCalls.def` + `MGPipeTypes.h` + `MGPipeHandles.h` + `MGPipeCallbacks.h`：**完整调用目录，即使暂未实现的条目也占位**（记录编号绝不 churn）。
- `scripts/gen_pipe.py` 与七个生成器 G1-G7 的骨架 + CI `pipe-gen-check`（重生成 + `git diff --exit-code`）。
- `scripts/gen_pipe_dirty_surface.py` 骨架（推论 4）与 CI 接线。
- **`scripts/check_doc_citations.py`**（v2 新增）：`docs/**` 里每个 `file:line` 必须在基线提交上解析到存在的行。**v1 有一批 `SamplerObject.h` 引用指向 160 行文件的 468-551 行**；本文件已修正，lint 防止再犯。
- `MOBILEGL_PIPE_PUSH` / `_VERIFY` / `_STATS` / `_LEGACY_MEMOS` / `_TEXEL_RETAIN_MB` / `_INDEX_MIRROR_MB` 在 `ConfigLoader.cpp` 与既有开关并列解析；两个 CMake option（§13.6）与 `MOBILEGL_TRANSPORT` 解析；§13.8 的 flatbuffers include-dir guard。
- **三个严格 no-op 的免费收益**：`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 的纯前端 case 移回 `MG_Impl`（Espryt 14 / Magma ~10 个读点）。**（P0 实测修正）后两项已落地**——`GetInteger64i_v` 与 `GetProgramiv` 的表项与两个 backend 实现已从 `GLFunctionsTable` 删除（提交 "retire the two frontend queries that were never asked"）；同时确认 **`GL_COMPUTE_WORK_GROUP_SIZE` 由 `GL_Program.cpp:928-946` 纯前端回答，不进 `MGPCaps`**，进 caps 的是 `GL_MAX_COMPUTE_WORK_GROUP_COUNT`/`_SIZE` 两个 compute 限制（§3.4.6）；`RenderbufferObject::GetLifetimeId()`（**不加 `GetVersion()`**——推送模型里 `glRenderbufferStorage*` 本身就是一次 pipe 调用）；D21 重键——**这一条是潜伏 bug 修复，先独立落 `dev`**。
- 回答两个阻塞问题：`FramebufferSrgb`/`DepthClamp` 无存储是潜伏 bug 还是有意为之（§13.4-6，**必须在渲染状态 chunk 表冻结之前**）；**语料里是否存在 `glRenderbufferStorage` 的 OOM 探测惯用法**（决定 `kNeedsAck` 要不要标它，§6.4）。
  **（P0 实测修正）两问均已调查完毕**：(a) OOM 探测惯用法在 41 个 fixture 里 **0 例**（9 次 `glRenderbufferStorage` 调用散在 5 个 fixture，无一在 3 个调用内跟 `glGetError`；实际的成功性检查是 `glCheckFramebufferStatus`）→ **`kNeedsAck` 只由 `glBufferStorage` 承担**，`glRenderbufferStorage*` 保持晚到/异步，整条 ack 路径省掉（§6.4、§12.2-7）。(b) `FramebufferSrgb` 有六个 backend 读点全在消费一个编译期常量 `false`、`DepthClamp` **零读点**，两者的 `glEnable` 被静默吞掉且不报 `GL_INVALID_ENUM`，41 个 fixture 无一开启任一项 → **调查结论 + 待拍板**，见 §13.4-6 与开放问题 10。
- `MG_Remote/{Protocol,Transport}` 骨架：`ITransport`、`InProcessTransport`、校验型 `Framing`、`Ring` + `RingControl`（**双 tail、双游标三元组、双向 doorbell**）、`Doorbell`、`ShmSegment`（memfd/ASharedMemory/shm_open/CreateFileMappingW）、**`SCM_RIGHTS` fd 传递（第一优先）**；`protocol.fbs` + 提交的 `protocol_generated.h` + `gen_protocol.py` + CI `flatc-check`；`MG_Test/Wire/` 目录。
- `mobilegl_server_main` 的 `extern "C" __attribute__((visibility("default")))` 声明（§11.2）。
- **spike A（Android 交付链，半天）**：从根 CMakeLists 造一个平凡的 `libMobileGLServer.so`（`add_executable` + `PREFIX "lib"/SUFFIX ".so"`），确认 AGP 把它打进 `lib/arm64-v8a/`；让 `TraceReplayActivity` 从 `getApplicationInfo().nativeLibraryDir` **`posix_spawn`** 它并打一行日志——在**应用自身进程（`untrusted_app` 域）**验证 exec，而不是靠 `run-as`。同时把一个通用 env 透传（`--es mobilegl_env "K=V;K=V"`）接进 trace 路径的五个文件（`trace-replay-ci.sh`、`TraceReplayActivity.java`、JNI Request marshalling、`trace_replay_core.cpp`、`run_android_retrace_local.py`），取代逐 knob 加 `--es/--ez`。
  **（P0 实测修正）已证 vs 待证。** **已在主机侧证明**三条：(1) AGP **确实**会把一个被改名成 `lib*.so` 的 `add_executable` 打进 `lib/arm64-v8a/`，前提是把它的 `RUNTIME_OUTPUT_DIRECTORY` 重定向到 AGP 收集原生产物的那个目录（默认 runtime 输出路径 AGP 不看）；(2) **`posix_spawn` 在 minSdk 26 上用不了**——bionic 从 **API 28** 才声明它，所以出货形态的那条臂是 **`fork` + `execve`**，本文其余处（§11.3 的注记）写 `posix_spawn` 的地方一并按此读；(3) 应用进程的 stdout/stderr 是 **`/dev/null`**，子进程"打一行日志"证明不了自己活过，**必须改成写一个 marker 文件**再由测试断言它出现。**待证**：`untrusted_app` 域内的真机 exec 本身（设备锁未解，on-device 运行仍欠着）——spike A 的核心结论因此**尚未闭合**。
- **spike B（external memory 可行性，半天）**：最小程序，导出一个 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，`mmap` 后回读校验，在 `35d0befa`（Adreno 830）与 `3B159D009VZ00000`（Mali）各跑一次。与 `SCM_RIGHTS` 测试同批。**目的是让 P11 的结论在第一周就有方向**：若两台都不行，P11 缩为"记录并回退"，省 6 天。
  **（P0 实测修正）已证 vs 待证。** 探针**已写好并在 lavapipe 上跑通**：**T1**（opaque-fd 的导出/导入）与 **T3**（host-pointer 导入）**两档都能完整往返**（导出 → 导入 → 回读字节相符）。**待证**：`35d0befa`（Adreno 830）与 `3B159D009VZ00000`（Mali）**两台真机都还没跑**（设备锁）。**所以 P11 的规模仍未定**——lavapipe 通过只说明探针本身正确，不构成任何移动端驱动的证据（§7.8 的三档选择、开放问题 3 保持开放）。

**验收**：`AdvertisedLimitsScenario`（6 个测试）绿；367 集成 × 2 backend + 428 单元逐名不变；40 个 trace 全绿；两台设备的基线**字节、调用、逐线程 CPU** 数字记录在案；`MG_Test/Wire` 的 fd 传递测试把一个 memfd 从 fork 出的子进程传回父进程并读到相同字节；spawn 测试断言进程树只多出恰好一个子进程；`nm --defined-only` 与去符号 `.text` size 与改动前的 `libMobileGL.so` 一致（OFF 构建），`nm -D | grep mobilegl_server_main` 在 RelWithDebInfo 下命中；spike A/B 出结论（spike B 直接决定 P11 规模）；citation lint 全绿。
**（P0 实测修正）本条验收目前的状态**：主机侧（lavapipe/llvmpipe）部分已达成——动态 accessor 基线已取（§2.3.1）、spike B 探针 T1/T3 往返通过、spike A 的打包与 `posix_spawn` 不可用两点已定论；**两台设备的基线数字与两个 spike 的真机运行仍欠着**（设备锁），P0 因此**尚未整体验收通过**。

### P0.5 — 值头与制品头抽取（6-9 天）★v2 新增，**P1 与 P7 的硬前置**

**交付物**
- **`MG_Pipe/MGPipeValueTypes.h`**：把 `MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint` 与相关枚举搬进来，**它不 include `MG_State/GLState` 的任何东西**；`RenderState.h` / `SamplerObject.h` / `VertexArrayObject.h` 反过来 include 它。
  **必须做的理由**：`RenderState.h:12` include `FramebufferState/FramebufferObject.h`，后者 `:12-13` 再 include `TextureObject.h` 与 `RenderbufferObject.h`；`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 给两个数组定长（`:263, 273`）。所以 v1 的"共享值头白名单"不是叶子集，把它交给"纯净的 `MG_Backend`"会拖进整张类图，而 `nm --undefined-only` 看不见（只 include 不调用不产生未定义符号）。
- **`MG_State/GLState/ProgramState/ProgramArtifacts.h`**：把 `TypeFacts`（`ProgramObject.h:44`）、`ResourceReflection`（`:76`）、`XfbVarying`（`:1146`）、`LinkArtifacts`（`:1210`）、`SpirvArtifacts`（`:1409`）抽出来，**不 include `ShaderObject.h`、不 include `SpvcSession.h`**；更新 7 个 includer（`ProgramFactory.h`、`UniformManager.cpp`、`VulkanRenderer.cpp`、`ProgramInterface.cpp`、`ProgramLinkTask.h`、`ProgramObject.h`、`ProgramTranslationCache.h`）。
  **必须做的理由**：server 要**反序列化进**这五个类型就必须有它们的定义，而它们今天住在会拖进 glslang（`ShaderObject.h:12` → `ShaderCompileTask.h`；`:146` 返回 `SharedPtr<glslang::TShader>`）与 spirv_reflect（`ProgramObject.h:14` → `SpvcSession.h`）的头里。**没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。**
- **CI include 闭包断言**：`MGPipeValueTypes.h` 的 `-H` 闭包里没有 `MG_State/GLState/`；`ProgramArtifacts.h` 的闭包里没有 glslang / SPIRV-Cross / spirv_reflect 任何头。
- `ProgramArtifacts.h` 的 `Visit()` 归档 + `sizeof` 绊线（§3.5.5）。

**验收**：全套现有测试逐名不变（这是一次纯搬移）；两条 include 闭包断言绿，且**人为把一个 `MG_State` include 加回 `MGPipeValueTypes.h` 能让它变红**；`nm --defined-only` 与 `.text` 变化可逐符号归因（搬移会改变某些内联决策，允许，但要解释）。

### P1 — `PipeInputs` 替换与 verify harness（10-13 天）

**交付物**
- `MG_Backend/MGPipe/PipeInputs.h`：每个 backend 真正用到的 `GLContext` 方法一个访问器（Espryt 32 / Magma 55），**字段类型与今天读到的完全一致**，按 memo 键组织。
- 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加逐条手工转换 58 行非箭头用法**（§2.4：~34 处 `MOBILEGL_ASSERT` 真值判定删除、7 处空守卫改直读、3 处 patch 三元、`DirectGLES.cpp:146` 的 `.get()` 裸指针捕获与 `:142` 的 `decltype` 别名、14 处 `!= nullptr`、1 处注释）。**这份 58 行清单是本阶段的显式交付物。**
- **逐 verb 类填充点**（v2 修正，§5.2.1）：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些 `PipeInputs` 字段"的表，并在 `MG_Impl` 的 ~93 个边界站点上生成对应的 validate/fill 调用。**不是只在 `PrepareForDraw`/`SetupDraw` 两处**——`MG_Impl` 用到的 70 个表项里 ~48 个不是 draw/dispatch，其中多个自己就读 `pGLContext`（`UpdateTextureBindingAtTarget` `:6051-6052`、`PackStateFromContext` `:6129`、`Clear` `:4106/:4165`、`BlitFramebuffer` `:5988-5989`、`GetTexImage` `:9254-9257`、DSA by-name `:4038-4043`、`:7417-7418`），而 `:1501-1502` 的注释已经点明"for every non-draw call site (Clear, readbacks)"。
- **G5 的逐 verb 世代 poison**：`m_filledGen[f] == m_currentVerbSerial`（非 sticky 字段）；debug 与 disaggregated 构建里读陈旧/未填字段 = `Fatal{UnmigratedPipeInput, "<field>@<verb>"}`。
- **G4 的 `MOBILEGL_PIPE_VERIFY=1` 逐字段影子比对器** + 第三种 CI 模式接线。
- **20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表**（§6.2、§4.8.1），作为文档交付物。

**验收（v2 修正）**
- **`nm --defined-only` 在 pull 构建里不变；`.text` size 变化必须能逐行归因。** v1 要求"完全一致"，但本阶段自己的交付物里就有 ~24 处会生成代码的转换（7 处 `if (pGLContext)` 空守卫、14 处 `!= nullptr`、3 处三元）——只有 ~34 处 `MOBILEGL_ASSERT` 是真免费（`Defines.h:114` 在非 debug 下宏为空）。此外 `SnapshotFromGLContext` 与 G4/G5 机制必须包在 `#if MOBILEGL_PIPE_PUSH/_VERIFY/DEBUG` 里，pull 构建才不多出调用。**把空守卫与三元的重写推迟到 P2**（那时字段确实永远有效），本阶段只做 assert 删除与 `sed`，则 `.text` 差异可压到零附近。
- 全部 40 个 trace 与 367 个集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；
- **故意损坏一个快照字段能让 verify 门变红**；
- **故意在某个非 draw verb（`glGenerateMipmap`）的填充表里漏一个字段，能在那条 verb 上触发 poison Fatal**——不是在某个后续 draw 上。

**★ 第 25 天（低端估计）— 最早可见里程碑：**零产品风险地证明"推送等价于拉取"，逐 draw 逐字段。**这不是 GO/NO-GO**（它没有性能数字，也没有 Track H 单位成本）。

### P2 — 值推送：渲染状态 CSO（双后端）+ 第一片 Track H + 残余值块（18-26 天）

**交付物**
- `MG_Impl/Pipe/Tracker.{h,cpp}`：dirty 位（§4.2，值类用既有计数器、**对象类新增 5 个聚合世代**）+ §4.3 的不变式 + §4.4-4 的集合 hash 抑制器骨架。
- **`MG_State` 的 5 个聚合世代**（`TextureState` 两个、`BufferState`、`VertexArrayState`、`FramebufferState` 各一，合计约 20 行）+ `gen_pipe_dirty_surface.py` 的首轮映射与 CI 接线。
- `MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` + **G7**：pipeline/dynamic chunk 表（从 `VulkanRenderer.cpp:4826-4906` 原样搬来）+ **遍历每个 `RenderState` public setter 断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变` 的测试**。
- `MG_Impl/Pipe/CsoCache`：64 项 LRU，键是 **pipeline 子集**的 xxHash（**不是整块**，D-B1 v2）。
- `create_render_state` / `bind_render_state` / **`set_dynamic_state`**：Espryt 侧 `RenderStateImpl` 的 693 行函数体、单 `Uint16` 早退、三段 memcmp、`g_syncedColorMaskAlphaWidenMask`、dual-source decline **一行不动**（消除 4 个读点）；Magma 侧 `ComputePipelineStateHash` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取（消除 ~55 个读点）。两个版本号都过线。
- `set_pixel_pack_state`（PACK only）、`set_patch_state`、`set_vertex_attrib_defaults`；P1 推迟的空守卫/三元重写。
- **`set_residual_value_state` + `ResidualValueBlock`**（§5.3）：`static_assert(sizeof == MGL_RESIDUAL_BLOCK_SIZE)`（逐阶段**下调**）+ **逐成员 `offsetof` 断言** + split 下逐字段序列化。
- **第一片 Track H（v2 新增，让 GO/NO-GO 测的是它要决定的事）**：Espryt 子系统 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / 2 个 GC 扫描）与 Magma 子系统 4（`VertexInputStateFactory` / `VaoDrawMemo` 重键，**删掉写进前端 VAO 的后端堆裸指针**）。
- **`MOBILEGL_PIPE_LEGACY_MEMOS`** 编译期开关（§5.7）：让前两波 handle 化保留一个**真正的**旧-vs-新臂。

**验收**
- 367 集成 × 2 backend × 2 模式（pull / push）逐名相同；40 个 trace 在 monolith-push 下 SSIM ≥ 0.99，双后端；`ClipDistance`、`SampleMaskScope`、`SampleVariables`、`DualSourceBlend`、`ViewportArray`、`PrimitiveRestart` 场景绿；verify 模式零分歧；
- **`HandleRecycleScenario` 绿，且它在 0b 重键之前必须是红的**；
- **G7 的 setter 一致性测试绿，且人为把一个字段从 pipeline chunk 表里拿掉能让它变红**；
- **两台设备 reboot-clean 配对**：monolith-push 在 p50 与 p99 逐线程 CPU 上落在 monolith-pull 噪声内或更好，**并且 tracker 每 draw 的绝对 ns 落在预设上限内**（相对阈值不够，§13.3-④a）；
- **Blaze3D blend-toggle 微基准**（enable/draw/disable/draw，MC batch 速率）单列发布；
- **负面对照**：关掉 CSO 内容寻址重跑，把"推送更慢"与"CSO 设计更慢"分开。

**★ 第 43 天（低端估计）— GO/NO-GO 决策点。** 此刻手上有：verify harness、双后端已推送的渲染状态、真实 CPU 增量与绝对 ns、Blaze3D 微基准、CSO 负面对照、**Track H 在两个 backend 的最便宜子系统上的实测单位成本**。两个出口（继续 / 收缩为 headless 工装用途或重新评估）与沉没成本口径写在 §0.5。

### P3a — handle wave 1（Espryt）：buffer、VAO（18-23 天）

> handle 基建（0b）已在 P2 交付。

**交付物**：7 个 `BufferBackendOps` → `resource_create/respecify/destroy`、`resource_subdata`、`buffer_subdata_resident`（**可 null，保住 Magma 的差异**）、`resource_flush_range`（带应用真实 access flags）、`resource_readback`、`map_persistent`（**不碰实现**）；pool 与延迟释放机制原样搬；`create/bind/delete_vertex_elements_state`（**两个视图都带**；`IsLong` 与 `Type` 分开）；`set_vertex_buffers`（**`baseInstance` 是显式字段**，不再是调用方武装的 `ScopedFetchBaseInstance` 作用域）；`set_index_buffer`（带 restart index 与模式）；Adreno 禁用属性 SIGSEGV workaround 原样保留；`MOBILEGL_PIPE_LEGACY_MEMOS` 分支维护。

**验收**：全套门（monolith-push，DirectGLES）；`LargeArenaAdoption`、`ResidentIndex`、`StorageBufferRegrow`（**发布 `map-persistent-roundtrips`**）、`AtomicCounter`、`BufferTexture`、`CrossFrameBuffer`、`SsboArrayLength`、`SsboArrayDynamicIndex`、`VertexArrayEnableDisable`、`VertexAttribBinding`、`DoublePrecision`、`DrawParameters`、`MultiDraw`、`PrimitiveRestart` 场景；`create-indirect`、`create-instancing`、`rd12-odinlite`、`improved-transparency-26.3`、`fabric-sodium` trace SSIM ≥ 0.99；MC 26.3 在 Adreno 上 p99 不变（16MiB 采纳结果不得回归）。
**⚠ 再基线检查点 1：若 P3a 超过 27 天（上界 +50%），"窄 handle 化"的前提就是错的，必须在 P4a 开始之前重定基线。**

### P4a — handle wave 2（Espryt）：FBO / 纹理 / sampler / program 的身份与描述符（26-34 天）

**刻意推迟到首帧之后的部分**：memo 重键、dirty 归属反转、跨步描述符改造、program 陈旧性重构（→ P3b/P4b）。

**交付物**：`set_framebuffer_state`（8 个 `MGPSurface` + **client 解析后的 `readSurface`** + 内联 `internalFormat` + `contentHash` + `isDefault` 保留 handle，退役 4 处 `pDefaultFramebufferInfo` 读）；四个跨对象 mask 在推送时刻推出；`create/bind/delete_sampler_state`（`SamplerParameters` 逐字节含 `borderColorForm`，`SamplerObject.h:66-96`）；`create/delete_sampler_view`（**只带视图限制**）+ **`set_texture_params`**（D10：base/max level、swizzle、dsMode、LOD 钳、`forceResync`）；`set_sampler_views`（client 侧解析，**无 stage 维度**）+ `bind_sampler_states`；`set_shader_images`；`create/bind/delete_shader_state`（逐 stage SPIR-V + `ProgramArtifacts.h` 的 `Visit()` 全结构体归档）；`set_draw_program` / `set_dispatch_program`；`set_global_constants`；`CompositeResolver.cpp`；纹理与 renderbuffer 的 `resource_create/respecify/subdata`。emulation 路径在 split 模式下**显式 Fatal** 直到 P8。

**验收**：全套门；`CrossFrameBuffer`、`LayeredAttachmentShape/Barrier`、`SnormAttachment`、`RenderbufferBlendFormat`、`FragmentOutputArrayIndex`、`Orientation`、`ClearThenReadPixels`、`FragCoordOrigin`、`TextureView`、`ProgramPipeline`、`PostLinkAttach`、`RelinkStageSet`、`SpirvShaderBinary`、`AsyncCompile`（6 个）场景；**新增"只作 FBO attachment / 只作 image 单元 / 只作 CopyImage 端点的纹理其 `glTexParameter` 生效"场景**（D10 的门，**必须在 `set_texture_params` 落地前是红的**）；`KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块在两台设备上绿（**~3300 个 framebuffer/用例，handle 复用的压力测试**）。
**⚠ 再基线检查点 1b：若 P4a 超过 39 天，同上处理。**

### P5 — 传输 + inproc applier + 发射表（12 天）

**交付物**：`MG_Remote/Client` 的发射表实现 `MGPipeScreen`/`MGPipeContext`；`Server/PipeApplier.cpp`；`ServerLoop`（`mgl-srv-io` + `mgl-srv-apply`，后者终身持有原生 context）；单一 hook 点 `MG_Backend/Init.cpp:48-70` 装 `BackendObject_Remote`；`MGPCaps` 快照；一条阻塞 `read_pixels`；client 侧保守 `MarkGpuWritten` 与 `emitSeq`；**client 侧块粒度 persistent-map 推送**（T2 档下强制，§7.8.1）；`InProcessTransport`；trace-replay 的 `SPLIT` 后缀与 `-DTRACE_TRANSPORT=` 接线（§13.8）。

**v2 规范条款：`InProcessTransport` 必须走与 spawn **完全相同**的 G3 编解码路径**，只在门铃/拷贝机制上不同。否则第 99 天的里程碑证明不了 wire 完整性，而 P6（第 104 天）才在关键路径上发现缺口。**`PipeApplier` 里加一条 debug 断言：任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界。**

**验收**：`ctest -R 'DirectGLES\.Split\..*(ClearThenReadPixels|Triangle)'` 在 `MOBILEGL_TRANSPORT=inproc` 下绿；**OpenRA trace 在 split 模式下 SSIM ≥ 0.99**；**`PersistentCoherentMapScenario` 绿**；**两个角色的峰值 RSS 记录在案**，作为 §7.11 内存预算的实测基线；`persistent-map-push` 字节量出数；任何未迁移的 `PipeInputs` 字段读产生 `Fatal{UnmigratedPipeInput}`。
**★ 第 99 天 — 首个 IPC 帧（`inproc`）。诚实标注：这是缩减路径**——client 数组、indirect-count 解析、索引宿主镜像在 split 下仍是 Fatal，全功能要等 P8。

### P6 — spawn transport（5 天）

**交付物**：`SocketTransport`（socketpair + fork/execve，**显式 envp 剔除 + `mobilegl_server_main` 内强制 Monolith 的双保险**）；`ServerMain`；`MOBILEGL_IPC_SERVER_PATH` 为主 + `dladdr` 兜底；就绪握手有界重试；client EOF 即时退出；server 死亡的 device-lost latch。

**验收**：P5 全部测试在 `MOBILEGL_TRANSPORT=spawn` 下绿；fork 链测试断言进程树只多一个子进程；`HeadlessGL` 的 fork 预检交互测试无孤儿 server（§11.3）；`run_android_retrace_local.py --case OpenRA --backend DirectGLES` 在 `35d0befa` 上 SSIM ≥ 0.99。
**★ 第 104 天 — 首个跨进程帧（缩减路径）。**

### P3b / P4b — 深化（Espryt）：memo 重键、dirty 反转、跨步描述符、XFB scatter、回读（29-38 天）

**交付物**：重键 `ResolvedDrawBuffers`、`PendingAttribValueMask`、`ConvertedFloat64Stream`、`SyncCurrentFBO` 四元组戳、`ResolvedTextureBindingMemo`、`SamplerPassMemo`、image sweep、program registry 到 `{slot, gen}`；**server 侧删** `g_unitTextureSyncList`、`g_fboTextureSyncList`、`g_unitSamplerLookupMemos`、`g_imageSweep*`、`DirectGLES.cpp:1372-1489` 的 ~115 行 unit-bindings epoch 推导，**同时在 `MG_Impl/Pipe/Tracker.cpp` 落地对应的集合 hash 抑制器**（§2.5、§4.4-4）；**dirty 归属反转**（§6.3，client 保 rect 模型与**按存储属主键控**的发射游标、发射后自清）；**`MGPSubRegion` 跨步描述符改造**（§3.5.6：`Managers.cpp:4274-4326` 从描述符取步长，替代 `uploadData == mipData` 指针比较与整 level 步长算术）；**XFB scatter 搬到 client**（§6.2.1）；**删** fragColor 重推导 workaround 与 `g_broadcastMemo*`；用推送状态退役 9 条陈旧性判定里的第 4-6、8-9 条；Espryt 的 raw-depth-fetch `SamplerObject` 原生化；回读 / pack state。

**验收**：~25 个纹理场景（`TextureView`、`LayeredTextureReadback`、`ImageSizeAfterRespec`、`FormatlessImageBake`、`NonCoreImageFormat`、`ImageFormatQualifier`、`ImageTargetKind`、`ImageLoadStoreSso`、`UnboundImageDescriptor`、`SwizzleAccessRoutine`、`IntegerBorderColor`、`PixelStoreSweep`、`SampledSetStaleness`、`ThreeChannelAttachment`、`BufferTexture`、`CopyImage*`×3、`ClearTexImageUndefinedLevelZero`、`DepthStencilReadback`×3、`PackedWordReadback`）；21 个 program 场景 + 整个 `MG_Test/ShaderTranspiler` 目录；两台设备上完整 `KHR-GL46.texture_*` / `internalformat.texture2d.*` / `shader_image_*` / `packed_pixels` 块，conformance 在 pull 基线 0.5pp 内；**每一个 Iris trace**；
**v2 新增三个门**：
- **`TextureUploadShapeScenario`**：逐纹理逐帧的上传形状（box vs N region、作业数）录金标比对——**+6ms 悬崖由形状相等把关，SSIM 对它不敏感**；**Mali 上帧时增量必须发布**；
- **view/owner 发射游标别名场景**：通过 view 上传、经属主采样（以及反向），跨 draw 边界各一次（§6.3 修正 1）；
- **verify 保留模式**：`MOBILEGL_PIPE_VERIFY=1` 下 `resource_subdata` 的 `(unionBox, regionCount, regions[])` 与快照重算逐项相等（§6.3 修正 2）；
- `XfbAfterClipDistance` / `XfbCaptureBufferReuse` / `XfbRepeatedCapture` / `TessellationXfbCapture` 与 **`KHR-GL46.transform_feedback.capture_special_interleaved_test`**（scatter 的 `gl_SkipComponents` 空洞保留，§6.2.1）。

### P7 — DirectVulkan（Magma）全量迁移（80-104 天，可与 P5/P6/P8 并行）

> 子系统 1（pipeline+动态状态）与子系统 4（VertexInput/VaoDrawMemo）已在 P2 交付，所以是 §5.5 的 85-111 减去 5-7。

**交付物**：§5.5 的其余 10 个子系统，重点四项：`SetupDrawSnapshot` 的 ~14 个探测字段（含两个**有损**的版本求和）塌成 dirty mask 比较；**`UniformManager` 的 8 类占位 `TextureObject` 换成原生 `VkImage`+view+descriptor**（~120 行删除，34 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个消失）；**具名 UBO 的 host payload**（D-B8：`ResolveUniformBufferPayload` `UniformManager.cpp:2022/2052` 改从 `set_shader_buffers` 的 `MGHostSpan` 取，`kCapNeedsHostUboBytes` 门控）；**blit / depth-mipmap 内部 shader 烘焙成签进树的 SPIR-V + uniform location + UBO 布局，由一个 `MG_Test` 重跑树内 glslang 逐字节比对的用例守新鲜度**；`VertexInputStateFactory` 的后端堆裸指针写回**直接删除**；`VkRenderPassManager` / `VkTextureManager` 的**节点式容器纪律原样保留**（D18，postmortem 注释逐字带进 review checklist）。

**验收**：367 集成 + 40 trace 在 DirectVulkan 的 monolith-push 与 split 下全绿；verify 零分歧；**`nm -D libMobileGLServer.so | grep glslang` 为空**——这是整个论点的强制执行点（**依赖 P0.5**）；`UnboundImageDescriptor`、`SampleMaskScope`、`ImageLoadStoreSso`、`AtomicCounter`、`SsboArrayDynamicIndex`、`NonCoreImageFormat`、`Orientation`、`DepthStencilReadback*` 场景；**Iris trace 上 `stage-ubo-named` 逐帧字节量发布**（D-B8 的定尺依据）；两台设备 CTS 在 0.5pp 内。
**⚠ 再基线检查点 2：P7 中点（第 40-52 个工作日）若已完成子系统 < 40%，立即重定基线**——P3a 的检查点发现不了 Magma 特有的超期，而 P7 在单跑道下位于关键路径。

### P8 — emulation 下放 + 索引宿主镜像 + 协议广度（12-16 天）

**交付物**：`MG_Impl/Pipe/HostResolve.cpp`——client 数组范围计算、**最大索引扫描**（`TryComputeMaxIndexFromHostBytes` 移到 client，唯一的无界应用指针读）、**`*IndirectCount` 计数解析**，每一条前面都有 §4.8.1 **逐站点表**规定的 reconcile（**不是笼统的 publish/wait/drain**：`*IndirectCount` 只做 `SyncPersistentMappedRange()`，因为 monolith 也只做这一个，`DirectGLES.cpp:4666-4667`）；`MGHostSpan` 的 split 填法；**`Server/IndexHostMirror`**（D-B7、§7.10）；**CopyImage shadow 镜像搬到 client**；`draw_vbo(info, indirect, ranges[], numDraws)` 收编 multi-draw 族（**分档仍在 server**）；viewport-array 回放验证在一次 pipe 调用驱动下各遍之间观察到的状态与今天一致（`EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`，`DirectGLES.cpp:3841`）；`generate_mipmap` 返回 level 计划（**形状，不带字节**）与 CPU 回退的纹素；**G3 的"单条记录大于段容量"分块/降级路径**（§7.1.1）；§9.3 的无 present fence tick 与一个无 present 的 split 用例。

**验收**：`ctest -L integration-gpu -R '^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` **逐名相同**，DirectVulkan 同；40 个 trace 在 split 下双后端 SSIM ≥ 0.99，含两个 `coherent_as_flush: true` 的 Create fixture（**两种模式都开着该开关跑**）；**新增 `ClientArrayAfterComputeWriteScenario` 绿，且去掉那次等待必须能看到几何缺失**；**`create-indirect` fixture 上 `roundtrips-per-frame` 读零**（§4.8.1 的绊线：证明没有给 `*IndirectCount` 平白加一次 publish-and-wait）；**`index-mirror-bytes` 与 `index-bytes-shipped` 逐用例发布**；`MultiDraw`、`PrimitiveRestart`、`ViewportArray`、`DrawParameters`、`CopyImage*`×3、`GuiBatch` 场景。
**★ 第 145 天 — 全功能 split。**

### P9 — 反向通道（10 天）

**交付物**：`SEG_REPLY` 4KiB slot 池；阻塞 `read_pixels`；PBO 回读 fire-and-forget；`on_gpu_written{res, ranges}` 收窄（配 `writableMask`）；`on_buffer_writeback` **按操作级批处理**（今天两处逐行循环：`Utils.cpp:2342`、`DirectGLES.cpp:7633`）配 epoch bump 的排序规则（§6.4）；`on_xfb_scatter_ready` + client 侧 scatter（§6.2.1）；`on_texture_writeback`（一个生产者）；`on_mip_levels_generated`（**只带形状**）；**`on_texture_pull_request` 四条缓解全上 + `resource_subdata_complete` 终止符**（§6.5）；`on_gl_error` 有序 + **收窄后的** `kNeedsAck`（§6.4）；`on_caps_invalidated`；`on_surface_changed`；**`on_log` 按严重级分级**（≤WARN 有损 / ≥ERROR 无损 + 每秒速率限制器 + "N errors suppressed"）；`SEG_EVENT` 溢出策略 + 等待循环内排空（§8.4）。

**验收**：`DepthStencilReadback`×3、`PackedWordReadback`、`LayeredTextureReadback`、`ClearThenReadPixels`、`XfbAfterClipDistance`、`XfbCaptureBufferReuse`、`XfbRepeatedCapture`、`TessellationXfbCapture`、`KHR-GL46.transform_feedback.capture_special_interleaved_test` 在 split 下绿；**`TextureRemintPullScenario` 绿**，**且它必须包含一个"答不出来"的用例**（一张只被渲染过、随后被 image-bind 的纹理）**并在终止符落地前表现为 apply 线程挂死/超时**；**拉取计数逐 trace 用例发布**；故障注入：client 被 credit 阻塞时灌满 `SEG_EVENT`，两侧都必须恢复；**日志洪泛下注入一次 backend link 失败，那行 ERROR 必须出现**。

### P10 — sync / query / present 节奏（6 天）

**交付物**：client 铸造 sync 与 query handle；轮询入口成为门铃点 + `MOBILEGL_IPC_POLL_ESCALATE` 饥饿升级（§8.2）；**fence 完成度来自真的逐 fence 退休**（§8.5，不是 present 水位——那正是 MC 1.21.5 native-heap OOM 的成因）；DirectGLES 的非 present fence tick；`present` 严格 1:1；`MOBILEGL_IPC_PRESENT_CREDIT` 默认 1 + 叠加公式；逐帧 roundtrip 计数器与**输入延迟直方图**；§8.6 的三个独立 `dev` monolith 修复。

**验收**：`XfbPrimitiveQuery`、`PrimitivesGeneratedNoXfb`、`AsyncCompile` 在 split 下绿；**40 个用例上 draw/state/upload 路径的 roundtrip 计数器读零**，条件渲染与阻塞 query 次数逐用例发布；零 timeout 轮询循环测试在有界时间退出；`bench.sh` 在 `35d0befa` 上配对 A/B：两侧都关采纳时 split 帧时在 monolith 10% 内，输入延迟直方图 p50/p99 记录在案。

### P11 — persistent map 与 ≥16MiB 采纳（8 天；spike B 全否则缩为 2 天）

**交付物**：由 P0 spike B 驱动的 POST 探针档位选择（T2 / T1 / T0，§7.8）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`MOBILEGL_IPC_ADOPT_TIER` 覆盖开关做负面对照。

**验收**：`LargeArenaAdoptionScenario` 在所选档位下绿；`improved-transparency-minecraft-26.3` 与两个 Create fixture SSIM ≥ 0.99；**`StorageBufferRegrowScenario` 发布 `map-persistent-roundtrips`**（T1 档下每次存储定义一次，不是每 store 一次）；`35d0befa` 上配对 reboot-clean 的 p99 帧时与峰值 RSS 对 monolith 采纳基线（p99 163→21ms、40→115fps、~400MB）——**split 在所选档位下 p99 不得回归超过 10%；若 T2 成为永久答案，其实测代价必须写进文档**。

### P12 — Android 生产窗口路径（10 天）

**交付物**：`android:process=":mgl"` 的 Service 收 Java `Surface`（Binder）后 `ANativeWindow_fromSurface`（minSdk 26 无公开 `ANativeWindow` 扁平化；树内先例是 `android:process=":bench"` 的 `BenchService`，§11.3）；server 生命周期绑 Activity；FCL 用户 env 与 plugin APK V2 开关表接线（**零新增管线**）。

**验收**：Minecraft 通过 FCL 在 spawn 模式下在 `35d0befa` 上双后端入世界；配对 reboot-clean bench + 输入延迟直方图；杀 server 产生干净的 device-lost latch；SIGKILL 故障注入。

### P13 — 退役 pull 路径（8-12 天）

**交付物**：删 `SnapshotFromGLContext()` 的**非 verify** 编译分支、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**保留 `MOBILEGL_PIPE_VERIFY` 及其 `SnapshotFromGLContext()` 与 `MG_State` include**（D-B5）；**交付 MGPipe recorder 金标模式**（`MG_Test` mock backend → 录制器，§13.4-9），作为不依赖 `MG_State` 的长期语义门与开放问题 11 的答案；删 `set_residual_value_state` 与 `ResidualValueBlock`；`MG_Backend` 的 `MG_State` include 收缩到 `MGPipeValueTypes.h`；**在计数器活着的情况下重调所有幸存缓存的容量**（Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`）并把它们变成带 env 覆盖的调优参数；最终符号/尺寸/CPU 报告。

**验收**：**`static_assert(sizeof(ResidualValueBlock) == 0)` 编译通过**；**三道纯度门在非 verify 构建上转绿**（include 图门 A、符号门 B、未声明门 C，§13.3-①）；verify 构建仍能跑且零分歧；MGPipe recorder 金标在 40 个 trace 上建立并可回归；全套门（367 × 2 backend × {monolith, split}、428 单元、40 trace SSIM ≥ 0.99、两台设备 CTS 在 `81b17c0b` 基线 0.5pp 内）；**monolith 逐线程 CPU 在两台设备的 p50 与 p99 上不差于 P0 基线**——本设计的性能主张在这里成立或倒下。

### 14.5 总估时、里程碑与 CTS 周转

**逐阶段求和（低端 / 高端，单跑道累计）**

| 阶段 | 天 | 累计（低端） | 构成（§5.4/§5.5 的行） |
|---|---|---|---|
| P0 | 9-11 | 9 | Espryt 0a(1-2) + Magma 0a(~1) + 共享基建 |
| P0.5 | 6-9 | 15 | 头文件抽取（新增） |
| P1 | 10-13 | 25 | `PipeInputs` + 逐 verb 填充 + verify（共享基建） |
| P2 | 18-26 | 43 | Espryt 1(3-5) + Magma 1(3-4) + Espryt 0b(5-7) + Magma 4(2-3) + tracker/CSO/G7(4-6) + 聚合世代(1) |
| P3a | 18-23 | 61 | Espryt 2(10-13) + 3(7-9) + LEGACY 维护(1) |
| P4a | 26-34 | 87 | Espryt 4(7-9) + 5 前半(11-15) + 6 身份半(7-9) + LEGACY(1) |
| P5 | 12 | 99 | IPC 跑道 |
| P6 | 5 | 104 | IPC 跑道 |
| P3b/P4b | 29-38 | 133 | Espryt 5 后半(12-15) + 6 后半(7-9) + 7(5-7) + 9(5-7) |
| P8 | 12-16 | 145 | Espryt 8(8-11) + Magma 份额(4-5) |
| P9 | 10 | 155 | IPC 跑道 |
| P10 | 6 | 161 | IPC 跑道 |
| P11 | 8 | 169 | IPC 跑道（spike B 全否则 2） |
| P12 | 10 | 179 | IPC 跑道 |
| P13 | 8-12 | 187 | Espryt 10(4-6) + Magma 11(4-6) |
| **P7（Magma）** | **80-104** | **267** | §5.5 的 85-111 减去已在 P2 交付的子系统 1 与 4 |

**报作 267-337 人天**（不含 CTS 周转）。两个工程师、P7 与 P5/P6/P8 并行 → **约 7-9 个月**，真正的约束是两台设备的争用而不是人头。

**与独立成本分析的一致性**：一次独立的改造成本调研给出 backend 工作**单独** 202-266 天（Espryt 95-125 + Magma 85-111 + 共享 22-30）。本节的 267-337 = 那个区间 + IPC 跑道 51 天 + P0.5 的 6-9 天，**方向一致**。v1 报的 200-260（含 IPC）落在其乐观端之外，已作废。

**里程碑（低端估计）**：第 **25** 天 verify harness 全绿（零产品风险，**不是** GO/NO-GO）；第 **43** 天 **GO/NO-GO**（含一片真 Track H，出口见 §0.5）；第 **99** 天首个 `inproc` IPC 帧（**缩减路径**）；第 **104** 天首个跨进程帧（**缩减路径**）；第 **145** 天全功能 split；第 **187 / 267** 天三道纯度门转绿。

**再基线检查点**：P3a > 27 天；P4a > 39 天；P7 中点（第 40-52 个工作日）完成子系统 < 40%。任一触发，先跑 `inproc` 的证伪数字再决定是否继续。

**CTS 周转必须单独计价，不折进阶段估时。** `gl44to46` caselist 约 56,271 例。分层门控：逐阶段只跑该阶段改动可能影响的具名 CTS 块（P4a 的 `packed_pixels`、P3b/P4b 的 `texture_*`/`shader_image_*`、P9 的 `transform_feedback*`），**完整 caselist 只在五个架构边界跑**（P0.5 头文件抽取、P3a handle、P4a framebuffer/纹理身份、P3b/P4b 纹理、P13 纯度）**以及每次合并 `dev` 之前**，且放在 CI 而不是关键路径上。设备锁协议照旧。若实测周转仍主导排期，**诚实做法是加宽估时而不是削弱门**。

---

## 15. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| **B-R1** | **总成本 267-337 人天，首个跨进程帧在第 104 天、全功能在第 145 天。** 排期驱动的评审可以只凭这一条否掉本方案 | 把价值排在承诺之前：P0-P2（43 天，其中 28-39 天是 MGPipe 独有）交付 handle 化 twin 与内容寻址的渲染状态 CSO——**零 IPC 风险的可测量 monolith 工作**——并产出字节/调用计数器与第一个逐线程 CPU 数字与 **Track H 单位成本**。**第 43 天显式 GO/NO-GO，两个出口写在 §0.5。** P13 是一个完全自洽、不含任何 IPC 的 monolith 交付物；P5 的 `inproc` 只要 12 天 |
| **B-R2** | **中心性能主张未经测量，且它的基线被 v1 高估了一个数量级。** 可达性遍历是**搬走**而不是消失；真实稳态拉取只有每 backend 每 draw 10-25 次 accessor（§2.3.1），不是 124/169 | 字节**与调用**计数器是 **P0 交付物**。每阶段验收用**逐线程 CPU 时间**，两台设备、reboot-clean、配对，**并设绝对 ns 上限**（相对噪声阈值在真实基线下会平凡通过）。P2 除渲染状态外**必须含一片 Track H**，否则测的不是要决定的事。加 Blaze3D blend-toggle 微基准与 CSO 内容寻址的负面对照。**先清工作树 per-draw `fprintf`** |
| **B-R3** | **monolith 字节一致门按构造死亡**，逐名集成基线也随之移动 | 五部分替代门，全部在 P0/P0.5/P1 落地（§13.3），其中 ② 逐 draw 逐字段影子比对在语义上严格强于任何符号 diff。两条字节等式仍作断言保留。**逐名功能基线明确定义为"P1 出口的重构后 monolith"**，而 P1 出口自己先用 verify 证明等价于 `81b17c0b`；`81b17c0b` 只作性能锚点 |
| **B-R4** | **server 发起的纹理拉取是新停顿类**，触发路径之一（整格式再生 `Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就会触发、无法被 hint 预防；**而且存在 client 根本答不出来的 level**（纯渲染产生 / `CanMirrorCopyImageShadow` 拒绝的 copy 目标 / GPU 生成的 mip），会让 apply 线程永久 park | 四条缓解同时上：`imageBindableHint` 预防主因；**异步** park-and-re-emit 让停顿落在 `mgl-srv-apply`；**`resource_subdata_complete` 终止符可携带零 region**，server 带着"已分配但为空"的存储继续（正是 monolith 的行为，`DirectGLES.cpp:6270-6271`）；保留 LRU **默认关闭**（`MipmapStorage` 保有完整 CPU 影子，所以拉取总能被服务，缓存买的是延迟不是正确性）。`TextureRemintPullScenario` **必须包含无解用例并在终止符前是红的**，**拉取计数逐 trace 用例发布** |
| **B-R5** | **P3b/P4b（29-38 天）与 P7 中的 `VkTextureManager` 是最大最险的段**，压在实测 +6ms/frame 悬崖（rect 列表 vs union box）与 7 条 fallback-repack 路径上，**而后者的可行性判定 `uploadData == mipData`（`Managers.cpp:4278-4283`）在 split 下不成立**——它要求上传源就是整 level shadow 并按整 level 步长跨步 | `resource_subdata` 同时带 box 与 region 列表、**server 选形状**；**`MGPSubRegion` 显式携带 `srcRowStride`/`srcSliceStride` 与 `sourceIsVerbatimLevelShadow`**，`Managers.cpp:4274-4326` 改为从描述符取步长（形状照抄已存在的 `UnpackStagingBlock`，`:4340-4390`，ring 路径本来就紧密重打包）。**这项工作计入子系统 5 的天数**（+3-4 天），不再列为"原地不动"。**`TextureUploadShapeScenario` 录金标比对上传形状与作业数**，因为 SSIM 对这个悬崖完全不敏感。P3b/P4b 拆成两个可独立落地的半 |
| **B-R6** | **tracker 完整性**：推送之后 server 不能再重读活状态校验快路径。任何 tracker 忘记发的 mutator 会静默漂移。历史上最危险的正是这个形状（`DirectGLES.cpp:1441-1465`） | **四层**：**(1) 构建期** G5 的逐 verb 世代表 + G7 的 render-state setter 一致性测试；**(2) 运行期** poison 在**需要该字段的那个 verb** 上 `Fatal`（不是某个后续 draw）；**(3) 语义** `MOBILEGL_PIPE_VERIFY` 逐 draw 逐字段比对（**含纹理 subdata 的保留模式**，否则最危险的子系统是瞎区）；**(4) 枚举** `gen_pipe_dirty_surface.py` 枚举 `MG_Impl` 里每个 mutator → 必须 bump 的聚合世代，CI 上未映射即失败。**迁移粒度是一个 accessor。** 477 行 inventory 保留为覆盖检查表 |
| **B-R7** | **`AcquirePersistentMap` 跨进程无解**会葬送 MC 26.3 的结果，而没有任何目标平台的支持被验证过 | **显式隔离**：改造期完全不碰，只有 IPC 那一步会打破它。决策交给三档 POST 探针与 **P0 第一周的 spike B**（§7.8）。T2 前端已在三处容忍并让 client 侧块推送成为强制（P5 交付）。若两台设备都否，P11 从 8 天缩为 2 天。**注意 T1 是每次存储定义一次 round trip，不是每 store 一次**（`StorageBufferRegrowScenario` 发布计数）。**不让一个平台未知数挡住 267 天的接口工作** |
| **B-R8** | **D18 的节点式容器纪律在重构中丢失**：`m_renderbufferResources` / `m_textureResources` 是**故意**用 `std::unordered_map`，一次扩表搬迁曾让 `BlitFramebuffer` 静默停在 "layout undefined"（`VkRenderPassManager.h:375-397`） | D18 是重键表里**唯一**标为 UNCHANGED 的身份行；**postmortem 注释必须逐字带进 P7 的 review checklist**。slot 数组在插入下稳定，实际改善了处境——但仍然点名 |
| **B-R9** | **逐 backend 的行为不对称被统一接口抹平**（Magma 故意不注册 `ResidentSubData`，`VkBufferManager.cpp:104-111`；`PrefersCpuXfbPrimitiveAccounting`；DirectVulkan 留空的 8 个槽） | 可选性是**接口的一等属性**：null 项在本代码库里**已经**表示"未实现，前端回退"（`BackendObject.h:212-215, 265-269`），`MGPCaps` 携带显式 `callMask`。**但 v2 收回了用 cap 位表达 emulation 归属的做法**（D-B7）：`ResolveTierForBatch` 逐 batch 用 `programReadsDrawID`（server 独有事实）选档，且两个 backend 都做 restart 重写，所以那五个 cap 位没有门可控。归属规则改成一句话 + 一个 `kCapNeedsHostIndexBytes` |
| **B-R10** | **接口在未测量的形状上过早冻结**；若干 server 侧缓存的容量是按拉取模式调的 | payload 结构从第一天走 structSize-first 版本纪律，可增长。字节**与调用**计数器在 P0 落地。**`stage-ubo-named` 出数之前不冻结 `set_shader_buffers` 的 host payload 形状**（D-B8）。**P13 在计数器活着的情况下重调所有幸存缓存的容量**，并把它们当作带 env 覆盖的调优参数。screen/context 划分在 P0 定进头文件但按 context 计数 == 1 实现 |
| **B-R11** | **58 行非箭头 `pGLContext` 用法的迁移缺口**；`DirectGLES.cpp:146` 的 `.get()` 与 `:142` 的 `decltype` 别名 `sed` 完全抓不到 | §2.4 已逐形态分类。P1 的交付物**包含这份 58 行清单的逐条转换**。**纯度门 grep 的是 `pGLContext` 而不是 `pGLContext->`** |
| **B-R12** | **残余值块是迁移期边界上的一个洞**：poison 抓不到"两侧布局不同"，而 monolith 的 verify harness **看不见它**（两侧是同一个 TU） | 逐成员 `offsetof` 断言 **加上** split 模式下逐字段序列化（走 G3 编解码器）。块的字节量单独计一类。`static_assert(sizeof == 0)` 让退役是编译错误 |
| **B-R13** | **`SEG_EVENT` 的 ERROR 无损化重新引入死锁** | 每秒 ERROR 速率限制器 + "N errors suppressed"；`MGLOG_E_ONCE` 的 latch 变 per-server；P9 的故障注入门要求"日志洪泛下注入一次 link 失败，那行 ERROR 必须出现"**且**"两侧都恢复"（§8.4） |
| **B-R14** | **排期估计**：v1 的阶段天数与它自己的子系统表矛盾，且低于同口径的独立分析 | §14.5 的每个天数都是它所含 §5.4/§5.5 行的求和，**算术公布**。总数改报 **267-337**（不含 CTS）。三个再基线检查点按求和后的上界 +50% 设定。CTS 周转**单独计价** |
| **B-R15** | **在 GL setter 时刻推送**会让整件事变慢，且这是最容易被后续实现者做错的一处 | 写成规范条款并给出证据（`DirectGLES.cpp:2029-2032` 的 Blaze3D per-batch blend toggle）；P2 的设备门直接暴露它。**v2 补一条同等重要的**：`glTexSubImage` **不是** GL 调用时刻推送的对象（它根本不调 backend 表，`GL_Texture.cpp` 只有 3 处 `MarkStorageDirtyRegion`），逐调用发 `resource_subdata` 会精确复现 Mali 的 ~100 作业形状（+6ms/frame）。规则的正确措辞在 §4.1.1；`resource_subdata` 逐帧发射次数进计数器并在 MC 动画图集 fixture 上设上限 |
| **B-R16（v2 新增）** | **stage C 之后 `MOBILEGL_PIPE_PUSH` 不再是对"旧 backend"的 A/B**：位清零时 `SnapshotFromGLContext` 仍要合成 handle，backend 仍跑重键后的 memo 代码，两个分支跑同一份新代码；一个重键 bug（D1/D2/D3/D11/D13 那一类）在两臂都在，位图二分不出来 | 在 §5.7 写明这条口径收窄。为 P3a 与 P4a 加**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`，让前两波 handle 化保留一个真正的旧-vs-新臂；随 pull 路径在 P13 退役。维护成本各阶段 +1 天，已计入 |
| **B-R17（v2 新增）** | **`MOBILEGL_PIPE_VERIFY` 是唯一的语义门，而 v1 的 P13 删掉了它的参照物**（`SnapshotFromGLContext`），删完之后设计没有语义绊线 | `SnapshotFromGLContext()` 与它的 `MG_State` include 整体包在 `#if MOBILEGL_PIPE_VERIFY` 里保留过 P13；三道纯度门**只跑非 verify 构建**；P13 另交付 MGPipe recorder 金标模式作为不依赖 `MG_State` 的长期语义门（同时是开放问题 11 的答案） |
| **B-R18（v2 新增）** | **monolith 的净代码量是增加的**（§2.7：约 +6,650 手写 + 4,000 生成，对 ~372 行真删除），所以"~550 行删除"不能当主论据 | 把 §13.3-④ 的**逐线程 CPU 数字**作为 monolith 论据的主体，删除清单降级为佐证。§2.7 公布净 LOC 估计，让 B-R2 有一个可证伪的预测。**若 P2 与 P13 的 CPU 数字持平而非改善，monolith 论据只剩架构性收益（ABA 不可表达、排序 hazard 消失、`inproc` 杠杆），必须据此重新评估是否值得** |

---

## 16. 开放问题

1. **client 侧 dirty 走查的真实每 draw CPU 代价是多少？** 中心性能主张是"遍历搬走而不是翻倍"，而真实基线只有每 backend 每 draw 10-25 次 accessor（§2.3.1）。P2 的头号数字，按逐线程 CPU + **绝对 ns**、两台设备报。
2. **真实语料上纹理重铸拉取的实际发生率？** `imageBindableHint` 能预防主因，但整格式再生（`Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就触发。若 MC 或 Iris fixture 上实测率非平凡，保留 LRU 从"默认 0"升为强制并需要真预算。
3. **`AcquirePersistentMap` 跨进程能不能成？** P0 spike B 第一周回答。未验证：`VK_KHR_external_memory_fd` 的 host-visible-coherent 支持在四条 lane 上的可用性；GLES 侧能否用 `GL_EXT_memory_object_fd` + `glBufferStorageMemEXT` 走同一条路。
4. **渲染状态的 wire 粒度**：pipeline 子集的 chunk 划分定下来之后，CSO LRU 的容量（暂定 64）与 `set_dynamic_state` 的 chunk 粒度仍需 P0 计数器定。
5. **`MG_Util` 的切割缝在哪里？** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、像素/纹理格式处理器、POST 探针、loader；client 需要 glslang phase A/B 与反射层。**P0.5 解决了 `ProgramObject.h` 这一处**，但 `MG_Util` 内部是否存在一条干净的 Transpile-vs-Reflect 缝**仍未审计**。
6. **一份反射归档能服务三个消费者吗？** Espryt 读前端表，Magma 跑 SPIRV-Reflect，而 `DirectVulkan.cpp:161` 为 `glGetProgramResource*` 又反射了第二遍。
7. **viewport-array 回放能塞进一次 `draw_vbo` 吗？** 今天它从 14 个 draw 入口经 `ForEachViewportRoutingPass` 重发应用的 draw N 次，而 `EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`（`DirectGLES.cpp:3841`）。未验证各遍之间观察到的状态是否与今天一致。
8. **`ResidentSubData` 的不对称该怎么收口？** null 项保住今天的行为，但拆分工作可能正是给 Magma 补一个真实现的时机——那是**行为变更而不是重构**，应作为独立 `dev` PR。
9. **`SEG_STAGE` 的上限定多少？** 六类新字节（§7.1.1）需要 P8 之后用 MC in-world 与 Create 两类 fixture 的 `stage-*` 计数器给 p99 占用。**并且 G3 的"单条记录大于段容量"分块路径需要设计与测试**。
10. **`FramebufferSrgb` / `DepthClamp` 无存储是潜伏 bug 还是有意为之？** 六个 backend 消费者今天读到恒定 false（`RenderState.cpp:380, 428-429`）。**必须在渲染状态 chunk 表冻结之前回答**。**（P0 实测修正）事实已查清、结论待拍板**：`FramebufferSrgb` 六个读点消费的是编译期常量 `false`，`DepthClamp` 零读点；两者的 `glEnable` 被静默吞掉且不报 `GL_INVALID_ENUM`；41 个 fixture 无一开启。建议是在冻结前补真存储、并把 `FramebufferSrgb` 划进 D-B1 的 pipeline 半边（它改变 attachment/blend 的解释）——**由计划所有者拍板**，详见 §13.4-6。
11. **P13 之后还有 server 侧"第二意见"吗？** **v2 部分回答**：保留 verify 构建（D-B5）+ P13 的 MGPipe recorder 金标。但 split-only 的**渲染** bug（而非状态推送 bug）仍然没有 server 侧第二意见——recorder 只覆盖推送内容，不覆盖 backend 对它的解释。
12. **~~client 侧 restart 重写与 indirect-count 解析会不会改变可观察行为？~~** **v2 已关闭**：D-B7 把 restart 重写与 multi-draw 分档留在 server，monolith 行为零变化，诊断仍落在原线程。**只有 `*IndirectCount` 的计数解析搬到 client**，它的 decline 路径（`DirectGLES.cpp:4682-4688`）随之落到应用线程——这是改善而非退化，但需要在 P8 的验收里核对日志文本与顺序。
13. **Magma 的两个内部 shader 烘焙后，uniform location 与 UBO 布局能否在没有活 `ProgramObject` 的情况下表达？**（`VulkanRenderer.cpp:4238-4241, 4319-4324, 8450-8452`）未做原型。
14. **推送模型会改变哪些按拉取模式调过的缓存命中率？** Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`；Espryt 的 4096/256/64 槽 `TwinLookupMemo`（后者会消失）。幸存者的容量在 P13 重调。
15. **（v2 新增）monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是一个潜在缺口？** `DirectGLES.cpp:4666-4667` 只做 `SyncPersistentMappedRange()`，而 compute 写的 indirect buffer 理论上需要前者。**这是一个独立的 `dev` 问题，拆分不得借机"顺手修"**——那会改变基线并让逐名对比失去意义。
16. **（v2 新增）索引宿主镜像的实际内存占用？** D-B7 的预算是 64 MiB 默认上限，但 MC/Sodium/Iris 语料里 element-array buffer 的总量未测。若显著超预算，退化路径（逐 draw 通过 `MGHostSpan` 传送）的频率与代价必须实测，因为它会把 §7.11 的内存预算和 §12.1 的零 round trip 主张同时削弱。

---

## 17. 对 `Feat/CS-Delta-IPC` 的复用清单

> 分支 worktree `../MobileGL-CS`。判定分三类：**REUSE**（原样取）、**CHANGE**（取走并改造）、**DROP**（不取，逐条给理由）。

### REUSE（原样取）

| 路径 | commit | 备注 |
|---|---|---|
| `MobileGL/Protocol/mg_protocol_base.h` | `546895aa` | 干净无依赖的词汇（`MobileGLResult`、span、`ShmRegion`、id typedef、**structSize-first 版本纪律**）。后者直接是 B-R10 的对策 |
| `docs/CS_Refactor/HandleSessionGeneration.md` | `546895aa` | 分支上最好的产物。三处修改：handle 清单补 `RenderbufferObject::GetLifetimeId()`——**只补它，不补 `GetVersion()`**（`GetVersion()` 只是 delta 触发器；推送模型里 `glRenderbufferStorage*` **本身**就是一次 pipe 调用）；把第 2 节的 server 侧 share-group 要求降为 v2（§1.2）；把"lifetimeId 不符 → 销毁重建"改成 `Fatal` |
| `docs/CS_Refactor/HANDOFF.md` 第 6 节"已知坑清单" | `d5c00b9d`/`5964628d` | 逐字留作事后复盘：路径转换、versionCode 降级、双设备 `ANDROID_SERIAL`、flatbuffers camelCase accessor、union vector 产生指针、Release 下 `MGLOG_D` 被编译掉、嵌套 submodule 配方、`assembleTraceDebug` 改名 |
| `MobileGL/Protocol/tests/ProtocolSmoke.cpp` | `546895aa` | schema 往返门（默认改 ON） |
| 根 `CMakeLists.txt` 的 `EXISTS` 保护 + `.gitmodules` 条目 | `546895aa` | 去掉 `NOT ANDROID`，另加 §13.8 的 include-dir guard |

### CHANGE（取走并改造）

| 路径 | commit | 改造 |
|---|---|---|
| `MobileGL/Protocol/protocol.fbs` | `546895aa` | 保留它的 delta 目录构想、`RenderStateBlob` **整块**思想、`BufferShmAdopt`、命令清单、事件分类学。改：热路径转 `struct` + ring（§8.1）；记录种类改为由 `PipeCalls.def` 生成，与 `MGPipeTypes.h` 逐条 `static_assert` 对齐；删掉冗余的 `inlineBytes`/`data` 双胞胎（`:111-112`、`:125-126`，两半代码对哪个字段是真的意见不一：`ServerCore.cpp:184-208` 只读 `data`，`StateEmitter.h:60,111` 只写 `inlineBytes`）；加 `AuxRequest`；kind 枚举生成 + 每 kind `static_assert` + 运行期边界检查 |
| `MobileGL/ServerCore/ServerCore.{h,cpp}` | `65717b4c`+`c2260dd8` | 保留握手→解码→apply→credit 的**形状**与 plugin manifest loader 思路，改造成 `Server/PipeApplier.cpp` + `Server/ServerLoop`。修：单次校验 + 零拷贝解码（今天校验两次外加一次整体拷贝，`:492-498` 与 `:218-221`）；io/apply 分线程（`:404-406` 自承 worker 从未落地）；完整事件集（`SendEvent` 只实现 `BATCH_APPLIED`，`:373-382`）；credit 用最后一条实际 seq（`:427` 的 `baseSeq + items.size()` 只有 `baseSeq==0` 时才对）；接收缓冲不能是对着 64MiB 帧上限的固定 4MiB（`:478`）；真正的段生命周期（`m_segments` 只增不减，`blobOwners` 只 push 不释放） |
| `MobileGL/Remote/InProcessTransport.h` | `65717b4c` | 重表述在 C++ `ITransport` 上；单侧 shutdown（今天 `:89-92` 连对端 inbox 一起关）；真段生命周期（`Unmap`/`Close` 今天是 no-op）；补 §7.2a 的双向 doorbell（condvar 版）。**并且必须走与 spawn 相同的 G3 编解码路径**（§14 P5 规范条款） |
| `MobileGL/Remote/Framing.h` | `65717b4c` | 保留帧格式；`m_pendingSize`/`m_haveHeader` 改 `mutable`（今天 `const_cast`，`:81,85`）；`Feed()` 真校验 magic 与长度（今天永远返回 OK，坏 magic = 静默永久挂起）；缓冲不足返回所需大小且**保留消息**；真正在 socket transport 里使用它（今天是死代码） |
| `MobileGL/RemoteClient/StateEmitter.h:39-307`（**仅 emit 半边**） | `b50f3348`+`d96be9f3` | 各域的字段遍历是真知识，而且**更直接可用**：那些字段集**就是** MGPipe 的状态对象 payload，抬进 `MG_Impl/Pipe/Tracker.cpp`。必须修的缺陷：GL name 换 `lifetimeId`/handle（今天 `:48-49, 85, 166-168, 203, 230` 全把 GL name 塞进 `handle`）、O(n²) 线性扫描换 slot 数组（`:175-181, 244-249, 253-258, 293-298`）、固定 6 attachment（`:232-236`）换 `MaxColorAttachments`、补上被跳过的 texture view（`:70-74`）。**applier 半边（`:312-501`）不取** |
| `scripts/extract_backend_read_inventory.py` | `546895aa` | 改造成 G6：**删掉制造"0 UNMAPPED"的前缀兜底规则**（`:234-241`），未知 accessor 一律 UNMAPPED 并编译失败；把真 pull point 与 signature handle 化分开统计。**用途改变**：它是 tracker 侧的**覆盖检查表**，真正的门是 §3.7.2 的**三道纯度门**。（`gen_pipe_dirty_surface.py` 在原分支没有任何对应物，是全新的。） |

### DROP

| 路径 | 理由 |
|---|---|
| `MobileGL/Protocol/bfa.h`（480 行） | "strict C ABI"不是 C ABI：`ServerCore.cpp:177-179` 把 FlatBuffers 生成表的指针交给插件，插件必须是 C++ 且链接 FlatBuffers（`StateEmitter.h:330,351,362,372` 就是这么用的）。手抄的 60 字段 `MobileGLDynamicParameters`（`:63-129`）自承尾部不全、同步脚本从未写过——正是已在本项目造成 481 例 CTS 失败簇的那类数据的**长期静默漂移炸弹**。而 MGPipe 根本不需要 delta-apply vtable：接口是两张生成的函数指针表 |
| `MobileGL/Protocol/mgruntime_api.h` + `MobileGL/UtilRuntime/*` | 360 行契约对 ~50 行实现（8 域实现 2 域）；唯一消费者传 `nullptr`（`ServerCore.cpp:61`）；缓存每次命中整份拷贝（`:79`）、按 `clear()` 淘汰（`:91-93`）；smoke 断言 `api->metrics == nullptr`（`RuntimeApiSmoke.cpp:66`）。它的唯一理由随 BFA 消失；且本设计里翻译全在 server（它无论如何要链 SPIRV-Cross），glslang 全在 client（§4.7） |
| `MobileGL/Remote/LocalSocketTransport.{h,cpp}`、`ShmFactory.{h,cpp}` 的**实现** | 从未被任何测试执行（`LoopbackSmoke` 用的是 `InProcessTransport`，唯一另一个消费者 `ServerHost` 编译不过）；每次 send 都 use-after-free（`:199`，`asio::buffer(next)` 指向局部 vector 而 lambda 捕获的是另一份拷贝）；按 wire 长度无上限分配（`:232-236`）；`Start` 里阻塞 accept/connect（`:116`、`:139-144`）；无 strand 且 `framesSent++` 非原子（`:177-178`）；**且完全没有 POSIX fd 传递**（`:296` 硬编码 `fd=-1`），Linux/Android 数据面一字节过不去。只保留 `ShmFactory.h:4-12` 作平台矩阵规格 |
| `MobileGL/ServerHost/main.cpp` | 编译不过（`:31,39,44,53-54` 对指针用 `.`，`c2260dd8` 改返回类型后成为死码）。`MobileGLServer` 在默认 ALL target 里，**分支 tip 无法完成一次完整构建** |
| `MobileGL/RemoteClient/tests/StateEquivalenceTest.cpp` | 把 delta apply 进第二个 `MG_State::GLContext`——它验证的正是本设计明确不存在的那条数据路径（server 侧没有第二份前端状态）；与生产 apply 路径零共享代码；只测全量 resync；`d96be9f3` 声称五域逐字段而文件只比了纹理、buffer、render-state blob、buffer binding slot（没有 VAO 属性/FBO attachment/RBO 格式比较）。**替代物是 §13.3-② 的逐 draw 逐字段影子比对**，它比的是同一份状态的推送版与拉取版 |
| `c7c9e346` + `29d721ef` 全部（share-group sessioning） | 非 v1 前提（monolith 只有一个 `GLContext`：`GLState/Core.cpp:20,1487`）；且非可合并质量：`VertexArrayState.cpp:+20-26` 往已共享的表里再压一个 default VAO 并重复 `Insert(0)`；四个头文件 `public:` 未复位泄漏私有成员；current session 是无锁进程全局，连它自己的 per-thread current 都没兑现；在状态权威里塞 `MOBILEGL_SESSION_SWAP` env kill switch 与 `s_defaultAdopted` 偷 context 的 hack。日后作为独立 PR 带多 context 测试落 `dev`（本设计的 `MGPipeScreen`/`MGPipeContext` 划分已经为它留好形状，§3.3） |
| `b50f3348` 的 `RenderState::InstallParameters` + 裸 `public:` | 本设计不需要 Install setter：server 侧的 working `RenderStateParameters` 由 `bind_render_state` / `set_dynamic_state` 的 chunk 散射填充（D-B1）。若日后需要整块安装，用正确作用域的方法或单条 friend，绝不靠裸 `public:` |
| `d96be9f3` 的 TRIAGE 指令（`DirectGLES.cpp:+2583-2590`） | per-draw `fprintf(stderr)`。**分支上每一次测量都跑在它上面。** 同规则适用于当前工作树的 `[IBOTX]`/`[BUFTX]`（P0 清除），并由 CI grep 门永久禁止（§13.8） |

---

## 附 A：接口调用目录速查表

> Flags：`A`=`kNeedsAck`、`B`=`kHasBlob`、`V`=`kVarTail`、`H`=`kHostSpan`、`R`=`kReplySlot`、`O`=`kOptional`。

> **（P0 实测修正）本表按功能分组，合计 68 条唯一调用**（不是"约 74"）。按 `.def` 的 Class 列才是权威口径：**screen 10、ctx-query 6、CSO 13、`kCtxState` 17、`kCtxObject` 9、`kCtxVerb` 13**。下面各小标题的括号数是**旧的功能分组数**，其中 CSO 与 `set_*` 对 `bind_sampler_states`/`set_sampler_views` 重复计数、query 族被并进 screen、transfer 标 12 而实列 11。**`PipeCalls.def` 是唯一真相源，线上 opcode 就是行的位置，所以目录必须是唯一记录的集合。**

### `MGPipeScreen`（14 → **10**，query 族 6 项归 `kCtxQuery`）

| 调用 | payload | flags | 取代 |
|---|---|---|---|
| `get_caps` | `MGPCaps` | R | 40 `pActiveBackendObject->` + 89 caps 读点 |
| `resource_create` | `MGPResourceDesc` | — | buffer/texture/renderbuffer 创建 |
| `resource_respecify` | `MGPResourceDesc` | — | `BufferBackendOps::Respecify` 泛化 |
| `resource_destroy` | handle | — | `OnDestroy` + 两个 `WeakPtr` GC 扫描 |
| `map_persistent` / `unmap_persistent` | handle | R, O | `AcquirePersistentMap`（改造期不碰） |
| `fence_create` / `_status` / `_wait` / `_destroy` | handle (+timeout) | — / — / R / — | `FenceSync`…`GetSyncStatus`（两值契约保留） |
| `query_create` / `_begin` / `_end` / `_available` / `_result` / `_destroy` | handle + kind | — | `BackendObject.h:230-256` |

### `MGPipeContext` — CSO（15 → **13**：`create`/`delete` × 5 + `bind` × 3）

`create/delete` × `render_state` / `vertex_elements` / `sampler` / `sampler_view` / `shader`，`bind` × `render_state` / `vertex_elements` / `shader`。
**sampler 与 sampler view 的绑定见下一组的 `bind_sampler_states` / `set_sampler_views`，此处不重复计。**
`create_render_state` 带 `B`（**只带 pipeline 子集的 chunk**）；`create_shader_state` 带 `B`（SPIR-V + `ProgramArtifacts` 归档）。

### `MGPipeContext` — `set_*`（17 + 1 临时；**`kCtxState` = 16 `set_*` + 1 临时 = 17**，`set_texture_params` 计入 `kCtxObject`）

`set_dynamic_state`(B) · `set_framebuffer_state` · `set_vertex_buffers` · `set_index_buffer` · `set_indirect_buffers` · `set_sampler_views`(V) · `bind_sampler_states`(V) · `set_texture_params` · `set_shader_images`(V) · `set_shader_buffers`(V,H) · `set_stream_output_targets`(V) · `set_global_constants`(B) · `set_vertex_attrib_defaults` · `set_pixel_pack_state` · `set_patch_state` · `set_draw_program` / `set_dispatch_program`
**临时（P2..P13）**：`set_residual_value_state`(B)，带 `static_assert(sizeof(ResidualValueBlock)==0)` 退役绊线。

### `MGPipeContext` — transfer（标 12，**实列 11**；在 `.def` 里分入 `kCtxObject` 9 与 `kCtxVerb` 13）

`resource_subdata`(B,V) · `buffer_subdata_resident`(B,O) · `resource_flush_range` · `resource_readback`(R) · `resource_copy_region` · `blit` · `clear` · `generate_mipmap` · `read_pixels`(R) · `get_texture_image`(R) · **`resource_subdata_complete`**（拉取终止符，可零 region）

### `MGPipeContext` — 命令（10；与 transfer 的动词合成 `kCtxVerb` 13）

`draw_vbo`(H,V) · `launch_grid` · `memory_barrier` · `begin/end/pause/resume_stream_output` · `flush` · `present` · `set_swap_interval`(O)

### 反向：`MGPipeCallbacks`（10）

`on_gl_error` · `on_gpu_written` · `on_buffer_writeback` · `on_texture_writeback` · `on_texture_pull_request` · `on_mip_levels_generated`（**只带形状**）· `on_surface_changed` · `on_caps_invalidated` · `on_log`（**≤WARN 有损 / ≥ERROR 无损 + 速率限制**）· `on_xfb_scatter_ready`

### 显式删除

`GetIntegeri_v` · `GetInteger64i_v`（**P0 已删表项**）· `GetProgramiv`（**P0 已删表项**）· `ShaderStorageBlockBinding`（折进 `MGPProgramDesc`）· `set_pixel_unpack_state`（不存在）· 压缩格式概念（不存在）· `pipe_transfer`（不存在）· `set_sampler_views` 的 stage 维度（不存在）· `kCapPrimitiveRestart` / `kCapPrimitiveRestartFixedIndex` / `kCapMultiDraw` / `kCapMultiDrawIndirect` / `kCapMultiDrawIndirectCount`（**归属不可表达，D-B7**）

---

## 附 B：环境变量与 CMake 选项

### CMake

| 选项 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 出货形态。开启后 `MG_Remote/**` 进 `SOURCE_FILES`，支持 `spawn`/`unix:`/`pipe:`。**两个**进程全局保持普通全局，GL 热路径无 TLS（§13.6） |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | CI/调试形态，隐含开启上者，额外加角色隔离 shim（只需隔离 `gPipeCtx` 与 `pActiveBackendObject`） |
| `MOBILEGL_PIPE_VERIFY` | OFF | **构建期开关**（不只是运行期）：编译进 `SnapshotFromGLContext()` 与 G4 比对器。**P13 之后仍保留**；三道纯度门只跑此项为 OFF 的构建 |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON（P2..P13） | 保留 registry / `TwinLookupMemo` 实现，给前两波 handle 化一个真正的旧-vs-新臂（B-R16） |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 只服务 CI 的 `flatc-check`；默认构建图里没有 `flatc` |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON（P7+） | DirectVulkan 的 blit/depth-mipmap shader 烘焙成签进树的 SPIR-V，由 `MG_Test` 重跑树内 glslang 逐字节比对守新鲜度。**monolith 也受益** |

> 注：`MG_Pipe/**`、`MG_Impl/Pipe/**`、`MG_Backend/MGPipe/**` **不在任何 option 之后**——它们是 monolith 的架构，永远进构建（§13.8）。

### 运行时（MGPipe 新增）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | 迁移期按阶段推进；P13 后删除 | 子系统位图（0 = 全 pull），**含一位关闭 CSO 内容寻址**（P2 的负面对照）。**注意 stage C 之后 A/B 口径收窄**（§5.7、B-R16） |
| `MOBILEGL_PIPE_VERIFY` | 0 | 逐 draw 逐字段影子比对（~5-10× 慢，**含纹理 dirty 集合的保留模式**，永不出货） |
| `MOBILEGL_PIPE_STATS` | 0 | 字节 / **调用** / roundtrip / 纹理拉取 / 上传形状 / 残余块 / 索引镜像计数器转储 |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | **0**（v2 从 32 改） | 纹理重铸拉取的保留 LRU 预算。默认关闭：`MipmapStorage` 保有完整 CPU 影子，缓存买的是延迟不是正确性（§6.5c） |
| `MOBILEGL_PIPE_INDEX_MIRROR_MB` | 64 | server 侧索引宿主镜像预算（D-B7、§7.10）。超预算退化为逐 draw 传送并计入 `index-bytes-shipped` |

### 运行时（传输与 IPC）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | `monolith` / `inproc` / `spawn` / `unix:<path>` / `pipe:<name>` |
| `MOBILEGL_IPC_SERVER_PATH` | 空 | server 可执行文件路径（**主要发现机制**，`dladdr` 兜底，§11.1） |
| `MOBILEGL_IPC_RING_MB` | 8 | `SEG_CMD` 大小 |
| `MOBILEGL_IPC_STAGE_MB` | 32 | `SEG_STAGE` 初始大小；上限由实测定（§7.1.1、开放问题 9） |
| `MOBILEGL_IPC_PRESENT_CREDIT` | **1** | client 允许领先的 present 数（1-4）；延迟叠加见 §9.1 |
| `MOBILEGL_IPC_SPIN_US` | 50 | 挂起前的自旋窗口（两侧 doorbell 共用，§7.2a） |
| `MOBILEGL_IPC_POLL_ESCALATE` | 64 | 同一 handle 连续无进展轮询多少次后升级为阻塞 round trip（§8.2） |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` | 64 | persistent-map 推送的块粒度（§7.8.1） |
| `MOBILEGL_IPC_ADOPT_TIER` | `auto` | `auto`/`0`(T0)/`1`(T1)/`2`(T2 拒绝)；与 `MOBILEGL_IPC_RESPAWN` 互斥（§11.6） |
| `MOBILEGL_IPC_SHADOW_SHM` | 1（Phase 2 起） | shadow-in-shm 零拷贝（§7.4） |
| `MOBILEGL_IPC_INLINE_PAYLOADS` | 0 | 负面对照：一律内联，不用 `SEG_STAGE` |
| `MOBILEGL_IPC_SERVER_AFFINITY` | `auto` | `mgl-srv-apply` 的核绑定；`auto` 用 `ShaderCompilePool` 的大核探测（§10） |
| `MOBILEGL_IPC_STRICT_ERRORS` | 0 | 诊断开关：让所有 backend 错误同步 ack |
| `MOBILEGL_IPC_AUDIT` | 0 | 记录级审计日志 |
| `MOBILEGL_IPC_TRACE` | 0 | 逐记录 trace（仅调试构建） |
| `MOBILEGL_IPC_ATTACH` | 空 | 附着到已运行的 server（调试） |
| `MOBILEGL_IPC_RESPAWN` | 0 | server 死亡后重启 + 全量重推（§11.6） |
| `MOBILEGL_IPC_IDLE_EXIT_S` | 30 | server 的最后保险看门狗（EOF 应当即时退出） |

**显式不设立**：`MOBILEGL_IPC_PROGRAM`（没有 relink 档——链接真 `ProgramObject` 就链接 glslang，§3.5.5）· `MOBILEGL_IPC_VALIDATE_SERVER`（server 没有 `MG_Impl` 校验器——替代手段是保留的 verify 构建 + P13 的 MGPipe recorder 金标，见开放问题 11）。

**保留的既有负面对照开关**：`MOBILEGL_ESPRYT_DISABLE_UBO_RING` · `_UNPACK_RING` · `_UPLOAD_RING` · `_INVALIDATE_FLUSH` · `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` · `MOBILEGL_COHERENT_AS_FLUSH`（**在拆分模式下照常生效**，§7.8.1，这样两个 `coherent_as_flush: true` 的 Create fixture 在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义）
