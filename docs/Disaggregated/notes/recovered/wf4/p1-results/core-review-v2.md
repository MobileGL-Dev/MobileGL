# Adversarial review — p1-core v2 (tree `~/w7/p1-core`, branch `p1/core`, HEAD `d9f4698d`)

Verdict: **NOT approved — 1 major, 5 minors.** Every claim in `core-v2.md` that can be re-run reproduced on this HEAD, all eight v1 items are fixed or honestly recorded, and the four rework commits are correct for what they set out to do. The major is not in the rework; it is a gap in the poison mechanism itself that neither round's tests reach, and it contradicts a sentence of D6 verbatim. It is a one-line fix in a file this package owns.

Tree left exactly as found: `git status` shows only the three untracked build directories; no file edited, nothing committed, nothing pushed. Reviewer logs under `~/w7/p1-core-review2/`; scratch inputs under `scratchpad/wf4/core-review2/` (`s1.sh`, `s2.sh`, `s3.sh`, `prefill.cpp`, `d7check.py`).

## 1. What I re-ran (all on `d9f4698d`; all three build dirs were `ninja: no work to do`)

| check | command | observed |
|---|---|---|
| G1 symbols | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `.text 10792579 -> 10792579 (+0, +0.000%)`; `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed, 27060 unchanged` |
| G1 bytes | `cmp -l … \| wc -l`; `readelf -SW` diff | 34 bytes; section table identical; library stamp `GIT@83b1656` (configure-time, as the result file records) |
| G2 (A's half) | `grep -rc pGLContext MobileGL/MG_Backend \| sort \| diff - ~/w7/p1-before-pglcontext.txt` | only `MG_Backend/MGPipe/PipeInputs.{cpp,h}:0` appear; every catalogued count unchanged — A converts no site, touches no B/C file |
| `grep -c pGLContext MobileGL/MG_Backend/MGPipe/*` | | `0 0` |
| fill points | `grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp` | Drawing 36, Framebuffer 11, Getter 3, Program 1, Query 18, Sync 6, Texture 8 = **83**; none outside `MG_Impl/GLImpl` |
| fill placement | read of every hunk in `git diff feat/disaggregated..HEAD -- MobileGL/MG_Impl/GLImpl` | every statement is immediately before its table call and after the early returns it is behind; the frontend mutation that precedes the call (`BindImageTexture`'s `Bind`/`NoteTextureUnitTouched`, `SetPatchVertices`, `BeginTransformFeedback`, `SetTransformFeedbackPaused`, `BindTransformFeedbackObject`, `ScopedNeutralPackState`) is before the fill in every case; the nine guarded-expression sites are the nine declared; `GL_Sync.cpp:236` now inside its guard (v1 minor 1 fixed) |
| generator | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0; `63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `generated files are up to date`; 6 controls trip, positive OK |
| **generator gates against real drift** (scratch copy `/tmp/gp-review`, never the tree) | 11 mutations, `--check` each | m1 `MGB_CTX->NotAnAccessor()` in `Utils.cpp` → rc 1 `Coverage.def: accessor(s) read by a backend with no row: NotAnAccessor (…/Utils.cpp)`; m2 the same inside a `//` comment → rc 0 (masked); m3 `MGB_CTX->GetBoundTransformFeedbackName()` → the "no backend reads" line disappears (the `MGB_CTX->` spelling IS collected — first time it has met one); m4 `pGLContext->NotAnAccessor2()` → rc 1; m5 `GenerateMipmap` verb row deleted → rc 1 `member(s) without a verb row: GenerateMipmap`; m6 verb class changed → rc 1 `OUT OF DATE: …PipeFillPoints.inc`; m7 `Int ReviewerExtraMember` added to `RenderStateParameters` → rc 1 `member(s) with no F(...)`; m8 field row swapped → rc 1 (stale inc); m9 generated file touched → rc 1; m10 duplicate `(kDraw, GetLineWidth)` → rc 1; m11 `X(kDraw, NotAField)` → rc 1 `not an accessor in Coverage.def` |
| D7 tables vs `FillPoints.def` | `d7check.py` (brief tables parsed mechanically) | 69/69 verb→class identical; 139/139 (class, field) rows identical, per class 47/14/18/18/7/15/8/11/1 |
| unit, 3 configs | `ctest -L unit --no-tests=error -j 12` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1481` × 3 |
| `ctest -N` names | padded regex, `comm` against `~/w7/p1-before-ctest-names.txt` | 2349 names in each dir; **0 removed**, 15 added (4 `PipeCatalogue.*` + 11 `PipeInputsTest.*`); linux/push/verify name sets identical |
| skip visibility | `ctest -R PipeInputsTest -V` | build-linux 11 `***Skipped`; build-push 9 skipped + `ReadingAFilledFieldCompletes`, `BadPoisonOmitKnobIsFatalNamingTheKnob` pass; build-verify 11/11 pass |
| **M1(v1) repro, fixed** | `ctest --test-dir build-verify -R PipeInputsTest -j 8` × 40 | **0/40 red**; build-push × 10 → 0/10; `ls /tmp/mobilegl-pipeinputs-test* \| wc -l` = 0 afterwards |
| nm -D | `nm -D --defined-only <lib> \| grep -c MGPipeVerifyInputs` | push 0, verify 1 |
| integration-gpu, push | `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | `100% tests passed, 0 tests failed out of 868`; `grep -c 'Fatal{'` = 0 |
| G4 lane smoke, verify | `MOBILEGL_PIPE_VERIFY=1 MOBILEGL_LOG_FILE_PATH=… ctest --test-dir build-verify -R 'DirectGLES\..*ClearThenReadPixels'` | 5/5 green; 1 × `MGPipe: verify armed`, 0 `Fatal{`, 0 `verify read of` (no backend reads `gPipeInputs` on this tree — expected) |
| G4 negative control A | + `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` | rc 8, 0/5; `Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear", verb=1, where=entry}` |
| FATAL=0 | + `MOBILEGL_PIPE_VERIFY_FATAL=0` | 5/5 green; 1 × `verify summary - … survived MOBILEGL_PIPE_VERIFY_FATAL=0` |
| POISON_OMIT env path | `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit` | green (no reader on this tree), 1 × `poison omission armed` |
| accessor edge semantics | `Core.cpp:98-110,246-253,309-319,806,938-948,965-1041,1087,1180`; `RenderState.cpp:18-26,101-107,471-478,541-593,666,694-696,770-776`; `BufferState.cpp:43-50,90-97`; `TextureState.cpp:157-173` vs `PipeInputs.h:199-556` | bounds handling matches GLContext's for every indexed accessor (same `MOBILEGL_ASSERT`+fallback shapes, `GetColorMaskIndexed` unchecked on both sides, stencil `Back→1 else 0` = `GetStencilFaceIndex`); `GetRenderStateParameters` returns `const&` on both sides — no 1168-byte copy on a read path |
| O-class target domain | `grep -o 'GetBufferBindingSlot([^)]*)' MG_Backend`; `SyncBoundBuffer` callers `DirectGLES.cpp:664,691`; `GetBufferBindingPoint` targets | slots read: DrawIndirect ×16, PixelPack ×7, Parameter ×4, DispatchIndirect ×1 — all in `GlobalBufferTargets`; points read: Uniform/TransformFeedback/AtomicCounter/ShaderStorage = `BufferBindPointTargets`; the variable-target loop at `DirectGLES.cpp:369-370` is bounded by a touched count that is 0 for any other target. No read the fill leaves null is reachable today |
| commit hygiene | `git log --format=%B 440d3c52..HEAD` | 4 commits, `[Type] (Scope): …` + blank + `- ` bullets, 0 `Co-Authored-By`; nothing pushed; all LF |
| ownership | `git diff --stat feat/disaggregated..HEAD` (26 files) | every path in A's C.5 rows; the rework touched 4 files, all A's |
| logging discipline | grep `MGLOG_D`/stdio in `PipeFill.cpp`, `PipeInputs.{h,cpp}` | none |

## 2. Major

### M1 — The poison (and the compare-at-read hook) cannot trip on any read made before the first fill: `FilledGen == 0` is "fresh" while `CurrentVerbSerial == 0`

D6, verbatim: `++m_filled.CurrentVerbSerial` "(starts at 1 so `FilledGen == 0` means never filled)", and the Fatal "before the first verb the name is `<none>`" (`PipeInputs.h:29-30` repeats the promise: `"<none>" before the first verb`). The freshness predicate that every `MGP_INPUT_CHECK` (`PipeInputs.h:44-49`) evaluates is

```
MobileGL/MG_Pipe/generated/PipeFilled.inc:313-317   (emitted by scripts/gen_pipe.py gen_filled)
inline Bool MGPipeInputFieldIsFresh(const MGPipeFilledState& state, MGPipeInputField field) {
    return kMGPipeInputFieldSticky[index] ? state.FilledGen[index] != 0
                                          : state.FilledGen[index] == state.CurrentVerbSerial;
}
```

Before the first `MGPipeFillForVerb` both sides are 0 (`gPipeInputs` is value-initialised, `PipeInputs.h:612,678`), so `0 == 0` is **fresh**: every V-class accessor and both `SharedPtr` O-class accessors return their default-constructed storage (zeros, empty `SharedPtr`) with no Fatal. The serial only "starts at 1" *after* the first fill (`PipeFill.cpp:517-519`), which makes 0 distinguishable from every filled generation but does not make it stale. The second net has the same window: `ArmVerify()` runs only inside `MGPipeFillForVerb` (`PipeFill.cpp:505-507,361-366`), so `MGPipeVerifyReadHook` sees `g_verify.Enabled == false` (`PipeFill.cpp:421`) until the first fill even when `Features.PipeVerify` is already set. Only the four raw-pointer O-class accessors (`GetBufferBindingSlot`, `GetBufferBindingPoint`, `GetFramebufferBindingSlot`, `GetImageTextureBinding`/`GetTextureUnitObject`) trip before the first fill, through their null-base checks (`PipeInputs.h:489,499,512,521,552`) — 55 of 63 fields are silently served wrong values.

Reproduction (a 30-line program compiled and linked with `PipeInputsTest`'s own ninja commands against `build-verify`, nothing in the tree touched; `scratchpad/wf4/core-review2/prefill.cpp`, `s3.sh` section (a)):

```
$ MOBILEGL_LOG_FILE_PATH=~/w7/p1-core-review2/prefill-plain.log ~/w7/p1-core-review2/prefill
before-first-fill: serial=0 gen[GetLineWidth]=0 fresh=1 live=1
read completed: stored=0 live=7                 # gPipeInputs.GetLineWidth() after pGLContext->SetLineWidth(7.f), no fill
GetRenderStateParameters completed: LineWidth=1
GetProgramForDraw completed: (nil)
rc=0                                            # expected per D6: SIGABRT, Fatal{UnmigratedPipeInput, "GetLineWidth@<none>"}
$ … prefill x                                   # same with MG_Config::Features.PipeVerify = true before the reads
before-first-fill: serial=0 gen[GetLineWidth]=0 fresh=1 live=1
read completed: stored=0 live=7
rc=0                                            # no "verify armed", no "verify read of", no Fatal in the log
```

Why major: this is the "poison that can never trip" shape, on the exact window the brief's own risk table names (E: "A read outside any verb … If the lane finds another (`@<none>` in the Fatal), the answer is a fill point"). With this predicate the lane cannot find one for a V-class field: a backend read that lands before the first verb — an init-time capability probe, the first link, anything reached from `eglMakeCurrent` — is served zeros by the push arm, is not compared by the read hook, and reports nothing. `PipeCatalogue.PipeInputFieldsStartUnfilled` (`PipeCatalogueTest.cpp:225-228`, pre-P1) sets `state.CurrentVerbSerial = 1` by hand before asserting "unfilled", i.e. the P0.5 test already steps around serial 0; none of the 11 `PipeInputsTest` cases reads before a fill. Honest scope: **latent on this tree** — the scout catalogue (`scout-pull-sites.md:951`) finds exactly two out-of-verb pulls, both `InvalidateCompileEnv` (sticky, forwarded, unchecked), so D.3 will not hit it today. It is still a deviation from D6 that nobody declared, in a mechanism whose whole value is catching the case that is not catalogued yet.

Fix (A's files): emit `state.FilledGen[index] != 0 && state.FilledGen[index] == state.CurrentVerbSerial` for the non-sticky arm in `gen_filled` (`gen_pipe.py`, the `MGPipeInputFieldIsFresh` text) and regenerate; add the serial-0 assertion to `PipeInputFieldsStartUnfilled`; add one fork case (`ReadingBeforeAnyFillAbortsNamingNoVerb`: live context, no fill, `gPipeInputs.GetLineWidth()` → SIGABRT + `Fatal{UnmigratedPipeInput, "GetLineWidth@<none>"}`). With the poison fixed the read hook's arming-at-first-fill is harmless in a verify build (a pre-fill read is Fatal before the hook matters); if a plain push build without POISON is also meant to be covered, arm in the hook when `!g_verify.Parsed`.

## 3. Minors

1. **The seven F-class forwarders carry no `MGP_INPUT_CHECK`** (`PipeInputs.h:563-571`, `PipeFill.cpp:464-500`), while D4 says "Every accessor body is: `MGP_INPUT_CHECK(...)` → `MGP_INPUT_VERIFY_READ(...)`" and the header comment narrows it to "every non-forwarded accessor" (`PipeInputs.h:40,155`). This is the *right* behaviour — the two init-time `InvalidateCompileEnv` calls (`BackendObject_DirectVulkan.cpp:388-390,786-788`) run before any fill and would otherwise be `Fatal{…@<none>}` — but it is an undeclared deviation, and it means the sticky stamp (`FilledGen = 1`, `PipeFill.cpp:530-535`) and the `Sticky → FilledGen != 0` branch are consulted by no accessor at all: the sticky machinery is exercised only by tests that call `MGPipeInputFieldIsFresh` directly. Record it in the deviations list (and in `MEASUREMENTS.md`), so P2's tracker does not "fix" the missing check.

2. **`ArmVerify` re-arms only when `Features.PipeVerify` changes** (`PipeFill.cpp:362`); `PipeVerifyCorrupt` and `PipeVerifyFatal` are latched at that moment (`:367-374`) and a later change to either without a `PipeVerify` toggle is not seen. Deviation 14 and the `878db2c4` subject ("re-parse the POISON_OMIT and VERIFY knobs when their Features value changes") overstate this. Lane behaviour is unaffected (all three are loaded before the first fill); the unit tests set all three before the child's first fill. Say so precisely in the record.

3. **`GetBufferBindingSlot(BufferTarget::Index)` diverges between arms by design and no `FillPoints.def` row can repair it.** `GLContext::GetBufferBindingSlot` is polymorphic (`Core.cpp:98-106`: `Index` returns the bound VAO's element-buffer slot); the fill copies `GlobalBufferTargets` only and leaves `Index` null (`PipeFill.cpp:63-69`), so the push accessor is `Fatal{UnmigratedPipeInput, "GetBufferBindingSlot@…"}` there (`PipeInputs.h:489-492`). Unreachable today (every backend slot read is DrawIndirect/DispatchIndirect/Parameter/PixelPack; table above), same class as v1 minor 5 (`IsCapabilityEnabledIndexed`): both belong in the same `MEASUREMENTS.md` line for the integrator, because the D.3 rule "a Fatal is fixed by adding the row" is false for these two accessors.

4. **`VerifyFatalOffLogsTheDivergenceAndContinues` never observes the `verify summary` line** — the child `_exit(0)`s (`PipeInputsTest.cpp:446`), so `VerifyState::~VerifyState` (`PipeFill.cpp:348-354`) does not run; the summary path is covered only by my lane run (`summary=1` under `MOBILEGL_PIPE_VERIFY_FATAL=0`). Fine as a unit-level statement of "continues"; note that the teardown line's coverage lives in E's lanes.

5. **`MGPipeVerifyReadHook` re-reads the whole field on every accessor call** (`PipeFill.cpp:430`) — for `GetRenderStateParameters` that is a 1168-byte copy plus a 65-member compare per backend read, and `GetProgramForDraw` re-joins the link per read. Declared (deviation 2) and inside the 5–10× verify budget; recorded here only so the D.3 wall-time numbers are read with this in mind, and because the `InHook` guard (`:429-432`) is what keeps a `GetProgramForDraw`-triggered backend re-entry from recursing — worth a comment at the site.

## 4. Where the tree and the brief disagree (verified; the implementer's list stands)

Unchanged from v1: `GL_Drawing.cpp:906` vs 907; `RenderStateParameters` 65 members not 67; `CapabilityInput` 35 not 37; c5 regenerates 1 file not 7; `integration-gpu` 868 entries; the `ctest -N` regex needs the padded form. Nothing new this round.

## 5. Notes for the integrator (not A's findings)

- The v1 note on the vacuous `-R 'DirectGLES\.Verify\..*Mipmap'` CI regex still stands; the implementer's record of it in `core-v2.md` is accurate.
- Once M1 is fixed, D.3 gains a new failure mode worth expecting: any backend read reached from EGL/init before the first verb will now be `Fatal{UnmigratedPipeInput, "<Field>@<none>"}` in the verify lane — per E's risk table the answer is a fill point at that boundary (an EGL virtual), never a sticky flag or a `FillPoints.def` row.
- `GetBufferBindingSlot(Index)` and `IsCapabilityEnabledIndexed(other cap)` are the two accessors where "add the row" does not apply (minor 3): put both on one `MEASUREMENTS.md` line.

## 6. Verdict

Not approved: fix M1 (predicate emission in `gen_pipe.py` + regenerate, serial-0 assertion in `PipeInputFieldsStartUnfilled`, one fork case for the `@<none>` Fatal), then re-review. Minors 1–3 are records for the deviations list / `MEASUREMENTS.md`; 4–5 are notes.
