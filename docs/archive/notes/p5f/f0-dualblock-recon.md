# P5f / f0 — f1 双块机制设计侦察（只读普查）

> 只读普查产物，未修改任何代码、未构建、未跑测试。所有 `file:line` 以
> `feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`）为准。
> 姊妹篇：`f0-env.md`（旋钮/车道环境一览）、`f0-statics.md`、`f0-magma.md` 等。
> 本文聚焦 `gPipeInputs` 的双块拆分（P5F-WIRE-COMPLETENESS.md §4）的设计侦察，
> 回答任务单 (1)–(7)。

---

## 1 `gPipeInputs` 的声明与全树引用点分类

### 1.1 声明

`MobileGL/MG_Backend/MGPipe/PipeInputs.h:835`：

```cpp
inline PipeInputs& gPipeInputs = *new PipeInputs();
```

- inline 引用变量，无 .cpp 定义；**leak-at-exit**（`PipeInputs.h:818-834`）：O 类成员是指向前端
  对象的 `SharedPtr`，退出时析构会沿 slot allocator / applier / twin 表触发不安全的销毁链，
  所以永不释放。
- 尺寸预算 `static_assert(sizeof(PipeInputs) < 20 * 1024)`（`PipeInputs.h:843`）。
- 该头只在 `MOBILEGL_PIPE_PUSH` 构建里经由 `MG_Pipe/PipeInputsSwitch.h:17-21` 进入 include
  闭包（`PipeInputs.h:39-43` 的论证）；`PipeInputs.cpp` 也只在此条件下编译
  （`PipeInputs.cpp:11-13`）。**pull 构建从符号层面就看不见它**，双块改动若同样关在
  `MOBILEGL_BUILD_DISAGGREGATED` 里则对 G1 零扰动。

### 1.2 引用点全清单（非测试代码），按角色/线程分类

**写者 — client 角色（GL 线程，`MG_Impl`）**：

| 位置 | 写什么 |
|---|---|
| `MG_Impl/Pipe/PipeFill.cpp:3348`（`MGPipeValidateForVerb`） | 绑定 `PipeInputs& inputs = gPipeInputs;`；`:3385` SetIdentity（**每个 verb 都写**，含 run-ahead）；`:3423` `++CurrentVerbSerial`；`:3437` `MGPipeServerClearVerbBoundary()`；`:3439` SetVerb；`:3746-3763` step 4 残余填充 `CopyField` + `FilledGen[i] = CurrentVerbSerial`（sticky 一次性盖 1，`:3753`） |
| `PipeFill.cpp:2204-2230`（`MGPipeLeaveVerb`） | `:2225` 再 bump serial（无 verb 时使旧戳过期）；`:2228` clear；`:2230` SetVerb(kVerbCount) |
| `PipeFill.cpp:702-724`（`MGPipeNoteFrontendMutation`） | 单字段刷新（P3a D-H2 类：后端在自己 verb 内改了前端对象后的回填） |
| `PipeFill.cpp:1217`（`MGPipeReleaseResidualFillPins`） | 释放四个 O 类 SharedPtr |
| `PipeFill.cpp:565-566, 627, 653, 663, 679`（verify 臂） | `g_snapshot` / `g_readScratch` 与 `gPipeInputs` 的入口比较、compare-at-read hook（`MOBILEGL_PIPE_VERIFY` 构建限定） |

**写者 — server 角色（apply 线程 `mgl-srv-apply`）**：

| 位置 | 写什么 |
|---|---|
| `MG_Backend/MGPipe/PipeInputs.cpp:265-305`（`MGPipeServerStampVerbBoundary`） | bump serial、SetVerb、按 `AnswerableMaskForVerb` 盖 RECORD_SUPPLIED/APPLIER_DERIVED、**清零** BARRIER_PULLED/FATAL 的戳（`:300-303`）、置 `m_serverStampedVerb`（`:304`）。调用点：`MG_Remote/Server/PipeApplier.cpp:1255-1258`（仅 Clear/DrawVbo/ReadPixels/Blit 四个 verb 形 op） |
| `PipeInputs.cpp:307`（`MGPipeServerClearVerbBoundary`） | 清 `m_serverStampedVerb`。**双角色都调它**：server 侧 `PipeApplier::LeaveApplier`（`PipeApplier.cpp:1261`），client 侧 `PipeFill.cpp:3437` 与 `:2228` |
| `MG_Pipe/PipeApply.cpp`（经 `MGPipeApplyAccess` 门，`PipeInputs.h:732-738`） | `:1524-1534` BindRenderState、`:1552-1557` SetDynamicState、`:1561` SetPixelPackState、`:1573` SetContextValues、`:1584-1638` SetPatchState、`:1643-1690` SetVertexAttribDefaults；`:1724` 是读（trip wire 比较）。**故意不盖戳**（`PipeInputs.h:735-737`） |

