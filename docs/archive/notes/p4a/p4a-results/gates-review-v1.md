# P4a package F — `gates`. Adversarial review, v1

Reviewed: `refs/heads/p4a/gates` `0bc9aafa..303a9dc8` (4 commits) in
`/home/swung/w7/p4a-gates`, against `BRIEF-P4A.md` (A.2 G5/G7/G8/G8b/G9/G10–G12, B's D-I/D-K/D-L/D-N,
C.5, C.7, D.2, D.3, E), `INTEGRATOR-DECISIONS.md` ID-1..11, `contract-v1.md`,
`contract-review-v1.md`, `contract-v2.md`, `wire-v1.md` and the package report `gates-v1.md` with
its four D.2 artefacts.

Experiments were run in a private detached worktree `/home/swung/w7/p4a-review-gates` @ `303a9dc8`
(no build; git + python + bash only). Nothing in `p4a-gates` was written to, and the private
worktree has been removed. §3's tail-boundary probe is reproducible in a fresh one:
`git worktree add --detach <dir> 303a9dc8`, insert a comment before the last statement of
`RecomputeBackendColorSlots`, inside `namespace DepthStencilSamplingReadImpl`'s closing brace and
before `ShouldUseCaveatTextureFormat`'s `return`, commit, then
`bash scripts/p4a_untouched_regions.sh 303a9dc8 HEAD`.

---

## Verdict

**ACCEPT WITH MAJORS — rework required before integration.**

The G5 gate is the strongest thing in the package: I attacked it four ways and it survived all four
(§3). The scenario work is honest about what it cannot assert and its skips are real skips. But:

* **G7 cannot report success at all.** Two independent defects each make exit 0 unreachable, and a
  third makes its central safety claim (`repair()` on every path including a mid-way interrupt)
  false. All three are unexercised today because the script exits at step 0 on the contract tree —
  i.e. the package's own green measurement is exactly the path that hides them. **F-M1, F-M2, F-M3.**
* **G8b's composite case reads a counter that c0b redefined**, and `contract-v2.md` §4.3 / §7.6
  assigns F the fix by name. As it stands the case will pass, will not skip, and will be blind to
  the band it exists for. **F-M4.**
* **R2's flip can be forgotten after all**, through a file-layout accident in the conjunction probe.
  **F-M5.**
* **G12's refusal assertion — the one P4a control that is not vacuous today — matches a lowercase
  `"sampler"` anywhere in the whole log**, so it is not clear it can go red for its stated reason.
  **F-M6.**

Six majors, twelve minors, three deviations judged acceptable, two judged under-declared. Nothing
here is an architectural objection: the shapes are right and every fix is local.

---

## 1. Hard-rule spot checks I re-ran

