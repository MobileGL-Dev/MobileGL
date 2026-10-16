---

## 15. 分阶段实施计划

> 通用纪律（每个 commit 都适用）：默认 ALL target 必须能完整构建；禁止提交热路径插桩；每个门必须**能因它存在的理由变红**；**Windows 机器不是正确性门**（其 Vulkan 缺 `vkCreateHeadlessSurfaceEXT`，占该机 567 个基线集成失败中的 423 个）；设备对比走 reboot-clean + 同窗口配对 A/B；**每个阶段的出口都跑一次 §12 第 4 层的 `nm`/`.text` monolith 门**（不只是 P0）。

### P0 — 卫生、骨架与两个 spike（5 天）

**交付物**
- 清除工作树 `[IBOTX]`/`[BUFTX]` fprintf（`DirectGLES.cpp:290-303`、`Managers.cpp:875-877`，后者在 `pendingMutex` 临界区内）。
- `RenderbufferObject::GetLifetimeId()` **与 `GetVersion()`**（§5.4）。
- 两个 CMake option：`MOBILEGL_BUILD_DISAGGREGATED`(OFF) 与 `MOBILEGL_BUILD_DISAGGREGATED_INPROC`(OFF)；`MOBILEGL_TRANSPORT` 解析；§13 的 flatbuffers include-dir guard。
- `MG_Remote/{Protocol,Transport}` 骨架：`ITransport`、`InProcessTransport`、校验型 `Framing`、`Ring` + `RingControl`（**双 tail、双游标三元组、双向 doorbell**）、`Doorbell`、`ShmSegment`（memfd/ASharedMemory/shm_open/CreateFileMappingW）、**`SCM_RIGHTS` fd 传递（第一优先）**。
- `protocol.fbs` + 提交的 `protocol_generated.h` + `gen_protocol.py` + CI `flatc-check`；`Records.def` 的 `static_assert` 与**运行期边界检查**生成。
- `gen_backend_state_surface.py` + `Coverage.def` **和** `gen_impl_mutation_surface.py` + `MutationCoverage.def` + `CoverageAssert.cpp` + CI `coverage-check`。
- `MG_Test/Wire/` 目录（复制 `MG_Test/Buffer/CMakeLists.txt`）。
- **`TracyPlot` 字节计数器**，装在 wire **两侧**，按类别分：`cmd-records`、`stage-buffer`、`stage-texture`、`stage-ubo`、`persistent-map-push`、`server-ring`、`server-staging`（树里今天完全没有 per-frame 字节度量：`MG_Util/Metrics` 只是格式算术，Tracy 只有 zone 无 plot，MC 26.3 战役的 PANDIAG 已不在树里）。
- `mobilegl_server_main` 的 `extern "C" __attribute__((visibility("default")))` 声明（§11.2）。
- **spike A（Android 交付链，半天）**：从根 CMakeLists 造一个平凡的 `libMobileGLServer.so`（`add_executable` + `PREFIX "lib"/SUFFIX ".so"`），确认 AGP 把它打进 `lib/arm64-v8a/`；让 `TraceReplayActivity` 从 `getApplicationInfo().nativeLibraryDir` **`posix_spawn`** 它并打一行日志——在**应用自身进程（`untrusted_app` 域）**验证 exec，而不是靠 `run-as`。同时把一个通用 env 透传（`--es mobilegl_env "K=V;K=V"`）接进 trace 路径的五个文件（`trace-replay-ci.sh`、`TraceReplayActivity.java`、JNI Request marshalling、`trace_replay_core.cpp`、`run_android_retrace_local.py`），取代逐 knob 加 `--es/--ez`。
- **spike B（external memory 可行性，半天）**：最小程序，导出一个 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，`mmap` 后回读校验，在 `35d0befa`（Adreno 830）与 `3B159D009VZ00000`（Mali）各跑一次。与 `SCM_RIGHTS` 测试同批。**目的是让 P7 的结论在第一周就有方向**：若两台都不行，P7 缩为"记录并回退"，省 6 天。

**验收**
- Linux 与 Android/NDK 上 `cmake --build .` 默认 target 成功。
- `ctest -L unit`、`-L integration-gpu` 与 `81b17c0b` 同一通过集。
- `MG_Test/Wire` 的 fd 传递测试把一个 memfd 从 fork 出的子进程传回父进程并读到相同字节。
- **`nm --defined-only` 与去符号 `.text` size 与改动前的 `libMobileGL.so` 一致**（OFF 构建）；`nm -D | grep mobilegl_server_main` 在 RelWithDebInfo 下命中。
- spike A：设备上打出那行日志。
- spike B：结论写进 §17 的开放问题并驱动 P7 的排期。
- **§12.2 的取舍拍板**：`inproc` 走"四全局角色隔离"还是"降级为纯测试模式"，写进文档。

### P1a — 垂直切片（client + inproc applier），Linux 门（6 天）

