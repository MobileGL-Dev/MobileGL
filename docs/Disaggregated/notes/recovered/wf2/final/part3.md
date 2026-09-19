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

### 7.2 95 个写回点的逐族归属

| 族 | n | 变成什么 |
|---|---|---|
| `SyncPersistentMappedRange` | **20** | **v2 修正：不是"全部消失"，而是逐站点归属。** 其中多数紧挨着一次对客户端字节的 CPU 读，而那些读搬到了 client（§5.8），由 **tracker 在填 `MGHostSpan` 之前**做同一次 reconcile（逐站点表见 §5.8.1）。**但至少一处的消费者搬不走**：`UniformManager::ResolveUniformBufferPayload`（`UniformManager.cpp:2022` 同步，`:2052` 读 `MappedData()+rangeStart`，`:2053-2057` 零填充）把具名 UBO 打进 **Magma 自己的 UBO ring**——由 D-B8 的 `set_shader_buffers` host payload 承载，client 在**发射前**做 reconcile。**P1 的交付物包含这 20 处的逐站点归属表**（哪些消失、哪些变 client 发射前 reconcile、哪些需要 host payload），不接受笼统结论 |
| `MarkStorageDirty` | **18** | 16 处是 server 本地记账——**零消息**（dirty 归属反转，§7.3）。2 处 `true`（`Managers.cpp:2813`、`DirectGLES.cpp:6852`）变 `on_texture_pull_request` / `on_texture_writeback` |
| `AllocateStorage` | **8** | 6 处是 **backend 凭空造出来的前端对象**（Magma 的占位纹理、`SwapchainObject` 的 default-FBO 占位，`SwapchainObject.cpp:284, 305, 329`）→ **server 原生，永不上线**；1 处是生成 mip 的 shadow（`DirectGLES.cpp:6261`）→ `on_mip_levels_generated`；1 处是 swapchain 尺寸变更 → `on_surface_changed` |
| `WritebackFromBackend` | **8** | `MGPReplySlot`（回读）+ `on_buffer_writeback`（PBO 回读、XFB 捕获）。**必须按操作级批处理**：其中两处今天在循环里**逐行**写回（`Utils.cpp:2342`、`DirectGLES.cpp:7633`），绝不能变成"每扫描线一次 IPC" |
| `SetInternalFormat` | **7** | 与 `AllocateStorage` 同批 |
| `SyncGpuWrites` | **6** | 同 `SyncPersistentMappedRange`：**逐站点**，见 §5.8.1 |
| `MarkGpuWritten` | **6** | client 在每个 draw/dispatch 发射点**保守自建**，镜像 `DirectGLES.cpp:459-467, 509, 1809` 与 `UniformManager.cpp:1073, 1229`、`VulkanRenderer.cpp:11210` 的输入。`on_gpu_written{res, ranges[]}` 是**收窄**通道 |
| `RecordError` | **6** | `on_gl_error`，**必须对命令流有序**（§7.4） |
| `SetBackendResource` | **4** | **删除。** server 拥有资源表；pooling / 延迟释放原样搬到 server |
| `EnsureGpuResidentStorage` | **3** | server 本地决策 |
| `SetBackendHashMemo` / `SetBackendAuxMemo` | **3** | 纯值 → server 侧 per-slot 字段 |
| `InvalidateCompileEnv` | **2** | `on_caps_invalidated`，低频 |
| `SetBackendStateMemo` | **1** | **直接删除，不翻译**（D12） |
| `UpdateMipmapSubData` / `TruncateMipmapLevels` / `SetSamples` | **3** | 全在 Magma 的占位纹理里 → server 原生 |

**6 处 backend 反向进 `MG_Impl`：** 四处 `pDefaultFramebufferInfo` 身份比较 → 保留 handle `{0,1}` + `MGPFramebufferState::isDefault`；`SwapchainObject.cpp:276-330`（backend **创建** default FBO 的三张 `ITextureObject`）→ `on_surface_changed`，client 自己合成对象——**顺带删掉 monolith 里的一处分层倒置**；`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`）→ `get_texture_image` 返回 **"该 level 无 GPU 背书，请从你自己的 shadow 回答"**（`:10691-10704` 今天测的正是这个条件）。

#### 7.2.1 v2 新增：XFB scatter 是对 client shadow 的 read-modify-write，必须搬到 client

v1 把 8 处 `WritebackFromBackend` 全部归给单向的 server→client 通道。**`ScatterCapturedRecords`（`DirectGLES.cpp:893-960`）不是单向的**：它在 `:928` 做

```cpp
Memcpy(staged.data(), target.buffer->MappedData() + target.start, rangeBytes);
```

——**从应用已有的字节起步**，然后只把捕获到的 varying 补进去，"这样 `gl_SkipComponents` 要求的空洞保留应用原本放在那里的东西——**这正是这个特性的全部意义**"（`:889-892` 的注释；`:880-883` 点名 `KHR-GL46.transform_feedback.capture_special_interleaved_test` 是走到这条路径的用例）。server 没有 `MappedData()`，而 `MGPipeCallbacks` 里也没有反向的 buffer 读。照 v1 实施，要么空洞被清零（一致性破坏），要么需要一次 §9.2 没有列出的、发生在 `glEndTransformFeedback` 上的同步反向读。

**修正（不新增停顿类）**：**scatter 搬到 client。**

1. server 把驱动捕获到的**紧密打包** scratch 字节通过 `on_buffer_writeback(scratchHandle, 0, bytes)` 推给 client，并用 `on_xfb_scatter_ready(scratchHandle, packedStride, vertices)` 告知布局参数；
2. client 拥有目的 shadow，也从反射归档里拥有 `GetTransformFeedbackVaryings()` / `GetTransformFeedbackStride()` / `GetTransformFeedbackPackedStride()`（`ProgramObject.h:1146-1171, 1357-1394`），于是原样跑今天 `:930-939` 的补丁循环；
3. client 把补好的范围当作**普通 `resource_subdata`** 重新发下去（复现今天 `:946-948` 的 `glBufferSubData` 回灌），并 bump 自己的 change serial（复现 `:942` + `BumpBufferMutationEpoch()`）。

