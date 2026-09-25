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

> **⚠️ 本节后来被 §8 部分更正。** 池深度是**加剧因素**而不是约束：把一帧回包从 55,903 砍到 4,543
> 之后，drain 依然一条都读不回来（readfail 8,951 → 8,872）。真正的约束是 §8 的
> 「有界缓冲 + 严格 FIFO + 读失败无出路」。本节的数字仍然有效，结论不完整。

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

## 8. 更正（同日晚，实现 no-reply 位之后）：池深度**不是**约束

§2 把「8 槽池 vs 55,903 条回复」写成了根因。**实测否掉了这个诊断的一半。** 后来按 §7 的第 2 条路
实现了那个 no-reply 位（服务端跳过没人读的答案），真机上服务端日志确认它生效：

    服务端：decisions=50001（跳过）  answered=4521（回包）
    客户端：收到的 Reply 帧 seen=4501, latest seq=97181

也就是说一帧的回包从 55,903 降到 ~4,543（−92%），池子不再被冲刷。**而窗口的 drain 依然一条都读不回来：**

    P12win call=4501 cfg=2 eff=2 pending=2 taken=62 pushed=64 blocked=4436 waitfail=65 readfail=8872
    P12miss n=1      want=310 replySeq=1504   ring=[1175..1504] slots=8
    P12miss n=9001   want=310 replySeq=102915 ring=[102866..102915] slots=8

**want=310 每一次都是同一个数**：窗口是按序的，队头那条的答案早就被有界缓冲覆盖掉了，而 drain 只会
**永远重试同一个队头**。于是 taken 停在 62、pending 钉在 2、readfail 每条 create 涨 2、suspects 恒为 0。

**真正的根因是「有界缓冲 + 严格 FIFO + 读失败无出路」三者相乘**，不是池子有多深：

- 回复通道在任何实现下都是有界的（shm 是 8 槽池、stream 是我那轮改的 8 帧环），覆盖是设计的一部分；
- 窗口只读队头，且读失败时**只记账不放弃**（`if (!session.ReadReply(...)) return;`）；
- 于是 4,536 条延后答案里**只要有任意一条**在被读之前被覆盖，窗口就永久卡死，之后每条 create 都走阻塞路径；
- 概率上这是必然事件，而不是小概率——这也解释了为什么去掉 50,001 条回复只让 readfail 从 8,951 动到 8,872（−0.9%）：
  冲刷速率降了两个数量级，但「至少丢一条」的概率仍然是 1。

**要修的话是一个明确的形状**（本轮按 goal 的止损条件停在这里，没有实现）：队头的答案读不回来时
**放弃它**——`MGPipeNoteHandleUnpublished` + `NoteCreateSuspect` + 弹出该条目——也就是把 DECLINE 的
处理路径复用到「不知道」这个结果上。这样窗口自愈（该对象回到阻塞路径，下一次 create 把真相带回来），
代价是丢过答案的对象变慢，而不再是把整行拖死。它需要一个自己的门：**读失败必须让窗口继续前进**。

顺带记两条本轮得到的事实：

1. **no-reply 位本身是好的，而且两端都验证过**：服务端跳过 50,001 条、回 4,521 条；客户端的 `by_op 2`
   随之变成「drain 的阻塞次数 + fallback 次数」，`frame-sent` 逐 op 不变。它救不了窗口，但它把
   「服务端为每条 kReplySlot 记录都回」这件事变成了可关的——每帧少 ~102,000 次 `sendmsg`。
2. **两端必须都升才生效**：客户端先落、服务端不升时，行为与改前逐字节相同（旧端忽略 bit 5 照旧回包）——
   这正是 B6 说的那一档，本轮在真机上先撞到了一次。

## 9. 第 1、2 步落地与实测：预算也不够，缺的是「保留」

按 §8 的次序做了两步并上真机。

**第 1 步（让回包失败可归因）**：`ReplyPool` 现在有 `PostedReplies()`/`FailedReplies()`，非 OK 的返回不再被吞
（原来只检查 `BUFFER_TOO_SMALL`，`TRANSPORT_CLOSED` 静默消失）。实测：

    服务端 P12post posted=4501 failed=0

