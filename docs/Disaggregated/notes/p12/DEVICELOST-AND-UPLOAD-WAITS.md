# device-lost 的回复容忍与纹理上传的等待粒度（handoff §4 / §5.1 落地记录）

> 2026-09-25。分支 `feat/disaggregated` @ `c984d952` 加本轮工作树改动。对应会话语料 
> [`MOBILEGL-CS-HANDOFF.md`](../../../../MOBILEGL-CS-HANDOFF.md) 的 §5.1（A：进世界后的崩溃）与 §4（B：纹理上传等待粒度）。

## 0. 一句话

- **A**：客户端不再把 **DECLINED 回包**当作协议损坏。那正是进世界后 `Fatal{ProtocolCorruption, "Fence.reply"}`
  （rc=134）的来源：device-lost 闩锁之后每个 verb 按设计 DECLINED，而 fence/query 两个消费点把它和 ERROR 折进了同一个 abort。
- **B**：wire 臂上纹理那半 `resource_subdata` 不再逐条买回包，等待点从「每条记录」落到「SEG_STAGE 窗口用尽」。
  一帧 55,428 次回包等待（handoff §3.2 的实测）在机制上归零；真机端到端墙钟仍待复测（§3）。

## 1. A —— 一个 DECLINED 是答案，不是损坏

### 1.1 根因（三处，都是同一个折叠）

`ReplySink` 的状态空间是三值的，且它自己的头文件就写着这句话：**「DECLINED IS A REAL ANSWER, not a failure」**
（`MG_Remote/Wire/PipeWireCodec.h:457-460`）；escape 那几行把另一半也说死了：**「ERROR IS NOT A DECLINE … an escape may not fold 2 into `false`」**
（`Client/WireTables.cpp:446-450`）。下面三处把 1 和 2 折成了一个 abort：

| 位置 | 折叠写法 | 真实后果 |
|---|---|---|
| `Client/EmitTables.cpp` `ReadFenceReply` | `status != kStatusOk \|\| replyBytes != sizeof(result)` → `WireProtocolFatal("Fence.reply", …)` | 进世界后区块上传期的 `glClientWaitSync` 撞上 device-lost 闩锁 → **Minecraft 26.2 崩溃（§5.1）** |
| `Client/QueryEmit.inc` `ReadQueryReply` | 同上 → `WireProtocolFatal("Query.reply", …)` | 同类：QueryAvailable / QueryResult / QueryTimestamp 三条 row 都会被它打死 |
| `MG_Impl/Pipe/PipeFill.cpp:1115` 自检 | 只比客户端记录序号 | device-lost 后 `resource_subdata` 按设计不发 → 自检误判「字节没送达」→ `Fatal{UncarriedInitialBytes}`，把**干净的 device-lost** 变成尸体（P12 出口门 (a) 卡在这条） |

DECLINED 有三条**完全不含损坏**的产生路径，两条在服务端、一条在客户端自己：

- 服务端按设计拒绝：`Wire/PipeWireCodec.cpp:1820-1830`（fence 两条 row）、`:1841-1861`（query 三条 row）——`PostReply(ok ? kStatusOk : kStatusDeclined, ok ? &result : nullptr, ok ? sizeof(result) : 0)`，
  即一个**零长度**的合法答案；
- 客户端自己：`Client/ClientSession.cpp:1837`（device-lost 闩锁后每个 verb DECLINED）与 `:2025`（teardown 中死掉的门铃，注释原文就写着「报 DECLINED 而不是 ERROR」）。

而设计文档对 device-lost 的要求恰好相反：**「GL 调用变空操作、交换缓冲返回上下文丢失，而不是崩溃或卡死」**
（`design/08-runtime-and-platform.md:33`、`guide/glossary.md:18`）。前端自己也早有这个空操作：
`MG_Impl/GLImpl/Sync/GL_Sync.cpp:103-106`、`MG_Backend/DirectGLES/DirectGLES.cpp:16489`、`Server/PipeApplier.cpp:175-177` 都对「等不到的 sync」答 `GL_ALREADY_SIGNALED`。

### 1.2 改法

