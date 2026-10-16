## 11. 分阶段实施计划

> **通用纪律（每个 commit 都适用）**：默认 ALL target 必须能完整构建；禁止提交热路径插桩；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门（其 Vulkan 缺 `vkCreateHeadlessSurfaceEXT`，占该机 567 个基线集成失败中的 423 个）；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议（大核 1.96 / 小核 1.55GHz，GPU 拉满，40°C 门槛）；**每个阶段的出口都跑一次 §10.3 的五部分门**（不只是 P0）；**每个阶段的性能判据都是逐线程 CPU 时间**，不是墙钟帧时。
> **两条跑道**：P0-P4a、P3b/P4b、P7、P8、P13 是 **monolith 跑道**，每一段都可独立交付、可随时中止且 monolith 严格好于起点；P5、P6、P9-P12 是 **IPC 跑道**，整段继承 `PLAN.md` §6-§13。

### P0 — 卫生、度量、门与骨架（8 天）

**交付物**
- **清工作树 per-draw `fprintf`**：`DirectGLES.cpp:290-303`、`:640-663`、`Managers.cpp:875-877`（后者在 `pendingMutex` 临界区内）。CI 加 grep 门禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`。
- **`TracyPlot` 逐帧字节/调用计数器，装在边界两侧**，按类分：`cmd-records`、`stage-buffer`、`stage-texture`、`stage-ubo`、`persistent-map-push`、`server-ring`、`server-staging`、`residual-value-block`、`texture-pull`。**树里今天完全没有 per-frame 字节度量**（`MG_Util/Metrics` 只是格式算术，Tracy 只有 zone 无 plot），所以之后每一个 ring 尺寸、批阈值、wire 粒度决策否则都是猜测。两台设备取基线。
- `MG_Pipe/PipeCalls.def` + `MGPipeTypes.h` + `MGPipeHandles.h` + `MGPipeCallbacks.h`：**完整调用目录，即使暂未实现的条目也占位**（记录编号绝不能churn）。
- `scripts/gen_pipe.py` 与六个生成器 G1-G6 的骨架 + CI `pipe-gen-check`（重生成 + `git diff --exit-code`）。
- `MOBILEGL_PIPE_PUSH` / `_VERIFY` / `_STATS` 在 `ConfigLoader.cpp` 与既有开关并列解析。
- **三个严格 no-op 的免费收益**：`GetIntegeri_v`/`GetInteger64i_v`/`GetProgramiv` 的纯前端 case 移回 `MG_Impl`（Espryt 14 / Magma ~10 个读点）；`RenderbufferObject::GetLifetimeId()`（**不加 `GetVersion()`**）；D21 重键（`m_xfbCounterSlotByObject` 改用 XFB 对象 lifetimeId，`VulkanRenderer.cpp:11136-11146`）——**这一条是潜伏 bug 修复，先独立落 `dev`**。
- 回答 `FramebufferSrgb`/`DepthClamp` 无存储是潜伏 bug 还是有意为之（§10.4-6）。**必须在渲染状态 blob 冻结之前。**
- `MG_Remote/{Protocol,Transport}` 骨架与 `PLAN.md` P0 完全一致（`ITransport`、`InProcessTransport`、校验型 `Framing`、`Ring`+`RingControl` 双游标三元组双向 doorbell、`Doorbell`、`ShmSegment`、**`SCM_RIGHTS` 第一优先**）；`protocol.fbs` + 提交的 `protocol_generated.h` + `flatc-check`；`MG_Test/Wire/`。
- **`PLAN.md` P0 的两个 spike 原样跑**：spike A（Android 交付链：改名成 `lib*.so` 的 `add_executable` 能否被 AGP 打包、能否在 `untrusted_app` 域内 exec）；**spike B（external memory 导出，两台设备）**。

**验收**：`AdvertisedLimitsScenario`（6 个测试）绿；367 集成 × 2 backend + 428 单元逐名不变；40 个 trace 全绿；`nm --defined-only` 与剥调试信息 `.text` 除三处刻意移动的符号外一致；两台设备的基线字节/调用/逐线程 CPU 数字记录在案；spike A/B 出结论并写进开放问题（spike B 的结论直接决定 P11 的规模）。

### P1 — `PipeInputs` 替换与 verify harness（8 天）

**交付物**
- `MG_Backend/MGPipe/PipeInputs.h`：每个 backend 真正用到的 `GLContext` 方法一个访问器（Espryt 32 / Magma 55），**字段类型与今天读到的完全一致**，按 memo 键组织。
- 机械 `sed`：`MG_State::pGLContext->` → `MGB_CTX->`（**293 处**）；**外加逐条手工转换 58 行非箭头用法**（§2.4：~40 处 `MOBILEGL_ASSERT` 真值判定删除、~10 处空守卫改直读、3 处 patch 三元、`DirectGLES.cpp:146` 的 `.get()` 裸指针捕获、2 处 `!= nullptr` 条件、1 处注释）。**这份 58 行清单是本阶段的显式交付物**，不是"顺手处理"。
- `SnapshotFromGLContext()` 在 `PrepareForDraw`（`DirectGLES.cpp:2916`）与 `SetupDraw`（`VulkanRenderer.cpp:6371`）顶端。
- **G5 的 `m_filledMask` poison**：debug 与 disaggregated 构建里读未填字段 = `Fatal{UnmigratedPipeInput, "<field>"}`。
- **G4 的 `MOBILEGL_PIPE_VERIFY=1` 逐字段影子比对器** + 第三种 CI 模式接线。

**验收**：**pull 构建里 `nm --defined-only` + 剥调试信息 `.text` size 与替换前完全一致**——本阶段可证明是一次替换（这是最后一次这条等式成立）；全部 40 个 trace 与 367 个集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；**故意损坏一个快照字段能让 verify 门变红**；故意留一个字段不填能在第一个 draw 上触发 poison Fatal。
**★ 第 16 天 — 最早可见里程碑：**零产品风险地证明"推送等价于拉取"，逐 draw 逐字段。

### P2 — 值推送：渲染状态 CSO（双后端）+ 残余值块（8 天）

**交付物**
- `MG_Impl/Pipe/Tracker.{h,cpp}`：dirty 位来自既有计数器（§5.2，三个回绕 `Uint16` 在 tracker 边界加宽）+ 固定 validate 顺序（§5.3）。
- `MG_Impl/Pipe/CsoCache`：64 项 LRU，键是三段（head/blend/tail，与 `DirectGLES.cpp:2038-2047` 完全一致的划分）的 xxHash。
- `create_render_state` / `bind_render_state`（D-B1）：Espryt 侧 `RenderStateImpl` 的 693 行函数体、三段 memcmp、`g_syncedColorMaskAlphaWidenMask`、dual-source decline **一行不动**（消除 4 个读点：`:2007, 2021, 2050, 2133`）；Magma 侧 `ComputePipelineStateHash` / `GetOrCreatePipeline` / `ApplyDynamicDrawStateTail` 改从 CSO 取（消除 ~55 个读点）。两个版本号都过线。
- `set_pixel_pack_state`（PACK only）、`set_patch_state`、`set_vertex_attrib_defaults`。
- **`set_residual_value_state` + `ResidualValueBlock`**（§6.3）：`static_assert(sizeof == MGL_RESIDUAL_BLOCK_SIZE)`（逐阶段**下调**）+ **逐成员 `offsetof` 断言** + split 下逐字段序列化。

**验收**：367 集成 × 2 backend × 2 模式（pull / push）逐名相同；40 个 trace 在 monolith-push 下 SSIM ≥ 0.99，双后端；`ClipDistance`、`SampleMaskScope`、`SampleVariables`、`DualSourceBlend`、`ViewportArray`、`PrimitiveRestart` 场景绿；verify 模式零分歧；**两台设备 reboot-clean 配对：monolith-push 在 p50 与 p99 逐线程 CPU 上落在 monolith-pull 噪声内或更好**。
**★ 第 24 天 — GO/NO-GO 决策点。** 此刻手上有：verify harness、双后端已推送的渲染状态、真实 CPU 增量、Track H 在两个最便宜子系统上的单位成本。**若 CPU 数字为负、或 Track H 单位成本超估计 50%，退回方案 A 只损失这 24 天中的 16 天**（P0 是 `PLAN.md` 共有的）。

### P3a — handle wave 1（Espryt）：slot 基建、buffer、VAO（12 天）

**交付物**：`SlotAllocator`（按 kind 稠密 + `gen` + 保留段）；`PipeObjectTables`（POD slot **数组**）；**删** `TwinLookupMemo`×3、`OwnerEquals`、`g_fbSlotCache`、6 个 registry GC 扫描、`VkTextureManager::PruneDeadTextures`、`m_convertedVertexStreams` 的 `sourcePin`；7 个 `BufferBackendOps` → `resource_create/respecify/destroy`、`resource_subdata`、`buffer_subdata_resident`（**可 null，保住 Magma 的差异**）、`resource_flush_range`（带应用真实 access flags）、`resource_readback`、`map_persistent`（**不碰实现**）；pool 与延迟释放机制原样搬；`create/bind/delete_vertex_elements_state`（**两个视图都带**：解析后的 `VertexAttribute[32]` 与 `VertexBufferBindingPoint`；`IsLong` 与 `Type` 分开）；`set_vertex_buffers`（**`baseInstance` 是显式字段**，不再是调用方武装的 `ScopedFetchBaseInstance` 作用域）；`set_index_buffer`（带 restart index 与模式）；Adreno 禁用属性 SIGSEGV workaround 原样保留。

**验收**：全套门（monolith-push，DirectGLES）；`LargeArenaAdoption`、`ResidentIndex`、`StorageBufferRegrow`、`AtomicCounter`、`BufferTexture`、`CrossFrameBuffer`、`SsboArrayLength`、`SsboArrayDynamicIndex`、`VertexArrayEnableDisable`、`VertexAttribBinding`、`DoublePrecision`、`DrawParameters`、`MultiDraw`、`PrimitiveRestart` 场景；`create-indirect`、`create-instancing`、`rd12-odinlite`、`improved-transparency-26.3`、`fabric-sodium` trace SSIM ≥ 0.99；**新增 `HandleRecycleScenario` 绿，且它在 slot 重键之前必须是红的**；MC 26.3 在 Adreno 上 p99 不变（16MiB 采纳结果不得回归）。
**⚠ 再基线检查点 1：若 P3a 超期 >50%（>18 天），"窄 handle 化"的前提就是错的，必须在 P4a 开始之前重定基线。**

### P4a — handle wave 2（Espryt）：FBO / 纹理 / sampler / program 的身份与描述符（16 天）

**刻意推迟到首帧之后的部分**：memo 重键、dirty 归属反转、program 陈旧性重构（→ P3b/P4b）。本阶段只做 wire 需要的身份与描述符。

**交付物**：`set_framebuffer_state`（8 个 `MGPSurface` + **client 解析后的 `readSurface`** + 内联 `internalFormat` + `contentHash` + `isDefault` 保留 handle，退役 4 处 `pDefaultFramebufferInfo` 读）；四个跨对象 mask 在推送时刻推出；`create/bind/delete_sampler_state`（`SamplerParameters` 逐字节含 `borderColorForm`）；`create/delete_sampler_view`（`glTextureView` 变普通 view CSO）；`set_sampler_views`（client 侧解析）+ `bind_sampler_states`；`set_shader_images`；`create/bind/delete_shader_state`（逐 stage SPIR-V + `Visit()` 全结构体反射归档 + `sizeof` 绊线）；`set_draw_program` / `set_dispatch_program`；`set_global_constants`；`CompositeResolver.cpp`（保留 slot 段 + 生命周期）；纹理与 renderbuffer 的 `resource_create/respecify/subdata`。emulation 路径在 split 模式下**显式 Fatal** 直到 P8。

**验收**：全套门；`CrossFrameBuffer`、`LayeredAttachmentShape/Barrier`、`SnormAttachment`、`RenderbufferBlendFormat`、`FragmentOutputArrayIndex`、`Orientation`、`ClearThenReadPixels`、`FragCoordOrigin`、`TextureView`、`ProgramPipeline`、`PostLinkAttach`、`RelinkStageSet`、`SpirvShaderBinary`、`AsyncCompile`（6 个）场景；`KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块在两台设备上绿（**~3300 个 framebuffer/用例，是 handle 复用的压力测试**）。

