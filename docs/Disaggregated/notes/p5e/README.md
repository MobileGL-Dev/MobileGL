# P5e — 退役 Espryt draw path 的 lockstep（run-ahead，2026-09-19 收官）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 一句话：client 发布完就走；server 用记录带的句柄解析每一个对象；它唯一可以读的 client 内存，是一条 client 确实被挡在后面（barriered）的记录的残余 fill。契约 `MobileGL/MG_Remote/CONTRACT-P5E.md`（规则 F），裁定 ID-80..136（[`INTEGRATOR-DECISIONS-P5E.md`](INTEGRATOR-DECISIONS-P5E.md)）。
- 十二包三波：c0e → id；vi / sb / pg / tx2 / fb ∥ ra（+ fix1）；pa / mv / gl / ra2。`kMGPipeP5eRunAheadReady` 与 `kMGPipeP5eClientWaitRuleLanded` 均已翻。
- 门：unit strict 两臂 2256/2256；`integration-split` 179/179；`RUN_AHEAD=0` 对照 179/179；`VERB_BARRIER=0` 变红；`MOBILEGL_IPC_AUDIT=1` 179/179。
- 设备：VD12 下 run-ahead 与 monolith 都撞 120 Hz 上限；**VD32（~3550 draws/帧）下 inproc 与 monolith 齐平**（59.9 vs 58.5 fps），run-ahead 相对 lockstep p50 +69%、client CPU −40%。
- **具名未决**：E1 对照需按 ID-114 / ID-122 重新定义（`VERB_BARRIER=0` 会顺手关掉 run-ahead，所求的 `Fatal{BarrierViolation}` 出现 0 次）。
- 报告：[`P5E-RUNAHEAD.md`](P5E-RUNAHEAD.md)（2026-09-24 从上层移入）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5e** 退役 Espryt draw path 的 lockstep
- **状态**：✅ **已收官（2026-09-19）**，附一条具名未决：E1 对照需按 ID-114 重新定义
- **落地什么 / 范围**：契约 `MG_Remote/CONTRACT-P5E.md`（规则 F：**未设障**记录的 apply 不得读任何 client 内存；ID-84 限定它只约束未设障记录）；裁定 **ID-80..136**（`~/w7/notes/p5e/INTEGRATOR-DECISIONS-P5E.md`）。**十二个包，三波**：wave 1 **c0e**（契约 + 线上行：`PipeCalls.def` 第五列 `WaitClass`、`set_program_bindings` opcode 80、`kCapRunAheadApply` 位 10 仅 DirectGLES 臂、`kDrawClientArrays`、子系统位 13、两个旋钮）→ **id**（registry 按 `{slot, gen}` 重键、by-handle resolver、分配器守卫）；wave 2 **vi / sb / pg / tx2 / fb** 五个 per-draw 家族 ∥ **ra**（等待规则、present credit、`gPipeInputs` 变 server 角色内存），加 **fix1**（设备发现的 monolith 臂回归，ID-107）；wave 3 **pa**（program 家族离开前端）、**mv**（multi-draw + 五个档位的门禁条目）、**gl**（门禁机制本身 + 两处潜伏崩溃）、**ra2**（翻开关后暴露的竞态）。**`kMGPipeP5eRunAheadReady` 与 `kMGPipeP5eClientWaitRuleLanded` 均已翻**。最大的那次迁移（逐 draw 的 `GetProgramForDraw`）**没有改动任何线上结构**：记录早就带着逐 link 的 `ProgramArchive`，缺的是仍去问前端对象的读者。报告 [`P5E-RUNAHEAD.md`](P5E-RUNAHEAD.md)
- **验收门 / 证据**：出口门已逐条跑过（BRIEF-P5E §3，2026-09-19 在集成树上，run-ahead 已武装）：**item 1** unit 在 strict 两臂均 **2256/2256**；**item 2/3** `integration-split` **179/179**、逐条普查零 `Fatal{`、`Admitted{` 恰为八行；**item 4(a)** `RUN_AHEAD=0` 作为配对对照 **179/179 绿**；**item 4(b)** `VERB_BARRIER=0` **变红**（车道 62/179）；**item 4(c)** Magma 逐字记下 `run-ahead requested, server does not publish kCapRunAheadApply - running lockstep`，而 Espryt 对照记下 `run-ahead ARMED`；**item 5** 在集成树上重跑 red-once（ID-121 重排后的形式）：倒掉 pa 的臂得 `GetProgramForDraw@DrawArrays`、倒掉 mv 的臂得 `GetBoundVertexArray@DrawArrays`，**恰好各一个具名对**，还原后各自转绿；**item 7** `MOBILEGL_IPC_AUDIT=1` **179/179**。**唯一未过的是 E1 对照脚本本身**（ID-122）：已修掉它两个遮蔽性缺陷（选中了一个在本车道永远跑不起来的阳性对照用例；ctest 与日志助手对“谁被选中”意见不一致），但它索要的 `Fatal{BarrierViolation}` 在整次运行里 **出现 0 次**：`BATCH_WAITS` 默认值 1 下那个重叠不发生，而 run-ahead 武装后 `VERB_BARRIER=0` 还会顺手关掉 run-ahead（ID-114），它产生的那条臂**既不是 lockstep 也不是 run-ahead**。E1 需要重新定义它要证伪什么，不是再调阈值；在想清楚之前不动它的计数规则，否则只是换一种伪绿。G1 仍由 CI 断言（ID-123）

