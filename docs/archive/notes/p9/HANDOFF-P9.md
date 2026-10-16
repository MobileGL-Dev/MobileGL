# P9 交接：反向通道（重划范围）

> 2026-09-29，写给实现方。基于 `feat/disaggregated@acfd36785`。**[V]** = 我读了代码 / 文档 / 日志确认；**[I]** = 我的推断，实现前要自己核。
> 路线图 P9 那一行（09-22 写的，原文在 [`README.md`](README.md)）与树里的现状已经对不上：一半已经做完，一半的设计前提被后来的阶段改掉了。**先读 §2，再动手。**

## 1. 目标

server → client 的结果回传（回读、回调、回复）在 split 下不阻塞、不丢、不乱序，剩下的 `Fatal{Unmigrated*}` 与"没生产者的回调"要么落地、要么明确删除。P9 属于 IPC 跑道（`P12 → P9 → P10 → P11`），与 P8（monolith 跑道）互不依赖。

## 2. 路线图每一项的现状（先看这张表）

| 路线图写的 | 现状 | 处置 |
|---|---|---|
| 异步 reply 语义，建在 P6.5 消息式 reply 上 | **[V] 大半已做**，但不在 P9 名下：P12 落了 `ILink::DeclareReplyRead` + `RetainsReplies`（StreamLink 的"声明过的答案"存储，容量 24，溢出是 Fatal 不是淘汰；ShmLink 为 false）、`kRecNoReply`（`Transport/Ring.h`，"这条答案没人读，server 别回"）、create 窗口（`MOBILEGL_IPC_CREATE_WINDOW` 默认 10，只在 `RetainsReplies` 的链路上生效）。rd12 加载帧的回包等待从 43,232 降到几百。见 `notes/p12/CREATE-WINDOW-MEASURED.md`、提交 `4e48702f2` / `3a2555805` | 不重做。剩下的只有 §3 的 W1 |
| 2 MiB reply cap 与 chunked readback（ID-47） | **[V] 已做**：`ReadPixels` 超过一个回复槽时由 client 切成行带（P7 ID-P7-58，`EmitTables.cpp` 的 `PlanReadbackBands` / `ForEachReadbackBand`，`BandedReadbackScenario`）；`GetTextureImage` 按 `DstOffset` 分窗（`TextureReadbackEmit.inc`） | 关闭；只在 W1 里保持行为不变 |
| PACK-PBO 回读 fire-and-forget + client `MarkGpuWritten`（ID-57） | **[V] 没做**：目标是 PBO 时，client 仍然等回复、再把行 `UploadSubData` 进 PBO（`EmitReadPixels` 里的 `pbo` / `pboSynced`，`EmitGetTextureImage` 末尾） | **W1** |
| 十个回调里未武装的四个：`OnTextureWriteback`、`OnTexturePullRequest`、`OnMipLevelsGenerated`、`OnLog` 分级 | **[V] 回调现有 9 个，`OnCapsInvalidated` 也没有生产者**（server 只装 4 个：`OnGlError`、`OnBufferWriteback`、`OnGpuWritten`、`OnSurfaceChanged`，`Server/ServerSession.cpp:823-827`；client 的消费者只有对应 4 种 `EventKind`，`Transport/EventRing.h`，`ClientSession.cpp` 的 `DrainEventRing`）。caps 失效走控制面的 CapsSnapshot 再到达（R-12），日志走控制面 `LogLine` | 逐个裁定，见 W2 / W3 |
| `OnGpuWritten` 收窄；`OnBufferWriteback` 批处理 + epoch 排序 | **[I] 没有实测需求**；writeback 已按 SEG_EVENT 容量的四分之一切片（`MGPipeBufferWritebackSliceBytes`，`MG_Pipe/MGPipeCallbacks.h`），"最后一片落地蕴含此前每一片" | 不做，除非 §5 的计数器显示事件量是问题（W4） |
| `OnGlError` 有序 | **[V] P5e 已裁定放宽**：run-ahead 动词的错误在下一个 drain 点观察，最多晚一个 present credit（`CONTRACT-P5E.md` §"glGetError relaxation"、`CONTRACT-P5C.md` §4.2） | 默认保持放宽，只记录（W5） |
| 纹理拉取四条缓解 + 终止符（`TextureRemintPullScenario`） | **[V] 拉取协议至今不存在**（`ResourceSubDataComplete` 终止符 op 有，但没有请求事件）；split 下重铸路径是 `Fatal{UnmigratedEmulation, "texture-remint-pull"}`（`MG_Backend/DirectGLES/Managers.cpp` 约 6519 / 6536）。**[I] 但设计前提变了**：`design/04-reverse-channel.md` §8.4 写"server 不保留纹素"，而 P5c 的 tx 之后 server 有 `StagedTextureStore`（`MG_Remote/Server/StagedTextureStore.h`，`CONTRACT-P5C.md` §2）——已定义的层级字节就在 server 手里 | **W2：先查、再定**，很可能不需要拉取协议 |
| XFB 命名空间（`DeleteTransformFeedback` 的 server 泄漏） | **[V] 已做**（`70df57bb2`）：`DeleteStreamOutput` op + `ServerVerbSink::OnDeleteStreamOutput`（`Server/PipeApplier.cpp` 约 1105）清 span 与 lifetime id | 关闭 |
| 两条故障注入 | 原文没说是哪两条 | §4 里我定了两条 |

