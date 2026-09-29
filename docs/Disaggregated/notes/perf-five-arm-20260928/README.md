# 五臂传输性能对比（monolith / inproc / spawn+shm / spawn+tcp localhost / tcp over Wi-Fi）

> 2026-09-28，设备 Redmi `2f7cbe2e`（Adreno 830，Android 16，aarch64，**无 root 未定频**——
> 以 A-B-C-D 交错 3 轮替代定频，臂内散布 ≤2.7%）。
> 驱动与 profiling 工具已入库 [`tools/device_bench/disagg/`](../../../tools/device_bench/disagg/README.md)；
> 原始数据在 `.trace-work/perf-compare/`（`summary.json` / `REPORT.md` / 逐轮目录 / `profile/` / `wifi/`）。
> 每臂均验证 logcat 臂标志（transport marker、`spawn ARMED`、`run-ahead ARMED`），全部 0 lockstep、0 Fatal；
> 每负载另跑一轮非 benchmark 的 ssim 门（openra 四臂 1.000000，rd12 四臂 0.999883，字节级一致）。

## 结果

### rd12（Minecraft 重负载，251 帧；~7,500 条 wire 记录/帧、816 KB/帧 stage 流量）

| 臂 | fps（best-of-3） | mean ms | median ms | 备注 |
|---|---|---|---|---|
| monolith | 131.3 | 7.61 | 7.34 | 基线 |
| inproc | 176.2 | 5.68 | 5.32 | **比 monolith 快 34%** |
| spawn+shm | 178.3 | 5.61 | 5.26 | **快 36%**；与 inproc 打平（历史门 8 同结论） |
| spawn+tcp localhost | 21.0–23.1 | 43.3–48.3 | 45–49 | **比 shm 慢 ~8 倍** |
| spawn+tcp localhost + `SPIN_US=2000` | 63.6 | 15.7 | — | 单 knob A/B，**2.9×** |
| tcp over Wi-Fi | — | — | — | 跨主机控制面 hang，0 有效帧（见下） |

### openra（轻负载，128 帧；28 draws/帧、199 记录/帧）

| 臂 | fps | mean ms | 备注 |
|---|---|---|---|
| monolith | 101.3 | 9.87 | mean 含首帧 setup 尖峰；median 0.76ms |
| inproc | 99.8 | 10.02 | 与 monolith 差 ~1.5%，噪声级 |
| spawn+shm | 88.8 | 11.27 | −12%，纯跨进程固定成本 |
| spawn+tcp localhost | 76.9 | 13.00 | −24% |
| spawn+tcp 同机 wlan0 IP（Wi-Fi 替代拓扑，不过空口） | 74.2–76.3 | 13.1–13.5 | ≈ tcp localhost，符合预期 |

## 结论

1. **inproc / shm 不慢，重负载下反而比 monolith 快 ~1/3。** 机制有二：
   run-ahead 把 server apply（3.5ms/帧）挪到另一核与 client 编码（5.7ms/帧）并行，帧时取 max 而非求和
   （实测 monolith 7.87 vs inproc 5.80 ms/帧）；且 applier 逐记录路径的状态探针更少
   （accessor/draw：monolith 6.08 vs split 4.05，每帧少 ~3,000 次探针）。
   轻负载（openra）下没有可重叠的工作量，split 只付固定握手成本：inproc ≈ monolith，shm −12%，tcp −24%。
2. **真正差的是 tcp 臂，瓶颈是"credit=1 串行 ping-pong + futex 唤醒税"，不是带宽、不是 syscall、不是 RTT。**
   rd12 稳态 transport_wait ≈ 20.5ms/帧的分解：

   | 分项 | 占比 | 证据 |
   |---|---|---|
   | present credit=1 等待（等上一帧 swap ack） | **~74%** | ftrace：GL 线程长 park 260 次 × 均值 27.5ms |
   | sendmsg 内联发送（GL 线程持锁，11 次/帧） | ~18% | simpleperf `__sendmsg` 占 client 用户态 7.6% |
   | reply RTT | 稳态 0% | wait_replies 全部集中在第 1 帧（create window） |
   | progress 64 记录/1ms 合批节流 | ≤5% | SPIN 对照 A/B 反证 |

   佐证：需求带宽 148MB/s vs 实测突发 recv 293MB/s、send 占空比 3.5%；
   `MOBILEGL_IPC_SPIN_US=2000` 只消唤醒税就把 fps 21.7→63.6（GL park 合计 8.99→4.97s，futex 阻塞 4.37→1.74s）。
   server 侧 io 线程每帧 41ms 阻塞在 recv 是"对端还没发"（credit 串行化）的症状，不是网络问题。
