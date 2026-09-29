# P10 — sync / query / present 节奏（已收官，2026-09-29）

> **2026-09-29 收官**。协议与行为：`MobileGL/MG_Remote/CONTRACT-P10.md`；裁定 [`INTEGRATOR-DECISIONS-P10.md`](INTEGRATOR-DECISIONS-P10.md)；计划与范围重划 [`PLAN-P10.md`](PLAN-P10.md)；测量 [`A-FENCE-POLL.md`](A-FENCE-POLL.md)、[`C-MEASUREMENTS.md`](C-MEASUREMENTS.md)。
>
> | 包 | 结果 |
> |---|---|
> | A fence 轮询 | server 报告完成的 fence（`kEventFenceSignaled`，含 present 时、归还 credit 之前），client 本地作答；第 `MOBILEGL_IPC_POLL_ESCALATE`（64）次未答的零超时轮询才往返；timed wait 语义不变。主机 Magma·tcp 每帧 −37%、fence 往返 598 → 0；主机 Espryt 无收益（llvmpipe 上只有阻塞等待才能看到 fence 完成）；Adreno 830 上两后端都是 0 次往返、598 / 598 次本地作答 |
> | B 覆盖与 class C | `SetSwapInterval` 槽改为 server 转发，class C 清零；`GL_KHR_parallel_shader_compile` 改由 client 决定；AsyncCompile / XfbPrimitiveQuery / PrimitivesGeneratedNoXfb 登记到 split 各臂（Espryt 27 条、Magma 64 条） |
> | C 测量 | 设备上 tcp loopback 的 `PRESENT_CREDIT` 1/2/3 在噪声内（rd12 51.4 / 51.6 / 52.4 fps），默认保持 1，下一个开销是传输停车（性能债）；主机 retrace 往返普查见 `C-MEASUREMENTS.md` §3 |
>
> 门（集成头）：见 ID-P10-12。
>
> 下面是该阶段在路线图上的原始范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）。

## 摘要

- 轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`；DirectGLES 的 fence 完成度改为逐 fence 退休；非 present fence tick；**`PRESENT_CREDIT > 1` 从未量过**（带延迟链路上的首要杠杆）；roundtrip 计数器与输入延迟直方图；`SetSwapInterval`（最后一个手写 class-C）。
- 门：query / XFB / `AsyncCompile` 场景在 split 下绿；draw / state / upload 路径 roundtrip 读零；credit 1..3 在 TCP loopback 与跨机臂上的配对记录。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P10** sync / query / present 节奏
- **状态**：待排
- **落地什么 / 范围**：~~client 铸造 query handle~~（已是：`QueryCreate` 为 `kWaitNone`）；轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`（0 处；TCP 上是首要杠杆之一）；DirectGLES 的 fence 完成度来自逐 fence 退休（`g_completedFrameSerial` 仍只在 `Present` 前进；Magma 那半已由 run-ahead 落地）；非 present fence tick（唯一记载处）；`present` 1:1（已是）；~~credit 默认 1~~（已是默认，`ConfigLoader.cpp:414`）；**`PRESENT_CREDIT > 1` 从未量过**（重审 §8.7）——带延迟链路上的首要杠杆，inproc 臂上即可取；roundtrip 计数器与输入延迟直方图（今天只有 `MapPersistentRoundtrips`）；`SetSwapInterval`（最后一个手写 class-C，`EmitTables.cpp:1665`）
- **验收门 / 证据**：query / XFB / `AsyncCompile` 场景在 split 下绿；trace 上 draw / state / upload 路径 roundtrip 读零；零 timeout 轮询有界退出；credit 1..3 在 TCP loopback 与跨机臂上的配对记录；配对 A/B 记录
