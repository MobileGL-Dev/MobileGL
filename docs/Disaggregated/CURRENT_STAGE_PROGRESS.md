# 当前阶段进度

**P5f 已收官（2026-09-20）**。最终行为验收头：WSL `cfca93c7885fd8db1e881f91189ba59090ae9c52`，Windows `c42577a4` 等价；本次整合来自 `codex/p5f-close`，主分支合入记录见 [`notes/p5f/close-report.md`](notes/p5f/close-report.md)。P6 的 P5f 前提已解除，a6 / c6 / spawn 尚未实施。以下 §2 为当前验收；其后的 P5c/P5d/P5e 数字与性能章节保留为历史，不替代当前结果。

## 1. 阶段状态

| 阶段 | 状态 | 范围 / 证据 |
|---|---|---|
| P0 / P0.5 / P1 / P2 / P3a / P4a（monolith 跑道） | 已落地 | `ROADMAP.md` 阶段表；`MEASUREMENTS.md` §1–§5 |
| **P5** 首个 IPC 帧（reduced path，lockstep inproc） | 已收官 | `ff2994d9..37fc4fdb`；`MEASUREMENTS.md` §6 |
| **P5b** inproc 下的 verb 迁移（Minecraft 优先） | **已收官（2026-09-16）** | `37fc4fdb..82683d4a`；`MEASUREMENTS.md` §7 |
| **P5c** `inproc` 共享内存读点归零 | **已收官（2026-09-17）** | `11ac3de6..b88e8487` + triage 修复；契约 `MobileGL/MG_Remote/CONTRACT-P5C.md`；审计 `~/w7/notes/p5c/p5c-audit-v1.md` |
| **P5d** `inproc` 性能专项 | **已收官（2026-09-18，三轮）** | `cb06538c`、`56a77348`、`1f8de61b`；报告 [`P5D-INPROC-PERFORMANCE.md`](P5D-INPROC-PERFORMANCE.md)；`MEASUREMENTS.md` §9 |
| **P5e** 退役 Espryt draw path 的 lockstep | **已收官（2026-09-19）**，附一条具名未决（E1 对照，ID-122） | 契约 `MobileGL/MG_Remote/CONTRACT-P5E.md`；计划 `~/w7/notes/p5e/BRIEF-P5E.md`、裁定 `~/w7/notes/p5e/INTEGRATOR-DECISIONS-P5E.md`（**ID-80..136**）；**十二个包已全部落地合并**（§2.7），`kMGPipeP5eRunAheadReady` 与 `kMGPipeP5eClientWaitRuleLanded` 均已翻。strict 车道硬绿 179/179、`integration-gpu` 1357/1357、三个构建 flavour 全绿；设备上 VD32 已与 monolith 齐平。报告 [`P5E-RUNAHEAD.md`](P5E-RUNAHEAD.md)，未完成项见 §5 |
| **P5f** 一切状态上 wire | **已收官（2026-09-20）** | f0 / f1 / fc / fe / fm / fs / fr / fv 及分类收口均已完成；零 BARRIER_PULLED、两份 marker 空表、逐帧 rsp=0。主机全门、Claude 异族终审两项修复及 red-once、Redmi 六 clean-boot 臂通过。见 [`close-report`](notes/p5f/close-report.md)、[`close-review`](notes/p5f/close-review.md)、[`device-report`](notes/p5f/device-report.md) |
| **P6** spawn transport | **前提已解除，尚未实施** | 计划 [`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md)、草稿 [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md)；下一步 a6 核验进程/链接边界，再 c6 和 spawn 包。传输原语与 P5f 值控制帧、静态世代/角色隔离已在树上；待做的是进程装配、socket 传输、握手/EOF/device-lost 等，不再重做已落地的控制帧与静态量 |

## 2. P5f 最终验收

| 门 | 总条目 | PASS | skip | failed / 结论 |
|---|---:|---:|---:|---|
| unit | 2310 | 2300 | 10 | 0 |
| integration-gpu | 1377 | 1118 | 259 | 0；既有功能/驱动/专属车道 skip 不计 PASS |
| ordinary / strict integration-split（各自） | 180 | 176 | 4 | 0；strict 三类 marker 均为零 |
| 双块 split + magma | 212 | 206 | 6 | 0；发现集合与 JUnit 完整核对，skip 名称和原因精确匹配 |
| integration-p5f-rsp | 2 | 2 | 0 | GLES、Magma 各一条真实执行，每帧 rsp=0 |
| Redmi 六个 clean-boot 臂 | 66 | 60 | 6 | 0；六个不同 boot-id，36 个统计窗口 rsp=0 |

字段表为 **41 RECORD_SUPPLIED / 6 APPLIER_DERIVED / 0 BARRIER_PULLED / 16 FATAL**。
`strict-expected-markers.txt` 与 `dualblock-expected-fatals.txt` 均为空集合，两侧棘轮仍有效；
空结果、漏跑、重复/notrun/disabled 或额外 skip 不得判绿。双块精确保留以下六个 skip：

- `DirectGLES.Split.TriangleScenario.TheServerStampedAVerbBoundaryOnThisDrawingFrame`
- `DirectGLES.Split.SmallRing.TriangleScenario.TheServerStampedAVerbBoundaryOnThisDrawingFrame`
- `DirectGLES.Split.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`
- `DirectGLES.Split.SmallRing.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`
- `DirectVulkan.Split.Fm.ClipDistanceScenario.ADisabledClipDistanceRemovesNothing`
- `DirectVulkan.Split.Fm.ClipDistanceScenario.TheEnablesAreIndependentPerDistance`

前四项是 strict-arming / counting 专属车道的 sibling；后两项是已知 Magma clip-distance
逐项开关限制，原因同样参与校验。独立 RSP 门不允许任何 skip。

pull / push 实际构建及生效宏通过；**G1** `.text` **10806051 → 10806051**、defined symbols
**27815 → 27815**、**0 added / removed / resized / renamed**；**G2** 两者各 **3016** 个测试名，
集合相同；**G14** f1 **3646 → 3744**，没有删除。生成器/闭包门与逐包 red-once 均已执行。
Claude 异模型族终审的 P1（结果集可伪绿）与 P2（callback owner 错投）均修复并实际红转绿，
见 [`close-review.md`](notes/p5f/close-review.md)。

设备门使用最终库在指定 Redmi `2f7cbe2e` 上重跑 GLES/Magma 的 monolith、role0、role1
六臂；仅为本阶段可执行子集的正确性与 rsp 验证，性能只记录，不代替 P5e 的 MC/FPS 实测。
原始身份、skip 原因与运行边界见 [`device-report.md`](notes/p5f/device-report.md)。
Magma 仍不发布 `kCapRunAheadApply`，已由同一双块/strict 判据证明可达子集；P7 应用 buffer /
native-format、P8 client arrays、P12 真窗口到达等具名边界继续保留。

## 2.1 历史实测：P5c / P5d 收官头

以下表格是当时的验收记录；包括当时未做的设备项与非零 rsp，不表示 P5f 当前状态。


| 门 | 结果 |
|---|---|
| P5d 三轮门（WSL `~/w7/p5d-gate`，split flavour，头 `1f8de61b`） | unit **2203/2203**（tcache_count=0）、`integration-split` **111/111**、两条曾 flaky 用例 `--repeat until-fail:5` 全绿；双生成器 --check/--self-test、include 闭包、dirty-surface、doc 引用全绿；G1 由 CI 核（四包自报 pull 构建零变化） |
| P5d 三轮设备（Redmi，CPU 定频，VD12 世界内 30 s） | inproc p50 **103-106 fps**（起点 64.3），client 线程 CPU 9.2 ms/帧（起点 15.0）、apply 7.0-7.4（起点 12.4）；monolith 115（封顶）/ 206（一次未封顶）（此设备 120 Hz vsync 封顶与线程放置不受控，见报告"数字"节） |
| 构建（P5c 收官时） | pull / push / verify / split 四个 flavour 全部通过 |
| G1（pull 符号恒等） | `.text` −16 B，0 增 / 0 删 / **3 认定 resize** / 0 重命名（`SwapchainObject::Create` ev 的表面事件化、`CopyTexSubImage2D` hd 的传输臂、`ScopedRestartIndexSubstitution` 的 server-shadow 臂，均已在合并提交具名）；pull 构建零 `MG_Remote` 符号 |
| 发射表分区（`EmitTables.cpp` 的 `static_assert`） | A 2 / B 54 / C 15，共 71 槽（未变） |
| unit | split **2187/2187**（`MOBILEGL_IPC_STRICT_ERRORS=1` 下同绿） |
| `integration-split`（inproc） | **111/111**（107 + ct 的 4 条 CtWireScenario） |
| `integration-gpu` 普查（inproc，对 11ac3de6 同机基线逐名比对） | 修复后 newly-failing 全部归类：设计红（传输下旧臂具名拒绝的对照车道 + Magma P7 未迁移面，逐条 Fatal 名证据在案）；三个真回归（默认 FBO 格式时序、大 writeback 切片、XFB scatter 读 server shadow）已修并回归绿 |
| G5（p3a / p4a 保护区） | 双绿（`11ac3de6` ↔ 收官头字节一致） |
| 生成器 / 卫生门 | `gen_pipe` / `gen_pipe_field_ownership` 的 --check/--self-test 绿；doc 引用 0 problem；include 闭包 0 problem；dirty-surface rc=0 |
| 纹理 `0xDD` audit（`MOBILEGL_IPC_AUDIT=1`） | bsl in-world（100000 调用）与 iris-complementary in-world 全程零 Fatal |
| `rsp` 按帧实测（`MOBILEGL_PIPE_STATS_PERIOD=1`） | bsl 948.5/帧（38.7/draw）、complementary 1591.9/帧；值类 = 0，残留即 FieldOwnershipTest 钉住的 15 行对象类 |
| Redmi 四臂复测 | **未做**（E-P5c #5 是记录项；本机无设备，需 Redmi `2f7cbe2e` 窗口） |
| 79 trace 普查 | **未重跑**（全集语料不在本机；本机可用语料的实测见 `rsp` 与 audit 行）。P5b 收官数字（72/6/1）仍以其头为准 |

## 2.5 P5e wave 2 与 fix1：设备发现的 monolith 回归（历史，头 `3cc4e1ec`）

八包全部落地并合并：wave 1 = **c0e**（契约 + 线上行）+ **id**（身份按 `{slot, gen}` 重键），wave 2 = **vi / sb / pg / tx2 / fb** ∥ **ra**，集成提交 `44f91c74` 解了三处两包相接的 seam。**`kMGPipeP5eRunAheadReady` 与 `kMGPipeP5eClientWaitRuleLanded` 仍为 false**：翻转它们的门是 BRIEF-P5E §3（strict 车道转硬绿，只余 §7 白名单）与 §4（设备出口），前者尚未达成。裁定见 `~/w7/notes/p5e/INTEGRATOR-DECISIONS-P5E.md`（ID-80..110）。

### 设备验证发现的 monolith 回归与修复（ID-107 → fix1 `66621767` → ID-109 / ID-110 `3cc4e1ec`）

| | |
|---|---|
| 症状 | `MOBILEGL_TRANSPORT=monolith` 下游戏启动即 SIGSEGV：`Lightmap.<init>` → `clearColorTexture` → `glClear` → `SyncCurrentFBO` → `SyncToBackend(前端 FBO)` → `SyncAttachmentSurface` → `SyncMipmapsToBackend(空 SharedPtr)` |
| 根因 | fb 的 `SyncAttachmentSurface` 把**存储同步**指向 tx2 的 by-handle 座，后者传空 `SharedPtr` 并依赖记录臂被选中；而记录臂自身由 `Transport != Monolith` 选择（ID-81）。push 构建自 P4a 起**两条臂都由记录驱动附件**（`FramebufferSubsystemEnabled()` 单独门控，无传输测试），于是 monolith 臂走进了一个无法作答的座 |
| 覆盖漏洞 | 结构性，而非"少写了一个用例"：门禁跑 `unit` + `integration-split`，而 `integration-split` 的**每一条**都导出 `MOBILEGL_TRANSPORT=inproc`；凡是"清一个带纹理附件的 FBO"的场景又都在 `SetUp` 里按 `SplitRuntimeSkipReason` 自跳过——**push 构建的第二条运行时臂在门禁集合里一条条目都没有**。实测 `44f91c74` 上 `ctest -L integration-gpu` = **1157/1294，137 个 SEGFAULT、横跨 38 个 scenario**，全部是带纹理附件的 FBO 用例。回归从来不只在 Lightmap 一条路径上 |
| 修复 `66621767` | (1) `SyncAttachmentSurface` 的存储同步按 `MG_Config::Transport` 选臂——monolith 臂拿回 `f6cfcbd3` 的前端同步，经一个**只用于存储同步**的 monolith 胶水指针（附件形状、空点测试、twin 认领两臂都仍归记录，N-6 的洞不重开）；renderbuffer 半边同理，它的 by-handle 形丢掉了应用可见的 OOM `RecordError`。(2) 三个纹理同步体改用 `RefuseNullFrontendTextureOffTheHandleArm`：空前端对象落到无法作答的臂上是 `Fatal{RoleViolation, "texture-handle-arm"}` 具名中止，而不是空指针解引用；pull 构建里折成与修复前逐 token 相同的常量，不新增调用（D-P）。(3) 新增 `MonolithAttachmentClearScenario`（可变纹理 / 不可变纹理 / renderbuffer 三例，**断言像素而非"没崩"**——附件点未填的 clear 会静默抛 `GL_INVALID_FRAMEBUFFER_OPERATION`，那是同一缺陷的无声形态）与 `DirectGLES.PushMonolithArm.` 注册（ctest ENVIRONMENT 钉 `MOBILEGL_TRANSPORT=monolith`，挂 `integration-split` 标签） |
| ID-109 | 标签保留（`integration-split` 指"门禁要跑的集合"而非"跑 inproc"；ID-81 给 push 构建两条服务端臂，只有一条在集合里，"门禁全绿"就一直等于"两条里的一条全绿"），**并把 `integration-gpu` 升为集成分支的常设门禁步骤**（`p5e_gate.sh gpu`，-j 4 约 10 分钟），不下放到每个包 |
| ID-110 | 新增 `MGB_TEXTURE_RECORD_ARM_SELECTED()`（`Transport != Monolith` 且 `TextureResourceSubsystemEnabled()`），把"记录臂被选中"这句话只说一次；`texBufferByRecord` 那处"靠链条安全"的座改为自述式传输测试。有传输时该合取项已被 `pushedStorage != nullptr` 蕴含，故行为不变、不关门 |

### 头 `3cc4e1ec` 的门（WSL `~/w7/p5e-int`，split flavour）

| 门 | 结果 |
|---|---|
| build | rc=0 |
| unit | **2250/2250** |
| `integration-split` | **116/116**（113 + 三条新的 monolith 臂条目） |
| `integration-gpu`（ID-109 新增步） | **1294/1294**（修复前同机 1157/1294） |
| 生成器 / 卫生门 | `gen_pipe`、`gen_pipe_field_ownership`、`gen_pipe_dirty_surface` 的 --check/--self-test 与 include 闭包共七项全 rc=0；doc 引用只余 `CONTRACT-P5.md:524` 的既有歧义行 |
| `integration-split-strict` | **7/116**（预期红，与 fix1 的基线逐数相同；本阶段进度以其"具名标记集合"而非通过数衡量） |

### 设备验证（Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 "test"，进世界后 60 s 采样）

| 臂 | 进世界 | fps | draws/帧 | 进程 | Fatal / crash buffer |
|---|---|---|---|---|---|
| monolith | 31 s | **263.5** | 845 | 存活 | 无 |
| inproc | 31 s | **136.1** | 853 | 存活 | 无 |
| inproc（复跑） | 32 s | **140.3** | 852 | 存活 | 无 |

两臂截图为同一视角同一世界，画面正确。monolith 这一次未被 120 Hz vsync 封顶（与 P5d 的 205.8 那次同类），故**不与 inproc 直接比大小**——本轮问的是"改完还能不能正常跑"，不是配对性能。inproc 的 136-140 与 ID-107 记的 140.0 / 144.9 同档，说明 fix1 触到的那行（臂选择本身，两臂都读）没有拖慢 split 臂。库版本以符号探针确认（`texture-handle-arm` 等四个字符串在 APK 的 `lib/arm64-v8a/libMobileGL.so` 内），不看游戏内的 `GIT@` 戳记——增量构建下它是旧的。

## 2.6 历史：P5e wave 3，strict 硬绿、run-ahead 武装（头 `25fba0d5`）

四个包：**pa**（program 家族，逐 draw 的那次拉取）、**mv**（multi-draw 的 VAO 读点 + 五个档位的门禁条目）、
**gl**（门禁机制本身 + 两处潜伏崩溃）、**ra2**（翻开开关后暴露的竞态）。裁定 ID-111..136。

### 2.6.1 strict 车道成为可被脚本判定的硬绿

`GetProgramForDraw@DrawArrays` 从 62 条归零（**无线上改动**：记录早就带着每次 link 的 `ProgramArchive`，
缺的是仍去问前端对象的读者），`GetProgramForDispatch@DispatchCompute` 7 → 0，
`GetBoundVertexArray@DrawArrays` 9 → 0。采纳规则最终是**三个析取项**：

1. 该 verb 的线上 op 静态设障（ID-116）；
2. 该字段的退役相不指向本阶段（ID-125）；
3. 记录是**按升级**而非按静态类设障的（ID-128，XFB 活跃与客户端数组两种 draw）。

三条都不是预想出来的，都是被真实条目逼出来的。白名单由 `gen_pipe_field_ownership.py` **生成**而非手抄
（ra 原本手抄的 13 行在两个方向上都错），棘轮**两侧**都会失败——新出现的对要认领，不再出现的对要删除，
否则车道会"烂绿"。合并当天棘轮就自己点名了两处：pa 退役的行不再出现，mv 的间接档位逼出了一行新的。

### 2.6.2 覆盖面

| | wave 3 前 | 现在 |
|---|---|---|
| `unit` | 2250 | **2256** |
| `integration-split` | 116 | **179**（+ 2 条 clientarrays 预期红、2 条 magma 预期红） |
| `integration-gpu` | 1294 | **1357** |
| 构建 flavour | 只有 split 编得过 | **pull / push / split 全绿**（ID-124） |

新增的 65 条条目全部覆盖此前**一条门禁条目都没有**的臂或档位。

### 2.6.3 翻开开关之后：strict 硬绿是必要而**不充分**的

翻开两个常量后 `unit`/`gens`/`flavours` 仍全绿，而 `isplit` 掉到 112/181、`gpu` 掉到 1290/1359，**且 flaky**。
70 条红只涉及四个字段、横跨十一个 verb——一个字段一个 verb 稳定失败是漏迁移，**四个字段散在十一个 verb 上且
flaky 是竞态**。根因（ra2 实测，推翻了我 ID-132 的机制猜测）：`gPipeInputs` 的**字段值**已由 `SuppliedFieldMask`
分区、不竞态；竞态的是**元数据标量**。而让它长期不可见的是守卫本身——
`RefusePipeInputsTouchWhileApplierOwnsIt` 的 run-ahead 臂对任何 `isBarrieredFill` 一律豁免，依据是一句
**关于未来**的断言（"客户端马上就要 park"），而真实顺序是 fill → emit → park。修法是把那句断言**变成事实**。
同源的第二个缺陷：`MGP_VERB_OP_LIST` 把整个 draw 家族并到一行，十九个索引 draw verb 都答"设障"、都填充、都不 park。

**教训（ID-132）**：strict 跑在 lockstep 下，`ApplyOne` 把每条记录都盖成 barriered，所以"只在客户端不再等待时
才发生"的事它按构造看不见。阶段出口必须有一条**独立的、翻开开关后的**车道。

### 2.6.4 头 `25fba0d5` 的门（run-ahead 已武装）

| 门 | 结果 |
|---|---|
| build | rc=0 |
| unit | **2256/2256** |
| `integration-split` | **179/179** |
| `integration-gpu` | **1357/1357** |
| `flavours` | rc=0（pull 915 TU push=absent，push 929 TU push=1） |
| 生成器 / 卫生门 | 全 rc=0 |
| 逐条普查（strict） | **179/179 绿，`Fatal{` 为零**，`Admitted{` 恰为 ID-128 的八行 |
| 预期红车道 | `integration-clientarrays-split` 2 条（`(Multi)DrawArrays+CLIENT_ARRAYS`）、`integration-magma-split` 2 条（`GetFramebufferBindingSlot@Clear`），均具名断言 |

### 2.6.5 设备（Redmi `2f7cbe2e`，MC 26.3-rc-3，~850 draws/帧，**CPU 定频**，风扇开，30 s 窗口，交错）

**先记一条测量纠错**：第一轮矩阵**没有定频**，monolith 自己那一臂的逐帧 CPU 在两次之间从 4.43 跳到 7.07 ms，
而那次慢的**温度更低**——是 walt governor 把 policy0 从 2745 拉到 748 MHz，不是热降频。
`tools/device_bench/pin_device.sh` 不认识本机 serial 且**拒绝猜**（正确），故按本机节点另写了 `pin_redmi.sh`，
写序 min→底、max→目标、min→目标，并**回读 `scaling_cur_freq` 验证定频生效**。

定频 little 1555200 / big 1958400（每臂两次，离散度 < 2%）：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.1 | 117.0 | 6.19 | — |
| inproc **run-ahead** | **117.6 / 117.9** | 118.3 / 118.6 | 6.71 / 6.63 | 4.30 / 4.32 |
| inproc lockstep（`RUN_AHEAD=0`，配对对照） | 111.1 / 110.7 | 114.9 / 115.1 | 8.27 / 8.09 | 6.44 / 6.41 |

定频 little 1996800 / big 1958400（本机允许的最高，再高被厂商限幅器夹住）：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.6 / 115.4 | 117.2 / 117.5 | 6.45 / 6.25 | — |
| inproc **run-ahead** | **117.6 / 118.0** | 118.5 / 118.6 | 6.92 / 6.87 | 4.43 / 4.35 |
| inproc lockstep | 111.6 | 113.7 | 8.16 | 6.53 |

**配对 A/B（同一构建、同一定频，唯一差别是客户端等不等）**：client −18%、apply −33%、fps p50 +6%。

**结论**：两个定频档下，**monolith 与 run-ahead 都撞到 120 Hz 面板上限（max 117-118.6），只有 lockstep 撞不到**
（max 113.7-115.1）。也就是说在这台设备的真实负载里，**inproc 现在与 monolith 齐平**，而阶段开始时是 0.62-0.74 倍。
逐帧 client CPU 上 run-ahead 比 monolith 多约 8%（6.9 vs 6.3），两者都在上限之下留有余量。

**面板上限之上谁更快，已由渲染距离 32 的一轮回答**（`MEASUREMENTS.md` §11.1，~3550 draws/帧，两臂都远离任何上限，每臂五次）：monolith p50 中位 58.5、run-ahead **59.9**，client CPU 16.19 vs **15.72**——**齐平，且 run-ahead 略前、离散度只有 monolith 的一半**（±4% vs ±10%）；相对 lockstep p50 **+69%**。下一个机会是两侧不平衡（apply 11-12 ms vs client 15.7 ms），run-ahead 之后才成立。

**尚未回答的**：VD12 下面板上限之上谁更快。本机定不住更高的频率，而未定频的一次高频窗口里 monolith p50 201 / run-ahead
p50 181（约 1.10 倍）——那一次不可配对，只能作为方向性提示。要回答它需要关掉 vsync 或换一台上限更高的设备。

## 2.7 历史：P5e 落地内容

| 波 | 包 | 内容 | 效果 |
|---|---|---|---|
| 1 | c0e | 契约与线上行：`PipeCalls.def` 第五列 `WaitClass` + `MGPipeWaitClassFor`、`set_program_bindings`（opcode 80）、`kCapRunAheadApply`（位 10，只在 DirectGLES 臂）、`kDrawClientArrays`、子系统位 13、`MGPipeImageAccess` 一张表、两个旋钮、跨包 seam | 十一个包按同一契约并行施工，且全部 inert |
| 1 | id | registry 按 `{slot, gen}` 重键、三个 by-handle resolver、分配器守卫（含按对方身份索引自己表的第二道守卫，ID-101） | 身份不再是前端指针 |
| 2 | vi | 顶点输入 / VAO / 索引 | 普通 draw 路径的 bound-VAO 行退役 |
| 2 | sb | 缓冲绑定点；`MGPShaderBuffers::WritableMask` 加宽（ID-104）；consumer 位 13（ID-106） | 索引绑定点过线 |
| 2 | pg | 逐 link 的反射归档进 SEG_STAGE；`set_program_bindings` 的三条 post-link 尾 | 后续 pa 的前提：数据已在线上 |
| 2 | tx2 | 纹理与 sampler、staged store 的按句柄读、纹理单元窗口 | |
| 2 | fb | framebuffer、附件、image、blit | |
| 2 | ra | 等待规则 `EmitAndWaitTails`、present credit、`gPipeInputs` 变 server 角色内存、事件环流控 | 等待规则本体 |
| 2.5 | fix1 | 设备发现的 monolith 臂回归（ID-107）：`SyncAttachmentSurface` 的存储同步按传输选臂；`MonolithAttachmentClearScenario` + `DirectGLES.PushMonolithArm.` 车道 | `integration-gpu` 1157→1294 |
| 3 | pa | `PrepareForDraw` / `PrepareForCompute` 的 program 拉取离开前端，`SyncCurrentVertexAttributeValues` 改读 `record->Archive->Link` | `GetProgramForDraw@DrawArrays` 62→0、`GetProgramForDispatch@DispatchCompute` 7→0，**无线上改动** |
| 3 | mv | `MultiDraw.cpp` 的 `BoundIndexBuffer` 改按句柄（ID-113）；五个 multi-draw 档位各自的门禁条目 | `GetBoundVertexArray@DrawArrays` 9→0；`integration-split` +60 条 |
| 3 | gl | 生成式采纳谓词、strict 旋钮的第三态、车道两侧棘轮、`ResourceCopyRegion` 等待类、Magma 车道切分；以及两处潜伏崩溃（ID-111 的盖章、ID-112 的编译期绊线） | 车道成为可被脚本判定的硬绿 |
| 3 | ra2 | 翻开开关后暴露的竞态（守卫把"关于未来的断言"当豁免依据）；`BoundDrawIndirectBufferId` 退役并撤回升级 (iii)（ID-136） | 翻开开关后 `isplit` 112→179 |

## 3. 历史：P5c 落地内容

| 包 | 内容 | 效果 |
|---|---|---|
| c0c | `MG_Remote/CONTRACT-P5C.md`：纹理 staged shadow 所有权 / SEG_EVENT blobref 约定 / 按句柄解析 / 两条控制记录 / 残余值字段表 / 角色守卫语义；落地偏差全部写回 | 六包按同一契约施工 |
| tx | `StagedTextureStore`（句柄为键、整段覆盖、defined-ness 追踪）；`SyncMipmapsToBackend` 四臂改读 store + 描述符；Magma mip 改标 server shadow | 纹理纹素不再回读 client；`0xDD` audit 覆盖纹理 |
| ev | `SEG_EVENT` 三个 producer + `kEventGlError`；producer 归 server session 安装；消费端三臂（拆 monolith guard）；`InvalidateCompileEnv` forward 删除 | 反向通道零裸指针 |
| hd | sink / twin 按记录句柄解析（具名 blit 经 verb 句柄工作区、CopyTex、mip、间接族）；`HasDefinedContent` 读描述符位 | 守卫面闭合前的解析层 |
| 合并协调 | 两个具名豁免 scope（通告家族 P4b/P7、G6 registry 家族 P3b/P4b）；PrimitiveRestart 改读 server shadow；Magma 具名 blit 的 G6 消费臂 | 审计漏报的三个无句柄家族有具名归宿 |
| ct | `applier_reset`（77）/ `object_death`（78，framebuffer 首个 wire delete）；GL 线程直调 `MGPipeApplierReset` 成 RoleViolation | 控制面补全 |
| rv | `set_context_values`（79）；值类 BARRIER-PULLED 清零；三 shutter 自答；`MGPAttribValue` 三视图 | rsp 值类 = 0 |
| gt | 纹理九表面 layer-1 守卫（挂 MipmapStorage 汇聚点）；`InBarrierWait` 接线成 gPipeInputs 单写者规则的 client 半边；strict CI 车道（unit 绿门 + 场景预期红）；rsp 按帧实测 | 每层守卫 red-once 按名验证 |
| triage 修复 | 默认 FBO 格式事件的第二类排空点（EGL RPC 返回）；大 writeback 的客户端切片（环 1/4）；XFB scatter 改读 server staged shadow | 普查三个真回归修复 |

## 4. 历史：P5b 落地内容

| 包 | 迁移的槽 / 内容 | 效果 |
|---|---|---|
| c0b | 25 个已测量 class-C 槽的 wire 记录、`WireVerbSink` 分派、具名 Fatal stub、`CONTRACT-P5B.md` | 四包可并行 |
| f1 | `ClearBuffer{iv,uiv,fv,fi}`、`ClearNamedFramebuffer*`、`CopyTexImage2D`、`CopyTexSubImage2D`、`GenerateMipmap` | 34 个车道中止归零 |
| i1 | `BindImageTexture`、`DispatchCompute`、`CopyImageSubData`、`MemoryBarrier`、`ShaderStorageBlockBinding` + 两个伴随槽 | 235 个车道条目解锁 |
| t2 | 四个 stream-output span 行、XFB 对象绑定、`PatchParameteri`、`kCapBackendOwnsXfbCapture` | 138 个车道条目解锁 |
| d1 | 十九个索引 / 实例 / multi-draw / indirect 槽下沉到 `draw_vbo` | Minecraft 主阻塞解除 |
| sync | `FenceSync`、`ClientWaitSync`、`GetSyncStatus`、`WaitSync`、`DeleteSync` 上 apply 线程；client 铸造 Fence 句柄，wire 只过 `{slot,gen}` | 顺带修掉 `MGL_BACKEND_SLOT_PTR_LOCAL` 对 split 恒返回 nullptr |
| blit / mip | `BlitNamedFramebuffer` 经作用域化 client-shadow 绑定下沉；生成 mip 存储按推送描述符验证 | 31 + 7 个 trace 阻塞解除 |
| r1 / r2 | P5 收官审查 13 项（coherent-map 服务器侧绕过、FBO 死亡在 client 线程、`PACK_SWAP_BYTES`、`RingOverrun` 等待、CI 对照九项） | — |
| 收官审查修复 | 索引 span 上界（`Count × IndexSize` 必须装进声明的 run）、fence wait 预算、`Fatal{ReplyStatusInvalid, "ReadPixels"}` | P5b 审查 0 blocker |

## 4.1 历史：P5b 真实负载（inproc，独立 apply 线程）

- `DrawElements` 不再是阻塞：d1 普查时 77 个 Minecraft 后端用例中 28 个渲染通过（SSIM ≥ 0.99995）。
- blit + mip 之后：`improved-transparency-minecraft-26.3` DirectGLES SSIM 1.0 / DirectVulkan 0.999914；`minecraft-1.21.4-fabric-iris-bsl-in-world` DirectVulkan 0.997324、DirectGLES 0.997496；OpenRA 双后端 1.0。
- 79 trace 普查 7 条未过项的首阻塞：`rd12` GLES `Fatal{InitialBytesNotCarried,"resource_respecify"}`、VK `Fatal{BarrierTimeout,"Present"}`；`iris-photon` / `iris-derivative` / `create-indirect` GLES 均 `Fatal{UnmigratedEmulation,"texture-remint-pull"}`；`iris-bsl-esc-menu-854` GLES 同 rd12 GLES；`create-indirect` VK 在 llvmpipe 上内存膨胀（>60 GiB RSS，两次复现，守护杀死）。
- Redmi 出口（2026-09-16）：正确性 8/8；barrier tax 首测，逐例见 `MEASUREMENTS.md` §7.4。

## 5. 开放项（按优先级）

| 项 | 证据 / 去向 |
|---|---|
| `SEG_STAGE` 默认 32 MiB 装不下目标负载的单次 128 MiB 上传；**决定 = 默认不改**，普查与 Redmi 显式 `MOBILEGL_IPC_STAGE_MB=256`；分块 / 专用 carrier 留 P8（`ROADMAP.md` 开放问题 11） | `p5b-results/blit-codex-v1.md`；`MEASUREMENTS.md` §7.2 |
| Magma 应用 buffer、placeholder/native-format 等 P7 功能广度 | P5b 的“89 个错答中82个”是历史普查计数；P5f 已验证无应用 buffer 的 draw/compute、sampler/image record 子集，后续按新语料重新点名，不能继续把旧计数当当前失败数 |
| rd12 GLES `InitialBytesNotCarried/resource_respecify`、rd12 VK `BarrierTimeout/Present`、`iris-bsl-esc-menu-854` GLES、三条 `texture-remint-pull` 仿真槽、`create-indirect` VK 内存膨胀 | 79 trace 普查 `counts.json` / `trace-transitions.json` |
| RGB 三通道 CPU mip 回退仍是具名 Fatal | `p5b-results/mip-codex-v1.md` |
| ~~P5c 历史跨角色清单与对象类残余~~ | **P5f 已全部收口**：双块空棘轮、零 BARRIER_PULLED、两后端逐帧 rsp=0；旧指针 accessor 以 FATAL 保持边界，未伪标为 record-supplied。见 [`close-report`](notes/p5f/close-report.md) |
| 27 个 P5 inproc 错答：22 纹理读回走 client-shadow 回退、3 query、1 inspection、1 FBO/RBO 删除后生命期 | P4b / P7 / P6 债 |
| `SEG_REPLY` 2 MiB 单槽上限；GetCaps 两个 blobref 的载体；PACK-PBO 读回真实形式 | P6+ / P9 的协议广度；`rsp` 残余读取已由 P5f 关闭，不再与这些功能债并列 |
| 15 个 class-C 槽（query / sync 尾 / `GetTexImage` / `SetSwapInterval` 等）无负载命中，仍具名拒绝 | P9 / P10 |
| Redmi 定频行只在树外 `~/w7/notes/p2/devices/pin_device.sh`；钉频口径 2026-09-16 起 1100 MHz（原 1050） | `devices/pin-verification-2026-09-07.md` |
| `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器是临时的，合入 dev 前必须移除 | — |
| ~~P5e：BRIEF §3 的三条阴性对照与逐包 red-once 尚未重跑~~ | 当期后续已执行，结果保留在历史 §2.6 与 P5e 报告；E1 自检债单列保留，不与执行完成混同 |
| **P5e 历史债：`split_negative_controls.sh` 的 E1 对照定义/自检**，原 `BATCH_WAITS=1` 组合下索要的 `Fatal{BarrierViolation}` 无法触发 | ID-122；仍需重新定义其证伪对象。P5f 的双块开绿/关红、RSP 硬门与逐包 red-once 不代替修复这条旧脚本 |
| ~~P5e：per-role stamp 存储未落地~~ | **P5f f1 / fs 已完成**：server 私有 stamp/identity/liveness、client 独立块与角色本地诊断。旧契约的历史 UNLANDED 注记不再作为当前施工状态 |
| **P5e：VD12 面板上限之上谁更快未答**（本机定不住更高频）；VD32 那一轮回答了一般性问题 | `MEASUREMENTS.md` §11 / §11.1 |
| **新机会（非缺陷）**：VD32 下 apply 11-12 ms 对 client 15.7 ms，两侧不平衡；把工作从 client 挪到 apply 会直接降瓶颈——lockstep 下无意义，run-ahead 才解锁 | `P5E-RUNAHEAD.md` 末节 |

## 6. 下一步

1. **P6 a6 尚未执行**：按 [`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md) 核验 P5f 的 wire-only 结论在独立进程装配中成立，重点是角色链接闭包、段映射/句柄传递与生命周期边界；控制帧、静态世代与对象字段不重新施工。
2. a6 后推进 **c6 契约与 spawn 实现**：SocketTransport、ServerMain、握手、EOF / device-lost 及子进程身份门。`integration-spawn` 应与现有 split 同名，且日志证明真正在子进程执行；这些代码和验收尚未实施。
3. P7/P8/P9 的 buffer、格式/placeholder、client arrays、大 blob、异步回读等功能债继续按阶段推进；79 trace 全集重新普查后再引用首阻塞，P5b 历史清单不当作当前结果。
4. **保留 P5e E1 历史债（ID-122）**与 VD12 面板上限、线程放置/负载平衡等测量问题。P5f 设备正确性通过不改写 P5e 的性能结论，也不声称解决上述性能问题。

## 7. 记录位置

| 内容 | 位置 |
|---|---|
| P5f 收官、异模型族终审、最终设备门 | [`close-report`](notes/p5f/close-report.md)、[`close-review`](notes/p5f/close-review.md)、[`device-report`](notes/p5f/device-report.md)；分包报告位于 `notes/p5f/` |
| P5 brief / 契约 / 前言 | `~/w7/notes/p5/BRIEF-P5.md`、`MobileGL/MG_Remote/CONTRACT-P5.md`、`~/w7/notes/p5/PACKAGE-PREAMBLE.md` |
| P5 包报告与审查 | `~/w7/notes/p5/p5-results/`（`joint-v1.md` 全门记录、`ab-v1.md` 设备 A/B、`p5-close-codex-review.md`） |
| P5b brief / 契约 / 报告 | `~/w7/notes/p5b/BRIEF-P5B.md`、`MobileGL/MG_Remote/CONTRACT-P5B.md`、`~/w7/notes/p5b/p5b-results/` |
| P5b 收官主机门 | `~/w7/p5b-final-host-348d22a4/`（`reconciliation-20260916.md`、`summary-counts.{json,md}`） |
| P5b 合并普查 | `~/w7/p5b-final-census-348d22a4/`（+ `-resume1/2/3`）、`p5b-results/joint-codex-v1.md` census 块 |
| P5b 设备证据 | `~/w7/notes/p5b/apk/p5bcodex2/`（APK + proof）；`MobileGL/.trace-work/p5b-redmi/p5bcodex2/2f7cbe2e/`（correctness 8/8、`ab-tables.md` 与 bsl 补充表） |
| class-C 普查 | `~/w7/notes/p6/census-classC.md`；基线 `~/w7/p5b-c0b-census-logs/results.json` |
| P5c 审计 | `~/w7/notes/p5c/p5c-audit-v1.md`（59 行清单、wire-clean 清单、与契约的分歧、已有 `rsp` 数字）+ `BRIEF-p5c-audit.md`；Kimi K3 只读静态审计，头 `a79a0af6`，关键行已由集成者逐条抽查 |
| P5d 三轮 | brief `~/w7/notes/p5d/BRIEF-P5D-R3.md`；设备记录 `~/w7/notes/p5d/RESULTS-P5D-R3.md`；四包报告与 lockstep 研究 `~/w7/notes/p5d/reports/`（`E-lockstep-feasibility.md`）；profile 数据 `~/w7/notes/p5d/perf/`（`mg-vd12-8`/`mg-mono-1` 为二轮头 `56a77348`）；bench 脚本 `~/w7/notes/tools/p5d_bench_*.sh` |
| P5c 契约 / 实测 | `MobileGL/MG_Remote/CONTRACT-P5C.md`（含落地修订）；`MEASUREMENTS.md` §8（逐门数字）；普查逐名 Fatal 证据 `~/p5c-fatal-map.tsv`；G1 报告 `~/p5c-g1-report.json`；audit 日志 `~/p5c-audit-{bsl,comp}.log` |
| 门日志 | `~/w7/p5-joint-gate.log`、`~/w7/p5b-quickgate.log`、`~/w7/p5-joint-evidence/` |
| 脚本 | `~/w7/notes/tools/`（`wsl_p5_gate.sh`、`p5_ab_redmi.sh`、`p5b_codex_redmi.sh`、`wsl_build_p5_apks.sh`、`p6_census_*.{sh,py}`、`p5b-c0b-census.sh`） |

## 8. 仍在生效的裁定（ID 索引；过程性裁定与 ID-1..39 见 git 历史）

| ID | 裁定 |
|---|---|
| 41 | G5 第十一行 `FlushPendingRangesFrom` 始终对固定 pin 比较；re-pin 必须同时改两份脚本 |
| 42 | persistent-map 成员性期望 `live == (arm == emulated)` |
| 43 | 带 CALL 旗标的 ring 头会被弹出，不当 wrap filler |
| 47 | `SEG_REPLY` 16 MiB / 8 × 2 MiB slot；超限在 client 侧具名拒绝；ABI fingerprint 尚未混入段尺寸 |
| 49 | ReadPixels 以紧凑形式过线：server 用中性 pack 读进回复，client 按应用 pack 散布 |
| 50/52 | 有传输时 `liveHostBase()` 不得回落到 client 的 `MappedData()` |
| 53 | 每个 `DirectGLES.Split.*` 条目一个私有日志；对照只从被选条目的私有日志取 Fatal |
| 54 | forwarder 每 (dpy, draw, read, ctx) tuple 只 bind 一次；client 的 release 不解绑 apply 线程的上下文 |
| 55 | R-17：33 行生成路由 + 4 escape，`PipeCatalogueTest` 钉 37；`&MGPipeApply*` 地址取用点同样在扫描内 |
| 57 | split 下绑定 PACK PBO 的 ReadPixels 具名拒绝；真实形式（server 写 buffer resource + client `MarkGpuWritten`）留 P6+ |
| 62 | 对照必须把"选中条目被跳过"判为失败 |
| 65 | 27 个 inproc 错答归 P4b/P7 债；普查只记录不设门 |
| 66 | 过程规则：不过度验证、轮次间不审查、每阶段收官由 Codex 审一次、只读 / 非核心任务派给 Codex |
| 67 | 相同 tuple 的重复 make-current 不重发 caps；不同 tuple 重发且 client 立即采纳 |
| 68 | class-C 普查决定迁移顺序（所有 Minecraft trace 首阻塞 = `DrawElements` → Minecraft 优先） |
| 76 | P5b 出口 = 主机门 `348d22a4` complete + 79 trace 普查 + Redmi 正确性 8/8 + 四臂 A/B |
| 77 | Redmi 钉频期望 1050 → 1100 MHz（2026-09-16）；iris-bsl 8 组判为 fixture 123 帧上限，补充表不进 200 帧尾门表 |
| 79 | **P5d 三轮规则（2026-09-18）**：批处理免等只允许 apply 不读残余填充任何字段的记录（`generate_mipmap` 因后端读 `GetActiveTextureUnit` 而必须等）；性能诊断以符号化调用图为准、不再从 self% 猜归属；Redmi 上 fps > ~115 的 monolith 数字视为 120 Hz vsync 封顶，配对比较以逐线程 CPU ms/帧为主指标；`sched_setaffinity` / `taskset` 在该内核对 app 线程无效，线程放置只记录 |
| 81 | **P5e**：handle 臂只在 `Transport != Monolith`；push-monolith 构建保留其前端臂、字节不动 |
| 84 | rule F 只约束**未设障**记录；设障记录保留 P5C 语义 |
| 90 | Magma 整个 P5e 保持 lockstep；`kCapRunAheadApply` 从不在 DirectVulkan 上发布 |
| 107/109/110 | 一整条运行时臂没被门禁（137 SEGFAULT / 38 scenario）；`gpu` 成为常设门禁步；**"记下了句柄"≠"记录臂被选中"**，要自述式传输测试 |
| 111 | 盖章必须与服务端**自身**能力合取，否则翻开开关会当场打死 Magma |
| 112 | `EmittedCallSuppliesTheWholeField` 上编译期绊线：把 Magma 的空指针崩溃换成构建断裂 |
| 116/125/128 | 采纳规则**生成**而非手抄，三个析取项（静态设障 / 退役相不在本阶段 / 按升级设障） |
| 117 | 被采纳的 pull **响而不致命**（`Admitted{` 标签），车道才可能真正变绿 |
| 119/115 | `rsp` 的钉子改写为可检查的形式；车道必须有阳性对照 |
| 121 | §3.5 的 red-once 证据形式在当时不可执行，重排到车道变绿之后 |
| 124 | 四个构建 flavour 里只有 split 编得过；**一条臂的证据必须自证它确实是那条臂**（看生效的编译宏，不看 flag、不看 cache） |
| 129 | 没有 stamp 行的 verb 什么都不采纳（采纳集合宁窄勿宽） |
| 131 | `ctest -L` 是**正则**：新标签不得以既有标签为前缀，且要数一遍证明 |
| 132/135 | **strict 硬绿是必要而结构性地不充分**（它跑在 lockstep 下）；竞态在元数据标量而非字段值，而**打不响的守卫比没有守卫更坏** |
| 136 | 为让车道变绿而加的等待是真实路径上的真实代价：要问**哪条臂在买单**，不是哪条车道绿了 |
| 78 | **P5c 插在 P6 之前**（2026-09-17）：`inproc` 先做到两角色之间除 wire 零直接内存访问，P6 只换传输；值类 BARRIER-PULLED 行在 P5c 过线，对象类行留 twin 表阶段；codex 无额度期间只读审计改派 Kimi（K3） |
