# P8-C：Espryt server 暂存影子与 GPU 写

> 2026-09-29，分支 `p8/c`（基线 `8d3e2317`）。计划行：[`PLAN-P8.md`](PLAN-P8.md) 包 C。

## 1. 结论

- 前提成立（red-once 见 §4）：split 下 server 的 R-11 暂存影子只由上传 / adopt 填，server 上的 GPU 写不进影子；CPU 读者读到旧字节，或因无影子直接拒绝。原 7 例在基线上 Espryt split / spawn / tcp 与 inproc A/B 全红。
- 修在 server，不动 client、不动 monolith：写者给 server 孪生体打标（`serverGpuWritten`，`MobileGL/MG_Backend/DirectGLES/Managers.h:1059`），读者消费字节前经 `SplitHostBytesForCpuRead`（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:3595`）从 server 自己的 GL buffer 整 store 回读。
- XFB 不打标：两条捕获路径本来就把字节拿在手里（给 client 的 writeback），直接写进影子（`StageServerWrittenRange`，`MobileGL/MG_Backend/DirectGLES/Managers.cpp:3651`）。
- 懒刷新：只在读者真正消费字节时回读；原生 indirect 不从 CPU 取字段时不回读，Create / Flywheel 式每帧 compute 剔除 + 原生 indirect 不付回读（`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:8555`）。
- 顺带关掉 P3a M-3 声明的偏差（fp64 顶点收窄读旧字节）在 split 臂上的一半；monolith 臂不动。
- monolith 两后端共 5 处缺陷，skip 按名、记给 dev（§6，ID-P8-3）；IndirectCount 的 monolith 按 OQ15 不改。

## 2. 读者与写者

| 读者（split 臂） | 位置 | 做法 |
|---|---|---|
| primitive restart 重写 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:9309` | `SplitHostBytesForCpuRead`（整 store） |
| multi-draw rebase | `MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp:196` | 同上 |
| `*IndirectCount` 的 count | `DirectGLES.cpp:9632`、`:9798` | 参数 buffer 刷新；命令 buffer 仅在无影子时取第一份 |
| indirect 命令字段 | `DirectGLES.cpp:8549` | CPU 路径全取；原生路径只在程序读 `gl_BaseVertex`、或无 `mg_IndirectParams` 视图而读 `gl_BaseInstance` 时取 |
| 无影子的 indirect 命令 buffer | `DirectGLES.cpp:487` | 被 GPU 写过就取第一份（原来 decline，`indirect_command_bytes`） |
| XFB scatter 的捕获前字节 | `DirectGLES.cpp:2037` | 刷新 |
| fp64 顶点收窄 | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:6696` | 刷新，并让 memo 失效（GPU 写不动 serial） |

| 写者 | 位置 | 效果 |
|---|---|---|
| SSBO 记录臂（draw 与 dispatch） | `DirectGLES.cpp:780` | 标记窗口内每个绑定点（与 client 的 `ShaderBufferEmit.h` 同口径） |
| 原子计数记录臂 | `DirectGLES.cpp:1017` | 标记 |
| 可写 buffer image | `DirectGLES.cpp:4464` | 纹理描述符的 `BufferForTexBuffer` → 标记 |
| XFB 普通捕获 | `DirectGLES.cpp:1874` | 映射出的字节写进影子；T0 store 只标记（`:1829`） |
| XFB scatter | `DirectGLES.cpp:2104` | 合成结果写进影子 |
| 清标记 | `Managers.cpp:2308`（respecify）、刷新末尾 | — |

- 刷新顺序同 `Ops_H_Readback`：`FlushPendingRangesFrom` 先落排队上传 → `DrainResidentWritesNow` → 整 store `glMapBufferRange(READ)` → `MGL_SERVER_STAGED_ADOPT`（覆盖变整段，孤儿 store 也就有了第一份）→ serial 戳 + epoch bump。
- T0 store：影子就是 server 对 client AHB 的一致映射；有标记或有排队常驻写时 `DrainResidentWritesNow` + `glFinish`（DEBTS 的 P11 B2 行的 split 半）。
- 刷新与写影子只在复制型暂存 store（split）下生效（`CopiesIntoServerStorage`）：monolith 的 store 保留调用方指针，映射一解就悬空。
- 全部新代码在 `MOBILEGL_BUILD_DISAGGREGATED` 下；`ReadsBaseInstance`（`Managers.h:3124`）同。

## 3. 场景

`GpuWrittenDrawInputScenario`（`MobileGL/MG_IntegrationTest/Scenarios/GpuWrittenDrawInputScenario.cpp`，split-only 源，登记 `MobileGL/MG_IntegrationTest/CMakeLists.txt:3461`）12 例：GPU 只写一次，暂存影子里是画不出正确结果的毒值。

| 例 | 写者 | 读者 |
|---|---|---|
| RestartIndicesAComputeShaderWroteSplitTheStrip | SSBO | restart 重写 |
| …IntoAnOrphanedStoreSplitTheStrip | SSBO，`glBufferData(NULL)` | restart 重写（原来无影子 → 跳过 draw） |
| …SplitEveryMultiDrawStrip | SSBO | multi-draw rebase |
| ACountAComputeShaderWroteDrivesMultiDrawElementsIndirectCount | SSBO（命令 + count） | count |
| …DrivesMultiDrawArraysIndirectCount | SSBO | count |
| ABaseVertexAComputeShaderWroteReachesGlBaseVertex | SSBO | 原生 indirect 的 `gl_BaseVertex` |
| IndirectCommandsAComputeShaderWroteIntoAnOrphanedStoreDraw | SSBO，`glBufferData(NULL)` | 无影子命令 buffer |
| RestartIndicesATransformFeedbackCapturedSplitTheStrip | XFB | restart 重写 |
| RestartIndicesAScatteredTransformFeedbackCapturedSplitTheStrip | XFB + `gl_SkipComponents1`（scatter） | restart 重写 |
| RestartIndicesImageStoresWroteSplitTheStrip | `imageStore` 到 buffer 纹理 | restart 重写 |
| ACountAnAtomicCounterProducedDrivesMultiDrawElementsIndirectCount | 原子计数即参数 buffer | count |
| DoubleVerticesAComputeShaderWroteAreNarrowedFromTheWrittenBytes | SSBO，两轮 | fp64 收窄（第 2 轮查 memo） |

- 臂：`DirectGLES.{Split,Spawn,Tcp}.`（门控）；分裂构建里的 `DirectGLES.` / `DirectVulkan.` 即 monolith 对照与 inproc A/B；Magma split 各臂经 tier 2（`integration-magma-all-*`，信息层）与 `DirectVulkan.*.Full.`（`integration-magma-full-split`，门控）。
- 三臂同源登记，`spawn_lane_parity.py` 无需例外行。

## 4. red-once

| # | 变异 | 载体 | 观察到的红 |
|---|---|---|---|
| 前提 | 基线 `8d3e2317` + 原 7 例 | `ctest -R GpuWrittenDrawInput`；inproc A/B 同命令加 gate 的环境 | Espryt split / spawn / tcp 21/21 红（像素：黑 = 旧毒值 / 跳过；baseVertex 例红色 = 几何对、`gl_BaseVertex` 旧）；inproc A/B 7/7 红；Magma split 各臂全绿 |
| F1 | 去 SSBO 标记、原子标记、image 标记、XFB 两处写影子（一个构建） | `ctest -R '^DirectGLES\.(Split|Spawn|Tcp)\.GpuWrittenDrawInput'` | 12/12 例在 Split、Spawn 红；Tcp 臂 Not Run（`TcpServer.Start` 撞上 :48440 的 TIME_WAIT），F2 覆盖了三臂；每例只依赖其中一个变异 |
| F2 | 读者侧：restart / rebase / 两处 count 改回 `SplitHostBytes`，`CurrentIndirectCommandBytes` 恒返回原指针，去无影子取第一份，fp64 memo 去掉 `!sourceGpuWritten`（一个构建） | 同上 | 10 例 × 3 臂 = 30 红；两例 XFB 绿（它们的字节来自写影子，不经刷新，符合设计）；fp64 例只红第 2 轮（memo） |
| M | 把 `SkipBrokenMonolith` 探针成恒 false | `ctest -R '^Direct(GLES|Vulkan)\.GpuWrittenDrawInput'` | Espryt monolith：两例 count、原子 count、baseVertex、fp64 红；Magma monolith：6 例 restart SIGSEGV、Arrays count 红、fp64 SIGSEGV |

还原一律 copy + `cmp` + `touch`。

## 5. 门

- 场景：134 条（含 monolith skip 13 条）全绿；inproc A/B 24/24；相邻套件（Restart / MultiDraw / DrawParameters / IndexedDrawFamily / ClientVertexArray / Xfb / TransformFeedback / AtomicCounter / BufferTexture / DoublePrecision / ResidentIndex）1304/1304。
- G1：`added=0 removed=0`，pull `.text` `a5ba13` 与基线相同。
- retrace（主机）：Espryt `SPLIT|SPAWN` 78 条 + `create-*` 12 条共 86 条，84 过；挂的 2 条是 `iris-iterationrp-in-world` 的宿主 llvmpipe JIT 崩溃（`LLVM ERROR: Cannot select ... vcvtps2ph`，已知项）。全量 236 条跑到 47 条（全过）时中止，全量留给集成。
- create 的 SSIM：indirect Espryt 0.999961275 / Magma 0.999956667，instancing Espryt 0.999957572 / Magma 0.999952470，monolith、SPLIT、SPAWN 三臂逐位一致；Espryt indirect 与 P9 W2 普查的值相同（[`../p9/W2-REMINT.md`](../p9/W2-REMINT.md) §4）。
- 懒刷新实测：create-indirect / create-instancing 的 Espryt SPLIT、SPAWN 回读 0 次（临时把刷新日志提到 INFO，数 server 日志，未提交）。
- 全门数字见包报告（`~/w7/notes/p8/c-report.md`）。

## 6. monolith 缺陷（给 dev，ID-P8-3）

| 后端 | 缺陷 | 位置 | 场景里的表现 |
|---|---|---|---|
| Espryt | `*IndirectCount` 读前端影子不调 `SyncGpuWrites`（OQ15） | `DirectGLES.cpp:9688-9689`、`:9852-9853` | count 读 0，什么都不画 |
| Espryt | 原生 indirect 的 `gl_BaseVertex` 从前端影子取（单进程臂 `ResolveIndirectCommandBytes` 只 `SyncPersistentMappedRange`） | `DirectGLES.cpp:521`、`:8640` | 几何对、颜色红 |
| Espryt | fp64 收窄的 handle 臂在 monolith 下读前端影子（P3a M-3 声明的偏差，monolith 半） | `Managers.cpp:6675` | 画在屏外 |
| Magma | `MultiDrawArraysIndirectCount` 的 CPU 臂不调 `SyncGpuWrites` | `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:749` | count 读 0 |
| Magma | draw 录制中途调 `SyncGpuWrites`（restart 重写、GL_DOUBLE 顶点转换）：提交并等待结束了当前帧命令缓冲，draw 接着往已结束的缓冲里录 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:4513`、`:4179` | SIGSEGV（lavapipe 工作线程 / `vkCmdBindVertexBuffers`）；校验层 `VUID-vkCmdBindIndexBuffer-commandBuffer-recording` |

