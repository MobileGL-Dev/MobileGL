# j0 — cross-family review (Claude) of `p5/j0@b2dcae71`, ID-53 ruling (1)

Reviewed range `c3e736a3..p5/j0` (one commit `b2dcae713f5c81e753fe1ddfd8203b58961d7827`, author =
committer `Swung0x48 <swung0x48@outlook.com>`, single-line subject
`[Fix] (IntegrationTest): give Split entries private diagnostic logs`, no attribution trailer, six
files, `+183/-34`). Everything below was executed in the idle worktree `~/w7/p5-j0` (`build-split`,
`build-push`); every perturbation was reverted, nothing was committed, `git status --porcelain` in
`~/w7/p5-j0` is empty at the end and the submodule links are **off**. No LFS fetch, no adb.

**Verdict: NEEDS A FIX (three edits, all outside the six files j0 touched — which is exactly the
claim j0 made and the claim that is wrong). The plumbing itself is correct and I could not break
it.**

---

## Per-check result

### Check 1 — 21 entries, 21 distinct absolute paths, the counting lane intact — **PASS**

```
cd ~/w7/p5-j0/build-split && ctest --show-only=json-v1 > ~/w7/p5-j0rev-split.json
python3 ~/w7/p5-j0rev-analyze.py ~/w7/p5-j0rev-split.json
```
```
total entries: 3135
integration-split entries: 21
entries with exactly one LOG_FILE_PATH: 21
distinct paths: 21
duplicated: []
```
`ctest -N -L integration-split` → `Total Tests: 21`.

The ENVIRONMENT property really is **appended** by CTest at test-load time rather than replaced —
this is the load-bearing assumption of `Harness/SplitLogPaths.cmake.in:5-12` and j0 asserted it
without showing it. Measured per entry: the ten default entries carry 16 env vars, the ten
`SmallRing` entries 19 (including `MOBILEGL_IPC_RING_MB`, `MOBILEGL_IPC_STAGE_MB`), and every one
of the 21 still carries `MOBILEGL_TRANSPORT`, `MOBILEGL_BACKEND_TYPE`, `MOBILEGL_IPC_SERVER_PATH`,
`MGITEST_SPLIT_LANE` and `__EGL_VENDOR_LIBRARY_FILENAMES` alongside the new
`MOBILEGL_LOG_FILE_PATH`. Nothing was lost.

`DirectGLES.Split.PersistentMapArm.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`
keeps 20 env vars including `MGITEST_PERSISTENT_MAP_ARM`, `MOBILEGL_PIPE_STATS`,
`MOBILEGL_PIPE_STATS_PERIOD`, its own
`…/MG_IntegrationTest/persistent-map-arm-split-DirectGLES.log`, and
`RESOURCE_LOCK=['persistent-map-arm-split-DirectGLES.log']`. b1's `pmap=`/`mpr=` reader is
untouched. (It is a Split entry and skips on this head, as the task says; the *property* survives,
which is what was checkable here.)

Log content, after a real lane run: 20 files under `split-logs/` + the arm file = 21; **0 empty**;
**20/20** contain `Initializing MobileGL` and the distinctive transport line
`Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an apply
thread`; **0** contain `Active() is null`. j0's §2 "correction to the premise" is confirmed.

### Check 2 — the distinctness check goes red by name — **PASS (red twice, two different shapes)**

Perturbed the file CTest actually reads,
`build-split/MobileGL/MG_IntegrationTest/SplitLogPaths.cmake`, restored from a backup afterwards.

*(a) one entry left without a path* (drop a `TEST_LIST` from the first `foreach`):
```
1/1 Test #3135: SplitLogPaths.PrivateAndDistinct ...***Failed
SplitLogPaths FAILED: DirectGLES.Split.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary: requires exactly one nonempty MOBILEGL_LOG_FILE_PATH
```
*(b) two entries with ONE identical path each* (`~/w7/p5-j0rev-sh/perturb_b.sh`):
```
SplitLogPaths FAILED: DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels: duplicate MOBILEGL_LOG_FILE_PATH /…/split-logs/DirectGLES.Split.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary.log: ['DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels', 'DirectGLES.Split.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary']
```
The same perturbation also reds the control script's preflight
(`scripts/ci/split_negative_controls.sh:58`) with the identical named message, so a collision is
caught in both places. It is a **test-time** failure, not a configure-time one — acceptable, but see
finding **F4**: neither place is reached by a CI lane that can be shown to run.

