# P6 spawn transport — 包计划

> 基线：`feat/disaggregated @ f23fbc1b`（P5e 收官头）。路径均在 `MobileGL/` 下，行号以该头为准。
> 契约草稿见 [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md)；它在 `c6` 落地时移为
> `MobileGL/MG_Remote/CONTRACT-P6.md`，与四份同胞契约并列。
>
> **本文件只是计划，尚未实现任何一行。** 它也写在自己的审计（`a6`）之前：
> §4 里凡标为待 `a6` 确认的条目，都是现在还不该下结论的。

---

## 0 一句话

ROADMAP 说"P6 此时只是传输替换"。这句话是 P5c 花了整整一个阶段去**换来**的，不是可以直接
引用的前提——所以 P6 的第一个包是**重新证明它**，而不是相信它。证明之后，P6 要造的东西比想象
的少：传输原语（socketpair、SCM_RIGHTS、shm adopt、socket 门铃）**已经在树上并且有测试**，
缺的是**进程**、**控制面**，以及 P5e 刚刚引入的一个新问题——run-ahead 的客户端不再等待，而
另一个进程**会死**。

---

## 1 起点：已经在树上的东西

这一节存在的理由是防止 P6 重写已经绿了的东西。每一行都在基线头上核过。

| 件 | 状态 | 位置 | 已有测试 |
|---|---|---|---|
| `ITransport`：`SendFrame`/`ReceiveFrame`/`PeekFrameSize`/`ShareFd`/`ReceiveFd`/`Shutdown` | 接口完整，`ShareFd` 是一等成员而非后补 | `MG_Remote/Transport/ITransport.h` | `InProcessTransportTest` 全套 |
| `SocketDoorbell`（spawn 的门铃） | **已实现** | 声明 `Transport/Doorbell.h:385-420`；实现 `Doorbell.cpp:211-360` | `FdPassingTest.cpp:197/227/260/289` 四例：唤醒、超时+早唤醒记忆、对端挂断停止 park、挂断前最后一次 ring 仍送达 |
| SCM_RIGHTS fd 传递 | **已实现**（专用 `AF_UNIX SOCK_DGRAM` socketpair，不是控制 socket） | `Transport/FdPassing.{h,cpp}`（`socketpair` 在 `FdPassing.cpp:92`） | `FdPassingTest.cpp:267/291` |
| `ShmSegment::Adopt(fd, size)` | **已实现**，POSIX only | `Transport/ShmSegment.h:57`；`ShmSegmentPosix.cpp:120` | `FdPassingTest.cpp:114/187` |
| inproc 也走 `ShmSegment`，attach 是 dup+Adopt+Map 的**真实第二份映射**而非别名 | 故意如此，正是为了 P6 当天不是第一次跑 | `Transport/SessionRings.h:22-28, 147-159` | 现有 split 车道每次运行 |
| 控制面消息表：`Hello`/`Welcome`/`CapsSnapshot`/`SurfaceOp`/`SurfaceReply`/`ResyncRequest`/`AuxRequest`/`Fatal`/`LogLine` + `CtrlEnvelope` union | **schema 已定**，含 `SurfaceOpKind` 七个枚举、`WindowKind` 五个 | `Protocol/protocol.fbs:166-290` | `ProtocolSmokeTest`、`flatc-check` |
| ABI 指纹握手（`sizeof(DynamicBackendParameters)`/`sizeof(MGPCaps)`/构建指纹，不符即 `Fatal{AbiMismatch}`） | 已实现；`CapsCodec.h:23-29` 明说 **P6 的 spawn 同机同二进制，原样继承** | `CapsCodec.{h,cpp}` | `SessionHandshakeTest.cpp:115` |
| `FatalCode` 里 `DeviceLost=4`、`ServerCrashed=5` | 枚举已留位 | `protocol.fbs:234-242` | — |
| `MOBILEGL_TRANSPORT=spawn\|unix:\|pipe:` 被**具名拒绝**而不是静默回落 | 已实现，且注释点名了它防的是哪种事故 | `ConfigLoader.cpp:310-348` | — |
| `MOBILEGL_IPC_SERVER_PATH` 已解析、未消费 | 故意：未解析的环境变量与"解析了又忽略"无法区分 | `Config.h` `IpcTable::ServerPath` | ctest `ENVIRONMENT` 块已携带 |

**结论**：传输层的难点（lost-wakeup 的双 fence、挂断检测、fd 生命周期、映射对齐）**都已经付过
账了**。P6 不是"写一个 socket 传输"，是"把已经绿的零件装进一个进程里，并造出控制面"。

