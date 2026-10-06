# P13 计划：退役 pull 路径，monolith 换到记录臂

> 2026-10-06，只读核查（树 `p13` = `origin/feat/disaggregated` `aee0632a`）。范围与出口门见 [`README.md`](README.md)；P8 留下的缺陷见 [`../DEBTS.md`](../DEBTS.md) ID-P8-13 / ID-P8-14 两行与 [`B`](../p8/B.md)、[`C`](../p8/C.md)、[`D`](../p8/D.md)、[`E`](../p8/E.md)、[`SV`](../p8/SV.md)。本页只写计划，代码一行未动；实施等集成方点头。行号是本树当日的，实施时以符号为准。

## 0. 结论

| # | 结论 | 依据 |
|---|---|---|
| 1 | **monolith 的「pull」已经不在编译期，而在运行期。** feat 上 CI 的主构建本来就是 push + split（`MOBILEGL_CI_DISAGGREGATED=ON`），pull 只剩 `build-linux-monolith-control` 一个库给 retrace-split 当阴性对照；`SnapshotFromGLContext` 早已只在 verify 下编译。真正要做的是：后端约 260 处 `Transport != Monolith` 分支里，monolith 走的都是读前端对象 / 前端影子的臂 | Espryt 113 处、Magma 63 处运行期分支；push 下 `MGB_CTX` = `&gPipeInputs`，但 `kFatal` 类字段（`GetTextureObject`、`GetBoundVertexArray`、`GetProgramForDraw`……）装的仍是前端 `SharedPtr`，后端照样解引用 |
| 2 | **记录臂只在 `MOBILEGL_BUILD_DISAGGREGATED` 下编译。** Espryt 113 处运行期分支有 108 处在该宏内；记录臂读 CPU 字节靠 `MG_Remote::Server::StagedShadowStore` / `StagedTextureStore`；Magma 整个 wire 臂（14 个 `Wire*` 文件、`VulkanRenderer.h:634-918` 的 `m_wire*`、按句柄的纹理臂 `VkTextureManager.cpp:1869-3265`）整体在该宏内。不带 MG_Remote 的 push 库（FCL 内嵌形态）今天**不可能**走记录臂 | `Config.h:749`（无该宏时 `Transport` 是 `constexpr Monolith`）；`WireDraw.inc:2` |
| 3 | **ID-P8-13 九条、ID-P8-14 两条缺陷全部在记录臂上已修好**，monolith 换臂即消失；回归用例已经存在，是 monolith 臂上按名跳过的那批（§3），取消跳过就是证明 | P8 B / C / D / E / SV 各包的 red-once「M」行 |
| 4 | **monolith 不能整套照搬 split 的存储语义。** split 下 server 暂存 store 复制字节（`CopiesIntoServerStorage()`），monolith 下 store 别名前端影子（`StagedShadow.h` 的 `m_copies=false`），不复制正是 monolith 不翻倍内存的原因（MC 26.3 有 441.5 MiB 存储）。记录臂里「GPU 写后刷新」在别名 store 上是空操作（`Managers.cpp:3755`），所以换臂要补一条别名 store 的刷新路径（§1.4），不能直接把 monolith 改成复制 | `Managers.cpp:1287`、`:3755`、`:3806` |
| 5 | **`static_assert(sizeof(ResidualValueBlock) == 0)` 在 C++ 里写不出来**（完整类型 sizeof ≥ 1）。出口门按「类型删掉」兑现：删结构、删 op 46 的发射与应用（线上行只追加，op 46 改为退役行、解码拒收，同 op 50 先例）、bit 4 退役 | `MGPipeTypes.h:1246-1273`；`PipeCalls.def:237-239` |
| 6 | **G1 不在 CI 里**，只在树外 `gate.sh`；P13 删 pull 后 G1 失去对象，需要一条裁定与接班门（§4.3） | `scripts/symbol_report.py` 在 CI 只跑 `--self-test`（`test.yml:3488`） |
| 7 | 删宏有**静默跳过陷阱**：24 个单元测试源用 `#if !MOBILEGL_PIPE_PUSH GTEST_SKIP()`、12 个套件用 `MGL_DECLARE_PULL_SKIP`、集成 CMake 三处 `if (MOBILEGL_PIPE_PUSH)`；宏一旦先于守卫消失，这些全部「绿着跳过」。另 `runtime_mode_proof.py --mode monolith` 把 monolith 定义成「没有 push」，push monolith 会让 `build-linux-monolith-control` 失败，进而让 retrace-split 整个矩阵**被跳过而不是变红**（`test.yml:3065`）。这两处必须在任何删除之前先改（W1） | `scripts/ci/runtime_mode_proof.py:33-42` |

## 1. 清点

计数方法：`grep -E '^\s*#\s*(if|elif|ifdef|ifndef).*<宏>'` 数行，一行同时提两个宏两边都计。全树（含测试）：`MOBILEGL_PIPE_PUSH` 613 行 / 99 文件，`MOBILEGL_PIPE_LEGACY_MEMOS` 49 行；`MGB_CTX` 409 处 / 28 文件；`MOBILEGL_BUILD_DISAGGREGATED`（MG_Remote 之外）约 700 行。

### 1.1 编译期：`MOBILEGL_PIPE_PUSH` / `MOBILEGL_PIPE_LEGACY_MEMOS`（删 `#else`，留 push 体）