### P5 — 传输 + inproc applier + 发射表（12 天）

**交付物**：`MG_Remote/Client` 的发射表实现 `MGPipeScreen`/`MGPipeContext`；`Server/PipeApplier.cpp` 从记录填 `PipeInputs` 与 `PipeObjectTables`；`ServerLoop`（`mgl-srv-io` + `mgl-srv-apply`，后者终身持有原生 context）；单一 hook 点 `MG_Backend/Init.cpp:48-70` 装 `BackendObject_Remote`（下游零 `#ifdef`）；`MGPCaps` 快照；一条阻塞 `read_pixels`；client 侧保守 `MarkGpuWritten` 与 `emitSeq`；**client 侧块粒度 persistent-map 推送**（T2 档下强制，`PLAN.md` §5.10 第 2 条）；`InProcessTransport`。

**验收**：`ctest -R 'DirectGLES\.Split\..*(ClearThenReadPixels|Triangle)'` 在 `MOBILEGL_TRANSPORT=inproc` 下绿；**OpenRA trace 在 split 模式下 SSIM ≥ 0.99**；**`PersistentCoherentMapScenario` 绿**（map PERSISTENT|WRITE|COHERENT、写、不做任何其它 GL 调用、draw、回读校验——这是唯一一个为一个致命缺陷而存在的门）；**两个角色的峰值 RSS 记录在案**，作为对 `PLAN.md` R14 的基线；`persistent-map-push` 字节量出数；任何未迁移的 `PipeInputs` 字段读产生 `Fatal{UnmigratedPipeInput}` 而不是静默垃圾。
**★ 第 64 天 — 首个 IPC 帧（`inproc`）。诚实标注：这是缩减路径**——client 数组、restart 重写、indirect-count 解析在 split 下仍是 Fatal，全功能要等 P8。

