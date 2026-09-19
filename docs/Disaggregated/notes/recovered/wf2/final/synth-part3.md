## 7. backend → frontend 反向通道

这是历次评审对任何薄 backend 设计的中心反对意见，所以逐条处理，**不做概括**。实测：`grep -rnoE "(->|\.)(SetBackendResource|SetBackendHashMemo|SetBackendStateMemo|SetBackendAuxMemo|WritebackFromBackend|MarkGpuWritten|MarkStorageDirty|AllocateStorage|SetInternalFormat|UpdateMipmapSubData|EnsureGpuResidentStorage|SyncPersistentMappedRange|SyncGpuWrites|RecordError|InvalidateCompileEnv|TruncateMipmapLevels|SetSamples)\(" MG_Backend/` = **95 个调用点 / 17 个方法**，外加 6 处 backend 反向进 `MG_Impl`。

### 7.1 `MGPipeCallbacks`：把反向通道具名化（对 gallium 的偏离 D8）

```cpp
// MG_Pipe/MGPipeCallbacks.h  —— context_create 时安装；monolith 里是直调，split 里是记录
struct MGPipeCallbacks {
    void (*on_gl_error)             (Uint32 code);
    void (*on_gpu_written)          (MGPipeHandle res, Uint rangeCount, const MGPRange*);
    void (*on_buffer_writeback)     (MGPipeHandle res, Uint64 off, MGPBlobRef bytes);
    void (*on_texture_writeback)    (MGPipeHandle res, const MGPBox*, MGPBlobRef bytes);
    void (*on_texture_pull_request) (MGPipeHandle res, Uint16 target, Uint16 firstLevel, Uint16 levelCount);
    void (*on_mip_levels_generated) (MGPipeHandle res, Uint16 base, Uint16 count);
    void (*on_surface_changed)      (const MGPSurfaceInfo*);
    void (*on_caps_invalidated)     ();
    void (*on_log)                  (Uint8 level, const char* text);
};
```

gallium 没有 shadow writeback、GPU-write 通知、纹理重发请求、default-FB 几何这些词汇——因为在 Mesa 里 state tracker 与 driver 共享地址空间。**把它们具名化为 9 个回调，好过藏在 95 个 poke 点里。**

### 7.2 95 个写回点的逐族归属

| 族 | n | 变成什么 |
|---|---|---|
| `SyncPersistentMappedRange` | **20** | **作为反向调用彻底消失。** 每一处都紧挨着一次对客户端字节的 CPU 读，而那些读全部搬到了 client（§5.8）；**tracker 在填 `MGHostSpan` 之前**做同一次 reconcile。**这是整份设计里最大的一处结构性简化**，而且它在 monolith 里也是净改进——今天这 20 处是 backend 在 draw 中途回头调前端 |
| `MarkStorageDirty` | **18** | 16 处是 server 本地记账（"我上传了这个 level"）——**零消息**，因为 dirty 归属反转（§7.3）。2 处 `true`（`Managers.cpp:2813`、`DirectGLES.cpp:6852`）变 `on_texture_pull_request` / `on_texture_writeback` |
| `AllocateStorage` | **8** | 6 处是 **backend 凭空造出来的前端对象**（Magma 的占位纹理、`SwapchainObject` 的 default-FBO 颜色/深度/模板占位，`SwapchainObject.cpp:284, 305, 329`）→ **server 原生，永不上线**；1 处是生成 mip 的 shadow（`DirectGLES.cpp:6261`）→ `on_mip_levels_generated`；1 处是 swapchain 尺寸变更 → `on_surface_changed` |
| `WritebackFromBackend` | **8** | `MGPReplySlot`（回读）+ `on_buffer_writeback`（XFB 捕获、PBO 回读）。**必须按操作级批处理**：其中两处今天在循环里**逐行**写回（`Utils.cpp:2342`、`DirectGLES.cpp:7633`），绝不能变成"每扫描线一次 IPC" |
| `SetInternalFormat` | **7** | 与 `AllocateStorage` 同批：server 原生或 `on_surface_changed` |
| `SyncGpuWrites` | **6** | 同 `SyncPersistentMappedRange`：变成 tracker 发射前的 reconcile |
| `MarkGpuWritten` | **6** | client 在每个 draw/dispatch 发射点**保守自建**，镜像 `DirectGLES.cpp:459-467, 509, 1809` 与 `UniformManager.cpp:1073, 1229`、`VulkanRenderer.cpp:11210` 的输入（client 全都有）。`on_gpu_written{res, ranges[]}` 是**收窄**通道——因为 server 拥有描述符解析，它能比今天"任何绑了 SSBO 的 draw 一律置位"**更精确** |
| `RecordError` | **6** | `on_gl_error`，**必须对命令流有序**（§7.4） |
| `SetBackendResource` | **4** | **删除。** server 拥有资源表；client 只留 `{slot, gen}`。`resource_destroy` 取代由 `~BufferObject` 驱动的 `OnDestroy`；pooling / 延迟释放（`Managers.cpp:1271-1300`、`VkBufferManager::OnResourceDestroyed`）原样搬到 server 侧 |
| `EnsureGpuResidentStorage` | **3** | server 本地决策（`map_persistent` 的档位在 IPC 期决定，§8.3） |
| `SetBackendHashMemo` / `SetBackendAuxMemo` | **3** | 纯值 → server 侧 per-slot 字段（Magma 的 `VaoDrawMemo` 已经在镜像其中两个） |
| `InvalidateCompileEnv` | **2** | `on_caps_invalidated`，context 重建时，低频 |
| `SetBackendStateMemo` | **1** | **直接删除，不翻译**（D12：前端对象里的后端堆裸指针，无 wire 对应物） |
| `UpdateMipmapSubData` / `TruncateMipmapLevels` / `SetSamples` | **3** | 全在 Magma 的占位纹理里 → server 原生 |

