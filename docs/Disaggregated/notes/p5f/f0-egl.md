# f0-egl — P5f §2.3 EGL / surface 控制面普查（只读）

> 基线 `feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`）。路径在 `MobileGL/` 下。
> 本文件只读普查，不改任何代码。对应 P5c 审计 G4 行（`docs/Disaggregated/ROADMAP.md:71`），
> 原计划记给 P6（`MobileGL/MG_Remote/CONTRACT-P5C.md:537`），P5f 计划 §2.3
> （`docs/Disaggregated/P5F-WIRE-COMPLETENESS.md:181-186`）将其收回 P5f 的包 `fc`。

---

## 1. 单槽邮箱机制（现状）

**机制本体。** 十二个 forwarder 共享同一条过线通道：

- `ServerLoop.h:220`：`using ControlWork = MobileGLResult (*)(void* user);` —— 一个**裸函数指针**
  加一个 `void*`。注释（`ServerLoop.h:217-219`、`ServerLoop.cpp:784-787`）说明为何不是
  `std::function`：析构路径不得分配（ID-8）。
- `ServerLoop.cpp:788-793`：每个 forwarder 用同一个无捕获 trampoline
  `+[](void* user) { return static_cast<Args*>(user)->Run(); }` 和**指向调用者栈上 `Args` 的
  指针**调用 `RunOnApplyThread`。
- `ServerLoop.cpp:645-707`（`RunOnApplyThread`）：`m_callerMutex` 串行化多个投递者（:653）；
  在 `m_controlMutex` 临界区内发布 `{m_controlWork, m_controlUser, m_controlPending}` 四元组，
  影子位 `m_controlPosted` 最后发（:685-694）；发布后再敲门铃（:702-704，先发布后敲门是
  Doorbell 的硬规则）；然后阻塞在 `m_controlDone.wait`（:705），返回 `m_controlResult`（:706）。
- `ServerLoop.cpp:533-568`（`PumpControlRequest`）：apply 线程在 drain 批之间取走请求、
  执行 `work(user)`（:560）、回填结果并 notify（:561-567）。
- 泵点：`ApplyThreadMain` 主循环 :397-398 与退出前 :453；park 谓词 `ready`（:391-395）把
  `ControlIsPending()` 列为唤醒条件之一。
- 重入臂：`RunOnApplyThread:651`——apply 线程自己投递就内联执行（拆构路径
  `~BackendObject_DirectGLES` → `ReleaseEGLResources` 走这里）。
- 无线程时的行为：backend 活着而线程不在 → `Fatal{ApplyThreadNotRunning}`（:670-676）；
  backend 已不在 → 返回 `MOBILEGL_ERR_NOT_INITIALIZED`（:678-683）。

**邮箱字段**在 `ServerLoop.h:367-389`：`m_callerMutex` / `m_controlMutex` /
`m_controlPosted`（一位影子）/ `m_controlDone` / `m_controlWork` / `m_controlUser` /
`m_controlResult` / `m_controlPending` / `m_controlFinished`。

**今天为何能工作**：两个角色同进程、同地址空间。函数指针是双方都能执行的代码地址；
`&args`、`major`/`minor`、`&handle` 指向的栈帧由阻塞握手（:705 的 wait）保证在整个执行期间存活；
单槽够用是因为"verb barrier 同一时刻只留一个 client 线程可运行"（`ServerLoop.h:367-368`
注释），加上 `m_callerMutex` 兜底。跨进程后这三样全部失效：代码地址、栈地址、以及"阻塞等待
同进程另一个线程"本身。

**P5f 处置建议**：邮箱机制在 inproc 下整体保留（它是 apply 线程归属语义的一部分），但在其上
叠一层"帧化"接缝：每个 forwarder 的参数先收敛成一个**纯值结构**（见 §4 草案），inproc 臂把该
结构放在栈上传指针（现状等价），spawn 臂把同一结构编码进 `SurfaceOp` 帧。这样 fc 包不动
线程模型，只动载荷的形状。

