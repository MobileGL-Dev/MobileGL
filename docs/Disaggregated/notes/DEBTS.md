# 仍开放的债务（跨阶段）

> **2026-10-07（P13 收官）关闭三行**：ID-P8-13（9 条 monolith 缺陷——monolith 换到记录臂后对应 monolith 登记在集成车道上全过，旧的 monolith 前端臂在 W6 删除）、ID-P8-14（`SyncClientSideVertexArraysForIndirectFetch` 的整 buffer 回读与 Magma monolith 深度 mip 的 view 类型都属于已删的 monolith 前端臂）、`MOBILEGL_PIPE_TEXEL_RETAIN_MB`（W3b 随 pull 构建删除）。新增两行：W9 缓存容量重调与 Magma monolith 性能，都排在 P13 之后的性能轮，基线是 `dev` 构建。
>
> 路线图只列"有债务、看这里"。每条写明去向；已关闭的历史债与当时的完整债务表见 [`p5b/README.md`](p5b/README.md) 末节。开放问题另见 [`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)。更新：2026-09-30（P8 第二波；按债务审查重排。同日：create-indirect 换成派生 fixture `minecraft-1.21.1-neoforge-create-indirect-in-world-align1024`（不是原抓取，16 字节对齐的 SSBO 范围绑定挪到 1024 对齐），ID-P8-14、ID-P8-19 两条关闭——红米两后端 monolith / spawn 全过，Magma spawn 不再丢设备）。去向「需裁定」= 等用户定；「可进行」= 随时可开的小修，不依赖别的阶段。

| 债务 | 去向 |
|---|---|
| CI 覆盖：`test.yml` / `apk.yml` 按分支名（`feat/disaggregated`、`codex/fix-github-ci`）切 `MOBILEGL_CI_DISAGGREGATED` 与车道（本次核对 `test.yml` 14 处、`apk.yml` 4 处）；pull 构建只有 G1 与 monolith 对照，没有集成测试门（`TextureRemintPullScenario` 在 pull 上自 P9 红到 P11 M 才修）；合入 `dev` 后 inproc 验收与 APK split 两臂随分支名关掉，build-linux-split / integration-split / retrace-split 无条件 | 需裁定：合入前是否加 pull 集成车道；合入 `dev` 后各车道的触发条件（顺带删 `feat/disaggregated` 触发器） |
| P9 转出：`TCP_USER_TIMEOUT` = 5000 ms——client 停读控制超过 5 s 且有待发日志字节时内核断开连接；`PublishSessionFault` 的阻塞写对停读的 client 最多等同样 5 s（ID-P9-10）。风险场景：FCL 退后台暂停 + 跨机 TCP | 需裁定：建议调到几十秒、`PublishSessionFault` 改有界写；取值由用户定 |
| P11 转出（B0 F1）：HyperOS 熄屏冻结 server app 整个 uid。B1 已靠用户设置解决（server app 与 client 所在 app 电池都设「无限制」，ID-P11-16）；剩 FCL 同机用法的保活 | 需裁定：只写须知 / app 内检测省电策略并引导 / 不管 |
| P11 转出（B2）：T0 只在红米一台 Adreno 上实测；Mali 等只有 spike B 的 shell 域证据（server 自测会在不支持的设备上退回 T2）。「respecify 时立即释放导入」的变异在 Espryt 上不红（驱动大概自己持有引用，未核实），带 fence 的释放保留 | 需裁定（受设备规矩所限）：开一台 Mali 复测，或长期接受自测退回 |
| P11 转出（B1）：外部 client 的 apply 线程策略只在红米一台（两大核）上量过；Magma openra 两个数据面都卡在 server（约 285 fps），共享内存无收益 | 同上一行 |
| P12 未做：TLS（开放问题 21 当时「令牌够，TLS 留作 P12 开放项」；P12 已收官） | 需裁定：正式放弃，或继续留作开放项 |
| `MOBILEGL_IPC_IDLE_EXIT_S` 未解析（client EOF 时 server 已即时退出） | 需裁定：建议废弃；点头后删 `design/09-build-switches-counters.md:67` 与 `Config.h` 注释等处 |
| P10 转出：`GL_ARB_gpu_shader_fp64` 按 server 的 `MOBILEGL_ADVERTISE_FP64` 宣告，fp64 收窄却在 client 的编译里；tcp 下 server 环境不同时两者不一致（同类的 `GL_KHR_parallel_shader_compile` 已修，做法见 `CapsMirror.cpp:72-101`，ID-P10-10） | 可进行（小修） |
| P11 转出：client 与 server 共用日志前缀时，client 以 `"w"` 打开 `<base>.server.log` 写转发来的 server 日志（`MobileGL/MG_Util/Debug/Log.cpp:89`），截断 server 自己的文件；同机两个进程用同一前缀时同样互相截断 | 可进行（小修） |
| P11 转出（B1）：会话位被慢启动程序占着时，第二个 client 若 2 s 内发 Hello 得具名 `Refuse{Busy}`，更慢的只见发送失败（那条路径不读待发的拒绝）；已写进契约 | 可进行（小修） |
| P11 转出（B1）：会话启动失败后 client 还多报一个误导性的 `Fatal{CapsBeforeFirstSnapshot}`（`CapsMirror.cpp:247`，早于 B1，helper 用户每次被拒都会看到） | 可进行（小修） |
| P11 转出（B2）：handle 臂的 CPU 读者读采纳 / T0 映射前先落排队常驻写——split 半已由 P8-C 实现（`SplitHostBytesForCpuRead` 的 T0 臂：落写 + `glFinish`，ID-P8-9），但主机无 T0、未能证红 | 需设备验证（红米 Android 集成测试 + static-peek） |
| P11 转出（B2）：server 准了 T0 而 client 选了 T2 时，每个存储的 `map_persistent` 等 2 s 再具名拒绝（产品里 grant 与 caps 一致，只在变异下见过） | 可进行（小修：每会话一个「client 不要 T0」提示） |
| P9 转出：Espryt 重铸保留路径上 framebuffer clear 后 `imageLoad` 偶读旧值（主机 3/30，加 barrier 后 0/30；`TextureRemintPullScenario` case C 暂钉 barrier），疑 llvmpipe（ID-P9-11）；P11 合并 dev 后 push 单进程臂上也见过一次，`ImageBindableRemintScenario` 同样钉 barrier（ID-P11-6） | 可进行（在红米上不加 barrier 归因） |
| P8 转出（D）：应用省略 `glMemoryBarrier` 时，两臂在计算写与顶点着色器读之间都没有依赖（GL 未定义，但真实应用依赖它）；D 的每轮屏障在真机上的开销未测；create-indirect（现为派生的 `minecraft-1.21.1-neoforge-create-indirect-in-world-align1024`，内容相同）验证不了这两件事（ID-P8-14），需要一个「计算写命令、无应用屏障、不重写」的 fixture | 待归因（需新 fixture） |
| P8 转出（E）：driver-written 标按整张纹理打（`GenerateMipmap` 也打），驱动拒读时连基级也具名拒绝，偏保守；跨格式拷贝 store 不跟随；拒绝仍是会话 Fatal（要改回包状态才能答 `GL_INVALID_OPERATION`） | 小修（按 level 打标）；其余按需 |
| P8 转出（B）：`PipeCalls.def:260` `GenerateMipmap` 仍是 `kWaitApplied`，裁定 14 的理由（CPU 滤波碰 client 影子）已不成立，可改 `kWaitNone` | 性能工作（协议行为变更，需单独裁定） |
| P8 转出（C）：server SSBO 标记过近似（窗口内每个绑定的 SSBO 都标）；一个留在 SSBO 绑定上又当 restart EBO 用的 buffer 每次 draw 都回读 | 性能工作 |
| P13 转出（W5 翻转后，ID-P13-2 记录项，用户裁定记为债务）：Magma monolith 走记录臂比前端臂慢约 16%——Y700 上 FCL MC 1.21.5、RD2 小场景、时钟未钉：记录臂 168 / 176 fps，同库强制前端臂 206 / 204 fps，即每帧 CPU 约 5.8 ms 对 4.9 ms（渲染线程都约占满一核）；Espryt 持平（224 / 226 对 222）。疑因：`VulkanRenderer::SetupDraw` 在记录臂直接进 `SetupWireDraw`，绕过只在前端臂跑的 `TrySetupDrawFastPath`（PLAN-P13 §1.3 已提示「换臂前先量」）。W6 删前端臂后这部分成本固定下来 | P13 收官后（用户裁定：P13 内不做性能工作）：性能工作的「之前」基线是 `dev` 分支构建（pull monolith），不是 W6 前的 feat 构建；在设备上钉时钟对比 dev 与 feat，场景为 FCL MC 与 anland glmark2，两个后端都测；再用 simpleperf profile Magma monolith 每 draw 成本，决定是否给 wire draw 臂补快路径 |
| P13 转出（W9，用户裁定：P13 内不做性能工作）：缓存容量重调（CSO / 描述符 / 管线缓存容量按 `MOBILEGL_PIPE_STATS` 读数定初值、设备复核）未做，容量仍是 P8 时的值 | P13 之后的性能轮：以 `dev` 构建为基线（同 Magma 16% 那行），先在主机语料取 `MOBILEGL_PIPE_STATS` 读数定初值，再在设备上钉时钟复核 |
| P13 转出（FCL 浸泡，用户裁定：FCL 暂不改、MobileGL 不做规避）：FCL game process can die silently (exit SIGNALED status=34, SIGRTMIN+2): the JVM signal reset after renderer selection drops libcore's AsynchronousCloseMonitor handler, and FCL's okhttp/Okio watchdog then kills the process; renderer-independent (reproduced on P12 and P13 libraries, both backends; untested with other renderers)。浸泡：P13 库 30 次（Magma 22 / Espryt 8）死 18 次、P12 库 10 次死 2 次；P12 / P13 逐次交替的对照（同场景同网络时段）P12 4/10、P13 2/10，时间线（`Renderer:` 行到 MobileGL 首行约 6.2 s、晚死固定在其后约 9.4 s）两库相同，无证据表明 MobileGL 拉高概率；全部 status=34，无 tombstone、无 kgsl；证据与机理见 [`p13/fcl-soak/README.md`](p13/fcl-soak/README.md) | fix belongs in FCL (save/restore libcore's SIGRTMIN+2 sigaction around JVM creation); left for the user |
| P8 转出（MD）：dev 的 `RingAllocateSlow` 在出错的上下文上无上限地排水（设备上挂在 `glFinish`） | 小修（dev） |
| P8 转出：`LogForwardChannel.APeerThatStopsReadingCostsWarnLinesNotTheCallersTime` 在负载下超时（两次），单跑必过 | 小修（放宽时限或隔离） |
| P8 转出（SV）：`gen_pipe_dirty_surface.py` 扫描集成测试源，测试成员名与 GLContext 成员同名（如 `m_parameters`）会静默弄坏它的自检；`.Mip3DValidation.` 在没装校验层的车道上是失败不是跳过 | 小修 / CI 记录 |
| P8 转出（MF）：两条 flush 阶梯附近几处注释仍写旧的冻结 / 钉住提交（`5cb826b0`、`3e298c9a`） | 文档小修 |
| P8-0 转出：设备上基线就有的 SSIM < 0.99——1.21.11-main-menu（Espryt 0.890、Magma 0.165）、derivative-main（Espryt 0.851）；两臂都有 | 待归因 |
| P12 未做：D8 窗口种类白名单——X11 / MetalLayer / Win32Hwnd / None 仍被接受（`SurfaceOpCodec.cpp:138-141`），`HandleFromToken` 把 token 强转为指针（`ServerLoop.cpp:1228`，经 `UnpackWindowHandle`；下游是否交给 EGL 未追到底） | 可进行（Ph 类小修） |
| P12 未做：修后的横屏 server 窗口未重测 | 可进行（红米只读检查） |
| P11 转出（M2，给 dev）：dev 的 `DirectVulkan.SplitRecording.` 车道没有 `RESOURCE_LOCK`、其用例读字面日志路径——`-j` 下可能互相截断；P11 线的副本已修（Magma 空 uniform 的重复修复已在 ID-P8-17 合成一条） | 下次向 dev 回流时带上 |
| P11 转出（M2）：超过 stage 分块预算的大层按分片发出，没有写入框覆盖的分片仍带整框（server 要靠它拼层图），会把影子盖到 GPU 写过的地方；修法要一个两个 server 都跳过的「只定位」区域写法，控制协议修订 +1（split 专属，早于 P11） | 小包或 P8 |
| P11 转出（M2）：上传仍有两种有损形状，各记一次日志——一层的精确写入框超过 4096 个时按存储的矩形表覆盖发出；applier 一个条目累计超过 4096 个区域时并成并集框。纹理带 GPU 写时两者都会把影子盖进空隙 | 小修，无触发报告 |
| P10 转出（性能）：设备上 tcp loopback 的 rd12 每帧 19.4 ms 中 ~11.2 ms 是生产者在 stream 传输里的停车（发送 + 环 / stage / ack 等待），credit 等待只有 ~0.2 ms（`notes/p10/C-MEASUREMENTS.md` §2，ID-P10-13） | 路线图推完后的性能工作 |
| P11 转出（性能）：设备上 MC 26.3，split T2 相对单进程采纳的 p99 Espryt +37–42%、Magma 约 2.5×；其中一部分是线程落位——server apply 线程钉在 6–7 核，client GL 线程多半落到慢核，每帧 CPU 约 2.3×（`notes/p11/A-DEVICE.md`）。RSS 一半已由 B2（T0）交付：峰值内存 Espryt −41–42%、Magma −18–26%（ID-P11-17） | 路线图推完后的性能工作（p99 与线程落位） |
| P11 转出（B2，性能）：`glBufferStorage(NULL)` 的采纳先把整块零存储当 `resource_subdata` 发一遍（T2 与 T0 都是；MC 26.3 8 个存储、441.5 MiB） | 路线图推完后的性能工作 |
| P12 未做：DirectGLES `g_Display` / `g_Surface` / `g_Context` 仍是全局（每进程一个会话时不需要）；cached-app freezer 的完整处理（文档从未定义）；多 context；server 窗口不转发输入 | 等出现第二个会话 / 第二个 context 的需求 |
| P9 转出：Magma 每次 draw 对可写 SSBO / texel buffer / XFB 目标发整 buffer 的 `OnGpuWritten`（create-indirect 首帧 7,093 条），client 已自标同一批，冗余无害；`OnMipLevelsGenerated`、`OnCapsInvalidated` 仍无生产者。`OnTextureWriteback` 也无生产者，但别删：CPU 三通道 mip 退路要它（`CONTRACT-P5B.md:255`） | 以后的收窄 / 清理候选（ID-P9-7） |
| 树外脚本：普查跑器、`wsl_p5_gate.sh` 等在 `~/w7/notes/`，git merge 不会传播；`~/w7/notes/p11/gate.sh` 是事实上的门 | 需要时入库 |

已关闭的历史债（P5 的 27 个 wrong-answer、`rsp` 残余读、P5b 的 inproc 依赖、ABI 指纹、默认 staging 装不下 128 MiB 上传、184 符号棘轮 CI 门）及当时的完整债务表见 [`notes/p5b/README.md`](p5b/README.md) 末节。
