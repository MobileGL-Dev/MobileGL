# MobileGL 方案 B 实施计划：gallium 式显式接口 + backend 自有状态机（MGPipe）

> 状态：设计定稿 v2（2026-09-05，经三轮对抗性评审修订）。基线 `dev@81b17c0b`；实施分支 `feat/disaggregated`（worktree `../MobileGL-disagg`）。
> 本文是**方案 B** 的实施计划。方案 A（server 内跑 `MG_State::GLState::GLContext` replica）见同目录 `PLAN.md`，其评审记录见 `REVIEW.md`。
> 本文继承方案 A 的 §6-§13（传输、数据面、同步、present、线程、平台、构建），**只替换它的状态模型**（§5 与 §12 的 replica 特化部分）。凡标注"继承 PLAN.md §X"的内容，以 `PLAN.md` 为准，本文不复述。
> 全部 `file:line` 引用针对**工作树** `dev@81b17c0b`。工作树有两处未提交的 `fprintf` 插桩，使 `DirectGLES.cpp` 在 ~660 行之后偏移 +11、`Managers.cpp` 在 872 行之后偏移 +3；`MG_State/`、`MG_Impl/`、`MG_Backend/DirectVulkan/` 的行号与 HEAD 一致。
> **v2 修订说明**：v1 里一批继承自调研报告的 `SamplerObject.h` 行号（`:455-492`、`:532-537`、`:551`）指向文件末尾之后——该文件共 160 行。实际位置：`BorderColorForm` 在 `:60-70`、`SamplerParameters` 在 `:72-96`、`GetLifetimeId()` 在 `:141`、`BumpVersion()` 在 `:151`、`m_version` 在 `:155`。**P0 增加一条 CI lint：本目录下所有 `.md` 里的 `file:line` 必须在基线提交上解析到存在的行**（`git show <base>:<path> | wc -l` 比较），防止同类转抄错误再次进入实施规格。
> 阅读顺序：本文件（§0-§3）→ `part2.md`（§4-§6）→ `part3.md`（§7-§10）→ `part4.md`（§11-§14 + 附）。

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
| `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`） | 渲染状态声明，**逐字节** | `create/bind_render_state` + `set_dynamic_state`（见 0.5 D-B1） |
| `UnpackStagingBlock`（`Managers.cpp:4340-4390`，`{src, rowBytes, rows, slices, srcRowStride, srcSliceStride, offset}`） | Espryt 纹理上传的**带步长的源描述符**，已经存在 | `MGPSubData` 的 region 形状 |
| `BufferBackendOps`（`BufferObject.h:76-120`，7 个 hook） | 已经是接口，且注释自称 "the `pipe_context` buffer-op analogue"（`:68`） | `resource_*` 全族 |

把这些结构的**输入集合**推过去，接口就按构造完整。gallium 是**目的地**（同名同形的词汇让形状可读、可迁移），不是**推导前提**。凡 gallium 的词汇与本仓库的证据冲突的地方，本文按证据走，并在 §4.6 逐条记名列出偏离与理由。

### 0.3 四条结构性推论（决定了后面每一节）

**推论 1 — 推送必须发生在 verb 时刻，不是 GL setter 时刻。** Blaze3D 每个 batch 都用 `glEnable/glDisable(GL_BLEND)` 包住，代码自己把它标成最热的路径（`DirectGLES.cpp:2029-2032`：`mc_state_toggle` 干的最热的事）。天真的 per-setter 推送会把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，**严格慢于今天**。正确形态是 gallium 的 `st_validate_state`。
**v2 修订**：v1 把这条写成"只有资源 mutation 在 GL 调用时刻推送——这恰恰是 `BufferBackendOps` 今天的做法"。**这句话对 buffer 成立，对纹理不成立。** 实测：`glTexSubImage*` **根本不调 backend 表**——`MG_Impl/GLImpl/Texture/GL_Texture.cpp` 里只有 3 处 `MarkStorageDirtyRegion`，全部纹理上传由 Espryt 在 sync 时刻按**累积**区域做（`Managers.cpp:4274-4390`），那里才跑 `MipmapStorage` 的 96-rect 级联合并与 `summedArea*4 >= unionArea*3` 回退，并在 unpack ring 可用时**刻意把 rect 列表塌成一个 union box**（`:4386-4390`：`if (BufferImpl::UnpackRingAvailable()) dirtyRectCount = 0;`，注释记录 ~100 个精灵 rect 变成 ~100 个 Mali 作业，实测 **+6 ms/frame**）。若每次 `glTexSubImage` 发一条 `resource_subdata`，就精确复现了那个 ~100 作业的形状。**规则的正确措辞见 §5.1.1。**

**推论 2 — handle 就是身份，而且必须是稠密 slot。** 每个前端对象已经有一个永不复用的 `GetLifetimeId()`（`BufferObject.h:202-208`、`VertexArrayObject.h:110-120`、`FramebufferObject.h:151-158`、`ProgramObject.h:1620`、`TextureObject.h:83`、`SamplerObject.h:141`），它们存在的唯一理由是 GL name 会被 `IndexGenerator::Generate` 从 free list 尾部 LIFO 复用（`MG_Util/Miscellany/IndexGenerator.h:30-42`）、堆地址会被分配器复用。但**单调的 64 位 id 不能索引数组**——如果 wire handle 直接用 lifetimeId，server 侧仍然是一张哈希表，那就只是把指针键换成整数键，并没有删掉查表层。所以 wire handle 是 `{slot: Uint32, gen: Uint32}`，**slot 由 client 按 kind 稠密分配**，`gen` 在 slot 复用时 ++。lifetimeId 留在 client 侧作为 tracker 自己的身份，不过线。这一条才真正把 6 个 `StateBackendObjectRegistry` 哈希表和 13 个 Magma 身份键缓存变成**数组**。

**推论 3 — server 拥有 client 看不见、也永远不该被问的 generation。** 今天有 12 个纯 backend 侧的单调计数器，它们表达的是"**我自己**重新铸造了驱动对象"，与任何前端版本无关：Espryt 的 `g_bufferMutationEpoch`（`Managers.h:397-441`）、`g_bufferBackendIdGeneration`（`:551`）、`g_attachmentBackendIdGeneration`（`:1298`）、`g_backendContextGeneration`；Magma 的 `m_textureImageEpoch`、`m_resourceEraseEpoch`、`m_renderbufferImageEpoch`、`m_sliceEpochCounter`、`m_cacheStructureEpoch`、`m_evictionEpoch`、`m_recordingGeneration`、`m_frameSerial`。本文把它们统称 `MGGen`，**它们永不上线**。"server 拥有自己的状态机"在工程上的确切含义就是这一条：client 绝不是"我的 server 侧状态是否新鲜"的唯一权威。

