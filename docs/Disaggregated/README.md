# MGPipe：MobileGL 前后端拆分

**做什么**：在 MobileGL 的前端（处理应用的 GL 调用）和后端（调用 GLES / Vulkan 驱动）之间立一份显式接口——前端把状态变化**推**给后端，后端不再直接读前端的内存。有了这份接口，前后端可以放在两个线程、两个进程，甚至两台机器上运行。

**做到哪了**（2026-09-25）：接口与拆分已完成到"两个进程、可经 TCP 跨机"。当前在做 **P12：让 Android 上的 server 自己开窗口，把画面直接显示到屏幕上**——审查后的主机定向测试和 Redmi 真机七项复测已通过；FCL 与跨机 TCP 两项验收仍未完成。→ [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md)

## 一张图

```
应用的 GL 调用
  → 前端 MG_Impl：处理 GL 语义；在每条"做事的命令"（draw、clear…）之前，把状态变化整理成记录
  → MGPipe 接口（两张函数指针表，由调用目录 PipeCalls.def 生成）
  → 三选一：直接调用（单线程）│ 队列 → 另一个线程 │ 队列 → 另一个进程 / 另一台机器
  → 后端 MG_Backend：Espryt（GLES）/ Magma（Vulkan），按记录更新自己的状态并调用驱动
  ← 反向消息（错误、GPU 写回、窗口尺寸）按顺序回到前端
```

## 文档导航

| 想知道 | 看 |
|---|---|
| 现在做到哪、下一步 | [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md) |
| 设计要点与各部分设计 | [`ARCHITECTURE.md`](ARCHITECTURE.md) → [`design/`](design/) |
| 分几个阶段、每个阶段做了什么 | [`ROADMAP.md`](ROADMAP.md) → [`notes/<阶段>/`](notes/README.md) |
| 性能与验证的关键数字 | [`MEASUREMENTS.md`](MEASUREMENTS.md) |
| 怎么构建、运行、测试 | [`guide/build-and-run.md`](guide/build-and-run.md) |
| 代码在哪 / 术语 / 工程纪律 | [`guide/code-map.md`](guide/code-map.md)、[`guide/glossary.md`](guide/glossary.md)、[`guide/discipline.md`](guide/discipline.md) |
| 各阶段的原始计划、裁定、报告、证据 | [`notes/`](notes/README.md) |

整理记录：2026-09-24 两次整理——顶层只留索引，设计正文拆到 `design/`，上手与参考放 `guide/`，阶段细节放 `notes/<阶段>/`；整理前的版本见提交 `1fb18d9`。更早的设计竞赛与评审稿在 git 历史（`8b31de2f`、`1794ac94`、`8349babe`、`87ee17c6`）。
