# P11：persistent map 采纳档位诚实化（A 包主机半）

2026-09-29。依据 [`PLAN-P11.md`](../../docs/Disaggregated/notes/p11/PLAN-P11.md) §2 的 A1、A2 与 C 的文档修正。档位名按 `design/07`：T0 = client 分配 `AHardwareBuffer`、server 导入；T1 = server 导出 opaque fd；T2 = 拒绝，client 保留 shadow 并推送。本文只记实现所需的行为与门；A3 / A4（设备）与 B（T0）不在本文。

## 范围

档位在握手时定下（§1）；断言 arena 落在哪一档、三臂登记（§2）；文档漂移（§3）。

## 1. 档位由知道数据面的一侧在握手时定下（A1）

### 线上格式

- 无变化。LinkTerms、`RefuseCode`、控制协议修订号（3）都不动：每一侧只判自己的旋钮，不需要把档位带过线。

### 规则（`Transport/AdoptTier.cpp` `SettleAdoptTierAtHandshake`）

| 数据面 | 侧 | `MOBILEGL_IPC_ADOPT_TIER=0/1` |
|---|---|---|
| Stream（tcp） | client / server | 一行 W `Refuse{AdoptTierOnStream, "T<n>"}`，本会话 T2；从不 Fatal |
| SharedSegments（inproc、fork / unix spawn） | client | `Fatal{UnimplementedAdoptTier, "T<n>"}`，握手期、任何记录之前 |
| SharedSegments | server | 一行 W（档位由 client 定），T2 |

- 调用点：client `StartOverTransportPair`（Welcome 校验后）与 `StartOverSocket`（link terms 定下 `stream` 后、开数据连接前）；server `ServerSession::Accept`（后端检查后、Welcome 前）。
- 使用处 `Client::AdoptTierIsEmulate`（`PipeApply.cpp`、`PipeWireCodec.cpp` 仍问它）：旋钮 2 → true；本进程已有握手定档 → true，不再打印；没有握手定过档的调用者 → 仍 `Fatal{UnimplementedAdoptTier}`（"no handshake settled"）。旋钮 > 2（解析器不收）→ Fatal。
- `ServerSpawn` 从它拉起的子进程环境剔除 `MOBILEGL_IPC_*`（五个 server 自有旋钮除外），所以 fork / unix spawn 的 server 看不到 client 的旋钮；tcp 的 server 读自己的环境。

### 普查

新的死亡走 `SessionFail`：abort 站点仍 79，family 词仍 45（复用 `UnimplementedAdoptTier`）；refusal 词 3 → 4，`AdoptTierOnStream` 进 `fatal_census.py` 的 `LOCAL_REFUSAL_WORDS`（本地拒绝，不成为 Refuse 帧）。

### 门

- 单元：`AdoptTierStream.*`（真 tcp 会话：client 0/1 → 会话起、一行拒绝、`map_persistent` 得 DECLINED、之后仍 apply；进程内显示 server 0 → 每会话一行、不死）；`RemoteClientControls.AdoptTier{Zero,One}OverSharedSegmentsDiesAtTheHandshakeBeforeAnyMapPersistent`（inproc 真握手，子进程在 `Start` 返回前死、具名）；`SplitBufferSet.OnlyAdoptTierTwoIsImplemented`（函数级矩阵）；`PipeWireCodecTest.AdoptTier{Zero,One}AfterAStreamHandshakeIsADeclineOnTheWirePath` 与 `AnUnsettledAdoptTierStillDiesByNameOnTheWirePath`。
- 集成：`{DirectGLES,DirectVulkan}.Tcp.AdoptTier0.LargeArenaAdoptionScenario.*`（`MOBILEGL_IPC_ADOPT_TIER=0`；`integration-tcp` / `integration-magma-tcp`），`AStreamSessionRefusesTheAdoptTierOnceByName` 读 client 日志恰一行，其余用例绿。tcp 专属：共享段上同一旋钮在握手期死（`spawn_lane_parity.py` 的 `MAGMA_TCP_ONLY`）。
- red-once：删 Stream 分支 → 两后端 tcp 条目 client 握手期 `Fatal{UnimplementedAdoptTier}`、无拒绝行，单元 `AdoptTierStream` 两例、codec 两例、`SplitBufferSet` 一例红；把检查挪回使用处 → inproc 会话起来、`map_persistent` 发出后才在 apply 线程死（子进程写出 `S`），spawn 臂则静默跑 T2（server 环境被剔除）。