**读者 — server 角色（apply 线程）**：

- **后端主通道**：`MG_Pipe/PipeInputsSwitch.h:19-21` 的 `MGB_CTX` 宏 = `&gPipeInputs`。
  全树 `MGB_CTX` 共 **379 处 / 24 文件**；生产后端：DirectGLES.cpp 103、Managers.cpp 24、
  MultiDraw.cpp 7、Utils.cpp 2、DirectVulkan.cpp 48、VulkanRenderer.cpp 133、
  UniformManager.cpp 23 等。inproc 下这些读全部发生在 `PipeApplier::ApplyOne`
  （`PipeApplier.cpp:1159-1243`）内部，即 apply 线程；monolith 下在 GL 线程。
- 直读（不经宏）：`MG_Backend/DirectGLES/DirectGLES.cpp:267`（`gPipeInputs.CurrentVerb()`）；
  `PipeApplier.cpp:402`（GetPixelStoreParameters）、`:505`（GetMaxTouchedTextureUnit）、
  `:858-859`（GetTextureObject）。
- 七个 F 类 sticky forward 的函数体在 `PipeFill.cpp:2131-2186`，**逻辑上被 server 调用、
  物理上在 apply 线程执行**（经后端调用进入），体内 `MGP_STICKY_FORWARD_PULL` 调
  `MGPipeStickyForwardPull`，后者读 `gPipeInputs.ServerStampedVerb()/CurrentVerb()`
  （`PipeInputs.cpp:376-377`）。

**读者 — client 角色（GL 线程）**：

- `PipeFill.cpp:3016`（`VertexAttribDefaultsOf`，emitter 自查）、`:629`（Fatal 消息取名）；
  verify hook 的 `&self == &gPipeInputs` 判等（`:663`）。
- `EmitTables.cpp:1200` 的注释明确反面规则：client 侧**不得**经 MGB_CTX 问问题
  （"on this side of a split MGB_CTX is gPipeInputs, which is the SERVER's view"）。

**测试引用**（非生产，但双块要决定它们落在哪块上）：`MG_Test/Pipe/PipeInputsTest.cpp`、
`FieldOwnershipTest.cpp`（约 40 处）、`RenderStateSpansTest.cpp`、`MG_Test/Wire/
ServerLoopTest.cpp:572-586`、`PipeWireCodecTest.cpp:1366-1380`、`RemoteClientTest.cpp:
2053-2364`（角色守卫死亡测试）。测试均跑 monolith/单线程形态，knob 关闭时行为必须逐字节不变。

**纯注释引用**（无代码语义）：`PipeMutation.h:223,248`、`PipeApply.h:21,802,1086`、
`ClientSession.h:25,150,191-199`、`WireTables.h:29`、`CapsMirror.cpp:37`、
`Init.cpp:80`、`PipeStats.h:172`、`SlotAllocator.cpp:428`、`EmitTables.cpp:256,812,1394`、
`PipeWireCodec.cpp:2257`、`Config.h:523`、`ConfigLoader.cpp:386`。

**今天为何能工作**：两角色同进程同对象，verb barrier 保证 {GL 线程, apply 线程} 同一时刻至多
一个可运行（`PipeInputs.h:904-908` 引 CONTRACT-P5 表 3 的裁定），所以单写者成立；client 填、
server 读，BARRIER_PULLED 就是"读对方留在同一块里的残余"。

**P5f 处置建议**：写侧/读侧的代码边界恰好是文件边界 —— **所有 client 写集中在
`MG_Impl/Pipe/PipeFill.cpp`，所有 server 写在 `MG_Backend/MGPipe/PipeInputs.cpp` +
`MG_Pipe/PipeApply.cpp`，所有后端读经 `MGB_CTX` 一个宏**。这意味着双块不需要碰 379 个后端
读点，只需要：(a) 在 `PipeInputs.h` 加第二块与选择函数；(b) 把 PipeFill.cpp 里约 10 处
`gPipeInputs` 拼写换成 client 块；(c) server 侧保持 `gPipeInputs` 拼写不变。详见 §7。

---

## 2 运行时如何知道当前角色

### 2.1 server 侧：线程身份

