# Adversarial review — P2 package B `p2/tracker`, round 2 (`~/w7/p2-tracker`, `9c6a8a25..dc452c02`)

Reviewer's brief: refute correctness / completeness against BRIEF-P2 §A (G1–G14), §B (D1–D20),
§C.1 and §C.5. Everything below was re-derived or re-run by me in `~/w7/p2-tracker`; the tree was
left as found (`git status --porcelain` empty before and after — only `build-*`, `~/w7/rev-tk2/`
and `~/w7/retrace-out/rev-tk2-*` were written; no source file was edited, and the two negative
controls I ran were run **in memory**, never against the file).

**Verdict: NOT APPROVED — 3 majors.**

Round 1's two majors are genuinely fixed and I re-established both fixes. What blocks approval is
that the *root fact* behind round-1 MAJOR 1 — "a publisher named in the mapping does not fire on
every path" — was fixed in two rows and in one gate, and the **same fact is still wrong in two
other rows of the same file (which the new gate is structurally unable to see) and in the shipped
residual-block emitter, where a code comment states the false invariant explicitly**. Plus a third,
independent hole: the one call P2 fully owns (`set_vertex_attrib_defaults`) publishes **nothing** on
a context change, and the `InvalidateAll()` that exists to force it is defeated by a second
suppressor two lines later.

None of the three renders anything wrong *today* (the residual fill still pulls every affected
field, which is why every lane is green). All three are in the direction `ARCHITECTURE.md:502`
names as the dangerous one, all three are load-bearing the moment P3a builds shutters from this
file or the pulls retire, and no gate in the tree can report any of them.

---

## 1. What I re-ran, and what it printed

Every row is my own run, not a quotation from `tracker-v2.md`.

| gate | command | observed |
|---|---|---|
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160 (+0.001%)`. Resized: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — all four the contract's. **Reproduced.** |
| **G2** | `ctest -N` name sets, `'^ +Test +#[0-9]+: '` | `build-linux 2404`, `build-push 2404`, **zero-line diff**. Reproduced. |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <build-linux names>` | empty (0 of 2363 baseline names removed). Reproduced. |
| **G5** | `git show {p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48…0efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha`. Reproduced. |
| **G9** | `gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` |
| **G9** | `… --self-test` | rc 0 — `5 negative controls, all tripped` |
| **G9** | `… --summary` | rc 0 — E's CI step still works |
| **G13** | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (`7 negative-control trip(s), positive control OK`) |
| **G13** | `check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| unit | `ctest -L unit -j 8` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1526` × 3 |
| integration | `ctest --test-dir build-push -L integration-gpu -j 4` | 878 / 878 |
| integration | `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | 878 / 878 (all-pull arm) |
| integration | `ctest --test-dir build-linux -L integration-gpu -j 4` | 878 / 878 (**no flake this time**, first try) |
| **G4** | `ctest --test-dir build-verify -L integration-verify -j 4` | 818 / 818 |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py --lib build-verify/libMobileGL.so -j 4` | rc 0, `passed 79 / 79; failed: []`, lowest SSIM **0.993475** |
| **G4** | corrected log evidence (`<out>/<case>/<backend>/output/mobilegl.log`, arming string `MGPipe: verify armed`) | 79 logs; `Fatal{` **0**; unarmed **0**; `PipeVerifyDiffer`/`PipeResidualDiverged`/`UnmigratedPipeInput` **0** |
| **G10** (desktop half) | `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 … ctest -R DirectGLES.CrossFrameBufferScenario` | 13/13; `resid=8.00 … cso[csom=1 csob=1]` in window 1, `resid=0.00 … csom=0 csob=0` in windows 2-4. Reproduced exactly. |
| G6/G7, G8/G12, G11 | — | not reachable from this tree (packages A / E / device). Correctly not claimed. |

