# P11 B2：T0 零拷贝导入（真机）

2026-09-29，Redmi `2f7cbe2e`（Adreno 830），默认环境、两个 app 的 HyperOS 电池设「无限制」、CPU 定频并回读、无解冻器。协议与行为见 `MobileGL/MG_Remote/CONTRACT-P11.md` B2 节；报告 `~/w7/notes/p11/b2-report.md`，证据 `~/w7/notes/p11/evidence/b2/`。

## 开关

`MOBILEGL_IPC_ADOPT_TIER`：**不设（默认，用户 2026-09-29）= 请求 T0**，本会话用不了（server 自测不通过、server 端 `MOBILEGL_IPC_ALLOW_ADOPT_T0=0`、Stream 数据面、没有 AHB）时静默退回 T2（只记 MGLOG_D、计数）；`0` = 同样请求 T0，但退回时两端各一行具名日志；`1` = `Refuse{AdoptTierClosed}` 后 T2；`2` = 与之前完全相同（共享段 + client 推送，T2）。不 Fatal。默认开后在红米上不设开关复跑 MC 26.3 inproc：两后端都导入 8 个缓冲（462,946,304 字节）、ssim 通过、无 T0 相关的 W/E 行。控制协议修订 4 → 5（Hello 带 client 请求的档位，Welcome 回显）。

## 先验：AHB 的 fd 跨过每条路线（先于任何构建）

| 路线 | client → server | 结果 |
|---|---|---|
| shell → server app（B1 helper 的 `fd:`） | `shell` → `untrusted_app:c87` | 64 KiB、32 MiB、128 MiB 都通；server 的 GPU 读到 client 在从不解锁的锁下写入的字节；0 个 avc |
| 另一个 app → server app | `c84` → `c87` | 通 |
| 同 app（经 helper / 直连 / spawn 的子进程） | `c87` → `c87` | 通 |

## MC 26.3：开关 0 vs 2（3 次交错，中位）

内存口径 = 两进程 VmHWM（去重共享页）+ kgsl 驱动分配 + kgsl 导入（AHB 页只出现在导入里，RSS 看不到）。

| 后端 | 臂 | 开关 | p99 ms | GL / apply CPU ms/帧 | **峰值总内存 MB** | 稳态 MB |
|---|---|---|---|---|---|---|
| Espryt | inproc | 2 | 46.7 | 10.9 / 11.3 | 3010 | 2992 |
| Espryt | inproc | 0 | 44.1 | 11.5 / 11.3 | **1758（−42 %）** | 1492 |
| Espryt | spawn+shm | 2 | 44.5 | 10.5 / 11.2 | 3049 | 3029 |
| Espryt | spawn+shm | 0 | 44.5 | 10.5 / 10.7 | **1789（−41 %）** | 1526 |
| Magma | inproc | 2 | 75.6 | 15.9 / 20.1 | 2677 | 2669 |
| Magma | inproc | 0 | 75.9 | 15.9 / 20.4 | **1971（−26 %）** | 1774 |
| Magma | spawn+shm | 2 | 76.3 | 15.9 / 20.4 | 2731 | 2713 |
| Magma | spawn+shm | 0 | 75.3 | 15.2 / 19.6 | **2244（−18 %）** | 1815 |

- ssim 开关 0 与 2 逐位相同：MC 26.3 Espryt 0.999684、Magma 0.999600；create-instancing 0.99998。每个开关 0 的会话 8 个大缓冲（441.5 MiB）全部导入。
- 帧时与 CPU 不变；每帧 stage 字节几乎不变（1071 → 1052 KiB）——T0 省的是内存，不是上传量。
- B1 的外部路线（另一个 app 经 helper）同样走 T0：Espryt 峰值 2830 → 1590 MB。

## T0 专属改动的 red-once（真机，Android 集成测试 + static-peek，inproc）

| 改动 | 变红的用例 | Espryt | Magma |
|---|---|---|---|
| 常驻写直接写进 AHB | `ADrawQueuedBeforeASubDataKeepsItsOwnBytes` | 红 | 红 |
| respecify 时立即释放导入 | `ARespecifyWithADrawQueuedKeepsThatDrawsStore` | 绿（驱动大概自己持有引用，未核实；带 fence 的释放保留） | 红 |
| 不发布 `kCapAdoptT0` | `TheArenaLandsInTheTierItsLaneDeclares` | 红 | 红 |
| CPU 读跳过回读往返 | `ReadbackSeesTheLatestCpuWrite`、`GpuWriteIntoTheArenaIsReadBack` | 红 | 红 |