- `ServerLoop::OnApplyThread()`（`MG_Remote/Server/ServerLoop.h:246-252`）：一次 relaxed
  读 `Detail::g_applyThreadKey` + 与当前线程 key 比较；key 由 apply 线程自己在
  `ApplyThreadMain` 第一行发布（`ServerLoop.cpp:337`），在与 `m_running` 清空的**同一个临界
  区**内清零（`ServerLoop.cpp:519`，理由见 `:496-518` 的长注释——身份必须与 m_running 同死）。
  key==0 时短路返回 false，所以 monolith/未启动/已停止都答"否"（`ServerLoop.h:248-251`）。
- 别名 `RunsAsTheServerRole() = OnApplyThread()`（`MG_Remote/Client/WireTables.cpp:87`），
  WireTables 每条发射路径用它分流 monolith/split。
- 它是**全树每个角色守卫的谓词**（`ServerLoop.h:89-92` 列举：BufferObject 的 legacy-arm
  守卫、PersistentMapTracker::OnServerRole、RefusePipeInputsTouchWhileApplierOwnsIt、
  分配器守卫等）。

### 2.2 client 侧：会话标志 + 线程局部

- `thread_local Bool g_inBarrierWait`（`MG_Remote/Client/ClientSession.cpp:117`，由
  `BarrierWaitScope` :120-122 举起）——"本线程是否在等 barrier"，**必须** thread_local
  （`ClientSession.cpp:1278-1284` 的论证）。
- `std::atomic<Bool> g_applyThreadInsideApplier`（`:118`），由 applier 侧的
  `ScopedApplierEntry` 在整个 `ApplyOne` 期间举起（`PipeApplier.cpp:1173`；
  Note* 函数在 `ClientSession.cpp:1381-1386`）。
- `ClientSessionInstance().RunAheadArmed()` / `BarrierArmed()` / `Started()`：
  会话级标志，守卫链按"常量优先"顺序短路（`ClientSession.cpp:1292-1305`）。

### 2.3 P5c 角色守卫本体

`ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt`（`ClientSession.cpp:1290-1380`），
被 PipeFill 的四个写点调用：`:723`（NoteFrontendMutation）、`:2214`（LeaveVerb）、
`:3412`（ValidateForVerb phase 1）、`:3737`（ValidateForVerb/residual phase 2）。
run-ahead 臂（`:1318-1354`）：除"barriered fill 且 applier 不在 applier 内"外，GL 线程触碰
即 `Fatal{RoleViolation, "gPipeInputs"}`；lockstep + BatchWaits==0 臂（`:1361-1379`）同理。

**读侧线程**：server 读全部在 apply 线程（`mgl-srv-apply`，`ServerLoop.cpp:338` 命名），
具体在 `PipeApplier::ApplyOne` 的 stamp→decode→LeaveApplier 区间内。
**填侧线程**：GL 线程（app 调 GL 入口 → `MGPipeValidateForVerb`）。

**今天为何能工作**：角色判定不靠"你在哪个进程"，而靠"你在哪条线程 + 会话是否 started/armed"。
双块机制可以复用同一套谓词，但更稳的轴是**代码位置**（§1.2：填侧全在 PipeFill.cpp）而不是
线程——线程谓词在 monolith 下会把 client 和 backend 判成同一角色，恰好正确；在 inproc 下
分开，也恰好正确。两条轴等价，代码位置轴更便宜（无热路径谓词）。

**P5f 处置建议**：块选择函数内部**不需要**调 `OnApplyThread()`（它在 P5d 被压成一次 relaxed
load 但仍是每 draw 的成本，`ServerLoop.h:89-99`）。填侧站点是静态已知的（PipeFill.cpp 的
~10 处拼写），直接拼 `MGPipeClientInputs()`；读侧拼 `gPipeInputs`（server 块）。线程谓词只在
调试断言/新守卫里用。

---

## 3 poison / Fatal{UnmigratedPipeInput} / FilledGen 世代戳机制

### 3.1 戳的结构与判定

- `MGPipeFilledState { Uint64 CurrentVerbSerial; Uint64 FilledGen[63]; }`
  （`MG_Pipe/generated/PipeFilled.inc:407-410`；63 = `kMGPipeInputFieldCount`，`:96`）。
- 新鲜判定 `MGPipeInputFieldIsFresh`（`PipeFilled.inc:421-426`）：`gen==0` → false
  （"从未填过"，两个分支同义）；sticky → true；否则 `gen == CurrentVerbSerial`。
- **serial 从 1 起**（`PipeFill.cpp:3420-3423` 与 `PipeInputs.cpp:280-283` 两处同述）：
  0 保留给"从未填过"，所以 server 的清零（`PipeInputs.cpp:300-303`）是真撤回而不是"碰巧旧"。

### 3.2 Fatal 形态

