# 阶段笔记（notes）

这一层放**各阶段的全部细节**：每个阶段一个目录，目录里的 `README.md` 是该阶段的**汇总**（摘要、阶段表原行、逐门实测、落地形状、当时的进度页），其余文件是那个阶段的原始工作笔记——包计划（`BRIEF-*.md` / `TASK-*.md`）、契约草稿、集成者逐条裁定（`INTEGRATOR-DECISIONS-*.md`）、只读审计与普查、包报告、设备记录与证据。顶层的索引（[`../ROADMAP.md`](../ROADMAP.md)、[`../MEASUREMENTS.md`](../MEASUREMENTS.md)、[`../CURRENT_STAGE_PROGRESS.md`](../CURRENT_STAGE_PROGRESS.md)）只留一句话并链到这里；不属于某个阶段的设计正文在 [`../design/`](../design/)，上手与参考在 [`../guide/`](../guide/)。

## 按阶段

| 阶段 | 汇总 | 重要文件 |
|---|---|---|
| P0 卫生、度量、门、骨架 | [`p0/`](p0/README.md) | `BRIEF-P0.md`、`p0-device-findings.md` |
| P0.5 值头与制品头 | [`p05/`](p05/README.md) | `BRIEF-P05.md`、`p05-results/` |
| P1 `PipeInputs` + verify | [`p1/`](p1/README.md) | `BRIEF-P1.md`、`P1-LANE-FINDINGS.md` |
| P2 渲染状态 CSO | [`p2/`](p2/README.md) | `BRIEF-P2.md`、`INTEGRATOR-DECISIONS.md`、`ab/`、`bench/` |
| P3a 句柄 wave 1 | [`p3a/`](p3a/README.md) | `BRIEF-P3A.md`、`INTEGRATOR-DECISIONS.md` |
| P4a 句柄 wave 2 | [`p4a/`](p4a/README.md) | `BRIEF-P4A.md`、`INTEGRATOR-DECISIONS.md`、`ab-redmi/` |
| P5 传输 + inproc | [`p5/`](p5/README.md) | `BRIEF-P5.md`、`PACKAGE-PREAMBLE.md`、`p5-results/` |
| P5b verb 迁移 | [`p5b/`](p5b/README.md) | `BRIEF-P5B.md`、`p5b-results/p5b-close-codex-review.md` |
| P5c 共享内存读点归零 | [`p5c/`](p5c/README.md) | `p5c-audit-v1.md` |
| P5d inproc 性能 | [`p5d/`](p5d/README.md) | `P5D-INPROC-PERFORMANCE.md`（报告）、`RESULTS-P5D-R3.md` |
| P5e run-ahead | [`p5e/`](p5e/README.md) | `P5E-RUNAHEAD.md`（报告）、`INTEGRATOR-DECISIONS-P5E.md`（ID-80..136）、`TASK-*.md` |
| P5f 一切状态上 wire | [`p5f/`](p5f/README.md) | `P5F-WIRE-COMPLETENESS.md`（计划）、`close-report.md`、`magma-runahead.md`、`HANDOFF-CI-INPROC-20260920.md` |
| P6 spawn transport | [`p6/`](p6/README.md) | `P6-SPAWN-PLAN.md`、`P6-CONTRACT-DRAFT.md`、`P6-ENDSTATE-REVIEW.md`、`a6-audit-v1.md`、`gate7-device-ab.md` |
| P6.5 两轴传输 + 跨机 | [`p65/`](p65/README.md) | `evidence-index.md`、`validation-status.md`、`code-review-findings.md` |
| P3b / P4b Espryt 深化 | [`p34b/`](p34b/README.md) | `espryt-d1.md` … `espryt-d3.md` |
| P7 Magma 全量迁移（含 Ph） | [`p7/`](p7/README.md) | `PLAN-PH-P34B-P7.md`、`INTEGRATOR-DECISIONS-P7.md`（ID-P7-1..63）、`HANDOFF-2026-09-23.md`、`device-window-*/`；审查稿在 [`handoff/`](handoff/) |
| **P12**（当前）server 自有屏幕窗口 | [`p12/`](p12/README.md) | `PLAN-P12.md`、`INTEGRATOR-DECISIONS-P12.md`、`device/` |
| P8 / P9 / P10 / P11 / P13（待排） | [`p8/`](p8/README.md)、[`p9/`](p9/README.md)、[`p10/`](p10/README.md)、[`p11/`](p11/README.md)、[`p13/`](p13/README.md) | 目前只有路线图上的完整范围与出口门 |

