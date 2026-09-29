# 仍开放的债务（跨阶段）

> 路线图只列"有债务、看这里"。每条写明去向；已关闭的历史债与当时的完整债务表见 [`p5b/README.md`](p5b/README.md) 末节。开放问题另见 [`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)。更新：2026-09-29（P10 收官；P11 进行中）。

| 债务 | 去向 |
|---|---|
| 未接入内容分块的 record 类型：program archive 与 `draw_vbo` range 尾 | P6.5 sl 与 P8 共用的分片（开放问题 11） |
| 仿真路径的具名拒绝：multi-draw client indices、RGB CPU mip 等（class C 已于 P10 清零） | P8 |
| P10 转出：`GL_ARB_gpu_shader_fp64` 按 server 的 `MOBILEGL_ADVERTISE_FP64` 宣告，fp64 收窄却在 client 的编译里；tcp 下 server 环境不同时两者不一致（与已修的 `GL_KHR_parallel_shader_compile` 同类，挂点 `CapsMirror::Adopt`，ID-P10-10） | 小修，无触发报告 |
| P10 转出（性能）：设备上 tcp loopback 的 rd12 每帧 19.4 ms 中 ~11.2 ms 是生产者在 stream 传输里的停车（发送 + 环 / stage / ack 等待），credit 等待只有 ~0.2 ms（`notes/p10/C-MEASUREMENTS.md` §2，ID-P10-13） | 路线图推完后的性能工作 |
| P11 转出（性能）：设备上 MC 26.3，split T2 相对单进程采纳的 p99 Espryt +37–42%、Magma 约 2.5×，RSS 1.4–2×；其中一部分是线程落位——server apply 线程钉在 6–7 核，client GL 线程多半落到慢核，每帧 CPU 约 2.3×（`notes/p11/A-DEVICE.md`） | 路线图推完后的性能工作；RSS 由 B（T0）处理 |
| P11 转出：client 与 server 共用日志前缀时，client 以 `"w"` 打开 `<base>.server.log` 写转发来的 server 日志，会截断 server 自己的文件（依赖 server 日志行的 P12 装置测试可能受影响） | 小修 |
| P11 转出（B0 F1）：HyperOS 熄屏时冻结 server app 的整个 uid（FGS 进程、supervisor、会话子进程同一 cgroup），在飞会话 120 s 后 `Fatal{BarrierTimeout}`；FCL 同机用法同样受影响（`notes/p11/B0-CROSS-APP.md`） | B1 的防冻结一项；FCL 侧另议 |
| P11 转出（M2）：上传仍有两种有损形状，各记一次日志——一层的精确写入框超过 4096 个时按存储的矩形表覆盖发出；applier 一个条目累计超过 4096 个区域时并成并集框。纹理带 GPU 写时两者都会把影子盖进空隙 | 小修，无触发报告 |
| P11 转出（M2）：超过 stage 分块预算的大层按分片发出，没有写入框覆盖的分片仍带自己的整框（server 要靠它拼层图），server 会把这片的影子盖到 GPU 写过的地方；修法要一个两个 server 都跳过的「只定位」区域写法（split 专属，早于 P11） | 待排 |
| P11 转出（M2，性能）：split server 上纹理无 GPU 写时，Espryt 改为自己按存储的阈值取舍精确写入框，非 ring 路径上可能多发几个小框；设备上未量 | 记录 |
| P11 转出（M2，给 dev）：dev 的 `DirectVulkan.SplitRecording.` 车道没有 `RESOURCE_LOCK`、其用例读字面日志路径——`-j` 下可能互相截断；P11 线的副本已修 | 下次向 dev 回流时带上 |
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
