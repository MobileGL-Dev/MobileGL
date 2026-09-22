# 当前阶段进度 — P6：backend 跑在第二个进程里

> **这份文档只记当前阶段（P6）。** 已收官阶段（P0…P5f）的逐门数字与证据在
> [`ROADMAP.md`](ROADMAP.md) 的阶段表与各 `P5*` 报告里，不在这里重复。规范契约见
> [`MobileGL/MG_Remote/CONTRACT-P6.md`](../../MobileGL/MG_Remote/CONTRACT-P6.md)；设计见
> [`ARCHITECTURE.md`](ARCHITECTURE.md)。

**状态（2026-09-21）：P6 全部包已落地（a6…t6），出口门第 1–6 项达成；第 7 项（真机配对 A/B
热窗口）与第 8 项（三个强制性能数）尚未正式采集，因此 P6 记为「实现完成、出口性能/设备数待补」，
未宣布收官。** 分支 `feat/disaggregated`，头 `16bfab10`，已推 origin。三种传输形态
（`monolith` / `inproc` / `spawn`）在桌面（WSL lavapipe）与真机（Redmi `2f7cbe2e`）上都跑通了
真实 trace 的两进程 retrace，逐像素对上 golden。

---

## 1. 包状态

契约把 P6 切成 a6（审计）→ c6（契约）→ lk / so / sm / cp / hs / dl / st / t6。全部已落地：

| 包 | 落地什么 | 状态 / 证据 |
|---|---|---|
| **a6** | 进程装配与链接边界的核验；198 行审计；184 符号的链接实验（server 不能不链 `MG_Impl`） | ✅ `785fed0b`；[`notes/p6/a6-audit-v1.md`](notes/p6/a6-audit-v1.md)、[`a6-link-experiment.md`](notes/p6/a6-link-experiment.md) |
| **c6** | 规范契约（21 条裁定）+ 数据面缝 `Transport/ILink.h` / `StreamLink.h`（声明不接线） | ✅ `0bbe41c2`；`CONTRACT-P6.md` |
| **lk** | `RingControl` 按写者重组 + 可证伪的 per-field 断言；数据面缝的 pre-flight | ✅ `a54ae47a`、`f4cbaa89` |
| **so** | `SocketTransport`（AF_UNIX SOCK_STREAM + SCM_RIGHTS），12 条 ITransport 契约用例 | ✅ `a3518361` |
| **sm** | 两个**独立启动**的进程（非 fork），靠 name 汇合、共享匿名段；D1c 的六谓词里 D10 需要的三个，四个「编进来却永不武装」的守卫接线 | ✅ `e156ea5f`、`207c31f6`、`743d1fac` |
| **cp** | EGL 控制帧跨进程并应答；`InitCapabilities` 上线（wire kind 11）；真实 trace 两进程 retrace 逐像素通过 | ✅ `b0f4f8b7`、`ea9fc7fa` |
| **hs** | build stamp 从「静默空串」变成显式 present 标志入指纹；客户端补检 `abiMajor/Minor`；`Hello::pid` / `Welcome::serverPid` 携带真实 pid | ✅ `570ebde9` |
| **dl** | device-lost 闩（取自描述符的挂断，绝不取自超时）；D5b「alive-but-silent」具名诊断；`MOBILEGL_IPC_RESPAWN` 具名拒绝；`Session::Fail` 漏斗（`FatalFamilies.def` 词表 + 90 站点归一 + `SessionFault` 帧向对端命名家族）；Fatal 普查门 | ✅ `f6a0bd30`、`e7663512`、`e50463dc`、`f98f2067`、`5968863f`、`91d0cb78`、`16bfab10` |
| **st** | fb-slot memo 的具名 Fatal（`nullptr==nullptr` 读成缓存命中、返回未填槽的潜在崩溃，sm 关掉可达性、st 出声） | ✅ `743d1fac` |
| **t6** | `integration-spawn` 车道（102/102）；集合一致做成结构性（一个宏两条臂）+ 校验门；per-role 日志 | ✅ `8cd00f42`、`b3155525` |

配套：CI 的 `retrace-split` 从「1 case × 1 后端 × inproc」扩成「全 CI 矩阵 × 两后端 × inproc/spawn」，
`split` 键改成 opt-out；Android CI 新增 `spawn-acceptance` 臂，真机 App 从 `nativeLibraryDir` 解析
server 路径（`94e2f2e0`、`15bd9a67`、`1039502e`、`2afa0002`）。

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
| 7 | **设备**：Redmi reboot-clean、同热窗口配对 A/B | ⏳ 功能已验（下 §3），正式配对 A/B 热窗口性能跑**未采集** |
| 8 | **性能只记录**，但三个数强制：socket 门铃 vs condvar、`SEG_STAGE` 字节/帧、chunking 后记录/帧 | ⏳ **未采集**，`MEASUREMENTS.md:342` 仍说 chunking 后分布不存在 |

负控（各跑红一次）：S1 不可解析镜像（sm）、S2 kill server（dl）、S3 未洗环境（sm）、S5 窗口 token
到达（cp）、S6 `dataPlane=StreamOnly`（hs 的 Refuse）、S7 裸 abort（dl 普查门）、S8 `SessionFaultCount()==0`
整轮（t6）。

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

---

## 4. 日志按角色分文件（本阶段的一处基础设施改动）

spawn 的两个进程原先 O_APPEND 追加同一个 `MOBILEGL_LOG_FILE_PATH`（正确但两个会话混在一处）。现在
库按**调用线程角色**写 `<base>.client.log` / `<base>.server.log`（inproc 下 apply 线程即 server 角色，
同样分开）。基名不再指向任何文件——这是刻意的：没跟上改名的读取方会「文件不存在」当场报错，而不是
半读通过。34 个消费方全部改到派生路径；拒绝普查读两份；派生规则由库 `RoleLogPath` 导出，不复制。
G1 守卫下 pull 构建不变。

---

## 5. 已知未决 / 明确不属于 P6

- **出口门 7、8**（真机配对 A/B、三个性能数）——待采集，见 §2。
- **`DirectVulkan` 真机分离路径分歧**——Magma × 真实 GPU，非 P6。
- **P6.5**：数据面流式（`ILink`/`StreamLink` 已声明未接线）、AF_VSOCK、`SEG_STAGE` 送窗口复活。
- **Ph**：让 `Session::Fail` 可返回的策略翻转（需在每个依赖 `[[noreturn]]` 的站点造真实返回路径）。
- **P12**：真窗口跨进程（`ANativeWindow*` 是客户端进程内指针，`SetWindowHandle` 至今具名拒绝）。
- **seq/op 未穿 SessionFault 帧**：90 个站点不逐一穿线，消息本身已带 op，帧只带 code/family/message。
