# gate8-wire-counters — 门 8 的两个 wire 计数器、首采，与 spawn server 侧的 wait 账

> 头 `94c130d6`（`git rev-parse HEAD`，写报告时读取；工作树另有并行 agent 在修子模块指针，未 commit）。
> 环境：`wsl -d Ubuntu`，clang-20 / Ninja / Release / `MOBILEGL_BUILD_TYPE=Release`，
> 快照 `~/w7/p6-gate8-src`（从 Windows 工作树 copy，见 §1），构建目录 `~/w7/p6-gate8-build`。
> 交付内容：
> 1. `CONTRACT-P6.md` §9 item 8 的三个数里的**后两个**（`SEG_STAGE` 字节/帧、chunking 后记录/帧），
>    外加可选加分项：分块后 `maxrec` 与逐 blob 大小分布（§2–§6）；
> 2. **spawn server 进程的 PipeStats 初始化缺陷及其修法**（§9）——该缺陷使 item 8 的**第一个数**
>    （socket 门铃 vs inproc condvar）在 spawn 臂上只有 client 半边（§9.1.2 把"哪条车道哪半边"
>    逐条查清，并把 client 半边**本来就是有效测量**这件事写明）；
> 3. **`MOBILEGL_PIPE_STATS_FILE` 的 spawn 角色分路**（§9.7），含 G1 上被抓出的三个泄漏点。
> 本报告不改 `MEASUREMENTS.md` / `CURRENT_STAGE_PROGRESS.md` / `ROADMAP.md`；数字供后续合并引用。

---

## 0 一句话

两个计数器都落在**唯一的收口点**上（stage 分配器、记录提交点），因此覆盖是构造性的而不是一张名单；
首采两负载的稳态数：`SEG_STAGE` **130.8 KB/帧（OpenRA 中位数）到 816.2 KB/帧（rd12 in-world 中位数）**、
记录 **211 条/帧到 7,497 条/帧**；含启动加载窗的均值分别高到 8.01 MB/帧与 1.53 MB/帧，
报告把两种读法都列出来（§6.4）。**默认 chunk 预算 8 MiB 在两条负载上都没被触发**
（`maxrec` 恒为 1808），这恰好印证 `MEASUREMENTS.md:342` 说的"分块后没有新测量"——
分块的效果必须人为压小预算才看得到（§6.5），而那一组 A/B 给出了一个清楚的负结果。

**§9 是后加的第二件事，也是这一轮真正的收尾缺口**：spawn 下 server 进程从不调用
`PipeStats::Init()`，于是该进程里所有 `Enabled()` 门恒为 false——它**一条汇总行都没有**，
而缺行的不是 client。修法是 server 角色初始化的唯一收口点加一行 `Init()`（§9.2），
配一行 `Shutdown()`。修后 spawn 的 server 半边首次可测：**`srv` 15.00 waits/帧、`srvpark` 4.38 parks/帧**（§9.4）。

**§9.7 是同一轮的第三个缺口**：两角色现在都会写 `MOBILEGL_PIPE_STATS_FILE`，于是 dump 路径按角色
派生成 `<base>.client.json` / `<base>.server.json`，基名不再指向任何文件（§9.7.1）。
这一条在 G1 上**红了三次**才做对（§9.7.3），每一次都是"guard 了新代码但动了旧代码"。

---

## 1 来源快照与可复现性

构建走的是快照而不是 Windows 工作树，因为同时有两个并行 agent 在改树（一个修子模块指针、
一个加 wire 计数器）。快照脚本 `~/w7/mgs/p6-snapshot.sh` 用 `tar` 排除 `build/`、`.git/`、
`android-plugin/`、`tools/device_bench/`，然后**补回两处被 `*/build` 误伤的东西**：

- `3rdparty/*/build/`（xxHash 的 CMake port 在 `3rdparty/xxHash/build/cmake`，是 submodule 内容不是产物；
  被排除后 configure 直接 `add_subdirectory` 报错）——脚本 `p6-fix-builddirs.sh`；
- `android-plugin/app/src/trace/cpp/`（`tools/trace_replay/CMakeLists.txt` 编译其中三个文件，
  排除 `android-plugin/` 会让 `mobilegl_trace_replay` 目标 "No SOURCES given"）——脚本 `p6-build-retrace.sh` 自动补。

构建配方逐字照 `.github/workflows/test.yml:942-957`（split）与 `:219-227`（pull control），
只把 `clang-20`/`clang++-20` 换成同名可执行文件（Ubuntu 24.04 装了 `clang-20` 1:20.1.2-0ubuntu1~24.04.3）。
`~/w7/p6-gate8-build` 是本次任务书点名的目录，没有触碰任何既有目录。

单元测试与构建是**同一个** split 配置（`MOBILEGL_PIPE_PUSH=ON` + `-DMOBILEGL_BUILD_DISAGGREGATED=ON`
+ `-DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON`），与 CI 的 `build-linux-split` job 同源。

---

## 2 设计取舍

### 2.1 `SEG_STAGE` 字节/帧：ByteClass，不是 Gauge

`ByteClass` 是**窗口化求和**（`AddBytes` → `g_frameBytes`/`g_totalBytes`，`OnPresent` exchange 清零，
`FormatWindowLine` 除以 window 帧数），而 `Gauge` 按设计是**运行总量**（`PublishGauge` 是 store 不是 add，
见 `PipeStats.h` 的 Gauge 枚举长注释）。门 8 要的是"字节/**帧**"，所以必须是 ByteClass——
`FormatWindowLine` 已经替它做了除法，`perFrame` 标注规则也自动适用。

### 2.2 记录/帧：CallClass + 一个额外的 `wrec/f` 字段

`WireRecords` 是 `CallClass`，于是 `wrec=` 跟 `draws=` 一样是**窗口计数**。
但"记录/帧"是交付数字，让操作者自己去除是没必要的，所以汇总行**另印一个 `wrec/f=`**，
与 `draws/f=` 同形。二者按 `perFrame` 规则联动：窗口内没有 Present 时**两个都退回窗口总量**
（`wrec=47 wrec/f=47`），这是该文件既有的标注规则，不是为这个字段新造的——
`PipeStatsTest.SummaryLineSurvivesZeroFrames` 钉住的正是同一个坑（47 倍高估）。

### 2.3 逐 blob 分布：第二个直方图，push-only

`MEASUREMENTS.md:342` 说的是"分块后**逐 blob 字节分布**尚无新测量"，而现有 `maxrec` 是**最大值**、
`seg` 是**平均值**，两个都答不了这个形状问题：8 MiB 的平均值在"一个 8 MiB blob"和"两个本该被切成一个的 4 MiB blob"
下是同一个数。所以另立 `staged-blob-bytes-histogram`，**复用 `PayloadBucketOf` 的桶边界**（bucket 0 = 0 字节，
bucket n>0 = [2^(n-1), 2^n)），使两个直方图读法一致；它是**运行总量**（分布没有"窗口"可言），
经 JSON dump 出口——Tracy 画不了分布，这与既有 `cmd-bytes-per-draw-histogram` 的选择一致。

### 2.4 落点为什么在收口点而不是调用方

任务书要求"覆盖所有写入者，不要在调用方重复计数"。两个落点都满足：

- **stage 字节**：`PipeWireEncoder::StageAllocate`（`PipeWireCodec.cpp:853` 之后）。全树只有一个
  `StageAllocate` 调用点（`StageBytes`，`PipeWireCodec.cpp:936`），而 `StageBytes` 是各 emitter
  （`WireTables.cpp` 的 `StageRequired`/`StageOptional`、`SetProgramBindings` 的逐个名字、
  `WireTables.cpp:549` 的 program archive、`EmitTables.cpp` 的 storage block name）唯一的入口。
  加在调用方就等于维护一张"今天存在哪些 producer"的名单，下一个 producer 漏掉时**没有任何东西会响**——
  这正是 `PipeStats.cpp` 顶部 site inventory 存在的理由。
- **记录数**：`PipeWireEncoder::EncodeRecord`（`PipeWireCodec.cpp:1171` 之后，`Reserve` 成功、
  正文写完、blob 槽诚实性检查之后、`++m_emitSeq` 之前）。放在 emitter 侧**不可能正确**：
  分块之后一次应用调用产生几条记录只有 encoder 知道。同一个函数内部**不重复计数**，因为
  `slot == nullptr` 时提前 `return kInvalidSeq`，重试成功才计一次。

### 2.5 边界：`size` 不是 `need`，pad 不算记录

