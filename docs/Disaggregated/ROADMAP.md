# 路线图（索引）

> **2026-09-30**：P0 到 P7、P12、P9、P10、P11、**P8** 已完成——拆分路线走完，单进程路线只剩 P13。P8（重定界）：server 侧仿真缺口补齐（Espryt 生成 mip、暂存副本与 GPU 写、驱动拒读；Magma wire 原生 indirect），246 个 monolith 用例登上 split 各臂并由覆盖门守住；红米上 Create 画错在 dev 与 feat 两条 flush 阶梯修好；契约 `MG_Remote/CONTRACT-P8.md`，G1 整阶段符号不变。下一阶段 **P13**（删掉旧的「后端直接读前端」路径；P8 已把 monolith 要换到的记录臂修好，9 条 monolith 缺陷随之消失）。每个阶段的计划、验收结果、实测与报告在 `notes/<阶段>/`（点阶段名进入）。

## 目标

client（跑应用的一方）与 server（跑驱动的一方）可以在**不同机器、不同系统**上经 TCP 相连；在同一台机器上则用两个进程 + 共享内存。传输分控制面与数据面两层，可各自选择、混搭。

## 两条路线

- **单进程路线**：先把接口建起来，让单进程版本也受益（后端拥有自己的状态、可以挪到渲染线程）。P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → **P8** → P13。
- **拆分路线**：在接口之上逐步拆开前后端。P5 → P5b → P5c → P5d → P5e → P5f → P6 → P6.5 → Ph → P12 → P9 → P10 → **P11**（走完）。

## 阶段一览

| 阶段 | 做什么 | 状态 |
|---|---|---|
| [P0](notes/p0/README.md) | 打地基：计数器、调用目录与生成器、CI 门；验证 Android 能拉起第二个进程 | ✅ 09-05 |
| [P0.5](notes/p05/README.md) | 抽出前后端共用的数据类型头，切断后端对前端头文件的依赖 | ✅ |
| [P1](notes/p1/README.md) | 后端改读一个"输入块"，并能逐 draw 核对它与前端真实状态一致 | ✅ |
| [P2](notes/p2/README.md) | 前端开始主动推送渲染状态；接口代价实测 +6–12%，决定继续 | ✅ 09-08 |
| [P3a](notes/p3a/README.md) | GLES 后端的 buffer、顶点数组改用句柄 | ✅ 09-08 |
| [P4a](notes/p4a/README.md) | GLES 后端的帧缓冲、纹理、sampler、program 改用句柄 | ✅ 09-08 |
| [P5](notes/p5/README.md) | 第一次拆成两线程：命令队列 + 后端线程（每条命令都等） | ✅ 09-16 |
| [P5b](notes/p5b/README.md) | 把剩余命令迁到队列上；真机首次以独立后端线程跑通目标游戏 | ✅ 09-16 |
| [P5c](notes/p5c/README.md) | 两线程之间不再偷偷共享内存，只经队列交换 | ✅ 09-17 |
| [P5d](notes/p5d/README.md) | 两线程版本的性能：游戏内 7–13 fps → 103–106 fps | ✅ 09-18 |
| [P5e](notes/p5e/README.md) | client 发完就走，不再每条命令等后端；重负载下与单线程持平 | ✅ 09-19 |
| [P5f](notes/p5f/README.md) | 所有状态都经队列传递，为拆进程扫清最后障碍；Vulkan 后端也能发完就走 | ✅ 09-20 |
| [P6](notes/p6/README.md) | 拆成两个进程；真机上两进程与两线程成本持平 | ✅ 09-22 |
| [P6.5](notes/p65/README.md) | 传输分控制面 / 数据面；电脑当 client、手机当 server 经 TCP 跑通 | 第一波 ✅，余项交 CI |
| [Ph](notes/p7/README.md) | server 能安全地接受外来连接：令牌、限额、坏数据不致崩溃 | 必需项 ✅（随 P7） |
| [P3b / P4b](notes/p34b/README.md) | GLES 后端的深化与收尾 | 大部分 ✅，余项并行 |
| [P7](notes/p7/README.md) | Vulkan 后端完整迁移；真机画面检查 36/36 通过 | ✅ 09-23 |
| [**P12**](notes/p12/README.md) | **server 自己开窗口上屏，client 不需要窗口** | ✅ 09-29：出口门 B（跨机 TCP）✅ 09-28、A（FCL + 杀 server）✅ 09-29；契约 `MG_Remote/CONTRACT-P12.md`；G1 成立；余项进 [`DEBTS.md`](notes/DEBTS.md) |
| [**P8**](notes/p8/README.md) | ~~把剩余的仿真路径挪到正确的一侧，补齐协议~~ → **补齐 server 侧仿真缺口与 split 覆盖** | ✅ 09-30：重定界（原 10 项里 6 项前序已完成）；Espryt 生成 mip / 暂存副本与 GPU 写 / 驱动拒读、Magma wire 原生 indirect 与 dispatch；246 例登上 split 三臂（`split_coverage.py`）；Create 在 Adreno 上的丢拷贝修到 dev 与 feat；契约 `MG_Remote/CONTRACT-P8.md`；余项进 [`DEBTS.md`](notes/DEBTS.md) |
| [**P9**](notes/p9/README.md) | **反向通道：回读、日志、重铸不再同步等待 client** | ✅ 09-29：PACK-PBO 回读 fire-and-forget（两后端）、日志分级转发、重铸无拉取（`OnLog` / `OnTexturePullRequest` 删，回调 9 → 7），F1 / F2；契约 `MG_Remote/CONTRACT-P9.md`；G1 成立；事件量批处理无实测需求不做；余项进 [`DEBTS.md`](notes/DEBTS.md) |
| [**P10**](notes/p10/README.md) | **同步对象、查询与帧节奏** | ✅ 09-29：fence 轮询由 server 报告本地作答（`kEventFenceSignaled`、`MOBILEGL_IPC_POLL_ESCALATE`；主机 Magma·tcp −37%，Adreno 830 上两后端 0 次往返）；`SetSwapInterval` 转发、class C 清零；query / XFB / AsyncCompile 登记 split 各臂；credit 1/2/3 设备 loopback 在噪声内；契约 `MG_Remote/CONTRACT-P10.md`；G1 成立；余项进 [`DEBTS.md`](notes/DEBTS.md) |
| [**P11**](notes/p11/README.md) | **同机大缓冲零拷贝共享** | ✅ 09-29：T0 零拷贝导入（两后端，默认开，自测不过或 Stream 上静默退回 T2）；同机外部 client 经 helper + 令牌 broker 走共享内存（B1）；连接按 `PairBind` nonce 配对（修订 5）；采纳档位在握手时定；契约 `MG_Remote/CONTRACT-P11.md`；余项进 [`DEBTS.md`](notes/DEBTS.md) |
| [P13](notes/p13/README.md) | 删掉旧的"后端直接读前端"路径 | 待排 |

## 另见

- 工程纪律与验证门：[`guide/discipline.md`](guide/discipline.md)
- 仍开放的债务：[`notes/DEBTS.md`](notes/DEBTS.md)；开放问题：[`notes/OPEN-QUESTIONS.md`](notes/OPEN-QUESTIONS.md)
