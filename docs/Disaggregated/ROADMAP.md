# MGPipe 路线图

> **状态（2026-09-24）**：P0 … P7 已收官（含 P5c–P5f 与 P6）；P6.5 第一波已落地（残余交 CI / 后续）；Ph 的必需小件随 P7 落地；P3b/P4b 余项并行。**当前阶段是 P12**「server 自有屏幕窗口」子集：已实现并入 `feat/disaggregated`，未收官——见 [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md)。
>
> 本页只留摘要。每个阶段的完整阶段表行、逐门数字、设计与报告在 `notes/<阶段>/README.md`（下表"细节"列）；设计见 [`ARCHITECTURE.md`](ARCHITECTURE.md)，实测摘要见 [`MEASUREMENTS.md`](MEASUREMENTS.md)。

## 终局

client 与 server 可以在**不同机器、不同 OS / 架构**上经 **TCP** 连接；同机的 `spawn`（AF_UNIX + 共享段）保留为本机形态。传输栈是控制面（`ITransport`）× 数据面（`ILink`）两根独立可选、握手协商、可混搭的轴，上层不按链路种类分支（[`ARCHITECTURE.md`](ARCHITECTURE.md) §11.9）。这一改判（2026-09-22，取代此前 AVF pVM / `AF_VSOCK` 的设想）让 P6.5 成为 IPC 跑道的关键路径，并让 Ph（不可信对端加固）成为非 loopback 监听的前提。

## 通用纪律（每个 commit）

- 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）。
- **每个门必须能因它存在的理由变红**：带阴性对照并真跑过一次红（R-16）；公共 GL 看不见的改白盒断言。
- 正确性门是硬门；**性能对着 pull 臂只记录、不作阻塞门**（用户 2026-09-08），但数字必须真的采到。专门的优化阶段排在路线图推完之后。
- 每阶段出口跑一次五部分门（[`ARCHITECTURE.md`](ARCHITECTURE.md) §13.2）；性能判据是**逐线程 CPU 时间**；设备对比走 reboot-clean + 同热窗口配对 A/B，只用 Redmi `2f7cbe2e`（[`devices/pin-verification-2026-09-07.md`](devices/pin-verification-2026-09-07.md)）。
- G1：pull 构建的符号集与 `.text` 对基线恒等（每阶段 0/0/0/0）；G2：pull 与 push 的 ctest 名集合相同；G14：测试名只增不删；G5：保护区字节一致。
- **拆分不借机顺手修 `dev` 的 bug**：发现的 `dev` 侧缺陷记成独立条目、独立 PR。
- 过程（用户 2026-09-16，ID-66）：不过度验证；每个阶段收官由异模型族审一次，发现进入下一阶段首轮；实现方与审查方错开模型族。

## 两条跑道

- **monolith 跑道**：P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13。每段可独立交付、可随时中止；接口本身即使 IPC 永不上线也有价值。
- **IPC 跑道**：P5 → P5b → P5c → P5d → P5e → P5f → P6 → P6.5 → Ph → **P12** → P9 → P10 → P11。顺序的理由：先有完整的独立渲染线程（P5b），再让 `inproc` 成为只经 wire 交换的诚实两角色（P5c / P5f），把 CPU 代价压到可接受（P5d）并退役 draw path 的 lockstep（P5e），此时 P6 才只是传输替换；之后传输栈两轴化并跨机（P6.5）、能安全接受外来连接（Ph）、server 成为拥有显示的常驻应用（P12）；异步反向通道与节奏（P9、P10）建在 P6.5 的消息式 reply 之上；同机 ≥16 MiB 采纳（P11）最后。

## 阶段

