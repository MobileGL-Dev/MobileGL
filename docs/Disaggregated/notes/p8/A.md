# P8-A：覆盖对齐与归一化覆盖门（ID-P8-2）

> 2026-09-29，分支 `p8/a`（基线 `8d3e2317`）。brief `~/w7/notes/p8/BRIEF-A.md`；普查原始输出 `~/w7/notes/p8/evidence-a/`（树外）。

## 1. 结论

- 门改为「归一化后 monolith ⊆ 门控 split ∪ 豁免表」，由 `scripts/ci/split_coverage.py` 强制；`test.yml` 在 parity 步之后跑它（含 `--self-test`）。
- 当前读数：DirectGLES monolith 669 例 = 门控 split 581 + 豁免 88，缺 0；DirectVulkan 669 = 门控 202 + 仅信息层 435 + 豁免 32，缺 0。
- 按 gtest 名只在 monolith 的 DirectGLES 用例是 335（审计的 396 按 TEST_F 计，含它自己列出的约 45 个已带尾登记在 split 的「命名假象」）；去掉机制过滤器（`MobileGL/MG_IntegrationTest/CMakeLists.txt:792`）的 31 例后 304 例进普查。
- 本包新登记：246 个 ambient 用例 × 三臂；5 条 server 旋钮车道共 21 例 × 三臂（tcp 各有自己的 supervisor）；撤 `ClientSideIndices` 排除（d1 块三臂 + 5 个 multi-draw tier）；GuiBatch 随 ambient 登记。
- 审计 (c′) 的「进程内 peek」类实测为 0：P4aFinalFix / P4aSeamAudit 在 spawn / tcp 上 white-box 读取自行降级，public-GL 半边照跑，三臂全绿，已登记。
- DoublePrecision 12 例在同环境的 loopback tcp 上绿，已登记；`DEBTS.md` 的 fp64 行说的是 server 环境与 client 不同（`MOBILEGL_ADVERTISE_FP64`），本机没测到那一形。

## 2. 普查（304 例 × Split / Spawn / Tcp，armed 环境，每臂一个 junit）

| 结果 | 例数 | 处置 |
|---|---|---|
| 三臂通过（含 F1 的 19 个 split 专属例，monolith 上跳过） | 235 | 登记 231；MonolithAttachmentClear 3 豁免 mechanism；MultiDraw ClientSideIndices 1 在 d1 块撤排除 |
| 三臂跳过，monolith 同样跳过（主机能力：ClipDistance 2、DualSourceBlend 2、IterationRP* 5、SsboDeclarationForm 2） | 11 | 登记（与 monolith 同形） |
| 三臂跳过，缺旋钮（PointSizeDemotion 2、UnlocatedIoBlock 1、ViewportArray 负控 1） | 4 | ambient 登记 + §3 旋钮车道 |
| 三臂跳过，属 Magma（Magma* 21、PrimitivesGeneratedNoXfb 8、F1 Magma 拒绝 / 着色器 mip 9、ImageLoadStoreSso 1、UnwrittenPosition 1） | 40 | 豁免 single-backend（`DirectGLES:` 限定） |
| 三臂 `EmitSeq` 不动（AdvertisedLimits 8、SpirvShaderBinary 4、ProgramPipeline 1） | 13 | 豁免 no-records（`MobileGL/MG_IntegrationTest/Harness/ScenarioFixture.h:85-99`） |
| inproc 绿，spawn / tcp 红 | 1 | 豁免 pending-fix（§6） |

同一批 304 例经 tier-2 回放在 Magma 三臂：只有 no-records 的 13 例红，外加 tcp 的 IterationRPProgram203（§6 已修）。

## 3. server 旋钮车道 (a′)

| 车道（尾） | 旋钮 | 例 | server 读点 | 旋钮只在 client 环境时的 tcp | 各自 supervisor |
|---|---|---|---|---|---|
| `PointSizeDemotion.` | `MOBILEGL_POINT_SIZE_DEMOTION=1` | 5 | `MobileGL/ConfigLoader.cpp:240` | 2 红（arming、capture） | 绿 |
| `UnlocatedIoBlocks.` | `MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1` | 3 | `MobileGL/ConfigLoader.cpp:239` | 1 红（arming） | 绿 |
| `NoViewportArrayEmulation.` | `MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION=0` | 1 | `MobileGL/ConfigLoader.cpp:262` | 1 红 | 绿 |
| `WidenedPacked16.` | `MOBILEGL_ESPRYT_WIDEN_PACKED16_STORAGE=1` | 3 | `MobileGL/ConfigLoader.cpp:264` | 绿（无 arming 例，走的是默认臂） | 绿 |
| `ForcedDs.` | `MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1` | 9（tcp 另含 Matrix 14） | `MobileGL/ConfigLoader.cpp:249` | 绿（无 arming 例，走的是原生回读） | 绿 |

- 共享 fixture 不能带这些旋钮：会改掉所有 tcp 例（视口仿真关掉即 ViewportArray 正例红）。先例 `MobileGL/MG_IntegrationTest/CMakeLists.txt:2164` 只对三臂本来都设的开关成立。
- supervisor 端口 = lane 端点端口 + `MOBILEGL_ITEST_TCP_KNOB_PORT_OFFSET`（默认 1000）+ 车道序号；fixture 名 `mobilegl-tcp-p8a-<lane>`；条目仍持 `RESOURCE_LOCK mobilegl-tcp`（parity / junit_tally 的 tcp 锁规则）。
- 既有 `DirectGLES.Tcp.ForcedDs.DepthStencilReadbackMatrixScenario.*` 14 例原先旋钮只在 client 环境（server 跑原生回读）；同名条目移到 `ForcedDs` supervisor，旧块只留 Split / Spawn（`MobileGL/MG_IntegrationTest/CMakeLists.txt:2648`）。
- 5 个旋钮都是 Espryt 的，车道不上 DirectVulkan。

