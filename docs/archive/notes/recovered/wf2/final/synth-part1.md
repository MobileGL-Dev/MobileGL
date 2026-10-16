# MobileGL 方案 B 实施计划：gallium 式显式接口 + backend 自有状态机（MGPipe）

> 状态：设计定稿 v1（2026-09-05）。基线 `dev@81b17c0b`；实施分支 `feat/disaggregated`（worktree `../MobileGL-disagg`）。
> 本文是**方案 B** 的实施计划。方案 A（server 内跑 `MG_State::GLState::GLContext` replica）见同目录 `PLAN.md`，其评审记录见 `REVIEW.md`。
> 本文继承方案 A 的 §6-§13（传输、数据面、同步、present、线程、平台、构建），**只替换它的状态模型**（§5 与 §12 的 replica 特化部分）。凡标注"继承 PLAN.md §X"的内容，以 `PLAN.md` 为准，本文不复述。
> 全部 `file:line` 引用针对**工作树** `dev@81b17c0b`。工作树有两处未提交的 `fprintf` 插桩，使 `DirectGLES.cpp` 在 ~660 行之后偏移 +11、`Managers.cpp` 在 872 行之后偏移 +3；`MG_State/`、`MG_Impl/`、`MG_Backend/DirectVulkan/` 的行号与 HEAD 一致。
> 阅读顺序：本文件（§0-§3）→ `synth-part2.md`（§4-§6）→ `synth-part3.md`（§7-§10）→ `synth-part4.md`（§11-§14 + 附）。

---

## 0. TL;DR、推荐与决策

### 0.1 一句话

**`MG_Backend` 已经是一台贴着目标 API 的状态机；它缺的不是状态，而是一份"我被告知了什么"的显式声明。MGPipe 就是那份声明。** 前端不再让 backend 每 draw 走 293 次 `MG_State::pGLContext->` 把整个 `GLContext` 拉出来，而是在每条命令之前由一个 state tracker 把变化**推**过去；server 进程因此只需要装 `MG_Backend` + MGPipe 的对象表，**不链接 `MG_State`、不链接 `MG_Impl`、不链接 glslang**。

### 0.2 接口不是从 gallium 自顶向下设计的，是从两个 backend 自己维护的关键结构反推出来的

这是本设计与"照抄 gallium"的根本区别，也是完整性论证的来源：

| backend 已有的结构 | 它是什么 | 反推出的接口 |
|---|---|---|
| `SetupDrawSnapshot`（`VulkanRenderer.h:948-1042`，40+ 字段） | Magma 一次 draw 必须钉住的**全部**东西的枚举 | `set_*` 组的并集 |
| `DrawTextureSyncKeys` + `BackendTextureObject::IsDrawSyncClean`（`Managers.h:1003-1020`） | Espryt 纹理"是否还干净"的**全部**输入 | `set_sampler_views` + `create_sampler_view` |
| `ResolvedDrawBuffers`（`Managers.h:697-717`）/ `ResolvedVertexBindings`（`VulkanRenderer.h:1153-1218`） | 顶点输入的完整声明 | `bind_vertex_elements_state` + `set_vertex_buffers` + `set_index_buffer` |
| `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`） | 渲染状态声明，**逐字节** | `create/bind_render_state`（整块，见 0.5） |
| `BufferBackendOps`（`BufferObject.h:76-120`，7 个 hook） | 已经是接口，且注释自称 "the `pipe_context` buffer-op analogue"（`:68`） | `resource_*` 全族 |

把这些结构的**输入集合**推过去，接口就按构造完整。gallium 是**目的地**（同名同形的词汇让形状可读、可迁移），不是**推导前提**。凡 gallium 的词汇与本仓库的证据冲突的地方，本文按证据走，并在 §4.6 逐条记名列出偏离与理由。

### 0.3 三条结构性推论（决定了后面每一节）

**推论 1 — 推送必须发生在 draw 时刻，不是 GL setter 时刻。** Blaze3D 每个 batch 都用 `glEnable/glDisable(GL_BLEND)` 包住，代码自己把它标成最热的路径（`DirectGLES.cpp:2044-2046`：`mc_state_toggle` 干的最热的事）。天真的 per-setter 推送会把每一次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，**严格慢于今天**。正确形态是 gallium 的 `st_validate_state`：前端的 21 个版本计数器就是现成的 dirty 模型，tracker 在每条命令前做一次 dirty word 比较，只发变了的组。**这是本计划里最容易做错、且做错就必然回归的一处。**

**推论 2 — handle 就是身份，而且必须是稠密 slot。** 每个前端对象已经有一个永不复用的 `GetLifetimeId()`（`BufferObject.h:202-208`、`VertexArrayObject.h:110-120`、`FramebufferObject.h:151-158`、`ProgramObject.h:1620`、`TextureObject.h:83`、`SamplerObject.h:533-537`），它们存在的唯一理由是 GL name 会被 `IndexGenerator::Generate` 从 free list 尾部 LIFO 复用（`MG_Util/Miscellany/IndexGenerator.h:30-42`）、堆地址会被分配器复用。但**单调的 64 位 id 不能索引数组**——如果 wire handle 直接用 lifetimeId，server 侧仍然是一张哈希表，那就只是把指针键换成整数键，并没有删掉查表层。所以 wire handle 是 `{slot: Uint32, gen: Uint32}`，**slot 由 client 按 kind 稠密分配**，`gen` 在 slot 复用时 ++。lifetimeId 留在 client 侧作为 tracker 自己的身份，不过线。这一条才真正把 6 个 `StateBackendObjectRegistry` 哈希表和 13 个 Magma 身份键缓存变成**数组**。

**推论 3 — server 拥有 client 看不见、也永远不该被问的 generation。** 今天有 12 个纯 backend 侧的单调计数器，它们表达的是"**我自己**重新铸造了驱动对象"，与任何前端版本无关：Espryt 的 `g_bufferMutationEpoch`（`Managers.h:397-441`）、`g_bufferBackendIdGeneration`（`:551`）、`g_attachmentBackendIdGeneration`（`:1298`）、`g_backendContextGeneration`；Magma 的 `m_textureImageEpoch`、`m_resourceEraseEpoch`、`m_renderbufferImageEpoch`、`m_sliceEpochCounter`、`m_cacheStructureEpoch`、`m_evictionEpoch`、`m_recordingGeneration`、`m_frameSerial`。本文把它们统称 `MGGen`，**它们永不上线**。"server 拥有自己的状态机"在工程上的确切含义就是这一条：client 绝不是"我的 server 侧状态是否新鲜"的唯一权威。

