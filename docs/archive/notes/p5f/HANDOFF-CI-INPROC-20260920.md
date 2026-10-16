# CI / inproc 功能补齐交接（2026-09-20，用户要求停止）

> 用户最后明确要求：“写个交接文档，不要继续你未完成的任务了”。已停止继续实现、构建、测试和推送，只整理本交接。所有子 agent 已结束/中断；停止时检查没有本任务的 retrace / CTest / replay 进程在运行。最后启动的完整 retrace 已自行结束，结果已读入本文。
>
> **任务尚未完成，GitHub CI 尚不能宣称全绿。本轮修复没有 push，也没有合并到 `feat/disaggregated`。**

## 1. 用户确定的范围

- `feat/disaggregated` 的主产物必须编译拆分实现，并实际以 `inproc` 执行主验收。
- 普通 OpenGL 功能场景、完整 retrace 矩阵都应迁移并通过。不得用 skip、降低 SSIM、缩矩阵、吞退出码或“仅记录 census”替代实现。
- 明确验证 monolith 内部机制的对照保留 monolith，不强改成 inproc。
- D/P 只是 `MOBILEGL_BUILD_DISAGGREGATED` / `MOBILEGL_PIPE_PUSH` 两个编译开关；运行时还需 `MOBILEGL_TRANSPORT=inproc`。已向用户解释，后续尽量直接说中文。
- 本轮是此前 P5f / Magma run-ahead / RD32 性能工作的后续 CI 修复。历史 P5f 完成不等于现在扩展出的全部 inproc 功能验收完成。

## 2. 工作树、提交与未提交改动

基础目录：`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/`。

| 用途 | 路径 / 分支 | 停止时 HEAD |
|---|---|---|
| 本轮所有实现，后续应从这里继续 | `MobileGL-ci-fix` / `codex/fix-github-ci` | **`70c8230b3e05a8d62d881d4996db27e0ad251936`** |
| 用户交付树，尚未集成本轮修复 | `MobileGL-disagg` / `feat/disaggregated` | **`30b1bf25a4ea5ed8394dc9909c420051633c5115`** |
| WSL Arch 测试树 | `/home/swung/w7/p5f-int` / `codex/ci-fix` | **`70c8230b3e05a8d62d881d4996db27e0ad251936`** |
| 默认会话 cwd | `MobileGL` / `dev` | 不是本轮修改树，勿误改 |

`MobileGL-ci-fix` 有两份**未提交、不能当成已验证成品**的改动：

- `.github/workflows/test.yml`
- `.github/workflows/apk.yml`

这是 fm 最后审查 artifact 清理时留下的修改：Linux cleanup 等待/识别 `retrace-split`；保留失败 case 的 fixture 和结果；两工作流先成功读取完整 GitHub 分页 inventory，再执行删除，避免 process substitution 的 API 失败被当成“没有失败用例”。修改已落盘，但 agent 随后结束，未拿到完整最终校验/提交回报。后续先审 diff，不要直接丢掉，也不要假定已测试。

本交接文档另外新增在 CI 树和交付树，尚未提交。交付树原 `HANDOFF-P5F.md` 顶部增加了本交接入口。

### 必须保留的交付树现有修改

- `MobileGL-disagg/build.gradle`：SHA256 `331C22D6509AC5E5794F135E27D024675808220EC5001E404803BAB889DF1FF5`
- `MobileGL-disagg/android-plugin/app/build.gradle.kts`：SHA256 `FA7F21664A05A7172376DD47E00F435158D20E2F6422626C5D932B3228CF950C`
- `.trace-work/`、`Testing/`、`docs/Disaggregated/inproc-dataflow.html` 均为停止时已有未跟踪内容。
- 前两份 Gradle 改动是先前 FCL 本地测试 plumbing，与本轮新提交的 Gradle wiring 有重叠。最终集成要逐项协调，尤其旧 embedded-FCL arm64 限制；不要 reset 或把无关修改一起提交。
- FCL 父仓库此前有 `UU FCL/src/main/java/com/tungsten/fclauncher/FCLauncher.java` 等用户改动，不属于本轮。
- 不修复既有 DiligentCore gitdir 噪声。看状态/差异可用 `--ignore-submodules` / `--ignore-submodules=all`。