**6 处 backend 反向进 `MG_Impl`：** 四处 `pDefaultFramebufferInfo` 身份比较（`DirectGLES.cpp:1917, 2838, 2867, 9675`）→ 保留 handle `{0,1}` + `MGPFramebufferState::isDefault`；`SwapchainObject.cpp:276-330`（backend **创建** default FBO 的三张 `ITextureObject`）→ `on_surface_changed{extent, colorFormat, depthFormat, stencilFormat}`，client 自己合成对象——**顺带删掉 monolith 里的一处分层倒置**；`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`，唯一一处真正无法 handle 化的调用）→ `get_texture_image` 返回 **"该 level 无 GPU 背书，请从你自己的 shadow 回答"**（`:10691-10704` 今天测的正是这个条件），client 跑它本来就拥有的 pack/convert 路径。

### 7.3 纹理 dirty 归属反转

**client** 保留 `MipmapStorage` 的模型（96-rect 级联合并 + `summedArea*4 >= unionArea*3` union-box 回退，`MipmapStorage.cpp:300-305`），维护一份**逐 `(texture, uploadTarget, level)` 的发射游标**，在发射后清自己的标志。**server 从不碰 client 的标志。**

这是安全的，且已核实：**`MG_Impl` 里没有任何 `IsStorageDirty(` / `GetStorageDirtyRects(` / `GetStorageDirtyRegion(` 调用点**（前端从不读自己的 dirty 状态），而它自己在五处主动清（`GL_Texture.cpp:528, 701, 5547, 5621, 5691`）。

**这一条删掉 `PLAN.md` §5.6a 的整个 ack 协议与风险 R6。**

**但上传形状决策留在 server**：`resource_subdata` 同时带 union box 与 rect 列表（§4.5.6），Mali 按作业数计价的 +6ms/frame 悬崖（`Managers.cpp:4311-4319`）在哪一侧付 GPU 代价，决策就留在哪一侧。

### 7.4 反向通道的有序性是正确性要求，不是优化

**`on_buffer_writeback` 必须与 epoch bump 有序。** 今天每一次 `WritebackFromBackend` 后面都紧跟一次 `BumpBufferMutationEpoch()`（`DirectGLES.cpp:834-837, 942, 7625-7629`），因为写回会 bump client 的 change serial 而没有对应的 op，否则 server 自己的 draw-clean memo 会在 epoch 背后变陈旧。split 里这变成**反向通道上的一条排序规则**：一次写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 侧应用。**反向通道需要与正向通道相同的有序保证。**

**`on_gl_error` 必须对命令流有序**，否则 `glGetError` 答错。分配类入口（`glRenderbufferStorage*`、可能失败的 `glTexImage*`/`glTexStorage*`/`glCopyTexImage*` 形式、`glBufferStorage`）带 `kNeedsAck`，让 OOM 探测惯用法（`allocate; if (glGetError()==GL_OUT_OF_MEMORY) 用更小的重试;`）成立；其余晚到。`glGetError` 本身永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`）。

**对 `PLAN.md` §7.4 的强制修正：`on_log` 必须按严重级分级。** `PLAN.md` 把**全部** `EvLogLine` 设为有损（覆盖最旧 + `eventDropped`）。但 §5.7 已经确认：**backend program link/compile 失败只以一行日志加一次 bind-program-0 的空 draw 呈现**，没有 GL error、没有前端变更、`GL_LINK_STATUS` 永不撤回。在统一有损策略下，系统里诊断价值最高的那一行会在日志压力下静默消失。

**规则**：`on_log(level ≤ WARN)` 有损（`eventDropped` 计数）；**`on_log(level ≥ ERROR)` 无损**，加入触发 `eventRingFull` + 停止 apply 的语义事件集。为了不让这条重新引入 §7.4 本来要防的死锁，再加一个**每秒 ERROR 速率限制器**，超限时发一条显式的 "N errors suppressed"。`MGLOG_E_ONCE` 的 latch 变成 per-server，这正是想要的行为。P9 的故障注入门：日志洪泛下注入一次 link 失败，那行 ERROR 必须出现。

### 7.5 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素字节，所以三个原因会要求 client 重发已发过的 level：`RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）。**上一轮薄 server 设计正是因为把这条一笔带过而被判死。三条缓解同时上，加一个专门的门和一个必须发布的计数器：**

**(a) 预防主因。** client 给纹理打 `everImageBound` 标记，`resource_create`/`respecify` 一直携带 `imageBindableHint`（`MGPResourceDesc`），于是 image-bindable 存储在前期就分配好。这把 `RequireImageBindableStorage` 从稳态里彻底移除。

**(b) 拉取是异步的。** server 发 `on_texture_pull_request{res, target, levels[]}` 并把那个 twin **标为 not-ready**；client 在下一次 publish 时重发。因为 client 跑在前面，常见情况下字节在 server 到达采样该纹理的 draw 之前就到了；即使没到，**阻塞的是 `mgl-srv-apply` 线程，不是应用线程**——这是一个应用没有发起的停顿应该待的地方，也严格好过 monolith 里阻塞应用线程的行为。

