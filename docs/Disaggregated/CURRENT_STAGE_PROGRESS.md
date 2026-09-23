# 当前阶段进度 — P6.5：全 TCP 传输

## P6.5 第一波（2026-09-22）

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
  [`code-review-findings.md`](notes/p65/code-review-findings.md)。
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

契约见 [`CONTRACT-P65.md`](../../MobileGL/MG_Remote/CONTRACT-P65.md)，
制品身份与证据总索引见 [`evidence-index.md`](notes/p65/evidence-index.md)；
具体结果与未决项见 [`validation-status.md`](notes/p65/validation-status.md)、
[`wire-and-validation.md`](notes/p65/wire-and-validation.md)、
[`performance.md`](notes/p65/performance.md)。以下保留 P6 收官时的原始进度快照。

## P6 收官快照

> **这份文档只记当前阶段（P6）。** 已收官阶段（P0…P5f）的逐门数字与证据在
> [`ROADMAP.md`](ROADMAP.md) 的阶段表与各 `P5*` 报告里，不在这里重复。规范契约见
> [`MobileGL/MG_Remote/CONTRACT-P6.md`](../../MobileGL/MG_Remote/CONTRACT-P6.md)；设计见
> [`ARCHITECTURE.md`](ARCHITECTURE.md)。

**状态（2026-09-22）：P6 已收官。** 全部包（a6…t6）落地，出口门第 1–8 项全部达成：门 7（真机
`2f7cbe2e` reboot-clean、同热窗口配对 A/B）判读为 **tie**——`inproc` 与 `spawn` 的 client CPU/帧
差 −0.15%（会话 1）/ +0.16%（会话 2），都落在臂内散布之内，如实记录、不设门；门 8 的三个强制
性能数（socket 门铃 vs condvar、`SEG_STAGE` 字节/帧、chunking 后记录/帧）已采齐，逐数见 §2；
G1 pull 构建 **0/0/0/0** 且 `.text` 逐字节相同。测量期的源头：门 7 报告记 `75dd0cb1`、门 8 的
wire 计数器与真机第二轮记 `94c130d6`。分支 `feat/disaggregated` 其后又落地两个 P6.5 设计提交
（`872370ba`、`4e7bf0fe`），**现头 `4e7bf0fe`，与 `origin` 同步**；**工作树另有未提交的 P6 收尾
改动**（wire 计数器、`ServerMain` 的 `PipeStats` 武装、`MOBILEGL_PIPE_STATS_FILE` 角色派生、设备行
与工具入库、测试），见 §1.1。三种传输形态（`monolith` / `inproc` / `spawn`）在桌面（WSL lavapipe）
与真机（Redmi `2f7cbe2e`）上都跑通了真实 trace 的两进程 retrace，逐像素对上 golden。性能**只记录、
不设门**。

---

## 1. 包状态

契约把 P6 切成 a6（审计）→ c6（契约）→ lk / so / sm / cp / hs / dl / st / t6。全部已落地：

