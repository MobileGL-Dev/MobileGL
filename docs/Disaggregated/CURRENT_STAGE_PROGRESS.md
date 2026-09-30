# 当前阶段：P11 已收官（2026-09-29）；下一阶段 P8 待排

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
| B0 跨 app client 探针 | ✅ SELinux 挡连接，连上后共享段可用（`notes/p11/B0-CROSS-APP.md`） |
| PAIR 连接按 nonce 配对 | ✅ 控制协议修订 3 → 4 |
| helper 可行性探针 | ✅ 无 Context 的 `app_process` helper 经 Binder 拿到连接（依赖 ROM，`HSPIKE.md`） |
| 合并 dev（M2） | ✅ KGSL 分段提交、零散写入按矩形（补齐 split 半边）、描述符池 |
| B1 同机外部 client 走共享内存 | ✅ `fd:` 端点 + 令牌 broker + helper + 启动命令 + 外部 client 的 apply 线程策略；rd12 Espryt 约 119 vs tcp 40–53 fps |
| B2 T0 零拷贝导入 | ✅ 两后端、同 app 与外部路线；**默认开**（用户），用不了静默退回 T2；MC 26.3 峰值内存 Espryt −41–42 %、Magma −18–26 %（`notes/p11/B2-DEVICE.md`） |
| 顺带修掉 | 删除仍绑定的 buffer 留旧句柄、Magma 空 uniform 绑定段错误、Espryt split 用例跳过算通过 |
| 主机门（收官头） | ✅ 全绿，数字见 ID-P11-20 |

## 下一步

拆分路线已走完。按 [`ROADMAP.md`](ROADMAP.md)：**P8**（monolith 跑道：把读前端字节的 CPU 变换下放到 client、仿真路径下放、协议广度），之后 P13（删掉旧的「后端直接读前端」路径）；P6.5 残余与 P3b / P4b 余项并行。

**待用户决策**：P8 开工确认（只读核查在跑）；[`DEBTS.md`](notes/DEBTS.md) 六项「需裁定」（CI 触发条件、`TCP_USER_TIMEOUT`、HyperOS 下 FCL 保活、Mali 复测、TLS、`IDLE_EXIT_S`）。
