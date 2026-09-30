# P8-SE：Espryt split 残项与 P8 包说明的行号漂移

> 2026-09-30，分支 `p8/se`（基线 `cbbaf77f`）。简报 `~/w7/notes/p8/BRIEF-SE.md`；证据 `~/w7/notes/p8/se-evidence/`（树外）。第二波，ID-P8-13。

## 1. 结论

| 项 | 结果 | 位置 |
|---|---|---|
| 1 默认帧缓冲深度 / 模板在 Espryt spawn / tcp 读 0 | 修；A 的 pending-fix 豁免行删去，用例登上三臂（含 `ForcedDs.` 三臂） | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16239` |
| 2 B2 的 server CPU mip 臂 × E 的驱动写标 | 修；驱动读回被拒且纹理被驱动写过时具名拒绝 `generate-mipmap-store-declined`，不读 store | `DirectGLES.cpp:13066`、`:13134` |
| 3 D 的 case 3 在 Espryt split 臂放开 | 放开，三臂 + inproc A/B 绿；Espryt monolith 跳过保留（证红） | `MobileGL/MG_IntegrationTest/Scenarios/WireIndirectDrawScenario.cpp:306` |
| 4 `PipeCatalogueTest` 名单 = 调用点 | 删 `copy-image-shadow-mirror` 调用与名（G1 不变）；新 lint 进 CI 与 unit 车道 | `scripts/ci/unmigrated_emulation_sites.py` |
| 5 A–F、S 包说明行号漂移 | 213 处引用逐条对内容；87 处按内容重定位 + E.md 一行改写；S.md 无引用 | §7 |

- monolith 缺陷：本包未发现新的（case 3 的 Espryt monolith 红是 C 已记的 OQ15 一条）。

## 2. 项 1：根因与修法

| 事实 | 出处 |
|---|---|
| 发布函数原 guard `!defaultFBOInfo \|\| ...` 在 `OnSurfaceChanged` post 之前 return | `DirectGLES.cpp:16229`（函数）、`:16360`（post） |
| spawn / tcp 的 server 进程没有前端，`pDefaultFramebufferInfo` 为空（临时探针：spawn `(nil)`，inproc 非空） | 探针未提交 |
| 结果：client 的默认帧缓冲 depth / stencil 留在占位格式 `Depth32FStencil8` | `MobileGL/MG_Impl/Init.cpp:33`、`:37` |
| 探针：spawn client 报 depth 32 / `GL_FLOAT`；inproc 报 24 / `GL_UNSIGNED_NORMALIZED`（pbuffer 是 D24S8） | `DirectGLES.cpp:16498` |
| 用例按报告分配 D32F_S8，server 驱动拒绝 D24S8→D32F_S8 的深度 / 模板 blit，只打 `the depth/stencil aspect was dropped`；颜色照过，client 无 GL 错误 | `DirectGLES.cpp:10926` |
| inproc 绿是巧合：client 与 server 同进程共用该指针 | — |
| Magma 的发布从不看该指针（callback 臂直接 post），三臂一直绿 | `MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp:346` |
| 修：`MOBILEGL_PIPE_PUSH` 下 callback 在场即发布，只有 monolith 臂写占位；pull 构建保留原句（`#else`） | `DirectGLES.cpp:16239`、`:16325` |
| 登记：`mgl_itest_register_split_arms` 三臂；`ForcedDs.` 过滤器不再减掉该例 | `MobileGL/MG_IntegrationTest/CMakeLists.txt:4866`；`MobileGL/MG_IntegrationTest/P8Coverage.cmake:177-178` |
| 豁免表 pending-fix 类清空 | `scripts/data/split_coverage_exemptions.txt:64-65` |

- 顺带生效：P12 服务器自有窗口在 spawn 下的 extent 发布（`g_publishWindowExtent`，`DirectGLES.cpp:16656`）被同一 guard 挡住、从未发出过；现在会发出，client 走 `ApplySurfaceChangedToClient` 的带 extent 臂（`MobileGL/MG_Remote/Client/ClientSession.cpp:325`）。主机无覆盖，需设备确认。

## 3. 项 2：CPU mip 臂的 store 路由

