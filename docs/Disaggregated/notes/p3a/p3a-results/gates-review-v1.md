# P3a package D (`gates`) — adversarial review v1

Reviewed `1ef91dec` / `588d25e9` / `80f6bf1b` on `p3a/gates` (worktree `/home/swung/w7/p3a-gates`, parent
`39722687`), against `BRIEF-P3A.md` A/B/C.3/D.2/D.3/E, `INTEGRATOR-DECISIONS.md` ID-1/ID-2/ID-8 **and the
newly added ID-9**, `contract-v1.md` + `contract-review-v1.md` (item 11), `wire-v1.md`, `espryt-v1.md`, and
the package's own `gates-v1.md` + `handlerecycle-before.log`.

Nothing in `p3a-gates` was modified. A private worktree `/home/swung/w7/p3a-review-gates` was created at
`80f6bf1b`, used to run both scripts, and removed (`git worktree list` clean; `p3a-gates` `git status` empty).

## Verdict: **REWORK** — four majors, all mechanical (three edits and one integrator decision)

The package is well built and the two scripts are genuinely honest instruments: I ran them and they answer
correctly on every path I could reach, including against the real P3a-rewritten `Managers.cpp`. Nothing here
passes on a broken tree and no test name was removed or renamed. What blocks acceptance is narrower: an
explicitly assigned open review item is two-thirds undone while the report says it is done (M1), eight new
CI entries whose whole purpose is the P3a A/B are never selected by any push-build CI run (M2), a script
that breaks its own written exit contract on the one path that matters (M3), and a G5 set that omits the
function the brief's risk table names as G5-protected (M4).

---

## 1. Majors

### M1 — contract-review item 11 is one-third addressed; the four CSO stats lanes still pin `0x7f`, and the report claims the item closed

**Evidence.** `contract-review-v1.md:344-347` reads: *"The `.Handles` **and stats** itest lanes still pin
`MOBILEGL_PIPE_PUSH=0x7f` (`MG_IntegrationTest/CMakeLists.txt:863, 965, 975`) … Package D owns that file:
G12's A/B needs an explicit `0x1ff` arm (or the pins re-read as the control they now are)."*

At the contract commit those three line numbers are:
- `:863` → `MGL_ITEST_HANDLES_ARM_KNOBS` — **fixed**, now `0x1ff` at `MG_IntegrationTest/CMakeLists.txt:930`.
- `:965` → `MGL_ITEST_GLES_CSO_ON_ENVIRONMENT` — **unchanged**, still `0x7f` at HEAD `:1032`.
- `:975` → `MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT` — **unchanged**, still `0x7f` at HEAD `:1042`.
- and their Off twins at `:1037`, `:1047` still `0x800000000000007f`.

`gates-v1.md` §1 and D4 describe the change as the `MGL_ITEST_HANDLES_ARM_KNOBS` pin only and never mention
the CSO lanes; the report presents item 11 as handled.

**Failure scenario.** After P3a lands, `DirectGLES/DirectVulkan.CsoContentAddressing.{On,Off}` — the P2
control that proves CSO content addressing is real — runs with bits 7 and 8 **cleared**, i.e. on a
configuration nothing ships. It keeps passing. This is verbatim the objection the package itself writes at
`CMakeLists.txt:934-941`: *"Pinning it at 0x7f after P3a would leave the Handles arm asserting the P2 shape
while the … handles it is supposed to be about stayed switched off — a lane that still passes and no longer
measures the key that ships."* The rule was applied to one lane family and not to the two the review named.

**Refutation attempted.** Could the CSO lanes need `0x7f` deliberately? The counters they read (`csom`,
`csob`) are render-state, untouched by bits 7|8, so raising the mask is behaviour-preserving for what they
assert — and D's own stated principle ("the mask is the PHASE default, not a hand-picked bit") points the
other way. I found no comment anywhere in the file or the report defending `0x7f` for these lanes. If there
is a reason, it is unwritten, which is itself the finding.

**Fix.** `0x7f`→`0x1ff` at `:1032, 1042` and `0x800000000000007f`→`0x80000000000001ff` at `:1037, 1047`,
re-run the four CSO lanes; **or** write the decision down in the file and in `gates-v1.md` §D4 and say item
11 is closed by argument rather than by change.

