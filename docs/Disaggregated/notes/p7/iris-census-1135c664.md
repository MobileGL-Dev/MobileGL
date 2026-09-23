# Iris trace 普查 @ `1135c664`（P7 wave 3 包 E1，主机 lavapipe）

> 计划：[`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §1.2 的「门：每一个 Iris trace」行与 §3 wave 3 的「Iris trace 普查 + P9 例外表」；
> [`ROADMAP.md`](../../ROADMAP.md) P3b/P4b 行门列「每一个 Iris trace（欠当前头的结果与 `texture-remint-pull` 的 P9 例外表）」；
> [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §11「Iris 普查的 P9 例外」。
> 同包的另一半（`MEASUREMENTS.md` §7.2 同名重跑）写在 [`MEASUREMENTS.md`](../../MEASUREMENTS.md) §7.2 的 P7 小节。
>
> 树 `~/w7/p7-census-e1`，分支 `p7/census-e1`，基线 `1135c664`（`origin/feat/disaggregated` 头，含 B3）。
> `build-split` = Release / clang / ccache / `MOBILEGL_BUILD_DISAGGREGATED=ON` `_INPROC=ON` `MOBILEGL_PIPE_PUSH=ON`
> `MOBILEGL_BUILD_INTEGRATION_TEST=ON` `MOBILEGL_BUILD_TRACE_REPLAY=ON`，ICD 钉 `lvp_icd.json`（lavapipe / llvmpipe）。
> 证据根（树外）：`~/w7/e1-census/head-1135c664/`（`results.jsonl`、`reduced.json`、`art/<case>/<backend>[-SPLIT|-SPAWN]/{actual.png,result.json,ctest.out}`），
> 重复轮 `~/w7/e1-census/rep{2,3}-1135c664/`，跑器 `~/w7/e1bin/census.py`（树外，未入库）。

## 0. 结论

**77 行（case × 后端）× 3 臂 = 231 次重放，231 活、0 死；split 两臂与 monolith 逐字节相同 70 / 77 行；
`texture-remint-pull` 在 231 次里一次未触发——P9 例外表为空。**

| 臂 | 活（rc 0 + 过阈值） | 其中与 monolith 逐字节相同 | 分歧 | 死 |
|---|---:|---:|---:|---:|
| monolith | 77 / 77 | — | — | 0 |
| inproc（`.SPLIT`） | 77 / 77 | 70 | 7 | 0 |
| spawn（`.SPAWN`） | 77 / 77 | 70 | 7 | 0 |

- 7 个分歧行（§3）里，**5 行是 DirectVulkan 上确定性的、与传输无关的 monolith ≠ wire 臂**（inproc 与 spawn 三遍逐字节相同、monolith 三遍逐字节相同、二者不同；
  同一 case 的 DirectGLES 行全部 `=`；5 行里 split 臂对 golden 的 SSIM 都**高于** monolith）；**1 行**（`iris-derivative` × DirectGLES）是 ~26.9k px 的系统差叠在 monolith 自身的逐跑噪声上；
  **1 行**（`iris-derivative` × DirectVulkan）是三臂各自的逐跑噪声（第 3 遍三臂逐字节相同）。
- 231 次的每一份角色日志（`output/mobilegl.client.log`，两进程臂另加 `mobilegl.server.log`）：`Fatal{` **0**、`Refuse{` **0**、`decline`（不分大小写）**0**；
  `MGWIRE-DECLINES` 行 **0**（该行只在有 decline 或挂起上传时打印，`WireDeclineTally.h:105-115`）；出现的 `MGWIRE-` 行全部是 B3 的
  `MGWIRE-FLOOR[shutdown] unsoundSerialComplete=0`。
- 对 P5b（`348d22a4`，只有 inproc 臂，79 条含 `rd12` × 2）：**72 过 / 6 abort / 1 failed → inproc 77 / 0 / 0、spawn 77 / 0 / 0**（§6）。
- 设备排除的三例（ID-P7-12）在主机三臂全活且逐字节相同（§5）。

## 1. 口径

- **分母。** 目录 `trace_cases.json` 40 例；CI split 子集 39 例（`split_trace_cases`，`trace_cases.py:296`；`split` 缺省随 `ci`，`trace_cases.py:110`），
  即设备矩阵用的同一 39 例；不在子集的只有 `minecraft-1.21.4-rd12-odinlite-in-world`（`ci: false`）。`iris-iterationrp` 的 `ci_backends` 只有 DirectVulkan，
  所以 39 × 2 − 1 = **77 行**。`build-split` 实际注册 236 条 `MobileGLTraceReplay.*`：split 臂按两个后端无条件注册（`tools/trace_replay/CMakeLists.txt:498-501`），
  多出的 `iterationrp × DirectGLES` 三臂与 `rd12` 两条 monolith 不在 CI 矩阵，本次未跑。
- **三臂。** monolith = `MobileGLTraceReplay.<case>.<backend>`（同一 disaggregated 库，transport 不设 → Monolith）；inproc = `.SPLIT`；spawn = `.SPAWN`
  （`libMobileGLServer.so`）。`run_trace_case.cmake` 的 transport 解析断言与 Fatal 断言照常生效。
- **旋钮 = CI 的旋钮**（`.github/workflows/test.yml:2734-2753`，retrace-split 作业与 monolith retrace 作业同一组）：DirectVulkan 全部导出
  `MOBILEGL_MAGMA_R11G11B10F_FALLBACK=1`；`improved-transparency` × DV 加 `MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE=1`；`iterationrp` × DV 加三个 iterationRP 旋钮。
  其余 `MOBILEGL_*` 一律清掉；`MOBILEGL_IPC_STAGE_MB` 用默认 32 MiB（P5b 的 trace 普查显式 256 MiB，本次不需要）。
- **逐字节判据。** `actual.png` 的 SHA-256 相等即 `=`；不等时用 `tools/trace_replay/compare_actuals.py compare`（retrace 门同一 SSIM）给 actual-vs-actual 的 SSIM 与 `mismatch_pixels`，
  再用同一解码器数出分歧像素的 bbox 与最大通道差。
- **日志普查。** 每条的两份角色日志逐行 grep `Fatal{`、`MGWIRE-`、`Refuse{`、`decline`（不分大小写）。控制台转录不算（配置里控制台 sink 编译掉，是假零）。
- **运行。** 每条单独 `ctest --test-dir build-split -R '^<name>$' --timeout 2400`（为了逐条导出 CI 旋钮），同时 2 条，`LP_NUM_THREADS=4`，进程树 RSS 守护 20 GiB
  （从未触发；峰值 2.45 GiB，`iterationrp` × DV × spawn）。
- **时间与负载。** 普查墙钟 **2254 s（37.6 min）**，2026-09-22 20:12:07–20:49:41（UTC−4）；231 条单条耗时之和 4501 s。主机 28 逻辑核、与另外五个代理共享，
  本次占用 ≤ 2 个重放 × 4 个 llvmpipe 线程；运行期间 1 分钟 loadavg 最低 3.96 / 平均 16.6 / 最高 30.3（每 30 s 采样，76 点）。重复轮（§3）2 × 21 条，墙钟 114 s + 150 s。

## 2. 逐行结果

`ssim` = 对 golden（`result.json`），9 位小数。`=` = actual.png 与 monolith 逐字节相同；`≠ a / n px` = actual-vs-actual SSIM 与不等像素数。
`F/R/D` = 两份角色日志合计的 `Fatal{` / `Refuse{` / `decline` 行数。`MGWIRE-FLOOR` 行全部是 `unsoundSerialComplete=0`（DirectVulkan 才有；两进程臂打在 server 日志）。

| # | case | 后端 | monolith ssim | inproc ssim | spawn ssim | inproc vs mono | spawn vs mono | F/R/D 行（mono · inproc · spawn） | `MGWIRE-FLOOR` 行（mono · inproc · spawn） | 秒（mono · inproc · spawn） |
|---:|---|---|---:|---:|---:|---|---|---|---|---|
| 1 | `OpenRA` | GLES | 1.000000000 | 1.000000000 | 1.000000000 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 2 · 2 · 2 |
| 2 | `OpenRA` | Vulkan | 1.000000000 | 1.000000000 | 1.000000000 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 2 · 2 · 2 |
| 3 | `minecraft-1.21.4-startup` | GLES | 0.999999662 | 0.999999662 | 0.999999662 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 1 · 1 · 1 |
| 4 | `minecraft-1.21.4-startup` | Vulkan | 0.999999662 | 0.999999662 | 0.999999662 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 1 · 1 · 1 |
| 5 | `minecraft-1.21.4-main-menu` | GLES | 0.997008676 | 0.997008676 | 0.997008676 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 3 · 3 · 3 |
| 6 | `minecraft-1.21.4-main-menu` | Vulkan | 0.997008630 | 0.997008630 | 0.997008630 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 3 · 6 · 6 |
| 7 | `minecraft-1.21.11-main-menu` | GLES | 0.993474922 | 0.993474922 | 0.993474922 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 3 · 3 · 3 |
| 8 | `minecraft-1.21.11-main-menu` | Vulkan | 0.993474920 | 0.993474920 | 0.993474920 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 3 · 7.1 · 7.1 |
| 9 | `minecraft-1.17-main-menu-854` | GLES | 0.999961600 | 0.999961600 | 0.999961600 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 2 · 2 · 2 |
| 10 | `minecraft-1.17-main-menu-854` | Vulkan | 0.999961130 | 0.999961130 | 0.999961130 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 2 · 4 · 4 |
| 11 | `minecraft-1.21.4-in-world` | GLES | 0.999976222 | 0.999976222 | 0.999976222 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 7.1 · 7.1 · 7.1 |
| 12 | `minecraft-1.21.4-in-world` | Vulkan | 0.999976105 | 0.999976105 | 0.999976105 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 7.1 · 15.1 · 15.1 |
| 13 | `minecraft-1.21.4-fabric-sodium-in-world` | GLES | 0.999985623 | 0.999985623 | 0.999985623 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 20 · 21 · 18 |
| 14 | `minecraft-1.21.4-fabric-sodium-in-world` | Vulkan | 0.999986092 | 0.999986092 | 0.999986092 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 20 · 19 · 19 |
| 15 | `minecraft-1.21.4-fabric-common-mods-in-world` | GLES | 0.999947917 | 0.999947917 | 0.999947917 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 6 · 6 · 6 |
| 16 | `minecraft-1.21.4-fabric-common-mods-in-world` | Vulkan | 0.999947917 | 0.999951280 | 0.999951280 | ≠ 0.999997 / 39 px | ≠ 0.999997 / 39 px | 0/0/0 · 0/0/0 · 0/0/0 | 2 · 2 · 2 | 8 · 11 · 11 |
| 17 | `minecraft-1.21.4-fabric-common-mods-inventory` | GLES | 0.999996158 | 0.999996158 | 0.999996158 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 7 · 8 · 8 |
| 18 | `minecraft-1.21.4-fabric-common-mods-inventory` | Vulkan | 0.999996158 | 0.999996192 | 0.999996192 | ≠ 1.000000 / 17 px | ≠ 1.000000 / 17 px | 0/0/0 · 0/0/0 · 0/0/0 | 2 · 2 · 2 | 12 · 18 · 19 |
| 19 | `minecraft-1.21.4-fabric-rei-inventory` | GLES | 0.999989912 | 0.999989912 | 0.999989912 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 56.2 · 61.2 · 66.2 |
| 20 | `minecraft-1.21.4-fabric-rei-inventory` | Vulkan | 0.999989912 | 0.999989912 | 0.999989912 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 69.2 · 108.4 · 107.4 |
| 21 | `minecraft-1.21.4-fabric-xaero-minimap-in-world` | GLES | 0.999999456 | 0.999999456 | 0.999999456 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 41.4 · 40.4 · 38.4 |
| 22 | `minecraft-1.21.4-fabric-xaero-minimap-in-world` | Vulkan | 0.999999456 | 0.999999456 | 0.999999456 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 42.3 · 79.2 · 83.3 |
| 23 | `minecraft-1.21.4-fabric-xaero-world-map-in-world` | GLES | 0.999999823 | 0.999999823 | 0.999999823 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 34.3 · 34.2 · 31.1 |
| 24 | `minecraft-1.21.4-fabric-xaero-world-map-in-world` | Vulkan | 0.999999823 | 0.999999823 | 0.999999823 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 26 · 50.1 · 49.1 |
| 25 | `minecraft-1.21.4-fabric-journeymap-in-world` | GLES | 0.999984122 | 0.999984122 | 0.999984122 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 32 · 36.2 · 37.2 |
| 26 | `minecraft-1.21.4-fabric-journeymap-in-world` | Vulkan | 0.999984122 | 0.999984122 | 0.999984122 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 43.1 · 68.2 · 79.3 |
| 27 | `minecraft-1.21.4-fabric-modernui-inventory` | GLES | 0.999996645 | 0.999996645 | 0.999996645 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 51.1 · 47.1 · 47.1 |
| 28 | `minecraft-1.21.4-fabric-modernui-inventory` | Vulkan | 0.999996645 | 0.999996645 | 0.999996645 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 2 · 2 · 2 | 57.1 · 108.2 · 104.2 |
| 29 | `minecraft-1.21.4-fabric-rei-inventory-normal-world` | GLES | 0.999988010 | 0.999988010 | 0.999988010 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 10 · 9 · 13 |
| 30 | `minecraft-1.21.4-fabric-rei-inventory-normal-world` | Vulkan | 0.999988010 | 0.999988010 | 0.999988010 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 14 · 19 · 18 |
| 31 | `minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world` | GLES | 0.999991832 | 0.999991832 | 0.999991832 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 5 · 6 · 6 |
| 32 | `minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world` | Vulkan | 0.999991832 | 0.999991832 | 0.999991832 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 6 · 13 · 13 |
| 33 | `minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world` | GLES | 0.999995376 | 0.999995376 | 0.999995376 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 7 · 7 · 7 |
| 34 | `minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world` | Vulkan | 0.999995376 | 0.999995376 | 0.999995376 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 8 · 15 · 18 |
| 35 | `minecraft-1.21.4-fabric-journeymap-in-world-normal-world` | GLES | 0.999978990 | 0.999978990 | 0.999978990 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 9 · 9 · 9 |
| 36 | `minecraft-1.21.4-fabric-journeymap-in-world-normal-world` | Vulkan | 0.999978990 | 0.999995028 | 0.999995028 | ≠ 0.999984 / 107 px | ≠ 0.999984 / 107 px | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 8 · 15 · 18 |
| 37 | `minecraft-1.21.4-fabric-modernui-inventory-normal-world` | GLES | 0.999997315 | 0.999997315 | 0.999997315 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 10 · 10 · 10 |
| 38 | `minecraft-1.21.4-fabric-modernui-inventory-normal-world` | Vulkan | 0.999997292 | 0.999997292 | 0.999997292 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 2 · 2 · 2 | 10 · 22 · 22 |
| 39 | `minecraft-1.21.4-fabric-iris-bsl-in-world` | GLES | 0.997496023 | 0.997496023 | 0.997496023 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 7 · 6 · 6 |
| 40 | `minecraft-1.21.4-fabric-iris-bsl-in-world` | Vulkan | 0.997324199 | 0.997324199 | 0.997324199 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 6 · 7 · 7 |
| 41 | `minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world` | GLES | 0.995425520 | 0.995425520 | 0.995425520 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 4 · 4 · 4 |
| 42 | `minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world` | Vulkan | 0.996629722 | 0.996629722 | 0.996629722 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 4 · 5 · 5 |
| 43 | `minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world` | GLES | 0.999643966 | 0.999643966 | 0.999643966 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 8 · 8 · 7 |
| 44 | `minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world` | Vulkan | 0.999439052 | 0.999439052 | 0.999439052 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 7 · 7 · 8 |
| 45 | `minecraft-1.21.4-fabric-iris-sundial-lite-in-world` | GLES | 0.996300657 | 0.996300657 | 0.996300657 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 14 · 15 · 13 |
| 46 | `minecraft-1.21.4-fabric-iris-sundial-lite-in-world` | Vulkan | 0.996284254 | 0.996284254 | 0.996284254 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 13 · 16 · 17 |
| 47 | `minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world` | GLES | 0.999503181 | 0.999503181 | 0.999503181 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 13 · 13 · 13 |
| 48 | `minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world` | Vulkan | 0.999296510 | 0.999296510 | 0.999296510 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 13 · 13 · 12 |
| 49 | `minecraft-1.21.4-fabric-iris-complementary-unbound-in-world` | GLES | 0.999478525 | 0.999478525 | 0.999478525 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 11 · 11 · 12 |
| 50 | `minecraft-1.21.4-fabric-iris-complementary-unbound-in-world` | Vulkan | 0.999244084 | 0.999244084 | 0.999244084 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 12 · 13 · 13 |
| 51 | `minecraft-1.21.4-fabric-iris-mellow-in-world` | GLES | 0.999822891 | 0.999822891 | 0.999822891 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 4 · 4 · 4 |
| 52 | `minecraft-1.21.4-fabric-iris-mellow-in-world` | Vulkan | 0.999312266 | 0.999312266 | 0.999312266 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 4 · 4 · 5 |
| 53 | `minecraft-1.21.4-fabric-iris-nostalgia-in-world` | GLES | 0.999988313 | 0.999988313 | 0.999988313 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 12 · 12 · 13 |
| 54 | `minecraft-1.21.4-fabric-iris-nostalgia-in-world` | Vulkan | 0.999983572 | 0.999983572 | 0.999983572 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 13 · 12 · 11 |
| 55 | `minecraft-1.21.4-fabric-iris-bliss-in-world` | GLES | 0.998057535 | 0.998057535 | 0.998057535 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 5 · 5 · 5 |
| 56 | `minecraft-1.21.4-fabric-iris-bliss-in-world` | Vulkan | 0.997986272 | 0.997986272 | 0.997986272 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 5 · 5 · 5 |
| 57 | `minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world` | GLES | 0.999992296 | 0.999992296 | 0.999992296 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 5 · 5 · 5 |
| 58 | `minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world` | Vulkan | 0.995753250 | 0.995753250 | 0.995753250 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 6 · 6 · 6 |
| 59 | `minecraft-1.21.4-fabric-iris-iterationt-in-world` | GLES | 0.997752215 | 0.997752215 | 0.997752215 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 6 · 5 · 6 |
| 60 | `minecraft-1.21.4-fabric-iris-iterationt-in-world` | Vulkan | 0.999198504 | 0.999209304 | 0.999209304 | ≠ 0.999987 / 10601 px | ≠ 0.999987 / 10601 px | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 6 · 6 · 6 |
| 61 | `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world` | GLES | 0.995919679 | 0.995919679 | 0.995919679 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 5 · 6 · 5 |
| 62 | `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world` | Vulkan | 0.998640770 | 0.998663674 | 0.998663674 | ≠ 0.999975 / 14551 px | ≠ 0.999975 / 14551 px | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 7 · 8 · 9 |
| 63 | `minecraft-1.21.4-fabric-iris-photon-v1.1-in-world` | GLES | 0.999135212 | 0.999135212 | 0.999135212 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 14 · 13 · 13 |
| 64 | `minecraft-1.21.4-fabric-iris-photon-v1.1-in-world` | Vulkan | 0.998781357 | 0.998781357 | 0.998781357 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 13 · 15 · 15 |
| 65 | `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | GLES | 0.998774617 | 0.998774617 | 0.998774617 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 14 · 15 · 15 |
| 66 | `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | Vulkan | 0.998529629 | 0.998529629 | 0.998529629 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 17 · 21.1 · 22.1 |
| 67 | `minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world` | GLES | 0.996196604 | 0.996376198 | 0.996376154 | ≠ 0.999784 / 26912 px | ≠ 0.999784 / 27066 px | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 15 · 13 · 12 |
| 68 | `minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world` | Vulkan | 0.996658898 | 0.996658844 | 0.996658798 | ≠ 1.000000 / 461 px | ≠ 1.000000 / 620 px | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 13 · 16 · 15 |
| 69 | `minecraft-1.21.1-neoforge-create-indirect-in-world` | GLES | 0.999961275 | 0.999961275 | 0.999961275 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 11 · 12 · 11 |
| 70 | `minecraft-1.21.1-neoforge-create-indirect-in-world` | Vulkan | 0.999956667 | 0.999956667 | 0.999956667 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 8 · 13 · 13 |
| 71 | `minecraft-1.21.1-neoforge-create-instancing-in-world` | GLES | 0.999957572 | 0.999957572 | 0.999957572 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 7 · 9 · 9 |
| 72 | `minecraft-1.21.1-neoforge-create-instancing-in-world` | Vulkan | 0.999952470 | 0.999952470 | 0.999952470 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 8 · 14 · 13 |
| 73 | `improved-transparency-minecraft-26.3` | GLES | 1.000000000 | 1.000000000 | 1.000000000 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 118.3 · 51.1 · 68.2 |
| 74 | `improved-transparency-minecraft-26.3` | Vulkan | 0.999914064 | 0.999914064 | 0.999914064 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 65.2 · 147.3 · 122.2 |
| 75 | `minecraft-1.21.4-fabric-iris-iterationrp-in-world` | Vulkan | 0.995833421 | 0.995833421 | 0.995833421 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 1 · 1 · 1 | 14 · 15 · 15 |
| 76 | `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | GLES | 1.000000000 | 1.000000000 | 1.000000000 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 81.1 · 84.1 · 81.1 |
| 77 | `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | Vulkan | 0.998402027 | 0.998402027 | 0.998402027 | = | = | 0/0/0 · 0/0/0 · 0/0/0 | 0 · 0 · 0 | 63.1 · 69.1 · 69.1 |

## 3. 分歧：7 行 × 2 臂

逐格的「第一可观测分歧」是像素（本包无产品代码范围，未做逐 draw 二分）。「三遍」= 本普查 + `rep2` + `rep3` 三轮同条目的 `actual.png` 摘要（前 8 位），
像素数一律对**第 1 轮 monolith** 计。

| # | case × 后端 | 臂 | 第一可观测分歧（对 monolith 第 1 轮） | 三遍摘要（monolith / inproc / spawn） | golden SSIM mono → split | 读法 |
|---:|---|---|---|---|---|---|
| 1 | `fabric-common-mods-in-world` × DV | inproc、spawn | 39 px，bbox x 704–709 / y 88–127，最大通道差 66；actual-vs-actual SSIM 0.999997 | `86f4e1ae`×3 / `1e609e9b`×3 / `1e609e9b`×3 | 0.999947917 → 0.999951280 | 确定性、与传输无关 |
| 2 | `fabric-common-mods-inventory` × DV | inproc、spawn | 17 px，x 704–709 / y 89–127，最大差 12；SSIM 1.000000（6 位） | `04f2c7ed`×3 / `4259fcdf`×3 / `4259fcdf`×3 | 0.999996158 → 0.999996192 | 同上，与 #1 同一屏幕列 |
| 3 | `fabric-journeymap-in-world-normal-world` × DV | inproc、spawn | 107 px，x 704–841 / y 64–168，最大差 115；SSIM 0.999984 | `db12b2ac`×3 / `4e18261e`×3 / `4e18261e`×3 | 0.999978990 → 0.999995028 | 同上 |
| 4 | `fabric-iris-iterationt-in-world` × DV | inproc、spawn | 10 601 px，全帧 bbox 0–846 / 0–478，最大差 29；SSIM 0.999987 | `3402c31a`×3 / `f4b7ac7c`×3 / `f4b7ac7c`×3 | 0.999198504 → 0.999209304 | 同上 |
| 5 | `fabric-iris-iterationt-nodsa-in-world` × DV | inproc、spawn | 14 551 px，0–850 / 0–478，最大差 65；SSIM 0.999975 | `0a8d24db`×3 / `8f2e3506`×3 / `8f2e3506`×3 | 0.998640770 → 0.998663674 | 同上 |
| 6 | `fabric-iris-derivative-main-d24.4.14-in-world` × GLES | inproc | 26 912 px，13–853 / 0–477，最大差 56；SSIM 0.999784 | `168c9d95`,`b128d91b`,`b128d91b` / `ebbe8e9d`×3 / — | 0.996196604 → 0.996376198 | 系统差 + monolith 自身噪声（两轮 monolith 相差 630 px、最大差 3） |
| 6 | 同上 | spawn | 27 066 / 27 290 / 26 912 px（三遍），最大差 56 | — / — / `758a247d`,`3ac060ec`,`ebbe8e9d` | 0.996196604 → 0.996376154 | 同一系统差，spawn 另有逐跑噪声；第 3 遍与 inproc 逐字节相同 |
| 7 | `fabric-iris-derivative-main-d24.4.14-in-world` × DV | inproc、spawn | 461 / 620 px（第 1 遍），最大差 ≤ 3 | `af950bf9`,`6ebc82be`,`c966f682` / `bdf82617`,`20546f48`,`c966f682` / `03a75145`,`6b4a2770`,`c966f682` | 0.996658898 → 0.996658844 / …798 | **不是分歧**：三臂各自逐跑不稳，第 3 遍三臂逐字节相同 |

**读法。**

- **#1–#5（DirectVulkan，确定性）**：inproc 与 spawn 三遍逐字节相同 → 与传输无关；同 case 的 DirectGLES 行 `=` → 不是 trace 或 client 半边的事；
  monolith 与 wire 臂在 Magma 里走的是两条绘制路径（`VulkanRenderer::SetupDraw` 在非 monolith 传输下直接转 `SetupWireDraw`，`VulkanRenderer.cpp:7059-7066`；
  ROADMAP P7 行 ID-P7-5 记的就是这一分叉），差异落在这两条路径之间。5 行里 split 对 golden 都**更近**，#1–#3 的分歧像素都落在屏幕右侧 x ≥ 704、y 64–168 的同一块区域。
  没有任何一行碰到 P9 机制（§4 的站点 0 触发）。门 1 的 trace 半边（`CONTRACT-P7.md` §2.4：78 条 DV split 行达阈值）不受影响——14 个分歧格全部过阈值。
- **#6（`iris-derivative` × DirectGLES）**：P5b 在这一格停在 `Fatal{UnmigratedEmulation, "texture-remint-pull"}`；在头上该站点**未被到达**
  （transport 下 `MGPipeUnmigratedEmulation` 无条件 `MGLOG_F` + `abort`，`PipeApply.cpp:3517-3524`，日志里没有这一行即没有到达），所以这 26.9k px **不是 remint 拉取**。
  monolith 自身在这条 trace 上两轮就不逐字节（630 px、最大差 3），系统差（最大差 56）比噪声大一个量级、split 对 golden 更近。**未归因**，记在 §7。
- **#7**：噪声，不是分歧；列出是因为单遍普查会把它读成分歧。

## 4. P9 例外表

**头上主机口径下，没有任何 case 因 `texture-remint-pull` 或其它 P9 范围的机制而在 split 臂上与 monolith 分歧、abort 或被排除——例外表为空。**
下表是 P9 行可以直接指向的站点清单与本次读数（所有站点在 transport 下都经同一个无条件 Fatal 臂 `PipeApply.cpp:3517-3524`，所以「0 触发」是可证的）：

| 机制 | 站点 | 归属 | 231 次重放中触发 | P5b 时停在它上面的 case | 头上读数 |
|---|---|---|---:|---|---|
| `texture-remint-pull`（`RequireImageBindableStorage` 的重上传，server staged 谓词 / N-4 标记） | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:6446`、`MobileGL/MG_Backend/DirectGLES/Managers.cpp:6463` | P9（纹理拉取，`TextureRemintPullScenario`）/ P8（仿真下放） | **0** | `create-indirect` × GLES、`iris-derivative` × GLES、`iris-photon-v1.3b` × GLES | 三格两臂都活；`create-indirect`、`photon-v1.3b` 与 monolith 逐字节相同；`derivative` 见 §3 #6（非 remint） |
| `get-tex-image-shadow` | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:14974` | P9（回读） | 0 | — | — |
| `copy-image-shadow-mirror` | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:13251` | P8 | 0 | — | — |
| `generate-mipmap-storage` / `-cpu-fallback` / `-cpu-filter` | `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:11561`、`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:12802`、`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:12890` | P8 | 0 | — | — |

含义：P3b/P4b 门「每一个 Iris trace」在主机上**不需要按名排除任何 case**；但语料因此**不再覆盖** remint 站点，P9 行门列的
`TextureRemintPullScenario`（含无解用例）必须自己造出这条路径，不能指望 trace 普查兜底。设备上未测（§7）。

## 5. 设备排除项的主机读数（ID-P7-12，不在这里排除）

| case | 设备（W4 §2，monolith × DV） | 主机 monolith / inproc / spawn（DV） | 主机 monolith / inproc / spawn（GLES） | 主机 split vs mono |
|---|---:|---|---|---|
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | 0.832（OQ-17） | 0.999956667 ×3 | 0.999961275 ×3 | 两后端两臂 `=` |
| `minecraft-1.21.11-main-menu` | 0.165（trace 伪差） | 0.993474920 ×3 | 0.993474922 ×3 | `=` |
| `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | 0.0089（ID-P7-13） | 0.998529629 ×3 | 0.998774617 ×3 | `=` |

三例都是设备事实（Adreno 830 驱动 / 本机 UBO 对齐），主机三臂全活、逐字节相同。附：`iris-bsl-esc-menu-854` 两后端三臂全活、`=`，DV × inproc 进程树峰值 1.16 GiB——
设备上 spawn/inproc 的 server 内存死（ID-P7-32，M2）在主机 lavapipe 的这台 88 GiB 机器上不表现为失败，不能当作 M2 的证据。
`create-indirect` × DV 在 P5b 的 llvmpipe 上 RSS > 60 GiB 被守护杀死，本次三臂峰值 0.65–0.75 GiB、8–13 s。

## 6. 与 P5b 的逐格比对（`348d22a4`，`~/w7/p5b-final-census-348d22a4/trace-transitions.json`，inproc 臂、trace 显式 256 MiB）

| 条目 | P5b 首阻塞 | 头 inproc | 头 spawn |
|---|---|---|---|
| `create-indirect` × GLES | `Fatal{UnmigratedEmulation, "texture-remint-pull"}` | 活，`=` | 活，`=` |
| `create-indirect` × DV | llvmpipe RSS > 60 GiB 守护杀（failed） | 活，`=` | 活，`=` |
| `iris-bsl-esc-menu-854` × GLES | `Fatal{InitialBytesNotCarried, "resource_respecify"}` | 活，`=` | 活，`=` |
| `iris-derivative` × GLES | `Fatal{UnmigratedEmulation, "texture-remint-pull"}` | 活，≠（§3 #6） | 活，≠（§3 #6） |
| `iris-photon-v1.3b` × GLES | `Fatal{UnmigratedEmulation, "texture-remint-pull"}` | 活，`=` | 活，`=` |
| `rd12-odinlite` × GLES / DV | `InitialBytesNotCarried` / `Fatal{BarrierTimeout, "Present"}` | 不在分母（`ci: false`），未跑 | 同左 |

P5b 的 72 个通过格在头上两臂全活。

## 7. 未跑 / 未答

- **TCP 臂**：`MOBILEGL_TRACE_TCP_ENDPOINT` 未配置（`tools/trace_replay/CMakeLists.txt:503-506` 只在配置时注册），本普查无 TCP 读数。
- `rd12-odinlite`（`ci: false`）与 `iterationrp` × DirectGLES（已注册、不在 CI 矩阵）未跑；verify 臂不在本包。
- 逐 draw 二分：§3 #1–#6 的第一条分歧 draw 未定位；#6 的系统差未归因（候选方向：monolith 自身在该 trace 上不确定，先要一个确定的 monolith 对照）。
- 三遍逐字节只对 7 个分歧行做了；其余 70 行是单遍 `=`。
- 设备：本包全在主机；设备上的 P9 例外（若有）由 wave 4 的门 3 终局跑回答。

## 8. 复现

```bash
bash ~/w7/notes/tools/p7_worktree.sh census-e1 1135c664
cmake -S ~/w7/p7-census-e1 -B ~/w7/p7-census-e1/build-split -DMOBILEGL_BUILD_TRACE_REPLAY=ON
ninja -C ~/w7/p7-census-e1/build-split -j 4
python3 ~/w7/e1bin/census.py ~/w7/e1-census/head-1135c664 -j 2      # 231 条，可续跑
python3 ~/w7/e1bin/reduce.py  ~/w7/e1-census/head-1135c664           # reduced.json + 表
python3 ~/w7/e1bin/repcmp.py                                         # §3 的三遍比对
```

（`3rdparty/apitrace/thirdparty/*` 在新 worktree 里是未初始化的子模块，`MOBILEGL_BUILD_TRACE_REPLAY=ON` 配置前需要从已水合的树拷入。）