## 4. 豁免表 `scripts/data/split_coverage_exemptions.txt`

| 类 | DirectGLES | DirectVulkan | 内容 |
|---|---|---|---|
| mechanism | 34 | 32 | 机制过滤器 31；MonolithAttachmentClear 3（钉 push-monolith 臂，ID-107）；DualBlock 1（Magma，需 `ROLE_SPLIT_STATE`） |
| single-backend | 40 | 0 | §2 |
| no-records | 13 | 0 | §2 |
| inproc-peek | 0 | 0 | 类保留，现无成员 |
| pending-fix | 1 | 0 | §6 |

脚本规则（都是 error）：模式匹配不到 monolith 例；模式所指全部已在 split（过期行）；pending-fix 不写包名或 `DEBTS.md`；single-backend 不加 `<Backend>:`；未知类、缺理由、重复行。

## 5. 门、red-once 与耗时

| 门 / 断言 | 变异 | 载体 | 观察到的红 |
|---|---|---|---|
| `split_coverage.py` 按名 | 从 `P8Coverage.cmake` 删 `GuiBatchScenario.*` 一行，重 cmake + ninja | `split_coverage.py build-split` | rc 1，`missing 9`，逐个点名 `DirectGLES.GuiBatchScenario.*` |
| 过期行规则 | `elif needed == 0` → `elif False` | `--self-test` | rc 1，「a stale exemption reds: rc 0 (want 1)」 |
| 旋钮 supervisor | 5 个 fixture 的 `ENVIRONMENT` 去掉旋钮，重 cmake | 三条车道的 `DirectGLES.Tcp.` 条目（15 例） | 红 4（§3 三车道的 arming / 负控）；恢复后 15 绿 |
| `ClientSideIndices` 守住 client 拼平 | `MobileGL/MG_Impl/Pipe/OwnedDrawInputs.h:51` 由清 `kDrawHasUserIndices` 改为置位 | `ctest -R ClientSideIndicesBatchMatchesUnrolledDraws` | Split / Spawn / Tcp 与 5 个 tier、Magma 各 split 臂共 14 例 abort：client `Fatal{ProtocolCorruption, "EncodeRecord.tailBytes"} got=0 expected=32`（`MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1107`）；monolith 两后端绿 |

- 第 4 行不是 brief 预期的 `PipeApplier.cpp:972-975` 闩：client 已无产生 user-index span 的代码，只把第 51 行删掉（不清位）全绿；而 server 解码在进 sink 前对 `NumDraws != 1` 的 span 报 `ProtocolCorruption "DrawVbo.userIndices.shape"`（`MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:622-623`，调用 `:2412`），所以 `CLIENT_INDICES` 闩对任何 peer 都不可达；`+INSTANCED` 闩（`MobileGL/MG_Remote/Server/PipeApplier.cpp:976`）仍可达。交 P8-F。
- 车道耗时（本树，`-j 6`，七棵树同时在跑）：

| 车道 | 前 | 后 |
|---|---|---|
| integration-split | 427 例 24 s | 700 例 68 s |
| integration-spawn | 346 例 25 s | 614 例 66 s |
| integration-tcp | 348 例 101 s | 626 例 248 s |
| integration-magma-all-split / spawn / tcp（信息层） | — | 556 / 556 / 558 例，45 / 57 / 199 s |

- Magma 门控车道本包未加；新登记经 tier-2 回放自动进入 `integration-magma-all-*`（`MobileGL/MG_IntegrationTest/CMakeLists.txt:4724`）。

## 6. 发现

| 项 | 臂 / 后端 | 证据 | 去向 |
|---|---|---|---|
| `DepthStencilReadbackAttachmentShapeScenario.DefaultFramebufferDepthStencilFormatIsBlitCompatible`：默认 framebuffer 的深度 blit 出来是 0 | Espryt spawn + tcp（inproc 绿；Magma 三臂绿；带不带 FORCE_DS 都红） | 普查 junit；server 是 pbuffer 128×96，EGL config 要了 depth 24 / stencil 8（`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16146`），根因未追到 | pending-fix；需集成方加 `DEBTS.md` 行（不属 B / C / D / E） |
| tcp 共享 supervisor 没有 Vulkan ICD 与 iterationRP 修复开关（`MobileGL/MG_IntegrationTest/CMakeLists.txt:344-347` 只进 client 环境） | Magma tcp（tier 2 / tier 3） | `DirectVulkan.Tcp{,.Full}.IterationRPProgram203Scenario` 差 1 texel，Split / Spawn 绿 | 已修（测试接线）：`TcpServer.Start` 带上 `MGL_ITEST_VULKAN_ENV` 中非 common 的项（`MobileGL/MG_IntegrationTest/CMakeLists.txt:2159-2165`）；IterationRP 全部 78 条绿 |
| `CLIENT_INDICES` server 闩不可达 | 两后端 | §5 | P8-F |
| monolith 缺陷 | — | 本包未发现 | — |

## 7. 给集成方

- `P8Coverage.cmake` 的 include（`MobileGL/MG_IntegrationTest/CMakeLists.txt:4671-4674`）必须在 tier-2 回放与 `SplitLogPaths` 的 `configure_file`（`:5079`）之前；解冲突时不要移到它们下面。
- `gate.sh` hygiene 可加：`python3 scripts/ci/split_coverage.py --self-test && python3 scripts/ci/split_coverage.py build-split`。
- 其他包新登记 split 用例后，对应的 pending-fix / 缺口行会被脚本判为过期，需同提交删行。
