# P8-B：Espryt `glGenerateMipmap` 在 split 下（B1 生成窗口、B2 RGB16F / RGB32F）

> 2026-09-29，分支 `p8/b`（基线 `8d3e2317`），代码提交 `8dac1fd8`。计划：[`PLAN-P8.md`](PLAN-P8.md) 包 B。

## 1. 结论

| 名 | 基线（split 臂） | 本包之后 | 位置 |
|---|---|---|---|
| `generate-mipmap-storage`（split 检查） | 描述符层数少于「从 level 0 起的整链」即 `Fatal{UnmigratedEmulation}`：MAX_LEVEL 偏小、`glTexStorage2D` 层数不足（合法 GL） | 删；生成窗口 = 记录的计划 | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:11759` |
| `generate-mipmap-cpu-filter` | RGB16F / RGB32F 生成一律 Fatal | 删；先原生 `glGenerateMipmap`，驱动拒绝或旋钮强制时 server 从自己的层做 CPU 滤波 | `DirectGLES.cpp:13091`、`:13209` |

- split 臂的 `UnmigratedEmulation` 可达名清零；树里剩 4 名（`copy-image-shadow-mirror`、`generate-mipmap-storage` 的 monolith 生长、`generate-mipmap-cpu-fallback`、`get-tex-image-shadow`），都只在 monolith 臂上。
- `PipeCatalogueTest.cpp:1451` 名单保持 4 名，注释补记漂移（它从未列过 `cpu-filter`）与本包的两处退役。
- 主机（llvmpipe）上 RGB16F（原生存储）与 RGB32F（加宽为 RGBA32F）都由原生 `glGenerateMipmap` 完成；CPU 臂只在旋钮下走到（日志核实：非强制条目 0 行 `generate-mipmap-server-cpu`）。

## 2. 设计

| 项 | 做法 | 位置 |
|---|---|---|
| B1 窗口 | `Base = VerbMipBaseLevel`；`End = min(VerbMipLevelCount, Desc.Levels, MaxLevel+1, Base + 基准层的整链)`，与 Magma 的裁剪同式（`WireFramebuffer.inc:1358`）；client 已按同一窗口定义层（`EmitTables.cpp:384-390`） | `DirectGLES.cpp:11759` |
| B1 两条 blit 链 | 深度 / R11F 的记录臂改为只跑 `[Base+1, End)`，不再从 level 1 跑到描述符末尾（BASE_LEVEL=1 时会用 level 0 覆盖 base） | `DirectGLES.cpp:12523`、`:12540` |
| B1 死臂 | `EnsureGenerateMipmapStorageAllocated(texture)` 的 transport 臂删除：split 下 `GenerateMipmap` 在它之前已经返回 | `DirectGLES.cpp:11781` |
| B2 原生 | 三通道浮点也先走原生 `glGenerateMipmap`；错误不转给应用，改走 CPU 臂 | `DirectGLES.cpp:13258` |
| B2 CPU 臂 | 源 = 驱动上的 base 层（`ReadTextureLevelTight`，RGB/FLOAT）；驱动读不回（不可附着的存储）才用 staged store，且 store 须整层覆盖；都没有则具名拒绝（`MGLOG_E_ONCE`，不 Fatal）；2×2 盒滤波（monolith 同式）；按驱动存储的通道数 `glTexSubImage2D`（RGB 或 RGBA，FLOAT）；支持 2D 与立方体，其余目标具名拒绝 | `DirectGLES.cpp:13036`、`:13091` |
| B2 旋钮 | `MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1`，server 读（`MGITEST_MAGMA_FORCE_SHADER_MIPMAP` 先例）；命中打一行 `generate-mipmap-server-cpu`（INFO，每进程一次） | `DirectGLES.cpp:13017` |
| B2 client 影子 | 任何臂都不读 client 影子 | — |

未改：`PipeCalls.def:260` 的 `GenerateMipmap` 仍是 `kWaitApplied`。P5e 裁定 14 留着它的唯一理由（CPU 滤波读 client 影子）已消失；翻成 `kWaitNone` 是协议改动，交集成方。

## 3. 覆盖（`GenerateMipmapServerScenario`）

用例：基准层分上下两半（A / B），逐层 `texelFetch` 到 RGBA8 回读；窗口外的层检查未变或仍未定义。

| 用例 | 形状 | 行 |
|---|---|---|
| `PackedFloat` / `Depth` × `MutableMaxLevelBelowTheChain` | 可变，定义 0..2，MAX_LEVEL=2；level 3 须未定义 | `GenerateMipmapServerScenario.cpp:307` |
| `PackedFloat` / `Depth` × `StorageWithFewerLevelsThanTheChain` | `glTexStorage2D` 3 层（整链 5） | `GenerateMipmapServerScenario.cpp:326` |
| `PackedFloat` / `Depth` × `MutableBaseLevelOneKeepsTheLevelsOutsideItsWindow` | 定义 0..3，BASE=1、MAX=2；level 0/3 不变，level 4 未定义 | `GenerateMipmapServerScenario.cpp:341` |
| `Rgb16f` / `Rgb32f` × `UploadedBaseGeneratesItsChain` | 仅上传 level 0 | `GenerateMipmapServerScenario.cpp:366` |
| `Rgb16f` / `Rgb32f` × `RenderTargetBaseGeneratesFromTheGpuWrittenLevel` | 上传哨兵色后把 level 0 当渲染目标绘制（Complementary colortex2 形状） | `GenerateMipmapServerScenario.cpp:382` |

| 臂 | 登记 | Espryt | Magma（对照） |
|---|---|---|---|
| monolith | 环境发现 `DirectGLES.` / `DirectVulkan.` | 4 绿 + 6 按名跳过（M1、M2） | 7 绿 + 3 按名跳过（M3） |
| Split / Spawn / Tcp | `MobileGL/MG_IntegrationTest/CMakeLists.txt:3442`；Magma 门控块 `MobileGL/MG_IntegrationTest/CMakeLists.txt:4174` | 10 × 3 绿 | 10 × 3 绿 |
| 强制 CPU（Split / Spawn） | `MobileGL/MG_IntegrationTest/CMakeLists.txt:3443-3459`，标签 `integration-cpu-mip-{split,spawn}` | 4 × 2 绿，server 日志须有 `generate-mipmap-server-cpu`（`GenerateMipmapServerScenario.cpp:295`） | — |
| A/B inproc（门的 `integration-gpu[inproc]` 环境） | 同环境发现 | 10 绿 | 10 绿 |

- 强制 CPU 臂不上 tcp：tcp 的 server 是共享的 `TcpServer.Start` 夹具（`MobileGL/MG_IntegrationTest/CMakeLists.txt:2168`），旋钮放那里会让所有 tcp 条目都走 CPU 臂、原生臂在 tcp 上失去覆盖；CPU 臂本身与传输无关（spawn 已证跨进程）。与 P9 `RemintFallback.` 同形，不需要 parity 例外行。

## 4. red-once

| # | 变异 | 载体 | 观察到的红 |
|---|---|---|---|
| 1 | 基线 `8d3e2317`（只加用例） | `DirectGLES.{Split,Spawn,Tcp}.GenerateMipmapServerScenario.*` | 30/30 abort：B1 六例 `Fatal{UnmigratedEmulation, "generate-mipmap-storage"}`，B2 四例 `Fatal{UnmigratedEmulation, "generate-mipmap-cpu-filter"}`；`CpuMip.` 8/8 abort |
| 2 | CPU 臂入口放回 `cpu-filter` 闩（去掉新臂） | `DirectGLES.{Split,Spawn}.CpuMip.*` | 8/8 Subprocess aborted |
| 3 | CPU 臂跳过驱动回读、改读 store | 同上 | 4/8 Failed：两个渲染目标用例 × 两臂读到哨兵色（陈旧） |
| 4 | server 不再读旋钮 | 同上 | 8/8 Failed：像素对（原生臂），server 日志无 `generate-mipmap-server-cpu` |
| 5 | 两条 blit 链改回 `1 .. Desc.Levels` | `DirectGLES.{Split,Spawn,Tcp}.GenerateMipmapServerScenario.{PackedFloat,Depth}*` | 6/18 Failed（另 2 条为 tcp 夹具）：`MutableBaseLevelOne` 两格式 × 三臂 |
| 6 | 窗口不按计划 / MAX_LEVEL 裁剪（`End = Desc.Levels`） | 同上 | 6/18 Failed：同 5 |
| 7 | `MGITEST_RUN_MONOLITH_MIP_DEFECTS=1` | `Direct{GLES,Vulkan}.GenerateMipmapServerScenario.*`，`MOBILEGL_TRANSPORT=monolith` | 9/20 Failed：M1 两例、M2 四例、M3 三例（§5） |

恢复一律复制比对 + `touch`（`cmp` 一致），每次恢复后重建。

## 5. monolith 缺陷（记给 dev，ID-P8-3；用例在该臂按名跳过，旋钮 `MGITEST_RUN_MONOLITH_MIP_DEFECTS=1` 复现）

| # | 后端 | 缺陷 | 位置 | red-once 读数 |
|---|---|---|---|---|
| M1 | Espryt | RGB16F / RGB32F CPU 滤波读前端层影子；level 0 被 GPU 写过后影子陈旧 | `DirectGLES.cpp:12927`（调用 `:13320`） | level 1..3 = 哨兵色 `rgba(255,0,255,255)`，两格式 |
| M2 | Espryt | R11F / 深度：存储生长从 level 0 算整链，定义 MAX_LEVEL 之外的层；两条 blit 链从 level 1 跑到末尾，无视 BASE_LEVEL / MAX_LEVEL | `DirectGLES.cpp:11707`、`:12580`、`:12610` | MAX_LEVEL=2 时 level 3 宽 2（应 0）；BASE=1 时 level 1..3 全被 level 0 覆盖，level 4 被定义 |
| M3 | Magma | 无 BLIT 的颜色格式（lavapipe 上 `B10G11R11_UFLOAT`，VkFormat 122）直接跳过生成，只打 `MGLOG_W_ONCE`；wire 臂自 P7 有着色器臂，monolith 没有 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:12229` | 三个 PackedFloat 用例生成层读到 `rgba(0,0,0,255)` |