**范围刻意收窄到 OpenRA 需要的东西**：仅 DirectGLES；buffer（仅 shadow，采纳强制关，**含 §5.10 的 persistent-map 推送**）；2D 纹理的整 level 与 union-box 上传（**含 §5.6a 的 clear-on-emit**）；VAO；FBO；render state；binds；索引与非索引 draw；clear；present；**server 从源码 relink**（带全字段 `reflectionDigest`）；一条阻塞 `ReadPixels`；**client 侧 `MarkGpuWritten` 保守置位（§5.6b）**。不含 sync/query/XFB/compute/dirty-rects/MultiDraw。

**交付物**：`WireMirror`（含 `PublishImplicitState`、`PersistentMapTracker`、`GpuWritePending`）、`EmitTable`、`EmitBufferOps`、`BackendObject_Remote`、`CapsMirror`、`ClientArrayBounds`、`CompositeResolver`（P1-4 走"server 自建 composite + digest 校验"，见 §5.7）；`ReplicaContext`、`Applier`（含 `MG_Remote::Shared::` 的 XFB/mipmap helper 接线，即使这一阶段还用不到 XFB）、`ServerLoop`(io+apply)；`InProcessTransport` 上跑通。

**验收**
1. `ctest -R "DirectGLES\.Split\..*(ClearThenReadPixels|Triangle)"` 在 Linux + `MOBILEGL_TRANSPORT=inproc` 绿。
2. **新增 `PersistentCoherentMapScenario`**（map `PERSISTENT|WRITE|COHERENT`、写、不做任何其它 GL 调用、draw、readback 校验）在 split 下绿。**这是本计划里唯一一个专为一个 fatal 缺陷设的门**，必须在 P1a 就绿。
3. 记录**两个进程/两个角色的峰值 RSS**（不只是 server 的）作为 P5 与 §16-R14 的基线。
4. Tracy 计数器给出 `persistent-map-push` 的字节量（§5.10 保守版的代价）。

**明确非目标**：性能。P1-4 双份 glslang，**MC 级负载不在此测**。

### P1b — spawn transport，Linux 门（4 天）

**交付物**：`SocketTransport`（socketpair + `fork`/`execve` + 显式 envp 剔除 `MOBILEGL_TRANSPORT`/`MOBILEGL_IPC_*`）、`ServerMain`、`MOBILEGL_IPC_SERVER_PATH` 发现链、就绪握手与有界重试、EOF 即时退出。

**验收**
1. P1a 的全部测试在 `MOBILEGL_TRANSPORT=spawn` 下绿（两个真进程、真 socket、真 `SCM_RIGHTS` 段）。
2. **fork 链测试**：spawn 一个 server 并断言进程树只多出恰好一个子进程（§11.1）。
3. **HeadlessGL 预检交互测试**：在开着 fork 预检的 Linux 上跑整套 split 集成用例，断言没有孤儿 server（用 `pgrep` 计数 + 预检结束后 100ms 内归零）。

### P2 — 广度：集成套件、trace 语料对齐、设备首跑（9 天）

**交付物**
- 其余记录种类（MultiDraw/indirect 族含 client 数组范围计算与索引扫描、纹理 dirty rects、texture view、buffer texture、image unit、sampler、**renderbuffer storage**、program pipeline、`CopyImageSubData`、`BlitNamedFramebuffer`、`PixelStorePack`、`CurrentAttrib`）。
- 完整 caps mirror 与 `tableSlotMask`。
- DirectVulkan applier 支持（`SwapchainObject` 的 default-FBO 占位写变成 `EvDefaultFramebufferInfo`）。
- `add_trace_replay_test` 的 `SPLIT` 参数：测试名加后缀、`-DTRACE_TRANSPORT=` 与 `run_trace_case.cmake` 的消费、`MOBILEGL_IPC_SERVER_PATH` 注入（§13）。
- 一个**无 present** 的 split 集成用例（§9.3）。
- `ClientArrayAfterComputeWriteScenario`（§6.10）。

**验收**
1. `ctest -L integration-gpu -R '^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` **逐名同一通过/失败集**；DirectVulkan 同。
2. CI 全部 trace case（OpenRA、`minecraft-1.21.4-startup`、`-main-menu`、`1.21.11`、`1.17`、两个 Create）在 Linux split 模式 SSIM ≥ 0.99。**两个带 `coherent_as_flush: true` 的 Create 用例在 split 与 monolith 下都开着该开关跑**（§5.10 已让两侧走同一路径），若 Tracy 显示保守推送在这两个 fixture 上代价不可接受，则把 §5.10 的精确版（P4.5 的块脏位）提前到本阶段——这是全计划唯一允许因测量改变阶段顺序的地方。
3. **`python tools/trace_replay/run_android_retrace_local.py --case OpenRA --backend DirectGLES` 在 `35d0befa` 上 SSIM ≥ 0.99（split 模式）** —— 本阶段的出口判据（从 P1 移来），每轮约 1 分钟。

### P2.5 — inproc 渲染线程：单机收益证伪门（3 天）

**交付物**：`add_trace_replay_test` 的 `INPROC` 变体；应用线程与 apply 线程的**逐线程 CPU 时间**插桩（不只是墙钟）；`MOBILEGL_IPC_SERVER_AFFINITY` 的大核绑定（复用 `ShaderCompilePool.cpp:73-96`）；用现有 `--benchmark --benchmark-tail-frames --benchmark-result` 在全部 fixture 上跑。