| 子系统 | 位置（PUSH / LEGACY 行数） | 删掉意味着什么 | 注意 |
|---|---|---|---|
| 前端：状态对象身份与死亡通知、mutation 记账 | `MG_State/**` 约 70；`PipeMutation.h:500`；`GL_Framebuffer.cpp` 18、`GL_Texture.cpp` 4、`GL_Buffer.cpp` 2 | 只去 `#if`，体保留；139 处里只有 6 处有 `#else` | 唯一有行为的 `#else` 是 `PipeInputsSwitch.h:22-26` 的 `MGB_CTX = pGLContext` |
| 前端 `MG_Impl/Pipe/*` | 14 个头整文件守卫（`Tracker.h`、`CsoCache.h`、`*Emit.h`……） | 去守卫 | `OwnedDrawInputs.h` 是 `DISAGG && PUSH`，W5 要放开 |
| CMake 源表 | `CMakeLists.txt:579-604` 七个 push-only 源 | 并入主表 | `:548-577` 三条强制规则删 |
| Espryt 缓冲 | `Managers.cpp:1690-1938`（pull 的 `BufferBackendOps`，`SetBackendResource`）；`Mh:1089-1332` | 删 `#else` | 遗留 `EnsureBufferResource`（`M:4434+`，`PipeResource::m_backend`）**不在 `#if` 里**，是运行期 bit 7 关时的臂，W6 才能删 |
| Espryt VAO / 顶点 | `DG:1101-1307`、`M:6013-6250`（遗留 `SyncToBackend` 226 行）、`M:6716-6836`（遗留 fp64） | 删 | 遗留 fp64 臂是**唯一**调 `SyncGpuWrites` 的那条（缺陷 c 的「正确」版本），删它之前 W4a 先修句柄臂 |
| Espryt 纹理 / mip | `DG:3164-3490`（`#else`@3313 pull 单元快照）、`M:7073-11398` 多段、`Mh:2052-2475` | 删 `#else` 与 `!LEGACY` 不可达桩（`M:10794`、`:11065`） | — |
| Espryt 帧缓冲 | `DG:200-332`（`!PUSH \|\| LEGACY` 槽缓存）、`DG:12171-12224`（pull 遍历）、遗留 `g_fboTwinLookupMemo`（`DG:6619`） | 删 | — |
| Espryt sampler / program | `DG:7203-7416`、`DG:6240-6522`、`Mh:3167`（`ProgramBuildSource = ProgramObject`）、`M:15984`（`#if !PIPE_PUSH`） | 删 | push-monolith 下 `ProgramArchiveSource` 仍由前端 `ProgramObject` 现造（`Mh:3523-3528`），这是运行期问题（W4c） |
| Espryt 注册表 / 遗留 memo | `DG:109-187`（`OwnerEquals` + 三个 `TwinLookupMemo`）、`Mh:664-671`、`M:472-481` 等 | 删 | `espryt_memo_purity.py` 的锚点会全部失配（§4） |
| Magma | PUSH 44 / LEGACY 13：`ComputePipelineStateHash`（`VR:5317-5408`）、遗留 VAO memo（`VertexInputStateFactory.cpp:127-215`）、`MagmaPipeArms.h:96-130`；`#if !DISAGG` 的隐藏 GL 程序（`VR:1489-1578`、`:4849-4908`）、`g_programResourceCaches`（`DV:143-379`） | 删 | `g_programResourceCaches` 只在 `!DISAGG` 编译——它在 push 无 MG_Remote 构建里**活着**，W5 换成 `LinkArtifacts::storageBlocks` 后删 |
| `MGB_CTX` / `_LIVE` / `_IDENTITY` | 后端 11 个文件：`VulkanRenderer.cpp` 135、`DirectGLES.cpp` 114、`DirectVulkan.cpp` 49、`Managers.cpp` 24、`UniformManager.cpp` 23…… | 机械改写为 `MG_Pipe::gPipeInputs` 的访问器（W3） | `scripts/gen_pipe.py:466` 的正则 `MGB_CTX\|pGLContext` 同步改 |
| 配置 | `ConfigLoader.cpp:396`（pull 默认位图 0）；`Config.h:419` `Features.PipeLegacyMemos`、`:420-423` `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | 删；`TEXEL_RETAIN_MB` 零消费者（另删 `tools/trace_replay/trace_env_overrides_test.cpp:83-84` 的字符串） | — |
| 统计 | `PipeStats.cpp/.h` 17 处，无 `#else` | 去守卫 | `ByteClass::ResidualValueBlock` 随 W3b 删 |

### 1.2 运行期：monolith 臂与记录臂（P13 的主体）

分三类。**K 保留**：同地址空间的合法捷径，不是读前端状态；**F 换臂**：monolith 读前端对象或前端影子，改走记录臂；**D 删除**：换臂后无人用。

| 子系统 | 运行期分支（代表行） | monolith 今天做什么 | 记录臂 | 类 |
|---|---|---|---|---|
| 缓冲 ensure / flush | `M:2476`、`:2644`、`:3510`、`:4095`（无宏守卫）、`:4166`、`:4203`、`:4311`；Magma `VkBufferManager.cpp:239`、`:1215`、`:1374` | `liveHostBase` 取前端 `MappedData()`、`SyncPersistentMappedRange`、问前端 `IsMapped`；Magma 把 `VkBufferResource` 挂在前端 `PipeResource::m_backend` 上 | `StagedShadowStore`、`hostBytes` 来自 store、句柄表 | F |
| persistent map 捐赠 | `BufferObject.cpp:308`、`:891`、`:976`；`PipeRoute` 的 `MapPersistent` escape | 后端返回 GPU 映射指针，前端收养为 store 基址（设计 §12 D-B4：monolith 改造期不动） | split 是 T0 / T2 | **K**（进程内的 T0；保留，W4a 里只把入口统一到 `map_persistent` 记录） |
| 别名 store | `Managers.cpp:1287`（`StagedShadowStore(false)`）；`TextureEmit.h:1568-1600`（纹理记录 blob 是前端层存储的裸地址） | 不复制 | 复制 | **K**，但要补刷新（§1.4） |
| indirect 命令 / count | `DG:496`、`:8661-8784`、`:9637`、`:9678`、`:9806`、`:9844`、`:10082`；`DV:652-953`；`VR:13416+`、`:13504-13511` | 前端 `GL_DRAW_INDIRECT_BUFFER` 槽、只 `SyncPersistentMappedRange`；Magma CPU 读 count、无 compute→indirect 屏障 | `VerbIndirectBuffer` + `SplitHostBytesForCpuRead`；Magma 原生 `DrawWireIndirectNative` + `TakeWireIndirectReadBarrier` | F |
| client 数组 / 索引 | `DG:2653`、`:2714`、`:2746`（三处无宏守卫）、`:9235`、`:9338`；`MultiDraw.cpp:183`；`DV:1254`、`VR:13635` | 后端读 `MGB_CTX` 的 VAO、前端 IBO `SyncGpuWrites` + `MappedData` | 早返回（client 在 GL 线程做 `OwnedDrawInputs` 快照）；`IndexBuffer.Res` + `SplitHostBytesForCpuRead` | F |
| SSBO / image GPU 写标记 | `DG:951`、`:4487`；Magma `UniformManager.cpp:1632`、`:1885`、`VR:12636` | 走前端绑定、`MarkBufferGpuWritten(前端对象)`、`MGPipeAnnounceBufferGpuWritten` | applier 视图 + `serverGpuWritten` / `MarkWireBufferGpuWritten` | F |
| fp64 收窄 | `M:6893` | `hostBytes` 不同步 GPU 写（缺陷 c） | `SplitHostBytesForCpuRead` | F |
| 有中途同步的 draw（Magma） | `VR:3730`（最大索引扫描）、`:4223`（GL_DOUBLE）、`:4557`（restart 重写） | 录制中途 `SyncGpuWrites` → 提交并结束当前 CB → SIGSEGV | 每个 `ReadWireBuffer` 都在 `BeginCommandRecording` 之前（`WireDraw.inc:355`、`:430`、`:468`） | F |
| XFB | `DG:1731-2427` 十二处 | 独立角色状态、前端捕获程序与目标、`WritebackFromBackend` 写前端对象 | applier span + `OnBufferWriteback`（切片） | F |
| 纹理存储 / view / 重铸 | `M:7081`、`:8402`、`:9068-9133`、`:12001`、宏 `M:8695` | 前端 `IsTextureView`、前端 `RequireImageBindableStorage`（`M:7399`，读回驱动层失败时退回前端影子）、读前端层纹素 | `SyncTextureViewToBackendByRecord`、`RequireImageBindableStorageByHandle` | F |
| mip | `DG:13354`；`VR:12204`、`:12212-12420` | 前端单元、CPU 滤波前端影子（缺陷 d）、从 level 0 生长（e）；Magma 无 BLIT 颜色格式直接跳过（M3）、深度 mip 不传 view 类型 | `GenerateMipmapByRecord` / `GenerateWireMipmap` | F |
| copy-tex / copy-image / readback | `DG:11530`、`:12788-13831`、`:14127-16191`；`VR:9663`、`:10305`、`:10624-10944`、`:11224`、`:11926` | 前端端点、影子镜像只做 RGB9_E5（缺陷 f）、PBO 经 `WritebackFromBackend` 写前端 | 按句柄端点、`FollowCopyImageInStagedStore`、`ReadWirePixels` | F |
| 单元 / sampler / image 单元 | `DG:367`、`:3424`、`:3566`、`:4597-4696`、`:7230`、`:7354`；`VkSamplerManager.cpp:476-482` | 遍历前端 `GetTextureUnitObject`、从前端 `SamplerObject` 铸造 | sampler-view / state 窗口、POD sampler | F |
| 帧缓冲 / render pass | `DG:325`、`:4850`、`:11365`；`M:12090-12515`；`VR:5760`、`:5994`、`:8120`、`:8524`；`VkRenderPassManager` | `HandleOf(前端 FBO)`、以前端 FBO 为键的 render pass | `m_pushedSyncHandle`、`DrawFramebuffer()` 记录、`m_wireDrawPass*` | F |
| program / uniform | `Mh:832` → `DG:6925-8862`（push-only）；Magma `UniformManager` 十几处 `IsWire()` 分叉 | 前端 `ProgramObject`；Magma 占位纹理是前端 `TextureObject`（ratchet 里 63 个 `# P13` 符号） | `DrawProgram` 等记录；`WirePlaceholderImages.inc` | F |
| swapchain | `SwapchainObject.cpp:352-388` | 直接写前端默认 FBO 的 `TextureObject2D` | `OnSurfaceChanged` | F |
| query | `DV:1663`、`:1714` | 暂停期间 `GL_PRIMITIVES_GENERATED` 的 CPU 增量 | 无 | F（换臂前先核对 wire 臂是否丢了这个增量，可能是 split 侧缺陷） |
| residual 填充 | `PipeFill.cpp:3609-3611`（`contextValuesWireLive` monolith 为 false）、`:2198-2289`（7 个转发访问器，`InvalidateCompileEnv` / `RecordError` 还会写前端） | 每 verb 把未被记录覆盖的字段从活的 GLContext 拷进 `gPipeInputs`，含前端 `SharedPtr` 字段 | split 下值类字段由记录携带 | F（W4d：值类字段改由记录供给；`kFatal` 前端指针字段在后端不再读后从 `PipeInputs` 删） |
| 设备特性 | `VR:15505-15580`、`:16128`、`:16381` | monolith 不开 wire 用的可选扩展（renderpass2 深度解析、stencil export、AHB 导入、robustness2 null descriptor）；`descriptorIndexingCore` 在 monolith 下把 1.2 设备当核心（实例只请求 1.1，疑为既有 monolith 小缺陷） | 都是可选、缺失时具名拒绝 | F（换臂后 monolith 也开这些可选扩展：**改动设备创建，需设备冒烟**） |
| 会话 / 生命周期 | `BackendObject_DirectGLES.cpp:901`、`:1189`；`ContextEpoch.h:19`；`DG:16737`、`:18343-18465`；`Utils.cpp:55`；`M:245`；Magma `BackendObject_DirectVulkan.cpp:347-756`、`FrameContext.cpp:231-247` | 无设备丢失闩、无 server 生命周期、总是原生 make-current | server 生命周期 | **K**（不是数据臂，留着 `Transport` 判断；但 `FrameContext.cpp:227-251` 的 present 屏障与 `VR:7954-7960` 的 HOST 阶段遗漏是 monolith 侧的 Vulkan 同步错误，换臂时一并对齐） |
| 槽 / 角色守卫 | `SlotAllocator.cpp:25-56`；`PipeFill.cpp:362`、`:1276-1334`；`PipeApply.cpp:1785-4006` | 空操作 | split 下守卫 | K |