- 基础版：`MGPipeInputPoisonFatal`（`PipeFilled.inc:412-416`）——
  `MGPipe: Fatal{UnmigratedPipeInput, "<field>@<verb>"}` + `std::abort()`；verb 取名经
  `MGPipeInputPoisonFatalForVerb`（`PipeInputs.cpp:30-32`，首个 verb 前是 `"<none>"`）。
- split 四路裁定 `MGPipeInputUnfreshRead`（`PipeInputs.cpp:312-345`）：
  非 server-stamped → monolith 语义 Fatal；server-stamped + RECORD_SUPPLIED/APPLIER_DERIVED →
  `Fatal{RoleViolation, "<field>"}`（P5c gt，`:333-341`）；BARRIER_PULLED 且在本 verb 类内 →
  `CountBarrierPull`；其余 → poison Fatal。
- `CountBarrierPull`（`PipeInputs.cpp:162-187`）三臂：**未设障记录 → 无条件 Fatal**
  （`:163-165`，无旋钮可豁免，理由 `:122-132`——值按构造是 torn 的）；设障 + strict + 未被
  静态表或 escalation 承认 → `StrictBarrierPullFatal`（`:182-184`，消息尾
  `[BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in <phase>]`）；设障 + admitted →
  每进程每 (field,verb) 一条 `Admitted{...}` MGLOG_W（`AdmittedBarrierPullOnce` :96-117，
  静态位图去重）。
- 参数窄化：`MGPipeInputArgumentRead`（`PipeInputs.cpp:347-370`），今天唯一行是
  GetPixelStoreParameters 的 unpack 半 → FATAL（`PipeInputs.h:439-447`）。
- sticky 七字段无 `MGP_INPUT_CHECK`（声明的例外，`PipeInputs.h:683-688`），由
  `MGPipeStickyForwardPull`（`PipeInputs.cpp:372-378`）在 server-stamped verb 内手工计入
  `rsp`/strict。

### 3.3 MOBILEGL_PIPE_POISON 旋钮在哪解析

**它不是运行时环境变量**：`PipeInputs.h:30-35` 的编译期派生宏——
`MOBILEGL_PIPE_PUSH && (LOG<=DEBUG || MOBILEGL_BUILD_DISAGGREGATED || MOBILEGL_PIPE_VERIFY)`。
运行时旋钮只有毒化家族的**测试**旋钮：`MOBILEGL_PIPE_POISON_OMIT=<Verb>:<Field>`
（阴性对照 B）→ `Features.PipePoisonOmit`（`Config.h:373-378` 定义、`ConfigLoader.cpp:278`
解析）→ `PipeFill.cpp:518-543` `ParsePoisonOmissionKnob` 在每次 Validate 时查对，
未知名 `BadKnob` Fatal。

**今天为何能工作**：戳是每 verb 的代际号而非位图（`PipeFilled.inc:19-23`：位图看不见
"上一 draw 填的、这一 verb 没重填"），stamp/withdraw 双写者靠 verb barrier 串行化。

**P5f 处置建议**：双块下 server 块的 BARRIER_PULLED 戳**已经被 server stamp 清零**——freshness
语义原样成立。关键缺口在**值**：knob 开时 server 块的 15 个 BARRIER_PULLED 字段没人填，
O 类 SharedPtr 字段（GetBoundVertexArray/GetProgramForDraw/GetTransformFeedbackProgram）
**没有 null 检查**（对比有检查的 `GetTextureUnitObject`，`PipeInputs.h:669-676`），直接返回
空 SharedPtr 给后端解引用 → 是**无名崩溃而不是具名 marker**。所以 f1 必须把
`CountBarrierPull` 在双块臂下改成无条件 `StrictBarrierPullFatal`（why 串填
`MOBILEGL_IPC_ROLE_SPLIT_STATE=1`，marker 语法与 `split_log_paths.py:44-46` 的
`MARKER_RE` 天然兼容）——这是"红就是清单"成立的前提，否则 O 类字段的红是段错误。

---

## 4 MOBILEGL_IPC_* 旋钮与新旋钮的最小接线

### 4.1 现有机制

- 声明：`Config.h:468-547` `struct IpcTable`（`MOBILEGL_BUILD_DISAGGREGATED` 内）；
  实例 `MG_Config::Ipc`（`ConfigLoader.cpp:35`）。
- 解析：`MG_ConfigLoader::InitIpc()`（`ConfigLoader.cpp:356-416`），由 `Init()` 在
  `InitTransport()` 后调（`:428-429`）。env 收集是前缀制
  （`IsAcceptedPrefix`，`ConfigLoader.cpp:42-44`），**任何 MOBILEGL_ 前缀自动可见，
  无需登记**（`:257-259` 注释明说）。