副作用：`:906-914` 的"CPU 模型给出 0 顶点 → 整批捕获丢弃"的诊断**落到应用线程**上，比落在 server 上更有用。计入 Espryt 子系统 7（§6.4）。

### 7.3 纹理 dirty 归属反转

**client** 保留 `MipmapStorage` 的模型（96-rect 级联合并 + `summedArea*4 >= unionArea*3` union-box 回退，`MipmapStorage.cpp:300-305`），维护一份**发射游标**，在发射后清自己的标志。**server 从不碰 client 的标志。**

这是安全的，且已核实：**`MG_Impl` 里没有任何 `IsStorageDirty(` / `GetStorageDirtyRects(` / `GetStorageDirtyRegion(` 调用点**（前端从不读自己的 dirty 状态），而它自己在五处主动清（`GL_Texture.cpp:528, 701, 5547, 5621, 5691`）。**这一条删掉 `PLAN.md` §5.6a 的整个 ack 协议与风险 R6。**

**v2 修正 1：发射游标必须按**存储属主**键控，不能按 `(texture, uploadTarget, level)`。**
`TextureObjectView` 把 `IsStorageDirty` / `MapMipmapData` / `MarkStorageDirty` / `MarkStorageDirtyRegion` / `GetStorageDirtyRegion` **全部转发给存储属主的 mipmap 并做索引重映射**（`TextureObjectView.cpp:290-322`；`:281` 直接写属主的数据）。一个 view 与它的属主**共用同一份 dirty 状态**却会各带一个游标：谁先发射谁就清掉了另一个还需要的标志，或者两边都发同一批纹素。
**正确键**：`(storageOwnerHandle, ownerUploadTarget, ownerLevel)`——查询与清除前先经 `GetViewStorageOwner()` 与 view 的 `ToOwnerUploadTarget()` / `ToOwnerLevel()` 映射。
**门**：新增场景，通过 view 上传、经属主采样（以及反向），跨 draw 边界各一次。

**v2 修正 2：`MOBILEGL_PIPE_VERIFY` 需要一个"保留模式"，否则它在最危险的子系统上是瞎的。**
影子比对（§10.3-②）的参照物是"从头重算一次快照"。但发射后 client 已经把 dirty 标志清了，**从头重算无法重建当时的 rect 集合**——于是子系统 5（`resource_subdata` 的 payload）恰恰是 verify 看不见的那一块，而它同时是 §6.4 标注"全表最危险"、押着 +6ms/frame 悬崖与 7 条 repack 路径的那一块。
**修正**：`MOBILEGL_PIPE_VERIFY=1` 时 tracker **保留清除前的 dirty 集合**到本次 draw 结束，G4 比对**发射出去的 `(unionBox, regionCount, regions[])`** 与快照重算的结果。**并且**新增 `TextureUploadShapeScenario`：把逐纹理逐帧的上传形状（box vs N 个 region、作业数）录成金标，与 SSIM 并列比对——**+6ms 悬崖由形状相等把关，不是由 SSIM 把关**（SSIM 对它完全不敏感）。

**上传形状决策留在 server**：`resource_subdata` 同时带 union box 与 region 列表（§4.5.6），Mali 按作业数计价的悬崖在哪一侧付 GPU 代价，决策就留在哪一侧。

### 7.4 反向通道的有序性是正确性要求，不是优化

**`on_buffer_writeback` 必须与 epoch bump 有序。** 今天每一次 `WritebackFromBackend` 后面都紧跟一次 `BumpBufferMutationEpoch()`（`DirectGLES.cpp:834-837, 942, 7625-7629`），否则 server 自己的 draw-clean memo 会在 epoch 背后变陈旧。split 里这变成**反向通道上的一条排序规则**：一次写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 侧应用。**反向通道需要与正向通道相同的有序保证。**