| 包 | 落地什么 | 状态 / 证据 |
|---|---|---|
| **a6** | 进程装配与链接边界的核验；198 行审计；184 符号的链接实验（server 不能不链 `MG_Impl`） | ✅ `c25a7760`；[`notes/p6/a6-audit-v1.md`](notes/p6/a6-audit-v1.md)、[`a6-link-experiment.md`](notes/p6/a6-link-experiment.md) |
| **c6** | 规范契约（21 条裁定）+ 数据面缝 `Transport/ILink.h` / `StreamLink.h`（声明不接线） | ✅ `b6293957`；`CONTRACT-P6.md` |
| **lk** | `RingControl` 按写者重组 + 可证伪的 per-field 断言；数据面缝的 pre-flight | ✅ `99649a90`、`db62d674` |
| **so** | `SocketTransport`（AF_UNIX SOCK_STREAM + SCM_RIGHTS），12 条 ITransport 契约用例 | ✅ `5f846d96` |
| **sm** | 两个**独立启动**的进程（非 fork），靠 name 汇合、共享匿名段；D1c 的六谓词里 D10 需要的三个，四个「编进来却永不武装」的守卫接线 | ✅ `70c45ab9`、`11807ac7`、`e3bb6d8b` |
| **cp** | EGL 控制帧跨进程并应答；`InitCapabilities` 上线（wire kind 11）；真实 trace 两进程 retrace 逐像素通过 | ✅ `5239ae0c`、`7e244b37` |
| **hs** | build stamp 从「静默空串」变成显式 present 标志入指纹；客户端补检 `abiMajor/Minor`；`Hello::pid` / `Welcome::serverPid` 携带真实 pid | ✅ `90f9a790` |
| **dl** | device-lost 闩（取自描述符的挂断，绝不取自超时）；D5b「alive-but-silent」具名诊断；`MOBILEGL_IPC_RESPAWN` 具名拒绝；`Session::Fail` 漏斗（`FatalFamilies.def` 词表 + 90 站点归一 + `SessionFault` 帧向对端命名家族）；Fatal 普查门 | ✅ `557dc2c1`、`8b13d8a8`、`3399b879`、`3a12a48a`、`4bb4d4fa`、`b46da700`、`77cbd176` |
| **st** | fb-slot memo 的具名 Fatal（`nullptr==nullptr` 读成缓存命中、返回未填槽的潜在崩溃，sm 关掉可达性、st 出声） | ✅ `e3bb6d8b` |
| **t6** | `integration-spawn` 车道（102/102）；集合一致做成结构性（一个宏两条臂）+ 校验门；per-role 日志 | ✅ `15f5b54b`、`5fb54f03` |

配套：CI 的 `retrace-split` 从「1 case × 1 后端 × inproc」扩成「全 CI 矩阵 × 两后端 × inproc/spawn」，
`split` 键改成 opt-out；Android CI 新增 `spawn-acceptance` 臂，真机 App 从 `nativeLibraryDir` 解析
server 路径（`dc5bd935`、`d83fc664`、`53406749`、`7c8120cc`）。

### 1.1 收尾（2026-09-22）

下列改动**在两轮测量（门 7 的 `75dd0cb1` 头、门 8 的 `94c130d6` 头）之后落地、目前仍在工作树里未
提交**（其后两个提交 `872370ba`、`4e7bf0fe` 只改文档与 `Transport/StreamLink.h` 的注释，不含这些代码）：

- **wire 计数器**：`PipeStats` 新增 `ByteClass::StageSegmentBytes`（唯一收口点
  `PipeWireCodec.cpp:878` `StageAllocate`）与 `CallClass::WireRecords`（`:1188` `EncodeRecord` 提交
  路径），外加一个 staged-blob 直方图（复用 `PayloadBucketOf` 的桶边界，只经 JSON dump 出口）；
  汇总行新增 `seg=` / `wrec=` / `wrec/f=`（后者与 `draws/f=` 同形，无 Present 时按既有规则退回窗口总量）。
  两个计数器都在 `#if MOBILEGL_PIPE_PUSH` 内，落点选在唯一收口点而不是调用方，否则等于维护一张
  "今天存在哪些 producer" 的名单。
- **`ServerMain` 的 `PipeStats` 武装**：spawn server 此前从不武装计数器（红一次的证据：
  **修复前 spawn server 0 条汇总行、修复后 29 条**，同一条 spawn trace、两侧 SSIM 均 1.000000）。
  `Init()` 放在 `MG_ConfigLoader::Init()` 与 `MGPipeSetServerProcessRole(true)` **之后**、
  `Shutdown()` 放在 `loop.Stop()` 之后 `_exit(0)` 之前——两处顺序都是承重的（前者让 latch 读到真
  配置值，后者让 apply 线程不再发布时取运行总量与 JSON dump）。见 §2.1。