| what | result |
|---|---|
| `p4a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, 17 shas, matching the report's list byte for byte |
| `p4a_untouched_regions.sh --self-test` | **rc 0**, 3 positive + 4 negative controls, each named |
| exit contract: no args / bad `<ref-a>` / bad `<ref-b>` / 3 args / `--self-test extra` | **2 / 2 / 2 / 2 / 2** — correct, nothing degrades to 1 |
| tail-boundary probe (new, §3) | **rc 1**, all three files' regions named |
| working tree clean, no attribution, single-line `[Type] (Scope):` messages | yes |
| only F's files + the one granted `MagmaPipeArms.h` | yes |
| each commit's `CMakeLists.txt` names only scenario sources that exist in that commit | verified for all four |
| D.2 artefacts | present and say what the report says they say: G9's four cases **Passed** on DirectGLES, the DirectVulkan four Skipped; every ObjectSubsystem entry Skipped; the TextureUploadShape lane Passed |

I did not re-measure G1 / G2 / G14 / the 1072-case `integration-gpu` run (they need the package's
build dirs, which are not mine to touch). What I *did* check is the thing the integrator cannot
re-derive from a number: whether the new CI filters actually reach every new entry — §6, they do.

---

## 2. The rulings

### R1 — G9 as a D10 regression net: **partial, and the gap should be written into esprytobj's brief**

The four cases are structurally correct for what they claim
(`/home/swung/w7/p4a-gates/MobileGL/MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp:286`,
`:337`, `:460`, `:506`): each puts a texture through exactly one reachability path, moves a
parameter through the DSA entry points so no setup step ever binds it to a unit
(`:231-238`), runs a frame, and only then samples (`:243-257`). The mechanism F documents for why
they are green today is correct and I re-derived it: the sample is the only observable, and the
sample is also what repairs the state.

**Would they go red if Espryt stopped applying params to an attachment-only texture?** It depends
on the shape of the regression, and only one of the two plausible shapes is caught:

* **Caught.** A P4a path that emits `set_texture_params` for the attachment-only texture and marks
  the frontend params version *synced* while the backend drops the record (no sampler view ⇒ no
  apply) — the observation at `:243` then finds `IsDrawSyncClean` true, skips the sync, and the
  wrong colour comes back. This is the likeliest P4a-introduced shape precisely because P4a is what
  moves the clean-marking, so the net is real.
* **Not caught.** A path that merely *defers* the apply to the first sampler view. The observation
  creates that view, the params are applied at that moment, and the case is green. The same
  self-repair that makes the red unobtainable today makes this regression unobservable.

So the four cases stand as a regression net, but they are not a complete one, and the ruling's
phrasing ("red if Espryt stopped applying params to an attachment-only texture") is only half
satisfied. **Recommendation for the esprytobj verification round: specify the backend-side probe to
take its reading while the texture is still attachment-only *and* to distinguish "not applied yet"
from "applied late"** — otherwise the genuine red will reproduce the same blind spot at a lower
level. Nothing about this is package F's to fix.

Two further R1 notes. The scenario deliberately takes **no lane** and runs in the ambient
registrations at the build default mask — correct, and it only works because the contract moved the
push default to `0x1fff` (`contract-review-v1.md` D10, verified there). And the DirectVulkan four
skip in `SetUp` naming the backend (`:162`), which the D.2 log confirms.

### R2 — the flip is one `if`, but it can be forgotten. **Verify: PARTIAL**

**The one `if` is real.** `ObjectAbaControlIsWiredHere()`
(`.../Scenarios/HandleRecycleScenario.cpp:257-266`) is
`MGITEST_HANDLE_ABA_OBJECTS_<backend> AND ThisBackendsObjectRekeyHasLanded()`, both build markers
from `CONFIGURE_DEPENDS` content probes
(`.../MG_IntegrationTest/CMakeLists.txt:531-563`). No test source has to be edited: the moment a
backend source reads `Features.PipeHandleAbaControl` over a P4a slot table, the probe finds it and
all six cases flip. `ExpectPixelsFor` gates on `arm == Arm::AbaControl && armExpectsCorruption`
(`:341`), so the flip cannot leak onto the Handles or Legacy arms; the framebuffer and renderbuffer
cases re-check `m_arm == Arm::AbaControl` explicitly (`:1234`, `:1489`). The lane that can carry the
flip exists and pins the shipping mask: `DirectVulkan.HandleRecycle.AbaControlHandles.`
(`CMakeLists.txt:1067-1070`, `0x1fff` + the knob). I confirmed the probe does **not** arm today —
`MagmaPipeArms.h` names `PipeHandleAbaControl` (`:222`, `:227`) but no P4a subsystem constant, and
no DirectGLES source names either.

**But it can be forgotten — F-M5.** The conjunction requires both regexes **in one file**
(`CMakeLists.txt:425-446`). If package D wires the knob in `Managers.cpp` while the P4a subsystem
constants live in `SlotTables.h`, the marker never arms, the six cases keep printing `wired=0` and
asserting the correct pixels, and **nothing fails, warns, or records that the expected flip did not
happen**. `RecordProperty("aba_control_wired", 0)` (`HandleRecycleScenario.cpp:500`) is recorded,
never checked; there is no TODO the gate itself checks.

The fix is cheap and strictly safer: make the conjunction **directory-wide** (regex A anywhere under
the backend directory AND regex B anywhere under it), keeping "one source" only in the message.
Today that is still false for both backends — DirectVulkan has the knob and no P4a constant,
DirectGLES has neither — so it costs nothing now and removes the layout dependency later.

`MagmaPipeAbaControlCoversKind` (`MagmaPipeArms.h:237`) does **not** discharge R2: nothing calls it
(F-m5).

---

## 3. G5 — `scripts/p4a_untouched_regions.sh`

**The region set is exactly D-N's, nothing missing and nothing extra.** 17 rows (`:116-133`) =
P3a's eleven in `Managers.cpp` + P4a's six across three files; `EXPECTED_FUNCTION_COUNT=17` is
asserted (`:135`, `:541`) and `SELF_TEST_FUNCTIONS` (`:159`) is exactly D-N's four, with the count
pinned (`:609-612`).

**I attacked the extraction four ways.** Three found nothing; one found a hole in the *self-test*,
not in the code:

1. **Tail boundary.** The self-test's `perturb` inserts at the very first byte after the opening
   brace (`:383-387`), so every negative control would still trip even if `find_definition` returned
   an extent that stopped one character in. Nothing in the self-test proves a region reaches its
   closing brace. **Refuted empirically for the current tree:** I committed a comment immediately
   before the last statement of `RecomputeBackendColorSlots` (`Managers.cpp`), inside
   `namespace DepthStencilSamplingReadImpl`'s closing brace (`DirectGLES.cpp:8578`) and before
   `ShouldUseCaveatTextureFormat`'s `return` (`Utils.cpp`), and the gate returned **rc 1** naming
   `RecomputeBackendColorSlots` first and the other two as "also moved". The extents are right,
   including the namespace kind. Recorded as **F-m2** because the *self-test* still cannot catch an
   extent regression.
2. **Duplicated preprocessor arms.** Handled the way the parent handles it: the two spellings of the
   flush ladder are two named rows (`FlushPendingRangesNow` in the `#else`, `FlushPendingRangesFrom`
   in the `#if MOBILEGL_PIPE_PUSH`), each found exactly once. A P4a arm that re-spells one of the
   seventeen *under the same name* is found twice and is exit 2 with a message that names the shape
   (`:345-358`) — the designed finding, not a limitation. Not a defect.