### P6 — spawn transport（5 天）

**交付物**：`SocketTransport`（socketpair + fork/execve，**显式 envp 剔除 `MOBILEGL_TRANSPORT`/`MOBILEGL_IPC_*` 加 `mobilegl_server_main` 内强制 Monolith 的双保险**）；`ServerMain`；`MOBILEGL_IPC_SERVER_PATH` 为主 + `dladdr` 兜底（两个桌面门都会挫败 `dladdr`）；就绪握手有界重试；client EOF 即时退出；server 死亡的 device-lost latch；trace-replay 的 `SPLIT` 后缀与 `-DTRACE_TRANSPORT=` 接线。

**验收**：P5 全部测试在 `MOBILEGL_TRANSPORT=spawn` 下绿；fork 链测试断言进程树只多一个子进程；`HeadlessGL` 的 fork 预检交互测试无孤儿 server（`pgrep` 计数在 100ms 内归零）；`run_android_retrace_local.py --case OpenRA --backend DirectGLES` 在 `35d0befa` 上 SSIM ≥ 0.99。
**★ 第 69 天 — 首个跨进程帧（缩减路径）。**

### P3b / P4b — 深化（Espryt）：memo 重键、dirty 反转、program 陈旧性（28 天）

**交付物**：重键 `ResolvedDrawBuffers`、`PendingAttribValueMask`、`ConvertedFloat64Stream`、`SyncCurrentFBO` 四元组戳、`ResolvedTextureBindingMemo`、`SamplerPassMemo`、image sweep、program registry 到 `{slot, gen}`；**删** `g_unitTextureSyncList`、`g_fboTextureSyncList`、`g_unitSamplerLookupMemos`、`g_imageSweep*`、`DirectGLES.cpp:1372-1489` 的 ~115 行 unit-bindings epoch 推导；**dirty 归属反转**（§7.3，client 保 rect 模型与发射游标、发射后自清；`resource_subdata` 带 box **与** rect 列表、server 选形状）；**删** fragColor 重推导 workaround（`:2712-2732`）与 `g_broadcastMemo*`；用推送的 FBO 状态、image 格式、`set_patch_state` 退役 9 条陈旧性判定里的第 4-6、8-9 条；Espryt 的 raw-depth-fetch `SamplerObject` 原生化。

**验收**：~25 个纹理场景（`TextureView`、`LayeredTextureReadback`、`ImageSizeAfterRespec`、`FormatlessImageBake`、`NonCoreImageFormat`、`ImageFormatQualifier`、`ImageTargetKind`、`ImageLoadStoreSso`、`UnboundImageDescriptor`、`SwizzleAccessRoutine`、`IntegerBorderColor`、`PixelStoreSweep`、`SampledSetStaleness`、`ThreeChannelAttachment`、`BufferTexture`、`CopyImage*`×3、`ClearTexImageUndefinedLevelZero`、`DepthStencilReadback`×3、`PackedWordReadback`）；21 个 program 场景 + 整个 `MG_Test/ShaderTranspiler` 目录；两台设备上完整 `KHR-GL46.texture_*` / `internalformat.texture2d.*` / `shader_image_*` / `packed_pixels` 块，conformance 在 pull 基线 0.5pp 内；**每一个 Iris trace**；**Mali 上 box-vs-rects 上传形状不得回归**（+6ms/frame 悬崖）；verify 模式全程零分歧。

### P7 — DirectVulkan（Magma）全量迁移（48-85 天，可与 P5/P6/P8 并行）

**交付物**：§6.5 的 11 个子系统，重点三项：`SetupDrawSnapshot` 的 ~14 个探测字段（含两个**有损**的版本求和）塌成 dirty mask 比较；**`UniformManager` 的 8 类占位 `TextureObject` 换成原生 `VkImage`+view+descriptor**（~120 行删除，43 个 `MOBILEGL_ASSERT(pGLContext)` 里的 9 个消失）；**blit / depth-mipmap 内部 shader 烘焙成签进树的 SPIR-V + uniform location + UBO 布局，由一个 `MG_Test` 重跑树内 glslang 逐字节比对的用例守新鲜度**（不用 host 工具构建步骤）；`VertexInputStateFactory` 的后端堆裸指针写回**直接删除**；`VkRenderPassManager` / `VkTextureManager` 的**节点式容器纪律原样保留**（D18，postmortem 注释逐字带进 review checklist）。

**验收**：367 集成 + 40 trace 在 DirectVulkan 的 monolith-push 与 split 下全绿；verify 零分歧；**`nm -D libMobileGLServer.so | grep glslang` 为空**——这是整个论点的强制执行点；`UnboundImageDescriptor`、`SampleMaskScope`、`ImageLoadStoreSso`、`AtomicCounter`、`SsboArrayDynamicIndex`、`NonCoreImageFormat`、`Orientation`、`DepthStencilReadback*` 场景；两台设备 CTS 在 0.5pp 内。
**⚠ 再基线检查点 2：P7 中点（第 24 个工作日）若已完成子系统 < 40%，立即重定基线**——P3a 的检查点只能发现 Espryt 侧的偏差，发现不了 Magma 特有的超期，而 P7 在单跑道下位于关键路径。

### P8 — emulation 下放 + 协议广度（14 天）

**交付物**：`MG_Impl/Pipe/HostResolve.cpp`——client 数组范围计算、**最大索引扫描**（`TryComputeMaxIndexFromHostBytes` 移到 client，唯一的无界应用指针读）、**primitive-restart 重写**、**indirect-count 解析**，每一条前面都有与 monolith 的 `SyncPersistentMappedRange()+SyncGpuWrites()` **完全相同位置**的 publish/wait/drain（§5.8.1）；`MGHostSpan` 的 split 填法；**CopyImage shadow 镜像搬到 client**（删掉一整条 server→client 字节通道）；`draw_vbo(info, indirect, ranges[], numDraws)` 收编 multi-draw 族；viewport-array 回放验证在一次 pipe 调用驱动下各遍之间观察到的状态与今天一致（`EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`，`DirectGLES.cpp:3841`）；`generate_mipmap` 返回 level 计划与 CPU 回退的纹素。

**验收**：`ctest -L integration-gpu -R '^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` **逐名相同**，DirectVulkan 同；40 个 trace 在 split 下双后端 SSIM ≥ 0.99，含两个 `coherent_as_flush: true` 的 Create fixture（**两种模式都开着该开关跑**）；**新增 `ClientArrayAfterComputeWriteScenario` 绿，且去掉那次等待必须能看到几何缺失**；`MultiDraw`、`PrimitiveRestart`、`ViewportArray`、`DrawParameters`、`CopyImage*`×3、`GuiBatch` 场景。

### P9 — 反向通道（10 天）

