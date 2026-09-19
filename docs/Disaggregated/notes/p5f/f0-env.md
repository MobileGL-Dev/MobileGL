# P5f / f0 — 构建与门禁机制侦察（实现代理 cheatsheet）

> 只读普查产物。仓内引用以 `feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`）为准；
> WSL 侧文件不在仓内，引用给绝对路径。未发起任何构建。

---

## 1 WSL 侧布局：树在哪、是什么关系

所有 WSL 树都是**同一个仓的 git worktree**（不是拷贝）。主 gitdir 在 `/home/swung/mgl/MobileGL`，
`git worktree list` 在任何一棵树里都能看到全部（证据：在 `~/w7/p5e-int` 里执行
`git worktree list` 列出 pipe / p5-* / p5e-* / land 等 20 棵树）。

| 树 | 分支 @ HEAD | 角色 |
|---|---|---|
| `~/w7/pipe` | `feat/disaggregated @ 11ac3de6` | **集成树**。比 Windows HEAD 落后 105 个提交（`11ac3de6..HEAD`），P5c 开工时的位置。`docs/Disaggregated/notes/p0/BRIEF-P0.md:13`："DO NOT edit `~/w7/pipe` or `~/w7/base` - they are read-only references" |
| `~/w7/base` | `dev @ 81b17c0b` | **性能锚点**，dev 分支，不动（`docs/Disaggregated/notes/p1/BRIEF-P1.md:3`） |
| `~/w7/land` | `dev @ 81b17c0b` | 与 base 同 commit，带 `build-android/` 目录。用途未见诸阶段文档——**未知**（从目录形态看是 dev 的 Android 构建树；P5f 用不到） |
| `~/w7/p5e-int` | `p5e-wave2-landed @ f23fbc1b` | P5e 集成树，**已过时**，见 §6.1 |
| `~/w7/p5e-<slug>` | `p5e/<slug>` | P5e 各包树（c0e/fb/fix1/gl/id/mv/pa/pg/ra/ra2/sb/tx2/vi），每棵一个包 |
| `~/w7/p5e-logs/` | — | p5e_gate.sh 的日志输出目录（每步一个 `<slug>-<step>.log`） |

每棵树的 remote 都有两个：`origin = /home/swung/mgl/MobileGL`（WSL 内部主仓），
`winrepo = /mnt/c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL`（Windows 侧）。

**Windows 侧关系**：工作区 `MobileGL-disagg` 是 `FoldCraftLauncher` 仓里 `MobileGL` 子模块的
worktree（`MobileGL-disagg/.git` 文件指向
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/.git/modules/MobileGL/worktrees/MobileGL-disagg`）。
Windows 侧（在该 worktree 执行 `git remote -v`）配了 **`wslpipe = //wsl.localhost/Arch/home/swung/w7/pipe`**，
以及 `origin = github.com/MobileGL-Dev/MobileGL.git`、`hitmoe`、`fu3`。

### 1.1 代码怎么同步（非对称，两个方向机制不同）

- **Windows → WSL**：按 SHA fetch。`~/w7/notes/tools/p5e_new.sh` 的核心三行：
  ```bash
  cd ~/w7/pipe
  git fetch -q /mnt/c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/.git/modules/MobileGL "$SHA"
  git branch -f "p5e/$SLUG" "$SHA" && git worktree add ~/w7/p5e-$SLUG "p5e/$SLUG"
  ```
  前提是**先在 Windows 侧 commit**——WSL 看到的是 gitdir 里的对象，工作区改动过不去。
  p5e_new.sh 随后做 `git submodule update --init --recursive`、补拷
  `3rdparty/glslang/External`（worktree 不带子模块的嵌套 External）、写
  `.git/info/exclude` 加 `.p5e/`，然后直接调 `p5e_gate.sh $SLUG configure` + `build`。
- **WSL → Windows**：在 Windows worktree 里 `git fetch wslpipe <branch>`。
  出处：`docs/Disaggregated/notes/p5e/FLIP-CHECKLIST.md:3-5`——"Integration tree
  `~/w7/p5e-int`（branch `p5e-wave2-landed`, fetched to the Windows worktree
  `MobileGL-disagg` via remote `wslpipe`, pushed to `origin/feat/disaggregated`）"。