**`on_gl_error` 必须对命令流有序**，否则 `glGetError` 答错。`glGetError` 本身永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`）。

**v2 修正：`kNeedsAck` 只标真正**同步**的分配点，不是"看起来像分配"的 GL 入口。**
v1 把 "`glRenderbufferStorage*`、可能失败的 `glTexImage*`/`glTexStorage*`/`glCopyTexImage*` 形式、`glBufferStorage`" 全标成 `kNeedsAck`，让 OOM 探测惯用法（`allocate; if (glGetError()==GL_OUT_OF_MEMORY) 用更小的重试;`）成立。**实测这批里纹理族根本不调 backend 表**：`MG_Impl/GLImpl/Texture/GL_Texture.cpp` 在 `:2515, 2671, 2755` 只做 `MarkStorageDirty(..., true)`，Espryt 在 sync 时刻才惰性分配；纹理侧的错误上报 `RecordGLError`（`DirectGLES.cpp:6309-6324`）**只有一个调用者**——`glGenerateMipmap`（`:6916`）。连唯一一处真正的同步分配 `glRenderbufferStorage*` 也是在 `BackendRenderbufferObject::SyncToBackend`（`Managers.cpp:8674-8684`）里惰性做的。

**修正后的规则**：
- **纹理分配的 OOM 在 monolith 里就已经推迟到 sync 时刻，拆分不改变任何可观察行为** —— 这批**不标** `kNeedsAck`，并把这条事实写进文档（避免后人以为是遗漏）。
- **`kNeedsAck` 只标两项**：`glBufferStorage`（真同步）与 `glRenderbufferStorage*`（**若**决定把它的分配提前到 GL 调用时刻以支持 OOM 探测；否则它也不标，同样写明）。**这个"若"由 P0 回答**：查 MC / Iris 语料里有没有真的 `glRenderbufferStorage` OOM 探测惯用法；没有就不标，省掉整条 ack 路径。
- 其余错误一律晚到，走有序的 `on_gl_error`。

**对 `PLAN.md` §7.4 的强制修正：`on_log` 必须按严重级分级。** `PLAN.md` 把**全部** `EvLogLine` 设为有损（覆盖最旧 + `eventDropped`）。但 §5.7 已确认：**backend program link/compile 失败只以一行日志加一次 bind-program-0 的空 draw 呈现**。统一有损策略下，系统里诊断价值最高的那一行会在日志压力下静默消失。

**规则**：`on_log(level ≤ WARN)` 有损；**`on_log(level ≥ ERROR)` 无损**，加入触发 `eventRingFull` + 停止 apply 的语义事件集；再加一个**每秒 ERROR 速率限制器**，超限时发一条显式的 "N errors suppressed"。`MGLOG_E_ONCE` 的 latch 变成 per-server。P9 的故障注入门：日志洪泛下注入一次 link 失败，那行 ERROR 必须出现**且**两侧都恢复。

### 7.5 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素字节，三个原因会要求 client 重发已发过的 level：`RequireImageBindableStorage` 的 re-dirty（`Managers.cpp:2813`）、整格式再生（`:3950-4195`）、view 源重铸（`:3616-3707`）。**四条缓解同时上**（v1 是三条，v2 补第 (e) 条终止符），加一个专门的门和一个必须发布的计数器：

**(a) 预防主因。** client 给纹理打 `everImageBound` 标记，`resource_create`/`respecify` 一直携带 `imageBindableHint`，于是 image-bindable 存储在前期就分配好。这把 `RequireImageBindableStorage` 从稳态里彻底移除。

**(b) 拉取是异步的。** server 发 `on_texture_pull_request{res, target, levels[], pullSerial}` 并把那个 twin **标为 not-ready**；client 在下一次 publish 时重发。因为 client 跑在前面，常见情况下字节在 server 到达采样该纹理的 draw 之前就到了；即使没到，**阻塞的是 `mgl-srv-apply` 线程，不是应用线程**。

**(c) 有上限的保留（默认关闭）。** 可选的逐纹理保留位，受一个显式的 LRU 字节预算约束（`MOBILEGL_PIPE_TEXEL_RETAIN_MB`，**v2 把默认从 32 改为 0**）。理由：`MipmapStorage` 保有每个 level 的完整 CPU 影子（`MipmapStorage.h:117` 的 `Vector<Vector<Uint8>> m_data`），所以一次拉取**总是能**从 client 已有的字节服务——保留缓存买的是**延迟**，不是正确性，而它花的是**内存**，恰好是 §0.4 用来对比 replica 的那个指标。只有 (d) 的实测拉取率非平凡才开，并拿真预算。

**(d) 门与计数器。** `TextureRemintPullScenario`：同时强制 `RequireImageBindableStorage` 与一次帧中格式再生。**拉取次数逐 trace 用例发布**，与 SSIM 并列。**本设计从不声称"零 round trip"，它测量并公布。**

**(e) v2 新增：显式终止符——因为存在"答不出来"的拉取。**
`RequireImageBindableStorage` 的重放会 re-dirty 每个上传目标的每个 level（`Managers.cpp:2789-2822`），而它自己已经跳过 `GetMipmapByteSize(...) == 0` 的 level（`:2810-2812`）。但还有一类 level：**内容只来自渲染、来自一次 `CanMirrorCopyImageShadow` 拒绝的 `glCopyTexSubImage`（`DirectGLES.cpp:7068-7073`）、或来自 GPU 侧 mip 生成**——client 那里根本没有字节。没有终止符，apply 线程会 park 在一个**永远不会 ready 的 twin** 上。B-R4 与 `TextureRemintPullScenario` 只针对拉取的**频率**，从来没针对**无解的拉取**。
**修正**：
- 拉取是 request/response 对，由 `resource_subdata_complete(res, target, firstLevel, levelCount, pullSerial)` 终止，**它可以携带零个 region**；
- 收到零 region 的应答时，server **带着"已分配但为空"的存储继续**（这正是 monolith 的行为：`EnsureGenerateMipmapStorageAllocated`（`DirectGLES.cpp:6270-6271`）也是 `AllocateStorage` + `MarkStorageDirty(false)`，不填内容），并记一条 `MGLOG_W`；
- **`TextureRemintPullScenario` 必须包含这个无解用例**（一张只被渲染过、随后被 image-bind 的纹理），**且它必须在终止符落地之前是红的**（表现为 apply 线程挂死或超时）。

若在真实语料（MC 与 Iris fixture）上实测拉取率非平凡，(c) 从可选升级为强制并拿到真预算。

---

## 8. 传输、数据面、同步、present、线程、平台、构建

### 8.1 原样继承方案 A 的部分

以下全部**逐条继承 `PLAN.md`，本文不复述**：

| `PLAN.md` 章节 | 内容 |
|---|---|
| **§6.1** | 段布局（`SEG_CMD` 8MiB / `SEG_STAGE` 32MiB↑ / `SEG_REPLY` 8MiB / `SEG_EVENT` 256KiB / `SEG_SHADOW[n]` / `SEG_ADOPT[n]`）；shm 创建矩阵；**`SCM_RIGHTS` 必须在第一个 transport commit 里实现**（`Feat/CS-Delta-IPC` 把 `out->fd = -1` 硬编码在 `LocalSocketTransport.cpp:296`，它的数据面在唯一重要的平台上一个字节都没过去）；`SEG_SHADOW` 块的 pending free-list 退休规则 |
| **§6.2 / §6.2a** | `RingControl`：两组独立游标三元组、三个 seq 水位、`serverEpoch`、`ringGeneration`、`consumerParked`/`producerParked`、`eventRingFull`/`eventDropped`；**双向 doorbell**，`MOBILEGL_IPC_SPIN_US` 默认 50µs，`inproc` 用 condvar |
| **§6.3** | 记录格式：8B `RecHeader`、24B `BlobRef`、**无 per-record 序号**、X-macro 每种一条 `static_assert` **加**生成的运行期边界检查 → `Fatal{ProtocolCorruption}`、`kVarTail` 自描述长度自洽校验。方案 B 把这套机制扩展到**全部** MGPipe 调用（G3） |
| **§6.4** | WAR 纪律：调用时刻拷进 ring slot（P1-4）；P4.5 的 `SEG_SHADOW` 零拷贝 + 逐 shadow 64KiB 块发送水位 |
| **§6.5** | ring 分配与背压：逐字移植 `PersistentRing`（`Managers.cpp:641-727`、`RingAllocateSlow` `:1891-1970`、`RingOnPresent` `:1975-2016`） |
| **§6.6 前三条** | unpack PBO 完全在 client 解析；压缩 internalformat 永不到达 backend；`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client |
| **§6.7 第 2、5 行** | PBO 回读改 fire-and-forget（**严格优于 monolith**，`DirectGLES.cpp:9189-9205` 无条件停等）；`glEndTransformFeedback` 的无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`）推迟到首次读 |
| **§6.8** | persistent map 与 ≥16MiB 采纳的三档，**由运行时 POST 探针选择，绝不硬编码驱动名** |
| **§7.1** | FlatBuffers 纪律；`protocol_generated.h` 提交；`gen_protocol.py` + CI `flatc-check`；**默认构建图里没有 `flatc`** |
| **§7.2** | 帧封装；publish 触发器（每记录 release-store `cmdHead`、显式门铃点、`SEG_STAGE` 余量 < 1/4、**轮询入口也是门铃点**、`GL_SYNC_FLUSH_COMMANDS_BIT` 无条件 publish、`MOBILEGL_IPC_POLL_ESCALATE` 饥饿升级）；**`glFinish`/`glFlush` 保持免费**（`Definitions.cpp:111-112`） |
| **§7.3** | 两个互相独立的窗口（字节 credit、present credit）；server 不发 credit 消息 |
| **§7.4** | 事件 ring + 排空点 + 溢出策略。**加上 §7.4 的分级修正** |
| **§8 末尾** | fence 完成度必须来自**真的逐 fence 退休**，不是 present 水位（`DirectVulkan.cpp:1120-1128`；`magma-mc1215-fence-oom`）；三个应先独立落 `dev` 的 monolith 修复 |
| **§9 / §9.1-§9.3** | `Present` 与 `eglSwapBuffers` 严格 1:1、绝不批量；`MOBILEGL_IPC_PRESENT_CREDIT` **默认 1** 与延迟叠加公式；Magma 从不注册 `SetSwapInterval`（`BackendObject_DirectVulkan.cpp:698`）；DirectGLES 的非 present fence tick |
| **§10** | 线程模型；server 的 `mgl-srv-io` + `mgl-srv-apply`；核心放置与 `MOBILEGL_IPC_SERVER_AFFINITY`、**报逐线程 CPU 时间**；拆机顺序 |
| **§11.1-§11.6** | 启动与握手；`extern "C" visibility("default")` 与 `nm -D` 门；Android 的 `android:process=":mgl"` Service 路径；X11 XID；`EGL_PLATFORM=surfaceless`；Windows overlapped named pipe；崩溃时的 device-lost latch |
| **§12 第 1-3 层 / §12.4** | 编译期折叠；**唯一 hook 点** `MG_Backend/Init.cpp:48-70`；P4.5 的 allocator 改动整段包裹；`MOBILEGL_TRANSPORT` 复用全部既有开关通道 |
| **§13** | 目录形状；一份库两个角色；FlatBuffers submodule 的双重 guard；ctest/trace-replay 的三个陷阱；`SPLIT` 后缀与 `-DTRACE_TRANSPORT=`；CI 的 `flatc-check` 与 `fprintf` grep 门 |
| **§14** | 对 `Feat/CS-Delta-IPC` 的 REUSE / CHANGE / DROP 判定 |
| **§15 P0** | 卫生清单与两个 spike |

### 8.2 与方案 A 的差异

**删除：**
`Server/ReplicaContext.{h,cpp}`（换成 `Server/PipeObjectTables.{h,cpp}` + `Server/IndexHostMirror.{h,cpp}`）；§5.0 的"replica vs 重写"决策；§5.1 的三步发射协议；§5.2；§5.4 的 replica 对象表规则与 `Fatal{IdentityDivergence}`；§5.6a 的纹理 ack 协议；§5.7 的 Phase 1-4 composite 分支；§5.9b 的 mutation **replay** 机制（`MutationCoverage.def`、`ImplMutationSurface.inc`、`MG_Remote::Shared::` helper 族）；§6.9 的 relink 档与 `MOBILEGL_IPC_PROGRAM`；§6.4 的拷贝第 (3) 行；§12.2 的 `pGLContext` shim；阶段 **P5**（6 天回收）；风险 **R1** 与 **R6**；开放问题 **§17-5**。
**不删**：`gen_impl_mutation_surface.py` 本体——它改造成 `gen_pipe_dirty_surface.py`（§0.3 推论 4）。

**改变：**

| `PLAN.md` § | 差异 |
|---|---|
| §5.9a | READ 面的**编目**生成器变成**三道禁止门**（§4.7.2）。原 477 行 inventory 保留为 tracker 侧覆盖检查表（G6） |
| §6.4 拷贝账 | 第 (3) 行不存在：**P1-4 = 3 次，P4.5 = 2 次**。`PLAN.md` 自己的"方案 B"目标**按结构达成**，开放问题 §17-5 自动关闭 |
| §6.6 第 4 条 | 逐 level `serverAuthoritative` 位被 dirty 归属反转（§7.3）+ `on_texture_writeback` + `on_texture_pull_request`/`resource_subdata_complete` 取代 |
| §6.9 | `RecProgramLinkOp` **不可能**（§5.7）。`ProgramPublish` 第一天；`reflectionDigest` 换成"schema 完整性绊线"；P5 消失。**新增前置 P0.5 的头文件抽取**（§4.5.5），否则 `nm -D | grep glslang` 判据不可达 |
| §5.10 | 第 2、3 条**逐字继承**（**R2 仍是最高优先级正确性项**）。第 1 条缩成**一个推送的 `hasLiveHostWrites` 位** |
| §6.10 | 四类应用指针按 §5.8 归属；`ClientArrayBounds` 变成 flag 门控的 `MGPDrawInfo::minIndex/maxIndex`。**陈旧索引纪律改为逐站点表**（§5.8.1），不是笼统规则 |
| §7.4 | `on_log` **按严重级分级**，加每秒 ERROR 速率限制器 |
| §12.2 | 需要角色隔离的进程全局从 **4 个降到 2 个** |
| §13 | `MG_Pipe/` 是**默认构建里的非可选目录**；只有 `MG_Remote/` 在 `MOBILEGL_BUILD_DISAGGREGATED` 之后 |
| §15 | **在 P1a 之前新增两整段**：P0.5（头文件抽取）与 backend 推送改造。`PLAN.md` 把后者定价为"~0 逻辑改动"；在方案 B 里它是工作量主体 |

**新增：**

- **`SEG_STAGE` 尺寸必须额外容纳这些它以前不承载的字节**（v2 修订清单）：
  1. client 顶点数组；
  2. client 索引数组；
  3. multi-draw 参数块（`first[]`/`count[]`/`indices[][]`/`basevertex[]`，`drawcount*4` 级）；
  4. client 解析后的 `*IndirectCount` 命令块（几十字节）；
  5. **具名 UBO 的 host payload**（D-B8，`kCapNeedsHostUboBytes` 下逐 draw 逐块）；
  6. **纹理 subdata 的紧密重打包区域**（§4.5.6；今天走 unpack ring 时也已经紧密重打包，所以字节量同阶，但现在过 ring slot）。
  **不在此列**（v1 曾担心，D-B7 解决）：restart 重写的整 EBO（`kMaxRestartRewriteBytes = 1<<26` = 64 MiB，是默认 `SEG_STAGE` 的两倍）与 multi-draw 展平的索引流（`kMaxFlattenedIndices = 1<<24`）——**它们由 server 侧的索引宿主镜像喂养，不过 `SEG_STAGE`**。
  上限由 P0 落地的计数器实测定，不用默认值猜。**并且 G3 必须为"单条记录大于段容量"定义明确的分块/降级路径**（大 subdata 分块成多条，而不是一条巨记录）。
- **`Server/IndexHostMirror`**（D-B7）：由 `resource_create/respecify/subdata` 流增量维护，覆盖 `bindMask & ELEMENT_ARRAY` 的资源；预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64）；逐帧发布 `index-mirror-bytes` 与 `index-bytes-shipped`（超预算退化路径的计数）。
- **新事件种类**：`on_texture_writeback`（CopyImage 镜像搬走后只剩一个生产者：CPU 生成 mip 路径 `DirectGLES.cpp:6811-6861`）、`on_texture_pull_request`、`on_mip_levels_generated`、`on_xfb_scatter_ready`；正向终止符 `resource_subdata_complete`。`on_buffer_writeback` 从"优化"升级为**承载语义**。
- **新环境变量**：`MOBILEGL_PIPE_PUSH`、`_VERIFY`、`_STATS`、`_LEGACY_MEMOS`、`_TEXEL_RETAIN_MB`（**默认 0**）、`_INDEX_MIRROR_MB`（默认 64）。
- **`RenderbufferObject::GetLifetimeId()`**（今天没有）。**但不需要 `GetVersion()`**——推送模型里 `glRenderbufferStorage*` **本身就是**一次 pipe 调用。

### 8.3 persistent map：唯一被显式隔离的传输相关决策

`AcquirePersistentMap`（`BufferObject.h:112`）是**永久的地址空间捐赠**（D4/D-B4）。**它原样穿过 monolith 改造（P0..P13 一动不动），只有 IPC 那一步才打破它。** 决策路径：

- **P0 的 spike B 在第一周给方向**：导出 `HOST_VISIBLE|HOST_COHERENT` VkBuffer 的 fd，client `mmap` 后回读，在两台设备上跑。
- **T2（拒绝，永久正确的回退）**：返回 `nullptr`，前端已在三处容忍（`BufferObject.cpp:174, 439-442, 470-472`）。**此档下 `PLAN.md` §5.10 的 client 侧块粒度推送是强制的**，由 `PersistentCoherentMapScenario` 把门。
- **T1（server 导出自己的映射）**：**每次存储定义一次** round trip（v2 修正 v1 的"每 store 生命周期一次"——`TryAdoptLargeStorage` 在存储定义时触发，反复扩容的 arena 付 N 次）。`StorageBufferRegrowScenario` 必须发布 `map-persistent-roundtrips`。
- **T0（server 导入 client 分配）**：理想但可用性未知。

若两台设备都否，IPC 期的该阶段从 8 天缩为 2 天的文档与负面对照。**绝不允许一个平台未知数挡住 260 天的接口工作。**

---

## 9. Roundtrip 清单与稳态零 roundtrip 论证

### 9.1 稳态零 roundtrip 的项

| 类 | roundtrip | 依据 |
|---|---|---|
| 全部 draw、clear、blit、copy、dispatch、barrier、XFB 跨度标记、全部 bind、全部 CSO create/bind、全部 `set_*`、全部 buffer/texture 上传、`present` | **0** | 单向记录；present 只查 credit |
| **全部 89 个 caps 站点** | **0** | 首次 `MakeEGLCurrent` 的一次 `MGPCaps` 快照（`BackendObject.cpp:341-347`，每次 surface 变更重新武装 `:301`）；`callMask` 精确复现 DirectVulkan 少注册的槽位 |
| `glGetError` / `glFinish` / `glFlush` | **0** | 前者永远本地（`GL_Getter.cpp:2811-2817`；不变式 `Core.cpp:48-49`），后两者是彻底的 no-op（`Definitions.cpp:111-112`）**且必须继续免费** |
| fence 与 query 的**创建**，以及每一次**非阻塞轮询** | **0** | handle 由 client 铸造；未命中合法地答 `GL_UNSIGNALED`/"未就绪"（`BackendObject.h:210-214`、`:236-241`；前端已遵守，`GL_Query.cpp:302-311`） |
| `glGetTexImage` / `glGetTextureImage`（**DirectGLES**），**包括 GPU 生成的 mip level** | **0** | client shadow 回答（`CopyTextureImageToClientOrPBO_State`，`GL_Texture.cpp:5368-5420`，取用点 `:6460`）。**v2 显式决定**：`on_mip_levels_generated` **只带形状不带字节**，因为 monolith 也是如此——`EnsureGenerateMipmapStorageAllocated`（`DirectGLES.cpp:6243-6274`）对每个新 level 做 `AllocateStorage(...)` + `MarkStorageDirty(..., false)`，**内容留空**。split 因此与 monolith **行为一致**：GPU 生成的 level 在两种模式下都返回已分配但未填充的影子。**只有 CPU 回退生成路径**（RGB16F/RGB32F，`:6811-6861`）产生真纹素，由 `on_texture_writeback` 回来 |
| `glReadPixels` → pack PBO | **0** | fire-and-forget + client 侧 `MarkGpuWritten`。**严格优于 monolith**（`DirectGLES.cpp:9189-9205` 无条件停等） |
| `glEndTransformFeedback` | **0** | 取消无限 fence 等待（`GL_Drawing.cpp:1326-1337`），改为对 capture target 置 `MarkGpuWritten`；scatter 由 §7.2.1 的 client 侧路径完成 |
| `eglSwapBuffers` | **0 次阻塞 round trip**，一次非阻塞 credit 检查 | 只有 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（默认 1）时才阻塞 |
| **`glMultiDrawElementsIndirectCount` / `glMultiDrawArraysIndirectCount`** | **0** | client 从自己的 shadow 解析计数，只做 `SyncPersistentMappedRange()`——**与 monolith 完全相同的 reconcile 集合**（§5.8.1）。**P8 验收要求 `create-indirect` fixture 上该计数器读零** |
| **primitive-restart 重写 / multi-draw 展平** | **0** | server 从索引宿主镜像读（D-B7） |

### 9.2 不可避免的阻塞点（全部罕见，逐条给理由与缓解）

| # | 站点 | 为什么不可避免 | 缓解 |
|---|---|---|---|
| 1 | 握手 `Hello`/`Welcome` + 段 fd 传递 | — | 一次 |
| 2 | `InitializeEGLDisplay`、`Create/Resize EGL*Surface`、首次 `MakeEGLCurrent` + `InitCapabilities` | 出参 / 返回 `Bool`；caps 只在那一刻存在 | 每 surface 至多一次；surface 回复顺带 `MGPSurfaceInfo`。`SwapEGLBuffers` 不需要回复（`BackendObject.cpp:365-393` 对 client 镜像的 EGL 状态求值） |
| 3 | `glReadPixels` → 客户内存 | GL 要求返回时字节已就位 | 像素进 `SEG_REPLY` slot；**逐行写回循环留在 server 内，按操作级批成一段** |
| 4 | `glGetTexImage`/`glGetTextureImage`（**DirectVulkan**） | Magma 对只存在于 GPU 的 level 没有 client 可答的 shadow | `get_texture_image` 对"无 GPU 背书"的 level 返回"请从你的 shadow 回答"（`VulkanRenderer.cpp:10691-10704`） |
| 5 | GPU-write pending 的 buffer 首次 CPU 读 | shader 在前端背后写了 store | monolith 里**本来就阻塞**（`Managers.cpp:1246` 的 `glFinish()`；`VkBufferManager.cpp:80-85` → `VulkanRenderer.cpp:9807-9817`）。client 保守 pending 集触发，由 `writableMask` 与 `on_gpu_written{ranges}` 两侧收窄 |
| 6 | `glClientWaitSync(timeout>0)`、`glGetQueryObject*(GL_QUERY_RESULT)` 未完成、`glBeginConditionalRender` | GL 定义即阻塞；`glBeginConditionalRender` 连 `_NO_WAIT` 模式也阻塞（`GL_Query.cpp:705-706`） | 非阻塞兄弟是 0 round trip。条件渲染谓词**只解析一次**（`Core.h:387-391`），之后每个条件 draw 在 client 侧丢弃，**server 永远不需要那个 query 对象** |
| 7 | 分配类入口的 ack | OOM 探测惯用法 | **v2 收窄**：只有 `glBufferStorage`（真同步）与——**若 P0 证实语料里确有 `glRenderbufferStorage` OOM 探测**——`glRenderbufferStorage*`。纹理族在 monolith 里就已经推迟到 sync 时刻，**不标 `kNeedsAck`**（§7.4） |
| 8 | `map_persistent`（仅 T1 档） | 应用必须拿到一个不再经过任何 API 调用就能写的地址 | **每次存储定义一次**（v2 修正），不是每 store 生命周期一次；`StorageBufferRegrowScenario` 发布计数 |
| 9 | **server 发起的纹理重铸拉取** | server 不保留纹素 | **四条缓解 + 终止符 + 专门的门 + 逐用例发布的计数器**（§7.5）。异步形态下阻塞的是 `mgl-srv-apply` 而非应用线程；零 region 的应答让 server 带着空存储继续，永不永久 park |
| 10 | client 侧索引扫描，当源 EBO 在 pending 集里 | monolith 在**同一位置**调 `SyncGpuWrites()`（`VulkanRenderer.cpp:3431`） | §5.8.1 的逐站点表；**`*IndirectCount` 不在此列**（它今天不调 `SyncGpuWrites()`） |
| 11 | ring/stage 耗尽、present credit | **节奏，非语义** | `PersistentRing` 的升级路径 + `producerParked` doorbell |

### 9.3 论证的形式：测量，不是声称

**验收门措辞**：在**全部 40 个 trace 用例**上发布**逐用例的 roundtrip 计数器、纹理拉取计数器、索引镜像字节数与 `index-bytes-shipped`**。**不做笼统的"零 round trip"声明。** 条件渲染与阻塞 query 的次数按用例列出。

轮询挂死的防护（继承 `PLAN.md` §7.2/R4）必须有它自己的门：`glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 必须在有界时间内退出。

