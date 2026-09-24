# P5 — 传输 + inproc applier + 发射表（`ff2994d9..37fc4fdb`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：同一 codec 上的 `InProcessTransport`；四种 build flavour（pull / push / verify / split）；发射表三类 + caps mirror + reply mailbox；apply 线程终身持有 context；lockstep verb barrier；tight ReadPixels；persistent-map 块推送；G8 字段归属生成器；split 测试 / CI / APK 车道。八包 c0 / w1 / s1 / p1 / b1 / t1 / c1 / v1 + j0 / x2 / v1-r3 收尾。
- 门：joint OpenRA inproc 2/2 SSIM 1.0、`integration-split` 21/21、E1 14/14 红、G1 0/0/0/0。收尾头全门在 E3(a) 停止（设计性 skip 被判红），Part 2 / 4 未到达。
- 收官审查 1 blocker / 10 major / 2 minor → P5b 的 r1 / r2。Redmi 四臂 A/B 的 split 臂全部停在索引 draw，barrier tax 归 P5b。
- 流程在 P5 尾声改变（ID-66）：不过度验证，每阶段收官一次异模型族审查。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5** 传输 + inproc applier + 发射表
- **状态**：✅ `ff2994d9..37fc4fdb`
- **落地什么 / 范围**：同一 codec 上的 `InProcessTransport`；四种 build flavour；发射表三类 / caps mirror / reply mailbox；apply-thread context 终身持有；lockstep verb barrier；tight ReadPixels；persistent-map 块推送；G8 字段归属生成器；split 测试 / CI / APK 车道；八包（c0 契约、w1 codec、s1 session、p1 归属、b1 persistent map、t1 车道、c1 路由、v1 server）+ j0 / x2 / v1-r3 收尾
- **验收门 / 证据**：joint：OpenRA inproc 2/2 SSIM 1.0；`integration-split` 21/21；E1 14/14 红；G1 0/0/0/0。收尾头全门在 E3(a) 停止（设计性 skip 被判红），Part 2/4 未到达；收官审查 1 blocker / 10 major / 2 minor → P5b r1/r2。§6

## 实测：P5（joint `e61d0012` → 落地 `eec0e836` → 收尾头 `37fc4fdb`）（原 `MEASUREMENTS.md` §6）

### 6.1 五部分门（joint）与落地快门

| 部分 | joint 实测 |
|---|---|
| 1 接口纯度 | include closure 4 probes / 0 problems；pull `MG_Remote` 符号 0、split 610；G1 `.text` 10806611 → 10806611、27814 符号 0/0/0/0；P3a/P4a G5 对 `ff2994d9` byte-identical |
| 5 覆盖 / 生成器 | `gen_pipe` self-test 9/9；dirty-surface 27/27（`UseProgram` 一项明确 UNDECIDED）；field ownership 15/15；emitter/CSO 185/185；verify controls 4/4 |
| 3 行为 A/B | G2 差 0；G14 0 / +42；unit 1816×3、split 2038；Wire 58/58；`integration-gpu` pull 1128、push 1128、split-monolith 1149；split-inproc 普查 426 pass / 185 skip / 511 abort / 27 fail；`integration-split` 21/21（19 run、2 design-skip）；persistent arm 2/2，split `pmap=2160.00, mpr=1`，push `pmap=0.00, mpr=1` |
| 2 语义 / retrace | `integration-verify` 930/930 零 `Fatal{`；OpenRA inproc 2/2 双后端 SSIM 1.0；push retrace 79/79；verify retrace 79/79 armed |
| 4 设备 | 未跑（joint 禁止 adb）；见 §6.4 |

落地快门（`eec0e836`，ID-66）：split unit 2038；`integration-split` 22/22；push `integration-gpu` 1128；G1 0/0/0/0 `.text +0`。

### 6.2 退出门 E1–E6 与 27 个 wrong-answer

