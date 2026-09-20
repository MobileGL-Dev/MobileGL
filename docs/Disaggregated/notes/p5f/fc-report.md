# P5f / fc — EGL/surface 控制面帧化：落地报告

> 落地于 `p5f/fc`。设计输入：`f0-egl.md`（逐 forwarder 点名表、schema 缺口清单、帧设计草案）；
> 契约草稿 [`fc-contract-draft.md`](fc-contract-draft.md)（CONTRACT-P5F.md 本体留给集成收口）。
> 范围：P5F-WIRE-COMPLETENESS.md §2.3（P5c 审计 G4 行，原记 P6，P5f 收回）。

---

## 1 落地内容

| 文件 | 内容 |
|---|---|
| `MobileGL/MG_Remote/Protocol/protocol.fbs` | **append-only**：`SurfaceOpKind` 补 `SetSwapInterval=8` / `ReleaseResources=9` / `SetWindowHandle=10`（与 P6-SPAWN-PLAN 的预测逐字一致）；`WindowKind` 补 `MetalLayer=6`（f0-egl schema 缺口 3：两枚举不互洽的另一半）；`SurfaceOp` 补 `readSurface`、`context` 两字段（MakeCurrent 的语义单元是四元组） |
| `.../Protocol/generated/protocol_generated.h` | 用 pinned flatc（`~/w7/p5-s1-flatc-build/flatc`，25.12.19）重新生成；diff 仅 +58/−6 行，全部落在追加的枚举值与字段上 |
| `.../Server/SurfaceControlFrame.{h,cpp}` | 纯值帧：`SurfaceControlOp`（10 个 wire op + 4 个 inproc-only kind）+ `SurfaceControlFrame`（`static_assert` 钉死 trivially-copyable / standard-layout——帧里**不可能**有指针） |
| `.../Protocol/SurfaceOpCodec.{h,cpp}` | 帧 ↔ `Wire::SurfaceOp`/`SurfaceReply` 编解码；`WindowBackend` ↔ `WindowKind` 显式映射表（Surfaceless/Pbuffer 具名无后端）；wire 入口 `ServerApplyWireSurfaceOp` 的两个具名 Fatal（见 §3）；枚举对齐用 static_assert 钉死而非强转 |
| `.../Server/ServerLoop.{h,cpp}` | **单槽邮箱的载荷从"函数指针 + 栈上 void\*"换成按值携带的一个帧**。`RunOnApplyThread(ControlWork, void*)` → `RunSurfaceControlFrame(SurfaceControlFrame&)`；十二个 forwarder 的 Args::Run 本体逐字搬进成员分发 `ApplySurfaceControlFrame`（C7/ID-54 分类、N-3 遗忘、R-12 重发布全部原位保留）；阻塞握手、影子位、publish-then-ring、C2 出口块全部不动。测试缝：`RunProbeOnApplyThreadForTesting`（hook 是成员变量，**不进槽位**） |
| `MG_Test/Wire/SurfaceControlFrameTest.cpp` | 新套件 9 用例（见 §3） |
| `MG_Test/Wire/ServerLoopTest.cpp` | 既有用例 5 处迁移到 probe 缝；新增 2 用例（见 §3） |
| `MG_Test/Wire/RemoteClientTest.cpp` + `RemoteClientControls.inc` | 18 + 11 处 `RunOnApplyThread` 调用点机械迁移到 probe 缝（lambda 签名逐字兼容） |
| 根 `CMakeLists.txt` / `MG_Test/Wire/CMakeLists.txt` | 两个新源文件进 disagg 闭包；新套件按既有"加一个 NAME"约定注册 |
| 文档 | ROADMAP.md G4 行（f0-egl F9 的行号修正 + 去向改为"已由 P5f fc 帧化"）；P5F 计划进度行与 §2.3 标注；P6-CONTRACT-DRAFT §1/§4 与 P6-SPAWN-PLAN 第 4 条改为"fc 已落，P6 只剩传输"；CONTRACT-P5E 两处 ServerLoop.cpp 行号随动 |

**grep 证明**（验收项）：`grep -rn "ControlWork\|m_controlWork\|m_controlUser\|RunOnApplyThread"
MobileGL/ --include="*.h" --include="*.cpp" --include="*.inc"` → **0 命中**；邮箱槽位类型是
`SurfaceControlFrame m_controlFrame;`（按值，`ServerLoop.h:412`），其 trivially-copyable 由
`SurfaceControlFrame.h` 的 static_assert 钉死。披露：测试缝的 `m_controlProbeHook` /
`m_controlProbeUser` 是 ServerLoop 的**成员**而非槽位内容（同 `m_beforeRetireHook` 的既有先例），
只在 `ProbeForTesting` kind 的分发臂被读。