- **LFS 纪律**（trace fixture）：`git worktree add` 不 hydrate LFS，新树里 41 个 fixture 有 40 个是
  130 字节指针。从 `~/w7/pipe/tools/trace_replay/fixtures` **拷贝**，并标 assume-unchanged；
  **永远不在 WSL 里 `git lfs pull` / `git lfs checkout`**（额度是既有规矩，且 checkout 会把真文件
  覆盖成指针）。出处：`~/w7/notes/tools/wsl_p5_gate.sh` Part 2 头注释（"FIXTURES FIRST…"段）。
- `~/w7/notes/` 本身已搬家：`~/w7/notes/MOVED-TO-REPO.txt`——415 篇 .md 已入仓
  `docs/Disaggregated/notes/<phase>/`（commit `2acea1b1`）；留在 WSL 的只有脚本、日志、census
  数据、patch、APK、nm dump（含绝对路径与设备序列号，不入仓）。

### 1.2 现成脚本（`~/w7/notes/tools/`）

| 脚本 | 作用 |
|---|---|
| `p5e_new.sh <slug> <sha>` | 按 SHA 建 `~/w7/p5e-<slug>` 包树并 configure+build（见 §1.1） |
| `p5e_gate.sh <slug> <step> [regex]` | **P5e 门禁本体**，step ∈ configure/build/unit/isplit/gpu/strict/flavours/gens/one，逐条见 §4 |
| `wsl_p5_gate.sh [quick]` | P5 时代的五段式大门（env `MGL_P5_TREE` 指定树），四 flavour 全建 + retrace 扫描；`flavours` 的 verify 臂只在它这里有（§3） |
| `wsl_p4a_gate.sh` / `wsl_p3a_gate.sh` / `wsl_p2_gate.sh` 等 | 各阶段同构老门，仅供考古 |
| `wsl_integrate.sh` / `p5_merge_*.sh` / `p5_land_*.sh` | 各阶段合并/落地脚本，阶段专用 |

WSL 工具链事实（`docs/Disaggregated/notes/p1/BRIEF-P1.md:3`）：clang 22.1.6 在
`/usr/sbin/clang++`（没有 `clang++-20`）；28 核；ccache 配 `CCACHE_BASEDIR=/home/swung/w7`。

---

## 2 仓内门脚本（`scripts/` 与 `scripts/ci/`）

全部入口都是 `python3 scripts/xxx.py` / `bash scripts/ci/xxx.sh`，在仓根跑。P5e gate 的 `gens`
步（`~/w7/notes/tools/p5e_gate.sh`）就是这套的最小全集：

```bash
python3 scripts/gen_pipe.py --check                    # G1..G7 七个生成器，产物已提交，重生成有 diff 即红
python3 scripts/gen_pipe.py --self-test                # 生成器自己的阴性对照
python3 scripts/gen_pipe_field_ownership.py --check    # 70 行归属表（63 字段 + 7 sticky）每行恰好一类，有洞即红
python3 scripts/gen_pipe_field_ownership.py --self-test
python3 scripts/gen_pipe_field_ownership.py --print-admitted   # 打印"允许 admitted 的 <field>@<verb>"，strict 棘轮的另一臂
python3 scripts/check_include_closure.py               # 三个头的 include 闭包（value/artifacts/wire 三 probe）
python3 scripts/gen_pipe_dirty_surface.py --check      # 脏面扫描：mutator→aggregate generation 映射
python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/check_doc_citations.py --rev HEAD --strict docs/Disaggregated/*.md MobileGL/MG_Remote/CONTRACT-P5*.md
```

细节：

- `scripts/gen_pipe.py`（文件头 docstring）：读 `PipeCalls.def / PipeFields.def / Coverage.def /
  FillPoints.def` + `scripts/data/backend_read_inventory.md`，写 `MobileGL/MG_Pipe/generated/*.inc`，
  产物是**提交的**，CI 重生成比对。无参 = 写文件；`--check` = 只校验；`--self-test` = 阴性对照。
- `scripts/gen_pipe_field_ownership.py`（文件头）：RECORD_SUPPLIED 是**推导**出来的
  （Coverage.def emitted − PipeFill.cpp 拒绝表），所以映射文件不可能与 emitter 漂移还保持绿。
- `scripts/check_include_closure.py`（文件头）：`--mode text|clang|both`，ctest 里跑的是 text
  （CI test job 的 runner 没子模块）；`--require-all` 把 SKIP 变红（棘轮）。
