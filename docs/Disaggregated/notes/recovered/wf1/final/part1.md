# MobileGL 前后端进程拆分实施计划（branch `feat/disaggregated`）

---

## 0. TL;DR 与核心决策

**Server 就是 `libMobileGL` 自己**，在自己的进程里跑一个**真实的 `MG_State::GLState::GLContext`（replica）**，由一个 delta applier 通过普通 MG_State mutator API 驱动。**两个 backend（DirectGLES 27k / DirectVulkan 40k 行）一行不改。** Client 也是同一个 `libMobileGL`，在 init 时换掉几个对象：`MG_Backend::gBackendFunctionsTable` 换成发射表，`MG_Backend::pActiveBackendObject` 换成 `BackendObject_Remote`，`SetBufferBackendOps` 换成发射 ops。一份产物，两个角色，由一个 env var 选择。

这样做的唯一理由是：**backend 的 draw-path 失效模型无法表达成 wire 字段。** DirectGLES 有 memo 直接借用 binding slot 的 `shared_ptr` 地址（`DirectGLES.cpp:1463-1477` `UnitTextureSyncEntry` + `PairingsIntact`）；`VertexInputStateFactory.cpp:78` 把**后端堆上的裸指针**写进前端 VAO；`IsBufferDrawClean` 开头就是裸指针身份比较（`Managers.cpp:1435-1436`，注释："Identity first: a respecify path can hand the frontend a NEW resource"）；三个门控计数器是回绕的 `Uint16`，只有配合指针身份比较才正确（`Managers.h:772-777`，postmortem 在 `DirectGLES.cpp:2823-2831`）；`UniformManager.cpp:1418-1497` 构造并驱动真实 `TextureObject2D`；`VulkanRenderer.cpp:4211-4356` 通过真实 `ShaderObject`/`ProgramObject` 编译链接 GLSL。replica 逐字满足 DirectGLES 107/107、DirectVulkan 165/169 次 `pGLContext` 读取；重写则是把一套被文档记录为"具体设备 bug 疤痕组织"的失效模型重新推导一遍——那正是 `Feat/CS-Delta-IPC` 走的路，它一帧都没渲出来。

**第二个核心决策：每个困难语义先上"慢但可证明正确"的版本，后续阶段用 flag 换成快版本，并把慢版本保留成 oracle。**
- Phase 1-4：**server 从源码重新 link shader**（只需 5 个 schema 字段，而不是 ~40 字段的 reflection schema），并带 `reflectionDigest` 交叉校验 → Phase 5 换成 `ProgramPublish`，`relink` 保留为 A/B 对照与常驻 oracle。
- Phase 1-6：**关闭 ≥16MiB persistent-map 采纳**（前端在 `BufferObject.cpp:174,439-442,470-472` 已容忍 `nullptr` 返回，`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` 已存在）→ Phase 7 攻 external memory 导出，**允许结论是"设备 X 上拒绝，已记录，回退成本 N ms"**。
- Phase 1-4：**调用时刻拷贝进 ring**（WAR 由构造消除）→ Phase 4.5 shadow-in-shm 零拷贝。

**第三个核心决策（本轮对抗性评审后新增，是本计划与上一版最大的语义差异）：客户端是所有"隐式发布"语义的唯一发起者。**
上一版把三件事交给"server 在 replica 上照常做，事件回传给 client"，全部被证伪：
1. **persistent-map 的写发布**：`BufferObject::SyncPersistentMappedRange()`（`BufferObject.cpp:238-250`）是 shadow-backed persistent 非-FLUSH_EXPLICIT map 的**唯一**推送点，而 `grep -rn SyncPersistentMappedRange MobileGL/` 的全部生产调用点都在 `MG_Backend/` 里（DirectGLES.cpp:262/4412/4666/4667/4768/4769、Managers.cpp:1547、MultiDraw.cpp:498、DirectVulkan.cpp:290/481/895、UniformManager.cpp:2022、VkBufferManager.cpp:573/620、VulkanRenderer.cpp:3432/3511/3826/7070/12015/12016）。MG_Impl 与 MG_State 里**一个都没有**。拆分后这段代码跑在 server 对 replica 上，而 replica 的 `m_isMapped` 是 false（没有 map delta），第一行就 return；client 侧则根本没人调。**应用通过 coherent persistent map 写下的字节会被静默丢弃。**
2. **`MarkGpuWritten`**：同样只有 `MG_Backend/` 里的 6 个调用点（`DirectGLES.cpp:465,509,1809`；`UniformManager.cpp:1073,1229`；`VulkanRenderer.cpp:11210`），而它在 monolith 里是**在 draw 调用内同步置位的**。拆分后 draw 是 fire-and-forget，`glDrawElements(); glMapBufferRange(SSBO, READ);` 会在 server 还没 apply 之前就读到陈旧 shadow，零 round trip、零报错。
3. **纹理 dirty flag**：上一版声称"client 从不清 dirty flag"，但 `MipmapStorage::MarkDirtyRegion`（`MipmapStorage.cpp:196-233`）只要 `m_isDirty[level]` 为真就把 incoming **并进** union box 并追加 rect，只有 `MarkDirty(level,false)`（`:171-189`）会重置。永不清 = union box 只增不减、rect 列表饱和、`summedArea*4 >= unionArea*3` 一触发就退化成整 level 上传，正好与计划要保留的调优相反。

所以本版的规则是：**任何 monolith 里由 backend 代码触发的"前端状态发布/消费"，在拆分模式下必须由 client 在发射点自己做一遍**，server 侧那份照常跑（它对 replica 操作，幂等或无害）。事件回传只允许作为**收窄优化**，永远不允许作为语义的**建立者**。

**Phase 1 的目标改为：在 Linux 上以 `inproc` 与 `spawn` 两种传输跑通垂直切片；真机 OpenRA trace（SSIM ≥ 0.99）移到 P2 出口判据。** 理由见 §15：Android 交付链（server `.so` 打包、`untrusted_app` 域 exec、trace app 的 env 透传）本身是独立工作量，把它压进 P1 的 10 天里是上一版最薄弱的排期假设。

### 核心决策速查

| # | 决策 | 理由 |
|---|---|---|
| D1 | Server = replica `GLContext` + 未改动 backend | 293 次 `pGLContext` 读、13 个身份键 memo、25 个 backend→frontend 写全部原样工作 |
| D2 | 发射点 = 三个**已经是间接的**边界（`gBackendFunctionsTable` / `pActiveBackendObject` / `SetBufferBackendOps`），不进 MG_State mutator | monolith 侵入面 = `MG_Backend/Init.cpp:48-70` 里一个 switch 分支；~250 个边界调用点零 `#ifdef` |
| D3 | 版本计数器**不上线**；replica 靠 mutator replay 自然 bump | 不需要 `Install*` setter，不需要在 wire 上维护回绕 `Uint16` 的单调性 |
| D4 | 控制面走 **SPSC shm ring**，watermark 放在一条**共享 cache line**；**双向 doorbell** | `GetSyncStatus`/`IsQueryResultAvailable`/ring 回收/present credit 变成一次 acquire load；但**所有等待都必须能挂起**，不能自旋 |
| D5 | FlatBuffers：热路径用 **`struct`**（定长、无 vtable、无 verifier walk），罕见/变长用 `table` 走 socket | 满足"用 FlatBuffers 序列化"的要求，同时 `DrawArrays` 记录 32B 而不是 ~60B |
| D6 | **composite pipeline program 由 client 解析**并下发 handle | `Core.cpp:644` 在 pipeline cache miss 时 `MakeShared<ProgramObject>(0u)` 并 **link**；server 在 Phase 5 之后没有源码，必须由 client 定 |
| D7 | 覆盖度由**两侧生成的编译期断言**保证：backend 的 READ 面 **和** MG_Impl 的 MUTATOR 面 | backend 新增一个 read、或 MG_Impl 在 table 调用旁新增一个 mutation 而 applier 没 replay → 编译失败，而不是设备回归 |
| D8 | monolith 保留由 **`nm --defined-only` + `.text` size diff** 机械证明，且**每个阶段都跑**，不只 P0 | 不靠"测试没变" |
| D9 | **client 是隐式发布语义的唯一发起者**（persistent map 推送、`MarkGpuWritten`、纹理 dirty 清除、XFB CPU 计数、生成 mip 的存储分配） | 见上文三条被证伪的假设 |
| D10 | `inproc` 与 `spawn` 拆成**两个 CMake option**：出货构建只开 `spawn`，`pGLContext` 保持普通全局，GL 热路径上没有 TLS | Android dlopen 的 shared library 无法用 initial-exec TLS，1494 个 `pGLContext->` 上每次 `__tls_get_addr` 调用不可接受 |