## 2. 十二个 forwarder 逐个点名

声明在 `ServerLoop.h:459-475`，定义在 `ServerLoop.cpp:802-1033`。下表"载荷"一列标出
**值**（可直接序列化）/ **栈上指针**（指向调用者存储）/ **句柄值**（client 进程的 EGL 句柄，
跨进程无意义）。

| # | forwarder（定义行） | 签名 | 语义 | 载荷分类 |
|---|---|---|---|---|
| 1 | `ServerInitializeEGLDisplay`（`ServerLoop.cpp:802-816`） | `Bool (EGLDisplay dpy, EGLint* major, EGLint* minor)` | 初始化 server 后端的 EGL display，回写主/次版本 | dpy 句柄值；**major/minor 是指向 client 栈的 out 指针**（:805-806） |
| 2 | `ServerCreateEGLWindowSurface`（:818-834） | `Bool (EGLSurface surface, const WindowHandle& handle)` | 建窗口 surface；成功后 `ForgetCurrentTuple()`（:829，N-3） | surface 句柄值；**`&handle` 是指向 client 存储的指针**（:821/:832），且 `WindowHandle::Handle` 本身是 `void*`（`BackendObject.h:561-566`） |
| 3 | `ServerResizeEGLWindowSurface`（:836-850） | `Bool (EGLSurface, Uint32 w, Uint32 h)` | 重建 swapchain | 全为值。**最干净的一个** |
| 4 | `ServerCreateEGLPbufferSurface`（:852-870） | `Bool (EGLSurface, EGLint w, EGLint h)` | 建 pbuffer；成功后 `ForgetCurrentTuple()`（:865） | 全为值 |
| 5 | `ServerMakeEGLCurrent`（:872-918） | `Bool (EGLDisplay, EGLSurface draw, EGLSurface read, EGLContext ctx)` | 经 `ApplyMakeCurrent`（:269-316）三分支：NativeBind / RepeatNoOp / ClientRelease（ID-54）；真绑定后重发布 caps 快照（:899-913，R-12） | 四个全是句柄值 |
| 6 | `ServerSwapEGLBuffers`（:920-933） | `Bool (EGLDisplay, EGLSurface draw)` | 直接在 server 上 present | 句柄值。**无生产调用者**（见 §5 发现 F8） |
| 7 | `ServerSetEGLSwapInterval`（:935-946） | `void (Int interval)` | 设置交换间隔；返回值被丢弃（:945） | 值 |
| 8 | `ServerReleaseEGLSurface`（:948-965） | `void (EGLSurface)` | 释放 surface；`ForgetCurrentTupleIfItNames`（:960，N-3） | 句柄值 |
| 9 | `ServerReleaseEGLResources`（:967-989） | `void ()` | **按契约阻塞**（:967-973 注释：之后 `MobileGL::Destroy()` 立即走）；`ForgetCurrentTuple()`（:984） | 无参数 |
| 10 | `ServerInitCapabilities`（:991-1007） | `Bool ()` | 跑 server 端 `InitCapabilities` 并 `PublishCapsSnapshot()`（:999-1002） | 无参数 |
| 11 | `ServerInitWindowSurface`（:1009-1020） | `Bool ()` | server 端 `InitWindowSurface` | 无参数。**无任何调用者**（见 §5 发现 F8） |
| 12 | `ServerSetWindowHandle`（:1022-1033） | `void (const WindowHandle& handle)` | 把窗口句柄交给 server 后端 | **`&handle` 是指向 client 存储的指针**（:1024/:1031），内含 `void*` |

参数里的**函数指针**：每一条都是同一个 trampoline（:790-791），它承载的唯一信息是"调哪个
`Args::Run`"，即**操作的身份**。参数里的**栈上 `void*`**：`&args` 永远存在（:792），其内部再
含 client 栈指针的是 #1（major/minor）、#2 与 #12（handle）。