3. **Comments and literals.** `mask()` blanks comments, strings, chars and raw strings
   length-preservingly, and it carries the C++14 digit-separator special case (`:218-224`) that
   would otherwise blank forward to a distant apostrophe. Hashing is from the **original** text
   (`:360-362`), so a comment change is a difference — deliberate and correct for a "byte-identical"
   claim. Not a defect.
4. **Call sites vs definitions.** `find_function` requires the closing paren to be followed (past
   `const`/`noexcept`/`override`/`final`) by `{` (`:264-268`); `RecomputeBackendColorSlots`'s own
   call site at `Managers.cpp:7580` is correctly not matched. A trailing-return-type definition
   would yield zero hits and exit 2 — loud, not silent. Not a defect.

**Deviations D-N/1 (namespace kind) and D-N/2 (pin consulted unconditionally): both correct and
both well argued.** `DepthStencilSamplingReadImpl` really is a namespace
(`DirectGLES.cpp:8117-8578`), the parent's extractor really would find zero and exit 2 forever, and
`find_namespace` (`:279-300`) will not match a *use* of the namespace. The unconditional pin is the
stronger reading of D-N and the two readings agree on this tree (measured: `37fc94ff…`).

### F-m1 (minor) — the one-ref listing is not in the fixed order, so a captured baseline diffs spuriously

`scripts/p4a_untouched_regions.sh:90-91` promises "stdout is always the sha list … in the fixed
order above — so a baseline capture is a plain redirect". It is not, in the one-argument mode:
`extract_baseline` strips the pinned rows from the spec and appends them **last** (`:463-489`),
while `extract_ref` (`:446-452`) emits in spec order.

Measured: `p4a_untouched_regions.sh 37da3c3a` puts `FlushPendingRangesFrom` at row 17;
`p4a_untouched_regions.sh 37da3c3a HEAD` puts it at row 11. A baseline captured the documented way
and later `diff`ed against the two-ref stdout shows **seven** spurious differences.

The gate itself is unaffected (`compare_lists` looks rows up by name), so this is minor — but the
documented capture workflow is the one that breaks. Fix: emit `$out.sha` through the `REGIONS`
order, or `sort` both sides in the doc.

### F-m2 (minor) — no control proves a region's closing boundary

Above. Fix: a fifth control that perturbs immediately *before* the closing brace, or pin each
region's line count beside its sha in the self-test.

### F-m3 (minor, inherited) — the hash starts at the line carrying the name

`:274` — `begin = text.rfind('\n', 0, m.start()) + 1`. A return type, attribute or template header on
an *earlier* line of a multi-line signature is outside the hash. Inherited from the parent verbatim,
so not a P4a regression; worth one line in the script header so the next reader knows the claim's
exact extent.

---

## 4. G7 — `scripts/p4a_descriptor_negative_control.sh`

The design is right (regex not line numbers; RHS replaced not deleted; both controls always run;
exit 2 outranks exit 1; logs under the build dir; `repair()` before the verdict is reported). Three
defects nonetheless make it unusable as written, and none of them is visible on the contract tree,
because there the script exits at step 0 before any of them runs.

### F-M1 (major) — the `borderColorForm` control cannot compile, so its half can never answer

`:233-234` replaces the right-hand side with the literal `0`:

```python
pattern = re.compile(r'(\.' + re.escape(field) + r'\s*=\s*)([^;,\n]+)([;,])')
patched, count = pattern.subn(r'\g<1>0 /* G7 NEGATIVE CONTROL: was \g<2> */\g<3>', text)
```

`borderColorForm` is a **scoped** enum — `MG_Pipe/MGPipeValueTypes.h:456`
`enum class BorderColorForm : Uint8 { … };`, field at `:485` — present in F's own tree, so this was
knowable at c0. `= 0` does not convert. Verified twice: on package C's real header (the assignment
is `canon.borderColorForm = src.borderColorForm;` at
`/home/swung/w7/p4a-clientsp/MobileGL/MG_Impl/Pipe/SamplerEmit.h:127`, in an `inline` function body,
so every including TU fails), and on a standalone `g++ -fsyntax-only` reduction:

```
error: cannot convert 'int' to 'BorderColorForm' in assignment
```

The script then takes the "the patched header did not compile" path (`:252-261`), reports
`could-not-run`, and `WORST=2` — with the summary at `:342-343` telling the reader that exit 2 is
"the expected answer on the P4a contract tree", which on an integrated B+C tree is exactly the wrong
diagnosis.

