# P8-SV：Magma split 残项

> 2026-09-30，分支 `p8/sv`（基线 `cbbaf77f`，第一波集成头）。计划：[`PLAN-P8.md`](PLAN-P8.md) 第二波 SV；来源：[`F.md`](F.md)「发现但未修」三条、[`D.md`](D.md) §7 第一条。主机 lavapipe，只动 Magma（Espryt 作对照）。

## 1. 结论

| 项 | 改动 | 位置 | 之前 | 之后 |
|---|---|---|---|---|
| 1 | 3D 原生 mip blit 的子资源 `layerCount` 改为 1，深度只走 z 偏移（逐级减半，原本就对） | `WireFramebuffer.inc:1626-1636` | `layerCount = Depth >> level`（`VkTextureManager.cpp:2738-2740` 的切片寻址）当层数，图像 `arrayLayers = 1`；验证层报 `VUID-vkCmdBlitImage-srcImage-00240`、`-srcSubresource-01707`、`-dstSubresource-01708` | 与 monolith 同形（`VulkanRenderer.cpp:12303`、`:12403`）；验证层 0 条 |
| 2 | 深度模板 mip 拒绝补 `WireDeclineTally` 行 | `WireDeclines.def:124`；站点 `WireFramebuffer.inc:1401` | 只有 `MGLOG_E_ONCE`，`MGWIRE-DECLINES` 看不到 | `MGL_WIRE_DECLINE_AT(MipmapDepthStencilAspect, …)`，日志原文保留 |
| 3 | 深度着色器 mip 拒 1D / 1D 数组 / 3D | 生长前判定 `WireFramebuffer.inc:1426`、`:1437`；pass 自身 `WireDepthMipmap.inc:67-74`；调用方传 view 类型 `WireFramebuffer.inc:1491` | 只拒 `is3D`，调用方从不设置；1D 深度链进 pass，对 1D 图像建 2D view（`WireDepthMipmap.inc:244`） | 生长前具名拒绝 `MipmapShaderFormatOrShape`（`WireFramebuffer.inc:1465`）；pass 的拒绝是漂移保护（`:1508-1513`） |
| 4 | wire 臂 `glDispatchComputeIndirect` 原生化 | `DispatchWireComputeIndirect`（`WireDraw.inc:789`），入口 `VulkanRenderer.cpp:7790-7797`，`DirectVulkan.cpp:938-945` | `ReadWireBuffer` 在 CPU 读 3 个字，shader 写过则整 GPU 等待 | `vkCmdDispatchIndirect`（`WireDraw.inc:844`）；不再有 CPU 路径 |

## 2. 设计

| 项 | 内容 |
|---|---|
| 1 | `ResolveWireTextureStorage` 不动：Tex3D 的 `layers` 是附件、描述符、回读共用的切片寻址；只在 blit 处按 `resource->viewType == 3D` 改用 `{层 0, 1 层}` |
| 2 | 行追加在 `WireDeclines.def` 末尾（只追加）；宏同时计数与 `_ONCE` 日志 |
| 3 | 两层：生长前 `depthArmTakesShape` 加 `planar`（与颜色臂同一谓词，决定在 `GrowWireTextureMipChain` 之前，`WireFramebuffer.inc:1469`）；pass 按 `WireImage::viewType` 再拒一次。monolith 调用方（`VulkanRenderer.cpp:4866`、`:4905`）不设 viewType，行为不变 |
| 4 | 形状同 monolith（`VulkanRenderer.cpp:7862`），store 取自 `VerbDispatchIndirectBuffer`（`AcquireWireSlice` 打 `lastUseSerial`） |
| 4 屏障 | P8-D 规则：store 被标 shader 写过（`VkBufferManager.cpp:603`）则首个 indirect 读前录一次 `ALL_COMMANDS/MEMORY_WRITE → DRAW_INDIRECT/INDIRECT_COMMAND_READ`（`WireDraw.inc:808`）；在绑定本次 dispatch 的 SSBO **之前** 取标，否则本次 dispatch 若把参数 store 也当输出，会吃掉下一个读者的标 |
| 4 拒绝 | store 缺失 `DispatchIndirectBufferUnbound`、越界 / 未 4 对齐 / 负偏移 `DispatchIndirectRange`（`WireDeclines.def:127-128`）；程序、纹理、管线、绑定四处拒绝复用 `DispatchWireCompute` 的行 |
| 4 计数 | 新 `wdsp`（`PipeStats.h:289`、`PipeStats.cpp:706`），屏障计入 D 的 `wibar`，等待仍是 `whw`；`WireIndirectCounters::nativeDispatches` 追加在结构末尾 |
| G1 | 全部在 `MOBILEGL_BUILD_DISAGGREGATED` / `MOBILEGL_PIPE_PUSH` 下 |