**今天为何能工作**：同进程；阻塞握手（`RunOnApplyThread:705`）使调用者栈帧在整个 RPC 期间
存活，所以 out 指针与引用参数安全。EGL 句柄值两边通用是因为同一个驱动在同一进程里认它们。

**P5f 处置建议（共性）**：
- 函数指针 → `SurfaceOp.kind` 枚举（操作身份显式化）。
- out 指针 → 帧的 reply 字段（#1 已被 `SurfaceReply.eglMajor/eglMinor` 覆盖）。
- `WindowHandle` → `windowKind` + `nativeToken` + `width`/`height` 值字段；`Handle` 指针在
  Android 上是 `ANativeWindow*`（`P6-SPAWN-PLAN.md:38`），**子进程无意义，真窗口到达是 P12**，
  P5f 只在 server 侧落具名拒绝 `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`
  （`P6-CONTRACT-DRAFT.md:43` 已预写这个 Fatal 名）。
- EGL 句柄值 → client 铸造的 token（`DisplayToken/SurfaceToken/ContextToken`，
  `P6-CONTRACT-DRAFT.md:41` 已裁定：client 铸造、从 1 起稠密、session 内不复用，server 持
  token → native 映射，永远不见 client 的 EGL 值）。`protocol.fbs:189-190` 的
  `display/surface: ulong` 已经就是 token 的宽度。

## 3. client 侧调用面（`BackendObject_Remote.cpp`）

九个 EGL 虚函数的路由（:196-310），核心纪律是"先转发，再跑基类"（:198-201 注释）：

- `InitializeEGLDisplay`（:203-207）→ forwarder #1。
- `CreateEGLWindowSurface`（:209-225）→ 先 #12（:214，"唯一告诉 server 窗口是哪扇的途径"），
  再 #2（:215），随后 `DrainPublishedEvents()`（:223）——surface-changed 事件走 SEG_EVENT。
- `ResizeEGLWindowSurface`（:227-234）→ #3 + drain（:232）。
- `CreateEGLPbufferSurface`（:236-243）→ #4 + drain（:241）。
- `MakeEGLCurrent`（:245-276）→ #5，成功后 `PumpControlPlane()` + drain + 刷新格式能力
  （:265-274，codex 12：把 R-12 的"第二次到达即失效"闭合在造成它的那次 make-current 上）。
- `SwapEGLBuffers`（:278-287）→ **不转发**：present 走记录（class-B `Present` emitter），
  注释明确 #6 "在 P5 没有调用者，这是刻意的"（:284-285）。
- `SetEGLSwapInterval`（:289-297）→ #7。覆写的理由：基类会调 class-C 的 `SetSwapInterval`
  槽而 Fatal（:291-295 注释）。
- `ReleaseEGLSurface`（:299-302）→ #8。
- `ReleaseEGLResources`（:304-310）→ #9，按契约阻塞。
- 另有 `InitCapabilities`（:106-142）调 #10（:125）；`InitWindowSurface`（:144-149）是
  **纯 client no-op，不调 #11**。

**每个 forwarder 调用前的 fence**：`WaitForApplyBeforeEglForwarder`（:189-193），对除
#6 外的每一次转发先 `WaitForApplyToCatchUp`。理由（:176-187 注释、CONTRACT-P5E §2.5
`CONTRACT-P5E.md:177-180`）：邮箱在 drain 批之间被泵，run-ahead 下控制请求会插进两条
在飞记录之间。client 入口侧：九个虚函数由 `MG_Impl/EGLImpl/EGLImpl.cpp` 的 EGL 入口调
（如 eglMakeCurrent → :284、eglSwapBuffers → :178、eglSwapInterval → :448、eglTerminate 路径
→ :328）。

**P5f 处置建议**：这个 fence 在 spawn 下**仍然是必要的**——socket 控制通道与 SEG_CMD 是两条
通道，控制帧一样可以超过在飞记录。fc 包的帧化必须保留"发帧前先等 `appliedSeq` 追上
`LastPublishedSeq`"这一步；它不属于 wire 格式而属于发送纪律，应写进 fc 的契约行。

