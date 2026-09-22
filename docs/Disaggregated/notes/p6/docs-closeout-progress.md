# docs-closeout-progress — `CURRENT_STAGE_PROGRESS.md` 的 P6 收官写入报告

2026-09-22。工作树 `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`，
分支 `feat/disaggregated`，**现头 `2bd86664`**，与 `origin/feat/disaggregated` 同步。任务书给的
头 `71aa9951` 是**两轮测量的源头**（门 7 报告记 `98d0b96c`、门 8 的 wire 计数器与真机第二轮记
`71aa9951`）；它之后分支又落了两个 P6.5 设计提交（`2d86e07a`、`2bd86664`），非本报告所加。

**只改了一份文档**：`docs/Disaggregated/CURRENT_STAGE_PROGRESS.md`。未 commit。工作树里既有的
未提交改动（`MobileGL/MG_Remote/**`、`MobileGL/MG_Util/**`、`tools/device_bench/pin_device.sh`、
`tools/trace_replay/run_android_retrace_local.py`、`tools/device_bench/p6/`）**一个字节未动**。
另外两份（`CURRENT_STAGE_PROGRESS.md` 之外的两份并行代理文档）未触碰；只读证据
`notes/p6/gate7-device-ab.md`、`gate8-doorbell-device.md`、`gate8-wire-counters.md`、
`git-worktree-fix.md` 只读未改。

---

## 0 改动总览

| # | 位置 | 改动 | 行数 |
|---|---|---|---|
| 1 | 头部状态段 | 「实现完成、出口性能/设备数待补」→「**P6 已收官（2026-09-22）**」+ 门 7 tie / 门 8 三个数齐 / G1 0/0/0/0 + 分支头事实 | 改写，+6 / −4 |
| 2 | 新增 §1.1「收尾（2026-09-22）」 | 包状态表之后补五条：wire 计数器、`ServerMain` 武装（红一次证据）、STATS_FILE 角色派生、`pin_device.sh` `2f7cbe2e` 行、工具入库 + 测试数 | 新增 39 行 |
| 3 | §2 出口门表第 7 行 | ⏳ → ✅，写两轮会话的 boot id / 臂序 / 臂数 / client CPU 数 / tie 判读 / 证据文件 | 改写 |
| 4 | §2 出口门表第 8 行 | ⏳ → ✅，写三个强制性能数 + 诚实判读 + 证据文件 | 改写 |
| 5 | §2 表后新增一段 | `MEASUREMENTS.md:342` 的「分块后分布不存在」据此关闭 | 新增 5 行 |
| 6 | §2.1 标题与首条 | 标题改为「出口门之外仍欠」；首条 `PipeStats` 结构性零由「修复进行中」改为「**已修**，0 → 29 行」 | 改写 2 行 |
| 7 | §3 新增 §3.1 / §3.2 / §3.3 | 配对 A/B、门 8① 门铃账、门 8②③ wire 计数器 | 新增 61 行 |
| 8 | §5 | 删去已解决的「出口门 7、8」条目；补两条结构性限制 | 净 +17 行 |

净变化：`134 insertions(+), 11 deletions(-)`（`git diff --stat docs/Disaggregated/CURRENT_STAGE_PROGRESS.md`；
本报告写完后又校过一遍，行数随 §2 一段的口径修正微调过）。无段落被误删：`DirectVulkan` 分歧、
P6.5、Ph、P12、seq/op 五条**逐字保留**（仅 `DirectVulkan` 条尾加了"见 §3 与 `ROADMAP.md` P7 行"）。

---

## 1 逐处对照（事实基线 → 落文）

### 1.1 头部状态段

旧：「状态（2026-09-21）：P6 全部包已落地（a6…t6），出口门第 1–6 项达成；第 7 项…第 8 项…尚未
正式采集，因此 P6 记为「实现完成、出口性能/设备数待补」，**未宣布收官**。分支 … 头 `16bfab10`」

新：`P6 已收官（2026-09-22）`，并把分支头写成**三层事实**，因为任务书要求"照实说"而
`71aa9951` 已不是树头：

- 门 7 报告头 `98d0b96c`（`gate7-device-ab.md:3`）、门 8 第二轮头 `71aa9951`
  （`gate8-doorbell-device.md:3`、`gate8-wire-counters.md:3`）——**测量期的源头**；
- 其后两个提交 `2d86e07a`、`2bd86664` 是 P6.5 设计（已核实：`2d86e07a` 的 diff 只含
  `CONTRACT-P6.md` / `StreamLink.h`(+5 行注释) / `ARCHITECTURE.md` / `CURRENT_STAGE_PROGRESS.md` /
  `P6-ENDSTATE-REVIEW.md` / `ROADMAP.md`；`2bd86664` 只含 `ROADMAP.md`）；
