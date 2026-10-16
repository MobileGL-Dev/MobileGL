# W5 验收 · p7w5（B3 + B2 + D3 之上）真机：OpenRA 27/27 golden

APK `p7w5-713bea9a`（`apk-p7w5.sha256` = `01040add…`；`apk-p7w5-install.txt`：`versionName=26.09.713bea9-trace`），= 集成树 `713bea9a` = `origin@b089e0de` + B3（`76bbb6c3..9a494b6f`）+ ID-P7-33。并排包 `top.mobilegl.plugin.p7w1.trace` 原地升级。全部 DirectVulkan × pbuffer；`--archive-dir .trace-work/p7w5/<run>`（不入库）。

## 1. 验收矩阵（ID-P7-25 定的判据：run-ahead 与 lockstep 各 6 遍全等 golden，冷热各半）

| 组 | run | 结果 |
|---|---|---|
| inproc run-ahead cold ×3（每遍前 `pm clear`） | `inproc-cold-{1,2,3}` | 1.000000 / 0 px / `ace2af04` ×3 |
| inproc run-ahead warm ×3 | `inproc-warm-{1,2,3}` | `ace2af04` ×3 |
| inproc lockstep（`MOBILEGL_IPC_RUN_AHEAD=0`）cold ×3 | `inproc-lock-cold-{1,2,3}` | `ace2af04` ×3 |
| inproc lockstep warm ×3 | `inproc-lock-warm-{1,2,3}` | `ace2af04` ×3 |
| spawn run-ahead cold ×3 | `spawn-cold-{1,2,3}` | `ace2af04` ×3 |
| spawn run-ahead warm ×3 | `spawn-warm-{1,2,3}` | `ace2af04` ×3 |
| spawn lockstep ×3 | `spawn-lock-{1,2,3}` | `ace2af04` ×3 |
| inproc `MOBILEGL_MAGMA_FRAMESINFLIGHT=8` ×3（p7w4 上 6/6 红） | `fif8-{1,2,3}` | `ace2af04` ×3 |
| inproc dump-armed（`--dump-texture-2d 31249,3,0`，p7w4 上 0/5 绿） | `dump-inproc-{1,2,3}` | `ace2af04` ×3 |

**27/27**，全部 ssim 1.000000、mismatch 0、actual sha `ace2af04` = golden。p7w4 上同类 26 遍是约 50% 掷硬币、三张图；B3 的三个预测（dump-armed ×3、FIF=8 ×3、lockstep）全部兑现。

## 2. 回归

| case | p7w5 | p7w4 | monolith（E0a） |
|---|---|---|---|
| `iris-iterationt-in-world` | 0.999419883 | 0.999419883 | 0.999421604 |
| `iris-iterationt-nodsa-in-world` | 0.998933123 | 0.998933123 | 0.998936755 |
| `improved-transparency-minecraft-26.3` | 0.999600026 | 0.999600026 | 0.999600026 |

逐位与 p7w4 相同：B3 的 floor 改动没有移动任何像素。

## 3. 读法

- 门 3 的 OpenRA 项**关闭**：机制（ID-P7-33：wire 臂 completed-frame-serial floor 在中帧 pooled-fence 提交退休时无证据上升，帧首次 `glBufferSubData` 走无序 host memcpy）由主机计数证实（26→0）、由真机 27/27 验收。
- 仍开的门 3 项只剩 **bsl-esc-menu-854**（M2，server 死 VkBuffer 无界累积）与 §7.2 的终局形式（分母 36 三遍逐位相同 + spawn 臂同会话），留 p7w6。
- 会话末 supervisor 已重启（`0.0.0.0:40613`）。
