# P6.5 — 传输栈两轴化 + 跨机（第一波已落地，未宣布收官）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 终局改判（2026-09-22）：client 与 server 可在不同机器、不同 OS / 架构上经 TCP 连接；同机 `spawn` 保留。传输栈拆成两根独立可选、握手协商、可混搭的轴——控制面 `ITransport`（`fork` / `unix:<path>` / `tcp://host:port`）× 数据面 `ILink`（`ShmLink` / `StreamLink`）。设计摘要见 [`ARCHITECTURE.md`](../../ARCHITECTURE.md) §11.9，契约 `MobileGL/MG_Remote/CONTRACT-P65.md`。
- 第一波（`feat/disaggregated@fe28bdb5`，2026-09-22）：全 TCP 控制 + StreamLink 数据，WSL client ↔ Redmi server。包 lk2 → wf（wire 定宽 + `wireFingerprint` + `Refuse`）→ ct（tcp 端点 + keepalive + 令牌）→ sl（`StreamLink`）→ lf（日志前送）→ sv（supervisor + 前台 Service）→ t65。
- 验收：主机 unit 2399、integration-tcp loopback 102/102、**Redmi device-102 102/102 全 run-ahead ARMED**、代表性 retrace OpenRA 1.0；G1 0/0/0/0；真实远端 kill 133 ms、真实 Wi-Fi 中断 5053 ms 闩住 device loss。四切片代码审查无硬缺陷 + 两修复。
- **残余（CI / 后续）**：完整 39 例 split 子集 device golden 矩阵与链路必测数（逐帧 `kWaitReply` / RTT、`SEG_STAGE` 字节/帧、`PRESENT_CREDIT` 1/2/3、loopback tcp-vs-spawn 逐线程 CPU）。第二波（同机 TCP 控制 + 共享段数据、nd `auto` / sd）未开工。
- 证据总索引 [`evidence-index.md`](evidence-index.md)。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P6.5** 传输栈两轴化 + 跨机
- **状态**：**第一波已落地**（`feat/disaggregated@fe28bdb5`，2026-09-22）：全 TCP 控制 + StreamLink 数据，WSL client ↔ Redmi server。四切片代码审查无硬缺陷 + 两修复（`AcceptPair` 重试 `EINTR`/`ECONNABORTED`、supervisor 自设 `MOBILEGL_IPC_ROLE=server`，均 `MG_Remote` 门内 G1 不动，见 [`code-review-findings.md`](code-review-findings.md)）；固定制品（两端 stamp `p65-acceptfix-20260922`）再验收：unit 2399、integration-tcp loopback 102、**device-102 102 全 ARMED**、代表性 retrace OpenRA 1.0 全 ARMED。**残余（CI/后续）**：完整 39 例 device golden 矩阵 + 必测数。设计见本表下方「立即的下一项」节
- **落地什么 / 范围**：包：**wf** wire 定宽与布局摘要——`wireFingerprint` = `PipeFields.def` 派生的逐成员 name / `offsetof` / `sizeof` 摘要 + 有序目录摘要 + 记录头布局 + 两个 blob codec 版本 + `kOpCount` + 字节序 / 指针宽度，**保留** `sizeof(MGPCaps)` / `sizeof(DynamicBackendParameters)`（wire 事实）、删 `sizeof(GLFunctionsTable)`；`buildFingerprint` 只在 `Dial == Fork` 下比较；`DynamicBackendParameters` / `MGPCaps` / `RenderStateParameters` 整块 blob 的定宽化（两端是不同编译器产的不同二进制，`long` / padding / 位域差异都是静默读错状态）；`Refuse{code, detail, 对端值}` 控制消息，握手路径零 abort → **ct** 控制面端点种类（`fork` 继承 fd / `unix:<path>` / `tcp://host:port`；`DialMode{No, Fork, Connect}` 与 `MGPipeShouldDial()` 按契约 §3 补齐；TCP keepalive 失败**作为传输层报告的挂断**进 dl 的闩，不是 apply 超时）→ **nd** 数据面协商（`LinkTerms{dataPlane, delivery, wireForm, maxReplyBytes, cmdWindowBytes, stageWindowBytes}` 入 Hello / Welcome，四个尺寸由 **server** 陈述；`dataPlane ∈ {SharedSegments, Stream}`；`auto` 的判据是**段实际交付成功**而不是地址判断，回落必须具名入日志与 stats；链路工厂在会话建立时选定一对，之后不变——缝是 setup-time 的，契约 §8.2）→ **sd** 共享段交付与控制面解耦（`ShmLink` 自带 AF_UNIX aux 汇合名、在 `Welcome` 里公布，`SCM_RIGHTS` 只走它；于是**控制面 TCP + 数据面共享段**在同机是合法配对；`ITransport` 收窄为纯控制帧，描述符传递归数据面）→ **sl** `StreamLink`（分块封帧；watermark 变消息且**规则逐字保留**——late-never-early、`>=` 不是 `==`、倒退是 Fatal；`appliedSeq` 修正案 + flush-on-idle；reply 变自带 seq 的消息，本链路上删 slot 池 / 取模 / stamp 自检；`SEG_STAGE` 变发送窗口，三个 cursor 要么完整复活要么不用、不留中间态；program archive 与 `draw_vbo` range 尾分片；`GetCaps` 两个 blobref 变消息；每方向每进程一条专用读线程）→ **t65** 车道。上层纯度：`SessionProducer` / `SessionConsumer` / `ServerLoop` / `PipeWireEncoder`、`ClientSession.cpp` 的 `kSegEvent` 挂载与 `ResourceTracker.h` 的 writeback 解析都只经 `ILink`。设计 `ARCHITECTURE.md` §11.9
- **验收门 / 证据**：门：inproc / spawn 车道结果数与逐线程 CPU 逐字不变（`ILink::ReserveRecord` 虚调用代价**实测**，`RingProducer::Reserve` 失去内联是具体风险）；纯度 grep——`ShmLink` / `StreamLink` / `RingControl` / `ReplySlotPool` / `EventRingConsumer` / `m_shm.` 只出现在 `Transport/` 与其测试，上层零链路种类分支；**2×2 矩阵车道**（控制面 {unix, tcp} × 数据面 {shm, stream}，loopback）名集合与 spawn 一致且同数绿，每条目记 `control=… data=…` 的 arm 证明（ID-124 的形状）；跨机臂一条真 trace 逐像素；wf 三条 red-once（临时树里同宽成员互换 → 摘要变而 stamp 不变；`wireMajor = 99` → 两端具名拒绝、server exit 0、client 不回落 monolith；仅 stamp 不同的对端在 `Dial != Fork` 下连接成功——正控制）；**必测数**：逐帧 `kWaitReply` 次数（`ResourceCreate` / `SetTextureParams` / 纹理半边 `ResourceSubData` 都在此列，TCP 上每次即一个 RTT）、`SEG_STAGE` 字节/帧对链路带宽、`PRESENT_CREDIT` 1..3 配对、混搭（tcp + shm）对（unix + shm）的逐线程 CPU 差