**交付物**：`SEG_REPLY` 4KiB slot 池；阻塞 `read_pixels`；PBO 回读 fire-and-forget；`on_gpu_written{res, ranges}` 收窄（配 `set_shader_buffers` 的 `writableMask`）；`on_buffer_writeback` **按操作级批处理**（今天两处逐行循环：`Utils.cpp:2342`、`DirectGLES.cpp:7633`）配 epoch bump 的排序规则（§7.4）；`on_texture_writeback`（一个生产者）；`on_mip_levels_generated`；**`on_texture_pull_request` 三条缓解全上**（§7.5）；`on_gl_error` 有序 + 分配类 `kNeedsAck`；`on_caps_invalidated`；`on_surface_changed`；**`on_log` 按严重级分级**（≤WARN 有损 / ≥ERROR 无损 + 每秒速率限制器 + "N errors suppressed"）；`SEG_EVENT` 溢出策略 + 等待循环内排空。

**验收**：`DepthStencilReadback`×3、`PackedWordReadback`、`LayeredTextureReadback`、`ClearThenReadPixels`、`XfbAfterClipDistance`、`XfbCaptureBufferReuse`、`XfbRepeatedCapture`、`TessellationXfbCapture` 在 split 下绿；**新增 `TextureRemintPullScenario`**（同时强制 `RequireImageBindableStorage` 与帧中格式再生）绿，**且逐 trace 用例发布拉取计数**；故障注入：client 被 credit 阻塞时灌满 `SEG_EVENT`，两侧都必须恢复；**日志洪泛下注入一次 backend link 失败，那行 ERROR 必须出现**。

### P10 — sync / query / present 节奏（6 天）

**交付物**：client 铸造 sync 与 query handle；轮询入口成为门铃点 + `MOBILEGL_IPC_POLL_ESCALATE` 饥饿升级；**fence 完成度来自真的逐 fence 退休**（不是 present 水位——那正是 MC 1.21.5 native-heap OOM 的成因）；DirectGLES 的非 present fence tick；`Present` 严格 1:1；`MOBILEGL_IPC_PRESENT_CREDIT` 默认 1 + 叠加公式写进文档；逐帧 roundtrip 计数器与**输入延迟直方图**（记录发射时刻 → present 完成时刻）；`PLAN.md` §8 末尾的三个独立 `dev` monolith 修复。

**验收**：`XfbPrimitiveQuery`、`PrimitivesGeneratedNoXfb`、`AsyncCompile` 在 split 下绿；**40 个用例上 draw/state/upload 路径的 roundtrip 计数器读零**，条件渲染与阻塞 query 次数逐用例发布而非声称为零；零 timeout 轮询循环测试在有界时间退出（不加 publish 规则它会永远挂住）；`bench.sh` 在 `35d0befa` 上配对 A/B：两侧都关采纳时 split 帧时在 monolith 10% 内，输入延迟直方图 p50/p99 记录在案。

### P11 — persistent map 与 ≥16MiB 采纳（8 天；spike B 全否则缩为 2 天）

**交付物**：由 P0 spike B 驱动的 POST 探针档位选择（T2 拒绝 / T1 server 导出 / T0 server 导入）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`MOBILEGL_IPC_ADOPT_TIER` 覆盖开关做负面对照。

**验收**：`LargeArenaAdoptionScenario` 在所选档位下绿；`improved-transparency-minecraft-26.3` 与两个 Create fixture SSIM ≥ 0.99；`35d0befa` 上配对 reboot-clean 的 p99 帧时与峰值 RSS 对 monolith 采纳基线（p99 163→21ms、40→115fps、~400MB）——**split 在所选档位下 p99 不得回归超过 10%；若 T2 成为永久答案，其实测代价必须写进文档**。

### P12 — Android 生产窗口路径（10 天）

**交付物**：`android:process=":mgl"` 的 Service 收 Java `Surface`（Binder）后 `ANativeWindow_fromSurface`（minSdk 26 无公开 `ANativeWindow` 扁平化，这条路径是强制的；树内先例是 `android:process=":bench"` 的 `BenchService`）；server 生命周期绑 Activity；FCL 用户 env 与 plugin APK V2 开关表接线（**零新增管线**，两条通道都已存在）。

**验收**：Minecraft 通过 FCL 在 spawn 模式下在 `35d0befa` 上双后端入世界；配对 reboot-clean bench + 输入延迟直方图；杀 server 产生干净的 device-lost latch 而不是挂起或崩溃；SIGKILL 故障注入。

### P13 — 退役 pull 路径（6 天）

**交付物**：删 `SnapshotFromGLContext()`、`MGB_CTX` 宏、`MOBILEGL_PIPE_PUSH`（推送成为唯一路径），**保留 `MOBILEGL_PIPE_VERIFY` 工装**；删 `set_residual_value_state` 与 `ResidualValueBlock`；`MG_Backend` 的 `MG_State` include 收缩到共享值头白名单；最终符号/尺寸/CPU 报告。

**验收**：**`static_assert(sizeof(ResidualValueBlock) == 0)` 编译通过**（临时物可证明地消失）；`grep -c 'pGLContext' MG_Backend/` == 0；`nm --undefined-only libMobileGLServer.so` 无 `MG_State::GLState::`、无 glslang 符号；全套门（367 × 2 backend × {monolith, split}、428 单元、40 trace SSIM ≥ 0.99、两台设备 CTS 在 `81b17c0b` 基线 0.5pp 内）；**monolith 逐线程 CPU 在两台设备的 p50 与 p99 上不差于 P0 基线**——本设计的性能主张在这里成立或倒下。

### 11.5 总估时、里程碑与 CTS 周转

**累计（单跑道）**：P0 8 → **8**；P1 8 → **16**；P2 8 → **24**；P3a 12 → **36**；P4a 16 → **52**；P5 12 → **64**；P6 5 → **69**；P3b/P4b 28 → **97**；P8 14 → **111**；P9 10 → **121**；P10 6 → **127**；P11 8 → **135**；P12 10 → **145**；P13 6 → **151**；**P7（Magma）48-85** → **199-236**。

**报作 200-260 人天**（上界含缓冲）。两个工程师、P7 与 P5/P6/P8 并行 → **约 6-7 个月**，真正的约束是两台设备的争用而不是人头。

**里程碑**：第 **16** 天 verify harness 全绿（零产品风险）；第 **24** 天 GO/NO-GO；第 **64** 天首个 `inproc` IPC 帧（**缩减路径**）；第 **69** 天首个跨进程帧（**缩减路径**）；第 **111** 天全功能 split（P8 出口）；第 **151/199-236** 天接口纯度门转绿。

**CTS 周转必须单独计价，不折进阶段估时。** `gl44to46` caselist 约 56,271 例。分层门控：逐阶段只跑该阶段改动可能影响的具名 CTS 块（P4a 的 `packed_pixels`、P3b/P4b 的 `texture_*`/`shader_image_*`、P9 的 `transform_feedback*`），**完整 caselist 只在四个架构边界跑**（P3a handle、P4a framebuffer、P3b/P4b 纹理、P13 纯度）**以及每次合并 `dev` 之前**，且放在 CI 而不是关键路径上。设备锁协议照旧。若实测周转仍主导排期，**诚实做法是加宽估时而不是削弱门**。

---

