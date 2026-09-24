# P13 — 退役 pull 路径（待排）

> 尚未开工。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- 删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`、`set_residual_value_state`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；重调幸存缓存容量；建立模块 target 与边界（D12）。P7 留下的 86 个链接棘轮符号标 `# P13`。
- 门：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P13** 退役 pull 路径
- **状态**：待排
- **落地什么 / 范围**：删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；删 `set_residual_value_state`；在计数器活着的情况下重调幸存缓存容量；建立模块 target 与边界（D12：`SOURCE_FILES` 是一张平表喂两个库 target，无模块可链）
- **验收门 / 证据**：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线