---

### M2 — the `ResourceSubsystemOn.` / `ResourceSubsystemOff.` lanes (8 entries) never execute in a push build in CI, so the G12/D-A3 A/B cannot go red where it was installed

**Evidence.** `.github/workflows/test.yml:600-602` (as edited by this package):

```
ctest --output-on-failure -L integration-gpu \
  -R 'HandleRecycle|CsoContentAddressing|ResourceSubsystemControl|MapPersistentRoundtrips' \
  --no-tests=error -j 4
```

None of the four alternatives matches the new lanes:
`DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.SubDataAfterAnInFlightDrawReachesTheNextDraw`
does not contain `ResourceSubsystemControl`, and
`…ResourceSubsystemOn.LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip` does not
contain `MapPersistentRoundtrip**s**` (the case name is singular). The only place the full label runs is the
**pull** `build-linux` job (`test.yml:293-298`, `ctest -L integration-gpu`), where `MOBILEGL_PIPE_PUSH`
steers nothing at all (`Config.h` declares the field inside the push guard) — so both arms are the same
legacy path there. `integration-verify` is the only job that unpacks a push build (`test.yml:573`), and it
runs only the filtered step above.

Net: the eight entries D added — the four `ResourceSubsystemOn.` and four `ResourceSubsystemOff.`
`LargeArenaAdoptionScenario` entries, whose stated purpose (`CMakeLists.txt:1176-1182`) is *"if the handle
path and the legacy BufferBackendOps path disagree about any of it, one of these two lanes goes red and
names which"* — are, in CI, either a duplicate legacy run or not run.

**Failure scenario.** Espryt's handle arm and its legacy `BufferBackendOps` arm disagree about an adopted
store's in-flight SubData / readback / GPU-write; the lane that exists to name that disagreement never runs
with the bits set; CI is green; the divergence ships and is found on device.

**Refutation attempted.** `gates-v1.md` §8 item 5 does say the label-wide A/B is the integrator's. That is
true of `MOBILEGL_PIPE_PUSH=0x7f ctest -L integration-gpu`, but D also chose to add these eight entries to
CI's push build and then did not select them there; ROADMAP.md:7 asks a gate to be able to go red where it
is installed. The pull job does run the entries — but with the mask inert, which is the "green that asserts
nothing" the package's own comments reject elsewhere.

**Fix.** One string: `-R 'HandleRecycle|CsoContentAddressing|ResourceSubsystem|MapPersistentRoundtrip'`
(drop `Control`, drop the plural). Confirm afterwards that all 16 new push-build entries appear in the
step's `ctest` listing.

---

### M3 — `p3a_vertex_input_negative_control.sh` breaks its documented exit-1 contract: the "did not trip" path leaves the build directory holding the library built from the patched header

**Evidence.** Header contract, `scripts/p3a_vertex_input_negative_control.sh:40-44`: *"1 the control did
not answer: either the suite stayed green with the field dropped, or it went red without ever naming
IsBgra … **and both leave the tree restored AND rebuilt**"*. The second of those two branches honours it
(`:186-197` restore → rebuild → re-ctest → `exit 1` at `:204`). The first does not:

```
161  if ctest --test-dir "$BUILD_DIR" -R "$TEST_NAME" … ; then
163    say "NEGATIVE CONTROL DID NOT TRIP: …"
166    cp -f "$LOG_DIR/ctest-after.log" ./p3a-vertex-input-control-failure.log
168    exit 1
169  fi
```

`exit 1` runs only the `trap 'restore' EXIT` installed at `:126`, which copies the header back and does
nothing else. The script's own comment at `:171-174` states the rule it is violating: *"leaving a build
directory holding the broken header is worse than any exit status."*

Separately, both exit-1 branches `cp` a log into `$REPO_ROOT` (`:166`, `:200`). `.gitignore` carries no rule
matching `p3a-*.log`, so a run leaves untracked files in the working tree.

**Failure scenario.** G6 has stopped checking (exactly the condition this control exists to detect). An
engineer sees "NEGATIVE CONTROL DID NOT TRIP", opens the suite, and re-runs
`ctest --test-dir build-push` — against a `libMobileGL.so` in which `IsBgra` is hard-zeroed. Every
downstream reading from that build directory until someone happens to rebuild is taken from a deliberately
corrupted library.

