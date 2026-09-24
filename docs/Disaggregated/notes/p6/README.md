# P6 — spawn transport（2026-09-22 收官）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 十包全落地：a6 审计 → c6 契约（`MobileGL/MG_Remote/CONTRACT-P6.md`）→ lk `RingControl` 按写者重组 → so `SocketTransport`（AF_UNIX + SCM_RIGHTS）→ sm 两个独立启动的进程 → cp EGL 控制帧跨进程 → hs build stamp / abi 补检 / 真实 pid → dl device-lost 闩 + `Session::Fail` 漏斗 → st fb-slot memo 具名 Fatal → t6 `integration-spawn` 车道（102/102）+ per-role 日志。
- 出口门 1–8 全达成：G1 0/0/0/0（`.text` `0xa52203`）；spawn 与 split 名集合一致且同数绿；进程树恰多一个子进程；逐条 arm 证明；每包 red-once；**门 7** 真机配对 A/B 判读 **tie**（inproc vs spawn client CPU −0.15% / +0.16%，monolith 约贵 30%）；**门 8** 三个强制性能数齐（门铃 tie、`SEG_STAGE` 字节/帧、分块后记录/帧）。
- 真机：`OpenRA.DirectGLES` spawn ssim 1.0 连跑三次；DirectVulkan 分离路径的不确定分歧归 P7（后由 B3 证明是 wire 臂 frame-serial floor 不健全）。杀 server 从 123 s 的错误 `BarrierTimeout` 变为 5.7 s 的正确 `DEVICE LOST`。
- 包计划、契约草稿与终局重审（2026-09-24 从上层移入）：[`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md)、[`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md)（已被 `CONTRACT-P6.md` 取代）、[`P6-ENDSTATE-REVIEW.md`](P6-ENDSTATE-REVIEW.md)（其 AVF pVM / `AF_VSOCK` 终局已撤回，见页首更正）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P6** spawn transport
- **状态**：✅ **已收官（2026-09-22）**；全部包已落地于 `feat/disaggregated`（头 `77cbd176`，2026-09-21），出口门第 1–6 项此前已达成，第 7/8 项于 2026-09-22 采齐。收官前核出的三项：server 进程侧 `PipeStats` 从未初始化（`srv=0 srvpark=0` 是结构性零，门 8 无 server 半边）**已在收尾中修好**；184 符号棘轮 CI 门（契约 §12.1）与契约 §2 分给 hs 的 `LinkTerms` / `Refuse` / 指纹拆分**未落地**，移交 P6.5（负控 S6 随之）
- **落地什么 / 范围**：十包全落地：**a6** 进程装配/链接边界审计 → **c6** 规范契约 + 数据面缝惰声明 → **lk** `RingControl` 按写者重组 → **so** `SocketTransport`（AF_UNIX SOCK_STREAM + SCM_RIGHTS，12 条契约用例）→ **sm** 两个独立启动的进程（非 fork，name 汇合、共享匿名段，四守卫接线）→ **cp** EGL 控制帧跨进程并应答（`InitCapabilities` wire kind 11）→ **hs** build stamp 显式 present 入指纹 + `abiMajor/Minor` 补检 + 真实 pid → **dl** device-lost 闩（取自描述符挂断绝不取自超时）+ `Session::Fail` 漏斗（`FatalFamilies.def` 词表 + 90 站点归一 + `SessionFault` 帧命名家族）+ Fatal 普查门 → **st** fb-slot memo 具名 Fatal → **t6** `integration-spawn` 车道（102/102）+ 名集合结构性一致门 + per-role 日志（`<base>.client.log` / `<base>.server.log`）。Android CI 新增 `spawn-acceptance` 臂，App 从 `nativeLibraryDir` 解析 server 路径。真机 Redmi `2f7cbe2e` 上 `OpenRA.DirectGLES` spawn ssim=1.0 连跑三次 + 6 case 绿；`DirectVulkan` 真机分离路径不确定分歧（inproc 与 spawn 同表现→ Magma × 真实 GPU，非 P6，桌面 lavapipe 为 1.0）。边界仍为 pbuffer / surfaceless / 离屏；真窗口归 P12。契约 [`MG_Remote/CONTRACT-P6.md`](../../../../MobileGL/MG_Remote/CONTRACT-P6.md)；a6 产出 [`a6-audit-v1.md`](a6-audit-v1.md)、[`a6-link-experiment.md`](a6-link-experiment.md)（**server 不能不链 `MG_Impl`**：184 符号，P7 101 / P3b-P4b 14 / 共有 60 / P6 自己 6）；终局重审 [`P6-ENDSTATE-REVIEW.md`](P6-ENDSTATE-REVIEW.md)。**收尾四项（2026-09-22，工作树未提交）**：wire 计数器——`PipeStats` 新增 `ByteClass::StageSegmentBytes`（`PipeWireCodec.cpp:878` `StageAllocate` 唯一收口）与 `CallClass::WireRecords`（`:1188` `EncodeRecord` 提交路径）+ staged-blob 直方图，汇总行新增 `seg=` / `wrec=`；`ServerMain.cpp` 补 `PipeStats::Init()` / `Shutdown()`（spawn server 此前从不武装计数器——修前 0 条汇总行、修后 29 条，SSIM 1.0 不变）；`MOBILEGL_PIPE_STATS_FILE` 按角色派生 `<base>.client.json` / `<base>.server.json`（走 `RoleLogPath` 同一导出规则，基名不指向文件）；工具入库 `tools/device_bench/p6/{ab_session.py,render_ab.py,README.md}` 与 `tools/trace_replay/run_android_retrace_local.py`（+19 行 `MOBILEGL_TRACE_PACKAGE`），`pin_device.sh` 新增 `2f7cbe2e` 条目（本板 GPU 钳 1050 MHz：`thermal_pwrlevel` 写 0 读回 1、`max_gpuclk` 只读 1 050 000 000；绝对值与 1100 MHz 时代的 `35d0befa` 行不可比）。
- **验收门 / 证据**：出口门（契约 §9）1–6 ✅：G1 pull 0/0/0/0、`.text` 恒 `0xa52203`；`integration-spawn` 与 split 名集合一致（可比 102==102）且同数绿；进程树运行时恰多一子进程、结束为零；每条 spawn 条目记子进程 pid 与 `transport=spawn`（`--require-spawn` 门）；每包 red-once。**7/8 已采（2026-09-22）**：**门 7** 真机 reboot-clean 同热窗口配对 A/B（Redmi `2f7cbe2e` / M332BF / SM8750 / Adreno 830v2，CPU 定频 little 1555200 / big 1958400，GPU 钳 1050 MHz（本板 `thermal_pwrlevel` 写 0 读回 1、`max_gpuclk` 只读 1 050 000 000；绝对值与 1100 MHz 时代的 `35d0befa` 行不可比，只有同会话配对可比），`minecraft-1.21.4-rd12-odinlite-in-world` + DirectGLES，~1471 draws/帧；六臂交错 × best-of-3：18/18 runner 退出 0、36/36 pin check `PINNED`、0 `Fatal`；client CPU p50 ms/帧 inproc 7.880/7.889 vs spawn 7.876/7.869，均值 **−0.15%**，符号随臂序翻转，臂内散布 0.2–4.2%；第二会话 inproc/spawn 交错 × best-of-2：4/4 OK、8/8 `PINNED`、0 `Fatal`，7.987/7.989 vs 7.992/8.009 = **+0.16%**）——判读 **tie**（差值在臂内噪声内），如实记录、不设门；monolith 基线约贵 30%（−29.8% / −29.9%）。**门 8①** socket 门铃 vs inproc condvar：client 121.1 waits/帧、5.6–6.1 parks/帧，两传输逐字相同（run total 30 401–30 407）；server inproc 10 866–11 963 waits/帧、5.0–6.2 parks/帧，spawn 10 415–10 416 waits/帧、4.66–4.90 parks/帧——socket 阻塞读**既不更便宜也不更贵**（契约 §9「阻塞读可能更便宜」未兑现），spawn server waits 少 8.9–12.9% 归因于 handoff 纪律而非门铃本身，唯一 socket 略差的列是 `clipark/f`（6.07 vs 5.60/5.75）。**门 8②③**（inproc，真实 trace retrace）：`SEG_STAGE` 字节/帧 OpenRA 130.8 KB median / 8.01 MB steady mean（含启动负载 8.07 MB），rd12 816.2 KB median / 1.53 MB steady mean（含负载 10.79 MB，rd12 首帧 981 654 draws vs 稳态 ~1471，run-wide mean 约 7× 稳态，两个读法都报）；分块后记录/帧 OpenRA 211 median / 206 mean，rd12 7 497 median / 7 504 mean；`maxrec=1808` 与分块前相同（默认 8 MiB chunk 预算在这些负载上从未被打到）。证据 [`notes/p6/gate7-device-ab.md`](gate7-device-ab.md) / [`notes/p6/gate8-doorbell-device.md`](gate8-doorbell-device.md) / [`notes/p6/gate8-wire-counters.md`](gate8-wire-counters.md)