| 阶段 | 状态 | 摘要 | 细节 |
|---|---|---|---|
| **P0** 卫生、度量、门、骨架 | ✅ 2026-09-05 | 边界计数器、调用目录 + 生成器 G1–G7 + CI `pipe-gates`、`MG_Remote` 骨架；spike A（应用进程 exec 第二个原生可执行文件）两台设备成立；spike B：只有 T0（AHB 交接）在两设备两后端完整读写 | [`notes/p0`](notes/p0/README.md) |
| **P0.5** 值头与制品头抽取 | ✅ `5d99ee43` | `MGPipeValueTypes.h`、`ProgramArtifacts.h`、include 闭包门、符号报告；`.text` 不变 | [`notes/p05`](notes/p05/README.md) |
| **P1** `PipeInputs` 与 verify harness | ✅ | 277 + 58 处读点换成 `MGB_CTX`；逐 verb 填充与 poison；G4 影子比对；verify 818 / 79 trace 零分歧 | [`notes/p1`](notes/p1/README.md) |
| **P2** 渲染状态 CSO + Track H 首片 | ✅ `738b289d` | Tracker、render-state CSO + 动态状态、`CsoCache`、残余块 1248 → 8、11 条 memo 删除；**GO/NO-GO：继续**（Release 边界代价 +6–12%，已接受） | [`notes/p2`](notes/p2/README.md) |
| **P3a** 句柄 wave 1：buffer、VAO | ✅ `fde5fda3` | `resource_*` / vertex-input 句柄化；五部分门全绿、G1 0/0/0/0；1 天 | [`notes/p3a`](notes/p3a/README.md) |
| **P4a** 句柄 wave 2：FBO / 纹理 / sampler / program | ✅ `8c458cd5` | 十四族接线、六种 kind 重键、`CompositeResolver`；全门绿；1 天；Redmi 上 P4a 自己 +3.4–5.7 pt（Espryt） | [`notes/p4a`](notes/p4a/README.md) |
| **P5** 传输 + inproc applier + 发射表 | ✅ `ff2994d9..37fc4fdb` | `InProcessTransport`、四种 flavour、lockstep verb barrier、persistent-map 块推送、G8；OpenRA inproc SSIM 1.0；收尾全门停在 E3(a)，审查发现转 P5b | [`notes/p5`](notes/p5/README.md) |
| **P5b** inproc verb 迁移 | ✅ `37fc4fdb..82683d4a` | 43 槽 + sync / blit / mip 迁移，发射表 A=2 / B=54 / C=15；`integration-split` 107/107；**Redmi 首次以独立 apply 线程渲染四条目标 trace**，barrier tax +5.9–18.2% | [`notes/p5b`](notes/p5b/README.md) |
| **P5c** 共享内存读点归零 | ✅ `11ac3de6..b88e8487` | 审计出的 59 处直接访问归零：纹理 staged shadow、`SEG_EVENT` 反向通道、按句柄解析、控制记录、角色守卫；契约 `CONTRACT-P5C.md` | [`notes/p5c`](notes/p5c/README.md) |
| **P5d** inproc 性能专项 | ✅ `1f8de61b` | VD12 Minecraft 上 inproc 7-13 → 103-106 fps，client 线程 15.0 → 9.2 ms/帧 | [`notes/p5d`](notes/p5d/README.md) |
| **P5e** 退役 draw path 的 lockstep（run-ahead） | ✅ 2026-09-19（附具名未决：E1 对照需重新定义，开放问题 24） | 规则 F、`WaitClass` 列、present credit；十二包三波；strict 179/179；VD32 下 inproc 与 monolith 齐平，run-ahead 相对 lockstep p50 +69% | [`notes/p5e`](notes/p5e/README.md) |
| **P5f** 一切状态上 wire | ✅ 2026-09-20 | 字段归属 41 / 6 / **0 BARRIER_PULLED** / 16；双块演练；双后端逐帧 `rsp=0`；Redmi 六个 clean-boot 臂通过。后续：Magma run-ahead（`194382c9`）、CI / inproc 功能补齐（`e1bf3677`，2026-09-21） | [`notes/p5f`](notes/p5f/README.md) |
| **P6** spawn transport | ✅ 2026-09-22 | 十包（a6 … t6）；`integration-spawn` 102/102；device-lost 闩与 `Session::Fail` 漏斗；门 7 真机配对 A/B **tie**（inproc vs spawn ±0.16%）；门 8 三个强制性能数已采 | [`notes/p6`](notes/p6/README.md) |
| **P6.5** 传输栈两轴化 + 跨机 | 第一波已落地（`fe28bdb5`），未宣布收官 | 全 TCP 控制 + `StreamLink` 数据，WSL client ↔ Redmi server；`wireFingerprint` / `Refuse` / 令牌；device-102 102/102 全 run-ahead ARMED。**残余**：39 例 device golden 矩阵与链路必测数；第二波（同机 TCP + 共享段）未开工 | [`notes/p65`](notes/p65/README.md) |
| **Ph** 不可信对端加固 + 配对 | 必需小件已落地（随 P7 F / F2，ID-P7-57） | 每会话闩、常量时间令牌、`dataNonce` 数据面绑定、预认证上限与退避、`SEG_EVENT` 弃投闩、fuzz 三臂进 CI；OQ-21 裁定令牌足够、不做 TLS | [`notes/p7`](notes/p7/README.md)（Ph 行） |
| **P3b / P4b** Espryt 深化 | D1 / D2 / D3 已落地，余项并行 | XFB 两洞、回调 10 → 9、上传形状成门、view 陈旧缺陷修复。余：R-3 另一半、`texture_view.coherency` 的 split 读数、CTS AFTER | [`notes/p34b`](notes/p34b/README.md) |
| **P7** DirectVulkan（Magma）全量迁移 | ✅ 2026-09-23（ID-P7-63） | §8 九项全绿：Magma 两进程三臂、`@P7` 拒绝归零、`StateObjectDeathOps`、verify × split、门 3 真机 ssim 36/36、门 5 CTS 五块 ≤ 0.5 pp；链接棘轮 186 → 171 | [`notes/p7`](notes/p7/README.md) |
| **P12** Android 生产窗口路径 | **当前**：子集已实现、未收官 | server 自建窗口上屏、`WindowKind::ServerOwned` 无头 client、失窗 device-lost；真机七项检查已过（审查轮后待复测）。出口门 (a) FCL + 杀 server、(b) 跨机 TCP 入世界均未打 | [`notes/p12`](notes/p12/README.md) |
| **P8** emulation 下放 + 索引宿主镜像 + 协议广度 | 待排 | client 侧 `HostResolve`、multi-draw client indices、CopyImage 镜像、mip CPU 回退、`texture-remint-pull` 下放；索引镜像按真实消费者重裁 | [`notes/p8`](notes/p8/README.md) |
| **P9** 反向通道 | 待排（P6.5 之后） | 异步 reply、chunked readback、PACK-PBO fire-and-forget、武装剩余回调、XFB 命名空间 | [`notes/p9`](notes/p9/README.md) |
| **P10** sync / query / present 节奏 | 待排 | 轮询入口成门铃点、逐 fence 退休、`PRESENT_CREDIT > 1` 实测、roundtrip 计数器、`SetSwapInterval` | [`notes/p10`](notes/p10/README.md) |
| **P11** persistent map 与 ≥16 MiB 采纳 | 待排（同机臂专属） | T0 / T1 档、`SEG_ADOPT`、`gPipeInputs` 版本化 | [`notes/p11`](notes/p11/README.md) |
| **P13** 退役 pull 路径 | 待排 | 删 `MGB_CTX` / `MOBILEGL_PIPE_PUSH` / legacy memo 臂 / 残余块；recorder；模块边界 | [`notes/p13`](notes/p13/README.md) |