- **fence**：DECLINED → `FenceWait` 答 `GL_ALREADY_SIGNALED`、`FenceStatus` 答 1（signaled）。ERROR 与「OK 但短」**仍然是**原来的具名 Fatal。
- **query**：DECLINED → 该 row 自己的「还没有答案」值（零值即三者皆是：availability=false、`Produced=0`→false、timestamp=0）。ERROR 与短 OK 仍然 Fatal。
- **自检**：只在**会话没闩 device-lost** 时保持全效力（`&& !ClientSession::DeviceLost()`）。活会话上一个「发了却什么都没发出去」的 follow-up 仍然是它命名的那个 R-13.3 缺口。
- **不动的**：`ReadPixels` 的 `Fatal{ReadbackDeclined}` 是**故意的**（阻塞回读没有像素可还），本轮没碰，`ClientSession.cpp:2023-2024` 早就把它点名钉住了。

### 1.3 门与 red-once（R-16，真跑过一次红）

`MG_Test/Wire/RemoteClientControls.inc` 四个用例，全部在**活会话 + 对抗性 peer**上驱动生产消费点：

| 用例 | 断言 | 去掉修复后的红（实测） |
|---|---|---|
| `DeclinedFenceReplyAnswersSignaledInsteadOfAborting` | DECLINED 的 fence 回包 → `GL_ALREADY_SIGNALED` / `GetSyncStatus()==true` | `Fatal{ProtocolCorruption, "Fence.reply"} missing or malformed sync result`（**就是 §5.1 那条崩溃**） |
| `DeclinedQueryRepliesAnswerNoAnswerYetInsteadOfAborting` | 三条 query row → false / 未写出的 out 参数 / 0 | `Fatal{ProtocolCorruption, "Query.reply"}` |
| `ErrorFenceReplyStillRefusesByName` | **宽容不是一刀切**：ERROR 仍然具名 Fatal | （反向控制，修复前后都红/绿相反） |
| `DeviceLostRespecifyDoesNotDieOfTheUncarriedBytesCheck` | 闩锁后 `glBufferData(size,data)` 不再自杀 | `Fatal{UncarriedInitialBytes, "resource_respecify"} … 64 bytes of initial content exist on no side of the wire` |

绿：`RemoteClientControls` 30/30、`RemoteClientTest` 全量 **166/166**。

## 2. B —— 纹理上传不再逐条等回包

### 2.1 机制：一条谓词，两个半边

`MG_Pipe/PipeRoute.h` 的 `MGPipeSubDataWantsItsReply` 是**唯一**一处决定「这条记录要不要等回包」的地方（`MG_Remote/Client/WireTables.cpp:370` 与 `PipeRoute.h:328` 都读它，注释明写「a second spelling of it is how the client comes to wait for an answer the emitter told the server not to bother with」）。
本轮给它加了 wire 臂的一支：**客户端在 wire 臂上不再为纹理那半等回包**。

答案在 wire 臂上**是可推导的**，理由不是「没人读它」，而是三条站着的事实：

1. **发射本来就被答案的内容闸住了**。纹理记录只在 `FamilyIsLive(kMGPipeSubsystemTextureResources, …)` 成立时才产生，
   而在 split 下那个闸**就是** `CapsMirrorInstance().ServerConsumes(bit 10)`（`PipeFill.cpp:1719-1740`）——
   和服务端自己的验收闸（`PipeApply.cpp:1408-1419` `NoP4aConsumer`）读的是同一个已发布事实。
   于是验收当初存在的唯一理由（「服务端没有消费者」，ID-39 / D-D5 / ID-18 M3）**不可能是**这条记录的回答。
2. **剩下的是 bug 类，而且本来就响**：target 不指纹理、没声明 texel、没字节 → 服务端 `MGP_TRIP_WIRE_REPORT`（`PipeApply.cpp:1201-1218`、`:2245-2252`）；
   handle 不存在 → 计数并具名（`ResolveResourceIn`，`:721-728`），而客户端自己的 tracker 靠「先 create 再 subdata」从源头杜绝（`PipeFill.cpp:1045-1051`）。
3. **客户端还能免费拿到的答案，一条都没丢**：发射器照旧读这条路返回的 Bool，所以**被客户端自己取消**的记录（device-lost、stage 用尽）仍然答「未接受」，
   level 保持 dirty 并重试——D-D5 的安全方向原样不动。

