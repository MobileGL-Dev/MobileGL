# MobileGL 前后端进程拆分实施计划（branch `feat/disaggregated`）

> 状态：设计定稿 v1（2026-09-05）。基线 `dev@81b17c0b`；实施分支 `feat/disaggregated`（worktree `../MobileGL-disagg`）。
> 产出方式：7 个只读代码调研 → 4 个独立架构方案 → 3 个评审打分 → 综合 → 3 个对抗性审查（38 条发现）→ 修订；评审记录见同目录 `REVIEW.md`。
> 上一次尝试 `Feat/CS-Delta-IPC`（2026-08-29/30，worktree `../MobileGL-CS`）的复用/丢弃结论见 §14。

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
`DirectGLES.cpp:640-663` 与 `Managers.cpp:875-877` 有**未提交的 per-draw `fprintf(stderr)`**（格式串里还有字面量 `' + NL + '`，且位于 `pendingMutex` 临界区内的 buffer flush 路径上）。`Feat/CS-Delta-IPC` 的 `d96be9f3` 提交过同类东西（`DirectGLES.cpp:+2583-2590`），导致该分支上**每一次测量**（144-failure Windows run、OpenRA `ssim=0.000036` 设备 run）都跑在每 draw 一次 stderr 写的构建上。

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

---

## 6. 数据面

### 6.1 段（segment）布局

| 段 | 拥有者 | 默认大小 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB，2 的幂，64B 对齐 | `RingControl`(4KiB) + POD 记录 + ≤4KiB 内联负载 |
| `SEG_STAGE` | client（server 只读） | 32 MiB → 上限由实测定，**不是默认 256 MiB** | bulk 字节：buffer sub-data、纹理区域、UBO scratch、client 顶点/索引/indirect 数组、persistent-map 脏块 |
| `SEG_REPLY` | **server**（client 只读） | 8 MiB，4KiB slot | readback 像素、buffer writeback |
| `SEG_EVENT` | **server**（client 只读） | 256 KiB SPSC ring | `EvQueryResult`/`EvGpuWritten`/`EvGlError`/`EvLogLine`/`EvDefaultFramebufferInfo`… |
| `SEG_SHADOW[n]` | client（server 只读） | 每对象，P4.5+，≥256KiB shadow | 零拷贝 buffer/texture shadow |
| `SEG_ADOPT[n]` | **server**（client RW） | 每 buffer，P7，≥16MiB adopted store | 应用直写 GPU 内存 |