- stage 字节记 **`size`（生产者给的字节）**，不记 `Align8(size)`：对齐衬垫是分配器的、
  不是生产者写的内容；记 `need` 会让"8 字节 blob 的负载"和"16 字节 blob 的负载"看起来一样。
  分配器的 wrap skip 同理排除。
- 记录数**不含 `kRecPad` 填充**：pad 没有 opcode、两侧都跳过、已经由 `ringpads` gauge 可见。
  不含被 `Reserve` 拒绝的那次发射，正是 2.4 里那个提前 return 保证的。

### 2.6 push-only（G1）

两个计数器都在 `#if MOBILEGL_PIPE_PUSH` 下，与该文件每个后来加的计数器一致。
`StageSegmentBytes` 在 `ByteClass` 枚举的 push 块内（该枚举的 push 块是 P4a 开的先例），
`WireRecords` 在 `CallClass` 枚举的 push 块内，`RecordStagedBlobBytes`/`TotalStagedBlobBucket`/
`g_totalStagedBlobBuckets`/JSON 行同样。理由：唯一采样者是 wire encoder，pull build 不编译 MG_Remote，
这些符号在那里永远采不到样本。

### 2.7 现有计数器有没有能直接回答这两个数的

没有，都已核对：

| 候选 | 为什么答不了 |
|---|---|
| `stage-buffer`/`stage-texture`（ByteClass） | 是**后端**在 apply 线程往**驱动**搬的字节；分块后一条记录的多个 blob 会分别被 server 应用，所以它们与 wire 侧字节不是一回事，且无法回答"发射侧写了多少" |
| `cso-blob-bytes` | 只覆盖 CSO blob（`WireTables.cpp:540`），不含 buffer 走查与纹理 slab |
| `ClientTextureUploadEmissions`/`framebuffer/sampler-*` | 是**发射调用**数，不是记录数；分块后一次调用可出多条记录 |
| `maxrec` gauge | 单条最大记录字节，不是每帧字节，也不是记录条数 |
| `vbs` | 服务端 verb 边界戳，与发射记录数不同（rd12 上是 1478 vs 7493） |
| `cmd-bytes-per-draw-histogram` | 是 **draw payload** 分布，不是 stage blob 分布 |

所以是**两个新计数器 + 一个可选直方图**，没有重复计数。

---

## 3 计数落点（file:line）

行号为改后内容。

| 计数器 | 文件:行 | 说明 |
|---|---|---|
| `ByteClass::StageSegmentBytes` 枚举成员 | `MobileGL/MG_Util/Metrics/PipeStats.h:115` | push 块内，在 `CsoBlobBytes` 之后 |
| 名称表 `"stage-segment-bytes"` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:228` | JSON key 与 `NameOf` |
| 短名 `"seg"` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:263` | 摘要行 `bytes/f[...]` 括号内 |
| `CallClass::WireRecords` 枚举成员 | `MobileGL/MG_Util/Metrics/PipeStats.h:251` | push 块内，在 `ServerVerbBoundaries` 之后 |
| 名称表 `"wire-records"` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:238` | |
| 直方图声明 `RecordStagedBlobBytes`/`TotalStagedBlobBucket` | `MobileGL/MG_Util/Metrics/PipeStats.h:382-386` | push-only |
| 直方图存储 | `MobileGL/MG_Util/Metrics/PipeStats.cpp:169` | 复位在 `ResetCounters` 内 |
| 直方图采样实现 `RecordStagedBlobBytes` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:403` | |
| 直方图读取 `TotalStagedBlobBucket` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:464` | |
| 直方图 JSON 行 `staged-blob-bytes-histogram` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:708` | 与 `cmd-bytes-per-draw-histogram` 同级，**在其之后** |
| **stage 字节与逐 blob 采样（唯一落点）** | `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:878`（`StageAllocate` 内） | `AddBytes(StageSegmentBytes, size)` + `RecordStagedBlobBytes(size)` |
| **记录数（唯一落点）** | `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1188`（`EncodeRecord` 成功路径） | `AddCalls(WireRecords, 1)` |
| `#include <MG_Util/Metrics/PipeStats.h>` | `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:44` | 新增，MG_Remote 已有的跨层 include 先例（`WireTables.cpp:50`） |
| 摘要行新字段 `wrec=` | `MobileGL/MG_Util/Metrics/PipeStats.cpp:581-586` | `wrec=` + `wrec/f=`，`perFrame` 联动 |

**额外改动的文件：无超过任务书授权范围的文件。** 授权清单里的 `EmitTables.cpp` **没有改**
（`git diff` 显示与 HEAD 逐字节相同）：Gauge 发布点本来就在那里且已覆盖 `maxrec`，`seg`/`wrec`
落在 `PipeWireCodec.cpp` 更准确，所以没有理由动它。任务书允许的"极小 Wire/ 或 Client/ 内部改动"
最终只需要 `PipeWireCodec.cpp` 一个 include 加两处计数（后者本就在授权清单内）。

---

## 4 单元测试

新增 8 个用例（`MobileGL/MG_Test/Util/PipeStatsTest.cpp`）；前 6 个属于 §2–§6 的两个计数器，
后 2 个属于 §9 的 spawn server 缺陷：

| 用例 | 钉什么 |
|---|---|
| `StageSegmentBytesIsAWindowedByteClassLikeAnyOther` | 累加、Present 清帧、`seg=6144.00`、空窗口退回 `bytes[...]` 总量标签 |
| `WireRecordsCarryBothTheWindowCountAndItsPerFrameForm` | `wrec=26` + `wrec/f=2.00`（26/13 帧） |
| `WireRecordsFallBackToTheWindowTotalWithoutAFrame` | 无 Present 时 `wrec/f=47` 而非 `47.00`（既有的 47 倍高估坑） |
| `StagedBlobHistogramSharesThePayloadBucketEdges` | 桶边界与饱和行为与 draw payload 直方图一致 |
| `StagedBlobHistogramIsARunTotalAndReachesTheJsonDump` | Present 不清分布；JSON 两个数组的顺序关系 |
| `CounterNamesAreStable` / `SummaryLineCarriesEveryClassAndGate`（扩写） | 长名 `stage-segment-bytes`/`wire-records`、短名 `seg=`/`wrec=`/`wrec/f=` 的稳定性（改名会破坏所有已记录基线） |
| `InitIsTheOnlyLatchAndItTakesTheConfigBothWays`（§9） | `Init()` 必须**两个方向**都从配置取值：true 时武装、false 时**清掉**旧 latch。单向版本会通过单边测试，却让 `MOBILEGL_PIPE_STATS=0` 在长生命周期进程里失去意义 |
| `PresentationsAtTheDefaultPeriodOfOneMakeOneWindowEach`（§9） | k 次 present → k 个窗口（车道费率所除的分母）；并钉住 gauge 是运行总量、**不是求和** |

结果（§9 修完之后）：

```
$ cd ~/w7/p6-gate8-build && ctest -L unit -R "PipeStats" --output-on-failure
100% tests passed, 0 tests failed out of 24
```

全量 unit 与 Wire 层回归（同一 split 构建）：

```
$ ctest -L unit --no-tests=error -j 24
100% tests passed, 0 tests failed out of 2366     (80.64 s)     # 22 → 24 是本轮新增的两个

$ ctest -L unit -R "Wire|Codec|Ring|Staged|ServerLoop|RemoteClient|Session" --no-tests=error -j 16
100% tests passed, 0 tests failed out of 295      (36.95 s)
```

**一处需要说明的非失败项**：首次跑 `ctest -L unit` 时有 1 个红 ——
`PipeCatalogue.FrontendNeverTakesAnApplierAddress`，报
`frontend source unavailable: "/MobileGL/MG_Impl"`。这是**构建目录在源码树之外**造成的：
该用例从 CWD 向上找 `MobileGL/MG_Impl` 做源码文本走查，而 CI 是 in-tree build
（`cmake -S . -B build-linux`）所以能找到。验证：把 `/home/swung/w7/MobileGL` 做符号链接指回快照后，
该用例 34/34 全绿，全量 2366/2366 全绿。**与本次改动无关**，但在离树构建下会红，记录在此。

---

## 5 G1：pull 构建符号集与 `.text`

协议 §9 item 1：pull build 必须 0/0/0/0 且 `.text` 不变。做法是**两棵同源树、只翻三个 MGPipe 选项**：

- `~/w7/p6-gate8-g1-baseline`：快照 + 本次改的 6 个文件**回退到 `git HEAD` 内容**（从工作树 `git show` 取出），pull 选项；
- `~/w7/p6-gate8-g1-head`：快照 + 本次内容，**同一套 pull 选项**。