**推论 4（v2 新增）— dirty 位对值类组可以**轮询**，对对象类组必须**标记**。**
v1 同时主张两件互斥的事：§5.2 说"dirty 位全部来自已有计数器，`MG_State` 零新增记账"，§5.1/§10.2 说稳态是"一次 64 位 dirty word 测试"。对**值类**组（渲染状态、pack、patch、attrib 默认值）两者兼容——一个 `Uint16` 比较就是全部。对**对象类**组不兼容：`NEW_SAMPLER_VIEWS` 在 §5.2 里映射到 `GetContentVersion`/`GetShapeVersion`/`GetTextureParamsVersion`（**逐纹理**）加 `GetTextureBindGeneration()`/`GetSamplingResolutionGeneration()`，没有任何聚合能回答"有没有哪张已绑定纹理的内容动了"。这正是 Magma 不得不用**有损**的 `sampledContentSum`/`sampledParamsSum`（`VulkanRenderer.h:975-1000`）的原因。轮询版本 = 每次 validate 走查 touched 单元，那不是 O(1)，而且是**新增的 client 侧工作**（backend 的 `ResolvedTextureBindingMemo` 今天恰好跳过它）。

**决定**：
- **值类组**：沿用既有计数器，O(1) 比较，`MG_State` 零新增。
- **对象类组**：在 `MG_State` 里**新增 5 个聚合世代计数器**，在既有的 choke point 上 bump，让 tracker 的快门是 O(1)：
  - `TextureState::m_anyTextureContentGeneration`（`ITextureObject::MarkStorageDirtyRegion` / `BumpContentVersion` 里 ++）
  - `TextureState::m_anyTextureParamsGeneration`（`BumpTextureParamsVersion` 里 ++）
  - `BufferState::m_anyBufferChangeGeneration`（`BufferObject::BumpChangeSerial` 里 ++）
  - `VertexArrayState::m_anyVaoAttributeGeneration`（属性/绑定点 setter 里 ++）
  - `FramebufferState::m_anyAttachmentGeneration`（attachment setter 里 ++）
  合计约 **20 行**，全部落在既有的 bump 点上，**不是**枚举 181 个 GL 入口。快门为真时 tracker 才做 touched 前缀走查并重算集合 hash。
- **完整性绊线**：把 `PLAN.md` 的 `gen_impl_mutation_surface.py` **改造**（而不是删除）成 `gen_pipe_dirty_surface.py`：它枚举 `MG_Impl/GLImpl/**` 里每一个会改变某组的 mutator，映射到必须 bump 的聚合世代，CI 上重生成 + `git diff --exit-code`，**未映射的 mutator 直接失败**。这是 B-R6 的第四层，也是对"reconciler 完整性只有测试绊线"这条历史结论的第二个答案。
- §5.2 的措辞随之改为"**值类零新增记账；对象类新增 5 个聚合世代，换掉 tracker 的逐对象走查**"。§10.2 的稳态成本行同步改写（见 §10.2）。

### 0.4 与方案 A 的结论性对比（详表见 §3）

**方案 B 在架构、内存、长期价值上赢；方案 A 在"多快能拿到第一帧"上赢，而且赢得毫无悬念。**

方案 B 赢的四点，全部可核对：

1. **内存（v2 修订过的算术）。** `PLAN.md` 自己的 R14（第 1222 行）给 replica 预算 "合计可达 ~450MiB 新增"：每个 <16MiB store 一份重复 `PipeResource`、每个纹理 level 一份重复 `MipmapStorage`、一整份 `GLContext` 对象图，叠在两侧都要付的传输段与 ring 之上。
   方案 B 的账（v1 的 "+50-60MiB" 漏算了它自己引入的两项，此处补全）：

   | 项 | 字节 | 说明 |
   |---|---|---|
   | 传输段 | **48.25 MiB** | `SEG_CMD` 8 + `SEG_STAGE` 32 + `SEG_REPLY` 8 + `SEG_EVENT` 0.25 |
   | `SEG_STAGE` 额外余量 | **+0～32 MiB** | 四类新字节（§8.2）实测后定；上限由 P0 计数器给 |
   | server 侧**索引宿主镜像**（**仅 split，仅 `kCapNeedsHostIndexBytes`**） | **0～64 MiB（默认上限）** | D-B7；只镜像曾被绑为 ELEMENT_ARRAY 的 buffer，由 subdata 流增量维护，零额外线上流量 |
   | 纹素保留 LRU | **默认 0** | `MOBILEGL_PIPE_TEXEL_RETAIN_MB` **默认改为 0**；只有实测拉取率非平凡才开（§7.5d） |
   | POD slot 记录 + CSO 缓存 | ~1-2 MiB | |
   | **典型（不开索引镜像）** | **≈ +50-60 MiB** | |
   | **最坏（镜像满 + stage 余量满）** | **≈ +145 MiB** | 仍是 replica 的 1/3 |

   **诚实注记**：索引宿主镜像是方案 B 唯一的"数据副本"，它是把 restart 重写与 multi-draw 分档**留在 server**（D-B7）所付的价钱。它只覆盖索引缓冲、有显式预算与计数器、且超预算时有回退路径（逐 draw 通过 `MGHostSpan` 发送，代价记账）。这与 replica 复制**全部** buffer 与**全部**纹素在量级上不是一回事。
2. **拷贝。** `PLAN.md` §6.4 数出 `glBufferSubData` → store 在 split P1-4 是 **4 次**、P4.5 是 **3 次**，其中第 (3) 次是 `SEG_STAGE`→**replica** shadow。没有 replica 就没有这次拷贝：方案 B 是 **3 / 2**。而 `PLAN.md` 自己把 2 次称作"方案 B（激进，需额外设计）"（第 549 行），要求给 replica 的 `PipeResource` 加第三种 `AdoptedClientShadow` 模式并处理 server 侧写的 copy-on-write 升级，且把它推迟到 P6 由 Tracy 数据决定（开放问题 §17-5）。方案 B **按结构就在那个目标上**，并顺带关掉它自己的开放问题。
3. **漂移面。** replica 是一份必须与 20k 行 `MG_State` **语义**长期锁步的手写状态模型，而它的守卫（生成的 `is_same_v`/`sizeof`/`offsetof` + `reflectionDigest`）只能看见**签名**漂移。`MipmapStorage` 的 96-rect 级联合并与 union-box 回退（`MipmapStorage.cpp:300-305`）、`VecRange1D` 的 7% gap 比、`PipeResource` 的模式切换、`BufferObject` 的 persistent-map 状态机——任何一处行为不一致都能编译通过、在多数内容上渲染正确，而这恰好是本项目已经实测出 **+6ms/frame** 悬崖的那块地方。方案 B 只有一份状态模型，这一类失效**不可表达**。
4. **整块子系统消失而不是被移植。** `PLAN.md` §2(g) 的"第七个面"（MG_Impl 在 table 调用旁做的 `MG_State` mutation：`AccountTransformFeedbackPrimitives`、`EnsureGeneratedMipmapStorageAllocated`）连同 `MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::` helper 族和风险 R1，在方案 B 里**不存在**——没有 replica 就不需要 replay。（**注意**：那个生成器本身**不删**，改造成 §0.3 推论 4 的 dirty-surface 生成器；replay 的义务消失，标记的义务出现，两者不是同一件事，v1 把它们混为一谈。）同样消失的还有：§5.6a 的纹理 ack 协议与 R6；§5.7 的 "server 自建 composite" 分支；§6.9 的 relink 档与整个阶段 P5（6 天）；§12.2 里跨越 1494 个 `MG_Impl` 站点的 `pGLContext` shim（`inproc` 需要隔离的进程全局从 4 个降到 2 个）。

