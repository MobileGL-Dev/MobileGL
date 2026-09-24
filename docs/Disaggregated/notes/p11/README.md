# P11 — persistent map 与 ≥16 MiB 采纳（待排，同机臂专属）

> 尚未开工。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- T0 / T1 只对共享映射成立：`dataPlane = Stream` 上点名 T0 / T1 是具名拒绝并强制 T2。POST 探针选档（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`gPipeInputs` 版本化 / 双缓冲。前置开放问题 3（`untrusted_app` 域复核）。
- 门：`LargeArenaAdoptionScenario` 在所选档下绿；Adreno 830 上 p99 帧时与峰值 RSS 对采纳基线（163 → 21 ms / 40 → 115 fps / ~400 MB）回归不超过 10%。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P11** persistent map 与 ≥16 MiB 采纳
- **状态**：待排，**同机臂专属、已让位**（2026-09-22）
- **落地什么 / 范围**：T0 / T1 只对共享映射成立：`dataPlane = Stream` 上点名 T0 / T1 是具名拒绝并强制 T2（重审 §6 第 5 项），今天 `MOBILEGL_IPC_ADOPT_TIER` 0/1 已是 Fatal-at-use、`kSegAdopt` 只有枚举；前置开放问题 3。POST 探针档位选择（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；**`gPipeInputs` 版本化 / 双缓冲**（P5d 研究：draw barrier 延后一个 verb 的前提，须在 P3b/P4b 的句柄键控 twin 表之后）。**P5e 的更正**：P5e 退役 draw lockstep **没有**需要它——ra2 实测证明字段**值**已由 `SuppliedFieldMask` 分区，当时那次竞态在**元数据标量**上，修法是让守卫测事实而非断言未来（ID-135）。本行保留采纳存储的一般版本/时序问题；契约 §3.2 的 per-role stamp 已由 P5f 落地，不再是待办
- **验收门 / 证据**：`LargeArenaAdoptionScenario` 在所选档下绿；26.3 与两个 Create fixture SSIM ≥ 0.99；Adreno 830 上 p99 帧时与峰值 RSS 对采纳基线（163→21 ms / 40→115 fps / ~400 MB）回归不超过 10%