**等待没有消失，它换了地方**：一条 run 放不进 SEG_STAGE 时会回收已退休字节并 park 在 stage 门铃上（`Wire/PipeWireCodec.cpp` 的 `StageAllocate` / `ReclaimStagedBytes`），
那是 run-ahead 客户端唯一欠的等待，且它正比于**窗口**而不是调用次数。

### 2.2 门与 red-once

`RemoteClientControls.TextureUploadsStopOwningAReplyOnTheWireArm` 用的是 handoff §3.2 的**同一个计数器**：
`LinkMetrics.cpp:76-80` 的 `wait_replies`（逐帧 `P65LinkMetrics` 行里那个字段，也就是「一帧 55,428」那个数）。本轮为它加了同进程读回的 `LinkMetricsReplyWaits()`，因为「等待没了」如果只能靠计时观察，那就只是实测、不是门。

| 步骤 | 断言 |
|---|---|
| 控制行：`glTexImage2D`（`resource_create` 是 kWaitReply） | `wait_replies` **必须**增加——否则下面的 0 会在「什么都没计数」的会话上假绿 |
| 被测行：`glTexSubImage2D`（纯 `resource_subdata`） | 记录真的过了线（`ClientWireRecordsEmitted()` 增加）**且** `wait_replies` 增加 **0** |

红（实测）：把 `kClientWire` 那一支拿掉 → 子进程 exit 142。
**monolith 不变**由既有的 `RemoteRunAhead.OnlyTheTextureHalfOfResourceSubDataWantsItsReply` 继续钉住（臂是 `kNone` 时仍答 true）。

### 2.3 G1

谓词里新增的那一支**在 `#if MOBILEGL_BUILD_DISAGGREGATED` 内**，fall-through 是原来那一行表达式；
其余改动全部落在 split-only 的翻译单元（`MG_Remote/**`）或 `PipeFill.cpp` 的 `#if MOBILEGL_BUILD_DISAGGREGATED` 块内。
因此 pull 臂预处理后与改动前**逐字符相同**。**本机已按 G1 跑过 A/B**：pull 配置（Release、`/usr/bin/c++`、`MOBILEGL_BUILD_DISAGGREGATED=OFF`、`--target MobileGL`）在 `c984d952`（基线）与 `3b9f536d`（本轮）各建一次，结果 `.text` **13872002 = 13872002** 字节、符号 **31746 / 31746**、**0 增 0 删**，逐符号 `comm` 无任何差异（两棵树留在 `build-g1-base/`、`build-g1-head/`，符号表在各自的 `syms.txt`）。

### 2.4 「同类第二条」到底该不该等（handoff §4.3 末条 + §5.4）

handoff §5.4 说「55,428 次等待里各 op 各占多少」未量化，§4.3 说 `ResourceCreate` / `SetTextureParams` 是同类第二条、"需单独分析"。本轮把两件事一起做了：
给 `LinkMetrics` 加了**按 op 的回包等待计数**（`LinkMetricsBeginReply(wantsReply, op)` / `LinkMetricsReplyWaitsFor(op)`，与总计数一样只在 `MOBILEGL_PIPE_STATS=1` 时记），
并用它量了拼接器会产生的两种形状（`RemoteClientControls.ReplyWaitsByOpForAnAtlasShapedLoad`，先打印后断言，好让红的那一次也留下数字）：

| 形状 | 没有 B | 有 B |
|---|---|---|
| **图集拼接**：一张 level 上 64 次 `glTexSubImage2D` | **64** 次回包等待 | **0** |
| **每 sprite 一张纹理**：64 张纹理 + 256 次 `glTexParameteri` | **384**（`ResourceSubData` 占 128） | **320**（`ResourceCreate` 76 + `ResourceRespecify` 130 + `SetTextureParams` 130，sub-data 0） |

结论两条：

1. **B 正好打在它要打的地方**：上传密集的形状（MC 的图集拼接就是这种）里，回包等待从「每次上传一次」变成 **0**——handoff §4.2 的预期在机制层面成立，
   与「96 MiB ÷ 32 MiB 窗口 ≈ 3 次等待」是同一件事的两面（剩下的是 stage 退休，不再是回包）。
