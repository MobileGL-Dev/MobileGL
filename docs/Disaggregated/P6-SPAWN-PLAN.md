# P6 spawn transport — 包计划

> 历史规划基线 `feat/disaggregated @ f23fbc1b`。路径在 `MobileGL/` 下，旧行号在 `c6` 落地时重核。
> 契约草稿 [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md)，`c6` 落地时移为 `MobileGL/MG_Remote/CONTRACT-P6.md`。
> **P5f 前提全部完成**（2026-09-20）：零 `BARRIER_PULLED` 分类、双块 / strict 门、静态状态世代与生命周期、registry / 反向通道边界均已收口；阶段审查修复及最终 Redmi 六个 clean-boot 臂也已通过。见 [`P5F-WIRE-COMPLETENESS.md`](P5F-WIRE-COMPLETENESS.md)、[收口审查](notes/p5f/close-review.md)和[设备报告](notes/p5f/device-report.md)。
>
> **P6 尚未开工：`a6`、`c6` 与 spawn 实现均未启动。** 下文保留历史包规划；标 `待 a6` 的 P6 进程边界事项仍需审计确认，P5f 完成不等于 P6 已实现。

---

## 1 树上已有的（不要重写）

| 件 | 位置 | 测试 |
|---|---|---|
| `ITransport`（含 `ShareFd`/`ReceiveFd`） | `MG_Remote/Transport/ITransport.h` | `InProcessTransportTest` |
| `SocketDoorbell` | `Transport/Doorbell.h:385-420`；`Doorbell.cpp:211-360` | `MG_Test/Wire/FdPassingTest.cpp:197`/`:227`/`:260`/`:289` |
| SCM_RIGHTS fd 传递（专用 `SOCK_DGRAM` socketpair） | `Transport/FdPassing.cpp:92` | `FdPassingTest.cpp:267`/`:291` |
| `ShmSegment::Adopt`（POSIX） | `Transport/ShmSegment.h:57`；`ShmSegmentPosix.cpp:120` | `FdPassingTest.cpp:114`/`:187` |
| inproc attach 是 dup+Adopt+Map 的真实第二份映射 | `Transport/SessionRings.h:22-28`、`:147-159` | 现有 split 车道 |
| 控制面 schema：`SurfaceOp`/`SurfaceReply`/`WindowKind`/`CtrlEnvelope` | `Protocol/protocol.fbs:166-290` | `ProtocolSmokeTest`、`flatc-check` |
| ABI 指纹握手（同机同二进制，原样继承） | `CapsCodec.h:23-29` | `SessionHandshakeTest.cpp:115` |
| `spawn`/`unix:`/`pipe:` 具名拒绝 | `ConfigLoader.cpp:310-348` | — |
| `MOBILEGL_IPC_SERVER_PATH` 已解析未消费 | `Config.h` `IpcTable::ServerPath` | — |

传输原语已付过账。P6 是装配 + 控制面 + 进程。

## 2 缺什么

1. `SocketTransport`。`ClientSession.cpp:468` 目前硬拒非 `InProcess`。
2. `ServerMain`。今天的 `MobileGLServer`（`CMakeLists.txt:923`）是 P0 spike 桩（`tools/spikes/server_stub/main.cpp`），Android-only、默认 OFF。
3. 进程机制：fork/execve；envp 剔除 + 子进程禁止递归选择 client transport（双保险）；`dladdr` 兜底；有界重试握手；EOF 即退；不留孤儿。原“子进程强制 monolith”的具体实现**待 a6**：P5f 的 server record 臂依赖 active transport 选路，必须区分反递归与 backend server 角色选择，不能因此回到 frontend glue。
4. 控制面帧。**P5f 包 fc 已把帧化本身落地**：forwarder 改发 `SurfaceControlFrame` 值帧（`MG_Remote/Server/SurfaceControlFrame.h`），schema 缺口（3 枚枚举 + `readSurface`/`context` 字段 + `WindowKind::MetalLayer`）已按 append-only 补齐，wire 编解码与具名拒绝在 `MG_Remote/Protocol/SurfaceOpCodec.cpp`（`ServerApplyWireSurfaceOp`）。P6 的 `cp` 只剩传输：client 侧 `EncodeSurfaceOpFrame` 上 socket、server 泵进 `ServerApplyWireSurfaceOp`、`SurfaceReply` 回程与 §4 的死亡接线。P5c 审计 G4 行，原记 P6。
5. 静态状态 / 生命周期的 spawn 验证，**不是重做 P5f fs**。unpack / render shadow 已按 native generation、served-context serial、执行臂组成的 `ContextEpoch` 失效；raw-depth sampler 按 native generation 重建。XFB 按 server lifetime id 隔离，served-context reset 保留仍活着的 paused/pending span，native context 销毁才清对象；server liveness 由控制生命周期持有。`st` / `a6` 核这些语义在第二进程与 socket 接线下仍成立，见草稿 §6 与 [fs 报告](notes/p5f/fs-report.md)。
6. 死亡语义，见 §4。

