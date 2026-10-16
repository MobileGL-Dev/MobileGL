# t1-review-v1 — adversarial review of package t1 (test and CI plumbing for the SPLIT arm)

Reviewed `p5/t1` `5175f9cc..8d0850bd` (4 commits, 13 files) + `~/w7/notes/tools/wsl_p5_{gate,rss}.sh`.
Worktree under review `~/w7/p5-t1`; a detached review worktree `~/w7/p5-t1rev` was built at
`8d0850bd` for the mutations and removed afterwards. No fix, no commit, no device, no LFS fetch.

Counts: **0 BLOCKER / 7 MAJOR / 8 MINOR / 3 QUESTION.**
Gates observed actually going red: **5 of 13** (see the table in §3). Every number t1 reports that
I could re-run, re-ran identical.

---

## 1. The MAJOR findings

### M-1. The arming conjunction's negative half is a grep for a message string the owning packages control. Renaming it arms 11 lanes and 8 of them go GREEN having run monolith.

**Evidence (mutation, observed).** In `~/w7/p5-t1rev` I changed nothing but the message text in
the six c0 stub files — `sed -i 's/Fatal{Unimplemented/Fatal{NotYetImplemented/'` over
`MG_Remote/Client/{CapsMirror,ClientSession,EmitTables}.cpp` and
`MG_Remote/Server/{PipeApplier,ServerLoop,ServerSession}.cpp`. Every entry point still ends in
`std::abort()`; `RemoteEmitTable()` still aborts on sight; `ImplementedVerbCount()` still returns
0. Reconfigured and rebuilt `build-split`, then `ctest -L integration-split`:

```
91% tests passed, 1 tests failed out of 11
  8 x Passed      (5 Split.ClearThenReadPixels, 2 Split.Triangle, 1 Split.PersistentCoherentMap)
  1 x Skipped     DirectGLES.Split.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares
  1 x Failed      DirectGLES.Split.PersistentMapArm.PersistentCoherentMapScenario...
                  "declared Which is: emulated / std::string(arm) Which is: adopted"
```
(`~/w7/p5-t1rev-mutA-isplit.log`.)

So the exact defect t1 discovered and reports as fixed in §3(a) is still reachable, by one word.
The CMakeLists comment claims the negative half "is layout-independent in the way the house rule
demands — c1, s1 and v1 have to delete those aborts to implement anything". That is not what the
probe tests. `mgl_itest_probe_for_symbol` is `file(GLOB_RECURSE …) + file(STRINGS … REGEX)`
(`MG_IntegrationTest/CMakeLists.txt:408-420`), so it tests only whether a **line of text**
matching `Fatal.Unimplemented` survives under two named directories. Any of these disarms it
without implementing anything:
* renaming the message (measured above);
* hoisting the `MGP5_C0_STUB` macro into a shared header outside `Client/` and `Server/` — the
  stubs are per-file macros today (`MG_Remote/CapsCodec.cpp:32` is already that shape);
* an implementation that keeps the abort but words it differently.

**Concrete failure scenario.** c1's author tidies the stub message while wiring the first three
verbs. On the next CI run the 11 `Split.` entries arm and 8 of them report green under names that
say they tested the split path, on an emit table that is not wired.

**What saves it today, and how thin that is.** The `integration-split` job's negative-control step
does catch this: in the fake-armed tree both controls correctly reported "entries stayed GREEN"
and would emit `::error` (verified — §3 rows 7/8). But that control is two-state, it is a CI-only
step, and the `wsl_p5_gate.sh` copy of it has its own hole (M-4).

**Fix.** Make arming a statement about *this process*, not about the source tree: assert
`MGITEST_SPLIT_EXPECT_TRANSPORT` per case against the library's own resolved transport. t1 already
carries that marker and names the gap as debt §6.3 — it is the fix for this finding, and it should
land before the lanes can arm rather than after.

### M-2. The same probe can be kept permanently DARK by a comment, and the only notice is a configure STATUS line.

**Evidence (mutation, observed).** With M-1's rename applied (i.e. "the packages have landed"),
adding one comment line to `MG_Remote/Client/CapsMirror.cpp`:

