# 当前阶段：P12 — server 自己开窗口上屏

> **更新 2026-09-28**（含五臂传输性能对比，见 [`notes/perf-five-arm-20260928`](notes/perf-five-arm-20260928/README.md)）。这一页只有摘要；带裁定编号的明细、余项清单、证据位置在 [`notes/p12/README.md`](notes/p12/README.md)（计划与交接 [`PLAN-P12.md`](notes/p12/PLAN-P12.md)）。P12 审查后的定向主机与真机复测已完成；两个收官验收门仍未打。上一阶段 P7 已于 2026-09-23 完成。

## 目标

Android 上的 server 应用自己开一个全屏窗口，把收到的渲染流直接画到屏幕上；client 不需要有窗口（完全无头）。窗口没了（比如按 HOME）时，client 干净地得到"设备丢失"，server 继续等下一个连接。

## 完成情况

| 项 | 状态 |
|---|---|
| 功能：server 自有窗口、无头 client、窗口丢失处理、一台设备同时只一个 server | ✅ 已并入主分支 |
| 代码审查发现的 10 个问题 | ✅ 已修 |
| 主机门与单进程构建二进制不变（G1） | ✅ 原主机门 / G1 见 ID-P12-14/15；审查后 P12 定向 CTest **46/46**（`1fb18d9e`） |
| 真机 7 项检查（两个后端上屏、排队与拒绝、窗口丢失、离屏不受影响……） | ✅ 审查后在 Redmi `2f7cbe2e` 复测通过（`1fb18d9e`）；明细见 [`notes/p12/README.md`](notes/p12/README.md) |
| 验收 A：同一台手机上用 FCL 以两个后端进游戏，杀掉 server 后干净报"设备丢失" | ❌ 未做 |
| 验收 B：另一台电脑经 TCP 连手机进游戏，并记录链路数据 | ❌ 未做 |

## 性能现状（2026-09-28，Redmi 未定频交错 A/B）

| 臂 | rd12 重负载 | openra 轻负载 |
|---|---|---|
| monolith | 131 fps | 101 fps |
| inproc | 176 fps（+34%） | 100 fps |
| spawn+shm | 178 fps（+36%） | 89 fps（−12%） |
| spawn+tcp localhost | 21–23 fps | 77 fps（−24%） |
| tcp localhost + `SPIN_US=2000` | 63.6 fps（2.9×） | — |
| tcp over Wi-Fi（WSL → 手机） | 17.4 fps（带宽约束） | 4.2 fps（RTT 约束） |

- inproc / shm 重负载比 monolith 快（run-ahead 并行 + 探针更少）；轻负载只付固定握手成本。
- tcp 慢的主因：present credit=1 串行等待 ~74% + futex 唤醒税；不是带宽。
- 原先记录的「跨主机 TCP 在 `eglMakeCurrent` 挂死」**不是源码 bug**：WSL 流量被主机 v2rayN 的 `xray_tun` 终结后中继卡住。绕开后 rd12 / openra 都跑通；跨机测试前先跑 `tools/device_bench/disagg/tcp_path_check.sh`。
- 直连 Wi-Fi 吞吐 14–20 MiB/s，所以 rd12 经 Wi-Fi 的上限 ~18–26 fps。

## 下一步

1. 打验收 B（另一台机器经 TCP 入世界；先过 `tcp_path_check.sh`）与验收 A（FCL 同机 spawn、双后端、杀 server 报 device-lost）。
2. 收尾：写 `MG_Remote/CONTRACT-P12.md`、修门脚本误导的一行输出、收官审查。
3. 零代码 knob A/B：`PRESENT_CREDIT=2`、shm 臂 server `SPIN_US` 降到 20–35、`SERVER_AFFINITY`；代码级：client doorbell 换 futex/eventfd、applier 自适应 spin、sendmsg 跨 Flush 合批。
4. 之后按 [`ROADMAP.md`](ROADMAP.md)：P8；P9 → P10 → P11。

**阻塞**：没有需要决策的事项。
