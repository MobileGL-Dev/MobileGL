# docs-closeout-measurements — P6 收官事实写入 `MEASUREMENTS.md`

> 负责文档：`docs/Disaggregated/MEASUREMENTS.md`（唯一被改的目标文件）。
> 报告文件：本文件。任务书要求「只改你负责的那一份文档，另外两份由并行代理负责」——
> 另一个代理的文档（`CURRENT_STAGE_PROGRESS.md`）与第三份**没有被触碰**。
> 工作树：`feat/disaggregated`，**未 `git commit`**，未动任何既有未提交改动。
> **头号勘误**：任务书写的分支头是 `71aa9951`，而写报告时 `git rev-parse HEAD` 读到的是
> `2bd86664`（`71aa9951` 是它的祖先，另有 `2d86e07a` 一个提交夹在中间，两者都是 P6.5 的文档/传输
> 提交，不涉及本次改动）。任务书同时把 `71aa9951` 标为门 8 的源码头——本文件里的源码头就是这样记的，
> 与分支头无关。

## 0 一句话

`MEASUREMENTS.md` 新增 **§12「P6 出口测量（2026-09-22，Redmi `2f7cbe2e` + WSL 桌面）」**（7 个子节：
12.1 协议 / 12.2 门 7 / 12.3 门 8① / 12.4 门 8②③ / 12.5 负控 / 12.6 收尾代码与工具 / 12.7 不可比项与未跑项），
`git diff --numstat` = **176 行新增 / 1 行替换**（文件 599 → 774 行）；并把 §6.3 末尾
「分块后的逐 blob 字节分布尚无新测量」一句**在保留历史语境的前提下**改为指向 §12.4。
除此之外全文逐字未动。本代理只写了两个文件：`MEASUREMENTS.md` 与本报告；
`CURRENT_STAGE_PROGRESS.md` / `ROADMAP.md` 的 `M` 状态由并行代理产生，**不是本代理改的**。

## 1 改动的段落（逐处）

| # | 位置 | 改动 | 为什么 |
|---|---|---|---|
| 1 | §6.3 末句（原 `:342`） | `上段的 maxrec 数字是分块前的测量，分块后的逐 blob 字节分布尚无新测量。` → `…当时分块后的逐 blob 字节分布尚无新测量；该分布与分块后的 maxrec 已在 §12.4 补测——结论是默认 8 MiB chunk 预算在已测的两条负载上从未被打到，maxrec 仍是 1808。` | 任务 (2)。**历史语境保留**（「当时…尚无」），只追加指向新节的结论；该句是 `CONTRACT-P6.md` §9 item 8 与 `CURRENT_STAGE_PROGRESS.md` 引用「分块后分布不存在」的那个锚点，所以不能删掉原陈述 |
| 2 | 文件末尾 | 新增 `## 12. P6 出口测量（2026-09-22，Redmi 2f7cbe2e + WSL 桌面）` | 任务 (1) |

## 2 §12 的内容结构