### 与 f0-egl 的逐条核对

- **9 个 forwarder / 10 种 op 帧化** ✔（含 `ReleaseCurrent` 复用：`ServerMakeEGLCurrent` 在打包侧按
  三 NO\_\* 形状拆出 `ReleaseCurrent` kind，分发到达同一个 `ApplyMakeCurrent` ClientRelease 臂）。
- **死代码不帧化** ✔（按 F8 的"标注 inproc-only"臂）：`ServerSwapEGLBuffers` / `ServerInitWindowSurface`
  与 `ServerInitCapabilities` 拿的是 **inproc-only kind，codec 拒绝编码它们**
  （`InprocOnlyOpOnTheWire`）。它们仍走帧通道是因为函数指针邮箱已整体拆除，而 G14 禁止删它们的
  钉住用例。这是与 f0-egl 字面建议的唯一偏差，理由是它自己的 F8 给的第二条路。
- **`ANativeWindow*` 具名拒绝** ✔：wire 解码入口 `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`
  （P6-CONTRACT-DRAFT 预写的名字）；inproc 路径不受影响（Android 同进程照常）。
- **`SurfaceReply.defaultFb` 不启用** ✔（f0-egl §4.4 路线冲突的二选一）：默认 FBO 形状已由
  SEG_EVENT `kEventSurfaceChanged` 承载，回复帧不回写它。契约草稿 §4。
- **发送纪律**（f0-egl §3）：`WaitForApplyBeforeEglForwarder` 原样保留；契约草稿 §2.2 写明它在
  spawn 下仍是硬要求。

## 2 双块车道：fc 名下的红

**零。** `dualblock-expected-fatals.txt` 的 6 对首阻塞全部是 `PipeInputs` 字段的 BARRIER_PULLED
读（GetFramebufferBindingSlot@ReadPixels 等），与 EGL 控制面无交集——双块机关探的是
`gPipeInputs` 的读侧，邮箱/帧通道不经过它。fc 的对应义务是**普查不变**：旋钮开的车道在 fc 前后
产生同一对集（逐门数字见 §5），棘轮文件未动。fc 自己的红一次对照见 §4。

## 3 新增测试（G14：只增不删）

`SurfaceControlFrameTest`（9）：帧是纯值聚合（trivially-copyable）；十个帧化 op 恰好有 wire
kind 且数值钉死（8/9/10 与 MetalLayer=6 是 wire ABI）；WindowBackend↔WindowKind 双向显式表
（含"两枚举整数值错位"的防强转断言）；十 op 逐个过真 `CtrlEnvelope{SurfaceOp}` 字节往返
（readSurface/context 在内）；SurfaceReply 往返（seq 回声 + 版本号）；inproc-only 与畸形帧的
编码拒绝；无后端 windowKind 的解码拒绝；**两个 fork 死亡用例**钉住 wire 入口的具名 Fatal。

`ServerLoopTest` +2：`AVoidForwarderCrossesAsOneDispatchedFrame`（无 backend 时
`ServerSetEGLSwapInterval` 仍恰好分发一帧——帧通道自己的 R-16 扳机）；
`ServerLoopEglTest.TheInitializeDisplayReplyArrivesThroughTheFrame`（major/minor 以**回复字段**
而非栈指针回到调用方，mesa 真实版本号佐证）。

既有钉住用例全部保留并经帧路径驱动：C7 的 native bind 计数（`readSurface`/`context` 字段若丢，
0x1 句柄会变 0 → ClientRelease 误分类 → NativeBindCount 不动 → 红）、N-3 的 destroy-recreate、
ID-67 的重发布计数、M-7/C2 的两个 !m_running 臂、影子位双清用例。

## 4 R-16 阴性对照（真跑过一次）

在 WSL 门树（未提交）把 `ServerSetEGLSwapInterval` 的投递行改成 `(void)frame;`（forwarder 不再
过帧通道），重链 ServerLoopTest：

- **红**：`ServerLoopTest.AVoidForwarderCrossesAsOneDispatchedFrame` Failed，失败文本正是该用例
  自己的断言——`loop.ControlFramesDispatched()` Which is: **0**，expected 1
  （"the forwarder reached the backend (or did nothing) without crossing the frame channel"）。
