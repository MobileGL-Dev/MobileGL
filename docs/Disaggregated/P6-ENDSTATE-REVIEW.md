# P6 重审：面向 gfxstream 形状的终局

> 审查头 `b3b9c2ee`（`feat/disaggregated`，与 `origin` 同步）。本文是对 [`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md)
> 与 [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md) 的逐行重审，判据是一个尚未写进路线图的终局：
> **client 在别处（AVF pVM / 容器类虚拟机）产生 GL 命令流，server 在一个 Android app 里解析、执行、渲染并显示。**
>
> 本文不推翻 P6，**也不改变 P6 的形状**。它给出：P6 哪些行在终局下是错的（四行，其中两行在树上已实测为假）、
> P6 必须现在付但很便宜的几项、以及终局在路线图上的落点。所有 `file:line` 在本头解析。
> 标 **[实测]** 的是在树上核对过的；标 **[外部未验证]** 的必须先做探针再当成前提。
>
> **2026-09-21 更正（a6 收官后）**：a6 逐条复核了本文，推翻四处——`MG_State` 八站点的形状（§2.5）、
> abort 计数 93→92、`m_windowHandle` 的注入点形状、以及 `MGPipeSplitActive()` 的命名冲突。
> 正文已就地更正。a6 另发现四处本文未及的阻塞项（apply 线程在第一条记录前 abort、控制面两端都没接、
> 控制回复等待无期限、`SurfaceReply` 装不下 result）。**以 [`notes/p6/a6-audit-v1.md`](notes/p6/a6-audit-v1.md) 为准。**
>
> **2026-09-22 更正（终局改判）**：本文假设的终局——client 在 AVF pVM、链路 `AF_VSOCK`——**撤回**。现定终局：
> client 与 server 可在**不同机器、不同 OS / 架构**上，经 **TCP** 连接；传输栈拆成控制面（`ITransport`）× 数据面（`ILink`）
> 两根独立可选、握手协商、可混搭的轴（同机可控制面 TCP + 数据面共享段）。后果：§3 加一行 **D TCP**（约束集与 C 相同——
> 无 fd 传递、无共享页——但 RTT 毫秒级、带宽 30–100 MB/s）；§3.1、§7.3、§8.1、§8.2 的 vsock 条目撤回；§6 第 3 行
> "P7 不变、定宽只是协商值"改为**定宽 + 布局摘要是 P6.5 的第一个包**；"形态 B 不需要 P6.5"不再是有效场景，**P6.5 上关键路径**；
> §5.2 的 `LinkTerms` / `Refuse` / 指纹拆分经树上核实**未由 hs 落地**，归 P6.5。以 `ROADMAP.md` 的 P6.5 / Ph 行与
> `ARCHITECTURE.md` §11.9 为准；本文其余部分作为审查记录不再改。

---

## 0 一句话结论

P6 的**进程那一半**（fork/exec、envp 剔除、EOF 死亡、`ServerMain`、控制帧传输、静态量核验、整个出口门）
在终局下**几乎逐字成立**——它讲的是"两个角色在两个地址空间"，对"哪个内核"一个字都没说。
P6 的**传输那一半**（`SCM_RIGHTS` + `ShmSegment::Adopt` + 共享 `RingControl` 页）**过不了 pVM 边界**，
这没关系——**前提是 P6 在它下面留一道缝**，而当前计划一道都没有：`ITransport.h:16-20` 自己写明热路径绕开它，
`Ring.h:259` / `ReplySlot.h:117` / `EventRing.h:125-128` 直接收 `void* base + capacity`。

所以重审的结论不是"重排 P6"，而是：**P6 的形状不动，范围加五件，其中三件机械、一件是纠错。**

---

## 1 P5e 已经替终局把最贵的账付了

这一点值得单独写一段，因为它决定了终局到底可不可行。

跨 VM 边界唯一致命的不是带宽，是**每帧会合次数**。侧写记录在案（`MEASUREMENTS.md` §11）：
lockstep 下客户端每帧进入 doorbell 等待约 **916 次**（对 ~849 draw），VD32 下约 **3600 次**，
`WaitForAppliedOrEventBacklog` 占客户端 GL 线程 **31.58%** 自身时间。
**任何一个 916×/帧 的会合结构，放到一条 RTT 以 µs 计的链路上都是死的。**

P5e 的 run-ahead 把它降到**每帧一次结构性会合**（present credit），`ARCHITECTURE.md` §13.1 的零 roundtrip
清单是它的正式表述。于是链路延迟的乘数从 ~900 变成 ~1：**1 ms 的 RTT 是 16.7 ms 帧的 6%，不是 900 ms。**

> gfxstream 花了很多年才走到"生成式编码器 + 可用/已消费计数器 + 门铃抑制 + 一帧一次同步"。
> 这套形状 MobileGL 已经有了，而且是为别的理由做的。终局要抄的是 gfxstream 的**协议**，不是它的**传输**（§4）。

带宽侧的账相形之下是小事：实测持久映射线流量 364.7 KB/帧，`ARCHITECTURE.md` §14 预算 ~1 MB/帧，
60 fps 即 22–64 MB/s。**但这里有一个必须先量的缺口，见 §7。**

---

## 2 在树上实测出的四个硬洞（本次审查的最高价值产出）

这四条不是设计意见，是在 `b3b9c2ee` 上核对出来的事实，其中两条与两份 P6 文档的**明文陈述相反**。

### 2.1 [实测] X11 / Win32 窗口种类今天是**被接受**的，而终局的首要 guest 正好走这条路

`P6-CONTRACT-DRAFT` §4.3 与 §8 的 S5 认为"真窗口在 server 端具名拒绝"。实际上只拒两种：

- `WindowBackendForWireWindowKind`（`Protocol/SurfaceOpCodec.cpp:104-113`）**接受** `WindowKind::X11 → WindowBackend::X11`
  和 `Win32Hwnd → Win32`；`DecodeWireSurfaceOp`（`:141-179`）只拒 `AndroidNativeWindow` 和 `MetalLayer`。
- 解码后 `op.nativeToken()` 原样抄过，`UnpackWindowHandle`（`Server/ServerLoop.cpp:875-882`）无条件
  `HandleFromToken<void*>`，交给 backend 当 `NativeWindowType` 用。
- `DetectWindowBackend()` 是编译期 `#if` 链（`MG_Impl/EGLImpl/EGLImpl.cpp:59-70`）。

**后果**：终局的首要 guest 是一个 Linux pVM，它编译 `__linux__`、宣告 X11、送一个 XID——
一个**完全正确、毫无恶意**的 guest，做 EGL 里最普通的一次调用，就穿过了计划认为已经关上的那道拒绝，
在 server 进程里变成一次野指针解引用。今天它是潜伏的（P6 只跑离屏），终局里它在用户的 app 里。

**改法**：拒绝改成**白名单**——只接受 `Pbuffer` / `Surfaceless` / `None`，凡是命名了 client 进程窗口对象的
种类一律具名拒绝。理由也要改：不是"P12 之前不支持"，而是 **Rule G 的重述——一个含义来自对端窗口系统的值永远不过线**。
S5 随之加宽：`WindowKind::X11` 带一个像样的 XID 到达 server 必须具名变红，且门要能证伪"token 到达了 `UnpackWindowHandle`"。
今天的 S5 只测 `AndroidNativeWindow`，**洞开着它照样绿**。

### 2.2 [实测] device-lost 闩锁**不存在**

`P6-CONTRACT-DRAFT` §5.1 写的是 "The latch is already designed and **is implemented unchanged**"，
`P6-SPAWN-PLAN` §4 写的是"照抄"。两句都是假的：

- `EGL_CONTEXT_LOST` / `GL_UNKNOWN_CONTEXT_RESET` 在 `MG_Test` 之外只出现在两个枚举转字符串函数里。
- `FatalCode::DeviceLost` / `ServerCrashed` 只是生成的 schema 枚举值，**零 producer、零 consumer**。
- `glGetGraphicsResetStatus` 无条件返回 `GL_NO_ERROR`（`MG_Impl/GLImpl/Getter/GL_Getter.cpp:2855-2861`）。
- `MOBILEGL_IPC_RESPAWN` 与 `MOBILEGL_IPC_IDLE_EXIT_S` **在任何地方都没有被解析**。

**今天 server 死掉，client 会永远静默地 DECLINE 每一条 verb，`glGetError` 干净。**

这是当前计划**最大的一处尺寸错误**：它被记成了"搬运"，实际是一个完整的包。P6 必须写它，并按包计价。
它也是唯一能把 S2（`kill -9` 之后客户端能否区分"慢"与"死"）做成一个能因错误实现变红、而不只是因缺失变红的门的前提。

### 2.3 [实测] `Fatal{}` 是 `std::abort()`，`MG_Remote` 下有 92 个非测试站点

`Transport/WireLog.cpp:50`：记日志 → echo 到 stderr → `std::abort()`。

今天这完全正确：server 是 client fork 出来的子进程，一条坏记录意味着"我们自己有 bug"，崩掉是对的。
**终局里 server 是一个长期存活的普通 Android app，替用户自己装的 VM 渲染。**
此时每一个从对端字节可达的 abort 站点都是一个远程 kill——而且很多不需要恶意字节：
一个仅仅**不排空 `SEG_EVENT`** 的 guest 就能在 60 秒内用 `Fatal{EventRingOverflow}` 杀掉宿主渲染 app。

P6 该做的**不是**把它改成闩锁（那需要在每个 `[[noreturn]]` 站点发明一条真实返回路径，不是"一个函数翻一下"），
而是**收口**：一个 `Session::Fail(FatalCode, detail)`，P6 里仍然 `[[noreturn]]`、仍然 abort、日志逐字不变，
但换来三件当下就值的东西——(a) 一个地方在死之前向对端发一帧 `SessionFault{family, detail, seq, op}`，
让 guest 的日志知道是哪条记录杀了会话，而不是读到一个裸 EOF；(b) 一个遥测点；
(c) 一道 CI 普查门，禁止在收口之外新增 abort 站点。**策略翻转留给 Ph（§6）。**

### 2.4 [实测] 段尺寸由 **client 的环境变量**决定

`ConfigLoader.cpp:363-364`：`MOBILEGL_IPC_RING_MB` / `STAGE_MB` 从 **client 进程**的 env 读，量程到 1024 / 4096 MiB。
终局里这意味着**一个 guest 里的环境变量决定渲染 app 内部一次几百 MB 的分配**。

配套的另一半：`ShmSegment::Adopt` 的 `fstat`"不信对端宣告的尺寸"检查（`ShmSegmentPosix.cpp:127-133`）
是目前**唯一**的边界，而它在没有 fd 的链路上没有对应物。

**改法（P6，便宜）**：四个尺寸变成 **server 陈述**的协商字段，client 的请求可被 server 钳制并在 `Welcome` 里报告钳制后的值。

### 2.5 另外三条尺寸性事实

| 事实 | 出处 | 影响 |
|---|---|---|
| `mobilegl_server_main` **在树上不存在**，只存在于 `ARCHITECTURE.md:488-496`；唯一的 `MobileGLServer` target 是 P0 spike 桩 | 实测 | `a6` 第 5 项（server 能否不链 `MG_Impl`）必须**真去链一次**，而不是再读一遍断言。三个阶段的文档都在断言它，没人试过 |
| `MG_Config::Transport != Monolith` 有 **238 个**非测试站点：155 `MG_Backend` / 35 `MG_Impl` / 21 `MG_Remote` / 12 `MG_Pipe` / **8 `MG_State`** / 7 Config | 实测 | 此前每一次普查都漏了 `MG_State` 那 8 个。**～～它们是唯一不按目录成形的一批、同一个函数里同时问两个问题～～ a6 已推翻这个判断**：8 个里 2 个是注释，其余 6 个里 5 个是单问题测试，只有 `MipmapStorage.cpp:55-56` 一个真的同时问两个；而"同时问两个"的形状散在 `SlotAllocator` / `PipeFill` / `PipeApply` / `Managers` / `MG_Remote/Client` 的 **18 个站点**上。`a6` 仍应先分类这一族,但取样对象是那 18 个,不是 `MG_State` 的 8 个 |
| `m_windowHandle` **不止一个注入点**。a6 给出精确形状：全树 store 只有 4 处（`BackendObject.cpp:475`、`:365`、`:206`、`:207`），read 只有 5 处；`:283` 是一次**调用**不是 store；`.Width`/`.Height` **没有任何读者** | 实测 | "server 拥有显示"确实不是一处改动,但最小 latch 点比原先写的更清楚：`surfaceState->Window` 只在 `RegisterEGLWindowSurface`（`:232-238`）写一处。连同 `sameHandle` 去重键共四处,见 [`a6-audit-v1.md`](notes/p6/a6-audit-v1.md) §6 |

---

## 3 三种部署形态，以及每种真正可用的东西

终局不是一个场景，是三个，约束差别很大。

| 形态 | 边界 | 可用传输 | 可用 bulk | P6 机制是否够用 |
|---|---|---|---|---|
| **A 同内核两进程**（P6 的 spawn） | 进程 | `AF_UNIX` | `SCM_RIGHTS` + memfd/ASharedMemory + mmap | ✅ 今天就成立 |
| **B 容器类 guest**（DroidSpace / Winlator / Termux，共享内核） | 命名空间 / 沙箱 | `AF_UNIX` | 同上（含 `AHardwareBuffer_sendHandleToUnixSocket`，API 26） | ✅ **整套 P6 机制逐字可用** |
| **C AVF pVM** | 内核 | `AF_VSOCK`，**仅此** | 无。无 SCM_RIGHTS、无共享页、无 dma-buf | ❌ 需要 P6.5 |
| **D 跨机 TCP**（2026-09-22 定的终局） | 机器 / OS / 架构 | `TCP`，仅此 | 无 | ❌ 需要 P6.5；且两端是**不同二进制**，wire 定宽 + 布局摘要成硬前提 |

**这是本次审查最令人愉快的一条结论：形态 B 的终局，从"P6 + 修订后的 P12"就能到达，不需要 P6.5。**（2026-09-22：形态 B 不再是操作性场景；终局是 D，P6.5 在关键路径上。）
同内核意味着 `AF_UNIX` + memfd + `SCM_RIGHTS` 全部照旧。
Winlator 在自己 app 内跑 VirGL / Vortek 渲染服务，是"终局减去 VM"的一个**已出货存在性证明**。

### 3.1 [外部未验证] AVF 形态有一个可能致命的外部约束

> **撤回（2026-09-22）**：终局不再是 AVF，本节只作历史记录，探针不做。

审查给出的判断是：**一个普通（untrusted）Android app 根本无法参与 AVF。**
两道独立的闸，任一成立即致命：

1. `android.system.virtualmachine` 是 `@SystemApi`，`MANAGE_VIRTUAL_MACHINE` 限预装 / 平台签名（开发期可 `adb shell pm grant`）。
2. AOSP sepolicy 带 `neverallow all_untrusted_apps *:vsock_socket ~{ getattr getopt read write };`
   ——即 `socket(AF_VSOCK)` 被 MAC 拒绝，不是权限门。app 只能读写**别人递给它的** vsock fd，而那只有 VM 拥有者拿得到。

若这条成立，"在一个普通 Android app 里跑 server"对形态 A / B **为真**，对 AVF **为假**——
AVF 下 server 必须**链进 VM 启动器 app 本身**，或者由第三方桥接后走 IP 到达。

另外一条方向性事实：`VirtualMachine.connectVsock(port)` 是 **host → guest** 拨号。
所以在 AVF 形态里 **MobileGL 的 server 是 socket 的拨号方，client 是监听方**——
这正是 §5.2 要把 Role 与 Dial 拆成两根轴的直接理由。

> **这条必须先做探针再当前提。** 一小时的设备实验：从一个普通 app 调 `socket(AF_VSOCK)`，抓 avc denial。
> 它不改变任何结构，但改变终局能承诺什么。**没有人应该靠口碑给一条链路定尺寸。**

---

## 4 抄 gfxstream 的协议，不抄它的传输

gfxstream 的传输——virtio-gpu MMIO 设备、256 M–8 G 的 host memory window、`RESOURCE_MAP_BLOB`、ASG 共享环——
**每一样都要 VMM 级权限加 hypervisor memslot 配合**，一个拿不到内核的 app 永远得不到。
virgl 同理：它们 present 的方式是把一个 guest 资源变成 virtio-gpu 的 scanout。

**纯用户态、可以抄、而且 MobileGL 已经有的**：生成式 1:1 编码器 / 解码器、available/consumed 计数器、
门铃抑制、blob-vs-inline 的载体选择、fence 时间线、每上下文的 capset 协商。

这条应当写进契约作为一条禁令：**任何需要 guest 可见的 host 内存窗口、virtio-gpu context/blob、
MMIO 门铃或 DRM 设备的设计，都不在本项目的可达集内。**
同理，presentation 侧不要再用 gfxstream 类比推理——它的 present 路径我们结构上就够不着。

---

## 5 P6 的重新定范围

**判决：P6 的形状不变**——fork/execve、socketpair、`SCM_RIGHTS`、只落 pbuffer / surfaceless / 离屏。
理由：P6 昂贵且不可逆的那一半是**进程/角色**那一半，它不在乎对端是 fork 出来的孩子还是 vsock 那头的 pVM，
而且**只有一个真的第二进程能逼它诚实**。传输那一半是死路，但那可以接受——只要下面有缝。

净新增两个包（`lk`、`hs`），一个是计划本来就欠、记错了科目的（`dl`），三个已有包各加一条。

### 5.1 `lk`（新）——数据面的缝

`Transport/ILink.h` + `ShmLink`（今天的 `SessionSegments` / `Ring` / `ReplySlotPool` / `EventRing` 逐行搬到接口后面，
行为零变化）。`SessionProducer` / `SessionConsumer` / `ServerLoop` / `PipeWireEncoder` 收 `ILink*`，其余不动。

**`ITransport` 不扩、不改**——它自己的头（`ITransport.h:16-20`）把自己限定在控制面，而那个范围对 vsock 本来就是对的。

一条实测出来的反例值得记下：**不要把一个 progress 块别名到 `RingControl` 上面**。
`Ring.h:134-147` 的 watermark 那条 cache line 是 `{appliedSeq, submittedSeq, retiredSeq, completedFrameSerial, presentAckSerial}`，
`eventRingFull` / `eventDropped` 在整整一条线之后的另一个 `alignas(64)` 组里，
而且 `submittedSeq` 是 **producer 写**、其余四个是 consumer 写。
任何声称"单写者 64 字节块别名它"的设计在三个独立的点上不成立。

**改法是在 P6 里重组 `RingControl`**：把 `submittedSeq` 挪到 producer 自己那条线（`Ring.h:45-50` 写明它是诊断量、无人等待），
把 `eventRingFull` / `eventDropped` 提进 watermark 组，整组包成具名 `LinkProgress` 成员并加逐字段 `offsetof` 静态断言。
`ShmLink::Progress()` 于是直接返回共享页里的指针——自旋循环里零额外 store、零虚调用，
而 `SessionRings.h` 的每一条规则（`Watermark::Reached` 的 acquire + `>=`、`AdvanceMonotonic` 的 backwards-is-Fatal，`Ring.cpp:364-386`）
在**源码层面保持唯一实现**。

今天重组是免费的（两端同一二进制，由指纹强制）；**有了第二份构建的那天它就是一次 wire break。**

缝必须覆盖两个方向：client 把 `kSegEvent` 装到自己的映射上（`ClientSession.cpp:661`），
而 `MG_Impl/Pipe/ResourceTracker.h:606` 通过它解析 writeback payload——
那个站点也要走缝访问器，否则缝的纯度门是假的。

**门**：G1 `.text` 差异具名认领；inproc 车道结果数与逐线程 CPU 与改缝前逐字相同
（`RingProducer::Reserve` 在 ~850–3550 记录/帧上失去内联的风险在这里**实测**，不靠论证）；
外加一道机械 grep 纯度门——`RingControl` / `RingProducer` / `ReplySlotPool` / `EventRingConsumer` / `m_shm.`
只允许出现在 `Transport/ShmLink.*` 与传输自己的测试里。
red-once：任一站点绕过缝则 grep 变红；回退 `RingControl` 重组则 `offsetof` 静态断言变红。

**代价**：约一周加半天。**不做的代价**：五个包加两个阶段的重写——
P9 要在 `SEG_REPLY` 里建 slot 池、P11 要建 `SEG_ADOPT`（一块 client 写 server 读、绑 `completedFrameSerial` 的映射），
"以后再说"意味着移植三个映射形状的机制而不是一个。

### 5.2 `hs`（新）——握手身份，以及一次不是 abort 的拒绝

把 `abiFingerprint` 拆成 `wireFingerprint`（永远比较）与 `buildFingerprint`（仅在 `Dial == Fork` 时比较）。

**有一个广为流传的改法是错的**：`AbiFingerprintInputs`（`SessionRings.h:544-566`）自己的注释写明
`GIT_COMMIT_HASH_SHORT` 在里面是因为"两份 sizeof 相同的构建仍然可能对**字段顺序**有分歧，而那是任何 sizeof 看不见的"。
所以"去掉 git stamp、加一个目录 hash"**如其所述是错的**——
一个覆盖 `(opcode, payload 结构名, flags, WaitClass)` 的目录摘要，既看不见 `MGPResourceDesc` 内部的成员重排，
也看不见一次宽度变化；去掉 stamp 而不补上那道守卫，就是开了一个静默读错状态的洞。

`wireFingerprint` 因此要混入：**有序目录摘要** + **payload 布局摘要**（由 `PipeFields.def` 派生，逐成员的
name / `offsetof` / `sizeof`——`gen_pipe.py` 已经在 CI 里强制字段表点名每个成员，所以这是生成器加法，不是工具链）
+ 记录头布局 + 两个 blob codec 版本 + `kOpCount` + `MOBILEGL_ABI_VERSION` + 字节序 + 指针宽度，
**并且保留** `sizeof(MGPCaps)` 与 `sizeof(DynamicBackendParameters)`——
因为 `MGPWireRec_GetCaps` 就是由 `sizeof(MGPCaps)` 复合出来的（`PipeWire.inc:529-535`），它们是 **wire 事实**，不是构建事实。
**删掉** `sizeof(GLFunctionsTable)`：它从不过线，今天加一个 backend 槽就作废所有对端，毫无 wire 理由。

独立的另一半：握手分歧变成一条 `Refuse{code, detail, 对端的值}` 控制消息 + 两端
`MOBILEGL_ERR_PROTOCOL_MISMATCH`——server 发 Refuse 并 exit 0，client 回收子进程并**具名拒绝，不回落 monolith**。
**握手路径上不留任何 abort。** 现成的写法就在同一个函数八行之外：`ServerSession.cpp:456-466` 已经为畸形帧
返回 `MOBILEGL_ERR_PROTOCOL_MISMATCH` 并带正确的 `msg_as_Hello() != nullptr` 守卫。
顺带修一个实测出的不对称：**client 只检查 `abiFingerprint`（`ClientSession.cpp:566-568`），从不检查 `abiMajor` / `abiMinor`。**

附 `LinkTerms{dataPlane, wireForm, maxReplyBytes, cmdWindowBytes, stageWindowBytes}` 追加进 Hello / Welcome，
`dataPlane` 在 P6 里**只有一个合法值** `SharedSegments`，其余具名拒绝；四个尺寸由 **server 陈述**（§2.4）。

**注意 P6 的策略反而更严**：`buildFingerprint` 在 `Dial == Fork` 下照样比较——
fork 下子进程本来就是同一二进制，比较它能抓住一个指向旧 `libMobileGLServer.so` 的陈旧 `MOBILEGL_IPC_SERVER_PATH`，
那是 P6 真实的失败模式。**终局是一次策略翻转，不是一次重新设计。**

**门（三条 red-once，各证伪一件不同的事）**：
(1) 在临时树里交换某个 payload 结构里两个同宽成员 → 布局摘要变化而 git stamp **不变**（这是今天的指纹想红都红不了的门）；
(2) `wireMajor = 99` → 两端具名拒绝，server exit 0 不留 core，client 不回落 monolith；
(3) 两份**仅 git stamp 不同**的对端在 `Dial != Fork` 下连接成功——**正控制**，没有它，拆分可以被报成"已落地"而 stamp 比较悄悄活着。

### 5.3 `dl`（计划已欠、记错科目）——device-lost 闩锁 + fail 收口

写 §2.2 那个闩锁：会话作用域，**从 `Doorbell::Dead()` 置位，永不从超时置位**；
此后 GL 入口 no-op、`eglSwapBuffers` 返回 `EGL_FALSE` + `EGL_CONTEXT_LOST`、
`glGetGraphicsResetStatus` 返回 `GL_UNKNOWN_CONTEXT_RESET`。首次产出 `FatalCode::ServerCrashed` / `DeviceLost`。
解析 `MOBILEGL_IPC_RESPAWN` 与 `IDLE_EXIT_S`，`RESPAWN=1` 在有阶段实现重推之前是具名拒绝。
加上 §2.3 的 `Session::Fail` 收口与 CI 普查门。

**门**：S2 重新瞄准——`kill -9` server → client 必须闩锁（不挂起、不继续提交、不 abort）。
red-once：**改成从超时置位闩锁，则配对的正控制"server 落后一帧是正常态"必须变红**。
这一对正是 §5.2 要求的"慢 vs 死"判别，也是这里**唯一能证伪错误实现、而不只是证伪缺失**的门。

### 5.4 三个已有包各加一条

| 包 | 加什么 |
|---|---|
| `a6` | 第 5 项从"读"改成**真链一次**（加 CMake target，试着不链 `MG_Impl`，失败就记下真实的未定义符号表）。新增：(6) 按 Role / Dial / SplitActive 分类 238 个站点，**先做 `MG_State` 那 8 个**；(7) 普查从对端字节可达的 abort 站点（`MG_Remote` 下 92 个），它决定 `dl` 的收口是改名还是重设计；(8) 枚举 `m_windowHandle` 的**每一处**写入（§2.5）与每一个**没有分片走查**的记录类型 |
| `cp` | 窗口种类改**白名单**（§2.1）；`SurfaceReply.defaultFb` 标 `(deprecated)`——编码器硬写 0、解码器从不读，P5f/fc 已裁定 `kEventSurfaceChanged` 是唯一载体，现在烧掉这个槽以免它被复活成第二真相源 |
| `sm` | `MG_Config::Role{Monolith, Client, Server}` × `Dial{No, Fork, Connect}` 两根正交轴 + 三个具名谓词（`MGPipeSplitActive()` / `MGPipeServerArm()` / `MGPipeShouldDial()`；**注意 `MGPipeSplitActive` 与既有的 `MGPipeRoleSplitActive()` 命名冲突,后者先改名**,且三者都必须 `inline constexpr` 可折叠否则 G1 红）。反递归变成**结构性的 `Dial = No`**，而不是强置 Monolith——后者会选中 frontend glue，正是 `CONTRACT` §3.1 点名的陷阱；envp 剔除留作第二道独立保险，S3 照样证伪它。`ClientSession::Start` 拆出 `Connect(ITransport&)`：client 不再在自己线程上调 `ServerSessionInstance().Accept()`（`ClientSession.cpp:534-539`）、不再按引用 attach（`:599-600`）、不再在失败路径上关 server 单例（`:758`）、不再拿 `Welcome` 去跟本地 server 对象对账（`:577`，对任何真对端都是空检查——四个 `SegmentRef` 本来就在线上，改成**每条车道都只对线上值校验**）。`MGPipeServerArm()` 写成 `Role == Server \|\| (Role == Client && ServerLoop::OnApplyThread())`：第二个析取项是迁移路径，独立 server 出现的那天第一项点亮、线程判据处处答否，于是它是**可证的死代码**而不仅仅是没人用 |

> `MGPipeRunAheadCapBitsFor(...)` 与 `ConsumedSubsystemsFor(ActiveBackendType)` 要挪到 server 侧。
> 注意一个被多处写错的细节：**CallMask 的能力位那一半已经读 server 自己的 backend 了**
> （`MG_Backend/Init.cpp:184-211` 读 `loop.Backend()->GetBackendFunctions()`，代码里带了理由）。只有那两个函数需要搬。

### 5.5 `t6` 的出口门加三条负控制、两个必测数字

- **S6**：对端宣告 `LinkTerms.dataPlane = StreamOnly` → 具名拒绝，且 **server 活下来继续服务下一个连接**。
- **S7**：在 `Session::Fail` 之外新增一个裸 abort → 普查门变红。
- **S8**：`integration-spawn` 全程断言 `SessionFaultCount() == 0`——**好路径**上的断言，
  正是 E1 / ID-122 想要而表达不出来的那种，而且 S5 / S6 能证伪它。
- **两个数字**（都可在现有 inproc 车道上取，不需要第二个进程）：
  `SEG_STAGE` 字节/帧与**分片后**的记录数/帧；以及 server 自己的 `PipeStats` 经控制通道回传——
  `PipeStats.h:255-263` 自己写明 spawn 下 client 打印 `srv=0 srvpark=0`，
  **一个意思是"在另一个进程里"的零，长得和一个意思是"从没等过"的零一模一样**，正是本项目到处禁止的那种假绿。
- 第 8 项（socket 门铃相对 condvar 的代价）的措辞要能**接受一个负向回归而不显得可疑**：
  今天是 ~1600 次自旋/帧打在一条共享 cache line 上（99.8% 靠自旋解决），
  socket 链路没有那条 line，但把它换成 ~4 次阻塞读/帧**可能更便宜**。

---

## 6 终局在路线图上的落点

终局不是一个阶段，是四件能力，家不一样。

| | 阶段 | 内容 | 顺序约束 |
|---|---|---|---|
| 1 | **P6（修订）** | 十个包，仍然可交付，仍然只落离屏 | — |
| 2 | **P6.5（新）stream 数据面** | `StreamLink` 作为 `ILink` 的第二实现：分块封帧、watermark 变消息（**规则逐字保留**——late-never-early、`>=` 不是 `==`、倒退是 Fatal，只有机制变）、reply 变成自带 seq 的真消息（于是 **删掉** slot 池、`seq & mask` 取模、stamp 自检与 `ReplySlot.h:56-62` 的 P9 漂移风险）、`SEG_STAGE` 的编码器局部分配器变成发送窗口、**每方向一条专用读线程**（否则 event-ring 的流控死锁以 write/write 互阻的形式原样复活） | **必须在 P11 之前**，且**约束 P9 的机制选择** |
| 3 | **P7 不变** | 定宽 payload 重写是**跨架构**前提，不是跨进程前提；`hs` 让它变成一个协商出来的 `wireForm` 值而不是新机器。**2026-09-22 改判：跨架构现在就是终局，定宽 + 布局摘要归 P6.5 wf，P7 不再持有它** | — |
| 4 | **P9 重定范围** | 从"`SEG_REPLY` 异步 slot 池"改成"**异步 reply 语义**"，机制推到 P6.5 之后 | 否则 P9 造一个阶段的工作量给 stream 删掉 |
| 5 | **P11 加一条具名拒绝** | `SEG_ADOPT` 的档位是**只对共享映射成立**的；在非 shm 链路上点名 T0/T1 是具名拒绝并强制 T2 | 在 P6.5 之后 |
| 6 | **P12 修订，而且更小** | **删掉 Surface 传递那一半**（Java `Surface` → Messenger/AIDL → `MobileGLServerService` → `ANativeWindow_fromSurface`，以及那 ~15–25 MB 的额外 ART）。server app 拥有自己的 SurfaceView，client 根本没有窗口，**于是 minSdk 26 没有扁平化 `ANativeWindow` 的 NDK API——那条逼得 P5–P11 必须无窗口的约束——彻底不再成立，因为什么都不需要被扁平化。** 新增：一个追加的 `WindowKind::ServerOwned`、两个后端各一条 `CreateEGLWindowSurface` 臂、**第二个 `m_windowHandle` 注入点**（§2.5；a6 精确化为：`ActivateEGLSurface` 每次 make-current 用 `surfaceState->Window` 覆写 server 的窗口，最小 latch 点在 `RegisterEGLWindowSurface` `BackendObject.cpp:232-238`，另需改 `sameHandle` 去重键 `BackendObject_DirectGLES.cpp:962-967`）、DirectGLES 进程全局 `g_Display`/`g_Surface`/`g_Context` 的去全局化、以及 DirectGLES 的 `kEventSurfaceChanged` Width/Height 缺口（Espryt 只发布格式，`DirectGLES.cpp:15793-15805`，server 拥有显示时 client 的默认 FBO 会永远停在 `MG_Impl/Init.cpp` 的 512×512 占位值）。**Magma 近得多**：它的旋转 / preTransform 补偿本来就整个在 server 侧、按 server 自己的 surface 取键 | 形态 C 需要 P6.5 + Ph；**形态 B 两个都不需要** |
| 7 | **Ph（新）不可信 guest 加固** | 当前路线图里**没有家**。内容：把 `Session::Fail` 的**策略**翻成每会话闩锁（P6 收口站点，Ph 改它们做什么——注意这需要在每个站点发明真实返回路径，不是一个函数翻一下）；handle slot 预算（guest 能跨 13 个 kind 各逼一次 `1<<20` 条目的 vector resize，而 `BackendSlotTable` 的两条拒绝是 `MOBILEGL_ASSERT`，release 下展开为空，留下一个**静默的 null twin**）；readback 尺寸门挪到 server 侧、且在 resize **之前**加绝对上限；`StagedTextureStore` 的 resize 按该 level 宣告的范围设界；`ArchiveVector` 按 `count * sizeof(element)` 设上限；`SEG_EVENT` 溢出策略不能再是"一个仅仅在自己门铃上 park 的 guest 60 秒内杀掉宿主渲染器" | **必须在任何出货的 server app 接受一个不是它自己产生的 guest 的连接之前** |

**顺序小结**：`P6 → {P7 ∥ P6.5 ∥ Ph}`；P6.5 在 P11 与 P9 机制选择之前；Ph 在任何出货 server 之前。**2026-09-22**：IPC 跑道改为 `P6 → P6.5 → Ph → P12 → P9 → P10 → P11（同机臂）`，P7 与 P3b/P4b 在 monolith 跑道上并行。

---

## 7 本周就能做、而且最省钱的三件事

### 7.1 发布 `SEG_STAGE` 字节/帧与**分片后**记录数/帧（数小时，零新代码）

计数器已经接好了（`stage-buffer`、`stage-texture`、`stage-ubo-global`、`stage-ubo-named`、
`stage-vertex-client`、`stage-index-client`、`stage-indirect-cmd`、`pmap`、`csob-blob`）。
跑一个 MC VD12 窗口和一个 VD32 窗口，`MOBILEGL_PIPE_STATS_PERIOD=1`，发布按字节类分的那一行。

**为什么是最值钱的一小时**：`MEASUREMENTS.md:342` 明文写着分片后的逐 blob 分布**不存在**，
而 `SEG_STAGE` 大约是链路预算里其余部分的 **10 倍**——它是决定 stream 链路到底可不可行的**那一个**数字。
今天这一类唯一实测到的量是 364.7 KB/帧的持久映射流量，而 `ARCHITECTURE.md:477` **预算**是 ~1 MB/帧，
整个"26 还是 64 MB/s 可行性"的论证就卡在这两个数中间。
记录数/帧同样从没直接量过（只量过 draw/帧与头字节/帧），而它是"stream 链路的反向通道每帧要 ~1 条消息还是 ~900 条"的分母——
也就是 **P6.5 到底是一次传输练习还是一次协议重设计**。

它还可以**在 `c6` 写下第一行之前**取到，而它的答案会改变 `c6` 对 `LinkTerms` 默认窗口尺寸的写法。

### 7.2 `a6` 的链接实验（同一周）

加上 `MobileGLServer` target，试着不链 `MG_Impl` 链一次。这是 P6 本来就欠的一个硬是非题，
三个阶段的断言都没有解决它，而它的答案同时决定 **guest 侧产物能做到多小**。

### 7.3 `AF_VSOCK` 探针（一小时，设备上）

> **撤回（2026-09-22）**：终局改为 TCP 跨机；对应的测量变成 TCP loopback 与跨机臂上的 RTT / 吞吐（`ROADMAP.md` 开放问题 19）。

从一个普通 app 调 `socket(AF_VSOCK)`，抓 avc denial；量一次 guest↔host 的 1 MiB 吞吐与单向延迟。
§3.1 那条外部约束不做这个探针就不能当前提。

> **明确不要先做**的一件事：几份草案提议在门铃 notify 路径上加一个合成延迟做链路扫描。
> `SessionConsumer::ApplyOne` **每应用一条记录**就调一次 `NotifyClient()`（`SessionRings.h:465`），
> 所以 1 ms 的延迟在 VD12 上是 ~850 ms/帧，臂根本跑不完；而且 `CondVarDoorbell::Notify` 取的是
> `Park` 持有的同一把 mutex，那个 sleep 是在**串行化等待者**而不是在延迟它。
> 要扫延迟，只放在 present-credit 返回路径上（一个站点，结构上每帧一次），或者在双进程 socketpair 彩排上正经做。

---

## 8 仍然未决（不要当成已答）

1. ~~**[外部未验证]** 普通 Android app 能否成为 pVM 的 vsock 对端（§3.1）。它不改结构，改的是终局能承诺什么。~~ 撤回（2026-09-22，终局改为 TCP）。
2. ~~**[外部未验证]** pKVM 下手机上的 `AF_VSOCK` 吞吐与单向延迟。~~ 撤回（2026-09-22；换成 TCP 跨机的 RTT 与吞吐，`ROADMAP.md` 开放问题 19）。树里、文档里、脚本里对 vsock / AVF / virtio / gfxstream / virgl
   **零引用**（已 grep）；公开数字都是服务器级 x86 的 iperf3，这个配置下的延迟数字**根本不存在**。
3. server image 能否不链 `MG_Impl`。`PipeInputs.h:281/:744/:957` 与 `P5F-WIRE-COMPLETENESS` §1.4 断言它，
   `SPAWN-PLAN:77` 与 `CONTRACT` §3.3 挂着它。读不出来，只能试链（§7.2）。
4. **`appliedSeq` 能否懒传输。** 这**不是**一个开放问题，是一条 P6.5 必须**修正案**处理的已关闭契约行：
   `Ring.h:71-77` 写的是"批处理只能让 watermark 变**晚**。除 `appliedSeq` 外的五个都可以懒发布"，
   `Ring.h:52-58` 称它是"P5 **禁止**批处理的那一个 watermark"。
   几份设计拿 late-never-early 去论证懒传输合法；那条规则**明文把 `appliedSeq` 排除在外**。
   真正未决的是修正它的**代价**：修正案需要一条 flush-on-idle 规则（apply 线程将要 park 时、以及 post 一次 reply 时 flush），
   否则一帧里最后一条 `kWaitReply` 记录会把两端一起挂住；而且它把合并间隔压到全部 14 条 `kWaitReply` 加 10 条 `kWaitApplied` 行的延迟上。
   在 P6.5 的契约里用一条**专门挑起"帧内最后一条记录"情形**的 red-once 定它，不要靠推理。
5. 缝的热路径代价。`ILink::ReserveRecord` 在 VD32 下是 ~850–3550 次/帧的虚调用，
   而客户端线程已经是 ~94% 占用（16.7 ms 帧里 15.72 ms）。预期在噪声里，**但 `lk` 的门要量，不要论证**。
   具体风险是 `RingProducer::Reserve` 失去向编码器的内联。若不在噪声里，答案是把 reserve/commit 对做成
   link 自有 arena 上的非虚函数，而不是加一个开关。
6. **并发多 guest**：一 guest 一进程，还是会话索引式解析器？`ServerSession` 在四个独立的点上是单会话
   （泄漏的函数静态单例；`g_sessionOwner` 的 CAS，第二个取用者是 `Fatal{RoleViolation}`；四个 `gMGPipeCallbacks`
   进程全局；`gMGPipeSegmentResolver` 一个每进程非原子 inline 变量，拒绝第二次安装）；
   DirectGLES 额外是**每进程单 surface**。`Close()` 写得可重入，顺序重连接近可达，并发不可达。
   **显式决定它，而不是等第二个 guest 连上来的时候发现它**——注意 Android 上第二个进程很便宜、
   而且免费换来崩溃隔离，这在这里比省内存重要得多；但数量被 manifest 卡死，因为 `android:process` 名是静态的。
7. `MOBILEGL_IPC_PRESENT_CREDIT > 1` 似乎**从没量过**。`ARCHITECTURE.md:475` 给了 1..8 的量程和一段反对调高的论证，
   同时说只有实测吞吐收益能抵掉延迟代价——这暗示那个测量没做。**在一条带延迟的链路上它是首要杠杆**，
   而它的代价因此未知；叠加 Magma 的 `Present` 本来就在 `vkWaitForFences` 上等 2–3 帧，credit 2 就是端到端 4–5 帧。
   在现有 inproc 臂上取它很便宜。
8. 哪些记录类型仍然没有分片走查。在 stream 链路上，一条超出发送窗口的未分片记录是 `Fatal{RingOverrun}` 而不是一次拆分，
   所以那份枚举是选 P6.5 默认窗口尺寸的**前提**，不是后续。`a6` 第 8 项产出它。

---

## 9 `c6` 要改的契约措辞

1. §1 的 ABI 指纹行 → `hs`（§5.2）。
2. §5.1 停止声称 device-lost 闩锁已实现（§2.2）。
3. §7 停止声称 "a transport swap and nothing else"——**这句话在本次重审之后不再为真**，
   §7 要把四项新增列出来，而不是把它们夹带过去。
4. Rule G 第二句改成传输中立：
   *"Descriptors cross only by a mechanism the transport declares it supports, and where the transport declares none
   they do not cross at all and the bytes are transferred instead; a transport may not invent a third answer."*
   （当前措辞把 `SCM_RIGHTS` 硬编进了一条通用规则。）
5. 新增两条规则：**Rule H——一个 client 命名的窗口永不过线，永久**（不是"直到 P12"）；
   以及一条对 `Ring.h:71-77` 的显式**修正案登记**：`appliedSeq` 按名被排除在懒发布之外，
   任何未来的 stream 链路**修正**这条规则而不是绕着它推理。P6 不改行为，它只是让这个问题不必三个阶段之后再凭记忆重吵一遍。
6. 声明 `ILink`、`StreamLink`（未实现，每个方法是具名拒绝 `Fatal{UnmigratedDataPlane, "StreamLink@P6.5"}`）与 `LinkTerms`。
   **门**：声明而未实现的 `StreamLink` 必须能对着 `ILink` **编译通过**——
   这是"缝能不能说出一条流需要的东西（progress 传输、窗口字节、flush）"的廉价充分性证明，一行实现都不用写。
   red-once：删掉 `StreamLink` 声明需要的任意一个 `ILink` 方法，构建变红。
