# a6-audit-v1 — spawn 之前，进程/角色边界还差什么

> 只读静态审计，头 `b3b9c2ee`（`git rev-parse HEAD`，2026-09-21）。无 git 变更。
> 八项中的第 5 项（能否不链 `MG_Impl`）是唯一的实验，单独成篇：[`a6-link-experiment.md`](a6-link-experiment.md)。
> 全部 198 行逐条清单、复审异议与遗漏补充：[`a6-audit-rows.md`](a6-audit-rows.md)。
> 形状照 [`../p5c/p5c-audit-v1.md`](../p5c/p5c-audit-v1.md)。
>
> 方法：七项各由一个审计 agent 读树产出，再由一个对抗复审 agent **逐条重新解析 `file:line`** 并反向找遗漏。
> 198 行里复审对 65 行提出异议，本文只采用**存活的**结论；被判为假的行保留在逐行清单里，标着为什么假。

---

## 0 一句话

**P6 的传输那一半比计划以为的少（fc/fs 确实做完了），进程/角色那一半比计划以为的多得多。**

P6-SPAWN-PLAN 与 P6-CONTRACT-DRAFT 有**四处陈述在树上是假的**（§2），
其中两处（device-lost 闩锁"已实现"、`cp` "只剩传输"）各自欠着一个完整的包。
而真正的头号发现是：**`spawn` 今天在代码里根本不是一个可观察的状态**，
所以计划里"子进程强制 monolith"这条反递归规则，**执行它就等于让 server 进程整体选中前端臂**。

---

## 1 头号发现：`Transport` 这一个 bit 同时回答三个问题，而 spawn 让它们分家

### 1.1 `spawn` 现在连一个可观察的值都不是

`ConfigLoader.cpp:339-346` 精确识别 `spawn` / `unix:` / `pipe:`，然后写

```cpp
MG_Config::Transport = MG_Config::TransportMode::Monolith;
```

并记一行"P5 不实现，退回 monolith"。**于是"子进程被强制成 monolith"和"父进程解析失败退回 monolith"是同一个状态**，
`ConfigLoader.cpp` 那条具名拒绝**证伪不了自己**——它落在与 server 静息态相同的值上。

### 1.2 `Transport != Monolith` 有 238 处，它选的是**server 臂**

复审精确确认了 238 这个数（`MG_Config::Transport` 的非测试 grep 行数，含 `TransportMode` / `TransportEndpoint` 后缀），
分布 155 `MG_Backend` / 35 `MG_Impl` / 21 `MG_Remote` / 12 `MG_Pipe` / 8 `MG_State` / 7 Config；
其中 **12 行是注释**，所以活代码是 226 行。

**`ARCHITECTURE.md:488` 的"子进程强制 monolith"如果按字面实现，148 行 `MG_Backend` 活代码会一起翻到前端臂**——
在那个唯一没有前端的进程里。最锋利的一处是 `PipeInputs::IsLive()`（`MG_Impl/Pipe/PipeFill.cpp:2135`）：
强制 monolith 后它读 `MG_State::pGLContext`，在 server 进程里是 null，于是 **58 个 `MGB_CTX_LIVE` 守卫全部答"不活"**。

> **给 c6 的第一个决定**：反递归的保险丝与 server 角色选择器**必须是两个独立的事实**。
> 推荐形状：子进程保持 `Transport == Spawn`（于是 238 个站点原样选中 server 臂），
> 反递归另设 `Dial = No`。这样 c6 只需重写真正在问 Dial / Role 的那一小撮，而不是 238 个。

### 1.3 三个谓词覆盖得了，但审计找出了**第四、第五个问题**和一个命名冲突

复审推翻了我在 `P6-ENDSTATE-REVIEW.md` 里的一个判断（见 §5），并给出更准的分类：

| 模式 | 活代码行 | 应成为 |
|---|---:|---|
| 后端体内的 server 臂测试（DirectGLES 98 + DirectVulkan 50） | 148 | `MGPipeServerArm()` |
| client 臂 emit/decide 测试（`MG_Impl` 34 + `MG_State` 6） | ~48 | `MGPipeSplitActive()` |
| 四个 `SlotCaps` 宏 | 4 | 局部性，见下 |
| 共享 applier / codec 里角色中立的测试 | ~11 | `MGPipeSplitActive()`，**最机械的一组** |
| Dial / bring-up（`Init.cpp` + `ClientSession`） | 3+ | `MGPipeShouldDial()`，**P6 的真活在这里** |

**没被三个谓词覆盖的**：