- 还原后同一条目转绿（`100% tests passed, 0 tests failed out of 1`）。

## 5 逐门数字（WSL 门树 `~/w7/p5f-fc`，split flavour，逐条实跑）

| 门 | 结果 |
|---|---|
| build（split flavour） | rc=0（全量 915+ TU；增量修订后最终全绿） |
| unit（旋钮关） | **2273/2273**（f1 基线 2262 + 本包新增 11：SurfaceControlFrameTest 9 + ServerLoopTest 2；G14 只增不删，无标签前缀误伤——未新增任何标签，`SurfaceControlFrameTest` 名集合与既有名无前缀关系） |
| integration-split（旋钮关） | **179/179**，零回归 |
| strict（旋钮关） | 179/179 绿、`Fatal{` 为 0、观测 Admitted 恰为既有 8 对（7 admitted + 1 escalated）。双向棘轮仍报 f1 §8.1 记录的**基线既有** 2 行 stale（`GetBoundVertexArray@DrawArrays`、`GetBufferBindingSlot@DrawArrays`）——与 fc 无关，f1 已归因基线树逐字复现，修复属 strict 车道养护 |
| 双块 census（旋钮开） | 181 条目：**155 红 / 22 绿 / 4 skip，与 f1 初版逐对逐数相同**（132/11/6/2/2/2 六对），`expect-fatal: ratchet OK - 6 fatal pair(s), zero admitted, every red entry named`。**棘轮文件零改动**——fc 名下无红（§2） |
| R-16 阴性对照 | 见 §4（改回即红、还原即绿，各真跑一次） |
| flavours | pull rc=0（915 TU，`MOBILEGL_PIPE_PUSH=1` absent ✓）、push rc=0（929 TU，=1 ✓），compile_commands.json 断言过 |
| **G1**（pull 对基线 `~/w7/p5f-base` @ e75e00cb） | `.text` 10806051 → 10806051（+0）；defined symbols 27815 → 27815，**0 added / 0 removed / 0 resized / 0 renamed**——零认定 |
| gens | gen_pipe --check/--self-test、field-ownership（fc 未动 FieldOwnership.def，仍跑）--check/--self-test、dirty-surface --check/--self-test、include 闭包 4 probes 全绿；**protocol 重生成对提交头零 diff**（pinned flatc 25.12.19）；doc 引用 --strict 恰为基线既有 2 处 ambiguous（CONTRACT-P5.md:349/:524，f1 §8.2 同此），fc 新增 0 处 |
| integration-gpu | 按任务分工由集成者跑，本包未跑 |

注：f1 §8.4 的教训已遵守——census 与其他车道串行跑，无私有日志覆写。

## 6 附带发现（未修）

1. `p5f_new.sh` 按 SHA fetch 在源仓未开 `uploadpack.allowAnySHA1InWant` 时失败
   （"couldn't find remote ref"）；fc 改用按分支名 fetch（`git fetch <gitdir> p5f/fc`）。
   建议脚本照改或源仓开 `allowTipSHA1InWant`。
2. `git submodule update --init --recursive` 在新建 worktree 上反复超时（嵌套子模块走网络）；
   fc 从同父提交的 `~/w7/p5f-f1` 树整体拷贝 submodule 工作区并改写 gitdir 指针解决。
   脚本那行 `cp -a .../glslang/External` 掩盖了这一点（它只补了 glslang 一家）。
3. `p5f_gate.sh` 的 build 步以 `[ $rc -ne 0 ] && grep ...` 收尾，成功时脚本退出码是 1
   （日志行 `build rc=0` 才是真相）。既有噪音，未动。

## 7 预期合并冲突点

- `MobileGL/MG_Test/Wire/CMakeLists.txt` 的套件清单（文件自述为"本阶段反复冲突点"，按
  "加一个 NAME" 约定解）。
- `ServerLoop.{h,cpp}` 与 `RemoteClient{Test,Controls}`：若有并发包碰 apply 线程或角色守卫
  测试（fm 的 Clear 臂不改这里；fv 若动 `RunOnApplyThread` 调用点则须平移到 probe 缝）。
- `protocol.fbs` 与 `protocol_generated.h`：若并发包也动 schema，枚举/字段追加序须人工保
  append-only 后重生成。
- `docs/Disaggregated/ROADMAP.md` G4 行、`P5F-WIRE-COMPLETENESS.md` 进度行、`P6-CONTRACT-DRAFT.md`
  §4：文档同点编辑。
