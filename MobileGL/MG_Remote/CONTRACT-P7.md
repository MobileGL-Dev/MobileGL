# CONTRACT-P7 — Magma（DirectVulkan）全量走完拆分路径

权威：本文，与 `CONTRACT-P5.md`（表 0、字节载体、字段归属、R-1…R-17）、`CONTRACT-P5C.md`（规则 E、两个具名豁免、`SEG_EVENT`、守卫）、`CONTRACT-P5E.md`（规则 F、等待规则）、`CONTRACT-MAGMA-RUNAHEAD.md`、`CONTRACT-P6.md`（规则 G/H、死亡漏斗、§12 记录债）、`CONTRACT-P65.md`（两轴传输、布局指纹、`LinkTerms`）并列；与它们不一致处以本文为准，§10 逐条列出。基线：`feat/disaggregated @ e8b2c4bd`。`file:line` 均在该提交上读取；路径在 `MobileGL/` 下，除非以 `docs/` 开头。

**怎么改。** 集成者的文件。wave 2 的实现包（A / B / C / D / E / F，§9）需要改行时经集成者；包从第一天起就对着下面的行编译。

**写自什么。** [`notes/p7/PLAN-PH-P34B-P7.md`](../../docs/Disaggregated/notes/p7/PLAN-PH-P34B-P7.md)（三行逐项审计，66 个代理，每个 done / unnecessary 判定两名反驳者）与 [`notes/p7/INTEGRATOR-DECISIONS-P7.md`](../../docs/Disaggregated/notes/p7/INTEGRATOR-DECISIONS-P7.md)（ID-P7-1..11）。本文只写**规范**——树必须变成什么样、门怎么判；为什么这样定在裁定日志里。

---

## §0 P7 是什么，以及一条高于每一行的规则

P7 收官 = 五道出口门同时成立（§2–§7），而不是路线图 P7 行的交付列逐字做完。路线图行里两项**本阶段拒做**（ID-P7-5：`SetupDrawSnapshot` 探测字段塌 dirty mask、`VertexInputStateFactory` 内容寻址 CSO——它们是 monolith 路径上的性能项，`VulkanRenderer::SetupDraw` 在任何非 monolith 传输下分支到 `SetupWireDraw`，split 下不跑，且改它们动 pull `.text`），一项只做窄半（内部 shader 烘焙的 monolith 臂退役与直通 TCS 烘焙归 P13 / OQ-7 宽半）。

**规则 I — Magma 的 wire 臂上没有 `std::abort()`。** 树上每一处 `@P7` 具名拒绝今天都是 `MGLOG_F` + `std::abort()`，绕过 `Session::Fail`、不给对端 `SessionFault` 帧、普查门看不见。P7 之后，`MG_Backend/DirectVulkan` 下任何 wire 臂的拒绝只有两种形状：(a) **decline**——`MGLOG_E_ONCE` + 返回失败 + 与 monolith 臂相同的可观测（屏蔽属性、跳过 blit……）；(b) **具名 Fatal**——经 `MG_Pipe` 的 `MGPipeSessionFailHook` 进 `MG_Remote::SessionFail`，带 `FatalFamilies.def` 里的家族词。`MG_Backend` 不 include `MG_Remote`（include 闭包门），hook 由 server 角色初始化安装。规则 I 的牙齿是 §3 的普查门：扩到 server 镜像后，`MG_Backend` 下裸 abort 的站点数只能下降。

**规则 J — 每个修复先在两进程臂上红一次。** wave 0 之前树上没有一条车道能让 Magma 两进程用例变红（四条 split 臂硬编码 `MOBILEGL_BACKEND_TYPE=DirectGLES`，`MG_IntegrationTest/CMakeLists.txt:2002/:2025/:2031`），所以 ID-P7-10：车道落地前不派 `@P7` 退役的活；落地后每个退役都带一条在 `integration-magma-spawn`（或 `-tcp`）上先红后绿的用例。

---

## §1 与前序契约的关系：什么已经不是 P7 的

| 项 | 谁做了 | 证据 |
|---|---|---|
| `DynamicBackendParameters` / `MGPCaps` / `RenderStateParameters` 定宽 + 布局指纹 | P6.5 wf（`fe28bdb5`） | `CONTRACT-P65.md` 「握手与布局」；`CONTRACT-P6.md:666` 仍记在 P7 名下，§10 更正 |
| server buffer consumers、Android 旋转 blit | `38919d45` | `notes/p5f/magma-inproc-fix.md` |
| Magma run-ahead、GPU 延迟退休、VAO 身份碰撞 | `194382c9` | `notes/p5f/magma-runahead.md`；`MG_Backend/Init.cpp:101` `kMGPipeMagmaRunAheadReady = true` |
| packed-float mip 原生 blit、resolve、占位图像、copy-image 端点、XFB 捕获 span、纹理回读 | `ed507b1a..d56c0e83`（随 `e1bf3677` 合入） | `WirePlaceholderImages.inc`、`WireFramebuffer.inc:634-636` |
| 颜色 blit 烘焙（仅 split 臂） | P5f fv | `Renderer/WireColorBlit.{vert,frag,inc}`、`WireColorBlitSpirv.h` |
| D-B8 具名 UBO 的载体 | 直绑常驻 `VkBuffer` range 的备选已在树上 | `UniformManager.cpp:2375` 直绑分支；欠一次 `stage-ubo-named` 字节/帧测量后关 OQ-6 |

---

## §2 出口门 1：集成 + trace 在 Magma 的 push 与 split 下全绿

**2.1 车道形状（wave 0 包 L）。** `mgl_itest_register_split_arms` 获得后端维度；DirectVulkan × {Split(inproc), Spawn, Tcp} 三臂的环境为 `MGL_ITEST_VULKAN_{SPLIT,SPAWN,TCP}_ENVIRONMENT`（`MOBILEGL_BACKEND_TYPE=DirectVulkan` + monolith `DirectVulkan.` 臂已用的 ICD 钉）。两档标签：

- **gating**：`integration-magma-split`（inproc，含义不变）、`integration-magma-spawn`、`integration-magma-tcp`——今天 `integration-magma-split` 的 73 条（及 magma-buffers / magma-runahead / magma-caches 子标签）重铺三臂；三臂**同数绿**才算门 1 的集成半边成立。
- **informational**：`integration-magma-{spawn,tcp}-all`——所有经共享宏注册的场景 × DirectVulkan × 两进程。首轮红名单是 wave 2 的工作清单；不进 CI 门，直到某条目连续绿后**按名**升入 gating（`test.yml` 的 require_green 名单）。