## 终局改判（原 `ROADMAP.md` 页首注记）

> **终局（2026-09-22 改判）**：client 与 server 可以在**不同机器、不同 OS / 架构**上，经 **TCP** 连接；同机的 `spawn`（AF_UNIX + 共享段）保留为本机形态。传输栈拆成**两根独立可选的轴**——控制面（`ITransport`：`fork` 继承 fd / `unix:<path>` / `tcp://host:port`）与数据面（`ILink`：共享段 `ShmLink` / 字节流 `StreamLink`）——由握手协商、可以混搭（同机可以控制面走 TCP、数据面走共享段以降开销），上层只见 `ITransport` + `ILink`，**不得按链路种类分支**。此前 [`P6-ENDSTATE-REVIEW.md`](../p6/P6-ENDSTATE-REVIEW.md) 假设的 AVF pVM / `AF_VSOCK` 终局撤回（该文顶部有更正）。这一改判把 **P6.5 从条件项变成 IPC 跑道的关键路径**，把 wire 定宽 + 布局摘要从 P7 / hs 的"以后"变成 P6.5 的第一个包，把 Ph（含配对 / 认证）变成 P12 非 loopback 监听的前提；设计见 `ARCHITECTURE.md` §11.9。

## 第一波验收快照（原 `CURRENT_STAGE_PROGRESS.md`，2026-09-22）

实现、代码审查与主要正确性门已完成；**已落地并推送 `feat/disaggregated@fe28bdb5`（2026-09-22）**（原
在 `codex/p65-all-tcp` 的 WIP + 本次审查修复 + 收尾文档一并合入）。完整 39 例 device golden 矩阵与全套
链路必测数按用户「本地代表性、CI 兜底」留 CI/后续，**尚未宣布阶段收官**。

- TCP 控制、ShmLink/StreamLink 数据面、布局指纹、portable program archive v2、令牌/Refuse、
  设备 supervisor/前台服务、日志前送与有序关闭均已接线。
- 初始 caps 到达前误锁为 lockstep 的启动竞态已修复。READY7 主机 TCP **102/102**、
  Redmi TCP **102/102**，零 skip；两臂逐例确认 **run-ahead ARMED**，无 fallback/demotion。
  保留车道 spawn **102/102**、inproc **186/186**，零 skip。
- **代码审查（四并行切片：数据面 StreamLink / wire 定宽 / 控制面握手）无硬正确性缺陷**；两处修复已落地——
  `SocketTransport::AcceptPair` 重试 `EINTR`/`ECONNABORTED`（Medium，防常驻 server 因 accept 瞬态 `return 73` 整体退出）、
  `mobilegl_server_main` 自设 `MOBILEGL_IPC_ROLE=server`（Low，不再盲信 launcher）；两处均在 `MG_Remote/`
  （`MOBILEGL_BUILD_DISAGGREGATED` 门内），G1 monolith `.text` 构造性不受影响。其余为跟进项（`MobileGLServerService.onDestroy`
  子进程泄漏→P12、`gen_pipe.py` 裸 `char` 符号性普查无实活风险、测试缺口等），见
  [`code-review-findings.md`](code-review-findings.md)。