### 1.3 换臂后可以删的 monolith 粘合（D 类，W6）

- Espryt：`g_fboTextureSyncList*`（`DG:3637-3657`、`:4101-4143`，无宏守卫）；`SlotTables.h` 以前端对象铸句柄的四个入口（`GetOrCreate(StatePtr)` / `HandleOf` / `NoteStateForHandle` / `StateForHandle`，`:85-104`、`:538-590`）；`MGPipeFrontendKeyedRegistryScope`（DG 14 处、M 11 处）；`ResolveBufferBindingSubsystemArm` 的 monolith `false`（`DG:723`）；`MGPipeUnmigratedEmulation` 的三个 monolith 名（`generate-mipmap-storage` `DG:11760`、`generate-mipmap-cpu-fallback` `DG:13009`、`get-tex-image-shadow` `DG:15417`，`PipeCatalogueTest.cpp:1451` 名单同步清空）；XFB 角色状态 `g_roleXfbState`。
- Magma：`MagmaPipeIdentityTables` / `m_pipeIdentity`（`MagmaPipeArms.h:319-610`）；VAO memo 表与 fast-path 快照（`TrySetupDrawFastPath` 只在 monolith 跑，`VR:6702`——**换臂前先量**，它可能是 monolith 每 draw 成本的大头）；`VkTextureManager::m_aliveObjects`；`VkRenderPassManager` 以前端 FBO / RBO 为键的条目；`UniformManager` 的三组前端占位纹理；`g_vulkanBufferBackendOps`；`RejectWireLegacyBuffer` 等守卫。
- 前端：`PipeResource::m_backend`（`PipeResource.h:201-211`）、`BufferObject::Get/SetBackendResource`、`g_bufferBackendOps` 遗留表（bit 7 关时的臂）；`ProgramObject::m_backendStateVersion` 的 `g_programResourceCaches` 读者；`PipeInputs` 里的前端指针访问器（`PipeInputs.h` 不再需要 `MG_State/GLState/Core.h`，verify 臂另起头文件）。

### 1.4 别名 store 的 GPU 写刷新（换臂里唯一的新设计）

记录臂读者（restart 重写、multi-draw 重定基、`*IndirectCount`、`gl_BaseVertex`、fp64 收窄、XFB scatter 的捕获前字节）在 split 下经 `SplitHostBytesForCpuRead` 从 server 自己的 GL buffer 回读进复制型 store。monolith 不复制，这一步今天直接返回旧指针。方案：

- **首选**：别名 store 上的刷新走具名反向回调——回读整 store（或读者需要的范围），以 `OnBufferWriteback` 交给前端落进它自己的影子（monolith 下回调直调、同步），清前端 `m_gpuWritePending`；store 的别名指针随之看到新字节。这正是 split 下 client 收到写回后做的事，monolith 只是同步完成；后端不写前端内存，前端也不读后端。
- **备选**（首选在某个读者上不成立时）：按条目「首次刷新即提升为复制」——只有被 GPU 写过、又被 CPU 读者消费的 store 付一份复制（实际是 indirect / count / 计算写的 EBO，量小）。
- 不选：monolith 整体改复制型 store（内存翻倍，P11 刚把峰值内存压下 18–42%）。
- 纹理侧不需要新机制：记录臂的 mip / 重铸 / 回读都先读驱动层，store 只作退路，别名下退路即前端影子（与 dev 的 `568090f0` 同路）。