| 子节 | 内容 |
|---|---|
| §12.1 协议 | 设备 / reboot-clean（boot id 前后比对）/ CPU 定频 little 1555200 + big 1958400 / GPU 钉 pwrlevel 0 / 风扇 / 同热窗口臂交错 / 负载 rd12 DirectGLES 251 帧 1471.36 draws/帧 / `--benchmark-repeats 3`·`2` + `--benchmark-tail-frames 200` + `--benchmark-no-finish` / split 开关 / 源码头 / 臂的证明。**帧分母取自哪条日志**单列一段：`frames=` 由 `PipeStats::OnPresent` 递增，spawn 下 present 在 server 进程应用，故 spawn client `frames=0 window=0`；费率一律除以真正数到帧的那条日志的 `frames=`（inproc → client log，spawn → server log，两者都 251），**不得除以同行的 `window=`**（本负载会虚高约 21×）。同时写明三个结构性零（spawn client 的 `srv=0 srvpark=0`、spawn server 的 `cli=0 clipark=0`、spawn client 的 per-frame 字段）与「`* total ms` 含第 1 帧 shader 编译（7332 ms）、不得除以 251」 |
| §12.2 门 7 | 会话 1（boot id `e69d0329…`→`107f24d3…`）六臂交错 × best-of-3 的**逐臂表**（wall p50 / client CPU p50 / fps / 臂内散布），18/18 runner 退出 0、36/36 `PINNED`、0 `Fatal{`、37.6–46.5 °C；会话 2（`107f24d3…`→`7e6c6e9f…`）inproc/spawn 交错 × best-of-2 的**逐臂表**，4/4 OK、8/8 `PINNED`、0 `Fatal{`、37.2–44.9 °C；两会话的配对差表（**两个顺序都列**）；monolith 基线行；判读 **tie** |
| §12.3 门 8① | 会话 2 的**完整四计数器账**（含 `counters 取自` / `frames 取自` 两列）+ 会话 1 的账（spawn 的 server 半边标 **不可得**，并写明这是 §12.6 修复前的结构性问题、不是漏采）；逐列读法与 hole 判读 |
| §12.4 门 8②③ | `SEG_STAGE` 字节/帧与分块后记录/帧的**稳态中位数 / 稳态均值 / 含启动负载均值三个 framing 都报**；`maxrec=1808` 分块后不变与其原因；`ringwraps`/`ringpads`/`ringwaits`；`seg` 与后端字节类的逐窗交叉核对（含「能排除什么、不能排除什么」）；**逐 blob 分布首测**（70 records / 8 blobs / bucket 3–12，逐桶列出） |
| §12.5 负控 | `MOBILEGL_IPC_STAGE_MB=1`（256 KiB chunk 预算）的 A/B 表：**`seg` 运行总量两侧逐字节相同（2 698 026 763）**、**`wrec` +0.11%**、稳态中位数 −32%（逐窗归属会动、总量不会）、`tex` +194% 记为**证据不作结论**（arena 大小与 chunk 预算绑在同一个 env 变量上，未隔离到单变量） |
| §12.6 收尾代码与工具 | `StageSegmentBytes`（`PipeWireCodec.cpp:878`）/ `WireRecords`（`:1188`）/ staged-blob 直方图 / `ServerMain.cpp` 的 `Init()`+`Shutdown()`（**红一次：0 条 → 29 条汇总行，SSIM 1.0 不变**）/ `MOBILEGL_PIPE_STATS_FILE` 角色分路 `/ <base>.client.json` `/ <base>.server.json`（含 G1 三次泄漏点）/ 测试数（PipeStats 25/25、全量 unit 2367/2367、spawn integration 102/102、G1 0/0/0/0 且 `.text` 逐字节相同）/ 工具入库 / `pin_device.sh` 的 Redmi 行 |
| §12.7 不可比项与未跑项 | **本板 GPU 钳 1050 MHz、与 1100 MHz 时代的 `35d0befa` 行不可比**；**OpenRA 真机辅负载未跑**；门 8① 只在一负载一设备上比过；spawn client 窗口化字段是结构而非缺陷；分块净效果未单变量隔离 |

## 3 与事实基线的逐条对照（自检）