## 实测：P5e（wave 1 `f6cfcbd3` → wave 2 `44f91c74` → fix1 `66621767` → ID-110 `3cc4e1ec`）（原 `MEASUREMENTS.md` §10）

P5e 尚未收官（`kMGPipeP5eRunAheadReady` 仍为 false），本节记的是**设备验证一轮**，问题是"八包合并后两条传输臂还能不能正常跑游戏"，不是配对性能。

### 主机门（WSL `~/w7/p5e-int`，split flavour，头 `3cc4e1ec`）

| 门 | `44f91c74` | `3cc4e1ec` |
|---|---|---|
| unit | 2250/2250 | **2250/2250** |
| `integration-split` | 113/113 | **116/116** |
| `integration-gpu`（两条运行时臂，ID-109 起为常设步） | **1157/1294**（137 SEGFAULT / 38 scenario） | **1294/1294** |
| `integration-split-strict` | 7/116 | 7/116（预期红） |

`integration-gpu` 那一列是本阶段最重要的一个数：`44f91c74` 的 137 个 SEGFAULT 在当时**没有任何门禁步骤会看到**——`integration-split` 的每一条都导出 `MOBILEGL_TRANSPORT=inproc`，而 push 构建有两条服务端臂。详见 `CURRENT_STAGE_PROGRESS.md` §2.5。

### 设备（Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 "test"，VD12，DirectGLES；进世界后 60 s 采样）

| 臂 | 进世界 | fps | draws/帧 | 进程 | Fatal / crash buffer |
|---|---|---|---|---|---|
| monolith（`44f91c74`，修复前） | — | — | — | **65 s 后进程消失** | SIGSEGV，栈见 §2.5 |
| monolith（`3cc4e1ec`） | 31 s | 263.5 | 845 | 存活 | 无 |
| inproc（`3cc4e1ec`） | 31 s | 136.1 | 853 | 存活 | 无 |
| inproc 复跑（`3cc4e1ec`） | 32 s | 140.3 | 852 | 存活 | 无 |

monolith 这一次未被 120 Hz vsync 封顶（与 §9 的 205.8 那次同类），两臂 fps 不可直接相比。inproc 的 136-140 与修复前记录的 140.0 / 144.9 同档：fix1 改的那行两臂都读，split 臂没有被拖慢。

### 测量纪律（本轮与上一轮踩到的坑，全部是污染而非代码问题）

- `adb install` 的收尾会在一两分钟后杀掉正在跑的同包进程（`am_kill … due to installPackageLI`）：装包与测量之间必须等到 `dex2oat` 退出。
- 派给外部 CLI 的任务，其进程会活过 harness 的完成通知；**派出后不得自己再跑同一件事**，否则两套实验同时对一台手机 force-stop / 清 logcat，得出假失败。
- 不得在脚本正被 bash 执行时修改它（边读边执行）；要改先冻结副本。
- 游戏内 F3 的 `GIT@` 戳记在增量构建下是旧的，**判断库版本要看 APK 里 `.so` 的符号 / 字符串**，不看戳记。

