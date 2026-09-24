# P4a — handle wave 2（Espryt）：FBO / 纹理 / sampler / program（`8c458cd5`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：十四条族调用接线（framebuffer、sampler state / view、texture params、shader images、shader state、draw / dispatch program、global constants）；纹理 / renderbuffer 复用 `resource_*`；六种 kind 按 `{slot, gen}` 重键；`CompositeResolver`；`Named = 3` framebuffer 记录；消费者门 + 客户端依赖表；子系统位 9–12，push 默认 `0x1fff`。
- 门：G1 0/0/0/0；G5 17 区；单元 1785×3；`integration-gpu` 1117 七臂；verify 920；八族拒绝普查 0。实际 1 天。
- 方法论：六个包 v1 全过自己的门、六份复审全判 REWORK，最贵的两个缺陷（丢上传、delete 后 UAF）只有**整体 diff 终审**抓得到。
- 设备换为 Redmi `2f7cbe2e`：P4a 自己在 Espryt 上 +3.4–5.7 pt，Magma 在噪声内。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P4a** handle wave 2（Espryt）：FBO / 纹理 / sampler / program
- **状态**：✅ `8c458cd5`
- **落地什么 / 范围**：十四条族调用接线（framebuffer state、sampler state / view、texture params、shader images、shader state、draw / dispatch program、global constants）；纹理 / renderbuffer 复用 `resource_*`（目录不加行）；六种 kind 按 `{slot, gen}` 重键；`CompositeResolver`；`Named = 3` framebuffer 记录；消费者门 + 客户端依赖表；位 9–12，push 默认 `0x1fff`；emulation 在 split 下具名 Fatal
- **验收门 / 证据**：全门：G1 0/0/0/0；G5 17 区；G2 2902；单元 1785×3；`integration-gpu` 1117 七臂；verify 920；retrace 79/79；八族拒绝普查 0；`PipeApplyPeek` 白盒对照。实际 1 天。§5

## 实测：P4a（`8c458cd5`，基线 `37da3c3a`，101 个提交，86 文件 / +30066 / −384）（原 `MEASUREMENTS.md` §5）

### 5.1 全门（`6035c9d7` 全量 + `8c458cd5` 复跑）

| 门 | `6035c9d7` | `8c458cd5` |
|---|---|---|
| **G1**（认定 resize 集为空） | 0 / 0 / 0 / 0，`.text` 不变 | 同上 |
| **G5** | P3a 十一函数 rc 0；P4a **17 区 / 3 文件** rc 0，self-test 8 个阴性对照按名变红 | 同上 |
| G2 / G14 | 差 0 / +275 | 差 0（2902 条）/ +314 |
| 单元 | 1772 × 3 | **1785 × 3** |
| `integration-gpu` | 1091/1091 × 七臂 | **1117/1117 × 七臂**（默认 `0x1fff`、`0x1ff`、`0`、`0x9ff`、`0x5ff`、pull、`ESPRYT_DISABLE_INVALIDATE_FLUSH=1`；DirectVulkan 559/559） |
| `integration-verify` | 896/896 零 `Fatal{` | **920/920** |
| retrace（79 例） | verify 79/79 armed 零分歧；push 79/79；G3b 12/12 | push 79/79 |
| 阴性对照脚本 | `g7_negative_control.sh`、`p3a_vertex_input_negative_control.sh`（`IsBgra`）、**`p4a_descriptor_negative_control.sh`（`Layered` 与 `borderColorForm`）** rc 0 | 同上 |
| 子系统对照套件 | `CsoContentAddressing` + `ResourceSubsystemControl` + `ObjectSubsystemControl` 24/24；`0x9ff` 依赖拒绝臂 14/14；`HandleRecycle`（verify）180/180 | 同上 + 184/184 controls |
| 八族拒绝普查 | — | **0**（逐用例读私有日志；`ctest -V` 是假零） |

G9 的"落地前必须红"没能通过公共 GL 达成（回读模拟自己会设 `GL_DEPTH_STENCIL_TEXTURE_MODE`）：改成 `PipeApplyPeek` 白盒断言，变异下 4/4 变红。真正红过再绿的两条来自终审：per-level respecify 丢上传（`0x1fff` 下读回全黑）与 delete-then-draw（`pure virtual method called`）。CTS `direct_state_access.framebuffers*` / `packed_pixels`（G15）未跑，随 P3b/P4b 补。

