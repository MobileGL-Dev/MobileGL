# P8-D：Magma wire 臂原生 indirect draw

> 2026-09-29，分支 `p8/d`（基线 `8d3e2317`）。计划：[`PLAN-P8.md`](PLAN-P8.md) 包 D；审计 §4、§6 风险 3。主机 lavapipe，只动 Magma（Espryt 作对照）。

## 1. 结论

- wire 臂的 `glDraw*Indirect` / `glMultiDraw*Indirect[Count]` 改为从 wire store 自己的 `VkBuffer` 发 `vkCmdDraw[Indexed]Indirect[Count]`：`VulkanRenderer::DrawWireIndirectNative`（`MobileGL/MG_Backend/DirectVulkan/Renderer/WireDraw.inc:627`）。
- `DrawWireIndirect`（`MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:381`）先走原生；CPU 展开只留给设备发不出的 COUNT 形态（与 monolith 同条件，§3）。
- create-indirect × Magma × split：indirect 引起的 host 等待 321 → 0，等待时间 8.07 s → 0.013 s（剩下 6 次是应用回读，§2）；SSIM 各臂不变。
- 新增 barrier：着色器写过的 store 在下一次 indirect 读前补一个 `INDIRECT_COMMAND_READ` barrier，每次写一次（`WireDraw.inc:693-705`）。create-indirect 的 322 次 cull→draw 里 320 次没有 `glMemoryBarrier`，旧 CPU 路径靠整 GPU 等待碰巧有序，原生路径靠这个 barrier。

## 2. 测量（主机，`MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1`，server 日志 `windr[...]` 求和）

两个 fixture 都只有 1 帧（frames=1），所以"每帧"= 每次重放。主机上同时有 7 个包在跑，毫秒只看量级。

| fixture | 臂 | draws | wind | wixp | wibar | whw | whwi | whwus | SSIM |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| create-indirect | monolith（前后相同） | 13172 | 0 | 0 | 0 | 0 | 0 | 0 | 0.999956667 |
| create-indirect | inproc，改前 | 13172 | 0 | 321 | 0 | 327 | 321 | 8065617 | 0.999956667 |
| create-indirect | inproc，改后 | 13172 | 321 | 0 | 321 | 6 | 0 | 12540 | 0.999956667 |
| create-indirect | spawn，改前 | 13202 | 0 | 322 | 0 | 328 | 322 | 8360015 | 0.999956667 |
| create-indirect | spawn，改后 | 13202 | 322 | 0 | 322 | 6 | 0 | 15769 | 0.999956667 |
| create-instancing | 三臂，前后相同 | 14158 / 14194 | 0 | 0 | 0 | 0 | 0 | 0 | 0.999952470 |

- 字段：`wind` 原生 indirect 调用、`wixp` CPU 展开、`wibar` 补的 barrier、`whw` / `whwus` 真正等了的 `WaitForWireBufferHostAccess` 次数 / 微秒（`MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp:321`）、`whwi` 其中由 indirect 读引起的。定义在 `MobileGL/MG_Util/Metrics/PipeStats.h:280-285`，打印在 `MobileGL/MG_Util/Metrics/PipeStats.cpp:697`。
- 改后剩下的 6 次等待全部来自 `ReadbackWireBuffer`（`VkBufferManager.cpp:574`，临时探针取调用方地址 + `addr2line`，未提交）：应用对 6 个 store 的回读，改前也是这 6 次（327 − 321）。
- 同一 trace（apitrace dump 到 target call）：322 次 `glMultiDrawElementsIndirect`、2901 次 `glDispatchCompute`、9 次 `glMemoryBarrier`；前 320 次 draw 与其 cull dispatch 之间没有应用 barrier，只有最后 2 次前有 `GL_COMMAND_BARRIER_BIT`。每次 draw 前都有 compute 写它的命令 store，所以 `wibar` = draw 数。
- create-instancing 没有 indirect draw，也没有等待：它的 p99 差异不归 D。

## 3. monolith 何时在 CPU 上读 indirect 字（D 只在同样的场合保留 CPU 路径）