**Fix (one character each):** replace the RHS with `{}` instead of `0` — `= {}` is valid for the
scoped enum *and* for `MGPSurface::Layered` (`Uint8`, `MG_Pipe/MGPipeTypes.h:501`), so one
replacement string covers both controls. Verified compiling.

### F-M2 (major) — the patcher's stdout is captured as part of the verdict, so exit 0 is unreachable

`run_control` is invoked in a command substitution (`:325` `verdict=$(run_control …)`), and the
patcher's success message goes to the function's **stdout** (`:240`
`print('[p4a-g7] neutralised %d assignment(s) to .%s' …)`), not stderr like every other message in
the file. So on a tree where a control really trips, `$verdict` is

```
[p4a-g7] neutralised 1 assignment(s) to .Layered
tripped
```

and the `case "$verdict" in tripped) ;;` at `:328-332` falls through to `*)`, setting `WORST=1`.
Reproduced in isolation: the two-line verdict "FELL THROUGH TO * -> WORST=1 (false 'did not
answer')". **The script reports "a control did not answer" for a control that answered
perfectly, and can never exit 0.**

**Fix:** redirect that `print` to `sys.stderr` (or `>&2` on the whole `python3` call), or take only
the last line of the verdict.

### F-M3 (major) — `repair()` cannot run from the traps: the state lives in a subshell

`:117-119` declare `PATCHED_HEADER` / `PATCHED_BACKUP`; `:152-154` install `repair` on `EXIT`, `INT`
and `TERM`; `:222` sets `PATCHED_HEADER` **inside `run_control`**, which `:325` runs in a command
substitution. Bash resets a script's traps inside a command-substitution subshell, and the subshell's
assignments never reach the parent — so:

* the subshell has no traps to fire, and
* the parent's traps fire with `PATCHED_HEADER` empty and `repair()` returns 0 immediately.

Measured (`/tmp` reduction, SIGINT to the process group two seconds into a long "build"): the
parent's `repair` printed *"NO-OP (PATCHED empty)"* and the patched file **survived the interrupt**.

The header's own claim at `:57-69` — *"a Ctrl-C during a rebuild cannot leave a hard-wired field in
the tree … repair() runs from the EXIT trap as well"* — is therefore false, and it is false for
exactly the `p3a-g7 m3` reason it cites. The handled paths are all covered by the explicit
`repair` calls (`:244`, `:258`, `:289`), so the exposure is signals and hard errors only — but that
is the case the traps exist for.

**Fix:** stop capturing the verdict through a subshell. Have `run_control` write its verdict to a
variable (`printf -v` or a global) or to a file under `$LOG_DIR`, and call it plainly.

### F-m10 (minor) — a pull build dir reports `did-not-trip`, not `could-not-run`

The emission suites compile their cases only under `MOBILEGL_PIPE_PUSH`
(`MG_Test/Pipe/FramebufferEmitTest.cpp`, `SamplerEmitTest.cpp`), so in a pull build every case is a
visible skip, ctest is green before and after the patch, and `:271` records `did-not-trip` — a
finding *about the suite* that is really a finding about the build directory. The usage text
requires a push build dir (`:54-55`) but nothing verifies it. Fix: check
`MOBILEGL_PIPE_PUSH` in `$BUILD_DIR/CMakeCache.txt`, or require at least one non-skipped case in the
"before" run.

### F-m8 (minor) — the `Layered` control perturbs two mechanisms, not one

On package B's tree the regex matches **three** assignments
(`/home/swung/w7/p4a-clientfb/MobileGL/MG_Impl/Pipe/FramebufferEmit.h:110`, `:119`, `:168`), and
`:168` is inside `MGPipeCopySurfaceForHash` — the **ContentHash** staging copy. Patching it also
removes `Layered` from the framebuffer content hash. The control still trips (`FramebufferEmitTest.
cpp:351-352` compares `surface.Layered` against the frontend attachment and streams
`"MGPSurface::Layered at colour point N"`, so the block-scoped grep at `:272-273` finds the name),
but the script's own claim of a single-field drop (`:21-27`) is inaccurate. Either exclude the hash
copy or say that two mechanisms are perturbed.

### The G7 assumption about B/C's headers — otherwise robust

Checked against the packages' real trees. `grep -qE "\.<field>[[:space:]]*="` matches in both;
both assignments are single-line, `;`-terminated and contain no comma, so the `[^;,\n]+` capture is
safe; the ctest regexes `FramebufferEmit\.` / `SamplerEmit\.` each select only their own suite (9 and
11 cases); and both suites' failure text carries the literal field name inside the gtest failure
block, which is what the awk-block grep needs. **The assumption is robust; only the replacement
value is wrong (F-M1).**

---

## 5. G8 / G8b — `HandleRecycleScenario`