- **修后固定制品再验收**（WSL client 与设备 APK 同 build stamp `p65-acceptfix-20260922`，`REQUIRE_SAME_BUILD=1` 真实通过）：
  主机 full unit **2399/2399**、P6.5 子集 **147/147**、integration-tcp loopback **102/102**、
  **device-102 102/102，全 102 条 client 日志 `run-ahead ARMED`**、代表性 device retrace
  OpenRA SSIM **1.0** / startup **0.999999511** / in-world **0.999995438**，三条全 **ARMED**。
- G1 符号 **0/0/0/0**，`.text` 逐字一致；真实远端 kill **133 ms**、真实 Wi-Fi 中断 **5053 ms** 闩住 device loss。
- 原有单角色日志读取的 CI 缺陷已与上游修复合并；模拟器掉线分类另有回归验证。
- **残余（CI/后续）**：正式 retrace 的完整 **39 项** split 子集 device 矩阵（DirectGLES 现有阈值、OpenRA 1.0；
  `ci:false` 的 rd12 单列）与链路必测数（逐帧 `kWaitReply`/RTT、`SEG_STAGE` 字节/帧、`PRESENT_CREDIT` 1/2/3、
  loopback tcp-vs-spawn 逐线程 CPU）。`minecraft-1.21.11-main-menu` 的 Redmi 32B UBO 对齐 golden 伪差为
  ci-false（设备实图 SSIM 0.998729），不作本波红。跑器 `tools/trace_replay/run_tcp_matrix.py`（支持进度超时、
  进程组清理、实际 run-ahead 证明与可核验 checkpoint）已入库。

契约见 [`CONTRACT-P65.md`](../../../../MobileGL/MG_Remote/CONTRACT-P65.md)，
制品身份与证据总索引见 [`evidence-index.md`](evidence-index.md)；
具体结果与未决项见 [`validation-status.md`](validation-status.md)、
[`wire-and-validation.md`](wire-and-validation.md)、
[`performance.md`](performance.md)。以下保留 P6 收官时的原始进度快照。

## 第一波设计与操作手册（原 `ROADMAP.md`「立即的下一项」节）

> 状态：**已实现并落地推送 `feat/disaggregated@fe28bdb5`（2026-09-22），代码审查完成（无硬缺陷 + 两修复），固定制品代表性再验收通过。** 主机 unit 2399、integration-tcp loopback 102、Redmi **device-102 102 全 ARMED**、代表性 retrace OpenRA 1.0 全 ARMED；G1 与真实断线门通过。完整 39 例 device golden 矩阵与余下链路必测数按「本地代表性、CI 兜底」留 CI/后续，**尚未宣布阶段收官**；逐项见 [`CURRENT_STAGE_PROGRESS.md`](../../CURRENT_STAGE_PROGRESS.md) 与 [`notes/p65/`](.)。目标形态：x86_64 的 WSL 进程作为 client，Redmi `2f7cbe2e`（aarch64，Adreno）上的 server 进程作为 server，两者在同一局域网内经 **TCP** 连接；`integration` 全部用例与 CI retrace 的 split 子集在这条链路上跑通并对上 golden。本节是 P6.5 行的第一波，只做「控制面 TCP + 数据面 stream」这一对；同机的「TCP 控制 + 共享段数据」混搭（nd `auto` / sd）是第二波，本节不做。

### 1 设计时的树上基线（实现前快照）

以下表格保留本波开工前的缺口定位；当前实现与契约更正见 `MG_Remote/CONTRACT-P65.md` 和阶段进度，不以历史行号描述今天的树。

