# P5b 包通用前言 — 每个包开工前必读

P5b = verb migration under `inproc`, Minecraft-first（ID-66/ID-68）。目标一句话：让真实负载
（首先是每一条 Minecraft trace）在 `MOBILEGL_TRANSPORT=inproc` 下由 apply 线程渲染，
越快越好。四个包并行迁移 25 个实测 class-C slot，门只有一个：inproc lane 的 abort 数与
trace 的 first-blocker 表。

## 你在哪、读什么

- 集成树 `~/w7/pipe`，分支 `feat/disaggregated`。**不要直接在里面干活。**
  你的工作树：`cd ~/w7/pipe && git worktree add ~/w7/p5b-<你的 slug> -b p5b/<你的 slug> refs/heads/feat/disaggregated`
- **契约优先于 brief，按这个顺序读：**
  1. `MobileGL/MG_Remote/CONTRACT-P5.md`（P5 的四张表与 R-1…R-17，仍然全部有效）。
  2. `MobileGL/MG_Remote/CONTRACT-P5B.md`（P5b 的增补：25 个 slot 每一行的 wire record、
     sink 签名、拒绝形状、stamp；§6 是 c0b 的裁定与推翻条件；§8 是文件所有权）。
  3. `~/w7/notes/p5b/BRIEF-P5B.md`（四个包各自的 slot、文件、门、以及之后的循环）。
  4. 普查：`~/w7/notes/p6/census-classC.md`（你的 slot 的命中数、"下一个 blocker"的估计）。
  5. c0b 的报告：`~/w7/notes/p5b/p5b-results/c0b-v1.md`（落了什么、每条裁定、数字、债）。
  6. P5 的材料按需：`~/w7/notes/p5/PACKAGE-PREAMBLE.md`、`INTEGRATOR-DECISIONS.md`
     （ID-57 的"按名拒绝"形状、ID-66 的流程规则、ID-68 的普查裁定）、
     `p5-results/c1-v3.md`（五个 class-B emitter 是怎么写的）、`v1-v3.md`（`ServerVerbSink`）。

## 流程规则（用户，2026-09-16，ID-66）

- **快，不做过度验证。** 不做轮次之间的对抗审查；整个 P5b 收尾时由 Codex 做一次。
- 一个包在**自己的门 + 集成者的快门**上落地：unit 四车道、`integration-split`（inproc）、
  push monolith 逐名一致（G2/G14）、G1。没有 verifier 包。
- 你的门（BRIEF §3）：**lane 里你的 slot 的 abort 数 → 0，受影响的每个场景到达它的下一个
  first blocker，按名报告**；d1 另加：每一条 Minecraft trace 在 inproc 下到达下一个 first
  blocker，按名报告。
- 设备 A/B 在后台记录，永不阻塞。

## 第一天必踩的两个坑（P5 原样继承）

1. **worktree 的 submodule 是符号链接**。抄 `~/w7/p5b-c0b-setup.sh` 与
   `~/w7/p5b-c0b-submodlinks.sh`（把 `p5b-c0b` 换成你的 slug）。
   **跑任何 git 命令之前 `submodlinks.sh off`，跑构建之前 `on`。**
2. **任何报构建结果的脚本，先确认 `CTestTestfile.cmake` 存在再相信它。**
3. （新）**后台任务要 `setsid nohup … < /dev/null &`**：从 Windows 侧的 `wsl.exe` 起的
   shell 退出时会带走普通的 `nohup … &`。

## 构建配方（不变）

```
export CCACHE_BASEDIR=/home/swung/w7
COMMON="-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
 -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_BUILD_BENCHMARK=OFF \
 -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
 -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0
```
- pull = 无额外 flag；push = `-DMOBILEGL_PIPE_PUSH=ON`；
  verify = `-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`；
  split = `-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON`。