---

## 10. Monolith 保留

### 10.1 接口在进程内就是直调

monolith 模式下 `MGPipeContext` 用 backend 自己的函数填充，`MGPipeCallbacks` 用对 `MG_State` 的直调填充，`MGHostSpan.ptr` 指向 client 自己的 shadow（**零新增拷贝**），`MGPipeHandle` 按值走一对寄存器。split 模式下同一张表换成发射器，applier 反序列化后调**同一批 backend 函数**。**全世界只有一份 backend 实现。**

### 10.2 热路径的间接成本，**动态口径**的诚实版（v2 重写）

v1 这张表把今天的每 draw 状态获取写成 "Espryt 124 / Magma 169 次 accessor 调用"。**那是静态调用点数**（§2.1(d) 的定义），不是动态每 draw 调用数——树里每一处都已被 memo 门控（§2.3.1 逐条列了早退位置）。按动态口径重写：

| | 今天（动态稳态） | 之后（动态稳态） |
|---|---|---|
| 每 verb 的分发 | 1 次间接调用 + 3 个寄存器实参（`DrawArrays`） | 1 次间接调用 + **~48 B 固定头**（`MGPDrawInfo`）+ 按 flag 的变长尾。**这是一项新增成本，不是持平** |
| 每 draw 的状态获取（值类） | Espryt：1 次 `Uint16` 比较（`DirectGLES.cpp:2016-2018`）早退；未命中时 1.2KB×3 段 memcmp。Magma：1 次版本比较（`:4982`）+ 1 次版本比较（`:5888`）；pipeline memo 未命中时 ~40 次 accessor 走查（`:5155-5200`） | 1 次 `Uint16` 比较；pipeline 版本动了才算 ~25-30 字的子集哈希 + 1 次 map 探测（D-B1）；动态子集动了才发 ~200 B |
| 每 draw 的状态获取（对象类） | Espryt：`SyncNeccessaryTextures` 6 值键 + `PairingsIntact` + 每条目 `IsDrawSyncClean`；`CurrentUnitBindingsEpoch` 三值快门。Magma：`TrySetupDrawFastPath` ~10 次 accessor + ~20 次字比较 + 两次**有损**版本求和（`:6249-6250`） | 5 个聚合世代各 1 次 `Uint64` 比较（推论 4）；命中才走 touched 前缀 + 集合 hash；hash 未变**不发**（§5.4-4） |
| memo 查表 | 对指针位做斐波那契散列的直接映射探测 + owner 相等性（3 次/draw） | 按 slot 的数组下标 |
| 真删除的机制 | — | **~372 行 per-draw 失效发现**（§2.5） |
| 搬到 client 的机制 | — | **~175 行**（去抖 + 完备性解析，§2.5） |