CTS 周转单独计价（`gl44to46` 约 56,271 例）：逐阶段只跑该阶段可能影响的具名块；完整 caselist 只在架构边界与合并 `dev` 之前跑，放 CI 不放关键路径。

## 里程碑

- **2026-09-08**：P2 出口 GO/NO-GO 判定继续；P3a、P4a 各 1 天，未触发重定基线。
- **2026-09-16**：P5 出口（首个 IPC 帧，全门停在 E3(a)）；P5b 出口（Redmi 正确性 8/8，barrier tax 首测）。
- **2026-09-17 / 18 / 19**：P5c 同日收官；P5d 三轮；P5e 收官（run-ahead 武装）。
- **2026-09-20 / 21**：P5f 收官；CI / inproc 功能补齐收官（`e1bf3677`）。
- **2026-09-22**：P6 收官；终局改判为 TCP 跨机 + 两轴传输栈；P6.5 第一波落地；P7 wave 2 落地（B3 找到 OpenRA 真机分歧的真因）。
- **2026-09-23**：P7 收官。
- **2026-09-24**：P12 子集并入，未收官。

仍是方向、不据此伪造日历：全功能 split（P8 之后）、纯度门在非 verify 构建上转绿（P13）。各里程碑原文在对应的 `notes/<阶段>/README.md`。

## 仍开放的债务

