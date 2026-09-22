# P6 收官事实写入 `ROADMAP.md` —— 改动报告

> 日期 2026-09-22；工作树 `C:\Users\geekerwan\AndroidStudioProjects\FoldCraftLauncher\MobileGL-disagg`，
> 分支 `feat/disaggregated`。**只改 `docs/Disaggregated/ROADMAP.md` 一份文档**，另两份（由并行代理负责）
> 未触碰；未 `git commit`；未触碰工作树里已有的未提交代码改动。
>
> 证据只读引用（未修改）：`notes/p6/gate7-device-ab.md`、`notes/p6/gate8-doorbell-device.md`、
> `notes/p6/gate8-wire-counters.md`、`notes/p6/git-worktree-fix.md`。

---

## 0 一句话

把 P6 的收官事实（门 7 / 门 8 已采、判读 tie、收尾四项落地）写进 `ROADMAP.md`，
**6 行改动**：顶部 blockquote、阶段表 P6 行的状态列 / 落地列 / 验收门列、里程碑列表两条、一处引用了
`ServerMain.cpp`「正在被门 8 的工作修改」的行内说明、一处写「门 8 的树上工作正在补」的开放问题。
其余全部逐字未动（含债务表整张、其余阶段行、P6.5「立即的下一项」整节）。

---

## 1 逐处改动（行号按改前 / 改后对照）

### 1.1 顶部状态 blockquote（第 3 行）

