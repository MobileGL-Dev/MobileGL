# Adversarial review — p1-core v3 (tree `~/w7/p1-core`, branch `p1/core`, HEAD `44ffafb2`)

Verdict: **NOT approved — 1 major, 6 minors.**

Everything the round-2 review asked for is genuinely fixed and I reproduced each fix independently
rather than through the implementer's own tests: the serial-0 window is closed on **both** branches
of the freshness predicate (sticky included), the `PipeInputsTest` log-path race is gone (0/20 red
under `-j 8`, no leftovers), `ArmVerify` keys on all three knobs, and the FATAL=0 child now observes
its teardown summary. All eight G-gates that can be checked in this tree are green, the 83 fill
points are in the right places, and the 63 accessors are signature-identical to `GLContext`'s.

The major is not in the rework. It is a **hole in `FillPoints.def`'s `kReadback` class mask** that
the espryt reviewer raised against package A in round 1, that is still open at HEAD, and that turns
gate G3 red on the integrated tree — the first of the two cross-package items I was asked to settle.
The second (the `FilledGen == 0` window) is genuinely closed.

Tree left exactly as found: `git status` shows only the three untracked build directories, `git diff`
is empty, HEAD is `44ffafb2`. All reviewer artifacts under `~/w7/p1-core-review3/`; scratch inputs
under `scratchpad/wf4/core-review3/` (`gates.sh`, `nc.sh`, `lane.sh`, `probe.cpp`, `probe2.cpp`,
`sig.py`, `mut.sh`, `fixsim.sh`). Two generator mutation experiments ran in `/tmp` copies, never in
the tree, and were deleted afterwards.

---

## 1. What I re-ran (all on `44ffafb2`; all three build dirs were `ninja: no work to do`)