## 2. arena 落在哪一档（A2）

- `LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares`：`PersistentMapPeek` 读 arena 的 `IsBackendPersistentMapped`，与车道的 `MGITEST_PERSISTENT_MAP_ARM` 比；问两次（NULL 定义后；一帧绘制 + SSBO 绑定 + dispatch 后）。未声明的车道跳过。
- 声明：monolith = adopted（`DirectGLES.ResourceSubsystemOn/Off.`、新 `DirectVulkan.ArenaArm.`，都钉 monolith）；split = emulated（Espryt 三臂经 `SplitLogPaths` 按条目声明；Magma `Arena.` 块与 Buffers 车道）。
- 登记：`mgl_itest_register_split_arms("LargeArenaAdoptionScenario.*")`（Espryt 进门、Magma 进信息层）；Magma 进门：`DirectVulkan.<arm>.Arena.`（band 口味，六例）与 Buffers 车道（加六例，`test.yml` 的 require-green 名单同步）。
- red-once：monolith 车道加 `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1` → 三条红；split 车道改声明 adopted → 两后端 8 条红。删 `PipeApply.cpp` 的 R-6 拒绝**不会**让 Espryt inproc 采纳（实测仍 emulated）：split 下 client 的采纳门经线路走，codec 不调 applier 就答 DECLINED；applier 只在 server 角色自己发射时被问到（只有 Magma 的 `EnsureGpuResidentStorage`），而 Magma 的线路表 `MapPersistent` 恒返回空。

## 3. 文档漂移（C）

- `Config.h`：大 buffer 采纳下的 SubData 自 `0ee3384b` 起经 resident-subdata GPU 有序落地，不是"plain memcpy"。
- `MG_Backend/Init.cpp`：DirectVulkan 在非 monolith 下注册线路资源表（`VkBufferManager::RegisterWireResourceOps`）并声明 bit 7。
- 档位命名按 `design/07`：`PersistentMapTracker.h` 注释已改。`protocol.fbs` 的 `Adopt = 6 // server-owned adopted store` 注释**没改**：`protocol_revision_pin.py` 对整文件取 sha256，改注释要 bump 修订号。
- `CONTRACT-P6.md`、`design/06`、`design/07` 的"stream 上具名拒绝"现为真，已指向本节代码。

## 不变量

- G1：新代码全在 `MOBILEGL_BUILD_DISAGGREGATED` 下（`MG_Remote`）。
- 线上格式与控制协议修订号不变（3）。（A 包；PAIR 包把修订号升到 4，见下节。）

## PAIR：socket client 的两条连接按身份配对（B0 F2）

依据 [`B0-CROSS-APP.md`](../../docs/Disaggregated/notes/p11/B0-CROSS-APP.md) F2：server 按到达顺序配对（`AcceptPair`，"先到是控制、后到是 aux"），设备上 server app 的就绪探测（一次空 `connect()` + `close()`）成了下一个真 client 的控制连接，会话读到探测的套接字后退出（`control peer closed before sending a first frame`），client `no Welcome … (rc=6)`；两个 client 同时连也会互换半边。

### 线上格式

- `protocol.fbs` 追加 `table PairBind { nonce: [ubyte]; aux: bool; }` 与 `CtrlMsg` 末尾的 `PairBind`。**控制协议修订号 3 → 4**（`mg_protocol_base.h`，`protocol_revision_pins.json` 已钉），`wireFingerprint` 随之变。
- `SocketTransport::ConnectTo`（`unix:` 与 fork spawn 两条 client 路径都走它；`tcp://` 上只有测试用）：CSPRNG 取 16 字节 nonce，控制、aux 两条连接各以一帧 `PairBind{nonce, aux}` 开头；控制连接上 Hello 在它之后。编解码在 `Transport/PairBind.h`。
- TCP 产品路径不变：`ConnectControl` 单连接，数据连接凭 server 发的 `Welcome.dataNonce` 绑定（`DataBind`，PH-7 (4)），`TcpSupervisor` 的预认证门本来就丢探测。

### 规则（`Server/PairAcceptor.{h,cpp}`，取代已删的 `SocketTransport::AcceptPair`）

