# 当前阶段进度

分支 `feat/disaggregated`，头 `c77831e0`（2026-09-16，已推 GitHub）。本文随每次落地更新。
逐条裁定的长文原文在 git 历史里（`git log -p -- docs/Disaggregated/CURRENT_STAGE_PROGRESS.md`，`ef35ea0c` 之前的版本 = ID-1..75 全文）。

## 1. 阶段状态

| 阶段 | 状态 | 范围 |
|---|---|---|
| P0 / P0.5 / P1 / P2 / P3a / P4a | 已落地 | 见 `ROADMAP.md`、`MEASUREMENTS.md` §1–§25 |
| **P5** 首个 IPC 帧（reduced path） | **已收官** | `ff2994d9..37fc4fdb`，八包 + j0/x2/ap/docs |
| **P5b** inproc 下的 verb 迁移 | **已收官（2026-09-16）** | `37fc4fdb..c77831e0`；出口证据见 `MEASUREMENTS.md` §33–§35 |
| P6 spawn transport | 未开始 | 设备出口已完成，可以开始 |

## 2. 当前头实测

| 门 | 结果 |
|---|---|
| 构建 | pull / push / verify / split 四个 flavour 全部通过 |
| G1（pull 符号恒等） | `.text +0`，27814 符号 0 增 / 0 删 / 0 resize / 0 重命名 |
| unit | pull / push / verify 各 1817/1817；split 2138/2138（`348d22a4` 主机门为 2132；含 `7cb29d46` 后 UserIndexSpan 3/3 复证） |
| `integration-split`（inproc） | **107/107**（P5 收官时 22） |
| `integration-gpu`（push） | 1148/1148 |
| `integration-gpu`（split，单体传输） | 1267/1267 |
| G5（p3a / p4a 保护区） | 双绿 |
| 主机收尾全门（`348d22a4`） | **complete=true, exit 0**：31 步原始 rc=0 + retrace-verify 续跑合并 **79/79**，retrace-push 79/79，OpenRA 2/2；对账 `reconciliation-20260916.md` |
| 集成普查（inproc 车道，`348d22a4`） | 1267 selected = 811 passed / 203 skipped / 62 aborted / 191 failed，对 c0b **零回退** |
| 79 trace 普查（inproc，stage 256 MiB，`348d22a4`） | **72 passed / 6 aborted / 1 failed**（三轮续跑补齐；首阻塞清单见 `MEASUREMENTS.md` §33） |
| Redmi 正确性（`82683d4a`，p5bcodex2，stage 256 MiB） | **8/8**，四条 A/B trace 双后端首次在设备上 inproc 渲染 |
| Redmi 四臂 A/B（同上） | 24/32 组 200 帧尾验证全绿，barrier tax 首次实测（`MEASUREMENTS.md` §35）；iris-bsl 8 组为 fixture 123 帧上限，补充表另记 |

## 3. P5b 落地内容

| 包 | 迁移的槽 / 内容 | 效果 |
|---|---|---|
| c0b | 25 个已测量 class-C 槽的 wire 记录、`WireVerbSink` 分派、具名 Fatal stub、`CONTRACT-P5B.md` | 四包可并行 |
| f1 | `ClearBuffer{iv,uiv,fv,fi}`、`ClearNamedFramebuffer*`、`CopyTexImage2D`、`CopyTexSubImage2D`、`GenerateMipmap` | 34 个车道中止归零 |
| i1 | `BindImageTexture`、`DispatchCompute`、`CopyImageSubData`、`MemoryBarrier`、`ShaderStorageBlockBinding` + 两个伴随槽 | 235 个车道条目解锁 |
| t2 | 四个 stream-output span 行、XFB 对象绑定、`PatchParameteri`、`kCapBackendOwnsXfbCapture` | 138 个车道条目解锁 |
| d1 | 十九个索引 / 实例 / multi-draw / indirect 槽下沉到 `draw_vbo` | Minecraft 主阻塞解除 |
| sync | `FenceSync`、`ClientWaitSync`、`GetSyncStatus`、`WaitSync`、`DeleteSync` 上 apply 线程；client 铸造 Fence 句柄，wire 只过 `{slot,gen}` | 顺带修掉 `MGL_BACKEND_SLOT_PTR_LOCAL` 对 split 恒返回 nullptr |
| blit / mip | `BlitNamedFramebuffer` 经作用域化 client-shadow 绑定下沉；生成 mip 存储按推送描述符验证 | 31 + 7 个 trace 阻塞解除 |
| r1 / r2 | P5 收官审查 13 项（coherent-map 服务器侧绕过、FBO 死亡在 client 线程、`PACK_SWAP_BYTES`、`RingOverrun` 等待、CI 对照九项） | — |
| 收官审查修复 | 索引 span 上界（`Count × IndexSize` 必须装进声明的 run）、fence wait 预算、`Fatal{ReplyStatusInvalid, "ReadPixels"}` | P5b 审查 0 blocker |