**验收**：`inproc` 与 `monolith` 的应用线程帧时差 + 两侧 CPU 时间在 Create/Flywheel 与 MC fixture 上被**测量并记录**，且带亲和性开/关两组。若不利，整个计划的价值主张在第 6 周（而不是第 15 周）被重新审视。**这是本计划最早的证伪点，也是 §16-R15 排期风险的退火器。**

### P3 — sync / query / present 节奏（5 天）

**交付物**
- client 铸造的 sync/query handle；轮询入口的 publish + 饥饿升级（§7.2）。
- **fence 完成度来自真实逐 fence 退休**（§8 末尾）：server 侧真 `FenceSync` + 非 present 轮询 + `EvFenceSignaled`。
- **DirectGLES 的非 present fence tick**（§9.3）。
- `EvQueryResult`；present credit **默认 1** + 三个 seq 水位；swap interval 搭 `RecPresent`。
- §8 的三个 `dev` 独立修复。
- per-frame round-trip 计数器；**输入延迟直方图**（记录发射 → present 完成，§9.1）。

**验收**
1. `XfbPrimitiveQueryScenario`、`PrimitivesGeneratedNoXfbScenario`、`AsyncCompileScenario` 在 split 下绿。
2. round-trip 计数器：在**全部 trace case** 的稳态帧上，draw/state/upload 路径的 round trip 读 **0**；conditional render 与阻塞式 query 的次数按用例列表公布（不是笼统宣称"零 round trip"）。
3. **零 timeout 轮询循环测试**：一个只有 `glFenceSync` + `while(glClientWaitSync(...,0)==GL_TIMEOUT_EXPIRED){}` 的用例必须在有界时间内退出（若无 §7.2 的 publish 规则它会永久挂起）。
4. `bench.sh` 在 `35d0befa` 配对 A/B（两侧均关采纳）显示 split 帧时在 monolith 的 10% 内，且**输入延迟直方图**的 p50/p99 被记录。

### P4 — 回读与 GPU-written（5 天）

**交付物**：`SEG_REPLY`；阻塞 `ReadPixels` → 客户内存；PBO readback 变 fire-and-forget + client 侧 `MarkGpuWritten`；`EvGpuWritten` 作为收窄提示；`EvBufferWriteback`；`glGetTexImage`/`GetTextureImage` 路由 + **per-level `serverAuthoritative` 位**（只覆盖生成 mip 与 CopyImage 镜像两处，§6.6）；`EvGlError` + **分配类入口的 `kNeedsAck`**（§5.6c）；`SEG_EVENT` 溢出策略与等待中排空（§7.4）。
**`glCopyTexSubImage*` / `glClearTexImage` 保持前端实现不变**（推翻上一版的"移到 server + `EvTexWriteback`"）。

**验收**
1. split 下 `DepthStencilReadbackScenario`、`DepthStencilReadbackMatrixScenario`、`DepthStencilReadbackAttachmentShapeScenario`、`PackedWordReadbackScenario`、`LayeredTextureReadbackScenario`、`ClearThenReadPixelsScenario`、`PixelStoreSweepScenario`、`CopyImage*`(4)、`SsboArrayLengthScenario`、`AtomicCounterScenario`、`StorageBufferRegrowScenario` 双 backend 全绿。
2. **OOM 探测用例**：请求一个必然失败的巨大 renderbuffer，断言紧接着的 `glGetError()` 返回 `GL_OUT_OF_MEMORY`。
3. **事件 ring 溢出故障注入**：client 被 present credit 阻塞时灌满 `SEG_EVENT`，双方都不死锁，`eventDropped` 只统计到 `EvLogLine`。

### P4.5 — 零拷贝 shadow-in-shm 前移（4 天）

（原计划推到 P6；MC pan 每帧 ~9MB 的额外拷贝不该背六个阶段）

**交付物**：`ShadowArena`；`MapAlignedAllocator` 与 `MipmapStorage` level vector 的 shm arena（≥256KiB 才走，**整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹**，§12 第 3 层）；per-shadow 64KiB 块发送水位 WAR 规则；**shadow 块退休规则**（§6.1）；§5.10 精确版 persistent-map 推送复用同一套块脏位；`MOBILEGL_IPC_SHADOW_SHM` 开关。

**验收**
1. P2/P4 门在开关两态下均不回归。
2. **两侧** `TracyPlot` 显示 buffer 上传路径的总拷贝次数从 4 降到 3（或选方案 B 则到 2，§6.4）；staged-copy 回退率被记录成数字。
3. `nm`/`.text` monolith 门仍绿（这一条是本阶段最容易破的）。
4. 对象删除/重定义与未 apply 记录并发的压力测试不读到别的对象的字节。

### P5 — `ProgramPublish`，退役 server relink（6 天）