名集合纪律不变：`scripts/ci/spawn_lane_parity.py` 规范化后 split / spawn / tcp 三集合相等；G14 只增不删；每条目私有角色日志（`SplitLogPaths.cmake.in` 由宏累积）。

**2.2 臂证明。** Magma 两进程条目必须在 client 私有日志里同时有 `transport=spawn`（或 `control=tcp data=stream`）与 `backend=DirectVulkan`（或等价的 ConfigLoader 行）——一条 Magma 车道静默跑成 GLES 会满足其它所有门。`junit_tally.py` 的 `--require-spawn` / `--require-tcp-ran` 各加后端断言。

**2.3 死亡通知前提。** `MG_Remote/Client/WireTables.cpp:740` 只在 DirectGLES 下安装对象死亡通知，Magma 两进程 client 的 `ObjectDeaths` 恒为 0。改成后端无关（wave 0 L）；Magma 自己的 `StateObjectDeathOps` 表（wave 2-C）落地前，`CtWireScenario` 的死亡用例在 Magma 臂上按名 skip 并列入 §12。

**2.4 trace 半边。** `retrace-split` 已是 39 case × 2 后端 × {inproc, spawn}（`test.yml:2449/:2470`）；欠 `iterationrp × DirectVulkan` 的三个旋钮在 retrace-split 作业里与 `:2183` 同样导出（wave 0 L）。门 1 的 trace 半边 = 78 条 DirectVulkan split 行全部达阈值。

**2.5 门 1 的判据。** gating 三臂同数绿（wave 0 落地时：magma-split 73 = 71 PASS + 2 SKIP，magma-spawn 52 = 50 + 2，magma-tcp 54 = 52 + 2 fixture——`MagmaRunAheadScenario`（14）与 `MagmaWireCacheScenario`（7）只注册 inproc 臂，`spawn_lane_parity.py` 的 `MAGMA_INPROC_ONLY` 具名例外；跨进程的 hold / cache peek 是 §12 的债）+ parity 通过 + retrace-split 78 条 DV 行达阈值 + informational 车道（`integration-magma-full-{split,spawn,tcp}`，整个集成二进制：510 / 510 / 512 条，首轮 453 PASS / 57 SKIP / **0 红**，tcp 臂 1 红 = `IterationRPProgram203Scenario` 的 exposure texel，StreamLink × Magma 交叉项，见 `notes/p7/magma-two-process-first-run.md`）的红名单为空或每条按名归入 P8 / P9 / P13 并在 `notes/p7/` 有票。**主机 lavapipe 打不到任何 `@P7` 拒绝**（驱动能力差异），门 1 的红名单实际来自真机（§7.1 的设备窗口）。

---

## §3 `@P7` 具名拒绝：普查、退役与 decline

**3.1 普查数（钉住）。** 基线上 `grep -rn "@P7" MobileGL/ --include=*.cpp --include=*.inc --include=*.h` = **15 处 / 4 文件**（wave 0 包 C 以 `fatal_census.py --json` 钉住；两次审计的 14 串 / 13 语句是把一个三元里的两个串算成一条），全部在 `Renderer/UniformManager.cpp`、`Renderer/WireFramebuffer.inc`、`Renderer/WireDraw.inc`、`Renderer/VulkanRenderer.cpp`。另有两项 P7 归属但不带 `@P7` 字样：`Fatal{RoleViolation, "buffer-legacy-arm"}`（`WireDraw.inc:14/:204`）与「Magma 无 `StateObjectDeathOps`」（两条集成用例 skip）。

**3.2 退役表。** 「退役」= 实现该形状，与 monolith 臂同可观测；「decline」= 规则 I (a)。

