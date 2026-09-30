# 实测（索引）

> 这一页只有结论和去向；完整的表、命令、协议与读法在各阶段 `notes/<阶段>/README.md` 的"实测"节。代码和旧笔记里的 `MEASUREMENTS.md §N` 按下表"§"列找到对应阶段（小节编号在那里原样保留）。

**口径**：性能只记录、不设门；看**逐线程 CPU 时间**，不看整机帧率。性能对比只用一台手机（Redmi `2f7cbe2e`，Adreno 830），定频、同热窗口、两臂交替跑（[`guide/pin-verification-2026-09-07.md`](guide/pin-verification-2026-09-07.md)）。

## 关键结论

- **接口本身的代价**：单进程里改成"前端推、后端收"，CPU 多约 6–12%（P2），已接受；顶点数组切换密集的场景更高（P3a，+27–30%）。
- **拆成两线程**：起初游戏里只有 7–13 fps；优化后 103–106 fps（P5d）；client 发完就走以后，重负载（渲染距离 32）下与单线程持平（P5e）。
- **拆成两进程**：真机上与两线程成本持平；两者的 GL 线程 CPU 都比单线程少约 30%（后端工作挪到了另一线程 / 进程）（P6）。
- **跨机**：电脑经 TCP 连手机，102 个集成用例全过；server 被杀后 133 ms、Wi-Fi 断开后约 5 s 干净报"设备丢失"（P6.5）。
- **纹理上传的等待粒度（P12，handoff §4）**：wire 臂上纹理那半 `resource_subdata` 不再逐条买回包，等待点从「每条记录」落到「SEG_STAGE 窗口用尽」。
  单条纯上传占用的回包等待 **1 → 0**（`wait_replies`，正是 §3.2 里"一帧 55,428"的那个计数器）。
  按形状实测（`RemoteClientControls.ReplyWaitsByOpForAnAtlasShapedLoad`，主机 inproc，按 op 计数）：**图集拼接**（一张 level 上 64 次 `glTexSubImage2D`）**64 → 0** 次回包等待；
  **每 sprite 一张纹理**（64 张纹理 + 256 次 `glTexParameteri`）**384 → 320**，剩下的全是 per-resource / per-call 的三条 row（create 76 / respecify 130 / params 130）——
  **这两条后来被"首次使用确认"取代了**（见下面那条）：对象的存在性由它自己的 create 确认为事实之后，
  params 与 texture respecify 都不再逐条买答案，而是只在**首次使用**时付一次阻塞的 create。
  同一帧的累计实测：**43,232 waits / 221.5 s → 4,543 waits / 31.3 s（7.1×）**，staged 字节与发出的记录数（103,094）都不变。
  真机（TrebleDroid GSI，MC 26.2 经 TCP 入世界）那一帧的构成为 `ResourceRespecify` 20,633 + `SetTextureParams` 18,056 + `ResourceCreate` 4,536 条**各等一次**，
  而 `ResourceSubData` 12,671 条**零等待**（B 的实机证据）；这些记录都是真实状态变化，客户端没有可去重的（同一值的 `glTexParameteri` 本来就不产生记录）。
- **单次往返那 ~5 ms 是设备侧的深度空闲退出，不是链路也不是客户端**（P12，本轮实测）：同一帧、同一批记录，
  客户端自旋 40× 只快 ~10%；换 `adb forward`（USB）快 ~25–30%；而把设备 8 核钉 `performance` 并关掉 `cpuoff_l`/`clusteroff_l`/`mcusysoff`/`s2idle` 后，
  单次等待 **5,040 → 3,715 µs**、那一帧墙钟 **221.5 s → 166.5 s（−25%）**——记录数与字节数都不变。
  这解释了 handoff §3.6「换快 3.4 倍的链路墙钟不变」：钱花在设备每次唤醒上；handoff §3.2 那 249 s 是在**未定频**的机器上取的，含这份空闲税。
  **同一个诊断给出一条可移植修法**：把服务端的自旋预算调大（Activity 的 `env` extra → `MOBILEGL_IPC_SPIN_US=2000`，不需要 root、不需要改代码），
  那一帧 **221.5 s → 182.8 s（−17%）**；设备定频+关深度空闲则是 **166.5 s（−25%）**。服务端日志会打印 `spin 2000 us` 自证。
  见 [`notes/p12/DEVICELOST-AND-UPLOAD-WAITS.md`](notes/p12/DEVICELOST-AND-UPLOAD-WAITS.md) §2.6–2.10。