| monolith 场合 | 位置 | wire 臂（本包后） |
|---|---|---|
| 没绑 `GL_DRAW_INDIRECT_BUFFER`：命令在 client 内存 | `DirectVulkan.cpp:684`、`:825`、`:884` | client 在兼容档拒绝（`MobileGL/MG_Remote/Client/EmitTables.cpp:730` `CLIENT_COMMANDS`）；store 为空则具名拒绝 `IndirectBufferUnbound` |
| `glMultiDrawArraysIndirectCount`：总在 CPU 读 count 字 | `DirectVulkan.cpp:720-757` | 原生 `vkCmdDrawIndirectCount`（`MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:15942` 加载，只在 disaggregated 构建） |
| `glMultiDrawElementsIndirectCount`：无 `VK_KHR_draw_indirect_count`，或 maxdrawcount > 1 且无 `multiDrawIndirect` → CPU 循环 | `VulkanRenderer.cpp:13421-13444` | 同条件（`WireDraw.inc:630-635`）返回 false，走原 CPU 展开（`DirectVulkan.cpp:390-413`，计入 `wixp`） |
| 无 `multiDrawIndirect` 或 stride 不是 4 的倍数：逐条原生 indirect，仍由 GPU 读 | `VulkanRenderer.cpp:13510`、`:13615` | 同（`WireDraw.inc:732-740`）；stride 非 4 倍数在 GL 是 INVALID_VALUE，wire 上具名拒绝 `IndirectCommandRange` |
| client 顶点数组 + arrays indirect：CPU 读命令定上传范围，draw 仍原生 | `VulkanRenderer.cpp:13576` | 不可达：client 把 client 数组拷成自有 buffer |
| `GL_LINE_LOOP`：indirect 不改写，退化为 LINE_STRIP | `MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.cpp:27` | 同 |
| uint8 索引无扩展 / 非固定 restart 索引 / 非对齐偏移：整段索引在 CPU 改写，命令仍 GPU 读 | 两臂都在各自的索引上传里 | `WireDraw.inc:398`，索引视图是整个 EBO（`WireDraw.inc:681`，同 monolith） |
| baseInstance / gl_DrawID | 原生（monolith 不查 `drawIndirectFirstInstance`） | 原生；旧 CPU 展开的第 3 档把 gl_DrawID 全给 0（`VulkanRenderer.cpp:13154`），原生反而更对 |

## 4. 设计

| 项 | 内容 |
|---|---|
| 入口 | `DrawWireIndirect`（`DirectVulkan.cpp:381`）→ `DrawWireIndirectNative`；false 才走 CPU 展开 |
| store | `AcquireWireSlice` 取 indirect / count / 元素 store（打 `lastUseSerial`，之后的 `glBufferSubData` 会排在这次读之后）；范围、4 字节对齐、count 字越界都是具名拒绝（`MobileGL/MG_Backend/DirectVulkan/Renderer/WireDeclines.def:115-118`） |
| draw 准备 | 复用 `SetupDraw` → `SetupWireDraw`，带 `IndirectDrawBuffer`；该 aspect 原来是 `buffer-legacy-arm` Fatal，现只对 monolith 入口仍 Fatal（`WireDraw.inc:213`，`m_wireNativeIndirectDraw`）；顶点流转换不再按 first+count 截断（`WireDraw.inc:326`） |
| 发射 | 同 monolith 的三档：COUNT 原生、`multiDrawIndirect` 一次发、否则逐条；另按 `maxDrawIndirectCount` 分块（`WireDraw.inc:719-740`） |
| barrier | `MarkWireBufferGpuWritten` 置 `indirectReadBarrierPending`（`VkBufferManager.cpp:611`），`TakeWireIndirectReadBarrier`（`VkBufferManager.cpp:624`）取走；有则结束 render pass 并记 `ALL_COMMANDS/MEMORY_WRITE → DRAW_INDIRECT/INDIRECT_COMMAND_READ`。transfer 写已有 `MEMORY_READ` 后置 barrier（`VkBufferManager.cpp:514`、`:1437`），不置位。应用自己的 `glMemoryBarrier(GL_COMMAND_BARRIER_BIT)` 仍走 `BuildMemoryBarrierForGlBarriers`（`VulkanRenderer.cpp:7873`），与 monolith 一样 |
| G1 | 全部在 `MOBILEGL_BUILD_DISAGGREGATED` / `MOBILEGL_PIPE_PUSH` 下；pull 构建 `added=0 removed=0` |

## 5. 门与 red-once

用例 `WireIndirectDrawScenario`（`MobileGL/MG_IntegrationTest/Scenarios/WireIndirectDrawScenario.cpp:225`、`:274`、`:304`、`:343`），读数经 `MobileGL/MG_IntegrationTest/Harness/WireIndirectPeek.cpp:22`（进程内 PipeStats，只在 inproc 臂看得到 server）。登记：`MobileGL/MG_IntegrationTest/CMakeLists.txt:4831-4864`——Magma 门控三臂 `DirectVulkan.{Split,Spawn,Tcp}.Indirect.`、DirectGLES 三臂（经宏，含 Magma 信息层）、两后端 monolith（环境发现）。