**`RecProgramLinkOp` 不是"不理想"，是不可能。** `ProgramObject.h:11` include `ShaderObject.h`，后者 `:12` include `ShaderCompileTask.h`、`:146` 返回 `SharedPtr<glslang::TShader>`；`ProgramObject.h:14` 又拉进 `SpvcSession.h`（后者 include `spirv_reflect.h`）。**任何链接真 `ProgramObject` 的 server 就链接了整条编译链。** 所以方案 A 的两档 program 方案在方案 B 里塌成一档。
**v2 修订（重要）**：v1 由此推出 `nm -D libMobileGLServer.so | grep glslang` 为空是"整个论点的强制执行点"，但**没有注意到它自己的反射 payload 也住在同一个头文件里**：`TypeFacts`（`:44`）、`ResourceReflection`（`:76`）、`XfbVarying`（`:1146`）、`LinkArtifacts`（`:1210`）、`SpirvArtifacts`（`:1409`）全部声明在 `ProgramObject.h` 内。server 要**反序列化进**这些类型就必须 include 那个被门禁止的头。所以**新增一个前置阶段 P0.5**（§11）：把这五个类型抽到独立的 `MG_State/GLState/ProgramState/ProgramArtifacts.h`，它不 include `ShaderObject.h`、不 include `SpvcSession.h`，更新 7 个 includer，并加一条 CI 断言"`ProgramArtifacts.h` 的传递 include 闭包里没有 glslang / SPIRV-Cross / spirv_reflect 头"。**没有这一步，P7 的验收判据不可达。**

方案 A 赢的一点，也毫无悬念：

- **到首个跨进程帧的时间。** `PLAN.md` 的阶段天数逐项相加恰好是 **77 天**，其中 **P1b 出口（≈第 15 天）就是首个跨进程帧**，因为它一行 backend 代码都不用改。方案 B 最早的 `inproc` IPC 帧在第 ~95 天，最早的**跨进程**帧在第 ~100 天，且那一帧是**缩减路径**（emulation 在 P8 之前于 split 模式下直接 Fatal），全功能要等 P8（第 ~139 天）。总估时 **261-329 人天**（含 IPC；不含 CTS 周转，见 §11.5）。
  **v2 修订**：v1 报的 "200-260 天 / 第 64 天 inproc 帧" 与它自己的 §6.4/§6.5 逐子系统表**互相矛盾**（例如 P3a 给 12 天，而它的三行子系统合计 22-29 天，等于"再基线检查点"按构造必然触发）。§11.5 已按逐行求和重建，并公布算术。

**如果目标是"这个季度拿到一个能跑的拆分"，选方案 A。如果目标是用户实际提出的那个——"backend server 拥有自己的状态机并暴露统一的、gallium 式的接口把前后端解耦"——方案 A 在任何价格下都不交付它**：它用复制前端来回答耦合，而不是用定义契约来回答耦合，而且那份复制的维护成本是**永久**的；方案 B 的成本是**一次性**的，且在第一个字节过 socket 之前就已经把 monolith 变好（净删除 ~370 行 per-draw 失效发现机制、让复用地址 ABA 一类失效不可表达、删掉一个排序 hazard、删掉一处分层倒置、修掉两个潜伏 bug、暴露一个死能力）。

### 0.5 八个必须先记下来的具体决定（这些是评审里争议最大的点）

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
v1 把 §5.3 的编号顺序（1 framebuffer → 2 program → 3 images → 4 render state → 5 vertex）写成契约，并说这是退役 `ImageUnitFormatsStillMatch`（`Managers.cpp:6545-6573`，注释明说"不可表达为单调版本"）与 fragColor 重推导 workaround（`DirectGLES.cpp:2712-2732`）的机制。**但它自己把 images 排在 program 之后**——所以退役这两条的其实是 **D-B2 的惰性特化**，不是调用顺序。
**规范条款改为**：
> 一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间**没有**顺序要求。

§5.3 的编号列表降级为**推荐实现顺序**（便于 tracker 的代码组织与 dirty 位遍历），不再是正确性契约。收益不变：`DirectGLES.cpp:2712-2732` 的 workaround 与 `g_broadcastMemo*` 照删，因为特化发生在 verb 处、那时 FBO 状态一定已在。

**D-B4：AcquirePersistentMap 在整个改造期一动不动。** 它是**永久的地址空间捐赠**而不是 gallium 的 scoped `transfer_map`：返回一个 host-visible coherent 指针，成为该 buffer 的唯一真相源（`BufferObject.h:102-118`），由 `PipeResource::AdoptPersistentMap`（`PipeResource.h:115`）采纳、经 `MappedData()` 交给应用、≥16MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到（`:226-228`）。实测代价是 MC 26.3 的 p99 163→21ms、40→115fps、省 ~400MB。**它今天就已经是一个"返回指针的显式调用"，因此原样穿过 monolith 改造；只有 IPC 那一步才会打破它。** 改造期不碰，IPC 期按 `PLAN.md` §6.8 的三档 POST 探针决定，spike B 第一周给答案。绝不允许一个平台未知数挡住 260 天的接口工作。
**v2 补注**：`map_persistent` 的 round trip 是**每次存储定义（respecify）一次**，不是"每 store 生命周期一次"——`TryAdoptLargeStorage` 在存储定义时触发，一个反复扩容的 arena 会付 N 次。`StorageBufferRegrowScenario` 必须发布 `map-persistent-roundtrips` 计数。

**D-B5（v2 修订）：monolith 字节一致门按构造死亡，这是本方案的成本；但语义门必须活过 P13。**
`PLAN.md` §12 第 4 层（`nm --defined-only` + 剥调试信息后 `.text` size 相等）在方案 B 里不成立——**不存在任何配置能让旧字节回来**。替换是**五部分门**（§10.3），其中第 ② 部分（每 draw 逐字段的 pushed-vs-snapshot 影子比对）在语义上**严格强于**任何符号 diff。
**但 v1 的 P13 删掉 `SnapshotFromGLContext()`，而那正是 verify 的参照物来源**——删完之后 verify 无物可比，设计从此没有语义绊线。**修正**：
- `SnapshotFromGLContext()` 与它需要的 `MG_State` include **在 P13 之后继续存在，但整体包在 `#if MOBILEGL_PIPE_VERIFY` 里**；verify 构建**永不出货**。
- 纯度门（`grep -c 'pGLContext' MG_Backend/` == 0、include 白名单、`nm --undefined-only`）**只跑非 verify 构建**，这一点写进门的定义。
- 另外在 P13 交付 §10.4-9 已经勾勒的**录制-金标**模式：把 `MG_Test` 的 mock backend 变成 MGPipe recorder，在一组 fixture 上录下每 draw 的已推送状态，后续构建对比录像。它不依赖 `MG_State`，所以是长期可用的语义门，也是开放问题 11 的答案。