### Check 3(a) — do the smoke cases assert their own messages, and does removing the read go red? — **PARTIAL PASS; see F1**

`bash scripts/ci/split_negative_controls.sh --self-test` → `9 passed, 0 failed` +
`private-log smoke test: 5 passed, 0 failed` (14/14, as claimed). The five new cases **do** assert
their own wording (`split_private_log_smoke.sh:20-25`: `E1 FAILED: selected private logs lack
expected Fatal` for the three E1 cases, `FAILED: red lacks its persistent-map push diagnostic` for
`e3-unrelated`) — R-16 satisfied at the case level.

j0's red-once reproduced exactly (`~/w7/p5-j0rev-sh/perturb_c.sh`, section P1 — replace
`split_negative_controls.sh:128-130` with `: "a non-zero ctest status is accepted"`):
```
negative control E1 (MOBILEGL_IPC_VERB_BARRIER=0) turned 1 selected entries red, and the red carries the scenario's own diagnostic, as it must
NOT OK missing-fatal: control must report FAILED for its own reason
P1 self-test rc=1
```
**But the same perturbation leaves the file CI runs green** — see F1.

### Check 3(b) — the E3(a) expected strings — **PASS, and the narrowing is right**

Both strings exist verbatim and are produced only by their own failing assertion:
`MobileGL/MG_IntegrationTest/Scenarios/PersistentCoherentMapScenario.cpp:414-417`
(`EXPECT_TRUE(WholeSurfaceIs(afterSecond, "red", "the SECOND write through the same mapping,
announced by nothing: …"))`) and `:442-443` (`"frame 1's write through the SAME mapping, after a
Present"`). They are `EXPECT_TRUE` on a `::testing::AssertionResult` (`:375-377` →
`RegionIsMostly`), not a `SCOPED_TRACE`, so they cannot appear because some *other* assertion in the
same test failed. `tr -s '[:space:]' ' '` normalisation still matches (single-space literals).

Dropping `cannot have pushed` is **correct**, and for a better reason than j0 gave: that assertion
(`:531-541`) is guarded by `if (SplitLane::DeclaredPersistentMapArm() == "emulated")`, and
`MGITEST_PERSISTENT_MAP_ARM` is set on the `PersistentMapArm` lane only (confirmed in the JSON env
lists). Measured selections on the real build:
`ctest -N -L integration-split -R 'DirectGLES\.Split\.(SmallRing\.)?PersistentCoherentMapScenario'`
→ **6** entries, `grep -c PersistentMapArm` → **0**. So the counting assertion could never have
fired in this selection and its presence in t1's regex was a hole. Dropping
`PersistentCoherentMapScenario\.cpp:[0-9]+: Failure` is likewise right (it was the ID-46 finding-8
shape). Could the control accept the text "for a different reason"? Only in the sense any scenario
assertion can — any defect that stops the unannounced second write produces it — and the
green-baseline gate immediately before constrains that. `PersistentMapTracker.cpp` confirmed: the
KiB→bytes conversion and the early `return` at `blockBytes == 0`; there is no Fatal to require.

### Check 3(c) — a stale file cannot satisfy the control — **PASS**

`perturb_c.sh` P2: keep the read, make `split_log_paths.py`'s `reset` a no-op. The smoke test goes
red by name and prints the false arming:
```
private-log evidence: DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels: /tmp/split-private-log.W10cPK/stale-fatal/entry.log
negative control E1 (MOBILEGL_IPC_VERB_BARRIER=0) turned 1 selected entries red, …
NOT OK stale-fatal: control must report FAILED for its own reason
P2 self-test rc=1
```
The delete-before-run at `split_negative_controls.sh:112-115` is genuinely load-bearing.

One thing j0 did not check and should have, because the whole design rests on it: the file sink
**flushes per line** — `MobileGL/MG_Util/Debug/Log.cpp:82-85` `std::fputs(msg, s_logFile);
std::fflush(s_logFile);` — so the `MGLOG_F` immediately followed by `std::abort()` does reach the
file. `InitFile()` opens with `"w"` (`:73`), i.e. truncate-per-process, which is exactly why one
file per entry was required. Verified, and it holds.

### Check 3(d) — call sites unchanged, no new CI-trigger dependency — **PASS**

