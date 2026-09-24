# P2 — 渲染状态 CSO + 第一片 Track H + 残余值块（`738b289d`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：前端 Tracker（dirty 位、5 个聚合世代、集合 hash 抑制器）；`MGPipeRenderStateSpans` chunk 表（7 pipeline chunk 396 B + 8 dynamic chunk 772 B）+ G7；`CsoCache`（64）；`create/bind_render_state` + `set_dynamic_state`（`SyncRenderState` 一行不动）；`ResidualValueBlock` 棘轮 1248 → 8；Espryt slot 表 + Magma vertex input 重键（11 条 memo 删除 + 2 条重键）；三个 capability 补真存储。
- 门：G1 4 处认定 resize、`.text` +160 B；单元 1566×3；`integration-gpu` 916 三臂；retrace 79/79 push + verify。
- **GO/NO-GO（2026-09-08）：继续。** Release 下边界代价 +6–12%（两机两后端一致），用户接受，自此性能只记录、不设门。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P2** 渲染状态 CSO + 第一片 Track H + 残余值块
- **状态**：✅ `738b289d`
- **落地什么 / 范围**：Tracker（dirty 位、5 个聚合世代、抑制器）；dirty-surface 成门；`MGPipeRenderStateSpans` chunk 表 + G7；`CsoCache`（64）；`create/bind_render_state` + `set_dynamic_state`（`SyncRenderState` 一行不动）；pixel pack / patch / attrib defaults；`ResidualValueBlock` 棘轮 1248 → 8；Espryt slot 表 + Magma vertex input 重键（11 条 memo 删除 + 2 条重键）；`MOBILEGL_PIPE_LEGACY_MEMOS`；三个 capability 真存储
- **验收门 / 证据**：G1 4 处认定 resize、`.text` +160 B；G2 名差 0；单元 1566×3；`integration-gpu` 916 三臂；retrace 79/79 push + verify；对照 44/44。**GO/NO-GO（2026-09-08）：继续**。§3

## 实测：P2（`738b289d`）（原 `MEASUREMENTS.md` §3）

### 3.1 门

| 门 | 结果 |
|---|---|
| G1 pull 构建符号 | 0 增 / 0 删 / 0 重命名；**4 处认定 resize**（`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}`、`_GLOBAL__sub_I_DirectGLES.cpp`）；`.text` +160 B |
| G5 `SyncRenderState` | `RenderStateImpl` 段 sha 逐字节相同 |
| G2 / G14 | 名差 0；0 删除 / +119 |
| 单元 | 1566 × {pull, push, verify} |
| `integration-gpu` | 916/916 × {pull, push, `MOBILEGL_PIPE_PUSH=0`}；渲染状态敏感子集 72/72 |
| `integration-verify` | 828 零 `Fatal{` |
| 79 例 retrace | push 79/79；verify 79/79 armed 零分歧 |
| G7 setter 一致性阴性对照 | 变红并点名 `SetColorMask` |
| `CsoContentAddressing` / verify 三组对照 | 6/6 / 44/44（`PoisonOmitted`、`VerifyCorrupted`、`HandleRecycle`） |

### 3.2 设备配对 A/B（-O0 勘误）

两机 64 次运行（小米 + Oppo，四 trace × 两后端 × finish 开/关，reboot-clean、钉频、best-of-3、尾 200 帧）所用的两臂 APK 是**未优化构建**（`libMobileGL.so` 43.2 MB / `.text` 21.8 MB，对 Release 15.5 / 9.4 MB），绝对值与差值都是 -O0 读数，不作基准线；Release 基准线以 §4.4 为准。-O0 读数：小米 p50 **+8–14%**（四 trace 两后端），Oppo Espryt **+16–18%** / Magma **+10–11%**，p99 同向；finish 开关两臂几乎一致（多出的是客户端 CPU）；计数器两机逐字相同。原始 32 行表在 git 历史（`ea34bccb` 版本的本文件 §10）。harness 误杀记录：`trace-replay-ci.sh` 对 `pidof` 单次采样失败即 force-stop，`dev` 侧跟进。

### 3.3 DriverBench：T1 / T2（桌面 llvmpipe / lavapipe，`wsl_p2_bench.sh`，Release，240 帧 × 5 次取中位数，`ns_per_op`）

| 臂 | `mc_vanilla_draw`（ns/draw） | `mc_state_toggle`（ns/开关对） | `mc_pass_switch`（ns/pass） |
|---|---|---|---|
| native | 4771 | 22968 | 437759 |
| Espryt pull / push / push `PIPE_PUSH=0` / push 位 63 | 5121 / 5443 / 5671 / 5374 | 23753 / 24869 / 24584 / 24630 | 443300 / 446017 / 443807 / 435454 |
| Magma pull / push / push `PIPE_PUSH=0` / push 位 63 | 16900 / 17245 / 17599 / 17444 | 32705 / 33857 / 34082 / 34780 | 438406 / 446067 / 445006 / 444758 |