- skip 只在 `transportName == "monolith"` 的对应后端生效，理由串带出处。

## 7. 未做 / 已知边界

- T0 与常驻写那一半主机上无法 red（主机无 T0，split 下采纳档钉在 T2）；代码按 `Ops_H_Readback` 的持久映射臂写，设备验证留给有设备的包。
- 标记是过近似：窗口内每个绑定的 SSBO 每次 draw 都标记；留着 SSBO 绑定、又拿同一 buffer 做任意 restart 索引的 EBO 时，每 draw 回读一次。
- bit 13 关（`BindingPointsComeFromRecords()` 为 false）的 A/B 配置下 SSBO / 原子不打标。
- `Ops_H_Readback`（client 发起的回读）没有顺带刷新 server 影子（可省一次回读，未做）。
- 同一 draw 既写 X（SSBO）又拿 X 当 EBO：标记在 `SyncNeccessaryBuffers` 里打，restart / rebase 读者在 draw 执行前就刷新并清标记，写发生在清之后，X 留成未标记。monolith 同形（`MarkShaderStorageBuffersGpuWritten` 先标，`SyncGpuWrites` 在 draw 前清），是对齐不是倒退。
- DEBTS 的 P11 B2 行（handle 臂读采纳 / T0 映射前不落常驻写）：split 半由 `SplitHostBytesForCpuRead` 的 T0 臂做了、主机不可验证；单进程半归 dev。
- Magma 的 `VulkanRenderer.cpp:3686`（最大索引扫描）与 §6 最后一行同模式，未验证。