| 门 | 实测与"为什么会红" |
|---|---|
| E1 barrier | reduced path 19 pass / 2 skip / 0 fail；`MOBILEGL_IPC_VERB_BARRIER=0` 选中的 14/14 全 abort，每个私有日志有自己的 `Fatal{BarrierViolation, "<slot>"}` |
| E2 OpenRA | 2/2 SSIM 1.0；x2：丢 29 clears 仍 SSIM 1.0（被后续地形 draw 覆盖），改为丢 758 `DrawVbo` 后 SSIM 0.000036；r2 验证真实 CTest 顺序 baseline → pull-library control → 恢复原库 → draw-drop，并断言库身份 |
| E3 persistent map | (a) 收尾头 6 selected 中 4 pixel red、2 条 `TheMapLandsInTheArmItsLaneDeclares` 设计性 skip 被 j0 判红 → r2 精选默认 / SmallRing 下 4 条 pixel case 各带自己的 assertion + 私有诊断；(b) ID-42 emulated membership green → 3 red → green；(e) x2 证明 wrap，r2 证明真实 retirement wait（1 MiB 臂 `ringwraps=1 ringwaits=1`，8 MiB 对照同时红） |
| E4 field ownership | 生成器 15/15；strict lane 19 abort / 2 skip，首条 `Fatal{UnmigratedPipeInput, "GetTextureContextId@Clear"}`——BARRIER-PULLED 债务的响亮读法 |
| E5 honest inproc | 19 pass / 2 skip；v1-r3 的 `StagedShadowProductionTest` 证明 ensure 上传 server shadow（删保护会读 client A 变红），覆盖 unmapped 对象；coherent-map 的 server 侧绕过由 r1 #1 关闭 |
| E6 phase gate | 普查在 v1-r3 修掉六条过宽 whole-store 拒绝后 **432 / 185 / 505 UnmigratedVerb abort / 27 failed / 0 segfault**（1149 项，`~/w7/p5b-c0b-census-logs/results.json`） |

| 27 个 wrong-answer 家族 | 条 | 去向 |
|---|---:|---|
| `LayeredAttachmentShapeScenario` | 14 | P4b/P7 layered texture readback |
| packed depth/stencil `GetTexImage` | 3 | P4b/P7 texture-shadow readback |
| framebuffer `HandleRecycleScenario` | 5 | P4b/P7 readback（不是已证明的 handle recycle bug） |
| `PrimitivesGeneratedNoXfbScenario` | 3 | P7 query |
| `TextureParamsWithoutASamplerView` | 1 | P6 inspection forwarder |
| `P4aFinalFixScenario` FBO/RBO delete | 1 | 读回用 ReadPixels，完整像素因果未隔离；client-thread framebuffer death 归 r1 #2 |

> 上表是 P5 的历史去向。**头上读数（`3c80cd62`，P7 wave 3 同名重跑）见 §7.2 末**：22 条 readback → 19 绿 + 3 条由错答变为具名停止（都是 `MOBILEGL_PIPE_PUSH=0` 的对照臂）；3 条 query 全绿；inspection 与 FBO/RBO 各 1 条绿。

### 6.3 R-10、逐帧 ledger 与内存

规范 ledger（`ProtocolSmokeTest` 钉住）：**SEG_CMD 8 MiB / SEG_STAGE 32 MiB / SEG_REPLY 16 MiB / SEG_EVENT 256 KiB**。`MOBILEGL_PIPE_STATS_PERIOD=1` 下 persistent-map 场景 `pmap=600.00 B/帧, mpr=1, rsp=35`（`rsp` 只是有 stamp 读点的下界）。inproc 进程峰值 RSS：server accept 时 11.5 MB、client teardown 141 MB（两角色共享一个进程，不是两个独立峰值）。

max record bytes（x2，`37fc4fdb`）：Triangle / persistent map / OpenRA 25 帧 / SmallRing 都是 **`maxrec=784 B` / `SetVertexAttribDefaults`**，默认 cap 4 MiB（占 0.019%）——这些负载不需要 chunking；oversized `DrawVbo` 尾的 unit 具名拒绝 `Fatal{RingOverrun, "DrawVbo"}`。

**内容分块落地（2026-09-20，`1e7c372e` buffer / `9469d48e` texture）**：`SEG_STAGE` 仍是"一条记录一个整 blob"的 arena，但会超出它的内容由发射侧按 stage chunk 预算 `MGPipeStageChunkBytes()` = `clamp(segment/4, 4096, segment)`（默认 32 MiB → 8 MiB）切成多条 `resource_subdata`——buffer 范围走查与纹理一级的整宽 slab（服务端 `StagedTextureStore::AdoptRun` 拼回整级）。§7.2 普查为容纳单次 128 MiB 上传而显式设的 `MOBILEGL_IPC_STAGE_MB=256` 因此不再是这两条路径的必需；未接入分片的 record 类型仍由 `Fatal{RingOverrun, "SEG_STAGE"}` 具名拒绝。上段的 `maxrec` 数字是分块前的测量，当时分块后的逐 blob 字节分布尚无新测量；该分布与分块后的 `maxrec` 已在 §12.4 补测——结论是默认 8 MiB chunk 预算在已测的两条负载上从未被打到，`maxrec` 仍是 1808。

