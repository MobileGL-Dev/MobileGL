# P7 wave 2-F：Ph 小件（分支 `p7/ph-f`）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §1.1（PH-7、PH-8、D11、门行）与 §3 wave 2-F；
> 裁定见 [`INTEGRATOR-DECISIONS-P7.md`](INTEGRATOR-DECISIONS-P7.md) ID-P7-3（令牌足够、不做 TLS、
> `Welcome.dataNonce`）；规范见 [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0
> 规则 I / J、§9 簇 F、§10 第 3 条。基线 `ec46a550`。
>
> 主机口径：WSL Arch + lavapipe，`build-split` 与集成者同配置（integration tests ON）。flatc 取
> `3rdparty/flatbuffers` 子模块构建的钉版本（`scripts/gen_protocol.py`）。
>
> 基线门读数（`~/w7/logs/p7-gate-f-base.log`）：G1 `.text` `0xa52203`、符号 0/0；普查 79 处 abort、
> 0 未标记；棘轮 173；车道 unit 2429 / split 303 / spawn 219 / tcp 222 / magma-tcp 74。

## 0. 每片一条提交，每片一次 red-once

下表的 SHA 都是**包树**（分支 `p7/ph-f`）上的；集成者 cherry-pick 到 `pipe` 后以那边的为准。

| 片 | 项 | 提交 | 状态 |
|---|---|---|---|
| 1 | PH-7 (1)(2)(3)：常量时间令牌合一、≥16 字节（TCP 监听处）、无令牌只 loopback、smoke 入 CI | `2975a337` | 落地 |
| 2 | PH-7 (4)：`Welcome.dataNonce` 把数据连接绑定到已认证的控制连接 | `596b34fd` | 落地 |
| 3 | PH-8：Hello 要 64 GiB → server 忽略请求、按自己的尺寸建段且不分配 | `a746417a` | 落地 |
| 4 | D11 五处上限 + PH-2 | — | **未做**，见 §4 |
| 5 | PH-6 drop-with-latch | — | **未做**（顺序在 4 之后） |
| 6 | PH-1 (3)(4) | — | **未做**（顺序在 4 之后） |
| F-a | 审查修复 1+4：绑定后关闭 hand-off / 单会话监听者，hand-off 发送不阻塞 | `3370833f` | 落地，见 §6.1 |
| F-b | 审查修复 2：`sha256(protocol.fbs)` 钉住 `MOBILEGL_PROTOCOL_CONTROL_REVISION` | `75f24db0` | 落地，见 §6.2 |
| F-c | 审查 nit 6/8/9/10（代码） | `2b2a0ae0` | 落地，见 §6.3 |
| F-d | 审查 nit 3/5/7/11 + 两条 §12 债（文档） | 本提交 | 落地，见 §6.4 |

red-once 原始行汇总在 `~/w7/logs/p7-ph-f-redonce.txt`。

---

## 1. 片 1：令牌策略合一（PH-7 (1)(2)(3)，ID-P7-3）

### 1.1 机制

`MOBILEGL_IPC_TOKEN` 原来有三种读法：`SocketTransport.cpp` 的 `ListenTcp` 只问「非空」，所以一个字节
的令牌也能打开 `tcp://0.0.0.0`；`ServerSession::Accept` 用 `std::strcmp`；`ServerMain::RunSession` 用
`std::string::operator==`，且在 server 没配令牌时反而要求对端送空令牌。两处比较都在第一个不同字节处返回。

- 新文件 `MG_Remote/Transport/AuthToken.h`：`ConfiguredAuthToken()`（空值 = 未配置）、
  `kMinimumAuthTokenBytes = 16`（**只在 TCP 监听处**问，见下条；unix 端点既不要求令牌也不问长度）、
  `ConstantTimeTokenMatch`（F 修复轮起循环固定 64 字节、两端越界读零，更长的令牌比到最后一字节；长度差并入
  累加器，嵌入的 NUL 参与比较）。
- `Handshake.h` `AuthenticatePeerToken`：**唯一**的策略函数，`ServerSession::Accept` 与 supervisor 子进程
  （`ServerMain::RunSession`）都调它。不配置令牌时不因对端送了令牌而拒绝——策略由监听侧的 loopback 限制负责。
- `ListenTcp`：配置了但不足 16 字节的令牌**让 TCP 监听失败**（具名 `Refuse{Authentication} ... minimum is 16`，
  进程退出 72），而不是悄悄退化成 loopback server。令牌值从不进日志。unix 监听者不检查这条：那条路上令牌
  不是访问控制（§6.4 的本地对端债）。
- `Refuse{Authentication}` 是 `protocol.fbs` `RefuseCode` 的枚举值，`fatal_census.py` 规则 4 的词表门
  直接认它；普查前后都是 3 个 refusal 词，无需新增 `.def` 行（Refuse 词不是 Fatal 家族词）。
- CI：`TcpLane.SupervisorProtocolControls` 从 wave 0 起挂在 `integration-tcp` 上却因 CI 没有 flatc 而恒为
  SKIP。`test.yml` 的 integration-split 作业现在从子模块构建钉版 flatc（`gen_protocol.py --check`，
  不改已提交头文件），导出 `MOBILEGL_FLATC_EXECUTABLE`，并用 `junit_tally.py --require-entry-passed
  TcpLane.SupervisorProtocolControls` 断言它**跑了且过了**——SKIP 在这里等于红（`tcp_lane_tools_test.py`
  覆盖 passed / skipped / notrun / failed / absent 五态）。
- smoke 新增两条监听控制：无令牌 + 通配地址 → 具名拒绝；13 字节令牌 + loopback → 具名拒绝、行里不含令牌。

### 1.2 red-once（两进程，`TcpLane.SupervisorProtocolControls`）

把 `SocketTransport.cpp` 还原到 `ec46a550`、只重链 `libMobileGL.so`：

```
RED   AssertionError: (None, '... MG_Remote server: pid=42256 listening on tcp://127.0.0.1:46447')
GREEN short_token_listen exit 72: MG_Remote: Refuse{Authentication} MOBILEGL_IPC_TOKEN is 13 bytes and the
      minimum is 16 (PH-7 (3)); refusing to listen on tcp://127.0.0.1:46545 rather than authenticating with
      a guessable secret
```

即：改前 13 字节令牌起了一个活的监听者；改后进程以名字拒绝。单测 `SocketTransportTest.TcpRefusesToListen
WithATokenShorterThanTheMinimum` 与 `TheTokenComparisonAnswersLengthAndPrefixTheSameWay` 是廉价的那一半。

---

## 2. 片 2：`Welcome.dataNonce`（PH-7 (4)，ID-P7-3）

### 2.1 现场故障

设备窗口 #1：经 Windows `adb forward` 时一个会话的两条连接**乱序**到达；`AcceptPair` 规定「先到的是控制、
后到的是数据」，于是数据连接被当成控制读 Hello，每个子进程等 10 s 后以 67 退出（日志里伴随
`only 1 of 2 connections arrived within 250 ms`）。同一条规则也意味着任何能连到端口的人都能把一条连接
插进窗口、成为别人会话的数据面。

### 2.2 机制

- `protocol.fbs`：`Welcome` 末尾追加 `dataNonce: [ubyte]`；新表 `DataBind { nonce: [ubyte]; }` 追加到
  `CtrlMsg` 末尾。头文件由钉版 flatc 重新生成。
- **wire 指纹随之改变。** 原指纹的输入全是数据面的结构尺寸与编解码摘要，控制 schema 改形状不会动它。新增
  `MOBILEGL_PROTOCOL_CONTROL_REVISION`（`mg_protocol_base.h`，本次 = 1）作为 `AbiFingerprintInputs.
  ControlSchemaRevision` 混入；`SessionHandshakeTest.TheAbiFingerprintChangesWhenAnyOfItsInputsDoes` 钉住
  该输入并加扰动行。实测 `wireFingerprint` 由 `0x7d4470f241d23539` 变为 `0x03c4c293a54b3e08`。
  **设备侧后果——两个方向不对称**（审查 nit 3）：
  - **新 server + 旧 client**：旧 client 仍开两条连接、送旧指纹，新 supervisor 读到 Hello 后以
    `Refuse{WireFingerprint}` 具名拒绝——这是有帧的那个方向。
  - **旧 server + 新 client**（= 手机上的 server **没有**随本提交重装的情形）：新 client 只开**一条**连接；
    旧 supervisor 的 `AcceptPair` 等第二条连接 2000 ms，记 `only 1 of 2 connections arrived within ... ms`，
    把两条都关掉、继续监听——**没有子进程、没有读 Hello、没有任何 `Refuse` 帧**。client 侧的症状是
    `MG_Remote client: no Welcome ... (rc=6)`（`MOBILEGL_ERR_TRANSPORT_CLOSED`，对端 EOF），不是一条
    具名拒绝。看到这条就先查设备上的 APK 是否是本次构建（`device-window-1-runbook.md` §2 第 3 步）。
  - 指纹随 `MOBILEGL_PROTOCOL_CONTROL_REVISION` 变，而 revision 是手改的整数：F 修复轮把
    `sha256(protocol.fbs)` 钉在它旁边（§6.2），改了 schema 不 bump 在单元车道与 flatc-check 里都是红。
- 流程（仅 `tcp://`；unix 端点的第二条连接是 SCM_RIGHTS 套接字，保持 `AcceptPair`——那是本地对端债，
  §6.4）：
  1. client `SocketTransport::ConnectControl` 只连控制连接，发 Hello；
  2. server 子进程在令牌通过后用内核 CSPRNG（`getrandom`，退回 `/dev/urandom`，绝不退回 PRNG）铸 128 位
     nonce，`StreamLink::AttachOwnedDeferred` 先建好私有内存（Welcome 要宣告其尺寸）但**不带 fd、不起
     reader 线程**，发 Welcome（带 nonce）；
  3. client 收到 Welcome 后 `ConnectDataConnection` 开数据连接，第一帧是 `DataBind{nonce}`，随后才挂
     StreamLink；
  4. supervisor 一次只 `AcceptOne`：无活会话时新连接是控制连接、fork 子进程（带一对 hand-off
     socketpair）；有活会话时读新连接的**恰好一帧**（`ReceiveOneFrame` 不多读一个字节——DataBind 后面
     紧跟 StreamLink 自己的帧），是 DataBind 就把 fd 连同所呈 nonce（sideband）经 `FdPassing::SendFd`
     交给子进程，否则照旧 `Refuse{Busy}`；
  5. 子进程里 `ServerSession::BindDataConnection` 是**唯一**比较点（`ConstantTimeNonceMatch`）：匹配者
     `StreamLink::BindDataFd` 起 reader；不匹配者在**那条连接上**回 `Refuse{Authentication} data
     connection nonce mismatch` 并关闭，等待继续——陈旧或抢插的连接结束不了它没能加入的会话。截止时间
     （5 s）到则在控制连接上回 `Refuse{Authentication} no data connection presented the session nonce`；
     控制对端先走则立即结束等待（子进程按 0 退出，不计入 `sessionsFaulted`）。
  6. 无活会话时到达的 DataBind 由子进程具名拒绝：`Refuse{Authentication} data connection names no live
     session`，干净退出 0。
- 250 ms / 2000 ms 的「两连接到达窗口」在 TCP 上不复存在；取而代之的是 nonce 匹配。非 `--serve` 的单会话
  TCP 模式保留监听者，由会话自己 accept 并读 DataBind（`ListenerSource`），比较仍在同一处；F 修复轮起，
  数据连接绑定后监听者随即关闭（§6.1）。
- 仍然**没有**做的：ID-P7-3 说「认证在 fork 之前完成」——本片的令牌检查与 nonce 铸造仍在 fork 出的会话子
  进程里（每会话一进程，不影响绑定的正确性）；把 Hello 读取与认证移进 supervisor 属于 PH-7 (5)「有界的预
  认证工作 + 失败退避」，未做。

### 2.3 red-once（两进程）

探针只跑「数据连接先到、控制连接后到」这一条（新 smoke 的 (a)），server 分别是片 1 的头与本片：

```
RED   (片 1 server) stray: RuntimeError: control channel closed before a complete frame
                    control: ConnectionResetError: [Errno 104] Connection reset by peer
                    supervisor: session pid=1705895 reaped exit=67 sessionsFaulted=1      ← 现场故障原样
GREEN stray: Refuse{Authentication} 'data connection names no live session'（子进程 exit=0）
      control: Welcome，nonce 16 字节
```

smoke 常驻控制（`TcpLane.SupervisorProtocolControls`）：`data_connection_first`（上面这条）与
`stale_nonce`（会话活着时先呈一个取反的 nonce → 那条连接收到 `Refuse{Authentication} data connection
nonce mismatch`，日志 `MGPipe: Refuse{Authentication} data connection nonce mismatch`，随后正确的 nonce
绑定、子进程打出 `ready`）。单测 `SocketTransportTest.TheDataBindIsReadExactlyAndTheStreamBehindItIsLeft
OnTheSocket` 钉住「恰好一帧」与 16 字节宽度。

`integration-tcp`（真 GL client 走新流程）222/222、`integration-magma-tcp` 74/74 保持全绿。

---

## 3. 片 3：PH-8（server 钳制尺寸）

### 3.1 机制

没有改代码：`ServerSession::Accept` 只看 Hello `linkTerms` 的 `dataPlane` / `wireForm`，段按 server 自己的
`SetSegmentSizes` 建，Welcome 宣告的也是这组。准确地说这不是「钳制」而是**忽略、按 server 尺寸**：四个计数
根本没被读去定尺寸（审查 nit 8）；F 修复轮起，请求超过所授条款时 `Accept` 记一条 `MGLOG_W`（§6.3），生产
client 的 Hello 不填这四个数，所以正常会话不会打这条。本片把它钉住：

- 单测 `SessionHandshakeTest.AHelloAskingFor64GiBIsClampedToTheServersTermsAndAllocatesNothingOfIt`：
  Hello 四个窗口各要 64 GiB，Welcome 必须回 server 的 4 KiB 条款，四个 SegmentRef 各 ≤ 64 KiB。
- smoke `hello_asks_64_gib`：两进程下同样的请求，Welcome 的条款 < 1 GiB，且回答它的会话子进程（Welcome
  `serverPid`）在数据连接绑定后（即全部段已存在）`VmPeak` < 64 GiB。实测：maxReply 2097136、cmd 8 MiB、
  stage 32 MiB、event 256 KiB，子进程 VmPeak 237264896。

### 3.2 red-once（变异：`Accept` 改为按 Hello 的条款定尺寸，已还原）

```
RED   unit  SessionHandshakeTest.cpp:294  Accept rc 2 != 0（64 GiB 私有段建不出来）
RED   smoke hello_asks_64_gib: RuntimeError: control channel closed before a complete frame（子进程死于该请求）
GREEN 如 3.1
```

---

## 4. 片 4–6 未做，以及为什么停在这里

计划的停止规则：「某片超过一天就停在上一片」。片 4（D11 五处 + PH-2）的代码改动本身不大，但每一处都要求
**一条在两进程臂上、字节来自对端的负控**，而树上没有任何车道能把一条**越过 client 自身守卫**的畸形记录
送进一个 spawn / TCP server：

- `create_render_state` 的 `Cso.Slot`、`read_pixels` 的 `W×H`（client 在发射前按回复槽尺寸自检）、程序归档
  里的 `ArchiveVector` 计数、`StagedTextureStore` 的 run 与 level extent、`SlotTables` 的 handle slot/gen——
  全都由 client 的编码器生成且被它自己的检查挡住；要到达 server 必须有一个能按需伪造记录的对端（fuzz 臂 2
  「超界计数」一行正是这件事，计划里记在 P7 之后）。
- 另外两处落在簇 F 之外、且会碰 pull 构建：`ProgramArtifactsCodec.cpp` 编进 pull 构建（G1 `.text` 要求新界限
  只能在 `MOBILEGL_BUILD_DISAGGREGATED` 内），`SlotTables.h` 在簇 D 的 `MG_Backend/DirectGLES/` 下，且
  `MGPipeFatalFamily` 目前只有 `UnmigratedVerb` / `RoleViolation`，PH-2 的具名拒绝需要先加一个
  `ProtocolCorruption` 族并在 hook 映射里接上。
- 已核实的一点备查：`MG_Pipe/PipeApply.cpp` **不在** pull 构建里（`build-linux/compile_commands.json`
  无该文件），D11 第一行在那里改不影响 G1。

所以片 4 连同其后的片 5（PH-6）、片 6（PH-1 (3)(4)）按规则未开工。建议下一包先落「对端伪造记录」的两进程
驱动（它同时是 fuzz 臂 2 的骨架），再逐处上界限，每处一条经它红一次的负控。

---

## 5. 门（片 1–3 的头 `a746417a`，包树 SHA；F 修复轮的头见 §7）

| 门 | 基线 `ec46a550` | 头 |
|---|---|---|
| G1 pull `.text` / 符号增删 | `0xa52203` / 0/0 | `0xa52203` / 0/0（`build-linux` 无需重编：簇 F 文件不在 pull 构建） |
| `fatal_census.py` | 79 abort、44 家族词、3 refusal 词、0 未标记 | 同左 |
| `link_ratchet.py --assert-monotone` | 173 | 173（不变） |
| `spawn_lane_parity.py` | 0 错 | 0 错 |
| unit | 2429 | 2433（+4：片 1 两条、片 2 一条、片 3 一条） |
| integration-split | 303 | 303 |
| integration-spawn | 219 | 219 |
| integration-tcp | 222 | 222 |
| integration-magma-tcp | 74 | 74 |

---

## 6. F 修复轮：片 1–3 的 fable 审查（裁定 land with fixes）

审查的 11 条里 should-fix 五条（1–5）、nit 六条（6–11）；下面按提交分。每条都有一次 red-once，原始行在
`~/w7/logs/p7-ph-f-redonce.txt` 的 `F fix round` 段。

### 6.1 修复 1 + 4（`3370833f`）：绑定之后，数据连接的来源随即关闭

**缺陷 1（回归）。** 子进程只在 `BindDataConnection` 里读 hand-off socketpair，数据连接绑定后再也不读；而
supervisor 的 `RouteWhileBusy` 对此后**每一条** DataBind 仍然 `SendFd` 进去。后果：每个 fd 在子进程的接收
队列里躺着、对端**没有任何回复**（不是具名拒绝；改前基线对第二条连接还会回 `Refuse{Busy}`），那条 TCP 连接一直
挂到会话结束；队列到 `net.unix.max_dgram_qlen`（手机内核默认 10；本机 512，但发送端 sndbuf 在 ~288 个
datagram 处先满）后 supervisor **阻塞在 `sendmsg`** 直到会话结束——不再 accept、不再回 Busy、不再 reap。

**缺陷 4。** 非 `--serve` 的单会话 TCP 模式为 `ListenerSource` 留着监听者，数据连接绑定后没人关它：内核继续
完成三次握手进 backlog，第二个 client / 重试要等满 `kSpawnConnectTimeoutMs`（20 s）而不是像改前那样立即
`ECONNREFUSED`。

**修法（同一个钩子）。** `RunSession` 多收一个 `sourceFd`——`--serve` 下是子进程那端 `pair[1]`，单会话下是
监听者——`session.Accept()` 一返回就 `close` 并清掉 `DataConnectionSource`。于是 (1) supervisor 再转发时
`sendmsg` 得到 `ECONNREFUSED`/`ENOTCONN`（`FdPassing::SendFd` 把「对端已走」当答案而不是错误日志），已有的
`Refuse{Authentication} data connection could not be handed to the live session` 按名字打出；`SendFd` 加
`dontWait`（`MSG_DONTWAIT`），队列因别的原因满也拒绝而不是卡住；(4) 单会话的第二个连接立即 `ECONNREFUSED`。

**red-once**（server 还原到 `da313010`，只重建 `.so`）：

```
RED   smoke data_bind_after_bound: TimeoutError: timed out（第一条多余 DataBind 的 receive 就超时——没人回）
RED   probe f1probe.py（qlen+2 条 DataBind 只发不收）: extras opened=288 of 514 后 connect/send 超时；
      answered by name=0 silent=288；此后新开控制连接: CONNECT FAILED TimeoutError（supervisor 卡在 sendmsg）；
      supervisor alive=True
RED   smoke single_session_listener: 绑定后的第二个 connect 成功（监听者还开着）
GREEN smoke data_bind_after_bound: refused=12，每条 Refuse{Authentication} 'data connection could not be
      handed to the live session'；随后新开控制连接 → Refuse{Busy}
GREEN probe: 514/514 具名拒绝、silent=0、新控制连接 → Busy、supervisor alive
GREEN smoke single_session_listener: second_connect=ConnectionRefusedError(111)
```

smoke 常驻控制两条：`data_bind_after_bound`（会话 `ready` 后 12 条多余 DataBind 各得具名拒绝、再开控制连接仍
Busy）与 `single_session_listener`（无 `--serve` 的 supervisor，`ready` 后 connect 必须 `ECONNREFUSED`）。
`held_session` / `pair` 共用 `connect_control`（启动期 `ECONNREFUSED` 重试 5 s）。

### 6.2 修复 2（`75f24db0`）：`sha256(protocol.fbs)` 钉住 revision

revision 是手改整数，改了 `protocol.fbs` 不 bump 就回到「新旧对端指纹相同、字段静默缺失」。
`scripts/ci/protocol_revision_pin.py` 持有表 `{revision → sha256(protocol.fbs)}`
（`protocol_revision_pins.json`，行尾折成 LF 再哈希），当前 revision 没有行、或行与文件摘要不符都红；`--write`
在 bump 后记新行，且拒绝把已有 revision 改钉到另一个摘要（那正是「改 schema 没 bump」的样子）。注册为
`ProtocolSchema.RevisionPinsDigest`（label unit，`--self-test` 先证明两种扰动都红才说绿），并作为 CI
`flatc-check` 作业的一步——正是每次 schema 改动都要重新生成头文件的那个作业。

```
RED   protocol.fbs 末尾加一行注释、revision 仍为 1:
      ::error::protocol.fbs has sha256 a7b5c7fb888d2926... but MOBILEGL_PROTOCOL_CONTROL_REVISION is still 1,
      which is pinned to 7fc859f7fa756818...   rc=1
GREEN 还原: protocol revision pin: revision 1 holds (sha256 7fc859f7fa756818..., 1 revision(s) pinned,
      self-test red on both perturbations)   rc=0
```

### 6.3 nit 6 / 8 / 9 / 10（`2b2a0ae0`）

- **nit 6** `AuthToken.h`：改前循环长度 = 本端令牌长度，响应时间正比于**秘密**的长度而头注释写得像什么都不泄。
  现在循环固定 `kAuthTokenCompareBytes = 64`（更长的令牌延到其长度，只泄露「长于 64」），两端越界读零、长度差
  仍并入累加器、不延到对端长度（不让对端选我们的工作量）。`SocketTransportTest.TheTokenComparisonAnswersLength
  AndPrefixTheSameWay` 加 70 字节令牌最后一字节判定的用例。
- **nit 8** `ServerSession::Accept`：请求超过所授条款 → 一条 `MGLOG_W`：`Hello asked for windows the server
  does not grant (cmd=68719476736 ...); the ask is ignored and the session is server-sized (cmd=8388608
  stage=33554432 event=262144 maxReply=2097136) - PH-8`。单测 §3.1 那条现在要求这一行（改前红：行不存在）。
- **nit 9** `ServerSession::Accept`：`MintNonce` 失败先在控制连接上回 `Refuse{LinkTerms} stream data plane
  unavailable: the server could not mint a data nonce`，再返回原错误（仍是 67、仍计入 `sessionsFaulted`——
  CSPRNG 给不出字节是 server 的故障不是对端的条款）。没有驱动：不注入故障造不出 `getrandom` + `/dev/urandom`
  同时失败，记录于此。
- **nit 10** `RouteWhileBusy`：有活会话时首帧 magic 错或超 1 MiB（`ReceiveOneFrame` 回 `PROTOCOL_MISMATCH`）
  改回 `Refuse{MalformedHello} first frame is not a control frame`（`Accept` 对不可验证 Hello 用的同一个词），
  关闭前先把对端已发的字节读掉免得 RST 冲掉拒绝帧；格式正确但不是 DataBind 的帧仍是 Busy。smoke 常驻控制
  `garbage_first_frame_while_busy`（magic `XXXX` → code 8；改前红：code 7）。普查仍 3 个 refusal 词（新拒绝
  走枚举值，源码里没有新字面量）。

### 6.4 记录债（本轮补记；CONTRACT-P7 §12 同步）

- **unix 端点仍按到达顺序配对**（`AcceptPair` 的 250 ms / 2000 ms 窗口）。`SocketTransport.h` 把它写成
  「同用户 rendezvous、别人够不着」——但抽象命名空间套接字**没有 mode bits**，「只有本 app 能连」这条性质靠的是
  Android 的 SELinux 域隔离，不是传输层自己。本地对端债，不在 Ph 的远端威胁模型里，未做。
- **两处 wave 2-F 之前就存在的预认证暴露**（审查核实、归 PH-7 (5)）：(a) supervisor 对每条 TCP 连接**先 fork
  再读 Hello**，未认证对端每连接可让 server 付出一次 fork + 最长 10 s 的 Hello 等待；(b) `ValidatePeerHandshake`
  在 `AuthenticatePeerToken` **之前**跑，`Refuse{ProtocolVersion|WireFingerprint|BuildFingerprint}` 把本端
  wire 指纹与 build stamp 送给了未认证的对端。**（P7 F2 包 f2-auth 已关 (a)(b)**：supervisor 在 fork 前自己读首帧、
  先验令牌，待认证工作有期限、有上限、按地址公平让位，失败按地址退避；unix 端点的「先 fork 再读 Hello」仍在，
  归本地对端债。旋钮与留下的债见 CONTRACT-P7 §12 末条、`MG_Remote/Server/PreAuthGate.h`。**）**
- 令牌最小长度只在 TCP 监听处检查（§1.1）。

---

## 7. 门（F 修复轮的头 `2b2a0ae0`，包树 SHA；F-d 只改文档，二进制同此头）

读数 `~/w7/logs/p7-ph-f-fgate.log`、车道日志 `~/w7/logs/p7-ph-f-ffinal-*.log`。

| 门 | 片 1–3 头 `a746417a` | F 修复轮头 |
|---|---|---|
| G1 pull `.text` / 符号增删 | `0xa52203` / 0/0 | `0xa52203` / 0/0（`build-linux` 无工作；`compile_commands.json` 里 `MG_Remote` 条目 0——簇 F 文件不在 pull 构建，`mg_protocol_base.h` 只改注释） |
| `fatal_census.py` | 79 abort、44 家族词、3 refusal 词、0 未标记 | 同左（新拒绝走枚举值，无新字面量） |
| `link_ratchet.py --assert-monotone` | 173 | 173 |
| `spawn_lane_parity.py` | 0 错 | rc 0（split 193 / spawn 190 / tcp 190） |
| `protocol_revision_pin.py --self-test` | — | revision 1 holds，两种扰动均红 |
| G14 名集合 | — | 对 `~/w7/p7-before` 基线 removed=1（`CapsMirrorTest.APlaceholderMirrorConsumesNothing`，在本包基线 `da313010` 上已不存在，是更早的 P7 波次退的）、added=2819；本轮只增 `ProtocolSchema.RevisionPinsDigest` |
| unit | 2433 | 2434（+1：`ProtocolSchema.RevisionPinsDigest`；`SessionHandshakeTest` / `SocketTransportTest` 的新断言在既有用例内） |
| integration-tcp | 222 | 222（`TcpLane.SupervisorProtocolControls` Passed，evidence 17 项含 `data_bind_after_bound` / `garbage_first_frame_while_busy` / `single_session_listener`） |
| integration-spawn | 219 | 219 |
| integration-magma-tcp | 74 | 74 |