```
// NOTE: this file used to raise Fatal{Unimplemented...} before c1 landed.
```
reconfigure prints
```
-- Integration tests: MG_Remote still carries c0 signature stubs (.../Client/CapsMirror.cpp ) -
   packages c1/s1/v1 have not landed, so every DirectGLES.Split. entry stays registered and SKIPS
```
`file(STRINGS … REGEX)` cannot tell code from comments. Every `Split.` entry and both negative
controls then skip forever, in a fully implemented tree, and nothing fails or warns — the "test
quietly measuring nothing" outcome the probe's own comment block says it exists to prevent.

**Fix.** Mask comments before matching (the house already has `MaskCommentsAndQuotedText` for this
class of pass), or replace the content probe with the runtime assertion of M-1.

### M-3. The probe scans only `MG_Remote/Client` and `MG_Remote/Server`. Two other stub files under `MG_Remote` are invisible to it — so the conjunction *can* arm partially.

`grep -rl 'Fatal.Unimplemented' MobileGL/MG_Remote` returns eight files. Six are the ones the probe
watches. The other two are **`MG_Remote/CapsCodec.cpp`** (`Fatal{UnimplementedCapsCodec …} - P5
package w1 has not landed`) and **`MG_Remote/Wire/PipeWireCodec.cpp`** — both package w1's, both
outside the probed directories.

**Concrete failure scenario.** c1/s1/v1 land; w1 has not. All 11 entries arm while every wire
encode aborts. That is loud rather than silent (the abort reds the entries), but it also makes both
negative controls tautological — the entries are red whatever the knob says, so E1 and E3(a)
"pass" having proved nothing, and the job's red comes from the wrong place.

**Fix.** Probe `${MGL_ITEST_ROOT}/MobileGL/MG_Remote` recursively rather than two subdirectories.

### M-4. `wsl_p5_gate.sh`'s E1/E3(a) negative controls report "RED, as it must be" when their filter selects **zero** tests.

`wsl_p5_gate.sh` (the E1/E3(a) loop):
```
( cd build-split && env "$knob" ctest -L integration-split -R "$filt" --no-tests=error > /dev/null 2>&1
  if [ $? -eq 0 ]; then echo "*** FAIL negative control $knob left the split lane GREEN"
  else echo "negative control $knob: RED, as it must be"; fi )
```
`--no-tests=error` makes an empty selection exit non-zero, which this reads as success.
**Observed:** `ctest -L integration-split -R 'ThisMatchesNothingAtAll' --no-tests=error` → `rc=8`
→ the gate would print "RED, as it must be."

The CI copy of the same control is correct — it counts `ctest -N … | grep -c` first and `::error`s
on `< 1`. The gate script is the one a package author runs before pushing; the two should not
differ on exactly the falsifiability question. Today both filters do select 7 and 3 entries, so
the hole is latent, not active.

**Fix.** Port CI's `matched=` guard into `wsl_p5_gate.sh`.

### M-5. The run-level transport guard matches a bare `KEY=VALUE` substring, so a DEBUG-level build of a **pull** library passes it.

`run_trace_case.cmake:255` is `string(FIND "${split_log}" "MOBILEGL_TRANSPORT=inproc" …)`, and the
`integration-split` job's step is `grep -q "MOBILEGL_TRANSPORT=inproc" "${log}"`. The intended
source of that string is `ConfigLoader::InitTransport`'s INFO line
(`ConfigLoader.cpp:333`, `"Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a
real ring to an apply thread"`), which exists only under `MOBILEGL_BUILD_DISAGGREGATED`.

But `ConfigLoader.cpp:71` unconditionally logs
`MGLOG_D("Config: Accepted env variable: %s=%s", key, value)` for every `MOBILEGL_*` variable, in
every build including the pull one. At `MOBILEGL_LOG_ACTIVE_LEVEL=…_DEBUG` that line reads
`Config: Accepted env variable: MOBILEGL_TRANSPORT=inproc` and satisfies both greps.

**Observed** (with a hand-written log of exactly that shape):
```
-- VERDICT: GREEN - t1's run-level guard accepted this log at offset 167
CI step: PASSES
```
Default and CI are INFO, where `MGLOG_D` is compiled out, so this is not live today. It becomes
live for exactly the person most likely to hit it: someone who rebuilds at DEBUG to debug a split
failure. The report calls this guard "the falsifiable half" (§3(b)) and it is one grep away from
being airtight.