| 事实 | 出处 |
|---|---|
| E 的驱动写标按纹理、粘性 | `MobileGL/MG_Remote/Server/StagedTextureStore.h:496-502` |
| split 的 `GenerateMipmap` 在进 `GenerateMipmapByRecord` 前无条件记驱动写：CPU 臂读 store 时标恒为真 | `DirectGLES.cpp:13296` |
| 所以在记写之前快照 `driverWrittenBefore` 并下传；否则 store 路由（B 为不可附着存储设计的）永远拒绝 | `DirectGLES.cpp:13291`、`:13305` |
| store 路由拒绝条件：view、无 store、驱动写过、未整层覆盖、尺寸不符 → `MGLOG_E_ONCE` `generate-mipmap-store-declined`，层不动，不 Fatal | `DirectGLES.cpp:13066`、`:13134` |
| E 的旋钮原只在 `ReadTextureImageWire` 查，CPU 臂直接调 `ReadTextureLevelTight`，主机到不了 store 路由；现在 CPU 臂也查（前向声明） | `MobileGL/MG_Backend/DirectGLES/WireTextureReadback.inc:445`；`DirectGLES.cpp:13026`、`:13045` |
| 粒度：标按纹理不按层，纹理被生成过一次或任一层被画过，之后读回被拒的生成一律拒绝（保守，不给旧字节） | — |

| 用例（`GenerateMipmapServerScenario`） | 形状 | 断言 | 行 |
|---|---|---|---|
| `Rgb16fDrawnBaseWithARefusedReadbackDeclinesByName` | level 0 上传哨兵色后当渲染目标画上下两半 | server 日志有 `generate-mipmap-store-declined`、无 store 滤波行；level 1..3 不是哨兵色 | `MobileGL/MG_IntegrationTest/Scenarios/GenerateMipmapServerScenario.cpp:408`、`:546` |
| `Rgb16fUploadedBaseWithARefusedReadbackIsFilteredFromTheStore`（对照） | 只上传 level 0 | 日志有 `filtering the staged store's copy`、无拒绝；level 1..3 正确 | `GenerateMipmapServerScenario.cpp:439`、`:550` |

| 条目 | 臂 | 登记 |
|---|---|---|
| `DirectGLES.{Split,Spawn}.CpuMipRefused.*` | 旋钮 `MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1` + `MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=16x16`，标签 `integration-gpu`、`integration-cpu-mip-<arm>` | `MobileGL/MG_IntegrationTest/CMakeLists.txt:4883` |
| 其他臂（monolith、无旋钮 split 三臂、Magma） | 两例按旋钮缺席跳过 | 环境发现 / `GenerateMipmapServerScenario.*` |

- 不上 tcp：server 是共享 `TcpServer.Start`，16x16 拒绝会碰到整条 lane 的 16x16 颜色读回；与 B 的 `CpuMip.` 同形，parity 无需例外行（实测 0 错）。

## 4. 项 3：D 的 case 3

- 跳过条件由「非 Magma」改为「非 Magma 且 monolith」（`WireIndirectDrawScenario.cpp:306`）；Espryt split 的 count 字由 C 的 `SplitHostBytesForCpuRead` 刷新（`DirectGLES.cpp:9632`、`:9798`）。
- 计数断言只在 Magma inproc（`CountersReadable`），Espryt 只断言像素。

## 5. 项 4：名单与调用点