- **`MOBILEGL_PIPE_STATS_FILE` 角色派生**：与日志 sink 同一套命名规则（库 `RoleLogPath` 导出、
  PipeStats 调用而非复制），dump 与 banner 都印/写 `<base>.client.json` / `<base>.server.json`，
  **基名不再指向任何文件**——没跟上改名的读取方会当场 `ENOENT`，这是刻意的。这一条在 G1 上红了
  三次才做对（无条件派生 → `.text +24`；加 `#if` 但两臂共用局部 → 1 symbol resized；两臂分离但
  重写 `MGLOG_W` 格式串 → `.rodata +64`），最终形态是 pull 臂的代码逐语句原样、所有新增都在 `#if` 内。
- **`pin_device.sh` 新增 `2f7cbe2e` 条目**：本板 GPU 钳 **1050 MHz**（`thermal_pwrlevel` 写 0 读回 1、
  `max_gpuclk` 只读 1 050 000 000、devfreq `max_freq` 同值），因此**绝对值与 1100 MHz 时代的
  `35d0befa` 行不可比**，只有同会话的配对可比。三种动作（`check` / `pin` / `unpin`）均已实测。
- **工具进树**：`tools/device_bench/p6/{ab_session.py,render_ab.py,README.md}`（真机配对 A/B 会话
  运行器 + 只读渲染器，门 7 快照里的 `gate7_ab.py` / `render_gate7.py` 是它们的前身）与
  `tools/trace_replay/run_android_retrace_local.py`（+19 行）：`MOBILEGL_TRACE_PACKAGE` 覆盖，
  让开发包能**装在既有 trace 包旁边**而不是替换它（两个不同密钥签的 APK 不能共用一个 id；
  缺省即原行为）。四个文件目前都在工作树里**未提交**。
- **测试**：`PipeStats` **25/25**、全 unit **2367/2367**、`integration-spawn` **102/102**；
  G1 复验 **0/0/0/0** 且 `.text` **逐字节相同**（pull 配置下两侧各 10 158 850 字节，`cmp` 无差异）。
  逐条 red-once 与 G1 三次泄漏见
  [`notes/p6/gate8-wire-counters.md`](notes/p6/gate8-wire-counters.md) §9.7.3。

---

## 2. 出口门（契约 §9）

