# P0.5 — 值头与制品头抽取（`5d99ee43`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：`MG_Pipe/MGPipeValueTypes.h`（不 include `MG_State/GLState`）、`ProgramArtifacts.h`、`Visit()` 归档 + `sizeof` 绊线、CI `-H` include 闭包断言（`scripts/check_include_closure.py`）与 `scripts/symbol_report.py`。
- 门：测试名零删除、`.text` 不变、符号 0 增 / 0 删 / 42 重命名。没有这一步，server 不链 glslang 的判据不可达。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P0.5** 值头与制品头抽取
- **状态**：✅ `5d99ee43`
- **落地什么 / 范围**：`MG_Pipe/MGPipeValueTypes.h`；`ProgramArtifacts.h`；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言（`scripts/check_include_closure.py`）+ `scripts/symbol_report.py`
- **验收门 / 证据**：测试名零删除、`.text` 不变、符号 0 增 / 0 删 / 42 重命名

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P05.md`](BRIEF-P05.md) | P0.5 implementation brief — value header + artifacts header + include-closure gate |
| [`p05-results/`](p05-results/) | 6 个文件：三个包（value-types / program-artifacts / include-gate）的实现与评审稿 |

捞回的独有材料：[`../recovered/wf3/`](../recovered/wf3/)（四份 scout）。