**(c) 有上限的保留。** 可选的逐纹理保留位，受一个**显式的 LRU 字节预算**约束（默认 32MiB，`MOBILEGL_PIPE_TEXEL_RETAIN_MB`），使一张被拉过一次的纹理永不第二次停顿。

**(d) 门与计数器。** 新增 `TextureRemintPullScenario`：同时强制 `RequireImageBindableStorage` 与一次帧中格式再生。**拉取次数逐 trace 用例发布**，与 SSIM 并列。**本设计从不声称"零 round trip"，它测量并公布。**

若在真实语料（MC 与 Iris fixture）上实测拉取率非平凡，(c) 从可选升级为强制并拿到真预算。

---

## 8. 传输、数据面、同步、present、线程、平台、构建

### 8.1 原样继承方案 A 的部分

以下全部**逐条继承 `PLAN.md`，本文不复述**。它们是传输/平台/节奏/构建机制，与状态模型无关，且已经过对抗性评审：

| `PLAN.md` 章节 | 内容 |
|---|---|
| **§6.1** | 段布局（`SEG_CMD` 8MiB / `SEG_STAGE` 32MiB↑ / `SEG_REPLY` 8MiB / `SEG_EVENT` 256KiB / `SEG_SHADOW[n]` / `SEG_ADOPT[n]`）；shm 创建矩阵（`ASharedMemory_create` API 26 / `SYS_memfd_create` / `shm_open` / `CreateFileMappingW`）；**`SCM_RIGHTS` 必须在第一个 transport commit 里实现**（asio 无 cmsg API → 在 `socket.native_handle()` 上裸 `sendmsg`/`recvmsg`，~80 行；`Feat/CS-Delta-IPC` 把 `out->fd = -1` 硬编码在 `LocalSocketTransport.cpp:296`，它的数据面在唯一重要的平台上一个字节都没过去）；`SEG_SHADOW` 块的 pending free-list 退休规则 |
| **§6.2 / §6.2a** | `RingControl`：两组独立游标三元组、三个不同的 seq 水位（`appliedSeq`/`submittedSeq`/`retiredSeq` + `completedFrameSerial`/`presentAckSerial`）、`serverEpoch`、`ringGeneration`、`consumerParked`/`producerParked`、`eventRingFull`/`eventDropped`；**双向 doorbell**，`MOBILEGL_IPC_SPIN_US` 默认 50µs，`inproc` 用 condvar。没有这条，每一次 client 等待都退化成跨进程自旋一条共享 cache line，在手机上就是一颗大核满频空转，而本库全无亲和性控制 |
| **§6.3** | 记录格式：8B `RecHeader`、24B `BlobRef`、**无 per-record 序号**、X-macro 每种一条 `static_assert` **加**生成的运行期边界检查 → `Fatal{ProtocolCorruption}`、`kVarTail` 自描述长度自洽校验。方案 B 把这套机制扩展到**全部** MGPipe 调用（G3） |
| **§6.4** | WAR 纪律：调用时刻拷进 ring slot（P1-4）；P4.5 的 `SEG_SHADOW` 零拷贝 + 逐 shadow 64KiB 块发送水位 |
| **§6.5** | ring 分配与背压：逐字移植 `PersistentRing`（`Managers.cpp:641-727`、`RingAllocateSlow` `:1891-1970`、`RingOnPresent` `:1975-2016`），升级路径 扩容 → 走 doorbell 的有界等待 → 硬 drain + `ringGeneration` bump |
| **§6.6 前三条** | unpack PBO 完全在 client 解析；压缩 internalformat 永不到达 backend；`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client |
| **§6.7 第 2、5 行** | PBO 回读改 fire-and-forget（**严格优于 monolith**，后者无条件停等，`DirectGLES.cpp:9189-9205`）；`glEndTransformFeedback` 的无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`）推迟到首次读 |
| **§6.8** | persistent map 与 ≥16MiB 采纳的三档，**由运行时 POST 探针选择，绝不硬编码驱动名** |
| **§7.1** | FlatBuffers 纪律：热路径 `struct` 进 ring、罕见/变长 `table` 走 socket；`protocol_generated.h` 提交；`gen_protocol.py` + CI `flatc-check`；**默认构建图里没有 `flatc`**（`Feat/CS-Delta-IPC` 的 NDK 陷阱本体） |
| **§7.2** | 帧封装（`MGLF`、64MiB 上限、读时校验、缓冲不足返回所需大小**且保留消息**）；publish 触发器（每记录 release-store `cmdHead`、显式门铃点、`SEG_STAGE` 余量 < 1/4、**轮询入口也是门铃点**、`GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 publish、`MOBILEGL_IPC_POLL_ESCALATE` 饥饿升级）；**`glFinish`/`glFlush` 保持免费**（`Definitions.cpp:111-112`） |
| **§7.3** | 两个互相独立的窗口（字节 credit、present credit）；server 不发 credit 消息，只做 release store + 反向门铃 |
| **§7.4** | 事件 ring + 排空点 + 溢出策略（等待循环内必须排空；语义事件无损，满时 `eventRingFull` + 停在记录边界停止 apply）。**加上 §7.4 的分级修正** |
| **§8 末尾** | fence 完成度必须来自**真的逐 fence 退休**，不是 present 水位（`DirectVulkan.cpp:1120-1128` 记录这是被修掉的 bug；`magma-mc1215-fence-oom` 记录它曾导致 native-heap OOM kill）；三个应先独立落 `dev` 的 monolith 修复 |
| **§9 / §9.1 / §9.2 / §9.3** | `Present` 与 `eglSwapBuffers` 严格 1:1、绝不批量（Magma 的四次 `OnFrameBoundary`、`TryDrainFrameTransients` 与全部四次 `BeginFrame` 只在 `Present` 内发生，`VulkanRenderer.cpp:12765-12904`；Espryt 在那里 retire 三个 ring 与 `TrimBufferPool`，`DirectGLES.cpp:10646-10649`）；`MOBILEGL_IPC_PRESENT_CREDIT` **默认 1** 与延迟叠加公式；Magma 从不注册 `SetSwapInterval`（`BackendObject_DirectVulkan.cpp:698`）所以 IPC credit 是它唯一的显式限帧器；DirectGLES 的非 present fence tick |
| **§10** | 线程模型：v1 不加 client 线程、flow = per context、外来线程的 sync/query 从 `RingControl` acquire load 回答、`ShaderCompilePool` 留 client、修 `EGLOperationMutex` 的两个漏洞；server 的 `mgl-srv-io` + `mgl-srv-apply`（终身持有原生 context）；核心放置与 `MOBILEGL_IPC_SERVER_AFFINITY`、**报逐线程 CPU 时间**；拆机顺序 |
| **§11.1-§11.6** | 启动与握手（`MOBILEGL_IPC_SERVER_PATH` 为主 + `dladdr` 兜底；socketpair + fork/execve；**显式 envp 剔除 + `mobilegl_server_main` 内强制 Monolith** 的双保险）；`extern "C" visibility("default")` 与 `nm -D` 门；Android 的 `android:process=":mgl"` Service 路径（minSdk 26 无公开 `ANativeWindow` 扁平化）；X11 XID；`EGL_PLATFORM=surfaceless`；Windows overlapped named pipe；崩溃时的 device-lost latch |
| **§12 第 1-3 层 / §12.4** | 编译期折叠（`MOBILEGL_BUILD_DISAGGREGATED` 默认 OFF）；**唯一 hook 点** `MG_Backend/Init.cpp:48-70`，下游 ~152 个边界调用点零 `#ifdef`；P4.5 的 allocator 改动整段包裹；`MOBILEGL_TRANSPORT` 复用全部既有开关通道（ctest `ENVIRONMENT`、trace-replay 的 `setenv` 块、FCL 用户 env、plugin APK V2 开关表、`/data/local/tmp` CTS） |
| **§13** | 目录形状；`MobileGLServer` 作为改名成 `lib*.so` 的 `add_executable` 链接**共享**的 `MobileGL`（一份库两个角色，版本必然匹配）；FlatBuffers submodule 的 `EXISTS` 保护 **加** include-dir guard；ctest/trace-replay 的三个陷阱（`ENVIRONMENT` 是替换而非追加、`;` 转义、property 覆盖 job env）；`SPLIT` 后缀与 `-DTRACE_TRANSPORT=`；CI 的 `flatc-check` 与 `fprintf` grep 门 |
| **§14** | 对 `Feat/CS-Delta-IPC` 的 REUSE / CHANGE / DROP 判定（本文 §14 只列差异） |
| **§15 P0** | 卫生清单与两个 spike，尤其是**两侧的 `TracyPlot` 字节计数器** |

