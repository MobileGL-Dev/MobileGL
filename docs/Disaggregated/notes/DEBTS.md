# 仍开放的债务（跨阶段）

> 路线图只列"有债务、看这里"。每条写明去向；已关闭的历史债与当时的完整债务表见 [`p5b/README.md`](p5b/README.md) 末节。开放问题另见 [`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)。更新：2026-09-29。

| 债务 | 去向 |
|---|---|
| `SEG_REPLY` 2 MiB 单槽 payload cap、PACK-PBO 回读的 fire-and-forget | P9（ID-47、ID-57）；stream 链路上 cap 变成 `maxReplyBytes` 窗口 |
| 未接入内容分块的 record 类型：program archive 与 `draw_vbo` range 尾 | P6.5 sl 与 P8 共用的分片（开放问题 11） |
| class-C 余项与仿真路径：`SetSwapInterval`（P10）、`DeleteTransformFeedback`（P9）；multi-draw client indices、RGB CPU mip、`texture-remint-pull` 等具名拒绝 | P8 / P9 |
| `GetCaps` 的两个 blobref | 一旦运输，必须有 server → client 的 carrier rule，不能套 `SEG_STAGE` |
| E1 对照的重定义（ID-122） | 无主（开放问题 24） |
| `MOBILEGL_IPC_IDLE_EXIT_S` 未解析；`CONTRACT-P6.md` 仍把 `DynamicBackendParameters` 定宽记在 P7 名下（已由 P6.5 wf 落地） | 文档 / 小修 |
| P12 未做（2026-09-29 收官时转入）：DirectGLES `g_Display` / `g_Surface` / `g_Context` 仍是全局（每进程一个会话时不需要）；cached-app freezer 的完整处理（文档从未定义）；多 context；D8 窗口种类白名单（X11 / Win32Hwnd / None 仍被接受并把 token 强转为指针）；TLS；server 窗口不转发输入；修后的横屏 server 窗口未重测 | 无主；D8 等 Windows / X11 client，其余等出现第二个会话 / 第二个 context 的需求 |
| FCL fork 的版本设置「游戏退到后台时不暂停」未提交（父仓库在合并冲突中：`FCLauncher.java` 为 `UU`，`.gitmodules` 已暂存） | 用户处理父仓库合并后提交 |
| 树外脚本：普查跑器、`wsl_p5_gate.sh` 等在 `~/w7/notes/`，git merge 不会传播 | 需要时入库 |
| 临时 CI trigger | 合入 `dev` 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器 |

已关闭的历史债（P5 的 27 个 wrong-answer、`rsp` 残余读、P5b 的 inproc 依赖、ABI 指纹、默认 staging 装不下 128 MiB 上传、184 符号棘轮 CI 门）及当时的完整债务表见 [`notes/p5b/README.md`](p5b/README.md) 末节。