| 已有 | 出处 | 对本波的意义 |
|---|---|---|
| 控制面 `SocketTransport`：`Listen` / `AcceptPair` / `ConnectTo`，**汇合点是一个名字**，两条连接按序接受（第一条控制、第二条 aux）；`auxFd = -1` 已是「这条链不传 fd」的诚实答案 | `SocketTransport.h:70-91` | TCP 只是第三种名字（`tcp://host:port`）；两条连接的规矩原样用：第一条控制，**第二条从 aux 变成数据流** |
| 帧：`[u32 'MGLF'][u32 len][payload]`，`FrameReader` 是真正的字节流重组器 | `Framing.h` | 数据连接复用同一重组器，只换 magic |
| server 主程序：endpoint 来自 `argv[1]` / `MOBILEGL_IPC_ENDPOINT`；`Listen` → `AcceptPair` → 建 backend → `Accept`（Welcome + 七个描述符）→ 控制泵，**对端 EOF 即退出**；`MOBILEGL_IPC_DIAL=no` 反递归 | `ServerMain.cpp`（门 8 的收尾修改已落入该文件，本表不引行号） | 单会话进程形状保留为 **child**；设备侧要加一个 **supervisor** 循环 |
| client：`StartOverSocket(transport)` 已把「socket 怎么来的」与握手分开；spawn 路径 = 造 `@mgl-<pid>-<this>` 名 → `LaunchServer` → `ConnectTo` | `ClientSession.cpp:693-724` | `Dial = Connect` 只是**跳过 `LaunchServer`**、把名字换成 `tcp://` |
| 握手后 client 收 **7 个 fd**（4 段 + 3 个门铃）再 `AdoptFromDescriptors` | `ClientSession.cpp:832-897` | stream 上这一段整体不存在：段是本地私有的，门铃是本地条件变量 |
| **数据面没有缝**：`ILink` 只有声明，树里用到它的只有 `LinkProgress`（`Ring.h:97`）；`ShmLink` 不存在；两端 session 直接持有 `SessionSegments`（`SessionRings.h:130`）；`ServerSession.h:93` 自己写着「`lk` 把门铃挪到 `ILink` 后面并删掉这些访问器」——**没做** | `Ring.h:108`（`RingControl` 重组已做）、`ILink.h`、`StreamLink.h` | 重审 §5.1 那「约一周」的 lk 主体仍然欠着，是本波**第一个包**，没有它 `StreamLink` 无处可插 |
| wire 寻址：`MGPBlobRef{Offset, Size, Seg}` 只允许 `kSegStage`，解析集中在 `ResolveOrFatal` | `PipeWireCodec.cpp:1421`（拒绝其它段：`:496`、`:1401`） | **两端各建一个同尺寸的本地 stage 窗口、偏移一致，codec 一行不改** |
| reply：`ReplySlotHeader{Seq, Status, Size}` 16 字节、`seq % slotCount`（8 槽） | `ReplySlot.h` | 头已经是消息形状；stream 上删掉取模与 stamp 自检 |
| event：四种 kind、定宽头 | `EventRing.h` | 已是 wire 形状，原样装帧 |
| 指纹：`CapsAbiFingerprint()` = 三个 `sizeof` + 协议版本 + git stamp | `protocol.fbs` `table Hello` 的注释 | x86_64 ↔ aarch64 两份二进制：**必须换成布局摘要**，否则要么永远拒绝（stamp 不同）要么静默读错 |
| 日志按角色分文件；server 角色由启动方设的 `MOBILEGL_IPC_ROLE=server` 决定；`LogLine` 已在 `CtrlMsg` 里、**零生产者** | `ServerSpawn.cpp:116`、`ClientSession.cpp:1948` | 34 个读 `<base>.server.log` 的消费方在跨机时读不到 → 日志前送（§3.5） |
| 车道：`mgl_itest_register_split_arms` 一个宏两条臂 {Split, Spawn}，环境差只有 `MOBILEGL_TRANSPORT=spawn` + `MOBILEGL_IPC_SERVER_PATH` | `MG_IntegrationTest/CMakeLists.txt:1985-2010` | 加第三条臂 `Tcp`；名集合门（`scripts/ci/spawn_lane_parity.py`）与 arm 证明（`scripts/ci/junit_tally.py:27`）各加一臂 |
| retrace：桌面经 `MOBILEGL_TRANSPORT` / `MOBILEGL_IPC_SERVER_PATH` 环境（`run_trace_case.cmake:47-52`）；设备经 `--env K=V;…` intent extra，server 路径从 `nativeLibraryDir` 解析（`TraceReplayActivity.java:374-386`）；**桌面与设备共用同一套 golden**（`trace_cases.json`），驱动差异由 SSIM 阈值吸收 | — | 跨机 retrace 的 golden 就是现有的，不另造 |
| 设备已能从 app 进程 exec `libMobileGLServer.so`（spike A、`spawn-acceptance` 臂） | `apk.yml:477-487` | supervisor 就是这个可执行文件加一个 `--serve` |
| manifest **没有 `INTERNET` 权限** | `android-plugin/app/src/main/AndroidManifest.xml` | server 要监听 TCP，trace flavour 必须加 |

### 2 拓扑：谁监听、谁拨号、为什么

server 在手机上**监听**，client 从 WSL **拨出**。理由不是偏好：WSL2 默认 NAT，出站到局域网直通、入站要 portproxy；手机侧无防火墙。`Role × Dial` 两轴（契约 §3）因此在本波是 `Role=Client, Dial=Connect` / `Role=Server, Dial=No`。备用形态：`adb forward tcp:40613 tcp:40613` 把 server 绑在手机 loopback、client 连 WSL 内 adb 的 loopback——**要求 adb 跑在 WSL 里**（adb-over-Wi-Fi：`adb connect <phone>:5555`），因为 Windows 侧 adb 绑的 127.0.0.1 WSL2 够不着；路由器开了 AP 隔离时用它。

