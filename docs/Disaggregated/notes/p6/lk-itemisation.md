# lk — 开工前的逐文件清单
> 头 `0bbe41c2`。`CONTRACT-P6.md` §8.5 要求 `lk` 在开工前发布这份清单，理由是早先一份草案的棘轮基线
> 差了三倍，而**装在错误数字上的棘轮第一次 CI 就会因为和泄漏无关的理由变红**。
> 全部计数由 `grep -rn -E` 在本头机械产出，口径写在每节标题下。

## A 纯度门棘轮基线：`Transport/` 之外的缝标识符

口径：`<标识符>`，排除 `MobileGL/MG_Remote/Transport/` 自身。`lk` 之后这些必须只出现在 `Transport/ShmLink.*` 与传输自己的测试里。

| 文件 | 类别 | 合计 | 明细 |
|---|---|---:|---|
| `MG_Test/Wire/SessionTest.cpp` | test | 55 | RingControl×16 ReplySlotPool×10 SessionSegments×9 RingProducer×4 RingConsumer×4 SessionConsumer×4 EventRingConsumer×3 SessionProducer×3 EventRingProducer×2 |
| `MG_Test/Wire/RingTest.cpp` | test | 28 | RingControl×11 RingProducer×9 RingConsumer×8 |
| `MG_Remote/Client/ClientSession.cpp` | prod | 26 | RingControl×7 EventRingConsumer×7 RingProducer×5 SessionProducer×4 ReplySlotPool×2 SessionSegments×1 |
| `MG_Remote/Wire/PipeWireCodec.h` | prod | 19 | RingControl×11 RingProducer×6 RingConsumer×1 SessionConsumer×1 |
| `MG_Test/Wire/PipeWireCodecTest.cpp` | test | 19 | RingProducer×7 RingConsumer×6 RingControl×5 SessionConsumer×1 |
| `MG_Remote/Server/ServerSession.cpp` | prod | 17 | RingControl×4 RingConsumer×4 EventRingProducer×3 SessionConsumer×3 ReplySlotPool×1 SessionSegments×1 SessionProducer×1 |
| `MG_Remote/Client/ClientSession.h` | prod | 14 | RingControl×3 SessionProducer×3 RingProducer×2 ReplySlotPool×2 EventRingConsumer×2 SessionSegments×2 |
| `MG_Remote/Wire/PipeWireCodec.cpp` | prod | 14 | RingControl×6 RingProducer×4 RingConsumer×3 SessionConsumer×1 |
| `MG_Test/Wire/ServerLoopTest.cpp` | test | 13 | RingControl×4 RingProducer×3 EventRingConsumer×2 ReplySlotPool×1 SessionSegments×1 SessionProducer×1 SessionConsumer×1 |
| `MG_Remote/Server/ServerLoop.cpp` | prod | 12 | SessionConsumer×6 RingControl×2 RingConsumer×2 RingProducer×1 EventRingConsumer×1 |
| `MG_Remote/Server/ServerSession.h` | prod | 11 | EventRingProducer×3 RingControl×2 RingConsumer×2 SessionSegments×2 SessionConsumer×2 |
| `MG_Remote/Server/PipeApplier.cpp` | prod | 5 | SessionConsumer×3 RingControl×1 ReplySlotPool×1 |
| `MG_Remote/Server/PipeApplier.h` | prod | 3 | RingControl×2 SessionConsumer×1 |
| `MG_Test/Wire/RemoteClientTest.cpp` | test | 3 | ReplySlotPool×3 |
| `MG_IntegrationTest/Harness/WireLedgerChecks.h` | test | 2 | RingProducer×2 |
| `MG_Util/Metrics/PipeStats.h` | prod | 2 | RingProducer×1 SessionProducer×1 |
| `Config.h` | prod | 1 | RingProducer×1 |
| `MG_IntegrationTest/Harness/SplitRuntimePeek.cpp` | test | 1 | RingProducer×1 |
| `MG_IntegrationTest/Harness/SplitRuntimePeek.h` | test | 1 | RingProducer×1 |
| `MG_Remote/Client/EmitTables.cpp` | prod | 1 | SessionProducer×1 |
| `MG_Remote/Client/EmitTables.h` | prod | 1 | ReplySlotPool×1 |
| `MG_Remote/Client/GpuWritePending.h` | prod | 1 | RingProducer×1 |
| `MG_Test/Wire/InProcessTransportTest.cpp` | test | 1 | RingControl×1 |

**合计：生产 127 / 测试 123，共 250 处，分布在 23 个文件。**

## B 重组影响面：`control.<字段>` 成员访问

口径：`(control|m_control|ctrl)(->|.)<字段>`，七个字段 = 四个 watermark + `submittedSeq` + 两个 event 标志。
这是把 watermark 组变成具名 `LinkProgress` 成员后**必须逐处改写**的集合。

| 文件 | 类别 | 处数 |
|---|---|---:|
| `MG_Remote/Transport/Ring.cpp` | transport | 14 |
| `MG_Test/Wire/RingTest.cpp` | test | 10 |
| `MG_IntegrationTest/Harness/SplitRuntimePeek.cpp` | test | 5 |
| `MG_Remote/Wire/PipeWireCodec.cpp` | prod | 4 |
| `MG_Remote/Client/ClientSession.cpp` | prod | 2 |
| `MG_Remote/Server/ServerLoop.cpp` | prod | 2 |
| `MG_Test/Wire/PipeWireCodecTest.cpp` | test | 2 |
| `MG_Remote/Server/ServerSession.cpp` | prod | 1 |
| `MG_Test/Wire/SessionTest.cpp` | test | 1 |

**合计 41 处**（传输内 14 / 生产 9 / 测试 18）。