2. **剩下三个 row 不能照搬 B**，因为它们等的不是「你收下这批字节了吗」，而是**服务端的记录表里还有没有这个 handle**：
   - `RespecifyOnce`（`TextureEmit.h:1332-1356`）原文：「the applier's REFUSAL is the only signal that says 'I hold nothing for this handle'」——
     served context 的 teardown（`MGPipeApplierReleaseObjectRecords`）会丢掉对象记录而前端对象还活着，这件事**客户端本地推不出来**；
   - `EmitTextureParams`（`:991-1017`）用同一个拒绝做自愈触发器（context 的默认纹理就是这条路径救回来的）；
   - `PublishCreate`（`:1303-1319`，D-I1/c0b）不能在「被拒绝的 create」上落发布闩，否则死亡路径会为一个不存在的记录发 `resource_destroy`。

   所以这三条按 per-resource / per-call 结算，不是 per-chunk：**删掉等待就是删掉信号**。这条裁定已写进 `CONTRACT-P5E.md` §2.5。

**真机那一轮会直接在日志里看到这张表**：`LinkMetricsEnd` 现在把按 op 的拆分单独打一行
`P65LinkMetrics kind=per-op wait_replies=N by_op=2=…,3=…,47=…`（id 是 `MGPWireOp`：2 create / 3 respecify / 47 params / 48 sub-data）。
handoff §5.4 说过这条数字拿不到——spawn 下 client 不推进帧窗口、逐帧行不打，SIGKILL 又带不走退出时的 dump——而这一行正是从「正常收尾的客户端一定会走到」的那个位置打出来的。


## 2.5 真机结果（2026-09-25，两台设备上的第二次复现）

> **设备不是 handoff 那台手机**：本轮 `adb` 上的是 `aikjs4s8cys8ba7l`（TrebleDroid GSI，mt6893/Mali，Android 14，GLES 3.2，`192.168.1.178`），
> 不是 `90cee93`（SM8650/Adreno 750）。所以**绝对数字不可与 handoff 直接比**，可比的是机制与每条 row 的行为。
> 服务器 APK 用仓库里那份 trace release 自签后装（`mgl-device/mgl-local.jks`，一次性），客户端是本轮 `build-split`，
> 控制面 `tcp://192.168.1.178:40613`、数据面 stream、`MOBILEGL_IPC_SURFACE=server`，MC 26.2 + NeoForge 用 `--quickPlaySingleplayer` 直接进世界。

### A —— §5.1 的崩溃路径在真机上被走过了两次，都没有崩

两次运行里设备端的窗口都在会话中途消失（设备日志：`Fatal{ServerWindowLost, "surfaceDestroyed"}`），客户端随即闩上 device-lost，
而**下一条 `FenceWait` 收到的就是 DECLINED**：

~~~
[20:42:45] [Render thread/ERROR]: MGPipe: DEVICE LOST - the barrier woke on a peer that had hung up …
[20:42:58] [Render thread/ERROR]: MG_Remote client: the peer DECLINED a FenceWait reply - the verb did not
                            happen (a lost device, or a teardown); answering the no-op the frontend uses
                            for a fence it cannot wait on
~~~

这就是 handoff §5.1 那条崩溃的完整前因后果：**修复前这一行是 `Fatal{ProtocolCorruption, "Fence.reply"}` → rc=134**。
两次运行客户端日志里的 `Fatal{` 计数都是 **0**；世界也确实进去了（`Saving chunks for level 'ServerLevel[新的世界]'`、
`Changing view distance to 2, from 10`、`Started 10 worker threads` —— 与 handoff 记录的崩溃上下文同一组）。

### B —— 在真机上确实生效，但它不是那台帧的主因

第一次运行复现了 handoff §3.2 的那种「一帧吞掉全部等待」：

| | handoff（**没有** B，90cee93） | 本轮（**有** B，GSI） |
|---|---|---|
| 那一帧的 `wait_replies` | **55,428** | **43,232** |
| `stage_bytes` | 100,684,804（96 MiB） | 101,421,396（96.7 MiB） |
| `wall_ms` | 248,996 | **224,780** |
| `cpu_ms` | 18,391 | 13,413 |