| # | 门 | 状态 |
|---|---|---|
| 1 | **G1** pull 构建 0/0/0/0、`.text` 不变 | ✅ 符号 0 差异、`.text` 恒为 `0xa52203`（每次改动后复测） |
| 2 | **G2/G14** `integration-spawn` 名集合 == `integration-split` | ✅ `scripts/ci/spawn_lane_parity.py`：可比 102 == 102；名只增不删 |
| 3 | `integration-spawn` 与 split 同数绿 | ✅ 102/102 |
| 4 | 进程树：运行时恰多一个子进程、结束后为零 | ✅ `ServerSpawnTest.StartsAServerProcessAndHandshakesAcrossIt`（`CountOwnChildren`） |
| 5 | Arm 证明（ID-124）：每条 spawn 条目记子进程 pid 与 `transport=spawn` | ✅ `--require-spawn` 门要求 ConfigLoader marker **且** `spawn ARMED - pid N` |
| 6 | 每包 red-once（R-16） | ✅ dl 的 S2/S7、hs 的 serverPid、sm/st 的守卫塌缩、t6 的名集合漂移均已跑红一次 |
| 7 | **设备**：Redmi reboot-clean、同热窗口配对 A/B | ✅ **配对 A/B 判读 tie**：会话 1（boot id `e69d0329…`→`107f24d3…`）六臂交错 `monolith,inproc,spawn,inproc,monolith,spawn` × best-of-3 —— 18/18 runner 退出 0、36/36 pin check `PINNED`、0 `Fatal{`；client CPU p50 ms/帧 **inproc 7.880/7.889、spawn 7.876/7.869**（均值 −0.15%，符号随顺序翻转，臂内散布 0.2–4.2%），`monolith` 约贵 30%。会话 2（boot id `107f24d3…`→`7e6c6e9f…`）inproc/spawn 交错 × best-of-2：4/4 OK、8/8 `PINNED`、0 `Fatal`；client CPU **7.987/7.989 vs 7.992/8.009**（+0.16%，亦在 0.14–0.69% 臂内散布内）。差值在臂内噪声内，**如实记录、不设门**。见 §3、[`notes/p6/gate7-device-ab.md`](notes/p6/gate7-device-ab.md)、[`notes/p6/gate8-doorbell-device.md`](notes/p6/gate8-doorbell-device.md) |
| 8 | **性能只记录**，但三个数强制：socket 门铃 vs condvar、`SEG_STAGE` 字节/帧、chunking 后记录/帧 | ✅ 三个数齐（见 §3.2 / §3.3 与 [`notes/p6/gate8-doorbell-device.md`](notes/p6/gate8-doorbell-device.md)、[`notes/p6/gate8-wire-counters.md`](notes/p6/gate8-wire-counters.md)）：① socket 门铃 vs inproc condvar —— client **121.1 waits/帧**、5.6–6.1 parks/帧，两传输逐字相同（run total 30 401–30 407）；server inproc **10 866–11 963** waits/帧、5.0–6.2 parks/帧（park 率 0.042–0.06%），spawn **10 415–10 416** waits/帧、4.66–4.90 parks/帧。判读（诚实）：socket 阻塞读**既不更便宜也不更贵**，契约 §9「阻塞读可能更便宜」**未兑现**；spawn server waits 少 8.9–12.9% 归因于 handoff 纪律而非门铃本身；唯一 socket 略差的列是 clipark/f（6.07 vs 5.60/5.75，每帧多约 0.4 次 park）。② `SEG_STAGE` 字节/帧（inproc，真实 trace retrace）：OpenRA **130.8 KB median / 8.01 MB steady mean**（含启动负载 8.07 MB），rd12 **816.2 KB median / 1.53 MB steady mean**（含负载 10.79 MB）。③ 分块后记录/帧：OpenRA **211 median / 206 mean**，rd12 **7 497 median / 7 504 mean**；`maxrec=1808` 与分块前相同，默认 8 MiB chunk 预算在这两条负载上**从未被打到** |

`MEASUREMENTS.md:342` 的「分块后的逐 blob 字节分布尚无新测量」据此可关闭：门 8 第二轮给了
默认预算下的分布（`maxrec=1808`，稳态 `seg/wrec ≈ 109 B/条`），结论是**默认配置下该分布就是未分块的
原始分布**（分块在这两条负载上没有机会生效，由 `MOBILEGL_IPC_STAGE_MB=1` 的负控独立证明：seg 运行
总量两臂逐字节相同 2 698 026 763，`wrec` 仅 +0.11%）。

负控（各跑红一次）：S1 不可解析镜像（sm）、S2 kill server（dl）、S3 未洗环境（sm）、S5 窗口 token
到达（cp）、S6 **未跑——`LinkTerms.dataPlane` 不存在于树上**（`protocol.fbs` 无 `LinkTerms` / `Refuse`，hs 只落了 build stamp、`abiMajor/Minor` 补检与真实 pid；此处此前写"已跑红一次"是错的，随契约 §2 三行移交 P6.5，契约 §10.3）、S7 裸 abort（dl 普查门）、S8 `SessionFaultCount()==0`
整轮（t6）。

### 2.1 出口门之外仍欠（2026-09-22 核出；出口门 1–8 本身已全部达成）

- server 进程侧的 `PipeStats` 半边是**结构性零**：`ServerMain` 不跑 `MobileGL::Initialize`，`PipeStats::Init()` 从未在 server 进程执行，~80 处 `if (Enabled())` 永远为假，`srv=0 srvpark=0` 长得和"从没等过"一模一样（`PipeStats.h:256` 自己写明了这一点）——门 8 的"socket 门铃 vs condvar"因此没有 server 半边。**已修（工作树未提交，见 §1.1）**：在 server 角色初始化的唯一收口点于 config 加载与角色声明之后加 `Init()`，`loop.Stop()` 之后加 `Shutdown()`，汇总行落 `<base>.server.log`；spawn server 汇总行 0 → 29 条，SSIM 1.0 不变。
- 184 符号棘轮 CI 门（契约 §12.1）：`test.yml` / `scripts/` 里**没有**。
- 契约 §2 表 0 分给 hs 的 `LinkTerms` / `Refuse` / `wireFingerprint` 拆分：**未落地**，移交 P6.5（`ROADMAP.md` P6.5 行，契约 §10.3）。
- `MOBILEGL_IPC_IDLE_EXIT_S`（`ARCHITECTURE.md` §15.3 默认 30）：未被解析。
- `TextureParamsWithoutASamplerView` 的"P6 inspection forwarder"债（`MEASUREMENTS.md` §7.2）：场景代码已无 P6 引用，是否已由 fc / cp 关闭**待核**。