| 名 | 站点 | 判定 | 形状 |
|---|---|---|---|
| `uniform-buffer-byte-tail` | `UniformManager.cpp:2391` | **退役**（真机 MC 报告 §5 的活边界） | 拷贝窗口外扩到 4 字节边界进 transient slice，再 `vkCmdCopyBuffer` 对齐内部到正确子字目的；`CopyWireBufferRangeToSlice` 扩 |
| `copy-image-in-place` | `VulkanRenderer.cpp` | **已退役（wave 2-B，`4f857aa5`）** | `srcImage == dstImage` 时单次转 `GENERAL`、两侧布局 GENERAL、一次写回；同 mip+layer 且矩形重叠 → decline（GL 未定义） |
| `mipmap-shader-format-or-shape`（真机 iterationt 的死因） | `WireFramebuffer.inc` | **半退役（wave 2-B，`c4be3800`）** | Fatal 行带上格式 / 尺寸 / aspect / 缺的 feature 位；image 不能作附件时经 scratch 走颜色 shader mip；仍无 shader 臂的形状**具名**拒绝 |
| **wire 回读竞态（曾被当作 OpenRA 真机 0.976494 的原因；真因是下一行）** | `WireFramebuffer.inc` / `VulkanRenderer.cpp` | **已修（wave 2-B，`76b74693`）** |
| **wire 臂 completed-frame-serial floor 不健全（OpenRA 真机 0.976494 / 0.988998 的真因，ID-P7-33）** | `VulkanRenderer.cpp` `OnSubmitsCompletedUpTo`、`VkBufferManager.cpp` `WriteWireBuffer` | **已修（wave 2-B3，`511c1f16` / `c9164f25`）** | 一个 serial 两次提交（S1 pooled-fence 中帧 flush、S2 Present），逐记录退休把 floor 提前 → 帧 N 首次 `glBufferSubData` 走无序 host memcpy 被 S2(N-1) 的排队拷贝盖掉；floor 只升到可证 serial + 有读者在飞的 buffer 一律有序；计数 26→0，red-once `StreamedBufferSubDataBeforeEachDrawIsOrdered` Split+Spawn；32 处静默出口具名（`WireDeclines.def`）。留：三处单 fence 退休站点的聚合等待（B4） | `ReadWirePixels` 在 pooled fence 下 flush 帧内 draw 但不等待，拷贝是后一次提交，二者只靠一条 barrier 相连，而 `UNDEFINED` / `PRESENT_SRC_KHR` 的源 scope 映射成 `TOP_OF_PIPE`+0 = 空 scope——刚 present 过的默认 framebuffer正是这个布局；修后回读等它读的那一帧（red-once：`fences=2` vs `1`，`completed=71` 不变）。真机验收 = 三次 `pm clear` 冷跑 1.0 |
| `vertex-layout` | `WireDraw.inc:245`、`VertexInputStateFactory.cpp:439-500` | **已拆分（wave 2-C，`f1b04edf`）** | `buffer-window` 一种保留为协议错（经 hook 的具名 Fatal，家族词暂留 `UnmigratedVerb`——改成更诚实的 `ProtocolCorruption` 要在 `MG_Pipe::MGPipeFatalFamily` 加一行 + 适配臂，§3.3 未花这笔，记 §12）；其余**七**种形状/格式原因（含审计之后新增的 `offset-overflow`）改 monolith 的「屏蔽该属性继续画」，`BuildWireVertexInput` 出 `unsupportedAttribMask`；`vertex-format-conversion` 同包改 decline。`WireDraw.inc` 从此无 `@P7`（普查 15 → 13 处 / 3 文件） |
| `default-color-blit-shape` | `WireFramebuffer.inc` | **已退役（wave 2-B，`583e12de`）** | 恒等变换回落 `vkCmdBlitImage` 臂；非恒等旋转先 resolve/copy 进 scratch 2D RGBA 再 shader blit；四旋转之外 decline。顺手修出拒绝背后的活 bug：普通 blit 臂对 3D 源按 layer 寻址（z 0..1 读到 slice 0）。旋转半边只在真机可证 |
| `multisample-blit-shape` / `-aspect` | `WireFramebuffer.inc` | **已退役（wave 2-B2，`452d33b5` / `51fa513d`）**：shape 走 scratch resolve + blit、MS 目的地具名 decline；aspect 走 `VK_KHR_depth_stencil_resolve` 或第三套烘焙 shader resolve（`WireMultisampleResolve.*`），无臂的采样数形状具名 decline；树上 `@P7` 拒绝站点 **0**（剩 19 行全是注释） | shape：resolve 进 scratch 单采样再 `vkCmdBlitImage` 缩放/翻转；aspect：MS 深/模板 → 单采样用 `VK_KHR_depth_stencil_resolve`（`DynamicBackendParameters` 里查扩展）或 `texelFetch(sampler2DMS)` 写 `gl_FragDepth` 的 shader resolve；单采样 → MS 尺寸不同 decline |
| `depth-stencil-mipmap` | `WireFramebuffer.inc` | **已退役（wave 2-B，`227d8220`）** | DEPTH-only 走 `WireDepthMipmap.{vert,frag}` 烘焙程序（(A)）；`MOBILEGL_BAKED_INTERNAL_SHADERS` 表驱动新鲜度门（(D)）覆盖树内全部烘焙头——首跑抓出 iterationRP witness 头过期；D\|S 合并 / 仅 stencil → decline |
| `uniform-buffer-byte-tail` → **已退役（wave 2-A，`450d3075`）** | — | — | 见上行；真正可达的形状是 `size` 非 4 倍数（offset 非对齐在前端就 `GL_INVALID_VALUE`） |
| `texel-buffer-native-format` | `UniformManager.cpp` | **已 decline（wave 2-A）** | 零占位 view，fetch 读零；只在 Adreno 上有可观测（`GL_RGB32F` buffer texture） |
| `unaligned-{texel,storage,atomic}-buffer-range` | `UniformManager.cpp` | **已具名 decline（wave 2-A）** | 跳过该绑定；可写 range 的 copyback 不做（分不出 readonly、录制点在 B/C 的 draw 尾，配方 `magma-a.md` §3.1）；真正活的缺口是 `glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, …, 4, 4)`，Adreno 64 字节对齐 |
| `storage-buffer-native-range` / `uniform-block-native-range` | `UniformManager.cpp` | **已钳制（wave 2-A）** | 钳到 `maxStorageBufferRange` / `maxUniformBufferRange` |
| `uniform-buffer-dynamic-offset` | `UniformManager.cpp` | **已 decline（wave 2-A）** | >4 GiB transient ring 跳过绑定 |
| `storage-buffer-offset-overflow` / `texel-buffer-offset-overflow` | `UniformManager.cpp` | 具名 Fatal（经 hook） | 算术不变量，不带 `@P7` |
| `vertex-format-conversion` | `WireDraw.inc:272` | decline | 可选：packed converter |
| `buffer-legacy-arm` | `WireDraw.inc:14/:204` | 具名 Fatal 经 hook（`RoleViolation`） | 已是拒绝，只换漏斗 |

**3.3 普查门（wave 0 包 C）。** `scripts/ci/fatal_census.py` 的扫描根扩到 server 镜像（`MG_Remote`、`MG_Pipe`、`MG_Backend`、`MG_Impl/Pipe`、`MG_State`，MG_Test / MG_Benchmark / MG_IntegrationTest 除外）；`abort_sites` 成为**向下棘轮**；每个 `Fatal{Word` 在 `FatalFamilies.def` 有行；`Refuse{Word}` 对 `protocol.fbs` `RefuseCode` 校验。baseline 数（wave 0 落地，`8bb6309a`）：**79 处 abort / 20 文件**（扩容前 4 / `MG_Remote` 一处），**43 个家族词**（补了 11 行 `.def`，`InitialBytesNotCarried` 并入 `UncarriedInitialBytes`），3 个 `Refuse{}` 词，0 无标记；`ResourceUnavailable` 投影到 `ServerCrashed` 而非 `SegmentMismatch`（段几何没错的时候不说段错了）；Magma 三个 wire fatal 漏斗（`MagmaWireFatal` / `WireDescriptorFatal` / `WireBufferLegacyFatal`）经 `MG_Pipe` 的 `MGPipeSessionFailHook` 进 `SessionFail`，家族词保持 `UnmigratedVerb`（改名会作废自 P5b 起的拒绝普查计数）；`Refuse{AuthenticationRequired}` 改为词表里的 `Authentication`。

---

## §4 出口门 4：184 符号棘轮下降 P7 的 101 个