`.github/workflows/test.yml:1098-1102`: `working-directory: build-split`,
`run: bash "${GITHUB_WORKSPACE}/scripts/ci/split_negative_controls.sh"`.
`~/w7/notes/tools/wsl_p5_gate.sh:179`: `( cd build-split && CONTROL_TMPDIR="$LOG-controls" bash
../scripts/ci/split_negative_controls.sh )`. Both give `$0`-relative `log_helper` the right path
(`…/scripts/ci/../../MobileGL/MG_IntegrationTest/Harness/split_log_paths.py`) and both give
`check`'s `"$PWD"` the build dir. Neither needed an edit **for path discovery**. R-14 holds: no new
step and nothing hung on the temporary `feat/disaggregated` trigger.

Executed verbatim in the gate's shape:
```
cd ~/w7/p5-j0/build-split && env -u MOBILEGL_IPC_VERB_BARRIER -u MOBILEGL_IPC_PERSISTENT_BLOCK_KB \
  GLIBC_TUNABLES=glibc.malloc.tcache_count=0 CONTROL_TMPDIR=/home/swung/w7/p5-j0rev-ctl \
  bash ../scripts/ci/split_negative_controls.sh    # rc=0
SplitLogPaths: 21 entries, 21 distinct private paths
…
split entries - passed: 0, failed: 0, skipped: 21 (ctest exit 0)
::warning::every DirectGLES.Split. entry SKIPPED, so neither negative control can fire. …
```
All 20 `split-logs/` files were rewritten by that run, none empty.

### Check 4 — regressions — **PASS**

| gate | command | result |
|---|---|---|
| split unit | `cd build-split && ctest --output-on-failure -L unit --no-tests=error -j 14` | `99% tests passed, 1 tests failed out of 1985`; the one red is `1951 - PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot (Failed) unit` — the known c1/ID-43 debt, nothing else |
| push integration-gpu | `cd build-push && MOBILEGL_TRANSPORT=monolith ctest --output-on-failure -L integration-gpu --no-tests=error -j 4` | `100% tests passed, 0 tests failed out of 1128` (1128 executed) |
| push all-label names | `ctest -N` | **2942**; vs the frozen `~/w7/p5-before-ctest-names.txt` (2902): **0 removed / 40 added**; vs `~/w7/p5-t1-names-push.txt` (2942) `comm -3` → **0 lines**, i.e. byte-for-byte the integrated t1 head |
| split all-label names | `ctest -N` | **3135**; vs frozen: **0 removed / 233 added** (the 21 Split entries, the other pre-existing disaggregated-only suites, and the one new unlabelled ownership test) |
| G1 | `git diff --name-only c3e736a3..p5/j0 \| grep -E '\.(c\|cc\|cpp\|cxx\|h\|hpp\|def)$'` | **empty** — no C++, no `.def`; G1 cannot move |

j0's §5 numbers all reproduce, including its honest correction that literal equality against the
frozen baseline is not true and was already not true in t1-v2.md §5.

### Check 5 — the integrator's verbatim command — **PASS with one omission (F11)**

Argument shape is identical to both call sites: no arguments, cwd `build-split`, `CONTROL_TMPDIR`
in the environment, script reached by a path whose `dirname` is `<repo>/scripts/ci`. The
joint-merge package can run it verbatim. The one difference from CI: `test.yml:1100` also sets
`MOBILEGL_ITEST_REQUIRE_GPU: "1"`, which turns "no GPU" from a skip into a failure
(`MG_IntegrationTest/Main.cpp:45-50`, `Harness/HeadlessGL.cpp:443`). Without it, a joint-head run on
a machine whose EGL is unhappy prints the disarmed `::warning::` and exits 0 — indistinguishable, in
that command, from "c1/v1 did not arm the lane".

---

## Findings, ranked

### F1 — MAJOR. The new R-16 smoke cases are executed by nothing. CI's R-16 step still runs the un-extended `control_smoke_test.sh`.

* `scripts/ci/testdata/split_private_log_smoke.sh` (new, 29 lines) is reachable only through
  `scripts/ci/split_negative_controls.sh:44-48` (`--self-test`).
  `grep -rn -- --self-test .github/workflows/test.yml ~/w7/notes/tools/wsl_p5_gate.sh` → **no hit for
  this script**. `.github/workflows/test.yml:2293` runs `bash scripts/ci/control_smoke_test.sh`, and
  `scripts/ci/control_smoke_test.sh` is **not in the diff** — its five split cases
  (`:67-75`) are unchanged and its `expect()` (`:34-48`) compares only the exit code, never a
  message.
