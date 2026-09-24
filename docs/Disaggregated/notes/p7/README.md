# P7 — DirectVulkan（Magma）全量迁移（2026-09-23 收官，ID-P7-63）；Ph 必需小件

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 计划：[`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md)（Ph / P3b-P4b / P7 统筹，五波）；规范 `MobileGL/MG_Remote/CONTRACT-P7.md`；裁定 [`INTEGRATOR-DECISIONS-P7.md`](INTEGRATOR-DECISIONS-P7.md)（ID-P7-1..63）；收官交接 [`HANDOFF-2026-09-23.md`](HANDOFF-2026-09-23.md)。
- **§8 九项全绿**：Magma 两进程车道三臂同数绿；§3.2 六个 `@P7` 退役（树上 `@P7` 拒绝站点 0）；Magma `StateObjectDeathOps`；内部 shader 烘焙；OQ-8 `storageBlocks`；OQ-10 `kCapResidentSubData`；verify × split；**门 3** 真机 ssim 36/36（终局窗口 3 `p7w8-78e71f2b`）；**门 5** CTS 五块 AFTER ≤ 0.5 pp。40% 检查点 6/9 已过，未触发重定基线。G1 全程恒等；棘轮 186 → 171（余 86 标 `# P13`，门 4 关闭）。
- 关键修复：B3 找到 OpenRA 真机分歧的真因（wire 臂 completed-frame-serial floor 不健全）；M2 / M3 收掉 server 端 VkBuffer 与 descriptor set 的无界累积；X1 修 arm64 persistent-map tracker 从不认领 fault。
- **Ph**（不可信对端加固 + 配对）的必需小件随 P7 的 F / F2 包全部落地（ID-P7-57）：常量时间令牌、≥16 字节、无令牌只 loopback、`Welcome.dataNonce` 数据面绑定、每会话闩、`SEG_EVENT` 弃投闩、fork 前认证 + 退避、fuzz 三臂进 CI。OQ-21 裁定令牌足够、不做 TLS。
- 集成者审查稿在 [`../handoff/`](../handoff/)（`review-*.md`，路径被本目录交接文档引用，未移动）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P7** DirectVulkan（Magma）全量迁移
- **状态**：**✅ 已收官（2026-09-23，ID-P7-63）**：§8 九项全绿，终局窗口 3 门 3 36/36、门 5 PASS。**此前：wave 0–2 已落地，40% 检查点 6/9 已过（2026-09-22，`origin@b089e0de` + 集成树上的 B3）**：审计 + 五波计划见 `notes/p7/PLAN-PH-P34B-P7.md` §1.3 / §3，规范 `MG_Remote/CONTRACT-P7.md`，裁定 `notes/p7/INTEGRATOR-DECISIONS-P7.md`（ID-P7-1..33）。**wave 0** 仪器（Magma 两进程车道三臂、`link_ratchet.py` 186 基线、普查门 79 站点下行棘轮 + `MGPipeSessionFailHook`、设备跑器 `--matrix/--repeat/--archive-dir`、CTS caselists）`origin@801f8eda`；**wave 1** 设备窗口 #1（门 3 分母 36、E0–E7 归因、CTS `$BASE` 五块）；**wave 2** X1（arm64 persistent-map tracker 从不认领 fault）、F1（split 起会提前）、C（`StateObjectDeathOps`、OQ-8、OQ-10、vertex-layout 拆分）、A（对齐 / native-range）、B（mip 半退役、深度 mip 烘焙 (A)(D)、copy-image、default-color-blit、回读 fence）、B2（multisample shape/aspect、AllocatorDebtScope ×2 删、(B′)、棘轮 **186→173**）、B3（**OpenRA 真机分歧的真因**：wire 臂 completed-frame-serial floor 在中帧 pooled-fence 提交退休时无证据上升 → 帧首次 `glBufferSubData` 走无序 host memcpy；计数 26→0，spawn 臂 red-once；ID-P7-33）——**树上 `@P7` 拒绝站点归零、§3.2 六退役全成、Magma 三臂 95/74/76**。**真机**：p7w4 门 3 AFTER 34/36 与 monolith 差 ≤ 0.0005；OpenRA p7w5 **27/27 golden**（ID-P7-34，项关闭）；bsl-esc-menu = server 死 VkBuffer 无界累积（一帧 1.3 M 调用，ID-P7-32，**M2** 在修）。**已到集成树（2026-09-22 夜，ID-P7-37）**：B2 返工 r1 + r2、B3 返工 + 修复轮、V1、F 片 1–3 已推送 `origin@87584d0b`（2026-09-22 夜，ID-P7-39；pipe 门全绿：magma 97 / 76 / 71、full 535、verify 构建 1140 / 1074、OpenRA split 回放 0 条 unsound；四份 fable 审查 land with fixes，修复轮已落地，ID-P7-44）；**M2 + M2 r2 已推送**（主机 bsl spawn server 825 → 546 MiB；memo destroy-epoch 关闭 wire store 句柄复用的 ABA；第二个泄漏 = descriptor set → M3；ID-P7-46）；V1 修复 r2 已推送（read 侧负控数到 2、FATAL=0 摘要在 Close 前写出，ID-P7-47）；**在跑**：F2 待派；窗口 1b 矩阵在跑（配对窗口在直连 LAN 上同样触发，ID-P7-41）。提交尾注已按用户令改写（origin `3c80cd62`）。**2026-09-23 更新（ID-P7-49–54）**：M3 已在 7ea7ce89 推送；B4 已集成于 pipe b19297d5，源码审查 land、完整集成门全绿（ID-P7-51）。p7w6 DirectGLES × TCP 38 例对照与 Photon / D24 同 APK monolith 对照已结案（ID-P7-52/53）；两项错图亦在 monolith 复现。F2 PH-4/5/PH-2 已在包分支 commits `2c6a6e25` / `51480f8c` 落地并通过 Luna 审查（ID-P7-54）；该矩阵不替代门 3。**CI 与门 4（ID-P7-55/56）**：CI 自 P6.5 起的七个红因已修；棘轮 171，`p7-magma` 余 86 标 `# P13`，门 4 关闭。**F2 与门 5 修复（ID-P7-57/58）**：Ph 余项与 fuzz 三臂落地；门 5 预演的三个 inproc CTS 缺陷修掉；窗口 2 工装与判读裁定（ID-P7-59）。**窗口 2 与收官审查（ID-P7-60–62）**：门 5 PASS；门 3 严格 33/36，三例 Iris monolith 同样不稳定 → 分布判据后 36/36；codex 收官审查修复、spawn 冷启动心跳（协议修订 2）、1:1 blit 最近邻落地；终局窗口 3 复跑。**余**：F2 PH-6 / PH-1 (3)(4) / PH-7 (5)、fuzz 臂 2 与完整包门；wave 3 棘轮 88；wave 4 CTS AFTER 和门 3；wave 5 fuzz 三臂与整体审查。逐日状态见 [`CURRENT_STAGE_STATUS.md`](../../CURRENT_STAGE_PROGRESS.md)
- **落地什么 / 范围**：`38919d45` 已补实际游戏的 server buffer consumers 和 Android 旋转 blit（[报告](../p5f/magma-inproc-fix.md)）；`194382c9` 又完成 Magma run-ahead、GPU 延迟退休与 VAO 身份碰撞修复（[报告](../p5f/magma-runahead.md)）；`ed507b1a..d56c0e83` 29 个 DirectVulkan 提交的 shape 波（随 `e1bf3677` 合入，本行此前未记）已退役 packed-float mip 原生 blit（`5310ff62`）、resolve（`ec4ca06d`）、占位图像（`WirePlaceholderImages.inc`）、copy-image 端点、XFB 捕获 span、纹理回读；`70fb6689` 链关闭 inproc RD32 性能回归（86.85→116.73 fps）。其余工作（2026-09-22 审计改判）：~~`SetupDrawSnapshot` 探测字段塌成 dirty mask~~ 与 ~~`VertexInputStateFactory` 内容寻址 CSO~~ **本阶段拒做**（`SetupDraw` 在非 monolith 传输下分支到 `SetupWireDraw`，monolith 快照机制在 split 下不跑；无性能门；ID-P7-5）；占位纹理原生化**已落地**（缺 split 车道条目）；具名 UBO host payload（D-B8）走了直绑常驻 `VkBuffer` range 的备选，欠一次测量 + 裁定关 OQ-6；内部 shader 烘焙**半已落地**（颜色 blit `WireColorBlitSpirv.h`，仅 split 臂），欠深度 mip 烘焙 + `MOBILEGL_BAKED_INTERNAL_SHADERS` 新鲜度门（0 处）；D18 按 ID-P7-6 读法一句闭。**新增必须项**：Magma `StateObjectDeathOps`（六处特判）、OQ-8 归档 `storageBlocks`（`MagmaProgramSource::EnsureStorageBlocks` 每 draw 重跑 SPIRV-Reflect）、OQ-10 `kCapResidentSubData` 从未发布（split 下双后端退回就地 memcpy）、两处 `MagmaP7AllocatorDebtScope`。**P6 真机发现（2026-09-22 归此）**：`DirectVulkan` 分离路径在 Redmi 上两次观测 0.988998 / 0.976494（inproc 与 spawn 同表现，pbuffer monolith 1.0；case 归属、重复次数未记录，原始证据不在树内；取自 run-ahead ARMED 下，**lockstep 对照从未在 golden 口径下跑过**——wave 1 的 E1）——Magma × 真实 GPU；树上 `@P7` 具名拒绝**实为 15 处 / 4 文件**（`multisample-blit-shape` / `-aspect`、`depth-stencil-mipmap`（已窄化到 D|S 合并）、`vertex-layout`、`vertex-format-conversion`、`texel-buffer-native-format`、`unaligned-{texel,storage,atomic}-buffer-range`、`storage-buffer-native-range`、`uniform-block-native-range`、`uniform-buffer-byte-tail` / `-dynamic-offset`、`copy-image-in-place`、`default-color-blit-shape`；外加 Magma 无 `StateObjectDeathOps` 与 `buffer-legacy-arm`），**全部是绕过 `Session::Fail` 与普查门的裸 abort**；必须退役 `.6/.8/.9/.11/.12/.13`，其余改 decline 即可；链接实验分给 P7 的 **101 个符号**（D12）。`DynamicBackendParameters` 定宽重写**不再是 P7 的**——它是跨平台 wire 的前提，归 P6.5 wf
- **验收门 / 证据**：集成 + trace 在 Magma 的 push 与 split 下全绿（inproc 半已是硬门；**spawn / TCP 臂今天 DirectVulkan 条目为 0**——四条 split 臂硬编码 `MOBILEGL_BACKEND_TYPE=DirectGLES`，wave 0 补后端维度）；verify 零分歧（verify 今天只注册 monolith，verify+split 撞旧 `PipeRespecifyScope` 断言，先清）；真机 DirectVulkan split ssim 回到 1.0（分母排除 rd12 in-world 与 create-indirect，ID-P7-4；必须带 `RUN_AHEAD=0` 对照臂）；184 符号棘轮（契约 §12.1）下降 P7 的 101 个——原 "`nm -D libMobileGLServer.so | grep glslang` 为空" 不可达，D12 裁定 server 镜像就是整个 `libMobileGL.so`；仪器 `scripts/link_ratchet.py`（存符号列表、只对新增红）归 wave 0；CTS 五块（`texture_*` / `shader_image_*` / `packed_pixels` / `direct_state_access.*` / `uniform_buffer_object*`）先立 monolith×DirectVulkan 基线再比 inproc，≤0.5 pp 且新 crash = 0 单独硬红。**再基线检查点：中点完成子系统 < 40% 立即重定基线**（分母 = `notes/p7/PLAN-PH-P34B-P7.md` §4 的 9 项）

### Ph 行（原 `ROADMAP.md`）

- **阶段**：**Ph** 不可信对端加固 + 配对
- **状态**：**已审计（2026-09-22，`notes/p7/PLAN-PH-P34B-P7.md` §1.1）**：P6.5 已在 LAN 上跑 `tcp://0.0.0.0`，「P12 之前」的顺序陈述落后于树。必需小件（supervisor 命名子进程死亡、两处漏斗旁路、D11 五处上限、常量时间令牌 + 数据面绑定、`SEG_EVENT` 弃投闩）随 P7 波次推进；fuzz 臂 2 在 P7 之后；OQ-21 已裁定令牌足够（ID-P7-3）。**进展（2026-09-22 深夜）**：supervisor 命名子进程死亡 + 两处头层漏斗旁路已随 wave 0 落地（`origin@801f8eda`，`abort_sites` 79 成下行棘轮）；其余小件是簇 **F** 包：**片 1–3 已到集成树（2026-09-22 夜，ID-P7-37）**——令牌合一 / ≥16 字节具名拒绝 / 无令牌只 loopback / smoke 进 CI；`Welcome.dataNonce` 绑定，TCP 上 250 ms 配对窗口消失，`wireFingerprint` 变更；PH-8 钳制证明——片 4–6 与 PH-7 (5) 需要对端字节的 fuzz 驱动，另派 F2（原顺序：常量时间令牌 / `Refuse{Authentication}` / ≥16 字节 + `tcp_supervisor_smoke.py` 接 CI → `Welcome.dataNonce` 数据面绑定 → PH-8 单测 → D11 五处 + PH-2 → PH-6 drop-with-latch → PH-1 (3)(4)）；逐日状态见 [`CURRENT_STAGE_STATUS.md`](../../CURRENT_STAGE_PROGRESS.md)**F2 完成（2026-09-23，ID-P7-57）**：PH-1 (3)(4) 每会话闩、PH-6、PH-7 (5) fork 前认证 + 退避、fuzz 三臂进 CI——Ph 必需小件全部落地。**codex closeout（p7/int3）**：PH-5 界改为元素最小编码字节数（finding 3）；`DrainRing` 在每次 pop 前查闩（finding 6：控制线程闩的窗口缩窄到查闩到 pop，未关闭）。
- **落地什么 / 范围**：`Session::Fail` 的策略翻成每会话闩（**按 ID-P7-1 重定界**：98 处 `SessionFail` + 6 处 `WireLogFatal` 的字面翻转拒做——≥9 处返回引用没有诚实返回值；只对对端字节可达站点 latch-and-decline；P6 dl 收口到的是**两个漏斗 + 两处头层旁路**（`Server/StagedShadow.h`、`Server/StagedTextureStore.h`），不是一处；P6.5 fork-per-session 已给出「下一连接照常服务」）；handle slot 预算（`BackendSlotTable` 的两条拒绝从 `MOBILEGL_ASSERT` 变 release 下的具名拒绝，杜绝静默 null twin）；readback 尺寸门挪 server 侧且在 resize **之前**设绝对上限；`StagedTextureStore` 按该 level 宣告范围设界；`ArchiveVector` 按 `count × 元素最小编码字节数` 设上限（codex closeout finding 3 改自 `count * sizeof(element)`，后者拒掉编码器自己写的紧凑归档；map / set 仍按每项 1 字节）；`SEG_EVENT` 溢出不再能让一个不排空（或仅涓流排空）的对端在 30 秒内、乃至瞬间用 `Fatal{EventRingOverflow}` 打掉承载它的 server 进程（drop-with-latch，不等 PH-1，ID-P7-2；原文「60 秒」是两轮 30 s 的算术上界）；**配对 / 认证**（`Hello` 带令牌与无令牌只 loopback **已由 P6.5 ct 落地**；欠常量时间比较、`Refuse{Authentication}` 入词表、≥16 字节、**数据面按 `Welcome.dataNonce` 绑定到已认证的控制连接**（今天 `AcceptPair` 按到达顺序配对、从不查 peer）、fork 前认证；OQ-21 裁定令牌足够、不做 TLS）；段 / 窗口尺寸由 server 钳制（P6.5 nd 已陈述，欠一条钳制单测）
- **验收门 / 证据**：非 loopback 监听在无令牌时具名拒绝（机制已在；名字入词表 + `tcp_supervisor_smoke.py` 接 CI 后算达成）；畸形帧 / 超界计数 / 不排空对端的模糊臂下 server **不 abort、会话闩住、下一连接照常服务**（臂 1 机制已在、欠端到端脚本；臂 2 = PH-1 (3)(4) + D11；臂 3 = PH-6）；`Session::Fail` 站点普查门扩到 server 镜像且 `abort_sites` 变棘轮（今天只扫 `MG_Remote`、站点数记了不比，wave 0）；契约 §12.2 五处无界分配各一条 red-once（**五处全开**，不是两处）

## 收官状态页（原 `CURRENT_STAGE_STATUS.md`，2026-09-23）

**更新：2026-09-23（ID-P7-49–63）** · 交接 [notes/p7/HANDOFF-2026-09-23.md](HANDOFF-2026-09-23.md)。**P7 收官**：§8 九项全绿。CI 七根因修、F2、门 5 三缺陷、codex 收官审查修复、spawn 冷启动心跳（协议修订 2）均已落地推送。下一阶段按 ROADMAP：P8 / P9 / P12 余项。

### 1. 出口门总览（CONTRACT-P7 §8 的 9 项分母）

| # | 子系统 | 状态 | 证据 |
|---|---|---|---|
| 1 | Magma 两进程车道三臂同数绿 | ✅ | M3 集成树门：magma-split/spawn/tcp **102 / 81 / 71**，full-split **540**；OpenRA DirectVulkan inproc + spawn 回放 1.000000、0 unsound |
| 2 | §3.2 六个 `@P7` 退役 | ✅ | A（byte-tail、native-range、对齐 decline）、B（copy-image、default-color-blit、mip 半退役）、B2（multisample shape/aspect）、C（vertex-layout 拆分）；树上 `@P7` 拒绝站点 **0** |
| 3 | `StateObjectDeathOps` | ✅ | C，`dce9953d..cf7ca59f` |
| 4 | 烘焙 (A)(D) + (B′) | ✅ | B 深度 mip 烘焙；B2 disaggregated 构建的 monolith 臂改用烘焙模块 |
| 5 | OQ-8 反射归档 `storageBlocks` | ✅ | C |
| 6 | OQ-10 `kCapResidentSubData` 按 server 表发布 | ✅ | C |
| 7 | verify × split（门 2） | ✅ | B4 集成树 verify build：unit 2439、integration-verify 1152、integration-verify-split 1086，全绿；V1/V1 r2 负控与 8 verify trace 证据仍有效 |
| 8 | 真机 ssim 1.0（门 3，分母 36） | ✅ | 终局窗口 3（`p7w8-78e71f2b`）**36/36**：33 例四臂逐位同，三例 Iris 按 ID-P7-62 分布判据通过，OpenRA 四臂 1.000000（ID-P7-63） |
| 9 | CTS 五块 AFTER ≤ 0.5 pp（门 5） | ✅ | 窗口 2 与终局窗口 3 均 PASS：shader-image +4.35 pp、texture +0.96 pp、其余持平，0 新 crash、0 流失；UBO（GTF）记 unrun |

**40% 检查点（§8 中点）已过：6/9**，不触发重定基线。G1 全程恒等（pull `.text` `0xa52203`、符号 0/0）；census 79 站点 / 0 未标；棘轮 186 → 173 → **171**（ratchet88：`p7-magma` 余 86 全标 `# P13`，门 4 关闭，ID-P7-55）。

### 2. 已落地（`feat/disaggregated`，按合并序）

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
| codex 收官修复 | 越界 / 空图像单元 nullDescriptor 或按单元私有占位、PH-5 最小编码宽度、闩 pop 前再查、探针按设备缓存、判读器硬编码 §7.2 常量 | ID-P7-60 |
| spawn 冷启动心跳 | server 执行 surface 操作期间发 `SurfaceProgress`，client 预算量沉默（封顶 120 s）；控制协议修订 2 | ID-P7-61 |
| 门 3 判读 + blit | monolith 不稳定的 case 用最近邻分布判据、monolith ×5；1:1 LINEAR blit 走最近邻 | ID-P7-62 |

### 3. 真机（Redmi 2f7cbe2e，Adreno 830）

- 当前 APK：**p7w8**（`26.09.78e71f2-trace`，控制协议修订 2），supervisor `0.0.0.0:40613`，`/data/local/tmp/mgcts` 为 `$BASE` 库。
- 终局窗口 3 证据 `notes/p7/device-window-2/final-p7w8/`，窗口 2 `notes/p7/device-window-2/window2-p7w7/`，三例 Iris 不确定性追查在 ID-P7-62。

## 里程碑记录（原 `ROADMAP.md`）

- **P7 wave 2 落地（2026-09-22）**：X1 / F1 / C / A / B / D1 / D2 / D3 / B2 / B3 十包合入 `feat/disaggregated`（`origin@b089e0de`，B3 在集成树上待推），每包八车道全绿、G1 恒等、每包一条两进程臂 red-once、每包一次 fable 聚焦审查；子系统分母 **6/9** 于 B2 落地时越过 40% 中点，不触发重定基线。真机 p7w4：门 3 34/36 与 monolith 同；OpenRA 的两个历史点值（0.976494 / 0.988998）被证明是同一帧一次 draw 的顶点前缀撕裂——wire 臂的 frame-serial floor 不健全，B3 关闭；bsl-esc-menu 是 server 死 VkBuffer 无界累积，M2 + r2 已落地（主机 spawn server 825 → 546 MiB；真机验收待 p7w6）。

## 本目录

| 文件 | 内容 |
|---|---|
| [`device-window-1-runbook.md`](device-window-1-runbook.md) | 设备窗口 #1 运行手册（wave 1，Redmi `2f7cbe2e`） |
| [`f1-caps-placeholder.md`](f1-caps-placeholder.md) | F1 — 占位 CapsMirror 回答「server 消费这个族吗」，并丢掉整整一个族的记录（P7 wave 2） |
| [`g5-msrbo.md`](g5-msrbo.md) | P7 门 5 包 g5-msrbo：`KHR-GL46.direct_state_access.renderbuffers_storage_multisample`（inproc × DirectVulkan） |
| [`HANDOFF-2026-09-22.md`](HANDOFF-2026-09-22.md) | P7 集成交接（2026-09-22 深夜，集成者会话结束态） |
| [`HANDOFF-2026-09-23.md`](HANDOFF-2026-09-23.md) | P7 集成交接（2026-09-23 收官） |
| [`INTEGRATOR-DECISIONS-P7.md`](INTEGRATOR-DECISIONS-P7.md) | P7 集成者裁定日志 |
| [`iris-census-1135c664.md`](iris-census-1135c664.md) | Iris trace 普查 @ `3c80cd62`（P7 wave 3 包 E1，主机 lavapipe） |
| [`link-ratchet.md`](link-ratchet.md) | 链接闭包棘轮（P7 wave 0，ID-P7-7） |
| [`magma-a.md`](magma-a.md) | wave 2 包 A — Magma 的 UBO / SSBO / texel 对齐与 range 簇 |
| [`magma-b.md`](magma-b.md) | P7 wave 2-B：Magma blit / copy / mip 簇（分支 `p7/magma-b`） |
| [`magma-b2.md`](magma-b2.md) | P7 wave 2-B2：Magma blit / copy / mip 簇里包 B 没有走完的部分（分支 `p7/magma-b2`） |
| [`magma-b3.md`](magma-b3.md) | P7 wave 2-B3：Magma wire 臂的「已完成帧序号地板」不可证（分支 `p7/magma-b3`） |
| [`magma-b4.md`](magma-b4.md) | P7 B4：Magma 提交等待与长帧清理（已落地） |
| [`magma-c.md`](magma-c.md) | P7 wave 2 package C — Magma object death, resident sub-data, the reflection archive, the vertex-layout split |
| [`magma-m2.md`](magma-m2.md) | P7 wave 4 M2：Magma wire 臂的延迟 VkBuffer 集有界（分支 `p7/magma-m2`） |
| [`magma-m3.md`](magma-m3.md) | P7 M3：长帧内 descriptor set 游标回卷（草稿，待门） |
| [`magma-two-process-first-run.md`](magma-two-process-first-run.md) | P7 wave 0 包 L：Magma 两进程车道的首轮（2026-09-22） |
| [`ph-f.md`](ph-f.md) | P7 wave 2-F：Ph 小件（分支 `p7/ph-f`） |
| [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) | Ph / P3b-P4b / P7 统筹计划（2026-09-22，基线 `feat/disaggregated@b95f5f1a`） |
| [`verify-split.md`](verify-split.md) | P7 wave 3 · V1：出口门 2 —— verify × split（分支 `p7/verify-split`） |
| [`x1-persistent-map-segv.md`](x1-persistent-map-segv.md) | X1 — the client-thread SIGSEGV in a mapped buffer under inproc (device window #1) |
| [`x2-caps-flake.md`](x2-caps-flake.md) | X2 — the `InitialCapsStartup` unit-test flake under load |
| [`device-window-1/`](device-window-1/) | 172 个文件：设备窗口 #1（wave 1）：门 3 分母 36、E0–E7 归因、CTS `$BASE`、各验证轮日志 |
| [`device-window-2/`](device-window-2/) | 51 个文件：窗口 2（`window2-p7w7/`）与终局窗口 3（`final-p7w8/`）证据、dry-run、判读工装 `tools/` |