## 2. 出口门

| 门（README） | 要做什么 | 主机可验 | 备注 |
|---|---|---|---|
| `static_assert(sizeof(ResidualValueBlock) == 0)` | 字面写不出（C++ 完整类型 sizeof ≥ 1）。按「类型删除」兑现：删结构、`PipeFields.def:179-183`、`EmitResidualValueState`（`PipeFill.cpp:3227`）、`Mono_`/`Wire_`/编解码臂、applier 的 `Residual` 与 `PipeResidualDiverged` 比对；op 46 改退役行（解码拒收），协议修订 +1（`protocol_revision_pins.json`）；bit 4 退役不复用；`gen_pipe.py:1053-1065` 不再发 size 断言。它守的意图（「哪个 capability 没被记录带上」）由 verify 比对器接管：加一条 verify 车道用例，用 `MOBILEGL_PIPE_VERIFY_CORRUPT` 打一个 capability 字段，必须红 | 是 | 单元：`PipeCatalogueTest.cpp:317-361`、`RenderStateSpansTest.cpp:1000-1169`、`PipeWireCodecTest.cpp:1760`、`:2263-2281`、`PipeStatsTest.cpp:62`、`:538`、`TrackerTest.cpp:1233` 改为断言退役行被拒 |
| 三道纯度门在非 verify 构建转绿 | A 包含闭包：`check_include_closure.py` 的 clang 模式不带 `-D`（`:152-159`），今天按 pull 配置读头文件——删 `#if` 后自然量到 push 头；`PipeInputs.h` 去掉 `MG_State/GLState/Core.h`（verify 另头）。B server 镜像不引用 `MG_State::GLState` / glslang：link ratchet 172 → 0（94 条 `# P13` 随 W6 的 monolith 臂删除落下，其余 78 条按 bucket 逐条归属，`MG_Remote::Client::*` 那几条要看分区表是否该算 SHARED）；W8 把它变成真链接。C `MG_Backend` 零 `pGLContext`：今天已只剩 `PipeInputs.h` 一处注释 | 是 | ratchet 的注解不被检查——W6 起在脚本里加「`# P13` 注解行必须为 0」的断言，防止回潮 |
| MGPipe recorder 金标 | MG_Test 今天没有 recorder，只有 `ResourceEmitTest.cpp` 的 Spy ops、`BufferTest.cpp:1567` 的 ZeroCopyMockBackend、`TextureUploadShapeScenario.cpp:190` 一个形状金标。新建：在 `MGPipeContext` / `MGPipeScreen` 表上装记录器（monolith 路由表本来就可替换，`PipeRoute.cpp:330-390`），每 verb 记下记录流与 `gPipeInputs` 的规范化哈希；fixture 用现有单元场景 + 2–3 条短 trace；金标入库；阴性对照：改一个 emitter 的字段必须红 | 是 | 开放问题 12 已写明它只覆盖推送内容 |
| monolith 逐线程 CPU 不差于 P0 基线 | 红米 `2f7cbe2e` 定频配对 A/B，rd12 / openra / MC 26.3。注意：用户 2026-09-08 定「性能对 pull 臂只记录、不设门」，P2 / P3a 已实测 push 比 pull 多 6–12%（VAO 密集 +27–30%），本门按字面今天大概率不过 | **否，延后** | 见开放问题 2；主机只做同机 `perf stat` 粗看，不当门 |
| 链接棘轮 `# P13` 符号 | 94 条（P7 记 86，后续 `d49df3549` 多标了 7 条裸 `# P13`、1 条双注）：63 条 UniformManager 占位纹理、14 条 sampler 读前端 `SamplerObject`、6 条读前端 FBO / RBO / VAO / buffer、4 条 `MGB_CTX` 访问器 / 回读打包器、7 条槽分配器与 apply 线程拒绝函数。前五组随 W6 删；`HasOpenTransformFeedbackSpan`、`InvalidateCompileEnv` 两个 wire 臂也用，要 pipe 侧替身 | 是 | 每次落下都在同一提交里重写基线 |
| D12 模块 target | `SOURCE_FILES` 一张平表喂 `MobileGL` 与 `MobileGL_s`。拆成 OBJECT 库：`mg_util`、`mg_pipe`、`mg_frontend`（MG_State + MG_Impl）、`mg_backend`（两后端 + 从 MG_Remote 挪出的暂存 store）、`mg_remote_transport`、`mg_remote_client`、`mg_remote_server`；加一个 server-only 链接检查目标（不链 `mg_frontend`，必须链得过）。link ratchet 的 PARTITION 表就是边界的书面形式 | 是 | 放在 W6 之后，否则 monolith 臂还把前后端缠在一起 |
| 幸存缓存容量重调 | CSO LRU 64（`CsoCache.h:54`）、sampler CSO 256（`SamplerEmit.h:210`）、六个 memo 门、`sve`≈draw 数的线索（开放问题 14）。主机语料上用 `MOBILEGL_PIPE_STATS` 量 `csom` / `csob` 与各门命中率，定容量；设备复核延后 | 主机可定初值；设备延后 | 放在换臂之后（monolith 的命中率换臂后才有意义） |

另加两条 P13 自己的门：**skip 普查**（§4.2，每波不增加跳过）与 **无 MG_Remote 符号**（§4.3，G1 的接班之一）。

## 3. 缺陷（ID-P8-13 / ID-P8-14）

全部在记录臂上已修；「换臂即修」指 W4 换臂后自动消失。主机能红的都已有 red-once 记录；Magma 只能靠 CI（本机不跑 DirectVulkan 集成）。