创建：Android `ASharedMemory_create`（API 26，`android/sharedmem.h:78`；libc 的 `memfd_create` wrapper 是 API 30，`sys/mman.h:196`）；桌面 Linux `syscall(SYS_memfd_create, …)`；macOS `shm_open`+`shm_unlink`；Windows `CreateFileMappingW`(`Local\`)。

**传递：POSIX `SCM_RIGHTS`，在第一个 transport commit 里实现**（asio 无 cmsg API → 在 `socket.native_handle()` 上裸 `sendmsg`/`recvmsg`，约 80 行）。`Feat/CS-Delta-IPC` 把它推迟到"P6"（`LocalSocketTransport.h:16-20`，`PollOffer` 里 `out->fd = -1` 硬编码于 `:296`），结果它的数据面在唯一重要的平台上**一个字节都过不去**。

**SEG_SHADOW 块的退休规则（本轮新增）**：§6.4 的 64KiB 块发送水位只解决"覆盖一个**活着的** shadow"；它没说怎么**释放**一个 shadow。`glDeleteBuffers` 或 `glBufferData` 重定义会释放/重分配 `SEG_SHADOW` 的 arena 块，而携带 `{segId, offset, size}` 指向该块的记录可能还没被 apply——server 于是读到另一个对象的字节。规则：释放的块进入 pending 链表，只有当 `appliedSeq`（对被借入 GPU 时间线的 slot 是 `retiredSeq`）越过最后一条引用它的记录之后才归还 arena，而不是在对象析构时立即归还。

### 6.2 RingControl：watermark 是一条共享 cache line，**且带双向 doorbell**

```cpp
// MobileGL/MG_Remote/Transport/Ring.h
struct alignas(4096) RingControl {
    // ---- SEG_CMD 游标 ----
    alignas(64) std::atomic<Uint64> cmdHead;              // producer：累计写入字节
    alignas(64) std::atomic<Uint64> cmdAppliedTail;       // consumer：已解码并拷出的字节
                std::atomic<Uint64> cmdRetiredTail;       // consumer：被借入 GPU 时间线的 slot 已释放
    // ---- SEG_STAGE 游标（独立三元组；上一版遗漏）----
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
                std::atomic<Uint32> producerParked;       // client 睡了，server 要敲门（本轮新增）
                std::atomic<Uint32> eventRingFull;        // SEG_EVENT 满，server 已停止 apply
                std::atomic<Uint32> eventDropped;         // 被丢弃的 EvLogLine 计数
};
```

**三个 seq 水位严格区分**（混为一谈是经典错误）：`appliedSeq` 释放 `cmdAppliedTail`/`stageAppliedTail`；`submittedSeq` 释放 staging；`retiredSeq`/`completedFrameSerial` 释放 `*RetiredTail` 与 `SEG_ADOPT` 复用。

**两个 tail 是必须的**：`Ops_ResidentSubData` 把字节拷进 `pendingResidentWrites`（`Managers.cpp:1158-1166`），P7 之后 server 会**借用** ring slot 而不是再拷一次——那种 slot 只能在 `completedFrameSerial` 之后回收。单 tail 会在 P7 落地当天变成保守回收。

**SEG_STAGE 必须有自己的游标三元组**：§7.2 把"`SEG_STAGE` 余量 < 1/4"列为 Publish 触发器，而第二个 ring 的占用率无法从第一个 ring 的游标算出；且 stage slot 的退休条件（`retiredSeq`）与 cmd 记录（`appliedSeq`）不同。

#### 6.2a 双向 doorbell（本轮新增，修 "client 只能自旋" 的缺陷）

- **client → server**：consumer 自旋 ~200µs → 置 `consumerParked=1` → 在控制 socket 上阻塞读 1 字节；producer 在 release-store `cmdHead` 之后，仅当 `consumerParked` 时写 1 字节（字节码 `0x01 = 'ring advanced'`）。
- **server → client**（上一版缺失）：client 在**任何**等待里（present credit、`kNeedsAck` 阻塞请求、ring/stage 满的升级等待）先自旋 `MOBILEGL_IPC_SPIN_US`（默认 50µs），再置 `producerParked=1`，然后在同一个 socket 的反向流上阻塞读；server 在 release-store 任何 watermark 之后，仅当 `producerParked` 时写 1 字节（字节码 `0x02 = 'watermark advanced'`）。

没有这一条，上一版的每一处 client 等待都退化成跨进程自旋一条共享 cache line：present-credit 等待最长一整帧（60Hz 下 16.6ms），在手机上就是一颗大核满频空转，与 GPU 和游戏 JVM 抢核；§6.5 的"有界 50ms 等待"就是 50ms 自旋。而 MobileGL 全库没有任何亲和性控制（`grep -rn 'sched_setaffinity\|cpu_set_t' MobileGL/` 零命中），无法把它赶到小核上。

`spawn` 模式用 socketpair 的两个方向做 doorbell；`inproc` 模式用一对 `std::condition_variable`（同一套 `producerParked`/`consumerParked` 语义）。**零 futex/eventfd/named-event 平台代码**（asio 已 vendored，`3rdparty/asio/include` 已在主 target 的 include path 上，`CMakeLists.txt:483`）。

### 6.3 记录格式

```cpp
// MobileGL/MG_Remote/Protocol/RecordKinds.h
struct RecHeader { Uint16 kind; Uint16 flags; Uint32 size; };   // 8 B，size 含 header，8 字节倍数
enum RecFlags : Uint16 { kNone=0, kNeedsAck=1<<0, kHasBlob=1<<1, kPad=1<<2, kBorrowSlot=1<<3, kVarTail=1<<4 };
struct BlobRef  { Uint32 seg; Uint32 pad; Uint64 offset; Uint64 size; };  // 24 B
```
**没有 per-record 序号字段**：seq 就是记录序数（producer `m_emitSeq++`，consumer `m_applySeq++`），省 8B/记录并消除一整类失步。

X-macro 单一真相源：
```cpp
// MobileGL/MG_Remote/Protocol/Records.def
#define MGL_REC_LIST(X)                                       \
    X(BindBuffer,      RecBindBuffer,       24)               \
    X(DrawArrays,      RecDrawArrays,       32)               \
    X(DrawElements,    RecDrawElements,     56)               \
    X(BufferSubData,   RecBufferSubData,    64)               \
    X(BufferMap,       RecBufferMap,        40)               \
    X(BufferUnmap,     RecBufferUnmap,      24)               \
    X(RenderStateBlob, RecRenderStateBlob,  40)               \
    X(XfbAccounting,   RecXfbAccounting,    56)               \
    X(GenerateMipmapLevels, RecGenerateMipmapLevels, 32)      \
    X(RenderbufferStorage,  RecRenderbufferStorage,  40)      \
    /* … ~95 项 … */
#define MGL_REC_SIZE_CHECK(name, T, sz) \
    static_assert(sizeof(MobileGL::Wire::T) == (sz), #name " record size drift");
MGL_REC_LIST(MGL_REC_SIZE_CHECK)
```
**每种一条 `static_assert`** ——修掉正是 `Feat/CS-Delta-IPC` 中过一次的 bug 类（`b50f3348`："旧的 off-by-one 让 applier 误读 TexImage 之后的每一条 state delta"），而它那条只断言 union 首成员的 assert（`ServerCore.cpp:31-33`）永远抓不到中间插入。

**运行期边界纪律（本轮新增）**：`SEG_CMD` 是对端并发写入的区域，编译期 `static_assert` 管不到运行期损坏。同一个 X-macro 额外生成 applier 分发前的前置条件：
```cpp
#define MGL_REC_BOUNDS_CHECK(name, T, sz)                                     \
    case RecKind::name:                                                       \
        if (h.size < (sz) || h.size > remainingRingBytes || (h.size & 7u))    \
            return Fatal(FatalCode::ProtocolCorruption, #name);               \
        break;
```
`kVarTail` 记录额外校验 `定长前缀 + 尾巴自描述长度 == h.size`。违反一律 `Fatal{ProtocolCorruption}`，绝不进入未定义行为。

变长记录（`RecVaoConfig`、`RecTexSubImage` 的 rect 列表、`RecProgramLinkOp`、`RecMultiDrawArgs`）：`kVarTail` + 定长前缀 + 自描述长度的内联尾巴。

### 6.4 WAR 危害与字节稳定性

**Phase 1-4 规则：GL 调用时刻把字节拷进 ring slot。** slot 从写入到 `stageAppliedTail` 越过它为止不可变，client 拿不回它 → **危害按构造消除**。代价是一次 memcpy，而 `Ops_ResidentSubData`（`Managers.cpp:1165`）和 `StageBlocksIntoUnpackRing` 在 monolith 里已经在付同样的钱。

**Phase 4.5 规则（shadow-in-shm，零拷贝）：** ≥256KiB 的 shadow 分配在 client 拥有的 `SEG_SHADOW` 里——`PipeResource` 的 `MapAlignedAllocator`（`PipeResource.h:33-60`，无状态、25 行、64B 对齐）增加一个 shm arena（保留 `MIN_MAP_BUFFER_ALIGNMENT=64` 契约，`PipeResource.h:28`），`MipmapStorage` 的 level vector 同理。`RecBufferSubData` 于是只带 `{segId, offset, size}`，**client 侧零拷贝**。
WAR 用 **per-shadow 64KiB 块发送水位**：若应用写入某块而该块最后一次发送尚未 `appliedSeq` 覆盖，这次写走 `SEG_STAGE`。有界、局部、压力下自动退化成 Phase-1 行为。这套块水位同时是 §5.10 精确版 persistent-map 推送的脏位来源。

**该改动必须整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹**：`PipeResource` 与 `MipmapStorage` 住在 `MG_State`，不在 `MG_Remote`，而改一个容器的 allocator 就改了类型；不包裹的话 §12/D8 的 `nm`/`.text` 门会在 P4.5 变红。写法是"分配器特化：option OFF 时逐字折叠成今天的 `MapAlignedAllocator`"。

#### 拷贝账（更正版，MC pan 一帧约 9MB section mesh + ~1MB UBO scratch）

上一版这张表把 monolith 和 split 两侧都数少了。逐条核对：

- monolith 的 `glBufferSubData` → shadow store 是 **2 次**：(1) app→shadow（`BufferObject::UploadSubData` 的 `Memcpy`），(2) shadow→目的地（`FlushPendingRangesNow`：`Memcpy(dst, bufferObject.MappedData()+start, size)` 进 invalidating map，`Managers.cpp:914`；或 `Memcpy(g_uploadRing.store.mappedPtr+ringOffset, ..., size)` 进 upload ring，`Managers.cpp:922`）。
- split P1-4 是 **4 次**：app→client shadow (1)、client shadow→`SEG_STAGE` (2)、applier replay mutator ⇒ `SEG_STAGE`→**replica** shadow (3)、server 的 `FlushPendingRangesNow` ⇒ replica shadow→upload ring (4)。
- P4.5 只去掉 (2)，剩 **3 次**。它去不掉 (3)，因为 `SEG_SHADOW` 是 client 拥有 / server 只读，而 replica 的 `BufferObject` 拥有自己的 `PipeResource` 分配。

| 路径 | monolith | P1-4 | P4.5 | P4.5+replica-adopt（可选，见下） |
|---|---|---|---|---|
| `glBufferSubData` → shadow store | 2 | 4 | 3 | **2** |
| `glBufferSubData` → adopted store（P7） | 2 | — | — | 2 |
| `glMapBufferRange(WRITE)`+unmap | 3 | 5 | 4 | 3 |
| persistent coherent map 推送（§5.10 保守版） | 0 | 2/发射点 | 1/发射点(精确块) | 1/发射点 |
| `glTexSubImage` | 2 | 3 | 2 | 2 |
| 全局 UBO / draw | 1 | 2 | 2 | 1 |
| adopted ≥16MiB（P7 T1/T0） | 0 | — | — | 0 |

**目标选择（必须在 P4.5 之前拍板）**：
- **方案 A（默认，保守）**：接受 3 次，写进文档。P4.5 的价值是消掉 client 侧那次拷贝与那份重复内存。
- **方案 B（激进，需额外设计）**：给 replica 的 `PipeResource` 增加**第三种模式** `AdoptedClientShadow`——`Bytes()` 返回 server 映射的 client `SEG_SHADOW`（只读），applier 的 `UploadSubData` 退化成一次 range 记账 + change-serial bump，只剩 server 的 ring 拷贝。这保持了 mutator replay 的全部副作用（包括 `IsBufferDrawClean` 比较的 change serial），只是不搬字节。风险：replica 的 shadow 变成只读会让任何 server 侧写（`WritebackFromBackend`、生成 mip、CopyImage 镜像）需要就地 copy-on-write 升级回普通 shadow。**先按方案 A 实现并测量，方案 B 作为 P6 的候选优化项，由 Tracy 计数器决定是否值得。**

无论选哪个，`TracyPlot` 字节计数器必须**装在 wire 两侧**（client 的 emit 字节 + server 的 apply 字节 + server 的 ring/staging 字节），P4.5 的验收看**总量**，不是只看 client 一侧的数字。

### 6.5 Ring 分配与背压

逐字移植 `PersistentRing`（`Managers.cpp:657-727`、`RingAllocateSlow` `:1891-1970`、`RingOnPresent` `:1975-2016`）：单调 head/tail、2 的幂掩码、frame mark。分配失败升级：**扩容(翻倍) → 对最老未 retire 批次有界等待（默认 50ms，走 §6.2a 的 producerParked doorbell，不是自旋） → 硬 `Drain` 请求 + `ringGeneration` bump**。generation bump 上线，防止后续记录引用被回收的 offset；硬 drain 之后按 §5.6a 重发未 apply 的纹理记录。

`SEG_CMD` 与 `SEG_STAGE` 各自独立跑这套升级（各有自己的游标三元组）。

### 6.6 纹理

- **Unpack PBO 完全在 client 解析**（`GL_Texture.cpp:1719,1765,1887,1976,2457,2604,2722,4458,6176` 读 `pixelUnpackBufferObject->MappedData() + (SizeT)pixels`，再由 `ProcessTexturePixelsDataUnpack` 紧密重排）。**没有任何纹理像素以 PBO 引用形式过线，server 永远不需要 `GL_PIXEL_UNPACK_BUFFER` 状态。`PixelStoreBlob` 只用于 PACK 方向。**
- **压缩纹理永不到达任何 backend**（前端在 `glTexImage` 时把压缩 internalformat 解析成非压缩后备，`GL_Texture.cpp:298-306`；`grep -i compress MG_Backend/DirectGLES/*.cpp` 只命中一条注释）。逐字节 `m_compressedData` blob 仅供 `glGetCompressedTexImage`，纯 client 侧，不过线。
- **`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client（推翻上一版的 P4 项）。** 已确认这两个入口今天就是**纯前端操作**：`CopyTexSubImage{1,2,3}D_State`（`GL_Texture.cpp:3955,3979`）调 `CopyReadFramebufferIntoMipmapRegion`（`:1044-1097`），它借一次 backend `ReadPixels` 进 CPU scratch（`:1079`）、逐行 memcpy 进 mipmap shadow（`:1089-1094`）、`MarkStorageDirty(...,true)`（`:1095`）。拆分后它恰好是**一次阻塞 ReadPixels round trip**，产生的脏区按普通纹理 delta 下发——正确，且不需要任何新命令。上一版提议"整体移到 server + `EvTexWriteback`"是错的：那个事件在 §7.4 的列表里根本不存在（只有 `EvBufferWriteback`），它仍然要付一次 round trip（client shadow 必须为 `glGetTexImage` 保持最新），还多出一个 `GLFunctionsTable` 里没有对应项的命令。`glClearTexImage`（`GL_Texture.cpp:985-1006`）同形。
- **per-level `serverAuthoritative` 位**只保留给两处**字节确实在 backend 里写进 shadow** 的场景：生成 mip 的 CPU 路径（`DirectGLES.cpp:6270-6271,6861` 的 `AllocateStorage` + 直写 `MapMipmapData`）与 `MirrorCopyImageIntoDestinationShadow`（`:7144`，`glCopyImageSubData` 的目的地镜像）。client 在发射对应命令时对受影响 level 置位。`CopyTextureImageToClientOrPBO_State` 查它：**清 → 本地 shadow 回答，零 round trip**（应用自己上传的 level 全走这条）；**置 → 一次 round trip**。

### 6.7 回读

| 路径 | monolith | 拆分后 |
|---|---|---|
| `glReadPixels` → 客户内存 | 阻塞 | 一次 round trip，像素放 `SEG_REPLY` slot；per-row 循环留在 server 内 |
| `glReadPixels` → pack PBO | **也阻塞**（`DirectGLES.cpp:9189-9205` 把整个 PBO map 回来写 shadow） | **fire-and-forget** + client 侧对该 PBO 置 `MarkGpuWritten`（§5.6b），代价推迟到之后的 map/read。**严格优于 monolith** |
| `glGetTexImage`/`glGetTextureImage` | DirectGLES 从 client shadow 回答 | DirectGLES **零 round trip**（除 `serverAuthoritative` level）；DirectVulkan 一次 |
| `glGetBufferSubData` / `glMapBuffer(READ)` on gpuWritePending | 阻塞（`glFinish()`，`Managers.cpp:1246`） | 一次，由 client 侧 pending 集合触发（§5.6b），被 `EvGpuWritten{ranges}` 收窄 |
| XFB capture writeback | `glEndTransformFeedback` 里无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`） | **不等**，client 对 capture target 置 `MarkGpuWritten`，首次读时付；`FixupGsStripCaptureOrder` 移到 server |
| `glCopyTexSubImage*` | 内含一次同步 ReadPixels | 一次 round trip（保持前端实现不变） |

### 6.8 persistent map 与 ≥16MiB 采纳

三档，由**运行时 POST 探针**选择（遵循本项目"后端限制一律探针判定、绝不硬编码驱动名"的既定规则）：

- **T2 — 拒绝（P1-6 默认，永久正确回退）**：`AcquirePersistentMap` 返回 `nullptr`。**此档下 §5.10 的 client 侧推送是强制的**，否则应用的 coherent persistent 写会丢。
- **T1 — server 导出自己的映射（P7 主攻）**：server 照常铸造 coherent map（`Managers.cpp:988-1058` / `VkBufferManager.cpp:515-563`），经 `VK_KHR_external_memory_fd` / `AHardwareBuffer_sendHandleToUnixSocket`（API 26，`hardware_buffer.h:521`）/ `VK_KHR_external_memory_win32` / `GL_EXT_memory_object_fd` 导出，client `mmap` 后调 `PipeResource::AdoptPersistentMap(base)`。**每 store 生命周期一次 round trip。** 采纳成功后 §5.10 的推送对该 buffer 自动停止（`SyncPersistentMappedRange` 的 `IsGpuResident()` 早退），与 monolith 一致。
- **T0 — server 导入 client 分配**：client 分配 `AHardwareBuffer`/dma-buf，server 以 `GL_EXT_external_buffer`+`glBufferStorageExternalEXT` 或 `VK_EXT_external_memory_host` 导入。理想但可用性未知。

**`MOBILEGL_COHERENT_AS_FLUSH` 在拆分模式下照常生效**（推翻上一版的禁令，理由见 §5.10 结尾）：有了 client 侧推送，被改写出来的 coherent map 与应用原生请求的 coherent map 走同一条正确路径，两个 Create/Flywheel fixture 才能在 split 与 monolith 下做同路径对比。

### 6.9 program artifacts

- **P1-4**：`RecProgramLinkOp{handle, shaderSources[], bindAttribLocations[], bindFragDataLocations[], xfbVaryings[], xfbMode, separable, reflectionDigest}` — server 重新 link。只需 5 个 schema 字段，**且分歧不可能静默**（两半跑同一二进制里的同一段代码）。源码可得：`ProgramObject::GetLinkedShaderSnapshot()`（`ProgramObject.h:157`）刻意持有 linked shader 的 `SharedPtr`（注释在 `:1716`），所以 `glDeleteShader` 之后源码仍在。
- **`reflectionDigest` 必须覆盖 backend 实际读的全集**：xxHash over
  `(uniformName, location, type, typeFacts, samplerOrImageUnitIndex)` 全表 + `maxUniformLocation` + `(blockName, blockBinding, blockSize)` 全表 + `shaderStorageBlockBindingOverrides` + `PointSizeDemoted` + `GetLinkedShaderStages` + `xfbVaryings/xfbStrides/xfbPackedStride/xfbBufferMode` + **`GetGeneratedSpirv()` 各 module 的 xxHash**。不匹配 → `Fatal{ReflectionDivergence}`。
  （理由：本项目自己的二分历史记录过"glslang 反射/生成顺序是真载重，桌面字节一致是语料受限的假绿"。）
- **P5**：`RecProgramPublish{handle, stages[], spirvBlobs[], reflectionBlobRef}`，reflection 用 **`Visit()` 式归档**：
```cpp
// MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsArchive.h
template <class Ar> void Visit(Ar& ar, LinkArtifacts& a) { ar(a.writtenUniformLocationBits, /*…全字段…*/); }
static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
              "新字段请加进 Visit() 并 bump MGL_LINKARTIFACTS_SIZE");
