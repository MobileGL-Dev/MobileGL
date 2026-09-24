# P5b — `inproc` verb migration（`37fc4fdb..82683d4a`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：以 class-C 动态普查决定顺序（Minecraft 优先）；d1 19 / i1 7 / t2 6 / f1 11 槽迁移 + sync 五槽 + 具名 blit + GLES mip descriptor；r1 / r2 收掉 P5 收官审查；发射表 A=2 / B=54 / C=15。
- 门：主机全门 `348d22a4` complete；`integration-split` 107/107；合并 inproc 普查零回退；79 trace 72 / 6 / 1。
- **Redmi 出口**（`82683d4a`，APK `p5bcodex2`）：正确性 8/8——四条 A/B trace 双后端首次在设备上以独立 apply 线程渲染；**barrier tax 首测**（split − push 逐线程 CPU p50）+5.9% – +18.2%。
- 唯一收官审查 0 blocker / 2 major / 1 minor，定向修复。本页末尾归档了 P5 / P5b 出口时记下的债务表（2026-09-22 口径）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5b** `inproc` verb migration
- **状态**：✅ `37fc4fdb..82683d4a`
- **落地什么 / 范围**：class-C 普查决定顺序；d1 19 / i1 7 / t2 6 / f1 11 槽迁移；sync 五槽；具名 blit；GLES mip descriptor；r1 / r2；收官审查三项修复。发射表 A=2 / B=54 / C=15
- **验收门 / 证据**：主机全门 `348d22a4` complete；`integration-split` 107/107；合并普查零回退、79 trace 72/6/1；**Redmi 正确性 8/8 + 四臂 A/B、barrier tax 首测**。§7

## 实测：P5b（主机门 `348d22a4`，设备源头 `82683d4a`）（原 `MEASUREMENTS.md` §7）

### 7.1 迁移与包证据

`348d22a4` 含 d1 / i1 / t2 / f1、r1 / r2、五槽 sync、具名 blit 与 GLES mip storage；发射表 A=2 / B=54 / C=15（目录 76 条，槽数与 opcode 数不是同一统计量）。

| 包 | 已落地行为与证据边界 |
|---|---|
| d1 | 19 个索引 / 实例 / multi-draw / indirect 槽经 `draw_vbo` 过线；独立包普查 77 个 Minecraft 后端用例 28 passed、49 first blockers，全部越过原 draw 首阻塞（默认 stage 32 MiB） |
| i1 | 七个 image / compute / barrier / copy-image / storage-block 槽；原首阻塞 239 归零；CopyImage 的 client-shadow 镜像被跳过，`GetTexImage` 前端回退仍可能读旧 shadow |
| t2 | 六个 XFB / 曲面细分槽；原首阻塞实测 140（`BeginTransformFeedback` 95 + `PatchParameteri` 43 + `BindTransformFeedback` 2）；包内基线 490 / 191 / 390 / 78，原 432 passes 全保留 |
| f1 | 11 个 clear / copy / mip 槽；原 34 个首阻塞归零；bound named-clear 接线，unbound 仍具名拒绝 |
| r1 / r2 | P5 收官 #1–13；r1 定向 10 passed / 0 skipped；client-only push suppression 两个像素控制变红 |
| 具名 blit | `BlitNamedFramebuffer` 经 scoped read/draw binding 发布，退出恢复公开绑定；两后端四个 split 像素 entry 通过 + 15 unit |
| GLES mip storage | server 只验证 applier descriptor 的 Levels/extent；三个 mip 像素控制 3/3；iris-BSL GLES 单例 SSIM 0.997496（默认 stage 32 MiB）；registry 只 Find 不 mint（barrier 债）；RGB 三通道 CPU fallback 保持 Fatal |
| sync | 五条现有 opcode 接线，wire 只传句柄，native fence 归 apply 线程；原 LOCAL guard 使 trace 的 `FenceSync` 没真正执行，所以 d1 旧普查里没有可扣除的 FenceSync 首阻塞数 |