* **Claim it contradicts:** j0-v1 §1 "No workflow or local gate edits were needed" and §4's framing
  of the five cases as the package's R-16 evidence.
* **Failure scenario (executed, `perturb_c.sh` P3):** delete `split_negative_controls.sh:128-130`
  (the private-file read) so E1 accepts a non-zero ctest status again — the ID-46 finding-8 defect,
  reintroduced. `bash scripts/ci/control_smoke_test.sh` → `smoke test: 9 passed, 0 failed` /
  `CONTROL_SMOKE_TEST_OK` / **rc=0**. CI is green. Only `--self-test`, which no gate invokes, goes
  red.
* **Confirm:** `bash ~/w7/p5-j0rev-sh/perturb_c.sh`, section P3.
* **Fix:** add the five cases to `control_smoke_test.sh` (preferred — then `test.yml:2293` runs them
  with no workflow edit), or change `test.yml:2293` to
  `bash scripts/ci/split_negative_controls.sh --self-test`.

### F2 — MAJOR. j0 breaks t1's mechanised red-once, `scripts/ci/redcheck_control_smoke_test.sh`, and it now reports the opposite of the truth.

* `redcheck_control_smoke_test.sh:33-44` finds the evidence check by
  `needle in line and line.lstrip().startswith('if ! ')`. j0 turned that line into
  `split_negative_controls.sh:131` `elif ! tr -s '[:space:]' ' ' < "${out}" | grep -qE
  "${evidence}"; then`, and moved E1's evidence into a `python3` call at `:128-130` that the
  perturbation does not know about.
* **Executed:** `bash scripts/ci/redcheck_control_smoke_test.sh` on `p5/j0` →
  `expected exactly one evidence check in /home/swung/w7/p5-j0/scripts/ci/split_negative_controls.sh, found 0`
  … `RED-CHECK FAILED: the controls accept an unrelated failure again and the smoke test still passed.`
  **rc=1.** Second-order: the redcheck's shell never checks the heredoc python's exit status
  (`:30-47`), so the perturbation silently no-ops and the script blames the control instead of
  itself.
* **Severity:** it is not in any workflow (`grep -rn redcheck_control_smoke_test .github/` → none),
  so it will not red a CI run — but it is the artifact that makes t1-v2.md §2's "I made it red once"
  re-checkable, it is now permanently broken on every branch, and a reviewer who runs it will be
  told the controls are defective when they are not.
* **Fix:** accept `elif ! ` as an anchor, and perturb E1's python evidence branch too; and add
  `|| exit 1` after the heredoc.

### F3 — MAJOR. The private logs this whole package exists to produce are not uploaded by CI.

* `.github/workflows/test.yml:1104-1110` — `name: integration-split-logs`,
  `path: build-split/MobileGL/MG_IntegrationTest/*.log*`. The 20 new files live one directory
  deeper, in `…/MG_IntegrationTest/split-logs/`, and `*` does not cross `/` in
  `actions/upload-artifact` globs. Only the `PersistentMapArm` file is still collected.
* **Failure scenario:** E1 goes red on the joint head; the `Fatal{BarrierViolation, "<slot>"}` file
  the control read, and the 13 sibling files that did *not* carry it, are absent from the artifact.
  ID-53's stated purpose — "E1's negative control needs a diagnostic it can read" — is only half
  delivered: the control can read it, the human triaging the red cannot.
* **Claim it contradicts:** j0-v1 §1, again.
* **Fix:** one line —
  `path: |` / `build-split/MobileGL/MG_IntegrationTest/*.log*` /
  `build-split/MobileGL/MG_IntegrationTest/split-logs/*.log`.

### F4 — MAJOR (borderline minor). `SplitLogPaths.PrivateAndDistinct` runs in no CI lane, and deleting its one CI call site is undetectable.

* `MobileGL/MG_IntegrationTest/CMakeLists.txt:2005-2008` registers it with **no `LABELS`**. The
  build-split job runs `ctest -L unit` (`test.yml:965`), `ctest -L integration-split` (`:982`) and
  `ctest -L integration-gpu` twice; no step runs a bare `ctest`. An unlabelled test is selected by
  none of them.