脚本 `~/w7/mgs/p6-g1.sh`（configure 与 build 都 `rc=0`），复核脚本 `~/w7/mgs/p6-g1-verify.sh`。
两边 `.so` 磁盘字节数相同（18,506,968）。

```
$ python3 scripts/symbol_report.py --before ...g1-baseline.so --after ...g1-head.so \
      --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --fail-on-text-delta
symbol-report: before: .../p6-gate8-g1-baseline.so (18506968 bytes on disk)
symbol-report: after : .../p6-gate8-g1-head.so (18506968 bytes on disk)
symbol-report: .text 10158850 -> 10158850 (+0, +0.000%)
symbol-report: .data 77864 -> 77864 (+0)
symbol-report: .bss 1296856 -> 1296856 (+0)
symbol-report: .rodata 1521854 -> 1521854 (+0)
symbol-report: Total 16585193 -> 16585193 (+0)
symbol-report: 27863 -> 27863 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27122 -> 27122 normalised names, 27122 unchanged (name, size and mangling all identical)
G1 rc=0
```

`.text` 段直接逐字节比对（`objcopy -O binary --only-section=.text` + `cmp`）：
**`b.text` 与 `h.text` 各 10,158,850 字节，`cmp` 无差异 → `.text` BYTE-IDENTICAL**（强于 gate 要求的 +0）。

补充两条该门惯用的正向断言：

```
$ nm --defined-only ...g1-head.so | grep -ci "MG_Remote"      → 0        （pull build 无 MG_Remote）
$ nm --defined-only ...g1-head.so | grep -ciE \
      "RecordStagedBlobBytes|StageSegmentBytes|WireRecords"    → 0        （新计数器 push-only，未泄漏进 pull）
$ nm --defined-only ...g1-head.so | wc -l                      → 30601
```

（`.so` 的 sha256 两侧不同，符合预期：`-g` 的调试信息带源码路径与时间戳；
门 1 要的是**符号集与 `.text`**，两者都已证明不变。）

---

## 6 首采：真实 trace 的 inproc retrace

### 6.1 方法

`MOBILEGL_PIPE_STATS=1` + `MOBILEGL_PIPE_STATS_PERIOD=1`（每个 `eglSwapBuffers` 一行，
所以每行 `window=1` 恰好一帧），车道 = `MOBILEGL_TRANSPORT=inproc` +
`MOBILEGL_IPC_ROLE_SPLIT_STATE=1` + `MOBILEGL_IPC_STRICT_ERRORS=1` + `MOBILEGL_IPC_RUN_AHEAD=1`，
后端 `DirectGLES`，库/服务端 `~/w7/p6-gate8-build/lib{MobileGL,MobileGLServer}.so`。

两负载：

1. **OpenRA**（`MobileGLTraceReplay.OpenRA.DirectGLES.SPLIT`，ctest 条目原样跑过：Passed 1.67 s，
   SSIM **1.000000**，mismatch 0）；
2. **minecraft-1.21.4-rd12-odinlite-in-world**（任务书推荐的负重载；该 case 在 `trace_cases.json`
   里没有 `"split": true`，所以没有 SPLIT ctest 条目，改为按 `run_trace_case.cmake` 的同一命令行手工驱动；
   SSIM **0.999998**，17226 mismatch 像素，阈值 0.99 下通过）。

**汇总行落在哪个 role 日志**：inproc 下 PipeStats 是一个进程里的一套计数器，
`OnPresent` 由**服务端 apply 线程**调用，所以 250 行摘要全在 `mobilegl.server.log`；
`mobilegl.client.log` 只有一行 `counters ON` 横幅。**两者不重复**，脚本读两个文件也不会双计
（已实测：client 1 行、server 250 行）。

**统计口径**：`PERIOD=1` 下每行 window=1，故
"窗口字段求和 / 帧数" = 运行平均每帧；**gauge 字段是运行总量，取最后一行，不能求和**
（本报告脚本第一版就是求和，得出 `maxrec=452000` 而真实最大值是 1808，已改正——
这正是 `PipeStats.h` Gauge 注释警告的那类错误）。

### 6.2 原始 stats 行（摘录）

rd12 in-world，第 250 帧（末行，`window=1`）：