| 事实 | 出处 |
|---|---|
| `copy-image-shadow-mirror` 调用在镜像函数自己的 transport return 之后，且在 `#if MOBILEGL_PIPE_PUSH` 内：任何 transport 到不了，pull 构建不编译它 | `DirectGLES.cpp:13580`、`:13582-13585` |
| 删调用与名：G1 符号 +0 / −0，`.text` `a5ba13` 不变 | §8 |
| 名单现 3 名，一名一处调用 | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:1448-1455` |
| lint 规则（全是 error）：名单每名恰一处产品调用；每处调用名在名单；名单无重复；调用须是字符串字面量。产品源 = `MobileGL/` 去掉 `MG_Test/`、`MG_IntegrationTest/`；先掩码注释 | `scripts/ci/unmigrated_emulation_sites.py` |
| CI：`test.yml` 在 `split_coverage.py` 之后一步 | `.github/workflows/test.yml:1475` |
| unit 车道：`PipeCatalogue.UnmigratedEmulationSites{SelfTest,MatchTheList}` | `MobileGL/MG_IntegrationTest/CMakeLists.txt:4895-4901` |
| 门命令：`python3 scripts/ci/unmigrated_emulation_sites.py --self-test && python3 scripts/ci/unmigrated_emulation_sites.py` | — |
| 回看：lint 在 `8d3e2317`（B 之前）红 2 条：`generate-mipmap-storage` 4 处调用、`generate-mipmap-cpu-filter` 不在名单；在 `cbbaf77f` 绿（4 名 4 处） | 证据 `r4b-lint-*.log` |

## 6. red-once

| # | 变异 | 载体 | 观察到的红 |
|---|---|---|---|
| R1 | 发布 guard 改回 `if (!defaultFBOInfo) return;`（= 基线） | `DirectGLES.{Split,Spawn,Tcp}.DepthStencilReadbackAttachmentShapeScenario.DefaultFramebuffer…` | Spawn、Tcp 红（depth 0、stencil 0），Split 绿 |
| R1′ | 同上 | 三个 `ForcedDs.` 条目 | Spawn、Tcp 红，Split 绿 |
| R2a | 去掉 `driverWrittenBefore` 判定 | `DirectGLES.{Split,Spawn}.CpuMipRefused.*` | 画过的例两臂红：level 1..3 = 哨兵 `rgba(255,0,255,255)`、无拒绝行、有 store 滤波行；对照绿 |
| R2b | 快照挪到自身记写之后 | 同上 | 对照两臂红：store 路由拒绝、level 1..3 `rgba(0,0,0,255)`；画过的例绿 |
| R2c | CPU 臂不查拒绝旋钮 | 同上 | 4/4 红：拒绝行与 store 滤波行都没有（原生读回路径） |
| R3 | 两处 `*IndirectCount` 参数读改回 `SplitHostBytes`（去 C 的刷新） | `DirectGLES.{Split,Spawn,Tcp}.WireIndirectDrawScenario.ComputeWrittenCount…` | 三臂红：两处 count 画错条带 |
| R3m | 去掉 Espryt monolith 跳过 | `DirectGLES.WireIndirectDrawScenario.ComputeWrittenCount…`（monolith） | 红：count 1 却画了条带 2、3（OQ15，跳过须保留） |
| R4a | `kNames` 加回 `copy-image-shadow-mirror` | lint；`PipeCatalogue.UnmigratedEmulationSitesMatchTheList` | rc 1：「listed and has no call site」；ctest 红 |
| R4b | `--self-test` 六例（多名、漏名、同名两处、名单重复、非字面量、匹配） | lint | 各规则按预期 rc；匹配树 rc 0 |

还原一律复制比对 + `touch`（`cmp` 一致）后重建再跑绿。

## 7. 项 5：行号漂移

- 方法：每份说明从最后写它的提交（A `06f28b0a`、B `b9feb9d7`、C `e815ce8d`、D `d639e546`、E `c00f5eab`、F `c7a14ed7`）经 `git diff -U0` 把每处 `file:line` 与同行 `:line` 简写映射到 HEAD；映射到的行与原行内容逐字相同。工具未提交。

| 说明 | 引用 | 未动 | 重定位 | 备注 |
|---|---|---|---|---|
| A | 21 | 7 | 14 | — |
| B | 31 | 2 | 29 | — |
| C | 30 | 20 | 10 | — |
| D | 44 | 32 | 12 | `WireDraw.inc` +7（F）、`WireDeclines.def` +6、`CMakeLists` D 块 `:4831-4864` |
| E | 41 | 21 | 18 + 1 行改写 | 第 93 行：调用与名已由本包删，改指删除注释与名单 |
| F | 46 | 42 | 4 | 第 11 行「旧 `:1427`…」是 F 之前的位置，按原意不动；`:1460` 属 `WireFramebuffer.inc`，未变 |
| S | 0 | — | — | — |

- 集成方简报里的几处量值是中间合并点的：B 的 `CMakeLists` `:3355` 在 HEAD 是 `:3442`（非 `:3375`），`:4059` 是 `:4174`，`:2154`（`TcpServer.Start` 的 fixture 属性）是 `:2168`；D 的 `:4651` 是 `:4831`（非 `:4764`）；按内容为准。
- 只改行号，不改历史陈述：B.md 的「名单保持 4 名」是 B 当时的读数，现为 3 名（§5）。

## 8. 门

（门跑完后填）

## 9. 发现未修

| 项 | 说明 |
|---|---|
| P12 extent 发布在 spawn 下首次生效 | §2 末条；需设备（红米 FCL→render server）确认 |
| store 路由粒度 | 驱动写标按纹理；按层的标需要改 E 的 store 接口 |
| B.md / E.md 的历史陈述 | 名单数、`copy-image-shadow-mirror` 的去向以本说明为准 |