**The four new ABA cases construct real ABA through public GL.** Renderbuffer (`:1420`), sampler
object (`:1524`), sampler view (`:1632`), program (`:1724`): each deletes, re-creates, checks the GL
name actually came back and `GTEST_SKIP`s *"inconclusive, not proven"* otherwise. The observables are
well chosen and each is defended against a false green — the renderbuffer's is the **storage extent**
(4×4 dead under an 8×8 replacement) because a renderbuffer cannot be sampled; the sampler's is the
**border colour** at a constant UV outside `[0,1]` with `CLAMP_TO_BORDER`, which is the parameter with
the fewest other paths to the driver; the sampler view's keeps a live sampler object across the
window so the *texture* half is the only thing that moved, which is what makes it a different
question from the texture case; the program's colour is baked into the shader source, not a uniform,
so a re-set uniform cannot hide the inheritance. All four correctly detach/unbind/`glUseProgram(0)`
before deleting, so the frontend object really dies inside the case.

**The two existing cases are now real controls.** The texture case takes
`ObjectAbaExpectation("texture")` (`:1118`). The framebuffer case adds the dead attachment's red
warm-up clear and reads **both** attachments (`:1214-1247`), which is the right upgrade: "the
replacement's attachment never became green" was half a verdict.

**The seven leak cases sample peak-live inside the round.** `AssertChurnReturnsEverySlot`
(`:630-708`) takes an `observe()` the round must call while the object is alive (`:655-658`), resets
`peakLive` after two warm-ups (`:661`), takes the baseline, runs `kChurn = 48`, and asserts
`liveAfter == liveBefore`, `highWaterAfter == highWaterBefore` and
`peakLive - liveBefore <= maxInFlight` (`:699-717`). The "nothing was ever minted" skip (`:688-697`)
is P4a's real addition and is what stops six of the seven being `0 == 0` greens. Each churn round
varies the object's content where the kind is content-addressed (the sampler's LOD bias, the
program's and the composite's fragment source) — correct, and non-obvious.

**They do run on the DirectVulkan lane.** `DirectVulkan.HandleRecycle.Handles.` exists
(`CMakeLists.txt`, `MGL_ITEST_VULKAN_HANDLE_HANDLES_ENVIRONMENT` at `0x1fff`), the leak cases gate
only on `m_arm == Arm::Handles`, and the D.2 verbose log shows all seven reaching the "nothing
minted" skip there. Once B and C land, the client mints slots regardless of backend and the cases
arm on Magma — which is the P3a C-1 shape and ID-8's requirement. **Satisfied.**

### F-M4 (major) — the composite leak case reads a counter c0b redefined, and c0b assigns F the fix

`Harness/PipeSlotPeek.cpp:50-54` reads `MGPipeSlots().HighWater(Translate(kind))` and
`PipeSlotPeek.h:47-55` states as fact that *"LiveCount counts both and HighWater is one past the
highest slot handed out in either"*. That was c0's semantics. c0b split them
(`contract-v2.md` §4.3): `HighWater(kind)` is now **the ordinary space only**, and the band is
`CompositeHighWater()` / `CompositeLiveCount()` / `CompositeFreeCount()`.

`contract-v2.md` §4.3 and §7 item 6 name the consequence for this package explicitly:

> **Package F, `PipeSlotPeek`**: the six new members G8b asks for want a **seventh**,
> `PeekPipeCompositeSlotHighWater`, and the composite leak case asserts on that one

