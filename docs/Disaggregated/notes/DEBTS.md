# 仍开放的债务（跨阶段）

> 路线图只列"有债务、看这里"。每条写明去向；已关闭的历史债与当时的完整债务表见 [`p5b/README.md`](p5b/README.md) 末节。开放问题另见 [`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)。更新：2026-09-29（P10 收官；P11 进行中）。

| 债务 | 去向 |
|---|---|
| 未接入内容分块的 record 类型：program archive 与 `draw_vbo` range 尾 | P6.5 sl 与 P8 共用的分片（开放问题 11） |
| 仿真路径的具名拒绝：multi-draw client indices、RGB CPU mip 等（class C 已于 P10 清零） | P8 |
| P10 转出：`GL_ARB_gpu_shader_fp64` 按 server 的 `MOBILEGL_ADVERTISE_FP64` 宣告，fp64 收窄却在 client 的编译里；tcp 下 server 环境不同时两者不一致（与已修的 `GL_KHR_parallel_shader_compile` 同类，挂点 `CapsMirror::Adopt`，ID-P10-10） | 小修，无触发报告 |
| P10 转出（性能）：设备上 tcp loopback 的 rd12 每帧 19.4 ms 中 ~11.2 ms 是生产者在 stream 传输里的停车（发送 + 环 / stage / ack 等待），credit 等待只有 ~0.2 ms（`notes/p10/C-MEASUREMENTS.md` §2，ID-P10-13） | 路线图推完后的性能工作 |
| P11 转出（性能）：设备上 MC 26.3，split T2 相对单进程采纳的 p99 Espryt +37–42%、Magma 约 2.5×，RSS 1.4–2×；其中一部分是线程落位——server apply 线程钉在 6–7 核，client GL 线程多半落到慢核，每帧 CPU 约 2.3×（`notes/p11/A-DEVICE.md`） | 路线图推完后的性能工作；RSS 由 B（T0）处理 |
| P11 转出：Espryt 对零散写入按并集框上传，把影子里的旧纹素盖到框内 GPU 写过的空隙上（8×8 探针 41 个错，pull 与全部 Espryt 臂；Magma 对）；dev `568090f0` 的 `AdoptDriverLevelIntoShadow` 有同样的并集框近似（读代码） | dev（独立任务） |
| P11 转出：Magma 单进程重放 rd12 在加载帧耗尽 `vm.max_map_count`（65530，其中 57–60k 是 `/dev/kgsl-3d0` 映射）而 abort；split 的 Magma 与 Espryt 都正常 | dev（独立任务） |
| P11 转出：client 与 server 共用日志前缀时，client 以 `"w"` 打开 `<base>.server.log` 写转发来的 server 日志，会截断 server 自己的文件（依赖 server 日志行的 P12 装置测试可能受影响） | 小修 |
| P11 转出：pull 构建没有任何门跑集成测试（`TextureRemintPullScenario` 在 pull 上自 P9 起就是红的，P11 M 修） | CI / 门的覆盖，需裁定 |
| `GetCaps` 的两个 blobref | 一旦运输，必须有 server → client 的 carrier rule，不能套 `SEG_STAGE` |
| E1 对照的重定义（ID-122） | 无主（开放问题 24） |
| `MOBILEGL_IPC_IDLE_EXIT_S` 未解析；`CONTRACT-P6.md` 仍把 `DynamicBackendParameters` 定宽记在 P7 名下（已由 P6.5 wf 落地） | 文档 / 小修 |
| P12 未做（2026-09-29 收官时转入）：DirectGLES `g_Display` / `g_Surface` / `g_Context` 仍是全局（每进程一个会话时不需要）；cached-app freezer 的完整处理（文档从未定义）；多 context；D8 窗口种类白名单（X11 / Win32Hwnd / None 仍被接受并把 token 强转为指针）；TLS；server 窗口不转发输入；修后的横屏 server 窗口未重测 | 无主；D8 等 Windows / X11 client，其余等出现第二个会话 / 第二个 context 的需求 |
| P9 转出：`TCP_USER_TIMEOUT` = 5000 ms——client 停读控制超过 5 s 且有待发日志字节时内核仍断开连接；`PublishSessionFault` 的阻塞写对停读的 client 最多等同样 5 s（ID-P9-10） | 传输策略，需裁定 |
| P9 转出：Espryt 重铸保留路径上 framebuffer clear 后 `imageLoad` 偶读旧值（主机 3/30，加 barrier 后 0/30；`TextureRemintPullScenario` case C 暂钉 barrier），疑 llvmpipe（ID-P9-11）；P11 合并 dev 后 push 单进程臂上也见过一次，`ImageBindableRemintScenario` 同样钉 barrier（ID-P11-6） | 待归因 |
| P9 转出：Magma 每次 draw 对可写 SSBO / texel buffer / XFB 目标发整 buffer 的 `OnGpuWritten`（create-indirect 首帧 7,093 条），client 已自标同一批，冗余无害；`OnTextureWriteback`、`OnMipLevelsGenerated`、`OnCapsInvalidated` 仍无生产者 | 以后的收窄 / 清理候选（ID-P9-7） |
| `MOBILEGL_PIPE_TEXEL_RETAIN_MB` 已无消费者，但在 pull 构建里 | P13（G1） |
| 树外脚本：普查跑器、`wsl_p5_gate.sh` 等在 `~/w7/notes/`，git merge 不会传播 | 需要时入库 |
| 临时 CI trigger | 合入 `dev` 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器 |

已关闭的历史债（P5 的 27 个 wrong-answer、`rsp` 残余读、P5b 的 inproc 依赖、ABI 指纹、默认 staging 装不下 128 MiB 上传、184 符号棘轮 CI 门）及当时的完整债务表见 [`notes/p5b/README.md`](p5b/README.md) 末节。