---

## 2 P6 真正要造的东西

按"这件东西今天完全不存在"排序：

1. **`SocketTransport`** —— `ITransport` 在 socketpair 上的实现。`Framing.h` 已有分帧，
   `FdPassing` 已有 fd 传递，`SocketDoorbell` 已有门铃。这个类是把三者接起来，加上
   `ReceiveFrame` 的 `BUFFER_TOO_SMALL`**留帧**语义（`ITransport.h` 明确这是接口存在的全部
   意义，早先的分支正是在这里丢帧把流永久卡死的）。
   `ClientSession::Start` 目前在 `ClientSession.cpp:468` 硬拒非 `InProcess`。
2. **`ServerMain`** —— 子进程入口。今天的 `MobileGLServer`（`CMakeLists.txt:923`）是 **P0 的
   spike 桩**（`tools/spikes/server_stub/main.cpp`），Android-only、默认 OFF，只写一个 marker。
   P6 要一个真的：解析继承来的 fd、adopt 四个段、建后端、跑 `ServerLoop`、EOF 即退。
3. **进程机制** —— `fork`+`execve`；**envp 剔除**（子进程必须看不见 `MOBILEGL_TRANSPORT`，
   否则递归 spawn）+ **子进程内强制 monolith** 的双保险；`MOBILEGL_IPC_SERVER_PATH` 找不到时
   `dladdr` 兜底定位同目录的 `libMobileGLServer.so`；**有界重试**握手（不是无限重试）；
   不留孤儿（`waitpid` / `PR_SET_PDEATHSIG`）。
4. **控制面帧** —— 十二个 `Server*` EGL forwarder（`ServerLoop.cpp:802-1022`）今天经
   `ServerLoop` 的**单槽邮箱**传 **函数指针 + 栈上 `void*`**（`ServerLoop.cpp:533-568`、
   `RunOnApplyThread` `:645`）。这是 P5c 审计的 G4 行，明确记给 P6。跨进程它们必须变成帧。
5. **`s_synced` / `g_syncedRenderStateParameters` 按 context 世代重置** —— P5c 明确留给 P6
   （`CONTRACT-P5C.md:538-539`）。`DirectGLES.cpp:4586` 的 `g_syncedRenderStateParameters` 与
   `ScopedDefaultUnpackState::s_synced` 是**进程级静态**，语义上却是 per-context。在同进程里
   它们碰巧跟着进程走；在一个会被复用的 server 进程里，它们会把上一个 context 的同步状态
   带给下一个。
6. **死亡语义** —— 见 §3.2，这是 P5e 留下的新债。

---

## 3 两个必须先决策的设计问题

### 3.1 窗口与 EGL 句柄：P6 和 P12 的分界线

`ServerCreateEGLWindowSurface(EGLSurface, const WindowHandle&)` 的 `WindowHandle::Handle` 是
`void*`（`MG_Backend/BackendObject.h:561-566`），在 Android 上是 `ANativeWindow*`。
`EGLDisplay`/`EGLSurface`/`EGLContext` 同理，全是**进程局部句柄**。子进程拿到这些值一个都
用不了。

schema 已经给了答案，而且是对的：`SurfaceOp` 用 `display`/`surface` 两个 `ulong` **令牌**，
`nativeToken` 的注释写着 *"X11 XID / HWND; Android transfers the window out of band"*
（`protocol.fbs:186-196`）。那个 out of band 就是 **P12**（`android:process=":mgl"` 的
Service 收 Java `Surface`）。

**所以 P6 的诚实边界是：**

- P6 落 **pbuffer / surfaceless / 离屏**路径，够跑 `HeadlessGL`、整条 `integration-split`
  等价车道、以及 trace replay（OpenRA 的 SSIM 门就是这条）。
- P6 落**真窗口的全部管道**（令牌、`SurfaceOp`、`SurfaceReply`），但
  `WindowKind::AndroidNativeWindow` 到达 server 时是**具名拒绝**（`Fatal{UnmigratedSurface,
  "AndroidNativeWindow@P12"}`），不是崩溃、不是静默黑屏。
- 真窗口的到达是 **P12**。ROADMAP 两阶段的门本来就是这么分的（P6 的门是 OpenRA SSIM，P12 的
  门才是"Minecraft 经 FCL 入世界"）。

**把这条写进契约，因为它最容易被误解成"P6 之后就能在手机上跑游戏了"。** 不能。P6 之后能在
手机上跑的是 trace 和离屏场景。