**Refutation attempted.** `restore()` uses `cp -f`, which refreshes the header's mtime, so the *next*
`cmake --build` does repair it. But nothing in the script, its message, or its exit code tells the caller to
build, and `ctest` alone will not. The sibling branch proves the author considered the requirement; this
branch simply misses it.

**Fix.** Hoist the restore → rebuild → re-ctest block (`:186-197`) above the verdict so all three outcomes
share it, and keep the failure logs in `$LOG_DIR` (or a gitignored path) rather than the repo root.

---

### M4 — G5's set omits `FlushPendingRangesNow`, which the brief twice calls G5-protected and names as a named risk's only mitigation

**Evidence.** `scripts/p3a_untouched_regions.sh:58` implements D-F's nine-function table verbatim. But
`BRIEF-P3A.md:420` says *"`FlushPendingRangesNow`'s three-tier drain (`:1004-1072`) is a G5-protected
function"*, and E's risk row at `BRIEF-P3A.md:1708` makes it the whole mitigation: *"`FlushPendingRangesNow`'s
three-tier drain silently changes tier … → **The function is in G5's byte-identical set** and its kill switch
… gets its own integration pass in D.3."* It is not in the set. `Managers.cpp:1004` has exactly one
definition (calls at `:1401`, `:1710`), so the extractor would take it as a tenth entry unchanged.

**Failure scenario.** During the P3a rewrite the tier-1 threshold or the whole-buffer test in
`FlushPendingRangesNow` drifts and drops the hot path into tier 3. No gate sees it — G5 does not cover the
function, and the brief's risk table records the risk as mitigated by a gate that does not exist. The
symptom is the Mali WAR stall and MC 26.3's p99, i.e. exactly `ROADMAP.md:19`'s headline number, and under
ID-6 performance is recorded rather than gated, so nothing else stops it either.

**Refutation attempted.** D-F's authoritative "**Decision**: nine functions" list omits it, and package D
implemented that list exactly — so this is first a contradiction inside the brief. But D owns G5, the
contradiction is in the same document D was implementing from, and `gates-v1.md` does not note it. Note also
that P3a may legitimately need to edit the function (D-C reshapes `resource_flush_range`), in which case
adding it would force a red — which is precisely why this needs a written decision rather than silence.

**Fix (integrator decision).** Either add `FlushPendingRangesNow` to `FUNCTIONS` (ten bodies; re-capture the
D.0 baseline) and update the script's header table, or record in `gates-v1.md`/D.5 that E's risk row's
mitigation is withdrawn and name what replaces it.

---

## 2. Minors