Ownership (C.5), re-checked with `git diff --name-only p2/contract..HEAD`: 26 files, every one B's row
plus `MG_State/GLState/Core.{h,cpp}`, `MG_Test/ScopedPipeVerb.h`, `MG_Test/Pipe/PipeInputsTest.cpp`
(all "nobody"). No `MG_Backend/**`, no `CMakeLists.txt`, no `.github/`, no `MG_IntegrationTest/**`,
no `scripts/gen_pipe.py`, no A-owned `MG_Pipe/*` file. **No ownership violation.** Commit subjects
are single-line `[Type] (Scope): …`; zero attribution lines.

### 1.1 Round 1's two majors — both genuinely fixed

**Round-1 MAJOR 1.** `render_state_publishers()` (`scripts/gen_pipe_dirty_surface.py:161-224`) now
derives the answer instead of accepting it. I dumped its table and compared it setter by setter
against `RenderState.cpp` for all 45: `SetCapability` → `NEW_RENDER_STATE` (the ClipDistance arm at
`:363-383` does `++m_version` only), `SetStencilFunc` → `NEW_RENDER_STATE` (`:641-649`, the pipeline
bump is conditional), the 18 `BumpVersions()` setters → both, `SetPixelStoreParam` → neither,
`SetPolygonOffset` → inherited from `SetPolygonOffsetClamped`. The old wrong answer trips in memory:

```
SetStencilFunc <- NEW_RENDER_STATE|NEW_PIPELINE_STATE
  -> ['UNDER-FIRING answer NEW_PIPELINE_STATE for SetStencilFunc - RenderState.cpp does NOT move
      it on every path ... (derived: NEW_RENDER_STATE)']
```

**Round-1 MAJOR 2.** `MGPAttribValue::ValueClass` is now the class the frontend actually wrote
(`Tracker.h:409-427` + `Core.h:249-271`, `Core.cpp:217-219,237-239,255-257`), all three
`SetCurrentVertexAttribute*` writers are covered (`grep` finds exactly three), and the emitter
verifies the applier reproduced the value and repairs it when it did not
(`PipeFill.cpp:851-866`). `TrackerAttribPayload` pins the flattening on all three classes and on
the per-attribute-ness of the class. The argument that the class need not be mirrored into
`PipeInputs` (`tracker-v2.md` §3.2) is sound: a rebuild is a function of (class, that class's four
words), so two writes leaving the three views identical rebuild identically either way.

Round-1 minors 1, 3, 5, 10, 12, 15 are also fixed as claimed, and 4, 6, 7, 8, 9, 11, 13, 14 are
recorded where the review asked. I verified the five `static_assert`s tying `MGPipeSubsystemForDirty`
to `SubsystemForEmitter` (`PipeFill.cpp:643-661`) and the shipped-emitter fixture that reads the
real singletons (`TrackerTest.cpp:539-620`).

### 1.2 The chunk table and the setters, re-derived again

I re-derived the split rule against `RenderState.cpp` for more than five setters, hunting for a
*pipeline* byte written by a `++m_version`-only setter (which would leave the assembled block
permanently stale) or a *dynamic*-only write by a `BumpVersions()` setter (which would break the G6
⟺). Spot checks: `SetSampleCoverage`→P2 ✓, `SetStencilMask`→D3/D4 ✓, `SetStencilOp`→P3/P4 ✓,
`SetCapability(ScissorTest)`→P6 ✓ while `SetCapability(ClipDistanceN)`→**D7** ✓ (the one trap, and
the boundary table gets it right — `ClipDistanceEnabledMask` is in the dynamic chunk),
`SetScissorBox`→D7 ✓, `SetPolygonMode`→P5 ✓, `SetHint`→D5 ✓, `SetColorMask`→P1 ✓,
`SetMinSampleShadingValue`→P2 ✓, `SetPatchVertices`→P0 ✓. **No violation found**; the partition
itself is a `static_assert` chain in `MGPipeRenderStateSpans.h` and the build is green.

---

## 2. Majors

### MAJOR 1 — the residual value block's shutter is the pipeline version, so a change to 8 of the 35 capabilities it carries never re-arms G10's trip wire; the comment above it asserts the opposite as its justification

`MobileGL/MG_Impl/Pipe/PipeFill.cpp:1096-1098`:

```cpp
if (tracker.FreshlyPrimed() || (dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) != 0) {
    g_residualDue = true;
}
```

