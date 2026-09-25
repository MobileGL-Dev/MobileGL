# 实测（索引）

> 这一页只有结论和去向；完整的表、命令、协议与读法在各阶段 `notes/<阶段>/README.md` 的"实测"节。代码和旧笔记里的 `MEASUREMENTS.md §N` 按下表"§"列找到对应阶段（小节编号在那里原样保留）。

**口径**：性能只记录、不设门；看**逐线程 CPU 时间**，不看整机帧率。性能对比只用一台手机（Redmi `2f7cbe2e`，Adreno 830），定频、同热窗口、两臂交替跑（[`guide/pin-verification-2026-09-07.md`](guide/pin-verification-2026-09-07.md)）。

## 关键结论

- **接口本身的代价**：单进程里改成"前端推、后端收"，CPU 多约 6–12%（P2），已接受；顶点数组切换密集的场景更高（P3a，+27–30%）。
- **拆成两线程**：起初游戏里只有 7–13 fps；优化后 103–106 fps（P5d）；client 发完就走以后，重负载（渲染距离 32）下与单线程持平（P5e）。
- **拆成两进程**：真机上与两线程成本持平；两者的 GL 线程 CPU 都比单线程少约 30%（后端工作挪到了另一线程 / 进程）（P6）。
- **跨机**：电脑经 TCP 连手机，102 个集成用例全过；server 被杀后 133 ms、Wi-Fi 断开后约 5 s 干净报"设备丢失"（P6.5）。
- **纹理上传的等待粒度（P12，handoff §4）**：wire 臂上纹理那半 `resource_subdata` 不再逐条买回包，等待点从「每条记录」落到「SEG_STAGE 窗口用尽」。
  单条纯上传占用的回包等待 **1 → 0**（`wait_replies`，正是 §3.2 里"一帧 55,428"的那个计数器）；**真机端到端墙钟仍待复测**，见 [`notes/p12/DEVICELOST-AND-UPLOAD-WAITS.md`](notes/p12/DEVICELOST-AND-UPLOAD-WAITS.md)。
- **画面正确**：Vulkan 后端真机画面检查 36/36 通过，CTS 五块相对基线没有超过 0.5 个百分点的退步、没有新崩溃（P7）；server 自有窗口上屏两后端 SSIM 1.0（P12）。

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
| 16 | P12 | 审查后真机复测：Espryt SSIM 1.0；Magma 1.0 / 0.999999511；P12 定向 CTest 46/46；纹理上传回包等待 1→0（主机机制门，真机墙钟待测）；FCL 与跨机 TCP 收官门仍待完成 | [`notes/p12`](notes/p12/README.md) |