| check | command | observed |
|---|---|---|
| **G1 symbols** | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `.text 10792579 -> 10792579 (+0, +0.000%)`; `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed, 27060 unchanged`; Removed/Added/Resized/Renamed sections all `_none_` |
| **generator** | `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 / rc 0; `63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `generated files are up to date`; 6 negative controls `tripped as expected`, `positive control OK` |
| generated files match their sources | `git status --porcelain` after `--check` | only the three untracked build dirs; no `.inc` drift |
| **ctest -L unit ×3** | `ctest --test-dir {build-linux,build-push,build-verify} -L unit --no-tests=error -j 12` | `100% tests passed, 0 tests failed out of 1482` in each |
| **`ctest -N` name diff** | padded regex `sed -E 's/^ *Test +#[0-9]+: //'`, `comm` vs `~/w7/p1-before-ctest-names.txt` | 2350 names in each of the three dirs; **0 removed**; 16 added (4 `PipeCatalogue.*` + 12 `PipeInputsTest.*`); linux/push/verify name sets byte-identical (`diff` empty) |
| fill points | `grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp` | Drawing 36, Framebuffer 11, Getter 3, Program 1, Query 18, Sync 6, Texture 8 = **83** over **69** distinct verbs; none outside `MG_Impl/GLImpl` |
| every table call has a fill | all `gBackendFunctionsTable.GL.<Entry>` occurrences in `MG_Impl` classified | 89 occurrences; every one that is a *call* has an `MGP_FILL` on its path; the surplus occurrences are null-checks (`BeginOcclusionQuery` ×3, `GetTexImage`, `GetTextureImage`, `EndTransformFeedback`) and captures (`MemoryBarrier` ×2, `MemoryBarrierByRegion`, `DispatchCompute*`, `PatchParameteri`). No missing fill point |
| **negative control A, unit** | `PipeInputsTest.VerifyCorruptKnobNamesTheFieldAtEntry`, plus my own `probe2 corrupt` | see §2.2 — red for the stated reason |
| **negative control B, unit** | `PipeInputsTest.ReadingAnOmittedFieldAbortsNamingTheVerb` / `PoisonOmitKnobArmsTheOmission`, plus my own `probe2 omit` / `omit-other-verb` | see §2.2 — red on that verb only |
| negative control A, lane | `MOBILEGL_PIPE_VERIFY=1 MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters ctest --test-dir build-verify -R 'DirectGLES\..*ClearThenReadPixels'` | **red**: `1586 - DirectGLES.ClearThenReadPixelsScenario.ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear (Subprocess aborted)`; log: `Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear", verb=1, where=entry}` |
| clean lane smoke | same without the knob | 5/5 green, `verify armed`=1, `Fatal{`=0, `verify read of`=0 |
| negative control B, lane | `+ MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit` | 5/5 green, `poison omission armed`=1, `Fatal{`=0 — vacuous on this tree by construction (minor 5) |
| M1(v1) log race, re-stressed | `ctest --test-dir build-verify -R PipeInputsTest -j 8` × 20 | **red = 0/20**; `ls /tmp/mobilegl-pipeinputs-test*` = 0 afterwards; log path is per-pid (`PipeInputsTest.cpp:525-526`) |
| skip visibility | `ctest -R PipeInputsTest` in the three dirs | build-verify 12/12 pass; build-push 4 skipped (verify-only + the poison-only new case); build-linux 12 skipped ("push not compiled in") |
| **M1(v2) fix, independently** | `probe2 prefill` (my own binary, linked against `build-verify`) | `serial=0: fresh fields = 0 of 63 (sticky RecordError fresh=0)`; `serial=5, all stamps 0: fresh fields = 0`; then rc **134** with `Fatal{UnmigratedPipeInput, "GetLineWidth@<none>"}` |
| accessor signature parity | `sig.py`: all 63 accessor declarations in `PipeInputs.h` vs `MG_State/GLState/Core.h` | **63/63 identical** in name, parameters, constness and return type. The nine my regex first reported as DIFF (`IsTransformFeedbackActive`, `GetTextureContextId`, …) are one-line inline definitions in `Core.h` (`:325,326,334,357,429,441,126,130,143`) that the regex skipped; re-read by hand, all match. **No `const&` became a copy**, on the draw path or anywhere else |
| build-configuration hygiene | `ninja -t commands` in the three dirs | build-verify/build-push: **0** `MobileGL/` TUs compiled without `-DMOBILEGL_PIPE_PUSH=1` (so the `#if MOBILEGL_PIPE_PUSH` members added to `MG_Config::Features` cannot produce an ODR/layout split); build-linux: 0 occurrences of either macro and `PipeInputs.cpp`/`PipeFill.cpp` not compiled |
| D3 verbatim | `cat MobileGL/MG_Pipe/PipeInputsSwitch.h` | identical to the brief's D3 text, including the `#else` arm being the only `pGLContext` spelling |
| generator gate on the M1 fix | `mut.sh` m1: delete `if (gen == 0) return false;` from the generated `PipeFilled.inc` in a `/tmp` copy | `gen_pipe.py --check` → **rc 1** (the predicate cannot silently regress) |
| commit hygiene / ownership | `git log`, `git diff --stat 087685d1..HEAD` | 14 commits, `[Type] (Scope): …` single-line subjects, 0 `Co-Authored-By`, nothing pushed; 26 files, every path in A's C.5 rows |

---

## 2. The two cross-package items I was asked to settle

### 2.1 (a) `kReadback` and the transform-feedback flags — **still open. This is the major.**

See §3 (M1). Answer to the question as posed: **no**, `FillPoints.def`'s `kReadback` class does not
carry `IsTransformFeedbackActive` / `IsTransformFeedbackPaused` at HEAD, and **yes**, HEAD's class
table still produces `Fatal{UnmigratedPipeInput, "IsTransformFeedbackActive@ReadPixels"}` — I
reproduced it directly against this tree's `build-verify` library, without the espryt patch.

