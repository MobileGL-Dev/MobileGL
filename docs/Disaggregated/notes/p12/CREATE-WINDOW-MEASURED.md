# create window 的实测否决，与 StreamLink 的一处 ILink 契约违反（handoff-2 §2 收尾记录）

> 2026-09-26。分支 `feat/disaggregated` @ `82ed1df9` 加本轮工作树改动。对应
> [`MOBILEGL-CS-HANDOFF-2.md`](../../../../MOBILEGL-CS-HANDOFF-2.md) 的 §2（工作树里那版 create window）
> 与 §7 的「把 create 那 4,536 批成 ~1,134」。

## 0. 一句话

- **窗口开了等于没开**：同一台设备、同一帧、同一 101,421,396 B staged，`MOBILEGL_IPC_CREATE_WINDOW=4`
  与 `=1` 的 create 行都是 **4,536 次等待**，墙钟都是 **28.64 s**（handoff 的 pre-window 基线是 31.3 s）。
  临时计数器给出的形态是：`pushed=24 blocked=4,476 took=20 readfail=8,951 pending=4`——**drain 读不回来**。
- **不是调参问题**：回复池 8 槽，而这一帧发出 **55,903 条带回复的记录**（服务端对每条 kReplySlot 记录都回，
  不管客户端要不要），所以一条答案在 **8 条回复**之后就被覆盖，等于 **0.65 个 create**。任何深度的窗口都读不回来。
- **顺带修掉一个真 bug**：`StreamLink::ReadReply` 只认「最新一条」（单槽），违背 `ILink::ReadReply(seq)`
  的按 seq 读取语义。已改成一个 8 槽环 + 用例。
- **默认关掉**：`MOBILEGL_IPC_CREATE_WINDOW` 默认 1。窗口开着不只是白费，它会让**被拒绝的 create 留在「已发布」状态**
  （反闩动作在同一个读不回来的 drain 里），正是 suspect 表要防的那种静默分歧。

## 1. 实测（同一设备 aikjs4s8cys8ba7l，同一世界，出厂省电）

| run | 窗口 | `frame-sent 2=` | `frame-op by_op 2=` | 墙钟 | 日志 |
|---|---|---|---|---|---|
| handoff 基线（无窗口的二进制） | — | 4,536 | 4,536 | 31.26 s | `logs/mcx.client` |
| 窗口 4 | 4 | 4,536 | **4,536** | **28.64 s** | `logs/mcx-fix4.client` |
| 窗口 1（负控） | 1 | 4,536 | **4,536** | **28.64 s** | `logs/mcx-fix1.client` |
| **默认（本轮收尾的 HEAD，窗口 1）** | 1 | 4,536 | **4,536** | 30.79 s | `logs/mcx-final.client` |

`by_op 2` 数的是**走了阻塞路径的 create**（`LinkMetricsBeginReply(ownsReplySlot, op)`，
`ownsReplySlot = rowCarriesReplySlot && wantReply`，`ClientSession.cpp:1840/1883`）。它没动，就是「一条都没延后」。
两臂的 `frame-sent` 全表逐 op 相同（4,536 / 20,633 / 3,770 / 18,056 / 12,671 …），也就是**记录一条没少发**；
收尾那一轮（默认值，`mcx-final`）的 `frame-sent` 与它们**逐字节相同**，墙钟的 30.79 s 在 handoff-2 §4.3 说的
「单次墙钟差含数秒噪声」带内——三臂的墙钟差（28.64 / 28.64 / 30.79）没有一个超出噪声。

窗口确实进去过——临时行（每 500 条打一次）在窗口 4 下的读数：

~~~
P12diag create=501  cfg=4 eff=4 suspects=0 refusals=0 pending=4 pushed=24 blocked=476  took=20 waitfail=22 readfail=951
P12diag create=4501 cfg=4 eff=4 suspects=0 refusals=0 pending=4 pushed=24 blocked=4476 took=20 waitfail=22 readfail=8951
~~~

`pending` 从第 500 条起**一直钉在 4**，`refusals=0`（一条答案都没读到过），`readfail` 约每条 create 涨 2
（非阻塞 poll + 阻塞 drain 各失败一次）。

## 2. 根因：回复池 8 槽 vs 一帧 55,903 条回复

答案只在**它的槽没被复用**之前可读（`ReplySlotPool`，`kDefaultReplySlotCount = 8`，
`Transport/ReplySlot.h:111`）。而这一帧发回复的记录数是（**只数带 `kReplySlot` 的行**——
`MG_Pipe/generated/PipeWire.inc:177-179` 的 flags 列，op 4 `ResourceDestroy` 是 `kNone`，
`PostReply` 对它直接 Fatal，从来不回；本文第一版把它的 3,770 条算了进来，审查纠正为下表）：