**⇒「答案没送到」被排除**：服务端 `answered` 与客户端 `seen` 的那 20 条差额是两端采样时刻不同，不是丢包。

**第 2 步（residency budget）**：客户端数「自最老窗口条目 push 以来发出了多少条带 `kReplySlot` 的记录」
（`ClientSession::ReplyPostings()`），逼近 8 就强制阻塞取回。实测：

    P12win call=4501 cfg=2 eff=2 pending=2 taken=63 pushed=65 blocked=4435 budget=4451 readfail=8872 posts=54377
    kind=frame-op frame=14 wait_replies=9078 by_op=2=9071   墙钟 27.87 s

`budget=4451`——预算在**几乎每一条 create** 上都判超支，于是窗口几乎不再延后（`pushed=65`），而 drain 依然
读不回来（`taken=63`、`readfail=8872`）。**预算无效，且原因是结构性的：**

1. **窗口只在 create 到达时才有机会动作。** 两条 create 之间客户端可以发出上千条记录——实测第一次读失败时
   回复环已经是 `[1175..1504]`，而队头是 **seq 310**；中间那段没有任何人看窗口。
2. **挤掉延后答案的不是客户端发了什么，是服务端回了什么。** 服务端是落后的一侧（每次唤醒 ~5 ms 回一条，
   客户端一路跑到前面），所以那段时间到达的回复是它**对更早那批 create 的迟到答案**，它们先把 8 个槽占满。
3. **所以这个判据在构造上就太晚**：它在下一条 create 才被问到，而那时答案早就没了。收紧常数只会让延后彻底消失
   （`pushed=65` 就是那个样子），不会有别的结果。

**结论（本轮最终认识）：延后要成立，客户端必须「保留」它延后的答案。** 回复缓冲按**条数**有界（8），而客户端
**无法**约束服务端在它两条 create 之间会回多少条答案——所以「指望缓冲里还有」不是策略，是一场客户端必输的竞争。
能工作的形状是**保留**：读线程把「还没被取走」的那几个 seq 的答案留着（同时只有个位数），取走即丢，
读不到就**报一次丢失**而不是无限重试。那是传输层改动（`StreamLink` 的环 → 带上限与丢失计数的保留表），
也是下一步该做的。

**两步都留在树里**：第 1 步是净收益；第 2 步是安全方向（只会让窗口退化成不延后＝pre-window 形状），
而且它用的时钟 `ReplyPostings()` 正是保留方案要用的。

## 10. 第 0 步实验：把 `StreamLink` 的环从 8 放到 1024 —— 诊断确认

§9 的结论是「延后要成立必须保留」。在写保留表之前先做了一个**一行实验**把这句话证死：把
`StreamLink::kReplyRing` 从 8 改成 1024（其余一切不动），同一个负载帧：

| | 环 = 8（现状） | 环 = 1024（实验） |
|---|---|---|
| `readfail` | 8,872 | **0** |
| `taken` | 63 | **4,499 / 4,500** |
| `blocked`（回退阻塞路径） | 4,435 | **0** |
| `by_op 2` | 9,071 | **3,443** |
| 墙钟 | 27.9 s | **23.04 s** |
| `frame-sent` | create 4,536 / records 103,097 | 逐 op 相同 |

**结论一：唯一的原因就是淘汰。** 环够深，延后的答案**每一个**都读回来了（4,499/4,500），一次都没有回退到
阻塞路径。§8 那个 `want=310` 的楔子、§9 那个 `budget=4451` 的无效预算，都是「8 个位置不够」这一个事实的两种表现。

**结论二：现在刹车的是预算，不是缓冲。** `by_op 2 = 3,443` 恰好等于 `budget` 触发次数（3,395 量级）——
也就是**每一条被预算强制提前取回的答案，都是一次真实的阻塞等待**。预算的常数是 7（按 8 槽池定的），
而实验里缓冲有 1024 深，于是预算变成了唯一的节流阀，墙钟只从 27.9 s 降到 23.0 s。