### 3.2 P5e 给 P6 留下的新问题：run-ahead 的客户端 + 会死的服务端

**先说已经设计好的部分**，免得重复发明：`ARCHITECTURE.md:504` 已经定死了死亡语义——
server 死 → client 读到 EOF/EPIPE → device-lost 闩锁（GL 调用 no-op、`eglSwapBuffers` 返回
`EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus` 返回 `GL_UNKNOWN_CONTEXT_RESET`）；
`MOBILEGL_IPC_RESPAWN=1` 重启并全量重推（默认关）；client 死 → server 读到 EOF 立即销毁原生
context 并退出，`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作最后保险。P6+ 的旋钮清单在
`ARCHITECTURE.md:639`。**这些照抄即可。**

**新的是 P5e 之后的交互**，它写于 run-ahead 之前，所以没有任何现成文档记着它。

lockstep 时代，apply 线程死掉就是进程 abort——客户端不可能"继续画"。P5e 之后：
- 客户端发了就走，不等 apply；
- present credit（默认 1）是唯一的节流。

于是跨进程后出现三个**新的**状态：

| 情形 | 今天会发生什么 | P6 必须做到 |
|---|---|---|
| server 进程被 kill，而客户端已经跑在前面若干条记录 | `SocketDoorbell` 的挂断检测会把等待者唤醒（已实现，`Doorbell.cpp` 的 `Drain` 在 EOF 上 latch `m_dead`）；但**上层没有闩锁**，而且 run-ahead 的客户端此刻手里有一批**已发布、永远不会被 apply** 的记录 | `ARCHITECTURE.md:504` 的闩锁语义，**永不挂起**、不继续提交；已发布未 apply 的记录被明确丢弃而不是等待 |
| server 被 SIGSTOP / Android **cached-app freezer** 冻住 | 客户端停在 credit 上，看起来像挂死，且**不会**有挂断事件 | 带上限的等待 + 具名诊断；不能靠"它总会回来" |
| server 活着但 apply 落后一整帧以上 | 这就是 P5e 想要的正常状态 | 必须与上面两种**可区分**——所以死亡判定不能用超时阈值 |

**闩锁本身是搬 `ARCHITECTURE.md:504`；新设计的是"慢与死如何区分"这一条。**
它也正好是 E1 对照（ID-122，P5e 唯一未决项）
应该被重新定义成的东西的邻居：一个真正能区分"慢"和"死"的判据。两件事建议一起想。

---

## 4 包与依赖序

```
a6  (审计，只读，不写代码)                         -- 先跑，决定后面几包的大小
 └─ c6  (契约 + 惰性线上行 + 令牌类型 + 知识桩)     -- 第二个落，行为零变化
     ├─ so  (SocketTransport)                  ┐
     ├─ sm  (ServerMain + fork/exec + 进程纪律)  │  并行 worktree，各自 rebase 在 c6 上
     ├─ cp  (控制面：12 个 forwarder → 帧)       │
     ├─ st  (静态量按 context 世代重置 + G5)      │
     └─ t6  (spawn 车道 + 阴性对照 + CI)         ┘
 └─ 集成 commit：ConfigLoader 不再拒绝 spawn
