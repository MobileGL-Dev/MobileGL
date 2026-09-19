# P5 scout — test/CI plumbing a SPLIT arm needs

Read-only survey of `/home/swung/w7/pipe` @ a29807cc (P4a landed). Nothing built, nothing run,
nothing edited. Paths are tree-relative unless they start with `~`.

**The three headline facts** (also the return summary):

1. **Nothing in CI or in the phase gate ever configures `MOBILEGL_BUILD_DISAGGREGATED=ON`.** Not
   one `cmake` invocation in `.github/workflows/test.yml` passes it, and `wsl_p4a_gate.sh` does
   not either — so every line of `MG_Remote/Transport/` and all five `MG_Test/Wire` suites are
   compiled by nothing today. P5 is the phase that must create the first `ON` configuration.
2. **`ClearThenReadPixelsScenario` exists; `TriangleScenario` and `PersistentCoherentMapScenario`
   do not exist in any form.** Two new scenario files plus three `gtest_discover_tests` blocks.
3. **`~/w7/retrace_gate.py` structurally cannot see a `SPLIT`-suffixed ctest name**, because it
   parses a *foreign* reference `CTestTestfile.cmake` from `~/mgl/MobileGL/build-retrace/` rather
   than the tree under test, and its name regex rejects the suffix.

---

## 1. The integration test harness (`MobileGL/MG_IntegrationTest`)

### 1.1 How a scenario is registered

Three layers, all explicit — no globbing anywhere:

* **One executable**: `MobileGLIntegrationTest`, `MG_IntegrationTest/CMakeLists.txt:51-144`. The
  source list is hand-written: `Main.cpp`, five `Harness/*.cpp`, then 85 `Scenarios/*.cpp` lines
  (52-143). A new scenario file is invisible until a line is added here.
* **One gtest fixture per scenario file**: `class XxxScenario : public ScenarioTest {}` and then
  `TEST_F(XxxScenario, CaseName)`. `ScenarioTest` is `Harness/ScenarioFixture.h:67-125`; its
  `SetUp` brings up the shared headless context, skips (or, under
  `MOBILEGL_ITEST_REQUIRE_GPU`, fails — `ScenarioFixture.h:73-84`) when there is no GPU, and
  exposes `Ready()` as the *only* legal gate a derived `SetUp` may use (`:106-109`).
  `Main.cpp:61-65` installs only a banner `::testing::Environment`, so `--gtest_list_tests`
  (what CMake discovery runs) never touches EGL.
* **ctest names = `TEST_PREFIX` + gtest name.** The backend is latched from
  `MOBILEGL_BACKEND_TYPE` at init, i.e. one process = one backend, so the *same binary* is
  registered many times with different prefixes and environments.

### 1.2 How `DirectGLES.` / `DirectVulkan.` are built

Two ambient registrations:

```
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectGLES."     ... ENVIRONMENT "${MGL_ITEST_GLES_ENVIRONMENT}")    # :738-745
gtest_discover_tests(MobileGLIntegrationTest
    TEST_PREFIX "DirectVulkan."   ... ENVIRONMENT "${MGL_ITEST_VULKAN_ENVIRONMENT}")  # :747-754
```

and then **23 further "lane" registrations**, each = `TEST_PREFIX` + `TEST_FILTER` +
`PROPERTIES {LABELS, TIMEOUT, [RESOURCE_LOCK], ENVIRONMENT}`. Representative shapes P5 should
copy:

| lane | line | shape |
|---|---|---|
| `DirectVulkan.AsyncCompile.` | 767-775 | whole scenario, one env knob pinned |
| `DirectGLES.ForcedDepthStencilEmulation.` | 785-793 | wildcard filter `DepthStencilReadback*Scenario.*` |
| `DirectGLES.UnlocatedIoBlocks.` | 816-825 | adds `RESOURCE_LOCK <logfile>` because the case reads the library log |
| `DirectGLES.MallocPerturb.` | 838-846 | single case by full name in `TEST_FILTER` |
| `DirectGLES.MapPersistentRoundtrips.` | 1377-1394 | **two registrations sharing one prefix**, different `TEST_FILTER` each |
| `DirectGLES.ObjectSubsystemControl.{On,Off,Refused,...}` | 1512-1565 | one lane per bitmask arm |

Environment strings are *never* written inline. They are assembled by
`mgl_itest_join_environment()` (`:321-331`), which escapes the `;` separators — without it only
the first entry survives `gtest_discover_tests`' flat `PROPERTIES` forwarding (`:315-320`). And
every list **must append `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}`**, because a ctest
`ENVIRONMENT` property *replaces* rather than adds, so an entry naming only its own knobs silently
loses the EGL-vendor / Vulkan-ICD pinning (`:678-682`, restated at `:1036-1039` and `:1064-1066`).

### 1.3 What it takes to add `DirectGLES.Split.*`

**`ClearThenReadPixels` exists.** `Scenarios/ClearThenReadPixelsScenario.cpp`; fixture declared at
`:60`; five cases at `:80, :143, :191, :241, :308`; already in the source list at
`CMakeLists.txt:76`. So `DirectGLES.Split.ClearThenReadPixelsScenario.*` needs **only** a
registration block.

