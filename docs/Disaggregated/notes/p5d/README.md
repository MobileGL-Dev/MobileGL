# P5d — `inproc` 性能专项（`cb06538c`、`56a77348`、`1f8de61b`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 起因：P5c 收官后 FCL + Minecraft 26.3（VD12）上 inproc 只有 7-13 fps，monolith ~116 fps。
- 三轮：persistent-map 推送三级臂（mprotect 脏页 + 哈希抑制 + 绑定过滤）、barrier 批处理（`MOBILEGL_IPC_BATCH_WAITS`）与延期销毁、自旋时钟与零锁 apply 轮询、`OnApplyThread()` 内联、shadow 页对齐等。
- 结果（Redmi，CPU 定频）：inproc **64.3 → 103-106 fps**（p50），client 线程 CPU 15.0 → 9.2 ms/帧、apply 12.4 → 7.0-7.4。未完成：R-1 序列化（研究结论归 P11 的 `gPipeInputs` 版本化）、线程放置（内核忽略亲和）。
- 报告：[`P5D-INPROC-PERFORMANCE.md`](P5D-INPROC-PERFORMANCE.md)（2026-09-24 从上层移入）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5d** `inproc` 性能专项
- **状态**：✅ `cb06538c`、`56a77348`、`1f8de61b`
- **落地什么 / 范围**：一二轮：persistent-map 推送三级臂（mprotect 脏页追踪 + 哈希抑制 + 绑定过滤 `PushDrawConsumers`）、barrier 批处理 `MOBILEGL_IPC_BATCH_WAITS` + 延期销毁队列、自旋时钟 64 合 1、零页注册 + shadow 页对齐分配。三轮（按符号化 profile 逐项切，四包 + 一份 lockstep 可行性研究）：apply 空转轮询零锁（控制邮箱原子影子位）、`Doorbell::Wait` 校准自旋稳态不读时钟、wait/park 计数进 stats；`OnApplyThread()` 内联为线程指针比较、三个 scope 深度计数器退掉 `thread_local`；shadow 分配尺寸补页、追踪范围向外对齐、边块哈希消失、脏页排空修复；image unit 高水位、残余填充 memo、server stamp 常量表；`generate_mipmap` 不再免等（apply 读残余输入）、death 测试围栏
- **验收门 / 证据**：三轮设备（Redmi，CPU 定频）：inproc **64.3 → 103-106 fps**（p50），client 线程 CPU 15.0 → 9.2 ms/帧、apply 12.4 → 7.0-7.4；monolith 同场景 115（120 Hz vsync 封顶的运行）/ 206（一次未封顶，渲染线程 5.0 ms/帧 大核）；配对比较以逐线程 CPU/帧为主，见报告"数字"节；unit 2203/2203、`integration-split` 111/111（两条曾 flaky 的用例复跑 6/6）；双生成器、include 闭包、dirty-surface、doc 引用绿。未完成：R-1 序列化（研究结论：留给 P3b/P4b 后的 P11 `gPipeInputs` 版本化）、线程放置（内核忽略亲和）、`SetHashSuppressor` 哈希等小项。报告：[`P5D-INPROC-PERFORMANCE.md`](P5D-INPROC-PERFORMANCE.md)

## 实测：P5d（`cb06538c`、`56a77348`、`1f8de61b`；Redmi `2f7cbe2e`，FCL + Minecraft 26.3-rc-3 世界 "test"，VD12，DirectGLES）（原 `MEASUREMENTS.md` §9）

协议：CPU 定频（大核 1958400 / 小核 1555200，`pin_device.sh`）、风扇二档、世界内 20 s 静置后取 30 s 窗口；fps = `MGPipe stats:` 行的帧数 / 时间（应用侧 swap 计数），逐线程 CPU = `/proc/<pid>/task/*/stat` 的 utime+stime 差 / 帧数。两个已知的不受控变量：GPU 由厂商守护进程在每次启动时把 `min_pwrlevel` 打回 12（inproc 各臂 GPU 都在 342 MHz 空转、CPU 受限，不受影响；monolith 或受 GPU 或 120 Hz vsync 封顶）；内核对 app 线程忽略 `sched_setaffinity`，client GL 线程常落在中核（cpu2/4/5）、apply 线程常驻大核 cpu7。因此配对比较以 **client 线程 CPU ms/帧** 为主指标。