```
  一份字段表服务两个方向 + `sizeof` 绊线。**序列化整个结构体**（而非 backend 当前读的 ~40 字段），这样 backend 新增一次 read 永不需要改协议。
  安装入口：`ProgramObject::InstallPublishedLink(LinkArtifacts&&, SpirvArtifacts&&, linkVersion, imageUnitVersion, backendStateVersion)`，绕过 `m_pendingLink`/`m_pendingSpirv`，**server 因此不需要 compile pool**。
- `relink` 路径保留为常驻 oracle 与 A/B 对照（`MOBILEGL_IPC_PROGRAM=publish|relink`）。
- 全局 UBO scratch 相反：小、每次 `glUniform*` 变、有版本 → 走 `SEG_STAGE`，键 `(programHandle, uboContentVersion)`，复现 monolith 的"每 program 每帧至多一次"（`DirectGLES.cpp:3369-3392`）。

### 6.10 应用指针（四类，范围全部可算）

| 类 | 范围 | 站点 |
|---|---|---|
| client 顶点数组（仅 DrawArrays 族） | `(first+count-1)*stride + elementSize` | `Managers.cpp:2560`、`VulkanRenderer.cpp:3737` |
| client 索引数组 | `count * indexSize` | `DirectGLES.cpp:4436`、`VulkanRenderer.cpp:4081` |
| client indirect / parameter 块 | `stride*(drawcount-1)+cmdSize` | `DirectGLES.cpp:276`、`DirectVulkan.cpp:303` |
| `MultiDraw*` 参数数组、`ClearBuffer*` value | `drawcount*4`、16B | `DirectVulkan.cpp:963-1057` |

唯一无界的是**索引 draw 下的 client 顶点数组**：索引扫描（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3406-3470`）必须在 **client** 侧跑，只有 client 同时持有两个数组。实现于 `MG_Remote/Client/ClientArrayBounds.cpp`，两个 backend 共用。

**陈旧索引危害（本轮新增）**：monolith 在每次这类扫描之前都调 `indexBuffer->SyncGpuWrites()`（`DirectGLES.cpp:4413`、`MultiDraw.cpp:499`、`VulkanRenderer.cpp:3431,4159`），因为 EBO 可能刚被 compute shader 或 XFB 写过。client 侧扫的是 client shadow，若不做同样的强制回读，算出的 `maxIndex` 来自陈旧字节，顶点数组会被少拷 → 几何缺失/花屏，或越界读应用数组。同样的暴露面还有 primitive-restart 重写（`DirectGLES.cpp:4412-4414`）与 `*IndirectCount` 的 parameter buffer 读（`DirectGLES.cpp:4666-4693,4768-4793`）。

**规则**：`ClientArrayBounds`、restart 重写、indirect-count 读者在触碰 shadow 之前，必须走 §5.6b 的 pending 检查（Publish + 等 `appliedSeq` + 排空事件），即 monolith 里 `SyncGpuWrites()` 所在的**同一个位置**。P2 增加 `ClientArrayAfterComputeWriteScenario` 作为门。

draw 记录里 `indicesAreClient` 由"是否绑定了 element array buffer"决定（`DirectGLES.cpp:4423` vs `:4425-4442`），在 binding 所在的一侧判定。

---

## 7. 控制面

### 7.1 FlatBuffers 用法

**一份 schema `MobileGL/MG_Remote/Protocol/protocol.fbs`，两种用法：**
- **热路径 → FlatBuffers `struct`**（flatc 保证定长布局、无 vtable、无偏移间接、无需 verifier walk，只需边界检查），直接放进 ring：`[RecHeader | struct | 可选变长尾]`。`DrawArrays` = 8+24 = 32B（对比 table-per-command 的 ~60B 与一次 vtable 遍历）。这正是 `Feat/CS-Delta-IPC` 自己的 plan 第 55 行要求而实现没做的事。
- **罕见/变长/需演进 → FlatBuffers `table`**，走 CTRL socket。

```fbs
namespace MobileGL.Wire;

// ---------- 热路径 struct（进 ring）----------
struct WireHandle  { kind:ubyte; p0:ubyte; p1:ubyte; p2:ubyte; glName:uint; lifetimeId:ulong; }
struct BlobRef     { seg:uint; pad:uint; offset:ulong; size:ulong; }
struct RecBindBuffer     { target:uint; index:uint; h:WireHandle; }
struct RecDrawArrays     { mode:uint; first:int; count:int; instances:int; baseInstance:uint; pad:uint; }
struct RecDrawElements   { mode:uint; count:int; type:uint; flags:uint; indices:ulong; blob:BlobRef; }
struct RecBufferSubData  { h:WireHandle; offset:ulong; size:ulong; blob:BlobRef; }
struct RecBufferMap      { h:WireHandle; rangeStart:ulong; rangeEnd:ulong; access:uint; pad:uint; }
struct RecBufferUnmap    { h:WireHandle; }
struct RecTexSubImage    { h:WireHandle; target:uint; level:uint; box:[uint:6]; rectCount:uint;
                           pad:uint; blob:BlobRef; }        // rects 在变长尾
struct RecGenerateMipmapLevels { h:WireHandle; target:uint; requiredLevelCount:uint;
                                 bytesPerTexel:uint; shrinkingAxes:uint; }
struct RecRenderbufferStorage  { h:WireHandle; internalFormat:uint; width:int; height:int;
                                 samples:int; pad:uint; }
struct RecXfbAccounting  { pausedPrims:ulong; inputPrims:ulong; prims:ulong;
                           capturedVerts:ulong; geomDraws:uint; accountedDraws:uint; }
struct RecRenderStateBlob{ version:ushort; pipelineVersion:ushort; pad:uint; blob:BlobRef; }
struct RecPresent        { frameSerial:ulong; swapInterval:int; pad:uint; }
struct RecSetResolvedDrawProgram { h:WireHandle; }
// … 共约 95 个

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
                     tableSlotMask:ulong;              // 远端实际注册了哪些 GLFunctionsTable 槽
                     prefersCpuXfbPrimitiveAccounting:bool; }
table DefaultFramebufferInfo { width:int; height:int; colorFormat:uint; depthFormat:uint; stencilFormat:uint; }
table SurfaceOp    { seq:ulong; kind:ubyte; display:ulong; surface:ulong; windowKind:ubyte;
                     nativeToken:ulong; width:int; height:int; swapInterval:int; }
table SurfaceReply { seq:ulong; ok:bool; eglMajor:int; eglMinor:int; defaultFb:DefaultFramebufferInfo; }
table ProgramReflection { /* Visit() 归档的结构化镜像，P5 */ }
table ResyncRequest { serverEpoch:uint; }  table ResyncDone {}
table AuxRequest   { seq:ulong; kind:ubyte; payload:[ubyte]; }   // 外来线程 sync/query
table Fatal   { code:uint; message:string; }
table LogLine { level:ubyte; text:string; }
union CtrlMsg { Hello, Welcome, CapsSnapshot, SurfaceOp, SurfaceReply,
                ProgramReflection, ResyncRequest, ResyncDone, AuxRequest, Fatal, LogLine }
table CtrlEnvelope { msg:CtrlMsg; }
root_type CtrlEnvelope;
```

`protocol_generated.h` **提交进仓库**，由 `scripts/gen_protocol.py` 重新生成（镜像 `tools/trace_replay/CMakeLists.txt:52-69` 驱动 `glproc.py` 的做法）；CI 加 `flatc-check` 步骤重新生成并 `git diff --exit-code`。

**codegen 绝不进默认构建图（本轮加强）**：`Feat/CS-Delta-IPC:MobileGL/Protocol/CMakeLists.txt:22-38` 在 `MOBILEGL_FLATC_EXECUTABLE` 未设时 `add_subdirectory(3rdparty/flatbuffers)` 并开 `FLATBUFFERS_BUILD_FLATC ON`——这正是它自称要修的 NDK 陷阱（交叉编译造出 arm64 `flatc` 然后在 host 上执行）。**本计划不复用这一段**：`gen_protocol.py` 是纯开发者/CI 目标，默认构建图里没有 `flatc`，`MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`。FlatBuffers 运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上（P4 用 `nm` 复核 `libMobileGL.so` 链接行没有新增库，不靠断言）。

### 7.2 帧封装与 flush 策略

CTRL socket 封帧：`[u32 'MGLF'][u32 len][payload]`，64MiB 上限，**读时校验**（`Feat/CS-Delta-IPC` 的 `Feed()` 永远返回 OK，坏 magic 变成静默永久挂起，`Framing.h:41-45`；`StartRead` 直接按 wire 长度分配无上限检查，`LocalSocketTransport.cpp:232-236`）。接收缓冲不足时**返回所需大小并保留消息**（上一版的 transport 会失败且不弹出消息，把流永久卡死）。

#### Publish 触发器（重写，删掉 64KiB 阈值）

上一版设 "records ≥ 64KiB" 为主触发器。按 §6.3 的记录尺寸，64KiB ≈ 1200-2700 条记录，即**一整帧**（计划自己把 MC 帧估为 1000-4000 draw）。那意味着 server 在 client 发完整帧之前无法开始工作——这不是异步，是一个整帧的流水线气泡，且在 present credit 之上再加一整帧延迟。它还在 P2.5 跑之前就先把 P2.5 的假设否掉了（inproc 的全部意义就是让 `PrepareForDraw` 与 GL 线程重叠，帧粒度 publish 保证零重叠）。而 `SEG_CMD` 是 SPSC ring，"publish" 只是一次 `cmdHead` 的 release store，唯一值得摊销的是门铃写。

