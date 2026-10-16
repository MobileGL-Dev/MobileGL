# P4a contract — c0d (Tracker.h): the two cross-package under-fires clientsp review v1 found

**Commit `9ea44389e7219f74794eccb8a32586ba9b6f806c`** on `refs/heads/p4a/contract`, parent
`17db7598` (= c0c = the pipe head at the time of writing). Two files, +269 / -6:
`MobileGL/MG_Impl/Pipe/Tracker.h` and `MobileGL/MG_Test/Pipe/TrackerTest.cpp`.

`PipeFill.cpp` is **not** touched: neither fix needed the `wants()` dirty-to-subsystem map to
move (see §1.3 and §2.4 for why that is the answer rather than a shortcut). `DirtySurface.def`
is **not** touched either, and §4 is the evidence that it did not need to be.

---

## 0. What the commit does, in one paragraph

Two shutters in `Tracker.h` could not see the state they exist to guard. `glBindSampler` moves
bit **12**'s generation and nothing else, so bit **13** — the bit `bind_sampler_states` is
emitted off — never fired for a sampler bind. And bits **6/7/8** (plus bit 14's program half)
read `GetCurrentProgram()`, which is null for the whole life of a bound separable program
pipeline, so after the first walk on a fresh context the entire program family latched `0` and
never moved again however the pipeline was restaged. Both are under-fires, which the file's own
rule calls the fatal direction; both are fixed by **widening the shutter**, which is where
over-firing is free. Four cases in `TrackerTest.cpp` pin them.

---

## 1. Fix 1 — `glBindSampler` now reaches `bind_sampler_states` (C-M4)

### 1.1 The defect, re-derived on this tree

`PipeFill.cpp:2262-2263` gates `EmitSamplerStates` on `wants(MGPipeDirty::NewSamplers)` — bit
13. Bit 13's shutter was `Mix(textureParams, ctx.GetSamplingResolutionGeneration())`.

`glBindSampler` is `GL_Sampler.cpp:360 BindSampler_State`, and it makes exactly two state calls:

* `MG_State::pGLContext->NoteTextureUnitTouched((Int)unit)` (`GL_Sampler.cpp:369`) →
  `Core.h:139` → `TextureState.h:78 NoteUnitTouched`, whose `bindingChanged` defaults to `true`,
  so it does `++m_textureBindGeneration` on **every** call — bit 12's counter;
* `textureUnit.SetSamplerObject(...)` (`GL_Sampler.cpp:372` / `:384`) →
  `TextureUnit.cpp:28-39`, which calls `pGLContext->BumpTextureBindGeneration()` — bit 12's
  counter again.

The only two writers of `BumpSamplingResolutionGeneration` in the tree are **parameter** paths
(`SamplerObject.cpp`, `TextureObject.cpp`). So the review's failure scenario holds exactly as
written: `glBindSampler(3, a); draw; glBindSampler(3, b); draw` fires bit 12 twice and bit 13
zero times, and the server's `BoundSamplerStates[3]` keeps naming `a`'s CSO. There is no pulled
twin to rescue it — `bind_sampler_states` has none, unlike the view set — and Espryt mixes
`SamplerStatesSerial` into a per-draw cache key (`DirectGLES.cpp`, the `SamplerViewsSerial` /
`SamplerStatesSerial` mix beside the `set_sampler_views` suppressor note), so the stale set is
also what its memo keys on.

### 1.2 The change

`Tracker.h:515-517` (comment `:496-514`):

```cpp
now[Index(MGPipeDirty::NewSamplers)] = MGPipeMixShutter(
    MGPipeMixShutter(textureParams, ctx.GetSamplingResolutionGeneration()),
    ctx.GetTextureBindGeneration());
```

### 1.3 Why the shutter and not `PipeFill.cpp`'s gate — the choice, stated

The review offered two one-line fixes: mix bit 12's generation into bit 13's shutter, or gate
`EmitSamplerStates` on `wants(NewSamplers) || wants(NewSamplerViews)`. **The shutter is the
correct one**, on the derivation of the three unit sets that the brief's D-G2 table gives:

* A **sampler-state set** entry is "the unit's sampler CSO, or the null handle when the unit has
  no `SamplerObject` — the texture's built-in sampler then applies". So the set is a function of
  *which sampler object is bound at each unit*, plus *which texture is bound there* whenever the
  unit has no sampler object and the built-in sampler is what applies. `GetTextureBindGeneration`
  is precisely the counter over which texture or sampler is bound at which unit: `Core.h:143-146`
  defines it as bumped whenever a texture bind/unbind/delete changes which texture is bound at a
  unit, and `TextureState.h:88-99` records that every texture **and sampler** bind entry point
  routes through the `NoteUnitTouched` that bumps it. It is **an input to the set**, not another
  bit's private business, so mixing it in is a correction to a shutter that was reading too
  little — not a coupling of two bits.
* A **sampler bind** changes the sampler-state set and, correctly, also the view set (mipmap
  completeness depends on the effective sampler), which is why bit 12 was already right. A
  **texture bind** changes both too when the built-in sampler is in play. The asymmetry the
  review noted — bit 12 firing, bit 13 not — was never a design, only a gap.
* The gate variant leaves the shutter itself still wrong, and three other consumers read it:
  `gen_pipe_dirty_surface.py` derives `DirtySurface.def`'s bit answers **from the shutter**
  (§4), the per-bit fire tallies (`m_fires`) are published as a measurement, and
  `MGPipeSubsystemForDirty` makes the push bitmask "a true per-subsystem A/B rather than
  approximately". A bit gated on another bit's shutter measures neither of those honestly.

Cost of the extra fires: bit 13 now also fires on every plain texture bind. That is bounded by
the emitter's own set-hash suppressor — `MGPipeSetHashSuppressorInstance().ShouldEmit(
BindSamplerStates, hash)` in `SamplerEmit.h`'s `EmitSamplerStates` — which `MGPipeTypes.h:614` makes
mandatory for every `kVarTail set_*` for exactly this traffic (MC 26.2 rebinds the same sampler
at every texture-unit switch). An over-fire costs the 192-entry walk and one XXH64, not a wire
record.

---

## 2. Fix 2 — the program family under a separable pipeline (C-M5)

### 2.1 The defect

`Tracker.h`'s old program block opened `const auto& program = ctx.GetCurrentProgram();` and did
everything under `if (program)`. `Core.cpp:609-631 GetProgramForDraw` shows what an application
that drives `glUseProgram(0); glBindProgramPipeline(P)` gets: `m_currentProgram` is null and the
draw program is the pipeline's composite. So bits 6, 7 and 8 all computed `0`, matched the `0`
they latched on the first (`!m_primed`) walk, and never fired again for the pipeline's whole
life. `Tracker.h` contained no reference to `ProgramPipelineObject` at all.

Rendering was not wrong, and the review is right about why: `GetProgramForDraw` is
emitted-and-still-pulled, the residual fill copies it at every verb, and that is exactly why
`DirtySurface.def:319` rules `BindProgramPipelineObject` `kPulledEveryVerb`. What breaks at P4a
is the **handle protocol**: `glUseProgramStages` makes `GetProgramForDraw` build composite B,
`EmitShaderState` is never called, so B gets no `ShaderCso` handle and no `create_shader_state`
while `set_draw_program` keeps naming composite A — a program D and E will see that the handle
protocol never announced. And bit 8 never firing means `set_global_constants` is never sent for
a pipeline draw at all, where the pull rescues nothing.

### 2.2 The change

`Tracker.h:361-421` adds an `else if` arm on the effective program source; the reasoning
paragraph is `:305-344`, and the file's own header summary is updated at `:30-40`.

```cpp
} else if (const auto& pipeline = ctx.GetBoundProgramPipeline(); pipeline) {
    using Pipeline = MG_State::GLState::ProgramPipelineObject;
    static_assert(sizeof(Pipeline::DrawProgramSignature) == 2 * Pipeline::kGraphicsStageCount * sizeof(Uint64), ...);
    static_assert(sizeof(Pipeline::UniformMirrorVersions) == 2 * Pipeline::kGraphicsStageCount * sizeof(Uint64), ...);
    Uint64 stageLinks = static_cast<Uint64>(ctx.GetBoundProgramPipelineName());
    Uint64 stageState = 0;
    Uint64 stageImages = 0;
    for (SizeT stage = 0; stage < Pipeline::kGraphicsStageCount; ++stage) {
        const auto& staged = pipeline->GetStageProgram(static_cast<ShaderStage>(stage));
        if (!staged) continue;
        stageLinks = Mix(Mix(stageLinks, staged->GetLifetimeId()), staged->GetLinkVersion());
        stageState = Mix(Mix(Mix(stageState, staged->GetBackendStateVersion()),
                             Mix(staged->GetUBOContentVersion(), staged->GetBlockBindingVersion())),
                         staged->GetUniformWriteSetVersion());
        stageImages = Mix(stageImages, staged->GetImageUnitVersion());
    }
    shader = stageLinks;
    stageState = Mix(stageLinks, stageState);
    bindings = Mix(stageState, stageImages);
    constants = stageState;
    programImages = Mix(stageLinks, stageImages);
}
```

### 2.3 Why these fields, and why the loop instead of the two helper functions

* **`{lifetimeId, linkVersion}` per graphics stage IS `ComputeDrawProgramSignature()`**
  (`ProgramPipelineObject.h:94-105`), the key the composite cache is keyed on. Bit 6 therefore
  fires **exactly** when `GetProgramForDraw` would hand back a different composite — exactly when
  a new `ShaderCso` handle has to be minted. These are the same non-artefact fields the plain
  program arm reads and the ones `Core.cpp:634-644` calls out as not passing through
  `ProgramObject`'s join gate, so the tracker's "must not force a compile" rule is intact: no
  join, no flatten, no `Link()`.
* **The four per-stage counters are `ComputeUniformMirrorVersions()`'s content**
  (`:123-141`) — `backendStateVersion`, `uboContentVersion`, `blockBindingVersion`,
  `uniformWriteSetVersion`. Under SSO per-program state is written to the **stage** programs and
  only reaches the composite through `RefreshCompositeUniforms`, so those are the counters bits 7
  and 8 have to watch. `GetImageUnitVersion` is read per stage as well, because D-G4 asks bit
  14's shutter to keep reading all three **frontend** counters rather than any server-side epoch.
* **`stageLinks` is mixed into `bindings`, `constants` and `programImages`.** A composite
  *rebuild* hands back a brand-new `ProgramObject` with an empty default uniform block and no
  backend state at all (`SetCachedDrawProgram` clears the mirror versions with it), so a shutter
  watching only the per-stage state counters would let a rebuilt composite inherit the bindings,
  the constants and the image units of the one it replaced.
* **The pipeline NAME is mixed in** because two pipelines can carry the same stage set and each
  caches its own composite object, so the signature alone would let a `glBindProgramPipeline`
  between two such pipelines pass without a fire.
* **The two helper functions are not called, and that is a gate constraint rather than a
  preference.** `gen_pipe_dirty_surface.py`'s `resolve_reader` follows an accessor only through
  `return m_member;`-shaped bodies; both `Compute*` helpers build a **local** array and return
  it, which it cannot place. Calling them made bits 6/7/8/14 `UNRESOLVED`, which cost
  `X(UseProgram, NEW_SHADER)` its writer-taint reason and **broke self-test negative control 6b**
  (`BumpTextureBindGeneration -> NEW_GLOBAL_CONSTANTS` must read UNDER-FIRING, and an unresolved
  shutter turns that verdict into UNDECIDED). Measured, not guessed: with the helper calls,
  `--self-test` printed *"negative control 6b did NOT trip"*. Reading the fields directly keeps
  every one of those bits derivable — which is the only mechanism that can catch the **next**
  under-fire here — and the two `static_asserts` are what tie the spelled-out pairs back to the
  functions they mirror.

### 2.4 The residual hole, recorded rather than hidden

A pipeline **name recycled** by `glDeleteProgramPipelines` + `glGenProgramPipelines` back onto
the same stage programs at the same link versions, with no other program-family change between
the two draws, produces the same shutter value for a different composite object. A
`ProgramPipelineObject` has **no lifetime id and no wire object at all** —
`DirtySurface.def:348-353` says so where it rules `MarkProgramPipelineForDeletion`
`kUnpublishedDestroy` — so there is nothing else in `Tracker.h`'s reach to mix. Closing it needs
a generation counter on the frontend object, i.e. an `MG_State` change, which is out of this
commit's file scope. It is stated in the header comment at `Tracker.h:337-344`.

### 2.5 Bit 14 moved too

`programImages` feeds `NewShaderImages`'s shutter, so the same null-`GetCurrentProgram()` hole
made bit 14's program term constantly 0 under a pipeline. It is fixed by the same arm. This is
the same family as clientsp minor **C-m1** (the emitters keying on `GetProgramForDraw()`), but it
is only the tracker half; C-m1's compute-pipeline half is still C's.

---

## 3. The tests (A-owned, `MG_Test/Pipe/TrackerTest.cpp`)

Four cases, all in the existing `TrackerWalk` fixture, and all four added to
`MGL_TRACKER_TEST_LIST` (`:79-82`) so G2's "the pull and push ctest name sets are identical"
holds — they are visible SKIPs in a pull build.

| case | line | what it drives, and what it pins |
|---|---|---|
| `TrackerWalk.ASamplerBindAloneFiresTheSamplerStateBit` | `:635` | Makes the two calls `BindSampler_State` makes, in its order (`NoteTextureUnitTouched` then `SetSamplerObject`), rebinding unit 3 from sampler 1 to sampler 2 with **no parameter write anywhere**. Asserts `NEW_SAMPLERS` **and** `NEW_SAMPLER_VIEWS` fire, and that the next walk is 0 (the widened shutter does not fire forever). |
| `TrackerWalk.ARestagedProgramPipelineFiresTheProgramBits` | `:662` | Binds a pipeline with no program in use (asserting `GetCurrentProgram() == nullptr` as the premise), reaches steady state, then does what `glUseProgramStages` ends in — one `SetStageProgram`. Asserts bits 6, 7 and 8, then 0. |
| `TrackerWalk.ARelinkOfAStageProgramFiresTheProgramBits` | `:691` | Same setup with a stage already installed, then a real `ProgramObject::Link()` through the entry point `glLinkProgram` drives. The link *fails* (no shaders attached) on purpose: the link-observable versions are bumped in Link()'s **prologue**, before every early-out, which is the invalidation contract. Asserts bits 6, 7, 8, then 0. |
| `TrackerWalk.UseProgramZeroLeavesTheBoundPipelineDrivingTheProgramBits` | `:723` | The shape an application writes: a program in use over a bound pipeline, then `glUseProgram(0)`. Asserts bit 6 fires on the **handover** (the draw's program source changed), that it then settles, and that a subsequent stage change reaches bits 6/7/8 — i.e. the pipeline is the source now. |

All four pass in **all three** build dirs (`build-linux` pull, `build-push`, `build-verify`):
`4/4, 100%`, none skipped in the pull build (they are the `MGL_DECLARE_PULL_SKIP` stubs there and
report as passed).

---

## 4. Item (3): the two UNDECIDED dirty-surface rows stay marked

`MGP_DIRTY_SURFACE_UNDECIDED_LIST` still carries `X(UseProgram, NEW_SHADER)` and
`X(BindVertexArray, NEW_VERTEX_ELEMENTS)`, and **nothing this commit learned makes either
decidable**, because neither mark is about the shutter side:

```
UNDECIDED, no verdict: BindVertexArray <- NEW_VERTEX_ELEMENTS (the write analysis is not complete
  for this mutator: Bind() writes 'Access', which it never declares ...)