---

## 1. 目标与非目标

### 目标
1. 前端（MG_Impl + MG_State + glslang 链接）与后端（MG_Backend + SPIRV-Cross + 驱动）跑在两个进程，通过 IPC 通信。
2. Client 把前端状态 reconcile 成 delta，序列化（FlatBuffers）后发送；server 更新自身状态并调用 backend API。
3. **稳态帧零 round trip**（readback / 阻塞式 query / sync wait / present credit / 分配类错误 ack 之外）。
4. 两半尽可能互相异步：client 至多领先 server 1 个 present（默认值，见 §9 的延迟叠加分析）。
5. 平台特定代码最小化并集中在 `MG_Remote/Transport/` 与 `MG_Remote/Client/Surface*`。
6. **单进程 Monolith 保持字节级不变**，且可机械验证。
7. 所有验收门用**现有测试**：`ctest -L unit` / `-L integration-gpu` / `tools/trace_replay` / `tools/cts` / `tools/device_bench`。

### 非目标（本分支明确不做）
- **share-group sessioning 重构。** monolith 今天所有 EGL context 共用一个 `GLContext`（`GLState/Core.cpp:20,1487`；`eglCreateContext` 只存 `SharedContext` 于 `EGLState/Core.cpp:640`，全代码库无人读取）。单 context client 与今天等价。`c7c9e346`/`29d721ef` 那套（共享 VAO-0 破坏、四个头文件 `public:` 泄漏、无锁进程全局 current session、`MOBILEGL_SESSION_SWAP` kill switch）整体丢弃。
- **BFA strict-C-ABI backend 插件 / UtilRuntime C-ABI 化。** server 与 backend 同一 CMake 工程、同一产物发布，ABI 边界永不移动。
- **macOS 拆分。** `CAMetalLayer` 无公开跨进程表示，MobileGL 在 macOS 是 `DYLD_INSERT_LIBRARIES` interposer（导出表锁定于 `CMakeLists.txt:600-612`），无 CI 无设备 → **monolith only，写进文档**。
- **Windows 窗口拆分。** WGL / ANGLE-DXGI 对外进程 HWND 不是受支持配置 → **headless(pbuffer) only**。
- Phase 9 之前不做任何窗口路径（全部离屏）。

---

## 2. 现状：今天的前后端边界（七个面）

### (a) `GLFunctionsTable` — 73 项，`MG_Backend/BackendObject.h:117-285`
MG_Impl 侧 91 个调用点（`GL_Drawing.cpp` 37、`GL_Query.cpp` 22、`GL_Framebuffer.cpp` 11、`GL_Texture.cpp` 10、`GL_Sync.cpp` 6、`GL_Getter.cpp` 3、`GL_Program.cpp` 1）+ `MG_Util/ShaderTranspiler/CompileEnv.cpp:134,138` 两处。

- 20 个 draw、9 个 clear（4 个 `ClearNamedFramebuffer*` 携带 `SharedPtr<FramebufferObject>`）、5 个 blit/copy（`CopyImageSubData` 携带两个 `CopyImageEndpoint`，`BackendObject.h:32-39`）、`GenerateMipmap`、3 个 readback、4 个 compute/barrier、`BindImageTexture`（已经收 GL name）。
- **两项是死代码**：`GetInteger64i_v`（`BackendObject.h:196`，MG_Impl 零调用点；`GL_Getter.cpp:1307` 把 64 位形式委派给 32 位）和 `GetProgramiv`（`:197`；`GL_Program.cpp:851` 全部从 `ProgramObject` 回答）。两个 backend 都注册并实现了它们。
- `GetIntegeri_v` 只有 `GL_MAX_COMPUTE_WORK_GROUP_COUNT/SIZE` 真正转发（`GL_Getter.cpp:1161-1179`）。
- `BeginOcclusionQuery != nullptr` 被当作能力探测用（`GL_Query.cpp:471,545,768`）；DirectVulkan 只注册 64/72 项（`BackendObject_DirectVulkan.cpp:690-770`，不注册 7 个 XFB + `PatchParameteri` + `SetSwapInterval`）。

### (b) `BackendObject` 虚函数 — `BackendObject.h:541-568`
MG_Impl 侧 89 个 `pActiveBackendObject->`，**其中 45 个是 `GetDynamicParameters()`**，若干落在 per-API-call 校验路径上（`Buffer/Validators.cpp:63`、`VertexArray/Validators.cpp:22`、`GL_VertexArray.cpp:536`、`GL_Texture.cpp:406`）。
**关键时序：`InitCapabilities()` 懒执行在第一次成功的 `eglMakeCurrent` 内部**（`BackendObject.cpp:341-347`），DirectGLES 在那里才改写 advertised extension string（`BackendObject_DirectGLES.cpp:786-796`）。

### (c) `BufferBackendOps` — 7 个 hook，`BufferState/BufferObject.h:76-121`，注册入口 `:124`
DirectGLES 注册 7/7（`Managers.cpp:1336-1345`），DirectVulkan 注册 6/7（无 `ResidentSubData`，`VkBufferManager.cpp:104-111`）。
`AcquirePersistentMap`（`:112`）**把 GPU 内存裸指针交给应用**；`TryAdoptLargeStorage`（`BufferObject.cpp:167-176`，`kLargeBufferAdoptBytes = 16MiB`）在 store **定义时**单方面采纳。理由块 `BufferObject.cpp:153-166`：MC 26.3 的 128MB chunk arena，实测 p99 163→21ms、40→115fps、省 ~400MB。

### (d) 状态拉取 — 293 个 `pGLContext->`（DirectGLES 124 / DirectVulkan 169）+ ~90 个前端对象 getter
`PrepareForDraw`（`DirectGLES.cpp:2916-2976`）与 `SetupDraw`（`VulkanRenderer.cpp:6371`）在这里把整个 `GLContext` 拉出来。**这一面在本设计中不过线。**

### (e) backend → frontend 写回（25 个语义点 / 14 个类）
`MarkGpuWritten` ×3、`WritebackFromBackend` ×7、`MarkStorageDirty` ×11、`SetBackendResource` ×2、`AllocateStorage` ×1、`RecordError` ×2，加两处 shadow `Memcpy`（`DirectGLES.cpp:6861` 生成 mip、`:7144` CopyImage 镜像）；DirectVulkan 另有 `SetBackendHashMemo`/`SetBackendStateMemo`（**存后端堆裸指针**）/`SetBackendAuxMemo`/`EnsureGpuResidentStorage`/`InvalidateCompileEnv`/`SwapchainObject.cpp:276-331` 改写 default-FBO 占位纹理。
**replica 模型下这 25 处大部分落在 server 自己的 replica 上**，但其中三类是"语义建立者"，client 必须自己做一遍（§5.6、§5.6a）。