### 0.4 与方案 A 的结论性对比（详表见 §3）

**方案 B 在架构、内存、长期价值上赢；方案 A 在"多快能拿到第一帧"上赢，而且赢得毫无悬念。**

方案 B 赢的四点，全部可核对：

1. **内存。** `PLAN.md` 自己的 R14（第 1222 行）给 replica 预算 "合计可达 ~450MiB 新增"：每个 <16MiB store 一份重复 `PipeResource`、每个纹理 level 一份重复 `MipmapStorage`、一整份 `GLContext` 对象图，叠在两侧都要付的传输段与 ring 之上。这在一个把"省 400MB"当作头条战果、且有 blanket-immutable 触发 LMK 屠杀记忆的项目里，是最难辩护的一条。方案 B 不复制任何东西：传输段（~48MiB）+ POD slot 记录 + 一个**可选的、有上限的 ≤32MiB 纹素保留 LRU** ≈ **+50-60MiB**。
2. **拷贝。** `PLAN.md` §6.4 数出 `glBufferSubData` → store 在 split P1-4 是 **4 次**、P4.5 是 **3 次**，其中第 (3) 次是 `SEG_STAGE`→**replica** shadow。没有 replica 就没有这次拷贝：方案 B 是 **3 / 2**。而 `PLAN.md` 自己把 2 次称作"方案 B（激进，需额外设计）"（第 549 行），要求给 replica 的 `PipeResource` 加第三种 `AdoptedClientShadow` 模式并处理 server 侧写的 copy-on-write 升级，且把它推迟到 P6 由 Tracy 数据决定（开放问题 §17-5）。方案 B **按结构就在那个目标上**，并顺带关掉它自己的开放问题。
3. **漂移面。** replica 是一份必须与 20k 行 `MG_State` **语义**长期锁步的手写状态模型，而它的守卫（生成的 `is_same_v`/`sizeof`/`offsetof` + `reflectionDigest`）只能看见**签名**漂移。`MipmapStorage` 的 96-rect 级联合并与 `summedArea*4 >= unionArea*3` 回退（`MipmapStorage.cpp:300-305`）、`VecRange1D` 的 7% gap 比、`PipeResource` 的模式切换、`BufferObject` 的 persistent-map 状态机——任何一处行为不一致都能编译通过、在多数内容上渲染正确，而这恰好是本项目已经实测出 **+6ms/frame** 悬崖的那块地方。方案 B 只有一份状态模型，这一类失效**不可表达**。
4. **整块子系统消失而不是被移植。** `PLAN.md` §2(g) 的"第七个面"（MG_Impl 在 table 调用旁做的 `MG_State` mutation：`AccountTransformFeedbackPrimitives`、`EnsureGeneratedMipmapStorageAllocated`）连同它的第二个代码生成器 `gen_impl_mutation_surface.py`、`MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::` helper 族和风险 R1，在方案 B 里**不存在**——没有 replica 就不需要 replay。同样消失的还有：§5.6a 的纹理 ack 协议与 R6；§5.7 的 "server 自建 composite" 分支；§6.9 的 relink 档与整个阶段 P5（6 天）；§12.2 里跨越 1494 个 `MG_Impl` 站点的 `pGLContext` shim（`inproc` 需要隔离的进程全局从 4 个降到 2 个，`PLAN.md` 最早的证伪门 P2.5 因此变便宜）。

**`RecProgramLinkOp` 不是"不理想"，是不可能。** `ProgramObject.h:11` include `ShaderObject.h`，后者 `:12` include `ShaderCompileTask.h`、`:146` 返回 `SharedPtr<glslang::TShader>`；`ProgramObject.h:14` 又拉进 `SpvcSession.h`。**任何链接真 `ProgramObject` 的 server 就链接了整条编译链。** 所以方案 A 的两档 program 方案在方案 B 里塌成一档：`create_shader_state` 第一天就是 SPIR-V + 反射归档，`reflectionDigest` 这个"分歧预言机"因为没有可分歧的对象而被换成一个**schema 完整性绊线**（`Visit()` 全结构体归档 + `sizeof` static_assert）。

方案 A 赢的一点，也毫无悬念：

- **到首个跨进程帧的时间。** `PLAN.md` 的阶段天数逐项相加恰好是 **77 天**（P0 5 + P1a 6 + P1b 4 + P2 9 + P2.5 3 + P3 5 + P4 5 + P4.5 4 + P5 6 + P6 6 + P7 8 + P8 6 + P9 10），其中 **P1b 出口（≈第 15 天）就是首个跨进程帧**，因为它一行 backend 代码都不用改。方案 B 最早的 `inproc` IPC 帧在第 ~68 天，最早的**跨进程**帧在第 ~73 天，且那一帧是**缩减路径**（emulation 在 P8 之前于 split 模式下直接 Fatal），全功能要等 P8。总估时 **200-260 人天**（含 IPC；不含 CTS 周转，见 §11.5）。

**如果目标是"这个季度拿到一个能跑的拆分"，选方案 A。如果目标是用户实际提出的那个——"backend server 拥有自己的状态机并暴露统一的、gallium 式的接口把前后端解耦"——方案 A 在任何价格下都不交付它**：它用复制前端来回答耦合，而不是用定义契约来回答耦合，而且那份复制的维护成本是**永久**的；方案 B 的成本是**一次性**的，且在第一个字节过 socket 之前就已经把 monolith 变好（删掉 ~550 行 per-draw 失效发现机制、让复用地址 ABA 一类失效不可表达、删掉一个排序 hazard、删掉一处分层倒置、修掉两个潜伏 bug、暴露一个死能力）。

### 0.5 六个必须先记下来的具体决定（这些是评审里争议最大的点）

