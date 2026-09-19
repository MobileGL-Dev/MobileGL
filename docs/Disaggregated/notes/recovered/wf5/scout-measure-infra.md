# P2 measurement & gating: what exists, what is missing, what it costs

Scout pass over `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`
(branch `feat/disaggregated` @ `e7a6a72f`). Read-only. Every claim below carries a
`file:line` I actually opened. Paths are repo-relative to that worktree unless absolute.

---

## 0. What P2 is obliged to produce (the target the infra has to hit)

- P2 acceptance row: `docs/Disaggregated/ROADMAP.md:18` — 「集成 × 2 后端 × {pull, push} 逐名相同；
  40 trace push 下 SSIM ≥ 0.99 双后端；verify 零分歧；`HandleRecycleScenario` 绿且重键前红；
  G7 测试绿且拿掉一个字段能红；**两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内；
  Blaze3D blend-toggle 微基准；CSO 内容寻址关闭的负面对照**」.
- Day-43 GO/NO-GO checklist: `docs/Disaggregated/ROADMAP.md:42-58`. The seven `[ ]` items are at
  `:46-52`; the pass/fail wording at `:56` is *「两台设备 p50 与 p99 逐线程 CPU 增量都不为负；
  tracker 绝对 ns 在上限内；Track H 单位成本不超出估计的 50%」*, and `:57` says NO-GO does **not**
  roll back.
- The five-part validation gate this replaces byte-identity with:
  `docs/Disaggregated/ARCHITECTURE.md:497-507`; part 4 (performance) is `ARCHITECTURE.md:504`:
  「两台设备 reboot-clean、同热窗口、配对 A/B，`tools/bench.sh` + trace replay `--benchmark` 逐帧 JSON；
  **指标是逐线程 CPU 时间**，p50 与 p99；**绝对阈值** … Blaze3D blend-toggle 微基准单列；
  关掉 CSO 内容寻址的负面对照」.
  - **Doc bug worth fixing in the brief**: `ARCHITECTURE.md:504` names `tools/bench.sh`. The script is
    at `tools/device_bench/bench.sh` (`tools/device_bench/README.md:1`, usage at `:36-38`). There is
    no `tools/bench.sh`.
- Milestone 「第 43 天（P2 出口）：GO/NO-GO」 — `docs/Disaggregated/ROADMAP.md:39`.
- Open question #1 (P2's headline number) — `docs/Disaggregated/ROADMAP.md:74`: 「client 侧 dirty 走查的
  真实每 draw CPU 代价 … 逐线程 CPU + 绝对 ns，两台设备」.
- The absolute-ns ceiling's source: steady-state pull cost is **6.5–9.3 accessor calls + memo probes
  per draw** (`docs/Disaggregated/MEASUREMENTS.md:64`, table at `:50-58`). A relative/noise threshold
  would pass trivially — that is exactly why `ARCHITECTURE.md:504` demands an absolute ns cap.

---

## 1. Per-thread CPU measurement infrastructure

### 1.1 The blunt finding: **there is no per-thread CPU-time instrumentation in the tree**

Greps over `MobileGL/`, `tools/`, `android-plugin/`, `scripts/` for
`CLOCK_THREAD_CPUTIME_ID | CLOCK_PROCESS_CPUTIME_ID | getrusage | RUSAGE_THREAD | /proc/self/task`
return **zero hits** (verified: `grep -rnE ... --include=*.cpp --include=*.h --include=*.py
--include=*.sh MobileGL tools android-plugin scripts` → empty). There is likewise no
`pthread_setname_np` / `prctl(PR_SET_NAME)` anywhere under `MobileGL/`, so MobileGL's own worker
threads (`ShaderCompilePool`) carry generic names in any external profiler.

Consequence: the day-43 metric that the whole GO/NO-GO hangs on (`ROADMAP.md:56`) has **no
first-party collector today**. Everything below is either wall-clock or a sampling profiler.

### 1.2 What does exist

**(a) `PipeStats` — the MGPipe boundary counters (P0, landed).**
- Header/contract: `MobileGL/MG_Util/Metrics/PipeStats.h` (whole file). Byte classes
  `PipeStats.h:46-77`, call classes `:81-102`, the six memo gates `:107-122`, payload histogram
  `:129`, default summary period 120 frames `:132`, the `Enabled()` latch `:139-141`.
- Site inventory (the contract for what is and is **not** counted): `MG_Util/Metrics/PipeStats.cpp:16-100`.
  `accessor-calls` is a **static tally at ~10 hot entry points, a LOWER BOUND**, not a wrapper over
  all sites (`PipeStats.cpp:76-88`); reading `acc/draw` outside a draw-dominated window is called out
  at `PipeStats.cpp:93-99`.
- Counters are `std::atomic<Uint64>` with relaxed adds (`PipeStats.cpp:111-122`); off-path cost is one
  global load + a predicted branch (`PipeStats.h:25-36`).
- Lifecycle: `PipeStats::Init()` at `MobileGL/Init.cpp:114` (right after config load);
  `PipeStats::Shutdown()` at `MobileGL/Init.cpp:50` (first thing in teardown; emits final line + JSON).
- Frame hook `OnPresent()` `PipeStats.cpp:305-349`, called from `MG_Backend/DirectGLES/DirectGLES.cpp:10596`
  and `MG_Backend/DirectVulkan/DirectVulkan.cpp:1339`.
- **63 counting call sites** across 8 backend files (`grep -c "PipeStats::Add|PipeStats::CountGate"
  MobileGL/MG_Backend` = 63; files: `DirectGLES/{DirectGLES,Managers,MultiDraw}.cpp`,
  `DirectVulkan/{DirectVulkan,Renderer/UniformManager,Renderer/VkBufferManager,
  Renderer/VkTextureManager,Renderer/VulkanRenderer}.cpp`).