## 收官快照（原 `CURRENT_STAGE_PROGRESS.md`，2026-09-22）



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

### 1. 包状态

契约把 P6 切成 a6（审计）→ c6（契约）→ lk / so / sm / cp / hs / dl / st / t6。全部已落地：

| 包 | 落地什么 | 状态 / 证据 |
|---|---|---|
| **a6** | 进程装配与链接边界的核验；198 行审计；184 符号的链接实验（server 不能不链 `MG_Impl`） | ✅ `c25a7760`；[`notes/p6/a6-audit-v1.md`](a6-audit-v1.md)、[`a6-link-experiment.md`](a6-link-experiment.md) |
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

#### 1.1 收尾（2026-09-22）

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
  [`notes/p6/gate8-wire-counters.md`](gate8-wire-counters.md) §9.7.3。

---

### 2. 出口门（契约 §9）

| # | 门 | 状态 |
|---|---|---|
| 1 | **G1** pull 构建 0/0/0/0、`.text` 不变 | ✅ 符号 0 差异、`.text` 恒为 `0xa52203`（每次改动后复测） |
| 2 | **G2/G14** `integration-spawn` 名集合 == `integration-split` | ✅ `scripts/ci/spawn_lane_parity.py`：可比 102 == 102；名只增不删 |
| 3 | `integration-spawn` 与 split 同数绿 | ✅ 102/102 |
| 4 | 进程树：运行时恰多一个子进程、结束后为零 | ✅ `ServerSpawnTest.StartsAServerProcessAndHandshakesAcrossIt`（`CountOwnChildren`） |
| 5 | Arm 证明（ID-124）：每条 spawn 条目记子进程 pid 与 `transport=spawn` | ✅ `--require-spawn` 门要求 ConfigLoader marker **且** `spawn ARMED - pid N` |
| 6 | 每包 red-once（R-16） | ✅ dl 的 S2/S7、hs 的 serverPid、sm/st 的守卫塌缩、t6 的名集合漂移均已跑红一次 |
| 7 | **设备**：Redmi reboot-clean、同热窗口配对 A/B | ✅ **配对 A/B 判读 tie**：会话 1（boot id `e69d0329…`→`107f24d3…`）六臂交错 `monolith,inproc,spawn,inproc,monolith,spawn` × best-of-3 —— 18/18 runner 退出 0、36/36 pin check `PINNED`、0 `Fatal{`；client CPU p50 ms/帧 **inproc 7.880/7.889、spawn 7.876/7.869**（均值 −0.15%，符号随顺序翻转，臂内散布 0.2–4.2%），`monolith` 约贵 30%。会话 2（boot id `107f24d3…`→`7e6c6e9f…`）inproc/spawn 交错 × best-of-2：4/4 OK、8/8 `PINNED`、0 `Fatal`；client CPU **7.987/7.989 vs 7.992/8.009**（+0.16%，亦在 0.14–0.69% 臂内散布内）。差值在臂内噪声内，**如实记录、不设门**。见 §3、[`notes/p6/gate7-device-ab.md`](gate7-device-ab.md)、[`notes/p6/gate8-doorbell-device.md`](gate8-doorbell-device.md) |
| 8 | **性能只记录**，但三个数强制：socket 门铃 vs condvar、`SEG_STAGE` 字节/帧、chunking 后记录/帧 | ✅ 三个数齐（见 §3.2 / §3.3 与 [`notes/p6/gate8-doorbell-device.md`](gate8-doorbell-device.md)、[`notes/p6/gate8-wire-counters.md`](gate8-wire-counters.md)）：① socket 门铃 vs inproc condvar —— client **121.1 waits/帧**、5.6–6.1 parks/帧，两传输逐字相同（run total 30 401–30 407）；server inproc **10 866–11 963** waits/帧、5.0–6.2 parks/帧（park 率 0.042–0.06%），spawn **10 415–10 416** waits/帧、4.66–4.90 parks/帧。判读（诚实）：socket 阻塞读**既不更便宜也不更贵**，契约 §9「阻塞读可能更便宜」**未兑现**；spawn server waits 少 8.9–12.9% 归因于 handoff 纪律而非门铃本身；唯一 socket 略差的列是 clipark/f（6.07 vs 5.60/5.75，每帧多约 0.4 次 park）。② `SEG_STAGE` 字节/帧（inproc，真实 trace retrace）：OpenRA **130.8 KB median / 8.01 MB steady mean**（含启动负载 8.07 MB），rd12 **816.2 KB median / 1.53 MB steady mean**（含负载 10.79 MB）。③ 分块后记录/帧：OpenRA **211 median / 206 mean**，rd12 **7 497 median / 7 504 mean**；`maxrec=1808` 与分块前相同，默认 8 MiB chunk 预算在这两条负载上**从未被打到** |