| # | 后端 | 缺陷 | 根因 | 换臂即修？ | 回归用例（今天在 monolith 按名跳过） | 波 |
|---|---|---|---|---|---|---|
| a | Espryt | `*IndirectCount` 读未同步影子 | `DG:9731-9768` 只 `SyncPersistentMappedRange` 后读前端 `MappedData()` | 是（`DG:9690` `SplitHostBytesForCpuRead`）+ §1.4 刷新 | `GpuWrittenDrawInputScenario` 的 `ACountAComputeShaderWrote…ElementsIndirectCount`（`:484`）、`…ArraysIndirectCount`（`:522`）、`ACountAnAtomicCounterProduced…`（`:700`）；`WireIndirectDrawScenario.ComputeWrittenCountWordIsTheOneTheGpuReads`（`:307`） | W4a |
| b | Espryt | 原生 indirect 的 `gl_BaseVertex` 读旧影子 | `ResolveIndirectCommandBytes`（`DG:533-541`）只同步 persistent 映射 | 是（`CurrentIndirectCommandBytes` `DG:8609-8623`） | `ABaseVertexAComputeShaderWroteReachesGlBaseVertex`（`:562`） | W4a |
| c | Espryt | fp64 收窄读未同步影子 | push 句柄臂 `M:6839-6907` 读 `hostBytes`（P3a M-3 声明的偏差），正确的遗留臂只在 LEGACY 下 | 是（`M:6891-6896`）+ §1.4 | `DoubleVerticesAComputeShaderWrote…`（`:758`，Espryt 半） | W4a（W3 删遗留臂不改变 push-monolith 行为：push 本来就走有缺陷的句柄臂） |
| a′ | Magma | `glMultiDrawArraysIndirectCount` CPU 臂读未同步 count | `DV:752-767`；`MultiDrawElementsIndirectCount` 的 CPU 退路 `VR:13504-13511` 同病，且是在 `SetupDraw` 开始录制之后读 | 是（`DrawWireIndirect` 原生 count） | `…ArraysIndirectCount`（`:522`，Magma 半）；`WireIndirectDrawScenario` 的 Magma 半（`:319-323`，今天是**静默早返回**，后半段从不跑） | W4a |
| b′ | Magma | 录制中途 `SyncGpuWrites` → SIGSEGV | `VR:3730`、`:4223`、`:4557` 在 `SetupDraw` 录制中回读，`Ops_ReadbackFromGpu` 冲刷并结束了仍持有的 CB；`DV:1254-1270` 的预同步只覆盖普通 `DrawElements` | 是（wire 臂所有 `ReadWireBuffer` 先于录制） | `RestartIndices…` 六例（`:431`、`:446`、`:464`、`:630`、`:651`、`:671`）、`DoubleVertices…`（`:758`，Magma 半）；另 `BuildClosedLineLoopIndices`（`DV:1183`）同模式，无用例——W4a 加一例 | W4a |
| d | Espryt | RGB16F / RGB32F CPU 滤波读陈旧前端影子 | `GenerateThreeChannelFloatMipmapOnCpu`（`DG:12994-13065`）在 `DG:13387` 无条件先走，连驱动都不试 | 是（`GenerateMipmapByRecord` 先原生，再 server 读驱动层） | `GenerateMipmapServerScenario` 的 `Rgb16f/Rgb32fRenderTargetBaseGeneratesFromTheGpuWrittenLevel`（M1） | W4b |
| e | Espryt | R11F / 深度链无视 BASE / MAX，从 level 0 生长 | `EnsureGenerateMipmapStorageAllocated`（`DG:11747-11787`）、`GenerateDepthTexture2DMipmap`（`:12626`）、`GenerateColorTexture2DMipmap`（`:12657`） | 是（`GenerateMipmapWindowByRecord` `DG:11826`） | `PackedFloat` / `Depth` × `MutableMaxLevelBelowTheChain`、`MutableBaseLevelOne…`（M2） | W4b |
| g′ | Magma | 无 BLIT 的颜色格式没有着色器 mip 臂 | `VR:12291-12296` 打日志返回 | 是（`WireFramebuffer.inc:1506-1574`） | `PackedFloat*` 三例（M3） | W4b |
| f | Espryt | 拷贝后 `glGetTexImage` 读拷贝前影子 | 镜像只做 RGB9_E5（`DG:13840-13843`）；`GetTexImage` 在 `DG:16074`、`:16133`、`:16147` 退回影子 | 是（`FollowCopyImageInStagedStore` 或驱动写标记后具名拒绝） | `CopyImageStoreReadbackScenario` 今天 monolith 不跳过、但主机驱动能 attach，走不到影子；monolith 加 `StoreRead.` 等价登记（旋钮 `MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=13x7` 今天只在 split 臂由 server 读，W4b 让 monolith 后端也读） | W4b |
| 14a | Espryt | 每次 indirect draw 在 CB 有 GPU 写时整 buffer 同步回读（`62bfe461`） | `SyncClientSideVertexArraysForIndirectFetch`（`DG:2711-2735`，无宏守卫）在判断「有没有 client 数组」**之前**就 `SyncGpuWrites`，每条命令、每个视口遍一次 | 是（记录臂 `DG:2714` 早返回，client 用 `OwnedDrawInputs`；**不需要 MG_Remote**，可最早落） | 无现成用例：W4a 加白盒计数（monolith 下一次带 GPU 写的 multi-draw indirect，回读次数 = 0）；红米上原症状是挂死 > 10 分钟（P8 S），设备复核延后 | W4a |
| 14b | Magma | split 构建 monolith 深度 mip 不传 view 类型 | `GenerateDepthMipmapWithShader`（`VR:4918-4962`）的 `WireImage` 默认 2D；调用方断言在 INFO 下编译掉 | 是（`WireFramebuffer.inc:1539-1540` + `WireDepthMipmap.inc:70-72`） | `WireMipShapeScenario.Depth1DChainWithoutANativeBlitDeclinesByName`：今天要 `MGITEST_MAGMA_FORCE_SHADER_MIPMAP`（server 读），只登在 split / spawn；W4b 加 monolith 登记 | W4b |
| e′ | Magma | compute 写 → indirect 读无依赖（P8-D §6 推断） | dispatch 后与 indirect 前都不记屏障；render pass 外部依赖不含 `DRAW_INDIRECT` | 是（`TakeWireIndirectReadBarrier`） | `WireIndirectDrawScenario.ShaderWrittenCommandsWithoutAnApplicationBarrierAreOrdered`（`:347`）、`WireIndirectDispatchScenario.ShaderWrittenGroupCounts…`（`:269`）：monolith 跳过；计数断言只在 inproc 可读——W4a 让 `WireIndirectPeek` 在 monolith 也可读 | W4a |
| h | Espryt | 重铸保留路径重放前端影子（设计 §8.4 记的已知 dev 缺陷） | `RequireImageBindableStorage`（`M:7399-7565`）读回驱动层失败时退回影子，并改写前端影子与脏标 | 是（`RequireImageBindableStorageByHandle`）；注意它在 monolith 下今天会跳过主体只清 `m_isInitialized`（`M:7233-7241`），换臂时必须去掉这个分支 | `TextureRemintPullScenario` / `ImageBindableRemintScenario` 已不跳过；后者为 ID-P9-11 钉了 `glMemoryBarrier`，保持 | W4b |

另有 monolith 侧的非 P8 跳过，换臂后一并取消（不是本阶段承诺修的缺陷，但它们同属「monolith 臂与记录臂不一致」，换臂后若仍红就各开一行债）：`DepthStencilReadbackMatrixScenario.AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines`（Magma，`:567-574`）、`InvalidImageUnitScenario` 全部（Magma monolith，`:168-176`）、`TriangleScenario` 的 verb 戳用例（`:327-333`）。

## 4. CI 影响与不降级的替代

### 4.1 逐项

