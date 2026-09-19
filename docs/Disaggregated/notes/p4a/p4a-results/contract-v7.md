# contract v7 — c0g: D-K2's dependency table, on the client (S-3 / ID-41)

**Commit `29bd4d2c`** on `p4a/c0f`, worktree `~/w7/p4a-c0f`, rebased onto
`refs/heads/feat/disaggregated` = `588b2277` (contract c0..c0f + wire + clientsp v3 + clientfb v2
+ esprytobj v2/v3). One commit, four files, +392/-46. Not pushed.

```
[Fix, Test] (MG_Impl, Pipe): gate the four P4a families on D-K2's dependency bits as well -
Espryt refuses bit 10 without bit 11 or bit 7, but the client had already emitted and the
acceptance-cleared level flags left its legacy arm nothing to upload
```

---

## 1. The defect (S-3), restated from the code

Espryt's four `Resolve<Family>SubsystemArm()` functions (`Managers.cpp:3595` / `:3642` / `:3698` /
`:3735`) REFUSE a family whose D-K2 dependency bit is clear and run the legacy arm instead — the
shape `ResolveVertexInputSubsystemArm`'s bit-8-requires-bit-7 refusal set as the precedent, and the
one ID-15 extended with the fourth row.

That refusal is a **backstop** and it cannot restore a correct picture on its own, because the
client's emission was gated on the operator's mask **alone**: at `0x7ff` (bit 10 set, bit 11 clear)
the client emitted the whole texture family, the applier accepted it, the emitter cleared each
level's per-level dirty flag on that acceptance (D-D5 as amended by ID-18 M3) — and *then* the
server refused bit 10 and ran a legacy path with nothing left to upload. **438/491** on the
DirectGLES lane, the same 47 texture-upload failures ID-39 saw on Magma for the consumer-less
version of the identical mistake (c0f).

This is c0f's rule one signal over, so it is written with c0f's rule: with a dependency unmet the
client emits **NOTHING** for that family and the legacy pull path runs untouched — *nothing at all,
not less*.

---

## 2. The dependency table, as coded

`MobileGL/MG_Impl/Pipe/PipeFill.cpp:950-1055`, immediately under `kMGPipeP4aFamilySubsystems` and
`P4aFamilyHasItsConsumer()`, in the same anonymous namespace, with the D-K2 reasons as comments:

```cpp
struct P4aFamilyDependencyRow {
    Uint64 Family;   // exactly one bit, and it is one of kMGPipeP4aFamilySubsystems
    Uint64 Requires; // the bits MOBILEGL_PIPE_PUSH must ALSO carry for it to be live
};

inline constexpr P4aFamilyDependencyRow kMGPipeP4aFamilyDependencies[] = {
    {kMGPipeSubsystemFramebuffer,      kMGPipeSubsystemTextureResources},
    {kMGPipeSubsystemTextureResources, kMGPipeSubsystemResources | kMGPipeSubsystemSamplers},
    {kMGPipeSubsystemSamplers,         kMGPipeSubsystemTextureResources},
    {kMGPipeSubsystemPrograms,         0},
};
```

