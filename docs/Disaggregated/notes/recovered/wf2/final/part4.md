## 11. 分阶段实施计划

> **通用纪律（每个 commit 都适用）**：默认 ALL target 必须能完整构建；禁止提交热路径插桩；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门（其 Vulkan 缺 `vkCreateHeadlessSurfaceEXT`，占该机 567 个基线集成失败中的 423 个）；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议（大核 1.96 / 小核 1.55GHz，GPU 拉满，40°C 门槛）；**每个阶段的出口都跑一次 §10.3 的五部分门**；**每个阶段的性能判据都是逐线程 CPU 时间**，不是墙钟帧时。
> **两条跑道**：P0-P4a、P3b/P4b、P7、P8、P13 是 **monolith 跑道**，每一段都可独立交付、可随时中止且 monolith 严格好于起点；P5、P6、P9-P12 是 **IPC 跑道**，整段继承 `PLAN.md` §6-§13。
> **v2 排期修订说明**：v1 的阶段天数与它自己的 §6.4/§6.5 逐子系统表互相矛盾（例如 P3a 给 12 天，而它包含的三行合计 22-29 天，等于"再基线检查点"按构造必然触发；P7 报 48 天下界而同口径是 85-111）。**本节的每个天数都是它所含 §6.4/§6.5 行的求和**，算术在 §11.5 公布。

### P0 — 卫生、度量、门与骨架（9-11 天）