**交付物**：`ProgramArtifactsArchive.h`（`Visit()` + `sizeof` 绊线）；`ProgramObject::InstallPublishedLink`；`GLContext::SetReplicaResolvedDrawProgram`（`#if MOBILEGL_BUILD_DISAGGREGATED` 包裹）+ client 侧 composite 解析；`MOBILEGL_IPC_PROGRAM=publish|relink`；`publish` 下移除 server compile pool；**顺带把 DirectVulkan 的 blit / depth-mipmap 四段固定 shader 在构建期烘成 SPIR-V**（`VulkanRenderer.cpp:4211-4356`，同时也从 **monolith 启动**里去掉一次 glslang 编译链接；逃生口 `MOBILEGL_BAKED_INTERNAL_SHADERS=0`）。

**验收**
1. P2 全门在 `publish` 下重跑不变。
2. `relink` 下 `reflectionDigest` 在每个 trace case 绿（即它是活门不是死门）。
3. `35d0befa` 上用 `minecraft-1.21.4-startup` trace 做首帧 link 延迟 A/B，`publish ≤ relink`。
4. **`nm` 复核 `libMobileGLServer.so` 在 `publish` 下不再引用 glslang 库符号**（注意 `ProgramObject.h` 传递包含 `ShaderObject.h` → `ShaderCompileTask.h`，所以这条必须**用 `nm` 验证而不是断言**）。
5. server 峰值 RSS 相对 P1a 基线下降；两个进程的 RSS 合计与 §16-R14 的预算对表。

### P6 — 数据面性能（6 天）

**交付物**：`PendingResidentWrite` 借用 ring slot（用 `*RetiredTail` 门控）；全局 UBO ring 进 shm；解码移到 `mgl-srv-io`；可选 `mgl-client-tx`；bind 合并（凭数据决定）；`mirror-map` 两次映射消除 ring wrap；`MOBILEGL_IPC_INLINE_PAYLOADS` 负面对照；§6.4 方案 B（replica adopt client shadow）的可行性评估与实现（若 Tracy 数据支持）；Windows AF_UNIX 评估（§11.5）；`MOBILEGL_IPC_SPIN_US` 与 `MOBILEGL_IPC_PRESENT_CREDIT` 的设备调优。

**验收**：两台设备上 `minecraft-1.21.4-fabric-sodium-in-world` 的配对 A/B，每项优化用自己的开关单独可 A/B；P2/P4 门在任意开关组合下不回归；输入延迟直方图不因任何优化恶化。

### P7 — persistent map 与 ≥16MiB 采纳（8 天，若 P0 spike B 全否则缩为 2 天）

**交付物**：`SEG_ADOPT`（server 分配）+ 三档探针（T2/T1/T0）+ 自动回退到 P4.5 路径；阻塞 `AcquirePersistentMap`；client 侧注册 `ResidentSubData`；`MOBILEGL_IPC_RESPAWN` 与 `MOBILEGL_IPC_ADOPT_TIER` 的互斥检查（§5.8）。

**验收**：`LargeArenaAdoptionScenario`、`ResidentIndexScenario` 在采纳开启下绿；`bench.sh` 在 Mali 设备 `3B159D009VZ00000` 上用 `minecraft-1.21.4-in-world` 报出 {monolith, split+采纳, split+回退} 的 p99 帧时，以 MC 26.3 的 163→21ms 为标尺。
**"设备 X 上拒绝，已记录，回退成本 N ms" 是本阶段的可接受结论**——因为回退路径在 P1a/P4.5 已交付并测量。

### P8 — XFB / compute / 健壮性 / 多线程（6 天）

**交付物**：XFB capture writeback 与 scatter（全部在 server 对 replica 执行，只有合并后的 range 过线）；**`RecXfbAccounting` 与共享 helper 的完整接线**（§2(g)-2；注意它必须跟着 `RecBindTransformFeedback` 的对象切换走，`Core.cpp:1273,1296`）；GS strip 顺序修正移到 server；compute dispatch/indirect/barrier/image load-store；`EvGlError` 与 `glGetError` 的顺序 + `MOBILEGL_IPC_STRICT_ERRORS` 诊断开关；server 死亡的 device-lost 闩锁与 client 死亡的 server 拆机；外来线程 sync/query 的 `AuxRequest`；修 `EGLOperationMutex` 既有漏洞（`ReleaseThread`、`SwapInterval`）。

**验收**
1. split 下 `Xfb*`(5)、`Tessellation*`(2)、`SsboArrayDynamicIndexScenario`、`ImageLoadStoreSsoScenario` 双 backend 绿。
2. `tools/cts/scripts/run_cts_local.py --backend {DirectGLES,DirectVulkan} --env MOBILEGL_TRANSPORT=spawn` 在 GL33 caselist 上 conformance rate 与 monolith 相差 ≤ 0.5 个百分点（按项目既定的逐 backend 表格式报告：行=GL 版本/扩展，列=状态计数，conformance rate = Pass/(Pass+Fail)，分母不含 NS）。
3. 故障注入测试在帧中 SIGKILL server，client 干净地以 `EGL_CONTEXT_LOST` 退出而不崩溃。

**注**：XFB 场景的 `RecXfbAccounting` 骨架其实在 P1a 就要落地（helper + 记录 + applier 分支），只是这里才被真正测到。§5.9b 的生成器会在 P0 就把它标成未映射并让编译失败，从而强制这个顺序。