### 3 机制

**3.1 端点与握手（包 ct）**。`SocketTransport::Listen/ConnectTo` 识别 `tcp://host:port`（其余仍是 AF_UNIX 路径）；两条 TCP 连接按序：控制、数据。socket 选项：`TCP_NODELAY`（批处理由我们的 publish 点决定，不交给 Nagle）、`SO_KEEPALIVE` + `TCP_KEEPIDLE=2s / KEEPINTVL=1s / KEEPCNT=3` + `TCP_USER_TIMEOUT=5000ms`——Wi-Fi 掉线不产生 FIN，这是唯一能把「半开」变成挂断的机制；它以 `POLLHUP`/错误进入 `Doorbell::Dead()`，走 dl 的闩，**是传输层报告的挂断，不是 apply 超时**（开放问题 23）。`Hello` 追加 `linkTerms{dataPlane=Stream, wireForm, maxReplyBytes, cmdWindowBytes, stageWindowBytes}` 与 `token`；`Welcome` 回填 server 陈述的四个尺寸；新增 `Refuse{code, detail, 对端值}`（`CtrlMsg` 只追加）。`buildFingerprint`（git stamp）比较策略：`Dial=Fork` 不匹配 → Refuse（抓陈旧 `MOBILEGL_IPC_SERVER_PATH`，不变）；`Dial=Connect` 不匹配 → **警告**，`MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` 时 → Refuse（车道设它：APK 与 WSL 构建必须同一 commit）。令牌：`MOBILEGL_IPC_TOKEN` 两端一致；server 无令牌时**只允许绑 loopback**，错令牌 → Refuse 并记日志。

**3.2 wire 定宽与布局摘要（包 wf）**。`wireFingerprint` = `gen_pipe.py` 从 `PipeFields.def` 生成的逐成员 `{name, offsetof, sizeof}` 表的摘要 + 有序目录摘要（opcode / payload 名 / flags / WaitClass）+ 记录头布局 + `MGPipeRenderStateSpans` 的 offsetof 表 + 两个 blob codec 版本 + `kOpCount` + `MOBILEGL_ABI_VERSION` + 字节序 + 指针宽度 + `sizeof(MGPCaps)` / `sizeof(DynamicBackendParameters)`；**删** `sizeof(GLFunctionsTable)`。x86_64 ↔ aarch64 都是 LP64、小端、Itanium ABI，布局大概率一致，但**由摘要断言、不由这句话断言**；已知的真实差异是 **`char` 的符号性**（aarch64 默认无符号）——wf 普查 payload 与 blob 里裸 `char` 当数用的站点。`DynamicBackendParameters` 定宽重写（从 P7 挪来）与 `RenderStateParameters` 1168 字节整块的成员表在此包。三条 red-once 见 P6.5 行。

**3.3 缝的主体（包 lk2，零行为变化）**。`SessionSegments` + `RingProducer/Consumer` + `ReplySlotPool` + `EventRingProducer/Consumer` + 两个门铃搬到 `ShmLink : ILink` 后面；`SessionProducer` / `SessionConsumer` / `ServerLoop` / `PipeWireEncoder` / `ClientSession.cpp` 的 `kSegEvent` 挂载 / `ResourceTracker.h` 的 writeback 解析都改收 `ILink*`；`ServerSession.h:93` 说要删的访问器删掉。门：inproc / spawn 车道结果数与逐线程 CPU **逐字不变**（`ILink::ReserveRecord` 的虚调用在 ~850–3550 次/帧上实测；若不在噪声里，答案是把 reserve/commit 做成 link 自有 arena 上的非虚函数）；纯度 grep 门上线。