- `scripts/gen_protocol.py`：flatc 解析顺序 `--flatc` / `MOBILEGL_FLATC_EXECUTABLE` → 从
  `3rdparty/flatbuffers` 源码建到仓外 `<repo>/../flatc-build`（`MOBILEGL_FLATC_BUILD_DIR` 可改）。
- `scripts/check_doc_citations.py`：解析 md 里的 `path:line` 引用对 git rev 校验，只有
  `--strict` 才非零退出。
- `scripts/ci/split_negative_controls.sh [--self-test]`：E1（verb barrier）与 E3(a)（persistent-map
  push）的阴性对照，是 `.github/workflows/test.yml:1097` 那个 step 的本体。环境变量
  `CTEST`（默认 ctest）等覆盖点在文件头 usage 段。两条规矩（文件头）：基线必须绿（数 PASSED）、
  红必须带被选条目自己的失败文本。
- `scripts/ci/control_smoke_test.sh`：用 stub ctest 跑上面真脚本，验证"该红时红"（R-16）。
- `scripts/ci/redcheck_control_smoke_test.sh`：smoke test 自己的 red-once（临时 revert 证据检查）。
- `scripts/ci/retrace_pull_library_control.sh` / `retrace_drop_draw_control.sh`：retrace-split
  车道的两个阴性对照（pull 库必须红于 transport 断言；丢 draw 必须红于 SSIM）。retrace 本体是
  WSL 侧 `~/w7/retrace_gate.py`（不在仓内）。
- `scripts/ci/census_junit.py` / `junit_tally.py`：JUnit XML 统计辅助。
- CI 接线点：`.github/workflows/test.yml:1097`（negative controls）、`:1249`
  （`--print-admitted`）、`:2568-2576`（gen_pipe）、`:2616-2617`（dirty surface）、
  `:2637-2638`（field ownership）、`:2736`（doc citations，`|| true` 非 strict）。

---

## 3 四个构建 flavour 与 CMake 强制规则

开关定义在仓根 `CMakeLists.txt`：

| 开关 | 行 | 含义 |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | `CMakeLists.txt:23` | 编译 MG_Remote 传输层（两进程形态） |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | `CMakeLists.txt:30` | 编译 inproc 角色隔离 shim；蕴含上一项（`:460-463`） |
| `MOBILEGL_PIPE_PUSH` | `CMakeLists.txt:35` | 后端经 PipeInputs 块读前端状态 |
| `MOBILEGL_PIPE_VERIFY` | `CMakeLists.txt:36` | 编译 SnapshotFromGLContext + G4 逐 verb 影子比对器；蕴含 PUSH（`:506-508`），永不发布 |

**强制规则（全是普通变量遮蔽，不写 cache——所以 CMakeCache 不可信，见 ID-124）**：

- `DISAGGREGATED=ON` 缺 flatbuffers 子模块 → 静默降级为 OFF（`:471-486`）。
- **`DISAGGREGATED=ON` 强制 `PIPE_PUSH=ON`（`:496-500`）——不存在 "disaggregated pull"**。
  同时传 `-DMOBILEGL_PIPE_PUSH=OFF` 会被静默覆盖，cache 里仍显示 OFF。
- `PIPE_PUSH=OFF` 强制 `PIPE_LEGACY_MEMOS=ON`（`:513-517`）——pull 构建里 legacy 臂是唯一臂。
- 生效宏落点：`:652-659`（`-DMOBILEGL_BUILD_DISAGGREGATED=1` / `-DMOBILEGL_PIPE_PUSH=1` /
  `-DMOBILEGL_PIPE_VERIFY=1`）。

四个 flavour 的配法（`~/w7/notes/tools/wsl_p5_gate.sh:63-66` 与 `p5e_gate.sh` 的 flavours 步）：

| flavour | 构建目录 | 开关 |
|---|---|---|
| pull | `build-linux`（老门）/ `build-pull`（p5e） | `DISAGGREGATED=OFF INPROC=OFF PIPE_PUSH=OFF` |
| push | `build-push` | `DISAGGREGATED=OFF INPROC=OFF PIPE_PUSH=ON` |
| verify | `build-verify` | `PIPE_VERIFY=ON` + `MOBILEGL_ITEST_REQUIRE_GPU=ON`（只在 wsl_p5_gate.sh 里有；p5e_gate.sh 不建它） |
| split | `build-split` | `PIPE_PUSH=ON DISAGGREGATED=ON DISAGGREGATED_INPROC=ON` |