## 实测：P5e wave 3 — run-ahead 武装后的设备矩阵（头 `25fba0d5`）（原 `MEASUREMENTS.md` §11）

Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 `test`，VD12，DirectGLES，~850 draws/帧。
**CPU 定频**（见下），GPU 钉 pwrlevel 0 且每臂重新断言，风扇开，进世界后 20 s 稳定再取 30 s 窗口，臂交错。

### 定频，以及为什么这一节必须先讲它

第一轮矩阵没有定频，于是 **monolith 自己那一臂**的逐帧 CPU 在两次运行之间从 4.43 ms 跳到 7.07 ms，
而慢的那次**温度更低**（53.5 °C vs 58.9 °C）——不是热降频，是 walt governor 把 policy0 从 2745 MHz
拉到 748 MHz。未定频的逐线程 CPU ms/帧**不能跨臂比较**，那一轮矩阵作废。

`tools/device_bench/pin_device.sh` 不认识本机 serial，并**明确拒绝猜**（"pin path 因 SoC 而异，猜错会静默失败"）——
这是对的，也正是它没有静默半应用的原因。按本机实测节点另写 `pin_redmi.sh`：写序 min→硬件底、max→目标、
min→目标（顺序要紧，否则按 stock 相对目标的位置会半应用），并**回读 `scaling_cur_freq` 确认**，跑完再 check 一次。

### 结果（每臂两次，离散度 < 2%）

定频 little 1555200 / big 1958400：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.1 | 117.0 | 6.19 | — |
| inproc run-ahead | 117.6 / 117.9 | 118.3 / 118.6 | 6.71 / 6.63 | 4.30 / 4.32 |
| inproc lockstep (`MOBILEGL_IPC_RUN_AHEAD=0`) | 111.1 / 110.7 | 114.9 / 115.1 | 8.27 / 8.09 | 6.44 / 6.41 |

定频 little 1996800 / big 1958400（更高会被厂商限幅器夹住）：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.6 / 115.4 | 117.2 / 117.5 | 6.45 / 6.25 | — |
| inproc run-ahead | 117.6 / 118.0 | 118.5 / 118.6 | 6.92 / 6.87 | 4.43 / 4.35 |
| inproc lockstep | 111.6 | 113.7 | 8.16 | 6.53 |

### 读法

- **配对 A/B**：`MOBILEGL_IPC_RUN_AHEAD=0` 与默认是同一构建、同一份记录、同一定频，唯一差别是客户端等不等
  （ID-114：**不得**用 `VERB_BARRIER=0`，它是 `RunAheadArmed()` 的第一个合取项，会顺手关掉 run-ahead，
  而且实测几秒内即 `Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@ClientWaitSync"}`）。
  差值：client **−18%**、apply **−33%**、fps p50 **+6%**。
- **两个定频档下 monolith 与 run-ahead 都撞到 120 Hz 面板上限**（max 117-118.6），**只有 lockstep 撞不到**
  （max 113.7-115.1）。所以"inproc 是否追平 monolith"在本机真实负载上的答案是**追平了**；阶段开始时是
  **0.62-0.74 倍**（见 §10）。
- 逐帧 client CPU：run-ahead 6.9 / monolith 6.3，多约 **8%**，两者都在上限之下有余量。
- **未回答**：面板上限之上谁更快。本机定不住更高频率；一次未定频的高频窗口给出 monolith p50 201 /
  run-ahead p50 181（约 1.10 倍），但那次不可配对，只作方向性提示。需要关 vsync 或换上限更高的设备。

### 侧写（`25fba0d5` 之前，`7c6f6886`，说明 run-ahead 为什么值得做）

symbolized `simpleperf cpu-cycles`，客户端 GL 线程：`SessionProducer::WaitForAppliedOrEventBacklog`
**31.58% 自身时间**，其后第二名只有 3.72%。设备计数器同期显示客户端每帧进入 doorbell 等待约 **916 次**
（对 ~849 次 draw），其中 99.8% 靠自旋解决——`MOBILEGL_IPC_SPIN_US=0` 让每次等待都 park，帧率塌到 20.9。
所以代价是**会合本身**，不是自旋参数，这也是为什么修法是"不再每次 draw 会合"而不是"把每次会合做便宜"。