| family | requires | why (the comment on the row) |
|---|---|---|
| bit 9 framebuffer | bit 10 | every `MGPSurface::Res` names a Texture or Renderbuffer handle and only bit 10 populates those two slot tables (`ResolveFramebufferSubsystemArm`) |
| bit 10 textures | bit 7 **and** bit 11 | a buffer texture's `MGPResourceDesc::BufferForTexBuffer` names a Buffer handle and only bit 7 puts twins in the resource slot table (D-D1); and `MGPTextureParams::BuiltinSampler` is a `SamplerCso` HANDLE, only bit 11 mints sampler CSOs, and the applier's verdict for a null one is `Fatal{ProtocolCorruption}` (D-K2's FOURTH row, ID-14/ID-15) |
| bit 11 samplers | bit 10 | every `MGPBoundView::Texture` and `MGPImageView::Res` names a Texture handle and only bit 10 populates that slot table (`ResolveSamplerSubsystemArm`); with the row above this is SYMMETRIC — bits 10 and 11 are one arm with two switches |
| bit 12 programs | — | a `ShaderCso` handle names no texture and no buffer. The empty row is written out so the table covers the four families exhaustively rather than by absence |

**The mirror pairs that stay fine**, stated in the code beside the table (Managers.cpp:2377-2381's
own argument: an unreachable branch that says something different is how the reachable one drifts):

- bit 10 set, bit 9 clear — FINE; the legacy FBO sync reaches the texture twin through
  `SyncTextureObjectToBackend`, which dispatches to the handle arm by itself;
- bit 11 set, bit 9 clear — FINE, for the same reason: a sampler view names a texture, never a
  framebuffer;
- bit 7 set, bit 10 clear — FINE, and it is P3a's shipped configuration;
- bit 12 with any or none of 9/10/11 — FINE, per the last row;
- bit 10 set, bit 11 clear (and its mirror) — **NOT** fine; it is the row above, and it is the one
  sentence in the brief that P4a as built withdrew.

**The rows are NON-TRANSITIVE, on purpose.** Espryt's resolvers classify their arms from
`MOBILEGL_PIPE_PUSH` alone, so the table asks the question they ask — *is the dependency BIT set* —
and never *is the other family live*. At `0x7ff` that means the framebuffer family stays LIVE on
both sides while the texture family is dead on both sides, which is the state the two must agree
on; a client that withheld more than the server refuses would leave the server's handle arm live
with no records to read, and a client that withheld less is the defect above. The `0x7ff` arm below
confirms the choice empirically (485/491, no framebuffer-shaped fallout).

**Five `static_assert`s pin the table** rather than leaving it to a reader: every P4a family has a
row (coverage `==` `kMGPipeP4aFamilySubsystems`), each of the four rows' required mask is spelled
out again by name, no row may name its own family, and `kMGPipeSubsystemsMigratedAtP4a` (0x1fff)
must satisfy every dependency the table declares — so the shipped arm cannot be narrowed by any of
this, and the table can only ever narrow a hand-picked A/B mask.

The predicate consulted at the four sites:

```cpp
Bool P4aFamilyDependenciesAreSet(Uint64 subsystem, Uint64 pushMask) {
    const Uint64 required = P4aFamilyDependencyBits(subsystem);   // the table, folded
    return (pushMask & required) == required;
}
```

The mask is a parameter, not a read, so the walk's single read of `MG_Config::Features.PipePush`
stays the one read a whole validate point resolves against.

---

## 3. The sites — the same four c0f joined

| file:line | site | what |
|---|---|---|
| `PipeFill.cpp:1068` | `FamilyIsLive(subsystem, wired)` | the fourth conjunct. This is the predicate every P4a birth hook resolves through (nine hooks, :1253-1304) |
| `PipeFill.cpp:2399` | `wants()` in the walk | a sixth condition beside c0f's fifth; the seven walk-side emissions are inert |
| `PipeFill.cpp:2474` | the `DrainTextureSubData` gate | the one with no dirty bit over it — **the path whose acceptance clears the level flags**, i.e. where S-3 actually bit |
| `PipeFill.cpp:2555` | the residual fill's `supplied` conjunction | "supplied" means a call went out carrying this field; at a mask with a dependency clear none did, so withholding the pull would leave the field unfilled at the verb that reads it |
| `PipeFill.h:46-71` | `MGPipeP4aFamilyEmits(subsystem, wired)` | the exported observable's contract, widened from three conjuncts to four; it still returns `FamilyIsLive` itself (`PipeFill.cpp:1232`), never a second copy |

Bits 7 and 8's existing rules are untouched — read only.

---

## 4. Tests (`MG_Test/Pipe/TextureEmitTest.cpp`, beside the c0f case)

| test | what it pins |
|---|---|
| `TextureEmit.WithTheSamplerBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier` | the **0x7ff shape** end to end: bit 10 set, bit 11 clear → gate false; the handle is still minted (the mint stays unconditional) but not published; `CreateCount`/`RespecifyCount` 0, `TextureResources` empty, `DrainListSize` 0, `SubDataCount` 0 after an explicit drain, **`IsStorageDirty` still true**, and `RefusedNoConsumer == 0` (the *gate*, not c0f's belt, is what stopped it). Second half: with bit 11 back, the same sequence lands and the flag clears |
| `TextureEmit.WithTheBufferResourceBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier` | the D-D1 half of the same row: bit 10 set, bit 7 clear, identical assertions |
| `TextureEmit.EveryDKTwoDependencyRowGatesItsOwnFamilyAndTheMirrorPairsStayLive` | the table itself, one row per family, through `MGPipeP4aFamilyEmits`: all four live when every dependency is met; bit 9 and bit 11 both false without bit 10; bit 10 false without bit 11 **while bit 9 stays true at that same mask** (the non-transitivity, asserted rather than assumed); bit 10 false without bit 7 while bit 11 stays true; bit 12 live entirely on its own; the mirror pairs (10+11 without 9 = the 0xdff arm; bit 7 alone) ; and `kMGPipeSubsystemVertexInput` still true with bit 7 clear — P2/P3a's rules are not narrowed |

All three names were added to the pull-build skip list `MGL_TEXTURE_EMIT_CLIENT_TEST_LIST` at the
top of the file, and **both builds compile** (ID-40's lesson): `ctest -N` gives 2732 names in
`build-linux` and 2732 in `build-push`, diff 0.

**Two fixtures had to arm bit 7**, and this is the one thing outside `PipeFill.*` the commit
touches. `TextureScope` (`TextureEmitTest.cpp`) armed bits 10|11 and `FramebufferScope`
(`FramebufferEmitTest.cpp`) armed bits 9|10|11 — both were arming a mask D-K2 forbids, which the
server would have refused and which the client now refuses too, so without bit 7 every case in both
suites would have gone green for the wrong reason (an emitter that emits nothing). Both now arm
`kMGPipeSubsystemResources` as well, with the row quoted in the comment. Neither file constructs a
`BufferObject`, so bit 7 changes nothing else in either suite (both suites: 37/37 and 22/22).

### Mutations (run, not committed)

| # | mutation | result |
|---|---|---|
| M1 | `FamilyIsLive` drops `&& P4aFamilyDependenciesAreSet(subsystem, pushMask)` | `TextureEmit` **3 failed of 37** — exactly the three new cases (`WithTheSamplerBitClear…`, `WithTheBufferResourceBitClear…`, `EveryDKTwoDependencyRow…`). Reverted: 37/37 |
| M2 | the texture row drops `kMGPipeSubsystemResources` (bit 7) | **does not compile**: `PipeFill.cpp:1020: static assertion failed … P4aFamilyDependencyBits(kMGPipeSubsystemTextureResources) == (kMGPipeSubsystemResources \| kMGPipeSubsystemSamplers)`. The row-by-row `static_assert`s are load-bearing, not decoration. Reverted, build rc 0 |

---

## 5. Gate numbers (all on `~/w7/p4a-c0f` @ `29bd4d2c`)

| gate | result |
|---|---|
| builds | `build-linux` rc 0, `build-push` rc 0 (**both compile** — ID-40's lesson) |
| **G1 `symbol_report --threshold 0`** vs `~/w7/p4a-before-libMobileGL.so` | **0 added / 0 removed / 0 resized / 0 renamed**; `.text 10806323 -> 10806323 (+0, +0.000%)`; `.data`/`.bss`/`.rodata` all +0; 27811 -> 27811 defined symbols |
| **G5 `scripts/p3a_untouched_regions.sh 37da3c3a HEAD`** | **rc 0** — "the 11 pool / deferred-release / ring / flush-drain functions are byte-identical between 37da3c3a and HEAD" |
| `scripts/gen_pipe.py --check` | rc 0 — inventory 477 rows, 0 UNMAPPED, "generated files are up to date" |
| `scripts/gen_pipe_dirty_surface.py --check` | rc 0 |
| `scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — 27 negative controls, all tripped |
| `scripts/check_include_closure.py --compiler clang++` | rc 0 — 4 probes, 0 skipped, 0 problems |
| **`ctest -L unit` build-linux** | **100%, 0 failed of 1766** |
| **`ctest -L unit` build-push** | **100%, 0 failed of 1766** |
| **G2 `ctest -N` pull vs push** | pull 2732, push 2732, **diff 0 lines** |
| G14 vs `~/w7/p4a-before-ctest-names.txt` | 0 removed, 144 added (+3 over the tree this branched from) |

---

## 6. The arms

Every DirectGLES arm is `ctest --test-dir build-push -L integration-gpu -R DirectGLES -j 8` with
`MOBILEGL_PIPE_PUSH` set as shown.

| arm | before c0g | **after c0g** | classification |
|---|---|---|---|
| `0x7ff` (bit 10 set, bit 11 clear — S-3's own mask) | **438/491** | **485/491** | the six pinned `.Handles` aborts only |
| `0x1fff` (the P4a default) | 485/491 | **485/491** | the same six |
| `0x3ff` (bit 9 set, 10/11/12 clear) | — | **485/491** | the same six |
| `0x5ff` (bit 10 set, 9/11 clear — ID-15's arm) | — | **485/491** | the same six |
| `0x9ff` (bit 11 set, bit 10 clear — G12's pair) | — | **485/491** | the same six |
| `0xdff` (bits 10+11 set, bit 9 clear — the mirror pair) | — | **485/491** | the same six |
| **DirectVulkan**, default mask | 475/475 | **475/475** | **0 failed** |

The failing set is **byte-for-byte identical across all six DirectGLES arms** (same six names, same
md5 of the sorted list, count 6 every time):

```
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.DestroyedVertexArraysReturnTheirVertexElementsSlots
```

all `Subprocess aborted`, all `Fatal{PipeLegacyMemosDisabled}` — the **0x1ff itest pins, package
F's**, closed by F's pin move. **Nothing else fails at any arm**, on either backend.

Note the two DirectGLES sampler-seam failures of ID-39 are gone at every arm here: that is D's S-1
fix, already on `588b2277`, not c0g.

---

## 7. What the other packages must know

- **The client and the server now say the same thing about a mask.** Espryt's four
  `Resolve<Family>SubsystemArm()` functions are unchanged and remain the backstop; the client's
  table is their mirror, bit for bit and non-transitively. If a row is ever added or changed on one
  side it must be changed on the other, and `PipeFill.cpp`'s `static_assert`s name each row so the
  change cannot be silent.
- **F**: the `0x5ff` dependency-refusal scenario still asserts the refusal *line* and the legacy
  arm running, and that is still true; what changes is that the surrounding lane at that mask is
  now green too (485/491, the pins only) — esprytobj-v3's "read F-v2-m2 as 'the scenario', not
  'the lane'" caveat is discharged. The suggested arms for F's dependency-refusal coverage are
  `0x5ff` / `0x9ff` / `0x7ff` / `0xdff`, all measured above.
- **B / C**: no emit header was touched. A suite that arms a hand-picked `MOBILEGL_PIPE_PUSH`
  must now arm a mask D-K2 permits — `TextureScope` and `FramebufferScope` were both fixed here,
  and any new emit suite that arms bit 10 needs bits 7 and 11 with it.
- **D / E**: `RefusedNoConsumer` still reads 0 on Espryt and the dependency gate never touches it
  — a dependency is refused by the client's gate, so no record reaches the belt at all.
- **The shipped arm is unchanged.** `kMGPipeSubsystemsMigratedAtP4a` = 0x1fff carries every
  dependency the table declares (a `static_assert`), so the default mask, the pull build and every
  P2/P3a bit behave exactly as they did at `588b2277`.
