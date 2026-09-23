# P6.5 第一波 代码审查（4 切片，2026-09-22）

审查对象：`codex/p65-all-tcp` HEAD `018fab0f` + 未提交工作树（含 archive v2 / Present-glFlush 尾批 / 取消 / half-close 修复）。
四个并行 Opus 审查各读设计（ROADMAP §51–116）后逐文件核验。**结论：数据面、wire 布局、握手三块无硬正确性缺陷；实现整体谨慎、扎实。** 下表按严重度排序，两项已修，其余记为跟进。

| 严重度 | 发现 | 出处 | 处置 |
|---|---|---|---|
| **Medium** | `AcceptPair` 把可重试的 `accept()` 错误（`EINTR` / `ECONNABORTED`）当致命 → supervisor 映射为 `return 73`，整台常驻 server 退出。LAN 对端在 accept 窗口内 reset、或信号（本进程按构造收 SIGCHLD）打断 `accept()` 都会拆掉 server；与兄弟函数 `WaitReadable` 刻意重试 `EINTR` 自相矛盾。潜伏——设备 102 条顺序会话已过，未触发。 | `SocketTransport.cpp` AcceptPair | **已修**：`EINTR`/`ECONNABORTED` 在 budget 内重新 poll，不返回 `TRANSPORT_CLOSED` |
| **Moderate** | `MobileGLServerService.onDestroy()` 只 `destroy()` supervisor PID；在飞的 fork 会话 child（无新 session/group）被 reparent 到 init，泄漏进程 + EGL 上下文。仅 `am force-stop`（按 UID 杀）能清。验收走 force-stop、CI 走 killpg，故门路径干净；属 Android 生命周期 / P12。 | `MobileGLServerService.java:96-102`, `ServerMain.cpp:292` | 跟进（P12 生命周期）；本波验收不受影响 |
| Low | supervisor 强检 `MOBILEGL_IPC_DIAL=no` 却不设/不检 `MOBILEGL_IPC_ROLE`；launcher 漏设会把 supervisor 主线程 server 日志误记为 client 且不前送。潜伏（四个 launcher 都设了）。 | `ServerMain.cpp` | **已修**：`mobilegl_server_main` 顶部自设 `MOBILEGL_IPC_ROLE=server`（首日志前，fork 继承） |
| Low | 设计要求的裸 `char` 符号性普查未在 `gen_pipe.py` 实现；成员摘要抓不到（`sizeof`/`offsetof` 两 ABI 相同）。**当前无实活风险**——所有 wire 子-int 成员都是定符号 `Int8`/`Uint8`。 | `scripts/gen_pipe.py` | 跟进：加 census 守卫 |
| Low | `DynamicBackendParameters` / `RenderStateParameters` 只按整体 `sizeof` + 分块边界 offset 入指纹，非逐成员。安全——成员全定宽（无 `long`/`size_t`/指针/位域）。 | `MGPipeRenderStateSpans.h`, `CapsCodec.cpp` | 跟进（可选）：逐成员 offset 入摘要 |
| Low | client `eventRingFull` 在 drain 后可能被在飞 `ProgressFrame` 短暂重置；自愈（下个 progress 带 `full=0`，1ms timer 兜底），非挂起。 | `StreamLink.cpp:293` | 跟进：加 latch-only 注释/守卫 |
| Low | `StreamLink` 的 `RecordArena/RecordCursor/EventArena/EventCursor` attach 时冻结 `LocalHead=0`，与 ILink 性能契约不符；当前无 cpp 消费者，无害。 | `StreamLink.cpp:342-345,399-409` | 跟进：随首个消费者刷新或删访问器 |
| 测试缺口 | 无测试断言 keepalive 调优值（KEEPIDLE/INTVL/CNT/USER_TIMEOUT）实际生效；`StreamLinkTest` 不强制跨 recv 边界的帧头拆分，且漏 Cmd 越窗 / reply-seq 倒退 / ProgressFrame 一致性三条 Fatal。 | `SocketTransportTest.cpp`, `StreamLinkTest.cpp` | 跟进：补断言 |

## 已核验干净（各切片焦点）