`MEASUREMENTS.md:342` 的「分块后的逐 blob 字节分布尚无新测量」据此可关闭：门 8 第二轮给了
默认预算下的分布（`maxrec=1808`，稳态 `seg/wrec ≈ 109 B/条`），结论是**默认配置下该分布就是未分块的
原始分布**（分块在这两条负载上没有机会生效，由 `MOBILEGL_IPC_STAGE_MB=1` 的负控独立证明：seg 运行
总量两臂逐字节相同 2 698 026 763，`wrec` 仅 +0.11%）。

负控（各跑红一次）：S1 不可解析镜像（sm）、S2 kill server（dl）、S3 未洗环境（sm）、S5 窗口 token
到达（cp）、S6 **未跑——`LinkTerms.dataPlane` 不存在于树上**（`protocol.fbs` 无 `LinkTerms` / `Refuse`，hs 只落了 build stamp、`abiMajor/Minor` 补检与真实 pid；此处此前写"已跑红一次"是错的，随契约 §2 三行移交 P6.5，契约 §10.3）、S7 裸 abort（dl 普查门）、S8 `SessionFaultCount()==0`
整轮（t6）。

#### 2.1 出口门之外仍欠（2026-09-22 核出；出口门 1–8 本身已全部达成）

- server 进程侧的 `PipeStats` 半边是**结构性零**：`ServerMain` 不跑 `MobileGL::Initialize`，`PipeStats::Init()` 从未在 server 进程执行，~80 处 `if (Enabled())` 永远为假，`srv=0 srvpark=0` 长得和"从没等过"一模一样（`PipeStats.h:256` 自己写明了这一点）——门 8 的"socket 门铃 vs condvar"因此没有 server 半边。**已修（工作树未提交，见 §1.1）**：在 server 角色初始化的唯一收口点于 config 加载与角色声明之后加 `Init()`，`loop.Stop()` 之后加 `Shutdown()`，汇总行落 `<base>.server.log`；spawn server 汇总行 0 → 29 条，SSIM 1.0 不变。
- 184 符号棘轮 CI 门（契约 §12.1）：`test.yml` / `scripts/` 里**没有**。
- 契约 §2 表 0 分给 hs 的 `LinkTerms` / `Refuse` / `wireFingerprint` 拆分：**未落地**，移交 P6.5（`ROADMAP.md` P6.5 行，契约 §10.3）。
- `MOBILEGL_IPC_IDLE_EXIT_S`（`ARCHITECTURE.md` §15.3 默认 30）：未被解析。
- `TextureParamsWithoutASamplerView` 的"P6 inspection forwarder"债（`MEASUREMENTS.md` §7.2）：场景代码已无 P6 引用，是否已由 fc / cp 关闭**待核**。

---

### 3. 真机验收（Redmi `2f7cbe2e`）

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

配对 A/B、门铃与 wire 计数器三张表的完整版在下文「实测」节 §12.2–§12.5（原快照里的 §3.1–§3.3 是它们的摘录，已去重）。

### 4. 日志按角色分文件（本阶段的一处基础设施改动）

spawn 的两个进程原先 O_APPEND 追加同一个 `MOBILEGL_LOG_FILE_PATH`（正确但两个会话混在一处）。现在
库按**调用线程角色**写 `<base>.client.log` / `<base>.server.log`（inproc 下 apply 线程即 server 角色，
同样分开）。基名不再指向任何文件——这是刻意的：没跟上改名的读取方会「文件不存在」当场报错，而不是
半读通过。34 个消费方全部改到派生路径；拒绝普查读两份；派生规则由库 `RoleLogPath` 导出，不复制。
G1 守卫下 pull 构建不变。

---

### 5. 已知未决 / 明确不属于 P6

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

## 实测：P6 出口测量（2026-09-22，Redmi `2f7cbe2e` + WSL 桌面）（原 `MEASUREMENTS.md` §12）

P6（spawn transport）的实现包（a6 / c6 / lk / so / sm / cp / hs / dl / st / t6）已全部落地，出口门 1–6 的逐项证据见 `CURRENT_STAGE_PROGRESS.md`；本节记录门 7（真机配对 A/B 热窗口）与门 8（三个强制性能数）在 2026-09-22 采齐的实测，以及支撑它们的收尾代码与工具。按本文件既有口径，下面所有性能数**只记录、不设门**：门 7 的判读是 tie，门 8 的三个数不带阈值。

### 12.1 协议（本节两个设备会话共同遵守）

| 项 | 值 |
|---|---|
| 设备 | Redmi `M332BF`，serial `2f7cbe2e`，SM8750 / Adreno 830v2 |
| reboot-clean | `adb reboot` + `sys.boot_completed=1`，**boot id 前后比对**（id 变了才算数，见 §12.2） |
| CPU 定频 | little `policy0` → 1555200，big `policy6` → 1958400（`pin_device.sh 2f7cbe2e pin`；每臂前 `pin`、后 `check`，非 `PINNED` 即作废该臂，不是降级） |
| GPU 钉频 | kgsl `min/max_pwrlevel = 0` → **1050000000 Hz**（可比性警告见 §12.7） |
| 风扇 | `/sys/class/xm_power/hw_monitor/pwm_fan` `target_level=2` |
| 同热窗口臂交错 | 每个会话都在**一次 boot 内的唯一热窗口**里按给定顺序交错跑完，臂间有冷却 |
| 负载 | `minecraft-1.21.4-rd12-odinlite-in-world`，DirectGLES，251 帧，**1471.36 draws/帧** |
| 跑法 | `--benchmark-repeats 3`（门 7）/ `2`（门 8），`--benchmark-tail-frames 200`，`--benchmark-no-finish`，best-of-3 / best-of-2 |
| split 开关 | `MOBILEGL_IPC_ROLE_SPLIT_STATE=1`、`MOBILEGL_IPC_STRICT_ERRORS=1`、`MOBILEGL_IPC_RUN_AHEAD=1` |
| stats | `MOBILEGL_PIPE_STATS=1`、`MOBILEGL_PIPE_STATS_PERIOD=120` |
| 源码头 | 门 7 会话 `75dd0cb1`；门 8 两会话 `94c130d6` + §12.6 的收尾改动（工作树） |
| 臂的证明 | 每个 spawn 臂必须在**自己的**私有日志里出现 `Config: MOBILEGL_TRANSPORT=spawn` 与 `spawn ARMED - the server role runs in pid N`；`--transport` 在 `--benchmark` 模式下被 runner 忽略（会静默跑成 monolith），所以传输一律用显式 `--env MOBILEGL_TRANSPORT=…`，跑完按库自己的日志核对 |

