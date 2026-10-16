# BRIEF-P5 — 传输 + inproc applier + 发射表

基线：`feat/disaggregated @ a29807cc`（P4a 收官 `8c458cd5` + docs D.5 `a29807cc`）。
路线图行：`docs/Disaggregated/ROADMAP.md:21`（估 12 天，里程碑「第 99 天：首个 IPC 帧（缩减路径）」）。
侦察产物：`~/w7/notes/p5/scout-{wire-codec,transport,backend-install-and-thread,caps-reply-reverse,
unmigrated-census,gpu-writes-and-persistent-map-push,test-ci-plumbing,premortem}.md`（8 份，4494 行，
全部 file:line 可复核）。本 brief 引用它们时写 `scout-<slug>:§n`。

---

## §0 这一阶段要交付什么

一句话：**让 `MOBILEGL_TRANSPORT=inproc` 下的一帧，真正从客户端编码进 ring、由另一条线程解码并应用到后端，
而且那条 ring 上跑的字节与 P6 的 spawn 完全相同。**

范围内（P5 owns）：
1. G3 编解码器的**真实实现**（今天只有骨架，见 §1.1）。
2. `MG_Remote/Client/` 与 `MG_Remote/Server/`：会话对象、发射表、applier、`ServerLoop` 与 apply 线程。
3. `MG_Backend/Init.cpp` 的单一 hook + `MG_Config::Transport` + `MOBILEGL_TRANSPORT` 解析 + CMake 布线。
4. `MGPCaps` 快照（客户端只能从快照答 56 个 caps 读点）。
5. 阻塞 `read_pixels`、客户端保守 `MarkGpuWritten`、块粒度 persistent-map 推送 + `persistent-map-push` 出数。
6. 两个新场景（`TriangleScenario`、`PersistentCoherentMapScenario`）、`DirectGLES.Split.` 车道、
   trace-replay 的 `SPLIT` 臂、`build-split` 构建目录与门脚本第五部分。
7. 未迁移输入的**可见化**：字段归属表 + 生成器检查 + 严格模式对照。

范围外（明确不做，写进 brief 免得包去建）：
- `spawn` / `SocketTransport` / `ServerMain` / `HeadlessGL` fork 预检 → **P6**。
- G3 分块协议、`HostResolve.cpp`、`MGHostSpan` 的 split 填法、`IndexHostMirror` → **P8**（见 R-10）。
- `SEG_REPLY` 的通用异步池、`OnGpuWritten` 收窄、纹理拉取四缓解 + 终止符、`OnLog`、`OnXfbScatterReady` → **P9**。
- client 铸造 sync/query handle、轮询入口门铃化、present credit 叠加公式 → **P10**。
- T0/T1 采纳档（P5 恒定跑 T2，见 R-6）→ **P11**。
- 71 条目录里的 **34 条无 applier 无发射器的行**，只做 §3 verb 普查点名的那一小撮（R-4）。

---

## §1 侦察结论：十条必须先知道的事实

**1.1 G3 没有编解码器。** `scripts/gen_pipe.py:605 gen_wire` 只产 8 字节头、`MGPWireOp` 枚举、71 个
`MGPWireRec_*` 结构 + 尺寸断言、以及一个**每条臂都 `return false` 的 bounds gate**
（`PipeWire.inc:708-933`；生成器自称 "SKELETON"，`gen_pipe.py:666`）。没有 encoder，没有 decoder。
G1 的两张表（`gMGPipeScreen`/`gMGPipeContext`，`MGPipe.h:137-138`）与 G2 的 71 个 thunk 同样**零调用者**。
今天真正的边界是 `MG_Impl/Pipe/*` 直接调 ~45 个 `MGPipeApply*`。`scout-wire-codec:§1`、`scout-install:§1.4`。

**1.2 Transport 层写完了但从未被编译过。** `Ring/Doorbell/ShmSegment/FdPassing/Framing/InProcessTransport`
共 2747 行、52 个测试；但 `.github/workflows/test.yml` 里**没有任何一次 cmake 传
`-DMOBILEGL_BUILD_DISAGGREGATED=ON`**，`wsl_p4a_gate.sh` 也没有。P5 的第一次 `ON` 配置**可能是这些文件
在本分支 HEAD 上第一次见到编译器**。给它留预算。`scout-test-ci:§5.2`。

**1.3 `InProcessTransport` 是消息队列，不是 ring。** 两个 `deque<vector<uint8_t>>` + 两个 condvar 门铃
（`InProcessTransport.cpp:38-97`），既不碰 ring 也不碰任何 codec。"inproc 走与 spawn 相同的 G3 编解码"
**不能靠改这个类实现**，要在它之上加一个持有 ring 的会话对象（`ARCHITECTURE.md:573-576` 把
`Client/`、`Server/` 标为 `[P5+]`，`Transport/` 下 P6 只加 `SocketTransport`）。`scout-transport:§3`。

**1.4 五个水位线和两个事件标志是声明了但没人写的。** `appliedSeq/submittedSeq/retiredSeq/
completedFrameSerial/presentAckSerial`、`eventRingFull/eventDropped`（`Ring.h:69-81`）全树只有
`InitRingControl` 把它们清零。§11.6 的整个 credit 协议是 P5 的。`scout-transport:§1`。

**1.5 blob 规则今天是"反着的"。** 每一条带 `MGPBlobRef` 的记录都写 `Seg=None, Size=0,
Offset=<宿主地址>`，字节作为**伴随指针**旁路传递；三条甚至传的是**有类型的前端指针**
（`const SamplerParameters*`；`const LinkArtifacts*` + `const SpirvArtifacts*`）。
另有两个载体带 `MGPBlobRef` 却**没有 `kHasBlob` 标志**：`MGPCaps`（两个）与 `MGPSamplerDesc`。
`scout-wire-codec:§2.1/§4`、`scout-premortem:§3`。

**1.6 63 个 `PipeInputs` 字段里，31 个没有任何推送记录供给**（24 个 V/O + 7 个 sticky F）。
缩减路径（clear + triangle + readPixels，再加 OpenRA）**实打实要读其中 21 个**，其中
`PrepareForDraw` 无条件读两个前端指针（`DirectGLES.cpp:4486 GetBoundVertexArray`、
`:4497 GetProgramForDraw`），退役它们的是 **P8**，不是 P5。`scout-unmigrated-census:§3`。
并且：**服务端没有任何东西 stamp 毒化世代**（applier 故意不 stamp，`PipeInputs.h:612-618`），
所以一个"纯粹的"服务端会在 `SyncRenderState` 的第一次读上就 `Fatal{…@<none>}`。

**1.7 七个 sticky（F 类）字段对毒化免疫**（`PipeFilled.inc:421` 无条件返回 fresh），而它们恰恰是
把前端对象或前端写入直接交给后端的那七个。P5 的退出门"未迁移字段读 = Fatal"**在最危险的七个字段上
结构性失明**。`scout-premortem:§0(B)`。

**1.8 acceptance 是同步返回值。** 四个返回 `Bool` 的 applier 入口
（`ResourceCreate`/`Respecify`/`ResourceSubData`/`SetTextureParams`）与 `MapPersistent` 的 `void*`
决定客户端**破坏性**状态变更（清 dirty、latch 参数、采纳指针）。队列一插进去，答案在调用点就没了。
`scout-premortem:§0(C)`。

**1.9 `inproc` 天然会作弊。** 同一地址空间里 `MGHostSpan::Ptr`、`Blob.Offset`、
`MGPipeApplyMapPersistent` 返回的指针**全都能解析**，于是 P5 的六条退出门可以在
"一个字节都没离开地址空间"的实现上全绿。`scout-premortem:§0(A)`。这条决定了 R-2。

**1.10 客户端的消费者门读的是服务端的事实。** `P4aFamilyHasItsConsumer()` / 资源子系统门读
`MGPipeGetResourceOps()`——那是**后端**注册的表。inproc 下碰巧答对；spawn 下客户端进程根本没有后端，
四个 P4a 族 + P3a 的 buffer 族**一条记录都不发**，静默全灭。`scout-premortem:§1 class 4`。

---

## §2 裁定（R-1…R-14）。包不得推翻，只能引用。

### R-1 — 目标形态：**lockstep inproc**

P5 交付的 `inproc` 是：
- **一条真正的 apply 线程** `mgl-srv-apply`，启动时把原生 context 变成自己的并**终身持有**
  （`ARCHITECTURE.md:535` 的形状，P6 直接继承），于是 `DirectGLES.cpp` 的 16 个
  `IsBackendContextCurrentOnThisThread()` 站点与 `Managers.cpp` 的 19 个 `CanTouchGLNow()` 站点
  **在服务端答 true**，off-thread 降级消失（`scout-install:§3.3`）。
- **客户端在每个 verb 边界阻塞等 `appliedSeq == emitSeq`**（下称**verb 栅栏**）。

栅栏的理由是 §1.6，不是保守：31/63 个字段仍由客户端的残余填充从活 `GLContext` 里拉，
一条自由奔跑的队列会让后端读到**未来的**字段值（`scout-premortem:H1`）。栅栏在的时候，
**任一瞬间两条线程里只有一条可运行**，于是 `gPipeInputs` 保持单实例是合法的。

配套裁定：
- 开关 `MOBILEGL_IPC_VERB_BARRIER`，**P5 默认 1**；`=0` 是负面对照臂（预期红，允许红，必须被跑过一次并记录
  它以什么方式红）。
- 栅栏是**可退役对象**，不是永久设计：它随 §3 表 2 里 `BARRIER-PULLED` 那一栏被 P3b/P4b/P7/P8 清空而逐族打开。
  每个包在自己的结论里写"我这一族还剩几行 BARRIER-PULLED"。
- **P5 不产生 CPU 收益，这是预期而非回归。** 性能按用户 2026-09-08 规则只记录不设门；
  MEASUREMENTS 里要明写"栅栏成本 = 每 verb 一次 spin/park 往返，这是 P5 的已知税，随栅栏退役而消失"。
- 契约包必须把"栅栏臂下 apply 线程与 GL 线程的可运行性互斥"写成一条**运行期断言**
  （debug/verify 构建：apply 线程进入 applier 时置标志，客户端在栅栏外触碰 `gPipeInputs` 时检查），
  否则这条不变量只活在 brief 里。

**退路（必须提前写下来，不许临场发明）**：若 EGL context 迁移在集成时证明是多日窟窿，允许先落
`inproc-inline`（客户端线程自己 drain ring，不建线程，context 不迁移）作为**中间态**并立刻开第二个包补线程臂。
触发条件：server 包在第 4 个工作日结束时 context 迁移仍未在 `ClearThenReadPixels` 上跑通。
由集成者宣布，包不得自行降级。