**Fix.** Match the distinctive part of the INFO line (`"Config: MOBILEGL_TRANSPORT=inproc - the
MGPipe record stream"`), not the substring an env dump also produces.

Related, same guard, unfixed either way: the string searched for is hard-coded `inproc` while the
condition is `NOT $ENV{MOBILEGL_TRANSPORT} STREQUAL "monolith"`, so `MOBILEGL_TRANSPORT=spawn`
(which ConfigLoader deliberately refuses by name and stays on monolith) reds with a message about
`inproc` rather than about the refusal it actually got.

### M-6. `retrace-split` is a clone of `retrace-verify` **with the negative control removed**, and the exit gate's own stated control (E2's "drop one Clear emission") is neither implemented nor named as skipped.

`retrace-verify` carries an always-on step, *"Negative control - a corrupted snapshot field must
red this retrace"* (`MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` on OpenRA/DirectGLES),
written precisely so `"40 traces, zero divergences"` is not a statement about a comparator nobody
watched. `retrace-split` has no equivalent step. BRIEF §7 E2's control is *"把 `Clear` 的发射器改
成丢弃一次（临时补丁）必须让 SSIM 掉下阈值"*; the report substitutes the transport-resolution
assertion (§3(b)) without saying the charter's control was dropped.

Those two are not equivalent. The transport assertion proves ConfigLoader printed a line; it does
not prove a single record crossed the ring. Once c1 lands, an emitter that resolves the transport
and then emits nothing still passes `retrace-split` at ssim 1.000. t1 *did* verify the pull-library
control by hand (I reproduced it, §3 row 4) — but by hand, once, locally, not as a CI step.

**Fix.** Add the pull-library control to `retrace-split` as an always-on step (swap in
`build-linux/libMobileGL.so`, assert the case reds), and record E2's emitter-drop control as an
explicit debt on the commit that lands c1.

### M-7. Exit gate **E3(e)** — "以上五条再跑一遍 ring 小到足以至少发生一次背压等待的配置" — is absent from the package and absent from the report.

`git diff 5175f9cc..8d0850bd | grep -E 'IPC_RING_MB|IPC_STAGE_MB|背压'` is empty; no lane sets a
small ring; `t1-v1.md` §5 (brief/contract disagreements) and §6 (debts) do not mention E3(e) at
all. The brief's reporting rule is *"每一条 brief/契约没做的裁定（带证据 + 推翻它需要什么）"* —
this one is silently missing rather than argued away. It may well be un-runnable before s1 lands
a ring that can backpressure; that is the argument that should have been written down.

---

## 2. MINOR and QUESTION

**N-1 (MINOR). The skip reason the 11 entries print is factually wrong.** Verbatim from
`ctest -V`:
> `MobileGL/MG_Remote/Client does not exist yet - no source there names kRemoteEmitSlotCount /
> BackendObject_Remote / CapsMirror, so package c1 has not landed…`

`MG_Remote/Client` **does** exist and **does** name those symbols — the CMake probe's positive half
is TRUE today. What disarms the lane is the new negative half (the surviving stubs). `SplitLane.h`'s
`SkipReasonForSplitOnlyAssertions()` was not updated when the conjunction was added in CMake, so
the message points an investigator at the wrong missing thing. The report's acceptance row ("The
skip names `MG_Remote/Client` and the three packages, verbatim") quotes this as evidence of
quality. The CMake `message(STATUS …)` is correct; only the runtime text is stale.

**N-2 (MINOR). Three spellings of the same symbol list.** CMakeLists probes
`kRemoteEmitSlotCount|RemoteEmitTable|CapsMirror`; its own comment above says
`kRemoteEmitSlotCount|BackendObject_Remote|CapsMirror`; `SplitLane.h` says
`kRemoteEmitSlotCount / BackendObject_Remote / CapsMirror`. Exactly the drift this package is
supposed to be against.