## 12. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| **B-R1** | **效率是方案 A 的 3 倍、首帧晚 4-5 倍**（200-260 天 vs 77；第 69 天 vs 第 15 天）。排期驱动的评审可以只凭这一条否掉本方案 | 把价值排在承诺之前：P0-P2（~24 天，其中 16 天是方案 B 独有）交付 handle 化 twin 与内容寻址的渲染状态 CSO——**零 IPC 风险的可测量 monolith 性能工作**——并产出字节计数器与第一个逐线程 CPU 数字。**第 24 天显式 GO/NO-GO。** P13 是一个完全自洽、不含任何 IPC 的 monolith 交付物；P5 的 `inproc` 在 P13 之后是本项目手上最大的单一 CPU 杠杆，只要 12 天 |
| **B-R2** | **中心性能主张未经测量。** 可达性遍历是**搬走**而不是消失；若 client 侧 dirty 走查比它替换掉的拉取更贵，整个论证反转。而树里今天没有任何 per-frame 字节或调用度量 | 字节/调用计数器是 **P0 交付物**，不是后续性能阶段的事（这是历次评审对四个候选设计的共同发现）。每阶段的验收都用**逐线程 CPU 时间**，两台设备、reboot-clean、配对。P2 刻意选最便宜的面，好让机制在花掉 200 天之前就被证明。**先清工作树 per-draw `fprintf`**——它污染每一个基线 |
| **B-R3** | **monolith 字节一致门按构造死亡**，P2 的逐名集成基线也随之移动（参考构建变了） | 五部分替代门，全部在 P0/P1 落地（§10.3），其中第 ② 部分（逐 draw 逐字段影子比对）在语义上严格强于任何符号 diff。两条字节等式仍作断言保留。**在设计文档里明写这条损失，不藏。** P2 的参考构建明确定义为"重构后的 monolith"，并要求 P1 出口先证明重构本身是等价的 |
| **B-R4** | **server 发起的纹理拉取是新停顿类**，且触发路径之一（整格式再生 `Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就会触发、无法被 hint 预防 | 三条缓解同时上：`imageBindableHint` 预防主因；**异步**park-and-re-emit 让停顿落在 `mgl-srv-apply` 而不是应用线程；有上限的 ≤32MiB 保留 LRU 让一张纹理不会停顿两次。`TextureRemintPullScenario` 强制两个成因，**拉取计数逐 trace 用例发布**而不是声称为零。若实测率非平凡，保留缓存从可选升为强制并拿真预算 |
| **B-R5** | **P3b/P4b（纹理，28 天）与 P7 中的 `VkTextureManager` 是最大最险的段**，且正压在项目已实测 +6ms/frame 悬崖（rect 列表 vs union box）与 7 条 fallback-repack 路径上（其可行性判定要求 `uploadData == mipData`） | `resource_subdata` **同时**带 box 与 rect 列表、**server 选形状**——决策留在做过那次测量的那一侧。repack 族原地不动，只把输入从拉取的 shadow 指针换成 `MGPBlobRef`（monolith 里是同一个指针）。**在 Mali 设备上专门设门并公布帧时增量**。P3b/P4b 拆成两个可独立落地的半（先 sampler view + sampler，再 image unit + dirty 反转），让回归能二分到其中一半 |
| **B-R6** | **tracker 完整性**：推送之后 server 不能再重读活状态来校验快路径命中，它必须信任上一次推送。任何 tracker 忘记发的 mutator 会静默漂移，直到某个场景恰好触及。历史上最危险的正是这个形状——DSA by-name 模拟曾在不移动任何计数器的情况下交换一个纹理单元绑定（`DirectGLES.cpp:1441-1465`） | 三层：**(1) 构建期** G5 的 written-once 位图断言每个 `kCtxState` 组在第一个 verb 消费它之前被写过；**(2) 运行期** poison mask 在读未填字段时 `Fatal{UnmigratedPipeInput}`；**(3) 语义** `MOBILEGL_PIPE_VERIFY` 逐 draw 逐字段比对，专抓"dirty 位触发得太少"这个危险方向。删掉 `SnapshotFromGLContext()`（P13）之后完整性成为构建期事实。**迁移粒度是一个 accessor**，所以一次遗漏被限制在一个阶段 B 步骤内。477 行 inventory 保留为 tracker 侧检查表 |
| **B-R7** | **`AcquirePersistentMap` 跨进程无解**会葬送 MC 26.3 的结果（p99 163→21ms、40→115fps、省 400MB），而没有任何目标平台的支持被验证过 | **显式隔离**：改造期完全不碰（它已经是一个返回指针的显式调用，原样穿过 P0-P13），只有 IPC 那一步会打破它。决策交给 `PLAN.md` §6.8 的三档 POST 探针与 **P0 第一周的 spike B**。T2（拒绝）前端已在三处容忍并让 client 侧块推送成为强制（P5 就交付）。若两台设备都否，P11 从 8 天缩为 2 天的文档与负面对照。**不让一个平台未知数挡住 200 天的接口工作** |
| **B-R8** | **D18 的节点式容器纪律在重构中丢失**：`m_renderbufferResources` / `m_textureResources` 是**故意**用 `std::unordered_map` 而非本项目开放寻址的 `UnorderedMap`，因为调用方跨后续查表缓存 `Resource*`；一次扩表搬迁曾让 `BlitFramebuffer` 静默停在 "source image layout is undefined"（postmortem `VkRenderPassManager.h:375-397`）。把它"优化"回去正是一次大重构最容易犯的错 | D18 是全表**唯一**标为 UNCHANGED 的身份行，理由写进设计正文：接口对此零约束，`ska` 的 erase-shift 让 hazard 更糟而不是成为历史，**postmortem 注释必须逐字带进 P7 的 review checklist**。slot 数组在插入下稳定，实际改善了处境——但仍然点名，防止有人顺手改回去 |
| **B-R9** | **逐 backend 的行为不对称被统一接口抹平。** Magma 故意不注册 `ResidentSubData`（`VkBufferManager.cpp:104-111`），差别是 `glBufferSubData` 在活 coherent map 上的排序语义；`PrefersCpuXfbPrimitiveAccounting`、DirectVulkan 留空的 8 个槽、不同的 multi-draw 分档同形。强制统一会静默改变 Magma 行为并发布一次 Minecraft chunk 撕裂 | 可选性是**接口的一等属性**：null 项在本代码库里**已经**表示"未实现，前端回退"（`BackendObject.h:212-215, 265-269`），`MGPCaps` 携带显式的 `callMask` 能力位集，取代今天 `GL_Query.cpp:471, 545, 768` 的隐式槽位空否探测。**cap 位集同时决定每个 emulation 在哪一侧 lowering**，所以不对称成了机制而不是疣 |
| **B-R10** | **接口在未测量的形状上过早冻结**：渲染状态 CSO 粒度、`resource_subdata` 的 box+rect 双载荷、sampler-view 解析边界、screen/context 划分全在 P0-P2 定死；而若干 server 侧缓存的容量是按拉取模式调的（Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`；Espryt 的 4096/256/64 槽 `TwinLookupMemo`） | payload 结构从第一天走 structSize-first 版本纪律（复用 `Feat/CS-Delta-IPC` 的 `mg_protocol_base.h` 词汇），可增长。字节计数器在 P0 落地，让 P2 的 CSO 粒度决策有数据。**P13 在计数器活着的情况下重调所有幸存缓存的容量**，并把它们当作带 env 覆盖的调优参数而不是常量。screen/context 划分在 P0 定进头文件但按 context 计数 == 1 实现——现在免费，事后是一次全面重编号 |
| **B-R11** | **58 行非箭头 `pGLContext` 用法的迁移缺口**：三份候选设计都提了"机械 sed 293 处"，都没交代这 58 行；其中 `DirectGLES.cpp:146` 的 `.get()` 裸指针捕获 `sed` 完全抓不到 | §2.4 已把它们逐形态分类。P1 的交付物**包含这份 58 行清单的逐条转换**，不是"顺手处理"。**纯度门 grep 的是 `pGLContext` 而不是 `pGLContext->`**，所以漏一条就红 |
| **B-R12** | **残余值块是迁移期边界上的一个洞**：poison mask 抓得到"未填字段"，抓不到"两侧布局不同"——而这正是一个异质 POD 并集跨编译器/ABI 边界最容易出的问题，且 monolith 的 verify harness **看不见它**（monolith 里两侧是同一个 TU） | 逐成员 `offsetof` 断言（不只是 `sizeof`）**加上** split 模式下逐字段序列化（走 G3 生成的编解码器）而不是整块 memcpy。块的字节量单独计一类，让"它还有多大"可见。`static_assert(sizeof == 0)` 让退役是编译错误 |
| **B-R13** | **`SEG_EVENT` 的 ERROR 无损化重新引入 §7.4 要防的死锁**：一个 shader 风暴产生的 `MGLOG_E` 洪泛可能停住 apply | 每秒 ERROR 速率限制器，超限发一条显式的 "N errors suppressed"；`MGLOG_E_ONCE` 的 latch 变 per-server（这正是想要的）；P9 的故障注入门要求"日志洪泛下注入一次 link 失败，那行 ERROR 必须出现"**且**"两侧都恢复" |
| **B-R14** | **排期估计偏乐观**：独立的改造成本分析给出 backend 工作**单独** 202-266 天；本文的 200-260（含 IPC）落在其乐观端，而 P7 的 48 天下界明显低于同口径的 85-111 | **两个再基线检查点**（P3a 超期 >50%、P7 中点完成度 <40%），且 P7 的上界 85 已经写进总估时区间。CTS 周转**单独计价**不折进阶段。若两个检查点任一触发，先跑 `inproc` 的证伪数字再决定是否继续——这与 `PLAN.md` R15 的退火思路一致 |
| **B-R15** | **在 GL setter 时刻推送**（而不是 draw validate 时刻）会让整件事变慢，且这是最容易被后续实现者做错的一处 | 写成规范条款并给出理由与证据（`DirectGLES.cpp:2044-2046` 的 Blaze3D per-batch blend toggle）；P2 的设备门直接暴露它（monolith-push 必须在 p50 **与** p99 逐线程 CPU 上落在噪声内）；两个高水位标记（`TouchBindPoint`、`NoteUnitTouched`）明确要求留在 tracker 走查里 |