- **画面正确**：Vulkan 后端真机画面检查 36/36 通过，CTS 五块相对基线没有超过 0.5 个百分点的退步、没有新崩溃（P7）；server 自有窗口上屏两后端 SSIM 1.0（P12）。
- **五臂传输对比（2026-09-28，未定频交错 A/B）**：重负载（rd12）下 inproc ≈ spawn+shm 比 monolith **快约 1/3**（run-ahead 流水线并行 + applier 路径探针更少），轻负载（openra）下 split 只付固定开销；**tcp localhost 慢 ~8 倍，瓶颈是 present credit=1 的串行等待（~74%）+ futex 唤醒税**，`MOBILEGL_IPC_SPIN_US=2000` 单项实测 21.7→63.6 fps；跨主机 TCP 的「`eglMakeCurrent` hang」**是测试环境造成的**（主机 v2rayN `xray_tun` 代理终结 TCP 后中继卡住，不是源码 bug）；绕开后 P12 出口门 (b) 通过：WSL client 经 Wi-Fi 重放 rd12 世界内 trace，ssim 0.999883，稳态 17–22 fps（`PRESENT_CREDIT` 1/2/3 = 16.9 / 21.3 / 22.1），openra 稳态 105–133 fps；加载帧 2.7 GB 受带宽约束（server 实测 52 MB/s），稳态受往返延迟（20–24 MB/s）。全文 [`notes/perf-five-arm-20260928`](notes/perf-five-arm-20260928/README.md)。

## 按阶段

| § | 阶段 | 一句结论 | 全文 |
|---|---|---|---|
| 1 | P0 | 能从应用进程拉起第二个进程；跨进程共享 GPU 内存只有一条路在所有设备上可用 | [`notes/p0`](notes/p0/README.md) |
| 2 | P1 | 逐 draw 核对推送状态，79 条 trace 零分歧 | [`notes/p1`](notes/p1/README.md) |
| 3 | P2 | 桌面基准：新 tracker 比它替掉的旧填充更便宜 | [`notes/p2`](notes/p2/README.md) |
| 4 | P3a | 两机真机 Release：接口代价 +6–12%，VAO 切换密集的场景 +27–30% | [`notes/p3a`](notes/p3a/README.md) |
| 5 | P4a | 换到 Redmi；P4a 自己只加 3–6 个百分点 | [`notes/p4a`](notes/p4a/README.md) |
| 6 | P5 | 两线程第一帧画对；真机两线程臂还跑不完 | [`notes/p5`](notes/p5/README.md) |
| 7 | P5b | 真机两线程跑通四条游戏 trace；两线程额外代价 +6–18% | [`notes/p5b`](notes/p5b/README.md) |
| 8 | P5c | 两线程之间零共享内存后，全部测试仍绿 | [`notes/p5c`](notes/p5c/README.md) |
| 9 | P5d | 游戏内 7–13 → 103–106 fps，client CPU 15.0 → 9.2 ms/帧 | [`notes/p5d`](notes/p5d/README.md) |
| 10–11 | P5e | 发完就走比每条都等快 69%；重负载下与单线程持平 | [`notes/p5e`](notes/p5e/README.md) |
| 12 | P6 | 两进程与两线程持平；每帧数据量与记录数首次实测 | [`notes/p6`](notes/p6/README.md) |
| 13 | P3b/P4b | 纹理上传形状在四种拓扑下完全一致 | [`notes/p34b`](notes/p34b/README.md) |
| 14 | P6.5 | 跨机 TCP 102/102；断线约 5 s 检测到 | [`notes/p65`](notes/p65/README.md) |
| 15 | P7 | 真机画面 36/36；server 内存无界增长收住（主机 825 → 546 MiB） | [`notes/p7`](notes/p7/README.md) |
| 16 | P12 | 审查后真机复测：Espryt SSIM 1.0；Magma 1.0 / 0.999999511；P12 定向 CTest 46/46；纹理上传回包等待 1→0（主机机制门，真机墙钟待测）；**P12 已收官（09-29）**：跨机 TCP 门 (b) 通过（rd12 ssim 0.999883、稳态 17–22 fps）；FCL 门 (a) 通过（Minecraft 上屏到 render server 窗口，杀 server 后 33–54 ms 闩住 device-lost） | [`notes/p12`](notes/p12/README.md) |
| 17 | 五臂传输测量（2026-09-28，非阶段） | inproc/shm 重负载比 monolith 快 ~1/3；tcp localhost 瓶颈是 credit=1 + 唤醒税（SPIN_US=2000 → 2.9×）；跨主机 hang 实为主机 xray TUN 代理造成；直连 Wi-Fi 稳态 rd12 17–22 fps、openra 105–133 fps | [`notes/perf-five-arm-20260928`](notes/perf-five-arm-20260928/README.md) |
| 18 | P11 | 同机零拷贝（T0）MC 26.3 峰值内存 Espryt −41–42%、Magma −18–26%，ssim / 帧时 / CPU 不变；外部 client 走共享内存 rd12 Espryt 约 119 fps（TCP 40–53）；split T2 相对单进程采纳 p99 Espryt +37–42%、Magma 约 2.5×。未量：M2 后 split server 上纹理无 GPU 写时 Espryt 自己按存储阈值取舍精确写入框，非 ring 路径可能多发几个小框 | [`notes/p11`](notes/p11/README.md) |