- **第四个问题——"对端会话是否活着"**。已有两处在用：`PipeApply.cpp:1355`（`ClientSession::Active() != nullptr`）、
  `ClientSession.cpp:1320`/`:1383`（`ClientSessionInstance().Started()`）。在 server-only 进程里前者**永远是 null**，
  于是 `MGPipeApplierReset` 的第二层角色守卫**在最需要它的进程里永远武装不起来**。
- **第五个问题——"两个角色是否共用一份存储对象"**，即已经存在的 `MGPipeRoleSplitActive()`（`PipeInputs.cpp:292`）。
  spawn 下它结构上恒为假（两个地址空间），但它的定义是 `Ipc.RoleSplitState && Transport != Monolith`，**默认关**。
- **局部性**："后端在不在本进程"。`SlotCaps.h:116`/`:133-136` 今天拼成 `Transport == Monolith && slot != nullptr`。
- **命名冲突**：我提议的 `MGPipeSplitActive()` 与既有的 `MGPipeRoleSplitActive()` 只差一个词、含义严格更窄。**先改名，再引入。**

### 1.4 一条 G1 约束

非 disaggregated 构建里 `Transport` 是 `inline constexpr Monolith`（`Config.h:566`），
所以三个新谓词**都必须 `inline constexpr` 可折叠为 false**，否则 pull 构建的 `.text` 恒等门会红。

---

## 2 计划文档里在树上为假的四处

### 2.1 `cp` **不是**"只剩传输"——`InitCapabilities` 的**请求**没有 wire 形式

`P6-CONTRACT-DRAFT:98-101` 与 `P6-SPAWN-PLAN:76` 都写 "`InitCapabilities` 由 `CapsSnapshot` 回答"。
**答案**是 `CapsSnapshot` 没错，但**请求**不是：`BackendObject_Remote.cpp:126` 有活的 client 调用者，
而 `EncodeSurfaceOpFrame` 对这个 kind **具名拒绝**（`SurfaceOpCodec.cpp:119-122`，`InprocOnlyOpOnTheWire`）。
spawn 的 client **问不出来**。这是 `cp` 必须先决的第一件事，不是传输。

### 2.2 device-lost 闩锁不存在（上轮已发现，a6 逐条坐实）

复审确认：`FatalCode::DeviceLost`/`ServerCrashed` 零 producer 零 consumer；
`glGetGraphicsResetStatus` 恒返回 `GL_NO_ERROR`；`MOBILEGL_IPC_RESPAWN`/`IDLE_EXIT_S` 除 `Config.h:466` 一句注释外无人解析。
另外 `P6-CONTRACT-DRAFT:46` 引 `protocol.fbs:234-242` 指 `FatalCode`，实际在 `:248-256`——**漂了十四行**。

### 2.3 "server 不链 `MG_Impl`"——`PipeInputs.h:281` 与 `ARCHITECTURE.md:9` 都在断言，都是假的

见 [`a6-link-experiment.md`](a6-link-experiment.md)：184 个符号。
a6.2 独立给出同一结论的另一半证据：`PipeInputs::IsLive()` 与另外 **17 个 `PipeInputs::` 成员定义**编译在 `MG_Impl/Pipe/PipeFill.cpp`，
**两个角色拒绝助手**（`MGPipeRefuseAllocatorFromApplyThread` / `...FrontendKeyedRegistryFromApplyThread`）编译在 `MG_Impl/Pipe/SlotAllocator.cpp:29-57`。
**把 `MG_Impl` 从 server 镜像里拿掉，等于把守卫一起拿掉。**

### 2.4 大量行号漂移

`ClientSession.cpp:468`→`:482`；`BackendObject.h:561-566`（`WindowHandle`）→`:579-584`；
`protocol.fbs:186-196`（`SurfaceOp`）→`:194-210`；`ServerSession.cpp:670-690`（门铃访问器）→`:709-740`；
`ConfigLoader.cpp:310-348`→`:319-351`；`CONTRACT-P5C.md:262`、`PipeInputs.h:854` 的计数（379→383）等。
`c6` 落地时逐条重解，`check_doc_citations.py` 应当覆盖这些。

---

## 3 阻塞 spawn 的硬发现（复审存活）

### 3.1 apply 线程会在弹出第一条记录之前 abort

`ServerSession::ConsumerDoorbell()`（`ServerSession.cpp:709-724`）对**任何非 `InProcess` 的 role**
`Fatal{UnmigratedVerb}` + `std::abort()`，`ProducerDoorbell()` 同（`:727-739`）。
`ApplyThreadMain` 无条件调用它。注释自陈这是按**契约 §3.9** 把访问器留在 session 上"好让它保持一处 switch"。

