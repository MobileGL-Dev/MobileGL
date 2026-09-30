# 当前阶段：P8 已收官（2026-09-30）；下一阶段 P13

> **更新 2026-09-30**。这一页只有摘要；计划与裁定在 [`notes/p8/PLAN-P8.md`](notes/p8/PLAN-P8.md)、[`notes/p8/INTEGRATOR-DECISIONS-P8.md`](notes/p8/INTEGRATOR-DECISIONS-P8.md)。上一阶段 P11 于 09-29 收官，见 [`notes/p11/README.md`](notes/p11/README.md)。

## 目标

补齐 server 侧仿真缺口（Espryt 生成 mip、Espryt 暂存影子与 GPU 写、Magma wire indirect、驱动拒读），并把 monolith 用例登记到 split 各臂；为 P13 删掉旧的「后端直接读前端」路径扫清前置。原范围已重定界（划掉保留在 [`notes/p8/README.md`](notes/p8/README.md)）。

## 完成情况

| 项 | 状态 |
|---|---|
| 只读核查与重定界 | ✅ 原 10 项里 6 项已完成、直接关闭（ID-P8-1） |
| P8-0 普查（设备 + 主机） | ✅ 真实内容只 Magma wire indirect 命中；G 不做；create-indirect 门在多帧 trace 上成立（ID-P8-5） |
| A 覆盖对齐 | ✅ 246 例登上 split / spawn / tcp，`split_coverage.py` 进门（ID-P8-7） |
| B Espryt 生成 mip | ✅（ID-P8-8） |
| C Espryt 暂存影子与 GPU 写 | ✅（ID-P8-9） |
| D Magma wire 原生 indirect | ✅ create-indirect 每次整 GPU 等待 321 → 0（ID-P8-10） |
| E 驱动拒读 / CopyImage | ✅（ID-P8-11） |
| F 死闩清理与重分类 | ✅（ID-P8-6） |
| G 大 blob 分片 | 不做（ID-P8-5） |
| 第一波集成头 `39cd8fb7` | ✅ 整套门全绿，G1 不变（ID-P8-12） |
| 第二波 SE / SV（split 余项） | ✅（ID-P8-15、-16） |
| 第二波 MD + MF（Create 在 Adreno 上） | ✅ dev `f973008c` + feat 镜像：create-instancing × Espryt × monolith 0.870 → 0.99998；create-indirect 坏在 fixture（ID-P8-14） |
| 出口 | ✅ 候选头 `49430538` 整套门全绿；设备矩阵无一格变差；P12 跨机门复跑一致（ID-P8-18、-19） |

## 下一步

**P13**：删掉旧的「后端直接读前端」路径，monolith 换到 P8 已修好的记录臂（P8 留下的 9 条 monolith 缺陷随之消失，`DEBTS.md`）。开工前先做只读核查。

**待用户决策**：[`DEBTS.md`](notes/DEBTS.md) 六项「需裁定」（CI 触发条件、`TCP_USER_TIMEOUT`、HyperOS 下 FCL 保活、Mali 复测、TLS、`IDLE_EXIT_S`）。