**主指标 = client GL 线程自身的逐线程 CPU ms/帧**（§9 的规则：内核对本 app 的 `sched_setaffinity` 请求不生效，进程总量会把两个时钟域混在一起），由设备在**尾 200 帧**上汇总；`fps` 与 wall p50/p95 并列记录，不作主指标。

**帧分母取自哪条日志。** `frames=` 由 `PipeStats::OnPresent` 递增，而它是 backend 的帧边界：`spawn` 下 present 在 **server 进程**被应用，所以 spawn **client 的 `frames=0 window=0`**——它一个窗口都不推进。因此费率一律除以**真正数到帧的那条日志**的 `frames=`（inproc 取 client log，spawn 取 server log，两者都是 251），**不得**除以同一行的 `window=`，那会把费率抬高 run/window（本负载约 21×），得到一个看似合理而错的数。同理：

- spawn client 行里的 `srv=0 srvpark=0` 与 spawn server 行里的 `cli=0 clipark=0` 都是**「另一个进程」的零，不是「从未等待」**；`srv`/`srvpark` 由 apply 线程发布进**它自己进程**的 PipeStats，`cli`/`clipark` 由 client 的发射器发布进 client 进程。
- spawn client 的 `cli`/`clipark` 仍然可信，因为 `EmitPresent` 走 `PublishGauge`（**store 不是 add**），run total 与窗口无关；而它的窗口化字段（`draws=`、`wrec=`、`bytes[...]`、`gates[...]` 及所有 `*/f`）在同一次运行里**无效**。
- spawn client 整轮只有 `Shutdown` 的**一条终行**（benchmark 模式）或**一条都没有**（snapshot 模式：该模式在快照完成后抛出退出，跳过 `retrace::cleanUp()` → `MobileGL::Destroy()` → `PipeStats::Shutdown()`）。
- `* total ms` 那类**每角色运行总量**包含第 1 帧的 shader 编译（本 fixture 的帧 1 花 **7332 ms** client CPU，其余 250 帧各 7.7–8.5 ms），**不是**每帧率，不得除以 251；它跨臂差 4.5–6.7% 与传输无关，这正是主指标取「尾 200 帧的逐帧值」的原因。

### 12.2 门 7：配对 A/B（两会话）

**会话 1**（boot id `e69d0329-2264-4a2d-9b00-e1a4ffb6154d` → `107f24d3-b1f1-4a97-ab8c-9706d1901f32`）：六臂交错 `monolith, inproc, spawn, inproc, monolith, spawn` × best-of-3，**18/18 runner 退出 0、36/36 pin check `PINNED`、0 `Fatal{`**，12 次 pin check 的 `cpuss-0-0` 温度 37.6–46.5 °C。

| 臂 | 顺序 | wall p50 ms | **client CPU p50 ms/帧** | fps | 臂内散布 |
|---|---|---:|---:|---:|---:|
| monolith (a) | 1 | 11.320 | **11.231** | 86.32 | 1.0% |
| inproc (a) | 2 | 7.971 | **7.880** | 106.04 | 0.3% |
| spawn (a) | 3 | 7.989 | **7.876** | 106.12 | 2.2% |
| inproc (b) | 4 | 8.001 | **7.889** | 105.78 | 0.2% |
| monolith (b) | 5 | 11.287 | **11.240** | 86.26 | 1.3% |
| spawn (b) | 6 | 7.979 | **7.869** | 105.95 | 4.2% |

**会话 2**（boot id `107f24d3-b1f1-4a97-ab8c-9706d1901f32` → `7e6c6e9f-1894-4916-bf39-cce48c9abd37`）：只跑 inproc / spawn 交错 `inproc, spawn, spawn, inproc` × best-of-2（聚焦门 8 ①，见 §12.3），**4/4 runner 退出 0、8/8 pin check `PINNED`、0 `Fatal{`**，温度 37.2–44.9 °C。

| 臂 | 顺序 | wall p50 ms | **client CPU p50 ms/帧** | fps | 臂内散布 |
|---|---|---:|---:|---:|---:|
| inproc | 1 | 8.105 | **7.987** | 105.72 | 0.14% |
| spawn | 2 | 8.116 | **7.992** | 104.43 | 0.34% |
| spawn | 3 | 8.113 | **8.009** | 104.19 | 0.26% |
| inproc | 4 | 8.089 | **7.989** | 103.72 | 0.69% |

**配对差（spawn − inproc）**，两个顺序都列，不挑好看的那个：

| 会话 | 指标 | inproc (a / b) | spawn (a / b) | Δ（均值） | Δ% |
|---|---|:---|:---|---:|---:|
| 1（best-of-3） | **client CPU p50 ms/帧** | 7.880 / 7.889 | 7.876 / 7.869 | **−0.012** | **−0.15%** |
| 1 | wall p50 ms | 7.971 / 8.001 | 7.989 / 7.979 | −0.002 | −0.03% |
| 1 | fps | 106.04 / 105.78 | 106.12 / 105.95 | +0.125 | +0.12% |
| 2（best-of-2） | **client CPU p50 ms/帧** | 7.987 / 7.989 | 7.992 / 8.009 | **+0.0125** | **+0.16%** |

**monolith 基线行**（不属配对，按协议记录）：

| 比较 | client CPU p50 ms/帧 | Δ | fps | Δ |
|---|---|---:|---|---:|
| monolith → inproc | 11.236 → 7.885 | **−29.8%** | 86.29 → 105.91 | +22.7% |
| monolith → spawn | 11.236 → 7.873 | **−29.9%** | 86.29 → 106.04 | +22.9% |

**判读：tie。** 会话 1 的差值（−0.15%）落在臂内散布 0.2–4.2% 之内，且 wall 列的**符号随顺序翻转**（顺序 (a) inproc 领先 0.03%、顺序 (b) spawn 领先 0.03%），这是噪声的特征而不是效应的特征；四个 inproc / spawn 臂的 client CPU p50 都落在 7.87–7.95。会话 2 在同一负载、不同构建与不同 boot 上复现了这个 tie（+0.16%，臂内散布 0.14–0.69%，差值比它取自的臂内散布低一个数量级）。两会话之间移动的是绝对水平（约 0.1 ms），那是运行间漂移，不是传输效应。**两条 transport 在 client 每帧 CPU 上不可区分，两者都比 monolith 约便宜 30%**——spawn 是通过 socket 拿到这个数的，**这条负载的分辨率看不见 socket 门铃的代价**。性能只记录，**不设门**。