---

## 13. 开放问题

1. **client 侧 dirty 走查的真实每 draw CPU 代价是多少？** 中心性能主张是"遍历搬走而不是翻倍"，树里没有任何度量。这是 P2 的头号数字，按逐线程 CPU、两台设备报。
2. **真实语料上纹理重铸拉取的实际发生率？** `imageBindableHint` 能预防主因，但整格式再生（`Managers.cpp:3950-4195`）在普通 `glTexImage` 格式变更上就触发。若 MC 或 Iris fixture 上的实测率非平凡，保留 LRU 从可选升为强制并需要真预算。
3. **`AcquirePersistentMap` 跨进程能不能成？** P0 spike B 第一周回答。未验证：`VK_KHR_external_memory_fd` 的 host-visible-coherent 支持在 Adreno 830 / Oppo Mali / lavapipe / Windows ANGLE lane 上的可用性；GLES 侧能否用 `GL_EXT_memory_object_fd` + `glBufferStorageMemEXT` 走同一条路（Espryt 今天的采纳走的是 `glBufferStorageEXT` + `glMapBufferRange(PERSISTENT|COHERENT)`，不是外部内存）。
4. **渲染状态的 wire 粒度定多少？** ~1.2KB 整块 vs 三个现有段 vs 逐 dirty 位子集。P0 落计数器、P2 用数据定。CSO LRU 的容量（暂定 64）同理。
5. **`MG_Util` 的切割缝在哪里？** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、像素/纹理格式处理器、POST 探针、loader；client 需要 glslang phase A/B 与反射层。`ProgramObject.h` 传递性 include `ShaderObject.h → ShaderCompileTask.h`，所以任何链接真 `ProgramObject` 的 client 都会拉进编译机制。**没有审计过是否存在一条干净的 Transpile-vs-Reflect 缝。**
6. **一份反射归档能服务三个消费者吗？** Espryt 读前端表，Magma 跑 SPIRV-Reflect，而 `DirectVulkan.cpp:161` 为 `glGetProgramResource*` 又反射了第二遍。没有确认一份 payload 能同时满足三者。
7. **viewport-array 回放能塞进一次 `draw_vbo` 吗？** 今天它从 14 个 draw 入口经 `ForEachViewportRoutingPass` 重发应用的 draw N 次，而 `EndViewportRoutingPasses` 会调 `InvalidateSyncedRenderState`（`DirectGLES.cpp:3841`）。没有验证单次 pipe 调用驱动下各遍之间观察到的状态是否与今天一致。
8. **`ResidentSubData` 的不对称该怎么收口？** null 项保住今天的行为，但拆分工作可能正是给 Magma 补一个真实现的时机——那是**行为变更而不是重构**，应当作为独立的 `dev` PR。
9. **`SEG_STAGE` 的上限定多少？** 现在多了四类字节（client 顶点数组、client 索引数组、multi-draw 参数块、client 解析的 indirect 命令块）。体量不变但都走 ring slot。需要 P8 之后用 MC in-world 与 Create 两类 fixture 的 `stage-*` 计数器给 p99 占用。
10. **`FramebufferSrgb` / `DepthClamp` 无存储是潜伏 bug 还是有意为之？** 六个 backend 消费者今天读到恒定 false（`RenderState.cpp:380, 428-429`）。**必须在渲染状态 blob 冻结之前回答**，因为它决定接口里要不要这个字段。
11. **P13 之后 `MOBILEGL_IPC_VALIDATE_SERVER` 还有对应物吗？** 方案 A 靠"server 侧保留 `MG_Impl` 校验器"把分歧变成 server GL error。方案 B 的 server 没有 `MG_Impl`，所以 split-only 的渲染 bug 没有第二意见。什么替代那个诊断能力？
12. **client 侧 restart 重写与 indirect-count 解析会不会改变可观察行为？** 两者今天都以 `m_valid=false` 加诊断的方式退出（`DirectGLES.cpp:4405-4409`、`:4685-4688`）；把 decline 移到 client 改变了哪个线程打这条日志，也可能改变它相对 GL error 的顺序。
13. **Magma 的两个内部 shader 烘焙后，uniform location 与 UBO 布局能否在没有活 `ProgramObject` 的情况下表达？**（`VulkanRenderer.cpp:4238-4241, 4319-4324, 8450-8452`）未做原型。
14. **推送模型会改变哪些按拉取模式调过的缓存命中率？** Magma 的 2048 槽 `VaoDrawMemo`、4 个 `SetupDrawSnapshot`、8 个 pipeline memo、8 个 `syncedTextureMemo`；Espryt 的 4096/256/64 槽 `TwinLookupMemo`（后者会消失）。幸存者的容量要在 P13 重调。