### (f) backend 反向进 MG_Impl — 恰好 6 处
`DirectGLES.cpp:1917,2838,2867,9675`（`pDefaultFramebufferInfo`）、`SwapchainObject.cpp:276`、`VulkanRenderer.cpp:10700`（`CopyTextureImageToClientOrPBO_State`）。replica 模型下全部正常解析（server 也链接完整 MG_Impl）。
但注意：`MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo` 全库 22 处引用，client 侧 MG_Impl 也在读（`GL_Framebuffer.cpp:495,1827,1837,1897,1905,1913,1927,1936,2549,2590,2598,2608,2611`）。它是**第二个进程全局**，`inproc` 模式下必须与 `pGLContext` 一起做角色隔离（§12）。

### (g) MG_Impl 在 table 调用旁做的 MG_State mutation（**上一版遗漏的第七个面**）
`GLFunctionsTable` 是一个**命令**边界，不是一个**状态**边界的两侧对称点：MG_Impl 在调 table 之前/之后还会自己改 MG_State，而这些改动 applier 只 replay table 是拿不到的。已确认的两族：

1. **`glGenerateMipmap` / `glGenerateTextureMipmap` / 自动 mipmap**：`GLImpl::GenerateMipmap`（`GL_Texture.cpp:6681-6699`）在 `GenerateMipmap_Backend` **之前** 调 `EnsureGeneratedMipmapStorageAllocated(*mipmapTexture)`（`GL_Texture.cpp:501-541`），后者对 level 1..N 做 `AllocateStorage`、`MarkStorageDirty(...,false)`（`:528`）、`TruncateMipmapLevels`（`:533`）、`BumpContentVersion()`（`:538`）。`:534-537` 的注释写明了这个 version bump 存在的理由：没有它，"a cached sampled VkImageView built for the pre-generate level range would otherwise stay stale and clamp LOD>0 sampling to mip 0"。只 replay table 的 applier 会在 replica 上**精确复现这个已知 bug**。`GenerateTextureMipmap`（`:6702-6711`）和 `MaybeAutoGenerateMipmap`（`:1625-1635`）同形。
2. **Transform feedback CPU 计数**：`AccountTransformFeedbackPrimitives`（`GL_Drawing.cpp:172-236`）在每个被捕获的 draw 上改 6 个 GLContext 计数器：`AddTransformFeedbackPausedPrimitives`（:177）、`AddTransformFeedbackInputPrimitives`（:184）、`AddTransformFeedbackGeometryCaptureDraw`（:214）、`AddTransformFeedbackPrimitives`（:231）、`AddTransformFeedbackCapturedVertices`（:232）、`AddTransformFeedbackAccountedCaptureDraw`（:237）。DirectGLES 在 `DirectGLES.cpp:900` 读 `GetTransformFeedbackCapturedVertices()` 来给 scattered capture 定容量；DirectVulkan 在 `DirectVulkan.cpp:1384` 读 `GetTransformFeedbackPausedPrimitiveCounter()` 并在 `:1337` 把前端 delta 折进 query 结果。这些计数器**没有版本号**，也不在任何 accessor 的门控里；replica 上它们恒为 0 → scattered XFB 什么都不捕、`PRIMITIVES_WRITTEN`/`PRIMITIVES_GENERATED` 错。它们还在 XFB 对象绑定时按对象存取（`Core.cpp:1273,1296`；`Core.h:313-357`），所以简单"发个标量"的补丁必须跟着对象切换走。

§5.9 的覆盖生成器**抓不到这一类**：它扫 `MG_Backend/**` 的 READ 面，所以 backend 读 `GetTransformFeedbackCapturedVertices` 会被正常分类并通过，而 MG_Impl 那半个生产者从来没被审计过。**所以 §5.9 必须有第二个生成器**（见 §5.9b）。

### (h) 工作树污染（Phase 0 必须先清）
`DirectGLES.cpp:290-303` 与 `Managers.cpp:875-877` 有**未提交的 per-draw `fprintf(stderr)`**（格式串里还有字面量 `' + NL + '`，且位于 `pendingMutex` 临界区内的 buffer flush 路径上）。`Feat/CS-Delta-IPC` 的 `d96be9f3` 提交过同类东西（`DirectGLES.cpp:+2583-2590`），导致该分支上**每一次测量**（144-failure Windows run、OpenRA `ssim=0.000036` 设备 run）都跑在每 draw 一次 stderr 写的构建上。

---

## 3. 目标架构总览

```
┌───────────────────────── CLIENT 进程 (libMobileGL.so) ─────────────────────────┐
│  App / LWJGL                                                                   │
│      │ gl*                                                                     │
│      ▼                                                                         │
│  MG_Impl  (validate → RecordError → 调 MG_State mutator)   ← glGetError 本地   │
│      │                                                                         │
│      ▼                                                                         │
│  MG_State::pGLContext  (权威状态 + ShaderCompilePool + glslang)                │
│      │                                                                         │
│      ├─ gBackendFunctionsTable = EmitTable      ─┐                             │
│      ├─ pActiveBackendObject   = BackendObject_Remote (+ CapsMirror)           │
│      └─ SetBufferBackendOps(&g_emitBufferOps)   ─┤                             │
│                                                  ▼                             │
│                                     MG_Remote::WireMirror                      │
│                              (① PublishImplicitState：persistent-map 推送、    │
│                                  MarkGpuWritten 保守置位、XFB 计数、mip 分配   │
│                               ② 读版本计数器 → 决定发什么                      │
│                               ③ 清 dirty flag / 记录 shipped 水位)             │
│                                                  │                             │
└──────────────────────────────────────────────────┼─────────────────────────────┘
        SEG_CMD (SPSC ring, POD 记录)  ────────────┤  写
        SEG_STAGE (bulk 字节 ring, 独立游标)  ─────┤  写
        SEG_SHADOW[n] (P4.5+, client 拥有)────────┤  RW
        RingControl (一条 cache line 的 atomics)  ◄─┤  读 watermark（acquire load）
                                                   ├─ producerParked ──► server 敲门铃
        SEG_REPLY / SEG_EVENT (server 拥有)       ◄─┘  读（在每个等待循环里排空）
        CTRL socket (socketpair / 继承 overlapped pipe): FlatBuffers table
                                                        + SCM_RIGHTS + 双向 doorbell
┌──────────────────────────────────────────────────┼─────────────────────────────┐
│  MobileGLServer (dlopen libMobileGL.so → mobilegl_server_main)                 │
│   thread mgl-srv-io   : asio，framing，fd 传递，doorbell，控制面 RPC            │
│   thread mgl-srv-apply: 终身持有 EGL/Vulkan context（可绑大核）                 │
│        │                                                                       │
│        ▼  Applier::Apply(RecHeader)  →  MG_State mutator / 共享 helper /       │
│                                          GLFunctionsTable                      │
│   MG_State::pGLContext  (replica)                                              │
│        ▲                                                                       │
│        │ 293 次 pGLContext-> + ~90 getter，**零改动**                          │
│   MG_Backend (DirectGLES / DirectVulkan) + MG_Util(SPIRV-Cross, 转译缓存)      │
│        │                                                                       │
│        ▼  真实 GLES / Vulkan 驱动                                              │
└────────────────────────────────────────────────────────────────────────────────┘
```

三种运行模式（`MOBILEGL_TRANSPORT`）：`monolith`（默认，编译期折叠）、`inproc`（同进程第二个 `GLContext` + apply 线程，**需要 `MOBILEGL_BUILD_DISAGGREGATED_INPROC`**）、`spawn` / `unix:<path>` / `pipe:<name>`（真跨进程，出货形态）。

---

## 4. 边界定义（每个面变成什么）