### 12.3 门 8① socket 门铃 vs inproc condvar

四个计数器：`srv` / `srvpark` = apply 线程进入 `Doorbell::Wait` 的次数 / 其中停止自旋转为阻塞的次数；`cli` / `clipark` = client 生产者的同一对。**两对都是运行总量**，所以费率除以该轮运行的 `frames=251`，**不是**同一行的 `window=`（见 §12.1）。

**会话 2 的完整账**（这次 spawn 的 server 半边也在——见 §12.6 的修复）：

| 臂 | 传输 | counters 取自 | frames 取自 | `cli frames=` | `srv frames=` | srv | srvpark | cli | clipark | **srv/f** | **srvpark/f** | **cli/f** | **clipark/f** |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 01 | inproc | client log（同进程两角色一行） | client log | 251 | 240 | 2 869 874 | 1 341 | 30 407 | 1 444 | **11433.8** | **5.343** | **121.1** | **5.753** |
| 02 | spawn | client(cli) + server(srv) | server log | 0 | 251 | 2 614 177 | 1 229 | 30 401 | 1 524 | **10415.0** | **4.896** | **121.1** | **6.072** |
| 03 | spawn | client(cli) + server(srv) | server log | 0 | 251 | 2 614 456 | 1 170 | 30 401 | 1 525 | **10416.2** | **4.661** | **121.1** | **6.076** |
| 04 | inproc | client log | client log | 251 | 240 | 3 002 771 | 1 265 | 30 407 | 1 406 | **11963.2** | **5.040** | **121.1** | **5.602** |

**会话 1 的账**（同一个窗口里的另一轮，spawn 的 server 半边当时**采不到**）：

| 臂 | 传输 | srv | srvpark | cli | clipark | **srv/f** | **srvpark/f** | **cli/f** | **clipark/f** |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| inproc (a) | inproc | 2 738 247 | 1 543 | 30 407 | 1 548 | **10909.4** | **6.148** | **121.1** | **6.169** |
| spawn (a) | spawn | **不可得** | **不可得** | 30 401 | 1 538 | — | — | **121.1** | **6.127** |
| spawn (b) | spawn | **不可得** | **不可得** | 30 401 | 1 530 | — | — | **121.1** | **6.096** |
| inproc (b) | inproc | 2 727 377 | 1 281 | 30 407 | 1 399 | **10866.0** | **5.103** | **121.1** | **5.574** |

会话 1 的 spawn 半边不可得是**结构性**的、不是漏采：spawn server 进程当时从不跑 `PipeStats::Init()`（它只从 `MobileGL::Initialize()` 调用，而 spawn server 走的是 `MG_ConfigLoader::Init()` + `MG_Backend::InitServerRoleForSpawn()`），于是该进程里所有 `PipeStats::Enabled()` 门恒为 false，**一条汇总行都没有**——不是携带 0，是整行不存在。client 日志里的 `srv=0 srvpark=0` 因此是「另一个进程」的零。**该缺陷已在工作树修好**（§12.6），会话 2 就是修后的复采。

**逐列读法：**

- **`srv/f`（server 会合率）**：inproc `10 866–11 963`，spawn `10 415–10 416`；折算成每 draw，spawn 两臂各 **7.1**，inproc 两臂 **7.8 / 8.1**（1471.36 draws/帧；会话 1 的 inproc 两臂相应是 **7.4**）。spawn 比 inproc **低 8.9–12.9%**，但这不是传输的胜利：两个计数器数的**不是同一个事件**——inproc 下 apply 线程的 `Doorbell::Wait` 同时是同地址空间内发布 client 队列排空的机制、client 在 verb barrier 后面大段同步，spawn 下同一个循环是被描述符就绪唤醒；每帧两个角色必须达成一致的次数是**handoff 纪律**的性质，不是门铃的代价。可比的是 park 比例，而它两边几乎相同。
- **`srvpark/f`（会合真的阻塞了多少）**：会话 2 inproc 5.343 / 5.040、spawn 4.896 / 4.661；会话 1 inproc 6.148 / 5.103（spawn 不可得）。两个会话合起来即**服务端 5.0–6.2 parks/帧、其中只有 0.042–0.06% 的等待进入阻塞**（会话 2 四臂逐臂 1 341/2 869 874 = 0.047%、1 229/2 614 177 = 0.047%、1 170/2 614 456 = 0.045%、1 265/3 002 771 = 0.042%；并入会话 1 的 inproc 两臂后上界到 0.056%）。自旋预算在两条传输上几乎覆盖全部会合——P5d/P5e 的结论在这里再次成立：代价是**会合本身**，不是唤醒。
- **`cli/f`（client 自己的等待）**：**四个臂上都是 121.1**（会话 1 的四个臂也一样，运行总量 30 401–30 407 次），逐字到十分位。client 两种传输做同样的工作，传输不改变它**多久**等一次。
- **`clipark/f`（client 的 park）**：**唯一 socket 略差的列**——会话 2 spawn 6.07（6.072 / 6.076）对 inproc 5.75 / 5.60，每帧多约 **0.4 次 park**（~121 次等待里的 0.4 次）。client 等待的 **4.6–5.1%** 会 park，比服务端高两个数量级，因为 client 的自旋预算花在一个经常还没到的生产者上。
- **tie 判读**：client CPU 差 **+0.16%**（会话 2）落在 0.14–0.69% 的臂内散布内，会话 1 是 −0.15%；契约 §9 那条注记（「把 ~1600 spins/帧 转成 ~4 次阻塞读，**可能更便宜**」）的观测对象正是 client CPU ms/帧，而它**两个方向都没动**。**结论是诚实的：socket 阻塞读既不更便宜、也不更贵**——该注记的「更便宜」未兑现。注记里的 1600 spins/帧 是更重场景的数，本机 rd12 的参照是服务端 ~10 400–12 000 次会合/帧、client 121 次/帧；park 比例 0.042–0.06%（服务端）与 4.6–5.1%（client）也说明本场景的自旋几乎全都奏效。
- 两条传输**都比 monolith 约便宜 30%**（§12.2），spawn 走的是一条 socket 而不是共享地址空间。

### 12.4 门 8②③：`SEG_STAGE` 字节/帧、分块后记录/帧、`maxrec` 与逐 blob 分布