## 3 边界：P6 不做真窗口

`WindowHandle::Handle`（`MG_Backend/BackendObject.h:561-566`）在 Android 上是 `ANativeWindow*`，子进程用不了。`protocol.fbs:186-196` 的 `nativeToken` 注释已写明 Android 的窗口 out of band 传递——那是 P12。

- P6 落 pbuffer / surfaceless / 离屏：够 `HeadlessGL`、等价 split 车道、trace replay，也正是 P6 出口门量的东西。
- 真窗口的管道（令牌、`SurfaceOp`、`SurfaceReply`）落，但到达 server 时具名拒绝。
- **P6 新增的手机 spawn 臂只承诺 trace 和离屏；真窗口游戏接入归 P12。** 现有 monolith / inproc 运行能力不因此改变。

## 4 P5e 留下的：不等待的客户端 + 会死的服务端

闩锁语义已定（`docs/Disaggregated/ARCHITECTURE.md:504`）：EOF → device-lost latch、`EGL_CONTEXT_LOST`、`GL_UNKNOWN_CONTEXT_RESET`；`MOBILEGL_IPC_RESPAWN` 默认关；`IDLE_EXIT_S` 默认 30。照抄。

新的是 run-ahead 之后的三个状态：

| 情形 | 要求 |
|---|---|
| server 被 kill，客户端已跑在前面 | 走上述闩锁；已发布未 apply 的记录明确丢弃，不等待 |
| server 被 SIGSTOP / Android freezer 冻住（**不产生挂断事件**） | 有界等待 + 具名诊断 |
| server 活着但落后一帧以上 | 这是 P5e 的正常态，必须与上两者可区分 |

死亡判定取 `SocketDoorbell` 在 EOF 上 latch 的 `m_dead`（`Doorbell.cpp` 的 `Drain`），**不用超时阈值**。这是 P6 唯一的新设计，其余是搬运。它与 E1 对照的重新定义（ID-122）共用同一个谓词，建议一起做。

## 5 包与依赖序

```
a6  (只读审计，不写代码)
 └─ c6  (契约 + 惰性线上行，行为零变化)
     ├─ so  (SocketTransport)
     ├─ sm  (ServerMain + fork/exec + 进程纪律)
     ├─ cp  (已有控制帧的 socket 传输 + 回程)
     ├─ st  (验证 P5f epoch / lifetime 的 spawn 接线 + G5)
     └─ t6  (spawn 车道 + 阴性对照 + CI)
 └─ 集成 commit：ConfigLoader 不再拒绝 spawn
```

**a6**（先跑，尚未启动）。以 P5f 已完成的双块、身份守卫、生命周期、反向通道及设备证据为输入，核验从同进程双角色变为真实进程后的新增边界，不重复领取已完成的迁移。产出：逐行清单，形状照 `notes/p5c/p5c-audit-v1.md`。点名查：

1. 仍过线的裸指针 / 进程局部句柄。
2. **进程级静态中语义属于 context 或 session 的**——以 `notes/p5f/f0-statics.md` 与 fs 全清单处置为基线，验证 §2.5 的 epoch / lifetime 规则与新 process/session 退出边界；不可把每次 `applier_reset` 解释为清空所有对象。
3. `gPipeInputs` 在 spawn 下的角色归属，以及禁止子进程递归当 client 时仍选中 server record 臂的机制。
4. 复用 fc 已补齐的 `SetSwapInterval` / `ReleaseResources` / `SetWindowHandle` 等 schema 与 wire codec，核 socket 对接与 inproc-only op 的边界（`SwapEGLBuffers` 走记录、`InitCapabilities` 走 `CapsSnapshot`、`InitWindowSurface` 是客户端 no-op），不重复追加已落地枚举。
5. server 能否不链 `MG_Impl`（`MG_Backend/MGPipe/PipeInputs.h` 如此声称）。做不到就记账。