**D-B6：方案 B 引入一个方案 A 没有的新停顿类：server 发起的纹理重铸拉取。** server 不保留纹素字节，所以 `RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）都必须回头向 client 要数据。**三条缓解同时上，不是三选一**，加一个专门的门、一个逐 trace 用例发布的计数器，**以及一个显式的"答不出来"终止符**（§7.5）——因为存在 client **没有**字节可发的 level（纯渲染产生、`CanMirrorCopyImageShadow` 拒绝的 copy 目标、GPU 生成的 mip），没有终止符 apply 线程会永久 park。上一轮 thin-server 设计正是因为把这条一笔带过而被判死。

**D-B7（v2 新增）：restart 重写与 multi-draw 分档**留在 server**，split 下由一份**索引宿主镜像**喂养。**
v1 的 §5.8 把这两条按 `!kCapPrimitiveRestart` / `!kCapMultiDraw` 下放到 client，而 §4.5.7 的表又写"monolith：`ptr` 指向 shadow（server 做）"——**两处互相矛盾**。更根本的是这个划分不可表达：
- `ResolveTierForBatch`（`MultiDraw.cpp:282-320`）**逐 batch**在五档里选，输入包含 `programReadsDrawID`——**转译出的 ESSL 的性质，只存在于 server**——以及 `perSubDrawBaseVertex`、`hasIndexBuffer`、`arbitraryRestart`，并在 `kMaxFlattenedIndices`（`:72`，1<<24）与 `kMaxComputeFlattenedIndices`（`:82`）上做容量判定。自动阶梯是 Ext → BaseVertex → MultiIndirect → Indirect → DrawElements（`:241-243`），CPU 展平的 `DrawElements` 档是**回退**，client 无法预判。
- restart 重写**两个 backend 都做**（`DirectGLES.cpp:4283/4377`、`VulkanRenderer.cpp:3990/4089/4161`），所以 `kCapPrimitiveRestart` 恒为 false，"cap 门控"没有门可控。

**决定**：`kCapPrimitiveRestart` / `kCapPrimitiveRestartFixedIndex` / `kCapMultiDraw` / `kCapMultiDrawIndirect` / `kCapMultiDrawIndirectCount` 作为**归属开关**删除。规则改为一句话：**multi-draw 分档与 restart 重写永远由 server 拥有；client 在 caps 说 server 可能需要时提供索引字节。** 提供方式不是逐 draw 拷贝，而是：

> **`kCapNeedsHostIndexBytes` 开启时，server 为"曾被绑为 `GL_ELEMENT_ARRAY_BUFFER` 的 buffer"维护一份宿主镜像**，由它本来就要收的 `resource_subdata` / `resource_respecify` 流**增量**维护，**零额外线上流量、零 round trip**。预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），逐帧计数；超预算时该 buffer 退化为逐 draw 通过 `MGHostSpan` 传送并计入 `index-bytes-shipped` 计数器。

好处：monolith 行为**零变化**（不搬代码、不改诊断落在哪个线程 → 开放问题 12 关闭）、split 下 restart/multidraw 零 round trip、`kMaxRestartRewriteBytes = 1<<26`（64 MiB，`DirectGLES.cpp:4218`）这种单条记录不再需要塞进 32 MiB 的 `SEG_STAGE`。代价是那份镜像的内存，已计入 §0.4-1。

**D-B8（v2 新增）：per-draw 的**具名 uniform block 字节**必须有自己的载体。**
v1 §7.2 断言 20 处 `SyncPersistentMappedRange` "作为反向调用彻底消失，因为紧邻它们的 CPU 读全部搬到了 client"。**有一处反例**：`UniformManager::ResolveUniformBufferPayload` 在 `UniformManager.cpp:2022` 调 `SyncPersistentMappedRange()`，随后在 `:2052` 读 `bufferObject->MappedData() + rangeStart`（不足时在 `:2053-2057` 零填充），把具名 UBO 块打进 **Magma 自己的 UBO ring**——消费者在 server，搬不走。而 §4.4.3 的 `set_shader_buffers` 只有 `V` 标志，没有 `kHasBlob`/`MGHostSpan`；`set_global_constants`（D6）只覆盖**默认** uniform block。**结果是每个带具名 UBO 的 Iris/MC draw 都有一条没被承载的数据依赖。**
**决定**：`set_shader_buffers(cls == Uniform, ...)` 的每个 range 增加可选的 `MGHostSpan payload`（`kHostSpan` 标志），由 `kCapNeedsHostUboBytes` 门控（Espryt 不需要——它把具名 UBO 直接绑给驱动）。字节量进 `SEG_STAGE` 的尺寸表（§8.2）与 P0 计数器（`stage-ubo-named`）。**在 P0 计数器给出逐帧字节量之前，不冻结这个 payload 的形状。** 备选（不在本计划内、需独立 `dev` PR + Iris 性能门）：让 Magma 直接描述符绑定常驻 `VkBuffer` 的 range，不再 ring-pack。

### 0.6 推荐

**推荐执行方案 B，但按下面这个对冲路径起步，在第 42 天做一次真正的 GO/NO-GO：**

先原样跑 `PLAN.md` 的 P0（卫生、传输骨架、两个 spike，尤其是 **`TracyPlot` 逐帧字节计数器**——树里今天完全没有 per-frame 字节或调用度量，`MG_Util/Metrics` 只是格式算术，Tracy 只有 zone 无 plot），然后跑本文的 **P0.5 + P1 + P2**。

- **第 ~25 天（P1 出口）— 机制里程碑，零产品风险**：`MOBILEGL_PIPE_VERIFY` 影子比对 harness 在全部 40 个 trace 用例与 367 个集成测试上逐 draw 逐字段证明"推送等价于拉取"。这一天**不**是 GO/NO-GO——它只证明机制，不给性能数字。
- **第 ~42 天（P2 出口）— GO/NO-GO**。

**v2 修订：GO/NO-GO 的口径必须包含一片 Track H，否则它测的不是它要决定的事。**
v1 把 GO/NO-GO 放在"只迁了渲染状态"的时点，而渲染状态恰好是推送**收益最小、v1 的 CSO 设计开销最大**的那个面：Espryt 已经有逐字节镜像 + 单个 `Uint16` 早退（`DirectGLES.cpp:2016-2018`），Magma 已经按 `GetPipelineStateVersion()` 缓存哈希（`:4982-4993`）并双门控动态尾巴（`:5888-5893`）。绿灯不能证明它要担保的事（Track H 的 handle 化在 260 天里划得来），红灯更可能是在指控 CSO 设计而不是推送模型。
**因此 P2 的范围扩大为**：渲染状态 CSO（双后端）**＋ 最便宜的两片 Track H**——Espryt 的 0b handle 基建（`SlotAllocator` + 6 个 registry 变 slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`）与 Magma 的子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，§6.5 自评"低（纯结构性收益）"）。第 42 天你手上会有：