- 工具函数：`QueryEnvFlag`（truthy：非空、非 "0"、非 "false"）、`QueryEnvUint32`（带
  min/max 与非法值 WARN 回落）、`QueryEnvVariable`。
- 纪律：**新旋钮必须进 Config 由集成者落地，不得在调用点现造**
  （`EmitTables.h:109` 原文：调用点发明的 MOBILEGL_IPC_ 形名字是反模式；
  `Config.h:463-467`：P5 只落自己包读的旋钮，后续阶段的旋钮由集成者加在这里）。

### 4.2 `MOBILEGL_IPC_ROLE_SPLIT_STATE=1` 的最小接线

1. `Config.h` IpcTable 尾部加 `Bool RoleSplitState = false;` + 注释（照 `StrictErrors`
   `Config.h:534-537` 的形状：一句话说从什么升成什么）。
2. `ConfigLoader.cpp` `InitIpc()` 加 `ipc.RoleSplitState = QueryEnvFlag("MOBILEGL_IPC_ROLE_SPLIT_STATE");`，
   并进 `:400-406` 的那行汇总日志。
3. 两个交互要写死：
   - **monolith 下无效**：`:397` 已有 `if (Transport == Monolith) return;`，选择函数处再判一次
     `Transport != Monolith`（monolith 的 backend 与 fill 同线程，分块会立刻饿死所有读）。
   - **与 PipeVerify 互斥**：verify 的入口比较比的是填侧块、compare-at-read hook 以
     `&self == &gPipeInputs` 判活块（`PipeFill.cpp:663`），双块下 hook 会在 apply 线程重读
     活 context。照 `:388`（`if (PipeVerify) ipc.RunAhead = 0;`）的形状强制
     `RoleSplitState = 0` 并留一行 WARN。
4. 消费点：`PipeInputs.h` 的选择函数（§7）。`PipeInputs.cpp` 已 include `Config.h`
   （`:20`），无新依赖方向问题。

**今天为何能工作**（对"加旋钮"这件事）：前缀收集 + IpcTable + InitIpc 三点一线，StrictErrors
/ Audit 两个 Bool 旋钮是逐字可照抄的样例。

---

## 5 CONTRACT-P5E §3.2 的 per-role stamp 存储：内容与归属裁定

### 5.1 内容（承诺了什么）

`MG_Remote/CONTRACT-P5E.md:224-229`（§3.2）：`MGPipeServerStampVerbBoundary` 应把
`m_filled` / `m_currentVerb` / `m_serverStampedVerb` 写进 **applier 私有存储而绝不写共享块**；
client 侧的 `MGPipeServerClearVerbBoundary()`（PipeFill 的两处调用）在 transport 下删除；
`SetIdentity` 移到 `ApplyOne`、每会话一次。

### 5.2 落地状态与编号归属（任务单说"ID-120"，需更正）

- §3.2 **UNLANDED**，且契约自己在 ra2 修正里点名了它（`CONTRACT-P5E.md:266-272`）：
  "§3.2 above, which is UNLANDED and was never marked as such"，stamp 今天仍写进
  `gPipeInputs` 本体。拆 stamp 在契约里被记为 **P11 项**，并附关键告诫：**只有 per-role
  stamp 而不给 BARRIER_PULLED 值做版本化，比今天更坏**——会把具名 Fatal 变成静默旧值读
  （`:270-272`）。
- 编号：`CURRENT_STAGE_PROGRESS.md:232` 把这一行挂为 "ID-120 / ID-135"。逐条核对后：
  **ID-120 是 §8 修正 8**（`GetBufferBindingPointCount` 的 forward → FATAL 子句，
  `CONTRACT-P5E.md:606-617` 标 UNLANDED、Lands with P7/P13）；**§3.2 的 per-role stamp
  对应 ID-135**（ra2 元数据竞态的裁定，`notes/p5e/FACTS-P5E.md:178`、ROADMAP.md:39
  P11 行："当时那次竞态在元数据标量上，修法是让守卫测事实而非断言未来（ID-135）……
  契约 §3.2 的 per-role stamp 存储仍未落地"）。任务单的"ID-120"应读作"ID-135（§3.2）"。

### 5.3 双块机制应不应该顺手落它

**应该，而且是 f1 的必然子集而非可选项**，理由：

1. 双块一落地，server 块**就是** applier 私有存储（client 再也摸不到它）——§3.2 第一句在
   knob 开的臂上自动成立；knob 关的臂保持共享块旧语义（A/B 所需）。
2. §3.2 的另两条在双块下**变成 moot 而非要删的代码**：client 侧的
   `MGPipeServerClearVerbBoundary`（`PipeFill.cpp:3437, 2228`）清的是自己块的 flag
   （无人读过它，无害），不需要"删除"，只需要让它作用于 client 块（见 §7 的参数化）。