---

## 14. 对方案 A 文档与 `Feat/CS-Delta-IPC` 的复用清单

### 14.1 对 `PLAN.md` 的复用（本文的 §8.1 是权威列表，这里给的是"取/改/弃"总账）

| 判定 | `PLAN.md` 章节 |
|---|---|
| **原样取（不复述，以 `PLAN.md` 为准）** | §6.1（段布局、shm 矩阵、`SCM_RIGHTS` 第一优先、`SEG_SHADOW` 退休规则）；§6.2/§6.2a（`RingControl`、双向 doorbell）；§6.3（记录格式与运行期边界检查）；§6.4（WAR 纪律与 P4.5 零拷贝）；§6.5（ring 分配与背压）；§6.6 前三条；§6.7 第 2、5 行；§6.8（三档采纳与 POST 探针）；§7.1-§7.3；§8 末尾（真 fence 退休 + 三个独立 `dev` 修复）；§9-§9.3；§10（线程模型、核心放置、拆机顺序）；§11.1-§11.6；§12 第 1-3 层与 §12.4；§13（目录、一库两角色、submodule guard、三个 ctest 陷阱、`SPLIT` 接线、CI 门）；§15 P0 的卫生与两个 spike |
| **取并改** | §7.4（事件通道：**`on_log` 按严重级分级**，§7.4）；§12.2（隔离从四个进程全局降到**两个**）；§5.10（第 2、3 条逐字取，第 1 条缩成一个 `hasLiveHostWrites` 位）；§6.10（四类应用指针改由 cap 门控 lowering；**陈旧索引纪律逐字保留并加门**）；§5.9a（READ 面**编目**生成器改为**禁止**门）；§6.4 的拷贝账（删掉第 (3) 行，P1-4=3 / P4.5=2，`PLAN.md` 的"方案 B"目标结构性达成） |
| **弃** | §5.0、§5.1、§5.2、§5.4 的 replica 对象表规则与 `Fatal{IdentityDivergence}`、§5.6a、§5.7 的 Phase 1-4 分支与 `SetReplicaResolvedDrawProgram` 钩子、§5.9b 与全部 mutation 覆盖机制（`gen_impl_mutation_surface.py`、`MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::`）、§6.9 的 relink 档与 `MOBILEGL_IPC_PROGRAM`、§12 第 4 层的字节一致断言、`Server/ReplicaContext.*`、阶段 **P5**、风险 **R1** 与 **R6**、开放问题 **§17-5** |
| **新增（`PLAN.md` 没有的）** | `MG_Pipe/` 全套与六个生成器；`PipeInputs` + poison mask；`MOBILEGL_PIPE_VERIFY` 影子比对；残余值块与它的编译错误退役绊线；`on_texture_pull_request` / `on_texture_writeback` / `on_mip_levels_generated` 三个事件；纹理拉取的三条缓解与计数器；`HandleRecycleScenario` / `TextureRemintPullScenario` 两个场景；`RenderbufferObject::GetLifetimeId()`（**不含 `GetVersion()`**）；D21 的潜伏 bug 修复 |

### 14.2 对 `Feat/CS-Delta-IPC`（worktree `../MobileGL-CS`）的复用

`PLAN.md` §14 的 REUSE / CHANGE / DROP 判定**整体继承**。方案 B 的四处差异：

| 条目 | `PLAN.md` 判定 | 方案 B 的差异 |
|---|---|---|
| `docs/CS_Refactor/HandleSessionGeneration.md`（`546895aa`） | REUSE，三处修改，其中"handle 清单补 `RenderbufferObject::GetLifetimeId()` **与 `GetVersion()`**" | **只补 `GetLifetimeId()`**。`GetVersion()` 只是 replica 的 delta 触发器；推送模型里 `glRenderbufferStorage*` **本身**就是一次 pipe 调用 |
| `docs/CS_Refactor/backend_read_inventory.md` + `extract_backend_read_inventory.py`（`546895aa`） | CHANGE 成 `gen_backend_state_surface.py`，未知 accessor 一律 UNMAPPED 并编译失败 | **同意其修正**（删掉制造"0 UNMAPPED"的 `GetBuffer*`/`GetTexture*`/`GetProgram*`/`GetVertex*` 前缀兜底规则 `:234-241`，把真 pull point 与 signature handle 化分开统计），但**用途改变**：它变成 tracker 侧的**覆盖检查表**（G6），而真正的门是 §4.7.2 的**纯度门**——禁止那次读，比编目它强得多，且不会过期。**另外：`gen_impl_mutation_surface.py` 在方案 B 里不存在**（没有 replica 就没有 mutation 要 replay） |
| `MobileGL/RemoteClient/StateEmitter.h:39-307`（仅 emit 半边，`b50f3348`+`d96be9f3`） | CHANGE，各域字段遍历抬进 `WireMirror` | **更直接可用**：在 MGPipe 下那些字段集**就是** pipe 的状态对象 payload（`MGPFramebufferState`、`MGPVertexElements`、`MGPSamplerView` …），不再是一份 delta 目录。同样必须修的缺陷不变：GL name 换 lifetimeId（`:48-49, 85, 166-168, 203, 230`）、O(n²) 线性扫描换 handle map（`:175-181, 244-249, 253-258, 293-298`）、固定 6 attachment（`:232-236`）换 `MaxColorAttachments`、补上被跳过的 texture view（`:70-74`）。**applier 半边（`:312-501`）仍然不取** |
| `MobileGL/Protocol/mg_protocol_base.h`（`546895aa`） | REUSE（干净无依赖的词汇） | **同意**，且 **structSize-first 版本纪律是 B-R10 的对策**：payload 结构体从第一天就可增长 |

**DROP 名单完全一致**：`bfa.h`（480 行，"strict C ABI" 名不副实，且手抄的 60 字段 `MobileGLDynamicParameters` 是本项目已造成 481 例 CTS 失败簇的那类静默漂移炸弹）、`mgruntime_api.h` + `UtilRuntime/*`、`LocalSocketTransport` 的实现（每次 send 的 UAF、无上限分配、**`fd=-1` 硬编码**）、`ServerHost/main.cpp`（编译不过）、`StateEquivalenceTest.cpp`、`c7c9e346`+`29d721ef` 的 share-group sessioning、`b50f3348` 的 `RenderState::InstallParameters` + 裸 `public:`、`d96be9f3` 的 per-draw `fprintf` TRIAGE 指令。

