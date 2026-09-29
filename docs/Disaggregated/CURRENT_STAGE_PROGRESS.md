# 当前阶段：P11 进行中（2026-09-29）

> **更新 2026-09-29**。这一页只有摘要；计划、裁定与测量在 [`notes/p11/README.md`](notes/p11/README.md)（计划 [`PLAN-P11.md`](notes/p11/PLAN-P11.md)、裁定 [`INTEGRATOR-DECISIONS-P11.md`](notes/p11/INTEGRATOR-DECISIONS-P11.md)）。上一阶段 P10 同日收官，见 [`notes/p10/README.md`](notes/p10/README.md)。

## 目标

大 buffer（≥ 16 MiB）的 persistent map 在 split 下诚实地落在它声明的档位上；同一台设备上从其他 app 启动的 GL 程序（Termux 一类）能走共享内存，并能导入 client 分配的 AHardwareBuffer（T0）。

## 完成情况

| 项 | 状态 |
|---|---|
| 范围重划 | ✅ `gPipeInputs` 版本化与 T1 关闭；采纳基线三个数撤下，改为同会话重测（ID-P11-1） |
| 合并 dev（重铸读回、局部上传、Magma 排序、Magma `eglSwapInterval`） | ✅ 两后端 × 单进程 / Split / Spawn / Tcp；push 单进程臂上的一个丢写已修；G1 换基线（ID-P11-6） |
| A1 档位在握手时定 | ✅ Stream 上一行具名拒绝后 T2；共享段上 client 握手期具名 Fatal；无线上格式改动 |
| A2 断言 arena 落在哪一档、三臂登记 | ✅ 两后端 |
| A3 T0 在 app 域 | ✅ 两后端 GO（含 128 MiB 与持续锁定） |
| A4 采纳基线（Adreno 830） | ✅ 采纳省 RSS 360–480 MB、不改帧时；split T2 在 MC 26.3 上 RSS 1.4–2×、p99 Espryt +37–42% / Magma 约 2.5×（`notes/p11/A-DEVICE.md`） |
| B0 跨 app client 探针 | 进行中（shell 里的 retrace 可执行文件 → 另一个 app 里的 server；再在 app 域复核） |
| B1 外部 client 的共享内存数据面 / B2 T0 | 待 B0 |
| 主机门（集成头） | ✅ 全绿，数字见 ID-P11-9 |

## 下一步

B0 结果 → B1（通道 + 共享内存数据面）→ B2（T0 导入，两后端）。转出的 dev 缺陷（Espryt 零散写入并集框、Magma 单进程 rd12 映射耗尽）已开独立任务，见 [`notes/DEBTS.md`](notes/DEBTS.md)。

**阻塞**：没有需要决策的事项。