| 项（`test.yml` 行号） | 今天守什么 | P13 动了什么 | 替代（同一意图） | 波 |
|---|---|---|---|---|
| `build-linux` `-DMOBILEGL_PIPE_PUSH=${MOBILEGL_CI_DISAGGREGATED}`（`:123-126`）与 `runtime_mode_proof --mode monolith`（`:137-141`） | 主运行库的形态；feat 上已是 split | 选项删 | 去掉该 `-D`；`runtime_mode_proof` 的 monolith 改定义为「无 MG_Remote 符号」（`:33-42`、自测 `:126-132` 同改） | W1 / W3 |
| `build-linux-monolith-control`「build explicit pull runtime control」（`:200-274`）→ retrace-split「Negative control (pull library)」（`:3265-3277`，`retrace_pull_library_control.sh`） | 证明 `run_trace_case.cmake` 的传输解析断言能拦下一个没有 split 的库 | pull 库消失 | 同一 job 改造 **push、无 MG_Remote、非 verify**（即 FCL 内嵌形态）库；脚本逻辑不变（它查的就是 MG_Remote 缺席与证据句），改名为「transport-negative control」；W1 先两库并跑一轮，确认新库同样因同一句话变红，再在 W2 撤 pull 库。**retrace-split 的 `if: needs.build-linux-monolith-control.result == 'success'`（`:3065`）是跳过陷阱**：W1 同时把该条件改成「上游失败即本 job 失败」 | W1 → W2 |
| `prepare_test_runtime` 十二处 `pull-runtime/build-monolith-control`、`control_smoke_test.sh`、`testdata/stub_ctest.sh` | 路径与措辞 | 改名 | 改名；smoke test 用例数不变 | W2 |
| pipe-gates「Espryt memo-key purity」（`:3469-3485`，`espryt_memo_purity.py`） | 新代码不得用前端指针作 memo 键；遗留臂豁免 | 遗留臂删，锚点与 ALLOW 全失配 → 红 | 同一提交里删豁免与锚点，规则变成「全树无前端指针 memo 键」（更严）；自测的阴性对照保留 | W3 |
| `build-linux-split`「Server link-closure ratchet」（`:1288-1317`） | server 分区不得新增对前端符号的引用 | `REQUIRED_FLAGS` 要求 `MOBILEGL_PIPE_PUSH=ON` 出现在 CMakeCache（`link_ratchet.py:184-188`），选项删后 exit 2 | 改为只要求 DISAGG + INPROC；`# P13` 落下时同提交重写基线；加「注解 `# P13` 行数必须为 0」断言（W6 之后） | W3 / W6 |
| `include-graph-check`（`:2284-2314`）与 `MobileGLPurity.IncludeClosure` | 门 A | 删 `#if` 后探针量到 push 头 | 不改脚本；若因此变红，是真红，按包含关系修（**不**加 `-DMOBILEGL_PIPE_PUSH=1` 让它量死代码） | W3 |
| integration-split-strict（`:1700-1716`）grep `kMGPipeP5eRunAheadReady = (true\|false)`（`MG_Backend/Init.cpp:102`） | 常量决定严格车道期望 | 不动 | 保证 P13 不改名、不挪该常量；改动它的提交必须同改 grep | 全程 |
| integration-split「Split coverage」「lane parity」与豁免表 `split_coverage_exemptions.txt:20`、`:27`（`PushMonolithArm`） | 每个 monolith 用例要么上 split 臂要么有原因 | `DirectGLES.PushMonolithArm.`（集成 CMake `:2763-2800`）原为 pull 是默认 monolith 时补的 push-monolith 车道；P13 后环境车道就是它 | 保留登记（只增不删，G14），豁免行原因改写；不删 | W3 |
| integration-verify「Push-build controls (G8, G8b, G9, G10, G12)」（`:788-800`）与 Transport A/B（`:1504-1543`）里的位图车道 | 见 4.1 下表 | 遗留臂删 | 见下表 | W3 |
| fatal_census（`:2544`） | abort 点棘轮 | 删 `PipeLegacyMemosDisabled` 等站点 → 变少 | 同提交 `--write-baseline` | W3 / W6 |
| wire_declines_audit（`:2551`） | 每行 decline 有站点 | 只有删到某行唯一站点才红 | 换臂若让某站点变成两臂共用，不影响；删 monolith 臂时逐行核对 | W6 |
| `symbol_report.py --self-test`（`:3488`） | G1 工具自测 | 不动 | 工具留给接班门复用（§4.3） | — |
| apk.yml `-Pmobilegl.pipePush`（`:109`、`:158`） | APK 形态跟分支 | 属性删 | 删参数（Gradle 不认识的 `-P` 无害，但删干净） | W2 |
| 树外 G1（`notes/p12/gate.sh`）、G5 脚本（`p3a/p4a_untouched_regions.sh`，CI 已于 `c1e957cb` 退役） | pull `.text` 不变 | 失去对象 | §4.3 | W2 |

位图 A/B 车道（集成 CMake，标签 `integration-gpu;integration-monolith-control`，都钉 `MOBILEGL_TRANSPORT=monolith`）：

| 车道 | 掩码 | 意图 | P13 后 | 替代 |
|---|---|---|---|---|
| `{DirectGLES,DirectVulkan}.HandleRecycle.Legacy.` | `PUSH=0` | 遗留臂对照 | 遗留臂不存在 | 结构性删除（需裁定，开放问题 3）；句柄臂的 `Handles.` 车道保留 |
| `DirectVulkan.HandleRecycle.AbaControl.` | `PUSH=0` + ABA | ABA 控制能打穿遗留臂的守卫 | 同上 | 同上；`AbaControlHandles.`（打穿句柄臂的身份）保留，它才是 ship 臂的阴性对照 |
| `HandleRecycle.Handles.` / `AbaControlHandles.` | `LEGACY_MEMOS=0`、`0x1fff` | 句柄臂 | 保留 | 去掉 `LEGACY_MEMOS=0`；掩码改为默认 `0x3fff` 或干脆不设 |
| `CsoContentAddressing.On.` / `Off.` | `0x1fff` / bit 63 | 内容寻址的行为对照 | bit 63 与 pull 无关 | 保留 |
| `ResourceSubsystemControl.On.` / `Off.`；`ResourceSubsystemOn.` / `Off.`（LargeArenaAdoption） | `0x1fff` / `0x7f` | 资源族开 / 关的 A/B | 关 = 遗留 `g_bufferBackendOps` 臂，W6 删 | W3 时 Off 仍可跑（遗留缓冲 ops 是运行期臂、不在宏下）；W6 删该臂时 Off 车道改为「清该位 → 启动期具名拒绝」的阴性对照（`WILL_FAIL` + 具名正则），On 车道的计数断言不变 |
| `ObjectSubsystemControl.{On,Off,Refused,RefusedTexture,Consumer}`、`DirectVulkan…NoConsumer` | `0x1fff` / `0x1ff` / `0x9ff` / `0x5ff` | 对象族 A/B 与依赖拒绝 | Off 落进 D14 `Fatal{PipeLegacyMemosDisabled}` | `Refused*`（依赖两侧拒绝）保留——依赖表还在；Off 同上改具名拒绝对照 |
| `DirectGLES.MapPersistentRoundtrips.`、`TextureUploadShape.` | `0x1fff` | 资源 / 纹理上传形状 | 保留 | 掩码随默认 |

单元测试里对应：`SanityTest.cpp:75-121`（`EsprytSlotArmEnvironment`）、`:4673-4831` 三个「无臂组合」用例、`SubsystemDepsTest.cpp:198-203`、`:250`，以及 `PipeCatalogueTest.cpp:216-220`、`:307-311` 两个只在 pull 下有断言的用例（`UninstalledTablesAreAllNull`、`ExactlyTheRoutedRowsAreInstalled`）。前者随遗留臂结构性删除；后两个的 pull 半没有对象，push 半已在。

### 4.2 静默跳过的防线（W1 先落）