**4.1 定义（wave 0 包 R，`scripts/link_ratchet.py`）。** 在 disaggregated 构建的对象树上：按 `notes/p6/a6-link-experiment-data.md` 的路径规则把 `MobileGL.dir` 下对象分 SERVER / FRONTEND / SHARED；集合 = (SERVER 未定义) − (SERVER 已定义) − (SHARED 已定义) ∩ (FRONTEND 已定义)。baseline 存**符号列表**（`scripts/data/link_ratchet_baseline.txt`），`--assert-monotone` 只对新增符号红，消失只提示重基线（ID-P7-7）。基线数（`scripts/data/link_ratchet_baseline.txt`，`8bb6309a`）：**186** = `p7-magma` **101** / `p3b-p4b-espryt` **14** / `both-backends` **60** / `p6-core` 11——三个后端桶与 a6 的手数逐符号相同，多出的 2 个是 P6.5 的 `ClientSession::StartSpawned()` 与 `ProgramArtifactsSchemaFingerprint()`。Espryt 桶里 5 个是 `MG_Impl/Pipe/SlotAllocator.cpp.o` 定义的 `MG_Pipe::` 符号，按文件位置落在前端桶、只有 P13 的模块搬迁能清（标 `# P13`），所以 P3b/P4b 的可清数是 9 不是 14。`MG_Remote/FatalFunnel.cpp.o` 归 SHARED（两个角色都经它）；`MG_Backend/Init.cpp.o` 按 a6 规则归 SERVER，虽然它引用 5 个 client-session 符号。CI 步骤在 `build-linux-split` 作业；baseline 由参考工具链生成，CI 的 `clang++-20` 内联决策可能让首轮红——三元规则：已列类的兄弟方法、无新引用对象 = 内联，重基线；新引用对象永远不是内联。**重基线（wave 2-B2，`3c2867d3`）：186 → 173** = `p7-magma` **88** / `p3b-p4b-espryt` 18 / `both-backends` 56 / `p6-core` 11；(B′) 清掉 13 个（`ProgramObject::*`、`SamplerObject::Set*`、`ShaderObject::*` 的编译/链接路径）。剩下的 88 不是「隐藏程序」而是 **monolith draw 路径**（`UniformManager.cpp.o` 引用 `TextureObjectBase`/`TextureObjectWithOneMipmap` 等 62 个、`VkSamplerManager.cpp.o` 引用 `SamplerObject` 14 个 getter、12 个单例），`SetupDraw` 在运行时才分支到 `SetupWireDraw`（ID-P7-5 保留该运行时）——`#if` 掉这些 monolith 臂与 (B′) 是同一种操作、属 wave 3，故不标 `# P13`；§4.2 的「降到 0」对 (B′) 单独不成立。

