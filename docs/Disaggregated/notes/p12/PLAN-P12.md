# P12「server 自有屏幕窗口」子集 — 计划与交接（2026-09-24）

> 接 P7 收官（[`../p7/HANDOFF-2026-09-23.md`](../p7/HANDOFF-2026-09-23.md)）。基线 `feat/disaggregated@9f669e52`。
> 包树 `~/w7/p12-onscreen`，分支 `p12/onscreen`（从 `9f669e52` 分出，15 个 `(P12)` 提交）。
> 任务书 [`BRIEF-onscreen.md`](BRIEF-onscreen.md)，只读审计 `map-*.md`（四份），本页是**交接与余项**。

## 1. 这一包做了什么

按用户裁定（[`BRIEF-onscreen.md`](BRIEF-onscreen.md) §User rulings，四条）：Android 上的 server **自己创建
`ANativeWindow`（自己的 SurfaceView）**，把 IPC 渲染流**渲染到屏幕上**；离屏（pbuffer）与上屏两条路径**都保留**，
**同一时刻只有一条活跃**，在 server 初始化 / context 创建时决定；拓扑为**进程内 server**——display Activity
自己的进程里用一个线程跑 TCP server 并直接渲染进它的 SurfaceView，会话在该进程内**串行**；client 侧用一个
**新的 `WindowKind::ServerOwned`** 接入，且此模式下 client **完全无头**（`eglCreateWindowSurface` 接受 `NULL`），
窗口几何由 server 回传。

落地的形状（15 提交，53 文件，+4566/−110）：

| 面 | 内容 |
|---|---|
| wire | `WindowKind::ServerOwned = 7`（fbs 追加）、控制修订 **2 → 3**、`SurfaceRefusal` / `SurfaceReply`（width/height/refusal）、codec 的 ServerOwned 规则（仅 `CreateWindowSurface`、`nativeToken == 0`、`SetWindowHandle` 具名拒绝）、非零 token 的 latch 行 |
| server 窗口 | `MG_Remote/Server/ServerDisplay.{h,cpp}`（不透明窗口 + hooks + lease + 有界 `Detach`）、ServerOwned 臂的 one-mode-per-session latch、无显示时的具名拒绝、失窗在 apply 线程释放、`ServerWindowLost` 家族行 |
| client | `MOBILEGL_IPC_SURFACE=server`（`Config.h` / `ConfigLoader.cpp`）→ 无头窗口 surface、**一次** ServerOwned `CreateWindowSurface`、**不发** `SetWindowHandle`、geteometry 进 EGL state 后才返回 |
| 进程内 server | `RunSession` 改为返回退出码、`TcpSupervisor` 把已认证会话交给线程（Busy / 会话间 `ResetSessionLatch` / 后端按进程钉住 / 非阻塞 stop）、fork 出的会话子进程 `PR_SET_PDEATHSIG` 随监督进程去 |
| Android | `MobileGLDisplayActivity`（`:mglwin`，全屏 SurfaceView，aspect-fit，env 在库加载前应用）、display server 的 JNI（`DisplayServerJni.cpp` + 几何 up-call）、与 `MobileGLServerService` 共享环境、**同一时刻只有一个 server**（Activity 停 service，service 杀 `:mglwin`） |
| 工具 | `tcp_device_server.py --surface window\|pbuffer`、Linux glws 的 `MOBILEGL_TRACE_SURFACE=window` |
| Espryt | 远端会话的 window-surface extent 随其默认 framebuffer 格式发布（仅 split 构建，pull 构建不动；G1） |

## 2. 门与证据

**G1（硬约束）**：pull 构建 `.text` `0xa52203` 不变、符号 0 增 0 减。门日志 `gate/gate-android1.log` 报
`a52203`，但那一行印的是 `added=30570 removed=30570`——**这是错的读法**：比较基准
`~/w7/logs/p12/pull-syms-base.txt` 是用 `nm --defined-only … | awk '{print $3}'` 生成的（每个目标文件
的符号头行 + 符号），而比较时用的是无 awk 的全量 `nm` 输出，两边不同源，于是"全部新增又全部删除"。
交接时已独立复核：在 `~/w7/pipe`（pull 构建，恰在 `9f669e52`）与 `p12-onscreen` 两个 `build-linux` 上
直接 `nm --defined-only | awk '{print $3}' | sort` 后 `comm`，**added 0 / removed 0，符号集各 30570 全同**；
`readelf -S` 两边 `.text` 都是 `a52203`。**G1 成立**；门脚本的那一行输出应在 P12 收尾时修好（见 §4）。

**主机门**（`gate/gate-android1.log`，head `29b7b284`，dirty=0，2026-09-24 00:55–01:04）：

- unit 2601、integration-split 322、spawn 239、tcp 243、magma-split 125、magma-spawn 104、magma-tcp 89、
  integration-gpu 2188 × 2（monolith / inproc）**全绿**。