| 连接的首帧 / 状态 | 处置 |
|---|---|
| 一字节未发就关（就绪探测） | 关闭，D 级日志 |
| `PairBind`，另一角色同 nonce 已在等 | 成对交出（控制作流、aux 作描述符通道） |
| `PairBind`，同 nonce 同角色已在等 | `Refuse{Authentication}`，关新来者 |
| `Hello`（修订号 < 4 的 client） | 按 `RunSession` 的顺序问：token → 线上格式；旧 client 得 `Refuse{WireFingerprint}`，都过的仍 `Refuse{MalformedHello}`；从不配对 |
| `DataBind` / 其它 | `Refuse{Authentication}` "data connection names no live session" / `Refuse{MalformedHello}` + 形状细节 |
| 自 accept 起 2000 ms 内未发 `PairBind`，或伙伴未到 | 控制与未识别者 `Refuse{Authentication}` 后关；aux 直接关（其 client 在那里只收描述符）；W 日志 |
| 同时未配对的超过 16 条 | 丢最老的未识别者（没有则最老者），`Refuse{Busy}` |

- 从不读过 `PairBind` 一个字节（`FirstFrameAssembler`）：控制连接后面的 Hello 归会话读，aux 后面是 `SCM_RIGHTS`。
- 有状态：A-control、B-control、B-aux、A-aux 时先交出 B，A-control 留给下一次 `Accept`。`Accept(timeoutMs)` 超时返回 `TIMEOUT`，未配对的留着。
- 调用点：`UnixInProcessSupervisor`（成员）与 `mobilegl_server_main` 的 unix 分支。`--serve` 的会话子进程 `CloseInForkedChild`（只 `close`，不 `shutdown`）；单会话形状配对后即析构，其余等待者得 `Refuse{Busy}`。`RefuseBusy` 不变（Hello 仍在套接字里）。

### 版本互通

| | 修订 3 server | 修订 4 server |
|---|---|---|
| 修订 3 client | — | `PairAcceptor` 代答 `Refuse{WireFingerprint}`，从不配对 |
| 修订 4 client | 旧 server 按顺序配对后首帧是 `PairBind` → `Refuse{MalformedHello}` "first control frame is not a verifiable Hello"；旧会话退出若抢在 client 发 Hello 之前，client 只见发送失败 | 正常 |

### 门

- 单元 `Endpoints/PairAcceptorTest.*/{unix,tcp}`（8 例）：(1) 探测后紧跟 client；(2) 四条交错；(3) 孤儿 aux 过预算被关、下一个 client 成对；(4) Hello 先到的旧 client 具名拒绝。每例用 `ServesExactly` 证身份（控制双向各一帧 + 该 client 的 aux 字节是 server aux 上的头几个字节）。
- 端到端 `ServerSpawnTest.AStrayConnectionAheadOfTheClientDoesNotTakeItsSession`：单会话 unix server，探测之后真 client 握手成功、server 退出 0。
- red-once：`PairAcceptor::Accept` 换回到达顺序配对（仍读掉首帧，只差配对规则）→ 单元 8/8 红（(1) 控制连接是已关的探测；(2) aux 读到的不是本 client 的字节），端到端握手 rc=6（与设备上 `no Welcome … (rc=6)` 同）；对照 `StartsAServerProcessAndHandshakesAcrossIt` 仍绿。拷回原文件、`cmp` 字节相同后复绿。

### 不变量

- G1：新代码全在 `MG_Remote`（`MOBILEGL_BUILD_DISAGGREGATED` 下）。
- 未改：`TcpSupervisor`、预认证门、`DataBind`、会话读 Hello 的路径。

## B1：同机外部 client 经 app_process helper 与令牌 broker 走共享内存

依据 [`PLAN-P11.md`](../../docs/Disaggregated/notes/p11/PLAN-P11.md) §2 B、[`B0-CROSS-APP.md`](../../docs/Disaggregated/notes/p11/B0-CROSS-APP.md)、[`HSPIKE.md`](../../docs/Disaggregated/notes/p11/HSPIKE.md)（ID-P11-13）。消费者：同一台手机上从另一个 app（Termux、adb shell）启动、没有 Java `Context` 的原生 GL 程序。证据根 `~/w7/notes/p11/evidence/b1/`（下文的 `evidence/b1/…`、`m-probe*`、`freeze-*` 都在它下面），报告 `~/w7/notes/p11/b1-report.md`。

### 链路