## 4. 真实负载（inproc，独立 apply 线程）

- `DrawElements` 不再是阻塞。d1 普查时 77 个 Minecraft 后端用例中 28 个渲染通过（SSIM ≥ 0.99995）。
- blit + mip 之后：`improved-transparency-minecraft-26.3` DirectVulkan SSIM 0.999914；`minecraft-1.21.4-fabric-iris-bsl-in-world` DirectVulkan 0.997324、DirectGLES 0.997496。
- OpenRA 双后端 SSIM 1.0。
- **新头完整 trace 普查已跑（2026-09-16，`348d22a4`，stage 256 MiB）：79 = 72 passed / 6 aborted / 1 failed**。首阻塞：`rd12` GLES `InitialBytesNotCarried/resource_respecify`、VK `BarrierTimeout/Present`；`iris-photon` / `iris-derivative` / `create-indirect` GLES 均 `UnmigratedEmulation/texture-remint-pull`；`iris-bsl-esc-menu-854` GLES `InitialBytesNotCarried/resource_respecify`；`create-indirect` VK 内存膨胀被守护杀死（llvmpipe 上 >60 GiB RSS，两次复现）。
- **Redmi 出口达成（2026-09-16）**：四条 A/B trace × 双后端在设备上以 inproc split 渲染，正确性 8/8；四臂 A/B 首次测得 barrier tax（逐线程 CPU p50 split−push：+5.9% – +18.2%，六组；`MEASUREMENTS.md` §35）。

## 5. 开放项（按优先级）

| 项 | 证据 |
|---|---|
| `SEG_STAGE` 默认 32 MiB 装不下 128 MiB 上传：**决定 = 默认不改**，普查/Redmi 显式 256 MiB profile；更大 blob 的分块/专用 carrier 留 P8 设计（ROADMAP 开放问题 11） | `p5b-results/blit-codex-v1.md`；`MEASUREMENTS.md` §33 |
| Magma（DirectVulkan）split compute/image 路径：89 个错答中 82 个 | `p5b-results/i1-v1.md` |
| rd12 DirectGLES `InitialBytesNotCarried/resource_respecify`；rd12 DirectVulkan `BarrierTimeout/Present`；`iris-bsl-esc-menu-854` GLES 同 rd12 GLES；`create-indirect` VK 主机内存膨胀（守护杀死，两次复现） | 79 trace 普查 `counts.json` / `trace-transitions.json` |
| RGB 三通道 CPU mip 回退 | `p5b-results/mip-codex-v1.md` |
| ~~设备 barrier tax 未测~~ **已测（2026-09-16）**：split−push 逐线程 CPU p50 +5.9% – +18.2%，逐例见 `MEASUREMENTS.md` §35 | `ab-tables.md` |
| 27 个 P5 inproc 错答：纹理读回走 client-shadow 回退、P7 query 计数、一个检查 harness、一个 FBO/RBO 删除后生命期 | P4b / P7 / P6 债 |
| `rsp` 残余输入；`SEG_REPLY` 2 MiB 上限；GetCaps 两个 blobref 的载体；PACK-PBO 读回真实形式 | P3b/P4b、P6+ |
| 39 个未被任何负载命中的 class-C 槽（query / sync / indirect / 具名 FBO clear 等） | P5b wave 3 / P9 / P10 |
| `test.yml`/`apk.yml` 里的 `feat/disaggregated` 触发器是临时的，合入 dev 前必须移除 | — |
| 设备钉频口径：Redmi `2f7cbe2e` 厂商 GPU 上限已消失，2026-09-16 起 pin 为 1100 MHz（原 1050），跨口径活动不可比钟频 | `pin_device.sh` 更新记录；`MEASUREMENTS.md` §35 |