### WSL 状态

停止时除了 `tools/trace_replay/fixtures/` 的 LFS 实体文件外，WSL tracked 源码无差异。fixture 显示很多 `M`：这是把 LFS pointer 替换成已按 pointer SHA256 校验的真实文件，**不是换 golden**。共补齐 92 个 LFS 文件，来源为已有 `/home/swung/mgl/MobileGL` 或 Windows CI 树。不要直接 reset 成 pointer 后又把解包失败误当 renderer 失败。

未跟踪的 `Testing/`、`build-pull/`、`build-trace-ci/` 保留。`build-pull/` 原已存在。

Windows → WSL 的正常同步方式（不要让 Linux git 直接操作 Windows 工作树）：

```bash
git -C ~/w7/p5f-int fetch /mnt/c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/.git/modules/MobileGL refs/heads/codex/fix-github-ci
git -C ~/w7/p5f-int -c submodule.recurse=false merge --ff-only FETCH_HEAD
```

## 3. 已完成的实现与关键提交

以下均已提交在 CI 分支；“已实现”不替代第 4 节的实际验证范围。

| 主题 | 提交 / 说明 |
|---|---|
| CI 主产物与完整双模式矩阵 | `7e82ea0f`、`bb3a5199`、`97ce032f`：目标分支主库/APK 开 D/P + INPROC；完整 **77 个 case/backend × 2 transport = 154**；日志、artifact、汇总分模式；独立 OFF/pull 库供错误库负控 |
| 清除旧“预期红 / census 算通过” | `347bb90c`：客户端数组专用通道必须绿并纳入主 GPU/split 标签；全量 inproc CTest 退出码为硬门 |
| 合法 monolith 对照 | `11132981`：指针/slot ABA、CSO 开关、resource/object mask、direct-adoption 计数等机制对照明确 monolith；普通像素功能保持当前模式。`03c6b8c9` 纠正 TextureUploadShape，应仍测 inproc，测试内等真实 applied boundary |
| 原 Linux CI 红 | `08c63f77`：统计等绘制 Present applied；`10bfd29f`：E1 负控改为观察真实协议 wait，避免要求所有 run-ahead draw 在 VERB_BARRIER=0 时必崩 |
| Android 回放生命周期 | `7fd83133`：retained ReplaySession、重建时取消旧 callback/unbind、trace configChanges 整数资源、native 原子 lease、ANativeWindow 生命周期；防两个 Activity 同时进入 JNI 全局回放 |
| 客户端顶点/索引 | `beba0256`：复制到 owned 普通 wire buffer；GPU EBO/indirect/count 先真实同步；保留 baseVertex/baseInstance/drawID；新增 GL 回归 |
| XFB / 查询 | `d0c6fdbc`、`70df57bb`、`8eaebc90`、`e2d6e33d`：Magma wire capture、真实 native query/result、XFB lifetime 删除、空捕获 buffer lazy readback；`4610b573` 防首次握手在 renderer 未创建时调用 Vulkan timer hook；`c7de0dff` 补齐 32 个 verb-boundary 的逐项断言 |
| GLES image / view / SSO | `c1bf7e15`、`57e0f6be`：server-owned promotion、native view；`c3a5d2f5`：空 image slot 保持实际 unit，避免高位空槽误解绑 unit 0，非当前 SSBO 程序先发布；`e2d6e33d`：sampler high-water tracker、whitebox peek 在 apply 线程执行 |
| 纹理读回 / PBO | `29b5e064`、`7875e28c`、`c0b40ff5`、`859c0efb`、`44cac27b`：真实服务端 GPU bytes、PACK/PBO 由 client 写入、texture 大回复分块、完整目的范围检查、避免同步 PBO 回复后虚构 pending GPU write；`f5234b9e` signed/half depth 编码 |
| sparse mip / packed format | `cf0f77b0` 暂时隔离 GLES native BASE/MAX；`40c5443f` **MGPSubData 72 → 88 字节**，携带完整 level extent；`ed01f4e3` 同时修 GLES hook 与通用 applier 的 adoption；`616f3c90` 按 Vulkan format 能力设置 attachment usage、只允许真正无 backing 且非 GPU-dirty 的 server stage 读回；`d339053e` RGB 展开前真实长度/溢出检查 |
| framebuffer / MSAA | `1389e8e7` cube face→layer；`ec4ca06d` color resolve；`0cb0e9af` RP2 depth/stencil resolve；`7b926429` 经纯 GPU 单 aspect buffer 中转保留另一 aspect，并补正确 resolve-write 屏障 |
| packed-float mip 生成 | `5310ff62` 缺 native blit 时用真实 GPU shader 逐 mip/layer 生成；`2426abb8` backend 裁 logical mip window；`70c8230b` frontend immutable/view 不再重分配或截断 owner，mutable 只更新生成窗口，新增窗口回归 |