| 步 | 谁 | 做什么 |
|---|---|---|
| 1 | 用户 | `MOBILEGL_IPC_TOKEN=<令牌> CLASSPATH=<server APK> app_process / top.mobilegl.plugin.ExternalClientHelper [--server <包名>] [--timeout-ms <n>] [--keep-affinity] -- <程序> [参数…]`（server 界面给出可复制的整行：APK 路径取 `getApplicationInfo().sourceDir`，不靠 `pm path`——包可见性会过滤 app uid 的查询） |
| 2 | helper（`app_process`，client 自己的 uid） | 令牌取自 `MOBILEGL_IPC_TOKEN`，否则取 `MOBILEGL_IPC_TOKEN_FILE` 指的文件；**从不取 argv**（`--token` 具名拒绝）。`--server` 缺省为 `BuildConfig.APPLICATION_ID`（加载它的那个 APK 的包名） |
| 3 | helper | 经隐藏的 `IActivityManager.broadcastIntentWithFeature`（null caller，参数按类型填，末位 int = user id）发显式广播：组件 `<包名>/top.mobilegl.plugin.ExternalClientBroker`，action `top.mobilegl.plugin.EXTERNAL_CLIENT_CONNECT`，`FLAG_RECEIVER_FOREGROUND \| FLAG_INCLUDE_STOPPED_PACKAGES`，extra `mobilegl_broker` = Bundle{`callback`: helper 自建的 Binder，`token`，`version`=2} |
| 4 | broker（导出的 receiver，两个口味都有，跑在 `:mglsrv`） | 按下表逐条检查；通过则连两次 server 的 unix 端点（`@name` 或路径；相对路径按 `filesDir` 解析）。server 只听 `tcp://` 时，`MobileGLServerService` 在旁边多起一个 supervisor，听私有抽象名 `@<包名>.broker.<8 位 hex>`（同一二进制、同一环境与令牌），broker 连它。连上后**立即写 PairBind 对**（ID-P11-12：谁开连接谁出示；新 nonce，控制 `aux=false`、辅助 `aux=true`；字节见下「交接时配对」） |
| 5 | broker → helper | **每个请求都回话**，单向 `transact`：`FIRST_CALL_TRANSACTION` = int version（2）+ 控制 PFD + 辅助 PFD + long applyCore（在跑的 supervisor 启动时宣布的保留核，0 = 无）+ int flags（bit 0 = 已在交接时配对）；`FIRST_CALL_TRANSACTION+1` = String 拒绝名 + String 说明 |
| 6 | helper | 两个 fd 清 `FD_CLOEXEC`，`execve` 程序（applyCore ≠ 0 且没给 `--keep-affinity` 时经 `/system/bin/taskset <在线 & ~applyCore>`）；环境 = 原环境 − `CLASSPATH` + `MOBILEGL_TRANSPORT=spawn`、`MOBILEGL_IPC_CONTROL=fd:<控制>,<辅助>`、`MOBILEGL_IPC_DATA=shm`、`MOBILEGL_IPC_TOKEN=<令牌>`，flags bit 0 时再加 `MOBILEGL_IPC_FD_PAIRED=1` |
| 7 | client（`fd:` 端点） | 见下；之后与 `unix:` 完全相同：Hello（dial = Connect）、Welcome、7 个 fd 经 `SCM_RIGHTS`、SharedSegments |

### `fd:<控制>,<辅助>` 端点（`ClientSession::StartSpawned`，`SocketTransport::AdoptConnectedPair`）

- 文本严格：两个十进制数、一个逗号，别的都不收；`MOBILEGL_IPC_DATA` 只收 `auto` / `shm`。违者 `Refuse{ProtocolMismatch}` 具名、`MOBILEGL_ERR_UNSUPPORTED`。
- 两个描述符必须互不相同、在本进程打开、是 `AF_UNIX` + `SOCK_STREAM`、已连接（`getpeername`）；否则 `Refuse{ProtocolMismatch}` 说明哪一条，`MOBILEGL_ERR_INVALID_ARGUMENT`，**一个字节不写**。TCP 套接字被拒（没有描述符通道，用 `tcp://`）。
- 收养后两个 fd 设 `FD_CLOEXEC`（程序自己的子进程不继承）。
- `MOBILEGL_IPC_FD_PAIRED=1`（helper 按 broker 的 flags 设）：PairBind 已由 broker 在交接时出示，本端点**不再写**（`SocketTransport.cpp:492`），直接 Hello。没有这个标记时本端点自己出示一对（CSPRNG 16 字节 nonce，控制 `aux=false`、辅助 `aux=true`，然后 Hello——与 `ConnectTo` 同），供裸连接交来的 fd（测试、旧 broker）。
- 自己出示时发送失败（server 已拒绝并关掉这一对）：两个 fd 留给调用方，client 读控制连接上待读的拒绝帧并具名报出（如 `peer Refuse{Authentication} no PairBind within the pairing budget`），不只是一个发送错误。
- 一对 fd 只够一个会话：会话结束（或同一进程再起会话）时 fd 已关，第二次收养具名失败。