### P9 — Android 生产窗口路径（10 天）

**交付物**：`MobileGLServerService`（`android:process=":mgl"`）；Messenger/AIDL 的 `Surface` 交接；surface 生命周期（`surfaceDestroyed`、1×1 pbuffer 交换舞、resize）作为协议消息；`ResyncSnapshot`；APK 打包与 `validate-plugin-apks.sh` 更新；`MOBILEGL_TRANSPORT` 进 plugin V2 metadata 与 FCL 用户 env 偏好。

**验收**：FCL 在 `35d0befa` 上以 split 模式把 Minecraft 1.21.4 拉到主菜单并进入世界；`bench.sh` 在同一个热窗口内报出 split vs monolith 的游戏内 FPS **与输入延迟**；plugin APK 通过 `.github/scripts/validate-plugin-apks.sh`；旋屏/后台切换的 surface 销毁重建无泄漏无挂起；两个进程的合计 RSS 落在预算内。

**合计 ≈ 77 人日 ≈ 16 周**（5+6+4+9+3+5+5+4+6+6+8+6+10）。里程碑：**第 3 周末 Linux 上跨进程渲染出第一帧**（P1b），**第 5 周末真机 OpenRA 绿**（P2），**第 6 周有 monolith 侧的独立收益数字**（P2.5）。

---