F was written against c0 and does not have it. **Failure scenario on the integrated tree:**
`EvictedPipelineCompositesReturnTheirShaderCsoSlots` (`.../HandleRecycleScenario.cpp:2060`) churns
composites; the two stage programs it also creates are ordinary `ShaderCso`s, so the "nothing
minted" skip does **not** fire and the case does not skip; `highWaterAfter == highWaterBefore` and
`liveAfter == liveBefore` are then statements about the ordinary space; a slot that never comes back
to the **band** — the double-free-refusal case the band exists to police (`SlotAllocator.cpp:117-119`,
D-H7) — moves neither. The case reports green having never looked at the thing it is for. This is
M5's failure mode reintroduced one level up, and the report's §3.3 note that "this case is one of
[M5's] consumers" is true but not acted on.

**Fix:** add `PeekPipeCompositeSlotHighWater` (and, for the live/peak assertions to mean anything
about the band, `PeekPipeCompositeSlotLiveCount`), point the composite case at them, and correct
`PipeSlotPeek.h:47-55`'s now-false statement.

### F-m5..m6, m12 (minor)

* **F-m4 (the skip reason is false).** `:636` tells the reader *"The Legacy and AbaControl lanes
  run `MOBILEGL_PIPE_PUSH=0`, where there is no allocator to leak from."* Measured against the lane
  table: `DirectGLES/DirectVulkan.HandleRecycle.Legacy.` and
  `DirectVulkan.HandleRecycle.AbaControl.` do set `MOBILEGL_PIPE_PUSH=0`
  (`CMakeLists.txt:1057-1066`), but `DirectVulkan.HandleRecycle.AbaControlHandles.`
  (`:1067-1070`) runs `${MGL_ITEST_HANDLES_ARM_KNOBS}` = `0x1fff` **with a live allocator**, and the
  seven cases decline there anyway. Not a correctness bug (the Handles lanes cover it), but the
  stated reason is false and one lane's coverage is left on the table. Fix the sentence, or gate on
  the mask rather than on the arm.
* **F-m5.** `MagmaPipeAbaControlCoversKind` (`MagmaPipeArms.h:237-258`) has **no caller**. It cannot
  make anything red or green, so it will rot silently; and it uses `default:` where
  `PipeSlotPeek.cpp:28-41` deliberately refuses one for exactly the "a kind added without a decision"
  reason. Either call it (from the DirectVulkan side of the scenario, or from a unit case pinning the
  two covered kinds) or reduce it to the comment it effectively is.
* **F-m6.** `:2083` reads "TWO in flight" beside `/*maxInFlight=*/3u` at `:2087`. 3 is the right
  number (two stage programs + the composite); the prose is stale.
* **F-m12.** The framebuffer ABA corruption assertion is `EXPECT_NE(deadIsNotRed, 0)` (`:1238`),
  which is weaker than the `sawStale` predicate the same case computes and prints at `:1224` (dead
  attachment now *all green*). A garbage dead attachment satisfies the assertion. Use `sawStale`.

---

## 6. The itest CMake, and `test.yml`

**Pins.** All nine C.5 pins raised, each re-read rather than pattern-replaced; the CSO Off lanes
moved `0x80000000000001ff → 0x8000000000001fff` (mask with the phase, control at bit 63). Grepping
the final file, the only surviving `0x7f` env pins are `ResourceSubsystemControl`'s Off lane
(`:1255`) and `LargeArenaAdoption`'s Off lane (`:1323`) — exactly C.5's `:1154`/`:1222` — and the
only surviving `0x1ff` env pin is the **new** `ObjectSubsystemControl` Off lane (`:1382`), which is
the named P4a control. `0x9ff` appears once, in the refusal lane (`:1387`). Correct, and the three
comment blocks were rewritten rather than left stale.

**`mgl_itest_join_environment`** is used for all four new environments, each with
`${MGL_ITEST_COMMON_ENV}` last. **The two-symbol probe** (`:425-446`) is well motivated and its
false-positive risk is real but not live: I confirmed `MagmaPipeArms.h` names the knob but no P4a
subsystem constant, so DirectVulkan does not arm today. Its false-*negative* risk is F-M5 above.

**`RESOURCE_LOCK`.** Not needed and correctly omitted: the three ObjectSubsystem lanes and the
TextureUploadShape lane each have a private `MOBILEGL_LOG_FILE_PATH` **and** a `TEST_FILTER` naming
one case, which is the stronger form of the guarantee the four existing locks (`:769`, `:886`,
`:922`, `:933`) provide.

**No test name removed or renamed**; the two pre-existing HandleRecycle cases keep their names and
every existing lane keeps its `TEST_PREFIX`. The new lanes are registered outside the
`if (MOBILEGL_PIPE_PUSH)` guard, so the pull/push name sets stay identical (G2) — this is the
structural check; the counts (2707 / +119) are the integrator's to re-take on the finished tree.

**`test.yml`.** `BASELINE 44c2b5cf → 37da3c3a` with a justification that is correct and that I
re-verified (`p3a_untouched_regions.sh 37da3c3a HEAD` rc 0; `FlushPendingRangesFrom` still hashes to
its `3e298c9a` pin). Two new `pipe-gates` rows under the same
`feat/disaggregated`-or-`workflow_dispatch` guard as the P3a rows, which stay. The push-build
control step's `-R` alternation reaches **every** new entry: `ObjectSubsystem` selects the three
`DirectGLES.ObjectSubsystemControl.{On,Off,Refused}.` lanes and the two ambient entries;
`TextureUploadShape` selects the lane and the ambient entries; `TextureParamsWithoutASamplerView`
selects the ambient four (the scenario has no lane by design); `HandleRecycle` already selects the
eleven new cases across six lanes. `ResourceSubsystem` genuinely does not match
`ObjectSubsystemControl`. **The TEMPORARY trigger lines are untouched** (verified at the top of the
file).

### F-m9 (minor) — the c0b hardening of the closure gate is not applied

`contract-v2.md` §7 item 6 also tells F that `.github/workflows/test.yml:774` must become
`--compiler clang++ --expect-probes 4`. The line is now at **`:793`** and still reads
`--mode both --compiler clang++-20 --self-test --require-all`.

On the **compiler** half, ID-11 is right and contract-v2 over-states the risk: the job installs
`clang-20` via apt (`:790`), and Debian/Ubuntu's `clang-N` package ships `/usr/bin/clang++-N`, so
`clang++-20` resolves in CI (contract-v2's "compiler not found" evidence was taken on the WSL box,
where it genuinely does not exist). The line is **consistent with what CI has**; no change is
required there and changing it to bare `clang++` would be a small regression in specificity.

The **`--expect-probes 4`** half is still owed: without it a `--probe` typo or a manifest edit makes
the gate run zero probes and exit 0, which is the second half of M6 and the reason the flag was
added. One word, F's file, and it could not have been written at c0 — so it is a rebase item rather
than an omission.

---

## 7. `TextureUploadShape` (recorded, not gated), and the D.2 artefacts

The scenario is well built for a recorded lane: it asserts `emissions > 0` first so the shape
numbers cannot be zeros that mean nothing (`:311-318`), then the server bracket's own arithmetic
(`box + rect == emit`, `jobs >= emit`), then the two-sided agreement only when both sides exist, and
finally the pixels so a recorded shape cannot be the shape of a workload that drew nothing. The
recorded baseline (`emit=6 box=6 rect=0 jobs=6`, 40 scattered rects collapsed to one box per frame
on llvmpipe) is a real number and is the right thing for P3b/P4b to be written against.

### F-m7 (minor) — "the counter is absent" and "the counter read zero" are the same branch

`:331` `if (clientEmissions > 0)`. `CounterOrAbsent` returns a negative for a missing field, so an
absent `ctu=` and a present `ctu=0` both fall into the else branch and print *"no P4a client emitter
has landed on this tree, so this run records the SERVER shape only. That is the expected reading on
the contract tree"* (`:337-346`). Once package B lands, a client emitter that stops emitting reads
exactly like no emitter at all, and the case passes. The report's own re-run list says "once B
lands, the `ctu == emit` assertion is live" — it will **not** be live under that condition. Fix:
branch on `clientEmissions >= 0` for "the counter exists" and require `> 0` once it does.

### The D.2 artefacts — what was red before, and where it is recorded

* `p4a-texparams-before.log` — G9's four cases, **all Passed** on `DirectGLES` (`#2066`–`#2069`),
  the DirectVulkan four Skipped. This is the artefact `ROADMAP.md:20` asks for and it records the
  **absence** of the mandatory red, not the red. R1 accepts that; it must be carried into
  `MEASUREMENTS.md` with §3.4's mechanism, or the next reader will read a green as a pass.
* `p4a-object-subsystem-before.log` — every `ObjectSubsystemControl` entry Skipped (ambient, and all
  three lanes), the `TextureUploadShape` lane Passed. Consistent with the report.
* `p4a-handlerecycle-before.log` / `-verbose.log` — 180/180, the per-kind arms and the leak cases'
  live/high-water numbers, which is the "重键前红" evidence per kind in the only form this tree can
  produce (skips with stated reasons, plus the texture leak case genuinely asserting).

All four are in `~/w7/notes/p4a/p4a-results/` as ID-5 requires.

---

## 8. Hygiene, and deviations judged

**Hygiene: clean.** No attribution lines; four single-line `[Type] (Scope): description` messages;
working tree clean; no trailing whitespace in the new scripts; only F's files plus the granted
`MagmaPipeArms.h`; each commit's CMake source list matches its own tree. One triviality:
`scripts/p4a_*.sh` are `100755` while the sibling `scripts/p3a_untouched_regions.sh` is `100644`
(**F-m11**); both are invoked as `bash <script>`, so it is cosmetic.

| deviation | judgement |
|---|---|
| **F-1** four commits, not three | **Accepted** — ruling R3, and the reasoning is right: `ObjectSubsystemControl` and `TextureUploadShape` belong to neither of C.5's two `[Test]` messages, and the three exact messages are present in order |
| **F-2** the six ABA arms assert correct pixels and say so | **Accepted** under R2, with F-M5 to fix. The alternative really would be a permanently red always-on lane |
| **F-3** G9's red-before could not be produced | **Accepted** under R1; the mechanism is correctly derived. See R1 for the half the regression net does not cover |
| **D-N/1** namespace region kind | **Accepted** — verified necessary and verified correct, including the closing boundary |
| **D-N/2** pin consulted unconditionally | **Accepted** — the stronger reading of D-N, and both readings agree here |
| **F-4** `MagmaPipeAbaControlCoversKind` instead of changing `…DefeatsIdentity` | **Accepted in principle, under-delivered** — see F-m5: nothing calls it |
| **F-5** `build-bench` absent | **Accepted** — nothing in the package touches `MG_Benchmark/` |
| **F-6** `p4a-trees2.log` never said `REBUILT gates` | **Accepted** — the pull `.so` being byte-identical to `$BASE`'s is the check that would have caught a stale tree |
| **(undeclared)** C.5's two `~/w7/notes/tools` rows — `wsl_p4a_gate.sh` and the `p4a_ab*.sh` / `wsl_p4a_bench.sh` / `ab_reduce3.py` family | **Under-declared.** ID-3 reassigns them to the integrator and they exist, timestamped before F's round began. Correct outcome, but the report is silent on two rows of its own file table; a reader checking C.5 against the report finds a hole. Should be one line in `gates-v1.md` §8 |
| **(undeclared)** the c0 → c0b delta | **Under-declared.** The report names M5 as an in-flight dependency (§3.3) but does not say that `contract-v2.md` §4.3/§7.6 assigns F two concrete follow-ups. F-M4 and F-m9 |

---

## 9. Rework list

**Blocking (must land before integration):**

1. **F-M1** `scripts/p4a_descriptor_negative_control.sh:234` — replace the RHS with `{}` rather than
   `0`, so the scoped-enum control compiles. Re-verify on an integrated B+C tree.
2. **F-M2** `:240` — send the patcher's success message to stderr (or take only the last line of
   `$verdict` at `:325`), so a tripped control is not scored as `did-not-trip`.