**`Triangle` does not exist.** There is no `TriangleScenario` anywhere in the tree. A whole-tree
grep for `Triangle` under `MobileGL/` returns only helper functions inside other scenarios:
`PostLinkAttachScenario.cpp:204` `DrawFullViewportTriangle`, `RelinkStageSetScenario.cpp:212`
`DrawTriangle`, `ClipDistanceScenario.cpp:134` `DrawClippedTriangle`,
`PipelineFailureScenario.cpp:86` `FullscreenTriangleStrip`. Two readings of the gate row:
(a) P5 authors a new minimal `Scenarios/TriangleScenario.cpp` (one VBO, one program, one
`glDrawArrays`, one `glReadPixels`) — this is what `ROADMAP.md:21`'s literal
`DirectGLES.Split.*(ClearThenReadPixels|Triangle)` reads as; or (b) the gate is satisfied by
pointing the Split prefix at an existing draw scenario. **Evidence that settles it**: the same
ROADMAP row also demands `PersistentCoherentMapScenario`, which likewise exists nowhere but in
`ARCHITECTURE.md:500` — a phase that is expected to author one new scenario file is being asked
for two. I read the gate as (a).

**`PersistentCoherentMapScenario` does not exist.** Only citations:
`docs/Disaggregated/ARCHITECTURE.md:500` (map `PERSISTENT|WRITE|COHERENT`, write, no other GL
call, draw, readback) and `ROADMAP.md:21`. New file + new source-list line + registration.

So the P5 integration-test package is, concretely:

1. `Scenarios/TriangleScenario.cpp` and `Scenarios/PersistentCoherentMapScenario.cpp`, added to
   `CMakeLists.txt`'s source list around line 143.
2. `mgl_itest_join_environment(MGL_ITEST_GLES_SPLIT_ENVIRONMENT "MOBILEGL_BACKEND_TYPE=DirectGLES"
   "MOBILEGL_TRANSPORT=inproc" "MOBILEGL_IPC_SERVER_PATH=..." ${MGL_ITEST_CAPABILITY_ENV}
   ${MGL_ITEST_COMMON_ENV})`. The `MOBILEGL_IPC_SERVER_PATH` entry is required by
   `ARCHITECTURE.md:543`: "每条新 ctest `ENVIRONMENT` 与 `add_trace_replay_test` 的 `SPLIT`
   分支带 `MOBILEGL_IPC_SERVER_PATH`" — because the integration test links `MobileGL_s`
   statically and `dladdr` fallback cannot find the server.
3. Two (or three) `gtest_discover_tests(... TEST_PREFIX "DirectGLES.Split." TEST_FILTER
   "<Scenario>.*" ...)` blocks. **Use one block per scenario**, following the
   `DirectGLES.MapPersistentRoundtrips.` precedent at `:1377-1394`: a `:`-separated multi-pattern
   `TEST_FILTER` is not used anywhere in this file today, so its CMake/list escaping is unproven
   (guess: it would work, since `TEST_FILTER` is forwarded to `--gtest_filter`, but two blocks
   cost nothing and are already proven).
4. Gate it on the build option, or not — see §2.

**Two standing rules the lane must not break.** (i) *G2*: `ctest -L integration-gpu` must be
name-for-name identical between the pull build and the push build (`:366-369`, and
`wsl_p4a_gate.sh:50` measures it). A Split family registered inside
`if (MOBILEGL_BUILD_DISAGGREGATED)` is absent from *both* of those builds, so G2 is untouched.
Registered unconditionally, it is present in both, so G2 is *also* untouched — but then the lane
either skips (if it reports "this build has no transport") or goes red. The house answer for a
capability a build may lack is a SKIP that names the missing thing, never a deleted registration
(`:341-345`). (ii) *G14*: an existing ctest name may never disappear (`:813-815`);
`wsl_p4a_gate.sh:51` counts removed/added against `~/w7/p4a-before-ctest-names.txt`. Adding names
is fine; P5 will want a fresh `~/w7/p5-before-ctest-names.txt` snapshot.

---

## 2. ctest labels

Five labels exist in the tree (plus one stray). Full inventory from a `LABELS` grep over all
`CMakeLists.txt`/`*.cmake` outside `build*` and `3rdparty`:

| label | where | count |
|---|---|---|
| `unit` | every `MG_Test/**/CMakeLists.txt`; `MG_Test/CMakeLists.txt:65`; **`MG_Test/Wire/CMakeLists.txt:35`** | ~45 registrations |
| `integration-gpu` | `MG_IntegrationTest/CMakeLists.txt` — 25 registrations (`:742` … `:1755`) | 25 |
| `integration-verify` | only inside `if (MOBILEGL_PIPE_VERIFY)` (`:1614-1759`), always paired: `LABELS "integration-gpu\;integration-verify"` at `:1684, :1692, :1706, :1715, :1728, :1737, :1746, :1755` | 8 |
| `retrace` | `tools/trace_replay/CMakeLists.txt:376, :380` | all trace cases |
| `benchmark` | `MG_Benchmark/**` (`CMakeLists.txt:41`, `Driver/CMakeLists.txt:35,60`, …) | 6 |
| `integration` (stray) | `MG_Test/Backend/DirectVulkan/CMakeLists.txt:52` — `DirectVulkanSanityTest` | 1; **nothing in CI selects it** |