### 11.1 渲染距离 32 的对照（同头 `25fba0d5`，~3550 draws/帧）

§11 的矩阵在 VD12 下跑，而两条臂都撞到了 120 Hz 面板上限，所以那一节回答的是"能不能撞到上限"，
不是"谁更快"。把渲染距离拉到 **32**（`options.txt` 的 `renderDistance`，备份为 `.p5e-backup`，测完已还原）
负载涨到 **~3550 draws/帧**（VD12 的 4.2 倍），**两条臂都远离任何上限**（游戏自身 `enableVsync:false`、
`maxFps:260`，实测 max 只有 37-71），这才是可比的一轮。

条件：CPU 定频 little 1555200 / big 1958400（跑前跑后各 check 一次，均 PINNED），GPU 钉 pwrlevel 0，
风扇开，进世界后**静置 75-150 s**再取 40 s 窗口——VD32 的区块流式加载远比 VD12 长，沿用 20 s 静置会把
加载期算进稳态。臂交错。

| 臂 | n | fps p50 中位数 | p50 范围 | client ms/帧 中位数 | apply ms/帧 |
|---|---|---|---|---|---|
| monolith | 5 | **58.5** | 56.3 – 67.9 | 16.19 | — |
| inproc **run-ahead** | 5 | **59.9** | 56.8 – 61.1 | **15.72** | 11.1 – 12.1 |
| inproc lockstep（`RUN_AHEAD=0`） | 2 | 35.5 | 34.5 – 36.5 | 26.17 | 19.7 – 20.5 |

**结论：在 VD32 下 inproc 与 monolith 齐平**，中位数上 run-ahead 甚至略微领先（59.9 vs 58.5，
client CPU 15.72 vs 16.19），差值远在 monolith 自身的离散度之内——**monolith 五次的范围是 56.3-67.9
（±10%），run-ahead 是 56.8-61.1（±4%）**，也就是说 split 臂不但追平，还更稳。`mono1` 的 67.9 是全场唯一
的离群值（装包后第一次运行）。

**run-ahead 相对它取代的 lockstep：p50 +69%、client CPU −40%、apply CPU −43%。**
负载越重它越值钱，这是预期内的——会合次数随 draw 数走，VD12 是每帧约 900 次，VD32 约 3600 次。

两条臂**都是 client 线程打满**（run-ahead 15.72 ms CPU / 16.7 ms 帧 ≈ 94%；monolith 同理），所以这一列
是各自真正的瓶颈，可以直接比。

**下一个机会，不是缺陷**：apply 线程只有 11.1-12.1 ms，client 有 15.7 ms——**两侧不平衡**。把工作从 client
挪到 apply 会直接降低瓶颈。这在 lockstep 下毫无意义（两侧串行相加），run-ahead 之后才成立，是本阶段
解锁出来的新优化方向，留给后续阶段。

## 落地形状（原 `ARCHITECTURE.md` §17.7）

### 17.7 P5e：client 在 Espryt draw 路径上跑在 apply 前面（权威为 `MobileGL/MG_Remote/CONTRACT-P5E.md`）

P5d 把 `inproc` 变成 client 线程 CPU-bound，帧仍是 `client 工作 +（每 draw）等 apply + 交接`。那个等待存在，是因为 draw 的 apply 仍在解引用 client 拥有的内存：`FieldOwnership.def` 的对象类 BARRIER-PULLED 行，以及两个 P5C 具名豁免 scope 里的前端键 twin registry。**P5e 的一句话：client 发布完就走；server 用记录带的句柄解析每一个对象；它唯一可以读的 client 内存，是一条 client 确实被挡在后面的记录的残余 fill。**