**4.2 P7 的目标。** `--bucket` 归到 DirectVulkan 引用对象的那一桶（a6 的 101）降到 **0**。唯一还在构造前端 `SamplerObject` / `ShaderObject` / `ProgramObject` 的 DirectVulkan 站点是 `VulkanRenderer.cpp:4526-4583 / 4624-4684`（monolith 臂的隐藏 blit / 深度 mip 程序），其 early-return 是运行时而非 `#if`，所以符号仍被引用。降到 0 的路径：在 **disaggregated 构建里** monolith 臂也改用烘焙模块（§5.2 的 (B')），pull 构建的代码逐语句原样——G1 不动。若 wave 3 实测某桶符号无法在不动 pull `.text` 的前提下消失，按名记入 §12 并在 baseline 里 `# P13` 标注，不算门 4 失败。

**4.3 与 P3b/P4b 的 14 个。** 不是 P7 的目标；棘轮落地后 P3b/P4b 的 Tier 1 才可调度。

---

## §5 内部 shader、反射归档与 resident 上传（wave 2-B / 2-C 的规范）

**5.1 烘焙 (A)(D)。** `WireDepthMipmap.{vert,frag}` + 生成的 `WireDepthMipmapSpirv.h` + `WireDepthMipmap.inc`（深度附件 render pass + pipeline），`GenerateWireMipmap`（`WireFramebuffer.inc:752-769`）深度分支接入；新鲜度门 `MOBILEGL_BAKED_INTERNAL_SHADERS`：MG_Test 用树内 glslang 重编每个签进树的 SPIR-V 头并逐字节比对，**表驱动**，覆盖 `MG_Util/SelfTest/DriverPostIterationRPWitnessSpv.h` 与 `PrimitivesGeneratedNoXfbProbeSpv.h`。
**5.2 (B') disaggregated 构建的 monolith 臂改用烘焙模块**（§4.2 需要）：`InitializeBlitResources` / `InitializeDepthMipmapResources` / `BlitWithShader` / `GenerateDepthMipmapWithShader` 在 `#if MOBILEGL_BUILD_DISAGGREGATED` 下走烘焙管线；`#else` 分支保留今天的代码逐语句原样。两处 `MagmaP7AllocatorDebtScope` 随之删除（先用临时断言证明它们在非 monolith 传输下从不武装）。
**5.3 OQ-8。** `LinkArtifacts` 加有序 `storageBlocks{name, binding, dataSize}`（顺序 = stage 序 → 规范化描述符名 → binding，跨 stage 去重，与 `MagmaProgramSource.h:194-195` / `DirectVulkan.cpp:220-260` 今天的资源查询序相同），进 `Visit()`，archive codec 版本 +1（`wireFingerprint` 随之变）；`MagmaProgramSource::EnsureStorageBlocks` 与 `DirectVulkan.cpp` 的 `GetProgramResourceCache` 都改读它；`g_programResourceCaches` 及其 rehash 悬挂风险随之删。单测：多 stage + 数组 SSBO + atomic-counter block 的索引序与 SPIRV-Reflect 逐项相同。
**5.4 OQ-10。** `MG_Backend/Init.cpp` `InitServerRoleCommon` 按 **server 自己的 wire 资源表** 判 `SubDataResident != nullptr` 发布 `kCapResidentSubData`（不按后端枚举猜——ID-39 的教训）；`RemoteClientTest.cpp:594` 改期望；integration-magma-buffers 与 integration-split 各一条白盒用例证明发出的是 `BufferSubDataResident(49)`；26.3 in-world 回归。
**5.5 Magma `StateObjectDeathOps`。** 镜像 `DirectGLES/Managers.cpp:335-474` 的表，从 DirectVulkan 后端注册处安装；删六处 Magma 特判；`PipeSlotPeek` 断言死亡后槽位回落；red-once = 卸表看槽位泄漏断言红。

---

## §6 出口门 2：verify 零分歧

verify **在 wave 3 之前**只在 monolith 臂注册（`MG_IntegrationTest/CMakeLists.txt` 的 `if (MOBILEGL_PIPE_VERIFY)` 块，约 `:1715` 起；写这一句时抄的 `:1684-1760` 是错的行段）；wave 3 V1 起同一文件的 `if (MOBILEGL_BUILD_DISAGGREGATED AND MOBILEGL_PIPE_VERIFY)` 块把整套用例在 split(inproc) 臂上也注册了一遍，所以这句只是门 2 的**起点**记录，不是现状。门 2 = **verify × {monolith, split(inproc)} × Magma 零分歧**，且两条既有负控（`VERIFY_CORRUPT`、`POISON_OMIT`）在 split 臂上各红一次。前置债：verify+split 撞旧 `PipeRespecifyScope` 断言（`notes/p65/validation-status.md:118`）——放宽为「按 level 的 producer 各自覆盖其宣告范围」，正反单测各一。trace 侧：8 个 verify case × DirectVulkan × inproc 一轮，分歧计数 0。**状态（wave 3 V1，`p7/verify-split`，`notes/p7/verify-split.md`）**：前置债已清（`70f86064`），`integration-verify-split` 双后端 1072/1072 绿、两条负控在 split 臂上各红一次（POISON_OMIT 用 split 臂自己的 `ReadPixels:GetPixelStoreParameters` 配对）、trace 侧 8 case 分歧 0，门 2 在主机上成立；唯一的真实分歧是 server `read_pixels` 的 ID-49 中性 pack 窗口，按预言而非规则处理，spawn 不在门内也无法测（比对器是同进程仪器）。

---

## §7 出口门 3 与 5：真机与 CTS

**7.1 门 3 分母（ID-P7-4，设备窗口 #1 E0a 补正）。** `trace_cases.json` 的 CI split 子集（39）× DirectVulkan × `--use-pbuffer`，排除**在 monolith 臂上就红的 case**——它们按定义不是分离缺陷：`create-indirect`（OQ-17：`dev@81b17c0b` 起在 Adreno 830 上就坏）、`minecraft-1.21.11-main-menu`（trace 自身 UBO offset 不满足本机 32 字节对齐，`notes/p65/mainmenu-trace-difference.md`）、`minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world`（Adreno 830 的 `vkCreateGraphicsPipelines` 对 photon 程序返回 `VK_ERROR_UNKNOWN`，`Renderer/PipelineFactory.cpp:660`，monolith SSIM 0.0089；桌面 lavapipe 绿；按 OQ-17 先例归 `dev` 侧独立条目）；rd12 in-world 不在 `--matrix` 里。**分母 = 36**。`iterationrp` 带 `apk.yml:460-462` 的三个 Magma 旋钮。排除表随每次窗口更新于 `notes/p7/device-window-<n>/E0-attribution/exclusions.md`。
**7.2 门 3 判据（逐 case 对 monolith 臂，不是对 golden 的平 1.0）。** E0a 实测 monolith × DirectVulkan 对 golden 的 SSIM 在 0.996–1.000（iris 光影 case 最低 0.99617），只有 OpenRA 是 1.000000——golden 来自桌面，阈值吸收驱动差。所以门 3 = 同一 reboot-clean 会话内，对分母中每个 case：inproc **与** spawn（或 tcp）三遍**逐位相同**（零翻转），且每遍 `ssim vs golden` ≥ 该 case 阈值 **且与 monolith 臂同会话读数之差 ≤ 0.0005**（OpenRA 两臂都必须 1.000000、mismatch 0）；`RUN_AHEAD=0` 对照臂一遍记录。证据目录 `docs/Disaggregated/notes/p7/device-window-<n>/`。定位实验顺序 E0–E6 见计划 §3 wave 1。**状态（W4，p7w4 = `ae3f43ab`，ID-P7-25）**：inproc 一遍 34/36 与 monolith 差 ≤ 0.0005；OpenRA 是与温度无关的约 50% 竞态（两张图，lockstep 第三张），验收改为 run-ahead 与 lockstep 各 6 遍全等 golden；bsl-esc-menu inproc OOM 待 spawn 臂。**状态（W5，p7w5 = `713bea9a`，ID-P7-34）**：OpenRA **27/27 golden**（inproc/spawn × run-ahead/lockstep × 冷/热 + FIF=8 + dump-armed）——项关闭；余 bsl-esc-menu（M2）与终局形式（p7w6：分母 36 三遍逐位 + spawn 臂同会话）。
**7.3 门 5。** 块与 caselist（`tools/cts/caselists/`，取自 VK-GL-CTS `0b04c470e`，即设备上 glcts 的源）：`KHR-GL46.texture_*`（1055）、`KHR-GL46.shader_image*`（125）、`KHR-GL46.packed_pixels`（4732）、`KHR-GL46.direct_state_access.*`（371）、**UBO 在 KHR 包里没有专属组**——取 GTF 模块 `GTF-GL46.gtf31.GL3Tests.uniform_buffer_object*`（90，设备 glcts 若未含 GTF 则该块记 unrun）、另加 `KHR-GL46.shader_storage_buffer_object`（124，簇 A 的 unaligned / native-range 拒绝实际打的是 SSBO range）。`KHR-GL46` 在 Android 跑器上未证实可开 4.6 core context：窗口先探针；不行则 `$BASE` 与 AFTER 都用 `gl33` 变体（texture 868 / packed_pixels 4732 / ubo 90；GL 3.3 无 shader image、无 DSA），**同一 API 版本比 0.5 pp**。先 monolith × DirectVulkan 基线入库 `tools/cts/baselines/`，迁移落地后 inproc × DirectVulkan 比对：每块 pass 率差 ≤ 0.5 pp（Pass/(Pass+Fail)，NS 不入分母），**新增 crash = 0 单独硬红**。完整 caselist（~56k）在合 `dev` 前跑，不在关键路径。

---

## §8 子系统分母与 40% 检查点

分母 9 项：(1) Magma 两进程车道 gating 三臂同数绿；(2) §3.2 的六个「退役」；(3) `StateObjectDeathOps`；(4) 烘焙 (A)(D)；(5) OQ-8；(6) OQ-10；(7) verify×split（**wave 3 V1 已完成**，`notes/p7/verify-split.md`；中点检查点那次计数时它还未成，两句说的是不同时刻）；(8) 真机 ssim 1.0；(9) CTS 五块 ≤0.5 pp。**中点** = wave 2 的 A / B / C 三簇全部合入集成树之时；届时完成 < 4/9 → 停下重定基线（路线图唯一幸存的再基线绊线，此前没有分母、不可执行）。**检查点已过（2026-09-22，B2 落地时）：6/9 成**——(1) 三臂 93/72/74 绿、(2) 六退役全成（ID-P7-31）、(3)(5)(6) 随 C、(4) 随 B；未成：(7) verify×split（wave 3）、(8) 真机（OpenRA 归 B3、bsl-esc-menu 归 M1）、(9) CTS AFTER（wave 4）。不触发重定基线。

---

## §9 wave 2 的文件分区与合并规则

| 簇 | 文件 | 内容 |
|---|---|---|
| A | `Renderer/UniformManager.cpp` | §3.2 的 UBO / SSBO / texel 对齐与 native-range 行 |
| B | `Renderer/WireFramebuffer.inc`、`Renderer/VulkanRenderer.cpp`、新 `WireDepthMipmap.*` | copy-image、default-color-blit、multisample、深度 mip 烘焙 (A)(D)、(B')、AllocatorDebtScope |
| C | `Renderer/VertexInputStateFactory.cpp`、`Renderer/WireDraw.inc`、`Renderer/VkBufferManager.cpp`、`MG_Backend/Init.cpp`、`Renderer/MagmaProgramSource.h`、`MG_State/.../ProgramArtifacts.h` | vertex-layout 拆分、`StateObjectDeathOps`、OQ-10、OQ-8 |
| D | `MG_Backend/DirectGLES/*`、`MG_State/*`、`MG_Impl/Pipe/*`、`MG_IntegrationTest/*` | P3b/P4b 的 Espryt 项（与 A/B/C 零重叠） |
| E | `docs/`、契约 | 已落地项更正、OQ 关闭、`MEASUREMENTS.md` §7.2 同名重跑 |
| F | `MG_Remote/Transport/*`、`MG_Remote/Server/ServerMain.cpp`、`Handshake.h`、`Protocol/protocol.fbs` | Ph 小件（令牌、D11 五处、PH-8 单测） |

合并规则：集成者串行合并；每次合并后 gating 三臂 + `link_ratchet.py --assert-monotone` + G1 + parity；**棘轮上升的提交拒收**；每包一条 red-once 记在 `notes/p7/`；性能只记录不设门（ID-P7-9）。实现方 `opus`、审查方 `fable`（ID-P7-8）。

---

## §10 对前序契约的更正

1. `CONTRACT-P6.md:666`（§10.3）把 `DynamicBackendParameters` 定宽记在 P7 名下——已由 P6.5 wf 落地，P7 不再持有。
2. `CONTRACT-P6.md` §12.1「棘轮在 CI 里重算 184 并断言只降」——由 wave 0 包 R 落地为 `scripts/link_ratchet.py`；「移交 P6.5」从未被 P6.5 接收，归 P7。
3. `CONTRACT-P6.md` §12.2 D11 把五行中的三行分给 P6 `dl`——树上五处全部仍在原状态（`PipeApply.cpp:1575`、`PipeApplier.cpp:444`、`ProgramArtifactsCodec.cpp:309`、`StagedTextureStore.h:440`、`SlotTables.h:347/:371`）；归 Ph（计划 §1.1 D11），wave 2-F 做。**wave 2-F 实况（`notes/p7/ph-f.md`）**：落地的是 PH-7 (1)–(4)（常量时间令牌合一、≥16 字节、无令牌只 loopback、`Welcome.dataNonce` 绑定数据连接——`protocol.fbs` 变、`wireFingerprint` 随 `MOBILEGL_PROTOCOL_CONTROL_REVISION` 变）与 PH-8 单测；**D11 五处与 PH-2 仍未落地**：每处都要一条字节来自对端的两进程负控，而树上没有能越过 client 自身守卫伪造记录的对端驱动（= fuzz 臂 2 的骨架），按计划停止规则未开工。
4. `CONTRACT-P6.md` §5.2「`Session::Fail` 在 P6 保持 `[[noreturn]]`」——Ph 的翻转按 ID-P7-1 重定界，不做 98 站点的字面翻转；`Server/StagedShadow.h:115`、`Server/StagedTextureStore.h:344` 两处旁路由 wave 0 包 C 收进漏斗。
5. `ARCHITECTURE.md` §8.5「XFB scatter 在 client」——树上取的是 server staged shadow + `OnBufferWriteback`（P5c/P5f）；`OnXfbScatterReady` 是死声明，删除。

---

## §11 不是 P7 的

- `SetupDrawSnapshot` dirty mask、`VertexInputStateFactory` 内容寻址 CSO（ID-P7-5）→ 优化阶段。
- 内部 shader 烘焙的 pull 构建 monolith 臂退役（动 pull `.text`）、直通 TCS 烘焙（64 模块）→ P13 / OQ-7 宽半。
- `MG_Util` 头/对象库拆分 → P13。
- Ph 的 fuzz 臂 2（对端字节可达站点逐站点闩转换）、PH-6、PH-7 (4)(5) → P7 之后（计划 wave 5）。
- P3b/P4b 的 CTS AFTER → 该阶段。Iris 普查的 P9 例外（`texture-remint-pull`）：wave 3 包 E1 在 `3c80cd62` 的主机普查里**为空**——77 行 × {monolith, inproc, spawn} 231 次全活，该站点一次未到达（`docs/Disaggregated/notes/p7/iris-census-3c80cd62.md` §4）；设备侧读数留 wave 4。

---

## §12 记录债

- ~~Magma 两进程 client 在 `StateObjectDeathOps` 落地前 `ObjectDeaths=0`：`CtWireScenario` 死亡用例在 Magma 臂按名 skip（§2.3）。~~ **已退（wave 2-C，`dce9953d`）**：两处 skip 守卫删除，死亡用例在三臂全绿；泄漏的是 **server 的 twin**（framebuffer 无 wire delete opcode，用例现在两边计数都读）。
- §5.4 的「白盒断言发出的是 `BufferSubDataResident(49)`」在本树**不可达**：resident 发射需要 GPU 常驻存储 → 需要 `AdoptPersistentMap` → split 下 T2 对每次采纳都 decline（`PipeApply.cpp:2331`），T0/T1 归 P11。本阶段证明到「server 按自己的表发布该位、client 镜像在双后端 × 三传输收到」为止；`rsd=` PipeStats 计数器在 P11 落地采纳档后会显示这些记录。
- §5.3 的 `g_programResourceCaches` 删除只在 disaggregated 构建：monolith 消费者已改读归档（red-once 在 monolith 臂也红），但归档成员必须 `#if MOBILEGL_BUILD_DISAGGREGATED` 才能守 G1，pull 构建保留旧缓存——钉子挪动那天一行改完，**P13**。
- 门 3 的「不确定」在 wave 1 E0 之前无法区分「同 case 多次结果不同」与「不同 case 各自低于阈值」；两个历史点值（0.988998 / 0.976494）无 per-case 归属。**已归属（W4，ID-P7-25）**：两个点值都是 OpenRA 同一帧里被静默丢掉的一段 draw（9836 / 14658 像素），run-ahead 与 spawn 臂都掷硬币；修复归 B3。
- ~~`TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary` 在三条 split 臂按名排除~~ **已退（wave 2-D3，`1e1f4bad`）**：split 的 clean gate 改读 storage record（`IsDrawSyncCleanByRecord` 走 `Desc.ViewOf` 到 owner，与 Magma 的 `ResolveWireTextureStorage` 同形），排除删除，三臂 3 红 → 14/14；P5E §5.2 改写。留下的：该场景的 Magma 臂只在 informational 层；`KHR-GL46.texture_view.coherency` 无 split 读数（门 5）。
- **OpenRA 调查裁判点名的 Magma 债（`notes/p7/device-window-1/W4-verify/README.md` §3b、`~/w7/notes/evidence/w4/judge.json`），B3 不动、记在这里**：`TryDrainFrameTransients` 的 wire-only 块 `CollectWireObjects(m_completedSubmitCounter, /*all=*/true)`（`VulkanRenderer.cpp:13526`）无视 `RetireWireObjects` 给未来对象打的 `submitIndex = m_submitCounter + 1` 标签（`WireDraw.inc:113-118, 175-194`）；`HasPendingRecordedWork`（`:13435-13441`）看不见 pre-pass 命令流；`FlushWirePendingCommandsForTextureUpdate = !HasPendingRecordedWork() || FlushPendingCommands()`（`VulkanRenderer.h:148-152`）把「此刻没在录」当「没在飞」；`StagedTextureStore::AdoptRun` 在 extent 变化时不重置 `Bytes`/`Covered`（`StagedTextureStore.h:234-243` vs `:270-284`，一条单测即红）；`FrameContext::WaitAndAcquireNextImage`（`FrameContext.cpp:303-312`）只凭槽 fence 释放中帧 flush 过的命令缓冲。本 trace 都不可达，但都是真债。
- **bsl-esc-menu-854 在 wire 臂上是内存缺陷不是排除项**（W4 §2/§3c）：monolith 807 MiB 峰值通过；spawn 下 server 进程涨到 ~1.2 GiB 在 scudo 二级分配器 `internal map failure (Out of memory)` 死（系统尚余 8 GB → 是映射数或每进程上限，不是系统 OOM），client 同时 543 MiB；inproc 1.6 GiB 也死。**M1 已归因（主机 lavapipe，三臂同图）**：不是 store 的字节复制，是 split server **无界累积死 `VkBuffer`**——`RespecifyWireBuffer`（`VkBufferManager.cpp:189-224`）对每个 `glBufferData` 无条件 `DeferRelease` 进按帧槽分桶的 `m_deferredBufferReleases`，而桶只在 `BeginFrame`/`CollectAllDeferredReleases` 扫（`TryDrainFrameTransients` 与 present 路径），**pbuffer 回放整跑 server 只收到 1 条 present 记录**、drain 约每分钟成功两次 → 25 935 个活 VkBuffer（25 923 死）对 monolith 7 个，lavapipe 上每个一条 `memfd` 映射（27.4k → 41k），+12.2 MiB / 7.2 s，与真机 server 527 → 1162 MB 的爬升同量；轻例 `iris-bsl-in-world` 同形 1000×小（93 活 / 74 死）——随帧界之间的调用数增长，不是结构性。`StagedTextureStore` 是每层第二份拷贝（43 MiB）但平台；`PendingUploads` 0 字节（与 B3 无关）；monolith 式条件 orphan 无效（6701 vs 6702）。附带：`CollectGarbage()`/`pruneWire`（`VulkanRenderer.cpp:6989`）在 `SetupDraw` 转 `SetupWireDraw` 之后、wire 臂从不执行。修复包 **M2**（`VkBufferManager.{h,cpp}`：按 serial 的 fence-gated 回收 + `MOBILEGL_IPC_WIRE_DEFERRED_MB` 水位线；red-once = 单帧内 N 次 respecify 后活 VkBuffer ≤ k×wire buffers）。**未答**：真机上是 `vm.max_map_count` 还是字节（采样器没记 maps 行数；Adreno 的 VMA 256 MiB 块推测是字节）。**「只有一条 present」已答（P-present 调查）**：不是 pbuffer、不是 client 决定——该 fixture 1 303 535 次调用里只有 **2 次 `eglSwapBuffers`**（call 1 301 789 与目标 call 1 303 534），harness 在目标 swap 上先快照后 `exit`（apitrace `retrace_main.cpp` 的 `frameComplete` → `mobilegl_apitrace_exit` 抛出（`tools/trace_replay/CMakeLists.txt:186-189` 的宏替换），在 apitrace `glretrace_egl.cpp` 的 `frame_complete` 里、`drawable->swapBuffers()` 之前解栈），所以到达 MobileGL 的 swap = 1、present 记录 = 1、`OnPresent` = 1；规则 `presents = (swaps with call_no ≤ target) − 1`（OpenRA 30 → 29，B3 测得 33 次 `BeginFrame` = 29 present + 起会 + drain）。client 的 `SwapBuffers → BackendObject::SwapEGLBuffers → Present()` 链无任何 surface-kind 分支，credit 只停不丢。结论：**server 侧任何挂在 present 上的回收都不能假设一帧有界**（一帧可以是 1.3 M 调用的 Iris 换包重编译），M2 的按 serial 回收正是这个形状；`TryDrainFrameTransients` 的两道闸 + 每 8 次的节流不是替代品。附带债：`BackendObject.cpp:385-388` 单活动面守卫对多 surface 的 client 会静默丢 present（本 trace 不触发）——该给 `MGLOG_E_ONCE`/计数。
- ~~`MEASUREMENTS.md` §7.2 的同名比对自 P5b 起未重跑；「22 → texture readback」「3 → query」两行债务的关闭以 wave 3 的重跑为准。~~ **已退（wave 3 包 E1，`3c80cd62`）**：同名重跑 27 条 inproc 1 passed / 26 failed → 24 passed / 3 aborted / 0 failed（spawn 同）。22 条 readback 中 19 绿；3 条是 `MOBILEGL_PIPE_PUSH=0` 对照臂的具名停止（`PipeLegacyMemosDisabled` / `Magma:clear-framebuffer-record`），不是错答；3 条 query 全绿（`MEASUREMENTS.md` §7.2 末）。
- **spawn 上的 verify 需要一个跨进程的比对形状**（wave 3 V1，`notes/p7/verify-split.md` §3.3 / §6）：比对器是同地址空间的仪器，两臂是 applier 写的 `gPipeInputs` 与前端 `GLContext`。`MobileGLServer` 链的是整个库，所以 `MGPipeVerifyReadHook` 在 server 进程里**在**、也被调用；缺的是**预言**——server 的 main 只走到 `MG_Backend::InitServerRoleForSpawn()`，不调 `MG_State::Init()`，`MG_State::pGLContext` 为空，hook 在 `ctx == nullptr` 处返回。补它 = server 把读到的字段值随回复带回由 client 比（或 server 侧立等价镜像），不是注册也不是链接问题。门 2 的文本是 `{monolith, split(inproc)}`，不含 spawn，所以这是记录债而不是门内项。
- Android 模拟器 retrace 车道每轮 2–6 例 flaky（含 monolith 对照）：门 1 的任何模拟器证据都先过「连续 N 轮绿」再算数。
- **X1 之后的两条残余**（`notes/p7/x1-persistent-map-segv.md`）：(a) 首次 push 后页面在映射存活期间重新武装，一个**屏蔽了 SIGSEGV 的非 GL 线程写者**仍会被杀——彻底关闭要把武装改成逐 buffer 可选，是设计改动；(b) `RefaultOfANonWriteAccess` 仍可能拒掉同 `(tid, pc, address)` 的一次合法重复写——延后武装后不再常见，未观察到，记录。
- **P5d 的 Adreno 数字存疑**：X1 证明在 arm64 上注册总是加保护、handler 总是不认领（bionic 指针标记 0xb4 与内核的 `si_addr` 比较），即 mprotect 臂在 Android 上**从未**回答过一次 fault；P5d 报告里把收益记在 mprotect 臂上的设备测量需要重取（`P5D-INPROC-PERFORMANCE.md`）。归优化阶段，不阻塞 P7。
- **B2 返工留下的记录债（`notes/p7/magma-b2.md` §6，ID-P7-35/37）**：(1) 多重采样颜色 resolve 的第二段 `vkCmdBlitImage` 不裁剪目的矩形（越界 = `VUID-vkCmdBlitImage-dstOffset-00248/00249`），与 monolith 臂共有，两臂一起裁是独立的活；(2) `MagmaWireFatal("multisample-resolve-region")` 对源矩形越出读缓冲的 blit 仍带走会话——GL 说那些像素未定义而非错误，应 decline 或钳制；(3) 1 → N 等矩形 decline 拒绝的是合法形状（18.3.1 样本复制），Vulkan 无对应命令、monolith 的可观测本身是 VU 违规；(4) tcp 车道带不了 server 端测试旋钮——六条 knob 条目只在 split + spawn，`spawn_lane_parity.py` 的 `MAGMA_SERVER_ENV_KNOB_NO_TCP` 具名；收回要 `tcp_server_fixture.py` 接受每用例 server 环境；(5) X 镜像与跨格式 Y 镜像的深 / 模板 resolve 以规则 I(a) 具名 decline（合法 blit）；跨格式那条在 renderbuffer 与 texture 的 D24S8 映射到不同 `VkFormat` 的设备上会让 `MsFlip.` 变红；(6) **monolith `DirectVulkan` 臂的翻转 / 缩放多重采样深度 blit**：翻转被拒（目的地保持清屏值、无错误）、缩放被执行（GL 说 `INVALID_OPERATION`）——wire 与 monolith 的分歧，wire 臂是更正确的一方，monolith 修正归 P13 / G1；(7) **Espryt（DirectGLES）的翻转 / 缩放多重采样深度 resolve**：翻转什么也不写且不报错、缩放不报错——P3b / P4b 债；用例在这两类条目上以具名理由 `Skipped`。
- **B3 复审留下的记录债（`notes/p7/magma-b3.md` §7.2，ID-P7-36/37）**：`WaitForFrameSerial` 在 disagg 构建上等的是第一条 `frameSerial >= serial` 的记录，钳制之后若更晚的记录（S2 Present）持有同一序号则地板到不了 `serial`，函数返回 true 而 `IsFrameSerialComplete(serial)` 为 false——唯一调用者 `WaitForTimerQueryResult` 重查就绪，今天退化为一次假超时；修法归 **B4**（只在 `IsFrameSerialComplete` 为真时返回，否则从头重扫，不能 continue）。陈旧注释两处（`CollectAllDeferredReleases` 的「mid-frame drains do not advance m_frameSerial」、`WaitForFrameSerial` 的「floor still advances after one fence wait」）归 B4 一起改。disaggregated 构建的 monolith 臂在中帧 flush 下比 pull 构建更可靠（`OnSubData` 经 `IsResourceBusy` 读同一地板），pull 构建的 `#else` 逐字保留、潜在缺陷由 G1 与 P13 持有。
- **V1 留下的记录债（`notes/p7/verify-split.md` §6，ID-P7-37）**：verify 在 spawn 上需要一个跨进程的比对形状（server 把读到的字段值随回复带回、client 比），门 2 的文本是 `{monolith, split(inproc)}`；server `ReadPixels` 窗口里 pack 半边的字段级盲区很窄（只有恰等于中性 pack 的错推送不可见）；DirectGLES × split 上四条 Magma decline 契约用例按名从 verify-split 车道减掉——Espryt 是否 decline 这些形状归 P3b / P4b。
- **F 片 1–3 之后（`notes/p7/ph-f.md` §4，ID-P7-37）**：`wireFingerprint` 随 `Welcome.dataNonce` / `DataBind` 变更（`0x7d4470f241d23539` → `0x03c4c293a54b3e08`），新 server 对旧 client 回 `Refuse{WireFingerprint}`，旧 server 对新 client **没有 Refuse 帧**（旧 `AcceptPair` 等第二条连接 2000 ms 后关闭，client 只见 `no Welcome … (rc=6)`）——设备上的 server 必须随下一个 APK 重部署；D11 五处 + PH-2、PH-6、PH-1 (3)(4)、PH-7 (5)（fork 前认证）未做，前置是一个能向 spawn / TCP server 投递畸形记录的对端字节驱动（fuzz 臂 2 的第一块），包 **F2**。
- **Ph 簇 F 的两处预认证暴露**（wave 2-F 之前就在树上，F 修复轮审查核实，归 PH-7 (5)「有界的预认证工作 + 失败退避」，P7 之后）：(a) TCP supervisor 对每条连接**先 fork 再读 Hello**（`ServerMain.cpp` `RunSession`），未认证对端每连接可让 server 付出一次 fork + 最长 10 s 的 Hello 等待；(b) `ValidatePeerHandshake` 在 `AuthenticatePeerToken` **之前**跑，`Refuse{ProtocolVersion|WireFingerprint|BuildFingerprint}` 把本端 wire 指纹与 build stamp 送给了未认证的对端。同一轮记下的本地对端债：unix 端点仍按到达顺序配对（`AcceptPair` 250 ms / 2000 ms 窗口），抽象命名空间套接字没有 mode bits，「只有本 app 能连」靠 SELinux 域隔离而非传输层。**指纹不对称**（§10 第 3 条的 `wireFingerprint` 变化）：新 server 对旧 client 回 `Refuse{WireFingerprint}`；旧 server 对新 client **没有任何 Refuse 帧**——旧 `AcceptPair` 等第二条连接 2000 ms 后关掉两条，client 只看到 `no Welcome ... (rc=6)`，即设备上的 server 未随本次构建重装。细节 `notes/p7/ph-f.md` §2.2 / §6.4。