- **m1 — the kept D.2 artefact does not contain the verdict lines the report quotes.**
  `handlerecycle-before.log` is 153 lines of `--output-on-failure` (the brief's D.2 command) and contains
  **zero** `arm=` lines: gtest's `std::cout` verdict from `HandleRecycleScenario.cpp:288-291` surfaces only
  for failing tests. `gates-v1.md` §5 prints a block of `[ HandleRecycle ] arm=AbaControl expected=STALE
  observed=STALE …` lines under *"That is the artefact ROADMAP.md's 重键前红 asks for"*, annotated
  "(`ctest -V`)", and that `-V` log was not kept. The artefact that goes into `MEASUREMENTS.md` therefore
  proves the AbaControl arm only by its "Passed" — the inference-from-exit-code the design says it wants to
  avoid. Fix: keep the `ctest -V` log beside it and cite it in §5.

- **m2 — `gates-v1.md` D6 says "**Both** counting workloads were rewritten to define a SECOND arena".**
  `StorageBufferRegrowScenario.cpp:266-272` re-specifies SetUp's `m_buffer` three times (it is an SSBO with
  no VAO attributes, so d7655247's crash does not apply). Only `LargeArenaAdoptionScenario.cpp:150-158` and
  `ResourceSubsystemControlScenario.cpp:222-225` use a second arena. Report text only.

- **m3 — the D6 workaround is now stale, and it costs the coverage ID-9 asks for.**
  ID-9 (added since `gates-v1.md` was written) merged `dev@9eae9858` — `d7655247` included — into
  `feat/disaggregated` as `5cb826b0`, and requires the fix to be present in **both** the legacy and the
  handle arm of the respecify/retire path. After gates rebases, the two workloads that route around the
  crash stop exercising respecify-of-an-adopted-store-under-a-live-VAO, which is now the path that most
  needs arm-parity coverage. I verified the merge does not disturb G5:
  `p3a_untouched_regions.sh 44c2b5cf 5cb826b0` → **rc 0** (d7655247's edit is at `Managers.cpp:1081-1084`,
  inside `Ops_Respecify`, outside the nine). Fix after the rebase: restore a re-specify window in at least
  one of the two counting cases; `d7655247`'s own four `Respecified*Arena*` cases arrive by union anyway.

- **m4 — the report's G14 numbers are against the superseded baseline.** Measured now:
  `build-linux`/`build-push` = 2521; `~/w7/p3a-before-ctest-names.txt` = 2502 (re-captured at `5cb826b0`).
  D's own contribution is exactly the claimed **+34** and every one is a new name — no rename, no removal.
  The 15 names that `comm -23` now reports as "removed" are the un-rebased dev additions
  (`{DirectGLES,DirectVulkan}.LargeArenaAdoptionScenario.Respecified{Vertex,Index}Arena*` and 11
  `PersistentBufferOrderingProbeTest.*`). G14 must be re-measured after the rebase, and the union must keep
  all 15.

- **m5 — `p3a_untouched_regions.sh`'s masker mishandles an odd number of `'` in code.** A C++ digit
  separator (`16'777'216`) opens a char literal at `:115` and blanks to the next `'`, which can hide a brace
  and shift a body's extent silently. There is none in `Managers.cpp` today (verified: no `[0-9]'[0-9]`;
  every one of its 272 apostrophes is inside a comment or a balanced literal, and comments/strings are
  masked first), and the extractor survives the real P3a rewrite (below). Cheap fix: skip `'` when the
  preceding character is a digit.

- **m6 — `perturb()` locates the body brace in the ORIGINAL text** (`:192`, `text.index('{', begin)`) rather
  than the masked text, so a brace in a comment or a default argument on the signature line would perturb
  the wrong place. `SELF_TEST_FUNCTION=ClearBufferPool` is chosen to avoid this and it works; the failure
  mode would be a loud self-test failure, not a silent pass.

- **m7 — the hash starts at the line containing `name(`** (`:164`), so a return type on its own preceding
  line falls outside the hashed body. None of the nine is written that way today.

- **m8 — C.3's `cmake --build build-bench && ctest -L benchmark` was not run** (D11: no `build-bench` in the
  worktree). No benchmark source changed, but it is a brief-mandated verification step; the integrator
  should run it once on the finished tree.

- **m9 — commit `588d25e9`'s message says nothing about the `RESOURCE_LOCK` fix (D5)** it carries for four
  pre-existing lanes. A future bisect for that flake will not find the fix by message. The brief prescribed
  the three messages verbatim, so this is a note rather than a rule break.

- **m10 — the buffer ABA has no positive control on the backend P3a actually re-keys.** The `.AbaControl`
  arm is DirectVulkan-only (the knob has no DirectGLES consumer), so on Espryt — the buffer path P3a lands —
  the corruption is never demonstrated reproducible; only the `.Handles` arm asserting FRESH stands between
  a broken re-key and green. The gate can still go red for its reason (a broken re-key reads STALE), and
  `HandleRecycleScenario.cpp:319-321` names exactly where a `Features.PipeHandleAbaControl` consumer over
  the resource slot table would go. Worth carrying into package C's scope rather than losing.

---

## 3. What I checked and found correct

Recorded because several of these were the likeliest places for this package to be wrong.