**这对设计意味着什么**：保留表落地之后，正确性不再依赖「答案在缓冲里活多久」——**答案被持有到被读走为止**。
于是预算不该再按 8 槽池定，而应当按**保留容量**定（或者干脆由「保留即安全」取代）。
按 `by_op 2 ≈ records / W` 推算，W=2 时应是 ~2,268 次等待、墙钟 ~15 s——**正是 goal 里那条 15-16 s 的判据**。

实验状态已还原（环回到 8、探针全部删除），四个车道 172+13+22+103 全绿。

## 11. 第 0b 步：预算关掉 + 环 1024 —— 收益是量出来的，不是外推的

§10 证明「环够深，延后的答案就都读得回来」，但那一轮 `by_op 2 = 3,443` 里绝大多数是**预算**触发的
（3,395），所以「W 变小能不能换来墙钟」还没答。于是再做一次一行实验：**环 1024 + 把预算关掉**
（`kCreateReplyResidency` 抬到不可能达到），W=2 与 W=4 各跑一轮：

| 臂 | waits | rtt_mean | `by_op 2` | **墙钟** |
|---|---|---|---|---|
| W=1（现默认） | 4,543 | 5,672 µs | 4,536 | 28.66 s |
| 预算开 + 环 8 | 9,078 | 2,598 µs | 9,071 | 27.04 s |
| 环 1024 + 预算 7 | 3,450 | 5,367 µs | 3,443 | 23.04 s |
| **环 1024 + 预算关，W=2** | **2,560** | **4,916 µs** | **2,553** | **16.80 s** |
| **环 1024 + 预算关，W=4** | **1,691** | **4,224 µs** | **1,684** | **11.45 s** |

（W=4 那轮的窗口读数：`taken=4498/4500 pushed=4500 blocked=0 readfail=0`。）

**结论一：分水岭答了，而且是好的一侧。** 等待数减半/减到 1/2.7，墙钟跟着减半/减到 1/2.5，
而 **`rtt_mean` 没有变大**（4,916 / 4,224 µs，比基线的 5,672 µs 还低）。这说明那 ~5 ms 是**唤醒往返**，
而且**服务端一次唤醒吃掉一整批**——所以 W 深一档就少一档唤醒，等待数与墙钟同步下降。
（审计把这条列为「仓库里判不出来、只有真机能答」的分水岭，它同时也是「这件事值不值得做」的分水岭。）

**结论二：收益比 handoff 的地板还低。** handoff §0.4 估计「窗口生效后 15-20 s」，而那把客户端 CPU（~13.4 s）
当成与等待**相加**；实测这个帧是等待主导的，等待摊薄之后 W=4 到 **11.45 s**——比估计的地板还低 3.5 s。

**注意 1024 是替身，不是设计。** 它能工作只因为「这个负载下 1024 恰好够深」；1024 个条目最坏是 ~2 GiB
（`ReadPixels` 的答案可到 2 MiB），审计已点名。**保留表给的正是同一个性质（答案还在）而有界**，
所以这一步的结论是「按审计的形状去实现」，而不是「把环改成 1024」。

实验状态已还原（环回 8、预算回 7、探针删除），172 + 13 全绿。

**顺带更正 §7 的第 1 条路**（审计 F15）：在 stream 臂上 `SEG_REPLY` / Hello 的 `ReplySlotCount` **与答案无关**
——答案走 Reply 帧、`StreamLink::ReadReply` 读客户端自己的 store，池只用来给 `MaxReplyBytes()` 定尺寸。
所以「加深池子 + 抬 `ReplyBytes`」根本没有针对实测那条臂，别为它付 8× 常驻内存。

## 12. 落地：保留表上线，默认翻到 2，真机 28.66 s → 16.52 s

按审计的形状实现（`ILink::DeclareReplyRead` + `LinkCapabilities::RetainsReplies`；`StreamLink` 的环 →
**按声明的保留表**：单调白名单 + 游标、取走即删、触顶具名 Fatal、Detach 清空；删掉 residency budget；
`CreateWindowEffective()` 在不保留答案的链路上强制为 1；窗口退休分支补一次 drain）。