**新规则**：
- **每条记录（或每 8-16 条，用来摊销 store）release-store `cmdHead`**；仅当 `consumerParked` 时敲门铃。
- 显式门铃点：`Present`、任何 `kNeedsAck` 阻塞请求、`eglMakeCurrent`、`glFlush`（**刷出 outbox，不等待**）。
- **`SEG_STAGE` 余量 < 1/4** 时敲门铃（用 `stageHead - stageAppliedTail`）。
- **轮询类入口点也是门铃点（本轮新增，修 livelock）**：`glClientWaitSync`（任意 timeout）、`glGetSynciv(GL_SYNC_STATUS)`、`glGetQueryObject*(GL_QUERY_RESULT_AVAILABLE | GL_QUERY_RESULT_NO_WAIT)`。
  理由：GL 的标准惯用法是 `glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 与 `while (!avail) glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &avail);`。循环里没有别的 GL 调用，若这些入口不 publish，`RecFenceSync` 就永远躺在 ring 里，server 看不到，watermark 不动，循环永久自旋——这是挂死，不是变慢。仓库自己在意这件事：`DirectVulkan.cpp:1158-1160` 写明 "GL_SYNC_FLUSH_COMMANDS_BIT: flush regardless of timeout, so a zero-timeout poll loop makes progress across calls"，而 MG_Impl 无条件把 flags 透传给 backend（`GL_Sync.cpp:96`）。
  **携带 `GL_SYNC_FLUSH_COMMANDS_BIT` 的调用无条件 publish**（spec 要求 flush）。
- **饥饿升级**：同一个 handle 连续 N 次（默认 64，`MOBILEGL_IPC_POLL_ESCALATE`）本地回答 `TIMEOUT_EXPIRED` / "未就绪" 而 watermark 毫无移动时，升级成一次阻塞 round trip，这样一个已经卡住的 server 不会把 client 自旋成死循环。

**`glFinish` 保持纯 no-op**（`Definitions.cpp:111-112`）——应用唯一的强制停顿手段在 monolith 里免费，拆分后也必须免费。

### 7.3 序号与 credit

seq = 记录序数。**两个互相独立的窗口，绝不是 per-batch 锁步**（`Feat/CS-Delta-IPC` 在 apply 循环里同步发 ack，`ServerCore.cpp:421-429`，是最差的节奏；而且它的 credit 算成 `baseSeq + items.size()`，只有 `baseSeq==0` 时才对）：

- **字节 credit**：`SEG_CMD` 与 `SEG_STAGE` 各自的占用，升级路径见 §6.5。
- **Present credit**：`eglSwapBuffers` 在 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（**默认 1**，见 §9）时阻塞。

server 端**不发 credit 消息**：它对 `RingControl` 做 release store，consumer 每 64 条记录更新一次 `appliedSeq`，并在 `producerParked` 时敲反向门铃。

### 7.4 事件回传通道

`SEG_EVENT` 是 server→client 的 SPSC POD ring：`EvQueryResult{handle, available, value}`、`EvFenceSignaled{handle}`、`EvGpuWritten{handle, rangeCount, ranges[]}`、`EvBufferWriteback{handle, offset, BlobRef}`、`EvReadbackDone{seq, BlobRef}`、`EvGlError{code}`、`EvDefaultFramebufferInfo`、`EvCompileEnvInvalidate`、`EvLogLine{level,len,text}`。

#### 排空点（补齐）

client 在下列位置排空：`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`glGetSynciv`、`eglSwapBuffers`、**`glMapBuffer` / `glMapBufferRange` / `glGetBufferSubData` / `glGetNamedBufferSubData` / `glCopyBufferSubData`**（§5.6b 要求），以及**每一次等待循环的每一轮**（present credit、`kNeedsAck`、ring/stage 满）。最后一条是必须的，见下。

#### 溢出策略（本轮新增，修一个双向死锁）

上一版没说 `SEG_EVENT` 满了怎么办，也没要求 client 在**等待中**排空。具体死锁：client 卡在 `eglSwapBuffers` 等 present credit；server 的 apply 线程一边 apply 一边产 `EvLogLine` 与 `EvGpuWritten`；`SEG_EVENT` 满；apply 线程阻塞在生产上；`presentAckSerial` 永不前进；client 永不离开 `eglSwapBuffers`，因而永不排空。两边都死。

**策略**：
1. client **必须**在每个等待循环内排空 `SEG_EVENT`，不只是在入口点边界。
2. `EvLogLine` 是**有损**的：覆盖最旧，并累加 `RingControl.eventDropped`（client 在排空时把丢失条数打进日志）。丢一条日志绝不允许卡住渲染。
3. 语义承载事件（`EvGpuWritten`、`EvReadbackDone`、`EvFenceSignaled`、`EvBufferWriteback`、`EvGlError`、`EvDefaultFramebufferInfo`、`EvCompileEnvInvalidate`）**无损**：ring 装不下时 server 置 `RingControl.eventRingFull=1` 并**停止 apply**（停在一条记录的边界上，不是记录中间），敲反向门铃；client 排空后清标志并敲正向门铃。状态因此永远可恢复。
4. 故障注入测试：在 client 被 credit 阻塞时灌满 `SEG_EVENT`，与 P8 的 SIGKILL 测试并列。

server 侧的 `MGLOG` 与延迟诊断按流顺序 replay 进 client 日志流——复用已存在的 `DeferredLogLine`/`ApplyDeferredDiagnostics` 机制（`JobNode.h:26-58,149-158`）。

---

## 8. Roundtrip 清单

### 不可避免的阻塞点

| # | 站点 | 频率 | 为什么 |
|---|---|---|---|
| 1 | 握手 `Hello`/`Welcome` + 段 fd 传递 | 一次 | — |
| 2 | `InitializeEGLDisplay`（写 `major`/`minor`） | 一次 | 出参 |
| 3 | `CreateEGL{Window,Pbuffer}Surface` / `Resize` | 罕见 | 返回 `Bool`；回复顺带 `DefaultFramebufferInfo` |
| 4 | 首次 `MakeEGLCurrent` + `InitCapabilities` → `CapsSnapshot` | 每 surface 一次 | caps 只在那一刻才存在（`BackendObject.cpp:341-347`） |
| 5 | `glReadPixels` → 客户内存 | 罕见（CTS 热） | GL 要求返回时字节已就位 |
| 6 | `glCopyTexSubImage*` / `glClearTexImage`（内含 ReadPixels） | 罕见 | 前端实现本来就借一次 ReadPixels |
| 7 | `glGetTexImage`/`glGetTextureImage`（DirectVulkan；DirectGLES 仅 `serverAuthoritative` level） | 罕见 | — |
| 8 | `glGetBufferSubData` / `glMapBuffer(READ)` on client-pending | 罕见 | monolith 里本来就阻塞；client pending 集合触发 |
| 9 | client 顶点数组的索引扫描 / restart 重写 / indirect-count 读（当 EBO 在 pending 集合里） | 罕见 | monolith 在同一位置调 `SyncGpuWrites()` |
| 10 | `glClientWaitSync(timeout>0)` 超出 watermark | 每帧级 | 应用请求的等待 |
| 11 | `glGetQueryObject*(GL_QUERY_RESULT)` 未完成；`glBeginConditionalRender` | 罕见 | `GL_Query.cpp:300`、`:705-706`（后者注释明说"by WAITING even for the _NO_WAIT modes"） |
| 12 | 轮询饥饿升级（连续 N 次无进展） | 极罕见 | 防死锁保险 |
| 13 | **分配类入口点的错误 ack**（`glRenderbufferStorage*`、部分 `glTexImage*`/`glTexStorage*`/`glCopyTexImage*`、`glBufferStorage`） | 罕见 | OOM 探测惯用法（§5.6c） |
| 14 | `AcquirePersistentMap`（仅 P7 T1） | 每 store 一次 | 返回映射 |
| 15 | ring/stage 耗尽、present credit | 节奏 | 非语义 |

### 变成异步或本地的

- 全部 20 个 draw、9 个 clear、blit/copy、`GenerateMipmap`、dispatch、barrier、image bind、7 个 XFB 跨度标记、`PatchParameteri`、`ShaderStorageBlockBinding`（权威状态已在 client，`GL_Program.cpp:3391`）、所有 buffer/texture/program/VAO/FBO delta、`Present`。
- **`glGetError` 永远本地**（`GL_Getter.cpp:2811-2817`；`Core.cpp:48-49` 的不变式）。
- **`glFinish`/`glFlush` 保持免费**。
- **89 个 caps 站点全部本地**（45 `GetDynamicParameters` + 8 `GetRendererInfo` + 4 `GetFormatCapabilities` + 3 `GetBackendType` + `IsTimerQuerySupported` + `PrefersCpuXfbPrimitiveAccounting` + `BeginOcclusionQuery!=nullptr`）。
- **`glGetIntegeri_v` 全部本地**；`glDispatchCompute` 的三次 per-dispatch 校验查询（`GL_Drawing.cpp:719`）改读 `CompileEnv::maxComputeWorkGroupCount`（`CompileEnv.h:52-54`）。
- **`GetInteger64i_v`、`GetProgramiv` 删除**。
- **`FenceSync`、`Begin{TimeElapsed,Occlusion,XfbPrimitives}Query`、`QueryCounterTimestamp` → client 铸造 handle**，fire-and-forget（前端本来就铸造应用可见的名字：`GL_Sync.cpp:61`、`GL_Query.cpp:54`）。
- **`GetSyncStatus`、`ClientWaitSync(0)`、`IsQueryResultAvailable`、`GetQueryResult64(wait=false)` → 先 publish（§7.2），再从水位一次 acquire load 回答**。miss 返回 `GL_UNSIGNALED` / "未就绪"，两处契约明确允许（`BackendObject.h:210-214`、`:236-241`；`GL_Query.cpp:302-311` 已遵守：读 0、**不缓存**、保留 backend handle）。
- **`glReadPixels` 进 PBO → fire-and-forget**（配 client 侧 `MarkGpuWritten`），比 monolith 更好。
- **`glEndTransformFeedback` 的无限 fence 等待取消**（配 client 侧对 capture target 置 `MarkGpuWritten`）。

**稳态帧：零 round trip**（对不使用 conditional render / 阻塞式 query / 分配类调用的帧而言；见 §15 P3 的门措辞修正）。

### fence 完成度必须来自真 fence，不是 present 水位（本轮新增）

上一版让 `retiredSeq`/`completedFrameSerial` 兜底 fence 语义。但在 DirectGLES 上这两个水位**只在 `Present()` 里前进**（`DirectGLES.cpp:10626-10643` 在 `eglSwapBuffers` 之后轮询 4 深 fence ring），或在 `WaitForFrameSerialCompleted`（`:10583-10607`）里。帧中创建的 fence 于是要等到**下一次 present 退休**才报 signalled，即 fence 完成度退化成帧计数推断。`DirectVulkan.cpp:1120-1128` 恰恰写明这是被修掉的 bug：完成度必须"track the GPU itself rather than the frame-count inference; MC 1.21.5's fence-paced ring buffers depend on this to recycle their space instead of growing without bound"，而项目记忆 `magma-mc1215-fence-oom` 记录了它曾导致 native-heap OOM kill。

**规则**：`RecFenceSync` 在 server 侧转成一次**真实的 backend `FenceSync()`**；server 用自己已有的逐 fence 轮询（DirectGLES 有 `WaitForFrameSerialCompleted` 的 fence 选择逻辑 `:10586-10600` 可复用；DirectVulkan 有 `IsSubmitIndexComplete`）在**非 present 时刻**也推进，并发 `EvFenceSignaled{handle}`。client 的本地快路径读的是"由真实逐 fence 退休导出的 handle 水位"，不是 present 水位。

### 三个应先独立落到 `dev` 的 monolith 修复（可二分、monolith 自身受益）
1. `glEndTransformFeedback` 的无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`）→ 用既有 `MarkGpuWritten`/`SyncGpuWrites` 推迟到首次读。
2. `glDispatchCompute` 三次 `GetIntegeri_v` → `CompileEnv`。
3. 删除 `GetInteger64i_v`/`GetProgramiv` 两个死表项及两个 backend 的实现。

---

## 9. Present 与帧节奏

`eglSwapBuffers` → `EGLImpl::SwapBuffers`（`EGLImpl.cpp:162-183`）→ `BackendObject::SwapEGLBuffers`（`BackendObject.cpp:369-398`，其线程归属校验全部对 client 镜像的 EGL 状态求值，**不需要回复**）→ 发 `RecPresent{frameSerial, swapInterval}` → publish + 敲门铃 → 返回，除非 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`。