| 行 | 条数 | 为什么回 |
|---|---|---|
| 2 `ResourceCreate` | 4,536 | 等 |
| 3 `ResourceRespecify` | 20,633 | **不等也给**（首用确认后 fire-and-forget） |
| 47 `SetTextureParams` | 18,056 | **不等也给** |
| 48 `ResourceSubData` | 12,671 | **不等也给** |
| 5 / 9 等 | 7 | 等 |
| **合计** | **55,903** | |

服务端**无法知道**客户端要不要这条答案——`ClientSession.cpp:1897-1900` 原文：「The server posts either way,
so the wire is unchanged and the row keeps its flag」。于是池子一帧 churn **6,988 次**，两条 create 之间平均夹
**12.3 条回复**，而一条答案只活得下 **8 条**：

    8 / 12.3 = 0.65 个 create

**所以窗口的失败不是「慢」，是「读不到」。** 1 深度的窗口就是阻塞路径本身。

## 3. 顺带发现并修掉：`StreamLink::ReadReply` 违背了自己的接口

`ILink::ReadReply`（`Transport/ILink.h:285`）的签名是**按 seq 读**；`ShmLink` 用 `ReplySlotPool` 实现它，
而 `StreamLink` 到本轮为止只保留**一条**回复（`reply` / `replySeq` / `replyStatus`），并且：

~~~cpp
x.replyReady.wait(lock, [&]{ return closed || x.replySeq >= seq || appliedSeq >= seq; });
if (x.replySeq != seq) return ... MOBILEGL_ERR_PROTOCOL_MISMATCH;   // 等的是 >=，判的是 ==
~~~

「等到至少 seq」与「只接受恰好 seq」自相矛盾：任何早于最新一条的答案都读不回来。此前没有调用者受影响，
是因为每个调用者读的都是自己刚等到的、也就是最新的一条；create window 是第一个读**旧**答案的调用者。
已改成 8 槽环（`kReplyRing`，深度取它顶替的那个池：`kDefaultReplySlotCount`），
用例 `StreamLinkTest.AnOlderReplyIsStillReadableByItsSeq` 三连发、按 1→2→3 顺序读回。

**注意这只治好了「单槽」，没治好「池只有 8 槽」**——`ShmLink` 侧本来就是池，也一样读不回来。这也是为什么
设备上的读数在修掉单槽之后仍然一模一样。

## 4. 为什么这是个必须默认关掉的开关，而不是一个慢一点的开关

窗口的答案是**临时接受**：`PublishCreate` 会因此落闩（「applier holds this handle」），
而纠正它的唯一动作——迟到 DECLINE → `MGPipeNoteHandleUnpublished` + suspect 表——**就在那个读不回来的 drain 里**。
换句话说窗口开着时，**服务端的拒绝会被客户端当成已发布**，之后每条已转换的行都对一个不存在的记录 fire-and-forget。
这正是 suspect 表存在的理由，也是 `TextureEmit.h` 里 `RespecifyOnce` 那句话（「applier 的 REFUSAL 是唯一说
『我手里没有这个 handle』的信号」）要防的东西。

## 5. 门（`MG_Test/Wire/RemoteClientControls.inc`）

窗口机制本身是自洽的，只是在这条协议上不可用，所以门保留并 pin 住机制：

| 用例 | 钉住什么 | 读数 |
|---|---|---|
| `ACreateWindowDefersTheAnswersAndTheWindowOffDoesNot` | 窗口开=12 记录 0 等待；**同一子进程**把窗口关回 1 = 12 记录 **12** 等待（负控）；`pending <= effective` 逐条断言；99 被夹到数组大小；数组 <= 池的一半 | ON 0 waits / OFF 12 waits |
| `ARefusedCreateUnlatchesItsObjectAndItsNextCreateBlocks` | 真拒绝（收窄服务端 consumed mask）→ **闩清零**、suspect 计数 +1、下一条 create **阻塞**（2 记录 2 等待）、恢复消费者后 **heal**（闩回来、suspect −1） | suspects 6 → 5 |
| `StreamLinkTest.AnOlderReplyIsStillReadableByItsSeq` | 传输层按 seq 可读 | 3/3 |

编译期还有一条：`WireTables.cpp` 的 `static_assert(kCreateWindowMax <= kDefaultReplySlotCount / 2, …)`，
把「有人把窗口改成 8 就会踩池子」变成编译错误而不是设备上的错画面。

## 6. 画面（handoff-2 §6 要的那一格）

设备端（`MOBILEGL_IPC_SURFACE=server`，画面在手机屏上）在负载帧闭合后 +8 s 截屏：
`logs/mcx-fix4.shot2.png` 与 `logs/mcx-fix1.shot2.png` 都是真实世界（地形、物品栏、聊天行），
**两臂 SSIM = 0.977**（均值绝对差 2.26/255）。同一 run 内 加载页 vs 世界 SSIM 只有 0.12，说明这个数字不是
「两张都黑」的假阳性。**这仍然只是间接证据**：两臂行为本来就相同（窗口是 inert 的），它能说明的是
「窗口开着没有把画面弄坏」，不是「延后一条确认后画面仍然对」。