包报告：`~/w7/notes/p5b/p5b-results/{d1-codex-v1,i1-v1,t2-codex-v1,f1-v1,r1-codex-v1,r2-v1,blit-codex-v1,mip-codex-v1,sync-codex-v1}.md`。

### 7.2 主机收尾门与合并普查（`348d22a4`，`~/w7/p5b-final-host-348d22a4/`；同名重跑 @ `3c80cd62` 见本节末）

| 车道 | Selected | Passed | Skipped | Failed |
|---|---:|---:|---:|---:|
| unit-linux / unit-push / unit-verify | 1817 × 3 | 1500 / 1782 / 1795 | 317 / 35 / 22 | 0 |
| unit-split | 2132 | 2122 | 10 | 0 |
| gpu-linux-monolith / gpu-push-monolith / gpu-split-monolith | 1148 / 1148 / 1178 | 895 / 953 / 953 | 253 / 195 / 225 | 0 |
| split（`integration-split`，inproc） | 107 | 105 | 2 | 0 |
| verify / audit | 950 / 10 | 804 / 9 | 146 / 1 | 0 |

G1 27,814 符号 0/0/0/0、`.text` 10,806,611 → 10,806,611；G5 两族 + pin 自测；生成器 / 纯度检查；G2 / G14 rc=0；E1 / E3(a)（精选 4 case）rc=0；E2 OpenRA 2/2、draw-drop SSIM 0.000036 / 758 records、pull-library 拒绝与库 SHA 恢复一致；smoke 16 core + 12 private。retrace-push **79/79**；retrace-verify 在 WSL 重启前完成 4/79，其余 75 条以 SHA-256 钉住的同一 verify 库串行续跑，合并 **79/79**；对账无缺步、无非零退出（`reconciliation-20260916.md`），`complete=true`、exit 0。一次 shell 异常（追加 runner 行触发 `-test-dir: command not found`）保留在原始输出，不冒充测试失败。

**P5b 基线。** 合并 inproc 普查（`~/w7/p5b-final-census-348d22a4/`）：integration lane 显式 32 MiB，**1267 selected = 811 passed / 203 skipped / 62 aborted / 191 failed**；旧 1149 名全部保留、新增 118；旧 432 passes 全保留、零回退；旧 505 abort → 265 passed / 18 skipped / 58 aborted / 164 failed；旧 27 failed → 1 passed / 26 failed。完整 trace 显式 256 MiB（容纳单次 128 MiB 上传，不是默认容量修复）：**79 = 72 passed / 6 aborted / 1 failed**（主跑止于 73/79，三轮续跑补齐；`create-indirect` DirectVulkan 在 llvmpipe 上内存膨胀 >60 GiB RSS 被守护杀死，两次复现，记 failed）。7 条未过项首阻塞：`rd12` DirectGLES `Fatal{InitialBytesNotCarried,"resource_respecify"}`、DirectVulkan `Fatal{BarrierTimeout,"Present"}`；`iris-photon`、`iris-derivative`、`create-indirect` 三个 DirectGLES `Fatal{UnmigratedEmulation,"texture-remint-pull"}`；`iris-bsl-esc-menu-854` DirectGLES `InitialBytesNotCarried`；`create-indirect` DirectVulkan 内存守护。通过项含 `improved-transparency-minecraft-26.3` DirectGLES SSIM 1.0 / DirectVulkan 0.999914、`iris-iterationrp` DirectVulkan 0.995833、`iris-bsl-esc-menu-854` DirectVulkan 0.998402。逐 trace 见 `joint-codex-v1.md` 的 census 块与 `identity.json` / `counts.json` / `trace-transitions.json`。

**同名重跑 @ `3c80cd62`（P7 wave 3 包 E1，2026-09-22，WSL `~/w7/p7-census-e1`，lavapipe）。** 方法逐字沿用上段普查的 helper（`~/w7/p5b-final-census-348d22a4/artifacts/census-helper.py`）：取 monolith 条目的命令与 ENVIRONMENT，`MOBILEGL_IPC_STAGE_MB=32` 与 `MOBILEGL_ITEST_REQUIRE_GPU=1` 作环境底、条目自己的 ENVIRONMENT 覆盖之，再强制 `MOBILEGL_TRANSPORT`、私有 `MOBILEGL_LOG_FILE_PATH`、tcache 关，状态映射相同；名单 = 上段 1267 个名字（头上 0 个缺失）。跑器 `~/w7/e1bin/{wa27,walane}.py`，证据 `~/w7/e1-census/{wa27,lane-inproc2}-3c80cd62/`；lane 普查单并发墙钟 426 s。