3. **F-M3** `:325` / `:117-154` — stop running `run_control` in a command substitution; keep the
   patched-header state in the parent so `repair()` on `EXIT`/`INT`/`TERM` is not a no-op. Prove it
   with a SIGINT during a rebuild.
4. **F-M4** `Harness/PipeSlotPeek.{h,cpp}` — add `PeekPipeCompositeSlotHighWater` (and a composite
   live count), point `EvictedPipelineCompositesReturnTheirShaderCsoSlots`
   (`HandleRecycleScenario.cpp:2060`) at them, and correct `PipeSlotPeek.h:47-55`. This is
   `contract-v2.md` §7 item 6, first half.
5. **F-M5** `MG_IntegrationTest/CMakeLists.txt:425-446` — make the ABA conjunction directory-wide so
   arming does not depend on package D's file layout.
6. **F-M6** `ObjectSubsystemControlScenario.cpp:476-482` — tighten the refusal assertion: require one
   **line** of the log to name both bits, at error severity, rather than a lowercase `"sampler"`
   substring anywhere in the file. As written the assertion goes green on any log that happens to
   contain the word, which is most of them once the sampler path logs anything.

**Non-blocking (declared minors are fine to carry, but these are cheap):**

7. **F-m9** `test.yml:793` — add `--expect-probes 4` (keep `clang++-20`, which CI has).
   `contract-v2.md` §7 item 6, second half.
