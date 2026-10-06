# P13 — 退役 pull 路径（进行中）

> 计划 [`PLAN-P13.md`](PLAN-P13.md)，裁定 [`INTEGRATOR-DECISIONS-P13.md`](INTEGRATOR-DECISIONS-P13.md)，CI 清理记录 [`docs/ci/ci-cleanup-log.md`](../../../ci/ci-cleanup-log.md)。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 进度

| 波 | 状态 | 提交 | 备注 |
|---|---|---|---|
| CI 基线 | 完成 | `cde6512b` `0cbb1a84` `c9085533` | 托管 Test `37414803850`、APK `37405703442` 的红全部归类修复：1.17 名字表（apitrace 分支 `45843686`）、CTest include 的 `IN_LIST`、环回 TCP 端口、spawn Welcome 冷启动预算 |
| W1 | 完成 | `5ff33cbb` | `runtime_mode_proof` 的 monolith = 无 MG_Remote；push 无 MG_Remote 控制库 + 零 MG_Remote 符号检查；retrace-split 不再因控制库失败整体跳过；skip 普查 |
| W2 | 本地 | `3c42fef8` `d0019120` | CMake 默认 push；Gradle 删 `pipePush`（FCL 内嵌 push monolith、无 MG_Remote）；撤 pull 控制库 |

### G1 退役读数（ID-P13-4，2026-10-06）

pull 构建（`DISAGGREGATED=OFF`、`PIPE_PUSH=OFF`，只编 `MobileGL` target），同机同编译器（WSL archlinux，clang 22.1.6）：

| | P12 收官 `743f4e8e` | W1 `5ff33cbb` |
|---|---|---|
| `.text` | `0xa52203` | `0xaa6ba3` |
| 定义符号（`nm --defined-only` 名字） | 30570 | 31578（增 1300、减 292） |

基准一侧的 `.text` 与 P12 门记录的 `0xa52203` 相同。变化来自 P12 收官之后的 346 个提交（GL 层修复、KWin 用的 ARB 字符串等，两臂共用的代码；增量按命名空间：`MG_Backend` 290、`MG_Impl` 140、`MG_State` 92），G1 在 P12 之后就不再是任何提交的门。本读数之后 G1 退役，接班：零 MG_Remote 符号检查（W1）、skip 普查（W1）、recorder 金标（W8）。

## 摘要

- 删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`、`set_residual_value_state`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；重调幸存缓存容量；建立模块 target 与边界（D12）。P7 留下的 86 个链接棘轮符号标 `# P13`。
- 门：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P13** 退役 pull 路径
- **状态**：待排
- **落地什么 / 范围**：删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；删 `set_residual_value_state`；在计数器活着的情况下重调幸存缓存容量；建立模块 target 与边界（D12：`SOURCE_FILES` 是一张平表喂两个库 target，无模块可链）
- **验收门 / 证据**：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线