口径：`MOBILEGL_PIPE_STATS=1` + `MOBILEGL_PIPE_STATS_PERIOD=1`（每个 `eglSwapBuffers` 一行，`window=1` 恰一帧），车道 = inproc + `MOBILEGL_IPC_ROLE_SPLIT_STATE=1` + `MOBILEGL_IPC_STRICT_ERRORS=1` + `MOBILEGL_IPC_RUN_AHEAD=1`，后端 DirectGLES，真实 trace retrace。两个负载：**OpenRA**（`MobileGLTraceReplay.OpenRA.DirectGLES.SPLIT`，SSIM **1.000000**）与 **rd12-odinlite-in-world**（无 SPLIT ctest 条目，按同一命令行手工驱动，SSIM **0.999998**）。新增的两个计数器都落在**唯一的收口点**上：`SEG_STAGE` 字节记在 stage 分配器（`MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:878`，`StageAllocate`），记录数记在记录提交路径（`MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1188`，`EncodeRecord` 成功路径）——所以覆盖是构造性的，不是一张「今天有哪些 producer」的名单。（两处行号取 §12.6 收尾改动后的工作树内容。）这两个数在 **inproc 车道**采集；spawn 下 client 的字节 / 记录类同样在采（它们由 client 的发射器 Add，run total 可信），但 spawn 的**服务端**字节 / 记录类要等 §12.6 的 `ServerMain.cpp` 修复之后才存在。

**framing 警告（两个读数都报）**：`PERIOD=1` 下**第一行覆盖整个启动 / 加载窗**——rd12 的帧 1 有 **981 654 draws**、5 053 895 条记录、`seg=2 317 939 359`，而其余每帧只有约 1 471 draws。所以「窗口求和 / 帧数」是**含加载的均值**；报这个数时必须说清是哪个口径。rd12 的加载窗把 run-wide 均值抬到稳态均值的**约 7 倍**（10.79 vs 1.53 MB/帧）；OpenRA 两个值接近（8.07 vs 8.01 MB/帧）是因为它的加载窗与稳态窗同量级。

| 口径 | OpenRA | rd12 in-world |
|---|---:|---:|
| **`SEG_STAGE` 字节/帧——稳态中位数** | **130.8 KB/帧**（130 848 B） | **816.2 KB/帧**（816 248 B） |
| **`SEG_STAGE` 字节/帧——稳态均值**（丢弃前 10% 窗） | **8.01 MB/帧**（8 012 890 B） | **1.53 MB/帧**（1 534 555 B） |
| `SEG_STAGE` 字节/帧——含启动负载的均值 | 8.07 MB/帧（8 068 255 B） | 10.79 MB/帧（10 792 107 B） |
| **记录/帧（分块后）——稳态中位数** | **211** | **7 497** |
| **记录/帧（分块后）——稳态均值** | **206** | **7 504** |
| 记录/帧——含启动负载的均值 | 196.8 | 27 689.5 |
| 参考：`draws` 稳态中位数 | 30 | 1 471 |
| `maxrec` / `maxcap`（运行总量） | 1808 B / 4 194 304 B（0.043%） | 1808 B / 4 194 304 B（0.043%） |
| 运行总量 `seg` / `wrec`（帧数） | 233 979 394 / 5 706（29 帧） | 2 698 026 763 / 6 922 378（250 帧） |
| `ringwraps` / `ringpads` / `ringwaits` | 0 / 0 / 0 | 61 / 53 / 0 |

读法：

- **两个负载的稳态形状完全不同，这正是必须分开报的理由**：OpenRA 稳态的 `tex` 中位数是 32 768 B 而均值 5.6 MB（少数窗有巨大的纹理上传尖峰），`seg` 的中位数 / 均值因此差 **61×**；rd12 稳态的 `tex` 中位数为 0（多数帧不上传纹理），差 **1.9×**。只报一个数就会掩盖另一个负载的形状。按本文件同类数字的惯例，稳态中位数与稳态均值这对都列出来。
- **`maxrec=1808` 与分块前完全相同，原因不是「分块没生效」而是「分块没有机会生效」**：默认 8 MiB chunk 预算（`MGPipeStageChunkBytes()` = `clamp(segment/4, 4096, segment)`，默认 32 MiB → 8 MiB）在这两条负载上从未被打到，所有 blob 都是原始尺寸。稳态 `seg/wrec ≈ 816 248/7 497 ≈ 109 B/条`，与全局的 390 B/条都在字节级别。§6.3 记的分块前 `maxrec=1808` / `SetVertexAttribDefaults` 因此原样成立；`ringwaits=0`（250 帧一次都没等过 retiredSeq）与 rd12 稳态 `seg` 中位数对 32 MiB 的 `SEG_STAGE` 约 40 帧余量一致。
- **`wrec` 是窗口计数**（`PERIOD=1` 下每窗恰一帧，所以 `wrec` 与 `wrec/f` 同值），且 `PERIOD=1` 每帧印一行 INFO 本身有可观开销——**这些帧数不能当性能数**，只用于按帧摊平字节与记录。
- **交叉核对（能排除什么、不能排除什么）**：`seg` 记 wire 写进 `SEG_STAGE` 的字节，后端的 `buf`/`tex`/`csob-blob` 记往驱动搬的字节，两个总体不同、不要求相等，但应在同一量级且同步起伏。逐窗（250 窗）比对：`seg` 中位数 818 030 / 均值 10 792 107，`parts`（= buf+tex+csob-blob）中位数 291 840 / 均值 3 978 204，均值比值 2.713，每条记录 389.8 B；两者在同一些窗一起抬升、同一些窗一起落底，没有出现「一个动另一个不动」的窗。比值 > 1 有解释（`seg` 稳态恒有 `csob-blob ≈ 281 KB/帧` 的 program archive 只走 wire 不经后端字节类，加上 persistent-map 推送与 tail 字节）。这**不能证明覆盖完整**（没有第三方真值），能排除的是「`seg` 明显偏离同一批字节」这一整类错误。
- **逐 blob 字节分布的首次测量**：答案是「默认配置下该分布就是未分块的原始分布」。取数车道是 **integration**（`ctest -R "^DirectGLES\.(Arena|Ssbo|Buffer|Atomic|Readback)"`，19/19 通过），经 `MOBILEGL_PIPE_STATS_FILE` 的 JSON dump——**retrace 车道拿不到 dump**，因为 snapshot 模式在快照完成后抛出退出、跳过了 `retrace::cleanUp()` → `MobileGL::Destroy()` → `PipeStats::Shutdown()`（§12.1），所以 dump 只能在 benchmark 模式或 integration 车道取。该 dump：**70 records**、`seg=5768 B`，直方图复用了 draw payload 的桶边界（bucket 0 = 0 字节，bucket n>0 = [2^(n-1), 2^n)）。**这里有两个不同的计数，必须写清，否则会被读成同一个数**：**非空 bucket 数 = 8**（落在 bucket 3–12），**blob 条数 = 12**；也就是说「**8 blobs / bucket 3–12**」这句简写里的 8 指的是**有内容的桶数**，而逐桶计数合计是 12 条 blob。逐桶：bucket 3 [4,8) 4 条、4 [8,16) 1 条、5 [16,32) 2 条、7 [64,128) 1 条、9 [256,512) 1 条、10 [512,1024) 1 条、11 [1024,2048) 1 条、12 [2048,4096) 1 条（4+1+2+1+1+1+1+1 = 12）。这批 integration 用例的 `frames=0`（跑 swap 但不走 Present 计数），所以窗口字段无意义，分布本身是**运行总量**（分布没有「窗口」可言）。