### 5.2 缝的分类（契约七次修正 `c0b`…`c0g` + 一轮缝类审计 + 一轮终审修复）

| 类 | 实例 | 症状 | 预防 |
|---|---|---|---|
| 编码没定死 | `MGPSubData::Target`、`DepthStencilMode`、`MGPSurface::Kind`、缺 `TextureTarget` | 两侧各自发明一套；`Texture1D == 0` 与 `kMGPipeResourceTargetBuffer == 0` 撞上 | 编码表放进契约 |
| 身份 vs 内容 | 内置 sampler：client 按身份铸、cache 按内容铸 | 查找永远落空 → 17 条 Iris trace 用驱动默认采样器 | 每 kind 两侧 handle 规则表 |
| 记录键错了维度 | framebuffer 记录按"当前绑定"存，DSA 按名字来 | 打进从没收到附件的 FBO | 记录按对象存；`Named = 3` |
| 进程级单例 vs 每上下文命名 | `CompositeResolver` 按管线 GL 名记忆 | 释放另一个上下文还活着的合成体 | 单例的键含上下文身份 |
| 破坏性客户端动作缺前置条件 | 按 acceptance 清 dirty，但 Magma 无消费者、依赖位只在服务端拒 | 上传丢失：66 条 DirectVulkan 用例、`0x7ff` 下 438/491 | 消费者门 + 依赖门都在客户端：一条不发 |
| 快门看不见自己的主体 | `glBindSampler`、SSO 下 `GetCurrentProgram()`、`glBindImageTexture` 只换 level | 记录停在上一次的值（`create-indirect` SSIM 0.887） | "记录字段 → setter → 快门"完备表；优先混入已有世代 |
| 清得太宽 | `resource_respecify` 清掉整张待上传表 | 已接受的那一级永久丢失 | 作用域随调用走 |
| 死亡没通知发射方 | 六个死亡 helper 只释放 slot | 已删句柄解析到已释放对象 → UAF | 死亡转发给每个 emitter |
| 门不能变红 | G7 脚本永远编译不过、`HighWater(ShaderCso)` 取段顶、G9 红前态公共 GL 不可见 | 绿得毫无意义 | 每个门带阴性对照并真跑过一次红 |

方法论结论：六个包的 v1 全部通过自己的门、六份对抗性复审全部判 REWORK，最贵的两个缺陷（丢上传、delete 后 UAF）是**整体 diff 终审**才抓到的——它们跨包，各方自洽。终审是唯一能看见跨包契约的那一轮。

### 5.3 Redmi 三臂 A/B、MC 26.3 的 p99、上传形状（记录项）

设备换为 Redmi `2f7cbe2e`（与小米同 SoC 同定频点，数值可比；GPU 当时钉 1050 MHz；主动风扇 level 2，40 个样本 40 个 PINNED）。APK `8c458cd5` Release 双臂；`--benchmark-no-finish`，尾 200 帧、best-of-3、逐线程 CPU p50 ms；`0x1ff` = P2+P3a 边界，`0x1fff` = P4a 默认。

| 用例 | 后端 | pull | `0x1ff` | `0x1fff` | Δ P2+P3a | Δ 合计 | **P4a 自己** |
|---|---|---|---|---|---|---|---|
| improved-transparency-26.3 | Espryt | 10.716 | 11.754 | 12.124 | +9.7% | +13.1% | **+3.4 pt / +0.37 ms** |
| improved-transparency-26.3 | Magma | 10.603 | 11.543 | 11.585 | +8.9% | +9.3% | +0.4 pt（噪声） |
| rd12-odinlite | Espryt | 8.210 | 10.798 | 11.182 | +31.5% | +36.2% | **+4.7 pt / +0.38 ms** |
| rd12-odinlite | Magma | — | — | — | — | — | 三臂全 `rc=1`（`scudo`，`dev` 侧，换机仍复现） |
| fabric-sodium | Espryt | 1.312 | 1.406 | 1.454 | +7.2% | +10.8% | +3.6 pt / +0.05 ms |
| fabric-sodium | Magma | 0.474 | 0.502 | 0.507 | +5.9% | +7.0% | +1.1 pt（噪声） |
| 1.21.4-in-world | Espryt | 2.369 | 2.732 | 2.867 | +15.3% | +21.0% | **+5.7 pt / +0.14 ms** |
| 1.21.4-in-world | Magma | 1.028 | 1.147 | 1.145 | +11.6% | +11.4% | −0.2 pt（噪声） |
| fabric-iris-bsl | Espryt | 1.727 | 1.740 | 1.811 | +0.8% | +4.9% | +4.1 pt / +0.08 ms |
| fabric-iris-bsl | Magma | 0.742 | 0.788 | 0.786 | +6.2% | +5.9% | −0.3 pt（噪声） |