How the phase gates select them:

* `test.yml:204/206` — `ctest -L unit --no-tests=error`
* `test.yml:293-298` — `ctest -L integration-gpu --no-tests=error`, then the *same whole label*
  again with `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` exported in the **job** environment
  (deliberately not a ctest property, `:290-292`)
* `test.yml:547/549` — `ctest -L integration-verify --no-tests=error`
* `test.yml:633` — a filtered `-L integration-gpu` run for the handle-ABA/CSO/subsystem controls
* `test.yml:850` — `ctest -L benchmark`
* the retrace jobs do **not** use the `retrace` label at all; they select by anchored name regex
  (`test.yml:1187`, `:1338`)
* `wsl_p4a_gate.sh:52-63` — `-L unit` in all three build dirs, `-L integration-gpu` in
  build-linux and build-push (plus re-runs at `MOBILEGL_PIPE_PUSH=0 / 0x1ff / 0x7f`),
  `-L integration-verify` in build-verify (`:66`)

**What label a Split arm should carry.** Copy the verify precedent exactly: `LABELS
"integration-gpu\;integration-split"`. Both halves matter and are argued in-file:

* the **second, private label** is what makes the lane falsifiable — `ctest -L integration-split
  --no-tests=error` in a build that forgot `-DMOBILEGL_BUILD_DISAGGREGATED=ON` matches nothing
  and *fails*, instead of reporting a green run of nothing (`:1025-1028`, the verify block's own
  argument for this);
* keeping **`integration-gpu`** too is why "a verify build's `ctest -L integration-gpu` still
  describes the whole registration set" (`:1676-1679`) — the same sentence will want to be true
  of a split build.

Note the escaping: a two-label list is written `"a\;b"`, not `"a;b"` (`:1684`).

---

## 3. The trace-replay harness

### 3.1 How a backend is chosen today

```
add_trace_replay_test(CASE_NAME BACKEND ...)                 tools/trace_replay/CMakeLists.txt:311
  add_test(NAME MobileGLTraceReplay.${CASE_NAME}.${BACKEND}                            :350-351
           COMMAND cmake -DTRACE_BACKEND=${BACKEND} ... -P run_trace_case.cmake)       :352-372
  set_tests_properties(... ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1;..."
                           LABELS retrace)                                             :373-381
add_trace_replay_test_for_backends(CASE ...) = the same twice                          :384-387
```

Cases come from `trace_cases.json` through `trace_cases.py --format cmake`
(`CMakeLists.txt:389-397`; emitter at `trace_cases.py:143-172`). `OpenRA` is
`trace_cases.json:14-28` — `verify: true`, target call 31249, 640x480, crop 1,1,638,478,
default `ssim_threshold` 0.99 (`defaults` block `trace_cases.json:2-12`, and the CMake default at
`CMakeLists.txt:335-337`).

The backend reaches the library as: `-DTRACE_BACKEND=` → `run_trace_case.cmake:61` `--backend` →
`trace_replay_cli.cpp:130` → `android-plugin/app/src/trace/cpp/trace_replay_core.cpp:161`
`setenv("MOBILEGL_BACKEND_TYPE", ...)`. That `setenv` block (`trace_replay_core.cpp:136-260`) runs
immediately before `libMobileGL.so` is loaded, and ends with
`ApplyEnvOverrides(request.envOverrides)` (`:237`) — the generic `K=V;K=V` passthrough. **That
passthrough is reachable only from the Android path**: `trace_replay_core.hpp:70`
`envOverrides`, filled by `trace_replay_jni.cpp:158` and driven by
`android-plugin/trace-replay-ci.sh:174 --env` / `run_android_retrace_local.py:197,374`. The
desktop CLI has **no `--env` option** (`trace_replay_cli.cpp:116-179` — the full option list, no
env entry).

### 3.2 Where `SPLIT` and `-DTRACE_TRANSPORT=` have to be threaded

Four edits, in order of increasing cost:

1. **`run_trace_case.cmake` consumes the transport.** Two possible spellings.
   *(a) environment* — add `TRACE_TRANSPORT` to the required/optional list at `:1-29` and
   `set(ENV{MOBILEGL_TRANSPORT} "${TRACE_TRANSPORT}")` before the `execute_process` at `:54`. This
   has exact precedent: the file already reads `$ENV{MOBILEGL_PIPE_VERIFY}` at `:153` and turns it
   into three assertions (`:153-190`). Zero C++ change, and `cmake -P`'s child inherits it.
   *(b) CLI flag* — a new `--transport` in `trace_replay_cli.cpp`, a new `request` field, a new
   `setenv` in `trace_replay_core.cpp` beside `:161`. Matches how `--backend` works.
   **Which is right is settled by** whether the split lane also needs the *Android* replay path
   (`trace_replay_jni.cpp`) to carry the transport: the Android path already has `--env`, so (a)
   covers desktop and Android both and (b) buys nothing. I'd brief (a).