---

## 3. 真机验收（Redmi `2f7cbe2e`）

trace APK 以 `-Pmobilegl.buildDisaggregated=ON` 构建，调试签名安装。功能全绿：

- **`OpenRA.DirectGLES` spawn**：ssim=1.000000、mismatchPixels=0、`Fatal{` 零条，连跑三次；
  `--require-spawn` 确认 `server role in pid N`。
- 另有 6 个 case（含 in-world、BSL 光影）spawn 下通过。
- **monolith** 控制组 ssim=1.0；证明门确认 monolith 只写 `mobilegl.client.log`、无 server 日志。

**如实记录、非 P6 引入**：`DirectVulkan` 在真机分离路径下画面分歧且**不确定**（inproc 与 spawn
抽自同一组结果：0.988998/0.976494…，而同 pbuffer 的 monolith 是 1.0）。两种 transport 表现一致，
故是 Magma 分离路径在真实 GPU 上的问题，P6 既未引入也修不了；桌面 lavapipe 上 DirectVulkan spawn 为 1.0。

那个会杀死 server 的 llvmpipe case（`iterationrp`，`LLVM ERROR: Cannot select vcvtps2ph`）：dl 落地后
从 123 秒的错误 `BarrierTimeout` 变成 **5.7 秒**的正确 `DEVICE LOST`（`revents=0x2010` = POLLRDHUP|POLLHUP）——
仍红（驱动 bug），但红得诚实。

### 3.1 配对 A/B（门 7，2026-09-22）

reboot-clean + 同热窗口，臂 = `inproc` vs `spawn`（`monolith` 作基线行）。负载
`minecraft-1.21.4-rd12-odinlite-in-world` + `DirectGLES`（~1471 draws/帧），
`--benchmark-tail-frames 200`，主指标 = client 线程自身 `CLOCK_THREAD_CPUTIME_ID` 的稳态
p50 ms/帧。两轮会话，各自一个 boot id、各自一次 `adb reboot`（boot id 变化即为事实）：

| 会话 | boot id | 臂序 | 重复 | 结果 | client CPU p50 ms/帧 |
|---|---|---|---|---|---|
| 1 | `e69d0329…` → `107f24d3…` | `monolith,inproc,spawn,inproc,monolith,spawn` | best-of-3 | 18/18 runner 退出 0、36/36 pin check `PINNED`、0 `Fatal{` | inproc **7.880 / 7.889**、spawn **7.876 / 7.869**（均值 **−0.15%**，符号随顺序翻转，臂内散布 **0.2–4.2%**），monolith **约贵 30%** |
| 2 | `107f24d3…` → `7e6c6e9f…` | `inproc,spawn,spawn,inproc` | best-of-2 | 4/4 OK、8/8 `PINNED`、0 `Fatal` | inproc **7.987 / 7.989**、spawn **7.992 / 8.009**（**+0.16%**，臂内散布 0.14–0.69%） |