- **barriered 谓词**（§2.1，裁定 3 / ID-83）：`MGPipeBarriered(op, payload, applierState)` = 静态 `WaitClass` 列 ≠ `kWaitNone`，或 XFB span 打开时的 `kCtxVerb`，或带 `kDrawClientArrays` 的 draw。两侧算同一个函数、同一份数据——client 在 emit 时用 `ctx.IsTransformFeedbackActive()`，server 用应用过的 `MGPContextValues` 镜像，而 `set_context_values` 在 ring 上先于 verb，所以两者一致。没有第四条升级；加一条是整合者的裁定加契约里的一行。
- **规则 F**（§0）：**unbarriered 记录的 apply 不读任何 client 内存**——不解引用前端对象、不读 BARRIER-PULLED 行、不探测也不铸造 `MGPipeSlots()`、不持有前端对象的 `SharedPtr`。违反是具名 Fatal，**与 `MOBILEGL_IPC_STRICT_ERRORS` 无关**：那个值按构造就是撕裂或陈旧的，没有"计一笔"的臂。barriered 记录原样保留 P5C 的语义（裁定 4 / ID-84），这正是第一次落地能做小的原因：回读、CopyTex、`GetTexImage`、`set_storage_block_binding`、XFB 都留在 barriered 且拖后。
- **`gPipeInputs` 变成 server 角色内存**（§3）：unbarriered verb 的 validate 只跑第 2 步（tracker 走查）和第 3 步（发射器）——它们产出记录，这正是 unbarriered verb 被允许交给 server 的全部——序号自增、verb 戳、撤回 server 戳与整个第 4 步都不跑。`MGPipeInputUnfreshRead` 的计数器在 unbarriered 记录下变成**无条件中止**，这是把 `integration-split-strict` 从"预期红"变成硬绿门的那一处（§7）。
- **run-ahead 的臂**：`RunAheadArmed()` = `m_barrierArmed && Ipc.RunAhead && Caps().HasCap(kCapRunAheadApply)`，在 `Start` 之后第一次 caps 采纳时**锁存**，此后只能被关掉、不能被打开——已经不等就发布出去的记录收不回来。Magma 的 `CallMask` 永远不带 bit 10（`MGPipeRunAheadCapBitsFor` 是纯函数，`MagmaPipeIdentityTest` 钉住），Espryt 则由 `MG_Backend/Init.cpp` 的 `kMGPipeP5eRunAheadReady` 一个常量把整相位关在惰性里，直到整合提交把它翻过来。
- **对照臂**：`MOBILEGL_IPC_RUN_AHEAD=0` 是 A/B 的**对照**而不是负控制——server 代码、fill 决定、每一条记录两臂都一样，唯一的差别是 client 等不等；`MOBILEGL_IPC_VERB_BARRIER=0` 保留它自己的意思，并且因为它是那个合取式的第一项，它会直接把 run-ahead 关掉。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P5E.md`](BRIEF-P5E.md) | BRIEF-P5E — the package plan for retiring the lockstep on the Espryt draw path |
| [`CONTRACT-P5E-draft.md`](CONTRACT-P5E-draft.md) | CONTRACT-P5E (draft) — the client runs ahead of apply on the Espryt draw path |
| [`FACTS-P5E.md`](FACTS-P5E.md) | P5e — the authoritative fact sheet for the doc update |
| [`FLIP-CHECKLIST.md`](FLIP-CHECKLIST.md) | P5e wave 3 — integration and the flip, as an executable checklist |
| [`INTEGRATOR-DECISIONS-P5E.md`](INTEGRATOR-DECISIONS-P5E.md) | INTEGRATOR-DECISIONS-P5E (2026-09-18, base `feat/disaggregated @ 2fde7034`) |
| [`KIMI-AUDIT-P5E.md`](KIMI-AUDIT-P5E.md) | Read-only audit brief (Kimi): every apply-thread read of client-owned memory on the DirectGLES inproc draw path |
| [`kimi-audit.md`](kimi-audit.md) | P5e audit — apply-thread reads of client-owned memory on the DirectGLES inproc draw path |
| [`KIMI-DEVICE-VERIFY.md`](KIMI-DEVICE-VERIFY.md) | Device verification brief (Kimi): build FCL with the P5e MobileGL and prove it still runs |
| [`P5E-RUNAHEAD.md`](P5E-RUNAHEAD.md) | P5e：退役 Espryt draw path 的 lockstep（run-ahead） |
| [`PACKAGE-PREAMBLE-P5E.md`](PACKAGE-PREAMBLE-P5E.md) | P5e package preamble (read first; applies to every package agent) |
| [`PERF-BASELINE-7c6f6886.md`](PERF-BASELINE-7c6f6886.md) | inproc vs monolith at `7c6f6886` — measured, and why |
| [`PERF-CORRECTION.md`](PERF-CORRECTION.md) | CORRECTION to `PERF-PROFILE-7c6f6886.md` §5 — run-ahead removes much less of the fill than §4 implied |
| [`PERF-NEXT-TARGETS.md`](PERF-NEXT-TARGETS.md) | After run-ahead: there is no second lever |
| [`PERF-PROFILE-7c6f6886.md`](PERF-PROFILE-7c6f6886.md) | The symbolized profile, and what run-ahead actually removes |
| [`PREAMBLE-ADDENDUM-WAVE3.md`](PREAMBLE-ADDENDUM-WAVE3.md) | Addendum to `PACKAGE-PREAMBLE-P5E.md` for wave 3 (pa / mv / gl), 2026-09-18 |
| [`SCOUT-P5E.md`](SCOUT-P5E.md) | P5e scouting brief: retire the lockstep on the Espryt draw path (client runs ahead of apply) |
| [`scout-S1.md`](scout-S1.md) | S1 — VAO / vertex input / index, under run-ahead |
| [`scout-S2.md`](scout-S2.md) | S2 — textures and samplers under run-ahead (P5e scouting report) |
| [`scout-S3.md`](scout-S3.md) | S3 - framebuffers, attachments, images (P5e scouting, head `2fde7034`) |
| [`scout-S4.md`](scout-S4.md) | S4 - programs / shader state (E note a.2) at head `2fde7034` |
| [`scout-S5.md`](scout-S5.md) | S5 — buffer binding points and the remaining rows (P5e scouting report) |
| [`scout-S6.md`](scout-S6.md) | S6 - transport mechanics of run-ahead (head `2fde7034`, paths under `MobileGL/`) |
| [`scout-S7.md`](scout-S7.md) | S7 - identity: the five twin registries and the allocator (head `2fde7034`) |
| [`STRICT-BASELINE-2fde7034.md`](STRICT-BASELINE-2fde7034.md) | Strict-lane baseline at `2fde7034` (the P5e starting point) |
| [`STRICT-WORKLIST.md`](STRICT-WORKLIST.md) | P5e — what is actually left before the strict lane can be a hard gate |
| [`TASK-c0e.md`](TASK-c0e.md) | Package c0e — the contract and the wire (P5e; lands first, inert) |
| [`TASK-fb.md`](TASK-fb.md) | Package fb — framebuffers, attachments, images, blit from records (P5e wave 2) |
| [`TASK-gl.md`](TASK-gl.md) | TASK-gl — make the strict lane mean something, and make the flip survivable |
| [`TASK-id.md`](TASK-id.md) | Package id — identity: handle-keyed twins, the guard, the scopes (P5e; lands second) |
| [`TASK-mv.md`](TASK-mv.md) | TASK-mv — the multi-draw path stops reading the frontend VAO |
| [`TASK-pa.md`](TASK-pa.md) | TASK-pa — the program family leaves the frontend on the draw and dispatch paths |
| [`TASK-pg.md`](TASK-pg.md) | Package pg — programs: the twin and every reflection answer come from records (P5e wave 2) |
| [`TASK-ra.md`](TASK-ra.md) | Package ra — the wait rule, present credit, server-owned `gPipeInputs` (P5e; parallel, lands last) |
| [`TASK-ra2.md`](TASK-ra2.md) | TASK-ra2 — make run-ahead actually correct |
| [`TASK-sb.md`](TASK-sb.md) | Package sb — `set_shader_buffers`: the binding-point tables cross the wire (P5e wave 2) |
| [`TASK-tx2.md`](TASK-tx2.md) | Package tx2 — textures and samplers by handle and window (P5e wave 2) |
| [`TASK-vi.md`](TASK-vi.md) | Package vi — VAO / vertex input / index from records (P5e wave 2) |
| [`WAVE3-CENSUS.md`](WAVE3-CENSUS.md) | Wave 3 census — the strict lane at `3c13fa4b` (pa + mv + ID-124 merged) |
| [`reports/`](reports/) | 14 个文件：各包报告与设备验证（`device-verify-3cc4e1ec.md` 等） |