> 这正好是 `P6-ENDSTATE-REVIEW` 里 `lk` 包要**废止**而不是**修补**的那条裁定——两条独立得出的结论对上了。

### 3.2 控制面在 server 侧**没有读者**，在 client 侧**被丢弃**

- apply 线程的 `ready` 谓词只有三项（stop / `ControlIsPending()` / `cmdHead != LocalTail()`）——
  **控制 socket 不在其中**（`ServerLoop.cpp:398-402`）。socket 上到达的 `SurfaceOp` **没有任何人读**。
- client 的控制泵只消费 `CapsSnapshot`，其余（含 `SurfaceOp`/`SurfaceReply`）记一条 WARN 然后 `continue`
  （`ClientSession.cpp:1425-1432`，注释写明"是 P6 的"）。

### 3.3 发帖方的等待**没有期限**

`m_controlDone.wait(lock, pred)`（`ServerLoop.cpp:724`）是无期限谓词等待。
对照 ring 侧有 `kBarrierTimeoutMs`。**被 SIGSTOP / Android freezer 冻住的 server 不产生 EOF 也不产生 reply**，
GL 线程就永远卡在 `eglMakeCurrent` 里，没有任何诊断。这正是 `P6-SPAWN-PLAN §4` 点名却没落到代码上的那个状态。

### 3.4 `SurfaceReply` 装不下一个 `MobileGLResult`

wire 只有 `{seq, ok, eglMajor, eglMinor}`（`protocol.fbs:212-218`），`ServerApplyWireSurfaceOp` 的 rc 被丢掉。
**client 因此分不清"server 半路死了"和"一次合法的拒绝"**，也就无法决定是武装 device-lost 闩锁还是返回 `EGL_FALSE`。

### 3.5 `seq == 0` 被静默重编号，且 reply 不做关联

`RunSurfaceControlFrame`（`ServerLoop.cpp:659-662`）在 `frame.seq == 0` 时自己铸号，
而它自己的注释说 wire 请求必须保留 client 的号。`DecodeWireSurfaceReply`（`SurfaceOpCodec.cpp:190-195`）
把 seq/ok 直接覆盖进交给它的帧并返回 void——**不做任何关联**。

### 3.6 server 进程只能有一套 EGL，且第二套**不具名**

`g_Display` / `g_Context` / `g_Surface` / `g_Config` 是每进程 file-static，
`InitDisplayAndContext` 开头无条件 `DestroyEGLContext()`（`DirectGLES.cpp:16012-16013`），
而后者最后一步是 `eglTerminate(g_Display)`——**进程级** EGL 拆除。
一 guest 一进程因此不是设计偏好，是**今天唯一成立的形状**；而它现在**没有具名拒绝**。
`ServerSession::Accept` 的同对象臂返回 `MOBILEGL_ERR_INVALID_ARGUMENT` **且没有任何日志**（`ServerSession.cpp:425`）。

### 3.7 角色守卫在 server 进程里**武装不起来**

| 守卫 | 武装条件 | 在 server-only 进程里 |
|---|---|---|
| `MGPipeApplierReset` 第二层 | `ClientSession::Active() != nullptr`（`PipeApply.cpp:1353-1355`） | 恒 null → **永远不武装** |
| `MGPipeServerBlockNoteIdentity` | `MGPipeRoleSplitActive()`（默认关） | 早退 → `ContextIdentity()` 恒 nullptr |
| `CountBarrierPull` 无条件 Fatal 臂 | 同上 | 关 |
| 两个 `RefuseFromApplyThread` | `ServerLoop::OnApplyThread()` | 与"是 server 进程"**不再重合** |

**其中 `ContextIdentity()` 恒 nullptr 是一个会崩的洞**：`DirectGLES.cpp:199-213` 的 fb-slot memo 以
`MGB_CTX_IDENTITY` 做裸指针比较、无世代，nullptr 会被读成**缓存命中**，
于是第一次调用就用上一个没初始化的槽——`PipeInputs.h:878-884` 的注释说这里本该是一条具名 Fatal。

---

## 4 a6.7：abort 普查——funnel 是改名还是重设计

`MG_Remote` 下 `std::abort()` 共 **94 处 grep 命中，其中 2 处在注释里，实际 92 个站点**
（上轮我写的 93 少一个，更正见 §5）。