- `integration-magma-full-split` 558 报 1 红（`IterationRPProgram203Scenario` 的 exposure texel）。**这不是本包引入的**：
  交接时独立复核，用 ctest 注册的那组环境变量直跑该用例，`p12-onscreen` 3/3 绿、`mismatches=0/262656`；
  整条车道 `-j 8` 重跑两遍也 558/558 绿。它是既有/环境敏感项，树上早有记录：`../p05/p05-results/value-types-v1.md`
  判为**构建目录 configure 差异**（`MG_IntegrationTest/CMakeLists.txt` 只在 `MOBILEGL_ITEST_VK_ICD` 匹配
  `lvp_icd|lavapipe` 时才挂三个 `MOBILEGL_MAGMA_*` 修复开关），`../p7/magma-a.md` §6.2 记它在该 worktree
  3/3 稳定红且还原后端改动照样红，`../p7/INTEGRATOR-DECISIONS-P7.md` ID-P7-22 **改判为"环境敏感而非 tcp 专属"**。
  `p12-onscreen` 两个 build 目录的 `MOBILEGL_ITEST_VK_ICD` 都已钉到 lavapipe，`ctest -N -V` 确认注册时带着三个修复变量。
- 卫生 12 项全 OK；fatal census 79 / link ratchet 171 不变；parity 0；protocol revision pin 报 **revision 3 holds**。

**真机**（Redmi `2f7cbe2e`，Adreno 830，证据 `device/`，`device/evidence-strip.png` 为九格拼图）：
七个编号检查在两个 head（`984db455`、`a4a940db`、`29b7b284`）上跑过，`trial-*/` 是按 head 归档的旧轮次。

| # | 检查 | 结果 |
|---|---|---|
| 1 | Espryt 上屏（OpenRA，`--window-surface`，`MOBILEGL_IPC_SURFACE=server`） | ssim **1.000000**；server 日志 `surface=window 640x480 owner=server`；屏幕截图非黑 |
| 2 | Magma 上屏（OpenRA）+ 同进程第二个会话（`minecraft-1.21.4-startup`） | ssim **1.000000** / **0.999999511**；两臂都 `owner=server` |
| 3 | 串行第二会话 + `Refuse{Busy}` | 第二会话绿；并发客户端拿到 `Refuse{Busy}`（rc=134）后长会话照常收尾绿 |
| 4 | 失窗（`surfaceDestroyed`） | `Fatal{ServerWindowLost}` → 会话 latch → client 读**干净 device lost**；Activity 回来后新会话 ssim 1.0，`:mglwin` 存活 |
| 5 | 离屏 service（不带旋钮） | ssim 1.0，`surface=pbuffer 640x480`（离屏路径未被破坏） |
| 6 | 负控：`MOBILEGL_IPC_SURFACE=server` 打离屏 server | 双侧具名 `Refuse ServerOwned (NoServerDisplay)`；随后同 service 的离屏会话绿 |
| 7 | 一台设备上只有一个 server | Activity 起来杀 service、service 起来杀 `:mglwin`，互相都验证了 |

### 审查后复测（`feat/disaggregated@1fb18d9e`，2026-09-24）

审查修复后的同一 P12 实现 head 已在 Redmi `2f7cbe2e`（Adreno 830）重建并复跑七项检查。结果与上表一致：Espryt OpenRA SSIM **1.000000**；Magma OpenRA / Minecraft startup **1.000000 / 0.999999511**；串行第二会话和 `Refuse{Busy}` 通过；HOME 失窗得到 `ServerWindowLost` 干净 device-lost，surface 释放 **15 ms**，Activity 存活且重附后新会话 SSIM 1.0；pbuffer 与 `NoServerDisplay` 负控通过；Activity / service 互斥通过。第 6 项初次尝试与上一会话回收竞态命中 Busy；临时测试副本等待 2 秒后重跑，双侧具名拒绝和后续 pbuffer 会话均通过，仓库脚本未改。

审查后主机 **P12 定向 CTest 46/46 通过**：`ServerDisplayTest`、`ServerLoopTest`、`InProcessServer`、`SupervisorChildren`、`ServerOwnedSurface`。这组定向测试不等于完整主机门。证据在测试机 `/home/swung/w7/logs/p12-verify-1fb18d9e/`；日志、APK 和截图按证据数据规则未入仓库。当前 `0fe01588` 相对验证 head 只有文档整理及文档检查配置更新。

## 3. 还没做（P12 未收官）

按 [`../ROADMAP.md`](../../ROADMAP.md) P12 行的两个出口门，以及本包任务书列出的改小后的条目：

1. **出口门 (a) 未达成。** 要求 FCL 同机 spawn、双后端**入世界**、杀 server 产生干净 device-lost latch。
   今天验证到的是：trace_replay（**不是 FCL**）经 `--window-surface` + `MOBILEGL_IPC_SURFACE=server` 在屏上重放 OpenRA，
   双后端都 1.0；device-lost 是**按 HOME（surfaceDestroyed）**触发的，**不是杀 server**。
