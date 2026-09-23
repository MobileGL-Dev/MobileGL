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

| 片 | 项 | 提交 | 状态 |
|---|---|---|---|
| 1 | PH-7 (1)(2)(3)：常量时间令牌合一、≥16 字节、无令牌只 loopback、smoke 入 CI | `2975a337` | 落地 |
| 2 | PH-7 (4)：`Welcome.dataNonce` 把数据连接绑定到已认证的控制连接 | `596b34fd` | 落地 |
| 3 | PH-8：Hello 要 64 GiB → server 钳到自己的条款且不分配 | `a746417a` | 落地 |
| 4 | D11 五处上限 + PH-2 | — | **未做**，见 §4 |
| 5 | PH-6 drop-with-latch | — | **未做**（顺序在 4 之后） |
| 6 | PH-1 (3)(4) | — | **未做**（顺序在 4 之后） |

red-once 原始行汇总在 `~/w7/logs/p7-ph-f-redonce.txt`。

---

## 1. 片 1：令牌策略合一（PH-7 (1)(2)(3)，ID-P7-3）

### 1.1 机制

`MOBILEGL_IPC_TOKEN` 原来有三种读法：`SocketTransport.cpp` 的 `ListenTcp` 只问「非空」，所以一个字节
的令牌也能打开 `tcp://0.0.0.0`；`ServerSession::Accept` 用 `std::strcmp`；`ServerMain::RunSession` 用
`std::string::operator==`，且在 server 没配令牌时反而要求对端送空令牌。两处比较都在第一个不同字节处返回。

- 新文件 `MG_Remote/Transport/AuthToken.h`：`ConfiguredAuthToken()`（空值 = 未配置）、
  `kMinimumAuthTokenBytes = 16`、`ConstantTimeTokenMatch`（循环长度只取本进程自己的令牌，长度差并入累加器，
  嵌入的 NUL 参与比较）。
- `Handshake.h` `AuthenticatePeerToken`：**唯一**的策略函数，`ServerSession::Accept` 与 supervisor 子进程
  （`ServerMain::RunSession`）都调它。不配置令牌时不因对端送了令牌而拒绝——策略由监听侧的 loopback 限制负责。
- `ListenTcp`：配置了但不足 16 字节的令牌**让监听失败**（具名 `Refuse{Authentication} ... minimum is 16`，
  进程退出 72），而不是悄悄退化成 loopback server。令牌值从不进日志。
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
  **设备侧后果**：旧 server 与新 client（或反之）现在以 `Refuse{WireFingerprint}` 拒绝，设备上的
  server 需要随本提交重新部署。
- 流程（仅 `tcp://`；unix 端点的第二条连接是 SCM_RIGHTS 套接字，保持 `AcceptPair`）：
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
  TCP 模式保留监听者，由会话自己 accept 并读 DataBind（`ListenerSource`），比较仍在同一处。
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
`SetSegmentSizes` 建，Welcome 宣告的也是这组。本片把它钉住：

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

## 5. 门（头 `a746417a`）

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