| `mc_vanilla_draw`，ns/draw | Espryt | Magma |
|---|---|---|
| **T1** = push − pull（整个边界） | **+322**（+6.3%） | **+345**（+2.0%） |
| **T2** = push(`PIPE_PUSH=0`) − pull（P1 残余填充） | +550 | +699 |
| **T1 − T2**（P2 自己加的减的） | **−228** | **−354** |
| 位 63 − push（CSO 内容寻址净值） | −69（≈ 0） | +200（内容寻址每 draw 省 200） |
| blend-toggle / pass switch | +4.7% / +0.6% | +3.5% / +1.7% |

读法：P2 的 tracker + CSO 比它替掉的 P1 残余填充便宜；剩下的 T1 是尚未句柄化的填充与 dirty 走查。`mc_state_toggle` 由 `DriverBenchStateToggle` 钉住每帧 46 对开关；位 63 对照的开关 `CsoContentAddressingScenario` 常开（内容寻址臂 `csom ≤ 4` 且 `csom < csob`，位 63 臂 `csom == csob`）。

### 3.4 计数器读数

`resid=` 已非零且棘轮压到底（1248 → 8）：桌面 push retrace `iris-bsl`，`resid=197.07` B/帧 = 每 draw 0.64 块；`csom/csob` = 8 / 1415（CSO 复用比）。小米 push 臂（-O0，计数器与优化无关）：

| trace | Espryt `ers` / `etl` / `eub` | Magma `mfp` / `mpm` / `mdt` | `resid=` B/帧 | `csom` / `csob`（每 120 帧） |
|---|---|---|---|---|
| `minecraft-1.21.4-in-world` | 6020/1850 · 6884/986 · 6962/908 | 0/7278 · 6030/1248 · 5928/1350 | 178.31 | 2 / 1719 |
| `improved-transparency-minecraft-26.3` | 78960/1367 · 75378/4949 · 75199/5128 | 10740/68928 · 67660/1268 · 78800/868 | 185.36 | 0 / 1157 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | 113/133 · 85/161 · 82/164 | 0/177 · 86/91 · 81/96 | 370.67 | 1 / 122 |
| `minecraft-1.21.4-startup` | 174/185 · 305/54 · 305/54 | 0/118 · 0/118 · 0/118 | 25.08 | 9 / 181 |

CSO 内容寻址在稳态窗口里几乎不再铸造；memo 门形状与 §1.3 的拉取基线一致。**未测**：逐 dirty 位触发率（`FireCount`/`WalkCount` 已实现但不在汇总行）、每 draw payload 直方图（只在 teardown JSON 里）。

### 3.5 遗留判定

- `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` 的 186 条过滤不进比对器（P3a 在 push 下跑了一次 958/958；进比对器推迟到 P3b）。
- Track H 单位成本的日历口径未记录（产出侧在案：11 条 memo 删除 + 2 条重键零回归）。
- 句柄 ABA 对照"重键前红"：修复前 `HandleRecycle` 28 条中 1 条红（`AbaControl` 期望死对象的红却看到替换对象的绿）；修后 32/32，且关掉 `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` 两臂都 `observed=FRESH` 并失败——污染只由被打掉的身份产生。

## 里程碑记录（原 `ROADMAP.md`）

- **P2 出口（2026-09-08）：GO/NO-GO 判定继续**——五部分门全绿，逐线程 CPU 代价约 +10% 被接受；tracker 绝对 ns 上限降为记录项。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P2.md`](BRIEF-P2.md) | P2 implementation brief — the frontend state tracker, render state as a CSO + dynamic state, the residual value block, and the first Track H slice |
| [`INTEGRATOR-DECISIONS.md`](INTEGRATOR-DECISIONS.md) | P2 integrator decisions (feat/disaggregated, 2026-09-07) |
| [`scout-docs-spec.md`](scout-docs-spec.md) | P2 specification, extracted from `docs/Disaggregated/` (read-only scout) |
| [`scout-measure-infra.md`](scout-measure-infra.md) | P2 measurement & gating: what exists, what is missing, what it costs |
| [`scout-render-state.md`](scout-render-state.md) | Scout: render state → CSO + dynamic state (P2 groundwork) |
| [`scout-track-h.md`](scout-track-h.md) | Track H, slice 1 — scout report |
| [`scout-tracker.md`](scout-tracker.md) | Scout: what the P2 frontend state tracker must do, and what already exists |
| [`ab/`](ab/) | 4 个文件：小米 / Oppo 配对 A/B 汇总（-O0 勘误见上文 §3.2） |
| [`bench/`](bench/) | 1 个文件：DriverBench T1/T2 |
| [`devices/`](devices/) | 1 个文件：两台设备的定频报告 |
| [`p2-results/`](p2-results/) | 42 个文件：各包（tracker / spans / espryt / magma / gates）的实现与评审稿 |

捞回的独有材料：[`../recovered/wf5/`](../recovered/wf5/)（BRIEF-P2 的完整版 / 重放版与分部）。