**`Present` 与应用的 `eglSwapBuffers` 严格 1:1，绝不批量。** Magma 侧四次 `OnFrameBoundary()` 缓存老化、`TryDrainFrameTransients` 和全部四次 `BeginFrame` 只在 `Present` 内发生（`VulkanRenderer.cpp:12765-12904`）；Espryt 侧三个 ring 与 `TrimBufferPool` 在那里 retire（`DirectGLES.cpp:10646-10649`）。批量会饿死这些排空。

### 9.1 延迟是叠加的：credit 默认改为 1

上一版设 credit=2 并论证它"镜像系统已有预算"，因此"不引入新的停顿类别"。停顿**类别**确实不新，但**延迟会叠加**，而上一版没有把它加起来：

- server 自己的 `Present` 在返回之前就已经等了 2-3 帧：`VulkanRenderer::Present` 末尾调 `FrameContext::WaitAndAcquireNextImage`，其第一条语句是 `vkWaitForFences(device, 1, &frame.imageInFlightFence, VK_TRUE, timeout)`（`FrameContext.cpp:288-290`）。`presentAckSerial` 因此只能在那次等待完成后才前进。
- 一个被允许领先 2 个 present 的 client，叠在一个自身已领先 GPU 2-3 帧的 server 上 = **端到端 4-5 帧**，60Hz 下 66-83ms，对第一人称游戏不可接受。
- 现有的验收门都看不见它：SSIM 是帧内容比较，`bench.sh` 量的是 FPS，都不是 input-to-photon。

**规则**：`MOBILEGL_IPC_PRESENT_CREDIT` **默认 1**（可配 1-4）。文档里写明叠加公式：`端到端 ≈ client credit + server FIF + 驱动深度`。P3 与 P9 的验收增加**输入延迟测量**：用已有的 `GetGpuTimestampNs` 与 trace-replay `--benchmark` 的逐帧 JSON 构建 "记录发射时刻 → present 完成时刻" 直方图；只有当实测吞吐收益能抵掉实测延迟代价时才调高 credit。

参考基线：`MagmaFramesInFlight = 3` 钳到 `[2, maxImageCount]`（`VulkanRendererConfig.h:14-19`、`VulkanRenderer.cpp:3051-3058`），Espryt 深度 4 的 fence ring 刻意高于驱动的 2-3（`DirectGLES.cpp:10071-10074`）。

### 9.2 swap interval 与 Magma

Swap interval 搭 `RecPresent` 过去。注意 Magma 从不注册 `SetSwapInterval`（`BackendObject_DirectVulkan.cpp:698` 只注册 `Present`）且偏好 `MAILBOX`/`IMMEDIATE`（`SwapchainObject.h:74-79`），因此 **IPC credit 成为 Magma 唯一的显式限帧器** —— 记录在案，P6/P9 在设备上测量输入延迟与帧节奏；若 Magma 需要，把"注册 `SetSwapInterval` 并映射到 FIFO"作为**独立的 `dev` 变更**，不让两套机制同时管节奏。

### 9.3 无 present 循环下的水位饥饿

`retiredTail` 的回收依赖 server 发布准确的 `completedFrameSerial`。DirectVulkan 有 `TryDrainFrameTransients`/`RefreshCompletedSubmits` 可以在非 present 时刻推进，**DirectGLES 没有对应物**：`g_completedFrameSerial` 只在 `Present()` 里（`DirectGLES.cpp:10626-10643`）和 `WaitForFrameSerialCompleted`（`:10583-10607`，且要求存在覆盖目标 serial 的活 fence，slot 被回收时返回 false）前进。在无 present 的负载里——`tools/cts` 的 `run_cts_local.py`、回读循环、从不 swap 的 `MG_IntegrationTest` 场景——一个 fence 都不会被插入，`retiredTail` 永不前进，`SEG_STAGE` 填满，§6.5 的升级路径在每个用例上都跑到硬 drain。那会把一次 CTS run 变成一连串 50ms 等待加整体 drain，并可能被误读成一致性回归。

**规则**：给 DirectGLES 的 server 加**非 present fence tick**——距上次 `Present` 超过阈值（默认 8ms）或每 N 条已 apply 记录（默认 4096）时，插入一个 `glFenceSync` 并轮询 fence ring，复用 `g_frameFenceRing` 机制。同时把 ring 占用率与升级次数打进 Tracy 计数器（P0 交付），让"水位饿死"表现为一个指标而不是一次无法解释的停顿。P2 增加一个无 present 的 split 用例。

---

## 10. 线程模型

### Client
- **v1 不加线程。** 编码在调用方 GL 线程上直接写进 ring。前端本来就是 per-context 单线程契约（`GLContext` 无 mutex；`EGLState::MakeCurrent` 强制一个 owner 线程，`EGLState/Core.cpp:1215-1220`，测试在 `MG_Test/EGLState/EGLStateTest.cpp:39-92`）。
- **flow = per context，不是 per thread。** 今天恰好一个 flow。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射。**顺手修既有漏洞**：`EGLImpl::ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）今天不取该锁而另外三个（`MakeCurrent`/`SwapBuffers`/`DestroySurface`）取。
- **外来线程的 sync/query**：读全部从 `RingControl` 无锁 acquire load 回答（比取 registry mutex 更好）；少数必须发射的（`FenceSync`、`Begin*Query`，以及 §7.2 要求的轮询 publish）取 `ctrlMutex` 并走 CTRL socket 的 out-of-band `AuxRequest` 帧（SPSC ring 不允许第二个 producer）。
- **等待必须能挂起**：所有 client 侧等待（present credit、`kNeedsAck`、ring/stage 满、轮询升级）走 §6.2a 的 `producerParked` + 反向门铃，自旋窗口 `MOBILEGL_IPC_SPIN_US`（默认 50µs）。
- ShaderCompilePool 原样保留在 client（`ShaderCompilePool.h:77-82`，≤4 worker，为 RSS 上限）。
- 可选 `mgl-client-tx` 双缓冲发送线程：**P6 项，凭测量决定**。在 P6 的 Tracy 数据出来之前不要预先加线程（会引入拷贝或锁）。

### Server
| 线程 | 职责 |
|---|---|
| `mgl-srv-io` | asio `io_context::run`：封帧读写、`SCM_RIGHTS`、双向 doorbell、CTRL RPC |
| `mgl-srv-apply` | **终身持有原生 EGL/Vulkan context**：消费 ring → 解码 → apply 进 replica → 调 backend 表 |
| `mgl-srv-dec`（可选，P6） | FlatBuffers/边界校验前置，凭测量决定 |

因为 context 永不迁移：`g_backendContextOwnerThread`（`DirectGLES.cpp:10052`）只写一次；`DirectGLES::MakeCurrent` 的 8 缓存失效风暴（`:10123-10140`）变成启动期一次性成本；`IsBackendContextCurrentOnThisThread` 的每帧 EGL 复核（`:10195-10228`，动机是 `eglGetCurrentContext` 实测占渲染线程 16%）恒真。DirectGLES 的 off-thread 降级（`FenceSync` 返回 null 等）消失——**保真度提升**。延迟 replay 机制（`Managers.h:458-473` 的 `pendingRespecify`/`pendingRanges`/`pendingResidentWrites`）保留但永不触发。

### 核心放置（本轮新增，是性能主张的前提）

§5.1 明说 reconciler "就是 `PrepareForDraw` 的可达性遍历"。这意味着这套遍历**每 draw 跑两次**：client 的 `WireMirror` 一次，server 未改动的 `PrepareForDraw`（`DirectGLES.cpp:2916-2975`）一次，外加编码与解码。其中有些并不便宜：`CurrentUnitBindingsEpoch`（`DirectGLES.cpp:1421-1438`）在 `GetTextureBindGeneration()` 变动时会退化成对每个 touched texture unit 做 owner-equality 全走查，而代码自己注明这在冗余重绑时就会发生（"26.2 re-binds the unit's own sampler around every texture-unit switch"）。

所以拆分的全部性能主张都押在"两半落在两个都快的核上"。而 MobileGL 全库从不设置亲和性（`grep -rn 'sched_setaffinity\|cpu_set_t\|affinity' MobileGL/` 零命中），server 是 fork/exec 出来的独立进程、不继承 launcher 的亲和性，项目记忆 `pojav-bigcore-affinity-trap` 又记录过 `pojavBigCore=true` 把整个游戏 JVM 加 MobileGL worker 钉死单核、让一整批历史测量作废。若 `mgl-srv-apply` 落到 1.55GHz 小核，它做的工作严格多于 monolith 在 1.96GHz 大核上做的，拆分按构造就是回归，而 §15 P3 的"帧时在 monolith 10% 内"会以一个没人会正确归因的理由失败。

**规则**：
1. 计划里必须写出**总 CPU 工作量差**（client reconcile + encode + decode + server `PrepareForDraw`  vs  monolith 的 `PrepareForDraw`），不只是单侧成本。
2. 复用 `ShaderCompilePool` 已有的大核探测（`ShaderCompilePool.cpp:73-96` 的 `ReadCpuMaxFrequencyKHz` / `DetectBigCoreCount`）把 `mgl-srv-apply` 绑到大核，开关 `MOBILEGL_IPC_SERVER_AFFINITY`（默认 auto），并把解析出的 mask 打进日志。
3. P2.5 与 P3 必须报**逐线程 CPU 时间**，不只是墙钟帧时，这样"没有收益"的结论能被归因到放置 vs 编码成本。

### 拆机顺序（三条约束）
`Publish()` + server 排空并 ack → 停 apply 线程 → 关 transport →（client）排空 compile pool（必须先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构，`ShaderCompilePool.h:106-110`、`Init.cpp:56-62`）→ `MobileGL::Destroy()`（`EGLImpl.cpp:335-338`）→ 释放 sync/query handle（`GL_Sync.cpp:223-226`）。

---

## 11. EGL/窗口与进程生命周期

### 11.1 启动与握手

client 定位 server 的顺序（**本轮修正**）：
1. `MOBILEGL_IPC_SERVER_PATH`（**主要机制**）。
2. `dladdr(&MobileGL::Initialize)` → dirname → `libMobileGLServer.so`（**兜底**）。

上一版把 `dladdr` 当主要机制，但两个桌面验收门都因此找不到 server：`MG_IntegrationTest/CMakeLists.txt:28-35` 在非 Android 上把 `MGL_ITEST_MOBILEGL_TARGET` 设成 `MobileGL_s`（**静态链接**），`dladdr` 解析到测试可执行文件自身的路径而不是库目录；trace replay 则由 `tools/trace_replay/CMakeLists.txt:285-290` 显式传 `-DMOBILEGL_LIBRARY=$<TARGET_FILE:MobileGL>`，其目录是 MobileGL 的构建输出目录，而 CMake 默认把 `add_executable` 放在定义它的目录的 binary dir。

**配套**：把 `MobileGLServer` 的 `RUNTIME_OUTPUT_DIRECTORY` 设成 `$<TARGET_FILE_DIR:MobileGL>`，并把 `"MOBILEGL_IPC_SERVER_PATH=$<TARGET_FILE:MobileGLServer>"` 加进每一条新的 ctest `ENVIRONMENT`（经 `mgl_itest_join_environment` 与 `${MGL_ITEST_COMMON_ENV}` 合并）以及 `add_trace_replay_test` 的 `SPLIT` 分支。**并复核绝对路径能否活过 CI 的 artifact 搬运**：`.github/workflows/test.yml:174-185` 只重写 `CTestTestfile.cmake` 里的 `cmake` 路径，不重写 `ENVIRONMENT` 值——若不行，改为在测试启动时由 harness 相对 `argv[0]` 解析。

启动方式：`socketpair(AF_UNIX, SOCK_STREAM)` + `fork`/`execve`，fd 3 = socket（Windows 见 §11.5）。**无文件系统 socket 路径、无 abstract namespace、Android 上无 SELinux 争议。**

**子进程必须被强制成 monolith（本轮新增，修无界 fork 链）**：`MG_Config::Transport` 由 `ConfigLoader` 从环境变量读（与 `features.CoherentAsFlush = QueryEnvFlag(...)`（`ConfigLoader.cpp:185`）同形），而 `fork`/`execve` 的子进程会继承 `MOBILEGL_TRANSPORT=spawn`。server stub 里 `dlopen(libMobileGL.so)` + `dlsym("mobilegl_server_main")` 之后必然要起一个真 backend，即走 `MG_Backend::Init()`（`Init.cpp:48-70`）——变量还在，于是它再构造一个 `BackendObject_Remote` 并再 spawn 一次，首次 GL 调用时形成无界 fork 链。
**规则**：(a) spawn 时构造**显式 envp**，剔除 `MOBILEGL_TRANSPORT` 与所有 `MOBILEGL_IPC_*`（只保留 server 真正需要的少数几个，如 `MOBILEGL_BACKEND_TYPE`、日志路径）；(b) `mobilegl_server_main` 在能到达 `MG_Backend::Init()` 之前把 `MG_Config::Transport` 硬置为 `Monolith`。两条都做，任一条单独失效时另一条兜住。P0 增加一个 `MG_Test/Wire` 测试：spawn 一个 server 并断言进程树只多出**恰好一个**子进程。

`Hello{abiVersion, backendType, buildFingerprint, configBlob}` → `Welcome`。`configBlob` 转发 client 解析好的 `MG_Config::Features`，两半不可能对某个 quirk 开关有分歧。`buildFingerprint`（git hash + `Records.def` 的 hash）不匹配 → 握手期 `Fatal`。

### 11.2 `mobilegl_server_main` 的可见性（本轮新增）

`CMakeLists.txt:497-510` 在**非 Debug** 构建上给共享目标设 `C_VISIBILITY_PRESET hidden` / `CXX_VISIBILITY_PRESET hidden` / `VISIBILITY_INLINES_HIDDEN ON`——而 plugin 与 FCL 出货的正是 RelWithDebInfo（`MobileGL/build.gradle` 的 `fordebug` 类型强制 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`）。所以 `dlsym("mobilegl_server_main")` 在 Debug 下能用、在设备上静默失败。

