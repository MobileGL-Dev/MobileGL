# 设备定频档案与核验

本分支性能 A/B 所用设备的定频方式与核验结果。规则：每次运行前后各跑一次 `pin_device.sh <serial> check`，DRIFT 的运行作废重跑；stock 值硬编码、不从已钉设备采样（否则一次 restore 会把钉频变成永久）；温度门 40 °C；reboot-clean、同热窗口、两臂背靠背。

## 当前战役设备（2026-09-11 起，P4a 之后唯一用于 A/B 的设备）

| 设备 | SoC / GPU | 大核 | 小核 | GPU | 备注 |
|---|---|---|---|---|---|
| Redmi M332BF `2f7cbe2e` | SM8750 / Adreno 830v2 | policy6 → 1958400（stock 1017600–3072000） | policy0 → 1555200（stock 556800–2745600） | kgsl `min/max_pwrlevel = 0`；2026-09-11 实测 1050 MHz（厂商 `thermal_pwrlevel` 锁 1），2026-09-16 起实测 1100 MHz | 主动风扇 `/sys/class/xm_power/hw_monitor/pwm_fan` 恒 level 2，用例间降温 <1 min；温度门 `cpuss-0-0` |

与小米 `35d0befa` 同 SoC、同 OPP 表，两台的钉频数值可比；跨钉频口径（1050 vs 1100 MHz）的活动不可比钟频，只有同场配对可比。Redmi 的行已于 P6 收尾（2026-09-22）并入树内 `tools/device_bench/pin_device.sh`（`2f7cbe2e` 条目；stock 范围与小米不同，GPU 实测钳在 1050 MHz：`thermal_pwrlevel` 写 0 读回 1，见 [`../notes/p6/README.md`](../notes/p6/README.md) §12.7）；此前它只在树外的 `~/w7/notes/p2/devices/pin_device.sh`。

## 2026-09-07 核验：小米 `35d0befa` 与 Oppo `3B159D009VZ00000`（P2 / P3a 使用）

结论：`bench.sh` 的 `/proc/ppm` + `/proc/gpufreq` 钉法在两台上都不存在（MT6993 已改 `/proc/gpufreqv2/`），所以用 `bench.sh --no-pin` + `tools/device_bench/pin_device.sh <serial> pin|unpin|check`（纯 adb + su；exit 0 PINNED / 1 DRIFT / 2 UNPINNED，UNPINNED 非零使 `check && measure` 不可能量到未钉设备）。`devices/*.env` 的 `PROFILE_VERIFIED=1` 只认证文件里的节点与钉值，不认证 `bench.sh` 能驱动它们；给 `bench.sh` 加 `PIN_STYLE` 仍未做。

| | 小米 24129PN74C（SM8750 / Adreno 830v2） | Oppo PLG110（MT6993 / Mali，gpufreqv2） |
|---|---|---|
| 大核 | policy6 → 1958400（精确 OPP；stock 1017600–2841600） | policy4 → 2000000（最近 OPP，+2.1%；stock 300000–3500000，厂商守护随后改回 3200000） |
| 小核 | policy0 → 1555200（精确；stock 556800–2745600） | policy0 → 1600000（+2.9%；stock 300000–2100000） |
| 额外 | — | policy7（单核 4.21 GHz）同钉 2000000，否则一个热线程就打破钉频 |
| 方法 | `scaling_min_freq = scaling_max_freq = 目标`；governor（walt / sugov_ext）不动；写序 min→`cpuinfo_min_freq`、max→目标、min→目标（min 高于当前 max 会被 cpufreq 钳住） | 同左 |
| GPU | `kgsl-3d0/{min,max}_pwrlevel = 0` → 1100 MHz（devfreq 路径封顶 1050）；`gpuclk` 单位是 Hz | `/proc/gpufreqv2/fix_target_opp_index = 0` → 1716000 kHz，恢复写 −1；不用 ged `custom_*`（取 OPP 下标、且被 urcc-service 争写） |
| 温度门 | `cpuss-0-0`（zone13），idle 34.7 °C | `soc_max`（zone14），idle 36–37 °C，门只勉强可达 |
| 30 s 八路负载核验 | 7 个样本零漂移，47 → 52 °C | 7 个样本零漂移，53 → 62 °C |
| 陷阱 | `gpubusy` 打的是周期数，busy 百分比读 `gpu_busy_percentage` | `scaling_governor/min/max` 是 0660 system，必须经 su 读，否则空值看起来像在工作 |

两台核验后均已解钉并复核 UNPINNED，写过的节点全部恢复。未核验：Oppo GPU 钉在图形负载下的实际时钟（核验窗口 GPU 处于 power-collapse）、整场 bench 窗口内的钉频持久性、重启后状态、厂商热管理介入。