**结论（诚实版）**：推送在稳态**应当**是净减少——省掉三次散列探测、一次 1.2KB 三段 memcmp（换成 ~30 字哈希）、两次有损求和、`CurrentUnitBindingsEpoch` 的 owner 走查；付出 `MGPDrawInfo` 的 payload 构造与集合 hash。**但差距远小于 v1 声称的量级**，而且 §2.7 表明 monolith 的净行数是**增加**的。**所以本设计的 monolith 论据是 §10.3-④ 的逐线程 CPU 数字，不是删除行数。**

两个诚实的告诫：
1. **可达性遍历是搬走了，不是消失了**，头号指标必须是**逐线程 CPU 时间**。
2. **Magma 的 `SetupDrawSnapshot` 快路径命中率在两种模式下会合法地不同**，A/B 比的是**渲染输出与计数器**，永远不是 memo 轨迹。

两个 backend 编进同一个共享库（`CMakeLists.txt:356-383`、`:485`），backend 在 init 时锁存一次（`ConfigLoader.cpp:212-225`），所以去虚化在两种形态下都不可得，也都不需要。**函数指针 struct 而非虚基类**的理由见 §4.1。

### 10.3 替代字节一致门的五部分验证门

**先把成本写在明面上**：`PLAN.md` §12 第 4 层在方案 B 里**按构造死亡**。这是方案 B 的代价，必须写进设计文档而不是藏起来。

