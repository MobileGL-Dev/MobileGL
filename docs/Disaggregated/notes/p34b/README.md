# P3b / P4b — Espryt 深化（P7 的并行流 wave 2-D）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 2026-09-22 审计（[`../p7/PLAN-PH-P34B-P7.md`](../p7/PLAN-PH-P34B-P7.md) §1.2）后作为 P7 的并行流推进，与 P7 文件零重叠。
- 已落地（`origin@22761a69`）：**D1** XFB 两洞（孤儿目标保留、回写事件切片）+ 回调 10 → 9 + 四个 resolver 具名启动期 stop + readback 家族三臂；**D2** `TextureViewAliasScenario`、`TextureUploadShape` 由记录升为门（零补丁 red-once）、`espryt_memo_purity.py`、`SubsystemDeps.def`、12 场景 split 普查；**D3** view 在属主上传后陈旧的产品缺陷修复 + D-K2 六处读者读同一张表。
- 余：R-3 另一半（`MGPSamplerView::Target` 谓词）、`KHR-GL46.texture_view.coherency` 的 split 读数、CTS AFTER；Iris trace 普查已有头上结果（[`../p7/iris-census-1135c664.md`](../p7/iris-census-1135c664.md)）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P3b / P4b** 深化（Espryt）
- **状态**：**已审计（2026-09-22，`notes/p7/PLAN-PH-P34B-P7.md` §1.2）**；Espryt 项与 P7 文件零重叠，作为 P7 的并行流（wave 2-D）推进。**D1 / D2 / D3 已落地（`origin@22761a69`，2026-09-22）**：D1 = XFB 两洞（孤儿目标保留 varyings、回写事件切片）+ 回调 10→9 + 四个 resolver 具名启动期 stop + readback 家族三臂（+39/臂，readback 半边其实早已关，ID-P7-24）；D2 + 返工 = `TextureViewAliasScenario`、`TextureUploadShape` 成门（散点内缩后零补丁 red-once）、`espryt_memo_purity.py`、`SubsystemDeps.def`（R-5）、12 场景 split 普查（+76/臂）（ID-P7-26/29）；D3 = view 在 owner 上传后陈旧的产品缺陷修复（clean gate 读 storage record，三臂 3 红→14/14）+ D-K2 六处读者读同一张表（ID-P7-28/30）。**余**：R-3 另一半（`MGPSamplerView::Target` 谓词）、`KHR-GL46.texture_view.coherency` 的 split 读数、CTS AFTER（wave 4）；~~Iris trace 普查结果（wave 3）~~ 已有头上结果（`3c80cd62`，[`notes/p7/iris-census-1135c664.md`](../p7/iris-census-1135c664.md)）；逐日状态见 [`CURRENT_STAGE_STATUS.md`](../../CURRENT_STAGE_PROGRESS.md)
- **落地什么 / 范围**：view/属主发射游标**别名场景**（代码半已由 P4a `TextureEmit.h` 属主键控游标 + `TextureObjectView::ToOwner*` + P5e tx2 落地，欠验证）；`g_fboTextureSyncList`（分离安全——记录臂先返回、退路是具名 Fatal；四个 resolver 改启动期具名 stop，文本删除受 P5e 裁定 1 约束归 P13）；memo 重键**已落地**（P4a/P5f），补纯度 grep 门；~~XFB scatter 搬到 client~~（P5c/P5f 以 server staged shadow + `OnBufferWriteback` 落地；剩两处真洞：孤儿捕获目标被静默丢弃、回写事件无界——>128 KiB 即 `Fatal{EventRingOverflow}`）；删 fragColor 重推导 workaround 与 `g_broadcastMemo*`（动 pull `.text`，归 P13，只改注释）；raw-depth-fetch sampler 的 monolith 清理（可选）；**回读收口**（pack 半已关：`set_pixel_pack_state` APPLIER_DERIVED + server 中性读 + client 侧 PACK-PBO 散射；readback 半开：`get-tex-image-shadow` / `copy-image-shadow-mirror` 非 monolith 下仍 abort、split 零覆盖）；P4a 的 R-5（D-K2 规则合一处）/ R-11 必须，R-3 / R-4 / R-7 / R-10 可选；~~`ProgramArtifacts.h` 的 NDK 尺寸钉~~（P6.5 portable archive v2 + 布局指纹取代）；链接实验分给它的 **14 个符号**（D12，棘轮落地后 Tier 1）
- **验收门 / 证据**：纹理 / program 场景（split 三臂零覆盖，先普查首阻塞再注册）；CTS `texture_*` / `shader_image_*` / `packed_pixels` + P4a 欠的 `direct_state_access.framebuffers*` 在 0.5 pp 内（caselist 入库 + monolith 基线 + delta 工具，今天三者皆无）；每一个 Iris trace（CI 管道 P6 已建；**`3c80cd62` 主机结果**：39 例 × 双后端 × {monolith, inproc, spawn} 231 次全活，split 与 monolith 逐字节相同 70 / 77 行，7 行分歧均过阈值、无 Fatal；`texture-remint-pull` 未到达，**P9 例外表为空**——[`notes/p7/iris-census-1135c664.md`](../p7/iris-census-1135c664.md) §3–§4）；`TextureUploadShapeScenario` 升级成门（Mali 增量无设备，按 Adreno 记录）