### 交接时配对与等 Hello 的时限（ID-P11-12）

- broker 连上两条连接后立即写 PairBind 对（`PairBindFrames.java`：帧头 magic `0x464C474D` + 长度，小端；负载是原生 `EncodePairBind` 的输出，16 字节 nonce 在最后）。两个前缀是编码器的字节；`ServerSpawnTest.TheBrokersPairBindTemplatesAreTheEncoders` 从 Java 源文件读出前缀、对 32 个随机 nonce 与 `AppendFrame(EncodePairBind(…))` 逐字节比较，`protocol.fbs` 一改就红。`PairAcceptor` 的 2000 ms 配对预算（`PairAcceptor.h:63`）因此在 broker 手里满足，不再等程序跑到第一个 EGL 调用。
- **配对之后等 Hello：没有时限，靠 EOF。** 之前是 10 s（`RunSession` 的 `helloWaitMs`，`ServerMain.cpp:385` 的读）；现在 unix 端点的 `--serve` 会话子进程与单会话形状都用 `kPairedUnixHelloWaitMs = kWaitForever`（`ServerMain.cpp:136`，调用点 `:1537`、`:1562`）。理由：配对时身份已定（nonce），unix 端点只有本用户 / 本 app 能连（Android 上 SELinux，路径上 0600），而一个先加载资源的程序要多久没有上界；程序退出或关掉描述符 → EOF → `control peer closed before sending a first frame`，会话子进程干净退出。代价：程序活着但迟迟不 Hello 时，它占着唯一的会话槽，别的 client 得 `Refuse{Busy}`（与一个正在渲染的会话相同）。TCP 保留预认证时限（`PreAuthKnobs`）；进程内显示 server（`:mglwin`，`ServerMain.cpp:1395`）保留 10 s（它停止时要等会话线程）。

### 令牌

- broker 比对的是**正在运行的 server 自己的** `MOBILEGL_IPC_TOKEN`（启动 server 时界面上的 Auth token），常量时间比较。
- server 的令牌不足 16 字节（含空）→ `server-no-token`：不接同机外部 client。unix 端点对同 app client 仍然不要令牌（行为不变）。
- helper 把令牌放进被 exec 程序的环境，Hello 也带它：server 配了令牌时 `AuthenticatePeerToken` 在 unix 会话上照样核对（`Refuse{Authentication}`）。
- 令牌只经环境、令牌文件、Binder 事务传；不进任何进程的 argv。

### 拒绝

| 名 | 条件 | helper 退出码 |
|---|---|---|
| `bad-request` | 没有 `mobilegl_broker` Bundle / callback（只能记日志）或 version ≠ 2 | 64 |
| `token-missing` | 未出示令牌 | 65 |
| `token-wrong` | 令牌不符；不连、不给 fd | 66 |
| `server-no-token` | server 令牌 < 16 字节 | 67 |
| `server-not-running` | `:mglsrv` 里没有在跑的 server（广播可以把 `:mglsrv` 进程拉起来——server app 被强停后也一样，设备实测——但 Android 12+ 不许后台 receiver 起前台服务，所以答"先在 MobileGL 的 server 界面启动 server"，不挂起）。只认离屏服务；`:mglwin` 显示 server 在跑时也答它 | 68 |
| `broker-busy` | 另一个请求正在 broker 里连接，或 server 的监听 3000 ms 内没接受两条连接 | 69 |
| `connect-failed` | server 在跑但它的端点拒绝连接，或 PairBind 写不进去 | 70 |
| （helper 本地）无应答 | `--timeout-ms`（缺省 10000）内 broker 没回话：包没装，或系统没把停止的 app 拉起来（自启动管控） | 71 |
| （helper 本地）广播发不出 | 反射拿不到 `IActivityManager` / 调用抛异常（见"依赖 ROM"） | 72 |
| （helper 本地）exec 失败 | | 73 |
| 用法错 | 无程序、未知选项、`--token` | 2 |