### 6.4 Redmi 四臂 A/B（split 未测）

会话 2026-09-16，runner `eec0e836`，三份 APK 源码头 joint `e61d0012`；臂 = pull APK / push APK / split APK + `MOBILEGL_TRANSPORT=inproc` / splitctl（split APK 不设 transport）。40 组前后 pin check 全 P/P，37.2–39.9 °C。**27 组完成 / 13 组失败：全部 10 组 split 在 benchmark 前中止**（improved-transparency 双后端 `DrawElementsInstancedBaseVertex`，其余 `DrawElements`），另 3 组是 rd12/DirectVulkan 的既有 `scudo` 崩溃。帧 p50 / p99（ms）：

| case / backend | pull | push | splitctl |
|---|---:|---:|---:|
| improved-transparency / DirectGLES | 10.535 / 25.415 | 12.106 / 26.973 | 12.235 / 27.131 |
| improved-transparency / DirectVulkan | 10.742 / 25.031 | 11.678 / 26.134 | 11.837 / 26.476 |
| fabric-sodium / DirectGLES | 8.314 / 9.573 | 8.311 / 9.701 | 8.303 / 9.486 |
| vanilla 1.21.4 / DirectVulkan | 1.043 / 2.293 | 1.159 / 2.404 | 1.179 / 2.426 |
| rd12 / DirectGLES | 7.999 / 22.089 | 11.105 / 24.865 | 11.261 / 24.771 |

splitctl 相对 push 的帧 p50 −0.1% – +1.6%（split build 的 monolith 臂与 push 同源等价）。barrier tax 在 P5 未测得，归 P5b（§7.4）。

### 6.5 收尾头停止点、r1 / r2

`~/w7/p5-final-gate.log`（`37fc4fdb`）：构建 4 × rc 0；Part 1 G1 27,814 符号 0/0/0/0、`.text +0`、两条 G5 byte-identical；Part 5 完成；Part 3 unit 1817×3 / split 2086、Wire 58、pull/push GPU 1128、split-monolith 1149 零失败、reduced 22 零失败、broad lane 1149 selected（532 non-success，含 abort）；E1 14 red；**E3(a) 因 2 条设计性 skip 被 j0-v3 的逐选项拒绝判失败，Part 2 / 4 未到达**。j0 的 skip 拒绝是对的（不能放宽），选择器才是错的。

收官审查（`p5-close-codex-review.md`，只读）：1 blocker / 10 major / 2 minor（ID-73）。r1（核心四项）：apply-role 不得进入 persistent-map producer；framebuffer death 转 apply mailbox；`PACK_SWAP_BYTES` 的 component / packed-word swap；command-ring retirement 等待与重试。r2（`37fc4fdb..d50183cb`，CI / 控制 / 跑器九项）：broad debt lane 记录后继续硬门、全 skip 则 baseline 失败；E2 库身份 SHA256 恢复；E3(a) 精选 4 pixel case；`ringwaits` 只计真正阻塞的分配；split unit 强制 `MOBILEGL_ITEST_REQUIRE_GPU=1`；普查跑器零执行拒绝、launch 前清私有日志；G5 pin 自测驱动生产选择路径；c1f 改用 ID-67 用例。r2 包门：split unit 2087、reduced 22、push 1128 零失败；E1 14 own reds、E3(a) 4 own reds；smoke 16 core + 12 private；census 432/185/505/27。r2 没有新跑 pull G1（不影响 pull production code）。

