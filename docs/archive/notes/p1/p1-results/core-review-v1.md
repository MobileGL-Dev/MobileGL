# Adversarial review — p1-core v1 (tree `~/w7/p1-core`, branch `p1/core`, HEAD `440d3c52`)

Verdict: **NOT approved — 1 major, 8 minors.** The package is otherwise complete against C.1 and the D-decisions; every claim in `core-v1.md` that I could re-run reproduced. The one major is a defect in the delivered G5 negative-control unit tests, not in the filler/poison/comparator code.

Tree left exactly as found (`git status` clean apart from the three untracked build dirs; no file edited, nothing committed). All logs under `~/w7/p1-core-review/`.

## 1. What I re-ran (all on `440d3c52`; builds were `ninja: no work to do` in all three dirs)

| check | command | observed |
|---|---|---|
| G1 symbols | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --json` | `.text 10792579 -> 10792579 (+0)`, `Total +0`, `27799 -> 27799: 0 added, 0 removed, 0 resized, 0 renamed` |
| G1 bytes | `cmp -l before after \| wc -l`; `readelf -SW` diff; `strings` diff | 34 bytes; section table identical; the only differing strings are `GIT@5d99ee4 -> GIT@83b1656` and glslang's `Timestamp: 2026-09-05T23:14:30 -> 2026-09-06T02:05:06` (see minor 6 for the stamp value) |
| G2 (A's half) | `grep -rc pGLContext MobileGL/MG_Backend \| sort \| diff - ~/w7/p1-before-pglcontext.txt` | only `MG_Backend/MGPipe/PipeInputs.{cpp,h}:0` appear; every catalogued file's count unchanged — A converts no site, touches no B/C file |
| `pGLContext` in A's backend files | `grep -c pGLContext MobileGL/MG_Backend/MGPipe/*` | `0 0` |
| fill points | `grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp` | Drawing 36, Framebuffer 11, Texture 8, Getter 3, Program 1, Query 18, Sync 6 = **83**; no `MGP_FILL` outside `MG_Impl/GLImpl` |
| generator | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0; `63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `generated files are up to date`; six controls trip, positive control OK |
| D7 table vs `FillPoints.def` | `d7check.py` (mechanical diff of the brief's verb→class and class→field tables against the .def) | 69/69 verbs identical; 139/139 (class, field) rows identical |
| D8 field lists vs the structs (independent oracle) | clang `-ast-dump=json -ast-dump-filter=<Struct>` on `PipeFill.cpp`'s compile command, `FieldDecl` names vs `PipeFields.def` (`ast.py`) | RenderStateParameters 65/65, PixelStoreParameters 8/8, PerBufferBlendState 7/7, StencilFaceState 7/7, DynamicBackendParameters 85/85, MGHostSpan 5 members of which `Pad0` is the excluded padding → 4/4. The regex-based `check_field_lists_cover_struct_members()` agrees with clang on every struct |
| accessor signatures | `Core.h:66-506` vs `PipeInputs.h` | all 63 identical in name, parameters, constness and return type (incl. the `const&` returns for `GetRenderStateParameters`, `GetBlendColor`, `GetViewportIndexed`, …; no by-value copy of the 1168-byte struct was introduced). Only difference: `GetBufferBindingPointCount` is `constexpr` on `GLContext` and a plain forwarder here — the three backend uses (`DirectGLES.cpp:474`, `UniformManager.cpp:1189,2013`) are runtime, so nothing breaks |
| fill-probe side effects | `BufferState.cpp:90-98`, `TextureState.cpp:157-173`, `Core.cpp:179,594` | `GetBindingPoint(t,0)`, `GetUnitObject(0)`, `GetImageTextureBinding(0)`, `GetBoundVertexArray()` are pure lookups; `TouchBindPoint` is only reached from `Core.h:84` (the setter). `GetProgramForDraw()` joins link+SPIR-V — same verb as today's first read |
| unit, 3 configs | `ctest -L unit --no-tests=error -j 12` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1475` × 3 |
| `ctest -N` names | `comm` against `~/w7/p1-before-ctest-names.txt` | 2343 names, **0 removed**, 9 added; identical name sets in push and verify |
| skip visibility | `ctest -R 'PipeInputsTest' -V` | build-linux: all 5 `***Skipped` ("push not compiled in"); build-push: 4 skipped (poison/verify), `ReadingAFilledFieldCompletes` passes; build-verify: all 5 pass |
| nm -D | `nm -D --defined-only <lib> \| grep -c MGPipeVerifyInputs` | push 0, verify 1 |
| integration-gpu, push | `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | `100% tests passed, 0 tests failed out of 868`, `grep -c 'Fatal{'` = 0 |
| G4 lane smoke, verify | `MOBILEGL_PIPE_VERIFY=1 ctest -R 'DirectGLES\..*ClearThenReadPixels'` | 5/5 green; log has 1 × `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`, 0 `Fatal{` |
| G4 negative control A | same + `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` | rc 8, 0/5; `verify corruption armed - GetRenderStateParameters`; `Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear", verb=1, where=entry}` |
| FATAL=0 | + `MOBILEGL_PIPE_VERIFY_FATAL=0` | 5/5 green, 0 `Fatal{PipeVerifyDiffer` aborts, `verify summary - 3 divergence(s) survived MOBILEGL_PIPE_VERIFY_FATAL=0` |
| bad CORRUPT name | `MOBILEGL_PIPE_VERIFY_CORRUPT=NoSuchField` | rc 8, `Fatal{PipeVerifyBadKnob, "MOBILEGL_PIPE_VERIFY_CORRUPT=NoSuchField": no such field in kMGPipeInputFieldNames}` |
| POISON_OMIT env path, verify | `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit ctest -R 'DirectGLES\..*ClearThenReadPixels'` | green (no backend reads `gPipeInputs` on this tree), log has `poison omission armed - GetActiveTextureUnit@GenerateMipmap`, 0 `Fatal{` |
| POISON_OMIT bad verb | `MOBILEGL_PIPE_POISON_OMIT=NoSuchVerb:GetActiveTextureUnit` | rc 8, `Fatal{PipeVerifyBadKnob, "MOBILEGL_PIPE_POISON_OMIT=NoSuchVerb:GetActiveTextureUnit": no such verb in kMGPipeVerbNames}` |
| POISON_OMIT in a POISON-less push build | same in build-push | green, `MGPipe: poison omission GetActiveTextureUnit@GenerateMipmap requested but the poison is not compiled in (MOBILEGL_PIPE_POISON=0)` |
| `MOBILEGL_PIPE_VERIFY=1` in a push build | build-push | green, one `MGPipe: MOBILEGL_PIPE_VERIFY=1 requested but the comparator is not compiled in` |
| G5 unit, serial | `ctest --test-dir build-verify -R PipeInputsTest -j 1` | 5/5 |
| **G5 unit, parallel** | `ctest --test-dir build-verify -R PipeInputsTest -j 8` × 40 | **40/40 runs red** (major 1) |
| commit hygiene | `git log --format=%B` | 6 commits, `[Type] (Scope): …` subject + blank + `- ` bullets; 0 `Co-Authored-By`; nothing pushed |
| ownership | `git diff --stat feat/disaggregated..HEAD` (26 files) | every path is in A's C.5 rows; `DirectGLES/**`, `DirectVulkan/**`, `PipeCalls.def`, fixtures, `MG_IntegrationTest/**`, `test.yml`, docs untouched |
| logging discipline | grep `MGLOG_D`/stdio in `PipeFill.cpp`, `PipeInputs.{h,cpp}` | none |
| line endings | `git diff --name-only … \| xargs file \| grep -i crlf` | none (all LF) |

## 2. Major

### M1 — The G5 negative-control unit tests share one fixed log file and fail 100% under `ctest -j`

`MobileGL/MG_Test/Pipe/PipeInputsTest.cpp:255-270` (`main()`): every `PipeInputsTest` process computes the same path `temp_directory_path()/mobilegl-pipeinputs-test.log`, **unlinks it** (`fs::remove(path, ec)`, line 261) and points `MOBILEGL_LOG_FILE_PATH` at it. `gtest_discover_tests` registers the five cases as five separate processes, so under a parallel ctest all five start within milliseconds, each deleting the file the others are writing to and reading from (`ReadLog()` reopens by path, lines 49-54, 144/162, 175/191).

Reproduction (verify build, this tree):

```
ctest --test-dir build-verify -R 'PipeInputsTest' -j 1        # 100% tests passed, 0 tests failed out of 5
ctest --test-dir build-verify -R 'PipeInputsTest' -j 8        # red; 40 of 40 iterations red (~/w7/p1-core-review/stress2-*.log)
```

Observed failure text (iteration 1, `--output-on-failure`):

```
../MobileGL/MG_Test/Pipe/PipeInputsTest.cpp:192: Failure
Expected equality of these values:
  log.find("Fatal{")
    Which is: 49
  std::string::npos
    Which is: 18446744073709551615
[02:20:47] [Linux PipeInputsTest/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}
[  FAILED  ] PipeInputsTest.ReadingAFilledFieldCompletes (0 ms)
```

i.e. `ReadingAFilledFieldCompletes` reads the **sibling process's** Fatal line out of the shared file and goes red; in the first stress loop (`stress-*.log`) the other direction showed: `ReadingAnOmittedFieldAbortsNamingTheVerb` red 25/25 because a sibling's `main()` unlinked the file between the child's write and the parent's read, so its own line was gone.

Why major: these two cases are the package's G5 proof (`core-v1.md` "G5 unit"), and they go red for a reason that is not the poison — the opposite of `ROADMAP.md:7` (a gate goes red for its own reason) — whenever anyone runs the unit label with `-j` (the integrator's habit in D.1/D.3 is `-j 4`; my full `-L unit -j 12` run happened to pass only because the five cases were spread among 1470 others). The CI `test` job (`test.yml:192,194`) runs `ctest -L unit` serially today, which is why it has not shown up there; that is luck, not a property of the test. Fix is local to the test: a per-process file (e.g. `mobilegl-pipeinputs-test-<pid>.log`, removed at exit) — the forked child inherits the path, so nothing else changes.

## 3. Minors

1. **Undeclared 10th placement deviation — `GL_Sync.cpp:235` (`DeleteSync` in the orphan sweep).** `MGP_FILL(DeleteSync);` sits inside the loop body but **before** `if (backendDeleteSync && syncObject->backendHandle)`, so a backend with a null `DeleteSync` entry, or a sync without a backend handle, bumps the serial for every orphan. D7 says a null entry never bumps, and the sibling loop at `GL_Query.cpp:937` (`DeleteBackendQuery`) was placed inside the guard. Harmless (kQuery fills one field; nothing reads between two fills) — the same argument as the nine sites the result file declares — but it is not in the declared list (deviation 1 names 9 sites). Either move it inside the guard or add it to the record.

2. **The compare-at-read arm has zero execution coverage in this package.** `MGPipeVerifyReadHook` (`PipeFill.cpp:409-426`) is the arm the result file and D8 call "the arm that is real in P1", yet no unit test calls an accessor with `Features.PipeVerify` set after mutating the live context, and on this tree no lane can reach it (no backend spells `MGB_CTX->` yet, so no accessor is ever entered). It is exercised for the first time only in D.3 on the integrated tree, where a hook that silently never fires would look like "zero divergence". The code reads correctly (`&self == &gPipeInputs`, sticky skip, `InHook` guard, `CopyField` into `g_readScratch`, `MGPipeInputsFieldEqual`, `ReportDivergence(field, "read")`), but the brief's own mitigation for "green because it never ran" is a falsifier, and the entry compare got one (CORRUPT) while this arm got none. Cheap unit test in the same fork shape: `MG_Config::Features.PipeVerify = true; MGPipeFillForVerb(DrawArrays); pGLContext->SetLineWidth(2.f); (void)gPipeInputs.GetLineWidth();` → expect `SIGABRT` and `Fatal{PipeVerifyDiffer, "GetLineWidth@DrawArrays", verb=<serial>, where=read}`. (Not required by C.1's test list, hence minor.)

3. **`OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale` does not prove "exactly".** `PipeInputsTest.cpp:106-132` asserts one sibling (`GetTextureUnitObject`) and one sticky field; "exactly that field" would be a loop over `kMGPipeClassFieldMask[kTextureOp]` asserting every other bit fresh (the shape `EveryVerbFillsItsClassAndNothingElse` already uses at `:238-247`). Two lines; makes the test name true.

4. **The two env-knob parsers are not covered by any registered test.** `ParsePoisonOmissionKnob` (`PipeFill.cpp:300-316`) and `ArmVerify`'s CORRUPT parse (`:351-371`) are only reachable through `MG_Config::Features`, which the unit binary never loads (I confirmed: `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit ctest -R ReadingAFilledFieldCompletes` in build-verify does not arm — the itest path does). I exercised both parsers manually through `DirectGLES.*ClearThenReadPixels` (table above: armed lines, `PipeVerifyBadKnob` on a bad field and a bad verb, the W_ONCE in a POISON-less push build) and they behave; the record is that this coverage lives in E's lanes on the integrated tree, not in A's tests.

5. **`IsCapabilityEnabledIndexed` semantic asymmetry to record.** Per D6 the push accessor aborts for a cap other than `Blend`/`ScissorTest` (`PipeInputs.h:448-460`), while the pull arm returns `false` with an `MGLOG_I` (`RenderState.cpp:471-488`: "a query must never be able to terminate the process"). No backend asks another cap today (`VulkanRenderer.cpp:5437` asks `Blend`), so it cannot trip in P1, but it is the one accessor where push and pull disagree on a *reachable* input by design; worth a line in `MEASUREMENTS.md` (integrator) so the next person who adds `glIsEnabledi`-driven backend reads knows the push arm is fatal there.

6. **`build-linux/libMobileGL.so` carries `GIT@83b1656`, not the `GIT@087685d` the result file cites.** The stamp is taken at configure time; the library in the tree was last linked at c5 (`83b16561`), and c6 (`440d3c52`) touches only `MG_Test/Pipe/**`, so the G1 comparison is of the right bytes (I confirmed `ninja: no work to do` and the section table identical). Purely a bookkeeping mismatch between `core-v1.md` and the artifact; the integrator's D.1 re-run on `~/w7/pipe` supersedes it.

7. **Generated-file "7 files change" (C.1) vs 3 — correctly recorded by the implementer**, and I confirm the reason: `Coverage.def`'s two new accessor rows change `PipeFilled.inc` (63 + sticky) but not `PipeCoverage.inc` (which iterates inventory rows only), and `PipeVerify.inc` changes at c5. `--check` is clean, so nothing is stale.

8. **`kMGPipeForwardedFieldCount = 7` is duplicated by hand** (`PipeInputs.h:133`) next to the generated `kMGPipeInputStickyFieldCount = 7` (`PipeFilled.inc:230`); the two are tied only by `StickyFieldsAreExactlyTheSeven`. A `static_assert(kMGPipeForwardedFieldCount == kMGPipeInputStickyFieldCount)` in `PipeInputs.h` would make the header refuse an eighth sticky row instead of the test.

## 4. Where the tree and the brief disagree (verified; the implementer's list is right)

- `GL_Drawing.cpp:906/907` `MemoryBarrier (TextureBarrier)`: the call is on 906, the brief's 907 is the closing brace — fill placed at (now) 932, before the call. ✓
- `RenderStateParameters` has **65** direct data members (clang FieldDecl count; brief D8 says 67). `DynamicBackendParameters` **85**. ✓
- `CapabilityInput`: storage is sized from the enum; the brief's "37" is not load-bearing anywhere in the code. (Not independently recounted.)
- The result file's `ctest -N` regex note (`'^\s+Test\s+#[0-9]+:'`) is right: ctest right-aligns the number; the brief's `'^\s+Test #'` would drop the padded rows. My name diff used the padded form and matched the baseline count (2343 = 2334 + 9).

## 5. Notes for the integrator (not A's findings, surfaced while reviewing)

- **The brief's CI negative-control step (C.4 (b)) as spelled is vacuous on this tree:** `ctest -L integration-verify -R 'DirectGLES\.Verify\..*Mipmap'` — no integration test has "Mipmap" in its name (`ctest -N | grep -i mipmap` finds 7 names, all `TextureTest.*`/`DirectGLESSanity.*` unit-label cases; `grep -rli generatemipmap MobileGL/MG_IntegrationTest/` is empty). With `--no-tests=error` a no-match run exits non-zero, so the inverted "must fail" check would pass without any process ever loading the library. E's `PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb` does not contain "Mipmap" either. Either the scenario's name or the regex needs to change, and the step should additionally grep the lane log for `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}` so it cannot be satisfied by "no tests found". Same applies to D.3's `-R 'Mipmap'` line.
- The compare-at-read hook (minor 2) is the thing to watch in D.3: if the 79 traces and 742 entries report zero `where=read` divergences, make sure at least one accessor was actually entered under verify (e.g. count `MGPipe: verify read of` lines with a deliberately mutated context, or land minor 2's unit test first).

## 6. Verdict

Not approved: fix M1 (per-process log path in `PipeInputsTest::main`, and re-run `ctest --test-dir build-verify -R PipeInputsTest -j 8` a few dozen times green), then re-review. Minors 1-3 and 8 are one-line-each and worth taking in the same pass; 4-7 are records.