- **skip 普查门**：`scripts/ci/skip_census.py` 读各车道 JUnit（`census_junit.py` / `junit_tally.py` 已有解析），按「标签 × 名」记 pass / skip / fail，对着入库的 `scripts/data/skip_census_baseline.json` 断言：skip 集合只能变小；名集合只增不删（G14）；任何一条从 pass 变 skip 即红。每波若有意退役用例，必须在同一提交改基线并在提交信息里点名。阴性对照：自测里塞一个「pass → skip」的假 JUnit 必须红。
- **删宏顺序**：W3a 先让 CMake 无条件 `-DMOBILEGL_PIPE_PUSH=1`（选项删、宏仍定义为 1），全部 `#if` 按 push 求值；skip 普查必须与 W2 末完全相同。W3b 再用 `unifdef -DMOBILEGL_PIPE_PUSH=1 -UMOBILEGL_PIPE_LEGACY_MEMOS` 机械去宏，并在同一提交删 `#if !MOBILEGL_PIPE_PUSH GTEST_SKIP` 守卫与 `MGL_DECLARE_PULL_SKIP` 桩、集成 CMake 的 `if (MOBILEGL_PIPE_PUSH)`（`:411`、`:511`、`:1246`）只留 push 分支。最后加一条 hygiene grep：树里出现 `MOBILEGL_PIPE_PUSH` / `MOBILEGL_PIPE_LEGACY_MEMOS` / `MGB_CTX` 即红。

### 4.3 G1 的接班

G1 的意图是「拆分期间出货的单进程库不被悄悄改动」。P13 起出货库就是 push monolith，而且有意改动，G1 无法平移。接班（需裁定，开放问题 4）：

1. **无 MG_Remote 门**（CI）：W1 新增的 push 无 MG_Remote 库 `nm --defined-only | grep -c MG_Remote` 必须为 0（设计 §13.2 那条「两条幸存字节级等式」之一，今天其实没人在 CI 里查）。W5 把暂存 store 挪出 `MG_Remote` 命名空间后它依然成立，且能因此变红。
2. **skip 普查**（§4.2）接管 G2「pull / push 测试名同集」与 G14。
3. **recorder 金标**（§2）接管「推送内容不悄悄变」。
4. W2 落地前在树外 `gate.sh` 跑最后一次 G1，读数记进 P13 README 作为退役读数。

## 5. 构建形态