**3.4 `StreamLink`（包 sl）——被消息复制的镜像段**。设计原则一句话：**两端各自持有一份同尺寸的私有段，链路只负责把字节搬到对面同一偏移；codec、watermark 规则、apply 线程、编码器全部不变。**
- 段：握手协商后两端各 `malloc` 四块：cmd 环（`cmdWindowBytes`）、stage 窗口（`stageWindowBytes`）、event 环、reply 邮箱。尺寸相同 ⇒ 偏移相同 ⇒ `MGPBlobRef` 与 `ResolveOrFatal` 原样工作。
- 数据连接帧：`[u32 'MGLD'][u32 len][u8 kind][u8×3][u64 a][u64 b][payload]`。C→S：`Cmd{ringOffset, bytes}`（记录字节逐字、写到对面环的同一偏移）、`Stage{offset, bytes}`、`ClientProgress{cmdHead, submittedSeq, eventTail}`。S→C：`Progress{appliedSeq, retiredSeq, completedFrameSerial, presentAckSerial, cmdAppliedTail, eventRingFull}`、`Reply{seq, status, bytes}`（头即 `ReplySlotHeader`）、`Event{ringOffset, bytes}`。
- client 侧：编码器照旧写本地 cmd 环与 stage 窗口；`Flush()` 把 `[上次已送, cmdHead)` 作为一帧 `Cmd` 发出（环回绕用两段 iov 一次 `sendmsg`），**之前先发本次新 stage 的 `Stage` 帧**——顺序由编码器「先 stage 后 reserve」保证；`LinkCapabilities.PublishIsDelivery=false`，所以每个等待点先 `Flush`（这个位就是为此设的）。C→S 背压：`WaitForCmdSpace` 不变（读 `Progress` 里的 `cmdAppliedTail`），`send()` 阻塞是第二层，没有无界缓冲。
- server 侧：`mgl-srv-io` 读线程把 `Cmd`/`Stage` 拷到镜像段同偏移，再推进本地 `cmdHead`/`submittedSeq` 并敲 apply 的本地门铃；**apply 线程一行不改**。
- `Progress` S→C 的发出时机（对 `Ring.h` 「`appliedSeq` 不得懒发布」的**修正案**，规则改成「最多晚一次 flush，绝不早」）：(a) apply 线程**即将 park 前**（flush-on-idle，解决「帧内最后一条 `kWaitReply` 记录两端互等」）；(b) 每次 `PostReply` 之后；(c) present ack / `completedFrameSerial` 变化时；(d) 每 64 条已 apply 记录或 1 ms 取先到者。接收侧对每个 watermark 做 `AdvanceMonotonic`，**倒退是 `Fatal{ProtocolCorruption}`**。
- reply：`kWaitReply` 下同一时刻最多一条未决，client 侧是「单槽 + 条件变量 + seq 核对」；本链路上 slot 池、取模、stamp 自检**删除**（`ShmLink` 保留）。
- event：server 的 `EventRingProducer` 写本地环，io 线程把新增字节装 `Event` 帧；client 读线程写进 client 本地环同偏移，`EventRingConsumer` 不变；`Drained()` 发 `ClientProgress{eventTail}`；server producer 在 `head − 对端 tail > capacity` 时**阻塞**（P5e 的规则原样，`eventRingFull` 经 `Progress` 回传）。
- 门铃：`ConsumerBell()` / `ProducerBell()` 是本地 `CondVarDoorbell`，由各自的读线程敲；读线程见 EOF / 错误置 `Dead()`。每进程数据连接**一条**专用读线程（写在调用线程上），控制连接的读法不变。
- 帧上限：`Cmd`/`Stage` 单帧 ≤ `kMaxFramePayloadSize`（64 MiB）；一条记录本来 ≤ 4 MiB。未分片的 program archive / `draw_vbo` range 尾在 stream 上和今天一样受 `stageWindowBytes` 约束，超出仍是 `Fatal{RingOverrun}`——**不是本波阻塞项**，记在开放问题 11。
- 不做的：`kSegShadow` / `kSegAdopt`（P8 / P11 的段）在 stream 上具名拒绝；`GetCaps` 的 blobref 若仍走 stage 之外 → 具名拒绝，本波不发明第三条载体。

**3.5 日志前送（包 lf）**。server 进程的日志 sink 在 `MOBILEGL_IPC_ROLE=server` 且有控制传输且 `MOBILEGL_IPC_LOG_FORWARD=1`（`Dial=Connect` 默认开）时，每行**同时**写本地 `<base>.server.log` 并作为一帧 `LogLine`（已在 `CtrlMsg` 里）经控制连接发出；client 收到后写入**自己的** `<base>.server.log`。于是 34 个消费方（`PipeStatsWindow` 的计数器窗口、四个 arming 场景、拒绝普查）**一字不改**。顺序：读者在 `MarkLaneLog` / `ReadLaneLogSince` 之前调一个导出的 `MGPipeSyncPeerLog()`（一次控制往返：`LogFlush` → server 冲刷并 ack），远端不存在时它是 no-op；导出符号一个，`-fvisibility=hidden` 下显式 `default`。若 lf 滑期，退路是这 34 处在 tcp 臂上**按名 SKIP**（清单文件，像 `dualblock-expected-fatals.txt`），不许静默通过。stats 行一帧一条，前送代价可忽略。

**3.6 设备侧 supervisor 与启动（包 sv）**。`mobilegl_server_main tcp://0.0.0.0:40613 --serve [--max-sessions 1]`：listen → 循环 { `AcceptPair`（控制 + 数据）→ `fork()` → child 关掉 listen fd、用 `SocketTransport(streamFd, dataFd)` 走今天的单会话路径到 `_exit`；parent 关掉已接受的 fd、非阻塞 `waitpid`、继续 accept }。每会话一进程 ⇒ `eglTerminate` 的进程级拆机被隔离（D9 原样）；同时只服务 1 个会话，第二个连接 → `Refuse{Busy}`。启动：trace flavour 新增前台 `MobileGLServerService`（`android:process=":mglsrv"`），`am start-foreground-service` 带 `--es listen`、`--es token`、`--es env "K=V;…"`（复用 `--env` 的 `K=V;` 语法），它从 `nativeLibraryDir` exec `libMobileGLServer.so`（spike A 的路径）、给 child 设 `MOBILEGL_IPC_ROLE=server`、持 partial wake lock；`am force-stop` 整包停掉。后端由 client 的 `Hello.backendType` 请求、supervisor 的 env 可钉死，不一致 → Refuse。EGL 在 child 里是 pbuffer / surfaceless（P6 边界；真窗口归 P12）。manifest 加 `INTERNET`。

