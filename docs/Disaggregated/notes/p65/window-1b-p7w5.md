# 窗口 1b · p7w5 DirectGLES × TCP 配速矩阵（中断报告）

2026-09-22 21:43–23:37（手机 Redmi `2f7cbe2e`，WSL client ↔ 手机 server，`p7w5-c89000f5`，credit=2，逐例要求 run-ahead ARMED）。原始摘要：`/home/swung/w7/logs/p7w1b/paced.log`；逐例目录：`/home/swung/w7/logs/p7w1b/paced/pass/`。

**实际分母 38**：脚本从 `trace_cases.json` 取 `ci=true` 且包含 DirectGLES 的条目，启动行明确写 `cases=38`。旧交接的「39」应更正。第一遍完整：11 绿、27 红；红中 23 例的 client 记 `DEVICE LOST`，表示 TCP 对端挂断，紧随其后的 `Fatal{UncarriedInitialBytes,"resource_respecify"}` 是下游判定。其余 4 例中，photon-v1.3b、create-indirect、26.3 有确定的 golden 低分；1.21.11-main-menu 的第一遍原始目录被第二遍覆盖，原因未能独立复核。

第二遍原计划只复测 27 个第一遍红例；**完成 13 个后 adb 从 Windows 和 WSL 同时消失**。其中 4 例转绿、9 例仍红（4 次对端挂断、5 次超时）。`adb logcat -d` 自 23:37 卡住，确认设备消失后终止了旧采集进程，因此没有 `ALL DONE`，余下 14 个未复测。旧脚本的 `local c=$1 p=$2 d=$OUT/pass$p/$c` 在同一条 `local` 语句里提前求值 `d`，把两遍都写到 `pass/<case>/`；已复测 13 例的第一遍原始文件被覆盖，以下第一遍状态来自 `paced.log`，不能据目录重建其详细日志。

第一遍在 `screen_off_timeout=60000`、AC 供电但 `stay_on_while_plugged_in=2` 的条件下进行。9 个对端挂断落在 55–62 s（按摘要中的 `seconds` 计），与息屏假设相符，但另有短时挂断；这批数据**没有证明** MIUI 息屏是唯一原因。p7w6 对照已设 `svc power stayon true`，并在新脚本中隔离每遍目录、逐例采集完整 logcat 和 child reap。

| 用例 | 第一遍 | 第二遍 | 第一遍主要现象 |
|---|---|---|---|
| `OpenRA` | passed (17.068 s) | 未复测 | — |
| `minecraft-1.21.4-startup` | passed (11.143 s) | 未复测 | — |
| `minecraft-1.21.4-main-menu` | failed (24.476 s) | passed (138.871 s) | peer disconnected |
| `minecraft-1.21.11-main-menu` | failed (282.564 s) | failed (39.648 s) | reason unavailable (raw run overwritten) |
| `minecraft-1.17-main-menu-854` | failed (17.101 s) | passed (58.618 s) | peer disconnected |
| `minecraft-1.21.4-in-world` | passed (61.466 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-sodium-in-world` | failed (56.346 s) | passed (253.035 s) | peer disconnected |
| `minecraft-1.21.4-fabric-common-mods-in-world` | passed (160.917 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-common-mods-inventory` | failed (17.261 s) | passed (300.71 s) | peer disconnected |
| `minecraft-1.21.4-fabric-rei-inventory` | failed (121.781 s) | failed (44.607 s) | peer disconnected |
| `minecraft-1.21.4-fabric-xaero-minimap-in-world` | failed (60.539 s) | outer timeout (— s) | peer disconnected |
| `minecraft-1.21.4-fabric-xaero-world-map-in-world` | failed (485.625 s) | failed (5.524 s) | peer disconnected |
| `minecraft-1.21.4-fabric-journeymap-in-world` | failed (61.055 s) | outer timeout (— s) | peer disconnected |
| `minecraft-1.21.4-fabric-modernui-inventory` | failed (56.915 s) | failed (6.538 s) | peer disconnected |
| `minecraft-1.21.4-fabric-rei-inventory-normal-world` | failed (60.836 s) | timeout (303.134 s) | peer disconnected |
| `minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world` | failed (60.754 s) | timeout (301.587 s) | peer disconnected |
| `minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world` | failed (60.735 s) | timeout (304.891 s) | peer disconnected |
| `minecraft-1.21.4-fabric-journeymap-in-world-normal-world` | failed (61.096 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-modernui-inventory-normal-world` | failed (56.767 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | passed (49.254 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world` | failed (7.744 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world` | passed (50.763 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-sundial-lite-in-world` | failed (6.327 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world` | passed (68.322 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-complementary-unbound-in-world` | failed (49.76 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-mellow-in-world` | passed (41.257 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-nostalgia-in-world` | passed (79.091 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-bliss-in-world` | failed (54.144 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world` | passed (81.812 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-iterationt-in-world` | failed (36.194 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world` | passed (48.377 s) | 未复测 | — |
| `minecraft-1.21.4-fabric-iris-photon-v1.1-in-world` | failed (8.736 s) | 未复测 | peer disconnected |
| `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | failed (72.606 s) | 未复测 | ssim 0.008877 < 0.99 |
| `minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world` | failed (45.459 s) | 未复测 | peer disconnected |
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | failed (217.164 s) | 未复测 | ssim 0.000005 < 0.99 |
| `minecraft-1.21.1-neoforge-create-instancing-in-world` | failed (22.099 s) | 未复测 | peer disconnected |
| `improved-transparency-minecraft-26.3` | failed (377.412 s) | 未复测 | ssim 0.988537 < 0.995 |
| `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | failed (43.849 s) | 未复测 | peer disconnected |