### apply 线程的亲和 / 自旋（`Server/ApplyThreadPolicy.h`）

`ServerSession::Accept` 记下 Hello 的 dial mode 与数据面；`ServerLoop` 的 apply 线程按它们解析 `MOBILEGL_IPC_SERVER_AFFINITY=auto`：

| peer | 数据面 | `auto` |
|---|---|---|
| in-process（inproc，dial No） | — | 全部大核、配置的自旋（不变） |
| forked（同 app fork spawn，dial Fork） | 共享段 | 同上（不变） |
| connected（dial Connect：`tcp://` / `unix:` / `fd:`） | stream | 同上（不变：io 线程喂 apply 线程，B0 实测 apply 7.9 ms/帧 vs shm 26.8） |
| connected | 共享段 | 一个核，`ReservedApplyCore(big, prime, online)`（`ApplyThreadPolicy.h:120`）：≥ 2 个 prime 核（峰值主频）→ 最低编号的 prime 核；只有 1 个 prime 核 → 它旁边最低编号的其他大核；只有 1 个 prime 核且无其他大核 → 不设亲和；无非对称（全是大核或读不到拓扑）→ 不设亲和。**从不把唯一的 prime 核给自旋的 apply 线程**。自旋 `kConnectedPeerSpinUs` = 50 µs（`MOBILEGL_IPC_SPIN_US` 显式设了则用它） |

- **一个来源**：保留核只由原生规则算。supervisor 启动时（`ServerMain.cpp:1485`）按本机拓扑与 `MOBILEGL_IPC_SERVER_AFFINITY`（显式掩码 / `off` → 0）算出 `ReservedApplyCoreForThisProcess`（`ServerLoop.cpp:210`），在 stdout 打印 `MG_Remote server: apply core reserved for dialled-in shared-segment clients: 0x40 (<规则>)`；`MobileGLServerService` 逐行读 supervisor 输出（`ServerEnvironment.parseReservedApplyCore`），broker 把它放进答复；helper 经 `/system/bin/taskset <在线 & ~applyCore>` exec 程序（亲和跨 exec 继承）；`--keep-affinity` 不设。Java 不再重算规则。代价：整个程序少用一个核。
- 显式掩码或 `off` 对所有 peer 生效（与以前相同）；`MOBILEGL_IPC_SPIN_US` 设了就用设的值。
- 启动日志 `mgl-srv-apply started … RESOLVED mask 0x…, spin N us; peer <in-process|forked|connected> on <shared segments|stream>, policy <规则>`，规则名写明是哪一种（最低 prime / 唯一 prime 旁的大核 / 唯一 prime 留给 client 而不设亲和 / 无非对称）。
- P12 的进程内显示 server（`:mglwin`）的 client 也是 dial Connect：它们走 stream 时不变，走 unix 共享段时得到本规则。

选择依据（红米，rd12 稳态 fps，helper 路线，默认落位、定频；`evidence/b1/m-policy.tables.md`）：

| 候选（apply 掩码, 自旋） | a shm Espryt | a tcp Espryt | b shm Espryt | b tcp Espryt | a shm Magma | a tcp Magma |
|---|---|---|---|---|---|---|
| 旧 auto（0xc0, 50） | 21.5 | — | 62.5 | — | 22.1 | — |
| 最高大核（0x80, 50） | 26.7 | — | **6.5** | — | — | — |
| 最低大核（0x40, 50）= q1 | 26.5 | 51.1 | **119.9** | 40.3 | 21.9 | 20.3 |
| 0x40, 不自旋 | 16.9 | — | 73.6 | — | 22.5 | — |
| 不设亲和, 50 | 21.7 | — | 64.9 | — | 23.3 | — |
| 不设亲和, 不自旋 | 20.2 | — | 12.8 | — | 20.7 | — |
| 非大核（0x3f, 50）= q3 | 49.0 | 27.0 | 50.3 | 29.2 | 18.4 | 13.9 |

- 前台 app 的 GL 线程被调度器放在**最高**大核（cpu7，44 次采样中 43 次）：把 apply 线程钉在 cpu7 就与它同核（6.5 fps）；钉在 cpu6 得到 B0 手工定核的布局（120）。
- 不自旋（0）在每一格都输：保留 50 µs。
- adb shell 的 client 被放在 cpu6（与 apply 线程同核）：q1 下路线 a 的 shm 26.5 < tcp 51.1。q3（apply 放非大核）能让 shm ≥ tcp，是因为它**同样**把 tcp 会话的 apply 线程放到慢核（tcp 51 → 27），会拖慢所有同机 TCP client（FCL loopback、`:mglwin`），所以不用；改为数据面分流（stream 不变）+ helper 让程序避开保留核。