注意：`MGPMipPlan.LevelCount` 在当前组合里保持 **logical end-exclusive**，不是 `BaseLevel` 后的数量。backend 先减 logical base，再映射 storage root。这个语义不可单边改。

GLES 的 texture-view 功能原本需 `MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1`。相关功能 case 的 CTest recipe 已显式开启；早先 view construction 失败不是 CapsMirror 非虚函数问题。

## 4. 验证快照：不要混淆不同提交

统一证据目录：

- WSL：`/home/swung/w7/ci-fix-logs/`
- Windows：`C:/Users/geekerwan/.codex/tmp/github-ci-fix-20260920/`

### 4.1 完整 unit / GPU 最近一次整轮

**`5922e54b`**：`/home/swung/w7/ci-fix-logs/full-5922e54b/`

- unit：2319 总数，2308 pass、10 skip、1 fail。唯一失败为 `FieldOwnershipTest.VerbBoundaryOpsCoverEveryVerbShapedCall`，已在 `c7de0dff` 修复并单项复测 PASS。
- GPU inproc：1443 总数，1198 pass、221 skip、24 fail。
- 24 个失败都曾用独立日志重新复现：`full-5922-failure-probes/summary.json`，有 command、原输出、XML、首 Fatal。
- 后续 **`ed01f4e3` 的这 24 个精确失败全部复测 PASS，0 skip**：`failure-recheck-ed01/summary.json`。
- **没有在最终 `70c8230b` 上重跑完整 unit、完整 GPU、完整 monolith GPU。** 不能把“24/24 修复”写成“最后完整门全绿”。221 个 skip 也尚未做最终逐项分类审计。

其它已执行证据：

- E1 三个协议用例 baseline 绿 → 关必要 barrier 仅两个指定断言红 → restored 绿；smoke/redcheck 通过。见 `e1-control-current2/`、`e1-control-driver2.log`、`control-smoke-current.log`、`redcheck-control-current.log`。尚需最终头完整 split 负控（包括 E3）与其它出口门。
- `4610b573` 的 Magma query 13 项：13 PASS / 0 SKIP / 0 FAIL，真正 inproc/REQUIRE_GPU=1，零 Fatal：`magma-init-4610-query13/summary.json`。此前 6 个 skip 的直接原因是旧 query 返回 0，并非仅 COUNTER_BITS 广告问题。
- `40c5443f` 的 4 项 GLES XFB / TextureParams：4 PASS，相关 tracker 回归 PASS：`gles-final4-40c5/summary.json`。
- depth/stencil aspect 修复：`depth-aspect-buffer.{log,stdout,xml}`，增强 case PASS，Vulkan/sync validation 无 VUID/SYNC 错误。

### 4.2 最新新增窗口回归仍有一个失败

在 **`70c8230b`**：

- `F1WireScenario.GenerateMipmapHonorsMutableBaseAndMaxWithoutChangingOtherLevels`：两后端 PASS。证据 `mip-window-70c/summary.json`。
- `F1WireScenario.GenerateMipmapThroughViewKeepsOwnerLevelsAndLayersOutsideItsWindow`：Magma PASS，**GLES FAIL**。证据 `mip-view-window-70c/summary.json`。
- 断言：`F1WireScenario.cpp:596`，`owner mip 3, layer 0` 有 4/4 texel 不对。该 view 应只改变 root mip 3 / layer 1；窗口外 layer 0 被改变。
- 已分给 fr，但该 agent 随后退出，**没有这项修复的未提交半成品或完成报告**。仍需定位 GLES view mip 生成 / owner stage replay，不能减断言。

