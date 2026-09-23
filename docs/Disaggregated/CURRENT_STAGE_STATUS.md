# 当前阶段状态

> 这一页只回答一个问题：**现在在哪、什么已落地、什么在跑、下一步是什么**。随每次合并 / 推送 / 真机窗口更新；历史与设计不在这里（设计见 [`ARCHITECTURE.md`](ARCHITECTURE.md)，阶段表见 [`ROADMAP.md`](ROADMAP.md)，逐条裁定见 [`notes/p7/INTEGRATOR-DECISIONS-P7.md`](notes/p7/INTEGRATOR-DECISIONS-P7.md)，规范见 [`MG_Remote/CONTRACT-P7.md`](../../MobileGL/MG_Remote/CONTRACT-P7.md)）。

**阶段：P7 DirectVulkan（Magma）全量迁移**，并行流 **P3b/P4b 深化（Espryt，wave 2-D）** 与 **Ph 小件（簇 F）**。计划 [`notes/p7/PLAN-PH-P34B-P7.md`](notes/p7/PLAN-PH-P34B-P7.md)（五波）。

**更新：2026-09-23（ID-P7-49–59）** · 当前交接 [notes/p7/HANDOFF-2026-09-23.md](notes/p7/HANDOFF-2026-09-23.md)。CI 自 P6.5 起的七个红因已修，`1997fd88` 上 Test 全绿（含 P6.5 以来首次跑的 retrace 链）；棘轮 171、门 4 关闭（ID-P7-55/56）。F2（Ph 余项 + fuzz 三臂）与门 5 预演抓出的三个 inproc CTS 缺陷已落地（ID-P7-57/58）；门 3 与 CTS AFTER 在设备窗口 2（ID-P7-59）。

## 1. 出口门总览（CONTRACT-P7 §8 的 9 项分母）

| # | 子系统 | 状态 | 证据 |
|---|---|---|---|
| 1 | Magma 两进程车道三臂同数绿 | ✅ | M3 集成树门：magma-split/spawn/tcp **102 / 81 / 71**，full-split **540**；OpenRA DirectVulkan inproc + spawn 回放 1.000000、0 unsound |
| 2 | §3.2 六个 `@P7` 退役 | ✅ | A（byte-tail、native-range、对齐 decline）、B（copy-image、default-color-blit、mip 半退役）、B2（multisample shape/aspect）、C（vertex-layout 拆分）；树上 `@P7` 拒绝站点 **0** |
| 3 | `StateObjectDeathOps` | ✅ | C，`dce9953d..cf7ca59f` |
| 4 | 烘焙 (A)(D) + (B′) | ✅ | B 深度 mip 烘焙；B2 disaggregated 构建的 monolith 臂改用烘焙模块 |
| 5 | OQ-8 反射归档 `storageBlocks` | ✅ | C |
| 6 | OQ-10 `kCapResidentSubData` 按 server 表发布 | ✅ | C |
| 7 | verify × split（门 2） | ✅ | B4 集成树 verify build：unit 2439、integration-verify 1152、integration-verify-split 1086，全绿；V1/V1 r2 负控与 8 verify trace 证据仍有效 |
| 8 | 真机 ssim 1.0（门 3，分母 36） | 🔄 | p7w5 OpenRA 27/27 golden；p7w6 bsl-esc-menu DirectVulkan inproc/spawn 6/6 且图像 SHA 与 monolith 相同；DirectGLES × TCP 38 例窗口矩阵 16/29 pass、8 idle timeout、2 visual failure、3 missing result。Photon / D24 两项错图已由同 APK DirectGLES monolith 复现；36 例 DirectVulkan 三遍、同会话 monolith 对照和 M3 后 APK 回归待跑 |
| 9 | CTS 五块 AFTER ≤ 0.5 pp（门 5） | ⏳ | `$BASE` 已取（`notes/p7/device-window-1/CTS-base/`）；AFTER 在 wave 4 |