---

## 附 A：接口调用目录速查表

> 完整 payload 定义见 `MG_Pipe/MGPipeTypes.h`；本表按 `PipeCalls.def` 的顺序。Flags：`A`=`kNeedsAck`、`B`=`kHasBlob`、`V`=`kVarTail`、`H`=`kHostSpan`、`R`=`kReplySlot`、`O`=`kOptional`（可为 null）。

### `MGPipeScreen`（14）

| 调用 | payload | flags | 取代 |
|---|---|---|---|
| `get_caps` | `MGPCaps` | R | 40 `pActiveBackendObject->` + 89 caps 读点 |
| `resource_create` | `MGPResourceDesc` | — | buffer/texture/renderbuffer 创建 |
| `resource_respecify` | `MGPResourceDesc` | — | `BufferBackendOps::Respecify` 泛化 |
| `resource_destroy` | handle | — | `OnDestroy` + 两个 `WeakPtr` GC 扫描 |
| `map_persistent` / `unmap_persistent` | handle | R, O | `AcquirePersistentMap`（改造期不碰） |
| `fence_create` / `fence_status` / `fence_wait` / `fence_destroy` | handle (+timeout) | — / — / R / — | `FenceSync`…`GetSyncStatus`（两值契约保留） |
| `query_create` / `_begin` / `_end` / `_available` / `_result` / `_destroy` | handle + kind | — | `BackendObject.h:230-256` |

### `MGPipeContext` — CSO（15）

`create/bind/delete` × `render_state` / `vertex_elements` / `sampler` / `sampler_view` / `shader`。
`create_render_state` 带 `B`（整块或变化段）；`create_shader_state` 带 `B`（SPIR-V + 反射归档）。

### `MGPipeContext` — `set_*`（14 + 1 临时）

`set_framebuffer_state` · `set_vertex_buffers` · `set_index_buffer` · `set_indirect_buffers` · `set_sampler_views`(V) · `bind_sampler_states`(V) · `set_shader_images`(V) · `set_shader_buffers`(V) · `set_stream_output_targets`(V) · `set_global_constants`(B) · `set_vertex_attrib_defaults` · `set_pixel_pack_state` · `set_patch_state` · `set_draw_program` / `set_dispatch_program`
**临时（P2..P13）**：`set_residual_value_state`(B)，带 `static_assert(sizeof(ResidualValueBlock)==0)` 退役绊线。

### `MGPipeContext` — transfer（12）

`resource_subdata`(B,V) · `buffer_subdata_resident`(B,O) · `resource_flush_range` · `resource_readback`(R) · `resource_copy_region` · `blit` · `clear` · `generate_mipmap` · `read_pixels`(R) · `get_texture_image`(R)

### `MGPipeContext` — 命令（10）

`draw_vbo`(H,V) · `launch_grid` · `memory_barrier` · `begin/end/pause/resume_stream_output` · `flush` · `present` · `set_swap_interval`(O)

### 反向：`MGPipeCallbacks`（9）

`on_gl_error` · `on_gpu_written` · `on_buffer_writeback` · `on_texture_writeback` · `on_texture_pull_request` · `on_mip_levels_generated` · `on_surface_changed` · `on_caps_invalidated` · `on_log`（**≤WARN 有损 / ≥ERROR 无损 + 速率限制**）

### 显式删除

`GetIntegeri_v` · `GetInteger64i_v` · `GetProgramiv` · `ShaderStorageBlockBinding`（折进 `MGPProgramDesc`）· `set_pixel_unpack_state`（不存在）· 压缩格式概念（不存在）· `pipe_transfer`（不存在）

---

## 附 B：环境变量与 CMake 选项

### CMake

| 选项 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 出货形态。开启后 `MG_Remote/**` 进 `SOURCE_FILES`，支持 `spawn`/`unix:`/`pipe:`。**两个**进程全局保持普通全局，GL 热路径无 TLS |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | CI/调试形态，隐含开启上者，额外加角色隔离 shim（方案 B 下只需隔离 `gPipeCtx` 与 `pActiveBackendObject`） |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 只服务 CI 的 `flatc-check`；默认构建图里没有 `flatc` |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON（P7+） | DirectVulkan 的 blit/depth-mipmap shader 烘焙成签进树的 SPIR-V，由 `MG_Test` 重跑树内 glslang 逐字节比对守新鲜度。**monolith 也受益**（少一次启动期 glslang 编译） |

> 注：`MG_Pipe/**` **不在任何 option 之后**——它是 monolith 的架构，永远进构建。

### 运行时（方案 B 新增）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | 迁移期按阶段推进；P13 后删除 | 子系统位图（0 = 全 pull）。**任何提交都能在同一份二进制上按子系统 A/B** |
| `MOBILEGL_PIPE_VERIFY` | 0 | 逐 draw 逐字段影子比对（~5-10× 慢，永不出货，P13 后保留为工装） |
| `MOBILEGL_PIPE_STATS` | 0 | 字节/调用/roundtrip/纹理拉取/残余块计数器转储 |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | 32 | 纹理重铸拉取的保留 LRU 预算上限（§7.5c） |

### 运行时（继承 `PLAN.md` 附录）

`MOBILEGL_TRANSPORT`（`monolith` 默认 / `inproc` / `spawn` / `unix:<path>` / `pipe:<name>`）· `MOBILEGL_IPC_SERVER_PATH` · `MOBILEGL_IPC_RING_MB`(8) · `MOBILEGL_IPC_STAGE_MB`(32，上限由实测定) · `MOBILEGL_IPC_PRESENT_CREDIT`(**1**) · `MOBILEGL_IPC_SPIN_US`(50) · `MOBILEGL_IPC_POLL_ESCALATE`(64) · `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`(64) · `MOBILEGL_IPC_ADOPT_TIER`(auto) · `MOBILEGL_IPC_SHADOW_SHM`(1，P4.5+) · `MOBILEGL_IPC_INLINE_PAYLOADS`(0，负面对照) · `MOBILEGL_IPC_SERVER_AFFINITY`(auto) · `MOBILEGL_IPC_STRICT_ERRORS`(0，诊断) · `MOBILEGL_IPC_AUDIT`(0) · `MOBILEGL_IPC_TRACE`(0) · `MOBILEGL_IPC_ATTACH`(空) · `MOBILEGL_IPC_RESPAWN`(0) · `MOBILEGL_IPC_IDLE_EXIT_S`(30)

**删除**：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）· `MOBILEGL_IPC_VALIDATE_SERVER`（server 没有 `MG_Impl` 校验器——替代诊断手段见开放问题 11）

**保留的既有负面对照开关**：`MOBILEGL_ESPRYT_DISABLE_UBO_RING` · `_UNPACK_RING` · `_UPLOAD_RING` · `_INVALIDATE_FLUSH` · `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` · `MOBILEGL_COHERENT_AS_FLUSH`（**在拆分模式下照常生效**，这样两个 `coherent_as_flush: true` 的 Create fixture 在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义）
