# P11 — persistent map 与 ≥16 MiB 采纳（进行中）

> **2026-09-29 开工**。计划与范围重划 [`PLAN-P11.md`](PLAN-P11.md)（B 已排期：消费者是同机从其他 app 启动的 GL 程序）；裁定 [`INTEGRATOR-DECISIONS-P11.md`](INTEGRATOR-DECISIONS-P11.md)；真机测量 [`A-DEVICE.md`](A-DEVICE.md)；协议 `MobileGL/MG_Remote/CONTRACT-P11.md`。已落地：合并 dev（两次）、A1 / A2、A3（T0 在 app 域两后端 GO）、A4（基线重测）、B0 / HSPIKE（跨 app 通道）、PAIR（连接按 nonce 配对，修订 4）、B1（同机外部 client 经 helper 与令牌 broker 走共享内存）。下一步：B2（T0，可开关）。
>
> 下面是该阶段在路线图上的原始范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）。

## 摘要

- T0 / T1 只对共享映射成立：`dataPlane = Stream` 上点名 T0 / T1 是具名拒绝并强制 T2。POST 探针选档（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`gPipeInputs` 版本化 / 双缓冲。前置开放问题 3（`untrusted_app` 域复核）。
- 门：`LargeArenaAdoptionScenario` 在所选档下绿；Adreno 830 上 p99 帧时与峰值 RSS 对采纳基线（163 → 21 ms / 40 → 115 fps / ~400 MB）回归不超过 10%。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P11** persistent map 与 ≥16 MiB 采纳
- **状态**：待排，**同机臂专属、已让位**（2026-09-22）
- **落地什么 / 范围**：T0 / T1 只对共享映射成立：`dataPlane = Stream` 上点名 T0 / T1 是具名拒绝并强制 T2（重审 §6 第 5 项），今天 `MOBILEGL_IPC_ADOPT_TIER` 0/1 已是 Fatal-at-use（**B2 起已过时**：T0 已实现，2026-09-29 起旋钮未设即 T0、回退安静，`=0` 点名、`=2` 为 T2，见 `MG_Remote/CONTRACT-P11.md` B2）、`kSegAdopt` 只有枚举；前置开放问题 3。POST 探针档位选择（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；**`gPipeInputs` 版本化 / 双缓冲**（P5d 研究：draw barrier 延后一个 verb 的前提，须在 P3b/P4b 的句柄键控 twin 表之后）。**P5e 的更正**：P5e 退役 draw lockstep **没有**需要它——ra2 实测证明字段**值**已由 `SuppliedFieldMask` 分区，当时那次竞态在**元数据标量**上，修法是让守卫测事实而非断言未来（ID-135）。本行保留采纳存储的一般版本/时序问题；契约 §3.2 的 per-role stamp 已由 P5f 落地，不再是待办
- **验收门 / 证据**：`LargeArenaAdoptionScenario` 在所选档下绿；26.3 与两个 Create fixture SSIM ≥ 0.99；Adreno 830 上 p99 帧时与峰值 RSS 对采纳基线（163→21 ms / 40→115 fps / ~400 MB）回归不超过 10%