**①（v2 扩为三道）接口纯度门。**
- **门 A（include 图）**：disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（或断言 `-H` 输出）。**这是唯一能因它存在的理由变红的检查**——`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h:12 → FramebufferObject.h:12-13 → TextureObject.h / RenderbufferObject.h` 正是这种耦合，`RenderStateParameters` 用 `FramebufferObject::MAX_DRAW_BUFFERS` 定长（`:263, 273`）。依赖 P0.5 的 `MGPipeValueTypes.h`。
- **门 B（符号）**：`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空。
- **门 C（未声明）**：`grep -c 'pGLContext' MG_Backend/` == 0（grep `pGLContext` 不是 `pGLContext->`）。**三道门都只跑非 verify 构建**（D-B5）。
- **外加**一条 debug 断言"每个 backend memo 键都是 `{slot, gen}` 对，永不是裸前端指针"，由 `HandleRecycleScenario` 支撑——**这个场景在 0e 重键之前必须在至少一个 backend 上是红的**。

**② 语义影子比对（`MOBILEGL_PIPE_VERIFY=1`）——决定性的那一条。**
阶段 B 期间两套状态模型活在同一个地址空间：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 生成的比对器**逐字段**、**每 draw** 与推送版本比对，打印第一个分歧字段名与 draw 序号。抓三种事：(a) tracker 忘了推的字段；(b) **dirty 位触发得太少**——危险的那个方向；(c) 两条路径上被变换得不一样的值。第三种 CI 模式，跑全部 40 个 trace 与 367 个集成测试；~5-10× 慢，永不出货。
**必须逐字段比而不是 `memcmp`**：`DirectGLES.cpp:2029-2033` 明确记录 `RenderStateParameters` 的 memcmp 会因 padding false-DIFFER（无害）但永不 false-match——比对器要零误报。
**v2 修正 A：verify 需要"保留模式"。** 消费即清的组（纹理 dirty rect）在发射后无法从头重算，所以 verify 在纹理 subdata 上是瞎的——而那正是最危险的子系统。`MOBILEGL_PIPE_VERIFY=1` 时 tracker 保留清除前的集合，G4 比对**发射出去的** `(unionBox, regionCount, regions[])`（§7.3）。
**v2 修正 B：verify 活过 P13。** `SnapshotFromGLContext()` 与它的 `MG_State` include 整体包在 `#if MOBILEGL_PIPE_VERIFY` 里保留；纯度门只跑非 verify 构建（D-B5）。P13 另交付**录制-金标**模式（MGPipe recorder，§10.4-9）作为不依赖 `MG_State` 的长期语义门。