- 逐 draw 逐字段的语义等价证明（P1 交付）；
- 两个 backend 上都已推送的渲染状态，`SyncRenderState` 的 693 行函数体一行未动；
- **Track H 的实测单位成本**（两片，两个 backend 各一）；
- 两台设备上 reboot-clean 配对的**逐线程 CPU 时间**增量，含一个专门的 Blaze3D blend-toggle 微基准；
- 一个**负面对照**：关掉 CSO 内容寻址（`MOBILEGL_PIPE_PUSH` 的一个子位）重跑，把"推送更慢"与"CSO 设计更慢"分开。

**退回成本（诚实版）**：P0（9-11 天）是 `PLAN.md` 共有的；P0.5 的头文件抽取对方案 A 也有用（它同样想序列化反射）；真正只为方案 B 花的是 P1 + P2 ≈ **27-37 天**。v1 说"只损失 16 天"是按一个与它自己的子系统表矛盾的排期算的。**若第 42 天的 CPU 数字为负、或 Track H 的单位成本比估计高 50% 以上，退回方案 A 损失 27-37 天。**

---

## 1. 目标与非目标

### 1.1 目标

1. **定义并落地一份显式的前后端接口 MGPipe**：句柄寻址、只推不拉、gallium 形状，client 与 server 都只依赖它。
2. **backend 拥有自己的状态机**：`MG_Backend` 在 MGPipe 构建（非 verify）下**不含** `MG_State::pGLContext`，`MG_State` include 收缩到一张共享**值**头文件白名单，server 产物的 `nm --undefined-only` 里没有 `MG_State::GLState::` 符号、没有 glslang 符号。
3. **前后端跑在两个进程**，通过 IPC 通信；client 把状态 reconcile 成推送调用、序列化（FlatBuffers）后发送；server 更新自身状态并调 backend API。
4. **稳态帧零 round trip**（回读 / 阻塞式 query / sync wait / present credit / 分配类错误 ack / 纹理拉取之外，且后者的次数必须**实测发布**而非声称为零）。
5. 两半尽可能互相异步；client 至多领先 server 1 个 present（默认，延迟叠加分析继承 `PLAN.md` §9.1）。
6. 平台特定代码最小化并集中在 `MG_Remote/Transport/` 与 `MG_Remote/Client/Surface*`（继承 `PLAN.md` §11）。
7. **单进程 Monolith 保持功能与性能不回归**，由五部分门机械验证（§10.3）。注意这**不是**方案 A 的"字节级不变"——见 D-B5。
8. 所有验收门用**现有测试**：`ctest -L unit`（428 个 `TEST(`）/ `-L integration-gpu`（367 个 `TEST_F`，75 个场景文件）/ `tools/trace_replay`（40 个用例，默认 SSIM ≥ 0.99）/ `tools/cts` / `tools/device_bench`。
9. **接口本身是可独立交付的产物**：即使 IPC 永不上线，`inproc`（同进程第二个 apply 线程）就是 monolith 的渲染线程交付物，且是本项目手上最大的单一 CPU 杠杆。

### 1.2 非目标

- **share-group sessioning 重构。** 与 `PLAN.md` 一致：`eglCreateContext` 的 `shareCtx` 只在 `EGLState/Core.cpp:632` 被校验、`:640` 被存进 `EGLContextState::SharedContext`，**全代码库无人读取**；`pGLContext` 是唯一进程全局（`GLState/Core.cpp:20, 1487`）。v1 = 一条 flow、一个扁平 handle 空间。但**接口头文件从第一天就把 `MGPipeScreen` 与 `MGPipeContext` 分开**（§4.3）。`c7c9e346`/`29d721ef` 那套整体丢弃（理由见 `PLAN.md` §14 DROP）。
- **BFA strict-C-ABI backend 插件 / UtilRuntime C-ABI 化**（同 `PLAN.md`）。
- **macOS 拆分**（同 `PLAN.md`：`CAMetalLayer` 无公开跨进程表示 → monolith only）。
- **Windows 窗口拆分**（同 `PLAN.md`：headless/pbuffer only）。
- **把 emulation 层重写到 client。** 只有**三**个"读前端字节的纯 CPU 变换"下放到 client（v1 说五个，D-B7 收回了两个）：client 顶点数组的范围计算、最大索引扫描、`*IndirectCount` 的计数解析。viewport-array 回放、**multi-draw 分档**、**primitive-restart 重写**、fp64 顶点转换、image-bindable 存储加宽等**全部留在 server 作为 lowering pass**，接口只负责把它们的输入表达清楚（含 D-B7 的索引宿主镜像）。
- **在 P13 之前删除 pull 路径。** 旧路径一直编译在里面，任何提交都能用一个 env 位 A/B（**但要注意 §6.7 说明的 A/B 口径在 stage C 之后会收窄**）。

---

## 2. 现状：边界为什么不清楚

### 2.1 今天的边界有七个面（沿用 `PLAN.md` §2 的分面，数字按工作树复核）

**(a) `GLFunctionsTable`** — `MG_Backend/BackendObject.h:117-278`。**实测 67 个函数指针 + 1 个 `Bool` 能力位**（`PrefersCpuXfbPrimitiveAccounting`），`GlobalBackendFunctionsTable`（`:279-285`）再加 `Present` 与 `SetSwapInterval` → **全体 69 个函数指针**。
MG_Impl 侧 **~93** 个 `gBackendFunctionsTable.GL.*` 调用点，覆盖 **70 个不同表项**。**null 项已经表示"未实现，前端回退"**，写进头注释（`:212-215` 的 sync 族、`:265-269` 的 XFB 跨度），且 DirectVulkan 确实留空 8 项而 Espryt 填满。三项是错位的前端查询：`GetIntegeri_v`/`GetInteger64i_v`（`:195-196`，`DirectGLES.cpp:7264-7386` 有 15 个 case 完全不碰 GL）、`GetProgramiv`（`:197`）。

**这 70 个表项里只有约 22 个是 draw/dispatch**（20 个 draw 族 + `DispatchCompute`/`DispatchComputeIndirect`）。**其余 ~48 个是 clear（9）、blit（2）、copy（3）、`GenerateMipmap`、回读（4）、barrier（2）、XFB 跨度（6）、query/sync（~19）、`BindImageTexture`、`PatchParameteri`、`ShaderStorageBlockBinding` 等**，而其中很多**自己就读 `pGLContext`**（例：`UpdateTextureBindingAtTarget` 在 `DirectGLES.cpp:6051-6052` 读 `GetActiveTextureUnit()` + `GetTextureUnitObject()`，被 `CopyTexImage2D`/`CopyTexSubImage2D` 路径命中；`PackStateFromContext` 在 `:6129` 读 `GetPixelStoreParameters(false)`；`Clear` 在 `:4106` 读 `GetRenderStateParameters().ClearColor`、`:4165` 读 draw FBO；`BlitFramebuffer` 在 `:5988-5989` 读两个 FBO slot）。代码自己说明了这一点：`DirectGLES.cpp:1501-1502` 写着无参 `CaptureDrawTextureSyncKeys` 包装存在是"for every non-draw call site (Clear, readbacks)"。
**这是 v1 的一个实质性缺口**：它只在 `PrepareForDraw` 与 `SetupDraw` 两处填快照。修正见 §6.2.1 与 §11 P1。