UNDECIDED, no verdict: UseProgram <- NEW_SHADER (the write analysis is not complete for this
  mutator: DestroyProgramSlot() writes 'attachedShaders', which it never declares ...)
```

Both reasons are **writer-side taints** — a call resolved by name into a body with an
unplaceable non-`m_` write — and they are properties of the scanner, not of the rows. Widening
bit 6's shutter cannot touch either. The reason strings after this commit are **byte-identical
to the ones c0 left**, `--check` still reports exactly 2 UNDECIDED, and self-test control 21
("the two P4a undecided marks are still needed") and control 18 ("a stale mark") both still trip.
So `DirtySurface.def` is unchanged: **no row moved, and no mark outlived its reason.**

One thing worth recording for whoever revisits this: the marks are one edit away from becoming
*shutter*-side (§2.3) — if a later change makes bit 6 read something `resolve_reader` cannot
place, the `UseProgram` mark silently changes meaning while staying green, and control 6b is what
notices, on a different bit.

---

## 5. Gates — every number

Run in `~/w7/p4a-contract`, `CCACHE_BASEDIR=/home/swung/w7`, `-j 8`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` for every ctest lane. All three build dirs already
existed and were rebuilt incrementally. **0 compiler errors, 0 warnings** attributable to this
commit (`grep -ci warning:` over the whole build log = 0).