**D-B1：渲染状态用"整块 blob + CSO handle"，绝不拆成 blend/depth-stencil/rasterizer 三个 CSO。**
`RenderStateParameters`（`RenderState.h:222-370`）是一个平凡可复制的 POD，Espryt 在 `DirectGLES.cpp:2035` 亲自 `static_assert(std::is_trivially_copyable_v<...>)`，紧接着做 head/blend/tail **三段 memcmp**（`:2042-2047`）；而 `RenderState.h:359-368` 白纸黑字写着 `ScissorBoxWrittenMask` 和 `ClipDistanceEnabledMask` 是**故意**摆在 tail 段里，好让那次 span memcmp 像抓其它状态一样抓到它们。**字段顺序是承重的。** 拆成三个 CSO 会丢掉一条被文档化的布局不变式，还要手工维护一张 ~150 字段的"字段→CSO"划分表且没有任何完整性绊线（漏分配的新字段会静默永不推送），而 Espryt 无论如何还要保留整块 blob 才能对着驱动做增量 GL 调用。
**最终形态**：`create_render_state(cso, const RenderStateParameters*)`（整块，每个唯一状态一次）+ `bind_render_state(cso, version, pipelineVersion)`（稳态 12 字节）。client 侧维护 64 项 LRU，键是三段的 xxHash。命中 → 12 字节；未命中 → 发生成的**变化段**（3 bit `dirtySpanMask`，段划分与现有 memcmp 完全一致）+ 前一个 CSO handle 作为基。Espryt 的 memcmp 变成 handle 比较且它自己保留每 CSO 的 blob；Magma 每 CSO 算一次 pipeline hash，而不是每次 `GetPipelineStateVersion()` 移动都算。**两个版本计数器都上线**（`m_version` `:522` 与 `m_pipelineStateVersion` `:529`）——合并它们会让每一次 `glViewport` 冲掉 Magma 的 pipeline memo，这正是 `:523-528` 记录的那次回归。

**D-B2：`create_shader_state` 不返回一个"做完了的"对象。** backend program 还依赖 8 个额外输入（`DirectGLES.cpp:2766-2818`：draw FBO 的 snorm/unorm fallback clamp mask、由 draw-buffer 数组推出的 fragColor 广播数、storage-block 绑定签名、atomic counter 绑定集、**活的** `glBindImageTexture` 格式、patch 参数；Magma 另加 FragCoord-Y-flip 的 default-FB 高度和 XFB 布局）。接口**明说规则**：`create_shader_state` 发布**制品**，server 在 draw 时刻从它已经被推送过的状态**惰性特化**。这正是两个 backend 今天的做法，而推送模型让它比拉取模型**更安全**（见 D-B3）。

**D-B3：validate 顺序是契约，不是 hazard。** framebuffer 严格先于 program。四个跨对象 mask 由 FBO sync 从 attachment 格式推出（`Managers.cpp:5616-5619`），又被渲染状态推送（`DirectGLES.cpp:2014`）和 program 陈旧性判定（`:2769-2770`）消费；今天 Espryt 靠**自己在 `SyncCurrentProgram` 里重新推导广播数**（`:2712-2732`）来绕过这个排序，注释写明"否则用陈旧计数编译出的 program 要等到下一次 draw 才会重链"。固定推送顺序把 hazard 变成免费属性，那段 workaround 和 `g_broadcastMemo*` **一起删掉**。同理 `ImageUnitFormatsStillMatch`（`Managers.cpp:6545-6573`，注释明说"不可表达为单调版本"）被 `set_shader_images` 直接告知。

**D-B4：AcquirePersistentMap 在整个改造期一动不动。** 它是**永久的地址空间捐赠**而不是 gallium 的 scoped `transfer_map`：返回一个 host-visible coherent 指针，成为该 buffer 的唯一真相源（`BufferObject.h:102-118`），由 `PipeResource::AdoptPersistentMap`（`PipeResource.h:115`）采纳、经 `MappedData()` 交给应用、≥16MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到（`:226-228`）。实测代价是 MC 26.3 的 p99 163→21ms、40→115fps、省 ~400MB。**它今天就已经是一个"返回指针的显式调用"，因此原样穿过 monolith 改造；只有 IPC 那一步才会打破它。** 所以：改造期不碰，IPC 期按 `PLAN.md` §6.8 的三档 POST 探针决定，spike B 第一周给答案。绝不允许一个平台未知数挡住 200 天的接口工作。

**D-B5：monolith 字节一致门按构造死亡，这是本方案的成本，必须写在明面上。** `PLAN.md` §12 第 4 层（`nm --defined-only` + 剥调试信息后 `.text` size 相等，且每阶段都跑）是它最强、最机械的保证。方案 B 让 backend 停止读 `pGLContext`、让 memo 换键、让 `MG_Impl` 多出 validate 调用——**不存在任何配置能让旧字节回来**。替换是**五部分门**（§10.3），其中第 2 部分（每 draw 逐字段的 pushed-vs-snapshot 影子比对）在语义上**严格强于**任何符号 diff，而且它只有在"接口先落在 monolith 里、两套状态模型活在同一个地址空间"的前提下才存在。