## 7. 下一步（如果要救这个想法）

要让延后的答案活下来，只有两条路，**都是协议面而不是调参**：

1. **加深回复池**：`ReplySlotCount` 在 Hello 里协商（`ClientSession.cpp:957` / `ServerSession.cpp:745`），
   两端一致即可改。需要的深度 \>= 两条 create 之间的回复数（本帧 12.3）乘以窗口深度，即 4 深度要 ~49 槽；
   而 `ReplyBytes` 固定 16 MiB 时槽会变小（16 MiB / 64 ≈ 256 KiB），`ReadPixels` 的 `ReplyCanHold`
   会先撞墙，所以要一起抬 `ReplyBytes`（W=4 保 2 MiB 上限约需 128 MiB）。**注意这是 8× 常驻内存**，
   而且评审指出服务端 SEG_REPLY 的实际分配路径**没人读过**——走这条之前先读它。
2. **让服务端别回那些没人会读的答案**：四个 Bool 行（create/respecify/params/sub-data）目前一律回，
   其中约 51,000 条的答案是**没有人会读**的。砍到 ~4,543 之后两条 create 之间只剩 ~1 条回复，8 槽够用。

   **判据必须是「这条的答案永远不会被读」，不是「fire-and-forget」。** 这两者今天在 4 个站点里 3 个重合，
   **在最重要的那一个上正好相反**：window 延后的那些 create **正是** `wantReply=false`，而 §1 的 drain
   读的就是它们。按字面实现会让窗口从「读不回来」变成「根本没得读」，而且连 `readfail` 这个唯一能诊断它的
   计数器一起消失——不会崩，只会静默退化。（这条是独立评审抓到的；本页 §7 的第一版写法就是那个错判据。）

   三件已经核过的事：

   - **位有空位**：`MGPWireRecHeader::Flags` 是链路 framing 空间 `RingRecordFlags`（**不是** `MGPipeCallFlags`），
     `Ring.h:281-285` 用掉 bit 0-4，**bit 5 空**（`PipeWire.inc:35-65` 的对照表自己写着「bit 5 kOptional vs（无）」）。
   - **wire fingerprint 不动**：`WireFingerprint()` 混的是 payload 布局 digest + PipeCalls.def 的 catalogue
     digest + 渲染状态 + OpCount，加一个 ring 位三个都不动 ⇒ **不必 bump `MOBILEGL_PROTOCOL_CONTROL_REVISION`**。
     但 `PipeWireCodec.cpp:106` / `:142-145` 与 `RingTest.cpp:740` 三条 `static_assert` **会红**——那是故意的
     绊线（新 ring 位正好别名 `kOptional`）。
   - **两个方向都退化成今天的行为**（旧端忽略 bit 5 照旧回包；旧客户端从不设位）⇒ 它**不是** handoff-2 §3.2
     那种「不升级就会坏」的协议面，而是**「不两端都升就不会生效」**的协议面——便宜一档，客户端可以先落。

   还有一处**必须同改**：`LinkMetricsBeginReply` 计的是 `ownsReplySlot`（`ClientSession.cpp:1883`），window
   生效后 **`by_op 2` 会掉到 ~0**，而 §1 正是教人用 `by_op 2` 判断窗口有没有生效——它会变成**假绿**。
   验证这个提案要另加「服务端为这条 op 回了多少次」的计数。

   （本节的两条路都经过一次独立评审。评审指出一处**未实测**：`StreamLink::PostReply` 把 `SendProgress()`
   搭在回包上（`StreamLink.cpp:577`），跳过回包会把进度粒度从「每条一次」降到「>= 64 条或 >= 1 ms 一次」
   （`:564-571`）——水位仍独立推进，但 barrier 的平均多等量没人算过。）

## 8. 复算

~~~sh
# 两臂的帧读数（handoff-2 §1 的读法）
python3 mgl-device/bigframe.py mgl-device/logs/mcx-fix4.client mgl-device/logs/mcx-fix1.client
grep -a "kind=frame-op frame=" mgl-device/logs/mcx-fix4.client | tail -1
grep -a "kind=frame-sent frame=" mgl-device/logs/mcx-fix4.client | tail -1
# 一轮设备 run（自带 force-stop、就绪判据、负载帧检测、截屏、SIGTERM 收尾）
cd mgl-device && python3 run-once.py <tag> [MOBILEGL_IPC_CREATE_WINDOW=4]
# 门
build-split/MobileGL/MG_Test/Wire/RemoteClientTest --gtest_filter='*CreateWindow*:*RefusedCreate*'
build-split/MobileGL/MG_Test/Wire/StreamLinkTest --gtest_filter='*OlderReply*'
~~~