- 现头 `2bd86664`，与 `origin` 同步（`git rev-parse origin/feat/disaggregated` = `2bd86664…`）；
  **工作树另有未提交的 P6 收尾改动**，指向 §1.1。

### 1.2 §1.1 收尾（2026-09-22）

任务书要求的六项全部落文，并各自指出处：

| 任务书要求 | 落文内容 |
|---|---|
| wire 计数器（seg=/wrec= + 直方图） | `ByteClass::StageSegmentBytes` 唯一收口 `PipeWireCodec.cpp:878`；`CallClass::WireRecords` `:1188`；staged-blob 直方图复用 `PayloadBucketOf`；汇总行 `seg=` / `wrec=` / `wrec/f=`；push-only |
| ServerMain PipeStats 武装（红一次证据） | 「修复前 spawn server **0** 条汇总行、修复后 **29** 条，同一条 spawn trace、两侧 SSIM 均 1.000000」；`Init()` 的两处顺序承重点 |
| STATS_FILE 角色派生 | `<base>.client.json` / `<base>.server.json`、基名不指向文件、`RoleLogPath` 导出不复制；G1 红三次（`.text +24` / 1 symbol resized / `.rodata +64`） |
| pin_device.sh `2f7cbe2e` 条目 | 本板钳 **1050 MHz**（`thermal_pwrlevel` 写 0 读回 1、`max_gpuclk` 只读 1 050 000 000）；**与 1100 MHz 时代的 `35d0befa` 行不可比，只有同会话配对可比** |
| tools/device_bench/p6 入库 | `{ab_session.py,render_ab.py,README.md}` + `run_android_retrace_local.py`（+19 行 `MOBILEGL_TRACE_PACKAGE`）；四文件**未提交** |
| 单测 2367/2367 与 spawn 102/102 | 落文；另加 `PipeStats 25/25` 与 G1 `.text` 两侧各 10 158 850 字节 `cmp` 无差异 |

### 1.3 §2 出口门表

第 7 行与第 8 行从 ⏳ 改 ✅，数字逐字取自只读证据，**不改写、不重算、不挑好看的**：

- 会话 1：boot id `e69d0329…`→`107f24d3…`，六臂 `monolith,inproc,spawn,inproc,monolith,spawn`
  × best-of-3，18/18 exit 0、36/36 `PINNED`、0 `Fatal{`；client CPU p50 ms/帧
  inproc **7.880/7.889**、spawn **7.876/7.869**（均值 −0.15%，臂内散布 0.2–4.2%），monolith 约贵 30%。
- 会话 2：boot id `107f24d3…`→`7e6c6e9f…`，交错 × best-of-2，4/4 OK、8/8 `PINNED`、0 `Fatal`；
  **7.987/7.989 vs 7.992/8.009**（+0.16%）。
- 第 8 行：① client 121.1 waits/帧、5.6–6.1 parks/帧（run total 30 401–30 407）；server inproc
  10 866–11 963、spawn 10 415–10 416；park 率 0.042–0.06%；clipark/f 6.07 vs 5.60/5.75。
  ② OpenRA 130.8 KB median / 8.01 MB steady mean（含负载 8.07 MB）；rd12 816.2 KB median /
  1.53 MB steady mean（含负载 10.79 MB）。③ OpenRA 211/206；rd12 7 497/7 504；`maxrec=1808`。

第 8 行末尾的**诚实判读**逐句保留证据里的定性：socket 阻塞读**既不更便宜也不更贵**、契约 §9
「阻塞读可能更便宜」**未兑现**、spawn server waits 少 8.9–12.9% 归因于 handoff 纪律而非门铃本身、
clipark/f 是唯一 socket 略差的列（每帧多约 0.4 次 park）。第 7 行明写 **tie** 与「不设门」。

### 1.4 §2.1 与 §5

- §2.1 首条的旧文写「修法是在 server 进程里…（工作区中进行中，未提交）」——**已过时**，改为
  「**已修**…spawn server 汇总行 0 → 29 条，SSIM 1.0 不变」。标题 `收官前另欠` 已不成立（出口门
  1–8 全达成），改成 `出口门之外仍欠`，并注明这些是门外的债。