`g_residualDue` is set nowhere else (`grep -n g_residualDue PipeFill.cpp` → `917` init, `1097` set,
`1107` clear). And `PipeFill.cpp:892-894` states the reason:

> `// Emitted once per context and again whenever the capability set may have moved,`
> `// which is whenever the pipeline version moved: every SET_CAPABILITY arm calls`
> `// BumpVersions, so that shutter cannot miss one.`

**That sentence is false, and it is false because of exactly the fact this round's MAJOR 1 fix was
built on.** `RenderState::SetCapability`'s `ClipDistance0..7` arm
(`MobileGL/MG_State/GLState/RenderState/RenderState.cpp:363-383`) ends:

```cpp
// Deliberately NOT BumpVersions(): no backend bakes a clip-distance enable ...
++m_version;
```

and `CapabilityInput::ClipDistance0..7` are values 1..8 of the enum
(`MobileGL/MG_Pipe/MGPipeValueTypes.h:170-177`), inside the `CapabilityInputCount` the block loops
over (`PipeFill.cpp:897-903`). So `glEnable(GL_CLIP_DISTANCE0)` changes
`ResidualValueBlock::CapabilityBits` and moves **only** `NEW_RENDER_STATE`; `g_residualDue` is not
set and `set_residual_value_state` is not emitted.

Reproduction (all from `~/w7/p2-tracker`):

```
$ grep -n 'g_residualDue' MobileGL/MG_Impl/Pipe/PipeFill.cpp
917:        Bool g_residualDue = true;
1097:                g_residualDue = true;
1105:            if (g_residualDue && MGPipeFieldMaskHas(mask, MGPipeInputField::IsCapabilityEnabled)) {
1107:                g_residualDue = false;
$ sed -n '893,894p' MobileGL/MG_Impl/Pipe/PipeFill.cpp
            // which is whenever the pipeline version moved: every SET_CAPABILITY arm calls
            // BumpVersions, so that shutter cannot miss one.
$ sed -n '377,381p' MobileGL/MG_State/GLState/RenderState/RenderState.cpp
                    // Deliberately NOT BumpVersions(): no backend bakes a clip-distance enable
                    ...
                    ++m_version;
$ sed -n '170,177p' MobileGL/MG_Pipe/MGPipeValueTypes.h        # ClipDistance0..7 ARE CapabilityInputs
$ python3 scripts/gen_pipe_dirty_surface.py --check            # rc 0 — and its own derivation says
                                                               # SetCapability is NEW_RENDER_STATE only
```

Consequence, and why it is a major rather than a nit:

* **It weakens a named P2 acceptance deliverable.** G10 and D9 specify "the block is emitted once
  per context **and again whenever the capability set changes**", and the whole value of the block
  is that "the day a later call takes a capability over and forgets to carry it, the residual block
  says so on the next draw". For `ClipDistance0..7` it cannot say so at all until some unrelated
  setter happens to bump the pipeline version — the trip wire is disarmed for 8 of 35 capabilities
  for an unbounded window.
* It is the **same defect class the package just fixed one file over**, in shipped emission code
  rather than in a data file, and carrying a comment that argues the opposite. Nothing in
  `tracker-v2.md` §6/§7/§8 declares it.
* No gate can see it: the block is rebuilt from `ctx` at each emission, so a *stale* block never
  produces a false `Fatal{PipeResidualDiverged}` — it simply never runs. G10's evidence
  (`resid=8.00` once, then `0.00`) is exactly as consistent with the hole as without it.

The fix is one clause: also set `g_residualDue` on `NEW_RENDER_STATE` (the coarser, always-true
shutter, the same answer the `.def` derivation now gives `SetCapability`), or key it on the
capability bits themselves.

### MAJOR 2 — two rows of `DirtySurface.def` still violate the file's own stated rule, and the new derivation gate is structurally blind to them: it checks 45 of 73 mutators and accepts an arbitrarily false answer for the other 28

`MobileGL/MG_Pipe/DirtySurface.def:21-24` (the rule this round introduced):