**3.7 谓词与测试的拓扑感知**。client 进程：`Transport=Spawn`（拓扑：两进程）、`Dial=Connect`、不 fork；`MGPipeBackendIsLocal()=false`。`CountOwnChildren()` 一类断言改为按 Dial 分支：Connect 下断言**本地零子进程** + `Welcome::serverPid` 非零 + 日志 `control=tcp`。S2（kill server）在跨机臂上经 `MGITEST_PEER_KILL_CMD` 模板（`adb -s <serial> shell run-as top.mobilegl.plugin.trace kill -9 {pid}`）真杀——**不**允许用协作式「请你退出」代替，那测的不是挂断。

### 4 包与顺序

`lk2`（缝主体，零行为）→ `wf`（定宽 + 摘要 + Refuse）→ `ct`（tcp 端点 + keepalive + 令牌 + Connect 拨号）→ `sl`（`StreamLink`，先在 **WSL loopback** 上对着 spawn 车道打平）→ `lf`（日志前送）→ `sv`（supervisor + Service + adb 脚本）→ `t65`（tcp 臂、跨机臂、门、数）。每包 red-once（R-16）。第二波（不在本节）：`nd` 的 `auto` 与 `sd` 的 aux 交付（同机 TCP 控制 + 共享段数据）、P11 的 stream 具名拒绝、Windows client。

### 5 出口门（本波）

1. G1 pull 0/0/0/0、`.text` 不变（全部新代码在 `MOBILEGL_BUILD_DISAGGREGATED` 下）。
2. lk2 后 inproc / spawn 车道结果数与逐线程 CPU 逐字不变（实测，不论证）。
3. **`integration-tcp`（WSL loopback，server 以 `--serve tcp://127.0.0.1:40613` 本地起）**：名集合 == spawn（parity 脚本加臂）、同数绿；每条目 arm 证明 `control=tcp data=stream server=<host:port> pid=N`（`junit_tally --require-tcp-ran`）。
4. **`integration-tcp-device`（server 在 Redmi）**：同一名集合；首轮**记录**、不设阻塞门，每一条与 loopback 臂的差异必须具名（GPU 能力 / P7 的 Magma 分歧 / 时限）；第二轮起转硬门。
5. **retrace**：CI split 子集（`trace_cases.json` 里未 `"split": false` 的）从 WSL 对着手机跑，DirectGLES 的 SSIM ≥ 现有阈值、OpenRA 为 1.0（E2 的锚）；DirectVulkan 记录不设门（真机分歧是 P7 的）。
6. red-once：wf 三条（P6.5 行）；ct——错令牌 → Refuse、`wireMajor` 不匹配 → 两端具名拒绝、`adb shell svc wifi disable` → 10 s 内闩住（而不是 120 s `BarrierTimeout`）；sl——`Progress` 倒退 → `Fatal{ProtocolCorruption}`、越窗的 `Stage` 帧 → 拒绝、去掉 flush-on-idle 后「帧内最后一条 `kWaitReply`」用例必须挂住并被超时抓到；lf——关掉前送后四个 arming 场景必须**按名红**；sv——第二个并发连接 → `Refuse{Busy}`。
7. **必测数（记录）**：逐帧 `kWaitReply` 次数与 RTT 分布（OpenRA、MC in-world 各一）；`SEG_STAGE` 字节/帧对链路实测吞吐（车道自己量一次 64 MiB `Stage` 突发）；`PRESENT_CREDIT` 1 / 2 / 3 的帧率；loopback tcp+stream 对 spawn（unix+shm）的逐线程 CPU 差。

### 6 两边怎么配（第一波操作手册）

**手机（server）**
```bash
# 1. 同一 commit 构建 trace APK（分离构建）并安装
./gradlew :app:assembleTraceDebug -Pmobilegl.buildDisaggregated=ON && adb install -r app/build/outputs/apk/trace/debug/*.apk
# 2. 手机 IP
adb shell ip -f inet addr show wlan0
# 3. 起 server（前台 Service → supervisor 监听）
adb shell am start-foreground-service -n top.mobilegl.plugin.trace/top.mobilegl.plugin.MobileGLServerService \
  --es listen tcp://0.0.0.0:40613 --es token devtoken-0123456789abcdef \
  --es env "MOBILEGL_BACKEND_TYPE=DirectGLES;MOBILEGL_PIPE_STATS=1;MOBILEGL_PIPE_STATS_PERIOD=1;MOBILEGL_LOG_FILE_PATH=/data/data/top.mobilegl.plugin.trace/files/mgl.log"
adb logcat -s MobileGL | grep -m1 'listening on tcp://'
# 停：adb shell am force-stop top.mobilegl.plugin.trace
```