**规则**：入口点声明为
```cpp
extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv);
```
并在 P0 验收里加 `nm -D libMobileGL.so | grep mobilegl_server_main` 断言（与既有的 `nm --defined-only` 门并列）。若哪天 macOS/Windows 也要托管 server，还需同步 `MG_Impl/DyldInterpose/ExportedSymbols.txt` 与 `wgl.def`。

### 11.3 Android

**minSdk 26 没有任何公开 NDK API 能扁平化 `ANativeWindow`**（NDK r27.3 的 `android/native_window.h` 无 parcel 符号；`libbinder_ndk` 是 API 29，`binder_ibinder.h:191`；`ASurfaceControl` 是 API 29，`surface_control.h:67`）。`Feat/CS-Delta-IPC` 的 `nativeBlob`"binder-flattened ANativeWindow"（`protocol.fbs:377-379`）不可实现。

- **P1-P8 验证路径：无窗口。** 两个 PIE ELF。**实测**：从解压出的 nativeLibraryDir exec 在 API 36 上可行（`run-as … libtrace_replay_runner.so` → exit 132 = SIGILL，即 ELF 已被加载进入，而非 `EACCES`；文件 0755 / `u:object_r:apk_data_file:s0` 且无 MLS category，**跨 package 也可**）。`useLegacyPackaging = true` 在 FCL（`../FCL/build.gradle.kts:76-82`）与 plugin（`android-plugin/app/build.gradle.kts:198-203`）都已开。surface 用 pbuffer 或 `AImageReader` 支持的 `ANativeWindow`（`HeadlessGL.cpp:86-131,268-274`），trace replay 默认 pbuffer（`apitrace_glws_egl.cpp:614-618`）。
  **注意实测的域**：上述 SIGILL 证据是经 `run-as` 取得的，即 `runas_app` 域，而不是 trace Activity 所在的 `untrusted_app` 域。**P0 的 Android spike 必须从应用自身进程 `posix_spawn` 一次**（见 §15 P0）。
- **P9 生产路径**：Java `Surface`（Parcelable）→ Messenger/AIDL → `MobileGLServerService`（`android:process=":mgl"`）→ JNI `ANativeWindow_fromSurface(env, surface)`，就是 FCLauncher 今天在 `egl_bridge.c:81` 做的那一次调用。**仓内先例**：`android-plugin` 的 `BenchService` 已在 `android:process=":bench"` 里跑 MobileGL（`BenchService.java:19-77`）。代价：server 进程多一个 ART（~15-25MB）。
- **纠正一条过期笔记**：FCL 把游戏 JVM 跑在**主进程**，不是 `:jvm`（`../FCL/src/main/AndroidManifest.xml:112-121`，`JVMActivity` 没有 `android:process`；`:jvm` 是下载 Service）。第二个进程必须新建。
- **HeadlessGL 的 fork 预检与孤儿 server（本轮新增）**：`MG_IntegrationTest/Harness/HeadlessGL.cpp:344-368` 会 fork 一个子进程跑完整 EGL bring-up 然后 `_exit(step)`，注释（`:364-366`）明说这是刻意的——"every atexit handler and static destructor in this address space belongs to the parent's copy of the world"。拆分模式下那个子进程的 bring-up 会走到 `MG_Backend::Init()` 并 spawn 一个 server；`_exit` 不跑任何拆机，那个 server 成为孤儿，活到它发现 EOF 或撞上 `MOBILEGL_IPC_IDLE_EXIT_S`（默认 30s）。父进程随即对同一设备起自己的 server。`HeadlessGL.cpp:585-589` 已经把这种失败模式命名为"a leaked exclusive device, an environment the child did not have"。
  **规则**：server 的 EOF 检测必须**即时且无条件退出**（亚秒级，不靠 30s 看门狗）；client spawn 时把 socket fd 设成 `_exit` 会确定性关闭的形态（不设 `FD_CLOEXEC` 以外的保活）；再加一次**有界重试的就绪握手**，这样残留的预检 server 不会把父进程弄 flaky。这个交互本身列为 P1 验收步骤 1 的一部分，先于任何广度工作。

### 11.4 Linux / X11

`Window` 是 XID，`nativeToken:u64` 直接送。backend 自己 `XOpenDisplay(getenv("DISPLAY"))` 并构造 `VkXlibSurfaceCreateInfoKHR`（`VulkanRenderer.cpp:14486-14521`），只要同 `DISPLAY`/`XAUTHORITY` 就免费。Wayland 今天不支持（`BackendObject.h:529` TODO），维持。
WSL/CI：**永不开窗** —— `EGL_PLATFORM=surfaceless` + `EnsureHeadlessPlatform()`（`HeadlessGL.cpp:160-196`，它存在正是因为一台带 WSLg `DISPLAY` 的工作站曾把这条 lane 弄挂）。

### 11.5 Windows

`HWND` 进 `nativeToken`。Vulkan 可行（`hinstance` 是历史遗留，`VulkanRenderer.cpp:14456-14463`）；**WGL/ANGLE-DXGI 对外进程 HWND 不受支持 → headless only**。

transport：默认 named pipe（asio `windows::stream_handle`）。**"继承句柄就免掉 accept/connect"这句在 asio 上不能直接照搬（本轮修正）**：`windows::stream_handle` 的 IOCP 服务要求句柄是 **overlapped** 的，而 `CreatePipe` 造的匿名管道不是。所以句柄对必须这样造：用一个 GUID 唯一命名的 `CreateNamedPipeW(..., FILE_FLAG_OVERLAPPED)` 做 server 端，配一次 `CreateFileW(..., FILE_FLAG_OVERLAPPED)` 做 client 端，然后把 server 端句柄设为可继承并 `CreateProcess` 传下去。§11 必须把这套构造写清楚。

asio 1.38.2 在 Win32 上确实定义了 `ASIO_HAS_LOCAL_SOCKETS`（`3rdparty/asio/asio/include/asio/detail/config.hpp:1085-1092`，只排除 `ASIO_WINDOWS_RUNTIME`，且自带 `sockaddr_un_type` 于 `socket_types.hpp:220`），但其 IOCP `async_accept` 走 `AcceptEx`，AF_UNIX 从不支持它——AF_UNIX-everywhere 是 P6 的**可选简化**，需真编真跑验证，named pipe 是已知可用的默认。

### 11.6 崩溃