**(b) `BackendObject` 虚函数** — `BackendObject.h:543-568`，MG_Impl 侧 **40** 个 `pActiveBackendObject->`（其中 35 个是 `GetDynamicParameters()`）。`InitCapabilities()` 懒执行在第一次成功的 `eglMakeCurrent` 内部（`BackendObject.cpp:341-347`），且每次 surface 变更重新武装（`:301`）。

**(c) `BufferBackendOps`** — `BufferObject.h:76-120`，**7 个 hook**，注册入口 `:124`。Espryt 注册 7/7（`Managers.cpp:1338-1346`），Magma 注册 6/7（**故意**不注册 `ResidentSubData`，`VkBufferManager.cpp:104-111`）。**这个面已经是 MGPipe 的三分之一，且注释自称 `pipe_context` 类比。**
**注意它只覆盖 buffer。** 纹理**没有**对应的 GL 调用时刻分发面（推论 1 的 v2 修订）。

**(d) 状态拉取** — `MG_State::pGLContext->` 在 `MG_Backend` 里 **293 次出现 / 290 行**（DirectGLES 124；DirectVulkan 169），**外加 58 行非箭头用法**（见 2.4）。此外还有约 1997 个前端对象 getter 调用点、186 个不同 getter（上界统计）。

**(e) backend → frontend 写回** — 逐名 grep 实测 **95 个调用点 / 17 个方法**：`SyncPersistentMappedRange` 20、`MarkStorageDirty` 18、`AllocateStorage` 8、`WritebackFromBackend` 8、`SetInternalFormat` 7、`SyncGpuWrites` 6、`MarkGpuWritten` 6、`RecordError` 6、`SetBackendResource` 4、`EnsureGpuResidentStorage` 3、`SetBackendHashMemo` 2、`InvalidateCompileEnv` 2、`SetBackendStateMemo` 1、`SetBackendAuxMemo` 1、`UpdateMipmapSubData` 1、`TruncateMipmapLevels` 1、`SetSamples` 1。

**(f) backend 反向进 MG_Impl** — 恰好 6 处：`DirectGLES.cpp:1917, 2838, 2867, 9675`（`pDefaultFramebufferInfo` 身份比较）、`SwapchainObject.cpp:276`（**写**）、`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`，一处真正的分层倒置）。

