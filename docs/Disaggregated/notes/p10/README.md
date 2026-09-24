# P10 — sync / query / present 节奏（待排）

> 尚未开工。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- 轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`；DirectGLES 的 fence 完成度改为逐 fence 退休；非 present fence tick；**`PRESENT_CREDIT > 1` 从未量过**（带延迟链路上的首要杠杆）；roundtrip 计数器与输入延迟直方图；`SetSwapInterval`（最后一个手写 class-C）。
- 门：query / XFB / `AsyncCompile` 场景在 split 下绿；draw / state / upload 路径 roundtrip 读零；credit 1..3 在 TCP loopback 与跨机臂上的配对记录。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P10** sync / query / present 节奏
- **状态**：待排
- **落地什么 / 范围**：~~client 铸造 query handle~~（已是：`QueryCreate` 为 `kWaitNone`）；轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`（0 处；TCP 上是首要杠杆之一）；DirectGLES 的 fence 完成度来自逐 fence 退休（`g_completedFrameSerial` 仍只在 `Present` 前进；Magma 那半已由 run-ahead 落地）；非 present fence tick（唯一记载处）；`present` 1:1（已是）；~~credit 默认 1~~（已是默认，`ConfigLoader.cpp:414`）；**`PRESENT_CREDIT > 1` 从未量过**（重审 §8.7）——带延迟链路上的首要杠杆，inproc 臂上即可取；roundtrip 计数器与输入延迟直方图（今天只有 `MapPersistentRoundtrips`）；`SetSwapInterval`（最后一个手写 class-C，`EmitTables.cpp:1665`）
- **验收门 / 证据**：query / XFB / `AsyncCompile` 场景在 split 下绿；trace 上 draw / state / upload 路径 roundtrip 读零；零 timeout 轮询有界退出；credit 1..3 在 TCP loopback 与跨机臂上的配对记录；配对 A/B 记录