### 12.5 负控：`MOBILEGL_IPC_STAGE_MB=1`（256 KiB chunk 预算）

要让分块真正发生，唯一的办法是把 stage 预算人为压小；同一条 rd12 trace、同一个库、只改这一个环境变量的 A/B：

| rd12 in-world，inproc split | `STAGE_MB=32`（chunk 8 MiB） | `STAGE_MB=1`（chunk 256 KiB） | 变化 |
|---|---:|---:|---|
| SSIM | 0.999998213 | 0.999998213 | 不变 |
| **`seg` 运行总量** | **2 698 026 763** | **2 698 026 763** | **逐字节完全一致** |
| `seg` 稳态中位数 | 816 248 | 554 104 | −32% |
| **`wrec` 运行总量** | 6 922 378 | 6 929 736 | +7 358（**+0.11%**） |
| `wrec` 稳态中位数 / 均值 | 7 497 / 7 504 | 7 497 / 7 508 | +0.05% |
| `tex` 运行总量 | 641 567 296 | 1 886 392 128 | **+194%** |
| `buf` / `csob-blob` / `draws` | 见 §12.4 | 与 `STAGE_MB=32` 逐项相同 | 不变 |
| `maxrec` / `maxcap` / `ringwraps` / `ringpads` | 1808 / 4 194 304 / 61 / 53 | 同左 | 不变 |

这是**它自己那个问题的负结果，如实记录**：

- **`seg` 运行总量两侧逐字节相同**说明该计数器测的是**生产者交出来的内容**，与发射侧怎么切、arena 开多大**完全无关**——这正是把落点放在唯一收口点的直接证据：若记在 emitter 侧或记成 arena 占用，这一行就会跟着预算动。
- **稳态中位数 −32% 与运行总量不变同时成立**：同样的总字节被**重新分到不同的窗口**（预算变小后，原本一次完成的上传被切成多次、跨帧重传），所以**逐窗归属会动、总量不会**。读数应以运行总量或明确声明的窗口口径比较，**不要**拿两套配置的单窗中位数直接对比。
- **`wrec` 只涨 0.11%**（+7 358 条 / 250 帧 ≈ +29 条/帧，稳态中位数两侧相同）说明这条 trace 的内容本来就没有超过 256 KiB 的 blob，即使预算压到 256 KiB 也几乎无可切；`maxrec` 仍是 1808，第三次确认（`maxrec` 是单条**记录**的字节上界，blob 被切不改变它）。
- **`tex` +194% 是这次 A/B 真正的发现，但不作结论**：`MOBILEGL_IPC_STAGE_MB` 同时是 **arena 大小**与 chunk 预算的来源，压小它改变了纹理 slab 的切分与重传行为（整宽 slab 被预算切碎后每片各自重传）。这一项**不在门 8 的定义里**，且**没有隔离到单变量**（arena 大小与 chunk 预算被同一个环境变量绑在一起），因此只作为 A/B 的可解释性证据记录，**不作结论**；要分开这两个效应需要把二者拆成可独立配置的量，那是另一个包的事。
- 因此门 8 的「chunking 后记录/帧」取**默认配置**的 §12.4 值。

### 12.6 支撑本节的收尾代码与工具（工作树，未提交）

| 改动 | 位置 | 内容与证据 |
|---|---|---|
| 新计数器 `ByteClass::StageSegmentBytes` | `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:878`（`StageAllocate`） | stage 分配器的**唯一收口**；摘要行 `bytes/f[...]` 里印 `seg=` |
| 新计数器 `CallClass::WireRecords` | `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1188`（`EncodeRecord` 提交路径） | 记录提交路径的**唯一收口**（`Reserve` 成功、正文写完、blob 槽诚实性检查之后、`++m_emitSeq` 之前）；摘要行印 `wrec=` 与 `wrec/f=`，与 `draws/f=` 同形、按 `perFrame` 规则联动 |
| staged-blob 直方图 | `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | 复用 `PayloadBucketOf` 的桶边界，**运行总量**，经 JSON dump 出口（Tracy 画不了分布，与既有 `cmd-bytes-per-draw-histogram` 的选择一致） |
| spawn server 进程从未武装计数器 | `MobileGL/MG_Remote/Server/ServerMain.cpp`，`PipeStats::Init()` 加在 `MG_ConfigLoader::Init()` 与 `MGPipeSetServerProcessRole(true)` **之后**，配一行 `Shutdown()` 在 `loop.Stop()` 之后、`_exit(0)` 之前 | **红一次**：修复前 spawn server 在 `mobilegl.server.log` 里 **0 条**汇总行，修复后 **29 条**；SSIM 1.0 不变。三处顺序都是承重的：`Init()` 从 config latch 该开关（必须晚于配置解析）、晚于定角色（让汇总行进 server 角色日志）、`Shutdown()` 晚于 `loop.Stop()`（apply 线程正是发布 `srv`/`srvpark` 的线程） |
| `MOBILEGL_PIPE_STATS_FILE` 的角色分路 | `MobileGL/MG_Util/{Metrics/PipeStats.cpp,Debug/Log.{h,cpp}}` | 走 `RoleLogPath` **同一套导出规则**派生成 `<base>.client.json` / `<base>.server.json`；**基名不再指向任何文件**（没改的读取方会当场 `ENOENT`，这是刻意的）。G1 上这里红过三次（无条件派生 → `.text +24 B`；两臂共用一个局部 → 1 symbol resized；重写 `MGLOG_W` 格式串 → `.rodata +64 B`），最终形态是 pull 臂的代码**逐语句原样**、所有新增都在 `#if` 之内 |
| 测试 | `MobileGL/MG_Test/Util/PipeStatsTest.cpp` 等 | PipeStats **25/25**；全量 unit **2367/2367**；spawn integration **102/102**；**G1 符号 0/0/0/0 且 `.text` 逐字节相同**（强于门要求的 `+0`），pull 构建零 `MG_Remote` 符号、零新计数器符号 |
| 设备 A/B 工具入库 | `tools/device_bench/p6/{ab_session.py,render_ab.py,README.md}` | 采集（reboot / pin / 按序跑臂 / 逐线程 CPU 采样 / 按角色拉日志 / 臂的证明）与渲染（`session-summary.json` → 表格）分离，渲染器只读采集结果，不能影响采集；`spawn` 的两个结构性零（`srv` 与 `frames` 在 server 日志）由渲染器处理并**在表里标明取自哪条日志** |
| runner 的包名覆盖 | `tools/trace_replay/run_android_retrace_local.py`（+19 行） | `MOBILEGL_TRACE_PACKAGE`；一台机器上已有另一个 key 签的 trace 安装（两个不同 key 签的 APK 不能共用一个 id），所以门 7 / 门 8 的 APK 装成 `top.mobilegl.plugin.p6gate7.trace` / `.p6gate8.trace` 并与既有安装并存，runner 的包名常量由此覆盖；既有安装原样保留 |
| `pin_device.sh` 的 `2f7cbe2e` 行 | `tools/device_bench/pin_device.sh`（+28 行） | 定频路径 + kgsl 节点 + GPU 钳 1050 MHz 的实测值（§12.7）；三个动作（`check` / `pin` / `unpin`）都实跑过，pin 在负载下保持、在 idle 下 90 s 稳定，stock 值是硬编码而非现场采样（否则会把已钉的状态读成 stock 并永久钉住） |

