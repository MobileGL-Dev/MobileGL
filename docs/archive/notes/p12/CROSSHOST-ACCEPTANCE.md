# P12 出口门 (b)：另一台机器经 TCP 在 Redmi 入世界

> 2026-09-28。client = WSL 里的 `mobilegl_trace_replay`（Windows 主机上另一套系统），server = Redmi `2f7cbe2e`（Adreno 830）的 trace APK server（`930c29e9`，split 构建），
> 控制面 TCP + 数据面 stream，DirectGLES，run-ahead 开。链路：手机 Wi-Fi（5 GHz 11ax，RSSI −36）↔ 主机以太网，经 `192.168.31.1`。
> 驱动 [`tools/device_bench/disagg/crosshost_accept.sh`](../../../../tools/device_bench/disagg/crosshost_accept.sh)，汇总 `crosshost_report.py`，原始数据 `.trace-work/perf-compare/crosshost/`。
> WSL 默认路由是主机 `xray_tun` 代理（会终结 TCP）；脚本加临时 /32 路由绕开它，并先跑 `tcp_path_check.sh`（用完删路由、停 server）。

## 结论

**门 (b) 通过。** 另一台机器上的 client 经 TCP 重放 Minecraft 世界内 trace（rd12），画面与设备上一致；P6.5 必测数已记录。
client 是 trace 重放器，不是活的 Minecraft 实例（那是门 (a) 的 FCL 那一半）。

| 检查 | 结果 |
|---|---|
| ssim（非 benchmark，重放到目标调用） | openra **1.000000**；rd12 **0.99988301**（与设备上四臂同值 0.999883） |
| rd12 benchmark，251/251 帧、rc=0 | 三次（PRESENT_CREDIT 1/2/3）全部跑完 |
| openra benchmark，128/128 帧、rc=0 | 三次全部跑完 |

## 必测数

| 项 | openra | rd12 |
|---|---|---|
| 稳态 fps（第 2 帧起的帧时中位数），credit 1 / 2 / 3 | 133.5 / 114.3 / 104.6 | 16.9 / 21.3 / 22.1 |
| 首帧（加载帧）耗时 | 1.2–1.6 s | 51–53 s |
| 加载帧 stage 字节 | 247 MB（整段） | 2,698 MB |
| 稳态 stage 字节/帧（均值） | 1.81 MB | 1.52 MB |
| 稳态实测吞吐 | 26–38 MB/s | 20.6–24.3 MB/s |
| `wait_replies`（整段） | 1 | 324–350 |
| 回包 RTT 均值 / p50 上界 / p99 上界 | 4.1–6.8 ms / 8.2 ms / 8.2 ms | 15.7–17.3 ms / 4.1–8.2 ms / 131 ms |
| client 线程 CPU（整段） | 1.2–1.3 s | 6.2–7.2 s |

- **加载帧受带宽约束，稳态不是。** server 侧实测：加载帧 2,696 MB 在 51 s 里读完（`P65ServerFrame`，**52.4 MB/s** 持续），稳态只有 20–24 MB/s，每帧平均 1.5 MB 的话带宽本可撑到 ~34 fps。
  稳态帧时 ~90% 是 `transport_wait`（client 发送本身每帧 <0.5 ms，`credit_wait_ms=0`），所以剩下的是往返延迟而不是 present 信用。
- **`PRESENT_CREDIT` 1 → 2：rd12 稳态 +26%（16.9 → 21.3）；2 → 3 只再 +4%。** openra 上 credit 越大反而略低，落在噪声内（单次）。
- Wi-Fi 往返 RTT（`ping`）3.3–61 ms（均值 19），4.6–110 ms（均值 30）两次测量；手机省电状态影响大，逐次差别很大。
- 第一次直连跑同一 rd12 是 235 s（加载帧 220 s），本轮 70 s：同样的 2.7 GB，**Wi-Fi 吞吐在不同时刻差 4 倍**。以上数字是这一时刻的，不是上限。
- `toybox nc` 汇点测得 18–22 MiB/s，**低估了链路**：同一时刻 server 自己读到 52 MB/s。`netbench.sh` 测的是 nc 汇点，不是 Wi-Fi 上限（见下）。

## 没做

- 门 (a)：FCL 同机 spawn、双后端入世界、杀 server 报 device-lost。
- 活的 Minecraft 在另一台机器上跑（这里是 trace 重放）。
- loopback tcp 对 spawn 的逐线程 CPU 差（五臂对比给了 fps，没给逐线程 CPU）。
- 每档只跑了一次，没有重复；credit 差别小于噪声的地方（openra）不下结论。