## 3. 门

| 用例 | 断言 | 登记 |
|---|---|---|
| `WireMipShapeScenario.VolumeChainIsGeneratedAlongItsDepth`（`WireMipShapeScenario.cpp:82`） | 8³ RGBA8，层 0 按切片对着色；1..3 级每片等于上一级两片均值；`GL_TEXTURE_DEPTH` 逐级 | 两后端 monolith + 各 split 臂；Magma 门控 `.Mip3D.` 三臂 |
| 同上，`.Mip3DValidation.` | `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`，输出含 `VUID-vkCmdBlitImage-` 即红（`FAIL_REGULAR_EXPRESSION`）；用例从 `/proc/self/maps` 证实层已加载（`WireMipShapeScenario.cpp:51`、`:156`） | 只 inproc（`MobileGL/MG_IntegrationTest/CMakeLists.txt:4931-4937`），`spawn_lane_parity.py:67-72` 的 `MAGMA_INPROC_ONLY`；CI 跑 `integration-magma-split` 的作业装了验证层（`.github/workflows/test.yml:1700`、`:1986`） |
| `WireMipShapeScenario.Depth1DChainWithoutANativeBlitDeclinesByName`（`WireMipShapeScenario.cpp:167`） | 1D `DEPTH_COMPONENT32F`，`MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1`：无 GL 错误、两次回读、层 1 仍是哨兵；inproc 上 `MipmapShaderFormatOrShape` +1 | Magma `.ShaderMipDepth1D.` split + spawn（server 读旋钮），`spawn_lane_parity.py:150` |
| `F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession` | 原像素断言 + inproc 上 `MipmapDepthStencilAspect` +1（`F1WireScenario.cpp:663-683`，经 `WireDeclinePeek.cpp:23`） | 原登记不变 |
| `WireIndirectDispatchScenario` 三例（`WireIndirectDispatchScenario.cpp:219`、`:239`、`:267`） | 网格逐 work group 标记；inproc Magma：`wdsp` = 调用数、`whw` = 0、`wibar` = 写次数（读数在结果回读之前取） | 两后端各臂；Magma 门控 `.IndirectDispatch.` 三臂；第三例（无应用屏障）只 Magma split 臂 |

## 4. red-once（全部执行并还原：拷贝比对 + touch）

| # | 变异 | 载体 | 观察到的红 |
|---|---|---|---|
| R1 | blit 改回 `{baseLayer, layerCount}` | `.Mip3D.` 三臂、`.Mip3DValidation.`、两后端 monolith / 各臂 | `.Mip3DValidation.` 红：`Error regular expression found`，三条 VUID 各 6 次；像素用例全绿（lavapipe 3D blit 按 z 偏移、不看 layerCount） |
| R2 | 站点改回裸 `MGLOG_E_ONCE` | `F1WireScenario.GenerateMipmapDepthStencilDeclines*` 各臂 | inproc 两例红（`.Split.Fm.`、`.Split.Full.`）：计数差 0 ≠ 1；spawn / tcp 绿 |
| R2b | 同上，静态 | `wire_declines_audit.py` | rc 1：`row MipmapDepthStencilAspect has NO SITE` |
| R3 | 生长前 `planar` 与 pass 的 view 类型拒绝都去掉 | `.ShaderMipDepth1D.` split、spawn | 两例红：层 1 被写成 0.25（哨兵 0.75），无拒绝计数；pass 在 1D 图像上建 2D view 仍产出像素 |
| R3b | 只去生长前判定 | 同上 | 绿：pass 拒绝兜住（`the depth pass refused level 1`，计数 1），漂移保护有效 |
| R4 | wire 分支恢复 `ReadWireBuffer` + CPU dispatch | `WireIndirectDispatchScenario` 各臂 | inproc 9 例红（3 例 × `.Split.IndirectDispatch.` / `.Split.` / `.Split.Full.`）：`wdsp` 0、GPU 写过的 `whw` 2、`wibar` 0；spawn / tcp / monolith 绿（CPU 路径网格正确） |
| R5 | 去屏障（`if (false && …)`） | 同上 | inproc 6 例红（两个 shader 写用例 × 3 登记）：`wibar` 0 ≠ 2；网格在 lavapipe 上不红（按序执行），spawn / tcp 绿 |
| R6 | `wdsp=` 改成 `wds=` | `PipeStatsTest.WireIndirectCountersPrintInTheirOwnBracket` | 红 |
| R7a | 删 `.ShaderMipDepth1D.` 例外行 | `spawn_lane_parity.py build-split` | rc 1：tcp 臂缺该例 |
| R7b | 删 `.Mip3DValidation.` 例外行 | 同上 | rc 1：spawn、tcp 两臂缺该例 |