**改前**：`P6 全部包（a6 / c6 / lk / so / sm / cp / hs / dl / st / t6）已落地于 feat/disaggregated
（头 16bfab10，2026-09-21）；出口门第 1–6 项达成，第 7 项（真机配对 A/B 热窗口）与第 8 项
（三个强制性能数）尚未正式采集，故 P6 记为「实现完成、出口性能/设备数待补」，未宣布收官**。

**改后**：`P6 已收官（2026-09-22）`：全部包已落地（头 `16bfab10`，2026-09-21），出口门第 1–6 项此前已达成；
第 7 项（Redmi `2f7cbe2e` 的 reboot-clean 同热窗口配对 A/B）与第 8 项（三个强制性能数）已于 2026-09-22 采齐
——门 7 判读 **tie**（client CPU p50 差 −0.15% / +0.16%，均在臂内散布 0.2–4.2% 之内，符号随臂序翻转；
monolith 约贵 30%），门 8 三数齐；证据链 `notes/p6/gate7-device-ab.md` / `gate8-doorbell-device.md` /
`gate8-wire-counters.md`。

**未动**：同一 blockquote 里的 P5f 验收代码句、P5e 债句、CI / inproc 收官句；第 5 行（Magma run-ahead）
与第 7 行（终局改判）整句原样——这两行的「P6 / P7 全阶段状态不因此改变」与「P6.5 关键路径」结论
不受收官影响。

### 1.2 阶段表 P6 行（第 37 行）

- **状态列**：`⏳ 全部包已落地（2026-09-21，头 16bfab10）；出口门 1–6 达成，7/8 待采` →
  `✅ 已收官（2026-09-22）`，并保留十包落地信息。原「收官前另欠」三项改写为：server 进程 `PipeStats`
  从未初始化**已在收尾中修好**；184 符号棘轮门与 hs 的 `LinkTerms` / `Refuse` / 指纹拆分**未落地、移交 P6.5**。
  （只改「已在收尾中修好」这一处口径，未落地项原样保留——契约 §12.1 的棘轮门与 hs 三行确实仍未做。）
- **落地什么列**：原有十包链与 a6 / P6-ENDSTATE-REVIEW 引用逐字保留，**追加收尾四项**：
  wire 计数器（`PipeWireCodec.cpp:878` `StageAllocate` 唯一收口、`:1188` `EncodeRecord` 提交路径、
  staged-blob 直方图、汇总行 `seg=` / `wrec=`）；`ServerMain.cpp` 补 `PipeStats::Init()` / `Shutdown()`
  （修前 0 条汇总行 / 修后 29 条、SSIM 1.0 不变）；`MOBILEGL_PIPE_STATS_FILE` 角色派生
  `<base>.client.json` / `<base>.server.json`；工具入库 `tools/device_bench/p6/{ab_session.py,render_ab.py,README.md}`
  与 `tools/trace_replay/run_android_retrace_local.py`（+19 行），`pin_device.sh` 新增 `2f7cbe2e` 条目。
- **验收门列**：`7/8 待采：……尚未正式记录` → `7/8 已采（2026-09-22）` 的达成摘要，逐字使用基线数字：
  - 门 7：Redmi `2f7cbe2e` / M332BF / SM8750 / Adreno 830v2、CPU 定频 little 1555200 / big 1958400、
    GPU 钳 1050 MHz、rd12-in-world + DirectGLES、~1471 draws/帧；六臂交错 × best-of-3 =
    18/18 runner 退出 0、36/36 `PINNED`、0 `Fatal`；inproc 7.880/7.889 vs spawn 7.876/7.869（−0.15%）、
    第二会话 7.987/7.989 vs 7.992/8.009（+0.16%）；判读 **tie**，如实记录、不设门；monolith 约贵 30%。
  - 门 8①：client 121.1 waits/帧、5.6–6.1 parks/帧、两传输逐字相同（run total 30 401–30 407）；
    server inproc 10 866–11 963 / 5.0–6.2，spawn 10 415–10 416 / 4.66–4.90；
    socket 阻塞读**既不更便宜也不更贵**（契约 §9 未兑现）；spawn server waits 少 8.9–12.9% 归因 handoff 纪律；
    唯一略差列 `clipark/f` 6.07 vs 5.60/5.75。
  - 门 8②③：OpenRA `seg` 130.8 KB median / 8.01 MB steady mean（含负载 8.07 MB）、记录 211 / 206；
    rd12 `seg` 816.2 KB median / 1.53 MB steady mean（含负载 10.79 MB、首帧 981 654 draws vs 稳态 ~1471、
    run-wide mean 约 7× 稳态，两个读法都报）、记录 7 497 / 7 504；`maxrec=1808` 与分块前相同。
  - 证据链接三条按任务书给出的相对路径写入。

### 1.3 里程碑列表（第 228–229 行）

采用任务书给的**改写旧条**方案（文件惯例是里程碑与阶段表的日期条目一一对应，新增一条会留下
「未宣布收官」与「已收官」两句并存）：

- 第 228 行：「P6 全部包落地（2026-09-21）」→「**P6 收官（2026-09-22）**」。十包合入、三种传输
  retrace 对上 golden、device-lost 闩 123 s → 5.7 s、`Session::Fail` 93→4 等历史事实逐字保留；
  出口门句改为「1–6 ✅；门 7 / 8 于 2026-09-22 采齐」并写入 tie 判读、monolith 约贵 30%、
  门 8 三数齐、G1 0/0/0/0 且 `.text` 逐字节相同，收尾为「P6 就此收官，未设性能门（只记录）」。
- 第 229 行「终局改判」条内，把「server 进程 `PipeStats` 结构性零」标注为**收尾中已修**
  （否则会与 1.2 的状态列自相矛盾）。该条其余内容与第 230 行「仍是方向……」「再基线检查点」
  逐字未动。

### 1.4 两处「门 8 还在做」的残留说明（第 61、266 行）

这两处若不改，会与新的收官状态直接冲突（一句说门 8 还在改代码、一句说门 8 的量还在补）：

- 第 61 行 P6.5 表格里 `ServerMain.cpp` 的出处列：「正在被门 8 的工作修改，不引行号」→
  「门 8 的收尾修改已落入该文件，本表不引行号」。**仍未引行号**（该表本来就不引，保持原风格）。
- 第 266 行开放问题 11 末句：「MC in-world / Create 的占用分布仍未量（门 8 的树上工作正在补）」→
  「（门 8 已给出两条负载的字节/帧与记录/帧，逐 blob 分布只在 integration 车道取到）」。
  这一改与 `gate8-wire-counters.md` §6.5 / §7.1 一致：逐 blob 分布只从 integration 车道的 JSON dump 拿到，
  retrace 的 snapshot 模式因不走 `cleanUp→Destroy` 拿不到。该问题其余文字（含「仍未答」的
  program archive 与 `draw_vbo` range 尾）逐字未动。

### 1.5 明确**未**改动（逐字保留）

- P6.5 行（第 38 行）与「立即的下一项」整节（第 51–158 行）：其「今天还跑不了」「正在被门 8 的工作修改」
  之类的 P6.5 语境与本次收官无关，除 §1.4 那一处外未动。
- 债务表（第 234–250 行）**整张逐字未动**，含「A3 指纹 → P6.5 wf / nd」「默认 32 MiB staging」
  「max record bytes」等行。
- 其余阶段行（P0…P5f、Ph、P3b/P4b、P7…P13）逐字未动；P5c 历史节、审计清单表、开放问题 1–24
  除 §1.4 那一句外逐字未动。
- 第 226 行「P5f 出口……P6 前提解除，实施未开始」是**当时**的事实陈述，保留（该条的历史价值高于
  与今天的对照，且下行第 228 行已给出实施与收官时间线）。

---

## 2 与基线数字的一致性自查

| 事实 | 写入位置 | 与证据文档是否逐字一致 |
|---|---|---|
| 门 7：18/18 退出 0、36/36 `PINNED`、0 `Fatal` | blockquote（概述）、阶段表、里程碑 | 是（`gate7-device-ab.md` §1 表） |
| 门 7：7.880/7.889 vs 7.876/7.869 = −0.15% | blockquote、阶段表 | 是（§2.2 / §2.3；臂内散布 0.2–4.2%） |
| 门 7 第二会话：4/4 OK、8/8 `PINNED`、0 `Fatal`、7.987/7.989 vs 7.992/8.009 = +0.16% | 阶段表 | 是（`gate8-doorbell-device.md` §1、§4） |
| monolith 约贵 30%（−29.8% / −29.9%） | blockquote、阶段表、里程碑 | 是（`gate7-device-ab.md` §2.4） |
| 门 8①：121.1 waits/帧、5.6–6.1 parks/帧、30 401–30 407 | 阶段表 | 是（两文档 §3 表；5.60/5.75 与 6.07/6.08） |
| 门 8①：server inproc 10 866–11 963 / spawn 10 415–10 416 | 阶段表 | 是（`gate8-doorbell-device.md` §3） |
| 门 8①：spawn server waits 少 8.9–12.9%、`clipark` 6.07 vs 5.60/5.75 | 阶段表 | 是（§3、§3.1、§7） |
| 门 8②③：OpenRA 130.8 KB / 8.01 MB、211 / 206 | 阶段表 | 是（`gate8-wire-counters.md` §6.4） |
| 门 8②③：rd12 816.2 KB / 1.53 MB、7 497 / 7 504、含负载 8.07 / 10.79 MB | 阶段表 | 是（§6.4） |
| rd12 首帧 981 654 draws、run-wide mean 约 7× 稳态 | 阶段表 | 是（§6.4 读法注意） |
| `maxrec=1808` 与分块前相同、默认 8 MiB chunk 未被触发 | 阶段表 | 是（§6.4 / §7.2） |
| 收尾四项（878 / 1188 / ServerMain Init+Shutdown / 角色派生 / 工具与 pin 条目） | 阶段表落地列 | 是（`gate8-wire-counters.md` §3、§9.2、§9.7、§9.7.1） |
| 修前 0 条 / 修后 29 条汇总行、SSIM 1.0 不变 | 阶段表落地列 | 是（§9.4(c)） |
| 本板 GPU 钳 1050 MHz（写 0 读回 1、`max_gpuclk` 只读 1 050 000 000）、与 `35d0befa` 行不可比 | 阶段表状态列与验收门列 | 是（`gate7-device-ab.md` §1、§5.1 与 `pin_device.sh` 的 `2f7cbe2e` 行） |

**未写入**（任务书未列、且属于证据文档范围的数）：负控 `MOBILEGL_IPC_STAGE_MB=1` 的
`seg` 运行总量逐字节相同 / `wrec` +0.11% / 同 A/B 下 `tex` +194%（未解释）、逐 blob 分布首测
（70 records、8 blobs、bucket 3–12）、G1 `.text` 10158850 与 symbols 27863。这些留在
`notes/p6/gate8-wire-counters.md`，`ROADMAP.md` 只在其开放问题 11 保留了「分块」的既有论述。

---

## 3 引用检查

在工作树根目录按任务书要求运行：

```
$ python scripts/check_doc_citations.py docs/Disaggregated/ROADMAP.md
check_doc_citations: 75 citations in 1 document(s) against HEAD, 0 problem(s)
rc=0
```

（改前同一条命令：`74 citations … 0 problem(s)`；+1 是本轮新写入的 `PipeWireCodec.cpp:1188`。

**该脚本覆盖 `docs/Disaggregated`**：它按参数吃任意 markdown，仓库里就是靠
`docs/Disaggregated/*.md` 调用的。为完整性另跑了一次全域：

```
$ python scripts/check_doc_citations.py docs/Disaggregated/*.md docs/Disaggregated/notes/p6/*.md
check_doc_citations: 1257 citations in 21 document(s) against HEAD, 27 problem(s)
rc=0
```

那 27 条**全部在只读证据文档里，与本轮改动无关**，且分三类：

1. **21 条 basename 歧义**（`Init.cpp` 命中 `MobileGL/Init.cpp`、`MG_Backend/Init.cpp`、`MG_Impl/Init.cpp`；
   1 条 `Core.cpp`）——脚本按 basename 解析时要求唯一。
2. **5 条 `path:line` 超出 HEAD 行数**——`PipeStats.h` / `PipeStats.cpp` / `ServerMain.cpp` /
   `PipeWireCodec.cpp` 的行号来自**工作树**（未提交的收尾改动），而脚本对 `HEAD` 解析。
   `notes/p6/gate8-wire-counters.md` §3 自己声明的就是「行号为改后内容」。
3. **2 条 `retrace_main.cpp` 第 258 / 1482 行的引用无此文件**——该文件属于 apitrace 子模块的 vendored 源码，
   不在 `git ls-tree -r HEAD` 的顶层路径集合里（脚本只解析仓库自身文件）。

按任务书要求，这些只读证据文档**未改动**，问题如实列出；`ROADMAP.md` 自身 0 problem。

---

## 4 边界与未做项

1. **没有 `git commit`**，也没有触碰工作树里已有的未提交改动
   （`MobileGL/MG_Remote/{Wire/PipeWireCodec.cpp,Server/ServerMain.cpp}`、
   `MG_Util/{Metrics/PipeStats.cpp,Metrics/PipeStats.h,Debug/Log.cpp,Debug/Log.h}`、
   `MG_Test/Util/PipeStatsTest.cpp`、`tools/device_bench/pin_device.sh`、
   `tools/trace_replay/run_android_retrace_local.py` 与新增的 `tools/device_bench/p6/`）。
   改动的只有 `docs/Disaggregated/ROADMAP.md`（6 行）与本报告文件。
2. **另两份文档未碰**：`CURRENT_STAGE_PROGRESS.md`（含 §2.1「收官前另欠」、§3「出口门 7、8 …… 待采集」）
   与 `MEASUREMENTS.md` 由并行代理负责；本报告不引用它们的行号，`ROADMAP.md` 里对
   `CURRENT_STAGE_PROGRESS.md` / `MEASUREMENTS.md` 的既有链接逐字保留。**注意**：在它们被同一轮收官改完之前，
   `ROADMAP.md` 说的「P6 已收官」与 `CURRENT_STAGE_PROGRESS.md` §2.1 / §3 的「未宣布收官 / 7、8 待采」
   会短暂并存，这是并行分工的中间态，不是本报告遗漏。
3. **里程碑采用「改写旧条」而非「新增一条」**（任务书允许二选一）：这样同一列表里不存在
   「未宣布收官」与「已收官」两句。若并行代理在 `CURRENT_STAGE_PROGRESS.md` 里选择了另一种写法，
   需要把两处措辞对齐时以 `ROADMAP.md` 第 228 行的口径为准。
4. **`git status` 里 12 条 ` m` 子模块与 flatbuffers 的 6 条 ` M` 是既有 stat 噪声**
   （见 `notes/p6/git-worktree-fix.md` §7.1），与本轮改动无关，未处理。