**N-3 (MAJOR-adjacent, filed MINOR because the local gate covers it). The three-arm name+status
identity is not checked in CI.** `integration-split`'s step *"The same integration entries under
monolith and under inproc"* writes `/tmp/names-split.txt`, asserts only that the count is `>= 1`,
**and never reads the file again**. It then runs both arms with `--output-on-failure`, under which
a skipped test is not a failure. So the failure W-3 says is the one worth catching — *"an inproc
arm that SKIPPED forty entries the monolith arm ran"* — is invisible to this job. Only
`wsl_p5_gate.sh` compares JUnit name+status. Fix: move the gate script's `statuses()` diff into the
CI step, or delete the dead file so nobody reads the step as doing the comparison.

**N-4 (MINOR). `github_split_matrix` can silently become empty on a key typo, and a comment in
test.yml claims it cannot.** Measured against `trace_cases.py`: `"ci": false` → ValueError;
`ci_backends` without DirectGLES → ValueError; `"split": "yes"` → ValueError; but
`"splitt": true` (key typo) → **loads clean**, the split subset becomes `[]`, the matrix becomes
`{"include":[]}`, and GitHub skips `retrace-split` with no red. test.yml says *"trace_cases.py
refuses a `split` case that is not in CI or does not run DirectGLES, so the subset cannot silently
become empty."* Fix: assert the matrix is non-empty in the `trace-cases` job (one line), or reject
unknown keys in the manifest.

**N-5 (MINOR). 10 of the 11 `Split.` entries have no runtime evidence of anything.** Only
`DirectGLES.Split.PersistentMapArm.` carries a `MOBILEGL_LOG_FILE_PATH` (measured: 11 entries
carry `MGITEST_SPLIT_LANE=1`, 1 of them carries a log path). `ClearThenReadPixelsScenario` and
`TriangleScenario` contain no split-specific assertion at all — once armed, their green is
"the same GL workload passed", nothing more. This is t1's debt §6.3 and it is the same gap as M-1.

**N-6 (MINOR). The "monolith" counting lane is not pinned to monolith.**
`DirectGLES.PersistentMapArm.` names `MOBILEGL_LOG_FILE_PATH` but not `MOBILEGL_TRANSPORT`, so a
job-level export reaches it. **Observed:** with `MOBILEGL_TRANSPORT=inproc ctest -R
'^DirectGLES\.PersistentMapArm'`, its own log carries `Config: MOBILEGL_TRANSPORT=inproc`. In the
inproc arm of the gate's own A/B, therefore, both halves of E3(c)'s "pair" are inproc. Nothing
breaks (each half asserts the constant `kExpectedRoundtrips = 1`), but the pair is not the pair
the name and §2c item 4 describe. The inverse is correct and deliberate: the `Split.` entries *do*
name `MOBILEGL_TRANSPORT` in a property and a job-level `monolith` cannot reach them (observed),
and `MOBILEGL_ITEST_REQUIRE_GPU` is correctly left to the job env.

**N-7 (MINOR). Both new scenarios hardcode attribute locations 0 and 1 against shaders with bare
`in vec2 aPos; in vec3 aColor;`** — no `layout(location=)`, no `glBindAttribLocation`. Location
assignment for unqualified inputs is implementation-defined. The house has both patterns already
(`VertexArrayEnableDisableScenario` binds explicitly, others use layout qualifiers), so this is a
portability foot-gun that will surface as a red on a driver whose linker orders differently — a
red for a reason that is not a defect, in the two scenarios whose whole job is the opposite.

**N-8 (MINOR). `RESOURCE_LOCK` on the two counting lanes currently guards nothing.** Each lane's
`TEST_FILTER` selects exactly one case and each has its own log path, so there is no second holder
of either lock. Harmless and defensive; the measured "3 runs at -j 8 → 4/0/2 spurious failures"
must have been measured on a different shape. I ran the full `integration-gpu` label at `-j 8`
**five** times (2 A/B arms + 3 dedicated runs) with **0 failures** — see §4.