| 面 | 变成 |
|---|---|
| **(a) `GLFunctionsTable`** | `MG_Remote::Client::MakeEmitTable()` 返回的发射表。61 项 = 先跑 `PublishImplicitState`、再追加一条定长记录、返回；5 项 request/reply；4 项分配类 `kNeedsAck`（§5.6c）；`GetIntegeri_v` 由 CapsMirror 本地回答；`GetInteger64i_v`/`GetProgramiv` **从 wire 与 table 中删除**（并提议在 `dev` 上删掉两个 backend 的实现）。7 个携带 `SharedPtr` 的项转成 `WireHandle`。**DirectVulkan 未注册的 8 个槽由 `CapsSnapshot.tableSlotMask` 精确复现**（`GL_Query.cpp:471,545,768` 拿槽位空否当能力探测）。 |
| **(b) `BackendObject` 虚函数** | `BackendObject_Remote`。8 个 EGL 生命周期虚函数 → `SurfaceOp` RPC（前 4 个阻塞，因为返回 `Bool`）。`GetDynamicParameters`(45)/`GetRendererInfo`(8)/`GetFormatCapabilities`(4)/`GetBackendType`(3)/`GetBackendAPIVersionString` → **CapsMirror 本地，零 round trip**。`DynamicBackendParameters`（`BackendObject.h:299-521`）是 flat POD，逐字节传。先例：`CompileEnv`（`CompileEnv.h:28-45`）就是为同一个原因做的同一件事。`GetBackendType()` 返回**远端**类型，所以 `GL_Texture.cpp:6453-6457`、`CompileEnv.cpp:122`、`GL_Framebuffer.cpp:38` 全部照旧。 |
| **(c) `BufferBackendOps`** | `g_emitBufferOps`：`Respecify`/`SubData`/`FlushMappedRange`/`OnDestroy` → 记录；`ResidentSubData` P7 之前不注册（只有 adopted store 才可达）；`AcquirePersistentMap` → P1-6 返回 `nullptr`，P7 返回 `AdoptSeg` 映射基址；`ReadbackFromGpu` → 阻塞请求（monolith 里本来就是 `glFinish()`，`Managers.cpp:1246`）。 |
| **(d) 状态拉取** | **不过线。** backend 读 server 自己的 replica。 |
| **(e) backend→frontend 写** | 大部分落在 replica 上；三类"语义建立者"由 client 自己做（`MarkGpuWritten`、纹理 dirty 清除、persistent-map 推送）；六类需要事件回传（§5.6）。**per-row `WritebackFromBackend` 循环（`Utils.cpp:2342`、`DirectGLES.cpp:7633`）在 server 内部执行，永远不会变成"每扫描线一次 IPC"。** |
| **(f) MG_Impl 反向调用** | server 链接完整 MG_Impl，6 处照常解析。default-FBO 描述通过 `EvDefaultFramebufferInfo` 事件回传给 client（`SwapchainObject.cpp:276-331` 的写在 server 侧发生）。 |
| **(g) MG_Impl 在 table 旁的 mutation** | 抽成 client/server 共享 helper（`MG_Remote::Shared::`），applier 在 replay 对应记录时调同一个 helper；或作为显式记录下发。由 §5.9b 的生成器强制全覆盖。 |

---

## 5. 状态 delta 模型

### 5.0 决定：replica `GLContext` vs 重写 backend

**选 replica。** 三条不可协商的证据：
1. **身份键 memo 无 delta 对应物。** `UnitTextureSyncEntry` 借用 binding slot 的 `shared_ptr` **地址**（`DirectGLES.cpp:1463-1467`），`PairingsIntact` 再校验 `entry.slot->get() != entry.texture`（`:1472-1477`）——注释明说没有它"replay 会拿纹理 B 的前端状态驱动纹理 A 的后端 twin"。`IsBufferDrawClean`（`Managers.cpp:1435-1436`）第一句就是资源裸指针身份比较。DirectVulkan 13 个缓存同类。replica 里这些**逐字工作**，因为对象仍由 `SharedPtr` 持有、unit 数组仍按值存放。
2. **回绕计数器。** `FramebufferBindingSlot::GetVersion()`、`FramebufferObject::GetObjectVersion()`、`VAO::GetIndexBufferBindingSlot().GetVersion()` 都是 `Uint16` 回绕，只有配合指针身份才正确。replay 让两侧跑同一段回绕逻辑。
3. **backend 自有 generation 表达的是"驱动对象被重新铸造"**（`g_bufferBackendIdGeneration`、`g_attachmentBackendIdGeneration`），**任何 client delta 都无法承载**——它们本来就该纯 server 侧，replica 天然满足。

代价：server 进程要链接 MG_State + MG_Impl + MG_Util（SPIRV-Cross、转译缓存、格式处理器、POST 探针）。这本来就无法避免——`BackendProgramObjectImpl::TranspileSpirvToEssl`（`Managers.cpp:6575-7110`）在 draw 线程跑 SPIRV-Cross，`UniformManager.cpp:1418-1497` 构造真实 `TextureObject`，`VulkanRenderer.cpp:4211-4356` 走 `ShaderObject::Compile()`/`ProgramObject::Link(false)`。"thin server"在这个代码库里是伪命题。

**replica 模型的边界必须明确写出来（R1 的真正内容）**：replica 只保证"对 backend 可见的状态"与 client 一致。凡是 client 的**入口点**（MG_Impl）在调 table 之外还做过的 MG_State 改动，applier 必须显式复刻——这不是理论风险，是 §2(g) 已经确认的两族实例。§5.9b 把它变成编译期门。

### 5.1 reconcile 在哪里发生

`MobileGL/MG_Remote/Client/WireMirror.{h,cpp}`，在**发射点**运行：每个 `GLFunctionsTable` 命令、`Present`、任何阻塞请求。

每个发射点分三步，顺序不可换：

**步骤 ①：`PublishImplicitState(scope)`** —— 复刻 backend 在 monolith 里会做的隐式发布，**必须在读任何版本计数器之前跑**，因为它自己会 bump 版本：
- 对 scope 内每个 **live persistent-mapped buffer** 调 client 侧的推送（§5.10）。
- 对 draw/dispatch scope，保守置 `MarkGpuWritten()`：镜像 `MarkShaderStorageBuffersGpuWritten`（`DirectGLES.cpp:459-467`，走 `GetTouchedBufferBindingPointCount(ShaderStorage)` + `GetBufferBindingPoint`）、`SyncAtomicCounterBuffers` 的 `:509`、以及可写 image-buffer 纹理的 `:1809`。XFB active 时对每个 capture target 同样置位（镜像 `VulkanRenderer.cpp:11210`）。
- 对 draw scope，若 XFB active，跑共享的 `AccountTransformFeedbackPrimitives` helper（§2(g)-2；monolith 里这一步本来就在 MG_Impl 里，拆分后它继续在 client 跑，同时把结果作为 `RecXfbAccounting` 下发给 replica）。
- 对 `GenerateMipmap` scope，`EnsureGeneratedMipmapStorageAllocated` 本来就在 client 的 MG_Impl 里跑过了；WireMirror 只需把它产生的 level 分配 + `TruncateMipmapLevels` + `BumpContentVersion` 作为 `RecGenerateMipmapLevels` 下发（§5.6a）。

**步骤 ②：可达性遍历。** 这就是 `DirectGLES::PrepareForDraw`（`DirectGLES.cpp:2916-2976`）的遍历，把 sync 换成 emit——不是比喻，是同一集合、同一顺序、同一门控：

