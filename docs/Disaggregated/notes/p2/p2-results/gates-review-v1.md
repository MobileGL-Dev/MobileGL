# Adversarial review — package E `p2/gates` (v1)

Tree `~/w7/p2-gates` @ `aef43358`, six commits on `refs/tags/p2/contract` (`9c6a8a25`).
Reviewed against BRIEF-P2 §A (G1–G14), §B (D1–D20), §C.4, §C.5, §D, §E and the implementer's
result file `scratchpad/wf5/p2-results/gates-v1.md`.

**Verdict: NOT APPROVED — 2 majors.**

Everything the result file claims about *this* tree reproduced. What it does not claim, and what
this review found, is that two of the three "arms itself when the owning package lands"
mechanisms do not arm correctly when those packages actually land. Both are demonstrated below
against the packages as they exist in `~/w7/p2-tracker`, `~/w7/p2-espryt`, `~/w7/p2-magma`.

---

## 0. What reproduced (so the majors are not confused with noise)

All run in `~/w7/p2-gates` unless stated.

| claim | my result |
|---|---|
| G1 `symbol_report.py --threshold 0` | identical: `0 added / 0 removed / 0 renamed`, 4 resized (`RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9) — all inherited from `9c6a8a25`; E touches no pull-library source |
| G5 `RenderStateImpl` sha | `d8fd1c48…20efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| G2 `ctest -N` pull vs push | 1402 vs 1402, `diff` empty |
| G13 `grep -rc pGLContext MG_Backend` | empty |
| G13 `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| G13 `check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` |
| G9 `gen_pipe_dirty_surface.py --check` | rc 2 (flag is package B's — declared D-7) |
| `ctest -L unit` in build-linux / build-push / build-verify | `1489` entries, `100% passed` in all three (14.0 s / 17.1 s) |
| G8 `ctest --test-dir build-verify -R HandleRecycle` | `100% passed, 0 failed out of 36`; 15 executed, 42 skipped. The six `.Legacy` cases and every `TheReproducerRecyclesEveryName` really run and pass |
| G12 `ctest --test-dir build-push -R CsoContentAddressing` | all skipped with the tracker reason |
| G14 vs the contract tree | contract `build-linux` 1368 → gates 1402: **0 removed, 34 added** |
| C-1 (`p2-before-ctest-names.txt` unusable) | confirmed: baseline 2363; `pipe/build-linux` 1364, `p2-contract/build-linux` 1368, `p2-contract/build-verify` 2186, `pipe/build-verify` 2182. `comm -23 baseline -` reports **999 "removed"** against the *untouched contract tree* too. The integrator must re-capture it |
| C-2 (four resized, not three) | confirmed |
| G7 `bash scripts/g7_negative_control.sh build-push` | rc 2, refuses with the "no test matches" message |
| G7 `… --verify-patch-only` | rc 0; patch applies, `build-push` rebuilds clean, sources restored (`git status --porcelain` shows only `?? build-retrace/` before and after; the two spans files hash the same) |
| `ctest --test-dir build-bench -R DriverBench` | `DriverBench` + `DriverBenchStateToggle`, both pass, 1.67 s |

I also re-derived the chunk table's rule for **twelve** setters out of
`MobileGL/MG_State/GLState/RenderState/RenderState.cpp` (`BumpVersions()` = `++m_version` +
`++m_pipelineStateVersion`, `RenderState.h:159-162`): `SetColorMask` (`:679`, `BumpVersions`),
`SetSampleCoverage` (`:786`, `BumpVersions`), `SetPolygonMode` (`:174`, `BumpVersions`),
`SetStencilFunc` (`:637`, `++m_version` + conditional `++m_pipelineStateVersion`),
`SetScissorBox` (`:926`), `SetBlendColor` (`:740`), `SetLineWidth` (`:109`), `SetViewport`
(`:69`), `SetDepthRange` (`:753`), `SetClearColor` (`:707`), `SetPolygonOffset` (`:251`),
`SetPrimitiveRestartIndex` (`:189`) — all `++m_version` only. Every one lands in the half
`MGPipeRenderStateSpans.h` assigns it, and the partition/parity assertions are exact. **No
chunk-table or subset-hash finding.** In particular `SetColorMask` really does `BumpVersions()`,
so the G7 control's chosen break (`ColorMasks` demoted) is a genuine invariant violation and not
a control that cannot trip.

---

## MAJOR 1 — the three build-capability markers ask the **source tree**, not **this build**, so after C/D land the pull build's `AbaControl` lane goes red and its `Handles` lane goes green for nothing

`MobileGL/MG_IntegrationTest/CMakeLists.txt:363-405` decides three markers purely from file
existence / file text in the source tree:

* `:364-366` `file(GLOB … "${MGL_ITEST_ROOT}/MobileGL/MG_Backend/DirectGLES/SlotTables.h")` →
  `MGITEST_HANDLE_REKEY_DirectGLES=1`
* `:386-390` `file(STRINGS … REGEX "kMGPipeSubsystemMagmaVertexInput")` →
  `MGITEST_HANDLE_REKEY_DirectVulkan=1`
* `:391-393` `file(STRINGS … REGEX "PipeHandleAbaControl")` → `MGITEST_HANDLE_ABA_IMPLEMENTED=1`

None of them consults `MOBILEGL_PIPE_PUSH`. The CSO scenario *does* gate on it first
(`CsoContentAddressingScenario.cpp:207-214`, `MGITEST_PIPE_PUSH_BUILD`); `HandleRecycleScenario`
does not (`HandleRecycleScenario.cpp:137-141`, `:252-274`). The lanes are registered in every
build (`CMakeLists.txt:770-800`), and the `Handles`/`AbaControl` lanes are registered with
`MOBILEGL_PIPE_LEGACY_MEMOS=0` / `MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1` unconditionally.

The knob the `AbaControl` arm needs is **compiled out of a pull build**: `Config.h:341`
(`#if MOBILEGL_PIPE_PUSH` around the field) and `ConfigLoader.cpp:261-267`
(`features.PipeHandleAbaControl = QueryEnvFlag(...)` inside the same `#if`). And
`~/w7/p2-magma` puts `PipeHandleAbaControl` inside
`MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp` (1 hit) and
`kMGPipeSubsystemMagmaVertexInput` there too (4 hits), so both markers *will* be set for the
pull build the moment magma lands.

### Reproduction (pull build, `MOBILEGL_PIPE_PUSH:BOOL=OFF` in `build-linux/CMakeCache.txt`)

`AbaControl`, with the marker forced exactly as CMake will set it after magma lands:

```
cd ~/w7/p2-gates/build-linux/MobileGL/MG_IntegrationTest
env MOBILEGL_BACKEND_TYPE=DirectVulkan MGITEST_HANDLE_ARM=aba MOBILEGL_PIPE_PUSH=0 \
    MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1 MGITEST_HANDLE_ABA_IMPLEMENTED=1 \
    ./MobileGLIntegrationTest --gtest_filter='HandleRecycleScenario.AVertexArray*'
```
```
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:202: Failure
  Actual: false (… [AbaControl expects the STALE object's pixels: the two guards are
  deliberately defeated]: region x[2,126] y[2,94] should be all red, but 11625 of 11625
  pixels (100%) are not; first offender at (2,2) is green rgba(0,255,0,255))
[  FAILED  ] HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
 1 FAILED TEST
```

`Handles`, same build, marker forced as CMake will set it after espryt lands
(`~/w7/p2-espryt/MobileGL/MG_Backend/DirectGLES/SlotTables.h` already exists, 9116 bytes):

```
env MOBILEGL_BACKEND_TYPE=DirectGLES MGITEST_HANDLE_ARM=handles MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    MGITEST_HANDLE_REKEY_DirectGLES=1 \
    ./MobileGLIntegrationTest --gtest_filter='HandleRecycleScenario.AVertexArray*'
```
```
[       OK ] HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput (10 ms)
```

### Why each half is a defect

1. **A hard red on a required gate.** G2 (§A) and D.3 part 3 both run
   `ctest --test-dir build-linux -L integration-gpu` and require it green. After magma lands it
   will contain four failing `DirectVulkan.HandleRecycle.AbaControl.*` entries, failing for a
   reason that has nothing to do with what they test.
2. **A green that asserts nothing.** In the pull build no `{slot, gen}` arm is compiled at all,
   yet the `Handles` lane reports `OK` — and the skip text it would otherwise print
   (`HandleRecycleScenario.cpp:255-260`, *"this build does not have it"*) is a claim about the
   **build** that the marker does not measure. This is exactly the "test that cannot fail" the
   scenario's own header warns about.
3. **A third consequence, per D14.** `MOBILEGL_PIPE_LEGACY_MEMOS=0` in a build where every
   subsystem bit is clear is D14's `Fatal{PipeLegacyMemosDisabled}` startup condition. The pull
   build's `Handles` lanes sit on exactly that combination.

### Fix shape (not applied)

Gate all three markers on `MOBILEGL_PIPE_PUSH` the way `MGITEST_PIPE_PUSH_BUILD` already is —
i.e. `if (MOBILEGL_PIPE_PUSH)` around the capability block, or an extra
`BuildMarkerIsSet("MGITEST_PIPE_PUSH_BUILD")` check in
`HandleRecycleScenario::SkipUnlessTheArmIsAssertableHere()` before the arm-specific checks, with
a skip reason naming the pull build. One line of the same shape the CSO scenario already carries.

---

## MAJOR 2 — G12's arming probe globs `MG_Impl/Pipe/Tracker.cpp`, which package B does not create; the CSO negative control will skip forever

`MobileGL/MG_IntegrationTest/CMakeLists.txt:373-379`:

```cmake
file(GLOB MGL_ITEST_TRACKER_SOURCE CONFIGURE_DEPENDS
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe/Tracker.cpp")
if (MGL_ITEST_TRACKER_SOURCE)
    list(APPEND MGL_ITEST_CAPABILITY_ENV "MGITEST_PIPE_TRACKER_PRESENT=1")
```

Package B (`~/w7/p2-tracker` @ `e49f0ea7`) implements the tracker and the CSO cache **header-only**:

```
$ ls -l ~/w7/p2-tracker/MobileGL/MG_Impl/Pipe/
CsoCache.h  PipeFill.cpp  PipeFill.h  SetHashSuppressor.h  SlotAllocator.cpp  SlotAllocator.h  Tracker.h
$ ls ~/w7/p2-tracker/MobileGL/MG_Impl/Pipe/Tracker.cpp
ls: cannot access '…/Tracker.cpp': No such file or directory
$ git -C ~/w7/p2-tracker diff --stat refs/tags/p2/contract..HEAD | grep 'MG_Impl/Pipe'
 MobileGL/MG_Impl/Pipe/CsoCache.h            | 185 +++
 MobileGL/MG_Impl/Pipe/PipeFill.cpp          | 406 +++-
 MobileGL/MG_Impl/Pipe/PipeFill.h            |  22 +-
 MobileGL/MG_Impl/Pipe/SetHashSuppressor.h   |  85 ++
 MobileGL/MG_Impl/Pipe/Tracker.h             | 402 +++
```

So after `tracker` merges, `MGITEST_PIPE_TRACKER_PRESENT` stays unset and all four
`CsoContentAddressing.{On,Off}` entries keep skipping
(`CsoContentAddressingScenario.cpp:215-221`) with the message *"the CSO counters have no emitter
in this build: MG_Impl/Pipe/Tracker.cpp does not exist"* — a statement that will then be false.
G12 says the control **cannot rot**: *"a ctest entry proves the switch actually changes CSO
mint/bind counts"*. A permanently-skipping entry proves nothing, and D.4.5's whole
"push is slower" vs "the CSO design is slower" separation rests on it. This is the "gate that is
green because it never ran" case, in the one entry whose stated purpose is to prevent a dead
measurement knob.

The rest of the control is sound — I verified the channel end-to-end by forcing the two markers
on the push build:

```
cd ~/w7/p2-gates/build-push/MobileGL/MG_IntegrationTest
env MOBILEGL_BACKEND_TYPE=DirectGLES MGITEST_CSO_LANE=content-addressed MOBILEGL_PIPE_PUSH=0x7f \
    MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 MOBILEGL_LOG_FILE_PATH=/tmp/cso-probe.log \
    MGITEST_PIPE_PUSH_BUILD=1 MGITEST_PIPE_TRACKER_PRESENT=1 \
    ./MobileGLIntegrationTest --gtest_filter='CsoContentAddressingScenario.*'
```
```
CsoContentAddressingScenario.cpp:285: Failure
Expected: (window.binds) >= (static_cast<long long>(kDrawsPerFrame)), actual: 0 vs 16
  … It reported: MGPipe stats: frames=2 window=1 draws=16 draws/f=16.00 … cso[csom=0 csob=0] …
```

The stats line is emitted per swap, the window brackets exactly the 16 toggle draws, the `cso[]`
bracket parses, and the assertion fails for precisely the right reason. Only the *arming* probe
is wrong.

### Fix shape (not applied)

Probe something the owning package actually produces: `MG_Impl/Pipe/Tracker.h` (or
`Tracker.*`/`CsoCache.*` with a `file(GLOB)`), or better, drop the file probe and let the
scenario decide from the runtime evidence it already has — a `cso[]` bracket whose `csob` is 0
across a frame that issued 16 pipeline-changing draws is exactly "no emitter", and the scenario
could skip on that instead of on a filename. Whatever is chosen, the probe must be robust to an
owning package's file layout, which E cannot pin.

---

## Minors

1. **`format_benchmark`'s `p50` is not the device's median, and its docstring says it is.**
   `run_android_retrace_local.py:229-242` `nearest_rank_percentile` returns
   `sorted[ceil(f·n)-1]` and its docstring says *"the same rule SummarizeSeries uses on the
   device"*; `trace_replay_core.cpp:868-871` uses the even-count **average** median. For an even
   window they differ. The implementer's own pre-flight shows it: the CLI printed
   `medianCpuMs=271.766` while `format_benchmark` printed `p50=8.261ms` on the same two-frame
   run. Harmless for a paired delta (both arms use the same rule) but the line mixes two
   statistics computed by different rules and documents them as one.
2. **Both new device profiles claim a guard that does not exist.**
   `tools/device_bench/devices/xiaomi-adreno830.env:9` and `oppo-mali.env:8` say
   *"bench.sh / session.sh / profile.sh REFUSE to run against a profile that says so"*. Only
   `bench.sh` and `session.sh` gained `require_verified_profile`; `tools/device_bench/profile.sh`
   has no such check and no `--allow-unverified-profile` flag (`grep -n PROFILE_VERIFIED
   tools/device_bench/profile.sh` → nothing). `profile.sh` does not pin, so the safety impact is
   small, but the false claim sits inside the safety comment.
3. **The verified-profile guard fails open.** `require_verified_profile` is
   `if [ "${PROFILE_VERIFIED:-1}" = "1" ]; then return 0; fi` — a profile that simply omits the
   key is treated as verified. `devices/odinlite.env` carries no `PROFILE_VERIFIED` at all, yet
   the new `README.md` table lists it "verified: yes". A profile written by copying odinlite.env
   inherits the fail-open default, which is the opposite of what D-6 is for. Default to `0` and
   set `PROFILE_VERIFIED=1` in `odinlite.env` explicitly.
4. **An unrecognised arm name silently degrades to `Legacy`.**
   `HandleRecycleScenario.cpp:111-117` maps anything that is not `handles`/`aba` to
   `Arm::Legacy`, so a typo in a lane's `MGITEST_HANDLE_ARM` turns the `Handles` or `AbaControl`
   assertion into a `Legacy` assertion that passes. `CsoContentAddressingScenario.cpp:309` does
   the opposite (`FAIL() << "unknown …"`), which is the right shape; adopt it here.
5. **`scripts/g7_negative_control.sh` has still never run its real path** (declared §7). I
   confirmed the refusal (rc 2) and the mechanism (`--verify-patch-only` rc 0, tree byte-restored),
   and confirmed the regex will match: `~/w7/p2-spans` names the suite
   `TEST(RenderStateSpans, SetterConsistency)` (`MG_Test/Pipe/RenderStateSpansTest.cpp:174`), so
   `RenderStateSpans\.SetterConsistency` resolves. The ctest-red half remains unproven and is on
   the integrator's D.3.
6. **The new CI control step is thinner than its sibling.**
   `.github/workflows/test.yml` — the `The handle-ABA and CSO-content-addressing controls
   (G8, G12)` step sets only `MOBILEGL_ITEST_REQUIRE_GPU` and omits the `ulimit -c unlimited` /
   `kernel.core_pattern` lines and the three `MOBILEGL_MAGMA_*` variables the
   `Integration scenarios under MOBILEGL_PIPE_VERIFY` step above it sets. A DirectVulkan crash in
   the new step therefore leaves no core for the black-box flow the job otherwise supports.
7. **`build-verify` name count does not reproduce.** The result file's C-1 quotes **2238** names
   in `~/w7/p2-gates/build-verify`; `ctest -N` reports **2230** today. Everything else in C-1
   reproduced exactly, so this is a stale figure rather than a finding, but the integrator should
   not carry it forward.
8. **`pipe-gates` is red on this branch alone** (declared D-7): `gen_pipe_dirty_surface.py
   --check` exits 2 here. `~/w7/p2-tracker/scripts/gen_pipe_dirty_surface.py` does define
   `--check` and `--self-test`, so D.1's order fixes it — but the warning "do not run
   `gh workflow run test.yml` on a tree that has `gates` without `tracker`" must survive into the
   integrator's notes.
9. **The `-j 4` `integration-gpu` flake's pre-existence is argued, not demonstrated.** §3.6's
   evidence is structural (each pinned arming lane registers a whole scenario against one
   `MOBILEGL_LOG_FILE_PATH`) plus "they pass in isolation", which does not discriminate between
   "pre-existing race" and "E's 24 extra processes changed the timing". E touched none of those
   lanes and CI runs the label serially, so the risk is contained; a control run of
   `-L integration-gpu -j 4` on an untouched tree would settle it and was not taken (I did not
   take it either, to leave `~/w7/p2-contract` unwritten).
10. **`D.2`'s "红 before the re-key" artefact is not produced** (declared D-1). C.4 makes it E's
    first deliverable; it is genuinely unreachable from E's ownership set because the knob's
    consumer lives in `MG_Backend/DirectVulkan/**` (package D, C.5). Recording it as an
    integrator action is the right call, but the deliverable is outstanding and the integrator
    must not treat E's merge as having produced it.

## Ownership and discipline

No C.5 violation: every path in `git diff refs/tags/p2/contract..HEAD --stat` is E's
(`.github/workflows/test.yml`, `MobileGL/MG_Benchmark/**`, `MobileGL/MG_IntegrationTest/**`,
`android-plugin/app/src/trace/cpp/**`, `scripts/g7_negative_control.sh`, `tools/device_bench/**`,
`tools/trace_replay/run_android_retrace_local.py`). No fixture binary touched. Commit subjects
follow `[Type] (Scope): …`; no attribution lines. No hot-path instrumentation is committed — the
`clock_gettime(CLOCK_THREAD_CPUTIME_ID)` lands in the retrace app, not in the library, as D17 and
D.4.1 require. Six commits instead of C.4's three is declared (D-9) and is justified by the two
post-verification fixes.

The tree was left as found: `git status --porcelain` reports only `?? build-retrace/` (the
implementer's own untracked build directory) before and after this review; the g7 control
restored `MGPipeRenderStateSpans.{h,cpp}` byte-for-byte and rebuilt `build-push` from them.