**ID-124 教训（p5e_gate.sh flavours 步头注释原文）**：每个 flavour 配完后必须先从
`compile_commands.json` 里 grep 生效宏再放行构建——cache 条目会说谎：

```bash
got=$(grep -o 'MOBILEGL_PIPE_PUSH=1' "build-$fl/compile_commands.json" | head -1)
# pull 要 absent，push 要 1，不等就 ARM CHECK FAILED
```

公共 configure 串（p5e_gate.sh 的 `$COMMON`）：`-G Ninja`、clang/clang++、ccache、
`-DCMAKE_BUILD_TYPE=Release`、`-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO`、
`-DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_BUILD_BENCHMARK=OFF`、
`-DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json`、
`-DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json`、
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5`。

**C-10 教训**（wsl_p5_gate.sh:8-11 头注释）：构建结果要用产物的存在来证明——`cmake --build`
拿错目录时 `$?` 也是 0（cmake 打 usage），所以每个 build 目录先查 `CTestTestfile.cmake`
再采信（`wsl_p5_gate.sh:74`）。

**`~/w7/p5e-int` 实测**（只读查 compile_commands.json）：`build-pull` 无任何宏、`build-push`
有 `MOBILEGL_PIPE_PUSH=1`、`build-split` 有 `PUSH=1 + DISAGGREGATED=1`、`build-nodisagg` 无宏；
**没有 build-verify**。`verify` flavour 在 P5e 只在 retrace 扫描里以
`MOBILEGL_PIPE_VERIFY=1` 环境变量的形式出现（wsl_p5_gate.sh:270）。

---

## 4 车道与 P5e gate 脚本

### 4.1 ctest 标签

| 标签 | 注册处 | 说明 |
|---|---|---|
| `unit` | `MobileGL/MG_Test/**/CMakeLists.txt`（gtest_discover_tests … LABELS unit，如 `MobileGL/MG_Test/Pipe/CMakeLists.txt:215`） | 单测 |
| `integration-split` | `MobileGL/MG_IntegrationTest/CMakeLists.txt:1917-2364` | 拆分车道。大多数条目同时带 `integration-gpu`（`:1917` 等 `LABELS "integration-gpu\;integration-split"`）；**`integration-split` 不等于 "split transport"**——`:1944` 注释明说 monolith 臂也带这个标签。strict 专用条目只带 `integration-split`（`:2339, :2349`）。`integration-split-clientarrays` 会被 `-L integration-split` 匹配（子串，`:2206-2209`） |
| `integration-gpu` | 同上 | GPU 车道。`MOBILEGL_ITEST_REQUIRE_GPU` 把"无 GPU 全 skip"从绿变红，否则该标签不可证伪（`MobileGL/MG_IntegrationTest/CMakeLists.txt:280-285`） |
| `integration-verify` | `:1695-1726` | verify comparator 车道（`-DMOBILEGL_PIPE_VERIFY=ON` 构建里跑） |

### 4.2 p5e_gate.sh 各 step（`~/w7/notes/tools/p5e_gate.sh`，逐字摘录的命令）

用法：`bash ~/w7/notes/tools/p5e_gate.sh <slug> <step> [regex]`，在 `~/w7/p5e-<slug>` 里跑，
日志落 `~/w7/p5e-logs/<slug>-<step>.log`。集成树就是 slug=int（FLIP-CHECKLIST.md:5：
"`G` = `bash /home/swung/w7/notes/tools/p5e_gate.sh int <step>`"）。

| step | 命令本体 |
|---|---|
| `configure` | `cmake -S . -B build-split $COMMON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_PIPE_PUSH=ON` |
| `build` | 无 cache 先 configure；`cmake --build build-split -j $(nproc)` |
| `unit` | `GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ctest --test-dir build-split -L unit -j $(nproc) --output-on-failure`（GLIBC_TUNABLES 是 ID-18/21：让 exit-order UAF 可复现） |
| `isplit` | `ctest --test-dir build-split -L integration-split --no-tests=error -j 4 --output-on-failure` |
| `gpu` | `ctest --test-dir build-split -L integration-gpu --no-tests=error -j 4 --output-on-failure`。**不是可选**：它是 push 构建的 monolith 运行时臂（ID-109 的洞——137 个 SEGFAULT 在 38 个 scenario 上无门通过，fix1/ID-107）；-j4 约 10 分钟，定为集成分支步骤而非每包步骤 |
| `strict` | `MOBILEGL_IPC_STRICT_ERRORS=1 ctest --test-dir build-split -L integration-split --no-tests=error -j 4 --output-on-failure`，然后 `grep -ho 'Fatal{UnmigratedPipeInput, "[^"]*"}'` 做首错直方图。**strict 是环境变量车道，不是独立 ctest 标签** |
| `flavours` | rm -rf 重建 build-pull / build-push，每个先过 compile_commands.json 宏断言再构建（§3 ID-124） |
| `gens` | §2 列的那串 + `check_doc_citations.py --rev HEAD --strict` |
| `one <regex>` | `ctest --test-dir build-split -R <regex> --output-on-failure`，附 Fatal 上下文摘录 |

关键运行时环境变量：

| 变量 | 取值/出处 |
|---|---|
| `MOBILEGL_TRANSPORT` | `monolith \| inproc \| spawn \| unix:<path> \| pipe:<name>`（`MobileGL/Config.h:425`）；pull 库里 `inproc` 被静默忽略（`Config.h:436`）——这就是 retrace pull 阴性对照存在的理由 |
| `MOBILEGL_IPC_STRICT_ERRORS` | `MobileGL/ConfigLoader.cpp:393` 读取；BARRIER-PULLED 读由计数升为 Fatal（`MobileGL/Config.h:534`、`MobileGL/MG_Backend/MGPipe/PipeInputs.cpp:183`） |
| `MOBILEGL_IPC_RUN_AHEAD` | `MobileGL/Config.h:508`（P5e，`MG_Remote/CONTRACT-P5E.md` §1） |
| `MOBILEGL_IPC_AUDIT` | `=1` 时读 `0xDD` 毒化即红（BRIEF-P5E.md §3.7） |
| `MOBILEGL_IPC_VERB_BARRIER=0` | **不要拿它当对照**（ID-114，FLIP-CHECKLIST.md Phase E）：它是 `RunAheadArmed()` 的第一合取项，副作用是关掉 run-ahead，且秒死于 `Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@ClientWaitSync"}` |
| `MOBILEGL_ITEST_REQUIRE_GPU` | 见 §4.1；configure 期 option 会注入公共 ENVIRONMENT（`MobileGL/MG_IntegrationTest/CMakeLists.txt:303-306`） |
| `MOBILEGL_LOG_FILE_PATH` | 每个 split 条目一条私有库日志（split_log_paths.py 的职责就是校验私有且互不重名）；库的 Fatal 只在私有日志里——**ctest 控制台 transcript 对库输出是假零**（split_log_paths.py evidence 模式错误信息原文） |