2. **出口门 (b) 完全未做。** 要求另一台机器上的 client 经 TCP 在 Redmi 入世界并记录 P6.5 的必测数。
   本轮的 client 是 `adb forward` 后的 `tcp://127.0.0.1:40613` **环路**（`android/dev.sh:7-8` 写得很清楚），
   **不是跨机**。
3. **ROADMAP P12 行里仍未落地的条目**（交接时在树上核过）：
   - `unix:` 监听（in-process server 只做了 tcp；`grep 'unix:' ServerMain.cpp` 为 0）；
   - DirectGLES `g_Display` / `g_Surface` / `g_Context` 去全局（`DirectGLES.cpp` 仍有 40 处）；
   - cached-app freezer 的"完整处理"（树上 0 处提及）；文档从未定义它是什么，`SPAWN-PLAN.md` §8.2 只说了它是 P12 的；
   - 多 context（契约 §11）；
   - FCL env 与 plugin APK 开关表接线；
   - **`AndroidNativeWindow@P12` / `MetalLayer@P12` 的 D8 白名单**（拒 X11 / Win32Hwnd / None）——`map-docs-contract.md` §5.3 记它
     **没落地**，且当时 `SetWindowHandle` 在 client 侧**无条件发**；本轮把 ServerOwned 模式下的
     `SetWindowHandle` 变成了具名拒绝，但 D8 本身（X11/Win32 白名单）仍是独立跟进项。
4. **文档债**（`map-docs-contract.md` §4 列的"落地时要记录的东西"）：
   - **没有 `MG_Remote/CONTRACT-P12.md`**（前几个阶段都有；本页与 `BRIEF-onscreen.md` 不是契约）；
   - 没有 `INTEGRATOR-DECISIONS-P12.md` 式的逐条裁定；
   - `ARCHITECTURE.md:526` 仍写着被删掉的 Surface → AIDL → `ANativeWindow_fromSurface` 旧路径，`protocol.fbs:258`
     仍写「Android transfers the window out of band」，`ARCHITECTURE.md:533` 仍描述 RESPAWN / IDLE_EXIT 可用；
   - `CURRENT_STAGE_PROGRESS.md:244` 仍把 P12 描述成"real windows cross processes"，与 Rule H 冲突；
   - IPC 顺序（`ROADMAP.md:19` 的 P6.5 → Ph → **P12** → P9 与 `CURRENT_STAGE_STATUS.md` / HANDOFF 的
     P9 → P10 → P11 → P12）两处说法仍不一致；
   - a6 行 A8-3…A8-15 的处置、CONTRACT-P6 §7.2 D8 的处置都还没写；
   - 阶段收尾该有一次 Codex 审查（`ROADMAP.md:17` 的惯例）。
5. **`MG_Test/SanityTest.cpp` 在本包里被改动过**——它属于 pull 构建之外的测试目标，但要确认它与 G1 的关系已在门里测到（门已绿，此处只是点名）。

## 4. 立刻可做的收尾

1. 把 `~/w7/notes/p12/gate.sh` 的 G1 符号比较改成与基准同源（`nm --defined-only … | awk '{print $3}'`），
   否则下一个跑门的人还会看到 `added=N removed=N` 而误判。
2. 决定 `IterationRPProgram203Scenario` 在 P12 的处置：既然 ID-P7-22 已判"环境敏感"，把它在门里的
   出现方式（钉 ICD 即可绿 / 记为已知偶发）写成一句结论，别再一次次口头带过。
3. 若要真正收官 P12：先补跨机 TCP（出口门 b），再补 FCL + 杀 server 的 device-lost（出口门 a）,
   最后写 `CONTRACT-P12.md` 并按惯例做一次收官审查。

## 5. 工装在哪

- 任务书与审计：本目录 `BRIEF-onscreen.md`、`map-apk-process.md`、`map-client-surface.md`、
  `map-docs-contract.md`、`map-server-surface.md`。
- 门与包树：`gate.sh`、`setup-tree.sh`、`fix-icd.sh`、`g1-base-names.sh`、`p203.sh`；主机门日志 `gate/`。
- 真机：`android/`（`dev.sh`、`check1/3/4/5/7.sh`、`final-run.sh`、`precheck.sh`、`redonce-tools.sh`、
  `redonce-twins.sh`、`hostrepro.sh`、`verify-apk.sh` 等），证据 `device/`。
- **没进仓库的**（按 [`../notes/README.md`](../README.md) "证据数据"的规则）：`~/w7/logs/p12/device/**/logcat*.txt`
  等约 66 MB 设备日志、APK、`pull-syms-base.txt*`（2.5 MB nm 转储）。要复核请去产生它们的机器。