两轮的 stage 字节几乎一样（形状相同），但**这不是受控 A/B**（两台设备），所以能下的结论只有一条，而它是直接证据：
第二次运行带了逐帧按 op 的拆分行，启动期那两帧是 `2=50,3=68,47=62`、稳态是每帧 1 次（op 9 = `FenceWait`，就是帧围栏），
**`ResourceSubData`(48) 一次都没有出现** —— B 在真机上按设计生效。

**handoff §4.2 的「55,428 → 约 3」不成立**：那一帧剩下的 43,232 次等待正是 §2.4 裁定「必须等」的三条 row（create / respecify / params），
按 96.7 MiB ÷ 每 sprite 一次上传估算，约每 sprite 3.5 次非上传等待。**B 是必要的，但不是那 2–3 分钟停滞的充分解**——
要动那一段，方向不是「把等待去掉」（那会删掉信号），而是**把记录数压下来**（同样的参数不必每条都发、同一 level 的 respecify 可去重）。这一条留给下一轮。

### 2.6 那一帧到底由什么构成（真机，`kind=frame-op` + `kind=frame-sent`）

第四次运行抓到了同一形状的那一帧（`wait_replies=43,232`、`stage_bytes=101,421,396`），两条行合起来第一次把「付了什么」和「发了什么」对齐：

| op | 发的记录 | 付的等待 | 说明 |
|---|---|---|---|
| `ResourceRespecify` (3) | 20,633 | **20,633** | 每条一等 |
| `SetTextureParams` (47) | 18,056 | **18,056** | 每条一等 |
| `ResourceCreate` (2) | 4,536 | **4,536** | 每条一等 |
| `ResourceSubData` (48) | 12,671 | **0** | ⭐ B：记录照发，回包一个不等 |
| `DrawVbo` (59) / `SetShaderBuffers` (38) / `SetSamplerViews` (35) | 9,121 / 9,120 / 9,122 | 0 | 这些 row 的等待是 barrier 类，不在本计数里 |
| `ObjectDeath` (78) / `ResourceDestroy` (4) | 7,515 / 3,770 | 0 | 死亡路径不发问 |

**每条等待正好对应一条记录**（三个 row 都是 1:1），所以 43,232 不是"等重了"，而是"真的发了 43,232 条会问问题的记录"。

### 2.7 「把记录数压下来」这条路的结论：客户端没有可去重的记录

§3 上一轮把「少发记录」列为头号方向。本轮先做了一个**冗余探针**（`RemoteClientControls.ReplyWaitsByOpForAnAtlasShapedLoad` 里那段 `params probe`），
再去读设备上的实际计数，结论是否定的：

~~~
params probe: first=1 same-again=1 changed=2 (deltas 1/0)
~~~

- **同一个值再设一次不产生记录**（delta 0）——前端 setter 的冗余过滤 + emitter 的版本闩已经把它挡掉了；
- 换一个值才产生一条。

所以设备上那 18,056 条 `SetTextureParams` 是**真实的状态变化**，不是重复；`ResourceRespecify`/`ResourceCreate` 同理（respecify 的 `unchanged` memcmp 去重早就在了，`TextureEmit.h:838/858/865`）。
**结论：客户端侧没有"少发记录"的空间**——剩下的唯一杠杆是「同样多的记录，更少的往返」，也就是把等待批起来（同一条 row 每 K 条等一次）或把单次往返的成本降下来（数据面 stream 的 syscall/wake 路径），
不是继续找冗余。顺带确认：`TCP_NODELAY` 已经在 `SocketTransport.cpp:100` 设了，5 ms 不是 Nagle 造成的。
### 2.11 首次使用确认落地后的真机 A/B：那一帧 221.5 s → 134.4 s

按「确认推迟到该对象首次被使用」实现（提交 `b6270960`）：发射端在发送参数前查发布闩，未发布就先补一次**阻塞的** create
（`HealUnpublishedRecord`，即把原来"被拒后才跑"的那段抽出来前移）；对象一旦确认，参数记录全部 fire-and-forget，
调用方读到的是 accept-by-construction（与 B 的 `resource_subdata` 同形）。wire 端读**同一个闩**决定要不要开回复槽，
所以两半不会对"是否欠一次等待"给出不同答案。

