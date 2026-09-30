# 当前阶段：P8 进行中（2026-09-29 起）

> **更新 2026-09-29**。这一页只有摘要；计划与裁定在 [`notes/p8/PLAN-P8.md`](notes/p8/PLAN-P8.md)、[`notes/p8/INTEGRATOR-DECISIONS-P8.md`](notes/p8/INTEGRATOR-DECISIONS-P8.md)。上一阶段 P11 同日收官，见 [`notes/p11/README.md`](notes/p11/README.md)。

## 目标

补齐 server 侧仿真缺口（Espryt 生成 mip、Espryt 暂存影子与 GPU 写、Magma wire indirect、CopyImage），并把 monolith 用例登记到 split 各臂；为 P13 删掉旧的「后端直接读前端」路径扫清前置。原范围已重定界（划掉保留在 [`notes/p8/README.md`](notes/p8/README.md)）。

## 完成情况

| 项 | 状态 |
|---|---|
| 只读核查与重定界 | ✅ 原 10 项里 6 项已完成、直接关闭（ID-P8-1） |
| P8-0 普查（设备 + 主机） | 进行中 |
| A 覆盖对齐 | 进行中 |
| B Espryt 生成 mip | 进行中 |
| C Espryt 暂存影子与 GPU 写 | 进行中 |
| D Magma wire 原生 indirect | 进行中 |
| E CopyImage 在 server store 上 | 进行中 |
| F 死闩清理与重分类 | 进行中 |
| G 大 blob 分片 | 等 P8-0 的测量 |

## 下一步

第一波七包并行（ID-P8-4）；逐包集成进 `p8/main` 并跑整套门。

**待用户决策**：[`DEBTS.md`](notes/DEBTS.md) 六项「需裁定」（CI 触发条件、`TCP_USER_TIMEOUT`、HyperOS 下 FCL 保活、Mali 复测、TLS、`IDLE_EXIT_S`）。
