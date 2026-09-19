# P1 package A (p1-core) — rework round 2 (v2)

Tree `~/w7/p1-core`, branch `p1/core`, base `feat/disaggregated @ 087685d1`, contract `bf86b1ed` (tag `p1/contract`). v1 HEAD was `440d3c52` (reviewed in `core-review-v1.md`: 1 major, 8 minors). Four new commits on top; nothing pushed; working tree clean apart from the three untracked build directories. `core-v1.md` remains the record of c2..c6; this file records the rework and the full re-verification on the new HEAD.

## Commits (oldest first, on top of `440d3c52`)

| sha | subject | review item |
|---|---|---|
| `510ecd92` | `[Fix] (Impl): move the DeleteSync fill of the orphan sweep inside its null-entry guard` | minor 1 |
| `77ecde15` | `[Fix] (Pipe): refuse an eighth sticky row at compile time - the forwarded count equals the generated sticky count` | minor 8 |
| `878db2c4` | `[Fix] (Pipe): re-parse the POISON_OMIT and VERIFY knobs when their Features value changes, not once per process` | minor 4 (enabler) |
| `d9f4698d` | `[Test] (Pipe): give every PipeInputsTest process its own log file, and cover the compare-at-read arm, the three knob parsers and VERIFY_FATAL=0 with forked children` | **M1**, minors 2, 3, 4 |

`git diff --stat 440d3c52..HEAD`: 4 files, +283/-37 — `MobileGL/MG_Backend/MGPipe/PipeInputs.h` (+4), `MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.cpp` (+1/-1), `MobileGL/MG_Impl/Pipe/PipeFill.cpp` (+15/-2), `MobileGL/MG_Test/Pipe/PipeInputsTest.cpp` (rewritten). All inside A's C.5 rows; no B/C/E file, no fixture, no generated file (`gen_pipe --check` clean), no signature that B/C/E code against changed (`MGB_CTX`, `MGP_FILL`, the 63 accessors, the knob names and every log line are as in the contract).

## What was done per review item