```

### a6 —— 审计（整合者，只读，**第一个**）

**为什么它是第一个**：P5c 存在的全部理由，是一次只读审计在一个"看起来已经很干净"的头上
找出了 **59 个**不经任何载体的直接内存读写。P5e 之后树又变了：多了 by-handle resolver、
多了 server 角色内存（`gPipeInputs`）、多了 run-ahead 这条不等待的路径。**"P6 只是传输替换"
这句话现在的证据是 P5c 时代的，已经过期。**

**产出**（对照 `~/w7/notes/p5c/p5c-audit-v1.md` 的形状）：逐行清单，每行给出
`file:line`（在 P6 基线头上重新解析）、方向、今天为何能工作、现有守卫、P6 的载体或**具名延期**。

**必须点名去查的**：
1. 任何仍然过线的裸指针 / 进程局部句柄（`EGLDisplay`/`EGLSurface`/`EGLContext`/`ANativeWindow*`/
   `void*`）。
2. **进程级静态**中语义上属于 session 或 context 的：`g_syncedRenderStateParameters`、
   `s_synced` 是已知的两个，问题是**还有几个**。这类东西 inproc 永远看不见。
3. `gPipeInputs` 在 spawn 下的归属（P5e 把它变成 server 角色内存，ID-135 证明值由
   `SuppliedFieldMask` 分区、竞态在元数据标量上——跨进程后这个结论**要重新验**）。
4. 十二个 forwarder 的参数形状逐个对照 `SurfaceOpKind` 的七个枚举，给出**缺几个**。
   预期答案（待 a6 确认）：`SwapEGLBuffers` 作为记录走（P5e 已裁定，`BackendObject_Remote.cpp`
   的 `WaitForApplyBeforeEglForwarder` 注释明说它不在名单上）、`InitCapabilities` 走
   `CapsSnapshot`、`InitWindowSurface` 是客户端 no-op，剩下九个里七个有枚举，
   **`SetEGLSwapInterval` / `ReleaseEGLResources` / `SetWindowHandle` 需要新枚举**（union
   与 enum 只许追加）。
5. server 端是否真的可以不链 `MG_Impl`。`MGPipe/PipeInputs.h` 里写着 "P6, where MG_Impl is
   not in the [server]"。如果 P6 做不到，**说出来并记成 P13 的账**，不要让一句注释替未来
   的自己许诺。

**a6 不写代码。** 它的输出是 c6 的输入。

### c6 —— 契约与线上行（整合者，第二个落，行为零变化）

`MG_Remote/CONTRACT-P6.md`；`protocol.fbs` 追加缺的 `SurfaceOpKind` 枚举（只追加）；
令牌类型（`SurfaceToken`/`ContextToken`/`DisplayToken`，客户端铸造、server 侧映射到自己的
真句柄）；`Config.h` 的新旋钮（`ARCHITECTURE.md:639` 已登记为 P6+ 生效的 `POLL_ESCALATE` / shadow shm /
`RESPAWN` / `IDLE_EXIT_S`，其中 `POLL_ESCALATE` 归 P10 —— **在这里加，不在调用点发明**；
`ARCHITECTURE.md:639` 同时明确**不设立** `MOBILEGL_IPC_VALIDATE_SERVER`，因为 server 没有
`MG_Impl` 校验器，这与 §7.3 是同一个问题的两面）；每个跨包接缝的声明 + "未落地"断言体，
让所有包从第一天起可编译；`CMakeLists.txt` 的 `MobileGLServer` 真目标（与 P0 spike 并存或
取而代之，a6 给结论）。

**门**：unit 绿；`integration-split` 绿（什么都没变）；G1 0/0/0/0。

### so —— `SocketTransport`

**关键纪律，照 `SessionRings.h:22-28` 的先例**：`SocketTransport` **必须在第二个进程出现之前
就绿**。用一次 `socketpair()` + 两个线程，跑 `InProcessTransportTest` 的同一套断言
（`BUFFER_TOO_SMALL` 留帧、`Shutdown` 双向半关、排队消息仍可读尽、`PROTOCOL_MISMATCH` 永久
latch）。**一个只在 fork 之后才第一次被执行的传输实现，是这个阶段最该避免的形状。**

### sm —— `ServerMain` 与进程纪律

fork/execve；envp 剔除 + 子进程强制 monolith；`dladdr` 兜底；有界重试握手；EOF 即退；
无孤儿。**进程树必须可机检**：不是"看上去没多余进程"，而是每条用例前后数一次子进程。

### cp —— 控制面（最大的一包）

十二个 forwarder 改成请求/应答帧；令牌取代进程局部句柄；server 拥有自己的
EGLDisplay/Surface/Context；`ForgetCurrentTuple` / `ForgetCurrentTupleIfItNames` 的语义
（`ServerLoop.cpp:318-330`，N-3 那条"创建不同 surface 会销毁重建原生 context"）必须在帧上
原样成立；真窗口 → 具名拒绝（§3.1）。

### st —— 静态量

`g_syncedRenderStateParameters` / `s_synced` 按 context 世代重置；G5 的客户端本地角色标志与
caps 快照 cap 位；a6 找出的其余同类。小包，但**红一次很容易写**：不重置 → 第二个 context
拿到上一个的同步状态 → 一条具名场景变红。

### t6 —— 车道

`integration-spawn` ctest label；`MOBILEGL_TRANSPORT=spawn`；阴性对照（§6）；
`HeadlessGL` 的 fork 预检；CI job。

---

## 5 出口门

走五部分门（`ARCHITECTURE.md` §13.2）之外，P6 自己的：

1. **G1** pull 构建 0/0/0/0，`.text` 不变。
2. **G2/G14** `integration-spawn` 的**用例名集合与 `integration-split` 逐名相同**。
   名集合不同就意味着 spawn 车道在偷偷少跑东西。
3. `integration-spawn` 全绿，数字与 `integration-split` 当期数字一致（今天是 179/179）。
4. **进程树门（机检）**：每条用例前后各数一次子进程；运行中恰好多一个；运行后为零。
   `HeadlessGL` 的 fork 预检不留孤儿。
5. **臂证明门（ID-124 的教训）**：spawn 车道的每条用例必须在**自己的私有日志**里留下子进程
   pid 与 `transport=spawn`。*"一条声称跑了 spawn、实际跑了 monolith 还绿了的车道"* 正是
   `ConfigLoader.cpp:315-317` 注释点名要防的事故，而 ID-124 证明这类假绿**真的会发生**。
6. **逐包 red-once**（R-16），每包一条具名对。
7. 设备：Redmi `2f7cbe2e`，reboot-clean + 同热窗口配对 A/B，OpenRA 在 Adreno 830 上
   split SSIM ≥ 0.99。
8. **性能只记录不阻塞**（路线图通则），但必须真采到，且必须回答一个具体问题：
   **socket 门铃相对 inproc 的 condvar 贵多少**。每次唤醒多一次 syscall；P5d 三轮把 client
   线程从 15.0 压到 9.2 ms/帧，其中相当一部分是门铃路径的钱。这是 P6 唯一预期的回归，
   值得单独一组数。

---

## 6 阴性对照（每条都要真跑过一次红）

| # | 旋钮 | 应该证伪什么 |
|---|---|---|
| S1 | `MOBILEGL_IPC_SERVER_PATH=/nonexistent` | 具名拒绝，**不是**静默回落 monolith。这是 §5 门 5 的运行时半边 |
| S2 | 运行中 `kill -9` server | 干净的 device-lost latch，客户端**不挂起**、不继续提交。§3.2 |
| S3 | 子进程 envp 未剔除 | 递归 spawn 必须被第二道保险挡住并具名 |
| S4 | 关掉 `st` 的世代重置 | 第二个 context 用到上一个的同步状态，一条具名场景变红 |
| S5 | 真窗口 `WindowKind::AndroidNativeWindow` 到达 server | 具名 `Fatal`，不是黑屏也不是崩溃 |

**S2 的形状要先想清楚再写**，否则会重蹈 E1 的覆辙（ID-122：它索要的证据在整次运行里出现 0 次）。
对照要证伪的是"客户端能区分慢与死"，不是"某个 Fatal 出现过"。

---

## 7 具名风险与未决

1. **run-ahead + 会死的对端**（§3.2）。P6 唯一的新设计。建议与 E1 重定义一起想。
2. **Android cached-app freezer** 会冻住子进程，而冻结不产生挂断事件。P12 的题材，但会在
   P6 的设备测试里先咬人。
3. **server 能否不链 `MG_Impl`** —— a6 回答；做不到就记账，不要留空头承诺。
4. **Windows**：`ShmSegment::Adopt` 是 POSIX-only，`NamedPipe`/`UnixSocket` 两种模式
   **建议继续具名拒绝**，P6 只落 POSIX（Linux + Android）。Windows 机器本来就不是正确性门。
5. **`SEG_REPLY` 2 MiB 单槽 cap 与 chunking**（`ReplySlot.h` 记作 "P6 debt"，ID-47）——
   **建议明确推给 P9**（反向通道那一阶段）。P6 不碰回读形状；混进来会让"只是传输替换"再次
   变成一句空话。
6. 511 条 `UnmigratedVerb` 的 class-C 账（`~/w7/notes/p6/census-classC.md`，2026-09-16）
   **不是 P6 的**。spawn 跑的是 P5b/P5c/P5e 那条同样的缩减路径，门是同一批 179 / 1357。
7. E1 对照（ID-122）仍未决，**不因 P6 开工而自动解决**。

---

## 8 明确不属于 P6

真窗口到达（P12）；`android:process=":mgl"` Service（P12）；chunked readback（P9）；
`DynamicBackendParameters` 的定宽重写（P7）；多 context（`MGPApplierReset::ContextSerial`
今天被断言相等，`CONTRACT-P5C.md:56` 说"P6's multi-context shape reads it for real"——
**建议推给 P12**，P6 维持单 context 并在契约里写明）；对象类 BARRIER-PULLED 行（P3b/P4b/P7）。

---

## 9 开工顺序建议

先跑 **a6**，只读，不改一行。a6 的清单出来之前，`c6` 的契约写不实，后面五包的大小也是猜的。
P5c 是这么开始的，也是那个阶段唯一没有返工的决定。