- §5 删去 `**出口门 7、8**（真机配对 A/B、三个性能数）——待采集，见 §2。` 一条（已解决）。
  其余五条保留。新增两条**结构性限制**（任务书指定）：
  1. spawn client 无 `OnPresent` → 窗口字段 `frames=0 window=0` 无效应，但 `PublishGauge` 是 store
     所以 `cli`/`clipark` 与**发射侧直接 Add** 的 `resid`/`csob-blob`/`seg`/`wrec` 的 run total 有效；
     并给出逐 role 取数规则（`srv` 读 server.log、`cli` 读 client.log、spawn client 只有 `Shutdown`
     终行或 0 行）。
  2. `MOBILEGL_PIPE_STATS_FILE` 已改角色派生 `<base>.client.json` / `<base>.server.json`，基名不指向
     文件；inproc 只产出 `.client.json` 是**正确**的。

### 1.5 §3 真机验收

原有三条功能行、`DirectVulkan` 分歧段、llvmpipe `iterationrp` 段**逐字未动**，在其后新增
§3.1（配对 A/B）、§3.2（门 8① 门铃账，含读法警告）、§3.3（门 8②③ wire 计数器，含
framing 警告、负控、逐 blob 分布、`tex +194%` 未解释记为证据不作结论）。

任务书指定的两个必须如实写的点都在：**OpenRA 真机辅负载「未跑」**（§3.3 末段），以及
**gate8-wire-counters.md §6 的数据来自 WSL + llvmpipe，门 8②③ 是桌面 inproc 数**（同一段）。

---

## 2 引用检查输出

脚本 `scripts/check_doc_citations.py` 的 `documents` 是**位置参数（`nargs="+"`），不接受空参**，
所以任务书里那条裸命令跑不出来：

```
$ python scripts/check_doc_citations.py
usage: check_doc_citations.py [-h] [--rev REV] [--strict]
                              documents [documents ...]
check_doc_citations.py: error: the following arguments are required: documents
EXIT=2
```

按脚本自己的用法（`python3 scripts/check_doc_citations.py docs/Disaggregated/*.md`）跑，结果如下。

**目标文档（本报告唯一改动的文件）：**

```
$ cd MobileGL-disagg && python scripts/check_doc_citations.py docs/Disaggregated/CURRENT_STAGE_PROGRESS.md
check_doc_citations: 3 citations in 1 document(s) against HEAD, 0 problem(s)
EXIT=0
```

（这是**改动后**的结果。改动前同一命令报 `2 citations … 0 problem(s)`；计数从 2 升到 3 是因为新增了
`PipeWireCodec.cpp:878` 一处被正则捕获的引用——另外两处 `MEASUREMENTS.md:342`、`PipeStats.h:256` 是
原有的。同段里的 `:1188` 因无文件名前缀，按脚本自己的注释「`:12-13` is NOT matched on purpose」不计数。）

**全量（覆盖 `docs/Disaggregated`，含只读证据与三份并行代理文档）：**

```
$ cd MobileGL-disagg && python scripts/check_doc_citations.py docs/Disaggregated/*.md
check_doc_citations: 179 citations in 11 document(s) against HEAD, 1 problem(s)
  docs/Disaggregated/P6-ENDSTATE-REVIEW.md:284: `Init.cpp:184-211` -> ambiguous: MobileGL/Init.cpp, MobileGL/MG_Backend/Init.cpp, MobileGL/MG_Impl/Init.cpp
EXIT=0
```

该脚本**确实覆盖 `docs/Disaggregated`**（上表 11 份文档含 `docs/Disaggregated/*.md`），
但**不递归**：`notes/p6/*.md` 不在通配里。唯一 1 条 problem 是 `P6-ENDSTATE-REVIEW.md` 里
**既有的** basename 歧义（非本次改动引入，且该文件由并行代理负责，未碰），无 `--strict` 时退出码 0。

**扩展核查**（把只读证据与 `notes/p6` 一并喂进去，确认我引用的三个证据文件本身的状态）：