## 16. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | **replica applier 在某个长尾副作用上与 client 的 MG_State 语义分歧**——具体形态是 MG_Impl 在 table 调用旁做的 mutation（§2(g) 已确认两族：`EnsureGeneratedMipmapStorageAllocated`、`AccountTransformFeedbackPrimitives`）。症状是错误像素或错误查询结果，不是崩溃 | **§5.9b 的第二个生成器**把这一面变成编译期门：MG_Impl 里任何与 table 调用同函数的 mutator 未映射即 `#error`。两族已知实例在 P1a 就用共享 helper 接线。P2 的门是**全部集成场景 + trace 语料的逐名通过集对齐**，远比 `Feat/CS-Delta-IPC` 的两 `GLContext` 逐字段比较（且只查了 5 域中的 2 域）严苛。外加 `MOBILEGL_IPC_VALIDATE_SERVER`（server 侧保留 MG_Impl 校验器，分歧变成 server 侧 GL error 而非错误像素；CI 常开，出货构建用 `kPrevalidated` 短路） |
| R2 | **应用通过 coherent persistent map 写下的字节丢失**（`SyncPersistentMappedRange` 无 client 侧调用者） | §5.10 三件套：map/unmap 上线、client 侧块粒度推送、`PersistentCoherentMapScenario` 作为 **P1a 门**。这是本轮新增的最高优先级修复 |
| R3 | **read-after-GPU-write 静默读到陈旧 shadow**（`MarkGpuWritten` 无 client 侧建立者） | §5.6b：client 在每个 draw/dispatch 发射点保守置位并记 `emitSeq`；读入口强制 publish+等待+排空；`EvGpuWritten` 降级为收窄提示。§7.4 的排空点补上四个 buffer 读入口 |
| R4 | **零 timeout 轮询循环挂死**（轮询入口不是 publish 触发器） | §7.2：`glClientWaitSync`/`glGetSynciv`/`glGetQueryObject*(AVAILABLE\|NO_WAIT)` 全部成为门铃点，`GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 publish；连续 N 次无进展升级为阻塞 round trip。P3 有专门的门 |
| R5 | **fence 完成度退化成帧计数推断**（DirectGLES 的 `completedFrameSerial` 只在 Present 前进），重蹈 MC 1.21.5 的 native-heap OOM | §8 末尾：server 侧真 fence + 非 present 轮询 + `EvFenceSignaled`；§9.3 的非 present fence tick 同时解决无 present 循环下的 ring 饥饿 |
| R6 | **纹理每次更新都传整 level**（永不清 dirty flag ⇒ union box 单调增长） | §5.6a：client 在发射后立刻 `MarkStorageDirty(...,false)`；ack 问题由"resync 从完好 shadow 传整 level"+"硬 drain 后重发未 apply 记录"两条收口。已确认 MG_Impl 从不读自己的 dirty 状态，所以清是安全的 |
| R7 | **每 draw 编解码成本超过它替换掉的东西**，MC 级帧（1000-4000 draw）反而更慢；且总 CPU 工作量本来就变大（遍历跑两次） | 记录是 FlatBuffers `struct`（8B header + 定长），无 verifier walk；publish 是每记录一次 release store 而不是 64KiB 攒批（§7.2）。**`TracyPlot` 两侧计数器在 P0 就落地**；P2.5 在第 6 周给出 inproc 的证伪数字**并带逐线程 CPU 时间**；`mgl-srv-apply` 绑大核（§10），mask 打日志；P3 门要求 split 帧时在 monolith 10% 内**才**授权后续优化 |
| R8 | **client 侧等待全是跨进程自旋**（无 producer 侧门铃），手机上一颗大核满频空转 | §6.2a 的双向 doorbell：`producerParked` + 反向 1 字节；自旋窗口 `MOBILEGL_IPC_SPIN_US` 可调可测。`inproc` 用 condvar |
| R9 | **`SEG_EVENT` 满 + client 被 credit 阻塞 = 双向死锁** | §7.4：等待循环内必须排空；`EvLogLine` 有损（覆盖最旧 + `eventDropped` 计数）；语义事件无损，满时 server 置 `eventRingFull` 并停在记录边界上停止 apply。P4 有故障注入门 |
| R10 | **端到端延迟叠加**（client credit + server FIF + 驱动深度 = 4-5 帧） | §9.1：credit 默认 1；文档写出叠加公式；P3/P9 增加**输入延迟直方图**门，只有实测吞吐收益抵得过实测延迟才调高 |
| R11 | **`inproc` 因为四个进程全局而不可行**，从而 P2.5 这个最早的证伪门消失 | §12.1/§12.2：拆成两个 CMake option（出货只开 `spawn`，热路径无 TLS）；四个全局都要角色隔离，shim 需求列全，非箭头用法实测 133 处；**P0 结束前必须拍板**是做隔离还是把 `inproc` 降级为纯测试模式，并写清后者对 P2.5 的含义 |
| R12 | **分配类 GL 错误晚到，OOM 探测惯用法失效** | §5.6c：只把分配类入口标 `kNeedsAck`（罕见且本来就贵），其余保持晚到；`glGetError` 永远本地。P4 有 OOM 探测门 |
| R13 | **server 分配的 host-visible coherent 内存无法导出重映射**，丢掉 ≥16MiB 采纳（值 p99 163→21ms、~400MB RSS） | 排在**最后**（P7），且 **P0 的 spike B 在第一周就给出方向**。此时 P1a/P4.5 的 shadow 路径已交付并测量。阶段明确允许"拒绝，已记录"的结论。前端已容忍 `nullptr`（三处），kill switch 已存在，无需回滚任何代码 |
| R14 | **内存翻倍无预算**：client 段（`SEG_CMD` 8MiB + `SEG_STAGE` 32MiB↑）+ 完整 replica context（每 buffer 一份 `PipeResource`、每 texture level 一份 `MipmapStorage`）+ server 自己的三个 ring（UBO/unpack/upload 各 4→64MiB，`Managers.cpp:82-96`）+ 64MiB buffer pool（`Managers.cpp:566`）。合计可达 ~450MiB 新增，而本项目把"省 400MB"当作采纳修复的头条成果，且有 blanket-immutable 导致 LMK 屠杀的记忆 | 计划里与 round-trip 预算并列写出**稳态内存预算**；P1a 验收记录**两个进程**的 RSS（不只是 server）；`SEG_STAGE` 上限由实测定而不是默认 256MiB；优先推进 §6.4 方案 B（replica 采纳 client shadow），因为它同时消掉重复 shadow 而不只是一次拷贝 |
| R15 | **排期乐观**（P0 5 天含两个 spike + 四平台 shm + SCM_RIGHTS + 两个代码生成器；P1a+P1b 10 天做完整 client 与 server）。校准点：`Feat/CS-Delta-IPC` 10 个 commit / 6668 行、从未渲出一帧，并自承四天耗在一个不可复现的回归上 | P1 已拆成 P1a/P1b，设备 retrace 移到 P2 出口；**P2.5 是排期风险的退火器**——第 6 周就能拿到"这条路值不值得走"的数字，且它本身不依赖任何跨进程工作。若 P0/P1 超期 50%，先跑 P2.5 的 inproc 部分再决定是否继续 |
| R16 | **socket transport 是新实现**，而上一版有每次 send 的 UAF、无上限分配、无 fd 传递 | 从设计草图重写而非修补：读时按 64MiB 上限校验 magic/长度；接收缓冲不足时返回所需大小**且保留消息**；`async_write` 用 `shared_ptr` payload 自持缓冲；socketpair + 继承 fd 完全去掉 accept/connect（Windows 用 overlapped named pipe 对，§11.5）。**`SCM_RIGHTS` 是 P0 交付物并带独立测试** |
| R17 | **Android 交付链**（server `.so` 打包、`untrusted_app` 域 exec、trace app env 透传）比想象的重，或被 AGP/SELinux 挡住 | **P0 的 spike A** 在第一周就验证；P1-P8 全部离屏且不依赖它（Linux 门优先）；两条回退：裸 exec PIE server 配 `AHardwareBuffer_sendHandleToUnixSocket` blit-back；或把 split 作为 headless/工装专用配置发布 |
| R18 | **spawn 出来的 server 继承 `MOBILEGL_TRANSPORT` 而无限 fork** | §11.1 双保险：显式 envp 剔除 + `mobilegl_server_main` 强制 Monolith；P1b 有进程树计数门 |
| R19 | **HeadlessGL 的 fork 预检留下持有 GPU 的孤儿 server** | §11.3：EOF 即时退出（亚秒）；就绪握手有界重试；P1b 有 `pgrep` 计数门 |
| R20 | **`MobileGLServer` 在两个桌面门里都找不到**（`dladdr` 对静态链接的 itest 与显式 `-DMOBILEGL_LIBRARY` 的 retrace 都失效） | §11.1：`MOBILEGL_IPC_SERVER_PATH` 为主、`dladdr` 兜底；`RUNTIME_OUTPUT_DIRECTORY` 对齐；每条新 ctest `ENVIRONMENT` 都注入；并复核 CI artifact 搬运后绝对路径是否还成立 |
| R21 | **`mobilegl_server_main` 在出货构建里 dlsym 不到**（非 Debug 的 hidden visibility preset） | §11.2：显式 `visibility("default")`；P0 加 `nm -D` 断言 |
| R22 | **Magma 的 present 节奏被 IPC credit 改变**（它从不注册 `SetSwapInterval` 且偏好 MAILBOX/IMMEDIATE） | `MOBILEGL_IPC_PRESENT_CREDIT` 可配；P6/P9 在设备上测量输入延迟与帧节奏；若 Magma 需要，把"注册 `SetSwapInterval` 并映射到 FIFO"作为**独立的 `dev` 变更**，不让两套机制同时管节奏 |
| R23 | **两件 Android 产物版本漂移** | 一份共享库两个角色：server 是 ~30 行 stub，`dlopen(libMobileGL.so)` + `dlsym(mobilegl_server_main)`；`Hello`/`Welcome` 里的 build fingerprint（git hash + `Records.def` hash）不匹配 → 明确报错而非静默协议故障 |
| R24 | **`SEG_CMD` 的记录被并发写坏导致 applier 游标走飞** | §6.3 的运行期边界检查（`size >= sizeof(T) && size <= remainingRingBytes && (size%8)==0`，`kVarTail` 另查尾长自洽），违反即 `Fatal{ProtocolCorruption}`，绝不进入 UB |

---

## 17. 开放问题

1. **T1/T0 采纳在 Adreno 830 与 Mali-G925 上到底能不能用？** 由 **P0 的 spike B** 在第一周回答（导出 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，client `mmap` 后回读），与 `SCM_RIGHTS` 测试同批。若两台设备都不行，P7 缩为"记录并回退"，节省 6 天；若可行，还要回答 GLES 侧能否用 `GL_EXT_memory_object_fd` + `glBufferStorageMemEXT` 走同一条路（DirectGLES 的采纳今天走的是 `glBufferStorageEXT` + `glMapBufferRange(PERSISTENT|COHERENT)`，不是外部内存）。
2. **`glGetError` 的严格性 CTS 到底要求到什么程度？** §5.6c 已把分配类改成同步 ack，剩下的晚到错误里，哪些 CTS case 可能观察到？P8 需要列出清单。若清单为空，`MOBILEGL_IPC_STRICT_ERRORS` 可以永久保持默认关。
3. **P2.5 的 inproc 数字若为负怎么办？** 需要事先约定：若 inproc 相对 monolith 无收益甚至更慢（含亲和性绑定之后），是继续（因为拆分本身还有内存隔离、崩溃隔离、工装价值）还是收缩到 headless 工装用途？**建议在 P2.5 前由协调者拍板判据**，并同时约定"绑大核后仍无收益"与"未绑核无收益"是两个不同的结论。
4. **§12.2 的隔离取舍**：`inproc` 做四全局角色隔离（含 133 处非箭头用法的 shim）值不值？若判定不值而把 `inproc` 降级为纯测试模式，P2.5 测的就不再是 monolith 渲染线程交付物——那时 monolith 侧的收益要靠什么证明？**P0 结束前必须有答案。**
5. **§6.4 的拷贝目标选方案 A 还是 B？** 方案 B（replica 的 `PipeResource` 采纳 client 的 `SEG_SHADOW` 只读映射）能把 buffer 上传路径从 3 次降到 2 次并消掉重复 shadow（对 R14 的内存预算意义更大），但要处理 server 侧写（`WritebackFromBackend`、生成 mip、CopyImage 镜像）的 copy-on-write 升级。P4.5 先做 A 并测量，P6 由数据决定是否做 B。
6. **`SEG_SHADOW` 在 Android 上应该用 `ASharedMemory` 还是 memfd？** 前者是平台正道且有 `setProt` 只读降权（正好匹配"client 拥有、server 只读"），后者有 sealing。大 buffer 频繁重映射的场景需要一次实测。
7. **client 侧是否需要 `mgl-client-tx` 发送线程？** 只有 P6 的 `TracyPlot` 数据能回答；在此之前不要预先加线程（会引入拷贝或锁）。
8. **`ResyncSnapshot` 与采纳的互斥能否放松？** §5.8 目前规定 `MOBILEGL_IPC_RESPAWN=1` 与 `MOBILEGL_IPC_ADOPT_TIER != 2` 互斥，因为 adopted store 的字节在 server。是否值得为 adopted buffer 单独做一条"server 死亡时其内容视为丢失、按 `hasDefinedContent=false` 重建"的降级路径？取决于 MC 的 chunk arena 在 respawn 后能否被应用自己重填。
9. **Windows AF_UNIX-everywhere 是否值得？** asio 的 IOCP `async_accept` 走 `AcceptEx`（AF_UNIX 从不支持）；我们用继承 overlapped 句柄绕开 accept，理论上可行但需真编真跑。P6 评估，named pipe 是已知可用的默认。
10. **P9 的 ART 启动成本具体是多少？** 若不可接受，是否接受"游戏内走 monolith，工装/CTS 走 split"的长期二元形态？
11. **`tools/trace_replay` 的 Android 应用内路径是否从非主线程驱动 GL、是否每重放帧调 `Present`？** 桌面重放器传 `--singlethread`（`trace_replay_core.cpp:430`），Android 应用内路径本次未完整追踪，它决定该工装能否验证节奏模型（尤其是 §9.1 的输入延迟直方图）。
12. **`MOBILEGL_IPC_PERSISTENT_BLOCK_KB` 的默认值与脏块判定方式**：P1-4 的保守版（整 mapped span 按块重传）在 Create/Flywheel fixture 上的实测代价是多少？精确版用 `memcmp` 还是 mprotect 写屏障？前者对 1MB 块是 ~50µs 量级且只在真正 mapped 的 buffer 上跑，看起来够用，但需要 P2 的数据确认。
13. **`SEG_STAGE` 的上限该定多少？** R14 要求由实测定而不是默认 256MiB。需要 P2 之后用 MC in-world 与 Create 两类 fixture 的 `stage-*` Tracy 计数器给出 p99 占用。

---

## 附：环境变量与 CMake 选项汇总

**CMake**
| 选项 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | OFF | 出货形态。开启后 `MG_Remote/**` 进 `SOURCE_FILES`，支持 `spawn`/`unix:`/`pipe:`。四个进程全局保持普通全局，GL 热路径无 TLS |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | OFF | CI/调试形态，隐含开启上者，额外加四全局角色隔离 shim |
| `MOBILEGL_FLATC_EXECUTABLE` | 空 | 只服务 CI 的 `flatc-check`；默认构建图里没有 `flatc` |
| `MOBILEGL_BAKED_INTERNAL_SHADERS` | ON (P5+) | DirectVulkan 的 blit/depth-mipmap shader 构建期烘 SPIR-V；monolith 也受益 |

**运行时**
| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | `monolith` / `inproc` / `spawn` / `unix:<path>` / `pipe:<name>` |
| `MOBILEGL_IPC_SERVER_PATH` | 空 | server 可执行文件路径（**主要发现机制**，`dladdr` 兜底） |
| `MOBILEGL_IPC_RING_MB` | 8 | `SEG_CMD` 大小 |
| `MOBILEGL_IPC_STAGE_MB` | 32 | `SEG_STAGE` 初始大小；上限由实测定（§17-13） |
| `MOBILEGL_IPC_PRESENT_CREDIT` | **1** | client 允许领先的 present 数（1-4）；延迟叠加见 §9.1 |
| `MOBILEGL_IPC_SPIN_US` | 50 | 挂起前的自旋窗口（两侧 doorbell 共用） |
| `MOBILEGL_IPC_POLL_ESCALATE` | 64 | 同一 handle 连续无进展轮询多少次后升级为阻塞 round trip |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` | 64 | persistent-map 推送的块粒度 |
| `MOBILEGL_IPC_PROGRAM` | `relink` (P1-4) → `publish` (P5+) | program artifact 传输方式；`relink` 保留为常驻 oracle |
| `MOBILEGL_IPC_ADOPT_TIER` | `auto` | `auto`/`0`(T0)/`1`(T1)/`2`(T2 拒绝)；与 `MOBILEGL_IPC_RESPAWN` 互斥（§5.8） |
| `MOBILEGL_IPC_SHADOW_SHM` | 1 (P4.5+) | shadow-in-shm 零拷贝 |
| `MOBILEGL_IPC_INLINE_PAYLOADS` | 0 | 负面对照：一律内联，不用 `SEG_STAGE` |
| `MOBILEGL_IPC_SERVER_AFFINITY` | `auto` | `mgl-srv-apply` 的核绑定；`auto` 用 `ShaderCompilePool` 的大核探测 |
| `MOBILEGL_IPC_VALIDATE_SERVER` | CI=1，出货=0 | server 侧保留 MG_Impl 校验器，分歧变成 server GL error |
| `MOBILEGL_IPC_STRICT_ERRORS` | 0 | 诊断开关：让所有 backend 错误同步 ack（分配类默认已是同步） |
| `MOBILEGL_IPC_AUDIT` | 0 | 记录级审计日志 |
| `MOBILEGL_IPC_TRACE` | 0 | 逐记录 trace（仅调试构建） |
| `MOBILEGL_IPC_ATTACH` | 空 | 附着到已运行的 server（调试） |
| `MOBILEGL_IPC_RESPAWN` | 0 | server 死亡后重启 + `ResyncSnapshot` |
| `MOBILEGL_IPC_IDLE_EXIT_S` | 30 | server 的最后保险看门狗（EOF 应当即时退出） |

**保留的既有负面对照开关**：`MOBILEGL_ESPRYT_DISABLE_UBO_RING`、`_UNPACK_RING`、`_UPLOAD_RING`、`_INVALIDATE_FLUSH`、`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`、`MOBILEGL_COHERENT_AS_FLUSH`（**在拆分模式下照常生效**，§5.10/§6.8）。