### 8.2 与方案 A 的差异

**删除：**
`Server/ReplicaContext.{h,cpp}`（换成 `Server/PipeObjectTables.{h,cpp}`：POD 记录的 slot 数组，不是状态模型）；§5.0 的"replica vs 重写"决策；§5.1 的三步发射协议；§5.2；§5.4 的 replica 对象表规则与 `Fatal{IdentityDivergence}`；§5.6a 的纹理 ack 协议；§5.7 的 Phase 1-4 composite 分支；§5.9b 与整个 mutation 覆盖生成器（`gen_impl_mutation_surface.py`、`MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::` helper 族）；§6.9 的 relink 档与 `MOBILEGL_IPC_PROGRAM=publish|relink`；§6.4 的拷贝第 (3) 行；§12.2 的 `pGLContext` shim；阶段 **P5**（6 天回收）；风险 **R1** 与 **R6**；开放问题 **§17-5**。

**改变：**

| `PLAN.md` § | 差异 |
|---|---|
| §5.9a | READ 面的**编目**生成器变成**禁止**门（§4.7.2）：不是"每个读都映射到某个 delta 类"，而是"这次读根本编译不过"。原 477 行 inventory 保留为 tracker 侧覆盖检查表（G6） |
| §6.4 拷贝账 | 第 (3) 行 `SEG_STAGE → replica shadow` 不存在：**P1-4 = 3 次，P4.5 = 2 次**。`PLAN.md` 自己的"方案 B"目标（第 549 行，被它标为"激进、需额外设计"并推到 P6）**按结构达成**，开放问题 §17-5 自动关闭 |
| §6.6 第 4 条 | 逐 level `serverAuthoritative` 位被 dirty 归属反转（§7.3）+ `on_texture_writeback`（一个生产者：CPU 生成 mip 路径；CopyImage 镜像已搬到 client 而消失）+ `on_texture_pull_request` 取代 |
| §6.9 | `RecProgramLinkOp` **不可能**（§5.7）。`ProgramPublish` 第一天；`reflectionDigest` 从"分歧预言机"换成"schema 完整性绊线"；P5 消失，其副产品（内部 shader 烘焙）升级为 Magma 阶段的前置条件，`nm -D | grep glslang` 为空成为验收判据 |
| §5.10 | 第 2、3 条**逐字继承**（client 侧块粒度 persistent 推送、`PersistentCoherentMapScenario` 作为门，**R2 仍是最高优先级正确性项**）。第 1 条从"完整 map 状态复制"缩成**一个推送的 `hasLiveHostWrites` 位**——因为 replica 的 `SyncPersistentMappedRange` 与 `IsBufferDrawClean` 的 `IsMapped()` 门（`Managers.cpp:1447`）在 server 侧都不存在 |
| §6.10 | 四类应用指针由 §5.8 的 cap 门控 lowering 处理；`ClientArrayBounds` 变成 `MGPDrawInfo::minIndex/maxIndex`。**陈旧索引纪律（§5.8.1）一字不改地保留并加门** |
| §7.4 | `on_log` **按严重级分级**（§7.4），加每秒 ERROR 速率限制器 |
| §12.2 | 需要角色隔离的进程全局从 **4 个降到 2 个**：`MG_State::pGLContext` 整个退出（server 不托管 `GLContext`，因此跨越 `MG_Impl` 1494 个站点的 `operator->` shim 与 Android dlopen TLS 论证一起消失）；`pDefaultFramebufferInfo` 变成接口输出（§7.2）后也退出。只剩 `gBackendFunctionsTable`（在方案 B 里是 `gPipeCtx`）与 `pActiveBackendObject`。**这实质性降低 R11 的风险，并让 `inproc`——最早的证伪门、且完全不需要跨进程工作——便宜到可以提前跑** |
| §13 | `MG_Pipe/` 是**默认构建里的非可选目录**（它是 monolith 的架构）；只有 `MG_Remote/` 在 `MOBILEGL_BUILD_DISAGGREGATED` 之后 |
| §15 | **在 P1a 之前新增一整段 backend 推送改造**。`PLAN.md` 把这块工作定价为"~0 逻辑改动"；在方案 B 里它是工作量主体，并有它自己的 monolith A/B（§10.3） |