**40% 检查点（§8 中点）已过：6/9**，不触发重定基线。G1 全程恒等（pull `.text` `0xa52203`、符号 0/0）；census 79 站点 / 0 未标；棘轮 186 → 173 → **171**（ratchet88：`p7-magma` 余 86 全标 `# P13`，门 4 关闭，ID-P7-55）。

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
| M2 | 孤儿 wire store 在 defer 路径回收 + `MOBILEGL_IPC_WIRE_DEFERRED_MB` 水位线 + 1,024 计数上限 + `wbuf[]` 计量 + `MagmaWireReclaimScenario`；主机 bsl spawn server 825 → 546 MiB、映射 41k → 5.8k | pipe `b81a8827..7d131527`（门全绿 magma 100 / 79 / 71，审查 land with fixes，ID-P7-43）；**r2** = memo destroy-epoch，must-fix 关闭：pipe `4d207468..f62703e9` + parity `1ab62ece`（门全绿 magma 101 / 80 / 71，集成者审查 land，ID-P7-46） |
| F 修复轮 | hand-off 端绑定后关 + `MSG_DONTWAIT`（多余 DataBind 具名拒绝）、listener 绑定后关、`sha256(protocol.fbs)` 钉到控制修订、定宽令牌比较、超额 Hello 记警、`MalformedHello`、文档更正 | pipe `0293aebb..9cea5140`（门全绿，集成者审查 land，ID-P7-44） |
| B3 修复 r3 + B2 r4 | 审计跨 `#endif` 只放行 disagg 守卫 + 缩进 `#else` 停走（16/16）、`EVIDENCE` 保护、tcp 信息；窗口腿独一底色、去死合取项、按符号引用 | pipe `bd849b28..4b96f55d`（门全绿，集成者审查 land，ID-P7-44） |
| M3 | 长帧 descriptor set 游标按 queue-idle 证明回卷，2049 SSBO 窗口用例 split/spawn 绿且 red-once 红；包树与 pipe 完整门绿 | pipe `9fd768d4`（ID-P7-50，集成者审查 land） |
| B4 | 聚合 submit 前缀等待、frame serial 重新扫描、Present 槽退休保护、pre-pass pending、texture prune 与 staged extent 清理 | pipe b19297d5（ID-P7-51；集成者 review land，完整 pipe 门通过；当前文档/citation/push 收尾中） |
| CI 修复 | 链接缝走工厂、TCP fixture headless + 资源锁守卫、D3 拒绝句、Magma-only decline、dual-block 登记、retrace 链（旋钮 / pull verify 日志 / spawn 冷启动预算）、并发组、80 个标题缩短 | pipe `eaef83b3..e751b07b`（ID-P7-56） |
| ratchet88 | `MGPipeTextureLegacyArmScope` 退役、余 86 标 `# P13`、棘轮 171 | `d49df354` / `d74e9923`（ID-P7-55） |
| F2 | Ph 余项：PH-1 (3)(4) 每会话闩、PH-6 事件通道弃投闩、PH-7 (5) fork 前认证 + 预认证有界 + 退避、fuzz 三臂进 CI；PH-4 两个回归（零尺寸层、`R8 == 0`）与 ID-49 回读在线内修掉 | F2 线 29 提交 + 集成 2 跟进（ID-P7-57） |
| 门 5 修复 | 超槽 ReadPixels 切带、深度/模板 resolve 臂由设备探针选（+ POST FIXED 行）、单采样深/模板 blit 改 copy、越界图像单元绑占位 | ID-P7-58 |
| 窗口 2 工装 | 门 3 / bsl / CTS AFTER 一条命令、可续跑、共享设备锁；判读按合同公式、五块必齐、同一 boot_id | ID-P7-59 |

## 3. 真机（Redmi 2f7cbe2e，Adreno 830）

- 当前 APK：**p7w6**，stamp `p7w6-0e16ca27`，已重装并重起 TCP supervisor；屏幕常亮设置 `stayon=15`。bsl-esc-menu DirectVulkan × pbuffer inproc/spawn 各三遍 **6/6**，PNG SHA 均与 E0a monolith 相同；OpenRA 的 p7w5 27/27 结论保留。p7w6 在 M3 落地前构建，后续设备回归需新 APK。
- DirectGLES × TCP 窗口 1b（p7w6）已结案，逐例状态与 phone reap 证据见 notes/p65/window-1b-p7w6.md：9 缓存跳过、29 次尝试（16 pass、8 idle timeout、2 visual failure、3 missing-result）。Photon v1.3b 与 D24.4.14 的 DirectGLES monolith 对照已在同设备、同 APK 下复现相同错误，见报告中的证据路径。
- 门 3 的 36 例 DirectVulkan 三遍逐位与同会话 monolith 对照仍待手机新 APK；CTS AFTER 五块也未运行。

## 4. 在跑

| 工作 | 状态 |
|---|---|
| 设备窗口 2（p7w7） | 最终功能 APK 构建 → reboot-clean → 门 3（36 例 × monolith ×1 + inproc ×3 + spawn ×3 + `RUN_AHEAD=0` ×1）→ bsl 映射 / `wbuf[]` → CTS AFTER 五块 → 判读 |

## 5. 下一步

1. 窗口 2 判读：门 3（臂内三遍逐位、与同会话 monolith 差 ≤ 0.0005、OpenRA 1.0）与门 5（每块 ≤ 0.5 pp、新 crash = 0）。
2. P7 收官异模型整体审查（codex，ID-66）。
3. P7 收官文档（ROADMAP / 本页 / 交接）。

## 6. 阻塞 / 需要人

- p7w6 TCP 矩阵和设备 monolith 对照已结束；后续门 3/CTS 仍不可同时 force-stop 同包 Service。当前没有需要用户决策的阻塞。
- X2 已关闭 `InitialCapsStartup` 的测试夹具乱序 flake；不再列为待修。