1. `GetBoundVertexArray()` → `GetConfigVersion()`；其 enabled attribute 的 `BufferObject`；index buffer slot（**版本 + 裸指针身份**）。
2. `GetProgramForDraw()`（在 client 侧 join compile pool，与今天一致）→ link/UBO-content/block-binding/SSBO-override 版本。**composite pipeline 见 §5.7。**
3. texture unit `[0, GetMaxTouchedTextureUnit()]`，门控 `GetTextureBindGeneration()`；每纹理 `GetContentVersion()`/`GetTextureParamsVersion()`；每 unit `GetSamplerObject()->GetVersion()`。
4. image unit `[0, imageHighWater]` 经 `GetImageTextureBinding(unit)`。
5. 每 target 的 buffer binding point，上界 `GetTouchedBufferBindingPointCount(target)`。
6. draw/read FBO，门控 slot version + `GetObjectVersion()` + `GetAllFramebufferAttachmentVersions()`，再逐 attachment；**attachment 若是 renderbuffer，另查 `RenderbufferObject::GetVersion()`**（P0 新增，见 §5.4）。
7. `GetRenderStateParameters()`，门控 render-state 版本。
8. **pack** pixel-store（backend 从不读 unpack；六个读点全部传 `false`：`DirectGLES.cpp:6129,7614,9101,9480`、`Utils.cpp:2301`、`VulkanRenderer.cpp:10622`；`ScopedDefaultUnpackState` 强制默认值，`Managers.cpp:2888-2910`）。

**步骤 ③：清消费型状态。** 对本次发射的每个纹理 level 调 `MarkStorageDirty(uploadTarget, level, false)`（§5.6a）。

因为 backend 自身这套门控已被证明有界且便宜，reconciler 的每 draw 成本形状是**已知的**，不是估计。

存储：
```cpp
// MobileGL/MG_Remote/Client/WireMirror.h
struct ShipRecord {              // 40 B
    Uint64 shippedA, shippedB, shippedC;  // 打包版本元组，按 kind 解释
    Uint32 flags;                         // Created | Published | Deleted | ServerAuthoritative
    Uint32 pad;
};
class WireMirror {
    ska::flat_hash_map<Uint64 /*lifetimeId*/, ShipRecord> m_ship;
    struct DrawKeys { Uint64 contextId, samplingGen, bindGen; Int maxUnit; } m_lastDrawKeys;
    // ① 的输入：只遍历真正 mapped / 真正可能被 GPU 写的对象，不是全表
    ska::flat_hash_set<MG_State::GLState::BufferObject*> m_livePersistentMaps;
    ska::flat_hash_map<Uint64 /*lifetimeId*/, Uint64 /*emitSeq*/> m_gpuWritePendingSeq;
public:
    void PublishImplicitState(EmitScope, RingProducer&);
    void ReconcileForDraw(RingProducer&);      // 上面 1-8
    void ReconcileForDispatch(RingProducer&);
    void ReconcileForClear(RingProducer&);
    void OnObjectCreated(ObjKind, Uint32 name, Uint64 lifetimeId);
    void OnObjectDestroyed(ObjKind, Uint64 lifetimeId);
    void OnBufferMapped(BufferObject&, Range1D, BufferMappingAccess);
    void OnBufferUnmapped(BufferObject&);
};
```
外加与 backend `DrawTextureSyncKeys`（`DirectGLES.cpp:1496-1518`）同键的 per-draw memo：状态未变的重复 draw 只花 ~10 次整数比较就追加一条 32 字节记录。

### 5.2 版本计数器**不上线**

applier 不设置版本，它 **replay mutation**，所以 replica 的计数器恰在 applier 改动了东西时 bump——恰是 backend 必须重新 sync 的时刻。

- 不需要给 `RenderState`/`ProgramObject` 加 `Install*` setter（`Feat/CS-Delta-IPC` 的 `b50f3348` 加了，代价是把 `RenderState.h` 的私有成员漏成 public）。
- 不需要在 wire 上维护回绕 `Uint16` 的单调性。
- 唯一残留风险是**过度失效**：replica bump 了而 client 没 bump。只要 applier 只做 client 明确下发的 mutation，就不会发生；`RenderStateBlob` 整块下发是唯一例外（它整块 bump `m_version`，与 client 自己的 bump 等价）。

### 5.3 触发器 → delta 对照表

| Client 触发器（accessor / 事件） | Delta 记录 |
|---|---|
| `BufferObject::GetChangeSerial()` + emit-ops 里排队的 range | `RecBufferRespecify` / `RecBufferSubData` / `RecBufferFlushRange` |
| `glMapBuffer*` / `glUnmapBuffer`（**新增**） | `RecBufferMap{handle, range, accessFlags}` / `RecBufferUnmap{handle}` |
| persistent-map 脏块（**新增**，§5.10） | `RecBufferSubData`（块粒度） |
| `MipmapStorage::IsStorageDirty(target,level)`, `GetContentVersion()` | `RecTexAllocLevel` / `RecTexSubImage`（union box 或 ≤96 rects，变长） |
| `glGenerateMipmap` 前的 level 分配（**新增**） | `RecGenerateMipmapLevels{handle, target, requiredLevelCount, bytesPerTexel}` |
| `ITextureObject::GetTextureParamsVersion()` | `RecTexParam` |
| `ITextureObject::GetViewStorageOwner()` + view 字段 | `RecTexView`（**必须先于 owner 的任何 re-mint 顺序到达**） |
| `SamplerObject::GetVersion()` | `RecSamplerParam` |
| `RenderbufferObject::GetVersion()`（**P0 新增**） | `RecRenderbufferStorage{handle, internalFormat, w, h, samples}` |
| `VertexArrayObject::GetConfigVersion()` + 每 attrib Switch/Format/Buffer 版本 | `RecVaoConfig`（变长，整份配置；P6 再做逐属性 diff） |
| index buffer slot version **+ 指针身份** | `RecVaoIndexBuffer` |
| `FramebufferObject::GetObjectVersion()` + attachment 版本 | `RecFboAttach` / `RecFboDrawBuffers` / `RecFboReadBuffer` |
| `RenderState::m_version` / `m_pipelineStateVersion` | `RecRenderStateBlob`（整个 trivially-copyable `RenderStateParameters`，`RenderState.h:517-535`） |
| `ProgramObject::GetLinkVersion()` | `RecProgramLinkOp`(P1-4) → `RecProgramPublish`(P5+) |
| `ProgramObject::GetUBOContentVersion()` | `RecProgramUboContent` |
| block-binding / SSBO-override 版本 | `RecProgramBlockBinding` / `RecProgramSsboBinding` |
| `GetProgramForDraw()` 解析出 composite | `RecSetResolvedDrawProgram`（§5.7） |
| `GetTextureBindGeneration()` + unit slot 遍历 | `RecBindTexture` / `RecBindSampler` / `RecActiveTexture` |
| `GetTouchedBufferBindingPointCount()` 遍历 | `RecBindBuffer` / `RecBindBufferRange` |
| `GetImageTextureBinding(unit)` | `RecBindImageTexture` |
| pack `PixelStoreParameters` | `RecPixelStorePack` |
| XFB active 时的 draw（**新增**） | `RecXfbAccounting{pausedPrims, inputPrims, prims, capturedVerts, geomDraws, accountedDraws}` 增量 |
| `GLFunctionsTable` 命令 | `RecDraw*` / `RecClear*` / `RecBlit*` / `RecCopy*` / `RecDispatch*` / `RecXfb*` / `RecPresent` … |

### 5.4 对象身份、创建/删除顺序

wire handle = `WireHandle { kind:u8, glName:u32, lifetimeId:u64 }`。`GetLifetimeId()` 永不复用（`BufferObject.h:208`、`FramebufferObject.h:158`、`ProgramObject.h:1620`、`VertexArrayObject.h:120`、`SamplerObject.h:141`、`TextureObject.h:83,161`）。

**`RenderbufferObject` 既没有 `GetLifetimeId()` 也没有 `GetVersion()`（已在 `MG_State/GLState/RenderbufferState/RenderbufferObject.h` 上确认为零命中）—— Phase 0 两个都补上**，`GetVersion()` 取 `SamplerObject::GetVersion` 的同款形状（`Uint16`，每次 `RenderbufferStorage*` bump），并在 §5.1 步骤②-6 的 per-attachment 遍历里读它。理由：`BackendRenderbufferObject::SyncToBackend`（`Managers.cpp:~8620-8700`）缓存 `{internalFormat,width,height,samples}`，而对一个**已 attach 的** renderbuffer 重新 `glRenderbufferStorageMultisample` 不必然 bump `GetAllFramebufferAttachmentVersions()`，没有 `GetVersion()` 就没有触发器。