读法：P4a 自己在 Espryt 上是 +3.4 – +5.7 pt（0.05–0.38 ms/帧），大头仍是 P2+P3a 的边界；Magma 两臂在四个用例上落在噪声内——消费者门在设备上的读数（Magma 一条 P4a 记录都不发）；`vanilla` 是 P4a 占比最高的用例（draw 少、状态切换密）。**MC 26.3 在 Adreno 上的 p99**：pull 25.297 → P4a 26.841 ms（+6.1%），仍在 21–26 ms 档。**上传形状 pull 与 push 逐项相同**（79 例两臂 `tex[emit/box/rect/jobs]`：18451/16060/2391/39926 对 18453/16062/2391/39928，唯一差异是 2 次 `trp` 带来的重放上传）。线索：设备上 26.3 Espryt 的 `sve` ≈ draw 数（9143 / 9138），桌面 ~0.07/draw——每 draw 重发一次 sampler-view 集合，进 P3b/P4b 优化清单。`acc/draw` 在这一波普遍下降而 CPU 上升，不是矛盾而是该计数器的定义。

### 5.4 DriverBench（`wsl_p4a_bench.sh`，`8c458cd5`，`mc_vanilla_draw` ns/draw）

Espryt T1（`0x1fff` − pull）= +1076.1，T2（`0x1ff` − pull）= +1064.7，T1 − T2 = +11.4；Magma T1 = +629.0、T2 = +452.6、T1 − T2 = +176.4。两臂绝对值在两轮之间各漂 ~200 ns 而差只有 10–130 ns，**桌面 bench 分辨不出 P4a 这一档**；可引用的是 T1 ≈ +1.1 µs/draw 的总边界（Espryt）与设备侧三臂表。Magma 的 +176 ns 不是"Magma 在跑 P4a"（消费者门让它一条不发），是 tracker 多算的快门加噪声。

## 里程碑记录（原 `ROADMAP.md`）

- **P3a / P4a 出口（2026-09-08）**：各 1 天，远低于 27 / 39 天的再基线绊线，未触发重定基线。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-P4A.md`](BRIEF-P4A.md) | P4a implementation brief — handle wave 2 (Espryt): framebuffer, texture, renderbuffer, sampler, program identity and descriptors |
| [`INTEGRATOR-DECISIONS.md`](INTEGRATOR-DECISIONS.md) | P4a integrator decisions (feat/disaggregated, 2026-09-08) |
| [`scout-docs-spec.md`](scout-docs-spec.md) | P4a docs specification — everything `docs/Disaggregated/` says about handle wave 2 |
| [`scout-espryt-framebuffer.md`](scout-espryt-framebuffer.md) | Scout — Espryt's framebuffer / renderbuffer / texture surface that P4a converts |
| [`scout-espryt-sampler-program.md`](scout-espryt-sampler-program.md) | P4a scout — Espryt's sampler / image / program surface |
| [`scout-frontend-state.md`](scout-frontend-state.md) | Scout — the frontend (MG_State/GLState + MG_Impl/GLImpl) as P4a's client emission must read it |
| [`scout-pipe-and-client.md`](scout-pipe-and-client.md) | P4a scout — the MGPipe scaffolding and the P3a client conventions |
| [`ab/`](ab/) | 5 个文件：小米三臂 A/B（含前缀 `a38bdab4` 的一轮） |
| [`ab-redmi/`](ab-redmi/) | 2 个文件：Redmi 两臂 / 三臂 A/B |
| [`bench/`](bench/) | 1 个文件：DriverBench |
| [`p4a-results/`](p4a-results/) | 41 个文件：各包的实现、评审与终审修复稿（含 `finalfix-v1-logs/`） |