| gate | result |
|---|---|
| **(a) G1** `symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **rc 0 — 0 added / 0 removed / 0 resized / 0 renamed.** `.text` 10806323 → 10806323 (+0, +0.000%); `.data` 76840 → 76840; `.bss` 1296872 → 1296872; `.rodata` 1536538 → 1536538; total 17226471 → 17226471; file 19114360 → 19114360 bytes. 27811 → 27811 defined symbols, 27072 → 27072 normalised names, **27072 unchanged**. This is the check that says the fix is in push-only code: `Tracker.h` is inside `#if MOBILEGL_PIPE_PUSH`, so a pull-build symbol moving would mean it was placed wrongly. |
| **(b) unit, pull** `ctest --test-dir build-linux -L unit -j 8` | **rc 0, 100% passed, 0 failed out of 1642** |
| **(b) unit, push** `ctest --test-dir build-push -L unit -j 8` | **rc 0, 100% passed, 0 failed out of 1642** |
| **(b) unit, verify** `ctest --test-dir build-verify -L unit -j 8` | **rc 0, 100% passed, 0 failed out of 1642** |
| the four new cases, each lane | **4/4 passed** in build-linux, build-push and build-verify |
| ctest names pull == push | `diff` empty, **2608 == 2608** |
| names vs `~/w7/p4a-before-ctest-names.txt` (`LC_ALL=C` on both sort **and** `comm`) | **0 removed**, **20 added**, 2588 → 2608. The 20 = c0's 13 + c0b's 1 + c0c's 2 + **c0d's 4**. |
| **(c) G5** `p3a_untouched_regions.sh 37da3c3a HEAD` (re-run on the commit, not the dirty tree) | **rc 0**, the eleven pool / deferred-release / ring / flush-drain functions byte-identical |
| **(d)** `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4` | **rc 0 — 4 probes, 0 skipped, 0 problems, 6 negative controls tripped** |
| **(e)** `gen_pipe_dirty_surface.py --check` | **rc 0** — 79 mutators all mapped, no stale rows, 45 render-state answers derived, **10 other (mutator, bit) answers derived**, **0 COARSE**, **2 UNDECIDED** (both marked, both with their pre-existing writer-taint reason), 26 prose answers |
| `gen_pipe_dirty_surface.py --self-test` | **rc 0**, **27 negative controls, all tripped**; positive controls OK |
| `gen_pipe.py --check` | **rc 0**, generated files up to date; 71 calls, 72 verify payloads, 63 PipeInputs fields, 69 verbs, 477 inventory rows, 0 UNMAPPED |
| `gen_pipe.py --self-test` | **rc 0**, 7 negative controls tripped |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | **rc 0** |