replica 使用**与 client 相同的 GL name**：applier 直接 `ctx.CreateBufferObject(name)`，绕过 server 自己的 `IndexGenerator`。server 侧维护 `ska::flat_hash_map<(kind,name), {SharedPtr<T>, clientLifetimeId}>`。

**若某次 create 的 `lifetimeId` 与记录不符 → `Fatal{IdentityDivergence}`，不做"先销毁再创建"的修复。** 上一版的"先销毁"是错的：replica 上那个对象可能仍被 FBO attachment、binding slot、texture view（`GetViewStorageOwner`）或 XFB capture target 通过 `SharedPtr` 合法持有，GL 保证它活到最后一个引用消失；强行销毁要么留下悬挂引用要么静默 detach，把一个协议 bug 变成一个会被归咎于 backend 的渲染 bug。协议正确时这个分支不可达，所以响亮地停下来严格优于静默的破坏性修复（`MOBILEGL_IPC_RESPAWN=1` 时改为强制 `ResyncSnapshot`）。

这就是 packed_pixels 的教训（身份 + 计数器，绝不单靠计数器）在协议层的应用，也是本设计对 name 空间漂移的**结构性预防**（而非事后 checksum 检测）。

创建/删除在 `glGen*`/`glDelete*` 时刻**立即**发射，顺序即 ring 顺序。client 的 `~BufferObject` 触发 emit-ops 的 `OnDestroy` 追加 `RecObjDelete`；server 的 replica `~BufferObject` 触发**真实**的 `Ops_OnDestroy`，完成 pooling / 延迟 `glDeleteBuffers`（`Managers.cpp:1271-1300`）——一行不改。

### 5.5 合并规则

1. **版本门控本身就是合并器。** 两个发射点之间的 N 次 mutation 折叠成一条 delta；改了又改回去的状态永不上线。
2. **Buffer range** 在 per-buffer `VecRange1D` 里累积（复用 `MG_Util/Math/VectorTypes.h:264` 已调优的 7% span gap 合并），在该 buffer 的下一个发射点 flush。**绝不 union 成整个 buffer**——`Managers.cpp:860-864` 的 postmortem 记录了那样会每帧重拷近乎整个 chunk-mesh arena。
3. **纹理区域**逐字沿用 `MipmapStorage::GetDirtyRects`，含 `summedArea*4 >= unionArea*3` 回退（`MipmapStorage.cpp:305`）。**注意代价轴是反的**：buffer 按字节计价，texture sub-image 按 **job 数**计价（`Managers.cpp:4311-4319`，~100 rects vs 一个 box 实测 +6ms/frame）。client 下发**区域形状**（union box 或 rect 列表，按 client 自己的 `GetDirtyRects` 判定），server 的 backend 从自己 replica 的 dirty 状态重新推导**上传形状**，让已调优的启发式留在付 GPU 代价的那一侧。
4. `RecRenderStateBlob`、各类 bind：last-writer-wins，reconciler 只发**当前值**。
5. **命令永不合并、永不重排。**

### 5.6 backend→frontend 写：三种归属

| 写 | 归属 |
|---|---|
| `SetBackendResource`、`SetBackendHashMemo`/`StateMemo`/`AuxMemo` | **纯 server 本地**，零 wire 流量 |
| `MarkGpuWritten` ×3 + `EnsureGpuResidentStorage` | **client 保守自建**（§5.6b）。server 侧照常在 replica 上置位；`EvGpuWritten{handle, ranges[]}` 仅作为**收窄提示** |
| `MarkStorageDirty(…,false)` ×11 | **client 在发射后自己清**（§5.6a）。server 侧照常在 replica 上清 |
| `MarkStorageDirty(…,true)`（`Managers.cpp:2813` RequireImageBindableStorage 的 re-dirty） | 纯 server 本地：它是 server 的 re-mint 导致的，client 无从预测，也无需知道——重传由 server 自己在 replica 上完成 |
| `WritebackFromBackend`（PBO/XFB） | server 侧写 replica shadow；合并后的 range 变成 `EvBufferWriteback` 回传 client |
| `RecordError` ×2（`DirectGLES.cpp:6319`、`Managers.cpp:8679`）+ DirectVulkan 4 处 | **分两类**（§5.6c）：分配类同步 ack，其余走 `EvGlError` 晚一批可见 |
| `AllocateStorage` 生成 mip（`DirectGLES.cpp:6270-6271,6861`）、`MirrorCopyImageIntoDestinationShadow`（`:7144`） | **per-level `serverAuthoritative` 位**（§6.6） |
| `InvalidateCompileEnv`、`SwapchainObject` 改写 default-FBO 占位纹理 | 事件 `EvCompileEnvInvalidate` / `EvDefaultFramebufferInfo` |

#### 5.6a 纹理 dirty flag：client 必须清（推翻上一版）

上一版写"client 的 dirty flag 从不被清，已发送状态存在 WireMirror 里"。这是错的：
- `MipmapStorage::MarkDirtyRegion`（`MipmapStorage.cpp:196-233`）只要 `m_isDirty[level]` 为真，就把 incoming **union 进** `m_dirtyRegions[level]` 并 `InsertDirtyRect`；只有 `MarkDirty(level,false)`（`:171-189`）重置两者。永不清 ⇒ box 单调增长、rect 列表撑满 `kMaxDirtyRects`、`GetDirtyRects` 一旦跨过 3/4 阈值就返回 0（"用 box"），于是每次动画图集 tick 都传整个 level。
- `ShipRecord` 只有三个 `Uint64` 版本字，**无法**从中重建区域。
- `MarkDirtyRegion` 的 rect 播种分支（`:214-221`：`if (!m_isDirty[level]) rects.clear(); else if (rects.empty() && !region.Empty()) rects.push_back(region);`）本身就是为"有人会清"写的。

好消息是清是安全的：**MG_Impl 里没有任何 `IsStorageDirty(` / `GetStorageDirtyRects(` / `GetStorageDirtyRegion(` 调用点**（已 grep 确认为零），前端从不读自己的 dirty 状态；它自己也在五处主动清（`GL_Texture.cpp:528,701,5547,5621,5691`）。

**规则**：WireMirror 在追加纹理记录之后，立刻对该 (target, level) 调 `MarkStorageDirty(..., false)`。ack 问题按两条收口：
1. `ResyncSnapshot` 永远从**完好的 shadow** 传整 level（shadow 从不被丢弃，除非 buffer 被 adopt——纹理没有 adopt 路径），所以"清早了导致重传丢数据"在 resync 场景不成立。
2. 硬 drain（§6.5）会 bump `ringGeneration`；drain 后 client 对**所有已发射但未 `appliedSeq` 覆盖的纹理记录**做一次重发（WireMirror 保留最近一批记录的 (handle, target, level) 列表 + emitSeq，drain 时把 seq > appliedSeq 的重新标脏并重发）。这是有界的，因为 ring 里最多只有 ring 容量那么多未 apply 的记录。

#### 5.6b `MarkGpuWritten`：client 保守自建（推翻上一版）

monolith 里这个 flag 是在 draw 调用**内部同步**置位的：`MarkShaderStorageBuffersGpuWritten`（`DirectGLES.cpp:459-467`）走 `GetTouchedBufferBindingPointCount(ShaderStorage)` 并对每个绑定对象 `MarkGpuWritten()`，从 draw 路径的 `SyncNeccessaryBuffers` 调用（`DirectGLES.cpp:687,697`）；atomic counter 在 `:509`；可写 image-buffer 纹理在 `:1809`；DirectVulkan 在 `UniformManager.cpp:1073,1229` 与 `VulkanRenderer.cpp:11210`。