- **The G8 "red before" is genuine, and the report's D.2 claims match the log exactly.**
  `ExpectPixelsFor` (`HandleRecycleScenario.cpp:287-299`) sets `expectsStale` only on `Arm::AbaControl` and
  then asserts the whole viewport is the **stale** colour, so a tree on which the corruption stopped
  reproducing turns the control **red**, not green. In `handlerecycle-before.log`: `.Legacy` Passed on both
  backends (#2479, #2485), `.AbaControl` and `.AbaControlHandles` Passed (#2491, #2497), `.Handles` Skipped
  (#2467 DirectGLES, #2473 DirectVulkan) — exactly as `gates-v1.md` §2 states.
- **The two-buffer window is a real D-H probe.** Shared POSITION buffer, differing COLOUR buffer, elements
  blob byte-identical by construction, so an inherited `set_vertex_buffers` shows as own-geometry-in-the-
  dead-VAO's-colour. It cannot distinguish *which* binding was inherited, and does not claim to. Correctly
  gated on the arm marker only (P2's vertex-input key), **not** on the new resource marker. D1's split into
  its own case is justified by a measurement recorded at the site and adds a name without removing one.
- **`mgl_itest_join_environment` usage is right on all six new lanes** — it is a `function` with
  `PARENT_SCOPE` joining on `\\;` (`CMakeLists.txt:312-322`), and every new lane appends
  `${MGL_ITEST_CAPABILITY_ENV} ${MGL_ITEST_COMMON_ENV}` (or `${MGL_ITEST_VULKAN_ENV}`), so the EGL/ICD
  pinning survives the property's REPLACE semantics.
- **`mgl_itest_probe_for_symbol` clears its output variable** (`:396`, `set(mglItestProbeHit "")`), so the
  new `foreach(DirectGLES DirectVulkan)` at `:461` cannot leak the first iteration's hit into the second.
  Both new probes answer correctly here (no `MGPipeResourceOps` under `MG_Backend/`, no
  `MapPersistentRoundtrips` under `MG_Impl/Pipe/`) **and will fire on the real trees**:
  `p3a-espryt/MobileGL/MG_Backend/DirectGLES/Managers.cpp:2177` declares
  `const MG_Pipe::MGPipeResourceOps g_glesResourceOps`, and
  `p3a-client/MobileGL/MG_Impl/Pipe/PipeFill.cpp` names `MapPersistentRoundtrips`.
- **No `PipeStatsWindow` timing flake.** `Log.cpp:84` `fflush`es every line, and
  `MOBILEGL_PIPE_STATS_PERIOD` is frames-per-line with a floor of 1 (`ConfigLoader.cpp:279`), so the summary
  line is on disk before the assertion reads it. `Last()` takes the last `MGPipe stats:` marker, so trailing
  log traffic is harmless.
- **`mpr=` cannot be "absent" on a push build**, so the Off arm's `mpr == 0` is a reading and not a
  missing-field failure: `PipeStats.cpp:431` appends `" mpr=" + to_string(...)` unconditionally inside the
  `#if MOBILEGL_PIPE_PUSH` `cso[` bracket, and `CounterOrAbsent`'s `" "` prefix matches it. This was my main
  suspected false-red on G12 and it is refuted.
- **Private-log discipline is complete.** Every new log-reading entry owns a `MOBILEGL_LOG_FILE_PATH`
  nothing else writes and selects exactly one case (`ResourceSubsystemControlScenario.cpp` has exactly one
  `TEST_F`; the two `MapPersistentRoundtrips.` registrations each name one case). The `ResourceSubsystemOn/
  Off.` lanes configure no log and the counting case skips saying so. `MGL_ITEST_COMMON_ENV` (`:279-287`)
  carries no log path. I enumerated all 20 log-owning registrations: the four D5 locked
  (`UnlocatedIoBlocks`, `PrimGenReroute`, both `PointSizeDemotion`) are exactly the ones that give a whole
  scenario one log **and** read it back; the CSO, verify-arming, verify-corrupt and poison lanes each filter
  a single case, and the `Verify.` lanes have no reader. **D5 is correct and complete.**
- **G12's direction is sound in both dead-switch modes**: bits stuck ON → the Off arm reads 2 and goes red;
  bits stuck OFF → the On arm reads 0 and goes red; and both arms additionally assert the same green
  viewport, so a switch that changed what is drawn also fails.
- **G10's numbers are far enough apart to be unambiguous**: 3 definitions vs 12 dispatches, 1 adoption vs 5
  draws, with 0 separately named in both messages.
- **The G7 script's hard-coded assumptions hold against package B's real header.**
  `p3a-client/MobileGL/MG_Impl/Pipe/VertexInputEmit.h:88` is `wire.IsBgra = attrib.IsBgra ? 1 : 0;`; the
  script's regex matches it **exactly once** (dry-run confirmed) and rewrites it to
  `wire.IsBgra = 0 /* … */;`; `VertexInputEmitTest` is registered at `MG_Test/Pipe/CMakeLists.txt:147,171`.
  Missing header → 2, header without the field → 2, bad build dir → 2 (I ran the last one).
- **Exit codes verified by running** (in the private worktree): `--self-test` → 0 with all three controls
  printing; `44c2b5cf HEAD` → 0; single-ref → 0 with 9 lines; nonexistent ref → 2; three arguments → 2.
  Worktree `git status` clean afterwards — **neither script leaves the tree modified**.
- **G5 survives the real P3a rewrite.** Run against `p3a-espryt@203120fa` — which rewrites `Managers.cpp`
  for the whole handle conversion and adds the resource op table at `:2177` — the extractor finds all nine
  and reports **byte-identical vs `44c2b5cf`**. This is the strongest available evidence that the
  brace-matching extractor neither false-reds nor fails to extract after the file is rewritten. Extraction
  is also correct in the tricky cases: `RingAllocate`'s forward declaration (`:958`) and its call sites are
  rejected on the `)`-followed-by-`{` test, and `RingAllocateSlow` / `UboRingAllocate` /
  `UnpackRingAllocate` are excluded by the `\b` anchor.
- **G5 will not false-red after the ID-9 merge**: `44c2b5cf → 5cb826b0` → rc 0.
- **test.yml.** `fetch-depth: 0` and job-level `BASELINE: "44c2b5cf"` are both required and correctly scoped
  (and correctly *not* the workflow's `baseline_sha` symbol input). The workflow has no `pull_request`
  trigger, so the two G5 steps' `if:` runs them on every `feat/disaggregated` push and skips them on `dev` —
  as intended. `include-graph-check` untouched, the TEMPORARY trigger lines untouched, no other job changed
  (the whole 61-line test.yml diff is confined to the two regions).
- **Commit hygiene.** Three commits, three single-line messages verbatim from C.3, no bodies, no trailers;
  files strictly inside C.5's package-D set; `wsl_p3a_gate.sh` / `p3a_ab.sh` correctly not written (ID-3
  supersedes C.3's table). Both scripts are mode 644, matching `scripts/g7_negative_control.sh`.

---

## 4. Judgement on the deviations and the two carried findings

| # | judgement |
|---|---|
| D1 (new case, not a second window) | **Accept.** The reason is measured, recorded at the site, and adds a name rather than removing one (G14). The alternative genuinely does not work: one ABA memo entry per process carries the resolved layout as well as the bindings. |
| D2 (a new resource marker, not P2's) | **Accept, and it is the right call.** Reading `MGITEST_HANDLE_REKEY_<backend>` would have armed the buffer arm on a tree where no buffer is keyed on a handle. Verified the new probe is absent here and present in `p3a-espryt`. |
| D3 (AbaControl expectation follows the measurement) | **Accept.** The reasoning is at the assertion, and the log backs it (#2491, #2497 Passed). |
| D4 (`0x7f`→`0x1ff`) | **Accept as far as it goes — see M1.** The Handles pin is right; the two CSO pins the same review item named were not touched and the report does not say so. |
| D5 (`RESOURCE_LOCK` on four flaky lanes) | **Accept — this is a good fix.** The diagnosis (whole-scenario log + `fopen(path,"w")` + `ctest -j`) matches the file's own documented rule, `RESOURCE_LOCK` avoids the rename G14 forbids, the four locked lanes are exactly the ones with that shape (I enumerated all 20 log-owning registrations), and three consecutive 950/950 runs is adequate evidence. Only m9 (message) against it. |
| D6 (`dev@d7655247` routed around) | **Superseded, and now a small liability.** ID-9 merged the fix as `5cb826b0` before gates rebases, so item 8 of §8 is already discharged. The workaround is not wrong (same number of storage definitions), but it now removes the only coverage of respecify-under-a-live-VAO at exactly the moment ID-9 requires that path to work in **both** arms. See m3. |
| D7 (branch-scoped G5 rows, `BASELINE`, `fetch-depth: 0`) | **Accept.** Correct, necessary, and correctly distinguished from `baseline_sha`. |
| D8 (single-ref listing mode) | **Accept.** Makes D.0's capture a plain redirect; verified rc 0 / 9 lines. |
| D9 (new `PipeStatsWindow.h`) | **Accept.** Shared reader, `CsoContentAddressingScenario`'s copy left alone — right call for a package with no other reason to touch that file. |
| D10 (mode 644) | **Accept.** Matches the nearest precedent. |
| D11 (`build-bench` not built) | **Accept with m8.** No benchmark source changed; the integrator should run it once. |

**The two "findings the integrator must carry":**
- *the d7655247 SIGSEGV* — real, correctly diagnosed, correctly not fixed in flight per ROADMAP.md:88, and
  **already resolved** by ID-9's merge. What must now be carried is the inverse: undo the detour so the path
  is covered again (m3).
- *the RESOURCE_LOCK fix for four flaky pinned lanes* — real, correctly fixed here rather than carried, and
  verified as the only lanes with that shape. Nothing left for the integrator except to notice it is in
  `588d25e9` (m9).

---

## 5. Rework list (exact)

1. **M1** — `MG_IntegrationTest/CMakeLists.txt:1032, 1042`: `0x7f` → `0x1ff`; `:1037, 1047`:
   `0x800000000000007f` → `0x80000000000001ff`. Re-run the four `CsoContentAddressing.{On,Off}` lanes.
   **Or** write the reason they stay at the P2 mask into the file and into `gates-v1.md` D4, and say item 11
   is closed by argument. Either way, `gates-v1.md` must stop implying item 11 was a one-line change.
2. **M2** — `.github/workflows/test.yml:601`: `-R 'HandleRecycle|CsoContentAddressing|ResourceSubsystem|MapPersistentRoundtrip'`.
   Verify the step now lists all 16 new push-build entries.
3. **M3** — `scripts/p3a_vertex_input_negative_control.sh`: move the restore → rebuild → re-ctest block
   (`:186-197`) above the verdict so the `:161-169` branch shares it; write the two failure logs into
   `$LOG_DIR` instead of the repo root, or add a `.gitignore` rule.
4. **M4** — integrator decision on `FlushPendingRangesNow`: add it to `FUNCTIONS` (`:58`) and to the header
   table, re-capture the D.0 baseline; or record the withdrawal of E's risk-row mitigation in D.5.
5. **m1** — keep the `ctest -V` HandleRecycle log beside `handlerecycle-before.log` and cite it in
   `gates-v1.md` §5; **m2**, **m4** — correct the D6 and G14 wording.
6. Optional (**m5**, **m6**): digit-separator guard in `mask()`; brace lookup in `perturb()` over the masked
   text.

## 6. What the integrator must re-run on the finished tree

`gates-v1.md` §8's list is correct and I have nothing to remove from it. Add:

7. **Re-measure G14 against the current baseline.** `~/w7/p3a-before-ctest-names.txt` is now `5cb826b0`'s
   (2502). After the rebase, `comm -23` must be empty and the union must contain all 15 dev-side names
   (`LargeArenaAdoptionScenario.Respecified{Vertex,Index}Arena*` × 2 backends, 11
   `PersistentBufferOrderingProbeTest.*`) **plus** D's 34. ID-9's union resolution in
   `LargeArenaAdoptionScenario.cpp` is where a name can be lost.
8. **Re-run `p3a_untouched_regions.sh 44c2b5cf HEAD` after the espryt merge specifically.** I already
   verified it is rc 0 against `p3a-espryt@203120fa` and against `5cb826b0`; the merge resolution is the
   place a pool or ring body can still move.
9. **After M2's fix, confirm the `ResourceSubsystemOn/Off` lanes actually assert** — i.e. that they do not
   all skip — in the `integration-verify` job's controls step.
10. **`cmake --build build-bench && ctest --test-dir build-bench -L benchmark`** once (C.3's block, m8).
11. Re-check that the `ResourceSubsystemControl.Off` arm reads `mpr == 0` **and** that the `On` arm reads a
    non-zero `mpr` on a tree carrying B **and** C. A green Off arm with a skipping On arm is not the A/B.
