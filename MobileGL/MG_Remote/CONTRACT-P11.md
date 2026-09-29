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