**判读：tie。** 两个方向的差值都在臂内噪声之内，且两轮之间绝对值漂移（~0.1 ms）大于传输差本身。
按本阶段「性能只记录、不设门」的纪律，这组数**如实记录、不设门**，不写成"spawn 更快"也不写成
"spawn 更慢"。臂证明（契约 §9.5）：每个 spawn 条目都记了子进程 pid 与 `transport=spawn`，
inproc 臂带 `Config: IPC` 行，两个 monolith 臂**既不**带 transport marker **也不**带 `Config: IPC` 行
（控制组 stayed a control）。逐臂 pin 证据、温度范围（会话 1：37.6–46.5 °C；会话 2：37.2–44.9 °C）
与全部原始行见 [`notes/p6/gate7-device-ab.md`](notes/p6/gate7-device-ab.md)、
[`notes/p6/gate8-doorbell-device.md`](notes/p6/gate8-doorbell-device.md)。

### 3.2 门 8① 的 socket 门铃 vs `inproc` condvar

| 半边 | inproc | spawn |
|---|---|---|
| client `cli` waits/帧 | **121.1**（run total 30 407） | **121.1**（run total 30 401），**两传输逐字相同** |
| client `clipark` parks/帧 | 5.60 / 5.75 | **6.07 / 6.08**（唯一 socket 略差的列，每帧多约 0.4 次 park） |
| server `srv` waits/帧 | 10 866–11 963 | 10 415–10 416（低 8.9–12.9%） |
| server `srvpark` parks/帧 | 5.0–6.2 | 4.66–4.90 |
| server park 率 | 0.042–0.06% | 0.042–0.047% |

**判读（诚实）**：socket 阻塞读**既不更便宜也不更贵**——client CPU 差 +0.16% 在噪声内，契约 §9
「阻塞读可能更便宜」**未兑现**（既未被证实也未被证伪，它该移动的那个可观测量没有向任何方向移动）。
spawn server waits 少 8.9–12.9% **归因于 handoff 纪律而非门铃本身**（两条臂统计的不是同一个事件，
可比的是 park 率，而 park 率两侧几乎相同）。这组数只记录、不设门。

**读法警告**（结构性零，不是测量）：spawn 的 `srv` 对在 **server.log**、`cli` 对在 **client.log**；
client 行上的 `srv=0 srvpark=0` 是「另一个进程」而不是「从未等待」，server 行上的 `cli=0 clipark=0`
同理。spawn client **从不调 `OnPresent`** → 窗口字段 `frames=0 window=0` 无效，但 `PublishGauge` 是
store，所以 `cli`/`clipark` 的 **run total 有效**；spawn client 只有 `Shutdown` 终行（benchmark 模式）
或 0 行（snapshot 模式）。

### 3.3 门 8②③ 的 wire 计数器（`SEG_STAGE` 字节/帧、分块后记录/帧）

inproc、真实 trace retrace，`MOBILEGL_PIPE_STATS_PERIOD=1`：

| 负载 | `SEG_STAGE` 字节/帧 | 记录/帧 |
|---|---|---|
| OpenRA | **130.8 KB median** / 8.01 MB steady mean（含启动负载 8.07 MB） | **211 median** / 206 mean |
| rd12 in-world | **816.2 KB median** / 1.53 MB steady mean（含负载 10.79 MB） | **7 497 median** / 7 504 mean |

**framing 警告**：PERIOD=1 下首窗含启动负载（rd12 帧 1 有 981 654 draws vs 稳态 ~1 471），
run-wide mean 约 **7×** 稳态，所以两个读数都报，引用时必须说明口径。**`maxrec=1808` 与分块前相同**
——默认 8 MiB chunk 预算在这两条负载上**从未被打到**。负控 `MOBILEGL_IPC_STAGE_MB=1`（256 KiB 预算）：
`seg` 运行总量两臂**逐字节相同**（2 698 026 763），`wrec` 仅 **+0.11%**——证明计数器测的是生产者而非切分；
同一 A/B 下 `tex` **+194%** **未解释**（`MOBILEGL_IPC_STAGE_MB` 同时是 arena 大小与 chunk 预算的来源，
未隔离到单变量），**记为证据、不作结论**。逐 blob 分布首测（integration 车道 JSON；retrace 车道拿不到
dump，因 snapshot 模式不走 `cleanUp()` → `MobileGL::Destroy()`）：**70 records、8 blobs、bucket 3–12**。