2. **`add_trace_replay_test` grows a variant/suffix parameter.** `ARCHITECTURE.md:584` is explicit:
   the `SPLIT` suffix exists because otherwise the entry collides with the same case+backend
   (`add_test` with a duplicate NAME is a hard CMake error). The name line to change is
   `CMakeLists.txt:351`; the `-D` block to extend is `:352-372`; the `set_tests_properties` at
   `:373-381` must also carry `MOBILEGL_IPC_SERVER_PATH` (`ARCHITECTURE.md:543`).
3. **`trace_cases.py` has no notion of a variant.** `github_test_matrix` (`:111-118`) emits
   `{backend, case}` only; `emit_cmake` (`:143-172`) emits one
   `add_trace_replay_test_for_backends(...)` per case. A split subset would mirror the existing
   `verify` flag exactly — `verify` is validated at `:29-38`, filtered at `:85-92`, and surfaced
   as `github-verify-matrix` at `:121-122` and `--format` at `:180-192`. A `split: true` flag plus
   a `github-split-matrix` format is a four-hunk change of the same shape.
4. **`test.yml`'s retrace job cannot match a suffixed name.** `test.yml:1187` is
   `ctest -V --no-tests=error -R '^MobileGLTraceReplay\.${{ matrix.case }}\.${{ matrix.backend }}$'`
   — anchored at both ends, so `...DirectGLES.SPLIT` never matches. A split arm needs its own job
   (or a third matrix key folded into the regex).

### 3.3 What the retrace gate would need to run an inproc arm

`~/w7/retrace_gate.py` (109 lines). It does **not** read the tree's CMake:

* `REF = ~/mgl/MobileGL/build-retrace/tools/trace_replay/CTestTestfile.cmake` (`:14`) — a *foreign*
  reference build. New tests added to `~/w7/pipe`'s CMakeLists will simply not be in this file.
* `parse()` (`:18-31`) lifts each `add_test` argv and its `ENVIRONMENT` property.
* `wanted()` (`:47-50`) requires the name to match
  `r'MobileGLTraceReplay\.(.+)\.(DirectGLES|DirectVulkan)$'` **and** `group(2)` to be in the
  tree's `ci_backends` for `group(1)`. A trailing `.SPLIT` fails the regex outright; an infix
  `.SPLIT.` makes `group(1)` = `"OpenRA.SPLIT"`, which misses the `ci_backends` dict (`:46`) and
  is **silently dropped** — the gate would report "39 of 39 passed" having never run the split arm.
* `run()` (`:54-92`) rewrites exactly five argv shapes — `-DMOBILEGL_LIBRARY=`,
  `-DTRACE_OUTPUT_DIR=`, `-DTRACE_ARTIFACT_DIR=`, the three fixture paths, and the
  `run_trace_case.cmake` path (`:59-68`). A `-DTRACE_TRANSPORT=` would pass through unchanged —
  but the reference file emits none.
* `env = dict(os.environ)` at `:70`, then the per-test `ENVIRONMENT` on top (`:71-72`).

**So there are two P5 paths, and they cost very different amounts:**

* **Cheap, works today, zero code change**: run the *existing* names with the transport exported
  in the process environment — `MOBILEGL_TRANSPORT=inproc python3 ~/w7/retrace_gate.py --tree
  ~/w7/pipe --lib ~/w7/pipe/build-split/libMobileGL.so --out ... --only OpenRA`. Line `:70`
  inherits it. This is character-for-character what `wsl_p4a_gate.sh:69` already does for
  `MOBILEGL_PIPE_VERIFY=1`, and what `test.yml:1334` does for the same knob in CI. The OpenRA
  SSIM ≥ 0.99 gate is satisfied by the case's own default threshold, unchanged.
  *Caveat*: the per-test `ENVIRONMENT` from the reference file is applied **after** `os.environ`
  (`:71-72`), so this works only because no reference entry names `MOBILEGL_TRANSPORT`.
* **Full, needed for the `SPLIT` ctest name to mean anything**: point `REF` at a
  `build-retrace` configured from `~/w7/pipe` itself, and relax `wanted()`/the `ci_backends`
  lookup to tolerate a variant segment. Note `REF_FIX`/`REF_ROOT` (`:15-16`) also hard-code
  `/home/swung/mgl/MobileGL/`.

A split retrace arm in CI has an exact template: **`retrace-verify`** (`test.yml:1246-1400`). It
reuses the *unchanged* ctest names, swaps the library into the frozen
`build-linux/libMobileGL.so` path (`:1303-1322`, with an `nm` check that makes the swap
falsifiable), exports the mode knob in the job env (`:1334`), and carries a one-case negative
control (`:1356-1385`). A `retrace-split` job is that file, with `MOBILEGL_PIPE_VERIFY=1` →
`MOBILEGL_TRANSPORT=inproc` and `MGPipeVerifyInputs` → an `MG_Remote` symbol.