### 5.1 `integration-gpu -R DirectGLES`, before **and** after

Both runs are the full 491-test selection, both under 30 s, so the before side was taken for
real rather than by subset: `git stash push` of the two files → `cmake --build build-push` →
run → `git stash pop` → rebuild → re-ran the push unit lane to confirm the restored binary
(**100%, 0 failed out of 1642**).

| | tests | passed | failed | skipped | wall |
|---|---|---|---|---|---|
| **before** (`17db7598`, same build dir) | 491 | **100%** | **0** | 63 | — |
| **after** (`9ea44389`) | 491 | **100%** | **0** | 63 | 27.4 s (216.85 s·proc) |

The **skipped-test name sets are identical** (`diff` of the two sorted name lists is empty),
which is the check that matters here — a fix that turned a real case into a skip would otherwise
read as "no regression". **No regression.**

---

## 6. What packages B / C / D / E must know — which bit now fires when

**For C (clientsp / sampler-image-program) — the two majors it handed up are closed at the
source, so C's rework must NOT re-fix them:**

1. `NEW_SAMPLERS` (bit 13) now also fires on **any texture or sampler bind at any unit**
   (`GetTextureBindGeneration`), not only on a sampler/texture *parameter* change. So
   `EmitSamplerStates` is now reached for `glBindSampler` — the case C-M4 named — and also for
   plain `glBindTexture` traffic. **C's set-hash suppressor is now load-bearing, not an
   optimisation**: without it MC 26.2's per-batch rebinds become a variable-length record per
   batch. C-m2's version-first skip in front of the CSO acquire is
   worth more after this commit than before it — the extra fires land exactly on that path.
   The `SamplerEmitTest` case that bumps `BumpTextureBindGeneration()` by hand and then calls
   the emitter directly, now documents a path that really is reachable.
