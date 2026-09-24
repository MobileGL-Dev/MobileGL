# P12 — Android 生产窗口路径（「server 自有屏幕窗口」子集已实现，未收官）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 当前阶段。状态摘要在 [`../../CURRENT_STAGE_PROGRESS.md`](../../CURRENT_STAGE_PROGRESS.md)；计划与交接 [`PLAN-P12.md`](PLAN-P12.md)；任务书 [`BRIEF-onscreen.md`](BRIEF-onscreen.md)；裁定 [`INTEGRATOR-DECISIONS-P12.md`](INTEGRATOR-DECISIONS-P12.md)（ID-P12-1..15）；只读审计 `map-*.md` 四份。
- 形状：server APK 自建 `ANativeWindow`（`MobileGLDisplayActivity` 的 SurfaceView，进程内 TCP server，会话串行）把渲染流**上屏**；离屏路径保留、同一时刻一条活跃；client 以 `WindowKind::ServerOwned`（控制修订 3）**完全无头**接入，几何由 server 回传；失窗 → `Fatal{ServerWindowLost}` → 干净 device-lost。
- 进展：15 个 `(P12)` 提交 + 审查轮修复（10 个问题，ID-P12-5..11）已并入 `feat/disaggregated`；主机门绿（ID-P12-14）、G1 成立（ID-P12-15）；真机七项检查在审查轮**之前**的 head 上通过，审查轮之后**未复测**（ID-P12-13）。
- 未完成：出口门 (a) FCL 同机 spawn + 杀 server 的 device-lost、(b) 跨机 TCP 入世界 + P6.5 必测数；`CONTRACT-P12.md`；阶段表行里的其余条目（`unix:` 监听、DirectGLES 去全局、freezer、多 context、FCL 开关接线、D8 白名单）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P12** Android 生产窗口路径
- **状态**：**子集已实现、未收官（2026-09-24，自 `9f669e52`，15 提交 + 审查轮 8 提交）**：上屏 server 窗口、`WindowKind::ServerOwned`、无头 client、失窗 device-lost 已落地并过真机七项检查；审查轮 10 个问题已修（两个大项：批内失窗、会话间 Espryt 单元影子悬空；裁定 [`notes/p12/INTEGRATOR-DECISIONS-P12.md`](INTEGRATOR-DECISIONS-P12.md) ID-P12-5–11），主机门复跑，**真机未复测**（ID-P12-13）；两个出口门均未打（见 [`notes/p12/PLAN-P12.md`](PLAN-P12.md) §3）
- **落地什么 / 范围**：**按重审 §6 改小**：~~Service 收 Java `Surface`~~ 删掉 Surface 传递那一半——server app 拥有自己的 SurfaceView，client 根本没有窗口，于是 minSdk 26 没有扁平化 `ANativeWindow` 的 NDK API 这条约束不再成立；`WindowKind::ServerOwned`；两个后端各一条 `CreateEGLWindowSurface` 臂；`m_windowHandle` 四处写点 + `sameHandle` 去重键（a6 §6）；DirectGLES `g_Display` / `g_Surface` / `g_Context` 去全局；`kEventSurfaceChanged` 的 Width / Height 缺口（Espryt 只发布格式，server 拥有显示时 client 默认 FBO 会停在 512×512 占位值）；client 侧不再无条件发 `SetWindowHandle`（`BackendObject_Remote.cpp:215`——今天 FCL 里开 spawn，第一次 `eglCreateWindowSurface` 就在 server 进程 abort）；server app 监听 `unix:` 与 `tcp://`（后者 Ph 之后）；server 生命周期绑 Activity；cached-app freezer（SPAWN-PLAN §8.2）；多 context（契约 §11）；FCL env 与 plugin APK 开关表接线
- **验收门 / 证据**：两个门：(a) Minecraft 经 FCL **同机 spawn** 在 Adreno 830 双后端入世界，杀 server 产生干净 device-lost latch；(b) **另一台机器上的 client 经 TCP** 在 Redmi 上进世界（数据面 stream），并记录 P6.5 的必测数

## 状态记录（原 `CURRENT_STAGE_STATUS.md`，2026-09-24）