3. **唯一真正要做的搬运是 `SetIdentity`**：今天 client 每个 verb 都写
   `m_live`/`m_contextIdentity`（`PipeFill.cpp:3377-3385`，这是 §3.1 的具名偏离，因为
   "本进程有没有活 GL context"不属于任何字段的归属类）。双块后 server 块的
   `MGB_CTX_LIVE`/`MGB_CTX_IDENTITY`（`PipeInputsSwitch.h:20-21`）**永远答 false/nullptr**，
   后端所有 null-context 守卫会立刻全部翻车。f1 必须在 server 侧给 server 块置一次身份
   ——契约 §3.2 末句"SetIdentity moves to ApplyOne, once per session"正是答案，
   落点在 `PipeApplier::Attach`（`PipeApplier.cpp:1140-1149`）或首个 verb boundary。
4. 契约的告诫（§3.2 不配版本化就更坏）在双块下由 §3.3 的"CountBarrierPull 改无条件
   Fatal"兑现：BARRIER_PULLED 值在 server 块里**不存在**（不是旧值），读它具名死——
   恰好是契约要求的方向。

---

## 6 integration-split 车道的声明方式与双块车道形状

### 6.1 现有车道形态（样例）

- **标签纪律（ID-131）**：`ctest -L` 是**正则**，所以"必须从 integration-split 里搬出去的
  条目"的标签不能含子串 `integration-split`。样例：
  - `integration-magma-split`（`MG_IntegrationTest/CMakeLists.txt:2422-2450`，理由
    `:2428-2433`：曾实测 `integration-split-magma` 拼法会被 `-L integration-split` 匹配中）；
    对应预期红 CI 步 `test.yml:1360-1381`：红 + 私有日志里必须有具名 marker，绿也是错。
  - `integration-clientarrays-split`（`CMakeLists.txt:2216-2225`）；CI 步
    `test.yml:1405-1440` 是**三臂**：run-ahead 未武装却红 = 真回归；武装却绿 = 具名拒绝没
    打响；红 + 日志带 `Fatal{UnmigratedVerb, "...+CLIENT_ARRAYS"}` = 通过。
- **strict 车道**：主要形态是**环境变量车道而非 ctest 标签**（`f0-env.md:196`：
  `MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L integration-split` + marker 直方图）；另有少量
  自带旋钮的注册条目（`StrictProgramArm.*`，`CMakeLists.txt:2327-2352`，LABELS 故意只写
  `integration-split`，理由 `:2321-2326`）。
- **预期红的双态门**：`test.yml:1207-1317`——同一车道在翻转常量
  （`kMGPipeP5eRunAheadReady`，grep 自 `Init.cpp`）前是"红即设计、跑 census"，后是"硬绿 +
  两侧棘轮"。
- **私有日志**：每条 split 条目一个 `MOBILEGL_LOG_FILE_PATH`，
  `SplitLogPaths.PrivateAndDistinct`（`CMakeLists.txt:2360-2364` +
  `Harness/split_log_paths.py` 的 `check` 模式 :183-188）断言私有且互不相同；
  markers 模式的日志集来自 `ctest --show-only=json-v1` 而非目录猜测（:49-70，ID-119）。
- **棘轮**：`Harness/strict-expected-markers.txt`（52 行，10 对活着的
  Admitted `<field>@<verb>`）+ `split_log_paths.py` 的 `markers` 模式（:116-178）——
  **两侧都咬人**：出现了表里没有的对 → 红；表里的对不再出现 → 红（防"烂绿"，:122-126）。
  Fatal 侧在 `no-fatal` 臂下任何一对即红。

### 6.2 双块车道的具体形状

1. **标签**：`integration-dualblock-split`（不含 `integration-split` 子串，正则安全；
   同样不带 `integration-gpu`，因为该标签下的车道必须能绿，`:2210-2211` 同论证）。
2. **条目**：`gtest_discover_tests` 同一批 split scenario，`TEST_PREFIX
   "DirectGLES.Split.DualBlock."`——前缀仍在 `DirectGLES.Split.` 下，
   `split_log_paths.py` 的 `paths()`（:19 的前缀判定）与 markers 模式继续覆盖，无需改工具。
   env = split env + `MOBILEGL_IPC_ROLE_SPLIT_STATE=1` + 每条目私有
   `MOBILEGL_LOG_FILE_PATH`（照 F1./Ct. 块形态，`CMakeLists.txt:2367-2404`）。