拆分后 draw 是 fire-and-forget，所以 `glDispatchCompute(); glMapBufferRange(SSBO,...,GL_MAP_READ_BIT);` 会在 server 还没 apply 前就走完 `AcquireMemoryRange` → `SyncGpuWrites()`（`BufferObject.cpp:454`）→ `m_gpuWritePending` 为 false → 立即 return（`BufferObject.cpp:266`）→ 应用拿到陈旧 shadow，零 round trip、零报错。这会以"看起来像 flaky"的形式打掉 P4 计划里的 `SsboArrayLengthScenario`、`AtomicCounterScenario`、`StorageBufferRegrowScenario` 一整族。

**规则**：`PublishImplicitState`（§5.1 步骤①）在每个 draw/dispatch 发射点保守置位，输入与 `DirectGLES.cpp:459-467/509/1809` 完全一致（client 全都有）。同时把 `emitSeq` 记进 `m_gpuWritePendingSeq`。在任一读入口（`glMapBuffer*`、`glMapBufferRange`、`glGetBufferSubData`、`glGetNamedBufferSubData`、`glCopyBufferSubData` 的源、`FillSubData`）：若该 buffer 在 pending 集合里 → `Publish()` → 等 `appliedSeq >= recordedSeq` → 排空 `SEG_EVENT` → 再读。`EvGpuWritten{handle, ranges[]}` 只用于**取消**该 pending 项或**收窄** readback 范围，晚到无害。