### 4.3 完整 Linux inproc retrace 已结束：67/77，10 失败

**这是停止整理时读取的最终结果，不是最初工具尚在运行时的 15/77 进度。**

证据：`/home/swung/w7/ci-fix-logs/retrace-inproc-70c8230b-hydrated/`

- `results.xml`、`ctest.log`、`matrix.json`、`cases.txt`
- **77 总数，67 PASS、10 FAIL、0 SKIP**，原阈值未修改。
- 单 case `result.json`、`mobilegl.log`、`retrace.log`、actual/diff 在：
  `/home/swung/w7/p5f-int/build-trace-ci/tools/trace_replay/<case>/<backend>/output/`
- 首次 `retrace-inproc-ed01f4e3/` 是 LFS pointer 解包失败，不能当 renderer 结果。之后校验补齐 92 个实体文件才跑上面这轮。

10 个失败：

| case | backend |
|---|---|
| `minecraft-1.21.4-fabric-iris-sundial-lite-in-world` | DirectVulkan |
| `minecraft-1.21.4-fabric-iris-iterationt-in-world` | DirectGLES、DirectVulkan |
| `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world` | DirectGLES、DirectVulkan |
| `improved-transparency-minecraft-26.3` | DirectGLES、DirectVulkan |
| `minecraft-1.21.4-fabric-iris-iterationrp-in-world` | DirectVulkan |
| `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | DirectGLES、DirectVulkan |

停止时只读取了已有输出，未继续诊断或修复：

- Sundial Vulkan：`ssim=0.000046370`，阈值 `0.99`，mismatchPixels `408472`，真实图像不匹配。
- 其余日志中有 7 次 `Fatal{RingOverrun, "SEG_STAGE"}`：单 blob `34,603,008` / `56,623,104` / `134,217,728` 字节超过 `33,554,432` 字节 stage。
- 另有 2 次 `Fatal{InitialBytesNotCarried, "resource_respecify"}`，描述 initial content 的后续 subdata 没有发出。
- 尚未逐 case 完成这 9 个 Fatal 的对应/根因分析。不能把放大 stage、放宽阈值或跳过 case 直接当迁移完成。
- 完整 monolith retrace 本轮未跑；新头 Android retrace / AVD lifecycle 实跑也未完成。

### 4.4 Android 构建与旧远端失败

Windows 已成功编译 trace/plugin Release APK，各含 arm64-v8a + x86_64，D/P/INPROC=ON：

- `android-plugin/app/build/outputs/apk/trace/release/MobileGL-plugin-trace-release-5922e54.apk`
- `android-plugin/app/build/outputs/apk/plugin/release/MobileGL-plugin-release-5922e54.apk`
- 日志：Windows evidence 目录的 `android-gradle-full-ready.log`，`BUILD SUCCESSFUL in 8m 8s`。
- APK/native SHA 和二进制 marker：`android-local-built-identity.json`。这是较早 5922 标记产物，**不是最终头产物，也没有声称已在设备/AVD 跑过本轮 APK**。
- 主库及 trace 编译依赖已初始化（排除了 DiligentCore）；glslang 的 `update_glslang_sources.py` 已成功执行。
- Windows Java selector loopback 需要当前命令的 `JAVA_TOOL_OPTIONS=-Djdk.net.unixdomain.tmpdir=C:/T`；Gradle `-Pmobilegl.xxx=...` 参数在 PowerShell 要整段加引号。

旧远端 `30b1bf25` 两轮均已完成且红：

- Test run `35513925180`：`integration-split-strict` job `106089389584`，`integration-split` job `106089389592`。
- APK run `35513925171`：Vulkan create-indirect retrace job `106089768679`。
- Windows `android-latest-vulkan/` 的 logcat 确认 Activity relaunch 后两个回放 TID（2276 / 2289）重叠；与先前 Bliss 的重复 JNI 生命周期根因一致。`7fd83133` 是代码修复，仍需新头远端验证。
- **本轮新修复从未 push，远端还没有这些提交的 CI 结果。**

## 5. 后续明确的未完成事项

仅在用户重新授权继续后执行：

1. 先审查并收尾两份未提交的 artifact-cleanup workflow diff。
2. 修最新 GLES view mip-window 回归。
3. 修上述 10 个完整 retrace 失败：大 stage blob、initial bytes 携带、Sundial 实际像素错误；保持完整矩阵与阈值。
4. 最终头重跑完整 unit、完整 inproc GPU、明确 monolith 对照；审查剩余 skip 的真实原因。
5. 跑完整 split strict / dual-block / clientarray / E1/E3 / run-ahead / generated/include/模式证明等适用门。部分已有老头证据，不等于最终头全绿。
6. 重建最后提交的两 ABI Android APK；验证真实 AVD overlay/recreate 生命周期和完整两 transport retrace。
7. push 修复分支，实际观察 GitHub workflows 结束并修远端差异；再协调交付树 Gradle 现有修改，合入/push `feat/disaggregated`，确认远端 SHA。
8. 写最终报告。不要把当前 handoff 或历史 P5f close-report 当成本轮完工证明。

## 6. 恢复所需命令与工具说明（未在停止后执行）

WSL `build-split` 已配置 D/P/INPROC。`build-trace-ci` 是独立 replay runner，使用：

```text
MOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY=/home/swung/w7/p5f-int/build-split/libMobileGL.so
```

retrace 用的是共享库，**只 build `MobileGLIntegrationTest` 不会更新它**；修改后需构建 `MobileGL`。CTest integration 多数链接静态库。

Windows evidence 目录已有复现助手，都是本任务临时脚本，不是产品代码：

- `run-feature-probes.py`：从 CTest discovery 保留 recipe，强制真实 inproc/strict/role/REQUIRE_GPU，逐 case 私有 log/stdout/XML；可 `--regex` 或 `--failed-junit`。
- `run-ci-retrace.py`：按原 `github-test-matrix` 精确选 77 项，`ctest --tests-from-file`，mode 参数为 inproc/monolith；输出按 HEAD 命名。
- `run-current-full.sh`：完整 unit 后完整 inproc GPU，两个退出码均报告。
- `summarize-results.py`：只读 JUnit 汇总。
- `hydrate-trace-fixtures.py`：校验 LFS pointer SHA 后从现有实体缓存复制，初次已成功，无需盲目重复。

当前 replay 已结束，无需恢复旧 session。未来两 transport 运行会共用 build-trace-ci 的 case 输出目录，**不要同时跑它们覆盖同名图片/日志**，下一轮前先归档已有失败证据。

多 agent 曾共享同一文件并发生提交夹带：`c3a5d2f5` 带入 query agent nativeDelete hunk，`e2d6e33d` 又带入 root LevelWidth adoption，直到 `40c5443f` 才是完整可编译组合。现在提交链配套已齐全。以后每次提交逐 hunk 检查归属；不要让另一 agent 的未完成 hunk 混入独立提交。

子 agent 在 completed/errored 后，`send_message` 不会启动新工作；恢复需 `followup_task`。本次所有 agent 已停止，不存在后台继续修复的约定。

## 7. 历史设备成果，仅作背景

本轮之前已交付 Magma inproc / run-ahead / RD32 性能修复；`70fb6689` 下 RD32 Magma inproc **116.73 FPS**，同包 monolith **115.01 FPS**。Redmi `2f7cbe2e`，FCL 包尾 `.mgdebug.debug`，MC `26.3-rc-3`。历史已安装库 SHA256：`11f26ca1f1d6c9a3d89134c31b2348306e669b1d9ebcc2fc77c7f5def2b4f818`。

这些是历史头证据，**不代表 `70c8230b` 或后续代码已完成 Redmi E2E/性能验收**。原世界/options 已在先前工作恢复并核验，勿无故再改。