3. **CI 步**（test.yml 新增，照 :1360-1381 的双态形状）：f1 落地时**预期红**——
   步断言 (a) 车道非绿，(b) 私有日志带 `Fatal{UnmigratedPipeInput, "..."} [BARRIER-PULLED,`
   形 marker（语法已在 `MARKER_RE`，`split_log_paths.py:44-46`），(c) 把 census 对表提交为
   新的预期文件（见 §7.4）。收口包翻转一个常量（照 `kMGPipeP5eRunAheadReady` 的
   grep-源码判臂法，`test.yml:1215`）后同一批条目转硬绿。
4. **阴性对照（§6 出口门，ID-122 教训：对照要证伪机制本身）**：关掉双块必须让至少一条具名
   用例由绿转红。单纯"共享块恢复旧行为"是全绿的，所以对照用例必须是**为双块专门写的确定性
   断言**，建议：`DualBlockScenario.TheTwoRolesHaveDistinctBlocks`——经新增 Harness peek
   （照 `SplitRuntimePeek.h`/`PipeApplyPeek.h` 形态）断言两块地址不同、且 client 填一个 verb
   后 server 块的 `CurrentVerbSerial` 不动。knob 开 → 绿；knob 关（`ROLE_SPLIT_STATE=0`）→
   同一块 → 断言失败 → 红。确定性、无时序、不在 pre-flight 里 skip（ID-62），形状可直接进
   `scripts/ci/split_negative_controls.sh` 的 `run_control`（该脚本现成的合同：基线必须绿、
   红必须带被选条目自己的断言文本，`split_negative_controls.sh:60-99, 118-168`）。

---

## 7 f1 落地方案

### 7.1 块的分区形态（最小改动面）

`PipeInputs.h:835` 一带（`MOBILEGL_BUILD_DISAGGREGATED` 内，保持 pull/verify 构建符号不动）：

```cpp
// server 角色的块：后端经 MGB_CTX 读的、applier 写的、stamp 落的那一块。
// gPipeInputs 的拼写不变，379 个 MGB_CTX 读点与 PipeApply.cpp 的写点零改动。
inline PipeInputs& gPipeInputsServerBlock = *new PipeInputs();
// client 角色的块：MG_Impl 的残余填充写的那一块。
inline PipeInputs& gPipeInputsClientBlock = *new PipeInputs();
inline PipeInputs& gPipeInputs = gPipeInputsServerBlock; // 读侧拼写，向后兼容

inline Bool MGPipeRoleSplitActive() {
    return MG_Config::Ipc.RoleSplitState &&
           MG_Config::Transport != MG_Config::TransportMode::Monolith;
}
// 填侧唯一新拼写。knob 关或 monolith：同一块，行为逐字节不变。
inline PipeInputs& MGPipeClientInputs() {
    return MGPipeRoleSplitActive() ? gPipeInputsClientBlock : gPipeInputs;
}
```

- 两块都 `*new`（leak-at-exit，`PipeInputs.h:818-834` 的论证原样适用，每条目多一块几 KB）。
- `MGPipeRoleSplitActive` 是进程寿命内常量；若要省热路径读，可在首次调用处 latch
  （照 `ArmVerify` 的 re-arm 形态，`PipeFill.cpp:598-624`），但 Config 读本身只是两次
  全局 load，填侧每 verb 一次，可接受。

### 7.2 拼写替换清单（client 侧，全在 `MG_Impl/Pipe/PipeFill.cpp`）

`gPipeInputs` → `MGPipeClientInputs()`：`:627`（ReportDivergence 的 serial/verb 取名）、
`:653`（SnapshotFromGLContext 的 SetVerb）、`:703`（NoteFrontendMutation）、`:1217`
（ReleaseResidualFillPins）、`:2205`（LeaveVerb）、`:3016`（VertexAttribDefaultsOf 读）、
`:3348`（ValidateForVerb）。
**例外留着不动**：`:663` 的 `&self == &gPipeInputs` 判等——hook 只该钉在**读侧块**上
（server 块），knob 开时它与 verify 互斥（§4.2），knob 关时两块同物，两种臂都正确。

### 7.3 server 侧需要的三处配合

1. **`MGPipeServerClearVerbBoundary` 参数化**（`PipeInputs.cpp:307`）：今天写死 gPipeInputs。
   拆成 `MGPipeServerClearVerbBoundaryFor(PipeInputs&)`（或直接让两个调用角色各调各的）：
   applier 的 `LeaveApplier`（`PipeApplier.cpp:1261`）清 server 块；client 的两处
   （`PipeFill.cpp:3437, 2228`）清 client 块。knob 关时同一块，语义不变。
2. **server 块身份**：`PipeApplier::Attach` 或首个 `StampVerbBoundary` 处给 server 块
   `SetIdentity`/置 `m_live`（§5.3.3），否则 `MGB_CTX_LIVE` 恒 false。
