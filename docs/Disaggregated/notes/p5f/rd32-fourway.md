# Redmi RD32 四组合性能记录

2026-09-20。用户要求把 Espryt / Magma × monolith / inproc 的 render distance 都设为32，实测一轮。
最终每组合选入一次完整窗口；原始数据、排除项与复算方法均保留。

## 结果

| 组合 | 平均 FPS | 秒区间 FPS 中位数 | 秒区间 FPS P10 | 有效秒数 / 区间数 |
|---|---:|---:|---:|---:|
| Espryt-monolith | 62.18 | 59.97 | 56.94 | 60.021 / 60 |
| Espryt-inproc | 64.02 | 63.00 | 58.94 | 60.024 / 60 |
| Magma-monolith | 116.08 | 116.00 | 114.87 | 60.029 / 60 |
| Magma-inproc | 86.85 | 84.50 | 70.00 | 60.021 / 60 |

这轮 Espryt inproc / monolith 为 **1.030 倍**，小差异不足以确立稳定优势；
Magma inproc / monolith 为 **0.748 倍**，即本轮平均低约 **25.2%**。
Magma monolith 仍在约120 FPS的平台附近，不能据此推断其无上限吞吐。
两种 inproc 均实际 `run-ahead ARMED`；不是把 Magma lockstep 混作 inproc 结果。

这是**单轮默认动态频率下的持续性能观测**，不是隔离出的固定频率架构开销。
不能把差值全部归因于线程拆分，也不能把它与此前 RD12、未统一条件的旧结果作严格倍率比较。

## 共同条件与采样

- Redmi M332BF `2f7cbe2e`；FCL `com.tungsten.fcl.mgdebug.debug`；Minecraft `26.3-rc-3`。
- 同一 APK / MobileGL 行为代码 `194382c96a8a5f51ef23f412fdccfbd40a82bf78`，测试期间未安装新库。
  `libMobileGL.so` SHA256：`046d86c288ce891b16770148578ba1a6fea4057783b69c2359f5830e58b7b223`，测试结束仍一致。
  APK 身份继承 [run-ahead 报告](magma-runahead.md)；后续 Git 提交仅文档。
- `renderDistance:32`、`simulationDistance:12`、`overrideWidth:2620`、`overrideHeight:1280`、
  `enableVsync:false`、`maxFps:260`、`inactivityFpsLimit:"minimized"`。F3关闭，FCL FPS计数开启。
  测前/测后 options 均保留，并核对关键值。
- 原世界完整保留，在副本上固定初始相机、晴天和正午，关闭时间/天气推进与新生物生成；
  每组恢复同一个副本快照，不依赖 force-stop 阻止自动保存。既有生物模拟仍保留。
- 每组独立重启，`cpuss-0-0` 入场低于40°C，进入世界后预热180秒，再独立采样至少60秒。
  所选四组有四个不同 boot-id，八张 pre-window/final 图均人工检查；场景、方向与HUD正常。
- 所选四组测量窗所有硬件观测均为风扇档位2、RPM>0。CPU/GPU由系统DVFS与温控管理，
  没有关闭温控。每约5秒记录频率、温度、GPU busy与风扇状态。
- FCL `getFps()` 返回的是自上次读取的 swap 计数。以同PID相邻 `FCLFPS` 日志的真实时间差归一化，
  只保留完整落在测量窗内的区间；平均为总帧数/总时间。P10与中位数是这些约一秒区间的FPS分位，
  **不是逐帧1% low**。MGPipe的120帧窗口计数仅作近似交叉核对，不作为主结果。
- 每组实际 backend/transport、capability 与 `rsp=0` 均核查。inproc使用strict=1、role-split=1、
  stage=256 MiB、present credit=1；monolith实际无IPC配置、`vbs=0`。

## 测量窗硬件观测

| 组合 | CPU温度 °C | 大核实际频率 GHz | GPU实际频率 MHz | 风扇 RPM |
|---|---:|---:|---:|---:|
| Espryt-monolith | 53.1–55.8 | 1.690–1.958 | 525–660 | 14466–14591 |
| Espryt-inproc | 53.9–56.6 | 1.690–1.690 | 443–607 | 14163–14363 |
| Magma-monolith | 55.8–56.6 | 1.690–1.690 | 832–900 | 14163–14487 |
| Magma-inproc | 52.3–56.2 | 1.690–1.958 | 525–734 | 14570–14804 |

表中为有限采样点的范围，不表示采样间绝无变化。部分截图有读取硬件状态时产生的
Shell root toast；所有组使用相同遥测方式，未把它当作游戏图形错误。
Magma monolith的屏幕缺口留白在右侧，其余组在左侧；前后图方向一致，游戏视口均为2620×1280。

## 排除与恢复

最初按历史档案尝试固定频率：1100 MHz被厂商限到1050，1050持续负载又降到900，
900尝试则在正式窗口开始后发现大核被温控从1958.4改到1689.6 MHz。这些尝试均作废，
不进入表格，也没有通过关闭保护来强行维持频率。

第一份 stock Espryt monolith 结果为59.884 FPS，但窗口风扇全程为0/RPM0；它保留为排除控制。
补测的fan2结果62.178 FPS替代它，未合并、未平均或挑选多个有效窗口中的最好者。
其他三组原始窗口直接选入。选择映射与原因保存在 `selection.json`。

结束后原世界61个文件逐一SHA256核对完全一致；options.txt、mg_env.txt、mg_transport.txt
逐字节恢复（包括原RD12设置），风扇恢复0。临时设备世界副本已清理，主机证据保留。
既有Gradle修改与父FCL未合并索引未改动；本轮没有改渲染代码。

## 证据

最终选入证据：`C:/Users/geekerwan/.codex/tmp/rd32-fourway-20260920-final/`，包括：

- `performance.json`：从原始日志重算的完整区间、硬件观测与运行模式检查；
- `root-signoff.json`、`restoration-verified.json`、`fixture-hashes.json`：验图与真实恢复核验；
- `selection.json` 与四组原始summary、完整library/logcat、before/after截图；
- 原采样脚本在 `../rd32-fourway-20260920-stock/benchmark.py`；补测在
  `../rd32-fourway-20260920-stock-fan2-espryt/benchmark.py`；离线分析器在
  `../rd32-fourway-20260920-1050/analyze.py`，支持 `--root <目录> --require-complete`。

仓库内精简机读数据见 [rd32-fourway.json](rd32-fourway.json)。