On the second half of the question ("whether any other verb class is missing a field its verb path
reads"): I found **no second gap**, from three independent angles —

1. the espryt reviewer's full `DirectGLES` `integration-gpu` run under verify on an integrated tree
   (441 entries) produced exactly 19 aborts, all this one Fatal (`espryt-review-v1.md:46`);
2. the magma reviewer's full `DirectVulkan` verify run produced **zero** `UnmigratedPipeInput` and
   zero `@<none>` (`magma-review-v1.md:99`);
3. my own source audit of the helpers that serve more than one verb class against the pull-site
   catalogue §4.2 and §1: `SyncRenderState` (kDraw/kClear/kBlitOrCopy/kProgramOp — all four masks
   carry its four accessors), `SyncNeccessaryTextures` (its five texture accessors are in every
   class that reaches it, kReadback included), `SyncCurrentFBO` /
   `FramebufferImpl::GetReadColorAttachment` (`Managers.cpp:5355`) and
   `IsFixedPointFallbackReadAttachment` (`:5460`) read `GetFramebufferBindingSlot`, which kReadback
   has; `BoundImageUnitFormat` (`Managers.cpp:6230`, `GetImageTextureBinding`) is reached only
   through `ImageUnitFormatsStillMatch` from `SyncCurrentProgram` (`DirectGLES.cpp:2812`), i.e. the
   draw/dispatch closure, and both masks carry the field; `MultiDraw.cpp:45,51,52,87,95` are kDraw
   accessors in kDraw's mask.

   Residual exposure worth naming for D.3: **nobody has yet run the 40-trace retrace under
   `MOBILEGL_PIPE_VERIFY=1`**, and that is the part of G3 that exercises paths the itest lanes do
   not (MC shaders, XFB spans, indirect count). More `FillPoints.def` rows may fall out there; per
   D7 the fix is always "add the row", never a sticky flag.

### 2.2 (b) The `FilledGen == 0` / `serial == 0` window — **closed, on both branches and for sticky fields.**

`MobileGL/MG_Pipe/generated/PipeFilled.inc:316-321` (emitted by `gen_pipe.py:813-818`):

```
inline Bool MGPipeInputFieldIsFresh(const MGPipeFilledState& state, MGPipeInputField field) {
    const SizeT index = static_cast<SizeT>(field);
    const Uint64 gen = state.FilledGen[index];
    if (gen == 0) return false;
    return kMGPipeInputFieldSticky[index] || gen == state.CurrentVerbSerial;
}
```

The `gen == 0` refusal precedes the branch, so it covers the sticky arm too. Verified three ways,
none of them by trusting the delivered tests:

```
$ MOBILEGL_LOG_FILE_PATH=~/w7/p1-core-review3/p2-prefill.log ~/w7/p1-core-review3/probe2 prefill
serial=0: fresh fields = 0 of 63 (sticky RecordError fresh=0)
serial=5, all stamps 0: fresh fields = 0
rc=134
[FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetLineWidth@<none>"}
```

(the second line is the case the v2 fix's wording does not cover explicitly: a *non-zero* serial with
an unstamped sticky field is stale too, so a process that filled before a sticky field was ever
stamped cannot read it as fresh either).

The compare-at-read branch: the hook still arms only inside `MGPipeFillForVerb`
(`PipeFill.cpp:532`), so it is blind before the first fill — but `MGP_INPUT_CHECK` precedes
`MGP_INPUT_VERIFY_READ` in every accessor body (`PipeInputs.h:199-474`) and a verify build always
defines `MOBILEGL_PIPE_POISON` (`PipeInputs.h:21-26`), pinned by
`static_assert(MOBILEGL_PIPE_POISON, ...)` at `PipeFill.cpp:349`. The probe run above sets
`Features.PipeVerify = true` before the read and still aborts on the poison first, with no
`verify armed` and no `PipeVerifyDiffer` in the log — i.e. the window is closed on that branch by
construction, not by luck. In a push build without the poison the window is open by design (both
macros compile out); that is D2's definition, not a defect.

### 2.3 The two negative controls, exercised independently

Driving `MG_Config::Features` exactly as `ConfigLoader.cpp:247-252` does, from my own binary
(`~/w7/p1-core-review3/probe2`, linked against `build-verify`):

```
===== corrupt                       (Features.PipeVerify=1, PipeVerifyCorrupt="GetRenderStateParameters")
rc=134
  MGPipe: verify armed - 63 fields, 69 verbs, fatal=1
  MGPipe: verify corruption armed - GetRenderStateParameters
  MGPipe: Fatal{PipeVerifyDiffer, "GetRenderStateParameters@DrawArrays", verb=1, where=entry}

===== omit                          (Features.PipePoisonOmit="GenerateMipmap:GetActiveTextureUnit")
draw read ok                        <- the preceding DrawArrays read is unaffected
sibling read ok                     <- GetTextureUnitObject, same verb, still fresh
rc=134
  MGPipe: poison omission armed - GetActiveTextureUnit@GenerateMipmap
  MGPipe: Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}

===== omit-other-verb               (same knob, fill BindImageTexture — same kTextureOp class)
green: BindImageTexture (same kTextureOp class) is unaffected      rc=0
```

Both controls turn something red, each for its stated reason, and G5's "on **that** verb and only
there" holds even against a sibling verb of the same class. The lane-level CORRUPT control is red as
well (§1).

---

## 3. Major

### M1 — `FillPoints.def`'s `kReadback` class lacks `IsTransformFeedbackActive` / `IsTransformFeedbackPaused`, which Espryt's `ReadPixels` **and** `GetTexImage` verb paths read; G3 is red on the integrated tree

**Where.** `MobileGL/MG_Pipe/FillPoints.def:225-240` — the `kReadback` block holds 15 rows and
neither flag. The generated mask says so:
`MobileGL/MG_Pipe/generated/PipeFillPoints.inc:290-291`, `// kReadback: 22 fields (15 own + 7 sticky)`,
`{{0x5c50f3a041300541ull, 0x0000000000000000ull}}`.

**The read, at `087685d1` and unchanged on `p1/espryt`:**

```
MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:5259-5260   (ScopedEmulationDrawState::ScopedEmulationDrawState, :5226)
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            !MG_State::pGLContext->IsTransformFeedbackPaused() && g_GLESFuncs.glPauseTransformFeedback) {
```

reached as
`ReadPixels` (`:8999`, the `GLFunctionsTable.ReadPixels` entry)
→ `ReadPixelsDepthComponent` (call `:9066`, definition `:8140`)
→ `DepthStencilSamplingReadImpl::Read` (call `:8128`, definition `:8039`)
→ `ScopedEmulationDrawState emulationState;` (`:8061`)
→ the two reads at `:5259-5260`.

`GetTexImage` (`:9166`) is in the same class and reaches the same helper through its own pack path
(`:9498`, `:9502`), so the identical Fatal is reachable as `IsTransformFeedbackActive@GetTexImage`.
The catalogue's §4.2 row for Espryt's `ReadPixels` lists only the pack/format-conversion helpers and
misses this branch entirely (`scout-pull-sites.md:893`), which is how the row was lost; the
catalogue's own §1 entry for `:5259` attributes it to `ResolveThenBlit` / "blit" only
(`scout-pull-sites.md:188-189`), and `kBlitOrCopy` does carry both flags — which is why only the
readback class is short.

**Reproduction (this tree, `build-verify`, nothing in the tree touched).** A 40-line program
(`scratchpad/wf4/core-review3/probe.cpp`, built with `PipeInputsTest`'s own compile/link flags into
`~/w7/p1-core-review3/probe`) that does what `DirectGLES.cpp:5259` does under `MGB_CTX` on the
`ReadPixels` verb:

```
$ MOBILEGL_LOG_FILE_PATH=~/w7/p1-core-review3/probe.log ~/w7/p1-core-review3/probe
after ReadPixels fill: serial=1 verb=ReadPixels
  fresh(IsTransformFeedbackActive)=0  fresh(IsTransformFeedbackPaused)=0  fresh(GetPixelStoreParameters)=1
Aborted     rc=134
$ cat ~/w7/p1-core-review3/probe.log
[FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "IsTransformFeedbackActive@ReadPixels"}
```

**Corroboration on a real lane** (not mine, but the reason this was raised against core in round 1):
`espryt-review-v1.md:46,50` — `MOBILEGL_PIPE_VERIFY=1 ctest --test-dir build-verify -L integration-gpu -R '^DirectGLES\.' -j 4`
on an integrated tree gives **421/441**, the 19 failures all `DirectGLES.ForcedDepthStencilEmulation.*`
(`MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1`, cases 2274-2293), each aborting on exactly this
Fatal (`~/w7/p1-espryt-review-int-one.log`). I confirmed the row is still absent on `p1/espryt`
(`f4ea84c5`) too, since that branch inherits `FillPoints.def` from `p1/contract`.

**Why this is a major rather than a note.** G3 requires zero `Fatal{UnmigratedPipeInput}` across the
`integration-verify` lane; with this row missing, 19 known entries abort the moment core and espryt
are integrated (D.1 order: core → espryt → magma → verify), i.e. the package's own acceptance gate
cannot pass. D7 designates `FillPoints.def` — package A's file — as the *only* place such a Fatal
may be fixed ("a `Fatal{UnmigratedPipeInput}` found there is fixed by adding the row to this table,
never by marking the field sticky"), so the fix belongs to A, and the lane that is the designated
oracle has already spoken. It was reported before this round's rework began and `core-v3.md`
neither fixes it nor records it (its "Unfinished" section says "Nothing in A's scope").

**In fairness to the implementer:** the brief's own D7 table for `kReadback` has the same hole, and
`FillPoints.def` reproduces the brief's 139 (class, field) rows exactly — the round-2 review's
`d7check.py` verified 139/139, and `FillPoints.def` is untouched since. A followed the specification
faithfully; the specification was derived from a scout reachability trace that missed one branch.
That makes it a *correct* implementation of a *wrong* table, which is still an open defect in A's
deliverable, not somebody else's.

**Fix, verified end to end in a `/tmp` copy** (`scratchpad/wf4/core-review3/fixsim.sh`): two rows in
`FillPoints.def`'s `kReadback` block plus a regenerate. `gen_pipe.py --check` correctly goes rc 1 on
the un-regenerated tree, and the regenerated mask is the only change:

```
-    // kReadback: 22 fields (15 own + 7 sticky)
-    {{0x5c50f3a041300541ull, 0x0000000000000000ull}},
+    // kReadback: 24 fields (17 own + 7 sticky)
+    {{0x5f50f3a041300541ull, 0x0000000000000000ull}},
```

I did **not** apply it (the tree is left as found). After applying, re-run the espryt reviewer's
lane on an integrated tree to confirm 441/441, and re-run my `probe` (it must print
`fresh(IsTransformFeedbackActive)=1` and exit 0).

---

## 4. Minors

1. **A `MOBILEGL_PIPE_VERIFY_CORRUPT` field outside every executed verb's mask arms, logs, and never
   reports** — `MobileGL/MG_Impl/Pipe/PipeFill.cpp:416`
   (`if (g_verify.Corrupt && MGPipeFieldMaskHas(mask, *g_verify.Corrupt))`). Observed:
   `probe2 corrupt-outside-mask` (CORRUPT=`GetPixelStoreParameters`, verb `DrawArrays`) → **rc 0**,
   log carries `verify corruption armed - GetPixelStoreParameters` and nothing else. Negative
   control A is therefore green-by-omission whenever it is pointed at a field no executed verb
   fills; only the unknown-name case is Fatal (D2). The CI step's `GetRenderStateParameters` happens
   to be in kDraw/kClear/kBlitOrCopy/kReadback/kProgramOp, so today it works — but the control
   cannot report its own inapplicability. One counter and a teardown `MGLOG_E` when a corruption was
   armed and applied zero times would make it falsifiable.

2. **The push arm loses the pull arm's DEBUG bounds assertion on two texture accessors.**
   `PipeInputs.h:518-533` (`GetImageTextureBinding`, both overloads) and `:549-556`
   (`GetTextureUnitObject`) index the raw base pointer with no range check, while
   `MG_State/GLState/TextureState/TextureState.cpp:157-172` guards both with
   `MOBILEGL_ASSERT(unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS, ...)`. In an INFO build the assert
   is inert (`Defines.h:113-115`), so G1/D9 are unaffected and behaviour is identical; a DEBUG push
   build silently reads out of bounds where the pull build would have said so. (`GetBufferBindingPoint`'s
   `index` is unchecked on both sides — `PipeInputs.h:506` vs `BufferState.cpp:93` — so it is
   symmetric and not part of this.)

3. **Nine fill points still bump the serial for a null table entry** — declared deviation 1, and I
   re-audited all 83 statements against appendix B.7 rather than trusting the record. The nine are
   `GL_Query.cpp:263,294,306,531,543,575,583` and `GL_Sync.cpp:179` (plus `GL_Drawing.cpp:1358`,
   which is actually exact — it sits inside the `FenceSync && ClientWaitSync` guard). The **dangerous**
   direction is absent everywhere: there is no call that can be reached without its fill. The
   round-1 finding (`GL_Sync.cpp` `DeleteSync` in the orphan sweep) is fixed — `GL_Sync.cpp:233` is
   now inside `if (backendDeleteSync && syncObject->backendHandle)`, matching its `GL_Query.cpp:930`
   sibling. Placement relative to frontend mutation is right in every case that matters
   (`BindImageTexture` after `.Bind`/`NoteTextureUnitTouched`, `PatchParameteri` after
   `SetPatchVertices`, `Pause`/`Resume` after `SetTransformFeedbackPaused`, `BindTransformFeedback`
   after `BindTransformFeedbackObject`, `EndTransformFeedback` *before* the frontend's own
   `EndTransformFeedback()`, `ReadPixels` inside the `ScopedNeutralPackState` scope at
   `GL_Texture.cpp:1078`).

4. **The env-var forms of both knobs are inert in the unit binary.** `MG_ConfigLoader` never runs
   there, so `MOBILEGL_PIPE_VERIFY_CORRUPT` / `MOBILEGL_PIPE_POISON_OMIT` in the environment do
   nothing to `ctest -R PipeInputsTest`; the unit-level controls set `MG_Config::Features` directly
   (the same fields `ConfigLoader.cpp:247-252` writes, through the same parsers). That is the right
   design, but "the negative controls at unit level" should be understood as the two tests, and the
   env→`Features` hop is proven only by the lane runs. Worth one sentence in `MEASUREMENTS.md` so
   nobody later "checks" the controls by exporting the variables around a unit run and reads green.

5. **The lane-level POISON_OMIT control is vacuous in this package** (declared): 5/5 green with only
   `poison omission armed` in the log, because no backend on `p1/core` spells `MGB_CTX->`. Its real
   coverage is the unit fork case plus E's `PoisonOmitted.` lanes on the integrated tree. Combined
   with the v1 note that the brief's CI regex `-R 'DirectGLES\.Verify\..*Mipmap'` matches nothing,
   this control is the one the integrator should watch most closely at D.3.

6. **`core-v3.md`'s "Unfinished: Nothing in A's scope" is not accurate** while M1 stands, and the
   result file's deviation list does not mention the `kReadback` row at all even though the espryt
   review named it as a core item before this round started. Bookkeeping, but it is the kind of
   bookkeeping that lets a known-open item reach D.3 unowned.

---

## 5. Where the tree and the brief disagree (verified; the implementer's list stands)

Unchanged from v1/v2: `GL_Drawing.cpp:906` vs 907; `RenderStateParameters` 65 members not 67;
`CapabilityInput` 35 not 37; c5 regenerates 1 file not 7; `integration-gpu` 868 entries; the
`ctest -N` regex needs the padded form. New this round: the brief's **D7 `kReadback` field list is
itself incomplete** (M1) — when the row is added, `BRIEF-P1.md`'s D7 table and
`scout-pull-sites.md:893` should be corrected in the same pass, or the next reader will "fix" the
`.def` back to the brief.

## 6. Notes for the integrator (not A's findings)

- The magma reviewer's verify lane found a genuine `where=read` divergence,
  `Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@DrawArrays", verb=3, where=read}` on
  eight `DirectVulkan` scenarios (`magma-review-v1.md:99`): Magma reads that counter four times
  inside one draw and the backend's own texture/sampler completion bumps it between the reads. Per
  D.3 that is recorded in `MEASUREMENTS.md` and decided by the plan owner, not silenced — it is the
  compare-at-read arm doing exactly its job, and it is not a core defect.
- Nobody has yet run the 40-trace retrace under `MOBILEGL_PIPE_VERIFY=1`. That is the half of G3
  most likely to surface further `FillPoints.def` rows; budget the "one day of D.3 iteration" E's
  risk table already names.
- `GetBufferBindingSlot(BufferTarget::Index)` and `IsCapabilityEnabledIndexed(cap ∉ {Blend, ScissorTest})`
  remain the two accessors where "add the row" does not apply; the one-line `MEASUREMENTS.md` record
  the implementer drafted is still owed.

## 7. Verdict

**Not approved.** One major: add `X(kReadback, IsTransformFeedbackActive)` and
`X(kReadback, IsTransformFeedbackPaused)` to `MobileGL/MG_Pipe/FillPoints.def`, regenerate, and
re-run the espryt verify lane (or, in this tree, `~/w7/p1-core-review3/probe`, which must then exit
0). Minors 1-2 are small code changes worth taking in the same pass; 3-6 are records.