3. **`CountBarrierPull` 的双块臂**（`PipeInputs.cpp:162-187`）：`MGPipeRoleSplitActive()` 时
   无条件 `StrictBarrierPullFatal(field, verb, "MOBILEGL_IPC_ROLE_SPLIT_STATE=1")`——
   这是把 §3.3 的"无名崩溃"变成"具名红"的一行，也是计划 §4 "立刻变成 Fatal" 的机制落点。
   `rsp` 计数语义不变（knob 开时 BARRIER_PULLED 读都死了，rsp 恒 0，与出口门 3 的
   "rsp 逐帧读零"自洽）。

### 7.4 "红就是清单"：红怎么收成清单

1. f1 落地当天跑双块车道，每条目在自己的私有日志里留下
   `Fatal{UnmigratedPipeInput, "<field>@<verb>"} [BARRIER-PULLED, MOBILEGL_IPC_ROLE_SPLIT_STATE=1, retires in <phase>]`。
2. `split_log_paths.py markers` 已能解析并按条目归类 Fatal 对（:83-113, :133-136）。
   需要一个小扩展：一个 `expect-fatal` 臂，把 **Fatal 集合**与提交的预期文件做与
   `strict-expected-markers.txt` 完全同构的双向棘轮（出现新对 → 红；旧对消失 → 红）。
   新文件建议 `Harness/dualblock-expected-fatals.txt`，首版内容由 census 生成，
   **不由手写**（strict 文件 `:15-20` 的刷新规程照抄）。
3. 之后每个 f 包（fm/fc/fs/fr/fv）退掉自己名下字段的载体，删对应行；文件空 + 车道绿 =
   收口（对照 P5f §5 的包图与 §6 出口门 1/2）。strict 车道的
   `strict-expected-markers.txt` 棘轮继续独立存在——双块车道管"值不在对面"，
   strict 车道管"读没被承认"，两者是同一债务的两个探针。

### 7.5 守卫与测试的兼容

- `RefusePipeInputsTouchWhileApplierOwnsIt`（`ClientSession.cpp:1290-1380`）保持原样：
  它守的是"GL 线程碰 server 拥有的块"，knob 开时填侧不再碰 server 块，守卫自然不再触发；
  knob 关时行为不变。RemoteClientTest 的死亡测试（:2071, :2295, :2347）不受影响。
- 单测/集成 GPU 车道 knob 默认关 → 单块，逐字节不变；FieldOwnershipTest/PipeInputsTest
  直读 `gPipeInputs`（server 块拼写），monolith 下同物。
- G1：新增符号全部关在 `MOBILEGL_BUILD_DISAGGREGATED` 内（该头本就只在 PUSH 闭包，
  `PipeInputs.h:39-43`），pull 构建符号集不动。

---

## 8 风险与待确认

1. **O 类 SharedPtr 字段无 null 检查**（§3.3）——f1 的第一批红若不走 §7.3.3 的 Fatal 化，
   会以段错误形态出现，census 收集不到 marker。**这是 f1 落地顺序里必须先落的一行。**
2. **`m_live`/`m_contextIdentity` 不属于任何字段归属类**（`PipeFill.cpp:3377-3385` 的具名
   偏离）：双块后 server 块的身份由 §7.3.2 置一次，但**失语境**（context destroy）时谁把
   server 块的 `m_live` 打回 false——契约 §3.2 没说，f1 设计时要点名（候选：applier_reset
   控制记录，P5c ct 已让它过线）。
3. **Magma 臂**：Magma 不发布 `kCapRunAheadApply`、跑 lockstep，九个 BARRIER_PULLED 字段
   全是它的读者。双块车道建议**双后端各注册一份**（照 `NamedBlit` 的双后端循环，
   `CMakeLists.txt:2423-2449`），Magma 侧的红就是 fm 包的清单——这与 §6 出口门 4
   "Magma 与 Espryt 跑同一套判据"直接对齐。
4. **verify 构建**：`MOBILEGL_PIPE_VERIFY` 强制 lockstep + 全填（`ConfigLoader.cpp:377, 388`），
   与双块互斥的建议已写进 §4.2；若未来要 verify 双块臂，compare-at-read hook 的块归属要重做。
5. `MG_Test` 里直接调 `MGPipeServerStampVerbBoundary` 的用例（PipeWireCodecTest :1366-1380、
   FieldOwnershipTest 约 15 处、RemoteClientTest :2393-2413）stamp 的都是 server 块拼写，
   knob 关时不变；若有人想在单测里开 knob，需要新 peek——目前无此需求，标为**未知/待 f1 定**。