| 项 | 今天 | P13 后 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | 选项，默认 OFF | 删（W3a 先固定为 1） |
| `MOBILEGL_PIPE_LEGACY_MEMOS` | 选项，默认 ON | 删；运行期 `Features.PipeLegacyMemos` 与 `Fatal{PipeLegacyMemosDisabled}`（`FatalFamilies.def:86`，只追加 → 改退役行）同删 |
| `MOBILEGL_PIPE_VERIFY` | 选项，隐含 push | 保留；车道全保留 |
| `MOBILEGL_BUILD_DISAGGREGATED` / `_INPROC` | 选项，默认 OFF | 保留，但含义收窄为「传输层」：MG_Remote 的 Transport / Wire / Client / Server 会话与 spawn。后端记录臂、暂存 store、applier 纹理 ops 不再受它门控（W5） |
| 运行期位图 `MOBILEGL_PIPE_PUSH` | 默认 `0x3fff`（P5e），可清 | bits 0–13 固定开，清任一位在启动期具名拒绝；bit 4 退役；bit 63（关 CSO 内容寻址）保留（开放问题 3） |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` | 零消费者 | 删 |
| Gradle `mobilegl.pipePush`（`build.gradle:45-47`、`:79`） | 默认 OFF | 删属性与 `-D` |
| Gradle `mobilegl.buildDisaggregated` | standalone plugin 默认 ON，FCL 默认 OFF | 不变 |
| FCL 内嵌（`FoldCraftLauncher/settings.gradle.kts:29`，`FCLauncher/build.gradle.kts:63`） | pull | push monolith、**不编 MG_Remote**（推荐，开放问题 1） |
| `android-plugin/*.kts` | 只传 DISAGG / INPROC | 不变 |
| anland 脚本（`anland-mobilegl-plasma/scripts/build-android.sh:17-20` 传 `-DMOBILEGL_PIPE_PUSH=ON`；`xbuild.sh`、`wsl-test.sh`、`anland-build-client.sh` 只传 DISAGG） | push + split | 删 `-DMOBILEGL_PIPE_PUSH=ON`（留着只是 CMake 未用变量警告）；worktree `anland-unified` 不归本阶段动，只在合并时提醒 |

**FCL 要不要编 MG_Remote**（集成方要求给出裁定理由）：

| | A1：所有出货构建都编 MG_Remote（FCL 打开 DISAGG，运行期默认 monolith） | A2（推荐）：把记录臂需要的东西从 MG_Remote 里拆出来，FCL 不编 MG_Remote |
|---|---|---|
| 工作量 | 小：记录臂已经在 DISAGG 下编译，FCL 换个开关即可 | 中：W5 要把 `StagedShadowStore` / `StagedTextureStore`（含其对 `FatalFunnel` 的依赖）、applier 纹理 ops、`ServerLoop::OnApplyThread` / `SessionLatch` / `ServerLoopInstance().Backend()` 的几处 caps 读、`OwnedDrawInputs.h` 的 `DISAGG` 守卫挪到角色中立的位置；约 700 处 DISAGG 站点要逐个分「数据臂」与「传输」 |
| 体积 / 启动 | MG_Remote 约 4.6 万行（非 MG_Remote 源约 26.6 万行，≈15%），外加 flatbuffers 运行时头与 glvnd 头；FCL 进程里多出 socket / spawn / 令牌代码（不监听，但进了地址空间）；启动多一次传输解析，量级可忽略。具体字节数 W5 前量一次（Release aarch64 `.text` 差） | 不变 |
| 门 | 「无 MG_Remote」这条字节级等式失去出货对象，只能在 CI 里另造一个不出货的库当阴性对照，而那个库还得能渲染——又回到要保留 monolith 臂 | 无 MG_Remote 的库就是出货库，传输阴性对照与它同一个东西；与 D12 的模块边界一致（`mg_backend` 不依赖 `mg_remote_*`） |
| 风险 | 低 | W5 是一次大挪动，但纯结构性，主机全门可验 |

推荐 A2，理由是第三行：A1 下要么出货库与对照库不同（对照库的 monolith 臂得一直留着，P13 的「删 monolith 臂」做不完），要么丢掉对照。A2 的工作与 D12 本来就要做的模块拆分重合。若集成方偏好先求快，可以 **A1 过渡、A2 收尾**：W4 在 DISAGG 构建里换臂时 FCL 暂按 A1 出货，W5 完成后改回不编 MG_Remote——但这意味着 W4 期间 FCL 形态与 CI 的无 MG_Remote 库不一致，不推荐。

## 6. 分波

每波：WSL `~/mgl-p13-*` 构建（clang；split 树 `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON`，另有无 MG_Remote 的 push 树与 verify 树按需），本机跑 `ctest -L unit`、`ctest -L integration-gpu -E DirectVulkan`、`-L integration-split` / `-spawn` / `-tcp`（`-E DirectVulkan`）、`retrace`（Espryt），**一次只跑一个套件**；skip 普查对着上一波；推到 `origin/feat/disaggregated` 后看 CI（Magma 集成只在 CI）。设备相关一律记「延后」。

| 波 | 内容 | 本机验证 | 风险 |
|---|---|---|---|
| **W1** CI 地基（不动产品代码） | `runtime_mode_proof` 改定义；retrace-split 的 `if` 跳过陷阱改成失败；`build-linux-monolith-control` 增产 push 无 MG_Remote 库，两库各跑一次阴性对照；新增「无 MG_Remote 符号」检查；新增 skip 普查脚本 + 基线 + 自测 | `control_smoke_test.sh`、各脚本 `--self-test`；本地造一个 push 无 MG_Remote 库跑 `retrace_pull_library_control.sh` 对照 | 低 |
| **W2** 默认翻转 | CMake `MOBILEGL_PIPE_PUSH` 默认 ON；Gradle 删 `pipePush`（FCL 变 push monolith）；阴性对照只用新库，撤 pull 库；改名；树外 G1 最后一次读数入档 | unit + integration-gpu（push 无 MG_Remote 树）；FCL 设备冒烟延后 | 低；FCL 第一次出货 push，设备上未验 |
| **W3a** 宏固定为 1 | 选项删，CMake 无条件定义 `MOBILEGL_PIPE_PUSH=1`；`link_ratchet` 的 `REQUIRED_FLAGS`；workflow / 脚本里的 `-D` | skip 普查与 W2 末逐名相同 | 低 |
| **W3b** 机械去宏（按目录拆提交：MG_State / MG_Impl / MG_Pipe / DirectGLES / DirectVulkan / 测试） | `unifdef`；删 pull 测试桩与守卫；`MGB_CTX*` 改写；删 `LEGACY_MEMOS`、`Features.PipeLegacyMemos`、D14 Fatal 行、`TEXEL_RETAIN_MB`；`espryt_memo_purity` 收紧；位图语义改为固定开（Off 车道暂保留，它们跑的是运行期遗留缓冲臂）；`HandleRecycle.Legacy.` / `AbaControl.` 按裁定处理；`fatal_census` 重写基线；hygiene grep | 全套本机门；skip 普查只允许裁定过的退役 | **中高**：diff 大、与并行分支冲突多；`unifdef` 后要人工看注释里「pull build」的陈旧说法（只改明显错误的，不做大段重写） |
| **W3c** 残余块删除 | §2 第一行；op 46 退役行；协议修订 +1；verify 车道加 capability 腐蚀对照 | unit（codec / catalogue / spans）、integration-verify | 低；协议修订要同步 `protocol_revision_pins.json` 与 PeerLatch |
| **W4a** 换臂：缓冲、indirect、client 数组、GPU 写标记、fp64、XFB（在 DISAGG 构建里） | 引入单一谓词（如 `MG_Backend::DataArmIsRecord()`，恒 true），只替换 §1.2 的 F 类分支，K 类保留 `Transport`；§1.4 别名刷新；14a 先落（不需要 MG_Remote）；Magma 读回移出录制；monolith 下 `WireIndirectPeek` 可读；取消 §3 W4a 行的全部跳过 | build-split 的 integration-gpu[monolith] / [inproc]；split 各臂不变；retrace 230/236（已知 6 条 iterationrp）；Magma 靠 CI | **高**：monolith 每 draw 路径整体换；`TrySetupDrawFastPath` 等 monolith 专属快路随之失效，主机先 `perf stat` 粗量 |
| **W4b** 换臂：纹理存储 / view / 重铸 / mip / copy / 回读 | 同上；`M:7233-7241` 的 monolith 早退删；monolith 后端读 `MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT` / `MGITEST_MAGMA_FORCE_SHADER_MIPMAP`；取消 M1–M3、14b 跳过并补 monolith 登记 | 同上 | 高 |
| **W4c** 换臂：帧缓冲、单元 / sampler / image、program / uniform、swapchain、query、设备可选扩展 | Magma 占位纹理改 `WirePlaceholderImages`；sampler POD；render pass 走记录；monolith 开 wire 用的可选扩展；先核对 query 的 CPU 增量 | 同上 | 高；设备创建改动只能 CI + 设备冒烟（延后） |
| **W4d** residual 填充与 `PipeInputs` 前端字段 | monolith 的值类字段由记录携带；7 个转发访问器换 pipe 侧替身；`kFatal` 前端指针字段删；`PipeInputs.h` 去 `MG_State` 头 | 同上 + verify 车道 | 中 |
| **W5** 记录臂去 DISAGG 门控（A2） | 暂存 store / applier 纹理 ops / 选择器挪出 MG_Remote；`OwnedDrawInputs.h` 去守卫；`g_programResourceCaches` 换 `storageBlocks`；无 MG_Remote 库从此走记录臂 | push 无 MG_Remote 树全套；verify 树；无 MG_Remote 符号检查仍为 0 | 中（结构性大挪动） |
| **W6** 删 monolith 臂与粘合 | §1.3 全部；ratchet `# P13` → 0 并加断言；Off 车道改具名拒绝对照；wire_declines 逐行核 | 全套；ratchet 基线同提交重写 | 中 |
| **W7** D12 模块 target | OBJECT 库拆分 + server-only 链接检查目标；ratchet 余量清到 0 或逐条具名豁免 | 全套 + 新链接目标 | 中 |
| **W8** recorder 金标 | §2 | unit | 低 |
| **W9** 缓存容量重调 | 主机语料定初值，设备复核延后 | `MOBILEGL_PIPE_STATS` 读数 | 低 |
| **W10** 收官 | 全套门；文档（ROADMAP、CURRENT_STAGE、DEBTS 的 ID-P8-13 / -14 / `TEXEL_RETAIN_MB` 三行关闭、设计 05 / 07 / 09 / 附 A 改写、`guide/build-and-run.md` 四 flavour 表、`guide/discipline.md` 的 G1 / G2 行）；设备延后清单 | — | — |

W4 的内部顺序可以按 Espryt / Magma 拆成并行包（两后端互不碰文件），集成方合并。W3b 与 W4 都动 `DirectGLES.cpp` / `VulkanRenderer.cpp` 的大片代码，**不并行**。

## 7. 需要用户定的事

1. **FCL 内嵌库编不编 MG_Remote**：推荐 A2（不编，记录臂从 MG_Remote 拆出，§5）；A1 更快但要么丢掉传输阴性对照，要么永远留着 monolith 臂。
2. **「monolith 逐线程 CPU 不差于 P0 基线」是门还是记录**：2026-09-08 定过「对 pull 只记录、不设门」，而 push 已实测比 pull 多 6–12%（VAO 密集 +27–30%）；P13 换臂会再动 monolith 每 draw 的路径。建议按 09-08 只记录（设备 A/B 延后），否则 P13 后要加一个优化包。
3. **运行期位图**：bits 0–13 固定开、清位即启动期具名拒绝，bit 63 保留；`HandleRecycle.Legacy.` / `.AbaControl.` 两组遗留臂车道随遗留臂结构性删除（句柄臂的 `Handles.` / `AbaControlHandles.` 保留），各 Off 车道改成具名拒绝的阴性对照。这算不算「让 CI 变松」需要你点头。
4. **G1 退役**：W2 记最后一次读数后退役，由「无 MG_Remote 符号」+ skip 普查 + recorder 金标接班（§4.3）。
