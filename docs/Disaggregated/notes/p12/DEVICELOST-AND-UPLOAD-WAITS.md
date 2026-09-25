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

## 3. 本轮**没有**做的（下一轮的直接入口）

1. **真机端到端**：A 需要「进世界不再崩」、B 需要「那一帧墙钟 249 s → 约 20 s」的真机数字。需要重出 APK（`assembleTraceRelease` + 签名）、手机 `90cee93` 起 server、跨机 TCP 起 MC 26.2。
   注意 handoff §1.3：**GL 别名软链（五个）是必需的**，且本会话的 shell 沙箱给 `/tmp` 挂的是**每次命令全新的 tmpfs**，
   所以要在同一条命令里建链再跑。本轮已把这条路径的工具做成持久版本放在仓库外的 `mgl-device/`：
   `launch-mc.py`（按版本 JSON 重建启动命令，已干跑校验：95 条 classpath 条目全部存在、Java 25 由 `javaVersion.majorVersion` 选出）、
   `gl-aliases.sh`（把五个别名建在 `mgl-device/gl-aliases/`，**不放 build 目录**：那里的 `libEGL.so` 会让 server dlopen 自己）、
   `start-phone-server.sh`（装包 + 起 Activity + handoff 那两个就绪判据）。
2. ~~同类第二条~~ **已分析完毕（§2.4）**：`ResourceCreate` / `ResourceRespecify` / `SetTextureParams` 保持 kWaitReply 是有据的裁定，
   不是遗漏——它们的拒绝是「服务端没有这个 handle」的唯一客户端可见信号。**这条不再是待办。**
3. **主机门的环境前提**：本机 shell 沙箱没有 `/dev/dri`，所以任何需要真 EGL pbuffer 的车道（`SpawnLane.EventForfeitPeer`、`TcpLane.EventForfeitPeer`）在本会话里必然红（子进程 `kEglSurface = 12`），与本轮改动无关；`EventForfeitPeerTest.cpp:134` 的注释本来就写明这种 runner 会「every case reds before the ring ever fills」。

## 4. 证据位置

| 位置 | 内容 |
|---|---|
| `MG_Test/Wire/RemoteClientControls.inc` | 本轮五个用例（四个 A + 一个 B），每个都写了它自己的红是什么 |
| `MG_Test/Wire/RemoteClientTest.cpp` | `RemoteRunAhead.OnlyTheTextureHalfOfResourceSubDataWantsItsReply`（monolith 侧的钉） |
| red-once 命令 | `./build-split/MobileGL/MG_Test/Wire/RemoteClientTest --gtest_filter='*Declined*:*DeviceLostRespecify*:*TextureUploadsStopOwning*'`，配合 `build-split/fix-a.patch`、`build-split/fix-b.patch` 两个临时补丁往返 |
| 全量 | `ctest --test-dir build-split -E "Scenario|EventForfeitPeer"`：2616 例，除上述沙箱 EGL 两条外全绿 |