## 4. schema 对照与 wire 帧设计草案

### 4.1 现有 schema（`Protocol/protocol.fbs`，已定但无人使用）

- `SurfaceOpKind`（:166-175）：`None / InitializeDisplay / CreateWindowSurface /
  CreatePbufferSurface / ResizeWindowSurface / ReleaseSurface / MakeCurrent / ReleaseCurrent`，
  共 7 个有效枚举。
- `WindowKind`（:177-184）：`None / AndroidNativeWindow / X11 / Win32Hwnd / Surfaceless /
  Pbuffer`。
- `SurfaceOp`（:186-196）：`seq, kind, display, surface, windowKind, nativeToken, width,
  height, swapInterval`。`nativeToken` 注释自述"Android transfers the window out of band"。
- `SurfaceReply`（:198-204）：`seq, ok, eglMajor, eglMinor, defaultFb`。
- 信封：`CtrlMsg` union（:273-284）已含 `SurfaceOp/SurfaceReply`；`CtrlEnvelope`（:286-288）。

### 4.2 映射表：12 forwarder → op

| forwarder | 映射 | 缺的枚举 / 字段 |
|---|---|---|
| #1 InitializeEGLDisplay | `InitializeDisplay`；reply 的 `eglMajor/eglMinor` 正是 major/minor out 参数的线形 | 无 |
| #2 CreateEGLWindowSurface | `CreateWindowSurface` + `windowKind`/`nativeToken`/`width`/`height` | 无（P12 前 server 具名拒绝 `AndroidNativeWindow`） |
| #3 ResizeEGLWindowSurface | `ResizeWindowSurface` + `width`/`height` | 无 |
| #4 CreateEGLPbufferSurface | `CreatePbufferSurface` + `width`/`height` | 无 |
| #5 MakeEGLCurrent | `MakeCurrent`；其 ClientRelease 臂（`ClassifyEglMakeCurrent`，:257-267）落 `ReleaseCurrent` | **`SurfaceOp` 缺 `readSurface` 与 `context` 两个字段**：MakeCurrent 的语义单元是 (dpy, draw, read, ctx) 四元组，schema 只有一个 `surface` 且没有 context 字段 |
| #6 SwapEGLBuffers | **不配帧**：present 走记录（`BackendObject_Remote.cpp:278-287`） | — |
| #7 SetEGLSwapInterval | 用 `swapInterval` 字段 | **缺 `SetSwapInterval` 枚举** |
| #8 ReleaseEGLSurface | `ReleaseSurface` | 无 |
| #9 ReleaseEGLResources | 无参数，阻塞 | **缺 `ReleaseResources` 枚举** |
| #10 InitCapabilities | **不配 SurfaceOp**：应答物是 `CapsSnapshot` 帧（server 已在 :999-1002 发布；`P6-CONTRACT-DRAFT.md:88-89` 同此裁定） | —（但 client 需要一个"等下一帧 CapsSnapshot"的同步点，见 4.4） |
| #11 InitWindowSurface | **不配帧**：client 侧 no-op（:144-149），无调用者 | — |
| #12 SetWindowHandle | 用 `windowKind`/`nativeToken`/`width`/`height` | **缺 `SetWindowHandle` 枚举** |

缺的三个枚举与 `P6-SPAWN-PLAN.md:76` 的预测（"预期缺 `SetSwapInterval`/`ReleaseResources`/
`SetWindowHandle`"）**逐字吻合，本普查确认**。9 个需映射的 forwarder = 7 个现有枚举 + 3 个新增
枚举 - 1 个复用（ReleaseCurrent 由 #5 的释放臂复用），与 `P6-CONTRACT-DRAFT.md:87-90` 的
"the remaining nine map onto the seven plus §1's additions" 一致。追加枚举必须 append-only
（schema 自述纪律在 :268-272 的 union 注释与 `CapsSnapshot` 的 deprecated 先例 :115-127）。