```
MGPipe stats: frames=250 window=1 draws=1472 draws/f=1472.00 acc=5958 acc/draw=4.05
bytes/f[buf=10320.00 tex=950528.00 ubog=281616.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00
pmap=0.00 resid=184.00 csob-blob=281808.00 seg=2914784.00]
tex[emit=2 box=2 rect=0 jobs=2] cso[csom=0 csob=20 mpr=0]
emit[fbe=8 sve=13 sse=0 sie=0 ctu=3 trp=0 rsp=0 vbs=1479 wrec=7532 wrec/f=7532.00
maxrec=1808 maxcap=4194304 ringwraps=61 ringpads=53 ringwaits=0]
wait[srv=115580 srvpark=1911 cli=30393 clipark=10580]
gates[ers=1458/21 etl=1466/13 eub=1479/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

OpenRA，第 29 帧（末行）：

```
MGPipe stats: frames=29 window=1 draws=30 draws/f=30.00 acc=186 acc/draw=6.20
bytes/f[buf=86184.00 tex=32768.00 ubog=112.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00
pmap=0.00 resid=248.00 csob-blob=112.00 seg=129552.00]
tex[emit=1 box=1 rect=0 jobs=1] cso[csom=0 csob=31 mpr=0]
emit[fbe=0 sve=27 sse=0 sie=0 ctu=1 trp=0 rsp=0 vbs=31 wrec=211 wrec/f=211.00
maxrec=1808 maxcap=4194304 ringwraps=0 ringpads=0 ringwaits=0]
wait[srv=482 srvpark=104 cli=267 clipark=87]
gates[ers=0/31 etl=4/27 eub=31/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### 6.3 全跑聚合（含加载窗；稳态口径见 6.4）

```
################ OpenRA inproc split, DirectGLES ################
summary lines        : 29    windows with a frame : 29    frames covered : 29

field            window total        per frame
seg                 233979394        8068254.97     <- SEG_STAGE 字节/帧
wrec                     5706            196.76     <- 记录/帧（分块后）
buf                  23728752         818232.83
tex                 151814144        5234970.48
csob-blob               30398           1048.21
draws                     758             26.14

gauges - RUN TOTALS
maxrec 1808    maxcap 4194304    ringwraps 0    ringpads 0    ringwaits 0

################ rd12-odinlite-in-world inproc split, DirectGLES ################
summary lines        : 250   windows with a frame : 250   frames covered : 250

field            window total        per frame
seg                2698026763       10792107.05     <- SEG_STAGE 字节/帧
wrec                  6922378          27689.51     <- 记录/帧（分块后）
buf                  95916960         383667.84
tex                 641567296        2566269.18
csob-blob           257066831        1028267.32
draws                 1348027           5392.11

gauges - RUN TOTALS
maxrec 1808    maxcap 4194304    ringwraps 61    ringpads 53    ringwaits 0
```

### 6.4 门 8 两个数的答案

**先说一件必须说清楚的事：全跑"每帧平均"被第一帧的加载窗严重拉高。**
`PERIOD=1` 下第一行 `frames=1 window=1` 覆盖的是**整个启动/加载**（rd12 那一帧有 981,654 draws、
5,053,895 条记录、`seg=2,317,939,359`），而其余每帧只有约 1,471 draws。所以直接"窗口求和 / 帧数"
得到的是**含加载的均值**，不是稳态值。两种读法都列出来，选用取决于问题：

| | OpenRA | rd12 in-world |
|---|---:|---:|
| **含加载的均值** `seg` | 8,068,255 B/帧 | 10,792,107 B/帧 |
| **含加载的均值** `wrec` | 196.8 条/帧 | 27,689.5 条/帧 |
| **稳态均值**（丢弃前 10% 窗口）`seg` | 8,012,890 B/帧 | 1,534,555 B/帧 |
| **稳态均值**（丢弃前 10% 窗口）`wrec` | 206 条/帧 | 7,504 条/帧 |
| **稳态中位数** `seg` | 130,848 B/帧 | 816,248 B/帧 |
| **稳态中位数** `wrec` | 211 条/帧 | 7,497 条/帧 |
| 参考：`draws` 稳态中位数 | 30 | 1,471 |
| 参考：`maxrec` / `maxcap` | 1808 B / 4,194,304 B（0.043%） | 1808 B / 4,194,304 B（0.043%） |

**门 8 要的数**（按 `MEASUREMENTS.md` 里同类数字的惯例，报稳态中位数与稳态均值这对，
因为该文件自己就警告过"变化的负载用运行总量平均值会掩盖 P2 要的那个数"）：

| 数 | OpenRA | rd12 in-world |
|---|---:|---:|
| **`SEG_STAGE` 字节/帧** | **8.01 MB/帧（稳态均值）** / **130.8 KB/帧（中位数）** | **1.53 MB/帧（稳态均值）** / **816.2 KB/帧（中位数）** |
| **记录/帧（分块后）** | **206 条/帧（稳态均值）** / **211（中位数）** | **7,504 条/帧（稳态均值）** / **7,497（中位数）** |

读法注意：

- **两负载的稳态形状完全不同**，这正是必须分开报的理由：OpenRA 稳态的 `tex` 中位数是 32,768 B
  而均值 5.6 MB（少数窗有巨大的纹理上传尖峰），`seg` 的中位数/均值因此差 61 倍；
  rd12 稳态的 `tex` 中位数为 0（大部分帧不上传纹理），`seg` 的中位数/均值差 1.9 倍。
  报一个数就会掩盖另一个负载的形状。
- rd12 稳态 `seg` 中位数 816 KB/帧 对默认 32 MiB 的 SEG_STAGE：**约 40 帧的余量**，
  与 `ringwaits=0`（250 帧里一次都没等过 retiredSeq）、`ringwraps=61` 一致。
- **`maxrec=1808` 说明默认配置下分块一次都没触发**：稳态 `seg/wrec ≈ 816248/7497 ≈ 109 B/条`，
  与全局的 390 B/条 都在字节级别。这与 `MEASUREMENTS.md:342` 记录的
  "分块前 `maxrec` 也是 1808 / `SetVertexAttribDefaults`" **完全一致**——
  也就是说，**在这两条负载上分块前后 `maxrec` 没有变化，因为它本来就没到过预算**。
- `wrec` 是**窗口计数**（与 `wrec/f` 同值，因为每窗恰好一帧），所以上表两列是同一个数的两种提法。

### 6.5 逐 blob 大小分布（可选加分项）

**结论：在默认预算下不存在"由分块产生的"分布**——见 6.4，`maxrec=1808` 远低于 8 MiB 预算，
所有 blob 都是原始尺寸。为让这个可选数有内容，做了两组采集：

**(a) 默认 32 MiB 预算（=chunk 8 MiB），rd12，分布来自 `MOBILEGL_PIPE_STATS_FILE` JSON dump**
（需要走 `MobileGL::Destroy()`，retrace 到不了；改用 integration 车道：

```
$ cd ~/w7/p6-gate8-build && MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_TRANSPORT=inproc \
    MOBILEGL_IPC_SERVER_PATH=.../libMobileGLServer.so MOBILEGL_IPC_ROLE_SPLIT_STATE=1 \
    MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_RUN_AHEAD=1 \
    MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 \
    MOBILEGL_PIPE_STATS_FILE=.../stats-igpu-inproc.json \
    ctest -R "^DirectGLES\.(Arena|Ssbo|Buffer|Atomic|Readback)" -j 1
100% tests passed, 0 tests failed out of 19
```

`stats-igpu-inproc.json`：

```
frames              : 0            （这些 integration 用例跑 swap 但不走 Present 计数）
stage-segment-bytes : 5768
wire-records        : 70
hist [0,0,0, 4, 1, 2, 0, 1, 0, 1, 1, 1, 1, 0, ...]
       bucket 3 [4,8)       4 条
       bucket 4 [8,16)      1 条
       bucket 5 [16,32)     2 条
       bucket 7 [64,128)    1 条
       bucket 9 [256,512)   1 条
       bucket 10 [512,1024) 1 条
       bucket 11 [1024,2048) 1 条
       bucket 12 [2048,4096) 1 条
```

**(b) 人为压小 stage 预算是让分块真正发生的唯一办法**（`MGPipeStageChunkBytes()` =
`clamp(segment/4, 4096, segment)`，所以 `MOBILEGL_IPC_STAGE_MB=1` → chunk 预算 256 KiB）。
同一条 rd12 trace、同一个库、只改这一个环境变量的 A/B：

```bash
bash ~/w7/mgs/p6-chunkab.sh 32   # 默认：chunk 8 MiB
bash ~/w7/mgs/p6-chunkab.sh 1    # 强制：chunk 256 KiB
```

| rd12 in-world，inproc split | STAGE_MB=32（chunk 8 MiB） | STAGE_MB=1（chunk 256 KiB） | 变化 |
|---|---:|---:|---|
| SSIM | 0.999998213 | 0.999998213 | 不变 |
| **`seg` 运行总量** | **2,698,026,763** | **2,698,026,763** | **逐字节完全一致** |
| `seg` 稳态中位数 | 816,248 | 554,104 | **−32%**（逐窗归属变了，见下） |
| `wrec` 运行总量 | 6,922,378 | 6,929,736 | +7,358（**+0.11%**） |
| `wrec` 稳态中位数/均值 | 7,497 / 7,504 | 7,497 / 7,508 | +0.05% |
| `tex` 运行总量 | 641,567,296 | 1,886,392,128 | **+194%** |
| `buf`/`csob-blob`/`draws` | 见 §6.3 | 与 STAGE_MB=32 逐项相同 | 不变 |
| `maxrec` / `maxcap` / `ringwraps` / `ringpads` | 1808 / 4,194,304 / 61 / 53 | 同左 | 不变 |

这一组是**它自己那个问题的负结果，如实记录**：

- **`seg` 运行总量两侧逐字节相同**（2,698,026,763）说明：这个计数器测的是**生产者交出来的内容**，
  与发射侧怎么切、arena 开多大**完全无关**。这正是 §2.4 落点选择的直接证据——
  若把它记在 emitter 侧或记成 arena 占用，这里就会跟着预算动。
- **但稳态中位数从 816,248 变成 554,104（−32%），而运行总量不变**——这两件事一起有意义：
  同样的总字节被**重新分到不同的窗口**（预算小了，某些原本一次完成的上传被切成多次、跨帧重传），
  所以**逐窗归属会动，总量不会**。读数时应以运行总量或明确声明的窗口口径比较，
  不要拿两套配置的单窗中位数直接对比。
- **`wrec` 只涨 0.11%**（+7,358 条 / 250 帧 ≈ +29 条/帧，稳态中位数 7,497 两侧相同）说明
  **这条 trace 的内容本来就没有超过 256 KiB 的 blob**，所以即使预算压到 256 KiB 也几乎无可切。
  `maxrec` 仍是 1808 第三次确认（`maxrec` 是单条**记录**的字节上界，blob 被切不会改变它）。
- **`tex` 涨 194% 才是这次 A/B 真正的发现**：256 KiB 预算下纹理上传走了近三倍字节。
  `MOBILEGL_IPC_STAGE_MB` 同时是 **arena 大小**，压小它改变了纹理 slab 的切分与重传行为
  （整宽 slab 被预算切碎后每片各自重传）。这一项**不在门 8 的定义里**，
  且**我没有把它隔离到单变量**（arena 大小与 chunk 预算被同一个环境变量绑在一起），
  因此**只作为 A/B 的可解释性证据记录，不作结论**；要分开这两个效应需要分别可配，
  那是另一个包的事。

> 因此门 8 的"chunking 后记录/帧"数取**默认配置**的 §6.4 值；
> "逐 blob 分布"的答案是：**默认配置下该分布就是未分块的原始分布**
> （`maxrec=1808`，稳态 `seg/wrec ≈ 109 B/条`），分块在 OpenRA 与 rd12 这两条负载上都没有被触发。

### 6.6 交叉核对：`seg` 与后端侧字节类

`seg` 记 wire 写进 SEG_STAGE 的字节，`buf`/`tex`/`csob-blob` 记**后端往驱动搬**的字节——
两个总体不同，不要求相等，但应该在**同一量级**并且**同步起伏**；任一条不成立就说明 `seg`
要么重复计数、要么漏了一个总体。逐窗比对（250 窗，`p6-crosscheck.py`）：

```
windows: 250
seg   median     818,030   mean 10,792,107
parts median     291,840   mean  3,978,204      (parts = buf + tex + csob-blob)
ratio mean seg/parts: 2.713
bytes per record (seg/wrec): 389.8
```

- **同步性**：逐窗比值在稳态下稳定在 1.0–8.2 之间（STAGE_MB=32）/ 1.2 平均（STAGE_MB=1），
  且两者在同一些窗一起抬升（第 0 窗两者都是峰值，第 124/155 窗两者都是谷值）。
  没有出现"一个动另一个不动"的窗。
- **差异有解释**：`seg` 稳态恒有 `csob-blob ≈ 281 KB/帧`（program archive，**只走 wire、
  不经后端字节类**——`WireTables.cpp:540` 把它记进 `csob-blob` 是同一个量在发射侧的照面），
  加上 map-persistent 推送与 tail 字节；`parts` 一侧的 `tex` 中位数是 0。
  所以比值 > 1 而不是 = 1 是**两个总体本来就不同**的结果，不是漏计。
- **比例随预算变化也是可解释的**：STAGE_MB=1 时 `parts` 的均值从 3,978,204 涨到 8,957,504
  （§6.5 的 `tex` +194%），而 `seg` 均值不动，所以比值从 2.713 掉到 1.205。
  两个数一起动、且动的方向就是 A/B 那一行预测的方向，这本身就是一次一致性验证。
- 这一核对**不能证明覆盖完整**（没有第三方真值），它能排除的是"`seg` 明显偏离同一批字节"这一整类错误。
  `maxrec`/`ringwaits` 与 §6.5 的 A/B 是另外两个独立的佐证。

---

## 7 已知问题与未做项（如实记录）

1. **retrace 的 snapshot 模式拿不到 JSON dump；benchmark 模式拿得到**。`MOBILEGL_PIPE_STATS_FILE`
   的 dump 在 `PipeStats::Shutdown()` 里，而它只由 `MobileGL::Destroy()`
   （`MG_Impl/EGLImpl/EGLImpl.cpp:336` 的 `eglTerminate` 触发）调用。snapshot 模式在快照完成后
   `exit(0)`，被 `-Dexit=mobilegl_apitrace_exit` 换成一次抛出，**跳过了 `retrace::cleanUp()`**，
   所以 Destroy 不跑；benchmark 模式正常走完 `cleanUp()`，Destroy 跑。
   完整机制与实测见 **§9.1.2**（那里是为了判定"设备那条 client 汇总行是谁打的"查清的，
   结论是同一个机制）。因此 §6.5 的逐 blob 分布仍然只能用 **benchmark 模式或 integration 车道**取
   （`stats-igpu-inproc.json` 是后者）；本报告 §6.5 分布用的是 integration 车道。
   把 snapshot 模式也接上需要改 runner 的退出路径，超出本任务授权的文件范围。
2. **`maxrec` 在两条负载上都是 1808，与 `MEASUREMENTS.md:342` 分块前的记录相同**：
   这不是"分块没生效"，而是**分块没有机会生效**（无 blob 超过预算）。任务书要求记录的"分块后数值"因此
   就是 1808，而它之所以等于分块前的值，原因已由 §6.5(b) 的 A/B 独立证明。
3. **`MOBILEGL_PIPE_STATS_PERIOD=1` 本身有可观开销**（每帧一行 INFO），所以 §6.4 的帧数/负载
   **不能当作性能数**，只用于按帧摊平字节与记录。门 8 也只要求记录，不要求这些数是性能基线。
4. **`seg` 的"每帧"是窗口量，不是硬件意义上的稳态帧时**。§6.4 因此同时给中位数与稳态均值，
   并把含加载窗的均值也列出来：rd12 的加载窗把它抬到稳态均值的约 7 倍（10.79 vs 1.53 MB/帧），
   OpenRA 两个值接近（8.07 vs 8.01 MB/帧）是因为它的加载窗与稳态窗同量级。
   引用这个数时必须说明选的是哪一个口径。
5. **离树构建下 `PipeCatalogue.FrontendNeverTakesAnApplierAddress` 会红**（§4 末），
   与本次改动无关，但任何用 `-B` 指到源码树外的构建都会碰到。
6. **`MOBILEGL_IPC_STAGE_MB` 同时是 arena 大小与 chunk 预算的来源**（§6.5(b) 的 `tex` +194%），
   所以**分块的净效果没有被单变量隔离**。本报告只报默认配置的数，并把 A/B 标为可解释性证据。
   要真正量化分块的收益/代价，需要把两者拆成可独立配置的量。
8. **integration-split 全车道未跑**（时间预算）。门 8 只需要一条真实采集，本报告给了两条 +
   一条 A/B。要做更宽的确认，可用 §6.5(a) 的同一条命令换 `-R` 选择器。
   （§9 那一轮跑了 `.Spawn.` 前缀的 102 条集成条目，全绿。）
9. **未改** `MEASUREMENTS.md` / `CURRENT_STAGE_PROGRESS.md` / `ROADMAP.md` / `Doorbell.*` /
   `protocol.fbs` / `pin_device.sh`（任务书禁止）；未 `git commit`。
   *注：`tools/device_bench/pin_device.sh` 在工作树里确有改动，但那是并行的门 7 设备 agent
   加的 Redmi `2f7cbe2e` 行，**不是本轮改的**；本轮的 `git diff --name-only` 只含 §3/§9.2 的
   5 个 `MobileGL/` 文件。*
10. **门 8 的第一个数（socket 门铃 vs inproc condvar）在本轮之后才算"两边都能采"**，
    但**真机数仍未采**（§9.6）：§9.4 的对比来自 `wsl -d Ubuntu` + llvmpipe。
    本轮交付的是"该数在 spawn 臂上可采"这个前提，不是设备结论。
    门 7（真机 `2f7cbe2e` reboot-clean 配对 A/B）仍属另一个 agent 的范围。

---

## 8 复现命令清单

```bash
# 0) 快照（含被 */build 误伤的两处补回）
bash ~/w7/mgs/p6-snapshot.sh && bash ~/w7/mgs/p6-fix-builddirs.sh

# 1) split 构建（= CI build-linux-split 的配方）
bash ~/w7/mgs/p6-cfg-split.sh
cmake --build ~/w7/p6-gate8-build --parallel "$(nproc)"

# 2) 新增单测 + 全量回归
cd ~/w7/p6-gate8-build
ctest -L unit -R PipeStats --output-on-failure          # 22/22
ctest -L unit --no-tests=error -j 24                    # 2364/2364

# 3) retrace 车道（runner 单独一棵树，库指到 split 构建的产物）
bash ~/w7/mgs/p6-build-retrace.sh
bash ~/w7/mgs/p6-stats-run.sh OpenRA DirectGLES 1
bash ~/w7/mgs/p6-stats-rd12.sh
bash ~/w7/mgs/p6-agg-all.sh                             # 聚合（含加载窗的均值）
bash ~/w7/mgs/p6-quant-all.sh                           # 分位数 + 稳态均值/中位数
python3 ~/w7/mgs/p6-crosscheck.py <server.log>          # seg 与后端字节类的逐窗交叉核对
python3 ~/w7/mgs/p6-quantiles.py <server.log>

# 4) 分块 A/B
bash ~/w7/mgs/p6-chunkab.sh 32 /home/swung/w7/mgs/chunkab
bash ~/w7/mgs/p6-chunkab.sh 1  /home/swung/w7/mgs/chunkab

# 5) G1
bash ~/w7/mgs/p6-g1.sh
python3 scripts/symbol_report.py \
  --before ~/w7/p6-gate8-g1-baseline.so --after ~/w7/p6-gate8-g1-head.so \
  --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --fail-on-text-delta

# 6) §9 spawn server 侧：单测 red-once、端到端 red-once、两传输 paired A/B
bash ~/w7/mgs/p6-redonce.sh          # 破坏 latch -> 单测红 -> 恢复
bash ~/w7/mgs/p6-redonce-e2e.sh      # 修前 0 行 / 修后 29 行，同一条 spawn trace
bash ~/w7/mgs/p6-ab-transport.sh     # inproc vs spawn，同一条 OpenRA trace
bash ~/w7/mgs/p6-waitrate-all.sh     # 逐 role 的 waits/parks 与 per-frame 费率

# 7) §9.1.2 车道模式判据 + §9.7 dump 角色分路
bash ~/w7/mgs/p6-mode-probe.sh       # snapshot: dump 缺 / bench: dump 在（= Destroy 是否跑）
bash ~/w7/mgs/p6-lineprovenance.sh   # 每条汇总行由哪个 role、哪种行打出
bash ~/w7/mgs/p6-dumppath.sh         # <base>.client.json / .server.json，基名不存在
bash ~/w7/mgs/p6-redonce-dump.sh     # 回退派生成裸基名 -> 单测红 -> 恢复
```

证据留存：`~/w7/mgs/`（`build-split.log`、`cfg-split.log`、`g1.log`、`g1-final.log`、`g1-step2.log`、
`build-retrace*.log`、`stats-OpenRA-DirectGLES/`、`stats-rd12-DirectGLES/`、`stats-igpu-inproc.json`、
`stats-itest.json`、`chunkab/stage{32,1}/`、`ab8-{inproc,spawn}/`、`spawn-{baseline,fixed,prefix,postfix}-*/`、
以及 `p6-*.sh` / `p6-{aggregate,quantiles,crosscheck,waitrate}.py` 全部脚本与解析器）；
G1 的两个 `.so` 在 `~/w7/p6-gate8-{g1-baseline,g1-head}.so`。
Windows 侧脚本副本 `C:/Users/geekerwan/wsl-tmp/`（含 `pristine/` 与 `g1head/` 两个文件集，
它们是 G1 两棵树各自的输入）。

---

## 9 spawn server 进程的 PipeStats 缺陷与修法

这一节是门 8 收尾的最后一个缺口。它不属于"加一个计数器"，而属于"让已有的计数器在 spawn 臂上
**可能**被采到"——没有它，item 8 的第一个数（socket 门铃 vs inproc condvar）只能采到半边。

### 9.1 现象：两条车道各自的真相

**先说结论，因为原文把两条车道混成了一条。** spawn 的 client 进程**确实**会打一条汇总行
（真机数据见 `gate7-device-ab.md` §10.4：`wait[srv=0 srvpark=0 cli=30401 clipark=1538]`）。
缺失的是 **server 进程**，而 client 那条行的**性质**取决于车道跑的是哪种 retrace 模式。

#### 9.1.1 server 半边：整行缺失（两条车道一致）

`pipeStatsEnabled` 在 spawn server 进程里恒为 false，所以它**一条 `MGPipe stats:` 行都没有**——
不是携带 0，是整行不存在。桌面实测，spawn、两种 retrace 模式都一样：

```
=== spawn, BEFORE the fix ===
server   : NO SUMMARY LINE (this process never ran PipeStats::Init, or never reached OnPresent)
```

这一点必须写清楚，因为 `wait[srv=0 srvpark=0]`（"服务端从未等待"）与"没有这一行"（"这个进程里
计数器根本没开"）是**两个不同的陈述**，而它们读起来一样。这正是 `PipeStats.h` 在 Gauge 注释里
反复警告的那类"看起来像答案的零"。

#### 9.1.2 client 半边：行存在与否取决于模式，不是取决于传输

我最初的 §9.1 写"两条 role 都没有行"，那是**桌面 integration / snapshot 车道**的观测，
不是设备的观测。查清了：**client 那条汇总行是 `PipeStats::Shutdown()` 的终行，而 Shutdown 只在
`MobileGL::Destroy()` 里被调用**。所以问题变成"retrace 车道到底跑不跑 Destroy()"，而答案是**看模式**：

| retrace 模式 | 谁结束 replay | `retrace::cleanUp()` | `MobileGL::Destroy()` | client 汇总行 |
|---|---|---|---|---|
| **snapshot**（`--target-call`，快照跑法） | 快照完成后 `exit(0)` | **不跑** | **不跑** | **无** |
| **benchmark**（`--benchmark`，设备 A/B 用的） | replay 走完 `main` 到 `retrace::cleanUp()` | 跑 | **跑** | **有，恰好一条** |

机制在源码上是确定的，不是推断：`tools/trace_replay/CMakeLists.txt:188,246` 用
`-Dexit=mobilegl_apitrace_exit` 编译 apitrace 的 `retrace_main.cpp`（它在
`mobilegl_trace_retrace_common` 里），而 `apitrace_exit.hpp` 把那个符号定义成
**`throw MobileGLRetraceExit{status}`**。于是 snapshot 的退出点
（`retrace_main.cpp:258` `if (call.no >= snapshotFrequency.getLast()) exit(0);`）变成一次**抛出**，
直接穿到 `trace_replay_core.cpp:488` 的 `catch`——**它跳过了 `retrace_main.cpp:1482` 的
`retrace::cleanUp()`**，而那是唯一会走 `glws::cleanup()` → `eglTerminate` →
`EGLImpl.cpp:336` `MobileGL::Destroy()` 的路径。benchmark 模式不抛，正常走完 `cleanUp()`。

**这条判据是可判定的，我用了两条独立证据**：

1. **直接实验**（`p6-mode-probe.sh`）：`MOBILEGL_PIPE_STATS_FILE` 只由 `PipeStats::Shutdown()` 写，
   所以它的 JSON dump 是否存在 = Destroy 是否跑。同一条 OpenRA trace、同一个 spawn 库：

   ```
   === mode=snapshot ===   JSON dump: ABSENT   -> Destroy() did NOT run
                           client summary lines: 0
   === mode=bench ===      JSON dump: PRESENT  -> Destroy() did run
                           client summary lines: 1
                           frames=0 window=0 wait[srv=0 srvpark=0 cli=685 clipark=273]
   ```

2. **与设备摘录的算术吻合**：设备 inproc 那段 client 行是 `window=11` / `frames=251`。
   `FormatWindowLine` 只在帧数是周期（默认 120）的整数倍时随窗口推进，所以终行的 `window`
   必然等于 `frames % period`——`251 % 120 = 11`，**正是设备摘录的值**。这就是终行，不是第 11 帧的窗口行
   （那个会在 frames=120 时出现、window=120）。

#### 9.1.3 由此得到的正确记账

| 车道 / 模式 | spawn client 行 | spawn server 行 | spawn client 的 `cli`/`clipark` | 窗口化字段 |
|---|---|---|---|---|
| 桌面 snapshot | 无 | 无（修前）/ 有（修后） | 不可得 | 不可得 |
| 设备 benchmark（§10.4） | **有，1 条终行** | 无（修前）/ 有（修后） | **可信**（见下） | **不可信**（`frames=0 window=0`） |

**`cli`/`clipark` 的 run total 在 spawn client 里可信，因为它不由 `OnPresent` 产生。**
`EmitPresent`（`EmitTables.cpp:1049-1050`）在**每条 present 记录**上直接
`PublishGauge(ClientWaits/ClientParks)`，而 `PublishGauge` 是 **store 不是 add**（`PipeStats.cpp`），
所以终行读到的是运行总量。设备那条 `cli=30401 clipark=1538` 因此是**真数**——
它与我修前修的无关，一直就是对的。

**但同一个 spawn client 的窗口化字段不可信**：`frames=0 window=0`，因为 spawn client 从不调用
`OnPresent`（那是 backend 的帧边界，而 client 的 Present 是**发射器**）。于是：

- `draws=`、`wrec=`、`bytes[...]`、`gates[...]`、以及**所有 per-frame 字段**（`draws/f`、`wrec/f`、
  `bytes/f[...]`、`acc/draw`）都印 **0 或 `n/a`**——`window=0` 走的就是该文件那条"无帧则退回窗口总量"
  的标注规则，而窗口总量也确实只有终行那点内容。
- 唯一在 spawn client 上非零的是 **`EmitPresent` 直接发布的 gauge**：`cli`/`clipark`（上表）、
  `maxrec`/`maxcap`/`ringwraps`/`ringpads`/`ringwaits`，以及 `resid`/`csob-blob`/`seg`/`wrec` 这类
  **由发射侧直接 Add 的计数器**——它们的 run total 可信，但**窗口归属**只有最后一段。

桌面 benchmark 复现的正是这个形状（`frames=0 window=0`，`cli=685 clipark=273`，
`seg=246975042 wrec=26117` 非零，而 `draws=0`、`gates[ers=0/0 ...]`）。

> **对门 8 item ① 的影响**：设备 §10.4 的 spawn client 半边**本来就是有效测量**，
> 我在 §9.4 说明的"spawn 的 `cli=0 clipark=0` 是另一个进程"指的是**在 server 进程那一行上**读到的
> `cli=0`——那是服务端进程的 client gauge 位，不是 client 进程自己那行。这两个陈述不冲突，
> 但**必须分开写**，否则读者以为 client 半边从来没采到过。item ① 真正缺的只有 **server 半边**。

### 9.2 根因与修法

`PipeStats::Init()` 只从 `MobileGL::Initialize()`（`Init.cpp:123`）调用，而 spawn server 走的是
`mobilegl_server_main` → `MG_ConfigLoader::Init()`（`ServerMain.cpp:182`）+
`MG_Backend::InitServerRoleForSpawn()`（`ServerMain.cpp:239`），**从不经过 `MobileGL::Initialize`**。
于是 `g_pipeStatsEnabled` 在该进程保持静态默认 false，**全进程所有 `if (PipeStats::Enabled())` 站点
（含 `ServerLoop.cpp:436-437` 自己发布 `ServerWaits`/`ServerParks` 的那一对）恒为 false**。

修在 server 角色初始化的唯一收口点，即 `ServerMain.cpp` 的 step 1.5（配置加载 / D1 定角色）之后：

| 改动 | 位置 | 代码 |
|---|---|---|
| 新增 include | `MobileGL/MG_Remote/Server/ServerMain.cpp:49` | `#include <MG_Util/Metrics/PipeStats.h>` |
| **初始化** | `ServerMain.cpp:231`（`MGPipeSetServerProcessRole(true)` 之后、step 2 之前） | `MobileGL::MG_Util::PipeStats::Init();` |
| **收尾** | `ServerMain.cpp:442`（`loop.Stop()`/`session.Close()` 之后、`::_exit(0)` 之前） | `MobileGL::MG_Util::PipeStats::Shutdown();` |

**代码只有这两行**（外加一个 include）；其余是解释"为什么是这个位置"的长注释。

三处顺序都是承重的：

1. **在 `MG_ConfigLoader::Init()` 之后**：`Init()` 把 `g_pipeStatsEnabled`
   **latch 自 `MG_Config::Features.PipeStats`**，而该字段由 `ConfigLoader.cpp:281` 的
   `QueryEnvFlag("MOBILEGL_PIPE_STATS")` 解析——解析器在上一行跑完之前都不存在。提前调用会把
   静态默认（false）latch 下来，launcher 传进来的 `MOBILEGL_PIPE_STATS` 就没人读——
   与 step 1.5(a) 记录的那个"没人读配置"缺陷是同一类。
2. **在 `MGPipeSetServerProcessRole(true)` 之后**：这是让**日志 sink** 知道本进程是 server 的
   地方（`Log.cpp` 的 `ProcessIsSpawnedServer()` 读 `MOBILEGL_IPC_ROLE`，launcher 设置且其 envp
   scrub 保留它，因此按 `ThreadIsServerRole()` 该进程所有线程都写 `<base>.server.log`）。
   顺序这样排，是为了让"摘要行进 server 角色日志"这条契约不依赖将来那次解析改动的运气——
   **并且它由车道实测断言，不由这段注释担保**（§9.4 的 `server: banner=1 summary_lines=29`）。
3. **`Shutdown()` 在 `loop.Stop()` 之后**：apply 线程正是发布 `ServerWaits`/`ServerParks` 的线程，
   它还在跑时取 dump 是竞态；而 `_exit(0)` 不跑任何 atexit handler，所以这是服务端唯一能写出
   运行总量与 JSON dump 的位置——没有它，spawn server 会丢掉最后一个窗（`Init.cpp:59` 在 client
   侧调 `Shutdown` 就是为了同一个"最后一帧的数别丢"）。

### 9.3 三个 env 变量的语义：与 client 进程**完全一致**

`MOBILEGL_PIPE_STATS` / `_PERIOD` / `_FILE` 三者都由**子进程自己的环境**经**同一个解析器**读，
再由**同一个 `Init()`** latch——stats 通道没有任何 spawn 特化。唯一的真实差异是**行节奏**：

> 服务端进程没有自己的 backend Present（它不做 swap），所以它的摘要行节奏骑在它**应用的 present
> 记录**上（`PipeApplier.cpp:317` 的 `table->Present()` → DirectGLES/DirectVulkan 的 `Present()`
> → `OnPresent()`），而不是骑在一次它并不执行的 swap 上。一次 present N 次的 spawn 会话，
> **服务端产生 N 行窗口行**。
>
> **client 侧不是 N 行**——我最初在这里写的"与 client 自己那行数的 N 相同"是错的，实测把它否掉了。
> spawn client 的 `Present` 是**发射器**（`EmitPresent`），它把 `PublishGauge(ClientWaits/ClientParks)`
> 与 `maxrec` 那一批发出去，但**不调** `PipeStats::OnPresent()`（那是 backend 的帧边界，不是发射器的）。
> 所以 client 一个窗口都不推进，它唯一的一行是 `Shutdown()` 的**终行**，`frames=0 window=0`——
> 前提是那条车道走到 `MobileGL::Destroy()`（§9.1.2：benchmark 模式走到，snapshot 模式不走）。
> 桌面 benchmark 实测：**server 128 行窗口行 + client 1 行终行**，两者不等。
>
> 清单里把 `Init.cpp` / `ConfigLoader.cpp` / `ServerLoop.cpp` / `ServerSession.cpp` 列为"按需"，
> 实测**四者都不需要改**，本轮 `git diff --name-only` 只动了 `ServerMain.cpp`（§9.2）
> 与 §9.7 的 dump 路径四个文件。

### 9.4 验证：单测 + 端到端 red-once

**(a) 单元层**：新增两个用例（`PipeStatsTest.cpp`，见 §4 表），`ctest -L unit -R PipeStats` = **24/24**。
其中 `InitIsTheOnlyLatchAndItTakesTheConfigBothWays` 钉住"latch 必须两个方向都取配置值"，
`PresentationsAtTheDefaultPeriodOfOneMakeOneWindowEach` 钉住"k 次 present → k 个窗口"这条
车道费率所依赖的算术（并顺带钉住 gauge 是**运行总量、不是求和**——本报告 §6.1 的聚合脚本
第一版正是栽在这里）。

**(b) 单元层 red-once**：把 `PipeStats.cpp` 的 latch 改成单向 `=true`，重跑：

```
2/2 Test #1718: PipeStatsTest.InitIsTheOnlyLatchAndItTakesTheConfigBothWays ...***Failed
50% tests passed, 1 tests failed out of 2
```

恢复后 24/24 绿。（脚本 `p6-redonce.sh`。这个控制证明的是**两个用例不是同义反复**；
它证不了端到端，因为用例直接调 `Init()`——被测对象是 latch 本身，而缺陷是 latch **从未被调用**。）

**(c) 端到端 red-once**（这才是修复的那个控制）：构建两版库，**唯一差别**是 §9.2 那两行，
同一条 spawn trace 各跑一次：

| | server 侧 summary 行数 | server 侧 `wait[...]` |
|---|---:|---|
| **修前**（step 1.6 两行移除） | **0** | 无（整行缺失） |
| **修后** | **29** | `srv=387 srvpark=133`（= 13.34 / 4.59 per frame） |

脚本 `p6-redonce-e2e.sh`；两侧 SSIM 均 1.000000，即修复不改渲染。

**(d) 同机 paired A/B**（`p6-ab-transport.sh` + `p6-waitrate.py`，同一条 OpenRA trace、同一个库，
只差 `MOBILEGL_TRANSPORT`）：

| | inproc | spawn |
|---|---:|---:|
| client 进程 | 无 summary 行（§9.3） | 无 summary 行（同因） |
| server 进程 summary 行 | 29 | 29 |
| `srv`（waits/帧） | 16.83 | **15.00** |
| `srvpark`（parks/帧） | 3.59 | **4.38** |
| `srvpark/srv` | 21.3% | **29.2%** |
| `cli` / `clipark` | 9.21 / 3.14（与 server 同行，因 inproc 单进程） | **0 / 0**（另一进程，见下） |

读法：

- **inproc 的两个半边在同一行上**，因为两个角色是同一进程的两个线程、共用一套计数器（§9.3 已
  解释了为什么这行出现在 server 日志里）；
- **spawn 的 `cli=0 clipark=0` 是"另一个进程"，不是"客户端没等"**。它与契约里 `rsp`、`srv` 在
  spawn 下的既有行为同类，也是 `PipeStats.h` Gauge 注释里写明的那种"形如零的零"。要取 spawn 的
  client 半边，必须让 client 进程也到达一次帧边界（§9.3 的结构性原因），那是独立于本轮的一步。
- **socket 门铃 vs condvar 的对比现在是可比的**：两侧 `srv`/`srvpark` 都由同一套代码、同一个
  wait 收口点、同一口径采集。采样中 socket 的 park 比例略高（29.2% vs 21.3%），
  与契约 §9 item 8 那条预告（"socket 链路没有那条共享 cacheline，转成阻塞读可能更便宜"）
  方向一致但**幅度很小，且这里是 llvmpipe/桌面**——真机数仍需在设备上另采，本节不做性能结论。

**(e) 回归**：`ctest -L unit` **2366/2366**（比上一轮多 2 个新用例）；
`.Spawn.` 前缀的集成条目 **102/102 全绿**。

### 9.5 G1 复验

§9.2 的改动在 `MG_Remote/Server/ServerMain.cpp`，属 `MOBILEGL_BUILD_DISAGGREGATED` 的源列表，
pull 构建根本不编译它——但 G1 是**实测**不是推理。重跑 §5 的两棵树（baseline = 本轮改的 5 个文件
全部回退到 `git HEAD`，head = 本轮内容，同一条 pull 配置）：

```
symbol-report: .text 10158850 -> 10158850 (+0, +0.000%)
symbol-report: 27863 -> 27863 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27122 -> 27122 normalised names, 27122 unchanged
G1 rc=0
.text BYTE-IDENTICAL
MG_Remote symbols : 0
gate8 symbols     : 0
```

即：**符号集 0/0/0/0，`.text` 逐字节相同，pull 构建仍无任何 `MG_Remote` 符号**。

### 9.6 本节的未做项

1. **`MOBILEGL_PIPE_STATS_FILE` 的角色冲突已修**（原为未做项 2），见 §9.7。
2. **spawn client 的窗口化字段仍不可信，且这一项不是缺陷而是结构**。要让 spawn client 推进窗口，
   得让 client 进程到达一次 `PipeStats::OnPresent`；而 client 的帧边界在 spawn 下存在于
   `EmitPresent`（每条 present 记录一次）——把它接上去是**改变 client 侧调用点**，
   超出本轮授权，且会与 `DirectGLES`/`DirectVulkan` 的 backend 帧边界语义重复计数
   （同一个进程里两个 `OnPresent` 会让 `frames` 走两倍）。需要单独判断，不要顺手接。
   本轮已把这条限制在 §9.1.3 写清：**spawn client 的 `cli`/`clipark` 可信，per-frame 字段不可信**。
3. **真机（Redmi）未跑**。本节全部数据来自 `wsl -d Ubuntu` + llvmpipe，门 7 与门 8 的设备数
   仍需在设备上采；本节只交付"server 半边可测"这个前提。
   §9.1.2 对**设备**那条 client 行的定性是**从源码机制 + 与本机 benchmark 复现的算术吻合**得出的
   （`251 % 120 = 11` 对上 §10.4 的 `window=11`），不是从设备日志原件里读出的 `frames=`——
   设备报告没有摘录该字段。若要在设备上把这条钉死，需要把那一行的 `frames=`/`window=` 一并留存。

---

### 9.7 `MOBILEGL_PIPE_STATS_FILE` 的角色分路

§9.6 原第 2 项记录的那个陷阱：修完 §9.2 之后 spawn 的**两个**进程都会执行 `WriteJsonDump()`，
而两者都写 `MG_Config::Features.PipeStatsFile` 的**同一个字符串**，`ofstream` 的 trunc 让后写的
覆盖先写的。读者会拿到**一个角色**的数，挂在一个自称是整轮运行的文件名下——比缺文件更坏，
因为看不出哪里不对。

### 9.7.1 修法：与日志 sink 同一套命名规则，规则不复制

| 改动 | 位置 | 内容 |
|---|---|---|
| 导出角色查询（split 臂） | `MobileGL/MG_Util/Debug/Log.h:177,183`、`Log.cpp:100,107` | `LogRole CurrentThreadRole()`、`bool CurrentProcessIsSpawnedServer()` |
| 同两个查询的 pull 臂 | `Log.h:222-223` | 内联；理由是 G1（见 9.7.3） |
| dump 路径派生 | `PipeStats.cpp:787`（声明 `PipeStats.h:463`） | 经 `MG_Util::Debug::RoleLogPath(base, CurrentThreadRole())` |
| `WriteJsonDump` / `Init` banner 改用它 | `PipeStats.cpp` 的 `WriteJsonDump()`、`Init()` | 两条路径都印/写派生名 |
| 新单测 + 负控 | `PipeStatsTest.cpp` | 见 9.7.2 |

命名规则**只有一处实现**（`Log.cpp:87` 的 `RoleLogPath`），PipeStats 调它而不是复制它——
这正是任务书要求的"派生规则由库导出、不复制"。

### 9.7.2 验证

**端到端**（`p6-dumppath.sh`，同一条 OpenRA trace、同一个库，只改传输）：

```
================ inproc ================
  run.json : absent          <- 基名不指向任何文件
  run.client.json : PRESENT (1789 bytes)
  run.server.json : absent
  banner: ... JSON dump to .../run.client.json
  dump  : wrote JSON dump to .../run.client.json

================ spawn ================
  run.json : absent          <- 基名不指向任何文件
  run.client.json : PRESENT (1736 bytes)   <- client 进程自己的数
  run.server.json : PRESENT (1714 bytes)   <- server 进程自己的数，不再被覆盖
  banner: ... JSON dump to .../run.client.json
  banner: ... JSON dump to .../run.server.json
  dump  : wrote JSON dump to .../run.client.json
  dump  : wrote JSON dump to .../run.server.json
```

inproc 只有 client 一个文件是**正确**的，不是缺陷：那个进程里只有 GL 线程会到
`MobileGL::Destroy()`（§9.1.2），服务端角色是线程不是进程，`Shutdown()` 只跑一次。

**单测 25/25**，新增 `JsonDumpPathIsRoleDerivedAndTheBaseNameIsNeverOpened`。
**但它的第一版是假绿，值得记下来**：那一版断言的是 `RoleLogPath` 自己的性质
（两角色路径不同、都不等于基名）——**把修复整个回退掉它照样通过**，因为它测的是命名辅助函数，
而辅助函数从来没坏；坏的是 `WriteJsonDump` **没有调用它**。一个在缺陷存在时仍然绿的测试比没有测试更坏。
改法是把被测对象换成 `RoleDerivedJsonDumpPathForTesting()`——dump 与 banner **共用**的那一个表达式，
于是回退它就会红。**red-once 复验**（`p6-redonce-dump.sh`，把那个表达式换回裸基名）：

```
2/2 Test #1720: PipeStatsTest.JsonDumpPathIsRoleDerivedAndTheBaseNameIsNeverOpened ...***Failed
96% tests passed, 1 tests failed out of 25
恢复后 25/25 绿
```

### 9.7.3 G1：这次它是真的会红，而且红了三次

这一轮 G1 **三个不同的泄漏点**依次被抓出来，每个都不是能从代码上看出来的：

| 尝试 | 结果 | 原因 |
|---|---|---|
| 无条件派生 | `.text +24 bytes` | 把 `std::string` 临时量与一次调用放进了 monolith 的函数体 |
| 加 `#if` 但两臂共用一个 `String` 局部 | **1 symbol resized**（`Shutdown()` 344→343，−1 字节） | 两个 `#if` 臂**即使算出同一个值也是不同的代码** |
| 两臂分离，但重写了 `MGLOG_W` 的格式串 | **`.rodata +64`** | 把基名与派生名都印出来的新字面量；**字面量在哪个分支里都是字面量，而 `.rodata` 也被测量** |

最终形态：**pull 臂的代码逐语句就是原样**（`const String& path = MG_Config::Features.PipeStatsFile;`
+ `if (path.empty()) return;`），所有新增都在 `#if` 之内。这个文件的规则因此比"把新代码 guard 起来"
更强：**旧代码一个字都不能动**。

最终复验：

```
symbol-report: .text 10158850 -> 10158850 (+0, +0.000%)
symbol-report: .rodata 1521854 -> 1521854 (+0)
symbol-report: Total 16585193 -> 16585193 (+0)
symbol-report: 27863 -> 27863 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27122 -> 27122 normalised names, 27122 unchanged
G1 rc=0
.text BYTE-IDENTICAL
MG_Remote symbols : 0
gate8 symbols     : 0
```

`g1.sh` 里加了一步前置断言：**先证明两组文件内容真的不同**（本轮 7 个里有 7 个不同），
否则"两棵树 diff 为 0"会以"我忘了拷贝"的形式假绿——这正是这一轮最容易犯的错，
因为漏掉的如果是一个新文件的声明，基线就等于 head，而门会说 OK。

### 9.7.4 本条对下游的提醒

- 任何**读** `MOBILEGL_PIPE_STATS_FILE` 的车道/脚本都要改成读派生名。基名**不会**存在，
  所以没改的读取方会当场 `ENOENT`——这是刻意的（与日志 sink 同一取舍：宁可响亮地失败，
  也不要拿到半轮数据却看不出来）。仓库内目前没有这样的读者（`grep` 确认），
  所以这次改名不破坏任何现有车道。
- `MOBILEGL_PIPE_STATS_FILE` 的**语义**没有变：仍然是"在这里写 dump"，
  只是"这里"由库按角色展开成两个名字。