| 构建 | 臂 | fps p50 | client ms/帧（核） | apply ms/帧（核） | 备注 |
|---|---|---|---|---|---|
| 二轮头 `56a77348` | inproc | 64.3 | 15.03 (cpu5) | 12.42 (cpu7) | 起点；`SPIN_US=2000` 68-73（三次），`=10000` 63，`SERVER_AFFINITY=off` 65.9 |
| 二轮头 `56a77348` | monolith | 116.5 | 5.85 (cpu7) | — | 75% 忙，vsync 120 Hz 封顶 |
| 三轮审查前构建 | inproc | 114.0 | 8.68 (cpu4) | 6.93 (cpu7) | `SPIN_US=2000` 112.3（不再有收益） |
| 三轮审查前构建 | monolith | 115.7 | 5.70 (cpu5) | — | 封顶 |
| 三轮（本提交） | inproc | 103-106 | 9.2 | 7.0-7.4 | 交错会话，见下 |
| 三轮（本提交） | monolith | 115（封顶）/ 206（一次未封顶） | 5.0-6.2 (cpu7) | — | 三次：115.3 / 115.1（120 Hz 封顶）与 205.8（未封顶） |

交错会话（三轮头，post-review 构建，同一次定频、同一天）：

| 臂（顺序） | fps p50 (min / max) | client ms/帧（核） | apply ms/帧（核） | 备注 |
|---|---|---|---|---|
| mono1 | 205.8 (110 / 280) | 5.0 (cpu7) | — | 未封顶的一次；窗口内 110→280 摆动 |
| inproc1 | 106.1 (91 / 112) | 9.24 (cpu4) | 7.0 (cpu7) | |
| mono2 | 115.3 (108 / 117) | 6.2 (cpu7) | — | 120 Hz 封顶 |
| inproc2 | 103.3 (82 / 111) | 9.2 (cpu5) | 7.4 (cpu7) | |
| inproc3 | 101.9 (83 / 109) | 9.1 (cpu5) | 7.3 (cpu7) | |
| mono3 | 115.1 (109 / 117) | 6.1 (cpu7) | — | 封顶 |

审查前构建（同一天早些、机身 50.8 °C 起）inproc 114.0 / client 8.68 ms（cpu4）/ apply 6.93；post-review 构建 pmap 线流量与之逐字节相同（364.7 KB/帧），差在运行间漂移（机身 53-54 °C 起、中核放置）之内。

`wait[]` 计数（三轮加入 stats 行）：审查前构建 inproc 30 s 窗口累计 srv=7.56M / srvpark=76k、cli=6.70M / clipark=17k；`SPIN_US=2000` 下 park 各降两个量级、fps 不变——park 已不是主项。持久映射线流量 pmap ≈ 0.36 MB/帧（二轮 0.23，三轮多了首推与脏页排空）。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P5D-R3.md`](BRIEF-P5D-R3.md) | P5d round 3 brief: inproc performance, the profile-directed cuts |
| [`P5D-INPROC-PERFORMANCE.md`](P5D-INPROC-PERFORMANCE.md) | P5d 报告：inproc 性能专项（2026-09-18） |
| [`RESULTS-P5D-R3.md`](RESULTS-P5D-R3.md) | P5d round 3 - device results log (Redmi 2f7cbe2e, FCL + MC 26.3-rc-3 world "test", VD12, DirectGLES) |
| [`reports/`](reports/) | 5 个文件：三轮各包（B / D / T …）与 lockstep 可行性研究的报告 |