| 读数 | P5b `348d22a4` | 头 `3c80cd62` | delta |
|---|---|---|---|
| 27 个 wrong-answer 同名，inproc | 1 passed / 26 failed | **24 passed / 3 aborted / 0 failed** | 23 转绿；3 条由错答变为具名停止 |
| 同上，spawn（P5b 未测） | — | 24 passed / 3 aborted / 0 failed | 与 inproc 逐名相同 |
| 合并 lane 普查 1267 同名，inproc | 811 / 203 skipped / 62 aborted / 191 failed | **1051 / 172 / 37 / 7** | failed→passed 187、aborted→passed 62、skipped→passed 31；passed→非绿 40、failed→aborted 4 |
| trace（CI split 子集），inproc | 79 = 72 / 6 aborted / 1 failed（含 `rd12` × 2，256 MiB） | **77 = 77 / 0 / 0**（默认 32 MiB；spawn 同为 77 / 0 / 0） | `rd12` 已 `ci: false` 出分母；其余 7 格全活（[`notes/p7/iris-census-3c80cd62.md`](../p7/iris-census-1135c664.md) §6） |

27 条按 §6.2 的家族：

| 家族 | 条 | 头（inproc = spawn） | 机制 / 读法 |
|---|---:|---|---|
| `LayeredAttachmentShapeScenario` | 14 | **14 绿** | 未二分到提交；DirectVulkan 的 7 条在 `integration-magma-full-{split,spawn}` 的同名注册条目上也绿 |
| packed depth/stencil `GetTexImage` | 3 | **3 绿** | DirectGLES `Split/Spawn.ForcedDs` 与 DirectVulkan `Split/Spawn.Full` 的注册条目同绿 |
| framebuffer `HandleRecycleScenario` | 5 | **2 绿**（DG `Handles`、DV `AbaControlHandles`）/ **3 具名停止** | 停止的 3 条都是 `integration-monolith-control` 里 `MOBILEGL_PIPE_PUSH=0` 的对照臂：DG `Legacy` → `Fatal{PipeLegacyMemosDisabled, "MOBILEGL_TRANSPORT is not monolith and kMGPipeSubsystemSamplers (bit 11) is clear …"}`（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:4289` 经 `StopOnArmlessPipeSubsystem`，`MobileGL/MG_Backend/DirectGLES/Managers.cpp:2915-2918`）；DV `Legacy` / `AbaControl` → server `Fatal{UnmigratedVerb, "Magma:clear-framebuffer-record"}`（`MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:8008`：push 全关时 transport 下没有 framebuffer record），spawn 臂 client 另记 `Fatal{ReadbackDeclined, "ReadPixels"}`。旋钮矛盾的响亮停止，不是错答；push 打开的两条同名兄弟全绿 |
| `PrimitivesGeneratedNoXfbScenario` | 3 | **3 绿** | DV `Split/Spawn.Full` 的 `CountsADrawMadeWithNoCaptureSpan` 同绿（`TheRerouteIsActuallyArmed…` 在 full 车道因不钉旋钮而 skip，按设计） |
| `TextureParamsWithoutASamplerView` | 1 | **绿** | DG `Split` / `Spawn` 注册条目同绿——ROADMAP 债务表的「待核」关闭 |
| `P4aFinalFixScenario` FBO/RBO delete | 1 | **绿** | 与 P5b 同 |

lane 的 44 条非绿（37 aborted + 7 failed）**全部是 `integration-monolith-control` 条目、且自己的 ENVIRONMENT 钉了非默认 `MOBILEGL_PIPE_PUSH`**（0 / 0x7f / 0x1ff / 0x5ff / 0x9ff / 0x1fff / 0x8000000000001fff）——普查方法把 monolith 对照臂也强制上了 transport，这 44 条读的是「这组旋钮在 split 下没有臂」：35 条具名停止（18 × `Magma:clear-framebuffer-record`、17 × `PipeLegacyMemosDisabled`），2 条 `Fatal{UnmigratedPipeInput, "GetBufferBindingPoint@DispatchCompute"}`（掩码 0x1fff），7 条断言失败在单体专用仪器上（4 × `CsoContentAddressingScenario` 在 client 日志里找带 `cso[csom= csob=]` 的统计窗口，而 split 下 CSO 在 server 铸造、那一行在 server 日志；3 × DV `HandleRecycle.AbaControlHandles` 的 buffer / VAO 用例是负控，期望 STALE，wire 路径读到 FRESH——即像素正确、负控不起作用）。这 44 条之外的 1223 条全部 passed 或 skipped。其中 40 条在 P5b 是 passed，头上转为上述读法；没有一条是默认旋钮下的产品回归。

### 7.3 唯一收官审查与定向修复

审查 `p5b-close-codex-review.md`（`348d22a4`，diff base `37fc4fdb`，只读）：**0 blocker / 2 major / 1 minor**。

| 项 | 问题 | 处置 |
|---|---|---|
| Major 1 | user-index span 在段内但 Size 可能短于 Count × IndexSize | `a021e3cc`：encoder / decoder / sink 共用 shape/extent gate（单 range、宽度 1/2/4、`Uint64(Count) × IndexSize ≤ Size`），三端短 span 均拒绝、段末 exact-fit 通过；定向 9/9 |
| Major 2 | `ClientWaitSync` 保留 64-bit timeout 但 applied/reply 等待一律 30 s | `82683d4a`：预算 = ceil(timeout ns / 1e6) + 30,000 ms，有限 chunk；普通容量等待 / `FenceWaitServer` 仍 30 s，shutdown 可唤醒。**该 30 s 常量后来在 CI 修复轮改成 120 s（`68b55ea3`），原因见本表下注** |
| Minor 3 | ReadPixels exact-size reply 的未知 status 穿过 production helper | `82683d4a`：tight 与 bounce 路径拒绝 status=3，`Fatal{ReplyStatusInvalid}` |

定向 `RemoteClientTest` 7/7（0 ns、1 ns、60 s、UINT64_MAX 预算，`FenceWaitServer`、shutdown、未知状态）。合并后 quickgate 里 split 单元车道仅有的 2 个失败（`PipeWireCodecTest.UserIndexSpan*`）是测试日志捕获缺陷（库按进程截断日志，fork 子进程增量读读空），`7cb29d46` 修复后 3/3。本阶段不再做第二轮全门或审查；原始全门源头仍为 `348d22a4`，三项修复只以定向证据补齐。

> 上表 Major 2 的 30 s 是 `82683d4a` 当时的值，保留为历史记录。**当前值是 120 s**（`68b55ea3`，`ClientSession.cpp` 的 `kBarrierTimeoutMs`），因为 lavapipe 在 texture-handle 注册（`vkCreateImageView` → `llvmpipe_register_texture`）上可合法阻塞 apply 线程 29.3–39.7 s，30 s 的看门狗会把合法慢路径间歇判红；详因见 `ARCHITECTURE.md` §17.1。`RemoteClientControls.FenceWaitBudgetHonorsGlTimeoutAndFiniteTransportChunks` 的期望值（30000/30001/90000）已同步改为 120000/120001/180000。

### 7.4 Redmi 出口与四臂 A/B（`82683d4a`，APK `p5bcodex2`）

Redmi `2f7cbe2e`，证据根 `MobileGL/.trace-work/p5b-redmi/p5bcodex2/2f7cbe2e/`；全部组合显式 `MOBILEGL_IPC_STAGE_MB=256`。**钉频口径变更**：2026-09-11 的厂商 GPU 上限（1050 MHz）已消失，本次 deterministic pin 为 **1100 MHz**；与 1050 MHz 时代的活动不可比钟频，只有同场四臂配对可比。

**正确性 8/8**：`improved-transparency-minecraft-26.3`、`minecraft-1.21.4-in-world`（GLES SSIM 0.999995）、`minecraft-1.21.4-fabric-sodium-in-world`、`minecraft-1.21.4-fabric-iris-bsl-in-world` × 双后端，inproc split 臂，逐组钉频取证 + PNG / SSIM 阈值 + inproc + apply 线程 + 正 wire 记录三证，skip 即失败。**四条 A/B trace 首次在 Redmi 上以独立 apply 线程渲染——P5b 出口判据达成。**

**四臂 A/B（barrier tax 首测）**：32 组中 24 组带 200 帧尾验证全绿；三次重复取 wall-time 均值最低者、尾 200 帧（`ab-tables.md`）。逐线程 CPU p50：

| trace | 后端 | push−pull | **barrier tax（split−push）** | barrier tax 帧 p50 |
|---|---|---:|---:|---:|
| improved-transparency-26.3 | DirectGLES | +15.6% | **+10.3%** | +0.1% |
| improved-transparency-26.3 | DirectVulkan | +8.8% | **+13.1%** | +13.3% |
| minecraft-1.21.4-in-world | DirectGLES | +21.4% | **+18.2%** | +0.2% |
| minecraft-1.21.4-in-world | DirectVulkan | +11.8% | **+17.9%** | +19.7% |
| minecraft-1.21.4-fabric-sodium | DirectGLES | +10.9% | **+5.9%** | −0.1% |
| minecraft-1.21.4-fabric-sodium | DirectVulkan | +7.6% | **+8.3%** | +4.3% |

splitctl−push 全部在 ±1.6% 内（增量来自 inproc 传输与 barrier，不是 APK / 构建差）；p99 同向放大（sodium DirectVulkan 帧 p99 +107.7% 为离群尾帧）。inproc 臂 peak RSS 389 MiB – 2.5 GiB，全角色映射 560.5 MiB（256 MiB × 角色视图是虚拟映射容量，不是 RSS 增量）。

**fixture 受限的 8 组**：`iris-bsl-in-world` 只有 123 个 benchmark 帧，低于 200 帧尾规则，四臂 × 双后端验证全部失败（pull 同失败，是 fixture 上限不是回归）；123 帧完整序列按同法折算成补充表 `ab-tables-bsl-123frame-supplement.md`：barrier tax CPU p50 DirectGLES **+7.1%**、DirectVulkan **+9.1%**，不混入上表。性能仍只记录、不作阻塞门。

## 落地形状（原 `ARCHITECTURE.md` §17.5）

### 17.5 P5b：class-C verb 迁移

- **顺序由动态普查决定**（ID-68）：79 个 trace × 集成车道在 inproc 下的首阻塞统计（`~/w7/notes/p6/census-classC.md`），所有 Minecraft trace 首阻塞 = `DrawElements` → Minecraft 优先，四包并行（d1 draws、i1 image/compute、t2 XFB/tess、f1 clear/copy/mip）+ 尾包（sync、具名 blit、GLES mip）。
- **一次迁移 = 一个发射器 + 一个 sink 体**：c0b 先给 25 个已测量槽铺 wire 记录（`Wire/PipeWireCodec` 的行）、`WireVerbSink` 分派与 `ServerVerbSink` 具名 Fatal stub；包只在 `EmitTables.cpp` 把槽从 C 挪到 B、在 `PipeApplier.cpp` 填 sink 体（apply 线程、只从记录与 server 状态取输入）。这些槽到达的是 sink，不是 `MGPipeApply*`。
- **规则 D**：记录把 GL 调用逐字带在句柄旁边（draw 的 mode/count/type/offset/instance/base 字段、clear 的值类与 drawbuffer 下标、named-framebuffer 的句柄形式），后端保留它的 barrier-pulled 读；未测量的形式**按名拒绝**（`+RENDERBUFFER`、`+UNBOUND` 等）而不是猜。
- **draw 家族**：19 个索引 / 实例 / multi-draw / indirect 槽全部下沉到 `draw_vbo`；用户索引 span 的 shape/extent 门在 encoder / decoder / sink 三端共用（单 range、宽度 1/2/4、`Uint64(Count) × IndexSize ≤ Size`）。
- **sync**：`FenceSync` / `ClientWaitSync` / `GetSyncStatus` / `WaitSync` / `DeleteSync` 五条现有 opcode 接线，client 铸造 Fence 句柄，wire 只过 `{slot,gen}`，native fence 归 apply 线程；顺带修掉 `MGL_BACKEND_SLOT_PTR_LOCAL` 对 split 恒返回 nullptr。
- **具名 blit**：`BlitNamedFramebuffer` 经作用域化 client-shadow read/draw 绑定发布，降为现有 bound backend 调用，退出恢复公开绑定。**GLES mip storage**：前端先定义并发布层级，server 只验证 applier descriptor 的 Levels/extent；registry 身份解析只 Find 不 mint。
- **P5b 留下的 inproc 依赖**：具名 blit 的 scoped client binding + barrier；mip descriptor 的 barrier-held registry 查询；FBO death 的 inproc mailbox。三者跨地址空间都不成立——它们只是 §17.6 清单里的三行。

## 里程碑记录（原 `ROADMAP.md`）

- **P5b 出口（2026-09-16，已达成）**：主机全门 `348d22a4` complete；79 trace 72/6/1；设备源头 `82683d4a`（APK `p5bcodex2`）Redmi 正确性 8/8——四条 A/B trace 双后端首次在设备上 inproc 渲染；barrier tax 首测 split−push 逐线程 CPU p50 +5.9% – +18.2%。

## 债务表全文（原 `ROADMAP.md`「P5 / P5b 出口记录的债务」，2026-09-22 口径）


| 债务 | 去向 / 当前口径 |
|---|---|
| P5 的 27 个普通 inproc wrong-answer（历史计数） | **已结（2026-09-22，`3c80cd62` 同名重跑，`MEASUREMENTS.md` §7.2；inproc 与 spawn 逐名相同）**：原「22 → P4b/P7 texture readback」→ **19 绿**（14 layered、3 packed depth/stencil、2 framebuffer recycle），余 **3 条 framebuffer recycle 由错答变为具名停止**——三条都是 `integration-monolith-control` 里 `MOBILEGL_PIPE_PUSH=0` 的对照臂（DG `Legacy` → `Fatal{PipeLegacyMemosDisabled}`；DV `Legacy` / `AbaControl` → `Fatal{UnmigratedVerb, "Magma:clear-framebuffer-record"}`），旋钮在 split 下没有臂，不是 readback 欠账；原「3 → P7 query」→ **3 绿**；1 inspection（`TextureParamsWithoutASamplerView`）→ **绿，「待核」关**；1 FBO/RBO delete → 绿。同一重跑的 1267 名合并 lane 普查 811/203/62/191 → 1051/172/37/7，44 条非绿全是钉了非默认 `MOBILEGL_PIPE_PUSH` 的对照臂 |
| `rsp` residual inputs | **P5f 已关闭残余读取**：字段归属 41 record / 6 derived / 0 barrier / 16 fatal；strict 与双块 marker 表均为空，双后端逐帧 RSP 门 2 PASS / 0 skip，Redmi 36 个统计窗口 rsp=0。P5e 的“rsp 只计被采纳设障 pull”是历史计数规则，八对 admitted 的历史观察不再是当前允许集合 |
| `SEG_REPLY` 2 MiB 单槽 payload cap | → **P9**（契约 §10.2.3 改判，ID-47）；stream 链路上 reply 本就是消息，cap 变成 P6.5 nd 的 `maxReplyBytes` 窗口 |
| `GetCaps` 的两个 blobref | 目前不骑 record；一旦运输，必须有 server→client carrier rule，不能套 `SEG_STAGE` |
| PACK-PBO readback | P5 曾具名拒绝；`70df57bb`（2026-09-20）起 **client 侧已实现**（bounce 紧凑 reply + 按 PACK 步长逐行 `UploadSubData`，`EmitTables.cpp`；`glGetTexImage` 同形且按 `MaxReplyBytes` 分块）；剩余只是 fire-and-forget（`MarkReadPixelsPackBuffer` 全仓无生产调用者）→ **P9**（ID-57） |
| ABI fingerprint | **已由 P6.5 wf / nd 落地（`fe28bdb5`）**：`wireFingerprint` 布局摘要 + `buildFingerprint` 拆分（只在 `Dial==Fork` 或 `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` 下比较）、`LinkTerms` 四个尺寸由 server 陈述、`DynamicBackendParameters` 定宽；`CONTRACT-P6.md:666` 仍把定宽记在 P7 名下，待改 |
| 默认 32 MiB staging | 目标负载单次 128 MiB 上传装不进默认 stage；**内容侧分块已落地**（buffer 范围走查 `1e7c372e`、纹理整宽 slab `9469d48e`，预算 `MGPipeStageChunkBytes()` = 默认 segment/4 = 8 MiB），故普查 / Redmi 的显式 256 MiB profile 不再是这两条路径的必需；未接入分片的 record 类型与专用 carrier 仍归 P8（开放问题 11） |
| P5b 的 inproc 依赖 | 历史的 scoped client binding、mip registry 查询与 FBO death mailbox 已经 P5c/P5f 改为记录、句柄和控制/事件路径；P6 核验并装配已有载体，不再重做这些迁移 |
| 未迁移与仿真路径 | **class-C 已归零到两个**（2026-09-22 核）：`EmitTables.cpp` 的 `MGR_UNMIGRATED_*` 列表全空，只剩手写的 `SetSwapInterval`（P10）与按裁定无行的 `DeleteTransformFeedback`（P9）；契约 §11 / SPAWN-PLAN §8 的"511 class-C entries"是 P5 joint 树上的旧普查（`notes/p6/census-classC.md`），不是当前数；~~client vertex arrays~~（已实现：`beba0256` 起 wire 复制到 owned buffer 并保留 baseVertex/baseInstance/drawID，本轮补齐 monolith 臂 `62bfe461..e258a822`）、multi-draw client indices、RGB 三通道 CPU mip、renderbuffer copy endpoint、未绑定 named clear、`texture-remint-pull` 仿真保留具名拒绝 → P8 / P9；Magma split compute / image 的“82 个错答”是 P5b 历史普查计数，不是当前失败清单；P5f 后续已补应用 buffer consumers 与 Android 旋转 blit，Magma inproc 已能运行所测 MC 世界；后续 run-ahead 已完成（[报告](../p5f/magma-runahead.md)）；XFB、部分不对齐范围、placeholder/native-format 及进一步性能工作仍归 P7（[报告](../p5f/magma-inproc-fix.md)） | **P5e 补 → CI 收尾更正**：client vertex arrays 曾是 run-ahead 下的具名拒绝（ID-82）与预期红车道 `integration-clientarrays-split`（ID-134）；`beba0256` 起已实现，`347bb90c` 已把该车道翻转为**硬绿门**并纳入主 GPU/split 标签，本轮补齐 monolith 臂（`62bfe461..e258a822`）；staging 仍归 P8
| E2 wire 内容控制 | draw-drop 是 OpenRA 的有效控制（758 draws，SSIM 0.000036）；clear-drop 被全屏 overdraw 掩盖，不能作控制 |
| max record bytes | 实测 reduced / OpenRA `maxrec=784 B`，默认 cap 4 MiB；只代表所测负载，后续索引 / indirect 尾仍须记录 |
| 树外脚本 | 普查跑器、`wsl_p5_gate.sh`、Redmi 定频行都在 `~/w7/notes/`，git merge 不会传播 |
| 临时 CI trigger | 合入 dev 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` TEMPORARY trigger |

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P5B.md`](BRIEF-P5B.md) | BRIEF-P5B — verb migration under inproc, Minecraft-first |
| [`PACKAGE-PREAMBLE.md`](PACKAGE-PREAMBLE.md) | P5b 包通用前言 — 每个包开工前必读 |
| [`p5b-results/`](p5b-results/) | 17 个文件：各包（d1 / i1 / t2 / f1 / r1 / r2 / blit / mip / sync）的报告 |