同一台设备（GSI，出厂省电策略、服务端 spin 默认 50 µs、Wi-Fi LAN），同一份工作量：

| | 那一帧 waits | `by_op` | staged | 墙钟 |
|---|---|---|---|---|
| 改动前（frame 12） | **43,232** | `2=4536, 3=20633, 5=6, 9=1, `**`47=18056`** | 101,421,396 B | 221.5 s |
| 改动后（frame 25） | **25,176** | `2=4536, 3=20633, 5=6, 9=1` | 101,421,396 B | **134.4 s（−39%）** |

**`47`（`SetTextureParams`）整条从等待里消失，其余每一行一个不差**，staged 字节也完全相同——所以这 87 秒是"少买了 18,056 次往返"，
不是别的什么变了；帧内 `rtt_mean` 5,040 → 5,176 µs（单次成本没动），正是"次数变了、单价没变"的形状。

**为什么不需要周期性金丝雀**：确实"服务端是否持有这个 handle"客户端本来就知道——`MGPipeHandleIsPublished` 在 create **被接受**时才置位，
而 applier 跨 make-current 保留纹理记录（`PipeFill.cpp:3756-3759`，这条注释直接否掉了"在 applier_reset 清闩"的做法：那会让每次 make-current 都无谓重发）。
所以确认只在**首次使用**付一次（那一次 create 本来就要发），之后每条记录都不必再问。兜底仍是老路径：
create 被拒 → 闩不置位 → 下一条参数记录照样阻塞取真答案 → 原来那段 one-retry 自愈照跑。

**下一个 20,633**：`ResourceRespecify` 仍逐条等（在 frame 25 里占 20,633/25,176 = 82%）。它与 params 同形（答案只用于 `NoteRespecified` 与自愈），
"对象已确认"这个前提同样成立——按同一条规则处理，估算落在 **~4.5k waits ≈ 24 s** 量级（create 那 4,536 是确认本身，应当保留）。

## 3. 剩余（下一轮的直接入口）

1. **把记录数压下来（新的头号项）**。§2.5 的真机数字说：那一帧剩下的 43,232 次等待全在「必须等」的三条 row 上，
   约每 sprite 3.5 次非上传记录。**这条已经被本轮证否（§2.6/§2.7）**：记录是真实的，客户端没有可去重的空间。
   于是唯一剩下的杠杆是「同样多的记录、更少的往返」，两个候选：
   - **把等待批起来**：同一条 row 每 K 条等一次（回复槽池是 8 个，所以 K ≤ 8 是可证明安全的），
     代价是消费点的答案要能延后兑现——`ResourceCreate`/`ResourceRespecify`/`SetTextureParams` 的答案只用来「落闩 + 自愈」，两者都可以延后到下一个同步点；
   - **降低单次往返的 5 ms**：`TCP_NODELAY` 已设（`SocketTransport.cpp:100`），所以不是 Nagle；
     数据面是 `stream`（socket），服务端 profile 曾经 65% 在 socket syscall —— 下一轮该重新采一次两侧的栈（handoff §3.3 的 `perf` + `simpleperf` 跑法仍然有效），
     把「5 ms 里有多少是网络、多少是两侧 wake/调度」分开。
2. **受控真机 A/B**：本轮两台设备不同（GSI vs handoff 的 90cee93），55,428 vs 43,232 只能算旁证。要给出可比的数字，需要在**同一台设备**上跑一次带 B、一次不带 B（工作树里已有 `build-split/fix-b2.patch` 的往返做法）。
3. ~~真机端到端~~ **已完成（§2.5）**：A 的崩溃路径在真机上走过两次都没崩；B 在真机上按设计生效（op 48 零等待）。工具在仓库外的 `mgl-device/`：
   `launch-mc.py`（按版本 JSON 重建启动命令，支持 `MC_QUICKPLAY` / `MC_GAMEDIR`）、`gl-aliases.sh`、`start-phone-server.sh`、`mkeystore`+签名步骤写在 README 里。