3. **shm/inproc 已贴近它们的实际上限：apply 线程 CPU。** 帧时 5.6ms 中 3.5ms 是真实 apply 工作；
   剩下可削的是握手开销——apply 线程 6,963 次/帧 doorbell 等待、自旋占其 CPU ~35%（1.8ms/帧），
   离跨核 cache-line ping-pong 上限（估 2–5M 记录/s，实际 1.24M waits/s）还有 2–4 倍余量；park 率仅 0.05%。
4. **Wi-Fi 臂被两个独立问题卡住。**
   - **跨主机控制面 hang（源码 bug，P12 验收 B 的直接阻塞项）**：WSL client → 手机 server 在
     `eglMakeCurrent`（控制面 seq 3）100% 复现挂死。字节级证据链：client 字节已被手机内核 ACK、
     `/proc/net/tcp` 接收队列已空，但 session child 的控制 fd `ppoll` 永远无 POLLIN（strace 现场：
     主线程 ppoll 100ms 超时循环、io 线程 recv 阻塞、apply 线程 futex parked）。
     同机 TCP（连本机 wlan0 IP）完全正常 → 特异于跨主机到达时序，疑为控制面帧重组/双 reader 竞态
     （本地 loopback 时窗口太小不出现）。已排除：丢包/PMTU/冷启动预算/LOG_FORWARD/run-ahead/token。
     复现与取证脚本：`tools/device_bench/disagg/wifi_hang_repro.sh`、`phone_strace.sh`、`fdprobe.sh`。
   - **带宽硬约束（即使修好 hang）**：实测 Wi-Fi TCP 吞吐 19–22 MiB/s（链路 5GHz 11ax、
     Tx 648–1441Mbps、RTT avg 0.78ms），rd12 满速需 ~49MB/s → **Wi-Fi 臂上限 ~40–45 fps**；
     轻负载则转为 RTT × 同步点主导。要满速必须减流量（压缩/去重）或提 credit 深度摊 RTT。

## 优化建议（按优先级）

knob 级（零代码）：

1. `MOBILEGL_IPC_SPIN_US=2000`（tcp/spawn 臂）——本机已实测 **2.9×**（21.7→63.6 fps），与 P12 结论一致。
2. `MOBILEGL_IPC_PRESENT_CREDIT=2`——投影砍掉 twait 主项（74%）的一半以上；代价 +1 帧延迟（P5e 裁定允许）。**未实测，待 A/B。**
3. shm 臂 server 侧 `SPIN_US` 降到 20–35——park 率 0.05% 说明 50µs 自旋预算几乎全在白烧（35% server CPU）。**投影，待 A/B。**
4. `MOBILEGL_IPC_SERVER_AFFINITY`——ftrace 显示 tcp 臂 apply 线程落在 1.02GHz 小核（cpu6/7），apply 8.8 vs 4.0ms/帧。待 A/B。

代码级：

5. client 侧 doorbell 从 condvar 换 futex/eventfd 直唤醒（`StreamLink.cpp` 的 `StreamBell` 现包 `CondVarDoorbell`；
   唤醒链 p50=128µs × 9,134 次/轮）。
6. applier 的 WaitForWork 改自适应 spin（连续空转才降预算），与 #3 同向但更精细。
7. sendmsg 跨 Flush 合批（收益上限 ~18% twait，排在后面）。
8. appliedSeq/progress 合批参数**不动**（A/B 反证 ≤5%）。
9. 修跨主机控制面 hang（方向：RunSession 控制泵 / ControlInbox 的读状态机与双 reader 交错；
   证据见 `.trace-work/perf-compare/wifi/`）。

## 方法与读法陷阱（复测必读）

- 未定频设备的替代口径：A-B-C-D 交错 ≥3 轮，看 best-of-N 与散布，不看单轮。
- benchmark 模式不落 PNG、ssim 恒 −1；正确性门要单独跑一轮非 benchmark。
- spawn 臂 client 侧 PipeStats `frames=0` 是结构性零；费率用 server 侧 dump 的 frames 做分母（merge 逻辑见 `make_report.py`）。
- 无 root 设备的 profiling 组合：simpleperf（`perf_event_paranoid=-1` 时 shell 可用）+ ftrace sched
  （shell 可写；本机 ring 丢弃 ~74% 事件，看分位数不看绝对计数）+ 自研 `/proc/<tid>/syscall` 采样器
  （`tools/device_bench/disagg/sampler.c`，strace attach 被 SELinux 拒时的替代）。
- 已知缺口：rd12 monolith/inproc 的客户端符号级 simpleperf 报告缺失（采样收尾 bug）；
  loopback 裸吞吐未测成（以突发 293MB/s + 占空比论证代替）；rd12 的 Wi-Fi 替代拓扑数据缺失（编排问题）。