**新增：**

- **`SEG_STAGE` 尺寸必须额外容纳四类它以前不承载的字节**：client 顶点数组、client 索引数组、multi-draw 参数块、client 解析后的 indirect 命令块。**字节体量不变**（它们今天就在每 draw 被重新上传，`glBufferData(..., GL_STREAM_DRAW)`，`Managers.cpp:~2578`），但现在这些字节要过一个 ring slot。上限由 P0 落地的计数器实测定，不用默认值猜。
- **新事件种类**：`on_texture_writeback`（CopyImage 镜像搬走后只剩一个生产者：CPU 生成 mip 路径 `DirectGLES.cpp:6811-6861`）、`on_texture_pull_request`、`on_mip_levels_generated`。`on_buffer_writeback` 从"优化"升级为**承载语义**。
- **新环境变量**：`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_VERIFY`、`MOBILEGL_PIPE_STATS`、`MOBILEGL_PIPE_TEXEL_RETAIN_MB`（见附录）。
- **`RenderbufferObject::GetLifetimeId()`**（今天没有）。**但不需要 `GetVersion()`**——`PLAN.md` P0 要求两个，`GetVersion()` 只是 replica 的 delta 触发器；在推送模型里 `glRenderbufferStorage*` **本身就是**一次 pipe 调用，不需要触发器。

### 8.3 persistent map：唯一被显式隔离的传输相关决策

`AcquirePersistentMap`（`BufferObject.h:112`）是**永久的地址空间捐赠**（D4/D-B4）。**它原样穿过 monolith 改造（P0..P13 一动不动），只有 IPC 那一步才打破它。** 决策路径：

- **P0 的 spike B 在第一周给方向**：导出 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，client `mmap` 后回读，在 `35d0befa`（Adreno 830）与 `3B159D009VZ00000`（Mali）两台上跑。
- **T2（拒绝，永久正确的回退）**：返回 `nullptr`，前端已在三处容忍（`BufferObject.cpp:174, 439-442, 470-472`）。**此档下 `PLAN.md` §5.10 的 client 侧块粒度推送是强制的**，由 `PersistentCoherentMapScenario` 把门。
- **T1（server 导出自己的映射）**：每 store 生命周期一次 round trip；采纳成功后推送自动停止（`IsGpuResident()` 早退），与 monolith 一致。
- **T0（server 导入 client 分配）**：理想但可用性未知。

若两台设备都否，IPC 期的该阶段从 8 天缩为 2 天的文档与负面对照，代价记录在案。**绝不允许一个平台未知数挡住 200 天的接口工作。**

---

## 9. Roundtrip 清单与稳态零 roundtrip 论证

### 9.1 稳态零 roundtrip 的项