### 4.3 P5e 的落地流程（FLIP-CHECKLIST 摘要）

`docs/Disaggregated/notes/p5e/FLIP-CHECKLIST.md`：Phase A 按 gl→pa→mv 序合并，每次合并后
`G build && G unit && G isplit && G gpu && G gens`；Phase B 翻转前记录四条基线
（`RUN_AHEAD=0` / `IPC_AUDIT=1` 各 116/116、`STRICT_ERRORS=1 ctest -L unit` 2250/2250、
默认臂日志含 "run-ahead requested … running lockstep"）；Phase D 翻转是一个 commit 同时落
`kMGPipeP5eRunAheadReady`（`MG_Backend/Init.cpp:98`）与
`kMGPipeP5eClientWaitRuleLanded`（`MG_Pipe/PipeApply.h:969`）——分开落会死。

---

## 5 strict-expected-markers.txt 刷新流程

文件：`MobileGL/MG_IntegrationTest/Harness/strict-expected-markers.txt`。当前 HEAD 上活着
**10 对** `<field>@<verb>`（`:34-52`：8 对表 admission + 2 对 escalation-only 的
client-vertex-array 条目，P8 的）。

它是双向棘轮（`:11-13`）：表里有的对子若不再出现 → 在退役它的那个 commit 里删行；
出现新对子 → 加行并注明归哪个阶段，或直接退役它。

**刷新流程（`:15-20` 原文，禁止手猜）**：

```bash
ctest -L integration-split --show-only=json-v1 > lane.json
MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L integration-split -j 4
python3 MobileGL/MG_IntegrationTest/Harness/split_log_paths.py markers lane.json . \
    <(python3 scripts/gen_pipe_field_ownership.py --print-admitted) strict-expected-markers.txt
# 把它打印的 census 贴回文件
```

