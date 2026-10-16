# P8-F 重分类与死闩清理

> 包说明：`~/w7/notes/p8/BRIEF-F.md`（树外）。基线 `8d3e2317`，分支 `p8/f`。我们自己的 client 行为不变：四项都是它发不出的形状，或只改注释。

## 结论

| 项 | 改动 | 位置 | 之前 | 之后 |
|---|---|---|---|---|
| 1 | multi-draw 的两个 server 闩改族 | `PipeApplier.cpp:779-784`、`:976-981` | `Fatal{UnmigratedVerb, "MultiDraw{Arrays,Elements}+{CLIENT_INDICES,INSTANCED}"}` | `Fatal{ProtocolCorruption, <同名>}`，仍按名闩住外来 peer |
| 2 | `Magma:client-vertex-array` 去掉 P8 阶段标签，改为协议不变量 | `WireDraw.inc:300-307` | `MagmaWireFatal`（UnmigratedVerb 族，名字带 `@P8`） | `MGPipeSessionFail(ProtocolCorruption)`，名字 `Magma:client-vertex-array` 加 location |
| 3 | Magma 生成 mip 的"无着色器臂形状"改为具名拒绝 | `WireFramebuffer.inc:1405-1459`（判定）、`:1456`、`:1500`（两个拒绝点） | wire 臂三处 Fatal（旧 `:1427`、`:1452`、`:1489-1493`） | `WireDeclines.def:112` 的 `MipmapShaderFormatOrShape`；判定在 `GrowWireTextureMipChain`（`:1460`）之前，拒绝时不增长 mip 链 |
| 4 | `kCapViewportArray`、`kCapNeedsHostIndexBytes`、`kCapNeedsHostUboBytes` 与 index-mirror 哨兵标为保留位 | `MGPipeTypes.h:110`、`:117-123`、`:315-320`、`:332-335`、`:994-999`、`:1007-1009`；`MGPipeHostSpan.h:16-26`；`VertexInputEmit.h:266-267`；`ShaderBufferEmit.h:250-253`；`MG_Backend/Init.cpp:189-191` | 注释说"P5 期间为 0"或"arms the index host mirror" | 注释说 RESERVED、从不发布、位不复用不重编号；只改注释 |

## 各项依据

| 项 | 事实 | 出处 |
|---|---|---|
| 1 | client 把 multi-draw 的 client 索引拼成一个自有 EBO | `EmitTables.cpp:688-709` |
| 1 | 自有 EBO 生效时清掉 `kDrawHasUserIndices` | `OwnedDrawInputs.h:45-57` |
| 1 | multi-draw 计划的实例数恒为 1、base instance 恒为 0 | `EmitTables.cpp:686` |
| 1 | 四个名共用一个 `SessionLatch` 格式串，`PeerLatchTest` 的一行覆盖整组 | `PipeApplier.cpp:779-784`、`PeerLatchTest.cpp:1162-1165` |
| 1 | 族选 `ProtocolCorruption`：它的桶就是"peer 发了协议不承载的东西" | `FatalFamilies.def:26-29` |
| 2 | client 把每个启用的 client 内存数组转成自有 buffer，指针为空就拒绝整个 draw | `OwnedDrawInputs.h:102-131` |
| 2 | `fatal_census.py` 只数 `Fatal{Word` 族词与 abort 点，不按 `@P` 字面计数 | `fatal_census.py:165-167` |
| 2 | 普查前后：79 个 abort 点、20 个文件、45 个族词，均不变；不重写基线 | 本包门禁读数 |
| 2 | 树内 `@P8` 字面：1（`WireDraw.inc:300`，基线）→ 0 | `git grep '@P8' -- MobileGL scripts tools` |
| 3 | monolith 臂对无 BLIT 的格式一直是拒绝（跳过、链不写） | `VulkanRenderer.cpp:12228-12232` |
| 3 | 增长前后格式、特性、knob 档位、aspect、view 类型、usage、层窗口都不变 | `VkTextureManager.cpp:2884-2995` |
| 3 | 深度 pass 对本函数交给它的图像只会因"格式不能同时采样和作深度附件"拒绝 | `WireDepthMipmap.inc:58-81` |
| 3 | 深度 pass 若仍拒绝（两份清单漂移），走同一行拒绝，不结束会话 | `WireFramebuffer.inc:1491-1502` |
| 4 | 生产者 0：唯一的 `SetCapabilityBits` 只置 XFB、query、resident、run-ahead 位 | `MG_Backend/Init.cpp:202-255` |
| 4 | 消费者 0：`kCapViewportArray` 在树内只有枚举定义；`kMGPipeBindElementArray` 只在 bind mask 表里被置 | `MGPipeTypes.h:110`、`ResourceTracker.h:95` |
| 4 | `0xFFFFFFFF` 哨兵无编码方；`SegmentTable::Resolve` 把它当未知段 | `MGPipeHostSpan.h:26`、`PipeWireCodec.cpp:458-465` |
| 4 | pull 构建只改注释，逐文件行数不变；G1 `added=0 removed=0`，`.text` 与基线同为 `a5ba13` | 本包门禁读数 |

