# 当前阶段状态

> 这一页只回答一个问题：**现在在哪、什么已落地、什么在跑、下一步是什么**。随每次合并 / 推送 / 真机窗口更新；历史与设计不在这里（设计见 [`ARCHITECTURE.md`](ARCHITECTURE.md)，阶段表见 [`ROADMAP.md`](ROADMAP.md)，逐条裁定见 [`notes/p7/INTEGRATOR-DECISIONS-P7.md`](notes/p7/INTEGRATOR-DECISIONS-P7.md)，规范见 [`MG_Remote/CONTRACT-P7.md`](../../MobileGL/MG_Remote/CONTRACT-P7.md)）。

**阶段：P7 DirectVulkan（Magma）全量迁移**，并行流 **P3b/P4b 深化（Espryt，wave 2-D）** 与 **Ph 小件（簇 F）**。计划 [`notes/p7/PLAN-PH-P34B-P7.md`](notes/p7/PLAN-PH-P34B-P7.md)（五波）。

**更新：2026-09-22 夜** · `origin/feat/disaggregated` = `3c80cd62`（B3 + ID-P7-33/34）；集成树 `~/w7/pipe` = `75858bfd`（B2 返工 r1 + B3 返工 + V1，未推：等 B2 r2 与 V1 门 / 审查，ID-P7-35/36）。

## 1. 出口门总览（CONTRACT-P7 §8 的 9 项分母）

| # | 子系统 | 状态 | 证据 |
|---|---|---|---|
| 1 | Magma 两进程车道三臂同数绿 | ✅ | `integration-magma-{split,spawn,tcp}` 96 / 75 / 71，full-split 535（pipe `75a64ce0`；tcp 少 6 条 = 六个 server 端旋钮条目的具名 parity 例外 `MAGMA_SERVER_ENV_KNOB_NO_TCP`）；OpenRA DirectVulkan inproc + spawn 回放 1.000000 且 0 条 `unsound-serial-complete`（B3 的新红条件已进 pipe 门） |
| 2 | §3.2 六个 `@P7` 退役 | ✅ | A（byte-tail、native-range、对齐 decline）、B（copy-image、default-color-blit、mip 半退役）、B2（multisample shape/aspect）、C（vertex-layout 拆分）；树上 `@P7` 拒绝站点 **0** |
| 3 | `StateObjectDeathOps` | ✅ | C，`dce9953d..cf7ca59f` |
| 4 | 烘焙 (A)(D) + (B′) | ✅ | B 深度 mip 烘焙；B2 disaggregated 构建的 monolith 臂改用烘焙模块 |
| 5 | OQ-8 反射归档 `storageBlocks` | ✅ | C |
| 6 | OQ-10 `kCapResidentSubData` 按 server 表发布 | ✅ | C |
| 7 | verify × split（门 2） | 🔄→✅ | **V1 已报告并 cherry-pick 到 pipe**（`a1d17566..75858bfd`）：monolith verify 1136 不变、`integration-verify-split` 1070（DirectGLES 533 / DirectVulkan 537，逐条 inproc 武装证明）、`VERIFY_CORRUPT` 10/10 红、`POISON_OMIT` 2/2 红（split 臂）、8 verify trace × DV × inproc 0 分歧；verify 只能 inproc（比对器要求推送态与 GL 上下文同进程）；pipe 门 + fable 审查在跑 |
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
| V1 | `PipeRespecifyScope` 放宽、`integration-verify-split`（1070 条，双后端 inproc）、两负控 split 臂红、8 verify trace × DV × inproc 0 分歧、比对器 ReadPixels 窗口 oracle 改中性 pack | pipe `a1d17566..75858bfd`（门 + 审查在跑） |

## 3. 真机（Redmi 2f7cbe2e，Adreno 830）

