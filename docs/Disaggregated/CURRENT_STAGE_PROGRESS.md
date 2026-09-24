# 当前阶段进展 — P12「server 自有屏幕窗口」子集

> 只记**当前阶段**的摘要：在哪、落地了什么、门到哪一步、下一步、阻塞。细节与证据都在 [`notes/p12/`](notes/p12/README.md)（计划与交接 [`PLAN-P12.md`](notes/p12/PLAN-P12.md)，裁定 [`INTEGRATOR-DECISIONS-P12.md`](notes/p12/INTEGRATOR-DECISIONS-P12.md)）。已收官阶段见 [`ROADMAP.md`](ROADMAP.md) 阶段表。随每次合并 / 真机窗口更新。

**更新：2026-09-24** · 分支 `feat/disaggregated`（P12 子集与审查轮已并入）· 上一阶段 P7 已于 2026-09-23 收官（ID-P7-63）

## 1. 一句话

Android 上的 server 自建窗口（自己的 SurfaceView）把 IPC 渲染流**上屏**，client 以 `WindowKind::ServerOwned` **完全无头**接入。子集已实现并入，**未收官**：审查轮修复之后欠一次真机复测，两个出口门都还没打。

## 2. 已落地

| 面 | 内容 | 出处 |
|---|---|---|
| wire | `WindowKind::ServerOwned`、控制修订 2 → 3、`SurfaceReply` 带回窗口真实尺寸或具名拒绝 | PLAN-P12 §1 |
| server 窗口 | `ServerDisplay`（钩子 + 租约 + 有界 3 s `Detach`）；会话表面模式首次创建即闩定（`SurfaceModeMismatch`）；无显示具名拒绝（`NoServerDisplay`）；失窗 → `Fatal{ServerWindowLost}` → client 读干净 device-lost | ID-P12-2、ID-P12-3 |
| client | `MOBILEGL_IPC_SURFACE=server` → 无头窗口 surface，不发 `SetWindowHandle`；resize 变成对 server 窗口的几何请求，采用 server 回的真实尺寸 | ID-P12-10 |
| 进程内 server | `MobileGLDisplayActivity`（`:mglwin`）里一个线程跑 TCP server：会话串行、会话间重置闩、后端按进程钉住；与离屏 `MobileGLServerService` 互斥，一台设备同一时刻一个 server | ID-P12-4 |
| 审查轮 | 10 个问题已修；两个大项：批内失窗检查（`DrainRing` 每次 pop 前）、会话间 Espryt 单元影子悬空；另有推流中停机、会话结束清理、陈旧尺寸、日志转发 | ID-P12-5 … 11 |

## 3. 门

| 门 | 状态 | 证据 |
|---|---|---|
| G1 | ✅ pull 构建同机前后 `.text` 一致、符号 0 增 0 减 | ID-P12-15 |
| 主机门 | ✅ unit 2605/2605，spawn / tcp 与三条 Magma 双进程车道全绿；剩余红全部归容器的 CMake/CTest 版本 | ID-P12-14 |
| 真机七项检查（Redmi `2f7cbe2e`） | ✅ 审查轮之前（`29b7b284`）：双后端上屏 ssim 1.0、串行 + `Refuse{Busy}`、失窗 device-lost、离屏不回归、无显示具名拒绝、一台设备一个 server；⏳ **审查轮之后未复测** | PLAN-P12 §2、ID-P12-13 |
| 出口门 (a) | ❌ FCL 同机 spawn、双后端入世界、**杀 server** 产生干净 device-lost（本轮的驱动是 trace_replay，device-lost 由按 HOME 触发） | PLAN-P12 §3 |
| 出口门 (b) | ❌ **另一台机器**的 client 经 TCP 在 Redmi 入世界，并记录 P6.5 必测数（本轮 client 是 `adb forward` 环路） | PLAN-P12 §3 |

## 4. 在跑

无。

## 5. 下一步

1. **真机复测**（ID-P12-13）：重建 APK（控制修订 3），重跑七项检查，外加"推流中按 HOME：回调返回前 surface 已释放，并记耗时"；在与原门同版本的 CMake/CTest 上复跑一次主机门。
2. **出口门**：先 (b) 跨机 TCP 入世界 + P6.5 必测数，再 (a) FCL + 杀 server。
3. **收尾**：写 `MG_Remote/CONTRACT-P12.md`；修 `notes/p12/gate.sh` 的 G1 符号比较（与基准不同源，会印出误导的 `added=N removed=N`）；按惯例做一次收官审查。
4. **阶段表行里的余项**（交接时在树上核过，均未做）：in-process server 的 `unix:` 监听、DirectGLES `g_Display` / `g_Surface` / `g_Context` 去全局、cached-app freezer、多 context、FCL env 与 plugin 开关表接线、D8 窗口种类白名单。
5. 其后按 [`ROADMAP.md`](ROADMAP.md)：monolith 跑道 P8；IPC 跑道 P9 → P10 → P11；P6.5 残余（39 例 device 矩阵 + 必测数）与 P3b/P4b 余项并行；`CONTRACT-P7.md` §12 的记录债按阶段认领。

## 6. 阻塞 / 需要人

- 没有需要用户决策的阻塞。真机复测需要一台装有 NDK、连着 Redmi 的机器（审查轮所在的云容器没有）。
- 已知环境敏感项：`IterationRPProgram203Scenario`（`integration-magma-full-split`）偶发红，按 ID-P7-22 / ID-P12-12 读作"既有、环境敏感"（钉 lavapipe ICD 即绿），不阻塞 P12。

## 7. 真机与证据

- 当前 APK `26.09.29b7b28-trace`（控制修订 3，与 P7 终局窗口的 p7w8 server 不兼容）；设备 Redmi `2f7cbe2e`（Adreno 830），定频协议 [`devices/pin-verification-2026-09-07.md`](devices/pin-verification-2026-09-07.md)。
- 真机证据 `notes/p12/device/`（`evidence-strip.png` 九格拼图，`trial-*/` 是按 head 归档的旧轮次）；主机门日志 `notes/p12/gate/`；设备脚本 `notes/p12/android/`。