| 类 | roundtrip | 依据 |
|---|---|---|
| 全部 draw、clear、blit、copy、dispatch、barrier、XFB 跨度标记、全部 bind、全部 CSO create/bind、全部 `set_*`、全部 buffer/texture 上传、`present` | **0** | 单向记录；present 只查 credit |
| **全部 89 个 caps 站点**（40 `pActiveBackendObject->` + 全部 `glGetIntegeri_v` + `IsTimerQuerySupported` + `PrefersCpuXfbPrimitiveAccounting` + `BeginOcclusionQuery != nullptr` 能力探测） | **0** | 首次 `MakeEGLCurrent` 的一次 `MGPCaps` 快照。caps 只在那一刻才存在（`BackendObject.cpp:341-347`，每次 surface 变更重新武装 `:301`）；`callMask` 精确复现 DirectVulkan 少注册的槽位 |
| `glGetError` / `glFinish` / `glFlush` | **0** | 前者永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`），后两者是彻底的 no-op（`Definitions.cpp:111-112`）**且必须继续免费** |
| fence 与 query 的**创建**，以及每一次**非阻塞轮询**（`GetSyncStatus`、`ClientWaitSync(0)`、`IsQueryResultAvailable`、`GetQueryResult64(wait=false)`） | **0** | handle 由 client 铸造（`GL_Sync.cpp:61` 今天就把前端堆对象 reinterpret 成 `GLsync`；`GL_Query.cpp:50-54` 铸造 id）；未命中合法地答 `GL_UNSIGNALED`/"未就绪"，两处契约明说（`BackendObject.h:210-214`、`:236-241`），而前端已经遵守（`GL_Query.cpp:302-311`：读 0、**不缓存**、保留 handle） |
| `glGetTexImage` / `glGetTextureImage`（**DirectGLES**） | **0** | client shadow 回答（`CopyTextureImageToClientOrPBO_State`，`GL_Texture.cpp:5368-5420`，取用点 `:6460`） |
| `glReadPixels` → pack PBO | **0** | fire-and-forget + client 侧 `MarkGpuWritten`。**严格优于 monolith**，后者无条件停等（`DirectGLES.cpp:9189-9205`） |
| `glEndTransformFeedback` | **0** | 取消无限 fence 等待（`GL_Drawing.cpp:1326-1337`），改为对 capture target 置 `MarkGpuWritten` |
| `eglSwapBuffers` | **0 次阻塞 round trip**，一次非阻塞 credit 检查 | 只有 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（默认 1）时才阻塞 |

### 9.2 不可避免的阻塞点（全部罕见，逐条给理由与缓解）

| # | 站点 | 为什么不可避免 | 缓解 |
|---|---|---|---|
| 1 | 握手 `Hello`/`Welcome` + 段 fd 传递 | — | 一次 |
| 2 | `InitializeEGLDisplay`（写 `major`/`minor`）、`Create/Resize EGL*Surface`、首次 `MakeEGLCurrent` + `InitCapabilities` | 出参 / 返回 `Bool`；caps 只在那一刻存在 | 每 surface 至多一次；surface 回复顺带 `MGPSurfaceInfo`（**同时删掉 `SwapchainObject.cpp:276-330` 往前端对象里的写**）。`SwapEGLBuffers` 不需要回复：它的线程归属校验对 client 镜像的 EGL 状态求值（`BackendObject.cpp:365-393`） |
| 3 | `glReadPixels` → 客户内存 | GL 要求返回时字节已就位 | 像素进 `SEG_REPLY` slot；**逐行写回循环留在 server 内，按操作级批成一段** |
| 4 | `glGetTexImage`/`glGetTextureImage`（**DirectVulkan**） | Magma 对只存在于 GPU 的 level 没有 client 可答的 shadow | `get_texture_image` 对"无 GPU 背书"的 level 返回"请从你的 shadow 回答"（`VulkanRenderer.cpp:10691-10704` 今天就在测这个条件），所以只有真正 GPU-only 的 level 付钱 |
| 5 | GPU-write pending 的 buffer 首次 CPU 读（`glMapBuffer*`、`glGetBufferSubData`、`glGetNamedBufferSubData`、`glCopyBufferSubData` 源、`FillSubData`） | shader 在前端背后写了 store | monolith 里**本来就阻塞**（`glFinish()`，`Managers.cpp:1246`；Vulkan `VkBufferManager.cpp:80-85` → `VulkanRenderer.cpp:9807-9817`）。client 保守 pending 集触发，由 `set_shader_buffers` 的 `writableMask` 与 `on_gpu_written{ranges}` 两侧收窄——**比今天"任何绑了 SSBO 的 draw 一律置位"更精确** |
| 6 | `glClientWaitSync(timeout>0)`、`glGetQueryObject*(GL_QUERY_RESULT)` 未完成、`glBeginConditionalRender` | GL 定义即阻塞；`glBeginConditionalRender` 连 `_NO_WAIT` 模式也阻塞（`GL_Query.cpp:705-706`） | 全部非阻塞兄弟是 0 round trip（§9.1）。条件渲染的谓词**只解析一次**（`Core.h:387-391`），之后每个条件 draw 在 client 侧丢弃（`ConditionalRenderDiscardsCommand`，用于 `GL_Drawing.cpp:711, 740, 795`），**server 永远不需要那个 query 对象** |
| 7 | 分配类入口的错误 ack | OOM 探测惯用法 | `kNeedsAck` 只标这一族；罕见且本来就贵 |
| 8 | `map_persistent`（仅 T1 档） | 应用必须拿到一个不再经过任何 API 调用就能写的地址 | **每 store 生命周期一次**，不是每次使用；档位由 POST 探针选（§8.3） |
| 9 | **server 发起的纹理重铸拉取** | server 不保留纹素 | **三条缓解 + 专门的门 + 逐用例发布的计数器**（§7.5）。异步形态下阻塞的是 `mgl-srv-apply` 而非应用线程 |
| 10 | client 侧索引扫描 / restart 重写 / indirect-count 解析，当源 buffer 在 pending 集里 | monolith 在**同一位置**调 `SyncGpuWrites()` | §5.8.1 的 publish/wait/drain 纪律；常见情况不 pending，代价为零 |
| 11 | ring/stage 耗尽、present credit | **节奏，非语义** | `PersistentRing` 的升级路径 + `producerParked` doorbell（不是自旋） |

### 9.3 论证的形式：测量，不是声称

**验收门措辞（`PLAN.md` §15 P3-2 的措辞加一行）**：在**全部 40 个 trace 用例**上发布**逐用例的 roundtrip 计数器与纹理拉取计数器**。**不做笼统的"零 round trip"声明。** 条件渲染与阻塞 query 的次数按用例列出而不是宣称为零。

轮询挂死的防护（继承 `PLAN.md` §7.2/R4）必须有它自己的门：`glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 必须在有界时间内退出——不加 publish 规则它会永远挂住。

