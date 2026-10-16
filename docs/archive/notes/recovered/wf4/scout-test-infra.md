# P1 verification infrastructure — scout report

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated`.
Every claim below carries a `path:line` I opened. Read-only pass; nothing in the tree was modified.

Reading order: **Part A** is the inventory (what exists, exactly where). **Part B** is the proposal for the three
things the task asks for. **Part C** is the trap list — read it before writing any CMake, because three of the
traps have already cost this project a shipped bug or a silently-green lane.

---

# Part A — inventory

## A1. `.github/workflows/test.yml` — the whole job graph

Triggers: `push` to `dev`, `Feat/Backend-Direct-GLES`, `Feat/Backend-Direct-Vulkan`, plus `workflow_dispatch`
(`.github/workflows/test.yml:3-9`). **`feat/disaggregated` is not a trigger branch** — P1 work does not run this
workflow on push unless the branch list is extended or `workflow_dispatch` is used. There is no `pull_request`
trigger and no `timeout-minutes` on any job in this file (`grep timeout-minutes` finds only `apk.yml:331`), so
every job inherits GitHub's 6 h default.

Nine jobs:

### `build-linux` (`test.yml:12-147`)
- `runs-on: ubuntu-latest`, 32 GB swap (`:26-29`), `submodules: recursive` (`:31-34`).
- ccache via job `env` (`:17-23`): `BUILD_DIR=build-linux`, `CCACHE_DIR=${{ github.workspace }}/.ccache`, key
  `${{ runner.os }}-test-${{ github.job }}-ccache-v1` (`:39-45`), saved only on the default branch (`:109-122`).
- Toolchain: clang-20 / libc++-20 / lld-20 / ninja / mesa + lavapipe + Vulkan loader (`:58-61`), Vulkan SDK
  1.4.304.1 headers+loader (`:47-52`), `python update_glslang_sources.py` (`:54-56`).
- Configure (`:71-93`) — the flags P1 has to mirror:
  ```
  -DCMAKE_BUILD_TYPE=Release            (Debug only when ACTIONS_STEP_DEBUG=true, :73-77)
  -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO
  -DMOBILEGL_BUILD_TEST=ON
  -DMOBILEGL_BUILD_BENCHMARK=ON
  -DMOBILEGL_BUILD_INTEGRATION_TEST=ON
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json
  -DMOBILEGL_BUILD_TRACE_REPLAY=OFF
  -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON -DBENCHMARK_ENABLE_TESTING=OFF
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  ```
  Note what is **absent**: `MOBILEGL_BUILD_DISAGGREGATED` is never passed, so it stays OFF
  (`CMakeLists.txt:23`), and `MOBILEGL_ITEST_REQUIRE_GPU` is never passed at configure time (it arrives as job
  env instead — see `integration` below). `MOBILEGL_ENABLE_LTO` defaults OFF (`CMakeLists.txt:108`).
- Packaging (`:124-147`): `ci-artifacts/mobilegl-linux-runtime.tgz` containing the root
  `build-linux/CTestTestfile.cmake`, the three test/benchmark/integration subtrees and **every** `*.so` found
  under the build dir. Uploaded as artifact `mobilegl-linux-runtime`.

### `test` (`test.yml:149-203`) — the unit lane
- `needs: build-linux`; installs runtime deps only (`:160-163`); downloads + unpacks the runtime tgz.
- **"Normalize CTest command paths"** (`:174-184`): a python inline script rewriting
  `"…/cmake-…/bin/cmake"` → `"cmake"` in every `CTestTestfile.cmake` under `build-linux`. Repeated verbatim in
  `integration` (`:234-244`), `benchmark` (`:380-390`) and `build-retrace` (`:526-536`). Any new job that
  unpacks the runtime artifact and calls ctest must repeat this step.
- Run (`:186-195`): cores armed (`ulimit -c unlimited`, `kernel.core_pattern=/tmp/core.%e.%p`), then
  `ctest --output-on-failure -L unit --no-tests=error` (`-V` under ACTIONS_STEP_DEBUG).
- Failure-only core-dump artifact `unit-core-dumps` (`:197-203`).

### `integration` (`test.yml:205-296`) — the GPU-scenario lane, the one P1 doubles
- `needs: build-linux`. Extra runtime dep `libegl-mesa0`, with the reason spelled out (`:216-223`).
- Job `env` (`:266-270`):
  ```
  MOBILEGL_ITEST_REQUIRE_GPU: "1"
  MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH: "1"
  MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS: "1"
  MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER: "1"
  ```
- The **ENVIRONMENT-property warning** lives here in full (`:252-259`): the lavapipe ICD pin is passed at
  configure (`-DMOBILEGL_ITEST_VK_ICD`) because the configure bakes it into each test's ctest `ENVIRONMENT`
  property, "and a property entry OVERRIDES the job environment — a `VK_ICD_FILENAMES` exported here would be
  silently ignored while looking like it works."
- The **counter-example is in the same job** (`:274-288`): the second, filtered pass exports
  `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` *inline* with the comment "The flag is NOT baked into the ctest
  ENVIRONMENT properties, so an inline env reaches the test processes (unlike the ICD pin above)."
  **This is the decisive evidence for P1**: a job-env variable that no `ENVIRONMENT` property names does reach
  the test process. `MOBILEGL_PIPE_VERIFY` / `MOBILEGL_PIPE_PUSH` are named by no property today.
- Two ctest invocations:
  ```
  ctest --output-on-failure -L integration-gpu --no-tests=error
  MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest --output-on-failure -L integration-gpu \
      -R 'Buffer|Readback|Atomic|Ssbo|Arena' --no-tests=error
  ```
- `integration-core-dumps` on failure (`:290-296`).

### `flatc-check` (`test.yml:298-323`)
Independent of `build-linux`; regenerates `protocol_generated.h` with `scripts/gen_protocol.py` and
`git diff --exit-code`. The model for a source-level gate that must not be hidden by a broken build.

### `include-graph-check` (`test.yml:332-349`) — the P0.5 purity gate, and the template for P1's controls
- No `needs`, three header submodules only (`:340-343`), clang-20 (`:345-346`), then:
  `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all`.
- The job comment states the doctrine P1 must satisfy: "The script's own `--self-test` is always on: a negative
  control that stopped tripping fails the job, because **a gate that cannot go red is not a gate**"
  (`test.yml:330-331`, quoting `ROADMAP.md:7`).

### `benchmark` (`test.yml:351-405`)
`needs: build-linux`; `ctest -V -C Release -L benchmark --no-tests=error`.

### `build-retrace` (`test.yml:407-550`)
- `needs: build-linux, test, benchmark, integration` — i.e. the retrace lane only starts after the three
  ctest lanes are green.
- Job env `MOBILEGL_LIBRARY: ${{ github.workspace }}/build-linux/libMobileGL.so` (`:424`); the runtime artifact
  is unpacked and that path asserted to exist (`:478-481`).
- Second configure (`:483-502`), separate build dir `build-retrace`, separate ccache key:
  ```
  -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF
  -DMOBILEGL_BUILD_TRACE_REPLAY=ON
  -DMOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY="${MOBILEGL_LIBRARY}"
  ```
- Builds only `--target mobilegl_trace_replay` (`:504-505`), normalizes CTest paths (`:526-536`), and packages
  **exactly two files** (`:538-543`): `build-retrace/tools/trace_replay/mobilegl_trace_replay` and
  `build-retrace/tools/trace_replay/CTestTestfile.cmake`. Artifact `mobilegl-trace-replay`.
- Consequence for P1: the *absolute paths* of the trace case commands (replay exe, `MOBILEGL_LIBRARY`, fixture
  paths) are frozen into that `CTestTestfile.cmake` at configure time. A later job re-creates the same paths by
  unpacking into the same workspace layout, it does not reconfigure.

### `trace-cases` / `trace-fixtures` / `retrace` / `retrace-summary` / `remove-artifact-clutter`
- `trace-cases` (`:552-570`) emits two outputs from `tools/trace_replay/trace_cases.py --ci`:
  `matrix` (`--format github-test-matrix`) and `names` (`--format names`).
- `trace-fixtures` (`:572-635`) is a per-**case** matrix (names only): derive cache key
  (`.github/scripts/trace-fixture-cache.sh key`), restore, verify, else `fetch-trace-fixture-lfs.sh`, stage the
  files listed by `trace_cases.py --format fixture-files --case <name>`, upload as `trace-fixture-<case>`.
- `retrace` (`:637-741`) is the per-`(backend, case)` matrix, `fail-fast: false`, **`max-parallel: 4`**.
  It downloads `trace-fixture-<case>` into `tools/trace_replay/fixtures/` (`:660-669`), unpacks both runtime
  tarballs (`:693-698`), then in `build-retrace/tools/trace_replay`:
  ```
  ulimit -c unlimited; sysctl kernel.core_pattern=/tmp/core.%e.%p
  export MOBILEGL_MAGMA_R11G11B10F_FALLBACK=1              # DirectVulkan
  export MOBILEGL_MAGMA_{FIX_ITERATIONRP_SUBGROUP_SCRATCH,DERIVE_NUM_SUBGROUPS,ITERATIONRP_FIX_BARRIER}=1
                                                            # DirectVulkan + iterationrp case
  export MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE=1       # DirectVulkan + improved-transparency-26.3
  ctest -V --no-tests=error -R '^MobileGLTraceReplay\.<case>\.<backend>$'
  ```
  (`:700-723`). **These `export MOBILEGL_*` lines are the second proof that job env reaches the retrace test
  process** — they are not in any `ENVIRONMENT` property (see A4) and they demonstrably take effect.
- Artifacts: `retrace-core-dumps-*` on failure, `retrace-result-<backend>-<case>` always (`:725-741`).
- `retrace-summary` (`:743-782`) renders `render_retrace_summary.mjs` over all `retrace-result-*`.
- `remove-artifact-clutter` (`:784-833`) deletes `trace-fixture-*` and `retrace-result-*` artifacts, retaining
  fixtures only for failed retrace jobs.

### `pipe-gates` (`test.yml:835-887`) — the MGPipe source gate, where P1's new gates belong
Deliberately independent of `build-linux` (`:837-838`). Four steps:
1. `python3 scripts/gen_pipe.py` + `git diff --exit-code -- MobileGL/MG_Pipe/generated` (`:850-853`).
2. stdio-instrumentation grep over `MobileGL/MG_Backend` and `MobileGL/MG_State` (`:862-869`) — bans
   `fprintf(stderr|stdout`, `printf(`, `puts(`, `std::cout|cerr`, with **no exceptions list**.
3. `python3 scripts/gen_pipe_dirty_surface.py --summary` — informational, and the comment says explicitly
   "It becomes a gate in P1, when the mapping file exists to diff against" (`:871-874`).
4. `python3 scripts/check_doc_citations.py docs/Disaggregated/*.md || true` — warning-only until the docs
   settle (`:876-887`).

## A2. ctest labels

Only three labels exist in the tree:
- `unit` — every `MG_Test` target (`MobileGL/MG_Test/CMakeLists.txt:65`, `Pipe/CMakeLists.txt:28`,
  `Util/CMakeLists.txt:56-58`, `Purity/CMakeLists.txt:13`).
- `integration-gpu` — every registration in `MobileGL/MG_IntegrationTest/CMakeLists.txt` (13 of them).
- `benchmark` — MG_Benchmark.
- **The trace-replay tests carry NO label at all** (`tools/trace_replay/CMakeLists.txt:350-379` sets only
  `ENVIRONMENT`). They are selected purely by the `-R '^MobileGLTraceReplay\.…$'` regex.

## A3. `MobileGL/MG_IntegrationTest/` — registration, env plumbing, harness

### Target and sources
`add_executable(MobileGLIntegrationTest …)` with `Main.cpp`, two Harness TUs and **79 scenario TUs**
(`CMakeLists.txt:51-130`). Links `GTest::gtest` (not `gtest_main`) plus `MobileGL_s` on desktop /
`MobileGL` (shared) on Android (`:29-36`, `:137-141`). `Main.cpp` installs a `HarnessBanner`
`testing::Environment` and calls `RUN_ALL_TESTS()` (`Main.cpp:62-66`); the banner prints backend, renderer,
surface and `EGL_PLATFORM`, and states whether it is skipping or failing every scenario.

**371 `TEST_F` cases** across `Scenarios/*.cpp` (counted by `grep -cE '^\s*TEST_F\('`).

### The 13 ctest registrations (`CMakeLists.txt:400-633`)
All share `LABELS integration-gpu`, `DISCOVERY_TIMEOUT 30`, `TIMEOUT ${MGL_ITEST_TIMEOUT}` where
`MGL_ITEST_TIMEOUT = 120` (`:393`, with the comment "a GPU test that wedges must fail the run, not hang it").

| Prefix | `TEST_FILTER` | cases | ENVIRONMENT var |
|---|---|---|---|
| `DirectGLES.` | — (all) | 371 | `MGL_ITEST_GLES_ENVIRONMENT` (`:400-407`) |
| `DirectVulkan.` | — (all) | 371 | `MGL_ITEST_VULKAN_ENVIRONMENT` (`:409-416`) |
| `DirectVulkan.AsyncCompile.` | `XfbAfterClipDistanceScenario.*` | 5 | `…VULKAN_ASYNC_ENVIRONMENT` (`:429-437`) |
| `DirectGLES.ForcedDepthStencilEmulation.` | `DepthStencilReadback*Scenario.*` | 20 | `…GLES_FORCED_DS_ENVIRONMENT` (`:447-455`) |
| `DirectGLES.UnlocatedIoBlocks.` | `UnlocatedIoBlockScenario.*` | 3 | `…GLES_UNLOCATED_IO_BLOCKS_ENVIRONMENT` (`:464-472`) |
| `DirectGLES.AsyncOn.` / `DirectVulkan.AsyncOn.` | `AsyncCompileScenario.*` | 6 + 6 | `:484-501` |
| `DirectGLES.AsyncOff.` / `DirectVulkan.AsyncOff.` | one case | 1 + 1 | `:511-528` |
| `DirectGLES.OptimisticShaderStatus.` / `DirectVulkan.…` | one case | 1 + 1 | `:537-554` |
| `DirectGLES.NoViewportArrayEmulation.` | one case | 1 | `:561-569` |
| `DirectVulkan.PrimGenReroute.` | `PrimitivesGeneratedNoXfbScenario.*` | 8 | `:580-588` |
| `DirectGLES.WidenedPacked16.` | `CopyImagePacked16Scenario.*` | 3 | `:598-606` |
| `DirectGLES.PointSizeDemotion.` / `DirectVulkan.…` | `PointSizeDemotionScenario.*` | 5 + 5 | `:615-633` |

**Total ctest entries under `integration-gpu`: 808.** Each entry is one process launch of
`MobileGLIntegrationTest --gtest_filter=<one case>`, and each process does a full EGL bring-up preceded by a
`fork()` pre-flight (`Harness/HeadlessGL.cpp:323-343`). Process count, not GL work, is the dominant cost of the
lane.

### The env-join idiom — copy it exactly
`mgl_itest_join_environment(outVar …)` (`:307-317`) escapes each `;` as `\\;` because
`gtest_discover_tests` forwards `PROPERTIES` as a flat list; without it "only `MOBILEGL_BACKEND_TYPE` reaches
the test and the vendor/ICD pinning is lost" (`:301-306`).

`MGL_ITEST_COMMON_ENV` (`:274-281`) = `__EGL_VENDOR_LIBRARY_FILENAMES=<detected>` +
`MOBILEGL_ITEST_REQUIRE_GPU=1` *only when the CMake option is ON* (`:279-281`; the option itself at `:258-259`
defaults OFF, which is why CI passes it as job env instead). `MGL_ITEST_VULKAN_ENV` (`:283-299`) adds
`VK_ICD_FILENAMES=<pin>` and, when the pin is lavapipe, the three iterationRP repairs.

`CMakeLists.txt:339-344` states the rule in bold: every mode list **appends** to `MGL_ITEST_COMMON_ENV` /
`MGL_ITEST_VULKAN_ENV`, because "A ctest ENVIRONMENT property REPLACES the job environment rather than adding
to it, so an entry that lists only its mode variable would silently lose the EGL vendor and Vulkan ICD pinning".

### How a scenario reads configuration (the `MG_Config` question)
It does **not** read `MG_Config`. `Harness/ScenarioFixture.h:34-65` defines `AmbientQuirk` /
`AmbientQuirkFromEnvironment(name)` which re-implements `ConfigLoader`'s truthiness rule character for
character, and the header states the two reasons (`:38-48`): the feature table is an internal symbol and the
Android build links the shipping `-fvisibility=hidden` library; and an in-process poke is already too late for
anything latched at initialization. The one exception is `Harness/BackendCapsPeek.h:20-28`, which peeks at the
active backend's caps block and **returns false on Android**, touching nothing — the sanctioned shape for a
desktop-only internal peek from this module.

`ScenarioTest::SetUp` (`ScenarioFixture.h:69-104`): `FAIL()` when `MOBILEGL_ITEST_REQUIRE_GPU` is set and the
harness is unusable, else `GTEST_SKIP()`; derived fixtures must gate on `Ready()`, never `IsSkipped()`
(`:106-109`, with the SIGSEGV story that motivated it).

### The "arming assertion" idiom — the model for proving verify is actually on
Three lanes pin a quirk *and* a per-lane log file so the test can prove the library armed it:
`MGL_ITEST_GLES_UNLOCATED_IO_BLOCKS_ENVIRONMENT` (`:366-369`),
`MGL_ITEST_VULKAN_PRIMGEN_REROUTE_ENVIRONMENT` (`:374-377`),
`MGL_ITEST_{GLES,VULKAN}_POINT_SIZE_DEMOTION_ENVIRONMENT` (`:383-390`).
The comment at `:361-365` explains why: the arming signal is a latched `MGLOG_I` and there is no other way for
a test process to learn it fired. Consumption side:
`Scenarios/UnlocatedIoBlockScenario.cpp:354-360` and `:489`,
`Scenarios/PrimitivesGeneratedNoXfbScenario.cpp:253` and `:526`,
`Scenarios/PointSizeDemotionScenario.cpp:304-308` and `:489` — each reads `MOBILEGL_LOG_FILE_PATH`, trusts only
bytes written after the case started, and `GTEST_SKIP()`s with an explicit message when the path is unset.

### Local runner
`MobileGL/MG_IntegrationTest/run_integration_test.sh` — `./run_integration_test.sh espryt|magma [gtest args]`;
pins `__EGL_VENDOR_LIBRARY_FILENAMES` / `VK_ICD_FILENAMES` from `MGL_EGL_VENDOR` / `MGL_VK_ICD` (defaults are
NVIDIA paths), sets `EGL_PLATFORM=${EGL_PLATFORM:-x11}`, and honours `MOBILEGL_ITEST_REQUIRE_GPU`. Note it is
*not* the ctest path: it does not carry the iterationRP pins or any mode variable.

## A4. `tools/trace_replay/` — the retrace gate

### Case manifest
`trace_cases.json`: `defaults` (854x480, ssim 0.99, `timeout_seconds: 900`) + **40 cases**.
- One case is CI-excluded: `minecraft-1.21.4-rd12-odinlite-in-world` has `"ci": false`
  (`trace_cases.json:63-70`, the flag itself at `:69`).
- One case is backend-restricted: `minecraft-1.21.4-fabric-iris-iterationrp-in-world` has
  `"ci_backends": ["DirectVulkan"]` (`:286-295`, the key at `:288`).
- `timeout_seconds` distribution over the 39 CI cases: 17×900, 17×1800, 5×180.

### Counts — reconcile these three numbers before writing any job
- `trace_cases.py --ci --format github-test-matrix` → **77** entries (39 CI cases × 2 backends − 1 restricted).
  This is what the `retrace` matrix runs today (`trace_cases.py:71-98`).
- The CMake generator is called **without** `--ci` (`tools/trace_replay/CMakeLists.txt:389-394`) and
  `add_trace_replay_test_for_backends` always registers both backends (`:382-385`), so a local
  `ctest -R '^MobileGLTraceReplay\.'` in `build-retrace` registers **80** tests.
- The **79** in the task statement = 80 minus the `iterationrp` × `DirectGLES` pair that `ci_backends` marks
  unsupported. That is the desktop gate the WSL harness (`retrace_gate.py`) drives. Implementers should confirm
  which of 77 / 79 / 80 the P1 acceptance means; the docs consistently say "40 trace"
  (`ROADMAP.md:17`, `ARCHITECTURE.md:502`, `:507`).

### Registration
`add_trace_replay_test(CASE_NAME BACKEND …)` (`tools/trace_replay/CMakeLists.txt:311-380`):
```
add_test(NAME MobileGLTraceReplay.${CASE_NAME}.${BACKEND}
         COMMAND cmake -DTRACE_REPLAY_EXE=$<TARGET_FILE:mobilegl_trace_replay>
                       -DMOBILEGL_LIBRARY=${mobilegl_trace_replay_mobilegl_library}
                       -DTRACE_CASE_NAME/-DTRACE_ARCHIVE/-DTRACE_FILE/-DTRACE_GOLDEN/
                       -DTRACE_ALTERNATE_GOLDEN/-DTRACE_BACKEND/-DTRACE_TARGET_CALL/
                       -DTRACE_WIDTH/-DTRACE_HEIGHT/-DTRACE_SSIM_THRESHOLD/-DTRACE_CROP_*/
                       -DTRACE_COHERENT_AS_FLUSH/-DTRACE_OUTPUT_DIR/-DTRACE_ARTIFACT_DIR
                       -P run_trace_case.cmake)                                      (:350-372)
```
Per-test `ENVIRONMENT` (`:373-379`):
- DirectGLES: `EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1;MESA_GL_VERSION_OVERRIDE=3.3;MESA_GLSL_VERSION_OVERRIDE=330`
- DirectVulkan: the same minus `EGL_PLATFORM`.

**No `MOBILEGL_*` variable is named in either property, and no `TIMEOUT` property is set.** Two consequences:
1. `MOBILEGL_PIPE_VERIFY` / `MOBILEGL_PIPE_PUSH` exported by a job reach the replay process unimpeded — the
   same mechanism `MOBILEGL_MAGMA_R11G11B10F_FALLBACK` already uses (`test.yml:705-707`).
2. Every trace test runs under **ctest's default 1500 s timeout**. The `timeout_seconds` field in
   `trace_cases.json` is consumed only by the Android lane (`apk.yml:449` `--timeout-seconds`) — `emit_cmake`'s
   key list (`trace_cases.py:122-136`) does not include it. So the 1800 s cases already sit above the desktop
   ctest default; anything that slows retrace down by 5–10× must raise it explicitly.

`mobilegl_trace_replay_mobilegl_library` is `MOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY` when set, else
`$<TARGET_FILE:MobileGL>` (`:303-308`).

### `run_trace_case.cmake` — how a case actually runs
- Required `-D` variables checked up front (`:1-5`); defaults filled for SSIM/crop (`:7-21`).
- Wipes and recreates `${TRACE_OUTPUT_DIR}/{input,output}` (`:31-35`), extracts the `.tgz`, asserts the trace
  file exists (`:37-52`).
- `execute_process()` the replay exe with `--trace/--golden/--diff/--output/--backend/--mobilegl-library/
  --target-call/--width/--height/--ssim-threshold/--crop-*` (+`--coherent-as-flush`) (`:54-74`).
  `execute_process` inherits the environment — this is the hop `MOBILEGL_PIPE_VERIFY` travels through.
- Echoes `MOBILEGL_TRACE_GL_*` identity lines from `output/retrace.log` (`:79-86`).
- Copies actual/golden/result.json/`retrace.log`/`mobilegl.log` into `TRACE_ARTIFACT_DIR` (`:88-110`).
- **Fails** if `output/result.json` is missing (`:112-125`) or if the replay exit status is non-zero
  (`:127-129`). Both are `message(FATAL_ERROR)`, i.e. a red ctest test.

`output/mobilegl.log` exists because `trace_replay_core.cpp:1011` does
`setenv("MOBILEGL_LOG_FILE_PATH", mobileGlLogPath.c_str(), 1)` — so a retrace run already has a library log the
harness can grep, which is exactly what a verify-arming assertion needs.

### Desktop CLI has no `--env`
`trace_replay_cli.cpp:116-179` lists every option; there is no `--env`. The `K=V;K=V` passthrough
(`ApplyEnvOverrides`, `android-plugin/app/src/trace/cpp/trace_replay_core.cpp:143-155`, called at `:237`) is
reached only from the Android side (`android-plugin/trace-replay-ci.sh:174` and `:415` `--es mobilegl_env`,
`tools/trace_replay/run_android_retrace_local.py:192-196`, `:302`). Its parser is pinned by a build-time
self-running check: `mobilegl_trace_env_overrides_test` is built and **executed as a POST_BUILD command**, and
`mobilegl_trace_replay` depends on it (`tools/trace_replay/CMakeLists.txt:291-301`), so `cmake --build … --target
mobilegl_trace_replay` runs it even though the job never invokes ctest. `trace_env_overrides_test.cpp:73-92`
already uses `MOBILEGL_PIPE_PUSH` / `MOBILEGL_PIPE_VERIFY` as its example strings.

### The Android retrace lane (`apk.yml`)
`apk.yml:417-470`: per-`(backend, case)` matrix, `timeout-minutes: 75` (`:331`), env-per-knob at `:420-425`,
and `android-plugin/trace-replay-ci.sh` invoked with the case's fields including `--timeout-seconds`. The
generic `--env "K=V;K=V"` hook (`trace-replay-ci.sh:35`, `:64`, `:174`, `:415`) is where a device-side
`MOBILEGL_PIPE_VERIFY=1` would go. Trap already documented at `MEASUREMENTS.md` §5.3: an `--env` value
containing `/data/...` needs `MSYS_NO_PATHCONV=1` on a Windows host.

## A5. The Fatal / assert idiom

- `MGLOG_F` is `MOBILEGL_LOG_INTERNAL("FATAL", ANDROID_LOG_FATAL, …)` and **is live at every build level**
  (`MobileGL/MG_Util/Debug/Log.h:87-92`). It logs; it does not terminate.
- `MOBILEGL_ASSERT(cond, …)` = `MGLOG_F` ×2 + `TRAP` (`MobileGL/Defines.h:104-112`), and is **compiled to
  nothing** unless `MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`:113-115`). CI builds at
  `MOBILEGL_LOG_LEVEL_INFO` (`test.yml:85`), so **every `MOBILEGL_ASSERT` in the tree is inert in CI.**
  `TRAP` is `__builtin_debugtrap()` under clang, `raise(SIGTRAP)` elsewhere, `assert(false)` on MSVC
  (`Defines.h:88-95`).
- The **poison Fatal already exists in generated form**:
  ```cpp
  [[noreturn]] inline void MGPipeInputPoisonFatal(MGPipeInputField field, const char* verb) {
      MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"}",
              kMGPipeInputFieldNames[static_cast<SizeT>(field)], verb);
      std::abort();
  }
  ```
  `MobileGL/MG_Pipe/generated/PipeFilled.inc:298-302` (emitted by `scripts/gen_pipe.py:497-503`).
  It is `MGLOG_F` + `std::abort()`, **not** `MOBILEGL_ASSERT` — deliberately, since the assert would vanish in
  an INFO build. Companion state and predicate: `MGPipeFilledState{CurrentVerbSerial, FilledGen[61]}`
  (`PipeFilled.inc:293-296`) and `MGPipeInputFieldIsFresh` (`:304-307`), with the sticky table at `:163` and
  the "which call fills it" table at `:229`.
- The wire-protocol Fatal has the same shape: `MGPipeWireProtocolFatal` ends in `std::abort()`
  (`scripts/gen_pipe.py:340`, emitted into `PipeWire.inc`).
- Termination therefore = `SIGABRT`. A test that wants to observe it must run it in a child process.

### Death tests: there are none
`grep -rn "EXPECT_DEATH|ASSERT_DEATH|ExitedWithCode|KilledBySignal|EXPECT_EXIT|DeathTest"` over
`MobileGL/` and `tools/` returns **zero hits**. The project's established way to observe a child's exit is
raw `fork()`/`waitpid()`:
- `MobileGL/MG_Test/Wire/FdPassingTest.cpp:62` (`fork`), `:85` (`_exit(status)`), `:130-132`
  (`waitpid` + `WIFEXITED` + `WEXITSTATUS == kChildOk`).
- `MobileGL/MG_IntegrationTest/Harness/HeadlessGL.cpp:323-423` — the pre-flight: fork, child `_exit(step)`
  (`:365-368`), parent `waitpid` with `WNOHANG` polling (`:380-388`), `WTERMSIG` (`:410`), `WIFEXITED` /
  `WEXITSTATUS` (`:420-423`). Its comment at `:323-325` is the exact rationale P1 needs: "SIGTRAP is a datum
  instead of a crash. fork() gives exactly that."

## A6. Existing negative-control patterns (the shapes to imitate)

1. **`check_include_closure.py --self-test`** (`scripts/check_include_closure.py:435-548`): three canned TUs
   that must trip (`negative_controls()`, `:469-497`), a parser check over canned `-H` output (`:447-467`), a
   "synthesize a third `glslang::` token and confirm the budget trips" control (`:499-515`), and — the piece
   that makes it a gate — `if trips == 0: error("negative control did not trip: the closure gate is not
   checking anything")` (`:541-543`). It runs in two places: the `include-graph-check` job (`test.yml:349`) and
   as a ctest under `unit` (`MobileGL/MG_Test/Purity/CMakeLists.txt:9-13`), where the comment reads
   "**NO ENVIRONMENT property: it replaces the job env (ARCHITECTURE.md:567)**".
2. **`gen_pipe.py` + `git diff --exit-code`** (`test.yml:850-853`) — regeneration as the anti-drift gate, and
   `gen_pipe.py` refuses a catalogue whose payload has no field list, "a payload the G4 comparator cannot see
   is a payload MOBILEGL_PIPE_VERIFY is blind to" (`scripts/gen_pipe.py:29-30`, enforced at `:194`).
3. **Comparator unit tests**: `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:184-207` —
   `MGPipeVerify(a, b, &field)` true for equal payloads, false after `b.InstanceCount = 7` with
   `EXPECT_STREQ(field, "InstanceCount")`, plus a padding control (equal despite padding) and an array/nested
   control (`right.Color[3].Level = 2` → field `"Color"`).
4. **Poison unit test**: `PipeCatalogueTest.cpp:224-235` — `PipeInputFieldsStartUnfilled`, including the
   line that matters ("The next verb makes the same value stale, which a written-once bitmap could not see").
5. **Env-pinned negative-control ctest lanes**: `DirectGLES.NoViewportArrayEmulation.` registers the *one* case
   that describes behaviour with the emulation off (`MG_IntegrationTest/CMakeLists.txt:556-569`);
   `DirectGLES.AsyncOff.` registers the one case that has anything to say with async off (`:503-528`).
6. **Log-file arming assertions** — A3 above.
7. **`LogLevelTest`** (`MobileGL/MG_Test/Util/LogLevelTest.cpp`): links `GTest::gtest` (not `gtest_main`) so its
   own `main()` can point `MOBILEGL_LOG_FILE_PATH` at a temp file before anything latches the sink
   (`MG_Test/Util/CMakeLists.txt:19-22`); `EmitAndRead()` (`:87-107`) emits five markers and greps the file;
   `SinkIsReachableAtAll` (`:110-118`) exists purely so the suppression assertions cannot pass vacuously.
   **This is the closest existing thing to a Fatal-capture hook and the cheapest template for one.**
8. **A build-time self-running check**: `mobilegl_trace_env_overrides_test` POST_BUILD
   (`tools/trace_replay/CMakeLists.txt:291-301`) — a gate that runs even in a job that never calls ctest.

## A7. `scripts/symbol_report.py` (P0.5)

- Purpose (docstring `:10-16`): per-symbol `nm --defined-only -S` and `.text` attribution between two
  `libMobileGL.so` builds; `--strip-scope` folds de-nesting renames into a "renamed-only" bucket.
- Options (`:266-296`): `--before`, `--after`, `--nm`, `--size`, `--cxxfilt`, `--markdown`, `--json`,
  `--strip-scope` (repeatable), `--rename-map OLD=NEW`, `--only-names`, `--threshold`,
  `--fail-on-added-bytes`, `--self-test`.
- **It always exits 0**: `--fail-on-added-bytes is reserved; this run stays informational` (`:342`, docstring
  `:33-35`). Making it a gate means either implementing the flag or asserting on `--json` in the job.
- Guard rails it documents (`:23-31`): identical `CMAKE_BUILD_TYPE` (visibility presets differ,
  `CMakeLists.txt:578-598`), `MOBILEGL_ENABLE_LTO` OFF on both sides (`CMakeLists.txt:108,137-146`), same
  compiler and stdlib. It prints both paths and byte sizes in the report header.
- Parsers: `nm --defined-only -S` sized/unsized rows (`:46-52`), `size --format=sysv` rows for
  `.text/.data/.bss/.rodata/Total` (`:54-58`).
- P0.5's measured result, for calibration: "测试名零删除、`MG_Backend` 零 diff、`.text` 字节不变、符号 0 增 /
  0 删 / 42 重命名" (`ROADMAP.md:16`).
- The job name is already reserved: `monolith-symbol-report` (`ARCHITECTURE.md:568`); it does not exist in
  `test.yml` yet.

## A8. What P1 is contractually required to show

`ROADMAP.md:17` (the P1 row), acceptance column verbatim:
> pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在
> `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏
> 一个字段能在**那条 verb** 上触发 poison Fatal

Supporting design: `ARCHITECTURE.md:502` (verify is "第三种 CI 模式，40 个 trace + 全部集成测试，~5–10× 慢,
永不出货"), `:335` (`#if MOBILEGL_PIPE_PUSH` / `MGB_CTX`), the phase table at `:337-345`, the poison paragraph
at `:367` ("debug 与 disaggregated 构建里读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput,
"GetStencilState@DrawVbo"}`"), the switch tables at `:579` (`MOBILEGL_PIPE_VERIFY` is a **build-time** CMake
option, planned, not yet existing) and `:588-589` (the runtime env vars, already landed).

Runtime plumbing that already exists: `MobileGL/Config.h:319-328` (`Uint64 PipePush`, `Bool PipeVerify`),
`MobileGL/ConfigLoader.cpp:245-246`, and the note at `:243-244` that no allow-list entry is needed because
every `MOBILEGL_`-prefixed variable is accepted by construction. `QueryEnvUint64` rejects octal and negatives
(`ConfigLoader.cpp:167-190`).

---

# Part B — proposals

## B1. Running the whole integration suite a third time under verify

### B1.0 The prerequisite nobody can skip: the build
`MOBILEGL_PIPE_VERIFY` is a **build-time** option (`ARCHITECTURE.md:579`) and the poison is gated on
`MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED` (`ARCHITECTURE.md:335` region). Today CI builds Release/INFO
with `MOBILEGL_BUILD_DISAGGREGATED` OFF (`test.yml:71-93`, `CMakeLists.txt:23`), so **both the verify
comparator and the poison would be compiled out of every existing job**. A job-env `MOBILEGL_PIPE_VERIFY=1`
against today's binary is a no-op that looks green — the exact failure mode this project keeps writing
comments about.

So P1 needs a fourth build job:

```yaml
  build-linux-verify:
    runs-on: ubuntu-latest
    permissions: { actions: write, contents: read }
    env:
      BUILD_DIR: build-verify
      CCACHE_DIR: ${{ github.workspace }}/.ccache
      CCACHE_BASEDIR: ${{ github.workspace }}
      CCACHE_COMPRESS: "true"
      CCACHE_MAXSIZE: 4G
      CCACHE_NOHASHDIR: "true"
    # steps 1-8 copied verbatim from build-linux (swap, checkout --recursive, get-cmake,
    # ccache restore with key ${{ runner.os }}-test-${{ github.job }}-ccache-v1,
    # Vulkan SDK, update_glslang_sources.py, apt deps, toolchain banner)
    # Configure: build-linux's flags PLUS
    #   -DMOBILEGL_PIPE_VERIFY=ON            # new option: compiles in SnapshotFromGLContext + G4
    #   -DMOBILEGL_BUILD_DISAGGREGATED=ON    # arms the per-verb poison; needs 3rdparty/flatbuffers
    # Package as ci-artifacts/mobilegl-linux-runtime-verify.tgz, same file list as :124-147
    # Upload as artifact `mobilegl-linux-runtime-verify`
```
Notes:
- `MOBILEGL_BUILD_DISAGGREGATED=ON` also compiles `MG_Remote/**` and registers the `MG_Test/Wire` suite
  (`CMakeLists.txt:454-469`, `MG_Test/CMakeLists.txt:93-95`); it is force-reset to OFF if
  `3rdparty/flatbuffers/include` is missing (`CMakeLists.txt:440-451`), which with `submodules: recursive` it
  is not. If you would rather not couple the two, give `MOBILEGL_PIPE_VERIFY=ON` its own
  `-DMOBILEGL_PIPE_POISON=1` and make the generated `MGPipeInputPoisonFatal` call sites depend on
  `MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED || MOBILEGL_PIPE_VERIFY`.
- Keep `-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO`. Do **not** build the verify lane at
  `..._DEBUG`: that would turn on every `MOBILEGL_ASSERT` in the tree (`Defines.h:104`) and change what the
  lane is measuring. Verify's own reporting must therefore use `MGLOG_E`/`MGLOG_F`, never `MGLOG_D`.
- Its own ccache key. Do not share `build-linux`'s: different `-D` flags, different objects.

### B1.1 Registering the verify entries — do NOT reuse the existing ones with job env
Job env *would* reach the process (proved by `test.yml:274-288`), but two things break:
- the per-test `TIMEOUT 120` (`MG_IntegrationTest/CMakeLists.txt:393`) is a property, so `ctest --timeout`
  cannot raise it, and 5–10× slower scenarios will start timing out;
- there is no ctest-visible name that says "this was the verify run", so `ctest -N` parity checks and the
  retrace-summary style reporting cannot tell the two apart.

Instead add a fourteenth and fifteenth registration in
`MobileGL/MG_IntegrationTest/CMakeLists.txt`, following the file's own idiom exactly:

```cmake
# The third CI mode (ARCHITECTURE.md:502): the same scenarios, with the per-draw per-field
# shadow comparison armed. Registered rather than driven by job env for two reasons: a ctest
# ENVIRONMENT property is the only spelling that survives `mgl_itest_join_environment`'s
# escaping intact (CMakeLists.txt:301-306), and these entries need a TIMEOUT of their own -
# verify is 5-10x slower and the ambient 120 s would start killing scenarios.
option(MOBILEGL_PIPE_VERIFY "Compile in SnapshotFromGLContext() and the G4 comparators" OFF)
set(MGL_ITEST_VERIFY_TIMEOUT 900)

if (MOBILEGL_PIPE_VERIFY)
    mgl_itest_join_environment(MGL_ITEST_GLES_VERIFY_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectGLES" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-gles.log"
        ${MGL_ITEST_COMMON_ENV})
    mgl_itest_join_environment(MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT
        "MOBILEGL_BACKEND_TYPE=DirectVulkan" "MOBILEGL_PIPE_VERIFY=1"
        "MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-vulkan.log"
        ${MGL_ITEST_VULKAN_ENV})

    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectGLES.Verify."
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "integration-gpu;integration-verify"
                   TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                   ENVIRONMENT "${MGL_ITEST_GLES_VERIFY_ENVIRONMENT}")
    gtest_discover_tests(MobileGLIntegrationTest
        TEST_PREFIX "DirectVulkan.Verify."
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "integration-gpu;integration-verify"
                   TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}
                   ENVIRONMENT "${MGL_ITEST_VULKAN_VERIFY_ENVIRONMENT}")
endif()
```
Points of care, each one a rake this repo has already stepped on:
- **The `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}` tail is mandatory** (`CMakeLists.txt:339-344`).
  Omitting it loses the EGL vendor pin, the lavapipe ICD pin, `MOBILEGL_ITEST_REQUIRE_GPU`, and the three
  iterationRP repairs — and the lane would still look green.
- Two labels: keep `integration-gpu` so nothing that enumerates the lane loses these, and add
  `integration-verify` so a job can select only them. A `;`-separated LABELS value inside a
  `gtest_discover_tests PROPERTIES` list needs the same escaping treatment as ENVIRONMENT if it ever grows a
  variable — safest is to write it as `LABELS integration-gpu integration-verify` (multi-value properties
  accept repeated tokens in `gtest_discover_tests`); verify with `ctest -N -L integration-verify` in the first
  build.
- **Name parity**: `DirectGLES.Verify.<Suite>.<Case>` keeps the suffix identical to `DirectGLES.<Suite>.<Case>`,
  so `ctest -N -L integration-gpu | sed 's/\.Verify\././'` parity is mechanical — the same test
  `ARCHITECTURE.md:507-3` demands between `DirectGLES.` and `DirectGLES.Pipe.`/`DirectGLES.Split.` later.
- **Arming assertion** (otherwise the lane is unfalsifiable, the recurring failure of this suite):
  add one scenario, e.g. `Scenarios/PipeVerifyArmingScenario.cpp`, that
  (a) skips unless `AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") == AmbientQuirk::On`
      (`ScenarioFixture.h:56-65`),
  (b) skips with an explicit message when `MOBILEGL_LOG_FILE_PATH` is unset (copy
      `UnlocatedIoBlockScenario.cpp:354-360` and its message at `:489`),
  (c) draws once and asserts the library's latched `MGLOG_I("MGPipe: verify armed, N fields, K payloads")`
      appears in the bytes written after the case started, and that no `Fatal{PipeVerifyDiffer` line does.
  This is what turns "the option was compiled out" and "the build ran with `MOBILEGL_PIPE_VERIFY` spelled
  wrong" from silent green into red.

### B1.2 The CI job
```yaml
  integration-verify:
    runs-on: ubuntu-latest
    needs: build-linux-verify
    steps:
      # identical to `integration` (test.yml:205-296) except:
      #   - download/unpack artifact `mobilegl-linux-runtime-verify` into build-verify/
      #   - repeat the "Normalize CTest command paths" step over build-verify (test.yml:234-244)
      #   - working-directory: build-verify
      env:
        MOBILEGL_ITEST_REQUIRE_GPU: "1"
        MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH: "1"
        MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS: "1"
        MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER: "1"
      run: |
        ulimit -c unlimited
        sudo sysctl -w kernel.core_pattern='/tmp/core.%e.%p'
        ctest --output-on-failure -L integration-verify --no-tests=error
```
`--no-tests=error` is doing real work here: if the option was OFF at configure the label matches nothing and
the job fails instead of passing empty. Keep the `integration-core-dumps` upload — a verify divergence that
aborts leaves a core, and that core names the diverging draw.

### B1.3 Cost
- Entries: 371 × 2 = **742 new ctest entries** (the verify lane covers the two ambient registrations; the 11
  filtered mode lanes stay pull-only — they pin *other* quirks and doubling them would multiply the matrix for
  no new information).
- Each entry is one process: full EGL/Vulkan bring-up plus one `fork()` pre-flight (`HeadlessGL.cpp:323-343`).
  Process startup dominates today; verify multiplies only the in-scenario GL work by the documented 5–10×
  (`ARCHITECTURE.md:502`). Expect the job to land somewhere between 2× and 5× the existing `integration`
  job's wall time — measure it on the first run and set `timeout-minutes` from that (the whole file has none
  today, so a wedged verify job would burn 6 h).
- If it proves too slow for every push: split by backend into two jobs (`-L integration-verify -R '^DirectGLES\.'`
  and `^DirectVulkan\.`), which halves wall time at no extra runner-minute cost.

### B1.4 Locally
```
cmake -S . -B build-verify -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
  -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
  -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON \
  -DMOBILEGL_ITEST_REQUIRE_GPU=ON \
  -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/<vendor>.json \
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/<vendor>.json
cmake --build build-verify --parallel "$(nproc)"
ctest --test-dir build-verify -L integration-verify --output-on-failure
```
`-DMOBILEGL_ITEST_REQUIRE_GPU=ON` here (not env) because locally the option bakes it into the property
(`MG_IntegrationTest/CMakeLists.txt:279-281`) and you want the same thing CI gets. Do **not** use
`run_integration_test.sh` for this: it carries neither the mode variables nor the iterationRP pins.

## B2. Running the retrace gate a third time under verify

### B2.1 The good news: no CMake change is needed for the env
No trace test's `ENVIRONMENT` property names any `MOBILEGL_*` variable
(`tools/trace_replay/CMakeLists.txt:373-379`), and the existing job already exports three of them
(`test.yml:705-722`). So `export MOBILEGL_PIPE_VERIFY=1` in the job step reaches the replay process, through
`run_trace_case.cmake`'s `execute_process` (`:54-74`), which inherits.

### B2.2 The bad news: the timeout, the library, and the silence
1. **Timeout.** Trace tests have no `TIMEOUT` property, so they run under ctest's 1500 s default. Because the
   property is absent, `ctest --timeout` *does* apply — this is the one place it is unambiguous:
   `ctest -V --no-tests=error --timeout 10800 -R '^MobileGLTraceReplay\.…$'`.
   Also raise the job's own `timeout-minutes` (there is none today).
2. **Which library.** The trace `CTestTestfile.cmake` hard-codes `-DMOBILEGL_LIBRARY=<workspace>/build-linux/
   libMobileGL.so` (baked at configure from `test.yml:424` / `:501`). The cheapest correct trick: in the
   verify retrace job, unpack the **verify** runtime tarball so that the verify `libMobileGL.so` lands at
   `build-linux/libMobileGL.so`, and unpack `mobilegl-trace-replay` unchanged. The replay binary itself does
   not need rebuilding — it `dlopen`s the library named on the command line. Assert the swap actually happened
   (`test -f build-linux/libMobileGL.so && nm -D build-linux/libMobileGL.so | grep -q MGPipeVerify` or the
   arming line, see 3).
3. **Silence.** Nothing today makes a verify divergence red. Decide and implement one contract:
   - **Recommended**: divergence is fatal by default — `MGLOG_F("MGPipe: Fatal{PipeVerifyDiffer, \"%s@%s\",
     draw=%llu}", field, verb, serial)` then `std::abort()`, exactly like `MGPipeInputPoisonFatal`
     (`PipeFilled.inc:298-302`). The replay process dies with SIGABRT → `replay_result != 0` →
     `run_trace_case.cmake:127-129` `FATAL_ERROR` → red ctest test, and `/tmp/core.*` is uploaded by the
     existing failure step. One env escape hatch, `MOBILEGL_PIPE_VERIFY_FATAL=0` (log-and-count, for triage
     and for the integration arming lane which must survive to read its own log), added next to
     `features.PipeVerify` in `ConfigLoader.cpp:246`.
   - **Plus an arming check in the harness**, so a build with the option off cannot pass this lane silently.
     Add to `run_trace_case.cmake`, guarded on a new `-DTRACE_PIPE_VERIFY=ON` passed only by the verify job:
     ```cmake
     if(TRACE_PIPE_VERIFY)
         if(NOT EXISTS "${mobilegl_log}")
             message(FATAL_ERROR "verify run produced no mobilegl.log")
         endif()
         file(STRINGS "${mobilegl_log}" armed REGEX "MGPipe: verify armed")
         if(NOT armed)
             message(FATAL_ERROR "MOBILEGL_PIPE_VERIFY=1 but the library never reported arming")
         endif()
         file(STRINGS "${mobilegl_log}" differ REGEX "Fatal\\{PipeVerifyDiffer")
         if(differ)
             message(FATAL_ERROR "verify divergence: ${differ}")
         endif()
     endif()
     ```
     `mobilegl_log` is already defined at `run_trace_case.cmake:80` and already copied to the artifact dir at
     `:107-109`. `-DTRACE_PIPE_VERIFY` needs one line in `add_trace_replay_test`
     (`tools/trace_replay/CMakeLists.txt:350-372`) forwarding a cache variable, or simply
     `-DTRACE_PIPE_VERIFY=$ENV{MOBILEGL_PIPE_VERIFY}`-style detection inside the script
     (`if(DEFINED ENV{MOBILEGL_PIPE_VERIFY} AND NOT "$ENV{MOBILEGL_PIPE_VERIFY}" STREQUAL "0")`) — the latter
     needs no registration change at all and is the smaller diff.

### B2.3 Job shape
```yaml
  retrace-verify:
    name: retrace verify (${{ matrix.backend }}, ${{ matrix.case }})
    runs-on: ubuntu-latest
    needs: [build-linux-verify, build-retrace, trace-cases, trace-fixtures]
    if: ${{ always() && needs.build-linux-verify.result == 'success' && needs.build-retrace.result == 'success' }}
    timeout-minutes: 240
    strategy:
      fail-fast: false
      max-parallel: 4
      matrix: ${{ fromJSON(needs.trace-cases.outputs.matrix) }}   # or a reduced matrix, see below
    steps:
      # identical to `retrace` (test.yml:651-741) except:
      #   - download `mobilegl-linux-runtime-verify` and unpack it so the verify .so lands at
      #     build-linux/libMobileGL.so
      #   - export MOBILEGL_PIPE_VERIFY=1 alongside the existing per-case exports
      #   - ctest ... --timeout 10800
      #   - artifact names suffixed -verify so retrace-summary and remove-artifact-clutter
      #     (test.yml:784-833, which greps job names beginning "retrace (") are unaffected
```
It reuses the **same** `trace-fixture-<case>` artifacts the `retrace` matrix downloads, so the ~800 MB of
fixtures (94 files in `tools/trace_replay/fixtures/`, individual `.tgz` 5–25 MB) are fetched once per run, not
twice — provided `retrace-verify` runs in the same workflow run and before `remove-artifact-clutter`. Add
`retrace-verify` to that job's `needs` (currently `needs: retrace-summary`, `test.yml:787`) or it will delete
the fixtures out from under the verify matrix.

### B2.4 Cost and cadence
- Full third pass = **+77 matrix jobs** (or 79/80 locally, see A4), at `max-parallel: 4` ≈ 20 sequential waves,
  each wave running a case that already takes minutes, now 5–10× slower. This is not a per-push cost anyone
  should pay.
- Recommended cadence, which still satisfies `ROADMAP.md:17` ("40 trace … 零分歧" is an *exit* criterion):
  - **per push on `feat/disaggregated`**: a reduced verify matrix of ~8 cases × 2 backends, chosen for path
    coverage rather than pixels — `OpenRA`, `minecraft-1.21.4-startup`, `minecraft-1.21.4-main-menu`,
    `minecraft-1.21.4-in-world`, `minecraft-1.21.4-fabric-sodium-in-world`, one Iris case,
    `minecraft-1.21.1-neoforge-create-indirect-in-world`, `improved-transparency-minecraft-26.3`. Express it as
    a new `trace_cases.py --format github-verify-matrix` (or a `"verify": true` key in
    `trace_cases.json`) so the selection is data, not YAML, and `pipe-gates`' regeneration discipline covers it.
  - **nightly / `workflow_dispatch`**: the full matrix.
  - **P1 exit and every stage boundary**: the full matrix, recorded in `MEASUREMENTS.md`.
- Known non-P1 red to exclude or expect: `minecraft-1.21.1-neoforge-create-indirect-in-world` fails on both
  devices on the `dev@81b17c0b` baseline (`MEASUREMENTS.md` §5.5) — it is a device-lane fact, not a Linux-lane
  one, but keep it in mind before attributing a verify red to P1.

### B2.5 Locally
```
cmake -S . -B build-retrace -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_BUILD_TRACE_REPLAY=ON \
  -DMOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY=$PWD/build-verify/libMobileGL.so
cmake --build build-retrace --target mobilegl_trace_replay --parallel "$(nproc)"
MOBILEGL_PIPE_VERIFY=1 ctest --test-dir build-retrace/tools/trace_replay \
  -R '^MobileGLTraceReplay\.' --timeout 10800 --output-on-failure -j2
```
(`-j2`, not `-j$(nproc)`: each case unpacks a multi-GB trace into `TRACE_OUTPUT_DIR`, which
`run_trace_case.cmake:31-33` wipes per run.)
While the trace tests carry no label, consider adding `LABELS retrace` in
`tools/trace_replay/CMakeLists.txt:374-379` as a one-line P1 quality-of-life change — every other lane in the
tree is label-selectable and this one is not.

## B3. Negative control 1 — "corrupting one snapshot field turns verify red"

Build it in three layers, cheapest first. Layers 1 and 2 run on **every** CI; layer 3 runs in the verify lanes.

### The injection knob (shared by all three layers)
Add, next to `features.PipeVerify` (`ConfigLoader.cpp:246`) and compiled only under
`#if MOBILEGL_PIPE_VERIFY` so it can never exist in a shipped build:
```
MOBILEGL_PIPE_VERIFY_CORRUPT=<FieldName>[.<Member>]   # perturb exactly this field in the SNAPSHOT arm
MOBILEGL_PIPE_VERIFY_FATAL=0|1                         # default 1: first divergence aborts
```
`<FieldName>` is one of the 61 names already generated into `kMGPipeInputFieldNames`
(`PipeFilled.inc`, list emitted by `gen_pipe.py:473-477`), so the knob's vocabulary is generated, not
hand-typed, and a renamed accessor breaks the control loudly. Perturb the **snapshot** side, never the pushed
side: corrupting the pushed side would also change what the backend renders and the control would be testing
two things at once.

### Layer 1 — unit, no GPU, `MG_Test/Pipe` (runs in the `test` job today)
Extend `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` (the comparator precedent is at `:184-207`) with a test
that drives the harness entry point rather than a payload comparator:
```cpp
TEST(PipeVerify, CorruptedSnapshotFieldIsNamedWithItsDrawSerial) {
    PipeInputs pushed{};  PipeInputs snapshot{};
    FillBothWithAKnownState(pushed, snapshot);
    const char* field = nullptr; Uint64 serial = 0;
    EXPECT_TRUE(MGPipeVerifyInputs(pushed, snapshot, &field, &serial));   // clean arm first
    ApplyVerifyCorruption(snapshot, "GetRenderStateParameters");          // the injector, not env
    EXPECT_FALSE(MGPipeVerifyInputs(pushed, snapshot, &field, &serial));
    EXPECT_STREQ(field, "GetRenderStateParameters");
}
```
The clean arm is not decoration: without it the test passes when the comparator is stuck returning false.
**Design requirement this imposes on P1**: expose the comparison as a pure function over two `PipeInputs`
(`Bool MGPipeVerifyInputs(const PipeInputs&, const PipeInputs&, const char** outField, Uint64* outSerial)`)
and the corruption as a callable (`ApplyVerifyCorruption(PipeInputs&, const char* fieldName)`) that the env
knob merely calls. If the only way to reach verify is a live GL context, this whole layer is impossible.
Registration: `PipeCatalogueTest` is already `LABELS unit` (`MG_Test/Pipe/CMakeLists.txt:28`) — but the new
test only compiles when `MOBILEGL_PIPE_VERIFY` is ON, so either guard the test body with
`#if MOBILEGL_PIPE_VERIFY … #else GTEST_SKIP() …` (visible skip, honest) or add a
`PipeVerifyTest` target registered inside `if (MOBILEGL_PIPE_VERIFY)`. Prefer the guard: a test that
disappears is a test nobody notices is gone.

### Layer 2 — end-to-end "the gate goes red", cheapest possible
One extra step in `integration-verify`, run after the green pass:
```yaml
      - name: Negative control - a corrupted snapshot field must turn the lane red
        working-directory: build-verify
        env:
          MOBILEGL_ITEST_REQUIRE_GPU: "1"
          MOBILEGL_PIPE_VERIFY_CORRUPT: GetRenderStateParameters
        run: |
          if ctest -L integration-verify -R 'ClearThenReadPixelsScenario' --no-tests=error; then
            echo "::error::verify did not go red with a corrupted snapshot field"; exit 1
          fi
          echo "negative control tripped as expected"
```
`MOBILEGL_PIPE_VERIFY_CORRUPT` is named by no `ENVIRONMENT` property, so the job env reaches the process —
the `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH` precedent (`test.yml:274-288`) verbatim. Pick a short,
always-drawing scenario (`ClearThenReadPixelsScenario`) so this costs seconds.
The identical step belongs in the retrace verify job on one cheap case (`OpenRA`, 180 s timeout,
`trace_cases.json:14-27`), inverted the same way.

### Layer 3 — the in-suite version, so it survives outside CI too
A `PipeVerifyArmingScenario` case that runs only when `MOBILEGL_PIPE_VERIFY_CORRUPT` is set, with
`MOBILEGL_PIPE_VERIFY_FATAL=0`, draws once, and asserts the per-lane log contains
`Fatal{PipeVerifyDiffer, "GetRenderStateParameters@…"` — the `MOBILEGL_LOG_FILE_PATH` idiom from
`UnlocatedIoBlockScenario.cpp:354-360`. Register it as its own tiny lane
(`DirectGLES.VerifyCorrupted.` / `DirectVulkan.VerifyCorrupted.`, `TEST_FILTER
"PipeVerifyArmingScenario.CorruptedFieldIsReported"`) with `MOBILEGL_PIPE_VERIFY_CORRUPT` and
`MOBILEGL_PIPE_VERIFY_FATAL=0` in its ENVIRONMENT — again appending `${MGL_ITEST_COMMON_ENV}`.
Two entries, seconds of runtime, and it makes the control reproducible with one `ctest -R` on a dev box.

## B4. Negative control 2 — "omitting a field in `glGenerateMipmap`'s fill table triggers the poison Fatal on that verb"

### The injection knob
`MOBILEGL_PIPE_POISON_OMIT=<CallName>:<FieldName>` (e.g. `GenerateMipmap:GetPixelStoreParameters`), read next
to `features.PipeVerify` and compiled only where the poison is compiled. The generated per-verb filler
(G5, `PipeFilled.inc` + the ~93 boundary fill sites, `ROADMAP.md:17`) consults it and skips *stamping* that one
field's `FilledGen` for that one call — it must skip the **stamp**, not the value, so the omission is
indistinguishable from a filler that genuinely forgot. `<CallName>` is validated against the catalogue
(`PipeCalls.def`, opcode enum in `PipeWire.inc`) and `<FieldName>` against `kMGPipeInputFieldNames`; an
unknown name is itself a Fatal, so a typo cannot silently disarm the control.

### Layer 1 — unit, no GPU, no death test (runs in the `test` job today)
Extend `PipeCatalogueTest.cpp:224-235` (`PipeInputFieldsStartUnfilled` already establishes the pattern):
```cpp
TEST(PipeCatalogue, OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale) {
    MGPipeFilledState state{};
    state.CurrentVerbSerial = 7;
    FillForVerb(state, MGPWireOp::GenerateMipmap);            // the generated filler
    EXPECT_TRUE(MGPipeInputFieldIsFresh(state, kOmittedField));   // clean arm
    EXPECT_TRUE(MGPipeInputFieldIsFresh(state, kSiblingField));

    MGPipeFilledState omitted{};
    omitted.CurrentVerbSerial = 7;
    SetPoisonOmission("GenerateMipmap", "GetPixelStoreParameters");
    FillForVerb(omitted, MGPWireOp::GenerateMipmap);
    EXPECT_FALSE(MGPipeInputFieldIsFresh(omitted, kOmittedField));  // the control
    EXPECT_TRUE (MGPipeInputFieldIsFresh(omitted, kSiblingField));  // and only that one
}
```
This is the layer that proves "on **that** verb": the sibling assertion is what distinguishes a targeted
omission from a filler that stopped working entirely.

### Layer 2 — the Fatal actually fires and kills the process
The tree has **zero** gtest death tests (A6), and its own idiom for observing a child's death is
`fork()`/`waitpid()` (`MG_Test/Wire/FdPassingTest.cpp:62-132`, `HeadlessGL.cpp:323-423`). Use that, not
`EXPECT_DEATH`:
```cpp
TEST(PipeCatalogue, ReadingAnOmittedFieldAbortsNamingTheVerb) {
#if !defined(MOBILEGL_PIPE_POISON)
    GTEST_SKIP() << "poison compiled out (needs MOBILEGL_DEBUG or MOBILEGL_BUILD_DISAGGREGATED)";
#else
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        MGPipeFilledState state{};  state.CurrentVerbSerial = 1;   // nothing filled
        if (!MGPipeInputFieldIsFresh(state, MGPipeInputField::GetPixelStoreParameters)) {
            MGPipeInputPoisonFatal(MGPipeInputField::GetPixelStoreParameters, "GenerateMipmap");
        }
        ::_exit(0);                       // reaching here is the failure
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFSIGNALED(status)) << "the poison did not terminate the process";
    EXPECT_EQ(WTERMSIG(status), SIGABRT);
#endif
}
```
Why not `EXPECT_DEATH`: it would work on Linux, but (a) nothing in this tree uses it, so nobody maintains the
death-test-style/threading constraints, (b) the "fast" style forks a process that has already initialized
gtest and any static GL state, and (c) `_exit(0)` on the non-fatal path gives a *positive* failure signal that
`EXPECT_DEATH`'s "died as expected" phrasing hides. Pair it with a clean-arm sibling test (a *filled* field
must let the child reach `_exit(0)`), or the test passes whenever `MGPipeInputFieldIsFresh` is broken in the
false direction.
To also assert the **message** (the `@GenerateMipmap` half): point `MOBILEGL_LOG_FILE_PATH` at a temp file in
the suite's own `main()` — the `LogLevelTest` shape (`MG_Test/Util/CMakeLists.txt:19-22`,
`LogLevelTest.cpp:76-107`) — and after reaping, grep the file for
`Fatal{UnmigratedPipeInput, "GetPixelStoreParameters@GenerateMipmap"}`, the exact string
`PipeFilled.inc:299-300` formats. Note `MOBILEGL_LOG_ENABLE_CONSOLE` is 0 (`Defines.h:71`), so the file is
the only channel; and the child must let the sink flush before aborting (the `MGLOG_F` write precedes
`std::abort()`, but confirm the file sink is unbuffered or flushes per line in `MG_Util/Debug/Log.cpp`).

### Layer 3 — on the real verb, through the real backend, per backend
An integration scenario `PoisonOmissionScenario` in the verify build: parent forks, child sets up a texture and
calls `glGenerateMipmap`, parent asserts `WIFSIGNALED && WTERMSIG == SIGABRT` and that the per-lane log carries
`…@GenerateMipmap`. Registered as its own lane with
`MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:<field>` + `MOBILEGL_LOG_FILE_PATH=<per-lane>` in ENVIRONMENT
(appending `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}`), two entries, one per backend. The forking
is already precedented inside this module (`HeadlessGL.cpp:343`), and the GL context is per-process so the
child inherits a usable one — but fork the child **before** any scenario-local GPU work and keep the child's
work minimal, and use `_exit`, never `exit` (the reason is written at `HeadlessGL.cpp:365-368`).
Its complement — the same scenario with the omission unset, expecting `_exit(0)` and an empty log — is what
keeps this from passing on a build where `glGenerateMipmap` aborts for some unrelated reason.

### Where these gates live
Both controls' layer-1 tests belong in `MG_Test/Pipe` (label `unit`, already in the `test` job). Both
controls' layer-2 CI steps belong in the verify jobs. Register the whole thing the way
`check_include_closure.py --self-test` is registered — **always on**, not behind a manual flag
(`test.yml:330-331`, `Purity/CMakeLists.txt:9-13`). If the omission/corruption knobs ever become optional,
add the `trips == 0 → error` check (`check_include_closure.py:541-543`) so a control that stopped tripping
fails instead of silently reporting nothing.

## B5. `nm --defined-only` / `.text` attribution for P1's "pull build unchanged"

### What the acceptance actually says
`ROADMAP.md:17`: **pull** build `nm --defined-only` unchanged, `.text` differences attributable line by line,
with the empty-guard / ternary rewrites explicitly deferred to P2. `ARCHITECTURE.md:507` adds that byte
identity is dead as a general gate but two byte-level equalities survive, and `ARCHITECTURE.md:568` reserves
the job name `monolith-symbol-report`.

Why this is even achievable: phase A is a mechanical `sed` of `MG_State::pGLContext->` → `MGB_CTX->` at 293
sites (`ARCHITECTURE.md:337-341`), and in the **pull** build `MGB_CTX` expands to `::MG_State::pGLContext`
(`ARCHITECTURE.md:335`), so the preprocessed token stream is identical. The only expected `.text` movement
comes from the 58 hand-converted non-arrow uses — ~34 deleted `MOBILEGL_ASSERT`s (already no-ops in an INFO
build, so **zero** `.text` effect there), 7 empty guards, 3 ternaries, `.get()` captures, 14 `!= nullptr`,
1 comment.

### Concrete procedure
```bash
# both sides: same compiler, same build type, LTO off (CMakeLists.txt:108 default OFF), not stripped
for ref in <P0.5-baseline-sha> <P1-head-sha>; do
  git -C <worktree> checkout "$ref"
  cmake -S . -B build-sym-$ref -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
        -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF \
        -DMOBILEGL_BUILD_INTEGRATION_TEST=OFF -DMOBILEGL_BUILD_TRACE_REPLAY=OFF \
        -DMOBILEGL_BUILD_DISAGGREGATED=OFF -DMOBILEGL_PIPE_VERIFY=OFF -DMOBILEGL_ENABLE_LTO=OFF \
        -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
  cmake --build build-sym-$ref --target MobileGL --parallel "$(nproc)"
done
python3 scripts/symbol_report.py \
  --before build-sym-<baseline>/libMobileGL.so \
  --after  build-sym-<p1>/libMobileGL.so \
  --threshold 0 \
  --markdown docs/Disaggregated/P1-symbol-attribution.md \
  --json     /tmp/p1-symbols.json
```
- **`-DMOBILEGL_BUILD_DISAGGREGATED=OFF` and `-DMOBILEGL_PIPE_VERIFY=OFF` are the whole point**: the gate is
  about the *pull* build. Comparing a verify build against the P0.5 baseline is meaningless — verify compiles
  in `SnapshotFromGLContext()` and the G4 comparators by construction (`ARCHITECTURE.md:579`).
- No `--strip-scope` / `--rename-map` here: unlike P0.5 (which de-nested five types and therefore renamed 42
  mangled names, `ROADMAP.md:16`), P1 moves no type out of a scope. If the report shows a renamed-only bucket,
  something unintended de-nested and that is itself a finding.
- `--only-names` is useful for a focused pass over the accessor names in `PipeInputs`, not for the gate.

### Making it a gate rather than a report
`symbol_report.py` always exits 0 (`:342`). Two options:
- **Minimal (recommended)**: keep the script informational and assert in the job on its `--json`:
  ```bash
  python3 - <<'PY'
  import json,sys
  r=json.load(open('/tmp/p1-symbols.json'))
  added=r['added']; removed=r['removed']       # confirm the exact key names against the writer
  if added or removed:
      print("::error::pull build symbol set changed: +%d / -%d" % (len(added),len(removed))); sys.exit(1)
  PY
  ```
- **Fuller**: implement `--fail-on-added-bytes` (the flag is already declared, `:287-288`) and pass `0`.
  Note this changes a tool P0.5 shipped as informational; if you do it, keep the default behaviour unchanged.

### The job
```yaml
  monolith-symbol-report:
    name: monolith symbol / .text attribution
    runs-on: ubuntu-latest
    steps:
      - checkout (submodules: recursive), get-cmake, apt clang-20 + binutils, Vulkan SDK,
        update_glslang_sources.py                                  # same prologue as build-linux
      - build the baseline .so (git worktree at the pinned baseline sha) and the head .so with
        the identical flags above; two ccache keys
      - python3 scripts/symbol_report.py --before … --after … --markdown … --json …
      - the json assertion above (P1 only; informational before and after)
      - upload the markdown as an artifact
      - the two surviving byte-level equalities (ARCHITECTURE.md:507):
          nm --defined-only build-sym-head/libMobileGL.so | grep -q MG_Remote && exit 1   # must be empty
```
Because this needs two full builds it is the most expensive gate in the file. Run it on
`workflow_dispatch` + at stage exit, not per push. Pinning the baseline as a **stored artifact** (upload the
P0.5 `libMobileGL.so` once, download it here) halves the cost and removes the "was the baseline built the same
way" doubt — the guard rails at `symbol_report.py:23-31` exist precisely because that doubt is the usual way
this comparison goes wrong.

### The deliverable P1 owes
The `.text` "line-by-line attribution" is a document, not a script output: take the per-symbol size table from
the markdown and add a reason column drawn from the 58-site conversion list. Put it in
`docs/Disaggregated/` so `scripts/check_doc_citations.py` (`test.yml:876-887`) lints its citations, and cite
`ROADMAP.md:17` from it.

---

# Part C — trap list

1. **A ctest `ENVIRONMENT` property wins over the job env for the variables it names, and only those.**
   `test.yml:252-259` (the ICD pin that must not be exported) vs `test.yml:274-288` (the flush flag that is
   exported inline and does reach the process). `MOBILEGL_PIPE_VERIFY` is named by no property today, so
   job-env works — but the moment you add it to a property, a job-level export of the same name becomes a lie.
2. **`ARCHITECTURE.md:567` and `MG_IntegrationTest/CMakeLists.txt:339-344` say the property "REPLACES" the job
   env.** Whatever the precise CMake semantics, the project's rule is: every mode list appends
   `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}`. Follow the rule; do not relitigate it in a P1 CMake diff.
3. **`;` inside a `gtest_discover_tests PROPERTIES` value must be escaped** — use
   `mgl_itest_join_environment` (`MG_IntegrationTest/CMakeLists.txt:307-317`). Symptom of getting it wrong:
   only the first variable survives and the pins vanish silently.
4. **`ctest --timeout` cannot raise a `TIMEOUT` property.** Integration entries have `TIMEOUT 120`
   (`:393`) → new registrations need their own. Trace entries have none → `--timeout` works there, and they
   currently run under ctest's 1500 s default even though `trace_cases.json` declares 1800 s for 17 cases.
5. **`MOBILEGL_ASSERT` is a no-op in every CI build** (`Defines.h:104-115`, `test.yml:85`). Neither negative
   control may be expressed with it. The generated poison already gets this right (`MGLOG_F` + `std::abort()`,
   `PipeFilled.inc:298-302`); the ~93 hand-written fill sites must too.
6. **`MGLOG_D` is compiled out at INFO** (`Log.h:59-64`). Verify's per-draw diagnostics must be `MGLOG_E`/`MGLOG_F`,
   or you will build the verify lane at DEBUG and accidentally also turn on every assert in the tree.
7. **`MOBILEGL_LOG_ENABLE_CONSOLE` is 0** (`Defines.h:71`) and the desktop default log path is empty
   (`Defines.h:85`). Any log-reading assertion needs `MOBILEGL_LOG_FILE_PATH` set in the lane's ENVIRONMENT —
   per-lane, so nothing else appends (`MG_IntegrationTest/CMakeLists.txt:361-365`).
8. **The poison is compiled out of today's CI builds** (`MOBILEGL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED`,
   `ARCHITECTURE.md:335`; CI is Release/INFO with DISAGGREGATED OFF). Without a build change, the
   `glGenerateMipmap` control is vacuous.
9. **Any job that unpacks the runtime tgz and runs ctest must repeat the "Normalize CTest command paths" step**
   (`test.yml:174-184` and three copies).
10. **`remove-artifact-clutter` deletes `trace-fixture-*`** (`test.yml:784-833`) and currently only waits on
    `retrace-summary`. Add any new retrace matrix to its `needs`.
11. **`feat/disaggregated` is not a push trigger** (`test.yml:3-9`). Either add it or drive P1's gates with
    `workflow_dispatch`.
12. **No job in `test.yml` sets `timeout-minutes`.** A wedged verify job burns 6 h of runner time.
13. **`ENABLE_INTEGRATION_TESTS` is referenced at `MG_Test/CMakeLists.txt:100` and defined nowhere in the
    tree** (`grep` over `*.txt`/`*.cmake`/`*.yml` finds only that use), so `MG_Test/Backend/DirectVulkan` is
    never built by any lane. Not a P1 blocker, but if P1 wants a DirectVulkan-side unit test that variable is
    a dead end.
14. **`gen_pipe.py` regeneration + `git diff --exit-code` is a gate** (`test.yml:850-853`). Any new generated
    table (poison-omission vocabulary, verify field ids) must be emitted by `gen_pipe.py`, or the gate will
    either miss it or go red on an untracked file.
15. **`pipe-gates`' stdio ban covers `MobileGL/MG_Backend` and `MobileGL/MG_State`** with no exceptions
    (`test.yml:862-869`). The verify reporter and the fill sites must not print.
16. **`gen_pipe_dirty_surface.py --summary` becomes a gate in P1** (`test.yml:871-874`). Its only flag today is
    `--summary` (`scripts/gen_pipe_dirty_surface.py:140`); making it a gate means adding a mapping file plus a
    `--check` mode with a self-test, in the `check_include_closure.py` shape.
17. **The trace-replay tests carry no ctest label** (`tools/trace_replay/CMakeLists.txt:373-379`) — selection is
    by name regex only.
18. **Reconcile 77 / 79 / 80** before writing the verify matrix (A4).

---

# Part D — files an implementer will touch

| File | Why |
|---|---|
| `.github/workflows/test.yml` | new `build-linux-verify`, `integration-verify`, `retrace-verify`, `monolith-symbol-report` jobs; `remove-artifact-clutter` `needs`; trigger branch; `timeout-minutes` |
| `CMakeLists.txt` (`:5-24`, `:108`, `:440-469`, `:735-762`) | `option(MOBILEGL_PIPE_VERIFY …)`, compile definition, interaction with `MOBILEGL_BUILD_DISAGGREGATED` |
| `MobileGL/MG_IntegrationTest/CMakeLists.txt` (`:274-393`, `:400-633`) | two verify registrations + the two negative-control lanes; `MGL_ITEST_VERIFY_TIMEOUT` |
| `MobileGL/MG_IntegrationTest/Scenarios/PipeVerifyArmingScenario.cpp` (new) | arming assertion + corrupted-field control |
| `MobileGL/MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp` (new) | fork/waitpid `glGenerateMipmap` poison control |
| `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` (`:184-235`) | the two layer-1 unit controls |
| `MobileGL/MG_Test/Pipe/CMakeLists.txt` (`:28`) | if a separate `PipeVerifyTest` target is preferred |
| `tools/trace_replay/run_trace_case.cmake` (`:79-129`) | verify arming + divergence check |
| `tools/trace_replay/CMakeLists.txt` (`:311-380`) | optional `LABELS retrace`; optional `-DTRACE_PIPE_VERIFY` forward |
| `tools/trace_replay/trace_cases.json` / `trace_cases.py` (`:71-136`, `:151-196`) | the reduced verify matrix as data |
| `MobileGL/Config.h` (`:319-328`) / `MobileGL/ConfigLoader.cpp` (`:243-256`) | `PipeVerifyFatal`, `PipeVerifyCorrupt`, `PipePoisonOmit` |
| `scripts/gen_pipe.py` (`:340`, `:375-440`, `:462-510`) | G4 harness entry point, G5 omission vocabulary |
| `scripts/symbol_report.py` (`:266-296`, `:342`) | `--fail-on-added-bytes`, if the gate is made hard |
| `docs/Disaggregated/{ROADMAP,ARCHITECTURE,MEASUREMENTS}.md` | record the lane, the costs and the attribution table |