### R-2 — **诚实 inproc 规则**（杀掉 §1.9）

`MG_Config::Transport != Monolith` 时：
1. 编码器**必须**把 `MGHostSpan::Ptr` 置 `nullptr`，`Seg/Offset` 指向 `SEG_STAGE`；
2. 编码器**必须**为每一条携带内容的 `MGPBlobRef` 填真实的 `Seg` / 段内 `Offset` / **非零 `Size`**；
3. 解码/应用侧**必须** `Fatal{ProtocolCorruption}`：`Ptr != nullptr`、内容记录的 `Blob.Size == 0`、
   `Size != 0 && Seg == kMGHostSpanSegNone`、以及段内 `Offset+Size` 越过段边界；
4. `MGPipeApplyMapPersistent` 在 split 下**必须返回 `nullptr`**（见 R-6）；
5. 负面对照：`MOBILEGL_IPC_AUDIT=1` 时，服务端在记录 retire 之后把它占的 `SEG_STAGE` 字节填 `0xDD`；
   任何"应用完还留着指针"的实现会在下一帧读到 0xDD 并红。这是 §1.9 与 class 3 的唯一机械对照。

这五条合起来就是 ROADMAP "inproc 走与 spawn 相同的 G3 编解码" 的**可检验定义**。
在 `MEASUREMENTS` 里以「inproc 诚实性五条」的名字登记，每条注明它被观察到红过一次的方式。

### R-3 — **回复槽的 Id 就是记录序号**

十条 `kReplySlot` 调用的 payload **今天一个都不含 `MGPReplySlot`**（`scout-premortem:§1 class 2`）。
不新增字段、不新增分配器：
- 记录序号（`m_emitSeq`/`m_applySeq`，`ARCHITECTURE.md:124`：wire 上没有逐记录序号字段，seq 就是序数）
  **就是**回复槽 id；服务端把答案写进 `SEG_REPLY[seq % slots]` 并把 `seq` 回填进槽头做自检。
- 这样 S-1 那一类"两个家族各铸一套 id、查表永远查不到"在 P5 结构上不可能发生。
- 栅栏在的时候，等 `appliedSeq >= mySeq` 与"等我的回复到了"是**同一次等待**，
  所以阻塞 `read_pixels`、`MapPersistent` 的 decline、四个 `Bool` acceptance（R-5）零额外往返。
- P9 把它推广成异步池时，这条规则升级为"seq 是 id 的**初值**"，不是推翻。

**pad 记录不推进 seq**（见 R-9）。

### R-4 — 发射表：69 个槽，未实现的槽是**具名 Fatal**

`BackendObject_Remote::GetBackendFunctions()` 返回一张 `GlobalBackendFunctionsTable`，
每个槽是一个发射器。**没有任何槽允许为 null**（91 个 `MG_Impl/GLImpl` 站点直接调它，
`scout-install:§1.2`），也**不允许穿透去调驱动**。P5 未实现的槽装
`Fatal{UnmigratedVerb, "<slot>"}`（`MGLOG_F` + `abort`，与 `MGPipeInputPoisonFatal` 同形）。

**实现哪些槽由 verb 普查决定，不由 brief 猜。** 契约包在第 1 天跑一次普查：
把两个场景 + OpenRA trace 跑在"全 Fatal 表"上，按 `Fatal{UnmigratedVerb` 收集点名，
输出 `~/w7/notes/p5/verb-census.md`。预期最小集（猜测，供排期用，不是承诺）：
`Clear`、`DrawArrays`/`DrawElements`（→ `DrawVbo`）、`ReadPixels`、`Present`、`Flush`、`Finish`，
OpenRA 再加 blit/纹理/程序若干；总计 6–15 个。**其余 ~25 条目录行明确不做。**

### R-5 — acceptance 不在客户端重新推导

四个 `Bool` 入口 + `MapPersistent` 的答案，**走 R-3 的回复槽，在栅栏的那次等待里取回**。
禁止的两条路（写下来是为了让复审可以直接判红）：
- ✗ 客户端复制一份 acceptance 谓词（那正是 c0f/c0g 两侧各一份 D-K2 依赖表的形状，P4a 为此付了两轮契约修）；
- ✗ "总是接受"（那是 ID-39 的 66 条 DirectVulkan 用例丢上传，中间再加一条线）。

### R-6 — split 下恒定 T2，且 `LargeArenaAdoptionScenario` 必须变成档位感知

`MGPipeApplyMapPersistent` 在 split 下恒返回 `nullptr`（R-2.4），前端三处已经容忍
（`BufferObject.cpp:238`、`:603-606`、`:657-660`）。理由：`persistent-map-push` 在采纳存活时**结构性为零**
（`PipeStats.cpp:68-70`），而它是退出门。`MOBILEGL_IPC_ADOPT_TIER` 作为**负面对照开关**落地
（默认 2；`=0` 在 P5 下不实现，置为 Fatal "P11"）。
`LargeArenaAdoptionScenario`（断言发生了采纳）在 split 车道下改成断言**decline** + `mpr` 不变——
改断言不改测试名（G14 安全）。`scout-gpu-writes:§2.3`。

### R-7 — 字段归属表是**生成的**，栅栏拉取是**被计数的**

见 §3 表 2。三条机械化要求：
1. 63 个字段 + 7 个 sticky forward，每一个恰好落在
   `RECORD-SUPPLIED(call) | APPLIER-DERIVED(record) | BARRIER-PULLED(retiring phase) | FATAL` 四类之一；
   表由生成器产出并 `--check`（照 `gen_pipe_dirty_surface.py --check` 的形状），**不在任何一类里 = 构建失败**。
2. split 下读一个 `BARRIER-PULLED` 字段：递增新的 `PipeStats::CallClass::ResidualPulls`
   （短名 `rsp`，放进 `#if MOBILEGL_PIPE_PUSH` 块内，G1 安全），逐帧发布。
   **它的值就是 P6/P7/P8 欠债的大小**，P5 结束时进 MEASUREMENTS。
3. `MOBILEGL_IPC_STRICT_ERRORS=1` 把 `BARRIER-PULLED` 提升为 FATAL；
   一条具名测试断言它确实 abort（证明这套插桩不是装饰）。
   同一开关把七个 sticky forward 也提升为 FATAL——**sticky 豁免在 split + strict 下取消**。

### R-8 — 消费者/存活门移到握手，即使 inproc 下它"碰巧对"

`MGPipeResourceSubsystemEnabled()` 与 `P4aFamilyHasItsConsumer()` 在 split 下**必须**读
`MGPCaps::CallMask` 的客户端镜像，不得读 `MGPipeGetResourceOps()`。
理由是 §1.10：inproc 下读错来源会答对，spawn 下会静默灭掉整条推送路径。
门：一条单元用例，给客户端一份缺少资源族的 caps 掩码，断言它一条记录都不发、且计数器点名该族。

`CallMask` 与 `protocol.fbs:94` 的 `tableSlotMask` **是同一个字段**（`ARCHITECTURE.md:114`：
"CallMask 取代『槽位是否为 null』这个隐式能力探测"）。契约包**删掉** `tableSlotMask` 这个名字
或把它改名成 `callMask` 并在 `protocol.fbs` 注释里写清；同时 `maxComputeWorkGroupCount/Size` 与
`prefersCpuXfbPrimitiveAccounting` 三个字段与 `dynamicParameters` 里的同名成员**冗余**，
契约包二选一并写进编码表（§3 表 0）。`scout-caps-reply:§1.4(6)(7)(8)`。

### R-9 — 五条水位线，每条一句话

契约包把这五句写进 `Ring.h` 的头注释并各配一条单元用例：
| 水位线 | 谁推进 | 谁可以等 | 比较 | 批处理 |
|---|---|---|---|---|
| `submittedSeq` | 生产者 publish 后 | 无（诊断用） | `>=` | 允许延迟 |
| `appliedSeq` | 消费者**每条记录**（P5 栅栏期禁止 64 条批处理） | 客户端栅栏、回复等待 | `>=` | P5 禁止 |
| `retiredSeq` | 消费者 drain 完 `SEG_STAGE` 引用后 | 暂存分配器 | `>=` | 允许延迟 |
| `completedFrameSerial` | 服务端 present 完成 | 回收/老化 | `>=` | 允许延迟 |
| `presentAckSerial` | 服务端 present credit 归还 | 客户端 present 节流 | `>=` | 允许延迟 |
通用规则：**批处理只允许让水位线变晚，绝不允许让等待者看到"比实际做的更多"的值。**
**pad 记录（`kRecPad`）不推进 seq**——两侧都必须跳过它再计数，否则每一次回绕都会让 seq 永久错位且无校验和。

### R-10 — blob 走 `SEG_STAGE`，记录上界，以及内容侧的分块

规则：**所有 blob 走 `SEG_STAGE`，记录本身只带 `{Seg, Offset, Size}`**，于是
`CreateShaderState` 的记录是 192+8 字节而不是整个 archive。var-tail 的长度由 GL 上限或发射器自己的
切分（`MGPipeForEachSubDataRecordRange`，`PipeFill.cpp:694-713`）界定。因此**没有一条记录会超过
`RingProducer::MaxRecordBytes() == Capacity()/2`**，超过即 `Fatal{RingOverrun}`——**记录本身永不分块**。
**内容侧分块已落地（本节原写"P5 不做分块…分块留 P8"）**：`SEG_STAGE` 仍是"一条记录一个整 blob"的
线性 arena，但会超出它的内容由发射侧按 stage chunk 预算切开——预算 `MGPipeStageChunkBytes()` =
`clamp(segment/4, 4096, segment)`，默认 `MOBILEGL_IPC_STAGE_MB=32` → 8 MiB
（`MG_Remote/Client/GpuWritePending.h:169`，无 session / monolith / server 角色自身返回 0 = 不切）。
已接入的两条内容路径：**buffer 内容走查**（`PipeFill.cpp` 的 `MGPipeContentChunkCap`，`1e7c372e`）
与**纹理一级的整宽 slab**（`TextureEmit.h` 的 slab 切分，服务端由 `StagedTextureStore::AdoptRun`
按 run 拼回整级、覆盖不全即 `Fatal{StageSnapshotTooNarrow}`，`9469d48e`）。**未接入分片的 record
类型、以及单片仍大于 arena 的情形，仍由编码器的 `Fatal{RingOverrun, "SEG_STAGE"}` 兜底**；编码器
上界（一条记录的 header + payload + 内联尾）与这条 Fatal 都不因分块而放松。
**证明义务（不变）**：wire 包加一个 `PipeStats` 最大记录字节的 max 计数器，在缩减路径 + OpenRA 上出数，
写进 MEASUREMENTS。若实测有记录逼近 `Capacity()/2`（默认 `MOBILEGL_IPC_RING_MB=8` → 4 MiB），
立刻上报集成者，由集成者决定是为该 record 类型加分片还是调大默认 ring。