| 事实 | 落点 | 一致 |
|---|---|---|
| 门 7 会话 1：18/18、36/36 PINNED、0 Fatal；inproc 7.880/7.889、spawn 7.876/7.869（−0.15%）、臂内散布 0.2–4.2%、monolith 约贵 30% | §12.2 | ✅ |
| 门 7 会话 2：4/4、8/8、0 Fatal；inproc 7.987/7.989 vs spawn 7.992/8.009（+0.16%） | §12.2 | ✅ |
| 判读 tie、不设门 | §12.2 末 | ✅ |
| 门 8① client 121.1 waits/帧、两传输逐字相同、run total 30 401–30 407；server inproc 10 866–11 963 / spawn 10 415–10 416；park 率 0.042–0.06% | §12.3 | ✅ |
| 契约 §9「阻塞读可能更便宜」未兑现（既不更便宜也不更贵） | §12.3 tie 判读 | ✅ |
| spawn server waits 少 8.9–12.9% 归因于 handoff 纪律而非门铃 | §12.3 `srv/f` 与同节解释段 | ✅ |
| clipark/f 是唯一 socket 略差的列（6.07 vs 5.60/5.75，每帧多约 0.4 次 park） | §12.3 `clipark/f` | ✅ |
| 读法警告（srv/cli 的零＝另一个进程；spawn client frames=0、per-frame 无效而 `cli`/`clipark` 可信；终行/0 行） | §12.1 + §12.3 | ✅ |
| OpenRA seg 130.8 KB median / 8.01 MB steady mean（含负载 8.07 MB）、记录 211/206；rd12 seg 816.2 KB median / 1.53 MB steady mean（含负载 10.79 MB）、记录 7 497/7 504 | §12.4 表 | ✅ |
| framing 警告（rd12 帧 1 有 981 654 draws；run-wide mean 约 7× 稳态；两个读数都报） | §12.4 表 + 警告段 | ✅ |
| maxrec=1808 与分块前相同、默认 8 MiB 预算从未被打到 | §12.4 | ✅ |
| 负控 `STAGE_MB=1`：seg 逐字节相同 2 698 026 763、wrec +0.11%、tex +194% 记为证据不作结论 | §12.5 | ✅ |
| 逐 blob 分布首测 70 records / 8 blobs / bucket 3–12，integration 车道，retrace snapshot 拿不到 dump | §12.4 末 | ✅ |
| 收尾代码：`PipeWireCodec.cpp:878` / `:1188`、`ServerMain.cpp` Init/Shutdown（红一次 0→29 行）、`MOBILEGL_PIPE_STATS_FILE` 角色派生、测试数、工具入库 | §12.6 | ✅ |
| 本板 GPU 钳 1050 MHz、与 `35d0befa` 的 1100 MHz 时代不可比 | §12.1（协议行）+ §12.7 | ✅ |
| OpenRA 真机未跑 | §12.7 | ✅ |

**未写进去的**（不在本任务范围，留给并行代理）：门 1–6 的逐项复述、`ROADMAP.md` 的阶段状态、
`CURRENT_STAGE_PROGRESS.md` 的出口门表。§12 开头只写「出口门 1–6 的逐项证据见
`CURRENT_STAGE_PROGRESS.md`」并给一行，不复述。

## 4 引用检查输出（工作树根目录）

```
$ python scripts/check_doc_citations.py docs/Disaggregated/MEASUREMENTS.md
check_doc_citations: 4 citations in 1 document(s) against HEAD, 0 problem(s)
exit=0

$ python scripts/check_doc_citations.py --strict docs/Disaggregated/MEASUREMENTS.md
check_doc_citations: 4 citations in 1 document(s) against HEAD, 0 problem(s)
exit=0
```

（改动前该文档是 `0 citations … 0 problem(s)`——因为是新加的引用，所以这是新增的 4 条，而不是存量。）

新增的 4 条引用都是 `path:line` 形式，全部解析成功：`MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:878`
与 `:1188`（各出现两次，§12.4 与 §12.6）。**覆盖范围说明**：本脚本只匹配
`文件.ext:行号` 形态（扩展名限于 `h/hpp/cpp/cc/c/py/def/inc/md/json/yml/yaml/txt/fbs`），
**不检查** `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` 这类花括号缩写、也不检查
`MobileGL/MG_Remote/Server/ServerMain.cpp` 这种**不带行号**的文件引用——这两处是照 §3 的行号风格
写的（`ByteClass` 枚举成员与 `CallClass` 枚举成员的位置由 `PipeStats.h`/`.cpp` 共同承载，写成 `:878`
反而是唯一收口点，`ServerMain.cpp` 的承重位置是「配置解析与定角色之后 / `loop.Stop()` 之后」）。
行号取自**收尾改动后的工作树**（`PipeWireCodec.cpp` 工作树 2335 行 vs HEAD 2299 行），不是 HEAD：
这是刻意的，因为 §12.6 描述的就是工作树里那份改动，已在 §12.4 正文里写明「两处行号取 §12.6 收尾改动后的工作树内容」。