**边界（别越界）**：`generate-mipmap-*`、`copy-image-shadow-mirror` 这些 Fatal 是 **P8**（把仿真挪到 client）；`FenceWait` / `Query*` 的每帧回包是 **P10**；`ResourceReadback` / `ReadPixels` 本身是必须等的同步语义，不是 P9 要消掉的。

## 3. 工作包

### W1 — PACK-PBO 回读不再等回复（唯一确定要做的）

- **现在**：client 发 `ReadPixels` / `GetTextureImage` 记录并阻塞等回复；回复里是紧排的像素，client 按 PACK 状态（行长、对齐、skip）散射进 PBO。
- **目标**：目标是 PBO 时，client **不等**：记录以 PBO 的 wire 句柄为目的（P5 已裁定"PBO 目的是资源句柄，不需要 `Seg` 字段"，`CONTRACT-P5.md` 行 23），带 `kRecNoReply`；client 在发出时把该范围标成"GPU 写过"（`MG_Remote/Client/GpuWritePending.*` 的保守 pending 集），此后任何 CPU 访问（`glMapBufferRange` / `glGetBufferSubData`）走既有的 `ResourceReadback` → `OnBufferWriteback` 路径拿到数据。
- **要先弄清的**：(1) monolith 的语义：原生读进驱动的 PBO 之后，**立刻**把整个 PBO 映射出来、`WritebackFromBackend` 进 client 的 buffer 影子，再 `BumpBufferMutationEpoch`（`DirectGLES.cpp` 约 15348–15361，日志 "PBO used, mapping buffer to client memory"）——即读完影子就是新的。异步化必须保住"此后任何 CPU 访问都看到数据"，有两条路：(a) client 只标"GPU 写过"，数据留在 server 的 twin，CPU 访问时惰性拉（既有的 `ResourceReadback`）；(b) server 读完后发一条 `OnBufferWriteback`，与 epoch bump 有序（`CONTRACT-P5C.md` §4.2 / §8.2）。选哪条要先量一下（PBO 通常多大、多久被 CPU 读）再定；(2) PACK 状态本来就推给 server（`PipeFill.cpp` 的 `EmitPixelPackState`），server 是否可以直接按 PACK 布局落字节，还是仍然回紧排数据、由 client 散射（后者就得等，等于没做）；(3) PBO 太小 / 范围越界仍然是 client 侧的 `RecordReadbackRangeError`，别改。
- **门**：新增场景——PBO 读 + 之后 `MapBufferRange` 读回的字节与 monolith 逐字节相同（含非紧 PACK 状态：`PixelStoreSweepScenario` 的形状）；该 op 的 `by_op` 回包等待为 0（`MOBILEGL_PIPE_STATS=1` 的 `P65LinkMetrics kind=frame-op`）；**red-once**：改回"等回复"后计数门变红；非 PBO 路径（`BandedReadbackScenario`）行为不变。

### W2 — `texture-remint-pull`：先做普查再定要不要协议