| 债务 | 去向 |
|---|---|
| `SEG_REPLY` 2 MiB 单槽 payload cap、PACK-PBO 回读的 fire-and-forget | P9（ID-47、ID-57）；stream 链路上 cap 变成 `maxReplyBytes` 窗口 |
| 未接入内容分块的 record 类型：program archive 与 `draw_vbo` range 尾 | P6.5 sl 与 P8 共用的分片（开放问题 11） |
| class-C 余项与仿真路径：`SetSwapInterval`（P10）、`DeleteTransformFeedback`（P9）；multi-draw client indices、RGB CPU mip、`texture-remint-pull` 等具名拒绝 | P8 / P9 |
| `GetCaps` 的两个 blobref | 一旦运输，必须有 server → client 的 carrier rule，不能套 `SEG_STAGE` |
| E1 对照的重定义（ID-122） | 无主（开放问题 24） |
| `MOBILEGL_IPC_IDLE_EXIT_S` 未解析；`CONTRACT-P6.md` 仍把 `DynamicBackendParameters` 定宽记在 P7 名下（已由 P6.5 wf 落地） | 文档 / 小修 |
| P12 文档债：`MG_Remote/CONTRACT-P12.md` 未写；`protocol.fbs` 的窗口注释仍是旧说法 | P12 收尾 |
| 树外脚本：普查跑器、`wsl_p5_gate.sh` 等在 `~/w7/notes/`，git merge 不会传播 | 需要时入库 |
| 临时 CI trigger | 合入 `dev` 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器 |

已关闭的历史债（P5 的 27 个 wrong-answer、`rsp` 残余读、P5b 的 inproc 依赖、ABI 指纹、默认 staging 装不下 128 MiB 上传、184 符号棘轮 CI 门）及当时的完整债务表见 [`notes/p5b/README.md`](notes/p5b/README.md) 末节。

## 开放问题

每题的完整陈述与已答部分的出处在 [`notes/OPEN-QUESTIONS.md`](notes/OPEN-QUESTIONS.md)（编号稳定）。

| # | 问题 | 状态 / 去向 |
|---|---|---|
| 3 | spike B 在 `untrusted_app` 域复核 | P11 前做；只对同机臂的 T0 / T1 有意义 |
| 6 | 具名 UBO host payload 的形状（D-B8） | P7 走了直绑常驻 `VkBuffer` range 的备选，欠一次测量与裁定 |
| 7 | `MG_Util` 的 Transpile-vs-Reflect 切割缝 | 未审计 |
| 9 | viewport-array 回放能否塞进一次 `draw_vbo` | P8 |
| 11 | `SEG_STAGE` 的上限（未分片的 program archive 与 range 尾；MC in-world 的占用分布） | P6.5 sl / P8 |
| 12 | P13 之后 split-only 渲染 bug 的 server 侧第二意见 | 开放 |
| 13 | 烘焙内部 shader 的表达力 | 颜色 blit、深度 mip、MS resolve 已答；直通 TCS 未盘点 |
| 14 | 推送模型改变了哪些缓存命中率 | P13 重调 |
| 15 | monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` | 独立 `dev` 问题 |
| 16 | 索引宿主镜像的实际内存占用 | P8 |
| 17 | `create-indirect` 在 Adreno 830 失败、rd12 + Magma 的 `scudo` 崩溃 | `dev` 侧既有问题，排除在设备 A/B 之外 |
| 18 | split 两侧的负载平衡（VD32 下 client 是瓶颈，apply 有余量） | 路线图推完后的优化阶段 |
| 19 | 链路带宽预算与压缩 | 等门 8 分布与跨机实测 |
| 20 | 大单次上传在慢链路上的停顿 | 跨机臂上记录，或随 P9 异步化 |
| 22 | client 平台范围（Windows client 第二批、macOS 不拆分） | 开放 |
| 24 | E1 对照的重定义 | 无主 |

已答：1（client dirty 走查的代价，P2–P4a）、2（纹理重铸拉取率，可忽略）、4（渲染状态 wire 粒度）、5（无存储的 capability）、8（反射归档服务三个消费者，P7 C 补齐 `storageBlocks`）、10（`kCapResidentSubData` 由 server 发布，P7 C）、21（令牌足够、不做 TLS，ID-P7-3）、23（半开连接由 TCP keepalive / 超时在传输层报告为挂断，P6.5 ct）。