**③ 行为 A/B。**
全部 ~40 个 trace 用例（`tools/trace_replay/trace_cases.json`，默认 SSIM 阈值 0.99）在 `{monolith-pull, monolith-push, split}` 三种下同一判定、SSIM ≥ 0.99；`ctest -L integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.`（以及 DirectVulkan 对）之间产生**逐名相同**的通过/失败集；428 个单元测试全绿；CTS 逐后端 conformance 在 0.5 个百分点内，按本项目的逐后端表格式上报（行 = GL 版本/扩展，列 = 状态计数，rate = Pass/(Pass+Fail)，NS 不进分母）。
**两个 Create fixture 带 `coherent_as_flush: true`**，必须在两种模式下都开着该开关跑。
**v2 补充：`TextureUploadShapeScenario`**——上传形状（box vs N region、作业数）录金标比对，因为 SSIM 对 +6ms 悬崖完全不敏感（§7.3）。
**v2 补充：参考构建的定义。** P2 之后 monolith 本身已经变了，所以逐名基线必须明确为**"P1 出口的重构后 monolith"**，而 P1 出口本身要先用 verify 证明重构等价于 `81b17c0b`。**`81b17c0b` 的 monolith 只作为 §10.3-④ 性能对照的锚点，不作为逐名功能基线。**

