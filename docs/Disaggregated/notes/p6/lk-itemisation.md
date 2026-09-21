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

---

## C 实际结果（重组落地后补记）

两个口径在本文里曾经打架：表 B 的 41（受体限定为 `control|m_control|ctrl`）与
`a54ae47a` 提交信息里的 44（受体不限）。**以表 B 的口径为准**，差额是三处受体不是
控制页对象的匹配；而实际改写时受体集合还要再宽一档，因为测试里普遍写作
`session.Control()->appliedSeq` / `owner.CmdControl()->appliedSeq`。

最终改写：**库 15 处 + 测试 58 处 = 73 处**（按 `re.subn` 计，一行可含多处）。

**"开集成测试构建"的真正理由不是计数，是守卫**：`MG_IntegrationTest/Harness/SplitRuntimePeek.cpp`
的五处改写躲在 `#if defined(MGITEST_SPLIT_RUNTIME_PEEK) && !defined(__ANDROID__)` 后面。
本轮用 red-once 证明了它确实在编译——往守卫体内塞一个未声明标识符，构建变红：

```
SplitRuntimePeek.cpp:99:13: error: use of undeclared identifier 'MGITEST_DELIBERATE_RED_ONCE_PROBE'
```

**差点踩坑的一处**：`SplitRuntimePeek.h:77` 另有一个结构体同样有 `appliedSeq` 字段，
而 `SplitRuntimePeek.cpp:146` 一行里两个都在——`state.appliedSeq = control->appliedSeq.load(...)`。
不限定受体的改写会静默改坏左边，`MagmaRunAheadScenario.cpp` 的九处访问的正是它（那个文件一行未动）。

## D 验收

| 项 | 结果 |
|---|---|
| pull 构建（G1） | 符号 0 增 / 0 删，`.text` 10822147 不变 |
| split 构建 | 符号 0 增 / 0 删，`.text` 11681395 不变 |
| 单元测试 | 2326 / 2328，与基线同样的两个环境性失败 |
| 新增静态断言 | 逐条 red-once（下表） |

**口径说明**：上面的 `.text` 是**尺寸**不是字节。字段偏移变了，所以指令里的立即数变了，
但代码尺寸没变。另外这套数字用的是 clang 22.1.6 且关闭 test/benchmark，**与路线图记录的
P5f 基线（`.text 10806051`）不可比**，只能做"本包前 vs 本包后、同工具链"的比较。

### 断言的 red-once

| 扰动 | 结果 |
|---|---|
| 交换 `LinkProgress` 里 `retiredSeq` 与 `completedFrameSerial` | 红：`LinkProgress field order: retiredSeq` / `completedFrameSerial` |
| 在 `LinkProgress` 内增加第五个字段 | 红：`sizeof(LinkProgress) == 32` |
| **在 `Progress` 与 `serverEpoch` 之间塞一个小成员** | **不红——已具名记在 `Ring.h` 里** |

最后一行是本轮审查最有价值的产出。`serverEpoch` 是 `alignas(64)` 且紧随 `Progress` 声明，
所以编译器总把它放在下一个 64 倍数处：**一个塞在 288 的小成员会落在 `Progress` 自己那条线上，
而两个断言照样通过**，最多能藏 32 字节。审查建议的修法（`serverEpoch == Progress + 64`）
也抓不住——实测过。`Ring.h` 的注释现在写明了这一点，因为**一道没人能证伪的门比没有门更糟**。