### 保活

- server 仍是前台服务（`dataSync`，带 partial wake lock）；broker 从不启动它。前台服务本身挡不住 HyperOS 的冻结（greeze / SmartPower，B0 F1）。
- **用户必须设**：server app 与 client 所在 app 的"省电策略"都选"**无限制**"（HyperOS：设置 → 应用 → 应用管理 → <app> → 省电策略 → 无限制；powerkeeper 记为 `bgControl=noRestrict`，并进 deviceidle 白名单），再在 server 界面设 Auth token（≥ 16 字节）、以离屏模式启动。
- 设了（红米，`evidence/b1/freeze-{a,b}`）：路线 a、b 各熄屏 180 s，在飞会话（rd12 + 200 s 保持）**存活**（正常结束，0 个 Fatal），熄屏期间每 45 s 新起的 3 个路线 a 会话都通过（ssim 1.0）；两 app 所有进程的 cgroup.freeze 采样 0 / 103 与 0 / 143，无 `do_freezer_trap`；系统日志 `PolicyMaker: uid=… pkg=<server> reason=NO_RESTRICT_APP`。
- 不设（缺省"智能限制后台运行"）时具名失败的样子：client app 在前、server app 在后，greeze 对 server uid `setUidState allow=false`、停用其 wake lock，路线 b 的会话 `no Welcome from the spawned server within 5000 ms (rc=7)`（`m-probe2-defaultbattery`，屏亮时就发生，非必现）；熄屏约 7 s 后冻结整个 server uid，在飞会话 120 s 后 `Fatal{BarrierTimeout}`（B0 F1、HSPIKE §7）。
- 已知：这台机上旧包 `top.mobilegl.plugin.trace` 的"省电策略"页不显示选项（原因未查），门改用新装的 `top.mobilegl.plugin.b1.trace`（像用户新装一样）。

### 依赖 ROM

- 直接跑的 `app_process` 不是 zygote fork 出来的，不受 app 的隐藏 API 黑名单约束，`IActivityManager.broadcastIntentWithFeature` 才能反射调用（HyperOS / Android 16 上 `hiddenapi` 日志 0 条，shell 与 app uid 都通）。若某 ROM 对它也强制执行，helper 以退出码 72 具名失败；没有替代（`am broadcast` 带不了 Binder）。
- HyperOS 的冻结（greeze / SmartPower）不看前台服务：见"保活"。

### 线上格式

无变化（控制协议修订 4，PAIR）。`fd:` 只是 client 端点的一种写法；broker 协议是 Android Binder 上的 Bundle / Parcel，不属于 MobileGL 线上格式。

### 门