**Q-1 (QUESTION). What can the E3 A/B still claim?** t1 measures, and I did not disturb, that on
llvmpipe the monolith arm is *adopted* (`mpr=1 pmap=0.00`) while R-6 pins split at *emulated*. Two
different buffer code paths. The only thing the A/B can then claim is `mpr` equality — and t1
implements that as "both lanes assert the same literal `1`", which is not a comparison at all: two
lanes asserting a constant would both stay green if the true value changed to 2 in both arms only
because they'd both fail, and would both stay green if the workload changed shape in a way that
kept the constant true for the wrong reason. The `pmap` half is the only real cross-arm statement
and it is one-sided (`EXPECT_GT` on the emulated lane, `RecordProperty` on the other). This is
argued honestly in §2c/§3(c); it is a question for the integrator, not a defect: **is an E3 whose
two arms run different buffer paths still the gate ROADMAP intends, or does D-3 need overturning
with a device measurement?**

**Q-2 (QUESTION). The last `MGPipe stats:` line in a lane log is a teardown window.** The counting
case reads `LastFromLaneLog()` *before* teardown, so it reads the right window — but the file ends
with `frames=2 window=0 draws=0 … pmap=0 … mpr=0` (observed). `wsl_p5_gate.sh` greps the whole file
(`grep -hoE 'mpr=[0-9]+|pmap=[0-9.]+' … | sort -u`) and therefore reports the union of the real
window and the teardown zeros. Is the gate's quoted `mpr=1 pmap=0.00` reading the window line or
the union? Worth pinning before a MEASUREMENTS row is written from it.

**Q-3 (QUESTION). `MOBILEGL_LOG_ACTIVE_LEVEL` admitting INFO is a load-bearing precondition of
every split lane and is enforced nowhere.** It is passed by hand in `build-linux-split` and by the
preamble's `$COMMON`. At WARN or above every split lane reds loudly (the message even names the
variable); at DEBUG the run-level guard silently passes a monolith library (M-5). A one-line
`if (MOBILEGL_BUILD_DISAGGREGATED AND NOT MOBILEGL_LOG_ACTIVE_LEVEL STREQUAL …_INFO/…_DEBUG)`
message would make the dependency a build fact rather than documentation. Deliberate or oversight?

---

## 3. Gate table — one row per gate t1 added