**家族分布**：`Fatal{}` 词汇有 **30 个不同的家族词**（`ProtocolCorruption` 37、`RoleViolation` 8、`RingOverrun` 4、
`UnmigratedVerb` 4、`EventRingOverflow` 4、`NoClientSession` 3、`ReplyError` 3 …），
而 schema 的 `FatalCode` 只有 **7 个值**（`protocol.fbs:248-256`）。
**`Session::Fail(FatalCode, detail)` 的第一个参数装不下今天的 30 个家族**——这是 c6 必须先决的签名问题。

**两个站点完全没有 `Fatal{` 标记**（`PersistentMapTracker.cpp:1052-1058`、`WireLog.cpp:41-50`），
所以"日志逐字不变"这个承诺对它们不成立，家族 grep 也看不见它们。

**从对端字节可达、且今天会 abort 的无界分配**（三处，`MG_Remote` 下**没有任何 try/catch**）：

| 站点 | 形状 |
|---|---|
| `PipeApply.cpp:1563-1569` | `MGPipeApplyCreateRenderState` 按对端给的 `Cso.Slot+1` resize，**无槽位上限**；兄弟家族全走 `RecordAt` 的有界路径。`Slot = 0xFFFFFFFE` → 约 1.7 TB |
| `PipeApplier.cpp:416-441` | readback scratch 按 `Box.W*Box.H*bpp` resize，**resize 发生在任何尺寸门之前**。65535² RGBA8 → 17 GB |
| `ProgramArtifactsCodec.cpp:166-223` | count 只按**剩余字节**设界，然后乘 `sizeof(element)`。32 MiB stage 可伪造出高一两个数量级的 resize |

---

## 5 更正我自己上一轮的四处

复审逐条核了 `P6-ENDSTATE-REVIEW.md`，以下四处我写错了，按树更正：

1. **`MG_State` 那 8 个站点不是我说的那个形状。** 我写"唯一不按目录成形的一批，同一个函数里同时问两个问题"。
   实际：8 个里 **2 个是注释**（`BufferObject.cpp:60`、`BufferObject.h:194`），
   剩下 6 个里 **5 个是单问题的 `SplitActive` 测试**，**只有 1 个**（`MipmapStorage.cpp:55-56`）真的同时问两个。
   而"同时问两个"的形状**根本不是 `MG_State` 局部的**——它散在 `SlotAllocator.cpp` / `PipeFill.cpp` / `PipeApply.cpp` /
   `Managers.cpp` / `MG_Remote/Client` 的 **18 个站点**上，而且树里已经有两种拼法。
   **结论不变**（a6 仍应先分类这一族），**理由和取样对象要换**。
2. **abort 数是 92 不是 93**（94 命中减 2 处注释）。
3. **`BackendObject.cpp:283` 是一次调用，不是 store**；全树对 `m_windowHandle` 的 store 只有 **4 处**（`:475`、`:365`、`:206`、`:207`），
   读只有 **5 处**。而且 `.Width` / `.Height` **没有任何读者**，所以 `:206-207` 今天 clobber 不了任何东西。
   **"两道缝"的结论仍然成立且更精确**：真正的第二道缝是 `ActivateEGLSurface → SetWindowHandle(surfaceState->Window)`，
   而 `surfaceState->Window` 只在 `RegisterEGLWindowSurface`（`:232-238`）写一处——**那才是最小的 latch 点**。
4. **`MGPipeSplitActive()` 这个名字不能直接用**，与既有 `MGPipeRoleSplitActive()` 冲突（§1.3）。

另有一处我从 P6 文档继承下来的失效引用：`BackendObject.h:561-566` 指 `WindowHandle`，实际在 `:579-584`。

---

## 6 a6.8：窗口句柄与分片

**窗口句柄**（复审给出的精确形状，比我上轮写的更小也更清楚）：

server 端拥有自己的窗口需要**四处**改动，不是一处：
(a) `RegisterEGLWindowSurface`（`BackendObject.cpp:232-238`）的一次替换；
(b) `ServerLoop.cpp:1015-1017` 的一次拒绝；
(c) `ResetEGLRuntimeState`（`:359-367`）的一次保留；
(d) **非显然的第四处**——`BackendObject_DirectGLES.cpp:962-967` 的 `sameHandle` 去重键拿 client 的窗口指针比 server latch 的那个，
server 拥有窗口后**永不相等**，于是每次 `eglCreateWindowSurface` 都会 `DestroyEGLContext` + `ResetEGLRuntimeState` 重建原生 context。