2. ~~同类第二条~~ **已分析完毕（§2.4）**：`ResourceCreate` / `ResourceRespecify` / `SetTextureParams` 保持 kWaitReply 是有据的裁定，
   不是遗漏——它们的拒绝是「服务端没有这个 handle」的唯一客户端可见信号。**这条不再是待办。**
4. **主机门的环境前提**：本机 shell 沙箱没有 `/dev/dri`，所以任何需要真 EGL pbuffer 的车道（`SpawnLane.EventForfeitPeer`、`TcpLane.EventForfeitPeer`）在本会话里必然红（子进程 `kEglSurface = 12`），与本轮改动无关；`EventForfeitPeerTest.cpp:134` 的注释本来就写明这种 runner 会「every case reds before the ring ever fills」。


### 2.8 那 ~5 ms 一次往返，到底花在哪：**不是链路**

handoff §3.6 留过一个没解释的现象：把链路从 Wi-Fi 换到 USB（裸往返 5.76 ms → 1.70 ms，快 3.4 倍）墙钟几乎不动（3m15s → 3m03s）。
本轮在同一台设备上把这件事量清楚了——**同样的帧号、同样的记录数**，只换客户端连的地址（LAN ↔ `adb forward` 的 `127.0.0.1`）：

| frame | Wi-Fi waits | Wi-Fi `rtt_mean` | USB waits | USB `rtt_mean` |
|---|---|---|---|---|
| 1 | 26 | 5,156 µs | 26 | **3,451 µs** |
| 2 | 180 | 7,934 µs | 180 | **5,716 µs** |
| 3 | 1 | 3,491 µs | 1 | **2,544 µs** |
| 4 | 1 | 8,106 µs | 1 | **6,579 µs** |

链路（含 adb 的用户态转发这一跳）把单次等待降了约 **25–30%**，量级不变。也就是说 **~5 ms 里的大部分不在链路上**，
而在「客户端 park → 服务端唤醒/读取/apply/回铃 → 客户端唤醒」这条本地路径上——这与两侧 profile 的形状一致（服务端 65% 在 socket syscall、约 15% 在门铃自旋）。
**结论：想动这一帧，只有两条路**——(a) 同样多的记录、更少的往返（把等待批起来，回复槽池 8 个 ⇒ K ≤ 8 可证明安全）；
(b) 降低单次往返的本地成本（服务端 apply/wake 路径、`MOBILEGL_IPC_SPIN_US` 之类）。**不是**换链路，也不是去重记录（§2.7 已证否）。

### 2.9 那 ~5 ms 的真身：**设备侧的深度空闲退出**（本轮把 handoff §3.6 的谜题解开了）

§2.8 排除了链路，本节把剩下的候选一个个排除，最后落在一个可复现、可量化的原因上。同一台设备、同一份工作量（那一帧 43k 条记录），只改一个变量：

| 变体 | 单次等待均值 `rtt_mean` | 那一帧的墙钟 | 说明 |
|---|---|---|---|
| 基线（LAN + 客户端 spin 50 µs + 设备出厂省电） | 5.0–7.9 ms | **221.5 s**（43,232 waits / 101.4 MB staged） | |
| 客户端 `MOBILEGL_IPC_SPIN_US=2000`（自旋 40×） | 4.8–6.9 ms | — | 只快 ~10% ⇒ **不是客户端 park/wake** |
| `adb forward`（USB，裸 RTT 低数倍） | ~25–30% 更快 | — | ⇒ **不是链路**（§2.8） |
| **设备 CPU 钉住**：8 核 governor→`performance`，关掉 `cpuoff_l`/`clusteroff_l`/`mcusysoff`/`s2idle` | **3,715 µs**（帧内均值从 5,040 降下来） | **166.5 s**（43,218 waits / 100.5 MB staged） | ⇒ **就是它**：同一条数、同样的字节，**快了 25%** |

**机理**：服务端 apply 线程在两条记录之间 park，SoC 随即掉进 `cpuoff_l`/`mcusysoff` 这类深度空闲；下一条记录到达时要付**退出延迟**（毫秒级）。
每一条记录一次唤醒 ⇒ 每次往返 ~5 ms，与链路无关（§2.8），也与客户端怎么等无关（自旋试验）。