**更新：2026-09-24（P12 子集）** · 计划与交接 [notes/p12/PLAN-P12.md](PLAN-P12.md)。**P12「server 自有屏幕窗口」子集已实现、未收官，已并入 `feat/disaggregated`**（15 提交自 `9f669e52` 起，加审查轮 10 个问题的修复——两个大项：批内失窗、会话间 Espryt 单元影子悬空；裁定 [notes/p12/INTEGRATOR-DECISIONS-P12.md](INTEGRATOR-DECISIONS-P12.md) ID-P12-5–15，主机门与 G1 见 ID-P12-14/15，**审查轮之后未上真机复测**，ID-P12-13）：Android 上的 server 自建 `ANativeWindow`（自己的 SurfaceView）把渲染流**上屏**，离屏路径保留，同一时刻只一条活跃；client 以新的 `WindowKind::ServerOwned` **完全无头**接入（控制修订 3）。主机门除一条既有环境敏感项外全绿，G1（pull 构建 `.text` `0xa52203`、符号 0/0）已独立复核；真机 Redmi `2f7cbe2e` 上七项编号检查过了双后端上屏、串行/Busy、失窗 device-lost、离屏不回归、无显示具名拒绝、一台设备一个 server。**两个出口门都还没打**：跨机 TCP（门 b）完全未做，FCL + 杀 server 的 device-lost（门 a）未做——本轮的真机是 trace_replay 经 `adb forward` 环路驱动的。上一阶段 **P7 已收官（2026-09-23，ID-P7-63）**：§8 九项全绿——终局窗口 3（`p7w8-78e71f2b`）门 3 PASS 36/36、门 5 PASS；门 4 以 `# P13` 读法关闭。

- **P12 子集**：APK `26.09.29b7b28-trace`（**控制协议修订 3**，与 p7w8 的 server 不兼容），证据 `notes/p12/device/`（九格拼图 `evidence-strip.png`）。七个编号检查在三个 head 上跑过，`trial-*/` 是按 head 归档的旧轮次。

## 当前状态详情（2026-09-24，原 `CURRENT_STAGE_PROGRESS.md` 正文）

> 顶层 [`CURRENT_STAGE_PROGRESS.md`](../../CURRENT_STAGE_PROGRESS.md) 只留摘要；带裁定编号的落地表、门表、余项清单与证据位置在这里。

### 1. 一句话

Android 上的 server 自建窗口（自己的 SurfaceView）把 IPC 渲染流**上屏**，client 以 `WindowKind::ServerOwned` **完全无头**接入。子集已实现并入，**未收官**：审查轮修复之后欠一次真机复测，两个出口门都还没打。

### 2. 已落地

| 面 | 内容 | 出处 |
|---|---|---|
| wire | `WindowKind::ServerOwned`、控制修订 2 → 3、`SurfaceReply` 带回窗口真实尺寸或具名拒绝 | PLAN-P12 §1 |
| server 窗口 | `ServerDisplay`（钩子 + 租约 + 有界 3 s `Detach`）；会话表面模式首次创建即闩定（`SurfaceModeMismatch`）；无显示具名拒绝（`NoServerDisplay`）；失窗 → `Fatal{ServerWindowLost}` → client 读干净 device-lost | ID-P12-2、ID-P12-3 |
| client | `MOBILEGL_IPC_SURFACE=server` → 无头窗口 surface，不发 `SetWindowHandle`；resize 变成对 server 窗口的几何请求，采用 server 回的真实尺寸 | ID-P12-10 |
| 进程内 server | `MobileGLDisplayActivity`（`:mglwin`）里一个线程跑 TCP server：会话串行、会话间重置闩、后端按进程钉住；与离屏 `MobileGLServerService` 互斥，一台设备同一时刻一个 server | ID-P12-4 |
| 审查轮 | 10 个问题已修；两个大项：批内失窗检查（`DrainRing` 每次 pop 前）、会话间 Espryt 单元影子悬空；另有推流中停机、会话结束清理、陈旧尺寸、日志转发 | ID-P12-5 … 11 |

### 3. 门

| 门 | 状态 | 证据 |
|---|---|---|
| G1 | ✅ pull 构建同机前后 `.text` 一致、符号 0 增 0 减 | ID-P12-15 |
| 主机门 | ✅ unit 2605/2605，spawn / tcp 与三条 Magma 双进程车道全绿；剩余红全部归容器的 CMake/CTest 版本 | ID-P12-14 |
| 真机七项检查（Redmi `2f7cbe2e`） | ✅ 审查轮之前（`29b7b284`）：双后端上屏 ssim 1.0、串行 + `Refuse{Busy}`、失窗 device-lost、离屏不回归、无显示具名拒绝、一台设备一个 server；⏳ **审查轮之后未复测** | PLAN-P12 §2、ID-P12-13 |
| 出口门 (a) | ❌ FCL 同机 spawn、双后端入世界、**杀 server** 产生干净 device-lost（本轮的驱动是 trace_replay，device-lost 由按 HOME 触发） | PLAN-P12 §3 |
| 出口门 (b) | ❌ **另一台机器**的 client 经 TCP 在 Redmi 入世界，并记录 P6.5 必测数（本轮 client 是 `adb forward` 环路） | PLAN-P12 §3 |