8. **F-m1** one-ref listing order; **F-m2** a closing-boundary control; **F-m3** one line in the
   script header about the hash's start.
9. **F-m4** `HandleRecycleScenario.cpp:636` — fix the false skip reason (and consider gating on the
   mask so `AbaControlHandles` is covered).
10. **F-m5** call `MagmaPipeAbaControlCoversKind` or delete it; **F-m6** "TWO in flight" → three;
    **F-m12** use `sawStale` in the framebuffer corruption assertion.
11. **F-m7** `TextureUploadShapeScenario.cpp:331` — separate "absent" from "zero".
12. **F-m10 (G7)** verify `$BUILD_DIR` is a push build; **F-m8** narrow or document the `Layered`
    triple patch.
13. **F-m11** exec bits; and one line in `gates-v1.md` §8 declaring the two `~/w7/notes/tools` rows
    ID-3 reassigned.

---

## 10. What the integrator must re-run on the finished tree

F's own list in `gates-v1.md` §9 is right and I would keep every row of it. Add:

| what | why |
|---|---|
| `bash scripts/p4a_descriptor_negative_control.sh build-push` **after the F-M1/M2/M3 rework**, on an integrated B+C tree | it has never once reached the "tripped" path. Expect **exit 0** naming `Layered` and `borderColorForm`; anything else, including exit 2 with the "expected on the contract tree" wording, is now a real finding |
| `ctest --test-dir build-verify -R 'EvictedPipelineComposites'` after F-M4, with the composite band's numbers printed | today it will pass while never reading the band |
| grep the configure output for `MGITEST_HANDLE_ABA_OBJECTS_DirectGLES` **and** `MGITEST_HANDLE_REKEY_OBJECTS_DirectGLES` after D lands | F-M5: if the first is absent while the second is present, the six ABA controls silently did not flip. The configure log is the only place this shows |
| `MOBILEGL_PIPE_PUSH=0x9ff ctest -R 'ObjectSubsystemControl'` with the lane log kept | after F-M6, confirm the refusal line really is what turned it green — read the log, do not trust the substring |
| `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` **and** `--self-test` after D and E | unchanged from F's list; this is the gate D and E can break, and it is in good order |
| the pull-build `-L integration-gpu` run (F's list already asks for it) | plus the pull/push name diff, which is the only unverified half of G2 in this review |
| `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all --expect-probes 4` locally is expected to fail on the compiler (the box has no `clang++-20`); run the CI job instead | ID-9 D16 / ID-11; do not "fix" `test.yml:793` to bare `clang++` on the strength of a local failure |