## 6. 下一步

1. **P6 spawn transport**：替换 P5b 的 inproc 依赖（具名 blit scoped binding + barrier、mip descriptor 的 barrier-held registry 查询、FBO death 的 inproc mailbox——ROADMAP 债务表），`SocketTransport` + `ServerMain` + 握手/退出语义。
2. 剩余首阻塞一轮（Magma compute/image、rd12、RGB mip、texture-remint-pull 仿真槽）。
3. P6 出口门：P5b 的完整渲染路径在 `spawn` 下绿；OpenRA 在 Adreno 830 上 split SSIM ≥ 0.99。

## 7. 记录位置

| 内容 | 位置 |
|---|---|
| P5 brief / 契约 / 前言 | `~/w7/notes/p5/BRIEF-P5.md`、`MobileGL/MG_Remote/CONTRACT-P5.md`、`~/w7/notes/p5/PACKAGE-PREAMBLE.md` |
| P5 包报告与审查 | `~/w7/notes/p5/p5-results/`（`joint-v1.md` 全门记录、`ab-v1.md` 设备 A/B、`p5-close-codex-review.md`） |
| P5b brief / 契约 / 报告 | `~/w7/notes/p5b/BRIEF-P5B.md`、`MobileGL/MG_Remote/CONTRACT-P5B.md`、`~/w7/notes/p5b/p5b-results/` |
| P5b 收官主机门 | `~/w7/p5b-final-host-348d22a4/`（`reconciliation-20260916.md`、`summary-counts.{json,md}`） |
| P5b 合并普查 | `~/w7/p5b-final-census-348d22a4/`（+ `-resume1/2/3`）、`p5b-results/joint-codex-v1.md` census 块 |
| P5b 设备证据 | `~/w7/notes/p5b/apk/p5bcodex2/`（APK + proof）；`MobileGL/.trace-work/p5b-redmi/p5bcodex2/2f7cbe2e/`（correctness 8/8、benchmark `ab-tables.md` 与 bsl 补充表） |
| class-C 普查 | `~/w7/notes/p6/census-classC.md`；基线 `~/w7/p5b-c0b-census-logs/results.json` |
| 门日志 | `~/w7/p5-joint-gate.log`、`~/w7/p5b-quickgate.log`、`~/w7/p5-joint-evidence/` |
| 脚本 | `~/w7/notes/tools/`（`wsl_p5_gate.sh`、`p5_ab_redmi.sh`、`wsl_build_p5_apks.sh`、`p6_census_*.{sh,py}`、`p5b-c0b-census.sh`） |

## 8. 裁定索引（ID-40..77；ID-1..39 见 git 历史）