**(g) MG_Impl 在 table 调用旁做的 `MG_State` mutation** — `EnsureGeneratedMipmapStorageAllocated`（`GL_Texture.cpp:501-544`，调用点 `:6698, 6708`）与 `AccountTransformFeedbackPrimitives`（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`）。**在方案 B 里这个面的 replay 义务不存在**；但**标记义务**出现（推论 4），由改造后的 dirty-surface 生成器覆盖。

**(h) 工作树污染** — `DirectGLES.cpp:290-303`/`:640-663` 与 `Managers.cpp:875-877` 的未提交 per-draw `fprintf(stderr)`（后者在 `pendingMutex` 临界区内）。**P0 第一件事就是清掉。**

### 2.2 backend 已有的状态机清单（这就是"server 已经是薄服务端"的实证）

**DirectGLES（Espryt）**
- 6 个 twin registry，全部是 `StateBackendObjectRegistry<StateObject*, {BackendPtr, weak_ptr}>`（模板 `Managers.h:270-390`；实例 `:806`(VAO) `:1123`(Texture) `:1216`(FBO) `:1731`(Program) `:1830`(Sampler) `:1858`(Renderbuffer)），键是**前端裸堆地址**，用同址 `weak_ptr` 防 ABA，GC 阈值 `kGCInterval=1024` draw / `kCreationGCInterval=64` 次创建。
- 三条 persistent-mapped bump ring（UBO `Managers.h:591-637`、纹理 unpack PBO `:639-671`、buffer upload `:673-…`），各自 4MiB 起 → 64MiB 上限；buffer pool 预算 `kMaxPoolBytes = 64MiB`、单 buffer 上限 8MiB（`Managers.cpp:564-565`）。
- 每对象 twin：`GLESBufferResource`（`Managers.h:443-497`）、`BackendVertexArrayObject`（`:675-803`）、`BackendTextureObject`（`:944-1119`）、`BackendFramebufferObject`（`:1140-1213`）、`BackendProgramObjectImpl`（`:1473-1725`）、`BackendSamplerObject`（`:1808-1824`）、`BackendRenderbufferObject`（`:1838-1855`）。
- 完整的渲染状态**值镜像** `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`）+ 单个 `Uint16` 早退门（`:2016-2018`）+ 三段 memcmp（`:2038-2047`）。
- 驱动绑定影子、三个共享 scratch FBO 及其驱动侧 attachment 影子、`PackState`。
- **`UnpackStagingBlock`**（`Managers.cpp:4340-4390`）——一个已经存在的**带步长源描述符**，`MGPSubData` 的 region 直接照抄它的形状（§4.5.6）。

**DirectVulkan（Magma）**
- `VulkanRenderer`：`PipelineMemoEntry m_pipelineMemo[8]`、`SetupDrawSnapshot m_setupDrawSnapshots[4]`（40+ 字段）、`VaoDrawMemo m_vaoDrawMemoTable[2048]`、`ResolvedVertexBindings`、`m_convertedVertexStreams`、`DynamicStateShadow g_dynamicStateShadow`、采样集/LOD/BaseVertex 三个 memo、11 个 per-draw scratch vector。
- 5 个 manager（`VkBufferManager`、`VkTextureManager` 3504 行、`VkRenderPassManager`、`VkSamplerManager`、`VkClearManager`）、3 个 factory、`UniformManager`、`FrameContext`、`SwapchainObject`。

**结论：两个 backend 都已经是完整的、贴着各自 API 的状态机。** 上面**没有一样东西需要在方案 B 里删除或重写**——需要改的只是它们**怎么知道**这些事实，以及它们的 memo **用什么做键**。

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

另一个角度：1997 个前端 getter 站点里，**89 个是纯版本/序号读（A 类）**——推送模型里根本不过线；**72 个是数据字节读（E 类）**，全部在 §5.7/§5.8 处理；**38 个是 `GetLifetimeId()` 身份读（D 类）**，全部变成 handle。

### 2.3.1 v2 新增：把"每 draw 成本"用**动态**口径说清楚

v1 的 §10.2 把今天的每 draw 状态获取写成 "Espryt 124 / Magma 169 次 accessor 调用"。**124/169 是静态调用点数（§2.1(d) 的定义），不是动态每 draw 调用数。** 树里每一处都已经被 memo 门控：

| 路径 | 稳态实际做的事 |
|---|---|
| `SyncRenderState`（`DirectGLES.cpp:2003`） | `:2007` 读一个 `Uint16`，`:2016-2018` 相等即 `return`。**三段 memcmp 只在版本移动后跑。** |
| `SyncNeccessaryTextures`（`:1520`） | 6 值键比较 + `PairingsIntact` + 每条目一次 `IsDrawSyncClean` 字比较；单元走查只在未命中时跑 |
| `CurrentUnitBindingsEpoch`（`:1418-1436`） | 三值快门；owner 走查只在 bind generation 移动后跑 |
| `TrySetupDrawFastPath`（`VulkanRenderer.cpp:5994`） | ~10 次 accessor + ~20 次字比较 |
| `GetOrCreatePipeline`（`:4948`） | `:4982-4993` 只在 `GetPipelineStateVersion()` 移动后重算哈希；`:5155-5200` 的 ~40 次 accessor 走查**只在 pipeline memo 未命中时**跑 |
| `ApplyDynamicDrawStateTail`（`:5871`） | `:5888-5893` 一次版本比较，然后一次 bulk fetch 建值键 |

**所以真实稳态大约是每 backend 每 draw 10-25 次 accessor 调用加几十次字比较，不是 124/169。** 推送模型的优势因此比 v1 声称的**窄得多**，而且它在 §10.2 的对照表必须按动态口径重写（已改）。**推论**：
1. P0 的计数器交付物**必须包含动态调用计数器**（每 draw 实际执行的 accessor 次数、每个 memo 门的命中/未命中），不只是字节计数器——否则 P2 仍然是在猜。
2. 第 42 天的 GO/NO-GO 阈值必须是一个**绝对数字**（tracker 每 draw 的 ns，两台设备实测），不能只写"落在 monolith-pull 的噪声内"——当真实基线是 20 次调用时，相对噪声阈值会平凡通过。

### 2.4 pull 模型里 293 之外的 58 行：迁移机制必须显式处理的缺口

| 形态 | 数量 | 例子 | 处理 |
|---|---|---|---|
| `MOBILEGL_ASSERT(MG_State::pGLContext, ...)` 真值判定 | ~34 | `DirectVulkan.cpp` 密集区、`UniformManager.cpp` 9 处 | **直接删除**（`Defines.h:114` 在非 debug 下宏为空，所以这批**在 RelWithDebInfo 里本来就不生成代码**）；替换成 §6.2 的 poison mask |
| `if (MG_State::pGLContext)` 空守卫 | 7 | `Managers.cpp:3608`（守 `BackendTextureObject::StampViewSyncKeys` 的三次赋值）、`:3737, 3808, 4663, 8678`、`BackendObject_DirectVulkan.cpp:388, 788` | 删除守卫，改读 `PipeInputs` 字段（永远有效）。**这批会改变 `.text`**（见 §11 P1 验收修正） |
| `MG_State::pGLContext != nullptr ? A : B` 三元 | 3 | `Managers.cpp:7120, 7128, 7131`（patch 参数，在 transpile 路径内） | 由 `set_patch_state` 覆盖，三元塌成直接读。**改变 `.text`** |
| `MG_State::pGLContext.get()` 裸指针捕获 | 1 | `DirectGLES.cpp:146` | **`sed` 完全抓不到**，必须手改。相邻的 `:142` 还有一个 `decltype(MG_State::pGLContext->GetFramebufferBindingSlot(...))` 类型别名，同属此类 |
| `!= nullptr` 条件 | 14 | `VulkanRenderer.cpp:11150, 12649` 等 | 同空守卫 |
| 注释 | 1 | `VertexInputStateFactory.h:133` | 改写措辞 |

**因此：纯度门 grep 的是 `pGLContext`，不是 `pGLContext->`**，且 P1 的机械替换步骤必须把这 58 行列成显式清单逐条转换。

### 2.5 pull 模型为了弥补"没有接口"而付的代价（v2：区分**真删除**与**搬迁**）

v1 把下表全部记作"~550 行删除"。**其中一部分是搬迁，不是删除**，必须分开记账，否则 §10.4 的 monolith 收益被高估。

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
| `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` / `CurrentUnitBindingsEpoch` / `UnitTextureSyncEntry` / `PairingsIntact` + 8 个支撑全局 | `DirectGLES.cpp:1372-1489` | ~115 | 它存在的理由是 `GetTextureBindGeneration()` **在冗余重绑时也 bump**（`:1414-1420` 注释：26.2 在每次纹理单元切换前后重绑同一个 sampler）。而 §5.2 恰好把这个计数器列为 `NEW_SAMPLER_VIEWS` 的 dirty 输入。**若 tracker 直接信它，每一次冗余 `glBindSampler` 都会重发一次 `set_sampler_views`——一条 `kVarTail` 变长记录，每 draw 几百字节，且 server 侧 `viewSetSerial` 一动就冲掉解析绑定 memo 与 sampler pass memo。** 这正是那 115 行要防的 per-batch 回归。**去抖必须搬到 client**：tracker 对已解析的 view/image/buffer 集合算 hash，hash 未变则**不发**（`MGPFramebufferState::contentHash` 已经演示了这个模式，这里把它推广到其余 `kVarTail` 的 `set_*`，并且在 client 侧当作**发射抑制器**用，不只是 server 的 memo 键） |
| `g_fboTextureSyncList`（`:1580-1601`） | | ~20 | 同上，针对 attachment；由 `MGPFramebufferState::contentHash` 抑制 |
| `ResolvedTextureBindingMemo` 的完备性解析（`IsMipmapCompleteForFilter` / `SamplesAsIncompleteTexture` / `IsUndefinedDefaultTexture`） | `DirectGLES.cpp:3218-3291` + `TextureObject.h:309/315/329` | ~40 | §5.5 把 view 解析放在 client，所以 client 需要自己的 memo 才不会每 draw 重解析 |
| **小计** | | **~175** |

**净账：monolith 侧真删除 ~372 行；另有 ~175 行从 backend 搬到 `MG_Impl/Pipe/Tracker.cpp`。** §10.4 与 §3 的对照表按这个数字改写。

### 2.6 21 个 D 类身份 memo：它们各自守什么，以及为什么 `{slot, gen}` 能等价替换

统一事实：**每一个进入 memo 键的版本计数器要么是回绕的 `Uint16`，要么根本不会被它真正害怕的那个 mutation bump。** `BindingSlot::m_version`（`MG_Util/Types.h:197`）、`FramebufferObject::m_objectVersion`（`:183`）、`SamplerObject::m_version`（`SamplerObject.h:155`）、`RenderStateParameters` 版本（`RenderState.h:522`）、`TextureObjectBase::m_textureParamsVersion`（`:203`）全部回绕。**身份比较是堵住回绕洞的那块补丁。** 完整的 21 条重键表在 §4.7；这里只点三条最有教育意义的：

- **D3 `UnitTextureSyncEntry` + `PairingsIntact`**（`DirectGLES.cpp:1441-1481`）：注释写明它存在是因为"一次不经过 bind generation 的 slot 交换（DSA by-name 模拟以前就会静默交换一个 slot）会让每个键都匹配，而借来的 slot 指向另一张纹理，replay 于是会**用纹理 B 的前端状态驱动纹理 A 的后端 twin**——用 B 的形状重新指定 A 的后端存储并毁掉 A 的内容"。**这是整份调研里最强的"支持推送接口"的论据**：这一整类 bug 只在"client 能改一个绑定而不移动任何计数器"时才存在。审计义务从"哪些读需要守卫"变成"哪些 mutator 必须发消息"，由 §10.3 的 verify 模式、poison mask 与推论 4 的 dirty-surface 生成器共同强制。（**注意**：这条的**去抖**部分搬到 client，见 §2.5。）
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

**所以 monolith 的净行数是增加的，不是减少的。** §10.4 与 §3 里 "~550 行删除" 不再作为主论据；**主论据是 §10.3-④ 的逐线程 CPU 数字**（每 draw 指令数与 cache line 触达数的减少），而删除清单降级为佐证。B-R2 因此有了一个可证伪的预测而不只是定性主张。

---

## 3. 与方案 A（replica `GLContext`）的逐项对比

> 方案 A = `../MobileGL-disagg/docs/Disaggregated/PLAN.md`（feat/disaggregated@8b31de2f）。阶段天数逐项相加 = **77 天**。

| 维度 | 方案 A（replica） | 方案 B（MGPipe） | 判定 |
|---|---|---|---|
| **边界清晰度** | 边界**就是** replica：server 侧跑一份真 `GLContext`，backend 的 293 次拉取原样成立。没有写下来的契约，也无法写。新增 backend 必须先学会 186 个前端 getter 与 17 个 mutator 族 | 一份显式函数表（~72 项）+ 一份 POD payload 表 + `PipeCalls.def` 单一真相源。新增 backend 只实现两张表。`MG_Backend` 的 `MG_State` include 从 50 行 / 18 个头文件收缩到一张共享**值**头白名单 | **B 完胜**，这正是用户提出的目标 |
| **状态副本** | 一份完整 `GLContext` 对象图 + 每个 <16MiB buffer 一份 `PipeResource` + 每个纹理 level 一份 `MipmapStorage` + server 侧 `MG_State`/`MG_Impl`/`MG_Util`（含 glslang ~43MB 文本页） | **一处副本**：split 且 `kCapNeedsHostIndexBytes` 时的索引宿主镜像（有预算、有计数器、有回退）。其余零副本 | **B 完胜**（量级差别） |
| **CPU 工作量** | `PLAN.md` §10 自承："这套遍历**每 draw 跑两次**"——client 的 `WireMirror` 一次、server 未改动的 `PrepareForDraw` 一次，外加编解码 | 遍历**搬走**而不是翻倍：client 做 O(1) 快门（值类用既有计数器、对象类用 5 个新增聚合世代）+ 未命中时的 touched 前缀走查 + N 次 `set_*`，server 侧真删除 ~372 行失效发现机制。**但基线比 v1 声称的窄**（§2.3.1）——**这是主张，不是测量** | **B 理论上更好，未证实**。两者都必须以逐线程 CPU 时间为准 |
| **内存** | `PLAN.md` R14 自估 **可达 ~450MiB 新增** | 典型 **+50-60MiB**；最坏（索引镜像满 + stage 余量满）**+145MiB** | **B 完胜** |
| **Roundtrip** | 稳态零（除回读/阻塞 query/分配 ack/present credit） | 稳态零（同上），**外加**一个新类：server 发起的纹理重铸拉取。三条缓解 + 终止符 + 专门的门 + 逐用例计数器（§7.5、§9.3） | **A 略优**，差距被压到"实测发布"而非"声称为零" |
| **改造量** | backend **一行不改** | backend 改 293 个读点 + 58 行非箭头用法 + 95 个写回点 + ~66 个 memo 族重键 + 两处 `MG_State` 类型内部用法重写 + 一个头文件抽取前置阶段 | **A 完胜** |
| **迁移期风险** | 风险**集中在末端**且**难以测试**：replica 的行为漂移编译通过、多数内容渲染正确，守卫只看签名 | 风险**分布在 ~14 个阶段**，每阶段可二分、有现成测试套件作门、有**逐 draw 逐字段的语义比对**。但它**改动 monolith**，且 **stage C 之后 `MOBILEGL_PIPE_PUSH` 的 A/B 口径会收窄**（§6.7 v2 修订） | **B 的正确性风险更低，A 的产品风险更低** |
| **到首帧时间** | **~第 15 天**跨进程首帧 | **~第 95 天** inproc 首帧、**~第 100 天** 跨进程首帧，且是**缩减路径**；全功能在第 ~139 天。最早可见里程碑是**第 ~25 天**的 verify harness 全绿 | **A 完胜（6-7 倍）** |
| **长期价值** | 拆分达成；monolith 不变；边界仍未定义。维护成本**永久** | 边界被写下来、被生成、被测试。第三个 backend、shader 缓存服务、record/replay 层、真正的第二个 context 都变得可行。成本**一次性**。**但 monolith 的净代码量增加**（§2.7） | **B 完胜** |
| **对 monolith 的收益** | 零（按设计如此） | ~372 行 per-draw 失效机制**真删除**（另 ~175 行搬到 client）；复用地址 ABA 一类不可表达；FBO→program 排序 hazard 消失；`pDefaultFramebufferInfo` 分层倒置消失；`inproc` = 渲染线程杠杆；顺带修两个潜伏 bug；顺带暴露一个死能力（`FramebufferSrgb`/`DepthClamp` 无存储，`RenderState.cpp:380/428-429`，6 个 backend 读点恒为 false） | **B 完胜**，但**收益要以 CPU 数字而非行数计**（§2.7） |

### 3.1 方案 A 里被证明**不可能**、而不只是"不理想"的两件事

1. **`RecProgramLinkOp`（server 从源码重新 link）**。`ProgramObject.h:11 → ShaderObject.h:12 → ShaderCompileTask.h`，`ShaderObject.h:146` 返回 `SharedPtr<glslang::TShader>`，`ProgramObject.h:14 → SpvcSession.h`。链接真 `ProgramObject` 就链接 glslang。所以 `ProgramPublish` 第一天上、`MOBILEGL_IPC_PROGRAM=publish|relink` 开关消失、阶段 P5 整个消失（6 天回收）。**但方案 B 因此欠下 P0.5 的头文件抽取**（§0.4）。
2. **方案 A 的字节一致门在方案 B 里不成立**（D-B5）。这不是方案 B 的缺陷论证，是它必须公开承认的成本。

### 3.2 方案 B 复用方案 A 的比例

`PLAN.md` 的 §6-§14 按体量算是全文的大部分，且与状态模型无关。方案 B 原样继承，逐条对照见 §8 与 §14。**因此"选 B 不选 A"并不浪费传输侧的设计投资。**

### 3.3 一句话决策规则

- 目标是**这个季度出一个能跑的拆分**，或拆分的价值主要按"进程隔离/崩溃隔离"计算 → **选方案 A**。
- 目标是**用户提出的那个架构** → **选方案 B**，按 §0.6 的对冲路径起步，第 42 天用真数字做 GO/NO-GO。
- **不要**试图先做 A 再做 B。A 的 replica 一旦上线就成为"边界"的既成事实，而 B 的第一步会作废 A 的全部 applier 代码——两条路的 backend 侧改造互斥，共享的只有传输层。