## 新门与 red-once

| 项 | 门 | 变异 | 载体 | 观测到的红 |
|---|---|---|---|---|
| 1 | `PeerLatchTest` 行 `MultiDrawInstanced` 期望 `Fatal{ProtocolCorruption, "MultiDrawArrays+INSTANCED"}` | 闩的族改回 `UnmigratedVerb` | `FuzzArm2/PeerLatchSite.*/MultiDrawInstanced`、`PeerLatch.SiteMap` | 两例红：首行 `Fatal{UnmigratedVerb, ...}` 不含期望标记；站点图报该站点未映射 |
| 3 | `F1WireScenario.GenerateMipmapWithoutAShaderArmShapeDeclinesAndKeepsTheSession`（3D RGBA8，`MGITEST_MAGMA_FORCE_SHADER_MIPMAP=1`） | 拒绝改回 `MagmaWireFatal` | `DirectVulkan.{Split,Spawn}.ShaderMipDecline.` 两例（当时前缀为 `.ShaderMip1.`，后改名，见下） | 两例 `Subprocess aborted`，server 日志 `Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape format=37 extent=4x4x4 ..."}` |
| 3 | 同上（前提） | 无（新用例落在未改的基线代码上） | 同上 | 同样两例 abort：前提成立 |
| 3 | `spawn_lane_parity.py` 的 `.ShaderMipDecline.` 例外行 | 删掉该行 | `spawn_lane_parity.py build-split` | rc 1：`the tcp arm does not match the split arm: missing=['DirectVulkan.ShaderMipDecline.F1WireScenario.GenerateMipmapWithoutAShaderArmShapeDeclinesAndKeepsTheSession']` |
| 2 | 普查读数 | — | `fatal_census.py` | 无变化：该脚本不计阶段标签（见上） |

- 新用例只登记 split + spawn：knob 由 server 读，tcp fixture 带不过去。登记在 `MobileGL/MG_IntegrationTest/CMakeLists.txt:3484-3502`，前缀 `.ShaderMipDecline.`。
- 该尾自成一行加进 `MAGMA_SERVER_ENV_KNOB_NO_TCP`（`spawn_lane_parity.py:141-143`）：脚本要求每个尾恰好命中一个用例，借用 `.ShaderMip1.` 会命中两个，门禁时 parity 报 1 个错，已改。
- Espryt 不涉及第 2、3 项（Magma 专属）；新用例在 DirectGLES 上按后端名跳过。第 1 项在后端无关的 applier 里。

## 发现但未修

| 发现 | 证据 | 影响 |
|---|---|---|
| Magma wire 原生 blit 生成 3D mip 时把 depth 当层数传给 `vkCmdBlitImage`（layerCount=4，图像 arrayLayers=1） | `WireFramebuffer.inc:1612`、`VkTextureManager.cpp:2738-2741`；验证层报 `VUID-vkCmdBlitImage-srcImage-00240`、`-srcSubresource-01707`、`-dstSubresource-01708`（临时探针，未提交） | 只在 split；lavapipe 容忍；其他驱动上 3D 生成 mip 行为未定义 |
| 深度/模板生成 mip 的拒绝只记日志，不计入 `WireDeclineTally` | `WireFramebuffer.inc:1397-1404` | `MGWIRE-DECLINES` 看不到这类拒绝 |
| `GenerateWireDepthMipLevel` 的注释说会拒绝 1D，代码只拒绝 `is3D`，而调用方从不设置它 | `WireDepthMipmap.inc:66`、`WireFramebuffer.inc:1495` | 无原生 blit 的 1D 深度链会进入 pass（推断，未测） |

- monolith 缺陷：无。