另外发现 **schema 缺的字段**（P6 计划未预写）：
1. `SurfaceOp` 缺 `readSurface`（MakeCurrent 需要 draw≠read 的场景）。
2. `SurfaceOp` 缺 `context` token 字段（MakeCurrent/ReleaseCurrent 都要指名 ctx）。
3. `WindowKind` 与 `WindowBackend`（`BackendObject.h:551-559`：Android/X11/MetalLayer/Win32）
   **不互洽**：`WindowKind` 缺 `MetalLayer`，多 `Surfaceless`/`Pbuffer`（那两个不是窗口后端）。
   需要一张显式映射表，不能按整数值直译。

### 4.3 帧设计草案（逐 forwarder）

公共规则：`seq` 由 client 单调铸造，`SurfaceReply.seq` 回声；携带 `ok=false` 等价今天的
`Bool false` 或 void 丢弃结果；server 侧答不出来等价 `ServerBackendOrNull()` 为 null
（:798）时的 `MOBILEGL_ERR_NOT_INITIALIZED`。token 化按 `P6-CONTRACT-DRAFT.md:41`。

| op 帧 | 载荷 | reply | 阻塞性 |
|---|---|---|---|
| `InitializeDisplay` | `display`(token) | `ok` + `eglMajor/eglMinor` | 阻塞（今天返回 Bool） |
| `CreateWindowSurface` | `surface`(token) + `windowKind` + `nativeToken` + `width/height` | `ok` + `defaultFb`（见 4.4 的路线冲突） | 阻塞。可并入 #12（见下） |
| `ResizeWindowSurface` | `surface` + `width/height` | `ok` + `defaultFb` | 阻塞 |
| `CreatePbufferSurface` | `surface` + `width/height`（`windowKind=Pbuffer`） | `ok` | 阻塞 |
| `MakeCurrent` | `display` + `surface`(draw) + **新字段 `readSurface`** + **新字段 `context`** | `ok` | 阻塞 |
| `ReleaseCurrent` | `display`（其余 token 为 0 合法，`P6-CONTRACT-DRAFT.md:41` 已许） | `ok` | 阻塞（今天经同一个 Bool 通道） |
| `SetSwapInterval`（新枚举） | `surface` + `swapInterval` | 可无 reply | **可改 fire-and-forget**：今天虽经阻塞邮箱但结果被丢弃（:945） |
| `ReleaseSurface` | `surface` | 可无 reply | 可 fire-and-forget（:964 丢结果）；但须保序 |
| `ReleaseResources`（新枚举） | 无 | `ok` | **必须阻塞**：`ServerLoop.cpp:967-973` 的契约，`Destroy()` 在它返回后立即走 |
| `SetWindowHandle`（新枚举） | `windowKind` + `nativeToken` + `width/height` | 可无 reply | 可 fire-and-forget，**但必须在后续 `CreateWindowSurface` 之前生效**——同一有序通道即保证 |

两个可合并项（供 fc 包定夺，本普查只摆出）：① `CreateWindowSurface` 的 schema 字段已自带
`windowKind/nativeToken/width/height`，#12 可以并进 #2，`SetWindowHandle` 枚举只在"先给窗口
后建 surface"的时序必须拆开时才需要——今天的代码就是拆开的（:214-215），保守做法是两者都
落；② `ReleaseCurrent` 也可用 `MakeCurrent` + 全 0 token 表达，现有枚举分开更好，保留。

### 4.4 三个与 schema 相邻的副作用，必须在 fc 的契约里点名

1. **caps 重发布**：#5 的真绑定臂（:899-913）和 #10（:999-1002）都会让 server 主动推一帧
   `CapsSnapshot`。跨进程后这是 server 自发帧；client 今天的吸收点（RPC 返回后
   `PumpControlPlane()`，:266-272 与 :130）天然对应"reply 之后泵控制通道"。schema 无需加
   字段，但 fc 应写明"MakeCurrent reply 到达后、下一帧 CapsSnapshot 到达前"的语义窗口。