**真机（两个二进制都重出，负载帧，`frame-sent 2=` 全程 4,536 不变）：**

| 臂 | waits | `by_op 2` | 墙钟 |
|---|---|---|---|
| 窗口 1（R-5 的形状） | 4,543 | 4,536 | 28.66 s |
| 窗口 2（显式） | 2,507 | 2,500 | 17.27 s |
| **窗口 2（出厂默认，无覆盖）** | 2,570 | 2,563 | **16.52 s** |
| 窗口 4 | 1,651 | 1,644 | 11.02 s |
| **窗口 2 + 旧服务端（不认 no-reply 位）** | 2,670 | 2,663 | **16.38 s** |

**版本偏斜那一轮是审计点名的必测项，它过了**：旧服务端照回 ~55,903 条/帧，客户端的白名单把这些
**没人声明**的答案在到达时丢弃（`SkippedUnwanted` 增长而不是保留表增长），延后的答案照旧读得回来，
整轮**零 Fatal**。这正是「按声明保留」相对「全保留 + 淘汰」的那个差别。

**门**（172 + 15 全绿）：
- `StreamLinkTest.ADeclaredAnswerSurvivesFarMoreInterveningAnswersThanAnyFixedBuffer` —— **已证明能红**：
  把存储退回「按到达保留、只留 8 条」的环语义，它立刻以「the declared answer did not survive 64
  intervening ones」失败。这就是把环=1024 那个替身换成有界机制的那条界线。
- `StreamLinkTest.AnAnswerNobodyDeclaredIsNotKeptAtAll` —— 没声明的答案不进保留表（红法：全保留）。
- `RemoteClientControls.TheWindowRefusesToDeferOnALinkThatCannotRetainAnswers` —— 在不能保留的链路
  （in-process/ShmLink）上，把旋钮开到 4 也必须每条 create 都走阻塞、`taken == 0`（红法：去掉能力位）。
- `ARefusedCreate...` 改成读这条臂上真正的修复链：拒绝 → 闩保持清零 → 下一次定义阻塞且再被拒 → 恢复消费者后闩回来；
  并断言窗口的账本（`refusals`/`suspects`/`pending`）在这条臂上**全为空**。

**还没做的一件**（审计 E10(d)，下一轮）：窗口级门目前只有真机这一层，**没有跑在 stream 臂上的单元门**。
三轮「门全绿而功能在设备上是死的」都源于同一件事——门跑在 in-process/ShmLink 上。传输层的保留门已经补上，
窗口那一层建议建在 `StreamClientPublicationTest` 的真 stream 会话上（`AttachStreamLink` + 可控 peer）。

## 13. 复算

~~~sh
# 两臂的帧读数（handoff-2 §1 的读法）
python3 mgl-device/bigframe.py mgl-device/logs/mcx-fix4.client mgl-device/logs/mcx-fix1.client
grep -a "kind=frame-op frame=" mgl-device/logs/mcx-fix4.client | tail -1
grep -a "kind=frame-sent frame=" mgl-device/logs/mcx-fix4.client | tail -1
# 一轮设备 run（自带 force-stop、就绪判据、负载帧检测、截屏、SIGTERM 收尾）
cd mgl-device && python3 run-once.py <tag> [MOBILEGL_IPC_CREATE_WINDOW=4]
# 窗口自己的读数（临时探针）：taken / pushed / budget / readfail / posts
grep -a "P12win" mgl-device/logs/<tag>.client | tail -1
# 回包送达（服务端）：posted / failed
grep -a "P12post" mgl-device/logs/<tag>.server | tail -1
# 门
build-split/MobileGL/MG_Test/Wire/RemoteClientTest --gtest_filter='*CreateWindow*:*RefusedCreate*'
build-split/MobileGL/MG_Test/Wire/StreamLinkTest --gtest_filter='*OlderReply*'
~~~