---

## 10. Monolith 保留

### 10.1 接口在进程内就是直调

monolith 模式下 `MGPipeContext` 用 backend 自己的函数填充，`MGPipeCallbacks` 用对 `MG_State` 的直调填充，`MGHostSpan.ptr` 指向 client 自己的 shadow（**零新增拷贝**），`MGPipeHandle` 按值走一对寄存器。split 模式下同一张表换成发射器，applier 反序列化后调**同一批 backend 函数**。**全世界只有一份 backend 实现。** 这比方案 A 的安排更强——后者两侧都跑 `MG_Impl`。

### 10.2 热路径的间接成本，诚实版

| | 今天 | 之后 |
|---|---|---|
| 每 verb 的分发 | 1 次间接调用（**已经在付**：`gBackendFunctionsTable.GL.DrawArrays(...)`，89 个 `MG_Impl` 站点） | 1 次间接调用 |
| 每 draw 的状态获取 | Espryt 124 / Magma 169 次 accessor 调用 + 版本比较 + 一次 ~1.2KB 三段 memcmp + `CurrentUnitBindingsEpoch` 的逐 unit owner 走查 + Magma 的两次有损版本求和 + ~40 次 payload accessor 走查 | 1 次 dirty word 测试 + N 次 `set_*` 间接调用（稳态 N=1-3；CSO 内容寻址下重复状态 N=0） |
| memo 查表 | 对指针位做斐波那契散列的直接映射探测 + owner 相等性 | 按 slot 的数组下标 |
| 删除的机制 | — | **~550 行 per-draw 失效发现**（§2.5） |

**推送在稳态应当是严格的减少——但只因为 tracker 在 draw 时刻 validate。** 如果有人把它实现在 GL setter 时刻，它会更慢，而 trace 语料会立刻说出来（§5.1）。

两个诚实的告诫：
1. **可达性遍历是搬走了，不是消失了**，所以头号指标必须是**逐线程 CPU 时间**，不是墙钟帧时。
2. **Magma 的 `SetupDrawSnapshot` 快路径命中率在两种模式下会合法地不同**，所以 A/B 比的是**渲染输出与计数器**，永远不是 memo 轨迹。

两个 backend 编进同一个共享库（`CMakeLists.txt:356-383` 的源文件列表，`add_library(... SHARED ...)` 在 `:485`），backend 在 init 时锁存一次（`ConfigLoader.cpp:212-225`），所以去虚化在两种形态下都不可得，也都不需要——代价是一次已经在付的间接调用。**函数指针 struct 而非虚基类**的理由见 §4.1。

### 10.3 替代字节一致门的五部分验证门

**先把成本写在明面上**：`PLAN.md` §12 第 4 层（`nm --defined-only` + 剥调试信息后 `.text` size **相等**，且每阶段都跑）在方案 B 里**按构造死亡**——backend 停止读 `pGLContext`、`PrepareForDraw` 的遍历搬到 client、~66 个 memo 族换键、`MG_Impl` 多出 validate 调用、Magma 的两处 `MG_State` 类型内部用法被重写。**不存在任何配置能让旧字节回来。** 这是方案 B 的代价，必须写进设计文档而不是藏起来。

替代品是**五部分，全部在 P0/P1 落地**，这样每一条都能因它存在的理由变红：