- 数据面：帧重组 / 短读 / iov 回绕 / EOF；水位单调 + late-never-early 接收侧强制（倒退 `Fatal{ProtocolCorruption}`）；单邮箱 reply seq 匹配（applied 越过无 reply → `PROTOCOL_MISMATCH`）；park 前 `Flush` + flush-on-idle 进度；event 背压（`head − 对端 tail > cap` 阻塞）；`ResolveSpan` 指向持久镜像段（非瞬态重组 buffer，无悬垂）；单 reader vs apply vs encode 线程安全；EOF→`Die(peer)`→`PeerHungUp`→device-lost 闩（clean detach 不误闩）。
- 控制面：`tcp://` 解析（IPv6 `[h]:p`、缺/坏端口、port 0/>65535 全拒）；四个 socket 选项全设且查返回值，控制与数据连接都设 keepalive；掉线经 keepalive/USER_TIMEOUT(~5s)→挂断（非 120s barrier）；握手零 abort（每条拒绝走具名 `Refuse`，server `_exit(0)`，client 不误回落 monolith）；token loopback-only-when-no-token；指纹策略按 Dial（Fork 拒 / Connect 警告 / REQUIRE_SAME_BUILD 拒）；fd 泄漏各失败分支已闭。
- wire：`gen_pipe.py` 生成与 `PipeFields.def` 同步由 CI `git diff --exit-code` 守；指纹成员齐、删 `GLFunctionsTable`；两端同一 endian-无关计算（非结构体 memcpy 哈希）；定宽确认。
- server：fork 循环 fd 卫生（child 关 listen、parent 关 accepted、无跨迭代泄漏）；僵尸回收 + max-sessions=1 + 第二连接 `Refuse{Busy}` 且干净关闭；child 得 `DIAL=no`；日志前送在 `LogMutex` 下 sink+forward、`LogFlush` 控制线程应答不经 apply 线程（无死锁）；exec/argv-env；INTERNET 权限；wake lock 平衡。

## 已应用修复（本次）

1. `SocketTransport::AcceptPair`：`EINTR`/`ECONNABORTED` 在 budget 内重新 poll，不再 `TRANSPORT_CLOSED`。防常驻 server 因 accept 瞬态整体退出。
2. `mobilegl_server_main`：顶部自设 `MOBILEGL_IPC_ROLE=server`（若未设），不再盲信 launcher。

两处均在 `MG_Remote/`（`MOBILEGL_BUILD_DISAGGREGATED` 门内），故 G1 monolith `.text` 不受影响（构造性保证）。

## 修后再验收（固定制品，2026-09-22）

固定制品：WSL client `libMobileGL.so` SHA16 `e17f82f1`、设备 APK SHA256 `780bb00e…`（`assembleTraceDebug`，`-O0 -g`），**两端 build stamp 同为 `p65-acceptfix-20260922`**（同源同戳，`REQUIRE_SAME_BUILD=1` 真实通过）。

| 检查 | 结果 | 证据 |
|---|---|---|
| G1 | monolith `.text` 不变（构造性：改动全在 `MG_Remote/`，非 split 构建不编译） | CMakeLists §497 门 |
| 主机 full unit | 2399/2399，0 fail | `/tmp/p65-fullunit2.log` |
| P6.5 子集单测 | 147/147（含 wf/ct red-once：同宽字段重排改摘要、wireMajor/layout 不匹配→具名 Refuse 不 abort、Connect 接不同 build、Fork 拒、REQUIRE_SAME_BUILD 拒） | ctest -R 输出 |
| integration-tcp **loopback**（Release 固定制品） | 102/102（一条 `ClientVertexArray…GpuWrittenIndices…` 会话槽竞态 flake，单独重跑绿——ci-false 类） | `/tmp/p65-tcp-loopback-fix2.log` |
| **device-102** integration（Redmi，固定 APK） | **102/102，0 fail、0 Fatal、0 eglInit 失败；102/102 client 日志 `run-ahead ARMED`** | `/tmp/p65-devlane-fix2.log` |
| 代表性 device retrace（Redmi，固定制品，`--require-run-ahead`） | OpenRA ssim=1.0 / startup 0.999999511 / in-world 0.999995438，**三条全 ARMED**（非 lockstep/disarmed） | `/home/swung/p65-repr-device/checkpoint.json` |

排障留痕（勿重犯）：restamp 后只 `ninja libMobileGL.so MobileGLServer` 的**局部构建**会让 `MobileGLIntegrationTest` 与生产库不一致，服务端报 `Fatal{ProtocolCorruption, PipeApplier::Attach missing link}`、客户端 `eglInitialize 0x3001 remote TCP bring-up failed`——**全量 `ninja` 后即消失**，不是 wave 缺陷也不是 debug/release 差异。设备车道无 wake keepalive 会因 Doze 冻结全挂，retrace 用 `--wake-adb-serial` 已规避，device-102 补了 `KEYCODE_WAKEUP` 循环。

代表性 scope 依用户偏好（本地代表性、CI 兜底全量）；此前另有一条完整 39 例 device 矩阵在跑，14 passed / 1 ci-false（`minecraft-1.21.11-main-menu` 的 Redmi 32B UBO 对齐 golden 伪差），因与固定制品身份不一致且 APK 需重装而停，其部分结果 `p65-final-matrix-gles*` 作旁证保留。

临时环境已恢复：设备 server 停、idle 白名单还原、WSL→手机 host route 删除（adb forward 与本地 fixture 早已清）。设备现装固定 APK `780bb00e`（含本次两修复）。