2. `NEW_SHADER` / `NEW_SHADER_BINDINGS` / `NEW_GLOBAL_CONSTANTS` (bits 6/7/8) and the program
   term of `NEW_SHADER_IMAGES` (bit 14) now fire under a bound pipeline, on: a
   `glBindProgramPipeline` to a different name; any `glUseProgramStages` that changes a stage; a
   relink of any stage program; a `glUniform*` / `glProgramUniform*` / block-binding write to a
   stage program; and `glUseProgram(0)` handing the draw to the pipeline. So `ProgramEmit.h`'s
   `EmitShaderState` is now reached for composites, and **the composite resolver is reached the
   way `CompositeResolver.h` expects** — which is what makes C-M1 (the `Live`/`Reset` early-return)
   and C-M2 (the never-invalidated `(Cso, Version)` latch) reachable in practice rather than in
   theory. C's rework should assume both are now on the hot path.
3. Nothing C owns needs to change *because of* this commit. `wants()`'s map is untouched, the
   emitter entry points are untouched, and the subsystem assignment (`kMGPipeSubsystemSamplers`,
   `kMGPipeSubsystemPrograms`) is unchanged.

**For B (clientfb / framebuffer-texture):** bit 13 firing on texture binds means the sampler
family's emitters now run on batches where only a texture moved. B's built-in-sampler `Acquire`
through C's cache (ID-14 / ID-15) is therefore exercised more often; the ref-count ruling in
ID-17 is what keeps that safe.

**For D (wire) and E (esprytdraw / esprytobj):** the important consequence is a **negative** one
— a re-composited program pipeline now gets a `ShaderCso` handle and a `create_shader_state`
*before* `set_draw_program` names it, so D's re-keyed twin tables will not see a program the
handle protocol never announced (`MGPipeSlots().FindByLifetimeId(ShaderCso, B)` will hit). E can
rely on `SamplerStatesSerial` advancing on a sampler bind, which is what its per-draw memo at
`DirectGLES.cpp` (the `SamplerViewsSerial` / `SamplerStatesSerial` mix) keys on; before this
commit that serial stood still across a
`glBindSampler`. Fire rates go up for bits 13 and 6/7/8, so the P4a fire-rate measurements
recorded against c0c are not comparable to ones taken after c0d.

**Rebase note:** every package rebasing onto pipe after c0d integrates gets `Tracker.h` at
`9ea44389`. Nobody but A may edit that file; a package that finds itself wanting to is looking at
a contract defect and should say so rather than patch around it.