**① 接口纯度门（机械，取代读点编目生成器）。**
`grep -c 'pGLContext' MG_Backend/` == **0**（grep 的是 `pGLContext` 不是 `pGLContext->`，因为还有 58 行非箭头用法，§2.4）；`MG_Backend` 只允许 include 一张共享**值**头白名单（今天是 50 行 include / 18 个不同 `MG_State` 头）；`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**外加**一条 debug 断言"每个 backend memo 键都是 `{slot, gen}` 对，永不是裸前端指针"，由 `HandleRecycleScenario`（删除并重建对象、跨 draw 边界复用同一个 GL name）支撑——**这个场景在 0d 重键之前必须在至少一个 backend 上是红的**，否则它没在测任何东西。

**② 语义影子比对（`MOBILEGL_PIPE_VERIFY=1`）——决定性的那一条。**
阶段 B 期间两套状态模型活在同一个地址空间：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 生成的比对器**逐字段**、**每 draw** 与推送版本比对，打印第一个分歧字段名与 draw 序号。它抓三种事：(a) tracker 忘了推的字段；(b) **dirty 位触发得太少**——危险的那个方向；(c) 两条路径上被变换得不一样的值。作为第三种 CI 模式跑全部 40 个 trace 与 367 个集成测试；~5-10× 慢，永不出货。
**必须逐字段比而不是 `memcmp`**：`DirectGLES.cpp:2029-2033` 明确记录 `RenderStateParameters` 的 memcmp 会因 padding 字节 false-DIFFER（无害）但永不 false-match——比对器要的是零误报。
**这是历次评审在每一个候选设计上指出的那个缺失，而它只有在接口先落 monolith 时才存在。**

**③ 行为 A/B。**
全部 ~40 个 trace 用例（`tools/trace_replay/trace_cases.json`，默认 SSIM 阈值 0.99；OpenRA、MC 1.17/1.21.4/1.21.11 族、15+ Iris shader pack、两个 Create fixture、`improved-transparency-minecraft-26.3`）在 `{monolith-pull, monolith-push, split}` 三种下同一判定、SSIM ≥ 0.99；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.`（以及 DirectVulkan 对）之间产生**逐名相同**的通过/失败集（367 个 `TEST_F`）；428 个单元测试全绿；CTS 逐后端 conformance 在 0.5 个百分点内，按本项目的逐后端表格式上报（行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母）。
**两个 Create fixture 带 `coherent_as_flush: true`**，必须在两种模式下都开着该开关跑，否则逐名对比没有意义。

**④ monolith 性能不回归。**
两台设备（`35d0befa` Adreno 830、`3B159D009VZ00000` Mali），reboot-clean、同热窗口、配对 A/B，用 `tools/bench.sh` + trace replay 的 `--benchmark --benchmark-tail-frames --benchmark-result` 逐帧 JSON。**指标是逐线程 CPU 时间**（因为遍历是搬走不是消失），monolith-push 在 **p50 与 p99** 上都要落在 monolith-pull 的噪声内。CPU 定频按本项目协议（大核 1.96 / 小核 1.55GHz，GPU 拉满，40°C 门槛）。

**⑤ 覆盖 + poison + handle 纪律。**
`gen_pipe.py` 重生成 477 行 inventory 的 MGPipe 映射列，0 UNMAPPED，`git diff --exit-code`；`PipeInputs::m_filledMask` poison（§6.2.2）；P13 的 `static_assert(sizeof(ResidualValueBlock) == 0)`；`ResidualValueBlock` 的逐成员 `offsetof` 断言（§6.3-2）。

**两条字节级等式仍然幸存，仍然作为断言保留**：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加任何库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中（`CMakeLists.txt:497-510` 在非 Debug 下设 hidden visibility）。
**符号与 `.text` 漂移每阶段作为信息性指标发布**——一次无法解释的跳变仍然是一个 smell，只是不再是一条断言。

### 10.4 monolith 侧净收益清单（即使 IPC 永不上线也成立）

1. **~550 行 per-draw 失效发现机制删除**（§2.5），连同运行它的每帧代价。
2. **复用地址 ABA 一整类不可表达**：D1/D2/D3/D10/D11/D13/D14/D16/D17/D20 全部由 `{slot, gen}` 关闭。
3. **FBO → program 排序 hazard 消失**：`DirectGLES.cpp:2712-2732` 的 fragColor 重推导 workaround 与 `g_broadcastMemo*` 删除（D-B3）。
4. **一处分层倒置消失**：`SwapchainObject.cpp:276-330` 不再往 `MG_Impl` 的 `pDefaultFramebufferInfo` 里写。
5. **两个潜伏 bug 顺带修掉**：D21（`m_xfbCounterSlotByObject` 用裸 GL name 做键，`VulkanRenderer.cpp:11136-11146`，删了又建的 XFB 对象会**恢复**一次本该重启的捕获）与 `RenderbufferObject` 缺 `GetLifetimeId()`。**这两条都值得先独立落 `dev`。**
6. **一个死能力被暴露**：`CapabilityInput::FramebufferSrgb` 与 `DepthClamp`（`RenderState.h:165, 168`）**没有任何存储**——`SetCapability` 落到 `default: // not supported currently`（`RenderState.cpp:380`），`IsCapabilityEnabled` 返回 `false`（`:428-429`）。**六个 backend 读点（`DirectGLES.cpp:2133`；`VkTextureManager.cpp:951`；`VkRenderPassManager.cpp:613, 965, 1111`；`VkClearManager.cpp:57`）今天恒为 false。** 把接口写下来这件事**发现了**它。这是潜伏 bug 还是有意为之，**必须在渲染状态 blob 冻结之前回答**。
7. **一次 glslang 编译离开 monolith 启动路径**（Magma 的内部 shader 烘焙，§6.1-2）。
8. **`inproc` = monolith 的渲染线程**，且在方案 B 下只需隔离两个进程全局而不是四个——这是本项目手上最大的单一 CPU 杠杆，且完全不需要 shm 或平台工作。
9. **`MG_Test` 的 mock backend 顺理成章变成 MGPipe recorder**，`tools/trace_replay` 可获得一种比 apitrace 精确得多的 MGPipe 级录制格式（它记录的是**已解析**的状态，重放确定性更强）。
