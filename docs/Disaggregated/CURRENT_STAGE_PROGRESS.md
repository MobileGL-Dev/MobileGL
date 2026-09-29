# 当前阶段：P9 已收官（2026-09-29）；下一阶段 P10 / P8 待排

> **更新 2026-09-29**。这一页只有摘要；裁定明细、包报告与证据在 [`notes/p9/README.md`](notes/p9/README.md)（范围重划与交接 [`HANDOFF-P9.md`](notes/p9/HANDOFF-P9.md)）。上一阶段 P12 同日收官，其状态与五臂性能对比见 [`notes/p12/README.md`](notes/p12/README.md)、[`notes/perf-five-arm-20260928`](notes/perf-five-arm-20260928/README.md)。

## 目标

server → client 的结果回传在 split 下不阻塞、不丢、不乱序：回读、日志、纹理重铸都不再让一方同步等另一方。

## 完成情况

| 项 | 状态 |
|---|---|
| W1：`glReadPixels` / `glGetTexImage` 读进 pack buffer 不等回复 | ✅ 两后端；新 op 82/83，server 经 buffer 写入口落行，client 标"GPU 写过"；UNPACK 源在 split 下先同步 |
| W2：纹理重铸 | ✅ 不需要拉取协议（Espryt 读回驱动上存在的层 + server 暂存，Magma GPU→GPU）；修"只定义未上传的层"的 Fatal；`OnTexturePullRequest` 删 |
| W3：server 日志按级别转发 | ✅ 队列 + 发送线程：≤WARN 有损计数、ERROR 无损限速、FATAL 有界；`OnLog` 删 |
| W4：事件批处理 / 收窄 | 不做：全语料 0 丢弃、0 阻塞 |
| W5：`OnGlError` 有序 | 保持 P5e 放宽，写进契约 |
| 故障注入 F1（device lost 时 PBO 读还欠着）/ F2（client 停读、server 狂打日志） | ✅ 各有门，各真红过一次 |
| 主机门与单进程构建二进制不变（G1） | ✅ 符号增 0 减 0，`.text` `0xa52203`；全部车道绿（数字见裁定 ID-P9-12） |
| 真机 | 未做（本阶段无真机门；主机覆盖两后端，CI 兜底） |

## 收官（2026-09-29）

- 契约 [`MG_Remote/CONTRACT-P9.md`](../../MobileGL/MG_Remote/CONTRACT-P9.md)；回调 9 → 7；目录追加 2 个 op；控制协议修订号不变。
- 转入 [`notes/DEBTS.md`](notes/DEBTS.md)：monolith Espryt 重铸覆盖 GPU 写过的纹素（dev，已开独立任务）；`TCP_USER_TIMEOUT` 5 s 与 `PublishSessionFault` 阻塞写；Espryt 保留路径 clear 后 `imageLoad` 偶读旧值（待归因）；Magma 冗余 `OnGpuWritten`。
- 收官审查沿用 P12 的做法：不派 agent / Codex，契约断言由本人对照代码核过，G1 同机复核。

## 下一步

按 [`ROADMAP.md`](ROADMAP.md)：P10（同步对象、查询与帧节奏；FCL 里 Minecraft 稳态每帧约 1 次的 `FenceWait` 回包在这里）→ P11（IPC 跑道）；P8（monolith 跑道）；P6.5 残余（39 例 device 矩阵）与 P3b/P4b 余项并行。

**阻塞**：没有需要决策的事项。