**④ monolith 性能不回归。**
两台设备（`35d0befa` Adreno 830、`3B159D009VZ00000` Mali），reboot-clean、同热窗口、配对 A/B，用 `tools/bench.sh` + trace replay 的 `--benchmark --benchmark-tail-frames --benchmark-result` 逐帧 JSON。**指标是逐线程 CPU 时间**，monolith-push 在 **p50 与 p99** 上都要落在 monolith-pull 的噪声内。CPU 定频按本项目协议。
**v2 补充三条**：(a) **绝对阈值**——tracker 每 draw 的 ns 必须公布并设上限，因为真实拉取基线只有 10-25 次 accessor（§2.3.1），相对噪声阈值会平凡通过；(b) **Blaze3D blend-toggle 微基准**（enable/draw/disable/draw，MC batch 速率）单列，它是 D-B1 的判据；(c) **负面对照**——关掉 CSO 内容寻址（`MOBILEGL_PIPE_PUSH` 的一位）重跑，把"推送更慢"与"CSO 设计更慢"分开。

**⑤ 覆盖 + poison + handle 纪律。**
`gen_pipe.py` 重生成 477 行 inventory 的 MGPipe 映射列，0 UNMAPPED，`git diff --exit-code`；**`gen_pipe_dirty_surface.py` 重生成 mutator→聚合世代 映射，0 未映射**（推论 4）；`PipeInputs::m_filledGen` 的**逐 verb**世代 poison（§6.2.2）；G7 的 render-state setter 一致性测试；P13 的 `static_assert(sizeof(ResidualValueBlock) == 0)`；`ResidualValueBlock` 的逐成员 `offsetof` 断言。

**两条字节级等式仍然幸存**：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加任何库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中。
**符号与 `.text` 漂移每阶段作为信息性指标发布**——一次无法解释的跳变仍然是一个 smell，只是不再是一条断言。

### 10.4 monolith 侧净收益清单（即使 IPC 永不上线也成立）

1. **~372 行 per-draw 失效发现机制真删除**（§2.5），另有 ~175 行搬到 client。**注意 §2.7：monolith 的净代码量是增加的**（约 +6,650 手写 + 4,000 生成），所以这一条是**佐证**，不是主论据。
2. **复用地址 ABA 一整类不可表达**：D1/D2/D3/D10/D11/D13/D14/D16/D17/D20 全部由 `{slot, gen}` 关闭。
3. **FBO → program 排序 hazard 消失**：`DirectGLES.cpp:2712-2732` 的 fragColor 重推导 workaround 与 `g_broadcastMemo*` 删除（机制是惰性特化，D-B3 v2）。
4. **一处分层倒置消失**：`SwapchainObject.cpp:276-330` 不再往 `MG_Impl` 的 `pDefaultFramebufferInfo` 里写。
5. **两个潜伏 bug 顺带修掉**：D21（`m_xfbCounterSlotByObject` 用裸 GL name 做键，`VulkanRenderer.cpp:11136-11146`）与 `RenderbufferObject` 缺 `GetLifetimeId()`。**两条都先独立落 `dev`。**
6. **一个死能力被暴露**：`CapabilityInput::FramebufferSrgb` 与 `DepthClamp`（`RenderState.h:165, 168`）**没有任何存储**——`SetCapability` 落到 `default: // not supported currently`（`RenderState.cpp:380`），`IsCapabilityEnabled` 返回 `false`（`:428-429`）。**六个 backend 读点今天恒为 false。** **必须在渲染状态 chunk 表冻结之前回答**（它决定 pipeline/dynamic 划分里要不要这个字段）。
7. **一次 glslang 编译离开 monolith 启动路径**（Magma 的内部 shader 烘焙）。
8. **`inproc` = monolith 的渲染线程**，且只需隔离两个进程全局——本项目手上最大的单一 CPU 杠杆。
9. **`MG_Test` 的 mock backend 顺理成章变成 MGPipe recorder**：`tools/trace_replay` 获得一种比 apitrace 精确得多的 MGPipe 级录制格式（记录的是**已解析**的状态），**而且它是 P13 之后不依赖 `MG_State` 的长期语义门**（D-B5、开放问题 11 的答案）。