### 12.7 不可比项与未跑项

- **本板 GPU 钳 1050 MHz。** `pin_device.sh` 的 `2f7cbe2e` 行把 kgsl `min/max_pwrlevel` 钉 0，但本板实际拿到的是 **1050000000 Hz（1050 MHz）**：`thermal_pwrlevel` 恒为 1（写 0 返回成功、读回仍是 1），`max_gpuclk` 只读且为 1050000000，devfreq 的 `max_freq` 也把 1100000000 的写夹下来。所以这一行记的是「**本板真正能跑到的最高频**」，不是顶端 OPP，`check` 对着真相比对。**后果**：本节的绝对时间与 **1100 MHz 时代**的 `35d0befa` 行（§3.2 / §4.4）**不可比**，与 §7.4 那一轮 Redmi 的 1100 MHz 口径也不可比——钟频不同，**只有同场配对可比**，本节的配对结论也只在这个意义下成立。
- **OpenRA 真机辅负载未跑。** 门 7 / 门 8 的真机会话只跑 `minecraft-1.21.4-rd12-odinlite-in-world`，OpenRA 只在 WSL 桌面 llvmpipe 的 retrace 车道跑过（§12.4 的两个数之一）。**真机 OpenRA 未跑**，如实记录。
- **门 8① 的两种门铃只在一条负载、一台设备上比过**，且参照注记里的 1600 spins/帧 属于更重的场景；若存在差异，应在每帧会合次数更多的负载上才看得见。
- **spawn client 的窗口化字段仍不可信**，而且这一项是**结构**不是缺陷：要让 spawn client 推进窗口，得让 client 进程到达一次 `PipeStats::OnPresent`，而 client 的帧边界在 spawn 下是 `EmitPresent`（每条 present 记录一次）——接上去会与 backend 的帧边界重复计数。**不要顺手接**。
- **`MOBILEGL_IPC_STAGE_MB` 同时是 arena 大小与 chunk 预算的来源**，所以分块的净效果没有被单变量隔离（§12.5 的 `tex` +194%）。

## 里程碑记录（原 `ROADMAP.md`）

- **P6 收官（2026-09-22）**：a6/c6/lk/so/sm/cp/hs/dl/st/t6 十包于 2026-09-21 合入 `feat/disaggregated`（头 `77cbd176`）；三种传输（monolith/inproc/spawn）桌面与真机两进程 retrace 逐像素对上 golden；device-lost 闩把杀 server 从 123 s 错误 `BarrierTimeout` 变 5.7 s 正确 `DEVICE LOST`；`Session::Fail` 漏斗把裸 abort 93→4。出口门 1–6 ✅；**门 7 / 8 于 2026-09-22 采齐**：真机 reboot-clean 同热窗口配对 A/B 六臂 18/18 runner 退出 0、36/36 `PINNED`、0 `Fatal`，inproc vs spawn 的 client CPU p50 差 −0.15%（第二会话 +0.16%）——**判读 tie**（臂内散布 0.2–4.2%），monolith 基线约贵 30%；门 8 三数齐（门铃 A/B tie、`SEG_STAGE` 字节/帧、分块后记录/帧）；G1 符号 0/0/0/0 且 `.text` 逐字节相同。**P6 就此收官，未设性能门**（只记录）。逐项见 `CURRENT_STAGE_PROGRESS.md`。

## 本目录

| 文件 | 内容 |
|---|---|
| [`a6-audit-rows.md`](a6-audit-rows.md) | a6 审计 — 全部行 |
| [`a6-audit-v1.md`](a6-audit-v1.md) | a6-audit-v1 — spawn 之前，进程/角色边界还差什么 |
| [`a6-link-experiment-data.md`](a6-link-experiment-data.md) | a6 item 5 - link experiment raw data |
| [`a6-link-experiment.md`](a6-link-experiment.md) | a6 第 5 项：server 能否不链 `MG_Impl`——实验结果 |
| [`census-classC.md`](census-classC.md) | P6 class-C dynamic census |
| [`docs-closeout-measurements.md`](docs-closeout-measurements.md) | docs-closeout-measurements — P6 收官事实写入 `MEASUREMENTS.md` |
| [`docs-closeout-progress.md`](docs-closeout-progress.md) | docs-closeout-progress — `CURRENT_STAGE_PROGRESS.md` 的 P6 收官写入报告 |
| [`docs-closeout-roadmap.md`](docs-closeout-roadmap.md) | P6 收官事实写入 `ROADMAP.md` —— 改动报告 |
| [`gate7-device-ab.md`](gate7-device-ab.md) | Gate 7 — Redmi `2f7cbe2e` paired A/B, and gate 8 item 1 (the doorbell) |
| [`gate8-doorbell-device.md`](gate8-doorbell-device.md) | Gate 8 item ① — the doorbell, measured on the device (server half included) |
| [`gate8-wire-counters.md`](gate8-wire-counters.md) | gate8-wire-counters — 门 8 的两个 wire 计数器、首采，与 spawn server 侧的 wait 账 |
| [`git-worktree-fix.md`](git-worktree-fix.md) | MobileGL-disagg worktree：git 修好报告 |
| [`lk-itemisation.md`](lk-itemisation.md) | lk — 开工前的逐文件清单 |
| [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md) | CONTRACT-P6 (DRAFT) — the backend runs in a second process |
| [`P6-ENDSTATE-REVIEW.md`](P6-ENDSTATE-REVIEW.md) | P6 重审：面向 gfxstream 形状的终局 |
| [`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md) | P6 spawn transport — 包计划 |