---

## 4. `.github/workflows/test.yml`

### 4.1 Triggers, and the TEMPORARY one

```
on:
  push:
    branches:
      - dev                                            :6
      - Feat/Backend-Direct-GLES                        :7
      - Feat/Backend-Direct-Vulkan                      :8
      # TEMPORARY, remove before merging the MGPipe work into dev: ...
      - feat/disaggregated                              :9-12
  workflow_dispatch:                                    :13-21   (input: baseline_sha, default 087685d1)
```

The trigger is `test.yml:9-12`. It is not alone — **four job steps carry the same branch guard**
and are documented as retiring with it:

* `pipe-gates` → "The buffer pool, the deferred-release drain and the rings did not move (G5)"
  and its self-test, `if: github.ref == 'refs/heads/feat/disaggregated' || github.event_name ==
  'workflow_dispatch'` (`:1694`, `:1698`; rationale `:1684-1689`);
* the P4a twin of the same pair (`:1727`, `:1731`; rationale `:1714-1717`).

`~/w7/notes/p4a/INTEGRATOR-DECISIONS.md:730` and `:792` already carry it as an open cleanup item
("TEMPORARY test.yml:9-12 trigger still present (remove before dev)", "the TEMPORARY CI trigger
branch in test.yml/apk.yml (must be ...)"). **P5 must not build anything that only runs on that
branch**: new split steps should use `workflow_dispatch` or run unconditionally, so that the day
the four lines are deleted nothing of P5's goes dark.

### 4.2 The job matrix (18 jobs)

| job | line | needs | what it does |
|---|---|---|---|
| `build-linux` | 24 | — | the ordinary library + tests + integration + benchmark; artifact `mobilegl-linux-runtime` |
| `test` | 161 | build-linux | `ctest -L unit` |
| `integration` | 217 | build-linux | `ctest -L integration-gpu` twice (ambient + `DISABLE_INVALIDATE_FLUSH=1`) |
| `build-linux-verify` | 315 | — | `-DMOBILEGL_PIPE_VERIFY=ON`; artifact `mobilegl-linux-runtime-verify`; `nm` proof the comparator is in (`:415`) |
| `integration-verify` | 484 | build-linux-verify | `-L integration-verify`, `-L unit`, the control lanes, and two "this step passes when ctest FAILS" negative controls (`:676`, `:705`) |
| `flatc-check` | 745 | — | regenerates `protocol_generated.h` with an out-of-tree flatc and diffs; **compiles no MG_Remote code** |
| `include-graph-check` | 773 | — | include-closure purity gate |
| `benchmark` | 804 | build-linux | `-L benchmark` |
| `build-retrace` | 860 | build-linux(+) | builds `mobilegl_trace_replay`; artifact carries `build-retrace/tools/trace_replay/CTestTestfile.cmake` |
| `trace-cases` | 1005 | test, benchmark, integration | emits `matrix`, `names`, `verify-matrix` from `trace_cases.py` |
| `trace-fixtures` | 1029 | trace-cases | per-case fixture cache / LFS fetch / upload |
| `retrace` | 1094 | build-linux, build-retrace, trace-cases, trace-fixtures | one job per {case, backend}, `ctest -R '^…$'` |
| `retrace-summary` | 1200 | | |
| `retrace-verify` | 1246 | build-linux-verify, build-retrace, trace-cases, trace-fixtures | `verify-matrix` subset under `MOBILEGL_PIPE_VERIFY=1` |
| `monolith-symbol-report` | 1403 | — | **workflow_dispatch only**; builds the library twice with `-DMOBILEGL_BUILD_DISAGGREGATED=OFF` (`:1467`, `:1484`) and asserts `nm --defined-only … \| grep MG_Remote` is empty (`:1491-1500`) |
| `remove-artifact-clutter` | 1522 | | |
| `pipe-gates` | 1582 | — (deliberately independent of build-linux) | `gen_pipe.py` + self-test, `symbol_report.py --self-test`, the stdio-instrumentation grep, `gen_pipe_dirty_surface.py --check/--self-test`, the two untouched-region gates, the citation lint |

**Where a split arm slots.** The verify pair is a ready-made template and P5 should clone it:

* `build-linux-split` — clone `build-linux-verify` (`:315-482`) with
  `-DMOBILEGL_BUILD_DISAGGREGATED=ON` (plus `-DMOBILEGL_PIPE_PUSH=ON`, since a remote backend
  needs the pushed block) and its own artifact name. Copy the "the library really carries it"
  `nm` step at `:415-437`, asserting an `MG_Remote` symbol instead of the comparator's. That step
  is what makes the lane falsifiable, and its absence is precisely what would let a split lane
  pass having run monolith.
* `integration-split` — clone `integration-verify` (`:484-743`), running
  `ctest -L integration-split --no-tests=error` plus a whole-label `-L integration-gpu` run under
  `MOBILEGL_TRANSPORT=inproc` in the **job** env (the `DISABLE_INVALIDATE_FLUSH` precedent at
  `:290-298` is exactly this: a knob that must not be a ctest property so it reaches every entry).
* `retrace-split` — clone `retrace-verify` (`:1246-1400`), see §3.3.
* `trace-cases` gains a `split-matrix` output beside `verify-matrix` (`:1012-1027`) if the split
  retrace arm is a subset (recommended: `OpenRA` only, which the gate names).

Note `build-linux` itself passes **no** `-DMOBILEGL_PIPE_PUSH` (`:91-105`), so it is a *pull*
build; the only push build in CI is the verify one (`MOBILEGL_PIPE_VERIFY=ON` forces
`MOBILEGL_PIPE_PUSH` ON, `CMakeLists.txt:467-473`). A split arm therefore cannot ride on
`build-linux`'s artifact.

---

## 5. `MOBILEGL_BUILD_DISAGGREGATED`

### 5.1 What the option gates

Declared `CMakeLists.txt:23`, default **OFF**, with the contract written at `:17-22`: with it OFF
"nothing under `MobileGL/MG_Remote/` is compiled, no include path is added, and no library is
linked, so `nm --defined-only libMobileGL.so | grep -i MG_Remote` is empty".

Four gated blocks, all in the root `CMakeLists.txt`:

| line | effect |
|---|---|
| `453-465` | if the flatbuffers submodule headers are missing, shadow the option OFF **as a normal variable, not a cache force** (`:460-463` explains why) |
| `503-520` | append 8 sources to `SOURCE_FILES`: `Transport/{Ring,Doorbell,ShmSegment,ShmSegmentPosix,ShmSegmentWin32,FdPassing,InProcessTransport,WireLog}.cpp` |
| `572-574` | append `-DMOBILEGL_BUILD_DISAGGREGATED=1` to `MOBILEGL_COMPILE_DEF` |
| `605-610` | append `3rdparty/flatbuffers/include` to the include path (header-only; no `add_subdirectory`, no flatc in the graph) |

Both library targets get the same `SOURCE_FILES`: `add_library(MobileGL SHARED ${SOURCE_FILES})`
at `:612-614` and `add_library(MobileGL_s STATIC ${SOURCE_FILES})` at `:681-684`. `MobileGL_s` is
what the desktop integration test links (`MG_IntegrationTest/CMakeLists.txt:29-37`), so a split
integration lane gets `MG_Remote` for free once the option is on.

One more thing the macro arms outside `MG_Remote`: the poison `#if` in
`MG_Backend/MGPipe/PipeInputs.h:19-21` has `MOBILEGL_BUILD_DISAGGREGATED` as its third arm.

`MG_Test/Wire` is registered **only** under the option: `MG_Test/CMakeLists.txt:105-106`
(`if (MOBILEGL_BUILD_DISAGGREGATED) add_subdirectory(Wire)`). Its five suites are
`Framing, Ring, InProcessTransport, ProtocolSmoke` + `FdPassing` on non-Windows
(`Wire/CMakeLists.txt:7-17`), all labelled `unit` (`:35`).

### 5.2 Is MG_Remote built in the normal CI configuration? **No.**

Every `cmake -S . -B` in `test.yml`, checked one by one:

* `build-linux` `:91-105` — no `-DMOBILEGL_BUILD_DISAGGREGATED`
* `build-linux-verify` `:384-397` — no, and the comment at `:375-382` says so explicitly: "this
  job needs neither a Debug log level nor `MOBILEGL_BUILD_DISAGGREGATED`"
* `build-retrace` `:944-955` — no
* `monolith-symbol-report` `:1461-1472` and `:1477-1489` — passes **`=OFF`** twice, on purpose,
  as the G1 control, then asserts at `:1491-1500` that the pull build defines no `MG_Remote`
  symbol

`flatc-check` (`:745-771`) only runs `scripts/gen_protocol.py` against an out-of-tree flatc build
and diffs the committed header; it compiles nothing of `MG_Remote`.

Local evidence agrees: `build-linux/CMakeCache.txt:437`, `build-push/CMakeCache.txt:437`,
`build-verify/CMakeCache.txt:437`, `build-bench/CMakeCache.txt:489` and all three
`.cxx/*/arm64-v8a/CMakeCache.txt` read `MOBILEGL_BUILD_DISAGGREGATED:BOOL=OFF`.
`wsl_p4a_gate.sh:12` (`$COMMON`) and its three configures (`:19-23`) never set it either.

**Consequence to put in the P5 brief in bold**: `MG_Remote/Transport/*.cpp` and all five
`MG_Test/Wire` suites — written in P0 and never touched since — have **never been compiled or run
by any lane**. The first `-DMOBILEGL_BUILD_DISAGGREGATED=ON` configure in P5 may well be the first
time those files see a compiler on this branch's current HEAD. Budget for it.

### 5.3 What does not exist yet

* `MOBILEGL_BUILD_DISAGGREGATED_INPROC` — not in `CMakeLists.txt`; `ARCHITECTURE.md:581` and the
  switch table at `:595` list it as "计划（P5）".
* `MG_Config::Transport` — a grep of `MobileGL/Config.h` and `MobileGL/ConfigLoader.cpp` for
  `Transport` returns **nothing**. `MOBILEGL_TRANSPORT` parsing is P5 work
  (`ARCHITECTURE.md:583`).
* The `Init.cpp` hook — `MG_Backend/Init.cpp:49-63` is a plain `switch (MG_Config::ActiveBackendType)`
  with `DirectGLES` / `DirectVulkan` / `Unknown` arms and **no `#if`**. `ARCHITECTURE.md:29`:
  "分支在 P5 落地；P0 的 `Init.cpp` 尚未含它".
* `MG_Remote/Client/` and `MG_Remote/Server/` directories — do not exist. The tree has only
  `MG_Remote/Protocol/` (3 files) and `MG_Remote/Transport/` (16 files).

---

## 6. `~/w7/notes/tools/wsl_p4a_gate.sh` — the five parts

77 lines; `wsl_p4a_gate.sh quick` skips the three retrace sweeps (`:3`, `:67`). `BASE=37da3c3a`
(`:7`). It runs the parts in the order 1, 5, 3, 2 (fast source-level answers first).

**Preamble (`:5-25`)** — three build directories, all failure-fast so a stale binary can never
produce a verdict (`:25`):

* `build-linux` — pull, reconfigured in place (`:19`)
* `build-push` — `$COMMON -DMOBILEGL_PIPE_PUSH=ON` (`:20-21`)
* `build-verify` — `$COMMON -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON` (`:22-23`)

`$COMMON` (`:12`) pins clang + ccache + Release + INFO + TEST/INTEGRATION ON + BENCHMARK OFF +
the mesa EGL vendor + the lavapipe ICD. `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` (`:16`)
makes an exit-order UAF reproduce natively (ID-18/21).

**Part 1 — interface purity (`:27-38`)**
`check_include_closure.py`; gate C (`grep -rc pGLContext MobileGL/MG_Backend` must be empty);
G13 (`MG_State` must not be inside `MG_Pipe/PipeApply.h`'s ops block); G13b
(`MGPipeUnmigratedEmulation` site count in DirectGLES); G1 `symbol_report.py` against
`~/w7/p4a-before-libMobileGL.so` with `--threshold 0`; G5 `p3a_untouched_regions.sh` (eleven
regions) and `p4a_untouched_regions.sh` (seventeen) against `$BASE`; the `RenderStateImpl` sha vs
`~/w7/p4a-before-syncrenderstate.sha`; `ctest -R HandleRecycle` in build-verify.

**Part 5 — coverage, poison, handle discipline (`:40-46`)**
`gen_pipe.py --check` + `--self-test`; `gen_pipe_dirty_surface.py --check` + `--self-test`; the
emitter/CSO unit suites in build-push via `$UNIT_RE` (`:15`); both untouched-region `--self-test`s;
`PoisonOmitted.|VerifyCorrupted.` in build-verify.

**Part 3 — behavioural A/B (`:48-63`)**
G2 (`ctest -N` name diff, pull vs push, must be 0 lines — `:49-50`); G14 (removed/added vs
`~/w7/p4a-before-ctest-names.txt` — `:51`); `ctest -L unit` in all three dirs (`:52`);
`-L integration-gpu` in build-linux and build-push with `--rerun-failed` (`:53-54`); the same
whole label three more times at `MOBILEGL_PIPE_PUSH=0 / 0x1ff / 0x7f` (`:55`); once more under
`MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` (`:56`); the 41-scenario `$FAMILY` regex (`:13`,
`:57`); G9 `TextureParamsWithoutASamplerView` (`:58`); `TextureUploadShape` (recorded, not gated —
`:59`); three negative-control scripts (`:60`); the CSO/Resource/Object subsystem controls plus
the `0x9ff` dependency-refusal arm (`:61-62`); the verify controls (`:63`).

**Part 2 — semantic shadow comparison (`:65-76`)**
`ctest -L integration-verify -j 4` in build-verify, counting and classifying `Fatal{` lines
(`:66`); then unless `quick`, three `retrace_gate.py` sweeps (`:68-75`): verify
(`MOBILEGL_PIPE_VERIFY=1`, build-verify's lib, then a check that every log carries
"MGPipe verify:" and none carries `Fatal{`), push (build-push's lib), and the `$NAMED` subset
(`:14` — `create-indirect|create-instancing|rd12-odinlite|improved-transparency-minecraft-26.3|fabric-sodium|photon-v1.3b`).

**Part 4 (device performance) is not in this script** — it lives in
`~/w7/notes/tools/wsl_p4a_bench.sh` and the A/B reducers beside it.

### P5's gate delta against this (proposal for the brief)

1. **A fourth build dir** `build-split`: `$COMMON -DMOBILEGL_PIPE_PUSH=ON
   -DMOBILEGL_BUILD_DISAGGREGATED=ON` (and `-DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON` once it
   exists). Same fail-fast rule as `:25`.
2. **Part 1 gains the surviving byte equality** `ARCHITECTURE.md:524`:
   `nm --defined-only build-linux/libMobileGL.so | grep MG_Remote` must be **empty**, and
   `nm -D build-split/libMobileGLServer.so | grep mobilegl_server_main` must **hit** in
   RelWithDebInfo. Neither is asserted anywhere today outside `test.yml:1491-1500`
   (workflow_dispatch-only).
3. **Part 3 gains the split A/B**: `ctest -L unit` in `build-split` — **this is the first run of
   the five `MG_Test/Wire` suites** — plus `ctest -L integration-gpu` there under
   `MOBILEGL_TRANSPORT=monolith` and again under `=inproc`, diffed name-for-name (the G2 shape,
   extended to a third arm, which `ARCHITECTURE.md:521` asks for verbatim: "`ctest -L
   integration-gpu` 在 `DirectGLES.` 与 `DirectGLES.Pipe.`/`DirectGLES.Split.` 之间逐名相同").
   Plus `ctest -L integration-split` (falsifiability: it must match something).
4. **Part 2 gains an inproc retrace arm**: the zero-code-change form is
   `MOBILEGL_TRANSPORT=inproc python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib
   ~/w7/pipe/build-split/libMobileGL.so --out ~/w7/retrace-out/p5-split -j 4 --only 'OpenRA'`
   (§3.3). The gate row asks only for OpenRA ≥ 0.99.
5. **Peak RSS for both roles is a brand-new measurement.** Nothing in the tree or in
   `~/w7/notes/tools/` measures RSS: a grep for `maxrss|VmHWM|peak rss|peakRss` across all
   `*.py/*.sh/*.cpp/*.h/*.yml/*.cmake` outside `build*`/`3rdparty` returns three **comments**
   only (`MG_State/GLState/ProgramState/ProgramLinkTask.h:105`,
   `MG_Util/Async/ShaderCompilePool.h:79`, `MG_Util/Async/ShaderCompilePool.cpp:20`) and zero
   hits under `~/w7/notes/tools/*.sh`. `--benchmark`'s JSON carries frame times only
   (`trace_replay_cli.cpp:32-40`). So "两个角色峰值 RSS 在案" needs a new mechanism — cheapest is
   `/usr/bin/time -v` or `/proc/<pid>/status:VmHWM` sampling around the inproc retrace, recorded
   into MEASUREMENTS rather than gated.
6. **`persistent-map-push` 出数** likewise needs a counter published through `PipeStats`, the way
   `mpr=` is (`MG_Util/Metrics/PipeStats.h:126`, emitter `MG_Impl/Pipe/PipeFill.cpp:736`), and a
   scenario that reads the summary line back from a **per-lane** `MOBILEGL_LOG_FILE_PATH` with a
   `RESOURCE_LOCK` — the rule at `MG_IntegrationTest/CMakeLists.txt:802-815` and `:1318-1322`
   (the library opens its log `fopen(path,"w")`, so every process in a lane truncates it; a
   log-reading case needs a lane whose `TEST_FILTER` selects it alone).

---

## 7. Traps worth repeating in the brief

* ctest `ENVIRONMENT` **replaces**, never appends; a property entry **overrides the job env**
  (`MG_IntegrationTest/CMakeLists.txt:678-682`, `:1036-1043`). That last point is load-bearing in
  the other direction too: the ambient `Verify.` lanes deliberately name *neither*
  `MOBILEGL_PIPE_VERIFY_CORRUPT` nor `MOBILEGL_PIPE_POISON_OMIT`, precisely so CI's job-level
  exports can reach them (`:1040-1043`). A split lane that wants a job-level `MOBILEGL_TRANSPORT`
  override must likewise not name it in its property.
* `;` inside an `ENVIRONMENT` or `LABELS` value must be escaped `\;` (`:321-331`, `:1684`).
* A log-reading case needs its own lane, its own `MOBILEGL_LOG_FILE_PATH`, and a
  `RESOURCE_LOCK` named after that log (`:802-815`) — measured: 3 runs of the full label at
  `-j 8` produced 4 / 0 / 2 spurious failures without it.
* An existing ctest name may never disappear (G14); renaming to fix a race is not allowed, which
  is why `RESOURCE_LOCK` exists (`:813-815`).
* `gtest_discover_tests` runs `--gtest_list_tests` at build time; it must not need a GPU, a
  transport, or a server process. A Split registration that made discovery spawn anything would
  break every build.
* The P4a-era capability markers (`MGL_ITEST_CAPABILITY_ENV`, `:358-655`) are the house pattern
  for "this arm only asserts once the owning package landed": a `file(GLOB_RECURSE …
  CONFIGURE_DEPENDS)` content probe for a **symbol**, never a filename (`:389-398` argues this at
  length). If P5's Split lanes need to skip before `MG_Remote/Client` exists, copy
  `mgl_itest_probe_for_symbol` (`:402-414`) rather than inventing a guard.