- M1 修法指向：monolith 也改为回读驱动层（P9 dev `568090f0` 的 `AdoptDriverLevelIntoShadow` 同路）。M2：monolith 两条链与生长改用 `ComputeMipmapGenerationRange` 窗口。M3：monolith 复用 wire 臂的着色器 mip。都动 pull 构建的 `.text`，按 G1 在 dev 修。

## 6. 门

`gate.sh ~/w7/p8-b p8b`，头 `1bb77890`，dirty 0：

| 项 | 读数 |
|---|---|
| G1 | `added=0 removed=0`（clang 22.1.6）；`.text` a5ba13，与建树时相同 |
| census / ratchet | 79 个 abort 点、45 个族词，与基线同：`MGPipeUnmigratedEmulation` 的调用点不是 abort 点（abort 在 `PipeApply.cpp` 的漏斗里），删两个名不改计数，基线不动；ratchet 172 不变 |
| parity / protocol pin / wire-declines | 0 错；revision 5；53 行 0 未记 |
| hygiene | 12/12 OK |
| unit | 2714/2714 |
| integration-split / spawn / tcp | 437 / 356 / 358，全绿 |
| integration-magma-split / spawn / tcp / full-split | 261 / 238 / 207 / 646，全绿 |
| integration-gpu[monolith] / [inproc] | 2746 / 2746，全绿 |
| retrace（`MOBILEGL_BUILD_TRACE_REPLAY=ON`，236 条） | 230 过；6 条 `iris-iterationrp-in-world`（两后端 × monolith / SPLIT / SPAWN）为已知主机 llvmpipe JIT 崩溃；全部重放日志 0 行 `generate-mipmap-*` |

## 7. 发现未修

| 项 | 说明 |
|---|---|
| `PipeCatalogue` 名单无门 | 名单与树里的调用点没有任何脚本比对，漂移只能靠人发现；`fatal_census` 数的是 abort 点，不数 `MGPipeUnmigratedEmulation` 调用 |
| CPU 臂的目标 | 2D 数组 / 3D / 立方体数组的 RGB16F / RGB32F 若驱动拒绝原生生成，CPU 臂具名拒绝（monolith 的 CPU 滤波对这些目标同样只按 2D 取址） |
| 层间精度 | CPU 臂层间以 float 传递，不在每层回量化到 half；GL 不规定滤波，与 monolith（每层从 half 影子读）可能差 1 ulp |