- 当前 APK：**p7w5**（`p7w5-713bea9a`，B3+B2+D3 之上）。验收 = OpenRA inproc / spawn × run-ahead / lockstep × 冷 / 热各 3、FIF=8 ×3、dump-armed ×3 → **27/27 `ace2af04` / 1.000000 / 0 px**；iterationt ×2 + 26.3 逐位同 p7w4（ID-P7-34）。
- p7w4 结论（[`W4-verify/README.md`](notes/p7/device-window-1/W4-verify/README.md)）：OpenRA 是与温度无关的约 50% 竞态、只出三张图；调色板纹理逐字节正确；错像素 100% 落在 call 30376 一次 draw 的 quad 前缀里；机制由裁判裁定并由 B3 计数证实。
- bsl-esc-menu-854：monolith 807 MiB 过，spawn 的 server 1.2 GiB 死（scudo `internal map failure`）；M1 归因 = 死 VkBuffer 无界累积（一帧 1.3 M 调用、整跑 2 次 swap）；M2 修复中。

## 4. 在跑（agent；2026-09-22 夜六包撞 session limit 后按 worktree 未提交 diff 重派，实现方 `claude-opus-5-5`，审查方 fable）

| 包 | 内容 | 状态 |
|---|---|---|
| B2 返工 r2 | 用例按 `IsSplitLane()` skip（monolith DirectVulkan / Espryt 两条新 §12 行）、逐行拷贝行距对齐 + stencil 腿 red-once、部分效应陈述、两个 `MGLOG_E_ONCE`、`MsFlip1.`、§7 双行 | 进行中（ID-P7-35） |
| B3 修复轮 | 审计剥注释 / 字面量 + 按块回溯 + `--self-test`、`run_trace_case.cmake` spawn server 日志缺失 fail-closed、注记标「包树」+ §12 `WaitForFrameSerial` 债 | 进行中（ID-P7-36） |
| M2 | `VkBufferManager` serial 门控回收（不假设帧有界）+ `MOBILEGL_IPC_WIRE_DEFERRED_MB` 水位线 + spawn red-once | 进行中 |
| V1 审查 | fable：放宽的形状、比对器 oracle 收窄（server ReadPixels 窗口的盲点）、`PipeInputs.cpp` 越分区改动、车道武装证明、inproc-only 的理由 | 进行中；pipe 门含首次 `build-verify` |
| F | Ph 小件：slice 1 令牌（常量时间 / ≥16 字节 / `Refuse{Authentication}` / smoke 接 CI）收尾 → `Welcome.dataNonce` → PH-8 → D11 + PH-2 → PH-6 → PH-1 (3)(4) | 进行中 |
| E1 | Iris trace 普查（39 × 2 后端 × 3 臂）+ `MEASUREMENTS.md` §7.2 同名重跑 + P9 例外表 | 进行中 |

## 5. 下一步（按序）

1. B2 返工 r2 落地 → pipe 全门（含 DirectGLES 三条 split 车道）→ fable 复审 → 推送（B3 返工同批）。
2. M2 落地 → p7w6 APK → bsl-esc-menu spawn 臂通过 → 门 3 分母 36 全部与 monolith 同（三遍逐位相同 + spawn 臂，§7.2）。
3. **B4**：裁判的 `WaitForSubmitsUpTo` 聚合等待（`Present:14094` / `WaitForSubmitIndex` / `WaitForFrameSerial`）+ 裁判点名的 Magma 债（§12）。
4. wave 3 余项：`MEASUREMENTS.md` §7.2 同名重跑、Iris trace 普查（39 × 2 后端 × 2 传输）、棘轮 88 的 monolith draw 路径 `#if`。
5. wave 4：CTS AFTER（inproc × DV 五块 vs `$BASE`，≤ 0.5 pp，新增 crash = 0）、门 3 终局三遍。
6. wave 5：F 余片、fuzz 三臂、P7 收官异模型整体审查（ID-66）。

## 6. 阻塞 / 需要人

- P6.5 残余 39 例 TCP 矩阵（窗口 1b）等 WSL 主机路由：Arch 里 `sudo ip route add 192.168.21.181/32 via 192.168.31.1 dev eth0 metric 10`（设备 supervisor 在 `0.0.0.0:40613` 带 token，冻结制品 `~/w7/logs/p7w1b/host/`）。