> `// ANSWERS. A row lists EVERY publisher that fires on EVERY path through that mutator,`
> `// and only those; several are joined with '|'. A publisher that fires on some paths but`
> `// not all must not appear, because a shutter built from this file would then UNDER-fire`

**(a) `DirtySurface.def:142` — `X(SetPixelStoreParam, NEW_PIXEL_PACK)`.**
`RenderState::SetPixelStoreParam` (`RenderState.cpp:827-855`) writes **both** halves — eight
`SET_PIXEL_STORE_PARAM(Pack, …)` arms and eight `SET_PIXEL_STORE_PARAM(Unpack, …)` arms — while the
tracker's bit 2 is the **pack half only** (`MG_Impl/Pipe/Tracker.h:275-280`:
`const PixelStoreParameters pack = ctx.GetPixelStoreParameters(false);`). So on the eight `Unpack`
paths — `glPixelStorei(GL_UNPACK_ALIGNMENT, 8)` and its seven siblings — the named publisher does
not fire. The package *knows* this: `PipeFill.cpp:678-682` says "the unpack half has no carrier at
all, so the field keeps being pulled". Under the file's own vocabulary the honest answer is
`kPulledEveryVerb` (or `NEW_PIXEL_PACK|kPulledEveryVerb` if a row may mix, which the checker
currently forbids).

**(b) `DirtySurface.def:154` — `X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS)`. This one is
false on *every* path.** `GLContext::SetNamedTransformFeedbackBinding`
(`MG_State/GLState/Core.cpp:1514-1530`) either calls `point.Bind(buffer)` / `SetRange` /
`ClearRange` on a `BufferState` binding point (index == bound XFB) or writes
`m_transformFeedbackObjects[index].bindings[…]` (index != bound). Neither bumps anything the
`NEW_SO_TARGETS` shutter reads:

* `Tracker.h:261-262`: `now[NewSoTargets] = MGPipeMixShutter(buffers, ctx.GetTransformFeedbackGeneration())`.
* `buffers` is `GetAnyBufferChangeGeneration()`, and the only bump sites are the seven
  `MGP_NOTE_AGGREGATE(BufferChange)` in `BufferState/BufferObject.cpp:47,55,65,80,88,311,376` —
  all buffer *content* mutations, none of them a bind.
* `m_transformFeedbackGeneration` is bumped in exactly one place,
  `MG_State/GLState/Core.h:386` inside `BeginTransformFeedback`.

So `glTransformFeedbackBufferBase/Range` moves **no** dirty bit, and the row claims one that never
fires.

**(c) The gate cannot see either, by construction.** `check_mapping`'s truth half intersects the
claimed answer with `{NEW_RENDER_STATE, NEW_PIPELINE_STATE}` only
(`gen_pipe_dirty_surface.py:283-315`), so every non-render-state answer is accepted verbatim.
Demonstrated in memory (no file was edited):

```
$ python3 -c '<load gen_pipe_dirty_surface.py; mutate the mapping dict; call check_mapping>'
baseline problems: []
SetNamedTransformFeedbackBinding   <- NEW_VERTEX_ATTRIB_DEFAULTS   problems=0 []
SetPixelStoreParam                 <- NEW_SO_TARGETS               problems=0 []
BumpTextureBindGeneration          <- NEW_GLOBAL_CONSTANTS         problems=0 []
SetCurrentVertexAttributeInt       <- NEW_FRAMEBUFFER              problems=0 []
SetStencilFunc                     <- NEW_RENDER_STATE|NEW_PIPELINE_STATE
                                   -> ['UNDER-FIRING answer NEW_PIPELINE_STATE for SetStencilFunc …']
```

i.e. the gate is real for the 45 RenderState rows and a rubber stamp for the other 28 — including
the two rows above, which are wrong today. `--check` prints
`45 render-state answers derived from RenderState.cpp and matching`, which reads as coverage but is
the count of the rows it *can* check.