**这同时解释了 handoff 的两处观察**：§3.6「换快 3.4 倍的链路墙钟不变」（因为钱花在设备的唤醒上），以及 §3.3 服务端 profile 里那 65% 的 socket syscall / 15% 门铃自旋（等待与唤醒的形状）。
**也要说清楚**：handoff §3.2 那 249 s 是在**没有**做设备定频/关空闲的机器上取的（项目自己的纪律 `guide/pin-verification-2026-09-07.md` 就是为这件事写的），所以它的绝对值里含这份空闲税。

### 2.9b 同一个诊断的可移植修法：**服务端的自旋预算**（已实测，不需要 root、不需要改代码）

§2.9 的机理直接指向一个已经接好线的旋钮：服务端 apply 线程在记录之间 park 得越早，SoC 越容易掉进深度空闲。
本轮把它调大再测（同一台设备、同一帧、43,232 条记录、101.4 MB staged）：

| 变体 | 最大帧 `rtt_mean` | 墙钟 | 代价 / 前提 |
|---|---|---|---|
| 基线（服务端 spin 50 µs，出厂省电） | 5,040 µs | **221.5 s** | — |
| **服务端 `MOBILEGL_IPC_SPIN_US=2000`** | 4,145 µs | **182.8 s（−17%）** | 设备侧多烧一点自旋 CPU；**不需要 root**、**不需要改代码** |
| 设备定频 + 关深度空闲 | 3,715 µs | **166.5 s（−25%）** | 需要 root/系统权限 |

`MOBILEGL_IPC_SPIN_US` 通过 Activity 现成的 `env` extra 传进去即可（`ServerEnvironment.apply` 的语法是 `KEY=VALUE;KEY=VALUE`，且它不在 `SCRUBBED` 名单里）：

~~~sh
adb shell am start -n top.mobilegl.plugin.trace/top.mobilegl.plugin.MobileGLDisplayActivity \
  --es listen tcp://0.0.0.0:40613 --es token <token> --es backend DirectGLES \
  --es env "MOBILEGL_IPC_SPIN_US=2000"
~~~

服务端日志自己会证明它生效了：`mgl-srv-apply started … spin 2000 us`。**这条已经是可用的缓解手段**；要不要把它做成 Android 上的默认值（一行 env 或一个默认常量）是产品决定：
它拿设备 CPU 换墙钟，而这一帧本来就 94% 在等。

### 2.10 于是「2–3 分钟停滞」的修法清单（按预期收益排序）

1. **把等待批起来**（K ≤ 8，回复槽池可证明）：同样多的记录、少得多的唤醒。与第 2 条互补，且**纯客户端**，不需要设备权限。
2. **让服务端在记录之间保持"热"**：它自己的自旋预算（设备日志里的 `spin 50 us`）——但 APK 今天没有把 `MOBILEGL_IPC_*` 接进去（P12 阶段表里那条 "FCL env 与 plugin APK 开关表接线" 未做），所以真机上只能靠 root 钉频率来证明这块蛋糕有多大（本节已证明：25%）。
3. **测量纪律**：任何 split 真机数字都应该在定频+关深度空闲的设备上取，否则把设备省电策略算进了"协议代价"。
## 4. 证据位置

| 位置 | 内容 |
|---|---|
| `MG_Test/Wire/RemoteClientControls.inc` | 本轮五个用例（四个 A + 一个 B），每个都写了它自己的红是什么 |
| `MG_Test/Wire/RemoteClientTest.cpp` | `RemoteRunAhead.OnlyTheTextureHalfOfResourceSubDataWantsItsReply`（monolith 侧的钉） |
| red-once 命令 | `./build-split/MobileGL/MG_Test/Wire/RemoteClientTest --gtest_filter='*Declined*:*DeviceLostRespecify*:*TextureUploadsStopOwning*'`，配合 `build-split/fix-a.patch`、`build-split/fix-b.patch` 两个临时补丁往返 |
| 全量 | `ctest --test-dir build-split -E "Scenario|EventForfeitPeer"`：2616 例，除上述沙箱 EGL 两条外全绿 |