**一条 P6 当下就该决的**：`BackendObject_Remote.cpp:214-216` **无条件**发 `SetWindowHandle`，
而 `AndroidNativeWindow@P12` 的拒绝在 **server 进程**里 abort。
所以在这台 Redmi 上跑 FCL/Minecraft，**第一次 `eglCreateWindowSurface` 就会把 server 进程 abort 掉**，
client 只看到对端死了。要么 P6 在 **client 侧**镜像这条拒绝，要么接受这个形状并写进契约。

**分片**：`resource_subdata` / `buffer_subdata_resident`（buffer 范围）、纹理层级（**三臂**走查：整深度切片、整行 run、
以及**次行 texel run**——比文档说的"整宽 slab"更细）、持久映射推送、`GetTextureImage` **都有**走查。
**没有走查的**按大小排：program archive（`WireTables.cpp:556`，唯一无界 blob）、
`draw_vbo` 的 `NumDraws * 12` range 尾（`EmitTables.cpp:536`，唯一无界尾）。
**ROADMAP 开放问题 11 点名的"宿主索引 span"在本头没有 producer**——`kDrawHasUserIndices` 唯一出现是
`OwnedDrawInputs.h:51` 的清零，client 索引已经变成 owned buffer 资源。**这条可以从开放问题里划掉。**

> 所以 stream 链路窗口应当对着 **program archive** 和 **draw_vbo range 尾**定尺，不是对着 `set_global_constants`。

---

## 7 给 c6 的决定清单（按该先决的顺序）

1. **`Transport` 在 spawn 的 server 进程里读作什么？** 148 行 `MG_Backend` 活代码和两个 Fatal 角色守卫一起翻在这一个 bit 上。
   推荐：子进程保持 `Spawn`，反递归另设 `Dial = No`。
2. **`Session::Fail` 的第一个参数**：留 7 值 wire 枚举，还是引入一个更宽的内部家族枚举？今天有 30 个家族词。
3. **`InitCapabilities` 请求的 wire 形式**（§2.1）。这是 `cp` 的前置，不是传输。
4. **控制 socket 谁读**：新 `mgl-srv-io` 线程投进现有单槽邮箱（`ServerLoop.cpp:673-727` 一行不改），
   还是给 apply 线程的 `ready` 谓词加第四项？
5. **控制回复等待的期限与名字**（§3.3）。
6. **`SurfaceReply` 是否追加一个 result 字段**（§3.4）。
7. **spawn 下谁铸 `seq`，`seq == 0` 在 wire 上什么意思**（§3.5）。
8. **窗口种类：黑名单还是白名单**；以及 `WindowKind::None` 带非零 token 今天被接受这件事怎么办。
9. **一 server 进程一 guest 写进契约，第二个具名拒绝**（§3.6）。
10. **四个角色守卫的重新武装条件**（§3.7）——尤其 `ContextIdentity()` 恒 nullptr 那个会崩的洞。
11. **三处无界分配的绝对上限**（§4），以及 `MG_Remote` 下要不要有 try/catch。
12. **server 镜像链不链 `MG_Impl`**：按 a6 的答案，P6 链，契约把 `PipeInputs.h:281` 与 `ARCHITECTURE.md:9` 的断言改成记账。
13. **`MGPipeRoleSplitActive()` 先改名**，再引入 `MGPipeSplitActive()`。
14. **三个新谓词必须 `inline constexpr` 可折叠**（§1.4），否则 G1 红。

---

## 8 未决（读不出来的）

1. **spawn 的 server 进程里谁跑 applier、在哪个线程上。** 十余处 `ServerLoop::OnApplyThread()` 守卫全部取决于它；
   若 `ServerMain` 在主线程驱动 applier 而不经 `ServerLoop::Start`，`g_applyThreadKey` 恒 0，**全部守卫静默失效**。
2. **server 的退出原语是 `_exit` 还是 `exit`。** 它决定 `g_processTeardown`（`Managers.cpp:188-197`）是否被置位，
   进而决定七个 twin 析构是走早退还是真调进驱动。
3. **握手要不要断言子系统掩码。** `EsprytSlotTablesEnabled()` 每进程从 `Features.PipePush` latch，
   两个进程各自解析自己的环境，而 `ARCHITECTURE.md:488` 的 envp 剔除清单**不含** `MOBILEGL_PIPE_*`。
4. **P6 是否把任何段映射成只读。** `ShmSegment::Adopt` 已有 `Map(bool readOnly)`，但两个调用点都传 `false`。
5. **`DualBlockScenario.cpp:69-73` 断言 `serverIdentity != nullptr`**，这是一个进程内事实；
   而 G2/G14 要求 `integration-spawn` 与 `integration-split` 逐名相同。这条用例在 spawn 下怎么办。