- `-j 24`。`~/w7/p5b-c0b-gate.sh` 是可抄的骨架（四目录 + 生成器 + 纯度 + G1 + 四条 unit 车道），
  `~/w7/p5b-c0b-census.sh <tree> <outdir>` 是把 P6 普查跑在任意树上的版本
  （`results.json` 里每条 entry 的 status 与第一行 `Fatal{…}`）。

## 每个包都要自测的五件事

1. **G1**：`python3 scripts/symbol_report.py --before ~/w7/p5-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0`
   必须 **0 增 / 0 删 / 0 resize / 0 重命名、`.text` +0**（基线 `.READY` = `4595163c`）。
   **P5b 最容易破的门**：你加的一切都必须 push/split-only。后端里的任何新函数、新字符串、
   新成员都要在 `#if MOBILEGL_BUILD_DISAGGREGATED`（或至少 `MOBILEGL_PIPE_PUSH`）之内。
2. `ctest -L unit` 四目录（c0b 头上的基线见 c0b-v1.md §2）。
3. `integration-split` 在 build-split、`MOBILEGL_TRANSPORT=inproc` 下 **22/22**；
   push `integration-gpu` 1128/1128 且与 `~/w7/p5-before-ctest-names.txt` 逐名相同（G2/G14）。
4. **你的普查**：`~/w7/p5b-c0b-census.sh ~/w7/p5b-<slug> ~/w7/p5b-<slug>-census-logs`，
   然后按 slot 数 `Fatal{UnmigratedVerb, "…"}`；c0b 头（`f5676103`，含 v1 round 3）上的基线是
   **432 passed / 185 skipped / 505 aborted / 27 failed，505 个 abort 全部是 `UnmigratedVerb`**，
   逐 slot 表与 `census-classC.md` 相同（P6 的 426/185/511/27 里那 6 个
   `StageSnapshotTooNarrow` 已被 v1 r3 修掉，见 c0b-v1.md §2.5）。基线的 `results.json` 在
   `~/w7/p5b-c0b-census-logs/`。你的 slot 的计数必须归零，多出来的 first blocker 必须按名列出。
   d1 另跑 `~/w7/notes/tools/p6_census_traces.sh` 的同等物（trace 的 first-blocker 表）。
   c0b 头上的其余基线：unit 1817 × 3、split 2084；`integration-split` inproc 22/22（2 skipped）；
   push `integration-gpu` 1128/1128；G1 `.text` 10806611 +0、27814 符号 0/0/0/0。
5. `python3 scripts/gen_pipe.py --check` / `gen_pipe_field_ownership.py --check`
   （若你碰了任何 `.def` — 除 CONTRACT-P5B.md §8 明确授权的两处外你不该碰）。

## 铁律

- 提交信息 `[Type] (Scope): description`，**单行，绝不加 Co-Authored-By 或任何署名行**。
- **只改你拥有的文件**（CONTRACT-P5B.md §8）。需要改别人的，向集成者提。
- 目录行的 opcode **永不移动**；新调用追加，退役的调用保留槽位。
- **不要拉 GitHub LFS**。fixture 从 `~/w7/pipe/tools/trace_replay/fixtures` 拷（全部已 hydrate）。
- **不要碰任何 adb 设备。**
- 临时文件 `~/w7/p5b-<slug>-*`，收工时只留报告引用的那几个。
  **别在别的任务正在写日志时用通配 `rm`。**
- **绝不把源文件通过"引号 heredoc 套引号 heredoc"写出去。**
- 性能只记录不设门；inproc 是 lockstep，**慢是预期**（R-1 的栅栏税）。

## 收工

1. 在你的分支上提交（多个单行提交没问题）。
2. 写 `~/w7/notes/p5b/p5b-results/<slug>-v1.md`：落了什么、每一条契约没做的裁定
   （带证据 + 推翻它需要什么）、五件自测的数字、你的 slot 归零后的**下一个 first blocker 表**
   （按场景、按 trace，逐名）、以及留给后续轮次的账。
3. 返回 ≤200 词：提交区间、验收数字、其他包最需要知道的三件事。
