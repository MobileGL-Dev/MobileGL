# P5 包通用前言 — 每个包开工前必读

## 你在哪、读什么

- 集成树 `~/w7/pipe`，分支 `feat/disaggregated`。**不要直接在里面干活。**
  你的工作树：`cd ~/w7/pipe && git worktree add ~/w7/p5-<你的 slug> -b p5/<你的 slug> refs/heads/feat/disaggregated`
- **契约优先于 brief**：`MobileGL/MG_Remote/CONTRACT-P5.md`（表 0/1/2/3 + 全部裁定）。
- brief：`~/w7/notes/p5/BRIEF-P5.md`（裁定 R-1…R-15；**§12 是契约包落地后的 14 处更正，先读它**）。
- 契约包的报告：`~/w7/notes/p5/p5-results/c0-v1.md`（尤其 §5 每个桩签名的理由、§6 brief 的 14 处错）。
- verb 普查（实测）：`~/w7/notes/p5/verb-census.md`。
- 侦察八份：`~/w7/notes/p5/scout-*.md`，全部 file:line 可复核。
- 集成者决策：`~/w7/notes/p5/INTEGRATOR-DECISIONS.md`。
- 计划底本：`~/w7/pipe/docs/Disaggregated/{ARCHITECTURE,ROADMAP,MEASUREMENTS}.md`。

## 第一天必踩的两个坑

1. **worktree 的 submodule 是符号链接**（`git worktree add` 不带 submodule 内容，递归 init 要联网，
   本地对象库里没有 apitrace 的 brotli 树）。`~/w7/p5-c0-setup.sh` 会把每个 `.gitmodules` 路径链过去
   （包括 `include/ska`，`Includes.h:53` 要它）。
   **git 在它们是符号链接时会整个拒绝运行**（"expected submodule path X not to be a symbolic link"），
   所以：**跑任何 git 命令之前 `~/w7/p5-c0-submodlinks.sh off`，跑构建之前 `on`**。
2. **任何报构建结果的脚本，先确认 `CTestTestfile.cmake` 存在再相信它。**
   一个早期 bring-up 脚本丢了 `$COMMON`、构建循环没传目录，`cmake --build -j 24` 只打印用法而 `$?`=0，
   于是报了四个从没发生过的"绿"构建。

## 构建配方

```
export CCACHE_BASEDIR=/home/swung/w7
COMMON="-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
 -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_BUILD_BENCHMARK=OFF \
 -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
 -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0      # ID-18/21：让退出顺序 UAF 原生复现
```
- pull = 无额外 flag；push = `-DMOBILEGL_PIPE_PUSH=ON`；
  verify = `-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`；
  split = `-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON`。
  （`DISAGGREGATED` 现在蕴含 `PIPE_PUSH`，`INPROC` 蕴含 `DISAGGREGATED`。）
- 构建 `-j 24`。`~/w7/p5-c0-gate.sh` 是一个可抄的骨架（四个目录 + 生成器 + 纯度 + G1 + 四条 unit 车道）。

## 每个包都要自测的四件事

1. **G1**：`python3 scripts/symbol_report.py`（照 `~/w7/notes/tools/wsl_p4a_gate.sh` 的调法，
   `--threshold 0`）对 `~/w7/p5-before-libMobileGL.so`，必须 **0 增 / 0 删 / 0 resize / 0 重命名、`.text` +0**。
   **这是 P5 最容易破的门**：新计数器一律放进 `#if MOBILEGL_PIPE_PUSH` 块内；任何会出现在 pull
   构建里的新代码、新字符串、新 `Features` 成员都可能移动符号。
2. `ctest -L unit` 在 pull / push / verify / split 四个目录（基线 1785/1785/1785 与 split 1842/1842）。
3. `ctest -L integration-gpu` 在 build-push 与 `MOBILEGL_TRANSPORT=monolith` 下 1117/1117，
   且与 `~/w7/p5-before-ctest-names.txt` 逐名相同（G2/G14）。
4. `python3 scripts/gen_pipe.py --check`（若你碰了任何 `.def` 或生成器——多半你不该碰，见所有权表）。

## 铁律

- 提交信息 `[Type] (Scope): description`，**单行，绝不加 Co-Authored-By 或任何署名行**。
- **不要改别的包拥有的文件**（所有权表在 BRIEF §5 + §12 的补充）。需要改，向集成者提。
- 目录行的 opcode **永不移动**；新调用追加，退役的调用保留槽位。
- **不要拉 GitHub LFS**。fixture 只用本地已 hydrate 的（`openra.tgz` 是唯一 hydrate 的那个）。
- **不要碰任何 adb 设备。**
- 临时文件：中间日志放 `~/w7/` 并带 `p5-<slug>-` 前缀，收工时只留报告引用的那几个。
  **别在别的任务正在写日志时用通配 `rm`。**
- **绝不把源文件通过"引号 heredoc 套引号 heredoc"写出去**——会静默写进字面量 `\n`。
  用 Write/Edit 工具，或脚本文件里的单层 heredoc。
- 性能按 2026-09-08 的规则：只记录，不设门。**P5 预期变慢**（R-1 的栅栏税），这不是回归。

## 收工

1. 在你的分支上提交（多个单行提交没问题）。
2. 写 `~/w7/notes/p5/p5-results/<slug>-v1.md`：落了什么、每一条 brief/契约没做的裁定
   （带证据 + 推翻它需要什么）、四件自测的数字、**你认为 brief 或契约错了的地方**、
   以及留给后续阶段的账。
3. 返回 ≤200 词：提交区间、验收数字、其他包最需要知道的三件事。