**做与未做，分开写**：OpenRA 真机辅负载**未跑**（如实记「未跑」）；
[`notes/p6/gate8-wire-counters.md`](notes/p6/gate8-wire-counters.md) 的 §6 数据全部来自 WSL + llvmpipe，
门 8②③ 是**桌面 inproc** 数，门 8① 的 server 半边另有真机重复（§3.2）。

---

## 4. 日志按角色分文件（本阶段的一处基础设施改动）

spawn 的两个进程原先 O_APPEND 追加同一个 `MOBILEGL_LOG_FILE_PATH`（正确但两个会话混在一处）。现在
库按**调用线程角色**写 `<base>.client.log` / `<base>.server.log`（inproc 下 apply 线程即 server 角色，
同样分开）。基名不再指向任何文件——这是刻意的：没跟上改名的读取方会「文件不存在」当场报错，而不是
半读通过。34 个消费方全部改到派生路径；拒绝普查读两份；派生规则由库 `RoleLogPath` 导出，不复制。
G1 守卫下 pull 构建不变。

---

## 5. 已知未决 / 明确不属于 P6

- **`DirectVulkan` 真机分离路径分歧**——Magma × 真实 GPU，非 P6。见 §3 与 `ROADMAP.md` P7 行。
- **P6.5**：传输栈两轴化（控制面 `ITransport` × 数据面 `ILink`，握手协商、可混搭）+ TCP 跨机 + wire 定宽与布局摘要；`StreamLink` 已声明未接线；`SEG_STAGE` 送窗口复活。终局已改为 TCP 跨机跨平台（2026-09-22），AF_VSOCK 撤回；见 `ROADMAP.md` P6.5 行、`ARCHITECTURE.md` §11.9。
- **Ph**：让 `Session::Fail` 可返回的策略翻转（需在每个依赖 `[[noreturn]]` 的站点造真实返回路径）+ 配对 / 认证；前置于 P12 的非 loopback 监听。
- **P12**：真窗口跨进程（`ANativeWindow*` 是客户端进程内指针，`SetWindowHandle` 至今具名拒绝）。
- **seq/op 未穿 SessionFault 帧**：90 个站点不逐一穿线，消息本身已带 op，帧只带 code/family/message。

两条**结构性限制**，随收尾一起生效、如实记录（不是缺陷但会绊住读者）：

- **spawn client 无 `OnPresent`** → 它的窗口字段 `frames=0 window=0`，于是
  `draws=` / `wrec=` / `bytes[...]` / `gates[...]` 与**所有 per-frame 字段**在 spawn client 上都印 0 或
  `n/a`，**无效**；但 `PublishGauge` 是 **store 不是 add**，所以 `cli` / `clipark`、`maxrec` /
  `maxcap` / `ringwraps` / `ringpads` / `ringwaits`，以及发射侧直接 `Add` 的
  `resid` / `csob-blob` / `seg` / `wrec` 的 **run total 有效**（窗口归属只有最后一段）。要取 spawn 的
  窗口化 client 数必须让 client 进程到达一次帧边界——那会与 backend 的帧边界重复计数，需单独判断，
  不在本阶段。**逐 role 的取数规则**因此是：`srv` 对读 server.log、`cli` 对读 client.log，
  spawn client 只有 `Shutdown` 终行（benchmark 模式）或 0 行（snapshot 模式）。
- **`MOBILEGL_PIPE_STATS_FILE` 已改角色派生**：dump 与 banner 都写/印
  `<base>.client.json` / `<base>.server.json`，**基名不再指向任何文件**（与日志 sink 同一套
  `RoleLogPath` 规则，刻意让没跟上的读取方当场 `ENOENT` 而不是半读通过）。inproc 只产出
  `.client.json` 是**正确**的（服务端角色是线程不是进程，`Shutdown()` 只跑一次）。仓库内目前没有
  这样的读者（`grep` 确认），但任何新写的车道/脚本都要读派生名。语义未变：仍是「在这里写 dump」，
  只是「这里」由库按角色展开成两个名字。
