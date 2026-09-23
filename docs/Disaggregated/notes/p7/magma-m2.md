# P7 wave 4 M2：Magma wire 臂的延迟 VkBuffer 集有界（分支 `p7/magma-m2`）

> 裁定见 [`INTEGRATOR-DECISIONS-P7.md`](INTEGRATOR-DECISIONS-P7.md) ID-P7-27 / ID-P7-32（M1 的归因与
> 「server 侧回收不能假设一帧有界」）；规范见
> [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0（规则 I / J）、§7.1 门 3、§9、
> §12 的 bsl-esc-menu 行。基线 = `ec46a550`（`build-split` / lavapipe 钉住 / 集成测试 ON）。
>
> 主机口径：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`），`build-split` = Release /
> clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON`。测量用 M1 的采样器
> （`~/w7/m1/memrun.sh`：0.1 s 采 `/proc/<pid>/status` 与 `maps` 行数，覆盖 spawn 出的 server）与 M1 的
> 只读探针（`M1WireBuffers` / `M1DeferredBuffers` / `M1LiveVkBuffers` / `MOBILEGL_M1_VMA_DUMP`），打在
> 两棵**只用于测量**的树上：`~/w7/m2-meas` = `ec46a550` + 探针，`~/w7/m2-meas-after` = 本分支 + 同一探针
> （外加一个数 descriptor set 的探针，§5.3）。探针不进任何提交。
>
> 基线门读数（`ec46a550`）：`unit` 2429、`integration-split` 303、`integration-magma-split` 93、
> `-spawn` 72、`-tcp` 74、`integration-magma-full-split` 533，全绿。

## 0. 提交

M2 已按下表的 **pipe 列**挑进集成树（`~/w7/pipe`）；全文引用一律用 pipe 树上的 SHA，分支
`p7/magma-m2` 上的原提交列在旁边只作对照。第二轮（§9）的提交在分支 `6f67fd7f` 之上，尚未挑入。

| # | pipe 树 | 分支 | 一句话 |
|---|---|---|---|
| 1 | `b81a8827` | `736b16a4` | `[MG_Backend]` wire 臂被 `glBufferData` 孤立的 store 在 defer 路径上一证明无 GPU 命令还能引用就销毁，不再等帧界 |
| 2 | `c569c916` | `e47e2eb6` | `[MG_Backend]` 仍停放的孤立 store 超过 `MOBILEGL_IPC_WIRE_DEFERRED_MB`（默认 64）时，server 在帧中取 host-access 同步点并全部回收 |
| 3 | `0acee60a` | `09901bae` | `[MG_Remote]` spawn 启动器把 `MOBILEGL_IPC_WIRE_DEFERRED_MB` 透传给 server 子进程（环境清洗当时唯一保留的 `MOBILEGL_IPC_*`；第二轮再留三个，§9.3） |
| 4 | `9328bdc7` | `c4d77917` | `[MG_Backend, MG_Util, MG_Test, MG_IntegrationTest, scripts]` `MagmaWireReclaimScenario` 进 `integration-magma-{split,spawn}`，读 server 的 `wbuf[]` gauge |
| 5 | `f0356dbe` | `7db41ff4` | `[MG_Backend, MG_IntegrationTest]` 同一同步点在停放超过 1024 个 store 时也触发 |
| 6 | `5d8a2b99` | `6f67fd7f` | `[docs]` 本文第一轮 |

`VulkanRenderer.cpp` 一行未动（B3 的文件）；`WriteWireBuffer` / `GetCompletedSerial` /
`NotifyFrameSerialComplete` 语义未动（`WriteWireBuffer` 只把它那条 Fatal 换成共享的
`WireBufferSyncFatal("host-write")`，同一行字、同一个 abort 站点）；monolith 的 `OnRespecify` 与
transient arena 的帧界节奏未动。

## 1. 缺陷与机制（`file:line` 按 `ec46a550`）

1. **每个过 wire 的 `glBufferData` 都孤立旧 store**：`VkBufferManager::RespecifyWireBuffer`
   （`VkBufferManager.cpp:189`）在 `:197` 无条件 `DeferRelease(std::move(resource->buffer))`，再 `Create`
   新 store。孤立本身是对的（M1 测过 monolith 式条件孤立：6701 对 6702，无效）。
2. **回收只在帧界**：`DeferRelease`（`:1183`）压进 `m_deferredBufferReleases[m_currentFrameIndex]`，
   只有 `CollectDeferredReleases`（`:1198`）清它，而它只从 `BeginFrame`（`:521`）与
   `CollectAllDeferredReleases`（`:530`）进；前者在 present 路径（`VulkanRenderer.cpp:14231`）与
   `TryDrainFrameTransients` 的每第 8 次 drain（`VulkanRenderer.cpp:13691/:13697`），后者在
   `TryDrainFrameTransients`（`:13630`/`:13675`），它要求所有提交完成**且**当前无录制——长帧里几乎不成立。
3. **帧界不来**：ID-P7-32 / §12，bsl fixture 1 303 535 次调用里只有 2 次 `eglSwapBuffers`，harness 在目标
   swap 上快照后退出，server 整次回放收到 **1** 条 present。

主机结果（`ec46a550` + 探针，spawn）：回放结束时 25 935 个活 VkBuffer，25 903 个在延迟桶里，
25 931 个出自 `VkBufferManager.cpp:221`（探针树上 `RespecifyWireBuffer` 的 `Create`），VMA 25 990 个
分配 / 345.5 MB；monolith 同一回放 10 个活 VkBuffer。

## 2. 修复

### 2.1 serial/submit 门控回收（`b81a8827`，仅 `VkBufferManager.{h,cpp}`，`#if MOBILEGL_BUILD_DISAGGREGATED`）

wire 臂的孤立 store 不再进按帧槽分桶的 `m_deferredBufferReleases`，而进一张扁平的
`m_deferredWireReleases`，每条带三个事实：`lastUseSerial`（**在 `RespecifyWireBuffer` 把它清零之前**读）、
`submitIndex`（停放时的 `GetSyncPointSubmitIndex()`）、`bytes`。死亡证明三条，任一成立即销毁：

| 证明 | 何时成立 | 为什么可靠 |
|---|---|---|
| `lastUseSerial == 0` | 自铸造或自上次 host-access 等待以来没有 GPU 命令引用过这个 store | 每条把 store 交给 GPU 的路径都盖 `m_frameSerial`（从 1 起）；`WaitForWireBufferHostAccess` 只在等完 sync point 后清零。**这种 store 在停放点直接销毁，不进表** |
| `IsSubmitIndexComplete(submitIndex)` | 能引用它的最后一次提交已退休 | 停放时 record 已在 bump 过的 slice epoch 之后，此后录制的命令拿不到它；此前录制的要么已提交（≤ `m_submitCounter`），要么在下一批（`m_submitCounter + 1`）。与 `CollectWireObjects` 对 wire image/view 表用的是同一形状 |
| renderer 空闲：`IsSubmitIndexComplete(GetSyncPointSubmitIndex())`（或无 renderer） | 无录制、所有提交已退休 | 此时表里每一条都死，包括被最小化 Present 丢弃的录制所标记、其 index 可能永远不完成的那条 |

**每次停放都扫一遍**（`DeferWireRelease` → `SweepDeferredWireReleases`），帧界只是多一个扫描点
（`CollectDeferredReleases` 末尾）。表按停放序、`GetSyncPointSubmitIndex()` 不减，所以死者是**前缀**：
第一条未完成的证明其后全未完成，一次扫描至多两三次 fence poll。`Shutdown` / `RecreateTransientArenas`
（二者已证明设备空闲）整表销毁。

**帧 serial 地板刻意不作证明**：它在帧内不动（`NotifyFrameSerialComplete` 拒绝当前 serial），对单 present
回放一个也放不掉；帧内能动的只有 submit index，而它是 fence 观测不是计数。（第一轮此处还引 B3 把地板判为不可靠
作第二条理由——那是对集成树上已修的地板的过期引用，第二轮删去；决定不变。）可作第四条证明的扩展见 §6.3。

### 2.2 水位线（`c569c916` + `f0356dbe`）

扫描之后若仍停放 `> MOBILEGL_IPC_WIRE_DEFERRED_MB` 字节**或** `> 1024` 个 store，停放点调
`pVulkanRenderer->WaitForSubmitIndex(pVulkanRenderer->GetSyncPointSubmitIndex(), UINT64_MAX, /*flush*/true)`
——`WaitForWireBufferHostAccess` 的同一个调用：先把当前录制提交，再等它——然后再扫；因为表里没有一条能标在
刚等完的 sync point 之后，这次扫描清空全表。同步点完成不了（`WaitForSubmitIndex` 返回 false：flush 失败、
设备丢失）时死于**既有**站点 `Fatal{ResourceUnavailable, "buffer-write-sync"}`：`WriteWireBuffer` 那条
`MGLOG_F` + `abort` 挪进 `WireBufferSyncFatal(site)`，两处共用，`{site=host-write|deferred-watermark}` 区分；
`fatal_census.py` 79 个 abort 站点不变，家族词不变。

**为什么还要个数上限（`f0356dbe`）**：只有字节水位线时，bsl spawn 的某段无提交区间停放了 12 497 个 store
而只占 39.5 MB（低于 64 MiB），一次同步也没触发（§5.1 中间行）。VkBuffer 按对象计价，小孤立永远碰不到字节
预算。1024 是常量 `VkBufferManager::kWireDeferredCountCeiling`，不是旋钮。

### 2.3 spawn 透传（`0acee60a`）

`ServerSpawn.cpp` 的 `BuildChildEnv` 清洗掉所有 `MOBILEGL_IPC_*`（`:83` `ShouldScrub`），于是新旋钮在 inproc
有意义、在 spawn 静默无意义——实测 spawn server 在 8 MiB 的车道预算下停放 47 MiB、0 次同步，它从一个已经没有
这个变量的环境里解析出了默认值。改为**按名**保留这一个（不放宽前缀）。它不指名端点、角色或段尺寸，两道防递归
检查（`Dial=No`、`ServerMain` 对 TRANSPORT / SERVER_PATH / RING_MB / STAGE_MB 的清洗核验）都不受影响。
tcp 的 server 读 supervisor 自己的环境，与所有旋钮相同。

## 3. 旋钮语义：`MOBILEGL_IPC_WIRE_DEFERRED_MB`

- **谁读**：只有 **server 角色**的 `VkBufferManager`（Magma wire 臂）。经 `MG_Config::Ipc.WireDeferredMb`
  （`Config.h` IpcTable，`ConfigLoader.cpp` `InitIpc` 读一次，范围 0..65536，默认 **64**，写进
  `Config: IPC ... wire-deferred=%uMiB` 那一行）。inproc：本进程环境；spawn：启动器透传；tcp：supervisor 环境。
- **管什么**：已孤立、但还可能被「已录制未退休」的 GPU 工作引用的 wire buffer store 的**停放字节**。不管活 store
  （record 持有的），不管 transient arena，不管 monolith 臂。
- **怎么管**：停放点扫描后，停放字节 `> 值 × 1 MiB` **或**停放个数 `> 1024` → 取一次同步点（flush + 等）→ 全表回收。
  所以 server 的孤立 store 峰值 ≤ 预算 + 一个 store、≤ 1025 个。代价是每一预算量（或每 1024 个）孤立一次帧中提交，
  不是每个 `glBufferData` 一次；bsl-esc-menu-854 整次回放 23 次。
- **0 = 负对照**，不是「无限」：两个触发都关，帧内 respecify-and-draw 回到无界（`MagmaWireReclaimScenario` 的两条
  水位线用例必须红，§4）。`lastUseSerial == 0` 与 submit 门控两条证明在 0 下照常工作。
- **按 CONTRACT-P5/Config.h 的规矩，新 IPC 旋钮应经集成者**：本包把它加进了 IpcTable，请集成者追认，并补进
  `ARCHITECTURE.md` 的旋钮清单。

## 4. 场景、车道、red-once（规则 J）

`MG_IntegrationTest/Scenarios/MagmaWireReclaimScenario.cpp`，三条用例都在**一帧之内**（第一个到最后一个
respecify 之间没有 swap），读 server 自己发布的四个 PipeStats gauge（`PipeStats.h` `Gauge::WireBuffers`
…`WireDeferredSyncs`，行上是 `wbuf[wbufs= wlivepk= wdefpk= wdefsync=]`，只在 push 构建里有，pull `.text` 不动）。
峰值是**运行最大值**：缺陷活在帧内，swap 时刻的采样读的是帧界自己扫完之后的值，什么也证明不了。

| 用例 | 负载 | 断言 |
|---|---|---|
| `RespecifiesWithNoDrawBetweenKeepTheLiveStoreCountBounded` | 1 个 buffer、1024 次 4 KiB `glBufferData`、中间不画 | `wlivepk ≤ 4 × wbufs`；最后一次画落地 |
| `RespecifyAndDrawEachStoreInOneFrameStaysWithinTheDeferredBudget` | 48 轮「1 MiB respecify + 画一条竖带」 | `wdefpk ≤ 预算 + 1 MiB`、`wlivepk ≤ wbufs + 预算/1 MiB + 2`；48 条带各是自己的颜色（store 在它的 draw 执行前被销毁会在这里现形） |
| `ManySmallRespecifyAndDrawRoundsStayUnderTheStoreCountCeiling` | 3000 轮「256 B respecify + 画」（750 KB，远低于字节预算） | `wlivepk ≤ wbufs + 1024 + 2`；最后一次画落地 |

车道：`DirectVulkan.{Split,Spawn}.Reclaim.*`，标签 `integration-magma-split` + `integration-magma-reclaim` /
`integration-magma-spawn` + `integration-magma-spawn-reclaim`，环境 = strict + 双块 + `MOBILEGL_PIPE_STATS=1`
`_PERIOD=1` + `MOBILEGL_IPC_WIRE_DEFERRED_MB=8` + 私有日志路径。**不注册 tcp 臂**（ID-P7-14 的形状）：tcp 的 server
是整条车道共用的 fixture supervisor 子进程，条目设不了它的环境（给它设 stats + 8 MiB 会改掉其它每条 tcp 条目的
server），也读不到它的日志。`spawn_lane_parity.py` 的 server 端环境例外表 `MAGMA_SERVER_ENV_KNOB_NO_TCP` 加入三条 `.Reclaim.…` tail（集成时并入 B2 的同一机制 `compare_arms(no_tcp=)`，一条 tail 恰对应一个 split 条目；只对 tcp 臂的比较扣除），
`PipeStatsTest` 钉住四个短名。

red-once（R-16，已执行并还原；数字是 server 的 gauge）：

| 场景 / 臂 | 红（修复前） | 绿（修复后） |
|---|---|---|
| 用例 1，split & spawn | 基线 defer 路径（`ec46a550` 的 `DeferRelease` 进帧桶，计数照记）：`wlivepk` **1025 vs 4** | `wlivepk=1`（`wbufs=1`） |
| 用例 2，split & spawn | 同上：`wdefpk` **49 287 168 vs 9 437 184**、`wlivepk` **49 vs 11** | `wdefpk=9 437 184`、`wlivepk=9`、`wdefsync=5` |
| 用例 2，`MOBILEGL_IPC_WIRE_DEFERRED_MB=0`（旋钮负对照，已提交代码） | `wdefpk` **49 287 168 vs 9 437 184**、`wlivepk` **49 vs 11**，两臂 | — |
| 用例 3，split & spawn | `9328bdc7`（只有字节水位线）：`wlivepk` **3001 vs 1027** | `f0356dbe`：`wlivepk=1025`、`wdefsync=2` |
| 修 spawn 透传前（`c569c916` + 本场景） | spawn 臂 `wdefpk=49 287 168`、`wdefsync=0`（server 没拿到 8 MiB） | `0acee60a` 后与 split 臂相同 |

用例 2 的负对照（在 `f0356dbe` 上重跑）里 48 条带仍全部正确——红的是回收，不是画面。

## 5. 主机测量（M1 的采样器与探针；lavapipe；pbuffer；DirectVulkan）

### 5.1 `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854`（854×480）

「前」= `ec46a550` + 探针；「字节线」= `9328bdc7`（只有字节水位线）；「后」= `f0356dbe`。三臂的 ssim 与 mismatch
前 / 字节线 / 后完全相同：ssim **0.998402**、mismatch **306 892**。

| 臂 / 进程 | 峰值 RSS（VmHWM） | maps 峰值 | 结束时活 VkBuffer（其中停放） | VMA 分配 / 字节 | `wbuf[]` |
|---|---|---|---|---|---|
| monolith，前 | 1058.6 MiB | 28 403 | 10 | 64 / 420.7 MB | — |
| monolith，后 | 1056.9 MiB | 28 402 | 10 † | 64 / 420.7 MB † | — |
| inproc，前 | 1174.6 MiB | 41 021 | 25 935（25 903） | 25 990 / 345.5 MB | — |
| inproc，字节线 | 1140.8 MiB | 41 032 | 32（0） | 87 / 283.6 MB | `wlivepk=12497 wdefpk=39536432 wdefsync=0` |
| inproc，后 | **894.2 MiB** | **5 847** | 1 053（1 025） | 1 108 / 235.7 MB | `wlivepk=1052 wdefpk=3312832 wdefsync=23` |
| spawn server，前 | 825.1 MiB | 41 007 | 25 935（25 903） | 25 990 / 345.5 MB | — |
| spawn server，字节线 | 793.8 MiB | 40 993 | 32（0） | 87 / 283.6 MB | `wlivepk=12497 wdefpk=39536432 wdefsync=0` |
| spawn server，后 | **546.3 MiB** | **5 832** | 1 053（1 025） | 1 109 / 236.7 MB | `wlivepk=1052 wdefpk=3312832 wdefsync=23` |
| spawn client，前 / 后 | 359.8 / 358.8 MiB | 69 / 66 | — | — | — |

† monolith 的 VMA 读数取自 `9328bdc7` 测量树上的 monolith 一遍（本包不碰 monolith 臂，`f0356dbe` 只改 wire 路径）。

spawn server 的 RSS / maps 逐 ~10 s 序列（`mem.tsv` 十分位）：

```
前（ec46a550）  t(s)   0    9.6   19.2  29.2  39.9  50.8  61.6  72.5  83.2  93.9  104.7
   RSS MiB            9   465   750   763   774   785   796   807   788   800   825
   maps              58  7160 27361 30324 32589 35019 37429 39828 29574 32799 28301
后（f0356dbe）  t(s)   0    9.6   19.4  29.2  38.8  48.2  57.9  67.6  77.5  87.0  96.4
   RSS MiB           19   239   453   455   458   462   465   468   473   477   546
   maps             200  2296  5783  5329  5197  5204  5051  4738  5686  5429  4664
```

前：进入稳态后 +11 MiB / +2 400 maps 每 10 s、锯齿（drain 偶尔成功时回落），与 M1 的 +12.2 MiB / +2600 每 7.2 s
同形。后：稳态斜率 ~+3 MiB / 10 s、maps 平台在 4.6k–5.8k。最后一个采样的 +70 MiB 在前后都有（前 +25），是回放
收尾（快照 / 回读）的一次性抬升，未归因。探针的活 VkBuffer 序列（后，spawn）：7 → 1 035 → 1 053 → … → 1 053，
停放恒在 1 025 附近，即个数上限在锯齿地收它；前：7 → 4 526 → 7 215 → 19 687 → 25 935。

### 5.2 `minecraft-1.21.4-fabric-iris-bsl-in-world`（轻例）

ssim **0.997324**、mismatch **392 471**，前后三臂相同。

| 臂 / 进程 | 峰值 RSS 前 → 后 | maps 峰值 前 → 后 | VkBuffer |
|---|---|---|---|
| monolith | 821.3 → 811.9 MiB | 995 → 989 | — |
| inproc | 940.2 → 934.7 MiB | 907 → 941 | 前：4 s 采样 86 活 / 68 停放；后：21 活 / 1 停放 |
| spawn server | 477.0 → 471.0 MiB | 897 → 890 | 后 `wbuf[wbufs=26 wlivepk=63 wdefpk=57760 wdefsync=0]` |
| spawn client | 459.7 → 460.2 MiB | 66 → 66 | — |

轻例本来就小（M1：1000× 小于 bsl），修复后峰值 63 个 store、57 KB 停放、零次强制同步——只靠 submit 门控。

### 5.3 M1 的 maps 归因不成立，maps 是 descriptor set

「字节线」那一行是决定性的：活 VkBuffer 从 25 935 降到 32、VMA 分配从 25 990 降到 87，**maps 峰值一点没动**
（41 007 → 40 993）。maps 普查：增长的全是 4096 B 的 `/memfd:allocation fd` 映射（前 spawn 峰值快照 34 653 个，
monolith 27 383 个）；VMA 的块是另外那几条 32–256 MiB 的映射。加一个只在测量树上的计数探针
（`UniformManager::AllocateDescriptorSetsFromActivePool` 成功 +1、`OnDescriptorSetLayoutDestroyed` 减去清掉的）后：

| 采样 | `dsets` | maps |
|---|---|---|
| 字节线 spawn | 5 552 / 23 276 / 27 377 | 6 092 / 24 013 / 28 134 |
| monolith | 14 948 / 23 271 / 27 376 | 15 730 / 24 053 / 28 202 |
| 后 spawn | 1 820 / 3 663 / 3 713（平台） | 2 294 / 4 392 / 4 458 |

**lavapipe 给每个 descriptor set 一个 4 KiB memfd 映射，maps 就是活 descriptor set 数。** M1 的「每个 VkBuffer 一条
memfd 映射」是相关不是因果（两者都随调用数涨）。「后」的 maps 之所以降到 5.8k，是个数上限的 23 次强制同步顺带让
`TryDrainFrameTransients` 成功，而 drain 每次都 `m_uniformManager->BeginFrame(frameIndex)` 回卷 descriptor 游标
（`VulkanRenderer.cpp:13630` 的 `TryDrainFrameTransients`，「Descriptor cursors rewind on every drain」）——**descriptor set 的有界现在
搭着 M2 的同步节奏**，不是它自己的机制（§6.1）。

## 6. 还没做 / 交给集成者

1. **wire 臂的 descriptor set 帧内增长（新发现，最该派包的一项）**。`UniformManager::AcquireDescriptorSet`
   （`UniformManager.cpp:2811`）在每帧每 layout 的缓存游标用尽后分配新 set、池满则 `GrowFrameDescriptorPool`
   （`:2745`）长新池；游标只在 `UniformManager::BeginFrame`（`:628`/`:653`）回卷，而那只在 present 与 drain 上跑——
   一 present 的回放把它饿死，与本包的缺陷同根。monolith 同一回放在 ~27.4k 个 set 处平台（16 s 后不再分配），
   wire 臂持续增长到 ~40k 后被偶发 drain 锯回。在 Adreno 上 descriptor pool 是驱动的堆内存（经 scudo），这恰是
   设备上 `Scudo ERROR: internal map failure` 的另一个嫌疑。M2 的强制同步现在碰巧给了它回卷点；正确的修法属于
   `UniformManager.cpp`（簇 A）/`VulkanRenderer.cpp`（B3/B4）：用「这些 set 的最后一次提交已退休」（与本包同形的
   submit 证明）回收，而不是等帧界。
2. **`CollectGarbage()` / `pruneWire` 在 wire 臂上几乎不跑**（B4）。`VulkanRenderer::SetupDraw`
   （`VulkanRenderer.cpp:7060`）在 `:7067` 的 `m_textureManager->CollectGarbage()` 之前就返回 `SetupWireDraw`；
   compute 的两处（`:7684`/`:7737`）同样在 wire 分支之后。`pruneWire`（`VkTextureManager.cpp:1779`）于是只剩
   `VkTextureManager::BeginFrame` 每第 64 个帧界的 `PruneDeadTextures`（`:729`），一 present 的回放上等于不跑：
   死的 wire texture / renderbuffer 记录与其图像一直留着（bsl 上量不大：`wireTex=48`、`deferredTex=3`）。
   按指示未修（`VulkanRenderer.cpp` 是 B3 的）。
3. **帧 serial 证明**：集成树上的地板（B3 之后）可靠，可以把 `lastUseSerial <= GetCompletedSerial()` 加作第四条
   证明（放掉「上一帧用过、本帧没碰」的 store 而不必等当前批提交）。本包没加：它在帧内不动，对本包针对的
   单 present 回放不增加任何回收（§2.1）。
4. **tcp 臂**：`wbuf[]` 是 server 进程的 gauge，tcp 的 fixture server 够不着——与 `MAGMA_INPROC_ONLY` 的两条同类，
   是「跨进程 peek」那条 §12 债的又一个实例。
5. **范围外的一处改动**：`MG_Remote/Server/ServerSpawn.cpp`（§2.3）不在指派的文件里，但没有它旋钮在 spawn 上是空的；
   请集成者追认，CONTRACT-P6 §3.1 的 S3（清洗被证伪）不受影响。旋钮本身按 `Config.h` 的规矩应经集成者（§3）。
6. **性能只记录**（ID-P7-9）：bsl 一次回放 23 次帧中强制提交；主机上 spawn 回放墙钟 104.8 s（前）/ 96.5 s（后），
   共享主机噪声大，不作结论。
7. **（第二轮，只在 pipe 树上能做）`RespecifyWireBuffer` 把 `lastUseSerial` 清零时也要把 B3 的
   `lastUseSubmitIndex` 清零**（`resource->lastUseSubmitIndex = 0;`，紧跟 `resource->lastUseSerial = 0;`）。审阅指出
   它在 respecify 后残留旧值；这个字段是 B3 在集成树上加的，本分支（`6f67fd7f` 之上）没有它，加了编译不过。
   请集成者在挑入第二轮提交时顺手补这一行——它属于本包的 `RespecifyWireBuffer`，不是 B3 的文件。

## 7. 门（`f0356dbe`）

- **G1**：pull 构建 rc 0，`.text` = **0xa52203**，`nm --defined-only` 对 `~/w7/p7-before/pull-syms.txt` **+0 / −0**
  （`~/w7/logs/m2-g1.sh`）。
- `fatal_census.py` rc 0：**79** 个 abort 站点 / 20 文件，44 家族词，3 拒绝词，0 无标记（不变）。
- `link_ratchet.py --assert-monotone`：**173**，不变。
- `spawn_lane_parity.py build-split` rc 0：gated 三臂 96 / 75 / 72（+2 fixture），inproc-only 21 条与 not-on-tcp 3 条
  按名扣除；informational 206×3；full-suite 536×3。
- 车道（逐条跑，`-j 8`）：`unit` **2429/2429**、`integration-split` **303/303**、`integration-magma-split`
  **96/96**（+3）、`-spawn` **75/75**（+3）、`-tcp` **74/74**、`integration-magma-full-split` **536/536**（+3，
  整二进制重放里新用例因无车道标记而 skip）。
- retrace（主机，`run_trace_case.cmake`，基线树 vs 本树）：OpenRA DirectVulkan monolith / inproc / spawn 三臂
  **ssim 1.000000、mismatch 0**，前后一致；bsl-esc-menu-854 与 iris-bsl-in-world 见 §5（前后逐字相同）。

## 8. 设备验收（集成者在 Redmi 上跑）

`minecraft-1.21.4-fabric-iris-bsl-esc-menu-854`，DirectVulkan，`--use-pbuffer`，同一 reboot-clean 会话：

1. monolith 一遍（对照，W4 读数 0.999791667 / 807 MiB）。
2. **spawn** 三遍、**inproc** 三遍，默认旋钮（64 MiB / 1024）。每遍采 server 与 client 的 `VmHWM`、**`/proc/<pid>/maps`
   行数**（W4 的采样器没记这一项，M1 标为未答），并开 `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1` 取 server
   日志最后一行的 `wbuf[...]`。
3. 判据：不死；门 3 的逐 case 判据（三遍逐位相同、ssim ≥ 阈值且与同会话 monolith 差 ≤ 0.0005）；`wlivepk` ≲ 1 060、
   `wdefsync` > 0。
4. **若 server 仍在 scudo 里死**：先看 maps 行数与 RSS 斜率——主机上剩下的帧内增长是 §6.1 的 descriptor set，
   它在设备上是驱动堆而不是 memfd，那是下一个包，不是本包的回退。
5. 可选对照：`MOBILEGL_IPC_WIRE_DEFERRED_MB=0` 一遍 spawn，应复现 W4 的形状（或更早死），证明旋钮在设备上生效。

## 9. 第二轮：审阅的 must-fix（ID-P7-43）与小项（分支 `p7/magma-m2`，`6f67fd7f` 之上）

### 9.1 缺陷：句柄复用 ABA 打穿 `UniformManager` 的 memo

M2 让 wire store 在**帧中**死（serial-0 停放即毁、扫描前缀、水位线），而 `UniformManager` 的两个 descriptor memo
都按 **VkBuffer 句柄**记：`m_descriptorReuseMemo` 的签名混入 `VkDescriptorBufferInfo` 的字（`UniformManager.cpp`
签名块），`FastRebindMemo` 直接比 `uboBuffer`。它们只在 `UniformManager::BeginFrame`（present / drain）清空——M2 之前
wire store 也只在那同一个边界死，所以配对成立；M2 打破了它。wire 臂上直接绑定的 UBO 描述符就是 store 自己的句柄
（`ResolveWireUniformBufferPayload` 的 `out.buffer = source.buffer`），SSBO 同样。于是：D1 用 store S1（句柄 H）画 →
memo 记下 (H, range) → DS1；`glBufferData` 停放 S1；一次不等待的 flush 提交；下一次停放的扫描销毁 S1；下一次铸造拿回 H
（lavapipe 的句柄是堆指针，glibc tcache 后进先出）；D2 同一程序解析出 (H, range) → memo 命中 → 重绑一个 descriptor
还指着 S1 已释放内存的 set。像素错、无 Fatal。serial-0 路径同理。

### 9.2 修复：一个 destroy epoch

`VkBufferManager` 加 `m_wireStoreDestroyEpoch`（`GetWireStoreDestroyEpoch()`），**每条**销毁 wire store 的路径都 `++`：
`DeferWireRelease` 的即时销毁、`SweepDeferredWireReleases`（每次有销毁的扫描一次）、`DestroyAllDeferredWireReleases`、
`Shutdown` 清 record 持有的 store；与 `m_sliceEpochCounter` 一样**永不复位**。`UniformManager`：签名 `mix64(epoch)`，
`FastRebindMemo` 记录 epoch 并在命中判断里比对——全部 `#if MOBILEGL_BUILD_DISAGGREGATED`。**不用 `m_sliceEpochCounter`**：
它每次 `WriteWireBuffer` 都动，会让 memo 每次 `glBufferSubData` 都失效。wire 臂上 `SetupWireDraw` 不传 sampler hint，
所以 `FastRebindMemo` 在 wire 臂上从不命中——签名那半是 wire 臂真正走到的，`FastRebindMemo` 那半是防御性的。
代价：每次销毁事件之后第一笔 draw 多一次 descriptor set 分配 + 写（bsl 整次回放 ~2.6 万次销毁对 130 万次调用），
顺带把 §6.1 的 descriptor set 增长每事件多推一个 set。`§9` 分区例外（`UniformManager.{h,cpp}`）由集成者授予。

### 9.3 场景与 red-once（规则 J）

`MagmaWireReclaimScenario.ADrawAfterTheEarlyReclaimFollowsTheNewStoreNotTheMemoizedHandle`，Split + Spawn 的 Reclaim
条目（与前三条同一 fixture、同一车道环境；`spawn_lane_parity.py` 按前缀例外，不动）。**每一步都是确定的，没有 sleep、
没有 fence 轮询竞态**：

1. D1 用 UBO store S1（256 B，颜色 A）画——memo 记下 S1 的句柄；
2. `glBufferData(U, 0)`：S1 孤立（停放，标在 D1 所在的待提交批），**不铸新 store**；
3. `glClientWaitSync(fence, FLUSH_COMMANDS, 0)`：把那批**提交而不等**（审阅说的 flush-without-wait）；
4. 对 D1 用过的 vertex store `glBufferSubData` 4 字节：store 忙 → `StagedWireRangeCopy` 录一条拷贝 → 录制又是 pending，
   于是第 5 步的等待**不能**取会清 memo 的帧界 drain（`TryDrainFrameTransients` 见 pending 就拒绝）；
5. `glClientWaitSync(fence, 0, 2 s)`：D1 那批退休，memo 完好；
6. `glBufferData(V, 0)`：vertex store 停放，**这次停放的扫描**发现 S1 那批已完成 → 销毁 S1；
7. `glBufferData(U, 64 KiB, 颜色 B)`：铸造拿回 S1 的句柄值（tcache LIFO）；64 KiB 放不进 S1 那个 256 B 的洞（紧接 S1
   铸的 1 MiB pin store 把洞封住），所以 DS1 指的内存还是 S1 的旧字节——同尺寸铸造会被 VMA best-fit 塞回同一段内存，
   靠别名意外读到 B，那是证明不了任何东西的绿；
8. D2 再画：解析出同一 (句柄, 16 B) → 修复前 memo 命中。断言只有一条：像素必须是 B。

| 臂 | 红（修复前：两处 epoch 判断去掉，`VkBufferManager` 的 epoch 留着不读） | 绿（修复后） |
|---|---|---|
| Split | `read rgba(200,60,30,255), wanted rgba(60,200,90,255)`——读到的正是死 store 的颜色 A | 过 |
| Spawn | 同上 | 过 |

**一个方法学发现**：`MobileGLIntegrationTest` 在桌面上**静态链接** `MobileGL_s`（`ldd` 无 `libMobileGL.so`，`nm` 里
`glDrawArrays` 在二进制内），inproc 臂跑的是编进测试二进制的 backend，只有 spawn 臂跑 `libMobileGL.so`。所以只
`ninja MobileGL MobileGLServer` 的变体到不了 Split 臂——本轮 red-once 第一次跑 Split 就是这样「绿」的。第二轮的
red-once 脚本三个目标都建；并用同样方式把第一轮的 base 变体（`DeferWireRelease` 回到帧桶）重跑了一遍：两臂三条用例的
红与 §4 表逐字相同（`wlivepk` 1025 vs 4；`wdefpk` 49 287 168 vs 9 437 184、`wlivepk` 49 vs 11；`wlivepk` 3001 vs 1027），
用例 4 在那个 base 上两臂都绿（没有帧中销毁就没有 ABA）。探针（只在临时变体上打的 `MGLOG_I`，spawn server 日志）
把机制照了出来：`destroy-sweep handle=0x7a0329f75dd0` → `mint slot=2 handle=0x7a0329f75dd0 size=65536` → `memo-hit
buffer=0x7a0329f75dd0`。

### 9.4 审阅的小项

- `ManySmallRespecifyAndDrawRounds...` 加 `ASSERT_GE(livePeak, 0)`：缺 gauge 读成 −1 会把单边上界当作「没漏」过掉。
- `SweepDeferredWireReleases` 的注释不再说 `GetSyncPointSubmitIndex()`「不减」：录制被丢弃（`RecreateSwapchain`、最小化
  Present 强清录制标志）时它退一；前缀扫描在那之后停在较高的旧 index 上，直到下一次提交占用该 index 并退休——保守、
  不早放，空闲规则照样兜底。
- `VkBufferManager.h` 的「地板不作证明」只留第一条理由（§2.1 同步改）。
- `ServerSpawn.cpp` 的保留集加 `MOBILEGL_IPC_SPIN_US`（两侧都读的 doorbell 自旋预算）、`MOBILEGL_IPC_SERVER_AFFINITY`
  （apply 线程 CPU 掩码）、`MOBILEGL_IPC_AUDIT`（退休 stage 字节 0xDD 填充）：都是调优 / 审计值，不指名端点、角色、路径
  或段尺寸，`ServerMain` 的 catch (b) 仍核验它核验的那四个。前缀下其余的照旧清掉（其中 `STRICT_ERRORS`、`RUN_AHEAD`、
  `VERB_BARRIER`、`BATCH_WAITS`、`PRESENT_CREDIT`、`PERSISTENT_*`、`ADOPT_TIER`、`RESPAWN` 要么是协议两侧要一致的形状、
  要么是 client 读的，超出审阅所列，未动）。
- `RespecifyWireBuffer` 的 `lastUseSubmitIndex = 0`：字段只在 pipe 树上有，见 §6.7。

### 9.5 门（第二轮，本分支 HEAD）

- `ninja -C build-split` 绿；G1：pull 构建 rc 0，`.text` = **0xa52203**，`nm --defined-only` 对 `~/w7/p7-before/pull-syms.txt`
  **+0 / −0**。
- `fatal_census.py` rc 0：**79** 个 abort 站点 / 20 文件，44 家族词，3 拒绝词，0 无标记（不变）。
- `link_ratchet.py --assert-monotone`：**173**，不变。
- `spawn_lane_parity.py build-split` rc 0：gated 三臂 97 / 76 / 72，not-on-tcp 按名 4 条；informational 206×3；full-suite 537×3。
- 车道（逐条，`-j 8`）：`unit` **2429/2429**、`integration-split` **303/303**、`integration-magma-split` **97/97**（+1）、
  `-spawn` **76/76**（+1）、`-tcp` **74/74**（`-j 8` 首跑 73/74：`Tcp.Fm.F1WireScenario.CopyTexSubImage2DPixels` 在会话
  建立前就 `no Welcome from the spawned server within 5000 ms`，任何记录都没发；单条重跑 3/3、整车道重跑 74/74——
  配对窗口的瞬时形状，与本轮改动无关：该车道 `audit=0 spin=50us affinity='auto'` 全是默认值）、
  `integration-magma-full-split` **537/537**（+1，无车道标记 skip）。
- retrace（主机，`run_trace_case.cmake`，本树）：OpenRA DirectVulkan monolith / inproc / spawn **ssim 1.000000、mismatch 0**，
  0 条 unsound。