* The only CI execution of the check is the preflight at `split_negative_controls.sh:58`. **Executed
  (`perturb_c.sh` P4):** delete that line entirely → `--self-test` still `9 passed, 0 failed` +
  `5 passed, 0 failed`, **rc=0**. Nothing anywhere notices that the ownership check stopped running.
* j0-v1 §1 presents the label-lessness as a virtue ("preserving their required counts") without
  noting the consequence.
* **Fix:** either add a `ctest -R SplitLogPaths.PrivateAndDistinct` step to the build-split job, or
  add a smoke case that removes `:58` and requires the control to fail.

### F5 — MINOR. `split_negative_controls.sh:139` names a function that does not exist.

`# E1: c1 ClientSession::Post emits MGLOG_F Fatal{BarrierViolation, "<slot>"}.` There is no
`ClientSession::Post` on `p5/c1`; the sole site is `ClientSession::EmitAndWait`
(`p5/c1:MobileGL/MG_Remote/Client/ClientSession.cpp:686-698`, the check at `:692`, the `MGLOG_F` at
`:693`), which j0's own report §3 cites correctly. The comment is the only in-repo statement of
where the evidence comes from; it should point at the real function. (The good news, which j0 also
did not state: `EmitAndWait` is the *only* emission entry point on `ClientSession`, so the barrier
check sits on every routed record, not just the reply-bearing ones.)

### F6 — RISK, to be recorded rather than fixed. E1's evidence is now a race observation, and every fallback was removed.

The Fatal fires only when `ApplyThreadIsInsideApplier()` happens to be true at an emit
(`ClientSession.cpp:692`). With `MOBILEGL_IPC_VERB_BARRIER=0` that is timing-dependent. j0 replaced
t1's whole evidence alternation — `ClearThenReadPixelsScenario\.cpp:(290|295)`, the two band
sentences, `TriangleScenario\.cpp:[0-9]+: Failure` — with the single file-only Fatal. On the one
barrier-0 run anyone has measured (t1-v2.md §3.2) **exactly one** of the 14 selected entries failed
with a readable assertion and the rest aborted silently. If no selected entry wins the race on the
joint head, E1 now fails by name where t1's version would have passed on the SmallRing assertion.
This follows ID-53 ruling (1) literally, so it is not a violation — but "E1 has evidence" is still
unproven, exactly as j0's §6 says, and the joint-head execution is the only thing that settles it.
Nothing in the mechanism itself is unsound: I confirmed the sink flushes before the abort
(`Log.cpp:82-85`), which was the one way this could have been dead on arrival.

### F7 — MINOR. The positive smoke case does not pin the line the integrator is told to look for.

`split_private_log_smoke.sh:14-17` asserts, for `mode=evidence`, only `rc == 0` and the **E3(a)**
success sentence. It never greps `private-log evidence: <entry>: <path>` — the line j0-v1 §6 tells
the integration owner is the proof that E1 read a file rather than a status. A regression that
stopped printing it would be invisible.

### F8 — MINOR. Two small duplications, one of them dead.

`Harness/SplitLogPaths.cmake.in:8` re-lists `ClearThenReadPixelsScenario TriangleScenario
PersistentCoherentMapScenario`, which `MG_IntegrationTest/CMakeLists.txt:1987-1988` already has; a
fourth SmallRing scenario added there and not here would be caught, loudly, by the ownership check —
so this is drift-safe but avoidable. `TEST_LIST MGL_SPLIT_ARM_TESTS`
(`MG_IntegrationTest/CMakeLists.txt:1953`) is set by the diff and read by nothing.

### F9 — MINOR. A new hard configure dependency.

`MG_IntegrationTest/CMakeLists.txt:2004` `find_package(Python3 REQUIRED COMPONENTS Interpreter)`
makes a Python interpreter mandatory for every `MOBILEGL_BUILD_DISAGGREGATED` configure. The
in-tree precedent for a test-only Python use is `MG_Test/Purity/CMakeLists.txt:7`,
`find_package(Python3 COMPONENTS Interpreter)` — without `REQUIRED`.

### F10 — MINOR. A workflow comment that j0's change made false.