| # | gate | the mutation that must make it red | observed red? |
|---|---|---|---|
| 1 | `ctest -L integration-split --no-tests=error` matches something in `build-split` | run it in a build without `-DMOBILEGL_BUILD_DISAGGREGATED` | **YES** — `rc=8` in both `build-push` and `build-linux` |
| 2 | `nm --defined-only build-split/libMobileGL.so \| grep -i MG_Remote` non-empty (`build-linux-split`, gate part 1, `retrace-split` swap) | point it at a pull library | **YES** — pull=0, push=0, verify=0, split=188; the guard's `[ "$remote" -lt 1 ]` branch fires |
| 3 | same `nm` on the pull library must be **0** (G1's split half) | a leak of MG_Remote into the pull build | not constructed; current reading 0 |
| 4 | `run_trace_case.cmake` transport-resolution assertion on the SPLIT retrace | replay with a pull library under `MOBILEGL_TRANSPORT=inproc` | **YES** — ssim 1.000000 and then `CMake Error … never reported resolving it` (`~/w7/p5-t1rev-pullneg.log`) |
| 4b | …the same assertion, against a DEBUG-level pull log | craft the `Accepted env variable:` line | **NO — it goes GREEN** (M-5) |
| 5 | `integration-split` step "The split lane really resolved the transport" | delete/skip `DirectGLES.Split.PersistentMapArm.`, or a monolith library | not run in CI here; by inspection it reds on a missing log and on a missing line, and shares M-5's substring weakness |
| 6 | the arming conjunction keeps the 11 `Split.` entries skipping while c0's stubs remain | remove the stubs' *text* without implementing anything | **NO — 8 of 11 went GREEN on monolith** (M-1); comment-only disarm also possible (M-2) |
| 7 | negative control E1 `MOBILEGL_IPC_VERB_BARRIER=0` must red the split entries | arm the lanes with nothing implemented | **YES, the control worked** — it reported the 7 entries stayed green, i.e. `::error` in CI |
| 8 | negative control E3(a) `MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` | same | **YES, the control worked** — 3 entries stayed green → `::error` |
| 8b | …the same two controls in `wsl_p5_gate.sh` | give them a filter that matches nothing | **NO — they print "RED, as it must be"** (M-4) |
| 9 | `Split.PersistentMapArm.…TheMapLandsInTheArmItsLaneDeclares` arm assertion (`emulated`) | run the lane against the monolith path on a driver that adopts | **YES** — `declared Which is: "emulated" / std::string(arm) Which is: "adopted"`. The single strongest guard in the package, and driver-dependent by D-3's own argument |
| 10 | `add_trace_replay_test(… TRANSPORT x)` without `VARIANT` → configure `FATAL_ERROR` | pass TRANSPORT alone | by inspection only |
| 11 | `trace_cases.py` manifest validation of `split` | `ci:false` / no DirectGLES / non-bool | **YES (3 of 3 raise ValueError)**; **NO** for an unknown-key typo → empty matrix, silent (N-4) |
| 12 | G14 (no ctest name ever disappears) / G2 (pull≡push) / G1 (symbols) | — | reproduced clean, not mutated |
| 13 | ARCHITECTURE.md:521 three-arm name+status identity | make the inproc arm skip entries the monolith arm ran | **not checkable in CI** — the names file is written and never read (N-3); only `wsl_p5_gate.sh` compares |

**5 of 13 rows were observed going red for the reason they exist** (1, 2, 4, 9, 11-partial), plus
two negative controls (7, 8) observed doing their job. Two rows were observed going **green when
they should have been red** (4b, 6) and two more are structurally unable to fire (8b, 13).

---

## 4. What I verified and it held

Everything re-runnable in t1's own `~/w7/p5-t1/build-*` trees, at `8d0850bd`:

* **G1** `symbol_report.py --threshold 0` vs `~/w7/p5-before-libMobileGL.so`:
  `.text 10806323 -> 10806323 (+0, +0.000%)`, `27811 -> 27811 defined symbols: 0 added, 0 removed,
  0 resized, 0 renamed`. Identical to the report.
* **G1's split half:** `nm --defined-only … | grep -ic MG_Remote` = **0 / 0 / 0 / 188** for
  linux / push / verify / split; `MGPipeApply*` = 0 / 39 / 42 / 39.
* **`ctest -L unit`:** 1785 / 1785 / 1785 / **1842**. Exactly the preamble's baselines.
* **`ctest -L integration-gpu` in `build-split`:** `MOBILEGL_TRANSPORT=monolith` **1139/1139**,
  `=inproc` **1139/1139**, JUnit **name+status diff = 0 lines**, 204 skipped in each arm.
* **`ctest -N`:** linux 2913, push 2913, split 2981. **G2** pull-vs-push diff **0 lines**.
  **G14** vs `p5-before-ctest-names.txt` (2902): **0 removed, 11 added** — and I checked all 11
  by hand: none is a rename of an existing name, all 11 are genuinely new
  (`{DirectGLES,DirectVulkan}.{Triangle,PersistentCoherentMap}Scenario.*` plus the single
  `DirectGLES.PersistentMapArm.` lane; there is no `DirectVulkan.PersistentMapArm.`, deliberately
  asymmetric but harmless). Split-only additions are 11 `DirectGLES.Split.*` plus the 57
  `MG_Test/Wire` cases, all inside `if (MOBILEGL_BUILD_DISAGGREGATED)`, so G2 is untouched.
* **`ctest -L integration-split` in `build-split`: 11/11, all skipping**, 22 `Skipped` markers.
* **Flake hunt:** the full `integration-gpu` label at `-j 8`, **five** runs (two A/B arms + three
  dedicated): 1139/1139 every time, zero failures. No RESOURCE_LOCK-class flakes reproduced.
* **E2, the OpenRA inproc retrace, through the registered `SPLIT` variant** in a
  `-DMOBILEGL_BUILD_TRACE_REPLAY=ON` split build: `MobileGLTraceReplay.OpenRA.DirectGLES.SPLIT`
  **Passed 1.30 s**, `ssim=1.000000`, and
  `MGPipe split: OpenRA DirectGLES transport=inproc, Fatal{ lines in …/OpenRA/DirectGLES-SPLIT/output/mobilegl.log: 0`.
  **The census was read from the right file for the right arm**, and the `-SPLIT` suffix really is
  carried by both directories (measured from the generated CTestTestfile: `OpenRA/DirectGLES`,
  `OpenRA/DirectVulkan`, `OpenRA/DirectGLES-SPLIT`; artifacts `actual-images` vs
  `actual-images-SPLIT`). D-5 lands as claimed.
* The variant entry's labels really are two: generated as `LABELS "retrace\;retrace-split"`, and
  `ctest -N -L retrace-split` selects exactly the one variant.
* **ctest `ENVIRONMENT` discipline.** All three new env lists go through
  `mgl_itest_join_environment` and all three append `${MGL_ITEST_COMMON_ENV}`; the generated
  property is one `[==[…]==]` blob with `;`-joined entries, and the EGL vendor pin survives into
  the Split entries (measured in `MobileGLIntegrationTest[4x]_tests.cmake`). `MGITEST_SPLIT_LANE=1`
  appears in exactly 11 generated entries and nowhere else. `MOBILEGL_ITEST_REQUIRE_GPU` is
  correctly *not* named in any new property, so CI's job-level export reaches the lanes.
* **Workflow.** `.github/workflows/test.yml` parses (js-yaml 4.1.1, 20 jobs).
  **Nothing t1 added is behind `github.ref == 'refs/heads/feat/disaggregated'`** — the only such
  conditions in the file are the pre-existing `monolith-symbol-report` steps (`:2202,2206,2235,2239`).
  The three new jobs have no `if:` except `retrace-split`'s `needs.*.result` guard, and their
  failures are visible as their own red jobs (there is no aggregate gate job to add them to).
  `build-linux-split`'s configure flags are a faithful clone of `build-linux-verify`'s (only
  `-DMOBILEGL_PIPE_VERIFY` ↔ the three disaggregated flags differ), and the **`nm` step is present
  in the build job** — the thing whose absence lets a split lane pass having run monolith.
* **Scenario shapes.** `glFlush()` really is an empty body
  (`MG_Impl/GLImpl/Exporting/Definitions.cpp:112` is one `MGLOG_D` and nothing else) and
  `TriangleScenario` correctly has none — `glReadPixels` is the ordering point. The draw is
  VBO-backed `glDrawArrays` with no client-array index pointer, so no `MGHostSpan` in the first
  frame. `PersistentCoherentMapScenario`'s `WriteThroughTheMapThenDraw` really is `std::memcpy` +
  `glDrawArrays` with nothing between, the sequence really is map/write/draw/write/draw/readback,
  the second write is announced by nothing, and the second case repeats it across a `Present`.
  The `static_assert` that `FLUSH_EXPLICIT` is absent is real. The arm is read from
  `IsBackendPersistentMapped()` through `Harness/PersistentMapPeek` (b1's spelling) and re-asserted
  after the draws, not inferred from `pmap` — and `CounterAsDoubleOrAbsent` is the right fix:
  `pmap` genuinely lives in `FormatWindowLine`'s `bytes/f[…]` bracket as `FormatFixed2`
  (`PipeStats.cpp:205`), which `strtoll` would read as 0.
* **Commit hygiene.** Four single-line commits, no bodies, `Co-Authored-By` count **0**. Working
  tree clean. The `p5-before-*` snapshots exist and are consistent (2902 = 2913 − 11).

---

## 5. Verdict

**Admit it, with M-1/M-2/M-3 and M-6 tracked as blocking the *arming* commit rather than this
merge.** Nothing here breaks a gate that is green today; every number in `t1-v1.md` that I could
re-run came back identical, and the package's two headline discoveries (the symbol-only probe, and
SSIM being useless for the split retrace) are correct and well evidenced. What it has not achieved
is the thing it claims in §3(a): the replacement arming condition is not layout-independent, it is
a string grep, and I made 8 lanes go green against monolith with one `sed`. The right fix is
already written down as t1's own debt §6.3 — assert the resolved transport per case — and it
should land *before* c1/s1/v1 can arm the lanes, not after. M-4 (the gate script's zero-selection
control) and N-3 (the dead names file) are ten-minute fixes and should not wait.

Logs kept: `~/w7/p5-t1rev-mutA-isplit.log`, `~/w7/p5-t1rev-pullneg.log`,
`~/w7/p5-t1rev-retrace-split.log`, `~/w7/p5-t1rev-ab-{mono,inproc}.txt`. Review worktree removed;
`~/w7/p5-t1` left clean with its submodule symlinks off, as found.