配套事实：

- marker 语法在写出处定义（`split_log_paths.py` 的 `MARKER_RE`）：
  `Fatal{UnmigratedPipeInput, "<F>@<V>"}` 与 `Admitted{UnmigratedPipeInput, "<F>@<V>"}
  [BARRIER-PULLED, ADMITTED[-ESCALATED], retires in <phase>]` 两种形态。
- 日志集来自 `ctest --show-only=json-v1` 而不是目录扫描（ID-119：目录是猜测，F1. readback
  块、NamedBlit、Ct.、PersistentMapArm、DirectVulkan 条目都不在 split-logs/ 目录里）。
- census 是**首错直方图不是清单**（FLIP-CHECKLIST.md Phase B.2）：退役一行后那 62 个条目不会
  变绿，会推进到各自的下一个 pull。
- 重测 census 要按 `ctest -L integration-split -N -V` 逐条驱动环境——`PersistentMapArm.` /
  `SmallRing.` / `NamedBlit.` 前缀带额外 ENVIRONMENT，固定环境的 census 对 14/109 是错的
  （FLIP-CHECKLIST.md Phase B.1）。

---

## 6 P5f 开工前必须知道的三件事

### 6.1 `~/w7/p5e-int` 不能直接用

p5e-int 停在 `f23fbc1b`（branch `p5e-wave2-landed`），它**不是** Windows HEAD `8b68b92c` 的
祖先（`git merge-base --is-ancestor` 判定否）——P5e 收官后 Windows 侧分支被重述过
（同一批提交、中英文两版 message、不同 SHA；`f23fbc1b` 对象本身在 Windows 仓里存在，
`git cat-file -t f23fbc1b` = commit）。P5f 的正确姿势是从当前代码头 `25fba0d5`（或 HEAD
`8b68b92c`，其后的提交只动文档）按 §1.1 新建树：`p5e_new.sh` 的形态照抄，slug 换 `p5f-<x>`。

### 6.2 gate 脚本要复制改造，不要直接复用

`p5e_gate.sh` 硬编码 `~/w7/p5e-$SLUG` 与 `~/w7/p5e-logs`。P5f 应复制为 `p5f_gate.sh`
（树改 `~/w7/p5f-$SLUG`）。P5f 新增的双块车道（P5F-WIRE-COMPLETENESS.md §4，旋钮建议名
`MOBILEGL_IPC_ROLE_SPLIT_STATE=1`）应加成新 step，形态照 `strict` 步（环境变量 + 同一标签 +
marker 直方图）。**阴性对照要写进脚本**：关掉双块必须让至少一条具名用例由绿转红
（P5F §6，ID-122 的教训）——参照 `scripts/ci/split_negative_controls.sh` 的"红必须带被选条目
自己的失败文本"规矩。

### 6.3 门禁的已知盲点都是"未被构建/未被运行的臂"

ID-107/109（运行时臂无门：push 构建的 monolith 臂漏跑 → 137 SEGFAULT）、ID-123（gate 只配
build-split，pull flavour 根本没建）、ID-124（cache 与生效宏不符的伪 pull 构建）、ID-122
（对照要证伪机制本身）。P5f 加任何新车道时按这个清单自查：**配了吗、建了吗、跑了吗、宏对吗、
红过一次吗**。

---

## 7 常用命令速查（Windows 侧发起，WSL 执行）

```bash
# 建 P5f 包树（先在 Windows worktree commit，拿到 SHA）
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_new.sh <slug> <sha>'   # 脚本内路径写死 p5e-，P5f 需先复制改造

# 集成树门禁（P5e 形态，P5f 复制后替换 slug 前缀）
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int build'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int unit'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int isplit'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int gpu'       # 集成分支步骤，非每包
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int strict'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int flavours'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int gens'
wsl.exe -e bash -lc 'bash ~/w7/notes/tools/p5e_gate.sh int one <regex>'

# 仓内门（Windows 侧 Git Bash 也能跑，与 WSL 等价）
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/gen_pipe_field_ownership.py --check && python3 scripts/gen_pipe_field_ownership.py --self-test
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/check_include_closure.py
python3 scripts/check_doc_citations.py --rev HEAD --strict docs/Disaggregated/*.md MobileGL/MG_Remote/CONTRACT-P5*.md
bash scripts/ci/split_negative_controls.sh          # 需要 build-split 与 bin 目录环境，一般在 WSL 跑
```