**WSL（client）**，在分离构建目录（今天是 `~/lk-split`）：
```bash
export MOBILEGL_TRANSPORT=spawn                 # 拓扑：两进程；不 fork，因为下面给了远端
export MOBILEGL_IPC_CONTROL=tcp://<phone-ip>:40613
export MOBILEGL_IPC_DATA=stream
export MOBILEGL_IPC_TOKEN=devtoken-0123456789abcdef   # ≥16 字节（PH-7 (3)）：更短的令牌让 server 以 72 退出而不是监听
export MOBILEGL_IPC_REQUIRE_SAME_BUILD=1        # APK 与本地构建必须同 commit
ctest -L integration-tcp-device -j 1 --output-on-failure   # -j 1：设备同时只服务一个会话
# retrace（同一环境）：
(cd build-retrace/tools/trace_replay && ctest -V -R '^MobileGLTraceReplay\.OpenRA\.DirectGLES$')
```
日志：client 侧 `<base>.client.log` 与前送来的 `<base>.server.log` 都在 WSL；设备本地副本用 `adb shell run-as top.mobilegl.plugin.trace cat files/mgl.server.log`。

**备用（AP 隔离 / 没有局域网直通）**：adb 跑在 WSL 里——Windows 上一次 `adb tcpip 5555`，WSL 里 `adb connect <phone-ip>:5555 && adb forward tcp:40613 tcp:40613`；手机侧 `--es listen tcp://127.0.0.1:40613`（loopback，不需令牌），WSL 侧 `MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613`。

### 7 风险与未决（本波）

1. **`kWaitReply` × RTT**：`ResourceCreate` / `SetTextureParams` / 纹理 `ResourceSubData` 每次一个 RTT（Wi-Fi 2–5 ms）；MC 加载区块的创建突发会可感知地慢。本波**只量**（门 7），减少这一列是 P9 / P10 的活。
2. **带宽**：`SEG_STAGE` 字节/帧对 Wi-Fi；单次 128 MiB 上传 = 秒级停顿（开放问题 19、20）。
3. **Wi-Fi AP 隔离**：用 §6 的 adb forward 备用。
4. **DirectVulkan 真机分歧**（P7）会在门 5 出现：记录，不当本波的红。
5. **`MOBILEGL_IPC_ROLE`**：supervisor 必须替 child 设它，否则 child 的日志会当 client 写（`Log.cpp` 从这个 env 解析角色）。
6. **ctest 并行 vs 一设备一会话**：`-j 1` 或 supervisor `--max-sessions N`（每会话一进程，N 个 EGL 上下文同时存在，Adreno 上未验证）。
7. **同 commit**：两端二进制不同是常态，指纹只保证 wire 布局；`REQUIRE_SAME_BUILD` 是车道纪律，不是协议要求。

## 里程碑记录（原 `ROADMAP.md`）

- **终局改判（2026-09-22）**：AVF pVM / `AF_VSOCK` 终局撤回，改为 TCP 跨机跨平台 + 控制面 × 数据面两轴传输栈；P6.5 上关键路径，Ph 前置于 P12 的非 loopback 监听；P6 核出三项漏项（server 进程 `PipeStats` 结构性零——收尾中已修、184 棘轮门、hs 的三行契约未落地）与 `CURRENT_STAGE_PROGRESS.md` 里一条错误的 S6 陈述（已改）；class-C 已归零到两个（`SetSwapInterval`、`DeleteTransformFeedback`）。

## 本目录

| 文件 | 内容 |
|---|---|
| [`code-review-findings.md`](code-review-findings.md) | P6.5 第一波 代码审查（4 切片，2026-09-22） |
| [`data-link-implementation.md`](data-link-implementation.md) | P6.5 数据面实现记录 |
| [`evidence-index.md`](evidence-index.md) | P6.5 第一波证据索引 |
| [`mainmenu-trace-difference.md`](mainmenu-trace-difference.md) | P6.5：1.21.11 主菜单 GLES 像素差异诊断 |
| [`peer-loss-probe.md`](peer-loss-probe.md) | P6.5 真实 peer-loss 探针 |
| [`performance.md`](performance.md) | P6.5 主机逐线程 CPU 前后记录 |
| [`validation-status.md`](validation-status.md) | P6.5 TCP 车道与设备验收记录 |
| [`window-1b-p7w5.md`](window-1b-p7w5.md) | 窗口 1b · p7w5 DirectGLES × TCP 配速矩阵（中断报告） |
| [`window-1b-p7w6.md`](window-1b-p7w6.md) | 窗口 1b：p7w6 DirectGLES × TCP（2026-09-23） |
| [`wire-and-validation.md`](wire-and-validation.md) | P6.5 wire、计量与主机验证记录 |