- Unit coverage: `MobileGL/MG_Test/Util/PipeStatsTest.cpp` (16 `TEST_F`s, 287 lines) — including
  `SummaryLineCarriesEveryClassAndGate:137`, `SummaryLinesReportDisjointWindows:174`,
  `CounterNamesAreStable:260`, `SummaryPeriodIsOneHundredAndTwentyFrames:280`. **Any new counter class
  P2 adds must extend these** (`CounterNamesAreStable` pins the strings).
- `PipeStats` measures *counts*, never time. There is no ns/CPU counter in it today.

**(b) Tracy.** `MOBILEGL_ENABLE_TRACY` option `CMakeLists.txt:13`; forced into `TRACY_ENABLE`
`CMakeLists.txt:227-234` (on-demand, delayed init, manual lifetime, no crash handler);
definition added to both targets `CMakeLists.txt:682-686`; header pulled in `MobileGL/Includes.h:140-145`
with three zone colours. Backend zones are dense — e.g. `MG_Backend/DirectGLES/DirectGLES.cpp:354,425,
473,517,543,685,1203,1220,1241,1303,1530,1727,1835,1895,2025,2725,2857,2899,2932,2952,3001
("BindCurrentVAO"),3027 ("ResolveAndBindUnitTextures"),3303,3377,3400,3462`.
`PipeStats::OnPresent()` also plots every counter per frame via `TracyPlot`
(`PipeStats.cpp:320-344`; per-gate hit/miss plot names `PipeStats.cpp:184-191`). Tracy gives
per-thread *zone* timelines, not aggregated per-thread CPU seconds, and needs a connected profiler —
usable for attribution, **not** as the paired A/B number.

**(c) simpleperf — the only real per-thread CPU attribution available.**
`tools/device_bench/profile.sh` (70 lines):
- record: `simpleperf record --app <PKG> -e cpu-clock -f 800 -g --duration N`
  (`profile.sh:52-53`); DWARF unwinding is mandatory, frame-pointer graphs are broken
  (`profile.sh:15-16`).
- symbolize: `binary_cache_builder.py` against the unstripped
  `MobileGL/build/intermediates/merged_native_libs/fordebug/mergeFordebugNativeLibs/out/lib/arm64-v8a`
  (`profile.sh:58-62`).
- **per-thread summary is exactly `simpleperf report --sort comm -n`** (`profile.sh:63-65`), then
  `--comms <renderthread> --sort symbol` to drill in (`profile.sh:66-67`).
- Its target is hardcoded to the FCL fordebug game package `com.tungsten.fcl.mgdebug.debug`
  (`profile.sh:25`), not the trace APK, and it needs Git Bash not PowerShell (`profile.sh:18`).
- Caveat carried in the user's memory `mobilegl-profiling-pipeline.md:13-15`: MC's render thread is a
  JVM thread named `Thread-1x` (varies per run), holds 50-75 % of samples; `--call-graph fp` useless.