- 单元：`ApplyThreadPolicy.*`（8 例：每个 peer × 数据面的 `auto`、最低大核与非连续大核集、对称 / 单大核 / 读不到拓扑、显式掩码 / `off` / 显式自旋、认不出的文本）；`ServerSpawnTest.AnFdPairTheBrokerConnectedBringsASharedSegmentSessionUp`（真 server 进程，两次裸连接交给 `fd:`，会话起、fd 为 CLOEXEC、server 退出 0）、`…AnAdoptedFdPairPresentsOnePairBindPairBeforeAnythingElse`（在 server 侧读：每条连接恰一帧 PairBind，同 nonce，aux 标志正确）、`…AnFdEndpointThatIsNotAConnectedUnixPairIsRefusedByNameAndWritesNothing`（管道、同一 fd 两次、未打开、未连接、八种坏文本、`stream`，都具名拒绝且一字节不写）；Java 镜像 `ServerEnvironmentTest`（`reservedApplyCore` 7 条，`test_android_lifecycle.py`）。
- red-once：`AdoptConnectedPair` 不写 PairBind → 端到端例红，client 日志 `peer Refuse{MalformedHello} the control connection's first frame is a Hello, not a PairBind (control revision 4 pairs by nonce)`，PairBind 例红（控制连接上无帧）；保留核取最高大核（设备否决的规则）→ 3 例红；策略不看数据面 → stream 例红。每次拷回原文件、`cmp` 相同后复绿。
- 跟进（交接时配对、唯一 prime 核、单一来源）：`ServerSpawnTest.TheBrokersPairBindTemplatesAreTheEncoders`（Java 前缀 = 编码器字节，32 个随机 nonce × 两个角色）、`…APairTheBrokerPresentedAtHandoffWaitsForAHelloThatComesSecondsLater`（broker 的字节在交接时写出，Hello 5 s 后才来，会话起）、`…APairPresentedOnlyWhenTheProgramStartsIsRefusedByNameOnceThePairingBudgetIsGone`（旧流程：3 s 后才出示 → client 日志 `peer Refuse{Authentication} no PairBind within the pairing budget`）；`ApplyThreadPolicy.*` 10 例（加唯一 prime 核旁有 / 无其他大核、无非对称、`ReservedApplyCoreForConfig`）；Java `PairBindFramesTest`、`ServerEnvironmentTest`（宣布行解析 5 条）。red-once：交接步骤拿掉（今天的 client 侧配对）→ 交接例红，`Refuse{Authentication} no PairBind within the pairing budget`；Hello 时限改回有限的 3 s → 红，`Refuse{Authentication} no authenticated first frame within the pre-auth deadline`；client 不看标记、再出示一对 → 红，`Refuse{MalformedHello} first control frame is not a verifiable Hello`；保留核不看 prime 集 → 唯一 prime 的两例红；Java 前缀改一个字节 → 模板例红。每次拷回、`cmp` 相同后复绿（`~/w7/logs/p11/b1/redonce4.out`）。
- 设备门（红米 `2f7cbe2e`，HEAD `3519a21b` 的 APK，两 app "无限制"，默认环境：不手工定核、无解冻守护；CPU 定频并回读只为可重复，`evidence/b1/m-gate.summary.md`）：16 个 ssim 格全过（rd12 0.99988 / 0.999884，openra 1.0）；shm（helper）对 tcp 的稳态 fps 中位数：

| 路线 | 后端 | rd12 shm / tcp | openra shm / tcp |
|---|---|---|---|
| a（shell uid） | Espryt | 119.2 / 53.1（2.25×） | 927.6 / 427.5（2.17×） |
| a | Magma | 36.8 / 20.2（1.82×） | 281.2 / 278.6（1.01×，8 次） |
| b（另一 app，在前） | Espryt | 119.9 / 39.0（3.07×） | 923.4 / 377.4（2.45×） |
| b | Magma | 31.5 / 20.2（1.56×） | **285.3 / 286.6（1.00×，8 次，差 0.5%）** |

  15 / 16 格 shm ≥ tcp；Magma openra 路线 b 按合并中位数差 0.5%（逐次配对 shm 赢 6 / 8，区间完全重叠）：该负载两个数据面都受 server 端限制，shm 不增益。拒绝（token-wrong a/b、token-missing、server-not-running a/b 与强停后、server-no-token）都按名、退出码对、无会话。同 app spawn+shm / inproc 的 rd12 p50 与 A4 相同（策略对 Forked / InProcess 不变，日志 `mask 0xc0`）。

- 重定基后的设备复测（`df826f42` 的 APK，两 app "无限制"，默认环境；`evidence/b1/m-pass2.summary.md`、`m-openra3.summary.md`）：rd12 八格 ssim 全过；shm / tcp 中位数（3 次交错）路线 a Espryt 119.3 / 53.2、Magma 36.4 / 20.3，路线 b Espryt 118.7 / 39.8、Magma 32.5 / 20.4；22 个 shm 运行全部"交接时配对"。Magma openra 在干净设备上重测：路线 a 296.4 / 284.4、路线 b 292.7 / 281.6（都 1.04×）——首轮门那一格的 5 次补测与一个失控的采样子进程（root `tr`，占满一个核约 65 分钟）同时跑过，不作数。慢启动：helper exec 一个先睡 10 s 的包装再 exec retrace，broker 交接后 10.2 s 才 Hello，两后端会话都起、ssim 过。plugin 口味（`.b1p`，界面上 Generate token + Start server）：helper 路线 openra ssim 1.0、rd12 0.99988，HyperOS 省电策略页显示四个选项。

### 不变量

- G1：原生改动全在 `MG_Remote`（`MOBILEGL_BUILD_DISAGGREGATED` 下）；pull 构建符号增 0 减 0。
- 线上格式与控制协议修订号不变（4）。
