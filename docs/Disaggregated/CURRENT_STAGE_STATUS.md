# 当前阶段状态

> 这一页只回答一个问题：**现在在哪、什么已落地、什么在跑、下一步是什么**。随每次合并 / 推送 / 真机窗口更新；历史与设计不在这里（设计见 [`ARCHITECTURE.md`](ARCHITECTURE.md)，阶段表见 [`ROADMAP.md`](ROADMAP.md)，逐条裁定见 [`notes/p7/INTEGRATOR-DECISIONS-P7.md`](notes/p7/INTEGRATOR-DECISIONS-P7.md)，规范见 [`MG_Remote/CONTRACT-P7.md`](../../MobileGL/MG_Remote/CONTRACT-P7.md)）。

**阶段：P7 DirectVulkan（Magma）全量迁移**，并行流 **P3b/P4b 深化（Espryt，wave 2-D）** 与 **Ph 小件（簇 F）**。计划 [`notes/p7/PLAN-PH-P34B-P7.md`](notes/p7/PLAN-PH-P34B-P7.md)（五波）。

**更新：2026-09-22 夜** · `origin/feat/disaggregated` = `87584d0b`（自 `3c80cd62` 起 42 提交：B2 返工 r1/r2、B3 返工 + 修复轮、V1、F 片 1–3、E1；提交尾注已按用户令改写；四份 fable 审查均 land with fixes，修复轮在跑，ID-P7-39）。集成树 `~/w7/pipe` = origin。

## 1. 出口门总览（CONTRACT-P7 §8 的 9 项分母）

| # | 子系统 | 状态 | 证据 |
|---|---|---|---|
| 1 | Magma 两进程车道三臂同数绿 | ✅ | `integration-magma-{split,spawn,tcp}` 97 / 76 / 71，full-split 535（pipe，B2 r2 + B3 修复之后；tcp 少 6 = 六个 server 端旋钮条目的具名 parity 例外 `MAGMA_SERVER_ENV_KNOB_NO_TCP`）；OpenRA DirectVulkan inproc + spawn 回放 1.000000 且 0 条 `unsound-serial-complete`（每个包的门都跑） |
| 2 | §3.2 六个 `@P7` 退役 | ✅ | A（byte-tail、native-range、对齐 decline）、B（copy-image、default-color-blit、mip 半退役）、B2（multisample shape/aspect）、C（vertex-layout 拆分）；树上 `@P7` 拒绝站点 **0** |
| 3 | `StateObjectDeathOps` | ✅ | C，`dce9953d..cf7ca59f` |
| 4 | 烘焙 (A)(D) + (B′) | ✅ | B 深度 mip 烘焙；B2 disaggregated 构建的 monolith 臂改用烘焙模块 |
| 5 | OQ-8 反射归档 `storageBlocks` | ✅ | C |
| 6 | OQ-10 `kCapResidentSubData` 按 server 表发布 | ✅ | C |
| 7 | verify × split（门 2） | ✅（修复轮在跑） | V1 落地 pipe：pipe 的 verify 构建上 monolith verify **1140**、`integration-verify-split` **1074**（双后端 inproc，逐条武装证明）全绿；`VERIFY_CORRUPT` 10/10 红、`POISON_OMIT` 2/2 红（split 臂）；8 verify trace × DV × inproc 0 分歧；verify 只能 inproc（比对器要求推送态与 GL 上下文同进程）；fable 审查 land with fixes（server 侧 read hook 补红、poison 分角色、中性 pack 共享函数、逐条武装证明）→ 修复轮在跑 |
| 8 | 真机 ssim 1.0（门 3，分母 36） | 🔄 | p7w4：34/36 与 monolith 差 ≤ 0.0005；**OpenRA 已关**（ID-P7-33/34：真因 = wire 臂 frame-serial floor 不健全，B3 修；p7w5 **27/27 golden**，[`W5-verify/`](notes/p7/device-window-1/W5-verify/README.md)）；**bsl-esc-menu** = server 死 VkBuffer 无界累积（ID-P7-32，**M2** 在修）；终局形式（三遍逐位 + spawn 臂同会话）留 p7w6 |
| 9 | CTS 五块 AFTER ≤ 0.5 pp（门 5） | ⏳ | `$BASE` 已取（`notes/p7/device-window-1/CTS-base/`）；AFTER 在 wave 4 |

**40% 检查点（§8 中点）已过：6/9**，不触发重定基线。G1 全程恒等（pull `.text` `0xa52203`、符号 0/0）；census 79 站点 / 0 未标；棘轮 186 → **173**（B2 重基线，余 88 = monolith draw 路径，wave 3）。

## 2. 已落地（`feat/disaggregated`，按合并序）