`.github/workflows/test.yml:1046-1047`: "…the `DirectGLES.Split.PersistentMapArm.` entry is the one
Split entry with a `MOBILEGL_LOG_FILE_PATH` of its own (nothing else writes it, so the grep means
what it says)". After this commit all 21 have one. The step still works — it names the arm file
explicitly — but the sentence that justifies it is now wrong, in the file j0 says needed no edit.

### F11 — MINOR. The joint-head command omits `MOBILEGL_ITEST_REQUIRE_GPU=1`.

See check 5. Recommend the integrator run
`… env -u MOBILEGL_IPC_VERB_BARRIER -u MOBILEGL_IPC_PERSISTENT_BLOCK_KB MOBILEGL_ITEST_REQUIRE_GPU=1
GLIBC_TUNABLES=… CONTROL_TMPDIR=… bash ../scripts/ci/split_negative_controls.sh` so that a GPU-less
skip cannot be mistaken for an unarmed lane.

---

## Claims in `j0-v1.md` the code does not support

1. **§1, last paragraph — "No workflow or local gate edits were needed: both existing call sites
   discover paths automatically."** True of path discovery, and false as a statement about the
   package. Three workflow-side edits were needed and are missing: the R-16 step does not run the
   new smoke cases (**F1**), the artifact upload does not collect the new logs (**F3**), and
   `test.yml:1046-1047` now says something untrue (**F10**). A fourth in-repo edit was needed:
   `redcheck_control_smoke_test.sh` (**F2**).
2. **§4 — the five private-log cases presented as this package's R-16 evidence.** They pass and they
   assert their own messages, but they are a self-test no gate runs (**F1**); R-16's "每条门在报告里
   都要带一行『我把它弄红过一次』" is met for the author's own run and not for the repository.
3. **§1 — "`SplitLogPaths.PrivateAndDistinct` is an additional split-build-only test (not labeled
   unit or integration-split, preserving their required counts)."** Accurate, and incomplete: it is
   therefore in no CI lane at all (**F4**).
4. **`split_negative_controls.sh:139` — "c1 `ClientSession::Post`".** No such function (**F5**).

Everything else I checked reproduced exactly: the 21/21 tables, the byte-level log content, the
14/14 self-test, the red-once by removing the file read, both ownership red-onces, 1985 with the one
c1 red, 1128/1128, 2942 push names identical to t1's, 0 removed against the frozen baseline, no C++.

---

## Verdict

**Needs a fix — not a re-round.** The six files j0 changed are, as far as I could break them,
correct: the paths are private and distinct and provably so, the lane environments survive, the
counting lane is intact, the delete-before-run is load-bearing, the E3(a) strings are the ones the
source emits and the narrowing is an improvement on t1's regex, and the regression numbers hold.

Merge after exactly these, none of which touches the reviewed logic:

1. **F1** — make the file CI runs (`scripts/ci/control_smoke_test.sh`) cover the five private-log
   cases, or point `test.yml:2293` at `--self-test`. *(blocking: R-16)*
2. **F2** — repair `scripts/ci/redcheck_control_smoke_test.sh` for the new `elif`/python evidence
   shape, and make it check its own perturbation's exit status. *(blocking: it is red today)*
3. **F3** — add `build-split/MobileGL/MG_IntegrationTest/split-logs/*.log` to the
   `integration-split-logs` artifact path. *(blocking for the joint-head execution ID-53 requires)*

Non-blocking, take with the joint-merge package: F4 (give the ownership check a lane it runs in),
F5/F10 (two wrong comments), F7, F8, F9, F11. F6 is a debt to record, not a fix: ID-53 already says
E1's evidence "derived from source and then EXECUTED on the joint head before it counts", and j0
correctly refuses to claim it here.

---

**Artifacts kept** (everything else `~/w7/p5-j0rev-*` deleted): `~/w7/p5-j0rev-unit.log`,
`p5-j0rev-igpu.log`, `p5-j0rev-controls.log`, `p5-j0rev-names-{push,split,push-igpu}.txt`,
`p5-j0rev-analyze.py`, and the reviewer's perturbation scripts `~/w7/p5-j0rev-sh/{perturb_b,perturb_c,regress,realrun,inspect,inspect2,lines}.sh`
— `perturb_b.sh` and `perturb_c.sh` are the re-runnable form of every red-once claimed above. The
worktree `~/w7/p5-j0` is clean and its submodule links are off.
