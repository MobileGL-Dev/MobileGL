# P5f 会话交接（HANDOFF）

> 写于 2026-09-19，会话因网络故障（auth.kimi.com OAuth 连接超时）中断时整理。
> 用途：新会话读这一份即可自举，继续推进 P5f。
> 分支 `feat/disaggregated`，主 worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`。

## 0. 三十秒版

P5f（一切状态上 wire，计划 `docs/Disaggregated/P5F-WIRE-COMPLETENESS.md`）已开工：
**f0 普查与 f1 双块机制已收官并合入 `feat/disaggregated`（头 `418c765c`）；波次 2 的 fc 包已完成但未合并（分支 `p5f/fc` @ `d5ed139c`）；fe / fm 两包的代理因 OAuth 网络超时从未启动，其 worktree 仍停在基线。波次 3（fs / fr / fv）尚未创建。**

下一步：恢复 / 重开 fe 与 fm 两个实现代理（§4 有完整任务说明），完成后集成 fc+fe+fm，再开波次 3。

## 1. 已完成（在 `feat/disaggregated` 上）

| 项 | 头 | 内容 |
|---|---|---|
| f0 普查 | `e75e00cb` | 8 份只读普查报告，全在 `docs/Disaggregated/notes/p5f/`：`f0-statics.md`、`f0-reverse-channel.md`、`f0-magma.md`、`f0-egl.md`、`f0-registry.md`、`f0-env.md`（构建/门禁 cheatsheet）、`f0-fields.md`（15 字段逐字段载体判定）、`f0-dualblock-recon.md`（双块设计侦察） |
| f1 双块机制 | `cfcc12f7` + 报告 `418c765c` | `PipeInputs` 按角色分区 + 旋钮 `MOBILEGL_IPC_ROLE_SPLIT_STATE`（默认关）+ 双块车道 + 红清单棘轮 `MobileGL/MG_IntegrationTest/Harness/dualblock-expected-fatals.txt` + 阴性对照（`dualblock_negative_control.sh`，旋钮 1 绿 / 0 红两态实测）+ CONTRACT-P5E §3.2 标 LANDED。**门**：unit 2262/2262、isplit 179/179、gpu 1359/1359、flavours 绿、**G1 零认定**；双块车道（旋钮开）181 条目 155 红全部具名 Fatal、6 对首阻塞（最大块 `GetFramebufferBindingSlot@ReadPixels` 132 条目） |

f1 报告：`docs/Disaggregated/notes/p5f/f1-report.md`（含与 f0-dualblock-recon 的四点设计偏差，均有理由）。

## 2. 已完成但未合并

**fc 包（EGL 控制面帧化）**：分支 `p5f/fc`，头 `d5ed139c`，worktree `../MobileGL-p5f-fc`。

- 单槽邮箱的"函数指针+栈上 void*"退役为按值 `SurfaceControlFrame`（`MG_Remote/Server/SurfaceControlFrame.h`，static_assert 钉死无指针）；12 个 forwarder 本体进 `ApplySurfaceControlFrame` 分发；protocol.fbs append-only 补 3 枚举 + 2 字段 + `WindowKind::MetalLayer=6`（generated 头用 pinned flatc 重生成，零 diff）；死 forwarder 不帧化（标 inproc-only）；`Fatal{UnmigratedSurface,"AndroidNativeWindow@P12"}` 具名拒绝真窗口；ROADMAP G4 行号漂移已修。
- 门全绿：unit 2273/2273、isplit 179/179、strict 179/179（Fatal=0，Admitted 恰 8 对）、flavours pull/push 绿、**G1 0/0/0/0 .text +0**、双块 census 与 f1 逐对相同（fc 名下无红）。R-16 red-once 已真跑（`AVoidForwarderCrossesAsOneDispatchedFrame`）。
- 报告：`docs/Disaggregated/notes/p5f/fc-report.md`（在该分支上）。
- **预期合并冲突点**：`MG_Test/Wire/CMakeLists.txt`、`ServerLoop.*`、`protocol.fbs` + generated、ROADMAP/P5F/P6-DRAFT 文档行。

## 3. 中断点

波次 2 的 swarm 发出后：

1. 第一次（3 代理并行）整批在启动时遇 `auth.kimi.com` OAuth 连接超时，全失败。
2. 第一次恢复：fc 成功完成（§2）；fe / fm 再次 OAuth 超时失败。
3. 第二次恢复 fe / fm：工具调用被用户打断，**结果未记录**——经核实两个 worktree 均无新提交、工作区干净，**fe / fm 确实从未开工**（不是半成品，无需清理）。

当前无任何后台任务在跑。

## 4. 待办（按序）

### 4.1 重开 fe / fm（原任务说明直接可用）

两包各自独立 worktree + 分支已建好，直接把 §5 的任务说明喂给新代理（或 resume `agent-12` / `agent-13`，但它们从未获得上下文，重开等价）。关键约束：

- **fe**（worktree `../MobileGL-p5f-fe`，分支 `p5f/fe`，WSL slug `p5f-fe`）：15 个 BARRIER_PULLED 字段的 **Espryt 侧**读者退役。设计输入 `notes/p5f/f0-fields.md`：11 个字段"数据已在线上、读者没跟上"（改读者不动线上结构）；真正要扩记录的只有 `GetTransformFeedbackProgram`、`GetBufferBindingSlot` 的 7 个无载体 target、`HasOpenTransformFeedbackSpan`、`GetBufferBindingPointCount`。每退役一对 `<field>@<verb>` 同步删 `dualblock-expected-fatals.txt` 对应行（双向棘轮）。RecordError 归 fv、Magma 侧归 fm，别越界。
- **fm**（worktree `../MobileGL-p5f-fm`，分支 `p5f/fm`，WSL slug `p5f-fm`）：9 个 "P7 (Magma)" 字段的 Magma 读点 + T5 残余 + f0-magma 的三类结构性耦合。**边界**：Magma draw/dispatch 在 transport 下汇入 `Fatal{buffer-legacy-arm}`（BufferObject.cpp:415-440）是 P7 功能债，不归 fm；不可达站点具名记录即可。fm 报告必须回答出口门 4（Magma 是否发布 `kCapRunAheadApply`、不发布的理由）。`ResourceTracker.h:732-739` 的直捅归 fv。
- 两包共用 `FieldOwnership.def`（fe 动 Espryt 行、fm 动 Magma 行），集成时解冲突。

### 4.2 集成波次 2

fe / fm 完成后，按 fc → fe → fm 顺序合入 `feat/disaggregated`，每合一包重新生成并核对棘轮（`dualblock-expected-fatals.txt` 只减不增，减少的每一行必须能被对应包点名）。集成者跑全门（含 `integration-gpu`，ID-109 常设步）+ 解 seam（参照 P5e `44f91c74` 的先例）。

### 4.3 波次 3（未创建 worktree/分支）

- **fs**（静态量分区）：输入 `f0-statics.md`。真缺世代钟的只有三小点：`ScopedDefaultUnpackState::s_synced`（Managers.cpp:6319，无任何重置点）、`g_syncedRenderStateParameters`（DirectGLES.cpp:4586，MakeCurrent 兜底但"tuple 相同而 ES context 换代"一格存疑）、`XfbImpl::g_xfbObjects`（DirectGLES.cpp:1548，值里嵌 client SharedPtr——与 fv 有交集）。Espryt 的 per-draw memo 大家族已自带双键自失效，不用动。
- **fr**（registry 重键）：输入 `f0-registry.md`。跨进程必需只有 2 处纯接线：DirectGLES.cpp `:2542`（resource_copy_region）与 `:12606`（set_storage_block_binding），记录已带句柄。13 处里 11 处在 transport 下不可达（P3b/P4b 债，不动）。顺带核实 `:8213` `GetBackendProgramId` 疑似死代码。
- **fv**（反向通道）：输入 `f0-reverse-channel.md`。实收 3 条：XFB scatter 的对象身份（`scatterProgram` SharedPtr、`targets` 裸地址当 registry 键）、`OnGlError` 绕过回调表（`PipeInputs::RecordError` 直调 `ServerSession::PostGlError`，PipeFill.cpp:2175）、Magma `MGPipeAnnounceBufferGpuWritten` handle 缺失时直捅 client 对象（ResourceTracker.h:732-739）。字节面 P5c 后已干净，别重做。

### 4.4 收口（出口门，计划 §6）

1. 双块车道 integration-split 全绿；2. strict 下 `strict-expected-markers.txt` 为空（**待裁决**：两个 ID-128 escalation 对是 P8 题材，与"为空"冲突，需集成裁定改写门口径）；3. `FieldOwnership.def` 零 BARRIER_PULLED、`rsp` 逐帧读零；4. Magma 判据（fm 报告回答）；5. G1/G2/G14 照旧；6. 逐包 red-once；7. Redmi 设备门——**本机无设备，需 `2f7cbe2e` 窗口，可能要像 P5c 一样记为记录项**。

## 5. 环境与操作要点（详见 `notes/p5f/f0-env.md`）

- 门禁在 WSL 跑：`wsl.exe -e bash -lc '...'`（用户 swung）。建树 `~/w7/notes/tools/p5e_new.sh <sha>`（**注意 fc 报告 §6：按 SHA fetch 失败时要按分支名 fetch**），门模板 `~/w7/notes/tools/p5e_gate.sh`。代码从 Windows 到 WSL 靠 `git fetch /mnt/c/.../FoldCraftLauncher/.git/modules/MobileGL`，**须先 commit**。
- 车道速查：`integration-split`（旋钮关必须 179/179 零回归）；双块车道 = `MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1` + `-L 'integration-split|integration-magma-split'`；strict 棘轮刷新流程在 `strict-expected-markers.txt` 头部注释。
- G1 硬门：pull flavour 符号/.text 恒等，跨进程代码在 pull 构建折常量（D-P 模式）。
- 提交风格：`[MG_X, ...] (Disaggregated): P5f <pkg> - ...`。
- `3rdparty/DiligentCore` 子模块的 git 报错是既有噪音，别修。

## 6. 本会话做出的集成裁定（后续会话应遵守）

- **ID-137**：包边界——fe = 15 字段 Espryt 侧 + 记录载体扩展；fm = 9 字段 Magma 侧 + T5 残余 + Magma 结构性耦合；fc = 控制面（已完成）；fs = 静态量；fr = registry 两处接线；fv = 反向通道 + RecordError + `ResourceTracker.h:732-739`。
- **待裁定（留白）**：escalation 两对（client VA，P8）与出口门 2"markers 为空"的冲突；f1 发现的 strict 棘轮 2 行 stale（基线既有漂移，f1 报告 §8.1 已归因，未修）。
- 过程纪律沿用 ID-66：每包自带 red-once；阶段收官由另一模型族审一次。