| 波 / 包 | 内容 | 落地 |
|---|---|---|
| wave 0 | Magma 两进程车道、`link_ratchet.py`、普查门扩容 + `MGPipeSessionFailHook`、设备跑器 `--matrix/--repeat/--archive-dir`、CTS caselists | `origin@801f8eda` |
| wave 1 | 设备窗口 #1（Redmi 2f7cbe2e，DirectVulkan）：门 3 分母 36、E0–E7 归因、CTS `$BASE` | `notes/p7/device-window-1/` |
| X1 | persistent-map tracker 在 arm64 不认领 fault（bionic 指针标记 0xb4） | `4cbf78c2` |
| F1 | split 起会提到 `MG_State::Init()` 之前 + `Fatal{CapsBeforeFirstSnapshot}` | `df17c416..3179c497` |
| C | Magma `StateObjectDeathOps`、OQ-10、vertex-layout 拆分、OQ-8 `storageBlocks` | `dce9953d..cf7ca59f` |
| A | UBO/SSBO/texel 对齐与 native-range 行 | `450d3075..5937b4fc` |
| B | mip 形状半退役、深度 mip 烘焙、copy-image、default-color-blit、wire 回读 fence | `c4be3800..76b74693` |
| D1 | XFB 两洞、回调 10→9、resolver 具名 stop、readback 三臂 | `faf0a1f9..ae3f43ab` |
| D2 + 返工 | `TextureViewAlias`、TUS 门、memo 纯度门、`SubsystemDeps.def`、场景普查 | `da30c130..519445f9`、`b3334cf3..a2d7fbd7` |
| D3 + 审查修复 | view 陈旧缺陷（clean gate 读 storage record）、D-K2 单一陈述 | `1e1f4bad..00d02396`、`22761a69` |
| B2 | multisample 两处退役、AllocatorDebtScope ×2 删、(B′)、棘轮 173 | `452d33b5..3c2867d3` |
| B3 | **OpenRA 真因**：wire 臂 completed-frame-serial floor 不健全 → 有序路径 + 可证 floor + 32 处静默出口具名 | `76bbb6c3..9a494b6f`，`origin@3c80cd62` |
| B2 返工 r1 | tcp 五条 server 端旋钮条目改 split+spawn（parity 例外 `MAGMA_SERVER_ENV_KNOB_NO_TCP`）、翻转 MS 深/模板 resolve 逐行镜像、缩放 / X 镜像 / 跨格式 decline、`MsFlip.` 用例 | pipe `9fed478d..cc3c18c5`（审查 rework → r2 在跑，ID-P7-35） |
| B3 返工 | `WireDeclines.def` 53 行全站点 + 审计脚本、`StaleSerial.` parity 例外、注记 + `:273`/`:432`、`run_trace_case.cmake` 对 `unsound-serial-complete` 打红 | pipe `b8d10d07..75a64ce0`（审查 land with fixes → 修复轮在跑，ID-P7-36） |
| V1 | `PipeRespecifyScope` 放宽、`integration-verify-split`（1074 条，双后端 inproc）、两负控 split 臂红、8 verify trace × DV × inproc 0 分歧、比对器 ReadPixels 窗口 oracle 改中性 pack | `origin@87584d0b`（审查 land with fixes → 修复轮在跑，ID-P7-37） |
| B2 返工 r2 | 用例按 `IsSplitLane()` + 后端名 skip、逐行拷贝行距对齐 + stencil 腿、`DeclineWireDepthStencilResolveShape` 先决定、`MsFlip1.`、parity tail 恰匹配 | `origin@87584d0b`（审查 land with fixes → r3 在跑，ID-P7-38） |
| B3 修复轮 | 审计剥注释 / 字面量 + 按块回溯 + `--self-test`、spawn 无 server 日志 FATAL、注记双行 + `WaitForFrameSerial` 债 | `origin@87584d0b`（审查 land with fixes → 修复 r2 在跑，ID-P7-38） |
| F 片 1–3 | 常量时间令牌 / ≥16 字节 / 无令牌只 loopback / smoke 进 CI；`Welcome.dataNonce` 绑定（TCP 250 ms 配对窗口消失，指纹变更）；PH-8 钳制证明 | `origin@87584d0b`（审查 land with fixes → 修复轮在跑，ID-P7-39） |
| E1 | Iris 普查 231/231 全活、70/77 逐字节同、P9 例外表空；§7.2 同名重跑 24 绿 / 3 具名停止 / 0 错答 | `origin@87584d0b`（docs only，ID-P7-38） |

## 3. 真机（Redmi 2f7cbe2e，Adreno 830）