**D-B6：方案 B 引入一个方案 A 没有的新停顿类：server 发起的纹理重铸拉取。** server 不保留纹素字节，所以 `RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）都必须回头向 client 要数据。**三条缓解同时上，不是三选一**，加一个专门的门和一个逐 trace 用例发布的计数器（§7.5）。上一轮 thin-server 设计正是因为把这条一笔带过而被判死，本文不重复。

### 0.6 推荐

**推荐执行方案 B，但按下面这个对冲路径起步，在第 24 天做一次真正的 GO/NO-GO：**

先原样跑 `PLAN.md` 的 P0（它的卫生、传输骨架、两个 spike，尤其是 **`TracyPlot` 逐帧字节计数器**——树里今天完全没有 per-frame 字节或调用度量，`MG_Util/Metrics` 只是格式算术，Tracy 只有 zone 无 plot），然后跑本文的 **P1 + P2**（共 ~16 天）。第 24 天你手上会有：

- 一套 `MOBILEGL_PIPE_VERIFY` 影子比对 harness，在全部 40 个 trace 用例和 367 个集成测试上逐 draw 逐字段证明"推送等价于拉取"，**产品零风险**；
- 两个 backend 上都已推送的渲染状态，`SyncRenderState` 的 693 行函数体一行未动、memcmp 门变成 handle 比较，Magma 的 ~40 次 accessor payload 走查消失；
- 两台设备上 reboot-clean 配对的**逐线程 CPU 时间**增量——本设计中心性能主张（"可达性遍历是**搬走**了而不是翻倍，而且更便宜"）的第一个真数字；
- Track H（对象类读点）在两个最便宜子系统上的实测单位成本。

这 16 天无论最后选哪个方案都不浪费：P0 完全是 `PLAN.md` 的，P1 的 `sed` 替换在 pull 构建里字节一致，P2 的渲染状态推送即使不做拆分也是 monolith 的净收益。**如果第 24 天的 CPU 数字是负的、或者 Track H 的单位成本比估计高 50% 以上，退回方案 A 只损失这 16 天。**

---

## 1. 目标与非目标

### 1.1 目标

1. **定义并落地一份显式的前后端接口 MGPipe**：句柄寻址、只推不拉、gallium 形状，client 与 server 都只依赖它。
2. **backend 拥有自己的状态机**：`MG_Backend` 在 MGPipe 构建下**不含** `MG_State::pGLContext`，`MG_State` include 收缩到一张共享**值**头文件白名单，server 产物的 `nm --undefined-only` 里没有 `MG_State::GLState::` 符号、没有 glslang 符号。
3. **前后端跑在两个进程**，通过 IPC 通信；client 把状态 reconcile 成推送调用、序列化（FlatBuffers）后发送；server 更新自身状态并调 backend API。
4. **稳态帧零 round trip**（回读 / 阻塞式 query / sync wait / present credit / 分配类错误 ack / 纹理拉取之外，且后者的次数必须**实测发布**而非声称为零）。
5. 两半尽可能互相异步；client 至多领先 server 1 个 present（默认，延迟叠加分析继承 `PLAN.md` §9.1）。
6. 平台特定代码最小化并集中在 `MG_Remote/Transport/` 与 `MG_Remote/Client/Surface*`（继承 `PLAN.md` §11）。
7. **单进程 Monolith 保持功能与性能不回归**，由五部分门机械验证（§10.3）。注意这**不是**方案 A 的"字节级不变"——见 D-B5。
8. 所有验收门用**现有测试**：`ctest -L unit`（428 个 `TEST(`）/ `-L integration-gpu`（367 个 `TEST_F`，75 个场景文件）/ `tools/trace_replay`（40 个用例，默认 SSIM ≥ 0.99）/ `tools/cts` / `tools/device_bench`。
9. **接口本身是可独立交付的产物**：即使 IPC 永不上线，`inproc`（同进程第二个 apply 线程）就是 monolith 的渲染线程交付物，且是本项目手上最大的单一 CPU 杠杆。

### 1.2 非目标

- **share-group sessioning 重构。** 与 `PLAN.md` 一致：`eglCreateContext` 的 `shareCtx` 只在 `EGLState/Core.cpp:632` 被校验、`:640` 被存进 `EGLContextState::SharedContext`，**全代码库无人读取**（repo 级 grep 只返回这两行）；`pGLContext` 是唯一进程全局（`GLState/Core.cpp:20, 1487`）。v1 = 一条 flow、一个扁平 handle 空间。但**接口头文件从第一天就把 `MGPipeScreen` 与 `MGPipeContext` 分开**（§4.3），因为事后再拆要给每个记录种类重新编号。`c7c9e346`/`29d721ef` 那套整体丢弃（理由见 `PLAN.md` §14 DROP）。
- **BFA strict-C-ABI backend 插件 / UtilRuntime C-ABI 化**（同 `PLAN.md`）。
- **macOS 拆分**（同 `PLAN.md`：`CAMetalLayer` 无公开跨进程表示，interposer 导出表锁定于 `CMakeLists.txt:600-612`，无 CI 无设备 → monolith only）。
- **Windows 窗口拆分**（同 `PLAN.md`：headless/pbuffer only）。
- **把 emulation 层重写到 client。** 只有五个"读前端字节的纯 CPU 变换"下放到 client（§5.7）；viewport-array 回放、multi-draw 分档、fp64 顶点转换、image-bindable 存储加宽等**全部留在 server 作为 lowering pass**，接口只负责把它们的输入表达清楚。
- **在 P13 之前删除 pull 路径。** 旧路径一直编译在里面，任何提交都能用一个 env 位 A/B。

---

## 2. 现状：边界为什么不清楚

### 2.1 今天的边界有七个面（沿用 `PLAN.md` §2 的分面，数字按工作树复核）

**(a) `GLFunctionsTable`** — `MG_Backend/BackendObject.h:117-278`。**实测 67 个函数指针 + 1 个 `Bool` 能力位**（`PrefersCpuXfbPrimitiveAccounting`），`GlobalBackendFunctionsTable`（`:279-285`）再加 `Present` 与 `SetSwapInterval` → **全体 69 个函数指针**。（`PLAN.md` 与几份调研报告里的 "73 项"/"71 项" 都是旧计数或口径不同，本文一律用 67+1/69。）
MG_Impl 侧 **89** 个 `gBackendFunctionsTable.GL.*` 调用点。**null 项已经表示"未实现，前端回退"**，这一点被写进头注释（`:212-215` 的 sync 族、`:265-269` 的 XFB 跨度），且 DirectVulkan 确实留空 8 项而 Espryt 填满。三项是错位的前端查询：`GetIntegeri_v`/`GetInteger64i_v`（`:195-196`，`DirectGLES.cpp:7264-7386` 有 15 个 case 完全不碰 GL）、`GetProgramiv`（`:197`）。`ShaderStorageBlockBinding`（`:207-208`）的注释自己就说 program interface 全部由前端回答。

**(b) `BackendObject` 虚函数** — `BackendObject.h:543-568`，MG_Impl 侧 **40** 个 `pActiveBackendObject->`（其中 35 个是 `GetDynamicParameters()`）。`InitCapabilities()` 懒执行在第一次成功的 `eglMakeCurrent` 内部（`BackendObject.cpp:341-347`），且每次 surface 变更重新武装（`:301`）。

**(c) `BufferBackendOps`** — `BufferObject.h:76-120`，**7 个 hook**，注册入口 `:124`。Espryt 注册 7/7（`Managers.cpp:1338-1346`），Magma 注册 6/7（**故意**不注册 `ResidentSubData`，`VkBufferManager.cpp:104-111`，保留旧的就地宿主写；`BufferObject.h:84-92` 记录了它存在的理由是 Minecraft 在活的 coherent map 上就地改字节会撕裂正在读旧字节的帧）。**这个面已经是 MGPipe 的三分之一，且注释自称 `pipe_context` 类比。**

**(d) 状态拉取** — `MG_State::pGLContext->` 在 `MG_Backend` 里 **293 次出现 / 290 行**（DirectGLES 124：`DirectGLES.cpp` 102 + `Managers.cpp` 15 + `MultiDraw.cpp` 5 + `Utils.cpp` 2；DirectVulkan 169：`VulkanRenderer.cpp` 126 + `DirectVulkan.cpp` 18 + `UniformManager.cpp` 14 + `VkRenderPassManager.cpp` 3 + `VkTextureManager.cpp` 2 + `BackendObject_DirectVulkan.cpp` 2 + `VkClearManager.cpp` 1），**外加 58 行非箭头用法**（见 2.4）。此外还有约 1997 个前端对象 getter 调用点、186 个不同 getter（上界统计，少数名字与 backend 本地结构撞名）。

**(e) backend → frontend 写回** — 逐名 grep 实测 **95 个调用点 / 17 个方法**：`SyncPersistentMappedRange` 20、`MarkStorageDirty` 18、`AllocateStorage` 8、`WritebackFromBackend` 8、`SetInternalFormat` 7、`SyncGpuWrites` 6、`MarkGpuWritten` 6、`RecordError` 6、`SetBackendResource` 4、`EnsureGpuResidentStorage` 3、`SetBackendHashMemo` 2、`InvalidateCompileEnv` 2、`SetBackendStateMemo` 1、`SetBackendAuxMemo` 1、`UpdateMipmapSubData` 1、`TruncateMipmapLevels` 1、`SetSamples` 1。

**(f) backend 反向进 MG_Impl** — 恰好 6 处：`DirectGLES.cpp:1917, 2838, 2867, 9675`（`pDefaultFramebufferInfo` 身份比较）、`SwapchainObject.cpp:276`（**写**，backend 在 client 侧创建 default-FBO 的颜色/深度/模板 `ITextureObject`）、`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`，一处真正的分层倒置）。

**(g) MG_Impl 在 table 调用旁做的 `MG_State` mutation** — `EnsureGeneratedMipmapStorageAllocated`（`GL_Texture.cpp:501-544`，调用点 `:6698, 6708`）与 `AccountTransformFeedbackPrimitives`（`GL_Drawing.cpp:172`，调用点 `:1133, 1141, 1195, 1668`）。**在方案 B 里这个面不存在**：它只有在"server 必须重放前端的副作用"时才是拆分问题，而 MGPipe 的 server 没有前端可以重放。

**(h) 工作树污染** — `DirectGLES.cpp:290-303`/`:640-663` 与 `Managers.cpp:875-877` 的未提交 per-draw `fprintf(stderr)`（后者在 `pendingMutex` 临界区内）。**P0 第一件事就是清掉**，它污染每一次基线测量。

### 2.2 backend 已有的状态机清单（这就是"server 已经是薄服务端"的实证）

**DirectGLES（Espryt）**
- 6 个 twin registry，全部是 `StateBackendObjectRegistry<StateObject*, {BackendPtr, weak_ptr}>`（模板 `Managers.h:270-390`；实例 `:806`(VAO) `:1123`(Texture) `:1216`(FBO) `:1731`(Program) `:1830`(Sampler) `:1858`(Renderbuffer)），键是**前端裸堆地址**，用同址 `weak_ptr` 防 ABA，GC 阈值 `kGCInterval=1024` draw / `kCreationGCInterval=64` 次创建。
- 三条 persistent-mapped bump ring（UBO `Managers.h:591-637`、纹理 unpack PBO `:639-671`、buffer upload `:673-…`），各自 4MiB 起 → 64MiB 上限，各带帧 fence 回收水位；buffer pool 预算 `kMaxPoolBytes = 64MiB`、单 buffer 上限 8MiB（`Managers.cpp:564-565`）。
- 每对象 twin：`GLESBufferResource`（`Managers.h:443-497`，含 `pendingRanges`/`pendingResidentWrites`/`drawCleanEpoch`/`syncedChangeSerial`）、`BackendVertexArrayObject`（`:675-803`，32 路 client-attribute 暂存 + 转换后的 fp64 流 + 同步版本影子）、`BackendTextureObject`（`:944-1119`，`StateTextureBasicInfo` + 采样器/参数缓存 + `IsDrawSyncClean` 聚合门）、`BackendFramebufferObject`（`:1140-1213`，含 `m_backendColorSlots` draw-buffer 置换表）、`BackendProgramObjectImpl`（`:1473-1725`，最大的一个：跑 SPIRV-Cross 转 ESSL 并用 ES 驱动 link，缓存全部 emulated builtin 的 uniform location、编译输入快照、`SamplerPassMemo`）、`BackendSamplerObject`（`:1808-1824`）、`BackendRenderbufferObject`（`:1838-1855`）。
- 完整的渲染状态**值镜像** `g_syncedRenderStateParameters`（`DirectGLES.cpp:1956`）+ 三段 memcmp 门（`:2035-2047`）。
- 驱动绑定影子：`BindBufferId`/`BindPixelPack/UnpackBufferId`、`g_indexedUBO/SSBO/XFBBindings`、`BindBackendVAOId`、`g_driverFBOBindings`、`g_boundTexturesCache[192][TargetCount]`、`g_boundSamplersCache[192]`、`g_activeTextureUnit`、`PackState`。
- 三个共享 scratch FBO 及其完整驱动侧 attachment 影子（`Managers.h:1338-1359`）。

**DirectVulkan（Magma）**
- `VulkanRenderer`：`PipelineMemoEntry m_pipelineMemo[8]`（`VulkanRenderer.h:808-830`）、`SetupDrawSnapshot m_setupDrawSnapshots[4]`（`:948-1042`，40+ 字段）、`VaoDrawMemo m_vaoDrawMemoTable[2048]`（`:1230-1266`）、`ResolvedVertexBindings`（`:1153-1218`）、`m_convertedVertexStreams`（`:1080-1144`）、`DynamicStateShadow g_dynamicStateShadow`（`VulkanRenderer.cpp:277-340`）、采样集/LOD/BaseVertex 三个 memo、11 个 per-draw scratch vector。
- 5 个 manager：`VkBufferManager`（`VkBufferResource` 二级 twin + transient arena + 延迟释放）、`VkTextureManager`（3504 行，`TextureIdentity{ptr, lifetimeId}` 键的节点式 map + 5 类 view 缓存 + 版本影子）、`VkRenderPassManager`（render pass 缓存 + renderbuffer twin + `m_rpFast` 快门）、`VkSamplerManager`、`VkClearManager`。
- 3 个 factory：`PipelineFactory`、`ProgramFactory`（带 `m_cacheStructureEpoch`）、`VertexInputStateFactory`（带 `m_evictionEpoch`）。
- `UniformManager`（描述符池 + 5 个 memo + 5 个 scratch）、`FrameContext`（frames in flight）、`SwapchainObject`。

**结论：两个 backend 都已经是完整的、贴着各自 API 的状态机。** 上面**没有一样东西需要在方案 B 里删除或重写**——需要改的只是它们**怎么知道**这些事实（从"每 draw 把 `GLContext` 拉出来自己推导"改成"被告知"），以及它们的 memo **用什么做键**（从裸指针 + 回绕 `Uint16` 改成 `{slot, gen}`）。

### 2.3 pull 模型的读点分类：A/B/C/D/E 五类

对 290 个 `pGLContext->` 读点逐个分类（D 与 B 有重叠，因为身份读通常同时是翻译输入）：

| 类 | 含义 | DirectGLES | DirectVulkan | 合计 | 占比 |
|---|---|---|---|---|---|
| **A** | 只为**探测变化**：读一个版本号，与 backend 自己已经镜像的值比较 | ~21 | ~14 | **~35** | 12% |
| **B** | **翻译输入**：每次都要读的值，backend 根本没有镜像 | ~88 | ~128 | **~216** | 74% |
| **C** | 瞬时 draw 参数 | ~2 | ~2 | ~4 | 1% |
| **D** | **身份 / 缓存键**（与 B 重叠计） | ~24 | ~24 | ~48 | — |
| **E** | 数据字节（经 `pGLContext` 本身的只有 XFB CPU 计数两处） | 1 | 2 | 3 | 1% |
| **写** | `RecordError` 6 + `InvalidateCompileEnv` 2 | 2 | 6 | 8 | 3% |

**这张表是整个设计里最重要的一张，它否定了两种直觉方案：**

- **"bump 一个版本让 server 自己拉"行不通。** 只有 12% 是 A 类。74% 是 B 类：值本身必须过去。一个只传失效信号的接口会在 IPC 上退化成今天的 pull 模型，而在 monolith 里退化成今天的样子加一层间接。
- **两个 backend 想要的推送粒度不同，但可以被同一个接口满足。** Espryt 持有逐字节 `RenderStateParameters` 镜像并做三段 memcmp（A 类带 B 类回退）；Magma **没有任何镜像**，它按 `GetPipelineStateVersion()` 缓存一个**值哈希**（`VulkanRenderer.cpp:4982-4993`），然后在 payload 构建器里把 ~40 个字段再读一遍（`:5155-5200`）——纯 B 类。**整块 blob 同时满足两者**：Espryt 的 memcmp 继续工作（它拿到的还是那个结构体的引用），Magma 继续算它的哈希。这就是 D-B1 的第二个理由。

另一个角度的统计：1997 个前端 getter 调用点里，**89 个是纯版本/序号读（A 类）**——这些在推送模型里**根本不过线**，因为推送调用本身就是变化信号；**72 个是数据字节读（E 类）**，全部在 §5.7 处理；**38 个是 `GetLifetimeId()` 身份读（D 类）**，全部变成 handle。

### 2.4 pull 模型里 293 之外的 58 行：迁移机制必须显式处理的缺口

前三份候选设计都提出"机械 `sed`：`MG_State::pGLContext->` → 宏"，覆盖 293 处，**但都没有交代剩下的 58 行非箭头用法**。实测分类：

| 形态 | 数量 | 例子 | 处理 |
|---|---|---|---|
| `MOBILEGL_ASSERT(MG_State::pGLContext, ...)` 真值判定 | ~40 | `DirectVulkan.cpp:347-1108` 密集区、`UniformManager.cpp:811-1967` 9 处 | **直接删除**：MGPipe 下没有 `GLContext`，assert 的对象不存在；替换成对 `PipeInputs` 已填位的 assert（由 §6.2 的 poison mask 自动提供） |
| `if (MG_State::pGLContext)` 空守卫 | ~10 | `Managers.cpp:3608, 3737, 3808, 4663, 8678`、`BackendObject_DirectVulkan.cpp:388, 788` | 删除守卫，改为读对应的 `PipeInputs` 字段（永远有效） |
| `MG_State::pGLContext != nullptr ? A : B` 三元 | 3 | `Managers.cpp:7120, 7128, 7131`（patch 参数，位于 transpile 路径内） | 由 `set_patch_state` 覆盖，三元塌成直接读 |
| `MG_State::pGLContext.get()` 裸指针捕获 | 1 | `DirectGLES.cpp:146` | **`sed` 完全抓不到**，必须手改 |
| `!= nullptr` 条件 | 2 | `VulkanRenderer.cpp:11150, 12649` | 同空守卫 |
| 注释 | 1 | `VertexInputStateFactory.h:133` | 改写措辞 |

**因此：纯度门 grep 的是 `pGLContext`，不是 `pGLContext->`**，且 P1 的机械替换步骤必须把这 58 行列成显式清单逐条转换（见 §6.2 与 §11 的 P1 交付物）。

### 2.5 pull 模型为了弥补"没有接口"而付的代价：~550 行 per-draw 失效发现机制

这些代码存在的唯一理由是 backend 必须**自己发现**状态变了，而且它们每 draw 都跑：

| 机制 | 位置 | 行数 | 为什么存在 |
|---|---|---|---|
| `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` / `CurrentUnitBindingsEpoch` / `UnitTextureSyncEntry` / `PairingsIntact` + 8 个支撑全局 | `DirectGLES.cpp:1372-1489` | ~115 | `GetTextureBindGeneration()` 在**冗余重绑**时也 bump（26.2 在每次纹理单元切换前后重绑同一个 sampler），所以计数器不可信，必须走查 owner 相等性 |
| `TwinLookupMemo` ×3（4096+256+64 槽 ≈ 140KiB）+ `OwnerEquals` | `DirectGLES.cpp:62-131` | ~75 | 让 `SharedPtr` 身份查表在 draw path 上够快 |
| `g_fbSlotCache` + `GetFramebufferBindingSlotFast` | `DirectGLES.cpp:139-155` | ~17 | 注释：前端 getter 每次调用线性扫 slot 数组 |
| `StateBackendObjectRegistry::CollectGarbage` ×6 | `Managers.h:353-390` | ~40×1 | **没有任何东西告诉 backend 一个 GL 对象被删除了** |
| `m_convertedVertexStreams` 的 `SharedPtr<const BufferObject> sourcePin` | `VulkanRenderer.h:1124-1127` | — | **纯粹**为了防地址复用而持有一个强引用 |
| `UniformManager` 的 8 类占位 `TextureObject` 构造 | `UniformManager.cpp:161-181, 1416-1500, 1624-1634` | ~120 | 只是为了让"未绑定单元"复用 `SyncTextureAndGetDescriptor(ITextureObject&)` 这个签名 |
| `SetupDrawSnapshot` 的 `sampledContentSum`/`sampledParamsSum` 与 ~14 个探测字段 | `VulkanRenderer.h:975-1000`、`.cpp:6249-6250, 6478-6483` | — | 用**有损的版本号求和**代替"有没有变" |
| `g_broadcastMemo*` + fragColor 重推导 workaround | `DirectGLES.cpp:2669-2732` | ~60 | FBO sync 与 program sync 之间的排序 hazard |
| `VkTextureManager::PruneDeadTextures` 的 `WeakPtr::expired()` GC | `VkTextureManager.cpp:1694-1720` | — | 同 registry GC |

**这些全部在 MGPipe 下删除**，因为推送调用**就是**变化信号，`resource_destroy` **就是**删除信号，`{slot, gen}` **就是**身份。

### 2.6 21 个 D 类身份 memo：它们各自守什么，以及为什么 `{slot, gen}` 能等价替换

统一事实：**每一个进入 memo 键的版本计数器要么是回绕的 `Uint16`，要么根本不会被它真正害怕的那个 mutation bump。** `BindingSlot::m_version`（`MG_Util/Types.h:197`）、`FramebufferObject::m_objectVersion`（`:183`）、`SamplerObject::m_version`（`:551`）、`RenderStateParameters` 版本（`RenderState.h:522`）、`TextureObjectBase::m_textureParamsVersion`（`:203`）全部回绕。**身份比较是堵住回绕洞的那块补丁。** 完整的 21 条重键表在 §4.7；这里只点三条最有教育意义的：

- **D3 `UnitTextureSyncEntry` + `PairingsIntact`**（`DirectGLES.cpp:1441-1481`）：注释写明它存在是因为"一次不经过 bind generation 的 slot 交换（DSA by-name 模拟以前就会静默交换一个 slot）会让每个键都匹配，而借来的 slot 指向另一张纹理，replay 于是会**用纹理 B 的前端状态驱动纹理 A 的后端 twin**——用 B 的形状重新指定 A 的后端存储并毁掉 A 的内容"。**这是整份调研里最强的"支持推送接口"的论据**：这一整类 bug 只在"client 能改一个绑定而不移动任何计数器"时才存在。推送之后 server 从不重读 slot，它被告知 `set_sampler_views(...)`——前提是**每一条会改绑定的前端路径都发消息**。审计义务从"哪些读需要守卫"变成"哪些 mutator 必须发消息"，由 §10.3 的 verify 模式与 poison mask 强制。
- **D11 `VertexInputStateFactory::ComputeHash`**（`VertexInputStateFactory.cpp:38-49`）：注释是一份 postmortem——"地址会被分配器复用，所以一个删了又建的 buffer 会重现它；配上逐字节相同的属性布局就重现了**整个** content hash……一个已销毁 buffer 的 GPU 切片被绑给了它的后继者的 draw，这就是一次 transform feedback 捕获拿回一个死 VAO 的顶点数据（0,0,0,1……）的原因"。**所以 `gen` 必须被混进 server 侧的每一个 content hash，而不只是被比较。**
- **D18 `VkRenderPassManager::m_renderbufferResources` / `VkTextureManager::m_textureResources` 用节点式 `std::unordered_map` 而不是本项目开放寻址的 `UnorderedMap`**（postmortem 在 `VkRenderPassManager.h:375-397`）：因为调用方会跨后续查表缓存 `RenderbufferResource*`/`TextureResource*`，一次扩表搬迁曾让 `BlitFramebuffer` 静默停在"source image layout is undefined"。**这一条在重键表里被显式标为 UNCHANGED**，并进 review checklist——`ska` 的 erase-shift 让这个 hazard 更糟而不是成为历史，而"把容器优化回去"正是一次大重构最容易犯的错。

---

## 3. 与方案 A（replica `GLContext`）的逐项对比

> 方案 A = `../MobileGL-disagg/docs/Disaggregated/PLAN.md`（feat/disaggregated@8b31de2f）。阶段天数逐项相加 = **77 天**。

| 维度 | 方案 A（replica） | 方案 B（MGPipe） | 判定 |
|---|---|---|---|
| **边界清晰度** | 边界**就是** replica：server 侧跑一份真 `GLContext`，backend 的 293 次拉取原样成立。没有写下来的契约，也无法写——"边界"等于"整个 `MG_State` 的语义"。新增 backend 必须先学会 186 个前端 getter 与 17 个 mutator 族 | 一份显式函数表（~72 项）+ 一份 POD payload 表 + `PipeCalls.def` 单一真相源。新增 backend 只实现两张表。`MG_Backend` 的 `MG_State` include 从 50 行 / 18 个头文件收缩到一张共享**值**头白名单 | **B 完胜**，这正是用户提出的目标 |
| **状态副本** | 一份完整 `GLContext` 对象图 + 每个 <16MiB buffer 一份 `PipeResource` + 每个纹理 level 一份 `MipmapStorage` + server 侧 `MG_State`/`MG_Impl`/`MG_Util`（含 glslang ~43MB 文本页） | **零副本**。server 侧只有 slot 数组里的 POD 记录 + backend 自己本来就有的 twin/ring/pool | **B 完胜** |
| **CPU 工作量** | `PLAN.md` §10 自承："这套遍历**每 draw 跑两次**"——client 的 `WireMirror` 一次、server 未改动的 `PrepareForDraw` 一次，外加编解码。最坏情形 `CurrentUnitBindingsEpoch` 在两侧各退化成对每个 touched unit 的 owner 全走查。全部性能主张押在"两半落在两个都快的核上" | 遍历**搬走**而不是翻倍：client 做一次 dirty word 比较 + N 次 `set_*`（稳态 N=1-3，且 CSO 内容寻址让重复状态 N=0），server 侧删掉 ~550 行失效发现机制。**但这是主张，不是测量**——树里没有任何 per-frame 字节或调用度量，所以 P0 落计数器、P2 出第一个数字 | **B 理论上更好，未证实**。两者都必须以逐线程 CPU 时间为准，不是墙钟帧时 |
| **内存** | `PLAN.md` R14 自估 **可达 ~450MiB 新增**。缓解措施是"优先推进 §6.4 方案 B（replica 采纳 client shadow）"——一个被它自己标为"激进、需额外设计"并推迟到 P6 的可选项 | 传输段 ~48MiB + POD 记录 + 可选 ≤32MiB 纹素保留 LRU ≈ **+50-60MiB**。无需任何额外设计 | **B 完胜**。在一个头条战果是"省 400MB"、有 LMK 屠杀记忆的项目里，这是最难辩护的一条 |
| **Roundtrip** | 稳态零（除回读/阻塞 query/分配 ack/present credit） | 稳态零（同上），**外加**一个新类：server 发起的纹理重铸拉取。三条缓解 + 专门的门 + 逐用例计数器（§7.5、§9.3） | **A 略优**，但差距被缓解措施压到"实测发布"而非"声称为零" |
| **改造量** | backend **一行不改**；改的是 `MG_Backend/Init.cpp:48-70` 一个分支 + 一份 `WireMirror` + 一份 applier + 两个代码生成器 | backend 改 293 个读点 + 58 行非箭头用法 + 95 个写回点 + ~66 个 memo 族重键 + 两处 `MG_State` 类型内部用法重写（Magma 的 8 类占位纹理、两个内部 shader 的 `ShaderObject`/`ProgramObject`） | **A 完胜** |
| **迁移期风险** | 风险**集中在末端**且**难以测试**：replica 的行为漂移编译通过、多数内容渲染正确，守卫只看签名。`PLAN.md` 自己的 R1 承认症状是"错误像素或错误查询结果，不是崩溃" | 风险**分布在 ~13 个阶段**，每阶段可二分、有现成测试套件作门、可用 `MOBILEGL_PIPE_PUSH` 位图在同一份二进制里 A/B 回退，且有**逐 draw 逐字段的语义比对**（两套模型活在同一地址空间才可能）。但它**改动 monolith**，因此能回归出货产品 | **B 的正确性风险更低，A 的产品风险更低**。这是本对比里唯一真正的取舍 |
| **到首帧时间** | **~第 15 天**（P0 5 + P1a 6 + P1b 4）跨进程首帧 | **~第 68 天** inproc 首帧、**~第 73 天** 跨进程首帧，且那一帧是**缩减路径**（emulation 在 P8 之前于 split 下 Fatal）；全功能在 P8 之后。最早可见里程碑是**第 16 天**的 verify harness 全绿（零产品风险，但不是一帧画面） | **A 完胜（4-5 倍）** |
| **长期价值** | 拆分达成；monolith 不变；边界仍未定义（replica **就是**边界）。维护成本**永久**：一份手写状态模型与 `MG_State` 长期锁步 | 边界被写下来、被生成、被测试。第三个 backend、shader 缓存服务、record/replay 层、校验层、真正的第二个 context 都变得可行。成本**一次性** | **B 完胜** |
| **对 monolith 的收益** | 零（按设计如此，这是它最强的性质） | ~550 行 per-draw 失效机制删除；复用地址 ABA 一类不可表达；FBO→program 排序 hazard 消失（`DirectGLES.cpp:2712-2732` 的 workaround 删除）；`pDefaultFramebufferInfo` 分层倒置消失；`inproc` = 渲染线程杠杆；顺带修两个潜伏 bug（`m_xfbCounterSlotByObject` 用裸 GL name 做键、`VulkanRenderer.cpp:11136-11146`；`RenderbufferObject` 缺 `GetLifetimeId()`）；顺带暴露一个死能力（`FramebufferSrgb`/`DepthClamp` 无存储，`RenderState.cpp:380/428-429`，6 个 backend 读点恒为 false） | **B 完胜** |

### 3.1 方案 A 里被证明**不可能**、而不只是"不理想"的两件事

1. **`RecProgramLinkOp`（server 从源码重新 link）**。`ProgramObject.h:11 → ShaderObject.h:12 → ShaderCompileTask.h`，`ShaderObject.h:146` 返回 `SharedPtr<glslang::TShader>`，`ProgramObject.h:14 → SpvcSession.h`。链接真 `ProgramObject` 就链接 glslang。方案 A 里它成立（server 反正链接完整 `MG_State`），方案 B 里它不成立，所以 `ProgramPublish` 第一天上、`MOBILEGL_IPC_PROGRAM=publish|relink` 开关消失、阶段 P5 整个消失（6 天回收），而它的副产品（DirectVulkan 内部 shader 烘焙）**升级为 P7 的前置条件**，`nm -D | grep glslang` 为空成为该阶段的验收判据。
2. **方案 A 的字节一致门在方案 B 里不成立**（D-B5）。这不是方案 B 的缺陷论证，是它必须公开承认的成本。

### 3.2 方案 B 复用方案 A 的比例

`PLAN.md` 的 §6（数据面）、§7（控制面）、§8（roundtrip 机制）、§9（present 与帧节奏）、§10（线程模型）、§11（EGL/窗口/进程生命周期）、§12 的第 1-3 层（编译期折叠、唯一 hook 点、allocator 包裹）、§12.4（运行时选择）、§13（构建布局）、§14（对 `Feat/CS-Delta-IPC` 的复用清单）**按体量算是全文的大部分，而且与状态模型无关**。方案 B 原样继承它们，逐条对照见 §8 与 §14。**因此"选 B 不选 A"并不浪费传输侧的设计投资**——那部分投资两条路都要付，且已经过对抗性评审。

### 3.3 一句话决策规则

- 目标是**这个季度出一个能跑的拆分**，或者拆分的价值主要按"进程隔离/崩溃隔离"计算 → **选方案 A**。
- 目标是**用户提出的那个架构**（backend 拥有自己的状态机 + 统一接口解耦） → **选方案 B**，并按 §0.6 的 16 天对冲路径起步，第 24 天用真数字做 GO/NO-GO。
- **不要**试图先做 A 再做 B。A 的 replica 一旦上线，它就成为"边界"的既成事实，而 B 的第一步（backend 停止读 `pGLContext`）会同时作废 A 的全部 applier 代码——两条路的 backend 侧改造是互斥的，共享的只有传输层。