### R-11 — 暂存字节的生命期：**应用侧不得跨返回持有指针**

`SEG_STAGE` 的一段字节从 publish 到 `retiredSeq` 越过该记录为止有效。
**任何 applier 入口不得把指针留到返回之后**——Espryt 的 `GLESBufferResource::hostBytes`
（`Managers.h:839`，写于 `Managers.cpp:1980-1983`/`:2035`，读于 `:2000/:2062/:2080/:2111/:2741/:2843`）
是今天唯一的违例，它在 split 下**必须改成把字节拷进服务端自有存储**。
对照见 R-2.5 的 `0xDD` 毒化。这是 P4a 分类学 class 3 在 P5 的复发点。

### R-12 — 反向通道：P5 只接三条半

`MGPipeCallbacks` 十条里 P5 接：`OnGpuWritten`(#2)、`OnBufferWriteback`(#3)、`OnSurfaceChanged`(#7)，
外加 DirectVulkan 车道需要的 `OnCapsInvalidated`(#8)。
`OnGlError`(#1) 在 inproc 下先走 `RecordError` 的 sticky forward（计入 `rsp`），
其有序化留 P9。其余六条留 P7/P9。**不新增第十一条回调**（`MGPipeCallbacks.h:56-58` 的 static_assert
存在就是为了让这个代价可见）；服务端 context 死亡的再开事件折进 `OnCapsInvalidated`
（`scout-gpu-writes:§5.2`）。
`OnCapsInvalidated` 的 DirectGLES 缺口按 `scout-caps-reply:§1.5(1)` 的方案 (a) 解决：
**每次 `InitCapabilities` 重跑就重发一次 caps 快照，客户端把"再次到达"当作失效**，不改 dev 形状的后端代码。

### R-13 — 目录与编码的四处不一致，契约包一次性改掉

1. `GetCaps`/`MGPCaps` 与 `CreateSamplerState`/`MGPSamplerDesc` 带 `MGPBlobRef` 却无 `kHasBlob` → 补标志
   （改标志不移动 opcode，`PipeCalls.def:26-29` 的编号规则不受影响）。
2. `ResourceFlushRange` 是 `kNone` 却带内容指针 → 或补 blob 成员 + 标志，或在编码表里写明
   "长度由 `MGPFlushRange` 的 range 决定，字节走 `SEG_STAGE`，无 `MGPBlobRef`"。二选一，写下来。
3. `ResourceRespecify` 的 `initialBytes`（每一次 `glBufferData(size,data)` / `glTexImage*(…,data)`）
   **今天没有任何载体**（`MGPResourceDesc` 没有 `MGPBlobRef` 成员，`PipeApply.h:76-79` 明确说这是故意的）
   → 必须在编码表里给它一个归宿：用 pad 位加一个 `MGPBlobRef`（`MGPResourceDesc` 有 `Pad0`/`Pad1`，
   但 24 字节的 blobref 放不进 pad，需要扩结构并改 `MGP_ASSERT_POD(…, 88)`），或者规定
   "初始字节永远以一条紧随其后的 `ResourceSubData` 送达"。**推荐后者**：零结构变更、零 POD 断言移动、
   复用已经走通的路径；代价是多一条记录。由契约包定，写进 §3 表 1。
4. 生成一张**逐 opcode 的标志表**（`kMGPipeCallFlags[kOpCount]`）——今天没有任何生成物导出标志，
   六个包会各硬编码一份（`scout-wire-codec:§2.7`）。一行 `gen_pipe.py` 改动。

### R-14 — 不要把任何 P5 产物挂在临时 CI 触发分支上

`test.yml:9-12` 的 `feat/disaggregated` 触发与四个带同样分支守卫的 job 步骤（`:1694/:1698/:1727/:1731`）
是**要在并入 dev 前删掉的**。P5 的新 job / 新步骤要么无条件跑，要么 `workflow_dispatch`，
不得写 `github.ref == 'refs/heads/feat/disaggregated'`。`scout-test-ci:§4.1`。
---

## §3 三张必须在任何包分叉之前冻结的表

P4a 的教训是：契约改了七次（c0b…c0g），根因表在 `fable-seam-audit.md:359-398` 与 `MEASUREMENTS.md:557-573`。
这三张表各杀掉那份清单的一整列。**契约包（c0）的产物就是这三张表 + 让它们编译得过的头文件与桩。**
表的权威副本落在 `MobileGL/MG_Remote/CONTRACT-P5.md`（随代码走，不是只在 notes 里），
本节是它的规格与已知行。

### 表 0 — 编码表（杀 class 1）

一行一个**不是句柄**的 wire 字段：位布局、零的含义、读者。已知必须在表里的行：

| 字段 | 规定 | 备注 |
|---|---|---|
| 段 id 空间 | `SEG_CMD=1, SEG_STAGE=2, SEG_REPLY=3, SEG_EVENT=4, SEG_SHADOW=5, SEG_ADOPT=6`，与 `protocol.fbs:36-44` 的 `SegmentKind` **同值**；**0 永远是「无段」**，不得作为真实段 id | `kMGHostSpanSegNone = 0` 已定；`kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFF` 保留给 P8 |
| `MGPBlobRef{Seg,Offset,Size}` | `Seg` 用上表；`Offset` 是**段内字节偏移**，不是宿主地址；`Size` 非零即"本记录声明了它的 blob"，split 下必须非零（R-2） | 今天全树是 `{0, <地址>, 0}` |
| `MGHostSpan{Ptr,Seg,Size,Offset}` | split 下 `Ptr` 恒 `nullptr`；32 字节布局**不得重排**（`MGPipeHostSpan.h:29-31`） | P5 的缩减路径应当**一条都不产生**（见 §4 的 cap 位裁定） |
| caps 载体 | **只有一个**：`MGPCaps`。`protocol.fbs` 的 `CapsSnapshot` 是它的运输形，三个 blob 是 POD 字节像；`tableSlotMask` 改名 `callMask` 或删除（R-8）；`maxComputeWorkGroupCount/Size`、`prefersCpuXfbPrimitiveAccounting` 与 `Dynamic` 冗余，二选一 | `MGPCaps` 只有**组合式**尺寸断言，因为 `DynamicBackendParameters` 还带 `SizeT`（P0.5 没做）。P5 的裁定：**不重写定宽**，改为在 `Hello`/`Welcome` 里断言双方 `sizeof(DynamicBackendParameters)`、`sizeof(MGPCaps)` 与 `buildFingerprint` 相等，不等即 `Fatal{AbiMismatch}`。P6 的 spawn 同机同 ABI，继承这条即可；定宽重写记到 P7 的账上 |
| `MGPSubData::Target` | 低字节 = 资源目标，高字节 = 上传目标（P4a ID-12 的编码） | 已定，抄进表 |
| `MGPImageView::Access` | `ImageEmit.h:146-159` | **今天只活在包头里**（P4a R-3），本表是它的第一个 wire 读者，必须收进契约 |
| `MGPSamplerView::Target` | `SamplerEmit.h:900` | 同上 |
| `MGPFramebufferState::DrawBuffers[8]` 的 −1 / default-token 收窄 | `FramebufferEmit.h:146-163` | 同上 |
| `MGPReplySlot::Id` | **= 记录序号**（R-3）；服务端把 seq 回填进槽头自检 | |
| 槽头 | `{Uint64 Seq; Int32 Status; Uint32 Size;}`，`Status` 0=OK、1=DECLINED、2=ERROR；`DECLINED` 是**真答案**不是失败（`MapPersistent` 的 nullptr 语义） | 新增，契约包定义 |

### 表 1 — 字节载体表（杀 class 1/3/5、H2、H3）

19 行，一行一个"带内容或带尾巴"的调用。列：
`PipeCalls.def 标志` · `payload 里的 blob 成员（或无）` · `今天的伴随指针` · **`字节住哪个段`** ·
**`谁拥有这块内存`** · **`槽什么时候 retire`（apply / submit / GPU-complete）** · **`谁声明长度、谁交叉校验`** ·
`kReplySlot 行的回复名`。

表头之上先写三条规则（R-2、R-11 的正式措辞）：
1. split 下，payload 带 `MGPBlobRef` 的记录若 `Blob.Size == 0` → `Fatal{ProtocolCorruption}`；
2. split 下 apply 侧 `MGHostSpan::Ptr != nullptr` → `Fatal`；
3. 任何 applier 入口不得跨返回持有指针，除非记录标了 borrowed；**Espryt 的 `hostBytes` 队列是唯一具名例外，
   且 P5 必须把它改成服务端自有拷贝**。

已知的 19 行（来源 `scout-premortem:§3` + `scout-wire-codec:§2`）：
`CreateRenderState`、`CreateVertexElements`、`CreateShaderState`、`SetDynamicState`、`SetGlobalConstants`、
`SetResidualValueState`、`ResourceSubData`、`BufferSubDataResident`（以上 8 条 `kHasBlob`）；
`SetVertexBuffers`、`SetSamplerViews`、`BindSamplerStates`、`SetShaderImages`、`SetShaderBuffers`、
`SetStreamOutputTargets`、`SetVertexAttribDefaults`、`DrawVbo`（以上 `kVarTail`，其中后两条另带 host span）；
`ResourceRespecify`（R-13.3）、`CreateSamplerState`（R-13.1）、`ResourceFlushRange`（R-13.2）。

**免费的一块**：`CreateShaderState` 的序列化已经存在且已被验证——
`EncodeProgramArtifacts`/`DecodeProgramArtifacts`（`MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h:53,60,cpp:252,264}`），
有专门测试套件，并且 verify 构建已经在**每一个真实程序**上跑过往返
（`PinProgramArchiveRoundTrip`，`PipeApply.cpp:1056-1087`，调用点 `:2638`）。
**任何包都不得再写第二份程序归档编解码器。**
`CreateSamplerState` 的序列化 = `memcpy(sizeof(SamplerParameters))`（POD，`borderColorForm` 必须逐字节存活）。
`MGPCaps` 的两个 blob 的序列化器**不存在**，是 P5 的新活（`MGPipeTypes.h:135-136` 明说"随传输在 P5 落地"）。

### 表 2 — `PipeInputs` 字段归属表（杀 class 6、裁定 B/§1.6/§1.7）

63 个字段 + 7 个 sticky forward，每个恰好一类，**生成 + `--check`**（R-7）。
四类的定义与已知归属：

**RECORD-SUPPLIED（40 条，已有）** — `kMGPipeFieldEmittedBy` / `kMGPipeEmittedFieldCount = 40`
（`generated/PipeFilled.inc:336-402`），减去 `EmittedCallSuppliesTheWholeField` 里返回 false 的 9 条。

**APPLIER-DERIVED** — applier 自己写的：render-state 镜像、`m_pixelStore[0]`、capability、
当前顶点属性、patch，加 `MGPipeDeriveRenderStateFields`（`PipeApply.cpp:2157`）派生的那批。

**BARRIER-PULLED（P5 的债，逐条点名退役阶段）** — 缩减路径实际会读的 **21 个**
（`scout-unmigrated-census:§3` 的并集）：
```
GetActiveTextureUnit              GetPixelStoreParameters(unpack 半)   GetTextureUnitObject
GetBoundVertexArray               GetProgramForDraw                    GetTransformFeedbackCapturedVertices
GetBufferBindingSlot              GetSamplingResolutionGeneration      GetTransformFeedbackGeneration
GetBufferBindingPoint             GetTextureBindGeneration             GetTransformFeedbackProgram
GetTouchedBufferBindingPointCount GetTextureContextId                  GetBoundTransformFeedbackLifetimeId
GetCurrentVertexAttribute         GetMaxTouchedTextureUnit             IsTransformFeedbackActive
GetFramebufferBindingSlot         GetImageTextureBinding               IsTransformFeedbackPaused
```
退役阶段：`GetBoundVertexArray` / `GetBufferBindingSlot`(indirect 半) → P8；
framebuffer / texture-unit / image / program / XFB 一族 → P3b/P4b（Espryt）与 P7（Magma）；
三个纹理快门（`GetTextureContextId`/`GetTextureBindGeneration`/`GetSamplingResolutionGeneration`）
**不是"迁移值"而是"服务端用自己的 Serial 回答"**（`Coverage.def:220-224`），P3b/P4b。
**`GetPixelStoreParameters` 必须在本阶段拆成 pack / unpack 两个字段**，否则 readback 路径会因为
unpack 半没有载体而整字段 Fatal（`scout-unmigrated-census:§3` 的两种读法）——这是 P5 自己的一条小迁移。

**FATAL** — 剩下的 3 个非 sticky 未迁移字段（`GetBoundTransformFeedbackName` 已死、
`GetTransformFeedbackPausedPrimitiveCounter` 只在 `kQuery`、`GetProgramForDispatch` 只在 `kDispatch`）
加上 `MGPipeUnmigratedEmulation` 的**五个站点**（`Managers.cpp:5334`、`DirectGLES.cpp:8051/:8702/:8997/:10623`）——
后者在 split 下从 `(void)name;` 变成 `Fatal`，这是 **P5 改一个函数就给五个站点装上牙齿**
（`PipeApply.cpp:2812-2817` 自己这么写的）。

**七个 sticky forward** 单列一节：`GetBufferBindingPointCount`、`GetProgramObject`、`GetTextureObject`、
`HasOpenTransformFeedbackSpan`、`ValidateProgramName`、`InvalidateCompileEnv`、`RecordError`。
P5 的归属：前五个 = `BARRIER-PULLED`（计入 `rsp`，strict 下 FATAL）；
`InvalidateCompileEnv` → `OnCapsInvalidated`；`RecordError` → 记 `rsp`，有序化留 P9（R-12）。
**`PipeFilled.inc:421` 的无条件 fresh 在 split 构建下必须取消**，否则这七个永远不进计数。

同一张表的形状再延伸到两个**没有字段 id 的集合**：
- 客户端保守 GPU-write 集：一行一个后端 `MarkGpuWritten` 站点（6 个：`DirectGLES.cpp:570/:618/:2603`、
  `UniformManager.cpp:1075/:1231`、`VulkanRenderer.cpp:11618`），映射到客户端必须触发的谓词，每行一条单元用例；
  外加 P5 的**两个新生产者**（`glReadPixels`→pack PBO、`glEndTransformFeedback`，`ARCHITECTURE.md:508`）。
- persistent-map 可达集：一行一个 `SyncPersistentMappedRange` 站点（文档记 20，侦察实点 21，
  差异见 `scout-gpu-writes:§2.4`，契约包负责裁定并写进表）。

### 表 3 — 角色/线程所有权表（杀 class 4、class 7、H1/H5/H6）

`ARCHITECTURE.md:581` 说 MGPipe 把需要按角色复制的进程全局从四个降到两个。**这个普查至少少算了五个**
（`scout-premortem:§1 class 4` + `scout-gpu-writes:§5.3`）：

| 全局 | 位置 | 谁写 | P5 裁定 |
|---|---|---|---|
| `gPipeInputs`（~20 KB） | `PipeInputs.h:706` | 客户端残余填充 + applier | **栅栏臂下单实例合法**（R-1 的互斥不变量）；栅栏退役前不得引入第二个写者 |
| `g_applier` | `PipeApply.cpp:396` | applier | 服务端独占；头注释已写"split 下每个被服务的 context 一个" |
| `g_resourceOps` | `PipeApply.cpp:402` | 后端注册 | 服务端独占；**客户端不得读**（R-8） |
| `gMGPipeSegmentResolver` | `MGPipeHostSpan.h:47`（非原子 inline 变量） | `MG_Remote` 安装 | **每角色一份**，在 apply 线程启动之前安装；inproc 下两角色的解析器必须指向各自的段视图 |
| `MG_Impl/Pipe` 的十个 `*Instance()` | `fable-seam-audit.md:122-124` | 客户端 | 客户端独占；**纹理 drain list `m_drain` 是进程级**，审计已经把"每个 client context 一份"记成 P5 项 |
| `ScopedDefaultUnpackState::s_synced` + 五个值影子 | `Managers.cpp:5490-5496` | 后端 | 服务端独占；**第六个漏网的进程全局**，登记即可（P5 缩减路径里客户端角色不碰 GL） |
| `pActiveBackendObject` | `GlobalObjects.cpp:23` | `MG_Backend::Init()` | 客户端装 `BackendObject_Remote`；**服务端的 `BackendObject_DirectGLES` 由 `ServerLoop` 私有持有**。唯一从后端内部读它的站点是 `DirectGLES.cpp:12446`（`ClampSamplesToBackendSupport`）→ 改成把 format cache 传下去，读法 (i) 成立，不需要 `MOBILEGL_BUILD_DISAGGREGATED_INPROC` 的线程键控 shim |
| `gBackendFunctionsTable` | `GlobalObjects.cpp:24` | `Init.cpp:44` | 客户端 = 发射表（R-4）；服务端直接持有真表，不经这个全局 |

表 3 的每一行还要写第四列：**make-current 与 teardown 对它做什么**。
teardown 顺序按 `ARCHITECTURE.md:537`，并补上 `ARCHITECTURE.md` 没写的那一句：
**`Doorbell::Kill()` 是唯一能把 `kWaitForever` 上的 apply 线程叫醒的东西**
（`Doorbell.h:211-221`，`InProcessTransportTest.cpp:344` 已经钉住这个形状）；
先 Kill 再 join，join 之前客户端不得释放任何发射器拥有的 `Vector`。
ID-8 的规矩（没有前端析构函数可以从退出处理器跑进 pipe/后端状态）**对两套角色局部单例各生效一次**；
所有新的 `MG_Remote/Client/*`、`MG_Remote/Server/*` 单例按 ID-8 一律 leak-at-exit，不是新判断。
证明配方继承：两条车道都跑 `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`。

---

## §4 缩减路径的精确定义

P5 的门只对下面这条路径负责，包不得越界实现：

**A. `DirectGLES.Split.ClearThenReadPixelsScenario.*`**（场景已存在，5 个用例，默认 FBO，**不走 PBO**）
→ 需要的目录行：`GetCaps`(1)、`Clear`(57)、`ReadPixels`(58)、`Present`(67)。

**B. `DirectGLES.Split.TriangleScenario.*`**（**新场景**）
→ 再加 `DrawVbo`(59)、`Flush`(66)，程序/VAO/buffer 侧复用 P2/P3a/P4a 已接的记录。
**用 VBO 后端的 draw，不要 client-array 索引**，这样 `kDrawHasUserIndices` 的 `MGHostSpan` 一条都不产生
（P8 才做 host span 的 split 填法）。相应地，**`MGPCaps` 里 `kCapNeedsHostIndexBytes` 与
`kCapNeedsHostUboBytes` 两位在 P5 恒为 0**——这是把 `MGHostSpan` 挡在首个 IPC 帧之外最便宜的办法。
写进表 0。

**C. `PersistentCoherentMapScenario`**（**新场景**，规格全文在 `ARCHITECTURE.md:500`）
→ map `PERSISTENT|WRITE|COHERENT`、透过指针写、**不发任何别的 GL 调用**、draw、回读校验。
**第一天就要写进去的陷阱**：场景大小的 buffer 远低于 16 MiB 的 `kLargeBufferAdoptBytes`，
所以 `TryAdoptLargeStorage` 不会触发，但 `AcquireMemoryRange` 自己的采纳**会**触发
（`BufferObject.cpp:655-661`），于是同一个测试在不同驱动/构建上落进**采纳臂**或**模拟臂**，
两臂代码完全不同。`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` **分不开它们**。
场景必须断言自己在哪条臂上。（R-6 让 split 恒定落模拟臂。）

**D. OpenRA trace，SSIM ≥ 0.99**（`trace_cases.json:14-28`，31249 次调用，640×480，阈值 0.99）
→ verb 集由 R-4 的普查决定。OpenRA **不会加宽字段集**（仍是表 2 的那 21 个），
它加宽的是**站点集**，并且是第一个够到读附件（`Managers.cpp:8603/:8966`）与
`maxTouchedUnit >= 0` 的纹理单元遍历的东西。

不在缩减路径上的、明确留给后续阶段的：fence 全族、query 全族、compute、XFB、
indirect、copy-region、`GetTextureImage`、`GenerateMipmap`、`SetShaderBuffers`、
`SetStreamOutputTargets`、`ResourceSubDataComplete`。
---

## §5 包划分

一个契约包先落地（其余七个都从它之后分叉），然后七个并行包。
每个包在自己的 worktree `~/w7/p5-<slug>` 上工作，结论写 `~/w7/notes/p5/p5-results/<slug>-v1.md`，
中间日志自己清（临时文件纪律：只留当前阶段）。

### 文件所有权（防 P4a 那种同点追加冲突）

| 区域 | 属主 |
|---|---|
| `MG_Pipe/PipeCalls.def`、`PipeFields.def`、`Coverage.def`、`scripts/gen_pipe.py`、`MG_Remote/CONTRACT-P5.md` | **c0** 独占。任何包要改，走集成者。 |
| `MG_Pipe/generated/*.inc` | 由生成器产出；只有 c0 与 p1 可以触发重生成并提交 |
| `MG_Remote/Wire/*`（新目录）、`MG_Pipe/MGPipeHostSpan.h` 的解析器安装点 | **w1** |
| `MG_Remote/Transport/*`、`MG_Remote/Protocol/protocol.fbs` + 生成头 | **s1** |
| `MG_Remote/Server/*`、`MG_Backend/Init.cpp` | **v1** |
| `MG_Remote/Client/*`、`MG_Backend/BackendObject*.{h,cpp}` 的 remote 侧 | **c1** |
| `MG_State/GLState/BufferState/BufferObject.{h,cpp}`、`MG_Backend/DirectGLES/Managers.{h,cpp}`、`MG_Util/Metrics/PipeStats.*` | **b1** |
| `MG_Backend/MGPipe/PipeInputs.{h,cpp}`、`MG_Impl/Pipe/PipeFill.{h,cpp}` 的毒化/残余部分、`scripts/gen_pipe_dirty_surface.py` | **p1** |
| `MG_IntegrationTest/**`、`tools/trace_replay/**`、`.github/workflows/test.yml`、`~/w7/notes/tools/wsl_p5_*.sh` | **t1** |
| `MobileGL/Config.h`、`ConfigLoader.cpp`、根 `CMakeLists.txt` | **c0** 先落全部新旋钮与构建布线，之后任何包新增旋钮走集成者追加 |

---

### c0 — 契约（模型：**fable**；串行，其余包等它）

**交付**
1. `MobileGL/MG_Remote/CONTRACT-P5.md`：§3 的表 0/1/2/3 全文（表 2 的权威副本由 p1 的生成器产出，
   c0 先落规格与四类定义）。
2. `PipeCalls.def` 标志修正（R-13.1/.2）+ `ResourceRespecify` 初始字节的归宿裁定（R-13.3）。
   **不得移动任何 opcode**；`PipeCatalogueTest.cpp:418-422` 钉着最后四个 opcode。
3. `gen_pipe.py`：逐 opcode 标志表 `kMGPipeCallFlags[kOpCount]`（R-13.4）；
   `--check` + `--self-test` 保持全绿，生成物提交。
4. `MG_Config::Transport`（`Config.h`，`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时是 `constexpr Monolith`，
   `ARCHITECTURE.md:576` 的 `#if` 形状）+ `ConfigLoader` 解析 `MOBILEGL_TRANSPORT=monolith|inproc`
   （`spawn|unix:|pipe:` 解析出来但返回"P6 未实现"的具名错误）+ `MOBILEGL_IPC_*` 全家旋钮的解析与默认值
   （`_RING_MB`=8、`_STAGE_MB`=32、`_SPIN_US`=50、`_PERSISTENT_BLOCK_KB`=64、`_ADOPT_TIER`=2、
   `_VERB_BARRIER`=1、`_STRICT_ERRORS`=0、`_AUDIT`=0、`_SERVER_AFFINITY`=auto）。
   解析器抄 `InitBackendType()`（`ConfigLoader.cpp:284-300`）的形状。
5. CMake：`MOBILEGL_BUILD_DISAGGREGATED_INPROC` 选项（蕴含 `DISAGGREGATED`）、
   `MG_Remote/{Wire,Client,Server}/` 的源文件列表、`MOBILEGL_PIPE_PUSH` 源列表
   （`CMakeLists.txt:484-499`）里追加新文件。
6. **头文件与桩**，让七个包当天就能并行编译：
   `MG_Remote/Wire/PipeWireCodec.h`（`EncodeRecord`/`DecodeAndApply`/`StageBytes` 的签名）、
   `MG_Remote/Client/ClientSession.h`、`MG_Remote/Server/ServerSession.h`、
   `MG_Remote/Client/EmitTables.h`、`MG_Remote/Server/PipeApplier.h`、
   `MG_Remote/CapsCodec.h`。全部带 `MGLOG_F`+abort 的桩体。
7. **verb 普查**（R-4）：用全 Fatal 的发射表跑 A/B/D 三条路径，产 `~/w7/notes/p5/verb-census.md`。
   这是唯一一件 c0 必须"先跑起来一点东西"的事——如果 c1/v1 还没有，普查可以用一个最小的
   `--gtest_filter` 桩程序 + 手工 trace 解析做，或推迟到 c1 落地后由集成者补跑并广播。
8. `Ring.h` 头注释补 R-9 的五句 + `kRecPad` 不计 seq 的规则 + 五条单元用例。
9. 纯度：新头文件若从 `ITransport.h` 可达，**不得** include `MG_Util/Debug/Log.h` 或任何够到
   `MobileGL/Includes.h` 的东西（`check_include_closure.py:136-144` 的 `wire-header` 探针），用 `WireLog.h`。

**验收**：`gen_pipe.py --check/--self-test` 绿；三个构建目录（pull/push/split）都配得起来并编译过；
`nm --defined-only build-linux/libMobileGL.so | grep -i MG_Remote` 仍为空（G1 不动）；
`MOBILEGL_TRANSPORT=monolith` 下 `ctest -L integration-gpu` 与今天逐名相同且全绿。

---

### w1 — G3 编解码器（opus）

**交付**
- `MG_Remote/Wire/PipeWireCodec.{h,cpp}`：71 条的 encoder（`MGPWireRec_*` 写入 + var-tail 写入 +
  blob 暂存）与 decoder（bounds + tail 算术 + 段解析 + 调 `MGPipeApply*`）。
  **decoder 不自己实现语义**：它解码完就调今天的 applier 自由函数，`PipeApply.cpp` 一行不改。
- `MGPipeApplyWireRecord`（`PipeWire.inc` 的骨架）改成把工作转给上面这个 decoder；
  生成器里那段 "SKELETON … until P5" 的注释换成真话。
- `gMGPipeSegmentResolver` 的安装（每角色一份，R-表 3）。
- `SEG_STAGE` 的线性分配器 + `retiredSeq` 回收；`MOBILEGL_IPC_AUDIT=1` 的 `0xDD` 毒化（R-2.5）。
- `MGPCaps` 的两个 blob 序列化器（`FormatCapabilityCache` 与 `RendererInfo`）+ 往返单元测试。
  `CreateShaderState` 用**已有的** `EncodeProgramArtifacts/DecodeProgramArtifacts`，禁止重写。
- R-2 的四条 Fatal 臂 + R-10 的最大记录字节计数器。
- var-tail 的**长度交叉校验**：`MGP_WIRE_CHECK_BOUNDS` 今天只证明 `size >= sizeof(rec)`，**看不见尾巴**；
  一条声明 `Count=4000` 只带 8 字节的记录今天能过。decoder 必须按 count 重算总长并与
  `MGPWireRecHeader.Size` 对齐校验。

**验收**：新单元套件 `MG_Test/Wire/PipeWireCodecTest.cpp`——71 条里至少覆盖
{每个标志类至少一条、两条双尾巴（`SetShaderBuffers`/`SetStreamOutputTargets`）、
`DrawVbo` 的条件尾巴、七 blob 的 `CreateShaderState`}，加四条 Fatal 的死亡测试。

---

### s1 — 会话与传输（opus）

**交付**
- `MG_Remote/Client/ClientSession.{h,cpp}` + `MG_Remote/Server/ServerSession.{h,cpp}`：
  在 `ShmSegment` 上建 `RingControl` + `RingProducer`/`RingConsumer`（**inproc 也用 `ShmSegment`，不用 `new`**，
  这是"同一条代码路径"的一半），拿 `InProcessTransport` 的两个门铃。
- 握手：`Hello`/`Welcome`/`CapsSnapshot`/`SurfaceOp`（今天零调用者）；
  `Welcome` 的四个 `SegmentRef` 尺寸已被 `ProtocolSmokeTest.cpp:72` 钉住，照抄。
  ABI 断言按表 0（`sizeof(DynamicBackendParameters)`、`sizeof(MGPCaps)`、`buildFingerprint`）。
- 五条水位线 + 两个事件标志的**真实写入**（R-9），含 publish→`NotifyIfParked` 的顺序
  （`RingTest.cpp:446` 已经钉住这个调用序）。
- `SEG_REPLY` 的最小槽池 + 槽头（表 0）+ `seq % slots` 寻址（R-3）；
  `SEG_EVENT` 只做到"能承载 `OnBufferWriteback` / `OnGpuWritten` / `OnSurfaceChanged`"，溢出策略留 P9。
- 两个门铃访问器今天挂在具体类 `InProcessTransport` 上而不是 `ITransport`
  （`InProcessTransport.h:64-68`）——**在 P5 里决定**：要么提到 `ITransport`，要么在会话层封一层。
  P6 才发现的话要重铺一个包的调用点。
- 双角色峰值 RSS 的记账钩子（`/proc/self/status:VmHWM` 采样 + 段大小台账），给 t1 用。

**验收**：`MG_Test/Wire/SessionTest.cpp`——两条真线程把 20000 条记录跑通、
水位线的五条规则各一条用例、`kRecPad` 不计 seq 的用例、`Kill()`+join 的关闭用例
（抄 `InProcessTransportTest.cpp:344` 的 5 秒有界 join，免得回归变成 CI 挂死）。

---

### v1 — 服务端与线程（模型：**fable**；本阶段最高风险）

**交付**
- `MG_Remote/Server/ServerLoop.{h,cpp}`：`mgl-srv-apply` 线程（P5 不建 `mgl-srv-io`，
  inproc 下控制面就在同进程；P6 再拆），park 走 `Doorbell::Wait(consumerParked, …, kWaitForever)`，
  `Wait==false && Dead()` 即关闭。
- **EGL 所有权迁移**：服务端角色私有持有 `BackendObject_DirectGLES`；
  `eglMakeCurrent` 在 apply 线程上跑**一次**并终身持有（`DirectGLES.cpp:11925` 那一串 + `:11933-11953`
  的六个缓存失效变成启动一次性）。客户端的 EGL 生命周期调用（`InitializeEGLDisplay`、
  `CreateEGLWindowSurface`、`MakeEGLCurrent`、`SwapEGLBuffers`、`SetEGLSwapInterval`、
  `ReleaseEGLSurface`、`ReleaseEGLResources`）作为**阻塞控制请求**转给服务端，在 apply 线程上执行。
  `ReleaseEGLResources` / `~BackendObject_DirectGLES` **必须是阻塞的**，否则
  `MobileGL::Destroy()` 会在服务端还持着 context 的时候往下走（`scout-install:§5.3`）。
- `MG_Backend/Init.cpp:50` 的单一 hook（`#if MOBILEGL_BUILD_DISAGGREGATED` + `Transport != Monolith`）。
  注意同文件 `:43-44` 还要给 `gBackendFunctionsTable` 喂东西——那是 c1 的发射表。
- `Server/PipeApplier`：把 w1 的 decoder 接到 `MGPipeApply*`；服务端的**毒化 stamp**
  （§1.6 的前置条件：applier 今天故意不 stamp，split 下必须有人在 verb 边界 stamp，否则第一次读就 Fatal）。
  与 p1 协同：p1 定规则，v1 放调用点。
- **R-11 的落地**：`Ops_H_SubData`/`Ops_H_FlushRange` 在 split 下把字节拷进服务端自有存储，
  `GLESBufferResource::hostBytes` 不再指向客户端影子。
- teardown 顺序（`ARCHITECTURE.md:537` + 表 3 的 Kill/join 补充）。

**验收**：`ClearThenReadPixels` 在 `MOBILEGL_TRANSPORT=inproc` 下绿；
`0xDD` 审计臂下跑同一场景不红；关闭路径在 `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` 下无 UAF；
apply 线程的亲和（`MOBILEGL_IPC_SERVER_AFFINITY`，抄 `ShaderCompilePool` 的大核探测）落地并把
解析出的 mask 打日志。

---

### c1 — 客户端与发射表（opus）

**交付**
- `MG_Remote/Client/BackendObject_Remote.{h,cpp}`：`BackendObject` 的 8 个纯虚 + 9 个 EGL 虚函数。
  `GetRendererInfo()` 返回**引用**，所以远程对象要自己持有一份 `RendererInfo`；
  `GetFormatCapabilities()` 是非虚的，必须**填** `m_formatCapabilities` 而不是重写访问器。
  `GetBackendType()` 返回**服务端的**后端类型，不是新枚举值。
  `LogBackendInfo()` 在 caps 到达之前就会读 `GetRendererInfo()`（`Init.cpp:21`）——
  按 `scout-caps-reply:§1.2` 的 (a)：返回占位，接受启动时一行日志不准，**不重构 `MG_Backend::Init()`**。
- `MG_Remote/Client/EmitTables.{h,cpp}`：69 个槽，未实现 = `Fatal{UnmigratedVerb}`（R-4）。
- `MG_Remote/Client/CapsMirror.{h,cpp}`：caps 快照的客户端副本；R-12 的"再次到达即失效"。
- **verb 栅栏**（R-1）：发射后等 `appliedSeq >= emitSeq`，spin(`_SPIN_US`)→park。
  栅栏的那次等待同时取回复（R-3/R-5）。
- R-8 的存活门改读 caps 镜像。
- 阻塞 `read_pixels`：把像素从 `SEG_REPLY` 拷回应用指针；
  **PBO 目的地的那一半（fire-and-forget + 客户端 `MarkGpuWritten`）属于 b1**。

**验收**：`Triangle` 场景在 inproc 下绿；发射表的 Fatal 臂有一条死亡测试；
caps 镜像的单元测试覆盖 `glGetString`/`glGetIntegerv`/`glGetStringi` 三条读路径。

---

### b1 — buffer 侧（opus）

**交付**
- 客户端保守 GPU-write 集（`MG_Remote/Client/GpuWritePending`）：按表 2 的 6+2 行站点映射建集；
  `SyncGpuWrites` 的第三状态（"已发回读、答案未到"）→ 在栅栏下退化成阻塞等 `OnBufferWriteback`。
  **不做收窄**（`ResourceTracker.h:587-592` 的 `rangeCount == 1` 断言**保持**，收窄是 P8/P9）。
- persistent-map 块粒度推送：`m_livePersistentMaps` 集合（= `SyncPersistentMappedRange` 早退链读作成员测试）、
  64 KiB 块、键 `{MGPipeHandle, blockIndex}`、**走已有的 `resource_subdata` 记录**，不新增记录类型；
  Phase 1 = 保守（整个 mapped span 按块推）。
- `hasLiveHostWrites` 位上线：`MGPResourceDesc`/`MGPSubData` 的 pad 位（`:303`/`:1004`），
  并且必须登记进 `MGPipeResourceRespecifyNeedsAck` 旁边的**元数据字段**清单（P4a ID-18 M4），
  否则第一次推这个位就要一次假重分配。
- 抬起四个 pin：`PinNoLiveHostWrites`（`PipeApply.cpp:839-850` 的五个调用点）、
  `Managers.cpp:2669-2674` 的断言、`:2675-2681` 的 `frontend->IsMapped()` 最后一处前端读、
  `SanityTest.cpp:4583` 的单元门（它记录的正是"这条搞错了会 draw-clean forever 且无诊断"）。
- `persistent-map-push` 接线（`PipeStats.h:72`，短名 `pmap`；**同一个提交里必须改
  `PipeStats.cpp:68-70` 那段"P0 未接线"的清单文字**，否则清单在说谎）。
- R-6 的 T2 强制 + `MOBILEGL_IPC_ADOPT_TIER` 负面对照。
- flush 阶梯（`Managers.cpp:1047-1076`）：`hostBase` 的"每次使用重求值"在 split 下变成
  "发射时快照进 `SEG_STAGE`"；tier 1 的 `INVALIDATE_RANGE` 是"旧字节已死"的断言，
  晚到或加宽的快照会**静默**打烂 GPU 写过的数据（`Managers.cpp:1126-1129` 已经流过一次血）——
  这条要有单元用例。

**验收**：`PersistentCoherentMapScenario` 在 inproc 下绿且断言自己在模拟臂；
`pmap` 出数非零、`mpr` 与 monolith 臂相同；`MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` 下同场景**变红**（负面对照）；
`MG_Test` 的 buffer 族零回归。

---

### p1 — 毒化、字段归属与可见性（opus）

**交付**
- 表 2 的**生成器**：63 字段 + 7 sticky 的四类归属，产 `generated/PipeFieldOwnership.inc` + `--check`
  （抄 `gen_pipe_dirty_surface.py` 的 check/self-test 形状），**不在任何一类里 = 构建失败**。
- `PipeStats::CallClass::ResidualPulls`（短名 `rsp`，放进 `#if MOBILEGL_PIPE_PUSH` 块内）。
- split 下的 stamp 规则（与 v1 协同）：谁在服务端 verb 边界 stamp、stamp 什么。
- `PipeFilled.inc:421` 的 sticky 无条件 fresh 在 split 构建下取消；七个 forward 走 R-7.3。
- `MOBILEGL_IPC_STRICT_ERRORS=1` 车道 + 一条具名的 abort 断言测试。
- `MGPipeUnmigratedEmulation` 的 split Fatal 臂（`PipeApply.cpp:2820` 一个函数，五个站点）。
- `GetPixelStoreParameters` 拆成 pack / unpack 两个字段 id（表 2 的小迁移）。

**验收**：`--check`/`--self-test` 绿且负面对照（把一个字段从表里删掉）按名变红；
strict 车道在缩减路径上按预期 abort 并点名；默认车道 `rsp` 逐帧出数。

---

### t1 — 测试与 CI 布线（opus）

**交付**
- `Scenarios/TriangleScenario.cpp`、`Scenarios/PersistentCoherentMapScenario.cpp`
  （+ `MG_IntegrationTest/CMakeLists.txt` 源列表两行）。
- `DirectGLES.Split.` 车道：**一个场景一个 `gtest_discover_tests` 块**
  （抄 `DirectGLES.MapPersistentRoundtrips.` 的 `:1377-1394`），
  `mgl_itest_join_environment` 拼环境并**必须**追加 `${MGL_ITEST_COMMON_ENV}`，
  标签写 `"integration-gpu\;integration-split"`（私有标签让
  `ctest -L integration-split --no-tests=error` 在忘了开构建选项时**变红**而不是绿跑零个）。
- trace-replay 的 `SPLIT`：走 `run_trace_case.cmake` 的**环境**读法
  （`set(ENV{MOBILEGL_TRANSPORT} ...)`，抄它已有的 `$ENV{MOBILEGL_PIPE_VERIFY}` 处理，`:153-190`），
  `add_trace_replay_test` 加变体后缀参数（`add_test` 重名是硬错误），
  `set_tests_properties` 里带 `MOBILEGL_IPC_SERVER_PATH`。
  **`TRACE_OUTPUT_DIR`/`TRACE_ARTIFACT_DIR` 必须也带变体**，否则两条臂写同一个
  `output/mobilegl.log`，而那个文件是**唯一有效的拒绝普查来源**（`ctest -V` 的普查是假零）。
- `build-split` 构建目录 + `~/w7/notes/tools/wsl_p5_gate.sh`（五部分，§6）。
- CI：`build-linux-split`（克隆 `build-linux-verify`，带 `nm` 证明库里真有 `MG_Remote` 符号——
  这一步的缺席正是"split 车道跑着 monolith 还全绿"的成因）、`integration-split`、`retrace-split`
  （克隆 `retrace-verify`，把 `MOBILEGL_PIPE_VERIFY=1` 换成 `MOBILEGL_TRANSPORT=inproc`）。
  **不挂临时分支触发**（R-14）。
- 双角色峰值 RSS 的采集（`/proc/<pid>/status:VmHWM` 采样 + s1 的段台账），记录不设门。
- 读日志的用例（`pmap` 出数那条）要**自己一条车道 + 自己的 `MOBILEGL_LOG_FILE_PATH` + `RESOURCE_LOCK`**
  （不加锁实测 3 次全标签 `-j 8` 会出 4/0/2 次伪失败）。
- `~/w7/p5-before-ctest-names.txt` 与 `~/w7/p5-before-libMobileGL.so` 快照（G14/G1 的基线）。

**验收**：`ctest -L integration-split` 在 `build-split` 里**匹配到东西**且绿；
在忘开选项的构建里**变红**；`retrace_gate.py` 能跑 OpenRA 的 inproc 臂并出 SSIM。
（`retrace_gate.py` 读的是 `~/mgl/MobileGL/build-retrace` 的**外来** `CTestTestfile.cmake`，
名字正则不认 `SPLIT` 后缀——**零代码改动的路子**是用现有名字 + 进程环境导出 `MOBILEGL_TRANSPORT=inproc`，
与 `wsl_p4a_gate.sh:69` 对 `MOBILEGL_PIPE_VERIFY=1` 的做法逐字相同。先走这条。）
---

## §6 门（五部分，沿用 P4a 的结构 + 第五个 split 臂）

`~/w7/notes/tools/wsl_p5_gate.sh`，四个构建目录：
`build-linux`（pull）、`build-push`、`build-verify`、**`build-split`**
（`$COMMON -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON`）。
全部 fail-fast，`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`。

**第 1 部分 — 接口纯度**（继承 P4a 全部）加：
- `nm --defined-only build-linux/libMobileGL.so | grep -i MG_Remote` **为空**（G1 的 split 版）；
- `nm --defined-only build-split/libMobileGL.so | grep -i MG_Remote` **非空**（否则 split 车道在跑 monolith）；
- **G1 本体不变**：pull 符号 0 增 / 0 删 / 0 重命名 / 0 resize，`.text` 字节不变。
  P5 碰了 `PipeInputs.h`、`BufferObject.cpp`、`Managers.cpp`、`PipeStats.*`——这些都在 pull 构建里，
  **G1 是本阶段最容易破的门**，每个包自测时就要跑。

**第 5 部分 — 覆盖与生成器**加：`gen_pipe.py --check/--self-test`、
`gen_pipe_dirty_surface.py --check/--self-test`、**新的字段归属 `--check/--self-test`**（p1）。

**第 3 部分 — 行为 A/B**加：
- `ctest -L unit` 在 `build-split` 里——**这是五个 `MG_Test/Wire` 套件的第一次真实运行**，加上 w1/s1 的新套件；
- `ctest -L integration-gpu` 在 `build-split` 下分别以 `MOBILEGL_TRANSPORT=monolith` 与 `=inproc` 跑，
  **逐名相同**（`ARCHITECTURE.md:521` 原话要求的就是这个三臂形状）；
- `ctest -L integration-split` 必须匹配到东西且绿；
- **G2 不变**（pull vs push 逐名相同）、**G14 不变**（0 删除，新增记数）。

**第 2 部分 — 语义对照**加：
- `MOBILEGL_TRANSPORT=inproc python3 ~/w7/retrace_gate.py --tree ~/w7/pipe
   --lib ~/w7/pipe/build-split/libMobileGL.so --out ~/w7/retrace-out/p5-split -j 4 --only 'OpenRA'`
  → SSIM ≥ 0.99；
- 79 例 retrace 的 push / verify 两臂**仍然 79/79**（P5 不许把 monolith 弄坏）。

**第 4 部分 — 设备**（`wsl_p5_bench.sh`）：只用红米 `2f7cbe2e`，其余 adb 设备一律不碰。
风扇按已记录的档位（只按 `real_speed` 判断状态，101–104 会锁进调试模式）。
P5 的设备任务是**记录**，不是门：inproc 臂的帧时与 monolith 臂配对、双角色峰值 RSS、
`rsp`/`pmap`/`mpr` 三个计数器逐用例。

---

## §7 退出门（把 ROADMAP:21 那一行改写成可以变红的形式）

ROADMAP 原文六项里有两项是**记录**不是门（"两个角色峰值 RSS 在案"、"`persistent-map-push` 出数"），
按 `ROADMAP.md:57` 对性能的处理方式标注清楚。真正的门，逐条带负面对照：

**E1** `DirectGLES.Split.ClearThenReadPixelsScenario.*` 与 `DirectGLES.Split.TriangleScenario.*`
在 `inproc` 下绿。负面对照：`MOBILEGL_IPC_VERB_BARRIER=0` 下**必须红**，且红的方式被记录一次
（证明栅栏是承重的，不是摆设）。

**E2** OpenRA trace 在 `inproc` 下 SSIM ≥ 0.99。负面对照：把 `Clear` 的发射器改成丢弃一次
（临时补丁）必须让 SSIM 掉下阈值——证明这条臂真的走了 split 路径。

**E3** `PersistentCoherentMapScenario` 绿，**且**（premortem §4 的五条加强版）：
(a) `MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` 下同场景红，红过一次并记录；
(b) 场景里有一次**没有任何 GL 调用宣告**的写（map、写、draw、再写、再 draw、回读），
    这样"每个 validate 点推送"是必需的而不是被某次 `glBufferSubData` 顺带满足；
(c) split 臂断言 `pmap > 0` **且** `mpr` 与 monolith 臂相等；
(d) 断言 split 下 `MGPipeApplyMapPersistent` **从不返回非空指针**——这是抓 inproc 地址泄漏的那条，
    也是让这个门在 inproc 与 spawn 下含义相同的唯一一行；
(e) 以上五条再跑一遍 ring 小到足以至少发生一次背压等待的配置。

**E4** 未迁移输入的门（加强版，替掉原来那一句）：
> split 下，63 个 `PipeInputs` 字段 + 7 个 sticky forward 每一个都在
> `RECORD-SUPPLIED | APPLIER-DERIVED | BARRIER-PULLED | FATAL` 四类之一，表是生成的且 `--check` 在 CI 里；
> 不在任何一类 = 构建失败。`FATAL` 类的读确实 abort 并点名（现有毒化 + 五个 emulation 站点的新 split 臂）。
> `BARRIER-PULLED` 类的读被 `rsp` 计数并逐帧发布；`MOBILEGL_IPC_STRICT_ERRORS=1` 把它提升为 FATAL，
> 一条具名测试断言 abort 发生。sticky 的毒化豁免在 split + strict 下取消。
> 负面对照：把一个字段从表里挪走（supplied → FATAL）必须让一条具名测试变红。

**E5** **inproc 诚实性五条**（R-2）各自被观察到红过一次并记录。
这是把 premortem 裁定 A（"inproc 什么都证明不了"）变成可检验命题的唯一办法。

**E6** 全门第 1/2/3/5 部分绿；G1 0/0/0/0；G2 逐名相同；G14 0 删除；
`integration-verify` 零 `Fatal{`；79 例 retrace 的 push/verify 两臂不退。

---

## §8 记录（不设门，进 MEASUREMENTS）

1. `rsp` 逐帧/逐用例——**P6/P7/P8 欠债的大小**，P5 结束时的数字是后续阶段的基线。
2. `pmap` 与 `mpr` 的对照（两者反相关：T2 下每次 `MapPersistent` 都 decline，`mpr` 不变、`pmap` 从 0 起来）。
3. 最大记录字节（R-10 的证明义务）。
4. 双角色峰值 RSS + 段台账（`SEG_CMD` 8 MiB / `SEG_STAGE` 32 MiB / `SEG_REPLY` 16 MiB（ID-47 起；原 8 MiB） / `SEG_EVENT` 256 KiB
   是 `ProtocolSmokeTest.cpp:72` 钉住的规范尺寸）。**要 N 帧上的斜率，不只是一个峰值**——
   present 1:1 的批处理/credit bug 表现为慢泄漏，SSIM 看不见，两个场景的车道也够不着。
5. 栅栏成本：inproc vs monolith 的配对帧时（红米，定频，尾 200 帧 p50/p99）。
   **预期变慢，这是 R-1 的已知税**，写清它随栅栏退役而消失。
6. 桌面 `DriverBench`：P4a 已经证明它分辨不出单阶段增量（T1−T2 在两次运行里 133.9 / 11.4 ns
   而每臂自己漂 ~200 ns），**P5 不单独引用它**。

---

## §9 风险登记（按"会不会让阶段停摆"排序）

| # | 风险 | 证据 | 缓解 |
|---|---|---|---|
| 1 | **EGL context 迁移到 apply 线程跑不通** | 16+19 个 off-thread 降级站点；`ReleaseEGLResources`/析构在 app 线程 | R-1 的退路（`inproc-inline` 中间态）+ v1 交给 fable + 第 4 个工作日的硬检查点 |
| 2 | **`Transport/` 从未被编译过** | `scout-test-ci:§5.2`，全 CI 零 `=ON` 配置 | c0 第一天就把四个构建目录配起来并编译，编译错误由 c0 就地修（这属于契约包，不是 s1 的账） |
| 3 | **G1 被打破**（pull 符号动了） | P5 要改 `PipeInputs.h`/`BufferObject.cpp`/`Managers.cpp`/`PipeStats.*` | 每个包自测跑 G1；新计数器一律放进 `#if MOBILEGL_PIPE_PUSH` 块内（`PersistentMapPush` 例外——它已经在两个构建里，安全） |
| 4 | **acceptance 被某个包自行"客户端重推导"** | P4a 的 c0f/c0g 就是这个形状，代价两轮契约修 | R-5 写死；复审第一条检查项 |
| 5 | **inproc 偷偷用宿主指针** | `MGPipeHostBytes` 优先 `Ptr` 分支；五个发射器今天都写 `Size=0` | R-2 的五条 + `0xDD` 审计 + E5 |
| 6 | **一条记录超过 `Capacity()/2`** | `CreateShaderState` 的归档 | R-10（blob 走 SEG_STAGE，记录恒小）+ 最大字节计数器兜底 |
| 7 | **`SPLIT` 臂的日志/目录撞车导致假零普查** | `TRACE_OUTPUT_DIR` 不带变体；ctest `ENVIRONMENT` 是替换不是追加 | t1 的目录变体 + `mgl_itest_join_environment` + 每车道自己的日志 + `RESOURCE_LOCK` |
| 8 | **teardown 挂死或 UAF** | 只有 `Doorbell::Kill()` 能叫醒 `kWaitForever`；ID-8 的退出顺序规矩 | 表 3 第四列 + 有界 join 的用例 + tcache_count=0 |
| 9 | **Magma 车道空转** | DirectVulkan 不注册 `MGPipeResourceOps`（P4a 的消费者门） | P5 的 split 车道**只跑 DirectGLES**；DirectVulkan 的 split 是 P7。写进 brief，免得有人去修一个不该在这一阶段修的东西 |
| 10 | **OpenRA 需要的 verb 比预期多** | R-4 的普查还没跑 | 普查在第 1 天出结果；若 >15 个 verb，集成者裁剪 E2 的范围并在 ROADMAP 上记账 |

---

## §10 流程（沿用，写明差异）

- 契约包 c0 先落地并推送；七个并行包从 c0 之后分叉，各自 worktree。
- 每包一轮对抗性复审。**同一个包返工超过两轮就交给 fable**（研究根因/同类缺陷，或直接做那一轮）。
- 集成按依赖序：c0 → w1 → s1 → v1 → c1 → b1 → p1 → t1；冲突按文件所有权表判，
  共享测试文件取该包的最终版本再重放另一边的改动（P4a 的教训：盲目 theirs-first union 会重复整块）。
- 验证轮 → 终审 → 全门 → APK → 设备记录 → docs（`README`/`ROADMAP`/`ARCHITECTURE`/`MEASUREMENTS` 四件）→ push。
- 决策全部写 `~/w7/notes/p5/INTEGRATOR-DECISIONS.md`（ID-1…），每条带证据与推翻它需要什么。
- 提交信息：`[Type] (Scope): description` 单行，**不带任何署名行**。
- 顺带修正（各包在自己的文件里做掉，不单独开包）：
  `ARCHITECTURE.md:83` 的 "61 个" → 63；`ARCHITECTURE.md:492` 引的 `PipeStats.h:126` → `:141`；
  ROADMAP P4a 行的 emulation 站点数 6 → 5 次调用 + 1 处注释；
  `ARCHITECTURE.md:578/581` 的"四 → 二"普查按表 3 补到至少七项。

---

## §11 给复审者的检查清单（第一轮就照这个打）

1. 有没有任何一处在 split 下仍然使用宿主指针 / `Blob.Size == 0` / `Seg == 0` 携带内容？（R-2）
2. 有没有第二份 acceptance 谓词、第二份 D-K2 依赖表、第二份程序归档编解码器？（R-5、表 1）
3. `appliedSeq` 有没有在栅栏期被批处理？`kRecPad` 有没有被一侧计进 seq？（R-9）
4. 有没有 applier 入口把指针留过返回？`hostBytes` 改干净了吗？（R-11）
5. 客户端还有没有在读 `MGPipeGetResourceOps()` 来决定发不发？（R-8）
6. 发射表里有没有 null 槽，或者穿透去调驱动的槽？（R-4）
7. 表 2 里有没有字段落在四类之外？sticky 的豁免在 split 下取消了吗？（R-7）
8. 新的 ctest `ENVIRONMENT` 有没有漏掉 `${MGL_ITEST_COMMON_ENV}`？`;` 转义了吗？
   `SPLIT` 臂的输出目录带变体了吗？（t1）
9. 新计数器有没有掉到 `#if MOBILEGL_PIPE_PUSH` 块外面去（G1）？
10. 有没有任何 P5 的 CI 步骤挂在 `feat/disaggregated` 这个临时触发上？（R-14）
11. **（R-16，第一波实测出来的本阶段头号缺陷类）测试有没有自己构造它本该观察的那个状态？**
    三个包在同一轮里各犯了一次：p1 的自测 harness 抓任何 `SystemExit`，于是 4 号对照
    因为转义写坏而跳去触发 1 号对照的消息，"11 条全红"实为 10；b1 的 persistent-map 用例
    手写记录字段，于是把生产者删掉它照样绿；t1 只看符号的探针对着契约桩武装，
    实测 11 条 `Split.` 条目全绿而跑的是 monolith。
    **规则：每一条门／对照都必须由生产路径驱动，并且必须实际观察到它因自己的理由变红一次。**
    做法：把它要抓的东西真的破坏掉，看它红，再恢复；对照必须断言**自己的**失败串，
    不能只断言"失败了"。报告里要写下这次观察。

---

## §13 R-16 — 门的自证义务（2026-09-11 第一波之后追加）

上面第 11 条的正式形式，对第二波（v1/c1）与终审同样生效：

- **一条断言不得构造它要观察的状态。** 手写记录字段、手动置位、直接调内部函数去"准备"被测状态，
  都会把"生产者不存在"这个失败模式变成不可见。
- **一条负面对照必须断言它自己的失败原因**（问题串／测试名／Fatal 的措辞），不能只断言进程失败了。
- **一条探针不得对着桩武装。** 符号存在 ≠ 实现存在；与"不再残留 `Fatal{Unimplemented`"之类的
  实现性条件取合取。
- **每条门在报告里都要带一行"我把它弄红过一次，方式是 X"。** 没有这一行的门按未验证处理。
- 已知的反例清单（三条，见第 11 条）进 `MEASUREMENTS` 的缝分类学，作为 P4a 九类之外的第十类：
  **「一条不会因自己的理由变红的门」的三种具体长相**。

---

## §12 契约包落地后的更正（2026-09-11，c0 `41b4f8df..b4bbcc11`）

**冲突时 `MobileGL/MG_Remote/CONTRACT-P5.md` 优先于本 brief。** 完整清单见
`~/w7/notes/p5/p5-results/c0-v1.md` §6（14 条）与 `~/w7/notes/p5/verb-census.md`。
下面只列会改变某个包做什么的那些。

**C-1（最重要）`ResourceRespecify` 有第二个没有载体的伴随物，本 brief 漏了。**
`const MGPRespecifiedLevel* level`（`PipeApply.h:792-795`）是重定义的**作用域**，
`MGPResourceDesc` 表达不了它。没有载体的话，OpenRA 里每一次逐 level 的 `glTexImage2D`
都会静默走整资源臂并丢掉全部 pending 上传。c0 已裁定用现有 pad 位（`Pad1` → 两个 `Uint16`，
`Pad0` 的一个字节 → `HasRespecifiedLevel`；尺寸不变、POD 断言不动）并在后续轮里落地。
**w1 编码这条记录之前必须先有它。**

**C-2 发射表是 71 个指针，不是 69。** `Present` 与 `SetSwapInterval` 在 `MG_Impl` 里
**零调用点**，只从 `BackendObject.cpp:396/:403` 经 EGL 路径到达——照着 GLImpl 的调用点镜像出来的
表两个都盖不到，而 `Present` 在三个目标里的两个上都在缩减路径。它们与 v1 正在改道的 EGL 那条缝是同一件事。

**C-3 verb 普查的答案是 6 个槽，不是 6–15**（OpenRA 是实测不是估算，fixture 本来就 hydrate 着，
没有任何 LFS 拉取）：`Clear`、`DrawArrays`、`ReadPixels`、`BlitFramebuffer`、`GetIntegeri_v`、`Present`。
其余 65 个槽 `Fatal{UnmigratedVerb}`。三个陷阱：
- **`Flush` 根本不是一个槽**，而且 `glFlush`/`glFinish` 是空函数体
  （`Definitions.cpp:111-112`）——R-4 预测集里的 `Flush` 作废；`TriangleScenario` 若想用 `glFlush`
  给回读排序，它排的是空气。
- **`GetIntegeri_v` 由一个上下文的第一次 `glCompileShader` 到达**（`CompileEnv.cpp:134-138`），
  不由任何 verb 到达；全 Fatal 的表会在**每个**场景的第一次着色器编译上 abort。
- OpenRA 是三个目标里**最薄**的那个，不是最宽的：它加宽的是站点集与记录集，verb 集一点没加宽。

**R-15（新裁定，因 C-3 而生）getter 形状的 `GLFunctionsTable` 槽由 caps 镜像在客户端就地回答，
不发射记录。** `GetIntegeri_v` 属于这一类（它携带的六个 compute 上限本来就在 `MGPCaps::Dynamic` 里），
现成的门是 `AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer`。
71 个槽分成「就地回答 / 发射 / `UnmigratedVerbFatal`」三类，清单在 `CONTRACT-P5.md`，c1 不要自己重推。

**C-4 R-8 按原文不可实现**：`CallMask` 只有九个 `MGPCapBit` 特性位，没有逐族位。
c0 裁定 bits 32..47 = `(serverConsumedSubsystems & 0xFFFF) << 32`，常量与折叠/测试 helper 在
`MG_Remote/CapsCodec.h`。`protocol.fbs` 的 `tableSlotMask` **删除**（`GLFunctionsTable` 有 69 个槽，
`ulong` 是 64 位，这个字段从来就寻址不了它自称的那张表），`maxComputeWorkGroupCount/Size` 与
`prefersCpuXfbPrimitiveAccounting` 一并删。

**C-5 表 1 是 23 行不是 19**，而且 brief 的 19 与 `scout-premortem:§3` 的 19 **是两个不同的清单**。
多出来的四行是服务端→客户端方向的（`MapPersistent`、`ResourceReadback`、`ReadPixels`、`GetTextureImage`）。
`ReadPixels` 的目的地裁定为**在 P5 里永远是 `SEG_REPLY`**，所以 `MGPReadbackInfo` 不加 `Seg` 字段。
另外 `SetResidualValueState` 是**第四个**有类型的伴随物（applier 收 `const ResidualValueBlock&`，
而 `MGPResidualValueState` 在活路径上从未被实例化过）——按 w1 最硬的一行排期，不是简单八行之一。

**C-6 `ResourceFlushRange` 不携带任何字节**（R-13.2 的第三个答案）：它驱动的阶梯是
「从权威影子重写自己的 range」，而 R-11 之下权威影子归服务端，所以 `resource_subdata` 已经是字节
到达那里的唯一途径；再给一个 blobref 等于给同一件事第二种可伪造的说法。
栅栏为 buffer 族退役时重新审视。

**C-7 那 18 个 abort 与毒化无关，本 brief 的诊断是错的。** 它们是测试**故意触发并期待**的
`Fatal{ProtocolCorruption}` 绊线；七个 `MG_Test/Pipe` TU 在 `#if` 里测 `MOBILEGL_PIPE_POISON`
却没 include 唯一定义它的头，于是编了不 abort 的那条臂，而 `PipeApply.cpp` 编了 abort 的那条。
`MOBILEGL_BUILD_DISAGGREGATED` 是唯一住在那个头后面的 arming 条件，所以 split 是第一个能暴露它的构建。
c0 已修（每文件一个 include + 一处潜伏的 guard 更正），**p1 的机制是清白的**。

**C-8 杂项数字更正**（照 c0-v1 §6 的 9/10/11/12/13/14 条）：
`CanTouchGLNow()` 守 16 个站点不是 19；`ScopedDefaultUnpackState` 有六个值影子不是五个；
`DirectGLES.cpp:12446` 不是 `ClampSamplesToBackendSupport`（真正的是
`BackendObject_DirectGLES.cpp:807-828`，后端内部读 `pActiveBackendObject` 是**七处跨六个函数**，
不是一处——表 3 的结论不变，diff 变）；`MGPipeApply*` 是 37 个入口 / 41 个调用点不是 ~45；
persistent-map 普查是 **21** 个站点不是 20，且 `ARCHITECTURE.md:290` 引的那张 §5.7 归属表
**在树里不存在**（b1 别去找）；`MGHostSpan` 不是 `MGPHostSpan`。

**C-9 所有包第一天都会撞到的一件事**：worktree 的 submodule 是指向 `~/w7/pipe` 的符号链接
（`git worktree add` 不带 submodule 内容，递归 init 要联网）。
**git 在它们是符号链接时会拒绝运行**，用 `~/w7/p5-c0-submodlinks.sh off|on` 切换：
跑任何 git 命令之前 `off`，跑构建之前 `on`。

**C-10 脚本自检的教训**：一个早期 bring-up 脚本报了四个"绿"的构建而它们根本没发生
（丢了 `$COMMON`、构建循环没传目录，`cmake --build -j 24` 只打印了用法而 `$?`=0）。
**任何 P5 脚本报构建结果之前，先确认 `CTestTestfile.cmake` 存在。**

**文件所有权补充**：`MG_Pipe/MGPipeTypes.h`（payload 结构形状）归 **c0**。