- 当前 APK：**p7w5**（`p7w5-713bea9a`，B3+B2+D3 之上）。验收 = OpenRA inproc / spawn × run-ahead / lockstep × 冷 / 热各 3、FIF=8 ×3、dump-armed ×3 → **27/27 `ace2af04` / 1.000000 / 0 px**；iterationt ×2 + 26.3 逐位同 p7w4（ID-P7-34）。
- p7w4 结论（[`W4-verify/README.md`](notes/p7/device-window-1/W4-verify/README.md)）：OpenRA 是与温度无关的约 50% 竞态、只出三张图；调色板纹理逐字节正确；错像素 100% 落在 call 30376 一次 draw 的 quad 前缀里；机制由裁判裁定并由 B3 计数证实。
- bsl-esc-menu-854：monolith 807 MiB 过，spawn 的 server 1.2 GiB 死（scudo `internal map failure`）；M1 归因 = 死 VkBuffer 无界累积（一帧 1.3 M 调用、整跑 2 次 swap）；M2 修复中。

## 4. 在跑（agent；实现方 `claude-opus-5-5`，审查方 fable）

| 包 | 内容 | 状态 |
|---|---|---|
| B2 r3（fable） | 用例按解析后的 transport 门控（`Full.` / `VerifySplit.` 重新跑）、pre-pass 含默认 draw framebuffer、措辞、§6 三条域外债 | 进行中（ID-P7-38） |
| B3 修复 r2（fable） | `LOGGED` 锚定 + 夹具、spawn FATAL 挪到普查前、inproc fail-closed、nit ×6、pull-library 控制证据核查 | 进行中（ID-P7-38） |
| V1 修复轮 | server 侧 read hook 红、poison 分角色、`MGPipeNeutralReadPixelsPack()`、逐条武装证明 + 具名例外表、措辞 | 进行中（Agent 通道） |
| M2 | `VkBufferManager` serial 门控回收（不假设帧有界）+ `MOBILEGL_IPC_WIRE_DEFERRED_MB` 水位线 + spawn red-once | 进行中 |
| F 修复轮（fable） | child 绑定后关 hand-off 端 + `MSG_DONTWAIT`（后续 DataBind 具名拒绝而非挂起）、listener 绑定后关、`protocol.fbs` 摘要 pin、旧 server + 新 client 症状写明、unix 配对 §12 债、nit ×6 | 进行中（ID-P7-39） |

## 5. 下一步（按序）

1. ~~推送~~ 已推 `origin@87584d0b`；四个修复轮（B2 r3、B3 修复 r2、V1 修复、F 修复）回来 → 各自门 + 复审 → 下一次推送。
2. **F2**：D11 五处 + PH-2、PH-6 drop-with-latch、PH-1 (3)(4)、PH-7 (5) fork 前认证——先要一个能向 spawn / TCP server 发畸形记录的对端字节驱动（fuzz 臂 2 的第一块）。
3. M2 落地 → p7w6 APK（**含 F 的 wireFingerprint 变更：手机 server 必须重部署**）→ bsl-esc-menu spawn 臂通过 → 门 3 分母 36 全部与 monolith 同（三遍逐位相同 + spawn 臂，§7.2）。
4. **B4**：裁判的 `WaitForSubmitsUpTo` 聚合等待（`Present:14094` / `WaitForSubmitIndex` / `WaitForFrameSerial`）+ 裁判点名的 Magma 债（§12）。
5. wave 3 余项：棘轮 88 的 monolith draw 路径 `#if`。~~§7.2 同名重跑、Iris trace 普查~~ **已做（包 E1，主机 lavapipe，基 3c80cd62 = 现 `3c80cd62`）**：Iris 普查 77 行 × {monolith, inproc, spawn} 231 次全活，split 与 monolith 逐字节相同 70 / 77 行（7 行分歧 = Magma monolith draw 路径 vs wire 路径的差异或运行噪声，均在阈值内），P9 例外表为空——`texture-remint-pull` 在头上从未到达（[`notes/p7/iris-census-3c80cd62.md`](notes/p7/iris-census-3c80cd62.md)）；27 个 P5 wrong-answer 同名 24 绿 / 3 具名停止（`PIPE_PUSH=0` 控制臂）/ 0 错答（`MEASUREMENTS.md` §7.2 末）。
6. wave 4：CTS AFTER（inproc × DV 五块 vs `$BASE`，≤ 0.5 pp，新增 crash = 0）、门 3 终局三遍。
7. wave 5：F 余片、fuzz 三臂、P7 收官异模型整体审查（ID-66）。

## 6. 阻塞 / 需要人

- P6.5 残余 39 例 TCP 矩阵（窗口 1b）等 WSL 主机路由：Arch 里 `sudo ip route add 192.168.21.181/32 via 192.168.31.1 dev eth0 metric 10`（设备 supervisor 在 `0.0.0.0:40613` 带 token，冻结制品 `~/w7/logs/p7w1b/host/`）。