- **`simpleperf --app` requires a debuggable app.** The trace APK's release build type is
  `isDebuggable = debuggableRelease` (`android-plugin/app/build.gradle.kts:183`), fed by the gradle
  property `mobilegl.debuggableRelease`, default `"false"` (`build.gradle.kts:62`). So profiling the
  *retrace* harness needs a `-Pmobilegl.debuggableRelease=true` build — **flag this to the
  implementers**; and note `fordebug -O0` history (memory `fordebug-o0-trap`) does not apply here
  (that was FCL's native lib, fixed at `34b7cc77`).

**(d) `tools/device_bench/bench.sh` — the in-game FPS harness (wall clock, not CPU).**
- What it measures: FCL's overlay counts `eglSwapBuffers` natively and logs one `FCLFPS: <n>` logcat
  line per second (`tools/device_bench/README.md:8-16`); `bench.sh:178` tails
  `logcat -v raw -s FCLFPS:I`, `:180-187` collects `--samples` (default 30, `bench.sh:36`) after
  `--warmup` (default 180 s, `bench.sh:37`, and `README.md:48-50` explains the 3-minute ART JIT ramp).
- Output: one JSON line per run into `results/results.jsonl` with
  `fps_mean/median/min/max/sd, gpu_busy_mean, temp_start/mid/end_mc, big_cur, little_cur,
  gpu_cur_khz, pinned, warmup_s` (`bench.sh:210-214`).
- Protocol discipline that P2 must repeat verbatim: thermal gate, 180 s warmup, pin-integrity check
  (`big_cur/little_cur/gpu_cur_khz` sampled at window end, discard if off-pin), **paired back-to-back
  A/B in the same session**, F3 off, FPS overlay on, root required — `README.md:45-58`.
- `devices/odinlite.env` is the only device profile present (`tools/device_bench/devices/`); **there
  is no profile for `35d0befa` or `3B159D009VZ00000`** — P2 must author two.
- `bench.sh` reports **no CPU time at all**. Pairing it with `profile.sh` is the only way to get the
  per-thread number out of the game path today.

### 1.3 Device protocol (from the P0 brief and the P0 run scripts)

- Devices: `35d0befa` = Xiaomi 24129PN74C, Adreno 830, Android 16; `3B159D009VZ00000` = Oppo PLG110,
  Mali, Android 16 (ColorOS) — `docs/Disaggregated/MEASUREMENTS.md:3`, and
  `.../eb1a694b-.../scratchpad/wf2/BRIEF-P0.md:25-27`.
- Lock protocol (`BRIEF-P0.md:27`): atomic `mkdir` lock **before any adb use** at
  `C:/Users/GEEKER~1/AppData/Local/Temp/claude/C--Users-geekerwan-.../8ab6b3cb-c783-4826-9fd7-39ff3498af5b/scratchpad/w7/locks/<serial>`,
  `echo "<owner> $(date)" > "$LOCK/owner"`, `rm -rf` when done, **hold ≤ 25 min per acquisition**;
  a lock held > 60 min is not to be broken — report "blocked by device lock". Always `-s <serial>`.
  Wake+unlock first (`input keyevent KEYCODE_WAKEUP; input keyevent 82`). Device logs from
  `/sdcard/MG/latest.log`, not logcat.
- The working P0 loop that already implements this end to end is
  `.../scratchpad/wf2/stats_baseline.sh:16-29` — lock acquire with a 1500 s timeout and 20 s poll
  (`:18`), one hold per (case, backend) run, results copied out of
  `.trace-work/android-retrace-result/*-$b/`, and `grep 'MGPipe stats'` on `mobilegl.log` (`:28`).
  **Copy this shape for P2's paired A/B rather than reinventing it.**
- Serialization rule: `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result`
  root per tree and `rmtree`s it per invocation (`run_android_retrace_local.py:14`, `:354-356`), so
  **two devices must be driven serially from one tree** — `MEASUREMENTS.md:93`. For P2's paired A/B
  across two devices, either accept the serialization or use two worktrees.
- ColorOS traps: first install of a not-yet-installed package blocks on the
  `com.oplus.appdetail InstallGuideActivity` dialog (`MEASUREMENTS.md:18`); a foreign-signed trace APK
  must be uninstalled first (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`).
- MSYS trap: an embedded `/data/...` inside an `--env` value gets path-converted; run with
  `MSYS_NO_PATHCONV=1` (`MEASUREMENTS.md:94`, and `stats_baseline.sh:2`).

### 1.4 The gap, and the cheapest way to close it

The GO/NO-GO wants **per-thread CPU p50/p99, paired, reboot-clean, on two devices**. Options, in
order of cost:

1. **(recommended) Add thread CPU time to the retrace benchmark itself.** Retrace runs
   `--singlethread` (`android-plugin/app/src/trace/cpp/trace_replay_core.cpp:459`), and
   `trace_benchmark.hpp:12-13` states that Begin/OnFrameBoundary/End are only ever reached from that
   one retrace thread — so *that thread's CPU time is the client-side CPU cost*, with no sampling,
   no root, no debuggable build and no profiler. The change is small and lands in shared code
   (desktop CLI and Android APK compile the same files —
   `tools/trace_replay/CMakeLists.txt:229,254-255`):
   - `android-plugin/app/src/trace/cpp/trace_benchmark.cpp:75-77` — alongside the
     `Clock::now()` wall delta, take `clock_gettime(CLOCK_THREAD_CPUTIME_ID, …)` and push a second
     per-frame series into `Report` (`trace_benchmark.hpp:32-39`).
   - `trace_replay_core.cpp:832-865` (`SummarizeBenchmark`) — the mean/median/nearest-rank-p95 code is
     already there and can be reused verbatim for the CPU series.
   - `trace_replay_core.cpp:867-899` (`WriteBenchmarkJson`) — add `meanFrameCpuMs / medianFrameCpuMs /
     p95FrameCpuMs` and a `frameCpuTimesMs[]` array next to the existing `frameTimesMs[]` at `:889-896`.
   - `tools/trace_replay/run_android_retrace_local.py:228-237` (`format_benchmark`) — print them.
   - **p99 needs no device change even today**: `frameTimesMs[]` is dumped in full
     (`trace_replay_core.cpp:889-896`), so any percentile is computable host-side from
     `benchmark.json`. Only the headline fields stop at p95.
2. **simpleperf `--sort comm`** (`tools/device_bench/profile.sh:63-65`) for the *game* path and for
   attribution ("where did the ns go"), accepting the debuggable-build requirement above.
3. **A `/proc/<pid>/task/*/stat` sampler.** Nothing like it exists in the tree; whether a non-root
   `adb shell` can read another app's task stats on these Android 16 ROMs is **unverified** — do not
   put it on the critical path without a probe.

---

## 2. Trace-replay benchmark mode, and how `MOBILEGL_PIPE_STATS` numbers are collected on device

### 2.1 `tools/trace_replay/run_android_retrace_local.py --benchmark`

- Flags and defaults (`run_android_retrace_local.py:310-339`):
  - `--benchmark` — replay each case end to end as a frame-timing benchmark instead of comparing one
    frame to its golden (`:310-315`).
  - `--benchmark-repeats`, **default 3**; the run with the lowest mean frame time is reported as
    "best" (`:316-321`, selection at `:281-291`; a run that recorded no frames reports `-1` and is
    forced to `inf` so it cannot win — `:281-284`).
  - `--benchmark-tail-frames`, **default 200** — statistics are computed over the trailing N frames
    only (`:322-327`; same default in C++ at `trace_replay_core.hpp:18`).
  - `--benchmark-no-finish` — drops the per-frame `glFinish`, so frame times measure **CPU submission
    only** (`:328-333`). The semantics are spelled out at `trace_benchmark.hpp:16-24`: with finish ON
    the numbers cover GPU completion and are pessimistic but deterministic and comparable between
    backends/revisions; with it OFF you measure the CPU side. **For P2 (a CPU-cost question) the
    `--benchmark-no-finish` arm is the primary one, and the finish-ON arm is the sanity check.**
  - `--benchmark-timeout-seconds`, **default 900** per run — a benchmark replays the whole trace, not
    just up to `target_call` (`:334-339`).
  - `--env KEY=VALUE` (repeatable) — generic passthrough applied immediately before
    `libMobileGL.so` loads; a KEY with no `=` unsets it (`:301-309`). It reaches the device as
    `--es mobilegl_env "'K=V;K=V'"` (`android-plugin/trace-replay-ci.sh:411-416`) and lands in
    `ApplyEnvOverrides` → `setenv` (`trace_replay_core.cpp:143-152`).
- Repeat economics: only run 1 installs the APK and pushes the trace; repeats pass `--reuse-fixture`
  (`run_android_retrace_local.py:257-258`, honoured at `trace-replay-ci.sh:522-525`). The previous
  repeat's `benchmark.json` is deleted first so it cannot be re-read as this run's result (`:259-262`).
- Printed per run and then a "best of N" line (`:276-291`), format from `format_benchmark`
  (`:228-237`): `frames=… total=…s tail=… mean=…ms median=…ms p95=…ms fps=…`.
- Benchmark mode renders no SSIM summary page (`:360-364`); "passed" only means the replay reached the
  end of the trace without an error (`trace_replay_core.cpp:1037-1049`).

### 2.2 `android-plugin/trace-replay-ci.sh` (what the runner shells out to)

- Usage/flags: `trace-replay-ci.sh:36-40` (`--benchmark`, `--benchmark-tail-frames`,
  `--benchmark-finish`, `--reuse-fixture`), semantics at `:58-63`; defaults `benchmark=0`,
  `benchmark_tail_frames=200`, `benchmark_finish=1` at `:129-131`.
- It turns those into Activity extras: `--ez benchmark true`, `--ei benchmark_tail_frames N`,
  `--es benchmark_result_path <appdir>/output/benchmark.json`, `--ez benchmark_finish …`
  (`:417-426`).
- Completion is polled once a second for `--timeout-seconds` by looking for `output/result.json`
  through `run-as` (`:443-454`), with an "app exited early" branch (`:447-452`) and an
  infrastructure-retry exit code 75 (`:6`, `:475-478`).
- Artifacts copied back on every run: `result.json` (`:481`), `retrace.log` (`:488`) and
  **`mobilegl.log` (`:489`)**; `benchmark.json` additionally in benchmark mode (`:490-492`).
  A benchmark run copies no `actual/diff` PNG (`:483-487`).
- **This is why one run gives you both numbers**: `mobilegl.log` (where `MGPipe stats:` lines land)
  is pulled even in `--benchmark` mode.

### 2.3 What the device writes

- `trace_benchmark.cpp` is the timer: `Begin(finishEachFrame)` `:49-58`, `OnFrameBoundary()` `:60-78`
  (optional `glFinish` resolved lazily through `dlsym` on the already-loaded `libMobileGL.so`
  `:29-45`), `End()` `:80-91`. Frame boundary hook is called from the platform glws swapBuffers
  override — `android-plugin/app/src/trace/cpp/apitrace_glws_android.cpp:191-192`.
- Statistics: `SummarizeBenchmark` `trace_replay_core.cpp:832-865` — mean/median over the trailing
  `tailFrames`, **nearest-rank p95** (`:858-863`), `fps = 1000/mean` (`:864`).
- JSON: `WriteBenchmarkJson` `trace_replay_core.cpp:867-899` — `tracePath, backend, benchmarkFinish,
  width, height, totalFrames, tailFrames, totalSeconds, meanFrameMs, medianFrameMs, p95FrameMs, fps,
  frameTimesMs[]`. `result.json` carries the same headline numbers under `benchmark*` keys
  (`:954-968`).
- Retrace argv: `-b --singlethread --no-context-check`, and in benchmark mode the `-s/-S` snapshot
  pair is omitted entirely so the readback and PNG encode are not in the timed loop
  (`trace_replay_core.cpp:455-471`, rationale at `:461-464`).
- The same `--benchmark` family exists on the desktop CLI
  (`tools/trace_replay/trace_replay_cli.cpp:32-40`, parsing `:160-170`), sharing the identical core
  (`tools/trace_replay/CMakeLists.txt:229,254-255`). So a WSL/lavapipe pre-flight of any benchmark
  change is free.

### 2.4 `MOBILEGL_PIPE_STATS` on device

- Knobs (all `MOBILEGL_`-prefixed, so no allow-list edit is ever needed —
  `MobileGL/ConfigLoader.cpp:242-244`, mechanism at `ConfigLoader.cpp:31-61` +
  `IsAcceptedPrefix`):
  - `MOBILEGL_PIPE_STATS` → `Features.PipeStats` (`Config.h:353-355`, `ConfigLoader.cpp:254`).
  - `MOBILEGL_PIPE_STATS_PERIOD` → `Features.PipeStatsPeriod`, default 120, range [1, 1000000]
    (`Config.h:370-374`, `ConfigLoader.cpp:262`); latched in `PipeStats::Init()`
    (`PipeStats.cpp:254-267`), never 0 (`:258-260`).
  - `MOBILEGL_PIPE_STATS_FILE` → `Features.PipeStatsFile`, empty = no dump (`Config.h:375-378`,
    `ConfigLoader.cpp:263`).
- **Summary line format** — built by `FormatWindowLine()` `PipeStats.cpp:366-432`, emitted by
  `EmitSummaryLine()` `:223-232` via `MGLOG_I` (deliberately INFO, justified at `:225-229`), fired
  every `g_summaryPeriod` frames from `OnPresent()` `:345-348`:

  ```
  MGPipe stats: frames=<total> window=<n> draws=<n> draws/f=<x.xx> acc=<n> acc/draw=<x.xx>
    bytes/f[buf= tex= ubog= ubon= vtxc= idxc= icmd= pmap= resid=]
    tex[emit= box= rect= jobs=] gates[ers=h/m etl=h/m eub=h/m mfp=h/m mpm=h/m mdt=h/m]
  ```
  Short names: `PipeStats.cpp:193-195` (`buf tex ubog ubon vtxc idxc icmd pmap resid`;
  `ers etl eub mfp mpm mdt`). Two decimals via `FormatFixed2`, and a zero denominator prints
  **`n/a`**, never `0.00` (`:159-166`). If a window contains no Present the label degrades from
  `bytes/f[` to `bytes[` and carries window totals (`:404-406`, reason at `:371-375`). Windows are
  disjoint: `AdvanceSummaryWindow()` `:434-446` rebases; `FormatWindowLine()` is pure and can be
  called twice (`PipeStats.h:175-183`).
- **JSON dump limitation (the trap)**: `WriteJsonDump()` runs only from `Shutdown()`
  (`PipeStats.cpp:271-278`), which is called from `MobileGL::Destroy` (`MobileGL/Init.cpp:50`).
  **The trace app never reaches that teardown**, so `MOBILEGL_PIPE_STATS_FILE` never produces a file
  on device — only the periodic `mobilegl.log` lines exist, and a trace shorter than one period
  yields nothing at all. Documented at `docs/Disaggregated/MEASUREMENTS.md:92`; that is precisely why
  `MOBILEGL_PIPE_STATS_PERIOD` was added (`Config.h:370-373`). It **does** work on desktop, where
  teardown runs.
- **The reproducible device recipe** (`MEASUREMENTS.md:70-76`):
  ```sh
  ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 \
  python3 tools/trace_replay/run_android_retrace_local.py \
    --case minecraft-1.21.4-in-world --backend DirectGLES \
    --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
  # numbers: grep 'MGPipe stats:' <result dir>/mobilegl.log, take the LAST complete window
  ```
  `MOBILEGL_LOG_FILE_PATH` is set by the harness to `<outputDir>/mobilegl.log`
  (`trace_replay_core.cpp:984`, `setenv` at `:1011`).
- The P0 baseline that P2 compares against is `MEASUREMENTS.md:50-58` (four traces × two backends ×
  two devices), and the reading at `:62-66`.

---

## 3. What a Blaze3D blend-toggle microbenchmark attaches to

**It already exists.** `MobileGL/MG_Benchmark/Driver/DriverBenchCases.inc:558-570`:

```c
/* Blaze3D toggles blend around batches: 46 glEnable/glDisable pairs and 28
 * glBlendFuncSeparate per vanilla frame. a = toggle pairs. */
static void case_mc_state_toggle(int frame, long a, long b) { … glEnable(GL_BLEND);
    glBlendFuncSeparate(...); glDrawElements(...); glDisable(GL_BLEND); glDrawElements(...); }
```
registered in the case table as `{"mc_state_toggle", case_mc_state_toggle, 46, 0, 46}`
(`DriverBenchCases.inc:619-639`, table type at `:611-617`; `a`=toggle pairs, `opsPerFrame` is the
normaliser). Sibling MC-shaped cases that P2 will want in the same table:
`mc_vanilla_draw` (5495 draws/f), `mc_pass_switch` (`:543-556`), `mc_tex_param` (`:575-586`),
`mc_use_program` (`:590-604`), `mc_ubo_range`, `mc_sampler_churn`.

**How it runs.**
- `MobileGL/MG_Benchmark/Driver/DriverBench.c` is a headless EGL client that is *not* linked against
  MobileGL: it `dlopen`s exactly one EGL provider from `$DRIVERBENCH_EGL_LIB` — the system
  `libEGL.so.1` for the native driver, or a `libMobileGL.so` path for either backend selected with
  `MOBILEGL_BACKEND_TYPE` (`DriverBench.c:9-18`, CMake comment `Driver/CMakeLists.txt:3-6`).
- Loop: 30 warmup frames then 120 timed frames, each closed with a **real fence wait**, not
  `glFinish` — because MobileGL implements `glFinish`/`glFlush` as no-ops and a finish-paced loop
  would time only CPU submit on MobileGL while timing submit+GPU natively
  (`DriverBench.c:245-266`, run loop `:268-292`). Knobs `g_warmup=30, g_frames=120`
  (`DriverBench.c:254`), env overrides `DRIVERBENCH_DRAWS / DRIVERBENCH_FRAMES / DRIVERBENCH_SPRITES`
  (`:475-477`).
- Output is one CSV line per case: `case,frames,ops_per_frame,median_frame_ms,ns_per_op,fps`
  (`DriverBench.c:31-32`, header `:482`, print `:288-289`). **`ns_per_op` is exactly the shape the
  day-43 "tracker absolute ns per draw" number needs.**
- Driver script `MobileGL/MG_Benchmark/Driver/run_driver_bench.sh`: modes `native | espryt <so> |
  magma <so>` (`:1-9`), pins the EGL vendor and Vulkan ICD explicitly so a bare `libEGL.so.1` does not
  silently fall to llvmpipe under a benchmark (`:10-15`, `:19-20`, `:26-40`).

**How it is built.**
- `MOBILEGL_BUILD_BENCHMARK` option, default **ON** (`CMakeLists.txt:6`), documented
  `README.md:140`; subdirectory added at `CMakeLists.txt:782-784` (after `MG_Test` so googletest is
  already available, `:776-777`).
- `MobileGL/MG_Benchmark/CMakeLists.txt` FetchContents google/benchmark v1.9.4 (`:18-25`), links
  `MobileGL_s` (`:14-16`), and adds `Program Buffer Driver Container ShaderCache Transpile` (`:43-48`).
  Every bench is a ctest under **`LABELS benchmark`** (e.g. `Driver/CMakeLists.txt:14-15`,
  `Buffer/CMakeLists.txt:19-20`), so CI runs them with `ctest -L benchmark`
  (`.github/workflows/test.yml:745`).

**Two hard constraints P2 must plan around.**
1. **`DriverBench` is Linux-desktop only**: `Driver/CMakeLists.txt:7-9` —
   `if (NOT UNIX OR APPLE OR ANDROID) return()`.
2. **The whole benchmark tree is force-disabled on Android**: `CMakeLists.txt:33-35` sets
   `MOBILEGL_BUILD_BENCHMARK OFF … FORCE` when `ANDROID`.
   → The blend-toggle microbench can be run today on WSL/lavapipe+llvmpipe (and against a real desktop
   GPU) but **not on `35d0befa`/`3B159D009VZ00000` without new work**. Since P2's question is
   client-side CPU per draw, the desktop run is legitimate evidence for the *tracker ns* number, but
   the checklist's two-device requirement (`ROADMAP.md:49`) applies to the per-thread CPU deltas,
   which come from the retrace benchmark (§2), not from DriverBench. If an on-device blend-toggle is
   wanted, the cheapest route is a new Android-only target modelled on `DriverBench.c` (it already
   dlopens EGL and needs no window — surfaceless fallback at `DriverBench.c:294-300, 417`), *not*
   enabling the google-benchmark tree on Android.
- The other benches (`Buffer/BufferBench.cpp:23-40`, `Program/ProgramBench.cpp`) drive frontend
  entry points against `MobileGL_s` with no driver; they are the wrong attachment point for a
  draw-path microbench.

---

## 4. The CSO negative control (a switch that disables content addressing)

**Design intent, already written down:**
- `docs/Disaggregated/ARCHITECTURE.md:367` (§9.6): 「`MOBILEGL_PIPE_PUSH` 子系统位图（**含一位关闭 CSO
  内容寻址，负面对照**）在阶段 B 是真正的旧-vs-新 A/B」.
- What content addressing *is*: `ARCHITECTURE.md:63` — per-class
  `ska::flat_hash_map<xxHash, MGPipeHandle>`, caps render-state 64 / vertex-elements 1024 /
  sampler 256 / sampler-view 4096, LRU evict emits `delete_*`.
- Why the identity is a *subset* of the render-state blob and not the whole thing:
  `ARCHITECTURE.md:184-189` — whole-blob addressing would mint a new CSO on every
  `glViewport/glScissor/glBlendColor/glClearColor` and flush the server's pipeline memo (a regression
  recorded in `RenderState.h`); the client order is *pipeline version unchanged → reuse handle, zero
  hashing; changed → xxHash the ~25-30-word pipeline subset → CSO map probe → hit sends a 12 B bind,
  miss sends create+bind; `m_version` changed but subset unchanged → `set_dynamic_state` (~200 B)*.
  **The negative control has to disable the map probe/reuse, not the CSO records** — otherwise it
  measures a different thing.
- Why the push happens at validate time and not in the GL setter: `ARCHITECTURE.md:151` (Blaze3D wraps
  every batch in `glEnable/glDisable(GL_BLEND)`; per-setter push would be strictly slower than today).
  This is the same workload the microbench of §3 models.

**Where the knob lives today.**
- `MG_Config::FeaturesTable::PipePush` — `MobileGL/Config.h:319-326`. The comment at `:320-325`
  already promises: *"per-subsystem bitmask selecting which state the frontend PUSHES … One bit of it
  also turns OFF client-side content addressing of CSOs, which is the negative control the CSO design
  is measured against. Accepts decimal or 0x-prefixed hex."* Default `0`.
- Parsed at `MobileGL/ConfigLoader.cpp:245` with `QueryEnvUint64("MOBILEGL_PIPE_PUSH", 0)`, whose
  contract is at `ConfigLoader.cpp:162-192`: decimal by default, explicit `0x` for hex, **never**
  `strtoull` base 0 (a leading zero would read `010` as 8), and any `-` is rejected rather than
  wrapped to all-bits-set.
- **Status: `Features.PipePush` has no reader.** `grep -rn "PipePush" MobileGL/` hits only
  `Config.h:326` and `ConfigLoader.cpp:245`. There is no bit enum anywhere (`grep` for
  `MGPipeSubsystem | kPipePush | ContentAddress` → nothing). So P2 mints both the bit constants and
  the first consumer.
- Do not confuse it with the CMake option of the same spelling: `MOBILEGL_PIPE_PUSH` is *also* a
  compile-time option (`CMakeLists.txt:28`, define at `:548-549`) that selects the `MGB_CTX` arm
  (`MobileGL/MG_Pipe/PipeInputsSwitch.h:17-27`). The runtime bitmask is a separate, additive thing
  that only matters in a push build.

**Naming and wiring conventions to follow (`MOBILEGL_PIPE_*` family).**
- All eight live knobs are declared in `Config.h:319-378` and parsed in one block at
  `ConfigLoader.cpp:242-263`. Helper choice is by type:
  `QueryEnvFlag` (default-off boolean, truthy rule at `ConfigLoader.cpp:72-87`),
  `QueryEnvQuirkOverride` (tri-state, used when the default is ON so only an explicitly falsy value
  turns it off — `ConfigLoader.cpp:89-98`, e.g. `PipeLegacyMemos` at `:258-259` and `PipeVerifyFatal`
  at `:249-250`), `QueryEnvUint32(key, def, min, max)` (`:141-160`), `QueryEnvUint64` (bitmask only,
  `:167-192`), `QueryEnvVariable` (string, `:63-70`).
- Knobs that must not exist in a shipping build are `#if MOBILEGL_PIPE_PUSH`-guarded so the pull
  build's `FeaturesTable` does not change size (`Config.h:333-352`, mirrored at
  `ConfigLoader.cpp:247-253`).
- **No allow-list edit is ever needed**: `InitializeAcceptedEnvVariables` accepts every
  `MOBILEGL_`/`LIBGL_`-prefixed variable found in the environment
  (`ConfigLoader.cpp:31-61`, noted at `:242-244`).
- Precedent for a knob that is explicitly *the negative control* and how to word it:
  `Config.h:341-345` (`MOBILEGL_PIPE_VERIFY_CORRUPT`, negative control A) and `:346-351`
  (`MOBILEGL_PIPE_POISON_OMIT`, negative control B). Both are wired as **always-on CI steps**, not
  as manual procedures — `MobileGL/MG_IntegrationTest/CMakeLists.txt:700-711` (A) and `:719-725` (B).
  **P2's CSO negative control should get the same treatment**: a ctest entry that must be red when
  the switch is on and green when it is off, so a dead switch cannot pass silently.
- Recommendation for the brief: name the bit in the same file/format
  (`Config.h`, near `:326`), reserve a high bit (e.g. `0x8000'0000'0000'0000` — "not a subsystem, a
  behaviour") so subsystem bits stay a dense low range, and document the decimal/hex parse in the
  knob comment because operators will pass it as hex.

---

## 5. CI cost picture — what exists and what P2 can afford to add

Workflow `.github/workflows/test.yml` (1541 lines). Triggers: `push` to `dev`,
`Feat/Backend-Direct-GLES`, `Feat/Backend-Direct-Vulkan`, plus `workflow_dispatch` with a
`baseline_sha` input (`test.yml:3-17`). **`feat/disaggregated` is not a push-triggered branch** —
every run on it so far has been a manual dispatch (`gh run list`: 34027009840 / 34009031893 /
34008829271 all `workflow_dispatch`).

### 5.1 Job inventory (name → what it runs → last observed duration)

Durations are from the last completed dispatch on `feat/disaggregated`,
run **34009031893** (head `087685d1`, 2026-09-06 03:27–04:13Z, 127 jobs, **~46 min wall**).

| job (line) | what it does | last run |
|---|---|---|
| `build-linux` (`:20`) | clang-20 + ccache configure/build, uploads `mobilegl-linux-runtime.tgz`; benchmarks ON here (`:95` `-DMOBILEGL_BUILD_BENCHMARK=ON`) | **11m** |
| `test` (`:157`) | `ctest -L unit --no-tests=error` on the downloaded runtime (`:194-203`) | **2m** |
| `integration` (`:213`) | `ctest -L integration-gpu` on llvmpipe/lavapipe **plus a second filtered pass** with `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 -R 'Buffer\|Readback\|Atomic\|Ssbo\|Arena'` (`:279-296`); `MOBILEGL_ITEST_REQUIRE_GPU=1` so a driverless runner fails instead of skipping (`:256-259`, `:274-278`) | **6m** |
| `benchmark` (`:699`) | `ctest -V -L benchmark --no-tests=error` (`:740-745`), needs `libegl-mesa0` explicitly (`:710-717`), uploads core dumps on failure | **3m** (2m on the previous run) |
| `flatc-check` (`:649`) | regenerates the flatbuffers header and diffs | 1m |
| `include-graph-check` (`:677`) | include-closure purity gate | 0m |
| `pipe-gates` (`:1477`) | **source-level only, deliberately independent of `build-linux`** (`:1480-1481`): `gen_pipe.py` + `git diff --exit-code` (`:1492-1495`), `gen_pipe.py --self-test` (`:1501-1502`), `symbol_report.py --self-test` (`:1505-1506`), the **stdio-instrumentation grep gate** over `MG_Backend`+`MG_State` (`:1515-1522`), the informational `gen_pipe_dirty_surface.py --summary` (`:1527-1528`), and the citation lint, warning-only for now (`:1533-1541`) | **10 s** |
| `build-retrace` (`:755`) | builds the desktop replay runner; needs `build-linux`, `test`, `benchmark` (`:757-760`) | 2m |
| `trace-cases` (`:900`) | emits three matrices from `trace_cases.json`: full, names, and the **verify subset** (`:915-922`) | 0m |
| `trace-fixtures` (`:924`) | 48 fixture jobs, `max-parallel: 4` (`:928-932`), LFS fetch + cache | ~0m each |
| `retrace` (`:989`) | 2 backends × ~39 CI cases, `max-parallel: 4` (`:998-1001`) | **0–3m per job**, ~78 jobs; dominates the tail of the 46 min |
| `retrace-summary` (`:1095`) | renders the overview page | 0m |
| `remove-artifact-clutter` (`:1417`) | artifact hygiene | 0m |
| `build-linux-verify` (`:311`) | second build with `-DMOBILEGL_PIPE_VERIFY=ON`, `timeout-minutes: 120`, own ccache (`:311-323`) | **never yet completed** |
| `integration-verify` (`:473`) | `ctest -L integration-verify` under the comparator, `timeout-minutes: 180` (`:473-539`) + an arming-log assertion (`:554-560`) | **never yet completed** |
| `retrace-verify` (`:1141`) | verify build × the `verify: true` subset, `max-parallel: 4`, **`timeout-minutes: 240`**, `--timeout 10800` per case because comparator arms are 5-10× slower (`:1141-1154`, `:1220-1230`); refuses to run against a comparator-free library via an `nm --defined-only … MGPipeVerifyInputs` check (`:1207-1218`) | **never yet completed** |
| `monolith-symbol-report` (`:1298`) | `workflow_dispatch` only, `timeout-minutes: 180` | in progress at time of writing |

### 5.2 Facts the brief should state plainly

- **The three verify lanes have never produced a green CI run.** They were added by `0fe7bf82`
  ("the third CI mode …") and hardened by `97b997d5`, both dated **2026-09-06**; the only run whose
  head contains them is **34027009840** (head `e7a6a72f`), still `in_progress` with
  `build-linux-verify` and `monolith symbol report` running at 10:28Z. The P1 verify evidence in
  `MEASUREMENTS.md:129-142` is therefore **local WSL evidence** (`~/w7/pipe`), not CI evidence.
  P2's plan must not assume a known CI cost for these lanes; the only cost datum is
  `MEASUREMENTS.md:142`: *integration-verify's 818 entries at 4-way parallelism are about the same
  order as integration-gpu; 79 retrace cases at 4-way take ~20 minutes* (locally, 28-core WSL).
- **Verify scope is deliberately narrower than `integration`**: `test.yml:520-526` states the second,
  `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` filtered pass (186 entries) is **not** affordable under
  the comparator and is explicitly deferred — *"P2 can take it once the comparator's cost is known."*
  That is a P2 decision item the brief should surface.
- Trace corpus: `tools/trace_replay/trace_cases.json` holds **40 cases**, **1** marked `"ci": false`
  (`minecraft-1.21.4-rd12-odinlite-in-world`, line 75), and **8** marked `"verify": true`
  (lines 16, 31, 39, 48, 56, 64, 79, 287 → `OpenRA`, `minecraft-1.21.4-startup`,
  `minecraft-1.21.4-main-menu`, `minecraft-1.21.11-main-menu`, `minecraft-1.17-main-menu-854`,
  `minecraft-1.21.4-in-world`, `minecraft-1.21.4-fabric-sodium-in-world`,
  `improved-transparency-minecraft-26.3`). The verify matrix is a strict subset so `retrace-verify`
  needs no fixtures of its own (`test.yml:920-922`).
- **Where P2's new CI cost would land, cheapest first:**
  1. `pipe-gates` (`test.yml:1477`) — 10 seconds, no build dependency. The
     `gen_pipe_dirty_surface.py` step is *already* flagged at `test.yml:1524-1526` as
     **"becomes a gate in P2, when the mapping file exists to diff against"**. Turning it from
     informational into `--exit-code` is P2's cheapest gate and costs nothing measurable.
     The G7 setter-consistency test and the `ResidualValueBlock` `offsetof` asserts
     (`ARCHITECTURE.md:505`) belong in `test` (unit), also ~free.
  2. `test` / `integration` — a `{pull, push}` A/B of the integration suite doubles the 6m
     `integration` job (~+6m of a lane that is not on the critical path, since `retrace` is).
     `ARCHITECTURE.md:503` requires name-for-name identity between `DirectGLES.` and
     `DirectGLES.Pipe.`, so this is a second ctest pass, not a second build.
  3. `benchmark` (3m) — adding one or two DriverBench cases to `kBenchCases`
     (`DriverBenchCases.inc:619-639`) costs ~1.2 s of wall per case (120 timed frames + 30 warmup at
     MC frame rates) and lands in an already-existing 3m job. **The blend-toggle case is already in
     the table**, so if CI is not currently running it as a separate assertion, exposing it costs
     nothing: `add_test(NAME DriverBench COMMAND DriverBench draw_tiny)`
     (`Driver/CMakeLists.txt:14`) currently runs *only* `draw_tiny`.
  4. What P2 must **not** put in CI: the two-device paired A/B. Both devices are shared with the
     GL4.6 Wave-7 campaign under the mkdir lock (§1.3); the numbers require reboot-clean, thermally
     matched, back-to-back sessions (`tools/device_bench/README.md:45-58`) that no hosted runner can
     provide. Device evidence is a **recorded artifact in `MEASUREMENTS.md`**, exactly as P0's
     baseline was, not a lane.

---

## 6. Suggested shape of the P2 measurement plan (assembled from the above)

1. **Land a CPU-time series in the shared benchmark core** (§1.4 option 1) — 4 small edits, all
   cited above, verifiable on WSL first through the desktop CLI, and it is the only change that turns
   the existing harness into a per-thread-CPU instrument. Recompute p50/p99 host-side from
   `frameTimesMs[]`/the new CPU array; the device already dumps the full per-frame arrays.
2. **Paired A/B protocol per device**, one lock hold per arm, `stats_baseline.sh:16-29` as the
   template: for each of {pull build, push build} × {`--benchmark-no-finish`, finish} run
   `--benchmark --benchmark-repeats 3 --benchmark-tail-frames 200`, and in the *same* run collect
   `mobilegl.log` for the `MGPipe stats:` window (both artifacts come out of one invocation,
   `trace-replay-ci.sh:489,491`). Cases: the four P0 baseline traces
   (`MEASUREMENTS.md:50-58`) so the numbers are directly comparable; `create-indirect` stays excluded
   (pre-existing failure on both devices, `MEASUREMENTS.md:96`).
3. **Tracker absolute ns** from DriverBench `ns_per_op` on `mc_state_toggle` +
   `mc_vanilla_draw` (`DriverBenchCases.inc:560,620,627`), desktop, with `run_driver_bench.sh
   espryt|magma` and a `native` control; ceiling derived from the 6.5–9.3 accessor/draw pull baseline
   (`MEASUREMENTS.md:64`).
4. **CSO negative control** as a `MOBILEGL_PIPE_PUSH` bit (`Config.h:326`, `ConfigLoader.cpp:245`),
   plus an always-on ctest entry in the style of `MG_IntegrationTest/CMakeLists.txt:700-725` that is
   red with the bit set and green without, so the switch cannot rot.
5. **Free gates**: promote `gen_pipe_dirty_surface.py` to `--exit-code` in `pipe-gates`
   (`test.yml:1524-1528`); add G7 + `ResidualValueBlock` asserts to the unit lane; extend
   `PipeStatsTest.cpp` (`CounterNamesAreStable:260`) for any new counter.
6. **Fix the doc pointer** `ARCHITECTURE.md:504` → `tools/device_bench/bench.sh`, and author the two
   missing device profiles under `tools/device_bench/devices/` (only `odinlite.env` exists).
