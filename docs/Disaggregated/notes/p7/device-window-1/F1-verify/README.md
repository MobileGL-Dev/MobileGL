# F1 复验 + E7/E8 · OpenRA 的分歧是「冷驱动缓存」下的时序竞态

APK `p7w3-3179c497`（wave 0 + X1 + F1；`../00-session/`），Redmi `2f7cbe2e`，DirectVulkan × `--use-pbuffer` × inproc。每次回放都是**新进程**（logcat pid 11917 → 12156 → 12478），所以「热」不在进程里，在 Adreno 驱动的磁盘 shader / pipeline 缓存里。

## F1 本身：生效，但不是分歧的原因

六次运行 `does not consume MGPipe subsystem` 行 **0 条**（修前每次 1 条）——F1 的机制修正在真机上成立。SSIM 却没有随之固定：

| 臂 | repeat-01 | repeat-02 | repeat-03 |
|---|---|---|---|
| inproc，run-ahead ARMED（`F1-ra1.log`） | **0.976493989**（14658 px） | **1.000000** | **1.000000** |
| inproc，lockstep `RUN_AHEAD=0`（`F1-ra0.log`） | 0.995787641（5321 px） | 0.988997893（9836 px） | 0.988997893 |

## E7：每次 `pm clear` 后再跑（冷驱动缓存）

| 运行 | SSIM | mismatch |
|---|---|---|
| E7-cold-ra1-1 | 0.976493989 | 14658 |
| E7-cold-ra1-2 | 0.976493989 | 14658 |
| E7-cold-ra1-3 | 0.976493989 | 14658 |
| E7-cold-monolith（同 APK，`--transport monolith`，同样 `pm clear`） | **1.000000** | 0 |

冷缓存下**三遍逐字相同**——给定「冷」，分歧是确定的；热了就 1.0。monolith 冷热都 1.0。

## E8：冷缓存 + `MOBILEGL_MAGMA_FRAMESINFLIGHT=1`

1.000000 / 0.976493989（两遍一绿一红）——竞态窗口变窄，没关上。

## 判读

- 历史两个观测值 0.976494 / 0.988998 = OpenRA 在 ARMED / lockstep 下的**冷缓存**读数（当时每次都是新装 APK 后的首跑）。
- 原因不在 run-ahead、不在 caps（F1 已关）、不在 client：两臂 client / server 日志逐行相同（505 行 server、12 条 pipeline 行、3 条既有 ERROR），无 Fatal 无拒绝。
- diff 图只缺帧内**最后画的**那些 sprite / 建筑（左上约 40%），背景全对 → wire 臂上的最终帧回读在 GPU 没画完最后几批（冷缓存下 pipeline 创建很慢）时就采样了；monolith 臂的回读路径同步正确。树上已有一条相关警告：`DirectVulkan::ReadPixels: no write is recorded for swapchain image N ... the readback returns its raw pixels`。
- → 交 wave 2 **B**（`WireFramebuffer.inc` / `VulkanRenderer.cpp` 的回读 / present / frames-in-flight 同步）作为第 0 片：主机 red-once = 用 `vkCreateGraphicsPipelines` 延迟旋钮复现，修后带旋钮仍 1.0；真机复验 = 三次 `pm clear` 冷跑全 1.0。

原始归档 `.trace-work/p7w3/{F1-openra-ra1,F1-openra-ra0,E7-cold-*,E8-cold-*}`（不入库）；本目录 `F1-ra1.log`、`F1-ra0.log`、`*-summary.json` 与 `../E7-coldcache/*.log`。