2. **`SurfaceReply.defaultFb` 与 SEG_EVENT 是重复路线**：P5c 已把默认 FBO 形状做成
   surface-changed 事件，client 在 create/resize reply 后 `DrainPublishedEvents()`
   （:223/:232/:241）。`defaultFb` 字段（:154-160, :203）若启用是第二载体。建议 fc 二选一
   并写明，不要两个都活（`CapsSnapshot` 的 deprecated 字段教训，:101-127，就是两种拼法
   并存的代价）。
3. **server 死亡的具名化**：今天的 `Fatal{ApplyThreadNotRunning}`（:670-676）在 spawn 下
   对应 `FatalCode::ServerCrashed`（:240 已分配，`P6-CONTRACT-DRAFT.md:44` 已点名 P6 是
   第一个能产生它的阶段）。fc 的 client 侧发送路径要把"发不出去/等不到 reply"接到这个码上。

## 5. 其他发现

- **F8：两个 forwarder 没有生产调用者。** `ServerSwapEGLBuffers` 只有单测调用
  （`MG_Test/Wire/ServerLoopTest.cpp:1549`、`:1568`）；`ServerInitWindowSurface` 全树无调用者
  （client 的 `InitWindowSurface` 是纯 no-op，`BackendObject_Remote.cpp:144-149`）。
  处置建议：fc 不为它们配帧（§4.2 已按此映射）；`ServerLoop.h:469`/`:474` 的声明与
  实现在 spawn 世界只剩 inproc 测试价值，fc 可具名删除或标注 inproc-only。
- **F9：文档行号漂移。** `ROADMAP.md:71`（P5c 审计 G4 行）引 `ServerLoop.cpp:543-598`，
  当前 HEAD 的邮箱是 `:533-568`（PumpControlRequest）+ `:645-707`（RunOnApplyThread）；
  `CONTRACT-P5C.md:537` 把控制面记给 P6，P5f 计划 §2.3（`P5F-WIRE-COMPLETENESS.md:181-186`）
  已改记 P5f 的包 `fc`（`P5F-WIRE-COMPLETENESS.md:252`）。处置建议：fc 落地时顺手刷新
  ROADMAP G4 行的行号与去向。
- **F10：`ServerLoop.h:436` 与 `ServerLoop.cpp:968` 引的 `EGLImpl.cpp` 行号**（:284/:326）
  与当前 `MG_Impl/EGLImpl/EGLImpl.cpp` 基本相符（:284 是 eglMakeCurrent；ReleaseEGLResources
  实际在 :328）。仅为记录，不构成缺陷。

## 6. 结论汇总

1. 十二个 forwarder 的过线载荷里，真正"不可序列化"的只有两类：**trampoline 函数指针**
   （操作身份 → `SurfaceOp.kind`）和**三处 client 栈/堆指针**（#1 的 major/minor → reply 字段；
   #2/#12 的 `WindowHandle*` → windowKind+nativeToken 值字段，Android 的 `ANativeWindow*`
   归 P12，P5f 只落具名拒绝）。其余参数全是值或 client 句柄值（→ token）。
2. schema 缺口清点确认：**缺 3 个枚举**（SetSwapInterval / ReleaseResources /
   SetWindowHandle，与 P6-SPAWN-PLAN 的预测逐字一致）+ **缺 2 个字段**（MakeCurrent 的
   readSurface 与 context token）+ WindowKind/WindowBackend 需要显式映射表。
3. #6、#10、#11 不配 SurfaceOp 帧：present 走记录、InitCapabilities 走 CapsSnapshot、
   InitWindowSurface 无调用者。需要帧化的是 9 个 forwarder、10 种 op（含 ReleaseCurrent）。
