# 仍开放的债务（跨阶段）

> 路线图只列"有债务、看这里"。每条写明去向；已关闭的历史债与当时的完整债务表见 [`p5b/README.md`](p5b/README.md) 末节。开放问题另见 [`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)。更新：2026-09-24。

| 债务 | 去向 |
|---|---|
| `SEG_REPLY` 2 MiB 单槽 payload cap、PACK-PBO 回读的 fire-and-forget | P9（ID-47、ID-57）；stream 链路上 cap 变成 `maxReplyBytes` 窗口 |
| 未接入内容分块的 record 类型：program archive 与 `draw_vbo` range 尾 | P6.5 sl 与 P8 共用的分片（开放问题 11） |
| class-C 余项与仿真路径：`SetSwapInterval`（P10）、`DeleteTransformFeedback`（P9）；multi-draw client indices、RGB CPU mip、`texture-remint-pull` 等具名拒绝 | P8 / P9 |
| `GetCaps` 的两个 blobref | 一旦运输，必须有 server → client 的 carrier rule，不能套 `SEG_STAGE` |
| E1 对照的重定义（ID-122） | 无主（开放问题 24） |
| `MOBILEGL_IPC_IDLE_EXIT_S` 未解析；`CONTRACT-P6.md` 仍把 `DynamicBackendParameters` 定宽记在 P7 名下（已由 P6.5 wf 落地） | 文档 / 小修 |
| P12 文档债：`MG_Remote/CONTRACT-P12.md` 未写；`protocol.fbs` 的窗口注释仍是旧说法 | P12 收尾 |
| 树外脚本：普查跑器、`wsl_p5_gate.sh` 等在 `~/w7/notes/`，git merge 不会传播 | 需要时入库 |
| 临时 CI trigger | 合入 `dev` 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器 |

已关闭的历史债（P5 的 27 个 wrong-answer、`rsp` 残余读、P5b 的 inproc 依赖、ABI 指纹、默认 staging 装不下 128 MiB 上传、184 符号棘轮 CI 门）及当时的完整债务表见 [`notes/p5b/README.md`](p5b/README.md) 末节。