- **server 死**：client 读到 EOF/EPIPE → device-lost 闩锁：后续 GL 调用变 no-op、`eglSwapBuffers` 返回 `EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus`（若 robustness 分支落地）返回 `GL_UNKNOWN_CONTEXT_RESET`。`MOBILEGL_IPC_RESPAWN=1` 时重启 + `ResyncSnapshot`（默认关，静默重启会掩盖 bug；且与 `MOBILEGL_IPC_ADOPT_TIER != 2` 互斥，见 §5.8）。
- **client 死**：server 读到 EOF → **立即**销毁原生 context 并退出（不等看门狗）；`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作为 EOF 都收不到时的最后保险。

---

## 12. Monolith 保留与模式选择

**四层保证，从强到弱：**

1. **编译期折叠。** `MOBILEGL_BUILD_DISAGGREGATED`（默认 **OFF**）关闭时 `MobileGL/MG_Remote/**` 不进 `SOURCE_FILES`，`MG_Config::Transport` 是 `constexpr Monolith`，`MG_Backend/Init.cpp` 里的分支在编译期消失。**默认构建与今天字节一致。**
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
`BackendObject_Remote::GetBackendFunctions()` 返回发射表，`Initialize()` 负责 spawn/connect。下游 ~250 个边界调用点**零 `#ifdef`**。
3. **P4.5 的 allocator 改动必须同样包裹。** `PipeResource::MapAlignedAllocator` 与 `MipmapStorage` 的 level vector 住在 `MG_State`，改它们的 allocator 就改了类型；写成"分配器特化，option OFF 时逐字折叠回今天的 `MapAlignedAllocator`"，否则第 4 层会在 P4.5 变红。
4. **机械证明**：对 `libMobileGL.so` 做 `nm --defined-only` 与去调试信息后的 `.text` size diff，改前改后必须一致。**这是每个阶段的出口判据（P0…P9），不只是 P0**（上一版只在 P0 跑）。

### 12.1 两个 option，不是一个（本轮重大修正）

上一版说"OFF 时字节一致"，但**每一条部署路径都要求出货构建是 ON**：FCL 用户可编辑 env、plugin APK 的 V2 开关表、ctest `ENVIRONMENT` 变体、`/data/local/tmp` CTS 路径。而上一版又说 ON 构建里 `inproc` 会把 `pGLContext` 变成 thread-local 加 `operator->` shim。那个 shim 坐在全库最热的路径上：`grep -rho 'pGLContext->' MobileGL/MG_Impl | wc -l` = **1494**，加 DirectGLES 124、DirectVulkan 169。Android 上 dlopen 的共享库无法可靠使用 initial-exec TLS，每次访问会退化成一次 `__tls_get_addr` 调用，而今天那里只是一次对全局引用的加载（`Core.h:564` `extern UniquePtr<GLState::GLContext>& pGLContext`）。

**规则**：拆成两个 option。
- **`MOBILEGL_BUILD_DISAGGREGATED`**（出货形态）：只含 `spawn`/`unix:`/`pipe:`。每进程只有一个 `GLContext`、一份 `gBackendFunctionsTable`、一个 `pActiveBackendObject`、一份 `pDefaultFramebufferInfo` → 这四个**全部保持普通全局**，GL 热路径上没有任何 TLS 与间接。侵入面就是 `MG_Backend/Init.cpp` 里那一个可预测的分支。
- **`MOBILEGL_BUILD_DISAGGREGATED_INPROC`**（CI/调试形态，隐含开启前者）：额外加角色隔离 shim。

### 12.2 `inproc` 需要隔离的是**四个**进程全局，不是一个（本轮修正）

上一版只谈了 `pGLContext`。实际上 `inproc` 下同一进程要同时扮演两个角色，以下四个全局都必须按角色分身：

| 全局 | 定义处 | 谁读 |
|---|---|---|
| `MG_State::pGLContext` | `GLState/Core.h:564` 声明，`Core.cpp:1487` 定义，`Core.cpp:20` 构造，`Init.cpp:63` reset | 全部 |
| `MG_Backend::gBackendFunctionsTable` | `MG_Backend/Init.cpp:44` 赋值 | client 侧 MG_Impl（91 处）**与 server 侧 MG_Impl**（`GL_Texture.cpp:1621` `GenerateMipmap_Backend`、`:6713-6725` `GetTexImage` 回退链、`FixupGsStripCaptureOrder`、`CopyReadFramebufferIntoMipmapRegion` 的 `ReadPixels`） |
| `MG_Backend::pActiveBackendObject` | `MG_Backend/Init.cpp:53-61` 赋值 | MG_Impl 89 处 + backend 内部 |
| `MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo` | `GL_Framebuffer.cpp:3344` 定义，全库 22 处引用 | client 侧 MG_Impl 13 处（`GL_Framebuffer.cpp:495,1827,1837,1897,1905,1913,1927,1936,2549,2590,2598,2608,2611`）+ server 侧 backend 5 处（`DirectGLES.cpp:1917,2838,2867,9675`、`SwapchainObject.cpp:276`，其中 `SwapchainObject` 是**写**） |

一旦 client 装上发射表，`inproc` 里 applier 与 server 侧 MG_Impl 就没有任何路径能拿到真正的 DirectGLES/DirectVulkan 表；而一个进程也不可能同时持有 client 的 default-FBO 描述与 server 的（`SwapchainObject` 直接往里写 server 的视角）。

**shim 的完整需求**（上一版只提了 `operator->`）：`operator->`、`operator bool`、`get()`、`== nullptr` 相等比较、从 `MakeUnique` 赋值、`reset()`。非箭头用法的实际数量是 **133**（`grep -rn pGLContext MobileGL/ --include=*.cpp --include=*.h | grep -v 'pGLContext->' | wc -l` = 133，上一版写的"约 65 处"少了一倍），其中 MG_Impl 只有 2 处（`GL_Debug.cpp:99` 的 `.get()`、`GL_Program.cpp:1630` 的 `== nullptr`），绝大多数在 MG_Backend——尤其 DirectVulkan 里约 90 处 `MOBILEGL_ASSERT(MG_State::pGLContext, ...)` 的真值判断，另有 `DirectGLES.cpp:146` 的 `.get()` 与 `Managers.cpp` 里十来处 `if (MG_State::pGLContext)` 守卫。**因为 backend 侧那一簇恰恰是必须看到 replica 的，shim 的原型应当先拿 `MG_Backend/DirectVulkan/DirectVulkan.cpp` 的 assert 密集区开刀。**

**如果这层隔离的成本被判定过高**，退路是把 `inproc` 降级为**纯测试模式**：applier 通过显式传入的表指针工作，server 侧不跑 MG_Impl（于是 `GenerateMipmap_Backend` 那类回退不可用，需要在 `inproc` 下走另一条路径）。但那样 P2.5 就不再测量它本该测量的"monolith 渲染线程"交付物——**这个取舍必须在 P0 结束前拍板并写进文档，不能悬着**。

### 12.3 `inproc` 作为产品交付物

在隔离成本可接受的前提下，`inproc` 不只是测试脚手架：同进程第二个 `GLContext` + `mgl-srv-apply` 线程 = monolith 的渲染线程。今天 `PrepareForDraw`（状态调和、VAO/FBO/纹理/program/render-state sync、UBO ring memcpy）加驱动调用全部同步跑在 `glDrawElements` 里；把它们搬到 apply 线程，对 GL 线程 CPU-bound 的应用（本项目的 profiling 史说 Minecraft 就是）是**手上最大的单一杠杆**，且不需要任何 IPC/shm/平台工作。§15 的 P2.5 就是证伪它的门。

### 12.4 运行时选择与开关

`MOBILEGL_TRANSPORT = monolith(默认) | inproc | spawn | unix:<path> | pipe:<name>`，在 `ConfigLoader.cpp` 与既有开关并列解析。这一个选择免费换来：ctest `ENVIRONMENT` 变体、trace-replay 的 `setenv` 块（`trace_replay_core.cpp:134-207`）、FCL 的用户可编辑 env 偏好（`FCLauncher.java:417-430`）、plugin APK 的 V2 开关表（`android-plugin/app/build.gradle.kts:77-103`，由 `.github/scripts/validate-plugin-apks.sh` 校验）、`/data/local/tmp` CTS 路径。**零新增管线。**

保留全部既有负面对照开关（`MOBILEGL_ESPRYT_DISABLE_{UBO,UNPACK,UPLOAD}_RING`、`_INVALIDATE_FLUSH`、`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`），新增：`MOBILEGL_IPC_SHADOW_SHM`、`MOBILEGL_IPC_ADOPT_TIER`、`MOBILEGL_IPC_PROGRAM`、`MOBILEGL_IPC_INLINE_PAYLOADS`、`MOBILEGL_IPC_PRESENT_CREDIT`、`MOBILEGL_IPC_SPIN_US`、`MOBILEGL_IPC_POLL_ESCALATE`、`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`、`MOBILEGL_IPC_SERVER_AFFINITY`。

---

## 13. 构建布局

```
MobileGL/MG_Remote/
  Protocol/  protocol.fbs  protocol_generated.h(提交)  Records.def  RecordKinds.h
             Handles.h  Coverage.def  MutationCoverage.def
             generated/BackendStateSurface.inc(提交)  generated/ImplMutationSurface.inc(提交)
  Transport/ ITransport.h  InProcessTransport.{h,cpp}  SocketTransport.{h,cpp}
             Framing.h  Ring.{h,cpp}  ShmSegment.{h,cpp} ShmSegmentPosix.cpp ShmSegmentWin32.cpp
             FdPassing.{h,cpp}  Doorbell.{h,cpp}
  Shared/    XfbAccounting.{h,cpp}   # client 与 applier 共用的 MG_Impl-side mutation helper
             MipmapLevelPlan.{h,cpp}
  Client/    WireMirror.{h,cpp}  EmitTable.cpp  EmitBufferOps.cpp
             BackendObject_Remote.{h,cpp}  CapsMirror.{h,cpp}
             ClientArrayBounds.cpp  CompositeResolver.cpp  ShadowArena.{h,cpp}
             PersistentMapTracker.{h,cpp}  GpuWritePending.{h,cpp}
             CoverageAssert.cpp  Surface/{X11,Win32,Android,Headless}.cpp
  Server/    ReplicaContext.{h,cpp}  Applier.cpp  ServerLoop.{h,cpp}
             ReplyPool.{h,cpp}  EventRing.{h,cpp}  ServerMain.cpp
  ServerJni.cpp                                  # Android，与 DriverPostJni.cpp 并列
scripts/     gen_protocol.py  gen_backend_state_surface.py  gen_impl_mutation_surface.py
MobileGL/MG_Test/Wire/CMakeLists.txt             # 复制自 MG_Test/Buffer/（27 行）+ MobileGL_Protocol
```

CMake：
- `MG_Remote/**` 仅在 `MOBILEGL_BUILD_DISAGGREGATED` 下追加进 `SOURCE_FILES`（`CMakeLists.txt:226-419`），因此 `MobileGL`（`:485`）与 `MobileGL_s`（`:552`）都拿到。
- `MobileGLServer`：桌面 `add_executable` 链接 `MobileGL_s`，`RUNTIME_OUTPUT_DIRECTORY` 设为 `$<TARGET_FILE_DIR:MobileGL>`（§11.1）；**Android** `add_executable` + `set_target_properties(MobileGLServer PROPERTIES PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "MobileGLServer")` 并链接**共享**的 `MobileGL`（一份 ~43MB 的 glslang/SPIRV-Cross/SPIRV-Tools），由 AGP 打进 `jniLibs`。server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`（可见性见 §11.2）。**一份共享库、两个角色，版本必然匹配**（对比 `Feat/CS-Delta-IPC` 的四件必须互相匹配的产物）。
  **AGP 能否打包一个被改名成 `lib*.so` 的 `add_executable`，是 P0 spike 的验证项之一**（`MobileGL/build.gradle` 没有设 `targets` 列表，上一版把这条当成已知事实）。
- **FlatBuffers**：submodule `3rdparty/flatbuffers` 置于既有的 `if (EXISTS .../flatbuffers/CMakeLists.txt)` 保护下，**去掉 `if (NOT ANDROID)` 一刀切**。因为 `protocol_generated.h` 已提交，**默认构建图里没有 `flatc`，也不 `add_subdirectory(3rdparty/flatbuffers)`**（§7.1）。运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上。
  **guard（本轮新增）**：若 `MOBILEGL_BUILD_DISAGGREGATED=ON` 而 `3rdparty/flatbuffers/include` 不存在，强制把该 option 设回 OFF 并 `message(WARNING ...)`——否则 `MG_Remote/**` 已经进了 `SOURCE_FILES` 而头文件找不到，构建以一个莫名其妙的错误失败（现有的 `EXISTS` 保护只包住 Protocol 子目录）。
  `MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`，经 `MobileGL/build.gradle:17-21` 已在用的 `externalNativeBuild { cmake { arguments } }` 槽传入。
- 测试接线：
  - `MG_Test/Wire/`（label `unit`）→ 现有 CI `test` job 自动收，**无需改 workflow**。
  - `MG_IntegrationTest/CMakeLists.txt` 每 backend 增加一条 `gtest_discover_tests`（`TEST_PREFIX "DirectGLES.Split."` / `"DirectVulkan.Split."`），**必须用 `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` 构造**，并带上 `MOBILEGL_IPC_SERVER_PATH`。三个已被文档记录的陷阱要遵守：ctest `ENVIRONMENT` 是**替换而非追加**（`:339-343`）、`;` 必须转义（`:322-332`）、property 覆盖 job env（`test.yml:253-262`）。
  - **trace replay 的 `SPLIT` 接线（本轮补细节）**：`add_trace_replay_test` 今天把测试命名为 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}`（`tools/trace_replay/CMakeLists.txt:330-332`），加一个 `SPLIT` 参数会与同 case+backend 的现有测试**重名**。改成 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}${SPLIT_SUFFIX}`。另外该测试的命令是 `cmake -P run_trace_case.cmake` 加约 18 个 `-DTRACE_*` 变量，所以还要加 `-DTRACE_TRANSPORT=` 并在 `run_trace_case.cmake` 里消费它——**这两个文件都要列进 P2 的交付物**。
- CI 新增三个 step：`flatc-check`（重生成 `protocol_generated.h` + `git diff --exit-code`）、`coverage-check`（重生成两个 `.inc` + `git diff --exit-code`）、`monolith-abi-check`（OFF 构建与 ON+monolith 构建的 `nm --defined-only` / `.text` size 对基线）。
- **CI 新增一条 grep 门**：禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`。

---

## 14. 对 `Feat/CS-Delta-IPC` 的复用清单

### REUSE（原样取）
| 路径 | commit | 备注 |
|---|---|---|
| `docs/CS_Refactor/HandleSessionGeneration.md` | `546895aa` | 分支上最好的产物。三处修改：handle 清单补 `RenderbufferObject::GetLifetimeId()` **与 `GetVersion()`**；把第 2 节的 server 侧 share-group 要求降为 v2；把"lifetimeId 不符 → 销毁重建"改成 `Fatal`（§5.4） |
| `MobileGL/Protocol/mg_protocol_base.h` | `546895aa` | 干净无依赖的词汇（`MobileGLResult`、span、`ShmRegion`、id typedef、structSize-first 版本纪律） |
| `MobileGL/Protocol/tests/ProtocolSmoke.cpp` | `546895aa` | schema 往返门（默认改 ON） |
| 根 `CMakeLists.txt` 的 `EXISTS` 保护 + `.gitmodules` 条目 | `546895aa` | 去掉 `NOT ANDROID`，另加 §13 的 include-dir guard |
| `docs/CS_Refactor/HANDOFF.md` 第 6 节"已知坑清单" | `d5c00b9d`/`5964628d` | 逐字留作事后复盘：路径转换、versionCode 降级、双设备 `ANDROID_SERIAL`、flatbuffers camelCase accessor、union vector 产生指针、Release 下 `MGLOG_D` 被编译掉、嵌套 submodule 配方、`assembleTraceDebug` 改名 |

### CHANGE（取走并改造）
| 路径 | commit | 改造 |
|---|---|---|
| `MobileGL/Protocol/protocol.fbs` | `546895aa` | 保留 delta 目录、`RenderStateBlob` 整块思想、`BufferShmAdopt`、命令清单、事件分类学。改：热路径转 `struct` + ring；删掉冗余的 `inlineBytes`/`data` 双胞胎（`:111-112`、`:125-126`，两半代码对哪个字段是真的意见不一：`ServerCore.cpp:184-208` 只读 `data`，`StateEmitter.h:60,111` 只写 `inlineBytes`）；加 `ResyncSnapshot`、`AuxRequest`；给 `ProgramPublish.reflection` 与 `ObjectCreate.params` 真 schema；kind 枚举生成 + 每 kind `static_assert` + 运行期边界检查 |
| `MobileGL/Protocol/CMakeLists.txt` 的 flatc 解析 | `546895aa`/`65717b4c` | **不再照搬**：`add_subdirectory(3rdparty/flatbuffers)` 从默认路径整段删除（它就是那个 NDK 陷阱本体）；只保留 `MOBILEGL_FLATC_EXECUTABLE` 供 CI；`enable_testing()` 移到根 |
| `MobileGL/ServerCore/ServerCore.{h,cpp}` | `65717b4c`+`c2260dd8` | 保留握手→解码→apply→credit 形状与 plugin manifest loader 思路。修：单次校验 + 零拷贝解码（今天校验两次外加一次整体拷贝，`:492-498` 与 `:218-221`）；io/apply 分线程（`:404-406` 自承 worker 从未落地）；完整事件集（`SendEvent` 只实现 `BATCH_APPLIED`，`:373-382`）；credit 用最后一条实际 seq（`:427` 的 `baseSeq + items.size()`）；接收缓冲不能是对着 64MiB 帧上限的固定 4MiB（`:478`）；真正的段生命周期（`m_segments` 只增不减，`blobOwners` 只 push 不释放） |
| `ServerCore/tests/LoopbackSmoke.cpp` + `Backends/Dummy/` | `65717b4c` | 分支上最便宜的端到端门，**第一个重建**，重定向到真 applier |
| `MobileGL/Remote/InProcessTransport.h` | `65717b4c` | 重表述在 C++ `ITransport` 上；单侧 shutdown（今天 `:89-92` 连对端 inbox 一起关）；真段生命周期（`Unmap`/`Close` 今天是 no-op）；补 §6.2a 的双向 doorbell（condvar 版） |
| `MobileGL/Remote/Framing.h` | `65717b4c` | 保留帧格式；`m_pendingSize`/`m_haveHeader` 改 `mutable`（今天 `const_cast`，`:81,85`）；`Feed()` 真校验 magic 与长度（今天永远返回 OK，坏 magic = 静默永久挂起）；缓冲不足返回所需大小且**保留消息**；真正在 socket transport 里使用它（今天是死代码） |
| `MobileGL/RemoteClient/StateEmitter.h:39-307`（**仅 emit 半边**） | `b50f3348`+`d96be9f3` | 各域字段遍历是真知识，抬进 `WireMirror`/`ResyncSnapshot`。GL name 换 `lifetimeId`（今天 `:48-49,85,166-168,203,230` 全把 GL name 塞进 `handle`）；`:175-181,:244-249,:253-258,:293-298` 的 O(n²) 线性扫描换 handle map；固定 6 attachment（`:232-236`）换 `MaxColorAttachments`；补上被跳过的 texture view（`:70-74`）。**不取 applier 半边（`:312-501`）** |
| `scripts/extract_backend_read_inventory.py` | `546895aa` | 改造成 `gen_backend_state_surface.py`：删掉前缀兜底（`:234-241`），未知 accessor 一律 UNMAPPED 并**编译失败**；把真 pull point 与 signature handle 化分开统计。**另写一个全新的 `gen_impl_mutation_surface.py`**（§5.9b），它在原分支没有对应物 |

### DROP
| 路径 | 理由 |
|---|---|
| `MobileGL/Protocol/bfa.h`（480 行） | "strict C ABI"不是 C ABI：`ServerCore.cpp:177-179` 把 FlatBuffers 生成表的指针交给插件，插件必须是 C++ 且链接 FlatBuffers（`StateEmitter.h:330,351,362,372` 就是这么用的）。手抄的 60 字段 `MobileGLDynamicParameters`（`:63-129`）自承尾部不全、同步脚本从未写过——正是已在本项目造成 481 例 CTS 失败簇的那类数据的**长期静默漂移炸弹**。而本设计根本不需要 delta-apply vtable |
| `MobileGL/Protocol/mgruntime_api.h` + `MobileGL/UtilRuntime/*` | 360 行契约对 ~50 行实现（8 域实现 2 域）；唯一消费者传 `nullptr`（`ServerCore.cpp:61`）；缓存每次命中整份拷贝（`:79`）、按 `clear()` 淘汰（`:91-93`）；smoke 断言 `api->metrics == nullptr`（`RuntimeApiSmoke.cpp:66`）。它的唯一理由随 BFA 消失；且本设计里翻译全在 server（它无论如何要链 SPIRV-Cross），glslang 全在 client |
| `MobileGL/Remote/LocalSocketTransport.{h,cpp}`、`ShmFactory.{h,cpp}` 实现 | 从未被任何测试执行（`LoopbackSmoke` 用的是 `InProcessTransport`，唯一另一个消费者 `ServerHost` 编译不过）；每次 send 都 use-after-free（`:199`，`asio::buffer(next)` 指向局部 vector 而 lambda 捕获的是另一份拷贝）；按 wire 长度无上限分配（`:232-236`）；`Start` 里阻塞 accept/connect（`:116`、`:139-144`）；无 strand 且 `framesSent++` 非原子（`:177-178`）；**且完全没有 POSIX fd 传递**（`:296` 硬编码 `fd=-1`），Linux/Android 数据面一字节过不去。只保留 `ShmFactory.h:4-12` 作平台矩阵规格 |
| `MobileGL/ServerHost/main.cpp` | 编译不过（`:31,39,44,53-54` 对指针用 `.`，`c2260dd8` 改返回类型后成为死码）。`MobileGLServer` 在默认 ALL target 里，**分支 tip 无法完成一次完整构建** |
| `MobileGL/RemoteClient/tests/StateEquivalenceTest.cpp` | 把 delta apply 进第二个 `MG_State::GLContext`——验证的是它自己的 thin-server 前提说不该存在的数据路径；与生产 apply 路径零共享代码；只测全量 resync；`d96be9f3` 声称五域逐字段而文件只比了纹理、buffer、render-state blob、buffer binding slot（没有 VAO 属性/FBO attachment/RBO 格式比较） |
| `c7c9e346` + `29d721ef` 全部（share-group sessioning） | 非 v1 前提（monolith 只有一个 `GLContext`：`GLState/Core.cpp:20,1487`）；且非可合并质量：`VertexArrayState.cpp:+20-26` 往已共享的表里再压一个 default VAO 并重复 `Insert(0)`；四个头文件 `public:` 未复位泄漏私有成员；current session 是无锁进程全局，连它自己的 per-thread current 都没兑现；在状态权威里塞 `MOBILEGL_SESSION_SWAP` env kill switch 与 `s_defaultAdopted` 偷 context 的 hack。日后作为独立 PR 带多 context 测试落 `dev` |
| `b50f3348` 的 `RenderState::InstallParameters` + `public:` | 本设计不需要 Install setter（D3）；若日后需要整块安装，用正确作用域的方法或单条 friend，绝不靠裸 `public:` |
| `d96be9f3` 的 TRIAGE 指令（`DirectGLES.cpp:+2583-2590`） | per-draw `fprintf(stderr)`。**分支上每一次测量都跑在它上面。** 同规则适用于当前工作树的 `[IBOTX]`/`[BUFTX]`（P0 清除） |

---

## 15. 分阶段实施计划

> 通用纪律（每个 commit 都适用）：默认 ALL target 必须能完整构建；禁止提交热路径插桩；每个门必须**能因它存在的理由变红**；**Windows 机器不是正确性门**（其 Vulkan 缺 `vkCreateHeadlessSurfaceEXT`，占该机 567 个基线集成失败中的 423 个）；设备对比走 reboot-clean + 同窗口配对 A/B；**每个阶段的出口都跑一次 §12 第 4 层的 `nm`/`.text` monolith 门**（不只是 P0）。

### P0 — 卫生、骨架与两个 spike（5 天）

**交付物**
- 清除工作树 `[IBOTX]`/`[BUFTX]` fprintf（`DirectGLES.cpp:640-663`、`Managers.cpp:875-877`，后者在 `pendingMutex` 临界区内）。
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