- **M1 (shared log path)** — `PipeInputsTest::main()` names the file `mobilegl-pipeinputs-test-<pid>.log` (`ProcessId()` = `getpid`/`_getpid`) and removes it after `RUN_ALL_TESTS`; a forked child inherits the path on purpose (it is the same file the parent reads the delta from). Proof: `ctest --test-dir build-verify -R PipeInputsTest -j 8` × 40 → **0/40 red** (was 40/40 red at v1 per the review); build-push × 10 → 0/10 red; `/tmp/mobilegl-pipeinputs-test*` leftover count after the whole suite: see the verification table.
- **Minor 1 (`GL_Sync.cpp` DeleteSync sweep)** — the fill moved inside `if (backendDeleteSync && syncObject->backendHandle)`, the shape of `GL_Query.cpp`'s `DeleteBackendQuery` sweep; D7 now holds there (a null entry never bumps). Fill count still 83 (Sync 6). Pull build unaffected (`MGP_FILL` is `((void)0)`): G1 re-proven below.
- **Minor 2 (compare-at-read arm had no falsifier)** — `PipeInputsTest.MutatedFieldIsNamedAtRead` (verify build, fork): child sets `Features.PipeVerify = true`, fills `DrawArrays`, reads `GetLineWidth()` (completes, boundary == live) and `GetRenderStateParameters()`, then `pGLContext->SetLineWidth(boundary + 1)` and reads `GetLineWidth()` again; parent asserts `SIGABRT`, the arming line `verify armed - 63 fields, 69 verbs, fatal=1`, `verify read of GetLineWidth (index 0, 0) differs from the live context`, exactly `Fatal{PipeVerifyDiffer, "GetLineWidth@DrawArrays", verb=<parent serial+1>, where=read}` and no `where=entry`. Passes in build-verify; visible skip elsewhere.
- **Minor 3 (name overclaimed)** — `OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale` now loops all 63 fields after the omitted `GenerateMipmap` fill: fresh iff the bit is in `kMGPipeVerbClass[GenerateMipmap]`'s mask and the field is not `GetActiveTextureUnit`.
- **Minor 4 (knob parsers unreachable from a unit test)** — the two latches in `PipeFill.cpp` now key on the VALUE rather than "parsed once": `ParsePoisonOmissionKnob` re-parses when `Features.PipePoisonOmit` differs from the last parsed string; `ArmVerify` re-arms when `Features.PipeVerify` differs from `g_verify.Enabled` (and resets `Corrupt` on re-arm). A lane loads `Features` before its first fill, so lane behaviour is one parse/arm per process as before; a forked test child that sets `Features` after its parent filled gets its own parse. Cost: one `String ==` (two empty strings in every shipped configuration) and one `Bool ==` per fill, push/verify builds only. Five new fork cases cover: the OMIT knob arming through `Features` (arming line + the exact `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}`), a bad verb in OMIT (`Fatal{PipeVerifyBadKnob, "MOBILEGL_PIPE_POISON_OMIT=NoSuchVerb:GetActiveTextureUnit": no such verb in kMGPipeVerbNames}`; this one runs in build-push too — the parser is switch-free), the CORRUPT knob through `Features` (`verify corruption armed - GetRenderStateParameters` + `Fatal{PipeVerifyDiffer, "GetRenderStateParameters@DrawArrays", verb=N, where=entry}`), a bad CORRUPT field (`Fatal{PipeVerifyBadKnob, "MOBILEGL_PIPE_VERIFY_CORRUPT=NoSuchField": no such field in kMGPipeInputFieldNames}` and no arming line), and `PipeVerifyFatal = false` (child exits 0 after two fills, log has both `PipeVerifyDiffer` lines at consecutive serials, `fatal=0` in the arming line). The `MGLOG_W_ONCE` for OMIT in a POISON-less push build and for `MOBILEGL_PIPE_VERIFY=1` in a comparator-less build stay lane-verified only (the review's table) — they are one-shot log lines and the review already exercised them.
- **Minor 5 (`IsCapabilityEnabledIndexed` asymmetry)** — record only (below, for the integrator's `MEASUREMENTS.md`); no code change: the push arm's Fatal for a cap other than Blend/ScissorTest is D6's rule for a field the block does not carry, and no backend asks another cap.
- **Minor 6 (GIT@ stamp)** — record: all three libraries still carry `GIT@83b1656` (the stamp is a configure-time value; no reconfigure happened in this round, only incremental builds, which relinked `build-linux/libMobileGL.so` because `GL_Sync.cpp` changed). The G1 comparison below is of the relinked library.
- **Minor 7 (3 vs 7 generated files)** — record only; `gen_pipe --check` clean.
- **Minor 8 (hand-written 7)** — `static_assert(kMGPipeForwardedFieldCount == kMGPipeInputStickyFieldCount)` in `PipeInputs.h` (the generated constant is visible through `MGPipe.h` → `generated/PipeFilled.inc`).
- **Integrator note in the review (vacuous `-R '...Mipmap'` CI regex)** — not A's file; recorded under "for the integrator" below.

## Verification (WSL, `~/w7/p1-core`, HEAD `d9f4698d`; logs `~/w7/p1-core-v2-*.log`, summary `~/w7/p1-core-v2-summary.txt`)

Builds: `cmake --build {build-linux,build-push,build-verify} -j 12` all rc=0 (`~/w7/p1-core-v2-build.log`), `CCACHE_BASEDIR=/home/swung/w7`.

| command | actual output |
|---|---|
| `python3 scripts/gen_pipe.py --check` | rc=0, `gen_pipe: generated files are up to date` |
| `python3 scripts/gen_pipe.py --self-test` | rc=0, 6 controls `tripped as expected`, `positive control OK` |
| `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc=0; `.text 10792579 -> 10792579 (+0, +0.000%)`; `Total 17209819 -> 17209819 (+0)`; `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed, 27060 unchanged` (`~/w7/p1-core-v2-symbol.md`) |
| `cmp -l before after \| wc -l` / `readelf -SW` diff | 34 bytes (build-id + the two build stamps, as at v1); section table identical |
| `strings ... \| grep GIT@` | `GIT@83b1656` in all three libraries (configure-time stamp; see minor 6) |
| `grep -rc pGLContext MobileGL/MG_Backend \| sort \| diff - ~/w7/p1-before-pglcontext.txt` | only `MG_Backend/MGPipe/PipeInputs.{cpp,h}:0` appear; every catalogued count unchanged |
| `grep -c pGLContext MobileGL/MG_Backend/MGPipe/PipeInputs.{h,cpp}` | `0` `0` |
| `grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp` | **83**: Drawing 36, Framebuffer 11, Getter 3, Program 1, Query 18, Sync 6, Texture 8 |
| `ctest --test-dir build-linux -L unit --no-tests=error -j 12` | rc=0, `100% tests passed, 0 tests failed out of 1481` |
| `ctest --test-dir build-push -L unit --no-tests=error -j 12` | rc=0, `100% tests passed, 0 tests failed out of 1481` |
| `ctest --test-dir build-verify -L unit --no-tests=error -j 12` | rc=0, `100% tests passed, 0 tests failed out of 1481` |
| `ctest -N` names vs `~/w7/p1-before-ctest-names.txt` (regex `'^\s+Test\s+#[0-9]+:'`) | 2349 names in each of the three dirs; `comm -23 before now` = **0 removed**; 15 added = the 4 `PipeCatalogue.*` of v1 + 11 `PipeInputsTest.*` (the v1 five + `MutatedFieldIsNamedAtRead`, `PoisonOmitKnobArmsTheOmission`, `BadPoisonOmitKnobIsFatalNamingTheKnob`, `VerifyCorruptKnobNamesTheFieldAtEntry`, `BadVerifyCorruptKnobIsFatalNamingTheKnob`, `VerifyFatalOffLogsTheDivergenceAndContinues`); push and verify name sets identical |
| `ctest -R PipeInputsTest -V`, skip visibility | build-linux 11 `***Skipped` ("push not compiled in"); build-push 9 skipped (5 "verify not compiled in", 4 "poison not compiled in"), `ReadingAFilledFieldCompletes` and `BadPoisonOmitKnobIsFatalNamingTheKnob` pass; build-verify 0 skipped, 11/11 pass |
| **M1 repro, fixed**: `ctest --test-dir build-verify -R PipeInputsTest -j 8` × 40 | **red=0/40** (`~/w7/p1-core-v2-stress-{1..40}.log`); v1 was 40/40 red per the review |
| `ctest --test-dir build-push -R PipeInputsTest -j 8` × 10 | red=0/10 |
| `nm -D --defined-only <lib> \| grep -c MGPipeVerifyInputs` | push 0, verify 1 |
| `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | PENDING-ITEST |
| leftover `/tmp/mobilegl-pipeinputs-test*` after the whole suite | PENDING-LEFTOVER |
| G4 unit (`CorruptedSnapshotFieldIsNamedWithItsSerial`, `VerifyCorruptKnobNamesTheFieldAtEntry`, `MutatedFieldIsNamedAtRead`) | pass in build-verify: entry compare names the corrupted field at its serial (block level and through the knob), and the compare-at-read arm names a mutated field `where=read` — both arms now have a falsifier |
| G5 unit (`OmittingOneField...`, `ReadingAnOmittedFieldAbortsNamingTheVerb`, `PoisonOmitKnobArmsTheOmission`, `ReadingAFilledFieldCompletes`) | pass in build-verify (the two poison-free ones also in build-push): the exact `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}`, no `@DrawArrays`, no `GetTextureUnitObject@`; the sibling exits 0 with no `Fatal{` |
| commit hygiene | 4 commits, `[Type] (Scope): ...` + blank + `- ` bullets, no Co-Authored-By/attribution, nothing pushed; `git status` clean; all LF |

Not run (unchanged from v1): the 40-trace retrace under verify and the `integration-verify` lane (E's files, D.3 on the integrated tree).

## Deviations from the brief (with reasons) — additions to v1's list 1-13

14. **The knob latches key on the value, not "once per process"** (`PipeFill.cpp`; the brief/contract said "parsed once at the first fill"). Reason: without it no unit test can reach the parsers or `Fatal{PipeVerifyBadKnob}` (review minor 4); lane behaviour is unchanged because `Features` is loaded before the first fill. Cost is one empty-string compare and one Bool compare per fill in push/verify builds only.
15. **`ArmVerify` resets `Corrupt` on re-arm** — a consequence of 14 (a stale corruption must not outlive the knob value that named it).
16. **v1 deviation 1's "nine guarded-expression sites" is now exactly nine**: the tenth, undeclared one (`GL_Sync.cpp` DeleteSync sweep) was moved inside its guard rather than added to the list, because the sibling `GL_Query.cpp` sweep already had the in-guard shape and D7 is satisfiable there at zero cost.
17. **`PipeInputsTest` has 11 cases, not the brief's/v1's 5**; the six additions are the review's minor 2 and minor 4 falsifiers. `PipeCatalogueTest` unchanged.
18. **`OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale` asserts the whole class mask** (the brief's "verbatim shape" names two fields); superset of the brief's assertions.

## Where the tree contradicted the brief's numbers

Unchanged from v1 (`GL_Drawing.cpp:906` vs 907; `RenderStateParameters` 65 members not 67; `CapabilityInput` 35 not 37; c5 regenerates 1 file not 7; integration-gpu 868 not 371+3; the `ctest -N` regex needs the padded form). New in this round: none.

## Records for the integrator (not A's code)

- **`IsCapabilityEnabledIndexed` asymmetry (review minor 5)**: the push accessor is `Fatal` for a cap other than `Blend`/`ScissorTest` (`PipeInputs.h`, per D6 — the block carries only those two indexed caps), the pull arm returns `false` with `MGLOG_I` (`RenderState.cpp`). Unreachable today (`VulkanRenderer.cpp` asks `Blend` only); a `MEASUREMENTS.md` line is the right place, so that a future `glIsEnabledi`-driven backend read adds a field rather than tripping the Fatal.
- **CI negative-control regex (review section 5)**: `-R 'DirectGLES\.Verify\..*Mipmap'` matches no integration test name on this tree, and E's `PoisonOmissionScenario` name has no "Mipmap"; with `--no-tests=error` the inverted step passes vacuously. The step should also grep the lane log for `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}`. The unit-level G5 proof (`ReadingAnOmittedFieldAbortsNamingTheVerb`, `PoisonOmitKnobArmsTheOmission`) is independent of that regex.
- **Compare-at-read in D.3**: `MutatedFieldIsNamedAtRead` is the falsifier the review asked for; a D.3 run reporting zero `where=read` divergences is now known to have a hook that fires when a value diverges.
- The `GIT@` stamp in the three libraries will refresh at the integrator's reconfigure on `~/w7/pipe`.

## Unfinished

Nothing in A's scope. Every major and minor in `core-review-v1.md` is either fixed (M1, minors 1, 2, 3, 4, 8) or recorded above with the reason (minors 5, 6, 7 are bookkeeping/records by the reviewer's own classification).