1. 普查：在四条 A/B trace 与你能拿到的 trace 语料上跑 `MOBILEGL_PIPE_STATS=1`，读 `TextureRemintPulls` 计数（`MG_Util/Metrics/PipeStats`；文档记录过"780 个统计窗口 2 次"）。
2. 判定：重铸重放的字节是否已在 `StagedTextureStore`（整段 staged run，按 (uploadTarget, level) 覆盖）。**[I]** 若是，client 拉取没有意义，重放应改读 server 自己的 store，`Fatal{UnmigratedEmulation,"texture-remint-pull"}` 变成普通路径；"已定义但没字节"的层（纯 `glTexStorage` 未上传）按 monolith 的行为（跳过 / 零）处理，先在 monolith 里测出来。
3. 结论若是"不需要拉取"：删掉 `OnTexturePullRequest`（回调 9 → 8，`kMGPipeCallbackCount` 与 `static_assert` 同步），写进裁定日志；`TextureRemintPullScenario` 改成"image 绑定后纹素与 monolith 相同、不 Fatal"。若确有 store 没有字节的层，再按 `design/04` §8.4 设计协议（server 发请求事件、client 回 `ResourceSubData` + `ResourceSubDataComplete{pullSerial}`、apply 线程有界等待、终止符可带零个 region），**并先回来问用户**。
- 注意 Magma 与 Espryt 是两条路径（Espryt 的重铸在 `Managers.cpp`，Magma 的 T5 见 `CONTRACT-P5C.md` §2.3）。

### W3 — 日志分级（`OnLog`）

- **[V]** server 转发日志时级别写死 Info（`Server/ServerMain.cpp` 的 `ForwardLog`：`CreateLogLine(builder, LogLevel::Info, …)`），因为 `LogForwarder` 的签名没有级别（`MG_Util/Debug/Log.h:139`，`void (*)(void*, const char*)`）。`LogLine` 里本来就有 `level` 字段（`protocol.fbs`），**不需要改 schema**。
- **做**：转发签名带上级别；client 写 `<base>.server.log` 时保留级别；`≤WARN` 有损（控制 socket 写不动就丢并计数）、`≥ERROR` 无损 + 限速。决定后把 `OnLog` 回调删掉（没有任何生产者，日志不走 `gMGPipeCallbacks`）。
- **门**：单元测试覆盖级别映射与丢弃计数；故障注入 F2（§4）。

### W4 —（可选）事件量

只有当 `OnBufferWriteback` / `OnGpuWritten` 的事件数在某个负载里成为问题（看每帧事件数、`eventDropped`、writeback 切片数）才做批处理 / 收窄；否则写进裁定"没有实测需求，不做"。

### W5 — `OnGlError` 有序（默认不做）

保持 P5e 的放宽；把"下一 drain 点观察"写进 `CONTRACT-P9.md`。要收紧的话（`glGetError` 触发强制追平）先问用户，会引入新的同步点。

### 收尾（必做）

`MG_Remote/CONTRACT-P9.md`（只记实现所需的协议与行为）、`notes/p9/INTEGRATOR-DECISIONS-P9.md`（一条裁定一个 ID，形式同 `notes/p12/INTEGRATOR-DECISIONS-P12.md`）、`notes/p9/README.md` 的阶段状态、`ROADMAP.md` 的 P9 行与 `notes/DEBTS.md`（关掉的债、转出的债）。

## 4. 故障注入（两条，我定的）

| 编号 | 场景 | 期望 |
|---|---|---|
| F1 | server 在 W1 的异步 PBO 读还在飞的时候被杀，随后 client `glMapBufferRange` 该 PBO | 以 device-lost 收场（DECLINED / 具名 Fatal），**不能读到"看起来成功的旧字节"** |
| F2 | client 停止读控制 socket（或 SEG_EVENT 不排空），server 持续产生 `≤WARN` 日志与事件 | apply 线程不被日志卡住超过一个有界时间；`≤WARN` 丢弃计数上涨；`≥ERROR` 不丢；事件环走既有的 `ReverseChannelForfeit`（Ph）而不是挂死 |

沿用 R-16：每条门都要能因它存在的理由变红，并真跑过一次红。

## 5. 怎么量、怎么验