```
$ cd MobileGL-disagg && python scripts/check_doc_citations.py \
    docs/Disaggregated/CURRENT_STAGE_PROGRESS.md \
    docs/Disaggregated/notes/p6/gate7-device-ab.md \
    docs/Disaggregated/notes/p6/gate8-doorbell-device.md \
    docs/Disaggregated/notes/p6/gate8-wire-counters.md
check_doc_citations: 55 citations in 4 document(s) against HEAD, 9 problem(s)
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:141: `MobileGL/MG_Util/Metrics/PipeStats.h:382-386` -> MobileGL/MG_Util/Metrics/PipeStats.h has 372 lines at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:145: `MobileGL/MG_Util/Metrics/PipeStats.cpp:708` -> MobileGL/MG_Util/Metrics/PipeStats.cpp has 646 lines at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:613: `retrace_main.cpp:258` -> no such file at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:614: `retrace_main.cpp:1482` -> no such file at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:669: `Init.cpp:123` -> ambiguous: MobileGL/Init.cpp, MobileGL/MG_Backend/Init.cpp, MobileGL/MG_Impl/Init.cpp
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:681: `ServerMain.cpp:442` -> MobileGL/MG_Remote/Server/ServerMain.cpp has 395 lines at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:699: `Init.cpp:59` -> ambiguous: MobileGL/Init.cpp, MobileGL/MG_Backend/Init.cpp, MobileGL/MG_Impl/Init.cpp
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:826: `PipeStats.cpp:787` -> MobileGL/MG_Util/Metrics/PipeStats.cpp has 646 lines at HEAD
  docs/Disaggregated/notes/p6/gate8-wire-counters.md:826: `PipeStats.h:463` -> MobileGL/MG_Util/Metrics/PipeStats.h has 372 lines at HEAD
EXIT=0
```

**这 9 条全部落在 `notes/p6/gate8-wire-counters.md`（只读证据，非本报告改动的文件），而且全部是
"对着工作树而非 HEAD 写的行号"** —— 正是它自己 §3 开头写的"行号为改后内容"：

- **5 条**（`:141`、`:145`、`:681`、`:826` ×2）引的是**未提交的工作树行号**——正是它自己 §3 开头
  写的"行号为改后内容"。实测两侧行数：`PipeStats.h` 工作树 **466** vs HEAD **372**、
  `PipeStats.cpp` 工作树 **800** vs HEAD **646**、`ServerMain.cpp` 工作树 **445** vs HEAD **395**。
  脚本按 `HEAD` 解析，故报超界。**这 5 条对着工作树是对的**，对着 HEAD 才红。
- **2 条**（`:613`、`:614`）引 `retrace_main.cpp`，该文件在 `3rdparty/apitrace` 子模块内，
  不在本仓库的 `ls-tree` 里，脚本解析不到。
- **2 条**（`:669`、`:699`）是 `Init.cpp` 的既有 basename 歧义（`MobileGL/Init.cpp` /
  `MG_Backend/Init.cpp` / `MG_Impl/Init.cpp`）。

**目标文档 `CURRENT_STAGE_PROGRESS.md` 自身 0 problem。** 三处行号引用按 HEAD 解析都在界内：
`PipeStats.h:256`（HEAD 372 行）、`PipeWireCodec.cpp:878`（HEAD 2299 行）、
`MEASUREMENTS.md:342`（HEAD 599 行）。`:1188`（`EncodeRecord`）写在中间点前无文件名，按脚本设计不计；
它与 `:878` 都取**证据文件 `gate8-wire-counters.md` §3 的同一口径（改后行号）**——
`PipeWireCodec.cpp` 工作树 2335 / HEAD 2299，两个行号在两侧都成立，所以这条无需改口径。

**链接目标存在性**（我新加的 markdown 相对链接逐个 `test -f`）：

```
OK docs/Disaggregated/notes/p6/gate7-device-ab.md
OK docs/Disaggregated/notes/p6/gate8-doorbell-device.md
OK docs/Disaggregated/notes/p6/gate8-wire-counters.md
OK tools/device_bench/p6/ab_session.py
OK tools/device_bench/p6/render_ab.py
OK tools/device_bench/p6/README.md
```

---

## 3 边界与未做

- **未 commit**：只写了工作树文件。任务书明确禁止 `git commit`。
- **未碰**其余两份并行代理文档，也未碰 `ROADMAP.md` / `MEASUREMENTS.md` / `ARCHITECTURE.md`
  （`ROADMAP.md` 的 P6 行与 `MEASUREMENTS.md:342` 仍是旧口径——本报告把"可关闭"写在
  `CURRENT_STAGE_PROGRESS.md` 里，**没有替它们改**，那不在本次授权内）。
- **`CURRENT_STAGE_PROGRESS.md` 的既有 `MEASUREMENTS.md:342` 句子未删**，只在其下增写了关闭依据；
  删除该句属 `MEASUREMENTS.md` 自身的改动，留待该文件的责任方。
- **未新增引用**到 `gate8-doorbell-device.md` 之外的新证据文件；`git-worktree-fix.md` 未在本文档引用
  （它是 git 元数据修复，与阶段进度无关）。
- 文档定位保持为「只记当前阶段（P6）」：新增内容全部是 P6 的收官事实与门 8 的数，
  P6.5 / Ph / P12 只在 §5 以「不属于 P6」的身份出现，入口提示段（顶部 blockquote）逐字未动。