同时，§7.4 的事件排空点必须补上 `glMapBuffer` / `glMapBufferRange` / `glGetBufferSubData` / `glGetNamedBufferSubData`——上一版的排空点列表（`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`eglSwapBuffers`）不含它们。

#### 5.6c GL 错误：分配类同步 ack，其余晚到

上一版把所有 backend `RecordError` 一律走"晚一批"事件，只给 CTS lane 留 `MOBILEGL_IPC_STRICT_ERRORS`。这在**分配探测**这个通用惯用法上是错的：那两个站点（`Managers.cpp:8679` renderbuffer 存储、`DirectGLES.cpp:6319` 纹理操作）报的是 `GL_OUT_OF_MEMORY`，而应用的标准写法是 `glRenderbufferStorage(...); if (glGetError() == GL_OUT_OF_MEMORY) { 用更小的目标重试; }`。晚到 ⇒ 应用走成功分支 ⇒ 往一块 server 从未分配的存储上渲染。

**规则**：只把**分配类**入口点标 `kNeedsAck`——`glRenderbufferStorage` / `glRenderbufferStorageMultisample` / `glNamedRenderbufferStorage*`、`glTexImage*` / `glTexStorage*` / `glCopyTexImage*` 中 backend 可能失败的形式、`glBufferStorage`。它们本来就罕见且昂贵，ack 几乎免费，换来 OOM 探测精确。其余全部保持晚到。有了这个划分，`MOBILEGL_IPC_STRICT_ERRORS` 从"CTS 专用"降级为纯诊断开关（默认 0，出问题时用来判断某个失败是不是错误时序引起的）。

`glGetError` 本身永远本地（`GL_Getter.cpp:2811-2817`；`Core.cpp:48-49` 的 "GL error state is GL-thread-owned" 不变式）。

### 5.7 composite pipeline program

`GLContext::GetProgramForDraw()`（`Core.cpp:612-660`）在 program-pipeline 路径下：join 每个 stage → `ComputeDrawProgramSignature()` → cache miss 时 **`MakeShared<ProgramObject>(0u)` 并 link 一个匿名 composite**（`Core.cpp:644`；注释明说"故意不是命名 program……不得占用应用可能拿到的 name"），随后 `RefreshCompositeUniforms`/`MirrorUniformValues` 每 draw 改它。

- **Phase 1-4（server relink）**：下发 pipeline 状态（`UseProgramStages` 等）+ 各 stage program 的 `RecProgramLinkOp`；server 的 replica 自己走同一路径构建自己的 composite。加一条 `RecResolvedProgramDigest{signature, reflectionDigest}` 让分歧当场暴露。
- **Phase 5+（ProgramPublish）**：server 没有源码，**不得 link**。client 解析 composite，把它作为**保留高位 handle 的合成 program** 发布（`RecProgramPublish` + `RecSetResolvedDrawProgram{handle}`）。MG_State 加：
```cpp
// MobileGL/MG_State/GLState/Core.h  （整段 #if MOBILEGL_BUILD_DISAGGREGATED 包裹，保证 monolith 字节不变）
void SetReplicaResolvedDrawProgram(SharedPtr<ProgramObject>);
void SetReplicaResolvedDispatchProgram(SharedPtr<ProgramObject>);
// GetProgramForDraw()/GetProgramForDispatch() 首行先查该槽位
```
server 因此**永不 link、永不 join compile pool**，`PrepareForDraw` 首条语句照常工作。

### 5.8 全量快照 / resync

稳态**没有初始状态**：transport 在 `MG_Backend::Init()` 内建立，早于任何 GL 对象存在。

`ResyncSnapshot` 只服务三件事：**server 重启**、**backend context 丢失**（EGL surface 变更销毁整个原生 context 并 bump `g_backendContextGeneration`/`g_syncContextGeneration`，`DirectGLES.cpp:10664-10676`）、**硬 drain 后的纹理重发**（§5.6a）。实现 = 同一个 reconcile 遍历，关闭"已发送版本"门控。

**关键纪律：一个 applier、两个 producer**——快照发同样的记录种类，因而被同一套测试覆盖。`Feat/CS-Delta-IPC` 的结构性错误正是有一个与生产路径零共享代码的平行 applier（`StateEmitter.h:312-501` vs `ServerCore.cpp:389-401`）。

**P7 之后的限制**：adopted store 的字节住在 server，client 无法重建它们。因此 `MOBILEGL_IPC_RESPAWN=1` 与 `MOBILEGL_IPC_ADOPT_TIER != 2` 互斥：要么关采纳换可 resync，要么开采纳并接受 server 死亡 = context lost（不重启）。这条互斥必须在 `ConfigLoader` 里显式检查并 `MGLOG_W`。

### 5.9 覆盖度的**编译期**保证

#### 5.9a READ 面（backend 读了什么）

1. `scripts/gen_backend_state_surface.py` 扫描 `MG_Backend/**`，抽出 `pGLContext->X` 与前端对象 getter，生成 `MG_Remote/Protocol/generated/BackendStateSurface.inc`（**已提交**）。相对 `Feat/CS-Delta-IPC` 的 `extract_backend_read_inventory.py`：**删掉 `GetBuffer*`/`GetTexture*`/`GetProgram*`/`GetVertex*` 前缀兜底规则**（`:234-241`，它把"0 UNMAPPED"制造出来），未知 accessor 一律 `UNMAPPED`。同时把"真 pull point"与"signature handle 化"分开统计（那 167 个 "handle-ify" 里含 `BackendObject.h:158-186` 的**声明**和 `DirectGLES.cpp:55` 的静态全局）。
2. 手维护 `MG_Remote/Protocol/Coverage.def`：`accessor → 记录种类 | MGL_COVER_LOCAL | MGL_COVER_NA(理由字符串)`。
3. `MG_Remote/Client/CoverageAssert.cpp` 同时 include 两者，未映射 accessor → `#error`。

#### 5.9b MUTATOR 面（MG_Impl 在 table 调用旁改了什么）—— **本轮新增，是 §2(g) 的门**

1. `scripts/gen_impl_mutation_surface.py` 扫描 `MG_Impl/**`：找出**同时**包含 `gBackendFunctionsTable.GL.*` 或 `pActiveBackendObject->` 调用**和** `pGLContext->` mutator 调用（写方法：`Add*`/`Set*`/`Mark*`/`Bump*`/`Allocate*`/`Truncate*`/`Record*`/`Notify*`/`Begin*`/`End*`）的函数，把每个 mutator 站点写进 `MG_Remote/Protocol/generated/ImplMutationSurface.inc`（**已提交**）。为避免误报，脚本对每个函数做一次简单的调用图一层展开（`EnsureGeneratedMipmapStorageAllocated` 这种 helper 会被计入调用它的 `GenerateMipmap`）。
2. 手维护 `MG_Remote/Protocol/MutationCoverage.def`：`函数::mutator → MGL_MUT_REPLAYED_BY(记录种类) | MGL_MUT_SHARED_HELPER(helper 名) | MGL_MUT_CLIENT_ONLY(理由) | MGL_MUT_NA(理由)`。
3. 同一个 `CoverageAssert.cpp` 展开两张表，未映射站点 → `#error`。

已知必须在第一轮映射的条目（不是穷举，是脚本首次运行时保证不为空的锚点）：
- `GenerateMipmap` / `GenerateTextureMipmap` / `MaybeAutoGenerateMipmap` → `EnsureGeneratedMipmapStorageAllocated` 的 `AllocateStorage` / `MarkStorageDirty(false)` / `TruncateMipmapLevels` / `BumpContentVersion` ⇒ `MGL_MUT_REPLAYED_BY(RecGenerateMipmapLevels)`。applier 收到该记录后调**同一个共享 helper**（把 `EnsureGeneratedMipmapStorageAllocated` 抽到 `MG_Remote::Shared::` 或让 applier 直接调 `MG_Impl::GLImpl::TextureImpl::` 里那个已存在的函数——server 链接完整 MG_Impl，这是可行且最省的做法）。
- `DrawArrays`/`DrawElements`/… 的 `AccountTransformFeedbackPrimitives` 六个计数器 ⇒ `MGL_MUT_REPLAYED_BY(RecXfbAccounting)`（applier 把六个增量加到 replica 的对应计数器上；必须跟着 `RecBindTransformFeedback` 的对象切换走，因为它们按 XFB 对象存取，`Core.cpp:1273,1296`）。
- `glCopyTexSubImage*` 里 `CopyReadFramebufferIntoMipmapRegion` 的 `MarkStorageDirty(...,true)`（`GL_Texture.cpp:1095`）⇒ `MGL_MUT_CLIENT_ONLY`（该函数整体留在 client，见 §6.6）。
- `glClearTexImage` 的 `MarkStorageDirty(...,true)`（`GL_Texture.cpp:1005`）⇒ `MGL_MUT_CLIENT_ONLY`（同上）。
- `GL_Query.cpp` 的 conditional-render 布尔与查询结果缓存 ⇒ `MGL_MUT_CLIENT_ONLY`。

CI：两个生成器都重新生成 + `git diff --exit-code`。

**backend 长出一个 reconciler 走不到的 read，或 MG_Impl 长出一个 applier 没 replay 的 mutation → 编译失败，而不是设备回归。**

### 5.10 persistent map：client 侧的推送（本轮新增的独立小节）

**问题**（已在仓库确认）：`BufferObject::SyncPersistentMappedRange()`（`BufferObject.cpp:238-250`）依次早退于 GPU-resident、非 Persistent、非 Write、FlushExplicit、空 range，剩下的情况（**persistent + write + coherent + shadow-backed**）走 `NotifySubData(整个 mapped range)`。它的全部生产调用点都在 `MG_Backend/` 里（19 处，见 §0）。P1-P6 默认关采纳（§6.8 T2），`AcquireMemoryRange`（`BufferObject.cpp:459-475`）于是回退到 shadow 并把 `m_resource.Bytes() + range.start` 交给应用——应用之后**不再调任何 GL 函数**就直接写。拆分后：client 没人推，server 的 replica `m_isMapped==false` 第一行就 return。字节丢失。

另外，`IsBufferDrawClean` 里 `if (frontend->IsMapped()) return false;`（`Managers.cpp:1447`，注释："A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push"）也依赖 map 位，replica 上恒 false 会把这个 buffer 判成 clean 而跳过整个同步。

**解法三件套**：

1. **map/unmap 上线**：`RecBufferMap{handle, rangeStart, rangeEnd, accessFlags}` 与 `RecBufferUnmap{handle}`，从 `glMapBuffer`/`glMapBufferRange`/`glUnmapBuffer`/`glFlushMappedBufferRange` 的 MG_Impl 入口发射（emit-ops 的 `FlushMappedRange` 已覆盖最后一个）。replica 的 `m_isMapped`/`m_mappedRange`/`m_mappingAccess` 于是与 client 一致，`IsMapped()` 门和 server 侧的 `SyncPersistentMappedRange` 都恢复 monolith 行为。

2. **client 侧脏块推送**：WireMirror 维护 `m_livePersistentMaps`（只装 persistent+write+非-FlushExplicit+非-GpuResident 的 buffer，进出由 `OnBufferMapped`/`OnBufferUnmapped` 维护）。`PublishImplicitState` 对**本次操作可达的**每个这类 buffer（VAO attribute buffer、index buffer、indirect/parameter buffer、UBO/SSBO/atomic binding point、XFB capture target——即 backend 那 19 个调用点的并集）做**块粒度**发送：把 mapped span 切成 64KiB 块，只发自上次发送以来被改过的块。

   "被改过"的判定：P1-4 用**保守版**（每个发射点把该 buffer 的整个 mapped span 当脏，但按块拆成多条 `RecBufferSubData`，让 §6.5 的 range 合并与 ring 复用机制生效）；P4.5 shadow-in-shm 落地后升级为**精确版**（shadow 住在 client 拥有的 `SEG_SHADOW` 里，用与 WAR 水位同一套 64KiB 块脏位跟踪；块脏位由 `SyncPersistentMappedRange` 的调用点触发一次 `memcmp` 或由 mprotect 写屏障提供——先做 `memcmp`，它对 1MB 块是 ~50µs 量级，且只在真正 mapped 的 buffer 上跑）。

   **这是 §6.4 拷贝表里上一版完全没有的一行**，且在 P1-4 的保守版下代价可观（一个持久映射的 chunk arena 会在每个可达发射点重传整个 mapped span）。所以：`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`（默认 64）可调，且**P1 验收必须记录这条路径的字节量**（Tracy 计数器分类为 `persistent-map-push`）。若 P1-4 的保守版在 Create/Flywheel fixture 上不可接受，把 P4.5 的精确版提前到 P2（这是计划里唯一一个允许因测量结果而改变阶段顺序的地方）。

3. **P1 就要有门**：新增 `PersistentCoherentMapScenario`（map PERSISTENT|WRITE|COHERENT、写、不做任何其它 GL 调用、draw、readback 校验），列为 P1 验收项。**今天计划里没有任何门能抓到这个 bug。**

**与 `MOBILEGL_COHERENT_AS_FLUSH` 的关系**：该开关（`GL_Buffer.cpp:297-305`，默认 false，`Config.h:174` / `ConfigLoader.cpp:185`）把应用请求的 persistent+FLUSH_EXPLICIT 改写成 coherent，从而**制造**上面这个情形。上一版禁止它在拆分模式下生效——但那只处理了"我们自己改写出来的 coherent map"，没处理"应用自己就请求 coherent"。有了上面的三件套，两种来源都被覆盖，所以**禁令改为可选**：`MOBILEGL_COHERENT_AS_FLUSH` 在拆分模式下**照常生效**，这样 `tools/trace_replay/trace_cases.json` 里那两个带 `coherent_as_flush: true` 的用例（`minecraft-1.21.1-neoforge-create-indirect-in-world`、`minecraft-1.21.1-neoforge-create-instancing-in-world`）在 split 与 monolith 下走同一条 buffer 路径，P2 的逐名对比才有意义。若 P2 测出保守推送在这两个 fixture 上代价过高，改为"这两个用例在 split 模式下同时关掉该开关，并在报告里标注"，而不是让两侧走不同路径还宣称对比通过。