- **回包等待的账**：`MOBILEGL_PIPE_STATS=1` 后 client 日志里每帧一行 `P65LinkMetrics kind=frame-op frame=N wait_replies=K by_op=<op>=<n>`，整段的汇总在 `kind=summary`。`op` 是 `MG_Pipe/PipeCalls.def` 里的 **1 起始序号**：回复行是 1 GetCaps、2 ResourceCreate、3 ResourceRespecify、5 MapPersistent、8 FenceStatus、9 FenceWait、14 QueryAvailable、15 QueryResult、47 SetTextureParams、48 ResourceSubData、52 ResourceReadback、55 GetTextureImage、58 ReadPixels、69 QueryTimestamp（共 14 行带 `kReplySlot`）。
- **基线（Redmi，WSL client 经 Wi-Fi TCP）**：rd12 整段 324–350 次回包等待，全在加载帧；稳态每帧约 1 次（FCL 里的 Minecraft 是 `FenceWait`，op 9，属于 P10）；openra 整段 1 次。数据与做法见 `notes/p12/CROSSHOST-ACCEPTANCE.md`、`tools/device_bench/disagg/crosshost_accept.sh`。**性能只记录、不设门**，对比用逐线程 CPU、Redmi 定频配对 A/B（`guide/pin-verification-2026-09-07.md`）。
- **门**（`notes/p12/gate.sh` 是全套的写法）：`unit`、`integration-split`、`integration-spawn`、`integration-tcp`、`integration-magma-{split,spawn,tcp}`、`integration-gpu`（`MOBILEGL_TRANSPORT=monolith` 与 `inproc` 各一遍）、卫生检查、`fatal_census.py`（79，只降不升）、`link_ratchet.py`（171）、`protocol_revision_pin.py`、`wire_declines_audit.py`、`check_doc_citations.py --strict`。
- **改 wire 就要**：`protocol.fbs` 改动 → `MOBILEGL_PROTOCOL_CONTROL_REVISION` 加 1、`python3 scripts/ci/protocol_revision_pin.py --write`、重新生成头文件；`EventKind` / opcode 只追加；新 `Fatal{Word}` 要在 `MG_Remote/FatalFamilies.def` 有行。
- **G1**：pull 构建的符号集与 `.text` 对基线恒等。所有新代码放在 `MOBILEGL_BUILD_DISAGGREGATED` / `MOBILEGL_PIPE_PUSH` 下。**G1 只能在同机同编译器上比**（`gate.sh` 已改成不同源就说"未比较"）。本机的做法：WSL Arch 的 `~/w7/pipe`（`build-linux` 是 pull 配置，基线 `.text` = `0xa52203`，符号 0 / 0）。

## 6. 仓库规矩

- 提交一行：`[Type] (Scope): description`，**不加 `Co-Authored-By` 或任何署名行**；第一次提交前 `git var GIT_COMMITTER_IDENT` 必须是 `Swung0x48`。推送用 `git push origin refs/heads/feat/disaggregated`（origin = GitHub；Windows 上分支名大小写会撞 `Feat/`）。
- 设备只用红米 `2f7cbe2e`（Adreno 830，root），别的 adb 设备一律不碰；只读动作（截图等）不用问。多设备在线时 adb 必须 `-s`。
- 不要拉 GitHub LFS（限额）：fixture 先从本地已 hydrate 的副本复制；`git lfs checkout` 在 WSL 上会把真文件覆盖成指针。
- 临时文件放 scratchpad，阶段结束清掉（WSL 也要）；耐久的放 `~/w7/notes/<阶段>/`。
- 不顺手修 `dev` 上的 bug；发现的记成独立条目。README 只放介绍与操作流程，排查叙事进 `notes/p9/`。
- 收官审查：P12 时用户裁定"不派 agent / Codex，自己核对并跑 G1"；P9 收官前先问用户是否沿用。
- FCL 是另一个仓库（用户的 fork），P9 不需要动它。

## 7. 回来汇报时给我

每个工作包一段：改了什么（提交号）、跑了哪些门（结果）、red-once 是哪条断言变红、没做的与为什么、需要用户裁的点。W2 的结论（要不要拉取协议）和 W3 的 `OnLog` 去留，请在动手实现前先报一次。