全文档车道（CI 的调用形态）也一并跑了，用于确认本次改动没有引入新问题：

```
$ python scripts/check_doc_citations.py docs/Disaggregated/*.md
check_doc_citations: 183 citations in 11 document(s) against HEAD, 1 problem(s)
  docs/Disaggregated/P6-ENDSTATE-REVIEW.md:284: `Init.cpp:184-211` -> ambiguous: MobileGL/Init.cpp,
  MobileGL/MG_Backend/Init.cpp, MobileGL/MG_Impl/Init.cpp
exit=0
```

那 1 条**不是本次引入**，它在 `P6-ENDSTATE-REVIEW.md`（另一个代理负责的只读文档），
且 CI 对这条 lint 是 `|| true` 的 warning-only
（`.github/workflows/test.yml:3096-3101`，注释写明「文档还在写，等定稿再变 `--strict`」）。

## 5 遗留 / 需要下游注意

1. **两个事实数在 `docs/Disaggregated/MEASUREMENTS.md` 与 `CURRENT_STAGE_PROGRESS.md` 之间会重复**——
   本文件按任务书写了「本节记录门 7 / 门 8 …出口门 1–6 的证据见 `CURRENT_STAGE_PROGRESS.md`」，
   请负责那份文档的代理确认两处措辞不冲突（数字本身逐字一致）。
2. **§12.6 的行号指向工作树、不是 HEAD**，已在正文里写明。合并/提交这次收尾改动后行号可能移动；
   届时按 §6.3 的既有风格（引用只在 `path:line` 无法替代的位置使用）复核那两处即可。
3. **`tools/device_bench/p6/README.md` 的已知限制段落已过时**：它还写着「Where that call is absent」
   （即 `PipeStats::Init()` 可能缺失），而 §12.6 的收尾改动已经把它加进 `ServerMain.cpp`。
   该文件不是本代理负责的文档，**未改**；留给后续一并订正。
4. **本次未跑任何构建或测试**：这是文档任务，改动只有 markdown 文本。§12 中的测试数
   （PipeStats 25/25、unit 2367/2367、spawn integration 102/102、G1 0/0/0/0 与 `.text` 逐字节相同）
   与设备数全部照任务书给的事实基线**逐字转写**，**未重算、未重跑、未挑好看的**。
   文件里的设备数按只读证据文档核对过（`gate7-device-ab.md`、`gate8-doorbell-device.md`、
   `gate8-wire-counters.md`），并把两处证据文档里互相略有出入的四舍五入统一到证据文档自身的口径
   （例如 §12.3 的 `srvpark/f` 与 park 比例统一按任务书的 **0.042–0.06%** 报；会话 2 四臂逐臂算出来是
   0.042–0.047%，并入会话 1 的 inproc 两臂后上界到 0.056%，正文里把逐臂数都列了、并声明了上界来源）。
5. **未触碰**：`CURRENT_STAGE_PROGRESS.md`、`ROADMAP.md`、`ARCHITECTURE.md`、`README.md`、
   三份只读证据文档（`notes/p6/gate7-device-ab.md`、`gate8-doorbell-device.md`、`gate8-wire-counters.md`）、
   `notes/p6/git-worktree-fix.md`，以及任何 `MobileGL/` 源码与 `tools/` 脚本。