**交付物**
- **清工作树 per-draw `fprintf`**：`DirectGLES.cpp:290-303`、`:640-663`、`Managers.cpp:875-877`（后者在 `pendingMutex` 临界区内）。CI 加 grep 门禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`。
- **`TracyPlot` 逐帧计数器，装在边界两侧**，**字节类**：`cmd-records`、`cmd-bytes-per-draw`（**直方图**，`SEG_CMD` 的定尺依据）、`stage-buffer`、`stage-texture`、`stage-vertex-client`、`stage-index-client`、`stage-ubo-global`、`stage-ubo-named`、`persistent-map-push`、`server-ring`、`server-staging`、`residual-value-block`、`index-mirror-bytes`、`index-bytes-shipped`、`texture-pull`；**调用类（v2 新增，`PLAN.md` 与 v1 都没有）**：每 draw 实际执行的 accessor 次数、每个 memo 门（`SyncRenderState` 早退、`SyncNeccessaryTextures` 键比较、`CurrentUnitBindingsEpoch` 快门、`TrySetupDrawFastPath`、pipeline memo、`ApplyDynamicDrawStateTail`）的命中/未命中、`resource_subdata` 发射次数与上传作业数。**没有调用类计数器，P2 的判据仍然是猜**（§2.3.1）。两台设备取基线。
- `MG_Pipe/PipeCalls.def` + `MGPipeTypes.h` + `MGPipeHandles.h` + `MGPipeCallbacks.h`：**完整调用目录，即使暂未实现的条目也占位**（记录编号绝不 churn）。
- `scripts/gen_pipe.py` 与七个生成器 G1-G7 的骨架 + CI `pipe-gen-check`（重生成 + `git diff --exit-code`）。
- `scripts/gen_pipe_dirty_surface.py` 骨架（推论 4）与 CI 接线。
- **`scripts/check_doc_citations.py`**（v2 新增）：`docs/**` 里每个 `file:line` 必须在基线提交上解析到存在的行。**v1 有一批 `SamplerObject.h` 引用指向 160 行文件的 468-551 行**；本文件已修正，lint 防止再犯。
- `MOBILEGL_PIPE_PUSH` / `_VERIFY` / `_STATS` / `_LEGACY_MEMOS` / `_TEXEL_RETAIN_MB` / `_INDEX_MIRROR_MB` 在 `ConfigLoader.cpp` 与既有开关并列解析。
- **三个严格 no-op 的免费收益**：`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 的纯前端 case 移回 `MG_Impl`（Espryt 14 / Magma ~10 个读点）；`RenderbufferObject::GetLifetimeId()`（**不加 `GetVersion()`**）；D21 重键——**这一条是潜伏 bug 修复，先独立落 `dev`**。
- 回答两个阻塞问题：`FramebufferSrgb`/`DepthClamp` 无存储是潜伏 bug 还是有意为之（§10.4-6，**必须在渲染状态 chunk 表冻结之前**）；**语料里是否存在 `glRenderbufferStorage` 的 OOM 探测惯用法**（决定 `kNeedsAck` 要不要标它，§7.4）。
- `MG_Remote/{Protocol,Transport}` 骨架与 `PLAN.md` P0 完全一致（**`SCM_RIGHTS` 第一优先**）；`protocol.fbs` + 提交的 `protocol_generated.h` + `flatc-check`；`MG_Test/Wire/`。
- **`PLAN.md` P0 的两个 spike 原样跑**：spike A（Android 交付链）；**spike B（external memory 导出，两台设备）**。

**验收**：`AdvertisedLimitsScenario`（6 个测试）绿；367 集成 × 2 backend + 428 单元逐名不变；40 个 trace 全绿；两台设备的基线**字节、调用、逐线程 CPU** 数字记录在案；spike A/B 出结论（spike B 直接决定 P11 规模）；citation lint 全绿。

### P0.5 — 值头与制品头抽取（6-9 天）★v2 新增，**P1 与 P7 的硬前置**

**交付物**
- **`MG_Pipe/MGPipeValueTypes.h`**：把 `MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint` 与相关枚举搬进来，**它不 include `MG_State/GLState` 的任何东西**；`RenderState.h` / `SamplerObject.h` / `VertexArrayObject.h` 反过来 include 它。
  **必须做的理由**：`RenderState.h:12` include `FramebufferState/FramebufferObject.h`，后者 `:12-13` 再 include `TextureObject.h` 与 `RenderbufferObject.h`；`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 给两个数组定长（`:263, 273`）。所以 v1 的"共享值头白名单"不是叶子集，把它交给"纯净的 `MG_Backend`"会拖进整张类图，而 `nm --undefined-only` 看不见（只 include 不调用不产生未定义符号）。
- **`MG_State/GLState/ProgramState/ProgramArtifacts.h`**：把 `TypeFacts`（`ProgramObject.h:44`）、`ResourceReflection`（`:76`）、`XfbVarying`（`:1146`）、`LinkArtifacts`（`:1210`）、`SpirvArtifacts`（`:1409`）抽出来，**不 include `ShaderObject.h`、不 include `SpvcSession.h`**；更新 7 个 includer（`ProgramFactory.h`、`UniformManager.cpp`、`VulkanRenderer.cpp`、`ProgramInterface.cpp`、`ProgramLinkTask.h`、`ProgramObject.h`、`ProgramTranslationCache.h`）。
  **必须做的理由**：server 要**反序列化进**这五个类型就必须有它们的定义，而它们今天住在会拖进 glslang（`ShaderObject.h:12` → `ShaderCompileTask.h`；`:146` 返回 `SharedPtr<glslang::TShader>`）与 spirv_reflect（`ProgramObject.h:14` → `SpvcSession.h`）的头里。**没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。**
- **CI include 闭包断言**：`MGPipeValueTypes.h` 的 `-H` 闭包里没有 `MG_State/GLState/`；`ProgramArtifacts.h` 的闭包里没有 glslang / SPIRV-Cross / spirv_reflect 任何头。
- `ProgramArtifacts.h` 的 `Visit()` 归档 + `sizeof` 绊线（§4.5.5）。

**验收**：全套现有测试逐名不变（这是一次纯搬移）；两条 include 闭包断言绿，且**人为把一个 `MG_State` include 加回 `MGPipeValueTypes.h` 能让它变红**；`nm --defined-only` 与 `.text` 变化可逐符号归因（搬移会改变某些内联决策，允许，但要解释）。

### P1 — `PipeInputs` 替换与 verify harness（10-13 天）

**交付物**
- `MG_Backend/MGPipe/PipeInputs.h`：每个 backend 真正用到的 `GLContext` 方法一个访问器（Espryt 32 / Magma 55），**字段类型与今天读到的完全一致**，按 memo 键组织。
- 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加逐条手工转换 58 行非箭头用法**（§2.4：~34 处 `MOBILEGL_ASSERT` 真值判定删除、7 处空守卫改直读、3 处 patch 三元、`DirectGLES.cpp:146` 的 `.get()` 裸指针捕获与 `:142` 的 `decltype` 别名、14 处 `!= nullptr`、1 处注释）。**这份 58 行清单是本阶段的显式交付物。**
- **逐 verb 类填充点**（v2 修正，§6.2.1）：G5 从 `PipeCalls.def` 生成"每个 `kCtxVerb`/`kCtxObject` 调用可能读哪些 `PipeInputs` 字段"的表，并在 `MG_Impl` 的 ~93 个边界站点上生成对应的 validate/fill 调用。**不是只在 `PrepareForDraw`/`SetupDraw` 两处**——`MG_Impl` 用到的 70 个表项里 ~48 个不是 draw/dispatch，其中多个自己就读 `pGLContext`（`UpdateTextureBindingAtTarget` `:6051-6052`、`PackStateFromContext` `:6129`、`Clear` `:4106/:4165`、`BlitFramebuffer` `:5988-5989`、`GetTexImage` `:9254-9257`、DSA by-name `:4038-4043`、`:7417-7418`），而 `:1501-1502` 的注释已经点明"for every non-draw call site (Clear, readbacks)"。
- **G5 的逐 verb 世代 poison**：`m_filledGen[f] == m_currentVerbSerial`（非 sticky 字段）；debug 与 disaggregated 构建里读陈旧/未填字段 = `Fatal{UnmigratedPipeInput, "<field>@<verb>"}`。
- **G4 的 `MOBILEGL_PIPE_VERIFY=1` 逐字段影子比对器** + 第三种 CI 模式接线。
- **20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表**（§7.2、§5.8.1），作为文档交付物。

**验收（v2 修正）**
- **`nm --defined-only` 在 pull 构建里不变；`.text` size 变化必须能逐行归因。** v1 要求"完全一致"，但本阶段自己的交付物里就有 ~24 处会生成代码的转换（7 处 `if (pGLContext)` 空守卫、14 处 `!= nullptr`、3 处三元）——只有 ~34 处 `MOBILEGL_ASSERT` 是真免费（`Defines.h:114` 在非 debug 下宏为空）。此外 `SnapshotFromGLContext` 与 G4/G5 机制必须包在 `#if MOBILEGL_PIPE_PUSH/_VERIFY/DEBUG` 里，pull 构建才不多出调用。**把空守卫与三元的重写推迟到 P2**（那时字段确实永远有效），本阶段只做 assert 删除与 `sed`，则 `.text` 差异可压到零附近。
- 全部 40 个 trace 与 367 个集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；
- **故意损坏一个快照字段能让 verify 门变红**；
- **故意在某个非 draw verb（`glGenerateMipmap`）的填充表里漏一个字段，能在那条 verb 上触发 poison Fatal**——不是在某个后续 draw 上。

**★ 第 25 天（低端估计）— 最早可见里程碑：**零产品风险地证明"推送等价于拉取"，逐 draw 逐字段。**这不是 GO/NO-GO**（它没有性能数字，也没有 Track H 单位成本）。

### P2 — 值推送：渲染状态 CSO（双后端）+ 第一片 Track H + 残余值块（18-26 天）

**交付物**
- `MG_Impl/Pipe/Tracker.{h,cpp}`：dirty 位（§5.2，值类用既有计数器、**对象类新增 5 个聚合世代**）+ §5.3 的不变式 + §5.4-4 的集合 hash 抑制器骨架。
- **`MG_State` 的 5 个聚合世代**（`TextureState` 两个、`BufferState`、`VertexArrayState`、`FramebufferState` 各一，合计约 20 行）+ `gen_pipe_dirty_surface.py` 的首轮映射与 CI 接线。
- `MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` + **G7**：pipeline/dynamic chunk 表（从 `VulkanRenderer.cpp:4826-4906` 原样搬来）+ **遍历每个 `RenderState` public setter 断言 `pipelineSubsetHash 变 ⟺ m_pipelineStateVersion 变` 的测试**。
- `MG_Impl/Pipe/CsoCache`：64 项 LRU，键是 **pipeline 子集**的 xxHash（**不是整块**，D-B1 v2）。
- `create_render_state` / `bind_render_state` / **`set_dynamic_state`**：Espryt 侧 `RenderStateImpl` 的 693 行函数体、单 `Uint16` 早退、三段 memcmp、`g_syncedColorMaskAlphaWidenMask`、dual-source decline **一行不动**（消除 4 个读点）；Magma 侧 `ComputePipelineStateHash` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取（消除 ~55 个读点）。两个版本号都过线。
- `set_pixel_pack_state`（PACK only）、`set_patch_state`、`set_vertex_attrib_defaults`；P1 推迟的空守卫/三元重写。
- **`set_residual_value_state` + `ResidualValueBlock`**（§6.3）：`static_assert(sizeof == MGL_RESIDUAL_BLOCK_SIZE)`（逐阶段**下调**）+ **逐成员 `offsetof` 断言** + split 下逐字段序列化。
- **第一片 Track H（v2 新增，让 GO/NO-GO 测的是它要决定的事）**：Espryt 子系统 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / 2 个 GC 扫描）与 Magma 子系统 4（`VertexInputStateFactory` / `VaoDrawMemo` 重键，**删掉写进前端 VAO 的后端堆裸指针**）。
- **`MOBILEGL_PIPE_LEGACY_MEMOS`** 编译期开关（§6.7）：让前两波 handle 化保留一个**真正的**旧-vs-新臂。

**验收**
- 367 集成 × 2 backend × 2 模式（pull / push）逐名相同；40 个 trace 在 monolith-push 下 SSIM ≥ 0.99，双后端；`ClipDistance`、`SampleMaskScope`、`SampleVariables`、`DualSourceBlend`、`ViewportArray`、`PrimitiveRestart` 场景绿；verify 模式零分歧；
- **`HandleRecycleScenario` 绿，且它在 0b 重键之前必须是红的**；
- **G7 的 setter 一致性测试绿，且人为把一个字段从 pipeline chunk 表里拿掉能让它变红**；
- **两台设备 reboot-clean 配对**：monolith-push 在 p50 与 p99 逐线程 CPU 上落在 monolith-pull 噪声内或更好，**并且 tracker 每 draw 的绝对 ns 落在预设上限内**（相对阈值不够，§10.3-④a）；
- **Blaze3D blend-toggle 微基准**（enable/draw/disable/draw，MC batch 速率）单列发布；
- **负面对照**：关掉 CSO 内容寻址重跑，把"推送更慢"与"CSO 设计更慢"分开。

**★ 第 43 天（低端估计）— GO/NO-GO 决策点。** 此刻手上有：verify harness、双后端已推送的渲染状态、真实 CPU 增量与绝对 ns、Blaze3D 微基准、CSO 负面对照、**Track H 在两个 backend 的最便宜子系统上的实测单位成本**。
**退回成本（诚实版）**：P0 与 P0.5 对方案 A 也有用（后者同样要序列化反射），真正只为方案 B 花的是 **P1 + P2 ≈ 28-39 天**。**若 CPU 数字为负、或 Track H 单位成本超估计 50%，退回方案 A 损失 28-39 天。**

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

**交付物**：`MG_Remote/Client` 的发射表实现 `MGPipeScreen`/`MGPipeContext`；`Server/PipeApplier.cpp`；`ServerLoop`（`mgl-srv-io` + `mgl-srv-apply`，后者终身持有原生 context）；单一 hook 点 `MG_Backend/Init.cpp:48-70` 装 `BackendObject_Remote`；`MGPCaps` 快照；一条阻塞 `read_pixels`；client 侧保守 `MarkGpuWritten` 与 `emitSeq`；**client 侧块粒度 persistent-map 推送**（T2 档下强制）；`InProcessTransport`。

**v2 规范条款：`InProcessTransport` 必须走与 spawn **完全相同**的 G3 编解码路径**，只在门铃/拷贝机制上不同。否则第 99 天的里程碑证明不了 wire 完整性，而 P6（第 104 天）才在关键路径上发现缺口。**`PipeApplier` 里加一条 debug 断言：任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界。**

**验收**：`ctest -R 'DirectGLES\.Split\..*(ClearThenReadPixels|Triangle)'` 在 `MOBILEGL_TRANSPORT=inproc` 下绿；**OpenRA trace 在 split 模式下 SSIM ≥ 0.99**；**`PersistentCoherentMapScenario` 绿**；**两个角色的峰值 RSS 记录在案**，作为对 `PLAN.md` R14 的基线；`persistent-map-push` 字节量出数；任何未迁移的 `PipeInputs` 字段读产生 `Fatal{UnmigratedPipeInput}`。
**★ 第 99 天 — 首个 IPC 帧（`inproc`）。诚实标注：这是缩减路径**——client 数组、indirect-count 解析、索引宿主镜像在 split 下仍是 Fatal，全功能要等 P8。

### P6 — spawn transport（5 天）

**交付物**：`SocketTransport`（socketpair + fork/execve，**显式 envp 剔除 + `mobilegl_server_main` 内强制 Monolith 的双保险**）；`ServerMain`；`MOBILEGL_IPC_SERVER_PATH` 为主 + `dladdr` 兜底；就绪握手有界重试；client EOF 即时退出；server 死亡的 device-lost latch；trace-replay 的 `SPLIT` 后缀与 `-DTRACE_TRANSPORT=` 接线。

**验收**：P5 全部测试在 `MOBILEGL_TRANSPORT=spawn` 下绿；fork 链测试断言进程树只多一个子进程；`HeadlessGL` 的 fork 预检交互测试无孤儿 server；`run_android_retrace_local.py --case OpenRA --backend DirectGLES` 在 `35d0befa` 上 SSIM ≥ 0.99。
**★ 第 104 天 — 首个跨进程帧（缩减路径）。**

### P3b / P4b — 深化（Espryt）：memo 重键、dirty 反转、跨步描述符、XFB scatter、回读（29-38 天）

**交付物**：重键 `ResolvedDrawBuffers`、`PendingAttribValueMask`、`ConvertedFloat64Stream`、`SyncCurrentFBO` 四元组戳、`ResolvedTextureBindingMemo`、`SamplerPassMemo`、image sweep、program registry 到 `{slot, gen}`；**server 侧删** `g_unitTextureSyncList`、`g_fboTextureSyncList`、`g_unitSamplerLookupMemos`、`g_imageSweep*`、`DirectGLES.cpp:1372-1489` 的 ~115 行 unit-bindings epoch 推导，**同时在 `MG_Impl/Pipe/Tracker.cpp` 落地对应的集合 hash 抑制器**（§2.5、§5.4-4）；**dirty 归属反转**（§7.3，client 保 rect 模型与**按存储属主键控**的发射游标、发射后自清）；**`MGPSubRegion` 跨步描述符改造**（§4.5.6：`Managers.cpp:4274-4326` 从描述符取步长，替代 `uploadData == mipData` 指针比较与整 level 步长算术）；**XFB scatter 搬到 client**（§7.2.1）；**删** fragColor 重推导 workaround 与 `g_broadcastMemo*`；用推送状态退役 9 条陈旧性判定里的第 4-6、8-9 条；Espryt 的 raw-depth-fetch `SamplerObject` 原生化；回读 / pack state。

**验收**：~25 个纹理场景（`TextureView`、`LayeredTextureReadback`、`ImageSizeAfterRespec`、`FormatlessImageBake`、`NonCoreImageFormat`、`ImageFormatQualifier`、`ImageTargetKind`、`ImageLoadStoreSso`、`UnboundImageDescriptor`、`SwizzleAccessRoutine`、`IntegerBorderColor`、`PixelStoreSweep`、`SampledSetStaleness`、`ThreeChannelAttachment`、`BufferTexture`、`CopyImage*`×3、`ClearTexImageUndefinedLevelZero`、`DepthStencilReadback`×3、`PackedWordReadback`）；21 个 program 场景 + 整个 `MG_Test/ShaderTranspiler` 目录；两台设备上完整 `KHR-GL46.texture_*` / `internalformat.texture2d.*` / `shader_image_*` / `packed_pixels` 块，conformance 在 pull 基线 0.5pp 内；**每一个 Iris trace**；
**v2 新增三个门**：
- **`TextureUploadShapeScenario`**：逐纹理逐帧的上传形状（box vs N region、作业数）录金标比对——**+6ms 悬崖由形状相等把关，SSIM 对它不敏感**；**Mali 上帧时增量必须发布**；
- **view/owner 发射游标别名场景**：通过 view 上传、经属主采样（以及反向），跨 draw 边界各一次（§7.3 修正 1）；
- **verify 保留模式**：`MOBILEGL_PIPE_VERIFY=1` 下 `resource_subdata` 的 `(unionBox, regionCount, regions[])` 与快照重算逐项相等（§7.3 修正 2）；
- `XfbAfterClipDistance` / `XfbCaptureBufferReuse` / `XfbRepeatedCapture` / `TessellationXfbCapture` 与 **`KHR-GL46.transform_feedback.capture_special_interleaved_test`**（scatter 的 `gl_SkipComponents` 空洞保留，§7.2.1）。

### P7 — DirectVulkan（Magma）全量迁移（80-104 天，可与 P5/P6/P8 并行）

> 子系统 1（pipeline+动态状态）与子系统 4（VertexInput/VaoDrawMemo）已在 P2 交付，所以是 §6.5 的 85-111 减去 5-7。

**交付物**：§6.5 的其余 10 个子系统，重点四项：`SetupDrawSnapshot` 的 ~14 个探测字段（含两个**有损**的版本求和）塌成 dirty mask 比较；**`UniformManager` 的 8 类占位 `TextureObject` 换成原生 `VkImage`+view+descriptor**（~120 行删除，34 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个消失）；**具名 UBO 的 host payload**（D-B8：`ResolveUniformBufferPayload` `UniformManager.cpp:2022/2052` 改从 `set_shader_buffers` 的 `MGHostSpan` 取，`kCapNeedsHostUboBytes` 门控）；**blit / depth-mipmap 内部 shader 烘焙成签进树的 SPIR-V + uniform location + UBO 布局，由一个 `MG_Test` 重跑树内 glslang 逐字节比对的用例守新鲜度**；`VertexInputStateFactory` 的后端堆裸指针写回**直接删除**；`VkRenderPassManager` / `VkTextureManager` 的**节点式容器纪律原样保留**（D18，postmortem 注释逐字带进 review checklist）。

**验收**：367 集成 + 40 trace 在 DirectVulkan 的 monolith-push 与 split 下全绿；verify 零分歧；**`nm -D libMobileGLServer.so | grep glslang` 为空**——这是整个论点的强制执行点（**依赖 P0.5**）；`UnboundImageDescriptor`、`SampleMaskScope`、`ImageLoadStoreSso`、`AtomicCounter`、`SsboArrayDynamicIndex`、`NonCoreImageFormat`、`Orientation`、`DepthStencilReadback*` 场景；**Iris trace 上 `stage-ubo-named` 逐帧字节量发布**（D-B8 的定尺依据）；两台设备 CTS 在 0.5pp 内。
**⚠ 再基线检查点 2：P7 中点（第 40-52 个工作日）若已完成子系统 < 40%，立即重定基线**——P3a 的检查点发现不了 Magma 特有的超期，而 P7 在单跑道下位于关键路径。

### P8 — emulation 下放 + 索引宿主镜像 + 协议广度（12-16 天）

**交付物**：`MG_Impl/Pipe/HostResolve.cpp`——client 数组范围计算、**最大索引扫描**（`TryComputeMaxIndexFromHostBytes` 移到 client，唯一的无界应用指针读）、**`*IndirectCount` 计数解析**，每一条前面都有 §5.8.1 **逐站点表**规定的 reconcile（**不是笼统的 publish/wait/drain**：`*IndirectCount` 只做 `SyncPersistentMappedRange()`，因为 monolith 也只做这一个，`DirectGLES.cpp:4666-4667`）；`MGHostSpan` 的 split 填法；**`Server/IndexHostMirror`**（D-B7：`bindMask & ELEMENT_ARRAY` 的资源由 subdata 流增量维护，`MOBILEGL_PIPE_INDEX_MIRROR_MB` 预算，超预算退化为逐 draw 传送并计数）；**CopyImage shadow 镜像搬到 client**；`draw_vbo(info, indirect, ranges[], numDraws)` 收编 multi-draw 族（**分档仍在 server**）；viewport-array 回放验证在一次 pipe 调用驱动下各遍之间观察到的状态与今天一致（`EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`，`DirectGLES.cpp:3841`）；`generate_mipmap` 返回 level 计划（**形状，不带字节**）与 CPU 回退的纹素；**G3 的"单条记录大于段容量"分块/降级路径**。

**验收**：`ctest -L integration-gpu -R '^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` **逐名相同**，DirectVulkan 同；40 个 trace 在 split 下双后端 SSIM ≥ 0.99，含两个 `coherent_as_flush: true` 的 Create fixture（**两种模式都开着该开关跑**）；**新增 `ClientArrayAfterComputeWriteScenario` 绿，且去掉那次等待必须能看到几何缺失**；**`create-indirect` fixture 上 `roundtrips-per-frame` 读零**（§5.8.1 的绊线：证明没有给 `*IndirectCount` 平白加一次 publish-and-wait）；**`index-mirror-bytes` 与 `index-bytes-shipped` 逐用例发布**；`MultiDraw`、`PrimitiveRestart`、`ViewportArray`、`DrawParameters`、`CopyImage*`×3、`GuiBatch` 场景。
**★ 第 145 天 — 全功能 split。**

### P9 — 反向通道（10 天）

**交付物**：`SEG_REPLY` 4KiB slot 池；阻塞 `read_pixels`；PBO 回读 fire-and-forget；`on_gpu_written{res, ranges}` 收窄（配 `writableMask`）；`on_buffer_writeback` **按操作级批处理**（今天两处逐行循环：`Utils.cpp:2342`、`DirectGLES.cpp:7633`）配 epoch bump 的排序规则；`on_xfb_scatter_ready` + client 侧 scatter（§7.2.1）；`on_texture_writeback`（一个生产者）；`on_mip_levels_generated`（**只带形状**）；**`on_texture_pull_request` 四条缓解全上 + `resource_subdata_complete` 终止符**（§7.5）；`on_gl_error` 有序 + **收窄后的** `kNeedsAck`（§7.4）；`on_caps_invalidated`；`on_surface_changed`；**`on_log` 按严重级分级**（≤WARN 有损 / ≥ERROR 无损 + 每秒速率限制器 + "N errors suppressed"）；`SEG_EVENT` 溢出策略 + 等待循环内排空。

**验收**：`DepthStencilReadback`×3、`PackedWordReadback`、`LayeredTextureReadback`、`ClearThenReadPixels`、`XfbAfterClipDistance`、`XfbCaptureBufferReuse`、`XfbRepeatedCapture`、`TessellationXfbCapture`、`KHR-GL46.transform_feedback.capture_special_interleaved_test` 在 split 下绿；**`TextureRemintPullScenario` 绿**，**且它必须包含一个"答不出来"的用例**（一张只被渲染过、随后被 image-bind 的纹理）**并在终止符落地前表现为 apply 线程挂死/超时**；**拉取计数逐 trace 用例发布**；故障注入：client 被 credit 阻塞时灌满 `SEG_EVENT`，两侧都必须恢复；**日志洪泛下注入一次 backend link 失败，那行 ERROR 必须出现**。

### P10 — sync / query / present 节奏（6 天）

**交付物**：client 铸造 sync 与 query handle；轮询入口成为门铃点 + `MOBILEGL_IPC_POLL_ESCALATE` 饥饿升级；**fence 完成度来自真的逐 fence 退休**（不是 present 水位——那正是 MC 1.21.5 native-heap OOM 的成因）；DirectGLES 的非 present fence tick；`Present` 严格 1:1；`MOBILEGL_IPC_PRESENT_CREDIT` 默认 1 + 叠加公式；逐帧 roundtrip 计数器与**输入延迟直方图**；`PLAN.md` §8 末尾的三个独立 `dev` monolith 修复。

**验收**：`XfbPrimitiveQuery`、`PrimitivesGeneratedNoXfb`、`AsyncCompile` 在 split 下绿；**40 个用例上 draw/state/upload 路径的 roundtrip 计数器读零**，条件渲染与阻塞 query 次数逐用例发布；零 timeout 轮询循环测试在有界时间退出；`bench.sh` 在 `35d0befa` 上配对 A/B：两侧都关采纳时 split 帧时在 monolith 10% 内，输入延迟直方图 p50/p99 记录在案。

### P11 — persistent map 与 ≥16MiB 采纳（8 天；spike B 全否则缩为 2 天）

**交付物**：由 P0 spike B 驱动的 POST 探针档位选择（T2 / T1 / T0）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`MOBILEGL_IPC_ADOPT_TIER` 覆盖开关做负面对照。

**验收**：`LargeArenaAdoptionScenario` 在所选档位下绿；`improved-transparency-minecraft-26.3` 与两个 Create fixture SSIM ≥ 0.99；**`StorageBufferRegrowScenario` 发布 `map-persistent-roundtrips`**（T1 档下每次存储定义一次，不是每 store 一次）；`35d0befa` 上配对 reboot-clean 的 p99 帧时与峰值 RSS 对 monolith 采纳基线（p99 163→21ms、40→115fps、~400MB）——**split 在所选档位下 p99 不得回归超过 10%；若 T2 成为永久答案，其实测代价必须写进文档**。

### P12 — Android 生产窗口路径（10 天）

**交付物**：`android:process=":mgl"` 的 Service 收 Java `Surface`（Binder）后 `ANativeWindow_fromSurface`（minSdk 26 无公开 `ANativeWindow` 扁平化；树内先例是 `android:process=":bench"` 的 `BenchService`）；server 生命周期绑 Activity；FCL 用户 env 与 plugin APK V2 开关表接线（**零新增管线**）。

**验收**：Minecraft 通过 FCL 在 spawn 模式下在 `35d0befa` 上双后端入世界；配对 reboot-clean bench + 输入延迟直方图；杀 server 产生干净的 device-lost latch；SIGKILL 故障注入。

### P13 — 退役 pull 路径（8-12 天）

**交付物**：删 `SnapshotFromGLContext()` 的**非 verify** 编译分支、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；**保留 `MOBILEGL_PIPE_VERIFY` 及其 `SnapshotFromGLContext()` 与 `MG_State` include**（D-B5）；**交付 MGPipe recorder 金标模式**（`MG_Test` mock backend → 录制器，§10.4-9），作为不依赖 `MG_State` 的长期语义门与开放问题 11 的答案；删 `set_residual_value_state` 与 `ResidualValueBlock`；`MG_Backend` 的 `MG_State` include 收缩到 `MGPipeValueTypes.h`；**在计数器活着的情况下重调所有幸存缓存的容量**（Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`）并把它们变成带 env 覆盖的调优参数；最终符号/尺寸/CPU 报告。

**验收**：**`static_assert(sizeof(ResidualValueBlock) == 0)` 编译通过**；**三道纯度门在非 verify 构建上转绿**（include 图门 A、符号门 B、未声明门 C，§10.3-①）；verify 构建仍能跑且零分歧；MGPipe recorder 金标在 40 个 trace 上建立并可回归；全套门（367 × 2 backend × {monolith, split}、428 单元、40 trace SSIM ≥ 0.99、两台设备 CTS 在 `81b17c0b` 基线 0.5pp 内）；**monolith 逐线程 CPU 在两台设备的 p50 与 p99 上不差于 P0 基线**——本设计的性能主张在这里成立或倒下。

### 11.5 总估时、里程碑与 CTS 周转

**逐阶段求和（低端 / 高端，单跑道累计）**

| 阶段 | 天 | 累计（低端） | 构成（§6.4/§6.5 的行） |
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
| **P7（Magma）** | **80-104** | **267** | §6.5 的 85-111 减去已在 P2 交付的子系统 1 与 4 |

**报作 267-337 人天**（不含 CTS 周转）。两个工程师、P7 与 P5/P6/P8 并行 → **约 7-9 个月**，真正的约束是两台设备的争用而不是人头。

**与独立成本分析的一致性**：一次独立的改造成本调研给出 backend 工作**单独** 202-266 天（Espryt 95-125 + Magma 85-111 + 共享 22-30）。本节的 267-337 = 那个区间 + IPC 跑道 51 天 + P0.5 的 6-9 天，**方向一致**。v1 报的 200-260（含 IPC）落在其乐观端之外，已作废。

**里程碑（低端估计）**：第 **25** 天 verify harness 全绿（零产品风险，**不是** GO/NO-GO）；第 **43** 天 **GO/NO-GO**（含一片真 Track H）；第 **99** 天首个 `inproc` IPC 帧（**缩减路径**）；第 **104** 天首个跨进程帧（**缩减路径**）；第 **145** 天全功能 split；第 **187 / 267** 天三道纯度门转绿。

**再基线检查点**：P3a > 27 天；P4a > 39 天；P7 中点（第 40-52 个工作日）完成子系统 < 40%。任一触发，先跑 `inproc` 的证伪数字再决定是否继续。

**CTS 周转必须单独计价，不折进阶段估时。** `gl44to46` caselist 约 56,271 例。分层门控：逐阶段只跑该阶段改动可能影响的具名 CTS 块（P4a 的 `packed_pixels`、P3b/P4b 的 `texture_*`/`shader_image_*`、P9 的 `transform_feedback*`），**完整 caselist 只在五个架构边界跑**（P0.5 头文件抽取、P3a handle、P4a framebuffer/纹理身份、P3b/P4b 纹理、P13 纯度）**以及每次合并 `dev` 之前**，且放在 CI 而不是关键路径上。设备锁协议照旧。若实测周转仍主导排期，**诚实做法是加宽估时而不是削弱门**。

---

## 12. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| **B-R1** | **效率是方案 A 的 3.5-4.4 倍、首帧晚 6-7 倍**（267-337 天 vs 77；第 104 天 vs 第 15 天）。排期驱动的评审可以只凭这一条否掉本方案 | 把价值排在承诺之前：P0-P2（43 天，其中 28-39 天是方案 B 独有）交付 handle 化 twin 与内容寻址的渲染状态 CSO——**零 IPC 风险的可测量 monolith 工作**——并产出字节/调用计数器与第一个逐线程 CPU 数字与 **Track H 单位成本**。**第 43 天显式 GO/NO-GO。** P13 是一个完全自洽、不含任何 IPC 的 monolith 交付物；P5 的 `inproc` 只要 12 天 |
| **B-R2** | **中心性能主张未经测量，且它的基线被 v1 高估了一个数量级。** 可达性遍历是**搬走**而不是消失；真实稳态拉取只有每 backend 每 draw 10-25 次 accessor（§2.3.1），不是 124/169 | 字节**与调用**计数器是 **P0 交付物**。每阶段验收用**逐线程 CPU 时间**，两台设备、reboot-clean、配对，**并设绝对 ns 上限**（相对噪声阈值在真实基线下会平凡通过）。P2 除渲染状态外**必须含一片 Track H**，否则测的不是要决定的事。加 Blaze3D blend-toggle 微基准与 CSO 内容寻址的负面对照。**先清工作树 per-draw `fprintf`** |
| **B-R3** | **monolith 字节一致门按构造死亡**，逐名集成基线也随之移动 | 五部分替代门，全部在 P0/P0.5/P1 落地（§10.3），其中 ② 逐 draw 逐字段影子比对在语义上严格强于任何符号 diff。两条字节等式仍作断言保留。**逐名功能基线明确定义为"P1 出口的重构后 monolith"**，而 P1 出口自己先用 verify 证明等价于 `81b17c0b`；`81b17c0b` 只作性能锚点 |
| **B-R4** | **server 发起的纹理拉取是新停顿类**，触发路径之一（整格式再生 `Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就会触发、无法被 hint 预防；**而且存在 client 根本答不出来的 level**（纯渲染产生 / `CanMirrorCopyImageShadow` 拒绝的 copy 目标 / GPU 生成的 mip），会让 apply 线程永久 park | 四条缓解同时上：`imageBindableHint` 预防主因；**异步** park-and-re-emit 让停顿落在 `mgl-srv-apply`；**`resource_subdata_complete` 终止符可携带零 region**，server 带着"已分配但为空"的存储继续（正是 monolith 的行为，`DirectGLES.cpp:6270-6271`）；保留 LRU **默认关闭**（`MipmapStorage` 保有完整 CPU 影子，所以拉取总能被服务，缓存买的是延迟不是正确性）。`TextureRemintPullScenario` **必须包含无解用例并在终止符前是红的**，**拉取计数逐 trace 用例发布** |
| **B-R5** | **P3b/P4b（29-38 天）与 P7 中的 `VkTextureManager` 是最大最险的段**，压在实测 +6ms/frame 悬崖（rect 列表 vs union box）与 7 条 fallback-repack 路径上，**而后者的可行性判定 `uploadData == mipData`（`Managers.cpp:4278-4283`）在 split 下不成立**——它要求上传源就是整 level shadow 并按整 level 步长跨步 | `resource_subdata` 同时带 box 与 region 列表、**server 选形状**；**`MGPSubRegion` 显式携带 `srcRowStride`/`srcSliceStride` 与 `sourceIsVerbatimLevelShadow`**，`Managers.cpp:4274-4326` 改为从描述符取步长（形状照抄已存在的 `UnpackStagingBlock`，`:4340-4390`，ring 路径本来就紧密重打包）。**这项工作计入子系统 5 的天数**（+3-4 天），不再列为"原地不动"。**`TextureUploadShapeScenario` 录金标比对上传形状与作业数**，因为 SSIM 对这个悬崖完全不敏感。P3b/P4b 拆成两个可独立落地的半 |
| **B-R6** | **tracker 完整性**：推送之后 server 不能再重读活状态校验快路径。任何 tracker 忘记发的 mutator 会静默漂移。历史上最危险的正是这个形状（`DirectGLES.cpp:1441-1465`） | **四层**：**(1) 构建期** G5 的逐 verb 世代表 + G7 的 render-state setter 一致性测试；**(2) 运行期** poison 在**需要该字段的那个 verb** 上 `Fatal`（不是某个后续 draw）；**(3) 语义** `MOBILEGL_PIPE_VERIFY` 逐 draw 逐字段比对（**含纹理 subdata 的保留模式**，否则最危险的子系统是瞎区）；**(4) 枚举** `gen_pipe_dirty_surface.py` 枚举 `MG_Impl` 里每个 mutator → 必须 bump 的聚合世代，CI 上未映射即失败。**迁移粒度是一个 accessor。** 477 行 inventory 保留为覆盖检查表 |
| **B-R7** | **`AcquirePersistentMap` 跨进程无解**会葬送 MC 26.3 的结果，而没有任何目标平台的支持被验证过 | **显式隔离**：改造期完全不碰，只有 IPC 那一步会打破它。决策交给三档 POST 探针与 **P0 第一周的 spike B**。T2 前端已在三处容忍并让 client 侧块推送成为强制（P5 交付）。若两台设备都否，P11 从 8 天缩为 2 天。**注意 T1 是每次存储定义一次 round trip，不是每 store 一次**（`StorageBufferRegrowScenario` 发布计数）。**不让一个平台未知数挡住 267 天的接口工作** |
| **B-R8** | **D18 的节点式容器纪律在重构中丢失**：`m_renderbufferResources` / `m_textureResources` 是**故意**用 `std::unordered_map`，一次扩表搬迁曾让 `BlitFramebuffer` 静默停在 "layout undefined"（`VkRenderPassManager.h:375-397`） | D18 是全表**唯一**标为 UNCHANGED 的身份行；**postmortem 注释必须逐字带进 P7 的 review checklist**。slot 数组在插入下稳定，实际改善了处境——但仍然点名 |
| **B-R9** | **逐 backend 的行为不对称被统一接口抹平**（Magma 故意不注册 `ResidentSubData`，`VkBufferManager.cpp:104-111`；`PrefersCpuXfbPrimitiveAccounting`；DirectVulkan 留空的 8 个槽） | 可选性是**接口的一等属性**：null 项在本代码库里**已经**表示"未实现，前端回退"（`BackendObject.h:212-215, 265-269`），`MGPCaps` 携带显式 `callMask`。**但 v2 收回了用 cap 位表达 emulation 归属的做法**（D-B7）：`ResolveTierForBatch` 逐 batch 用 `programReadsDrawID`（server 独有事实）选档，且两个 backend 都做 restart 重写，所以那五个 cap 位没有门可控。归属规则改成一句话 + 一个 `kCapNeedsHostIndexBytes` |
| **B-R10** | **接口在未测量的形状上过早冻结**；若干 server 侧缓存的容量是按拉取模式调的 | payload 结构从第一天走 structSize-first 版本纪律，可增长。字节**与调用**计数器在 P0 落地。**`stage-ubo-named` 出数之前不冻结 `set_shader_buffers` 的 host payload 形状**（D-B8）。**P13 在计数器活着的情况下重调所有幸存缓存的容量**，并把它们当作带 env 覆盖的调优参数。screen/context 划分在 P0 定进头文件但按 context 计数 == 1 实现 |
| **B-R11** | **58 行非箭头 `pGLContext` 用法的迁移缺口**；`DirectGLES.cpp:146` 的 `.get()` 与 `:142` 的 `decltype` 别名 `sed` 完全抓不到 | §2.4 已逐形态分类。P1 的交付物**包含这份 58 行清单的逐条转换**。**纯度门 grep 的是 `pGLContext` 而不是 `pGLContext->`** |
| **B-R12** | **残余值块是迁移期边界上的一个洞**：poison 抓不到"两侧布局不同"，而 monolith 的 verify harness **看不见它**（两侧是同一个 TU） | 逐成员 `offsetof` 断言 **加上** split 模式下逐字段序列化（走 G3 编解码器）。块的字节量单独计一类。`static_assert(sizeof == 0)` 让退役是编译错误 |
| **B-R13** | **`SEG_EVENT` 的 ERROR 无损化重新引入死锁** | 每秒 ERROR 速率限制器 + "N errors suppressed"；`MGLOG_E_ONCE` 的 latch 变 per-server；P9 的故障注入门要求"日志洪泛下注入一次 link 失败，那行 ERROR 必须出现"**且**"两侧都恢复" |
| **B-R14** | **排期估计**：v1 的阶段天数与它自己的子系统表矛盾，且低于同口径的独立分析 | §11.5 的每个天数都是它所含 §6.4/§6.5 行的求和，**算术公布**。总数改报 **267-337**（不含 CTS）。三个再基线检查点按求和后的上界 +50% 设定。CTS 周转**单独计价** |
| **B-R15** | **在 GL setter 时刻推送**会让整件事变慢，且这是最容易被后续实现者做错的一处 | 写成规范条款并给出证据（`DirectGLES.cpp:2029-2032` 的 Blaze3D per-batch blend toggle）；P2 的设备门直接暴露它。**v2 补一条同等重要的**：`glTexSubImage` **不是** GL 调用时刻推送的对象（它根本不调 backend 表，`GL_Texture.cpp` 只有 3 处 `MarkStorageDirtyRegion`），逐调用发 `resource_subdata` 会精确复现 Mali 的 ~100 作业形状（+6ms/frame）。规则的正确措辞在 §5.1.1；`resource_subdata` 逐帧发射次数进计数器并在 MC 动画图集 fixture 上设上限 |
| **B-R16（v2 新增）** | **stage C 之后 `MOBILEGL_PIPE_PUSH` 不再是对"旧 backend"的 A/B**：位清零时 `SnapshotFromGLContext` 仍要合成 handle，backend 仍跑重键后的 memo 代码，两个分支跑同一份新代码；一个重键 bug（D1/D2/D3/D11/D13 那一类）在两臂都在，位图二分不出来 | 在 §6.7 写明这条口径收窄。为 P3a 与 P4a 加**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`，让前两波 handle 化保留一个真正的旧-vs-新臂；随 pull 路径在 P13 退役。维护成本各阶段 +1 天，已计入 |
| **B-R17（v2 新增）** | **`MOBILEGL_PIPE_VERIFY` 是唯一的语义门，而 v1 的 P13 删掉了它的参照物**（`SnapshotFromGLContext`），删完之后设计没有语义绊线 | `SnapshotFromGLContext()` 与它的 `MG_State` include 整体包在 `#if MOBILEGL_PIPE_VERIFY` 里保留过 P13；三道纯度门**只跑非 verify 构建**；P13 另交付 MGPipe recorder 金标模式作为不依赖 `MG_State` 的长期语义门（同时是开放问题 11 的答案） |
| **B-R18（v2 新增）** | **monolith 的净代码量是增加的**（§2.7：约 +6,650 手写 + 4,000 生成，对 ~372 行真删除），所以"~550 行删除"不能当主论据 | 把 §10.3-④ 的**逐线程 CPU 数字**作为 monolith 论据的主体，删除清单降级为佐证。§2.7 公布净 LOC 估计，让 B-R2 有一个可证伪的预测。**若 P2 与 P13 的 CPU 数字持平而非改善，monolith 论据只剩架构性收益（ABA 不可表达、排序 hazard 消失、`inproc` 杠杆），必须据此重新评估是否值得** |

---

## 13. 开放问题

1. **client 侧 dirty 走查的真实每 draw CPU 代价是多少？** 中心性能主张是"遍历搬走而不是翻倍"，而真实基线只有每 backend 每 draw 10-25 次 accessor（§2.3.1）。P2 的头号数字，按逐线程 CPU + **绝对 ns**、两台设备报。
2. **真实语料上纹理重铸拉取的实际发生率？** `imageBindableHint` 能预防主因，但整格式再生（`Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就触发。若 MC 或 Iris fixture 上实测率非平凡，保留 LRU 从"默认 0"升为强制并需要真预算。
3. **`AcquirePersistentMap` 跨进程能不能成？** P0 spike B 第一周回答。未验证：`VK_KHR_external_memory_fd` 的 host-visible-coherent 支持在四条 lane 上的可用性；GLES 侧能否用 `GL_EXT_memory_object_fd` + `glBufferStorageMemEXT` 走同一条路。
4. **渲染状态的 wire 粒度**：pipeline 子集的 chunk 划分定下来之后，CSO LRU 的容量（暂定 64）与 `set_dynamic_state` 的 chunk 粒度仍需 P0 计数器定。
5. **`MG_Util` 的切割缝在哪里？** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、像素/纹理格式处理器、POST 探针、loader；client 需要 glslang phase A/B 与反射层。**P0.5 解决了 `ProgramObject.h` 这一处**，但 `MG_Util` 内部是否存在一条干净的 Transpile-vs-Reflect 缝**仍未审计**。
6. **一份反射归档能服务三个消费者吗？** Espryt 读前端表，Magma 跑 SPIRV-Reflect，而 `DirectVulkan.cpp:161` 为 `glGetProgramResource*` 又反射了第二遍。
7. **viewport-array 回放能塞进一次 `draw_vbo` 吗？** 今天它从 14 个 draw 入口经 `ForEachViewportRoutingPass` 重发应用的 draw N 次，而 `EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`（`DirectGLES.cpp:3841`）。未验证各遍之间观察到的状态是否与今天一致。
8. **`ResidentSubData` 的不对称该怎么收口？** null 项保住今天的行为，但拆分工作可能正是给 Magma 补一个真实现的时机——那是**行为变更而不是重构**，应作为独立 `dev` PR。
9. **`SEG_STAGE` 的上限定多少？** 六类新字节（§8.2）需要 P8 之后用 MC in-world 与 Create 两类 fixture 的 `stage-*` 计数器给 p99 占用。**并且 G3 的"单条记录大于段容量"分块路径需要设计与测试**。
10. **`FramebufferSrgb` / `DepthClamp` 无存储是潜伏 bug 还是有意为之？** 六个 backend 消费者今天读到恒定 false（`RenderState.cpp:380, 428-429`）。**必须在渲染状态 chunk 表冻结之前回答**。
11. **P13 之后 `MOBILEGL_IPC_VALIDATE_SERVER` 还有对应物吗？** **v2 部分回答**：保留 verify 构建（D-B5）+ P13 的 MGPipe recorder 金标。但 split-only 的**渲染** bug（而非状态推送 bug）仍然没有 server 侧第二意见——recorder 只覆盖推送内容，不覆盖 backend 对它的解释。
12. **~~client 侧 restart 重写与 indirect-count 解析会不会改变可观察行为？~~** **v2 已关闭**：D-B7 把 restart 重写与 multi-draw 分档留在 server，monolith 行为零变化，诊断仍落在原线程。**只有 `*IndirectCount` 的计数解析搬到 client**，它的 decline 路径（`DirectGLES.cpp:4682-4688`）随之落到应用线程——这是改善而非退化，但需要在 P8 的验收里核对日志文本与顺序。
13. **Magma 的两个内部 shader 烘焙后，uniform location 与 UBO 布局能否在没有活 `ProgramObject` 的情况下表达？**（`VulkanRenderer.cpp:4238-4241, 4319-4324, 8450-8452`）未做原型。
14. **推送模型会改变哪些按拉取模式调过的缓存命中率？** Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`；Espryt 的 4096/256/64 槽 `TwinLookupMemo`（后者会消失）。幸存者的容量在 P13 重调。
15. **（v2 新增）monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是一个潜在缺口？** `DirectGLES.cpp:4666-4667` 只做 `SyncPersistentMappedRange()`，而 compute 写的 indirect buffer 理论上需要前者。**这是一个独立的 `dev` 问题，拆分不得借机"顺手修"**——那会改变基线并让逐名对比失去意义。
16. **（v2 新增）索引宿主镜像的实际内存占用？** D-B7 的预算是 64 MiB 默认上限，但 MC/Sodium/Iris 语料里 element-array buffer 的总量未测。若显著超预算，退化路径（逐 draw 通过 `MGHostSpan` 传送）的频率与代价必须实测，因为它会把 §0.4 的内存优势和 §9.1 的零 round trip 主张同时削弱。

---

## 14. 对方案 A 文档与 `Feat/CS-Delta-IPC` 的复用清单

### 14.1 对 `PLAN.md` 的复用

| 判定 | `PLAN.md` 章节 |
|---|---|
| **原样取（不复述）** | §6.1（段布局、shm 矩阵、`SCM_RIGHTS` 第一优先、`SEG_SHADOW` 退休规则）；§6.2/§6.2a；§6.3；§6.4；§6.5；§6.6 前三条；§6.7 第 2、5 行；§6.8；§7.1-§7.3；§8 末尾；§9-§9.3；§10；§11.1-§11.6；§12 第 1-3 层与 §12.4；§13；§15 P0 的卫生与两个 spike |
| **取并改** | §7.4（**`on_log` 按严重级分级**）；§12.2（隔离从四个进程全局降到**两个**）；§5.10（第 2、3 条逐字取，第 1 条缩成一个 `hasLiveHostWrites` 位）；§6.10（应用指针按 §5.8 归属；**陈旧索引纪律改为逐站点表**，§5.8.1）；§5.9a（READ 面**编目**生成器改为**三道禁止门**）；§6.4 的拷贝账（删掉第 (3) 行，P1-4=3 / P4.5=2）；**§5.9b 的生成器改造而非删除**（`gen_impl_mutation_surface.py` → `gen_pipe_dirty_surface.py`，replay 义务消失、标记义务出现） |
| **弃** | §5.0、§5.1、§5.2、§5.4 的 replica 对象表规则与 `Fatal{IdentityDivergence}`、§5.6a、§5.7 的 Phase 1-4 分支与 `SetReplicaResolvedDrawProgram` 钩子、§5.9b 的 replay 半边（`MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::`）、§6.9 的 relink 档与 `MOBILEGL_IPC_PROGRAM`、§12 第 4 层的字节一致断言、`Server/ReplicaContext.*`、阶段 **P5**、风险 **R1** 与 **R6**、开放问题 **§17-5** |
| **新增** | `MG_Pipe/` 全套与七个生成器；**P0.5 的两个头文件抽取与 include 图门**；`PipeInputs` + **逐 verb 世代** poison；`MOBILEGL_PIPE_VERIFY` 影子比对（**含保留模式，且活过 P13**）；残余值块与其编译错误退役绊线；`MG_State` 的 5 个聚合世代 + dirty-surface 生成器；`set_dynamic_state`、`set_texture_params`；`Server/IndexHostMirror`（D-B7）；`on_texture_pull_request` / `resource_subdata_complete` / `on_texture_writeback` / `on_mip_levels_generated` / `on_xfb_scatter_ready`；纹理拉取的四条缓解 + 终止符 + 计数器；`HandleRecycleScenario` / `TextureRemintPullScenario` / `TextureUploadShapeScenario` / view-owner 游标别名场景 / `ClientArrayAfterComputeWriteScenario`；`RenderbufferObject::GetLifetimeId()`；D21 的潜伏 bug 修复；`MOBILEGL_PIPE_LEGACY_MEMOS`；`check_doc_citations.py` |

### 14.2 对 `Feat/CS-Delta-IPC`（worktree `../MobileGL-CS`）的复用

`PLAN.md` §14 的判定**整体继承**。方案 B 的四处差异：

| 条目 | `PLAN.md` 判定 | 方案 B 的差异 |
|---|---|---|
| `docs/CS_Refactor/HandleSessionGeneration.md`（`546895aa`） | REUSE，其中"handle 清单补 `RenderbufferObject::GetLifetimeId()` **与 `GetVersion()`**" | **只补 `GetLifetimeId()`**。`GetVersion()` 只是 replica 的 delta 触发器；推送模型里 `glRenderbufferStorage*` **本身**就是一次 pipe 调用 |
| `docs/CS_Refactor/backend_read_inventory.md` + `extract_backend_read_inventory.py` | CHANGE 成 `gen_backend_state_surface.py`，未知 accessor 一律 UNMAPPED 并编译失败 | **同意其修正**（删掉制造"0 UNMAPPED"的前缀兜底规则 `:234-241`），但**用途改变**：它变成 tracker 侧的**覆盖检查表**（G6），真正的门是 §4.7.2 的**三道纯度门**。**另外 `gen_impl_mutation_surface.py` 在方案 B 里改造成 `gen_pipe_dirty_surface.py` 而不是删除**（推论 4） |
| `MobileGL/RemoteClient/StateEmitter.h:39-307`（仅 emit 半边） | CHANGE，各域字段遍历抬进 `WireMirror` | **更直接可用**：那些字段集**就是** pipe 的状态对象 payload。必须修的缺陷不变：GL name 换 lifetimeId（`:48-49, 85, 166-168, 203, 230`）、O(n²) 线性扫描换 handle map（`:175-181, 244-249, 253-258, 293-298`）、固定 6 attachment（`:232-236`）换 `MaxColorAttachments`、补上被跳过的 texture view（`:70-74`）。**applier 半边（`:312-501`）仍然不取** |
| `MobileGL/Protocol/mg_protocol_base.h` | REUSE | **同意**，且 **structSize-first 版本纪律是 B-R10 的对策** |

**DROP 名单完全一致**：`bfa.h`、`mgruntime_api.h` + `UtilRuntime/*`、`LocalSocketTransport` 的实现（每次 send 的 UAF、无上限分配、**`fd=-1` 硬编码**）、`ServerHost/main.cpp`、`StateEquivalenceTest.cpp`、`c7c9e346`+`29d721ef` 的 share-group sessioning、`b50f3348` 的 `RenderState::InstallParameters` + 裸 `public:`、`d96be9f3` 的 per-draw `fprintf` TRIAGE 指令。

---

## 附 A：接口调用目录速查表

> Flags：`A`=`kNeedsAck`、`B`=`kHasBlob`、`V`=`kVarTail`、`H`=`kHostSpan`、`R`=`kReplySlot`、`O`=`kOptional`。

### `MGPipeScreen`（14）

| 调用 | payload | flags | 取代 |
|---|---|---|---|
| `get_caps` | `MGPCaps` | R | 40 `pActiveBackendObject->` + 89 caps 读点 |
| `resource_create` | `MGPResourceDesc` | — | buffer/texture/renderbuffer 创建 |
| `resource_respecify` | `MGPResourceDesc` | — | `BufferBackendOps::Respecify` 泛化 |
| `resource_destroy` | handle | — | `OnDestroy` + 两个 `WeakPtr` GC 扫描 |
| `map_persistent` / `unmap_persistent` | handle | R, O | `AcquirePersistentMap`（改造期不碰） |
| `fence_create` / `_status` / `_wait` / `_destroy` | handle (+timeout) | — / — / R / — | `FenceSync`…`GetSyncStatus`（两值契约保留） |
| `query_create` / `_begin` / `_end` / `_available` / `_result` / `_destroy` | handle + kind | — | `BackendObject.h:230-256` |

### `MGPipeContext` — CSO（15）

`create/bind/delete` × `render_state` / `vertex_elements` / `sampler` / `sampler_view` / `shader`。
`create_render_state` 带 `B`（**只带 pipeline 子集的 chunk**）；`create_shader_state` 带 `B`（SPIR-V + `ProgramArtifacts` 归档）。

### `MGPipeContext` — `set_*`（17 + 1 临时）

`set_dynamic_state`(B) · `set_framebuffer_state` · `set_vertex_buffers` · `set_index_buffer` · `set_indirect_buffers` · `set_sampler_views`(V) · `bind_sampler_states`(V) · `set_texture_params` · `set_shader_images`(V) · `set_shader_buffers`(V,H) · `set_stream_output_targets`(V) · `set_global_constants`(B) · `set_vertex_attrib_defaults` · `set_pixel_pack_state` · `set_patch_state` · `set_draw_program` / `set_dispatch_program`
**临时（P2..P13）**：`set_residual_value_state`(B)，带 `static_assert(sizeof(ResidualValueBlock)==0)` 退役绊线。

### `MGPipeContext` — transfer（12）

`resource_subdata`(B,V) · `buffer_subdata_resident`(B,O) · `resource_flush_range` · `resource_readback`(R) · `resource_copy_region` · `blit` · `clear` · `generate_mipmap` · `read_pixels`(R) · `get_texture_image`(R) · **`resource_subdata_complete`**（拉取终止符，可零 region）

### `MGPipeContext` — 命令（10）

`draw_vbo`(H,V) · `launch_grid` · `memory_barrier` · `begin/end/pause/resume_stream_output` · `flush` · `present` · `set_swap_interval`(O)

### 反向：`MGPipeCallbacks`（10）

`on_gl_error` · `on_gpu_written` · `on_buffer_writeback` · `on_texture_writeback` · `on_texture_pull_request` · `on_mip_levels_generated`（**只带形状**）· `on_surface_changed` · `on_caps_invalidated` · `on_log`（**≤WARN 有损 / ≥ERROR 无损 + 速率限制**）· `on_xfb_scatter_ready`

### 显式删除

`GetIntegeri_v` · `GetInteger64i_v` · `GetProgramiv` · `ShaderStorageBlockBinding`（折进 `MGPProgramDesc`）· `set_pixel_unpack_state`（不存在）· 压缩格式概念（不存在）· `pipe_transfer`（不存在）· `set_sampler_views` 的 stage 维度（不存在）· `kCapPrimitiveRestart` / `kCapPrimitiveRestartFixedIndex` / `kCapMultiDraw` / `kCapMultiDrawIndirect` / `kCapMultiDrawIndirectCount`（**归属不可表达，D-B7**）

---

## 附 B：环境变量与 CMake 选项

### CMake

| 选项 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 出货形态。开启后 `MG_Remote/**` 进 `SOURCE_FILES`。**两个**进程全局保持普通全局，GL 热路径无 TLS |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | CI/调试形态，隐含开启上者，额外加角色隔离 shim（只需隔离 `gPipeCtx` 与 `pActiveBackendObject`） |
| `MOBILEGL_PIPE_VERIFY` | OFF | **构建期开关**（不只是运行期）：编译进 `SnapshotFromGLContext()` 与 G4 比对器。**P13 之后仍保留**；三道纯度门只跑此项为 OFF 的构建 |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | ON（P2..P13） | 保留 registry / `TwinLookupMemo` 实现，给前两波 handle 化一个真正的旧-vs-新臂（B-R16） |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 只服务 CI 的 `flatc-check`；默认构建图里没有 `flatc` |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON（P7+） | DirectVulkan 的 blit/depth-mipmap shader 烘焙成签进树的 SPIR-V，由 `MG_Test` 重跑树内 glslang 逐字节比对守新鲜度。**monolith 也受益** |

> 注：`MG_Pipe/**` **不在任何 option 之后**——它是 monolith 的架构，永远进构建。

### 运行时（方案 B 新增）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | 迁移期按阶段推进；P13 后删除 | 子系统位图（0 = 全 pull），**含一位关闭 CSO 内容寻址**（P2 的负面对照）。**注意 stage C 之后 A/B 口径收窄**（§6.7、B-R16） |
| `MOBILEGL_PIPE_VERIFY` | 0 | 逐 draw 逐字段影子比对（~5-10× 慢，**含纹理 dirty 集合的保留模式**，永不出货） |
| `MOBILEGL_PIPE_STATS` | 0 | 字节 / **调用** / roundtrip / 纹理拉取 / 上传形状 / 残余块 / 索引镜像计数器转储 |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | **0**（v2 从 32 改） | 纹理重铸拉取的保留 LRU 预算。默认关闭：`MipmapStorage` 保有完整 CPU 影子，缓存买的是延迟不是正确性（§7.5c） |
| `MOBILEGL_PIPE_INDEX_MIRROR_MB` | 64 | server 侧索引宿主镜像预算（D-B7）。超预算退化为逐 draw 传送并计入 `index-bytes-shipped` |

### 运行时（继承 `PLAN.md` 附录）

`MOBILEGL_TRANSPORT`（`monolith` 默认 / `inproc` / `spawn` / `unix:<path>` / `pipe:<name>`）· `MOBILEGL_IPC_SERVER_PATH` · `MOBILEGL_IPC_RING_MB`(8) · `MOBILEGL_IPC_STAGE_MB`(32，上限由实测定) · `MOBILEGL_IPC_PRESENT_CREDIT`(**1**) · `MOBILEGL_IPC_SPIN_US`(50) · `MOBILEGL_IPC_POLL_ESCALATE`(64) · `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`(64) · `MOBILEGL_IPC_ADOPT_TIER`(auto) · `MOBILEGL_IPC_SHADOW_SHM`(1，P4.5+) · `MOBILEGL_IPC_INLINE_PAYLOADS`(0，负面对照) · `MOBILEGL_IPC_SERVER_AFFINITY`(auto) · `MOBILEGL_IPC_STRICT_ERRORS`(0) · `MOBILEGL_IPC_AUDIT`(0) · `MOBILEGL_IPC_TRACE`(0) · `MOBILEGL_IPC_ATTACH`(空) · `MOBILEGL_IPC_RESPAWN`(0) · `MOBILEGL_IPC_IDLE_EXIT_S`(30)

**删除**：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）· `MOBILEGL_IPC_VALIDATE_SERVER`（server 没有 `MG_Impl` 校验器——替代手段是保留的 verify 构建 + P13 的 MGPipe recorder 金标，见开放问题 11）

**保留的既有负面对照开关**：`MOBILEGL_ESPRYT_DISABLE_UBO_RING` · `_UNPACK_RING` · `_UPLOAD_RING` · `_INVALIDATE_FLUSH` · `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` · `MOBILEGL_COHERENT_AS_FLUSH`（**在拆分模式下照常生效**，这样两个 `coherent_as_flush: true` 的 Create fixture 在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义）