过程记录（不计入实现产量）：wave-1 跨族复审十项全部经独立 perturbation 确认（ring 空 wrap 算术、reply 几何差 16 B、blob 校验接受任意段、pad-bit 控制没发送该 bit、三个 CI 对照接受任意非零退出……）；后续 v1 / c1 各轮的 review 计数在 ID-52/58/62/64；本地 `rereview` 身份污染的 81 + 58 个提交经 parent/env rewrite 接回 GitHub `ff2994d9`（ID-40），此后每阶段首个 commit 前先跑 `git var GIT_COMMITTER_IDENT`。这些数字是流程在 P5 尾声改变（ID-66）的原因。

## 落地形状（原 `ARCHITECTURE.md` §17.1–§17.4）

### 17.1 verb barrier 与诚实的同地址空间传输

P5 的 `inproc` 是真第二线程，但还是 **lockstep**：每条 class-B verb 发射后 client 的 `EmitAndWait` 等到 `appliedSeq == emitSeq`；`MOBILEGL_IPC_VERB_BARRIER=1` 默认开启。理由不是吞吐，而是 BARRIER-PULLED 字段（G8 表里目前 36 行）尚无记录载体；在这些字段退役前让两线程同时跑，server 会读到 client 的"未来值"（R-1）。barrier 是逐族可退役对象，不是 P6 transport 的要求；它的设备代价（barrier tax）见 `MEASUREMENTS.md` §7.4。

同一地址空间不得成为旁路（R-2）：encoder 把 `MGHostSpan::Ptr` 恒写成 `nullptr`，内容 blob 必须带真实 `SEG_STAGE` offset / 非零 size；decoder 对四种形状分别 `Fatal{ProtocolCorruption}`。`MapPersistent` 在 split 恒 decline；`MOBILEGL_IPC_AUDIT=1` 在 retire 后把 staging 填 `0xDD`，让跨 applier 返回持针的实现下一次读取时可见地失败（R-11：任何 widened read 先过 `RequireStagedCoverageForPendingRanges`，否则 `Fatal{StageSnapshotTooNarrow}`）。apply 角色不得进入 client 的 persistent-map producer（r1）。

reply mailbox 以**记录序号作为 slot id**（R-3）：slot header 是 `{Seq, Status, Size}`，acceptance 的 Bool 答案与 `MapPersistent` decline 都在既有 verb wait 内读取；`Status=ERROR` 一律 `Fatal{ReplyError}`，未知 status 是 `Fatal{ReplyStatusInvalid}`；读到 reply 之前先由 `appliedSeq` 证明该记录已离开 applier（R-5）。P9 才把这套同步 mailbox 推广成异步池。等待预算：普通 verb / 容量等待 120 s（`Fatal{BarrierTimeout}`，`ClientSession.cpp` 的 `kBarrierTimeoutMs`）；`ClientWaitSync` 的 applied/reply 预算 = ceil(timeout ns / 1e6) + 120 000 ms，有限 chunk 避开溢出。这条看门狗只判**死锁**（记录永不退役），不是性能门：lavapipe/llvmpipe 在 texture-handle 注册（`vkCreateImageView` → `llvmpipe_register_texture`）上可合法阻塞 apply 线程 29.3–39.7 s，故预算从原 30 s 提到 120 s——monolith 走同一路径本就没有这个上限。

### 17.2 71 槽的三类、caps 与 tight readback

client 表（`Client/EmitTables.cpp`）把后端函数表的 71 个槽分成三类，`static_assert` 钉住 **A = 2**（getter 从 caps mirror 本地回答）/ **B = 54**（发射：P5 的 `Clear`、`DrawArrays`、`ReadPixels`、`Blit`、`Present` + P5b 的 d1 19、i1 7、t2 6、f1 11、`BlitNamedFramebuffer`、sync 5）/ **C = 15**（`Fatal{UnmigratedVerb}` 具名拒绝，永不回落到 monolith applier——R-4）。applier 侧 33 条走生成路由表 + 4 条 escape，`PipeCatalogueTest` 钉 37；生成路由的完备性覆盖值调用与 `&MGPipeApply*` 地址取用（R-17）。

能力存活只读 `CapsMirror` 中的 `MGPCaps::CallMask`（R-8）；server 的 consumer mask 由 backend 类型显式设置并以实际 op table 校验，**永不**从进程级 `MGPipeGetResourceOps()` 推导。成功的 `MakeEGLCurrent` / `InitCapabilities` 重新发布 snapshot，client 按 generation 采纳（R-12）；相同 (dpy, draw, read, ctx) tuple 的重复 make-current 不重发（ID-67）。