| ID | 裁定 |
|---|---|
| 40 | wave-1 谱系重写：`~/w7/pipe` 曾残留 `rereview` 身份，58 个提交改回用户身份并重挂到 GitHub 谱系 |
| 41 | G5 第十一行 `FlushPendingRangesFrom` 始终对 pin 比较，pin 重钉在 `3dadd4c1` |
| 42 | persistent-map 成员性期望改为 `live == (arm == emulated)`；emulated 方向红检在联调头 VERIFIED |
| 43 | `RingTest` 双旗标空间用例改述实树：带 CALL 旗标的头会被弹出，不再被当 wrap filler 吞掉 |
| 44 | dev 合并；G1 基线按 dev 重取（`4595163c`） |
| 45 | wave-2 从配额中断处重启 |
| 46 | codex 对 wave-1 的跨族审查：10 个 major，逐条执行验证全部确认 |
| 47 | `SEG_REPLY` 改 16 MiB / 8 个 2 MiB slot；超限在 client 侧具名拒绝 |
| 48 | wave 1.5 分派（w1/s1/t1/p1） |
| 49 | ReadPixels 以紧凑形式过线：server 用中性 pack 读进回复，client 按应用 pack 散布 |
| 50/52 | v1 两份审查合并；`liveHostBase()` 在有传输时不得回落到 client 的 `MappedData()` |
| 51 | CI 上内存台账测试红 = 内核 hiwater 滞后，台账自持峰值 |
| 53 | 每个 `DirectGLES.Split.*` 条目一个私有日志；E1 对照从日志读 `Fatal{BarrierViolation}` |
| 54 | forwarder 每 (dpy, draw, read, ctx) tuple 只 bind 一次；client 的 release 不解绑 apply 线程的上下文 |
| 55 | c1 的 R-17：33 行走生成表 + 4 行 escapes + 回复邮箱；`PipeCatalogueTest` 钉 37 |
| 56/58 | c1 两份审查（codex 12 项 / Claude 3 blocker 8 major 6 minor）；B1 = 五处取地址的 `&MGPipeApply*` 逃过转换 |
| 57 | split 下绑定 PACK PBO 的 ReadPixels 具名拒绝，P5 不实现 |
| 59 | v1 round 2 落地；联调 `integration-split` 21/21 |
| 60 | 实现包可派给 GPT 族；实现方与审查方错开模型族 |
| 61 | c1 round 3 + j0 review + 联调包 |
| 62 | v1 round-2 审查：`MOBILEGL_IPC_VERB_BARRIER=0` 杀掉 pre-flight 导致 21 个条目全 skip、ctest 返 0 —— 对照必须把"选中条目被跳过"判为失败 |
| 63 | 联调头五部分全门实测 |
| 64 | c1 round-3 判定 MERGEABLE；余项转 c1f 门包 |
| 65 | 联调处置：27 个 inproc 错答归 P4b/P7 债；E2 clear-drop 对照未变红 → x2 |
| 66 | 过程规则改变：不过度验证、轮次间不审查、阶段收官由 codex 审一次、只读/非核心任务派给 codex |
| 67 | 相同 tuple 的重复 make-current 不重发 caps；不同 tuple 重发且 client 立即采纳 |
| 68 | class-C 普查：所有 Minecraft trace 首阻塞 = `DrawElements`；P5b 计划（Minecraft 优先） |
| 69 | v1 round 3；红米 A/B 记录（split 臂当时不可测） |
| 70 | c0b 落地；四包启动；APK lane 根因 = setup-android 默认 cmdline-tools 20.0 已无 `tools` 包 |
| 71 | x2 经 x2m 合入，P5 代码齐全 |
| 72 | f1 落地：34 个 clear/copy/mip 中止归零 |
| 73 | P5 收官审查：1 blocker / 10 major / 2 minor → r1（核心）/ r2（CI） |
| 74 | 配额中断期间由用户的 Codex root task 推进 P5b 第二波；不重启同名 Claude 包 |
| 75 | P5b 整波落地推送 `7cb29d46`；快速门全绿；修掉一个测试日志捕获缺陷（库按进程截断日志，`RunInChild` 的增量读把第 2、3 个子进程的诊断读空） |
| 76 | P5b 收官：主机门（`348d22a4`）retrace-verify 以钉住库续跑合并 79/79、complete=true；79 trace 普查三轮续跑补齐（72/6/1）；审查补遗追加钉定结果；**P5b 出口 = Redmi 正确性 8/8 + 四臂 A/B（barrier tax 首测）** |
| 77 | Redmi `2f7cbe2e` 钉频期望 1050→1100 MHz：2026-09-11 实测的厂商 thermal_pwrlevel 上限已消失，`pin_device.sh` 按 2026-09-16 实测更新；`iris-bsl` benchmark 8 组判为 fixture 123 帧上限（pull 同失败，非回归），123 帧序列折算补充表不进 200 帧尾门表 |