| 用例 | 断言 | 两后端 / 各臂 |
|---|---|---|
| `EveryIndirectFormIsIssuedNativelyFromItsStore` | 6 种 indirect 形态像素；inproc Magma：wind=6、wixp=0、whw=0、wibar=0 | 两后端全臂 |
| `ComputeWrittenCommandsDrawWithoutAHostWait` | compute 写命令（初值毒化为别的条带）+ 应用 barrier，像素；inproc Magma：wind=2、wixp=0、whw=0、wibar=2 | 两后端全臂 |
| `ComputeWrittenCountWordIsTheOneTheGpuReads` | compute 写 count 字，像素；inproc Magma 计数 | Magma；Espryt 跳过（count 字读 server 影子，OQ15 / P8-C）；Magma monolith 跳过 arrays-count 半（§6） |
| `ShaderWrittenCommandsWithoutAnApplicationBarrierAreOrdered` | Flywheel 形状（无应用 barrier）；inproc Magma：每次写恰好 1 个 barrier（wibar=2 / 3 次 draw） | Magma split 三臂；monolith 与 Espryt 跳过 |

| red-once | 变异 | 载体 | 观察 |
|---|---|---|---|
| 1 | `DrawWireIndirectNative` 开头 `return false`（强制 CPU 路径） | 4 用例 × 3 个 inproc 登记（`.Split.Indirect.` 门控、`.Split.` 信息层、`.Split.Full.`） | 12 红；`ComputeWrittenCommandsDrawWithoutAHostWait`：nativeDraws 0、cpuExpansions 2、indirectWaits 2、hostWaits 2；spawn/tcp 只断言像素，保持绿（CPU 路径像素是对的） |
| 2 | 去掉 barrier（`if (false && ...)`） | 同上 | 3 个 compute 写用例 × 3 个 inproc 登记 = 9 红，全部是 `delta.barriers` 0 ≠ 2；**像素在 llvmpipe 上不红**（lavapipe 按序执行），spawn/tcp 绿 |
| 3 | 去掉 Magma monolith 的 arrays-count 跳过 | `DirectVulkan.WireIndirectDrawScenario.ComputeWrittenCountWordIsTheOneTheGpuReads` | 红：compute 写的 count=2，条带 1、2 未画（monolith 读到陈旧影子 count 0） |
| fixture 门 | 改前 = 旧 CPU 路径 | create-indirect × Magma × {inproc, spawn} | whwi 321 / 322 > 0（§2 改前行）；改后 0 |

## 6. monolith 缺陷（记给 dev，ID-P8-3，本包不修）

| 缺陷 | 位置 | 证据 |
|---|---|---|
| Magma monolith `glMultiDrawArraysIndirectCount` 从前端影子读 count 字，不 `SyncGpuWrites`：着色器写的 count 读成旧值 | `DirectVulkan.cpp:749-757` | red-once 3（主机 lavapipe 红） |
| Magma monolith 对"compute 写 → indirect 读"没有任何依赖：不补 barrier，render pass 的外部依赖也不含 `COMPUTE_SHADER` / `DRAW_INDIRECT`（`MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp:1413-1419`）。create-indirect 的 320 次 cull→draw 不发 `glMemoryBarrier`（GL 未定义），真 GPU 上 cull 结果与 indirect 读竞争 | `VulkanRenderer.cpp:7727-7787`（dispatch 后无 barrier） | 推断：主机 lavapipe 按序执行，red 不出；可能与 ID-P7-4（红米上 create-indirect monolith 坏）有关，需设备验证 |

## 7. 找到但未做

- `DispatchComputeIndirect` 的 wire 臂同样经 `ReadWireBuffer` 在 CPU 读 group 数（`DirectVulkan.cpp:938-947`），GPU 写过则整 GPU 等待；create fixtures 里没有 `glDispatchComputeIndirect`，本包未改（monolith 是原生 `vkCmdDispatchIndirect`）。
- 补的 barrier 只覆盖 indirect 读；同一批 cull 写的实例 SSBO 被顶点着色器读时，两臂都没有 compute→vertex 的依赖（应用没发 barrier 时），不在 D 范围。
- 同一 store 既作 indirect 又作本 draw 的 SSBO 时，SetupWireDraw 解析描述符会再次置位，下一次 indirect draw 会多一个 barrier（保守，不影响正确性）。