跨阶段：[`DEBTS.md`](DEBTS.md)（仍开放的债务与去向）、[`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)（开放问题全文，编号稳定）；[`handoff/`](handoff/)（P7 的集成者审查稿，路径被 P7 交接文档引用，保持原位）；[`recovered/`](recovered/)（从失败的 workflow 运行里捞回的 P0–P2 期材料，`wf1` 设计定稿前的计划草案、`wf2` P0、`wf3` P0.5、`wf4` P1、`wf5` P2；与阶段目录逐字节重复的副本已于 2026-09-24 删除，只留独有的）。

## 旧引用怎么找

**2026-09-24 整理时移动的文件**（旧笔记、代码注释与契约里仍按旧名引用它们，文件名没变）：

| 旧位置 | 新位置 |
|---|---|
| `docs/Disaggregated/P5D-INPROC-PERFORMANCE.md` | [`p5d/P5D-INPROC-PERFORMANCE.md`](p5d/P5D-INPROC-PERFORMANCE.md) |
| `docs/Disaggregated/P5E-RUNAHEAD.md` | [`p5e/P5E-RUNAHEAD.md`](p5e/P5E-RUNAHEAD.md) |
| `docs/Disaggregated/P5F-WIRE-COMPLETENESS.md` | [`p5f/P5F-WIRE-COMPLETENESS.md`](p5f/P5F-WIRE-COMPLETENESS.md) |
| `docs/Disaggregated/P6-SPAWN-PLAN.md`、`P6-CONTRACT-DRAFT.md`、`P6-ENDSTATE-REVIEW.md` | [`p6/`](p6/README.md) 同名文件 |
| `docs/Disaggregated/CURRENT_STAGE_STATUS.md` | 并入 [`../CURRENT_STAGE_PROGRESS.md`](../CURRENT_STAGE_PROGRESS.md)；其中 P7 的出口门 / 已落地 / 真机三节在 [`p7/README.md`](p7/README.md)，P12 部分在 [`p12/README.md`](p12/README.md) |
| 旧 `CURRENT_STAGE_PROGRESS.md` 的 P6.5 第一波与 P6 收官快照 | [`p65/README.md`](p65/README.md)、[`p6/README.md`](p6/README.md) |
| 当前阶段页的明细（带裁定编号的表、余项清单、证据位置） | [`p12/README.md`](p12/README.md) 的"当前状态详情"节 |
| `docs/Disaggregated/devices/pin-verification-2026-09-07.md` | [`../guide/pin-verification-2026-09-07.md`](../guide/pin-verification-2026-09-07.md) |
| `ARCHITECTURE.md` 的设计正文（§1–§17、附 A / B） | [`../design/`](../design/) 下十个文件，编号不变；对照表在 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) |
| 旧 `README.md` 的构建运行、代码地图、术语；旧 `ROADMAP.md` 的通用纪律 | [`../guide/`](../guide/) |

**按编号引用的旧章节**（小节编号在新位置原样保留）：

| 旧引用 | 现在在 |
|---|---|
| `MEASUREMENTS.md` §1 / §2 / §3 / §4 / §5 | [`p0`](p0/README.md) / [`p1`](p1/README.md) / [`p2`](p2/README.md) / [`p3a`](p3a/README.md) / [`p4a`](p4a/README.md) 的"实测"节 |
| `MEASUREMENTS.md` §6 / §7 / §8 / §9 | [`p5`](p5/README.md) / [`p5b`](p5b/README.md) / [`p5c`](p5c/README.md) / [`p5d`](p5d/README.md) |
| `MEASUREMENTS.md` §10 / §11 / §12 / §13 | [`p5e`](p5e/README.md)（§10、§11）/ [`p6`](p6/README.md) / [`p34b`](p34b/README.md) |
| `ARCHITECTURE.md` §17.1–§17.4 / §17.5 / §17.6 / §17.7 | [`p5`](p5/README.md) / [`p5b`](p5b/README.md) / [`p5c`](p5c/README.md) / [`p5e`](p5e/README.md) 的"落地形状"节（[`../design/10-phase-shapes.md`](../design/10-phase-shapes.md) 留一张索引表） |
| `ROADMAP.md` 阶段表某行、"P5c 计划与审计"、"立即的下一项：P6.5 第一波"、债务表、开放问题 | 对应阶段 `README.md` 的"阶段表行"节；[`p5c`](p5c/README.md)；[`p65`](p65/README.md)；[`DEBTS.md`](DEBTS.md)（历史全表在 [`p5b`](p5b/README.md) 末节）；[`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md) |
| `ARCHITECTURE.md:NNN`、`MEASUREMENTS.md:NNN` 这类**行号**引用 | 指向写下它时的那个版本，今天已不对应；按上下文所说的节去找，或 `git show <当时的头>:docs/Disaggregated/<文件>` |
| `~/w7/notes/<phase>/<file>.md` | `docs/Disaggregated/notes/<phase>/<file>.md`（这些笔记原先只存在于一台机器的 `~/w7/notes/`） |

## 收录规则

| | |
|---|---|
| **收录** | 全部 `.md` 笔记，以及阶段自己决定入库的小脚本、判读工装与证据摘要（如 `p7/device-window-*/`、`p12/device/`、`p12/gate/`） |
| **不收录** | 大体积、随每次运行重新产生、只在产生它的机器上有意义的证据数据：完整运行日志、APK 与 `.idsig`、`nm` 转储、设备 logcat。上层文档引用它们时给的是路径，不是内容 |

新阶段开工时建 `notes/<阶段>/`，把计划、裁定与报告都放进去；阶段收官时补齐该目录的 `README.md` 汇总，顶层索引只改一行。

## 不受 citation lint 管

CI 的文档引用 lint 只检查顶层、`design/` 与 `guide/`（`.github/workflows/test.yml`），**本目录不在其中，这是有意的**：笔记里的 `file:line` 写在各自当时的代码头上，拿今天的 HEAD 解析必然大面积失败——那是它们作为历史记录的正确状态。要核对某份笔记的引用，用它自己声明的那个 revision：

```bash
python3 scripts/check_doc_citations.py --rev <当时的头> docs/Disaggregated/notes/<phase>/<file>.md
```