## 实测：P3b/P4b wave 2-D 包 D2（`cf7ca59f`，WSL 桌面 lavapipe/llvmpipe）（原 `MEASUREMENTS.md` §13）

### 13.1 上传形状金标（R-11 / D-D4，`TextureUploadShapeScenario` 由记录升为门）

工作负载常量 `kScatteredRects=40`、`kFrames=3`、一条连续带、**三张**纹理（第三张经
`glTextureView` 的 level 1 上传，使 view/属主重映射落在被计数的窗口内）。

| 臂 | emit | box | rect | jobs | client emitter |
|---|---|---|---|---|---|
| `DirectGLES.TextureUploadShape.`（monolith） | 9 | 9 | 0 | 9 | 9 |
| `DirectGLES.Split.`（inproc） | 9 | 9 | 0 | 9 | 9 |
| `DirectGLES.Spawn.` | 9 | 9 | 0 | 9 | 9 |
| `DirectGLES.Tcp.` | 9 | 9 | 0 | 9 | 9 |

P4a 的记录是两张纹理、无 client emitter 的 `emit=6 box=6 rect=0 jobs=6`（`ctu=0`），与本行
**不可比**：工作负载不同。

`rect=0` 是 **unpack-ring 策略的既定输出**：client 确实建出 30 个 rect（2 texel 间隙，
`RegionsTouch` 不合并；summed area 120 远低于 union box 的 3/4，两道 client 侧谓词都不取 box 臂），
记录把 30 个 region 全部带过去；是 server 在 `Managers.cpp:8649` 的
`if (UnpackRingAvailable()) dirtyRectCount = 0;` 决定取 box——"One box, one job"，Mali 悬崖的既有
缓解。门钉的就是这个策略的输出。

**散点图案本身是被修过的（评审 fable 指出）**：原来的 stride `x=((i*7)%32)*2` / `y=((i*5)%32)*2`
在 64 px 图集里会打到 x=62、y=62，union box 覆盖整个 level，而 server 的 `subRectEligible`
（`Managers.cpp:8486-8491`）要求 `!dirtyRegion.CoversWholeLevel`（`MipmapStorage.h:30-33`，**包围盒**
判据）——两张散点纹理在 ring 策略之前就已经 rect-ineligible，金标数字一样但量的不是同一件事。
内缩一个 rect（`kScatterMargin`）后 union box 落在 (2,2)..(62,54)，决策点才真正带电。

红一次因此是**零补丁**的：`MOBILEGL_ESPRYT_DISABLE_UNPACK_RING=1` 一个环境变量就把两张散点纹理
从 box 翻成 rect——`box=3 rect=6 jobs=183`（3 = 连续带 x3 帧；6 = 两张散点 x3 帧；183 = 3 + 6x30），
`bytes/f[tex]` 同时从 104448 降到 9024，而 26 个像素用例全绿。内缩之前同一个变量是全绿的，这正是
`CoversWholeLevel` 把决策点短路掉的证据。

### 13.2 client 半边的读法（本包的修正）

`ctu=` 与 `tex[]` 是否在同一个窗口里，按传输分：

| 臂 | `tex[]` 在哪 | `ctu=` 在哪 |
|---|---|---|
| monolith | 唯一日志 | 同一行 |
| inproc | **server** 日志（两个文件、一个进程、一套计数器） | 同一行 |
| spawn / tcp | **server** 日志 | client 进程自己的计数器，**Present 节奏不同**——实测 `ctu=13` 对 `emit=9`，且用例读取时通常还没写出 |

这与 §12.7 最后一条"spawn client 的窗口化字段仍不可信（是结构不是缺陷）"是同一件事，从集成用例
这一侧再次证实。因此断言改读**进程内的 `MGPipeTextureEmitter::SubDataCount()`**（同一个计数器的源
头，测试进程在四个臂下都是 client），`ctu=` 只在窗口对齐时断言；是否对齐由
`PeekSplitRuntime()` 回答的传输决定，**不是**由数值是否 >= 0 决定——spawn 下 server 会发布一个
完全可读的 `ctu=0`，按数值判会把比较悄悄变成 `0 == 9`。

### 13.3 未跑项 / 不可调度项

- **Mali 帧时增量（D-D4 要求与金标并列）不可调度。** 本阶段只有一台 Redmi（Adreno 830v2），
  **无任何 Mali 设备**。按 Adreno 记录形状，偏差在此与场景头部注明：**Adreno-only; Mali delta
  deferred**。不建模、不从 2026 年那次测量的条件推算。形状是悬崖的**因**，帧时是它在某一家硬件
  上的**果**——门守的是因。

## 本目录

| 文件 | 内容 |
|---|---|
| [`espryt-d1.md`](espryt-d1.md) | espryt D1 — the Espryt (DirectGLES) stream, part 1 |
| [`espryt-d2.md`](espryt-d2.md) | P3b/P4b wave 2-D package D2 — the Espryt stream, part 2 |
| [`espryt-d3.md`](espryt-d3.md) | espryt D3 — the Espryt (DirectGLES) stream, part 3 |
| [`split-scenario-census.md`](split-scenario-census.md) | Texture / program split-scenario census (P3b/P4b wave 2-D, package D2) |
