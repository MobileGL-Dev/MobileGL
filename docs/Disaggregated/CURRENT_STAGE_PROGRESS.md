# 当前阶段：P13 已收官（2026-10-07）；下一阶段 P14

> **更新 2026-10-07**。这一页只有摘要；计划与裁定在 [`notes/p13/PLAN-P13.md`](notes/p13/PLAN-P13.md)、[`notes/p13/INTEGRATOR-DECISIONS-P13.md`](notes/p13/INTEGRATOR-DECISIONS-P13.md)，逐波记录在 [`notes/p13/README.md`](notes/p13/README.md)。上一阶段 P8 于 09-30 收官，见 [`notes/p8/README.md`](notes/p8/README.md)。

## 目标

退役 pull 路径：单进程（FCL 内嵌库）也走 P8 修好的记录臂，删掉旧的「后端直接读前端」路径与它的全部编译期 / 运行期开关。

## 完成情况

| 项 | 状态 |
|---|---|
| W1 CI 地基（skip 普查、零 `MG_Remote` 符号检查、运行期形态证明） | ✅ |
| W2–W3 默认翻转与去宏（`MOBILEGL_PIPE_PUSH` / `LEGACY_MEMOS` 删除，位 0–13 固定开） | ✅ |
| W4–W5 单进程换到记录臂，记录臂移出 `MG_Remote` | ✅ |
| W6 删后端的前端对象臂（Espryt / Magma），`# P13` 债务清零 | ✅ |
| W7 七个 OBJECT 模块 + server-only 链接检查 | ✅ |
| W8 recorder 金标（首跑抓到并修掉两个单进程推送内容缺陷） | ✅ |
| W9 缓存容量重调 | 移到 P13 之后的性能轮（用户 10-07） |
| W10 文档与收官 | ✅；FCL 真机两后端冒烟通过 |

## 下一步

**P14**：状态归属（同进程多 session / 多 context / share group），设计见 [`design/11-state-ownership.md`](design/11-state-ownership.md)。性能轮（基线 `dev` 构建，定频，FCL MC + anland glmark2，两后端）也排在 P13 之后。

**待用户决策**：[`DEBTS.md`](notes/DEBTS.md) 中「需裁定」各项；FCL 无声死亡留给用户（`notes/p13/fcl-soak/README.md`）。