`ReadPixels` 在线上恒为 **tight**：server 临时设 neutral pack，只向 reply 写 `width*height*bytesPerPixel`；client 用自己持有的 pack state scatter（含 `PACK_SWAP_BYTES` 的 component / packed-word swap，r1）。绑定 PACK PBO 时在 client 侧 `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}`；reply 单槽 payload 上限 `2 MiB - 16`，更大 readback 的 carrier 留给 P6+（ID-47/57）。

### 17.3 server 角色、shadow 与退出

`ServerLoop` 的 `mgl-srv-apply` 是 native context 的终身 owner。`ServerMakeEGLCurrent` 对 tuple **只 bind 一次**，client release 只记账、绝不让 apply thread native-unbind（ID-54）。罕见 EGL 操作经 caller-serialised one-slot control mailbox 进 apply thread；十二个 `Server*` forwarder 是唯一缝。有 backend 却无 thread 时 `Fatal{ApplyThreadNotRunning}`，不允许回落到 app thread。framebuffer death 的驱动调用也经 mailbox 上 apply thread（r1）。

`PipeApplier::ApplyOne` 依次 stamp verb、decode/apply、清 stamp；只有 `SessionConsumer::ApplyOne` 每条记录把 `appliedSeq` 加一。`StagedShadowStore` 按 resource twin 复制并合并**精确覆盖范围**；`Ops_H_SubData` / flush / respecify 只把 server-owned copy 交给后端；有传输时 `liveHostBase()` 不得回落到 client 的 `MappedData()`（ID-50/52）。

退出顺序：`ShutdownSplitRoles` 先让 client publish 并 bounded-drain，再 shutdown transport / 唤醒 wait，随后 bounded-join apply thread；线程在仍持 context 时 detach decoder、销毁 private backend，最后才销毁 emitter 与 transport。

### 17.4 lane 隔离与阶段边界

每条 `DirectGLES.Split.*` entry 有独立 `MOBILEGL_LOG_FILE_PATH`；E1/E3 控制从被选 entry 的私有文件取 Fatal，且把"选中条目被跳过"判为失败（ID-53/62）。`integration-split` 是缩减路径的硬门；broad inproc `integration-gpu` 车道只记录普查、不设门（ID-65）。

## 里程碑记录（原 `ROADMAP.md`）

- **P5 出口（2026-09-16）**：缩减路径首个 IPC 帧；最终全门在 E3(a) 停止；红米四臂的 split 均停在索引 draw，barrier tax 未测（归 P5b）。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P5.md`](BRIEF-P5.md) | BRIEF-P5 — 传输 + inproc applier + 发射表 |
| [`PACKAGE-PREAMBLE.md`](PACKAGE-PREAMBLE.md) | P5 包通用前言 — 每个包开工前必读 |
| [`scout-backend-install-and-thread.md`](scout-backend-install-and-thread.md) | P5 scout — backend installation, `BackendObject_Remote`, and the thread/EGL question |
| [`scout-caps-reply-reverse.md`](scout-caps-reply-reverse.md) | P5 scout — the caps snapshot, the reply slot, and the reverse channel |
| [`scout-gpu-writes-and-persistent-map-push.md`](scout-gpu-writes-and-persistent-map-push.md) | P5 scout — GPU-written tracking and the persistent-map push |
| [`scout-premortem.md`](scout-premortem.md) | P5 scout — adversarial pre-mortem |
| [`scout-test-ci-plumbing.md`](scout-test-ci-plumbing.md) | P5 scout — test/CI plumbing a SPLIT arm needs |
| [`scout-transport.md`](scout-transport.md) | P5 scout — MG_Remote/Transport as it stands, and what P5 must add |
| [`scout-unmigrated-census.md`](scout-unmigrated-census.md) | P5 scout — the unmigrated-input census |
| [`scout-wire-codec.md`](scout-wire-codec.md) | P5 scout — the wire codec (G3) and the call catalogue's flags |
| [`verb-census.md`](verb-census.md) | verb-census.md — which `GlobalBackendFunctionsTable` slots the reduced path actually reaches |
| [`p5-results/`](p5-results/) | 48 个文件：各包实现 / 评审 / joint / A/B 结果稿 |
| [`prompts/`](prompts/) | 2 个文件：派包提示词 |