Why major: `tracker-v2.md` §2 states the round-2 fix as "correct the two rows that named a
publisher which does not always fire" and "a row that claims a publisher the derivation does not
find is UNDER-FIRING". Two rows of the same file still name such a publisher, one of them
unconditionally, and the file is the input D16 hands to P3a to build the narrow shutters from —
`NEW_SO_TARGETS` built from row (b) would miss every transform-feedback buffer binding.
Minimum to clear: correct both rows, and either widen the derived check to the object-class bits
(the shutter expressions are all in `Tracker.h::Update` and are mechanically readable) or state
in the file and in `--check`'s output that only the RenderState family's answers are verified.

### MAJOR 3 — `set_vertex_attrib_defaults` publishes nothing on a context change, and the `InvalidateAll()` that exists to force it is defeated two lines later by the staging mirror

`MGPipeValidateForVerb` invalidates the set-hash suppressor on a fresh context
(`PipeFill.cpp:1044-1047`) for the stated reason "what the server has is no longer what this slot
last emitted". `EmitRenderState` handles the same case explicitly — `freshlyPrimed ?
kAllDynamicChunks` (`PipeFill.cpp:957-959`) — and `EmitPixelPackState` / `EmitPatchState` send the
whole value unconditionally. `EmitVertexAttribDefaults` does neither: after the suppressor it
diffs each attribute against the tracker's staging mirror and returns early when nothing differs
(`PipeFill.cpp:833`, `:846`):

```cpp
for (SizeT i = 0; i < kAttribs; ++i) {
    if (std::memcmp(&resolved[i], &staged[i], sizeof(resolved[i])) == 0) continue;
    …
}
if (header.Count == 0) return 0;
```

On a context change `MGPipeTracker::Reset()` sets `m_stagedAttribs = AttribDefaults{}`
(`Tracker.h:321`), which value-initialises 32 `CurrentVertexAttributeValue`s to the NSDMI defaults
`{0,0,0,1}` (`Core.h:45-47`) — and a fresh `GLContext`'s `m_currentVertexAttributes{}`
(`Core.h:587`) holds exactly those same defaults. So `resolved == staged` for all 32, `header.Count
== 0`, and **no call goes out**. Meanwhile the server-side mirror is *not* reset:
`MGPipeApplierReset()` (`MG_Pipe/PipeApply.cpp:114-119`) clears `RenderStateCsos`,
`BoundRenderStateCso` and `Residual` and nothing else — `gPipeInputs.m_currentVertexAttribute[]`
still holds the **previous** context's attribute defaults.

Reproduction:

```
$ sed -n '821,846p' MobileGL/MG_Impl/Pipe/PipeFill.cpp      # no freshlyPrimed arm; early return
$ sed -n '957,959p'  MobileGL/MG_Impl/Pipe/PipeFill.cpp      # the arm EmitRenderState does have
$ sed -n '321p'      MobileGL/MG_Impl/Pipe/Tracker.h         # m_stagedAttribs = AttribDefaults{};
$ sed -n '45,47p;587p' MobileGL/MG_State/GLState/Core.h      # the same {0,0,0,1} defaults
$ sed -n '114,119p'  MobileGL/MG_Pipe/PipeApply.cpp          # the applier reset does not touch it
```

Consequences:

* The two-level suppression means the deliberate `InvalidateAll()` at `:1046` has **no effect** for
  the only slot P2 wires — the mechanism `SetHashSuppressorTest.InvalidateMakesTheNextSetGoOutWhateverItHashesTo`
  proves in isolation is cancelled in the shipped path. That is precisely a "memo that can serve a
  stale answer", and it is the failure mode the tracker's own comment
  ("the first walk on a fresh context must publish a COMPLETE state rather than an increment",
  `Tracker.h:186-188`) exists to prevent.
* Today the residual fill re-pulls `GetCurrentVertexAttribute` at every `kDraw` verb (the row is
  shape-only), so nothing renders wrong; that is also why 818 verify entries and 79 verify
  retraces are green. It becomes a real stale answer the moment A's applier honours `ValueClass`
  and the pull retires — which is the very hand-off `tracker-v2.md` §7 asks the integrator to do.
* Not declared anywhere in §6/§7/§8.

The fix is the same one line `EmitRenderState` already has: send all 32 when
`tracker.FreshlyPrimed()`, or reset the applier's attribute mirror alongside the staging mirror.

---

## 3. Minors

1. **`TrackerShippedEmitter.APushedAttributeDefaultTheApplierCannotReproduceIsRepaired`
   (`TrackerTest.cpp:604-618`) asserts the *defect*, not the invariant.** It requires
   `MGPipeVertexAttribDefaultRepairCount() == before + 1`, so it goes **red** the day the blocking
   follow-up of `tracker-v2.md` §7 (A teaching `MGPipeApplySetVertexAttribDefaults` to switch on
   `ValueClass`) lands — the exact event §3.3 advertises as "stops repairing by itself with no edit
   here". Nothing in the test or the hand-off says the test must change with it. Assert
   `<= before + 1` plus a positive control on the emission, or name it in the hand-off.
2. **The repair path has unit coverage only.** Across all 79 verify-retrace logs I found **0**
   occurrences of the applier-cannot-reproduce warning — confirming §8.3: no fixture in the corpus
   moves a `glVertexAttrib*` default, so the corpus proves nothing about this call.
3. **`build-verify` was not up to date at `dc452c02` when I arrived**: my first
   `cmake --build build-verify` relinked 55 targets on an otherwise clean tree, and `DirtySurface.def`
   is included by no C++ file (`grep -rn 'DirtySurface.def' --include=*.h --include=*.cpp --include=*.txt`
   → empty), so it cannot explain the rebuild. `tracker-v2.md` §5 presents its verify rows as run at
   `dc452c02`; at least one of them was produced from a binary that was not the tip. Everything is
   green after my rebuild, and my verify retrace's lowest SSIM (**0.993475**) differs from the
   `0.997009` §5.2 reports for the same command — evidence hygiene, not a defect, but the
   integrator should not copy §5.2's verify rows into `MEASUREMENTS.md` unchecked.
4. **The applier / cache fresh-context reset sits inside the render-state subsystem gate.**
   `MGPipeCsoCacheInstance().Reset()` and `MGPipeApplierReset()` are reached only from
   `EmitRenderState` (`PipeFill.cpp:945-950`), which runs only when
   `wants(NewPipelineState) || wants(NewRenderState)` — i.e. only when bit 0 of the bitmask is set.
   A per-subsystem A/B that clears bit 0 and keeps bits 1–3 (which D14 explicitly invites) gives a
   fresh context a never-reset applier while the suppressor *is* invalidated. Latent; worth one
   `if (tracker.FreshlyPrimed())` outside the gate.
5. **The residual block is the one emission still gated by a hand-written subsystem constant.**
   `PipeFill.cpp:1096` tests `(pushMask & kMGPipeSubsystemResidualValues)` directly instead of going
   through `MGPipeSubsystemForDirty`, so round-1 minor 1's "the ONE map" argument
   (`PipeFill.cpp:1035-1038`) has one exception left, and no `static_assert` covers it.
6. **The `MOBILEGL_ASSERT` that replaced round-1 minor 5's assumption is compiled out in the
   shipping build.** `PipeFill.cpp:986-990` asserts "NEW_PIPELINE_STATE fired without
   NEW_RENDER_STATE", which is exactly right, but Release/INFO is the G1/G3 configuration — the
   invariant is checked in debug and verify only. Worth saying so, since §4 minor 5 reads as if the
   assumption is now unconditionally guarded.
7. **`X(SetPatchVertices, …)` changed answer class without a declaration.** D16's own example row
   is `X(SetPatchVertices, kImmediate)`; the file now answers
   `NEW_PATCH_STATE|NEW_RENDER_STATE|NEW_PIPELINE_STATE` and the report still labels the row
   `(immediate)`. The new answer is the better one and the comment explains it — but it is a
   departure from a brief-supplied row and is not in §7's list.
8. **`render_state_publishers()` unions across bodies of the same name.**
   `gen_pipe_dirty_surface.py:186-196` computes publishers per body and `|`s them, so two overloads
   of one setter — one bumping both counters, one bumping only `m_version` — would derive as "both
   always fire", i.e. bless an under-firing row. Latent only: I verified every `Set*` name in
   `RenderState.cpp` has exactly one body (45 names / 45 bodies). An intersection would be the safe
   fold.
9. **A conditional `BumpVersions()` derives as unconditional.** `SetColorMask`
   (`RenderState.cpp:679-688`, `if (changed) BumpVersions();`) and `SetCapability`'s Blend arm
   (`:349`) derive as always-firing. That is correct under "every path *that mutates*", which is
   the reading the file means — but the file's rule says "every path through that mutator", and the
   two readings differ. One sentence in the header would close it.
10. Still outstanding and correctly recorded by B: G9 is not a CI gate until E lands (`test.yml` is
    E's file); G1's admitted resize set has four symbols, not D15's three; D14/D.4.3's T1−T2 no
    longer isolates the tracker because the walk is unconditional; `MGPipeWidenedCounter` cannot see
    a change of exactly 65536; `s_hashForTest` is a live function pointer in production code; the
    header-only `Tracker.h`/`CsoCache.h`/`SetHashSuppressor.h` still want their one `list(APPEND)`
    line at integration.
11. Still outstanding in **§A of the brief itself** (B reproduced all three; so did I): `--ssim` is
    not a `retrace_gate.py` flag, G2/G14's `'^\s+Test #'` grep drops every test numbered under
    1000, and G4's log glob is one directory level short with an arming string
    (`MGPipe verify:`) that appears in no log. The integrator must fix §A and D.3 before running
    the five-part gate.

---

## 4. Deviations checked against §B

Declared and accepted: 1 (header-only, ownership-forced), 2 (a sixth aggregate
`VertexAttribDefault`), 3 (`Core.{h,cpp}` — "nobody" in C.5, all `#if MOBILEGL_PIPE_PUSH`, G1
unaffected — verified), 4 (coarse shutters for bits 5–17), 5 (`Vector` scan in the CSO cache), 6
(`s_hashForTest`), 7 (residual block after the fill), 8 (the one-field derivation probe), 9 (stale
comment references), 10 (`|`-joined answers), 11 (`GLContext`'s push-only class array), 12
(`MGPipeVertexAttribDefaultRepairCount`).

**Undeclared deviations found:** the three majors above (the residual shutter's disagreement with
D9's "again whenever the capability set changes"; two `.def` rows against the file's own rule; the
missing fresh-context republish of `set_vertex_attrib_defaults` against `Tracker.h`'s own
"COMPLETE state" contract) and minor 7.

Checked and clean: no hot-path timer anywhere (`Tracker.h` has none; every tally is behind
`PipeStats::Enabled()`), no stdio under `MG_Backend`/`MG_State`, `PipeCalls.def` untouched, the
walk does not use `GetProgramForDraw` (`Tracker.h:214-216`) so it cannot force a link join, the
C.1-mandated verdict on the 8 statically over-approximated `FillPoints.def` rows is written into
the def with a per-group reason (`FillPoints.def:29-64`), and the CSO cache's collision `memcmp`,
LRU eviction and content-addressing-off arm are each pinned by a test that can fail
(`CsoCacheTest.cpp:117-147`).

---

## 5. Bottom line

The machinery is sound and independently verified: G1, G2, G4 (818 integration-verify entries and
79/79 verify retraces, 0 `Fatal{`, all armed), G5, G9, G10's desktop half, G13 and G14 all
reproduce; three unit lanes are 1526/1526; three integration-gpu arms are 878/878 with no flake;
the chunk-table rule survived another setter-by-setter re-derivation; and round 1's two majors are
properly closed, one of them with a derivation gate that is a genuine improvement.

What blocks approval is that the round-2 fixes stopped at the two artefacts the round-1 review
named, while the underlying fact — "`SetCapability`'s clip-distance arm does not bump the pipeline
version, and a publisher must fire on every path" — is still wrong in the shipped residual-block
shutter (MAJOR 1) and in two more rows of the very file that was turned into a gate, which the gate
cannot check (MAJOR 2); and the one call P2 fully owns silently publishes nothing across a context
change while the invalidation written for that case is cancelled two lines later (MAJOR 3). All
three are invisible to every lane in the tree for the same reason: the residual fill still pulls
the fields. That is exactly the shape of defect this phase exists to stop shipping.