验证层后验：`.Split.IndirectDispatch.` 三例、`.Split.ShaderMipDepth1D.`、`.Split.Mip3D.`、`.Split.Fm.` 深度模板例在层下全绿，VUID 0 条。

## 5. 整套门（`~/w7/notes/p8/gate.sh`，树外）

| 项 | 读数 | 相对第一波集成头（ID-P8-12） |
|---|---|---|
| G1 | 符号增 0 减 0，`.text` `a5ba13` | 不变 |
| census / ratchet / parity / 修订 | 78 / 20 / 45；172；0；5 | 不变 |
| wire-declines | 61 行、66 站点、0 未记日志 | +3 行（`MipmapDepthStencilAspect`、`DispatchIndirect{BufferUnbound,Range}`），+7 站点（新派发函数另复用 4 个 `Compute*` 行） |
| split_coverage | Espryt monolith 706 = 门控 617 + 豁免 89；Magma 706：门控 227、只在信息层 447、豁免 32；missing 0 | +5 例（2 mip + 3 派发），无新豁免行 |
| 车道 | unit 2721；split / spawn / tcp 740 / 654 / 666；Magma 277 / 253 / 220 / 673；gpu monolith / inproc 3709 / 3709，全部 100 % | +5 / +5 / +5；Magma +6 / +5 / +4 / +5；gpu +25 |
| retrace（`2edb2aa8`，只改测试的提交之前） | 230 / 236，只挂已知 6 例 iterationrp | 同第一波 |

## 6. monolith 观察（记给 dev / P13，本包不修）

| 观察 | 位置 | 证据 |
|---|---|---|
| split 构建的 monolith 臂深度着色器 mip 同样不传 view 类型：设备无深度 BLIT_DST 时，1D 深度链会对 1D 图像建 2D view；Release 下 `MOBILEGL_ASSERT` 是空的 | `VulkanRenderer.cpp:4905`、`:12321` | 推断：lavapipe 原生 blit 深度，主机红不出；monolith 不读 `MGITEST_MAGMA_FORCE_SHADER_MIPMAP` |

## 7. 发现但未修

- `mipmap-shader-format-or-shape` 的明细对深度形状也打印 `missing=COLOR_ATTACHMENT`（1D 深度的真实原因是 `view=0`）；是 F 的 `MagmaWireShapeDetail` 口径，未改。
- create 系列 fixture 没有 `glDispatchComputeIndirect`（[`D.md`](D.md) §7），item 4 的真实负载收益未测。
- `gen_pipe_dirty_surface.py` 的「根外写者」扫描读整个 `MobileGL/`（`gen_pipe_dirty_surface.py:394`），集成测试源也在内：派发用例的 fixture 成员曾叫 `m_parameters`，让 NEW_PATCH_STATE 的 shutter 看起来在根外被写，自检否定对照 16（`gen_pipe_dirty_surface.py:1770-1778`）不再触发、门红。本包把成员改名 `m_groupCounts`；扫描口径未改，任何新测试 fixture 用到 GLContext 的成员名都会再撞上。