**c6**：契约；复用 fc schema，只有 a6 确认的新缺口才 append-only 扩展；令牌类型；新旋钮（`ARCHITECTURE.md:639` 已登记的 P6+ 项，`POLL_ESCALATE` 归 P10）；跨包接缝的声明 + 未落地断言体；`MobileGLServer` 真目标。

**so**：`SocketTransport`。**必须在第二个进程出现之前就绿**——一次 `socketpair()` + 两个线程跑 `InProcessTransportTest` 全套，含 `BUFFER_TOO_SMALL` 留帧语义。理由同 `SessionRings.h:22-28`。

**sm / cp / st / t6**：见 §2 与 §3。`cp` 在 fc 之后只剩传输；`ForgetCurrentTuple` 的 N-3 语义已在帧路径的分发里原样成立（`ApplySurfaceControlFrame`，ServerLoopTest 的 C7/N-3 对照驱动）。

## 6 出口门

五部分门（`ARCHITECTURE.md` §13.2）之外：

1. G1 pull 构建 0/0/0/0，`.text` 不变。
2. **G2/G14**：`integration-spawn` 用例名集合与 `integration-split` 逐名相同。
3. `integration-spawn` 全绿，数字与 `integration-split` 一致（历史草稿基线 179/179；`c6` 重核实际发现 / 执行集合）。
4. **进程树门（机检）**：每条用例前后数子进程，运行中恰好多一个，运行后为零；`HeadlessGL` fork 预检无孤儿。
5. **臂证明门**：每条用例在自己的私有日志里留下子进程 pid 与 `transport=spawn`。ID-124 已证明假绿会发生，而一条实际跑了 monolith 的 spawn 车道能通过 1–4。
6. 逐包 red-once（R-16），各一条具名对。
7. 设备：Redmi `2f7cbe2e`，reboot-clean + 同热窗口配对 A/B；OpenRA 在 Adreno 830 上 split SSIM ≥ 0.99。
8. 性能只记录不阻塞，但必须采到一个数：**socket 门铃相对 inproc condvar 的代价**。P5d 把 client 线程从 15.0 压到 9.2 ms/帧，其中有门铃路径的钱。P6 唯一预期的回归。

## 7 阴性对照

| # | 旋钮 | 证伪什么 |
|---|---|---|
| S1 | `MOBILEGL_IPC_SERVER_PATH=/nonexistent` | 具名拒绝，非静默回落 monolith |
| S2 | 运行中 `kill -9` server | 客户端能区分慢与死；不挂起、不继续提交 |
| S3 | 子进程 envp 未剔除 | 第二道保险挡住递归 spawn 并具名 |
| S4 | 临时关闭 spawn 接线中继承的 P5f epoch 失效 | 替换 context 错用旧同步状态；返回仍活着的 XFB lifetime 必须保留 paused span |
| S5 | `WindowKind::AndroidNativeWindow` 到达 server | 具名 Fatal |

S2 的形状先想清楚再写：E1 索要的 Fatal 在整次运行里出现 0 次（ID-122）。

## 8 风险与未决

1. 慢 / 死的区分（§4）。P6 唯一新设计，与 E1 重定义合并考虑。
2. Android cached-app freezer 冻结不产生挂断事件。完整处理归 P12，诊断归 P6。
3. server 能否不链 `MG_Impl` — 待 a6；做不到即记账，不留空头承诺。
4. Windows：`Adopt` 是 POSIX-only，`unix:` / `pipe:` 继续具名拒绝，P6 只落 POSIX。
5. `SEG_REPLY` 2 MiB 单槽 cap 与 chunking（`Transport/ReplySlot.h` 记作 P6 debt，ID-47）→ **推给 P9**。
6. class-C 的 511 条 `UnmigratedVerb`（`notes/p6/census-classC.md`）不是 P6 的账。
7. E1 对照（ID-122）不因 P6 开工而解决。

## 9 不属于 P6

真窗口与 `android:process=":mgl"` Service（P12）；多 context（P12，`CONTRACT-P5C.md:56` 原写 P6，本计划改判）；chunked readback 与 reply-slot 池（P9）；`DynamicBackendParameters` 定宽重写及已具名拒绝的功能债（P7）；`POLL_ESCALATE`（P10）；剩余 monolith-only frontend-object / twin-registry glue 清理（P3b/P4b）。`BARRIER_PULLED` 分类与 transport 前端身份访问已由 P5f 归零，不能继续列成未偿的 P6 前提债。

## 10 开工顺序

先跑 `a6`，只读。清单出来之前 `c6` 写不实，后面五包的大小是猜的。
