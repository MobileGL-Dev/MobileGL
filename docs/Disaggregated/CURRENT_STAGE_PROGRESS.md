# 当前阶段：P12 已收官（2026-09-29）；下一阶段 P9 / P8 待排

> **更新 2026-09-28**（含五臂传输性能对比，见 [`notes/perf-five-arm-20260928`](notes/perf-five-arm-20260928/README.md)）。这一页只有摘要；带裁定编号的明细、余项清单、证据位置在 [`notes/p12/README.md`](notes/p12/README.md)（计划与交接 [`PLAN-P12.md`](notes/p12/PLAN-P12.md)）。P12 审查后的定向主机与真机复测已完成；验收 A、B 都已通过（B 于 2026-09-28，A 于 2026-09-29）。上一阶段 P7 已于 2026-09-23 完成。

## 目标

Android 上的 server 应用自己开一个全屏窗口，把收到的渲染流直接画到屏幕上；client 不需要有窗口（完全无头）。窗口没了（比如按 HOME）时，client 干净地得到"设备丢失"，server 继续等下一个连接。

## 完成情况

| 项 | 状态 |
|---|---|
| 功能：server 自有窗口、无头 client、窗口丢失处理、一台设备同时只一个 server | ✅ 已并入主分支 |
| 代码审查发现的 10 个问题 | ✅ 已修 |
| 主机门与单进程构建二进制不变（G1） | ✅ 原主机门 / G1 见 ID-P12-14/15；审查后 P12 定向 CTest **46/46**（`1fb18d9e`） |
| 真机 7 项检查（两个后端上屏、排队与拒绝、窗口丢失、离屏不受影响……） | ✅ 审查后在 Redmi `2f7cbe2e` 复测通过（`1fb18d9e`）；明细见 [`notes/p12/README.md`](notes/p12/README.md) |
| 验收 A：同一台手机上用 FCL 以两个后端进游戏，杀掉 server 后干净报"设备丢失" | ✅ 2026-09-29：Render Server 屏 + FCL（新增版本设置「游戏退到后台时不暂停」），双后端入世界，杀 server 后 33–54 ms 闩住 device-lost，FCL 不崩；[`notes/p12/FCL-ACCEPTANCE.md`](notes/p12/FCL-ACCEPTANCE.md)。竖屏 server 窗口里画面被裁的问题已查明并修复（client 请求尺寸为 0×0，现在取 native window 的大小） |
| 验收 B：另一台电脑经 TCP 连手机进游戏，并记录链路数据 | ✅ 2026-09-28：WSL client 经 Wi-Fi 重放 rd12 世界内 trace，ssim 0.999883、251/251 帧；必测数见 [`notes/p12/CROSSHOST-ACCEPTANCE.md`](notes/p12/CROSSHOST-ACCEPTANCE.md)（client 是 trace 重放器，不是活的 Minecraft） |

## 性能现状（2026-09-28，Redmi 未定频交错 A/B）

| 臂 | rd12 重负载 | openra 轻负载 |
|---|---|---|
| monolith | 131 fps | 101 fps |
| inproc | 176 fps（+34%） | 100 fps |
| spawn+shm | 178 fps（+36%） | 89 fps（−12%） |
| spawn+tcp localhost | 21–23 fps | 77 fps（−24%） |
| tcp localhost + `SPIN_US=2000` | 63.6 fps（2.9×） | — |
| tcp over Wi-Fi（WSL → 手机） | 稳态 17–22 fps（加载帧 52 s） | 稳态 105–133 fps |

- inproc / shm 重负载比 monolith 快（run-ahead 并行 + 探针更少）；轻负载只付固定握手成本。
- tcp 慢的主因：present credit=1 串行等待 ~74% + futex 唤醒税；不是带宽。
- 原先记录的「跨主机 TCP 在 `eglMakeCurrent` 挂死」**不是源码 bug**：WSL 流量被主机 v2rayN 的 `xray_tun` 终结后中继卡住。绕开后 rd12 / openra 都跑通；跨机测试前先跑 `tools/device_bench/disagg/tcp_path_check.sh`。
- Wi-Fi 上加载帧（2.7 GB）受带宽约束（server 实测 52 MB/s 持续），稳态不受（20–24 MB/s，受往返延迟）；`PRESENT_CREDIT` 1→2 稳态 +26%，2→3 +4%。此前的「带宽上限 18–26 fps」结论已撤回（`toybox nc` 汇点低估了链路）。

## 收官（2026-09-29）

- 出口门 A、B 都过；契约 [`MG_Remote/CONTRACT-P12.md`](../../MobileGL/MG_Remote/CONTRACT-P12.md) 已写；`gate.sh` 的 G1 只在同机基线上比较；G1 在 Arch 上成立（pull 构建 `.text` 与基线同为 `0xa52203`，符号增 0 减 0）。
- 没做、转入 [`notes/DEBTS.md`](notes/DEBTS.md)：DirectGLES 去全局、freezer、多 context、D8 窗口种类白名单、TLS。
- 收官审查：用户裁定不派 agent / Codex；契约断言由本人对照代码核过。
- FCL fork 的「游戏退到后台时不暂停」在 FCL 仓库，已提交并推送（`Swung0x48/FoldCraftLauncher` 的 `merge-upstream-surfaceview`，`241471515`）。

## 下一步

1. 零代码 knob A/B：`PRESENT_CREDIT=2`、shm 臂 server `SPIN_US` 降到 20–35、`SERVER_AFFINITY`；代码级：client doorbell 换 futex/eventfd、applier 自适应 spin、sendmsg 跨 Flush 合批。
2. 之后按 [`ROADMAP.md`](ROADMAP.md)：P9 → P10 → P11（IPC 跑道）；P8（monolith 跑道）；P6.5 残余（39 例 device 矩阵）与 P3b/P4b 余项并行。

**阻塞**：没有需要决策的事项。
