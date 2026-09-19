# 工作笔记（原 `~/w7/notes/`）

这一层是 MGPipe 各阶段的**工作笔记原件**：包计划（`BRIEF-*.md`）、契约草稿、集成者逐条裁定
（`INTEGRATOR-DECISIONS-*.md`）、只读审计、普查、设备记录、包任务书（`TASK-*.md`）与阶段报告。

它们此前只存在于一台机器的 `~/w7/notes/` 下。上层那几份文档（`ROADMAP.md`、
`CURRENT_STAGE_PROGRESS.md`、`MEASUREMENTS.md` …）**大量引用它们**——裁定编号、审计行号、
逐门数字的出处几乎全在这里。一份引用了几百次、却只存在于本地未跟踪目录的记录，等于没有记录。
所以整体移进仓库。

## 路径映射

```
~/w7/notes/<phase>/<file>.md   →   docs/Disaggregated/notes/<phase>/<file>.md
```

上层文档里凡写作 `~/w7/notes/...` 的 `.md` 引用，都按这条规则读。

## 移进来的是什么，没移的是什么

| | |
|---|---|
| **移进来了** | 全部 `.md`，415 份，约 11 MB，目录结构原样保留 |
| **没有移** | 脚本（`.sh` / `.py`）、运行日志（`.log` / `.out` / `.err`）、普查与门的结果数据（`.json` / `.txt` / `.tsv`）、补丁、APK 与 `.idsig`、`nm` 转储 |

没移的那些仍在 `~/w7/notes/` 与各构建树下。它们是**证据数据**而不是文档：体积大、随每次运行
重新产生、且多数只在产生它的那台机器上有意义（绝对路径、设备序列号、构建目录）。上层文档引用
它们时给的是路径，不是内容。

## 两个需要知道的例外

1. **P6 的包计划已提升为正式文档**：原 `~/w7/notes/p6/BRIEF-P6.md` 现在是
   [`../P6-SPAWN-PLAN.md`](../P6-SPAWN-PLAN.md)，本目录不再保留副本，避免两份同源文档各自漂移。
   契约草稿同理，见 [`../P6-CONTRACT-DRAFT.md`](../P6-CONTRACT-DRAFT.md)。
2. **`recovered/`** 是从失败的 workflow 运行里捞回来的材料（`wf1`…`wf5`），与同名阶段目录
   **可能重复**，保留是因为其中几份是当时唯一的完整版本。查阶段记录请先看阶段目录。

## 不受 citation lint 管

CI 的文档引用 lint 只吃 `docs/Disaggregated/*.md`（非递归，`.github/workflows/test.yml`），
**本目录不在其中，这是有意的**：这些笔记的 `file:line` 是写在各自当时的代码头上的，拿今天的
HEAD 去解析必然大面积失败，而那不是错误——那是它们作为历史记录的正确状态。要核对某份笔记的
引用，用它自己声明的那个 revision：

```bash
python3 scripts/check_doc_citations.py --rev <当时的头> docs/Disaggregated/notes/<phase>/<file>.md
```