### 4. 在跑

无。

### 5. 下一步

1. **真机复测**（ID-P12-13）：重建 APK（控制修订 3），重跑七项检查，外加"推流中按 HOME：回调返回前 surface 已释放，并记耗时"；在与原门同版本的 CMake/CTest 上复跑一次主机门。
2. **出口门**：先 (b) 跨机 TCP 入世界 + P6.5 必测数，再 (a) FCL + 杀 server。
3. **收尾**：写 `MG_Remote/CONTRACT-P12.md`；修 `notes/p12/gate.sh` 的 G1 符号比较（与基准不同源，会印出误导的 `added=N removed=N`）；按惯例做一次收官审查。
4. **阶段表行里的余项**（交接时在树上核过，均未做）：in-process server 的 `unix:` 监听、DirectGLES `g_Display` / `g_Surface` / `g_Context` 去全局、cached-app freezer、多 context、FCL env 与 plugin 开关表接线、D8 窗口种类白名单。
5. 其后按 [`ROADMAP.md`](../../ROADMAP.md)：monolith 跑道 P8；IPC 跑道 P9 → P10 → P11；P6.5 残余（39 例 device 矩阵 + 必测数）与 P3b/P4b 余项并行；`CONTRACT-P7.md` §12 的记录债按阶段认领。

### 6. 阻塞 / 需要人

- 没有需要用户决策的阻塞。真机复测需要一台装有 NDK、连着 Redmi 的机器（审查轮所在的云容器没有）。
- 已知环境敏感项：`IterationRPProgram203Scenario`（`integration-magma-full-split`）偶发红，按 ID-P7-22 / ID-P12-12 读作"既有、环境敏感"（钉 lavapipe ICD 即绿），不阻塞 P12。

### 7. 真机与证据

- 当前 APK `26.09.29b7b28-trace`（控制修订 3，与 P7 终局窗口的 p7w8 server 不兼容）；设备 Redmi `2f7cbe2e`（Adreno 830），定频协议 [`devices/pin-verification-2026-09-07.md`](../../guide/pin-verification-2026-09-07.md)。
- 真机证据 `notes/p12/device/`（`evidence-strip.png` 九格拼图，`trial-*/` 是按 head 归档的旧轮次）；主机门日志 `notes/p12/gate/`；设备脚本 `notes/p12/android/`。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-onscreen.md`](BRIEF-onscreen.md) | P12 subset "on-screen server window" — implementation brief (2026-09-23) |
| [`fix-icd.sh`](fix-icd.sh) | 钉 lavapipe ICD |
| [`g1-base-names.sh`](g1-base-names.sh) | G1 基准符号名生成 |
| [`gate-container.sh`](gate-container.sh) | 主机门的容器变体 |
| [`gate.sh`](gate.sh) | 主机门脚本 |
| [`INTEGRATOR-DECISIONS-P12.md`](INTEGRATOR-DECISIONS-P12.md) | P12 集成者裁定日志（「server 自有屏幕窗口」子集） |
| [`map-apk-process.md`](map-apk-process.md) | MobileGL-disagg: Android APK and process model for an on-screen server path (commit 9f669e52) |
| [`map-client-surface.md`](map-client-surface.md) | Client-side surfaces in split mode: map, open questions and change list (commit 9f669e52) |
| [`map-docs-contract.md`](map-docs-contract.md) | Design docs and contracts for a server-owned on-screen window (P12 subset) |
| [`map-server-surface.md`](map-server-surface.md) | Server-side surface path and both backends' window surfaces (commit 9f669e52) |
| [`p203.sh`](p203.sh) | `IterationRPProgram203Scenario` 复核 |
| [`PLAN-P12.md`](PLAN-P12.md) | P12「server 自有屏幕窗口」子集 — 计划与交接（2026-09-24） |
| [`setup-tree.sh`](setup-tree.sh) | 包树搭建 |
| [`android/`](android/) | 27 个文件：真机与构建脚本（`dev.sh`、`check*.sh`、`final-run.sh`、red-once 工具等） |
| [`device/`](device/) | 85 个文件：真机七项检查证据（`evidence-strip.png` 九格拼图；`trial-*/` 为按 head 归档的旧轮次） |
| [`gate/`](gate/) | 6 个文件：三个 head 的构建日志与主机门日志（含审查轮、CMake 4 对照） |
