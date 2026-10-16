# P4a package A — `c0b`, the contract-fix round. Result, v2

Two commits on `refs/heads/p4a/contract`, on top of `08192d72` (= the tag `refs/tags/p4a/contract`,
**which was NOT moved and still points at `08192d72`**):

| commit | message |
|---|---|
| **`32033d69`** | `[Fix] (Pipe): land the birth half of the P4a client API behind a publication latch both halves read, gate every emitter on its family's wired constant, raise the sampler view's death notice and count the shader composite band apart from the ordinary slots` |
| **`2cb44039`** | `[Fix] (Pipe): stop the include-closure gate going green on a compiler it could not find or on a probe count nobody asked for` |

`git diff --stat 08192d72 HEAD` — 6 files, **+642 / −41**:

```
 MobileGL/MG_Impl/Pipe/PipeFill.cpp          | 368 ++++++++++++++++-
 MobileGL/MG_Impl/Pipe/SlotAllocator.cpp     |  35 ++-
 MobileGL/MG_Impl/Pipe/SlotAllocator.h       |  40 ++-
 MobileGL/MG_Pipe/PipeMutation.h             | 145 +++++++++++
 MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp |  62 +++++
 scripts/check_include_closure.py            |  33 ++-
```

Working tree clean, nothing pushed, no other worktree touched. **No file any in-flight package
owns was edited**: the five emit headers, `PipeApply.*`, `MG_State/**`, `MG_Backend/**`,
`MG_IntegrationTest/**` and `test.yml` are byte-identical to `08192d72`.

---

## 1. Verdict on the hard rules

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 renamed / 0 resized.** `.text` 10806323 → 10806323 (+0). File 19114360 → 19114360. 27811 → 27811 defined symbols. |
| behaviour neutrality in push and verify | preserved — see §5's argument, three separate mechanisms |
| three builds compile (pull / push / verify) | all three |
| `ctest -L unit` ×3 | **1636 / 1636 in each**, 0 failed (1635 at `c0` + the one case §4.4 adds) |
| ctest names pull == push | `diff` empty, 2602 == 2602 |
| no name removed vs `~/w7/p4a-before-ctest-names.txt` | **empty**; 14 added (`c0`'s 13 + `PipeCatalogue.TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace`) |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0 |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4` | rc 0, 4 probes, 0 problems, 6 controls tripped |
| `p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, the eleven byte-identical |
| the tag was not moved | `refs/tags/p4a/contract` = `08192d72`; `refs/heads/p4a/contract` = `2cb44039` |
| two commits, single-line `[Fix] (Pipe)`, no attribution | yes |

---

## 2. Per-finding disposition

### M1 — the birth half, and the death helpers' literal `false`  → **FIXED**, with one declared deviation

**(a) The birth half now exists.** `MG_Pipe/PipeMutation.h:225-358` declares thirteen hooks plus a
three-call publication latch, all inside the file's existing `#if MOBILEGL_PIPE_PUSH`, with the five
frontend classes forward-declared at `:95-106` beside `BufferObject`'s. Bodies are at
`MG_Impl/Pipe/PipeFill.cpp:1027-1114`. The exact declarations are §3.

**(b) The six death helpers read a latch instead of a literal.**
`PipeFill.cpp:1159-1175` adds `HandleOnly(kind, handle)` and
`EmitDeleteIfPublished(kind, handle, apply)`; the five kinds that have a wire delete now call it:

| helper | file:line | wire delete it now emits, when published |
|---|---|---|
| `MGPipeEmitSamplerViewCsoDestroyAndFree` | `PipeFill.cpp:1180-1181` | `MGPipeApplyDeleteSamplerView` |
| `MGPipeEmitTextureDestroyAndFree` | `:1199-1200` | `MGPipeApplyResourceDestroy` |
| `MGPipeEmitRenderbufferDestroyAndFree` | `:1235-1236` | `MGPipeApplyResourceDestroy` |
| `MGPipeEmitSamplerCsoDestroyAndFree` | `:1268-1269` | `MGPipeApplyDeleteSamplerState` |
| `MGPipeEmitShaderCsoDestroyAndFree` | `:1284-1285` | `MGPipeApplyDeleteShaderState` |
| `MGPipeEmitFramebufferDestroyAndFree` | `:1241-1263` | none, and now says **why the literal `false` is right for this kind**: nothing ever takes the latch for a kind with no create and no delete |

**[deviation D17] the latch is A's, not each emitter's `RecordIsPublished(handle)`.** The review's
fix (b) asks for `Bool RecordIsPublished(MGPipeHandle) const` on each of the five emitter classes.
That is an edit to the five emit headers, which **B and C are editing right now** — the one thing
this round may not do. It is also the wrong shape for P4a even without that constraint, and the
three reasons are in `PipeFill.cpp:1139-1146`:

- P3a had **one** kind behind **one** emitter, so the emitter could hold the latch. P4a has **six
  kinds behind four emitters**, so a per-emitter latch means asking a different object per kind and
  three of the four emitters answering for two kinds each.
- `SamplerViewCso` **has no frontend object and no emitter of its own** — it is minted off the
  texture's lifetime id — so "ask its emitter" has no well-defined answer.
- The composite `ShaderCso` has **two independent release paths** (the pipeline cache's LRU
  eviction and the composite `ProgramObject`'s destructor), and the second must be a proven no-op.
  One latch keyed `{kind, slot, gen}`, cleared by the first delete, makes it one.

So: `MGPipeNoteHandlePublished(kind, handle)` / `MGPipeHandleIsPublished(kind, handle)` /
`MGPipeNoteHandleUnpublished(kind, handle)` (`PipeMutation.h:277-279`, `PipeFill.cpp:1027-1040`),
backed by a per-kind dense table plus **the composite band's own table**
(`PipeFill.cpp:958-1024`) — the client-side worked example of must-know item 3. **B and C call
`MGPipeNoteHandlePublished` when a create actually goes out and touch nothing else.**

### M2 — `MGPipeApplyResourceSubData`'s missing region tail → **NOT THIS ROUND**

Assigned to package `wire`'s rework, as instructed: `PipeApply.{h,cpp}` is being edited there now.
Untouched here.

### M3 — `wants()` did not consult `kMGPipeWiredSubsystems` → **FIXED**

`PipeFill.cpp:2184-2191` — the rule as landed is §4.1. The three comments that claimed the
property are now true rather than intended: `PipeFill.cpp:2241-2245` (the P4a segment's "this whole
block is dead"), and the drain gate's "the same pair `wants()` applies to every other emission"
(`:2252-2254`) is now literally the same pair. The `docs/` sentences are the integrator's.

### M4 — `MGPipeEmitSamplerViewCsoDestroyAndFree` skipped the death notice → **FIXED**

`PipeFill.cpp:1182-1193`. It now calls the same `NotifyAndFree(SamplerViewCso, lifetimeId, handle)`
the other five use, and the old reason is replaced by the refutation: the notice is
kind-parameterised, `MGPipeKind` has `SamplerViewCso`, and the helper's own first line already
resolves the view through that kind's `ByLifetimeId` map. "No frontend object of its own" is why it
takes a lifetime id, not a reason to drop step 2.

### M5 — `HighWater(ShaderCso)` made the ordinary leak assertion vacuous → **FIXED**

`SlotAllocator.h:86-119`, `SlotAllocator.cpp:224-254`. The two spaces are reported apart. **API
for package F** (`PipeSlotPeek`'s new members) is §4.3. A unit case proves the marks move
independently: §4.4.

### M6 — the closure gate could not go red → **FIXED, and the real defect was one step further in**

At `08192d72` the script **does** exit 1 when `--compiler X` is missing *in a mode that runs one* —
`--mode clang` and `--mode both` both reach `pick_compiler`, which already called `sys.exit(1)`.
Measured on this tree before the fix:

```
$ python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all
::error::compiler not found: clang++-20                                             rc=1
```

The hole is the **default mode**. `--mode text` never reaches `pick_compiler`, so an explicit
`--compiler` is accepted and silently ignored:

```
$ python3 scripts/check_include_closure.py --compiler clang++-20 --self-test --require-all
include-closure: 4 probes, 0 skipped, 0 problem(s)                                  rc=0   # BEFORE
```

That is the false green D16 recorded, and it is the shape a gate line acquires the moment someone
drops `--mode both`. Two changes, `scripts/check_include_closure.py`:

1. `:608-614` — an explicit `--compiler` is resolved in **every** mode. Asking for a compiler that
   is not there is an operator error whatever the mode, and it stops the run rather than
   half-running it. `:630` reuses the resolved path so `--mode clang`/`both` are unchanged.
2. `:589-593`, `:699-704` — **`--expect-probes N`**: the run fails unless exactly N probes ran, so
   the gate line can pin 4 rather than only the exit code. A `--probe` typo or a manifest edit is
   exactly how a gate stops looking at anything.

Both arms verified red (§5). **The gate scripts spell `clang++`** — `/usr/sbin/clang++` exists,
`clang++-20` does not; the script's own usage block (`:47-56`) now spells `clang++` and
`--expect-probes 4`. **`.github/workflows/test.yml:774` still spells `--compiler clang++-20`** and
is package F's file, not A's: with this commit that line now goes **red** instead of green-and-empty,
which is the correct direction but F must move it to `--compiler clang++ --expect-probes 4` before
the lane can pass. `wsl_p4a_gate.sh` (the integrator's) already spells `clang++`.

### Minors

| minor | where it lives | disposition |
|---|---|---|
| **m1** vacuous `kMGPipeResourceTargetBuffer` drift assert | `MG_Pipe/MGPipeTypes.h` | **not taken — outside this round's file set.** For `wire` or an announced A amendment: give the constant its literal `0`, or drop the assert and say the constant is derived. |
| **m2** `MGPipeApplierReleaseObjectRecords` clears three of the working handles | `MG_Pipe/PipeApply.cpp` | **not taken — `wire` owns the file this round** |
| **m3** the texture death helper discarded the view helper's answer | `PipeFill.cpp:1224-1229` | **TAKEN.** `return published \|\| viewPublished;`, with the reason: a texture whose `ResourceDestroy` was suppressed but whose `DeleteSamplerView` went out has already spoken for this object, and `false` would run the legacy path for both halves. |
| **m4** the `~SamplerObject` ordering claim is not guaranteed | `PipeFill.cpp:1210-1223` | **TAKEN.** The conclusion is kept and the timing claim is deleted and replaced by its refutation (`m_sampler` is a `SharedPtr`; a unit slot or a view resolution delays it arbitrarily) plus an explicit "a package must not build an ordering on it". |
| **m5** the archive byte stream is not canonical | `ProgramArtifactsCodec.h` | **not taken — outside the file set.** Still worth one sentence in that header before P5 or the program disk cache wants a cache key. |
| **m6** `EveryUnmigratedEmulationIsNamedOnce` cannot fail for its reason | `PipeCatalogueTest.cpp` | **not taken.** The "red before" half genuinely belongs to F's grep script; adding a tree-side check here would duplicate it in the weaker direction. |
| **m7** `BindProgramPipelineObject`'s row is coupled to a `PipeFill.cpp` arm | `MG_Pipe/DirtySurface.def` | **not taken — outside the file set** (A's, but not this round's). One sentence in the row's comment naming `EmittedCallSuppliesTheWholeField(GetProgramForDraw)`. |
| **m8** the five singleton cases do not observe what they name | the five `*EmitTest.cpp` | **not taken — B and C own those files' contents now** |
| **m9** `FreeCount(ShaderCso)` sums two spaces | `SlotAllocator.h:115-119`, `.cpp:247-254` | **TAKEN, and more than asked**: the caveat is written down *and* `CompositeFreeCount()` answers the band, so a caller can tell which space a slot returned to instead of only being warned that it cannot. |

---

## 3. THE CONTRACT ADDITION — what B and C compile against

Everything in this section is new at `32033d69` and is what packages B and C must write their
emitters against. **Nothing calls any of it yet**; B and C add the call sites by **editing** the
existing `#if MOBILEGL_PIPE_PUSH` constructor / mutator / destructor bodies, never by adding one (G1).

### 3.1 The publication latch — `MG_Pipe/PipeMutation.h:255-279`

```cpp
    void MGPipeNoteHandlePublished(MGPipeKind kind, MGPipeHandle handle);
    Bool MGPipeHandleIsPublished(MGPipeKind kind, MGPipeHandle handle);
    void MGPipeNoteHandleUnpublished(MGPipeKind kind, MGPipeHandle handle);
```

**The protocol, and it is not negotiable.** An emitter calls `MGPipeNoteHandlePublished(kind,
handle)` **when, and only when, a create for exactly that handle actually went on the wire**. The
six death helpers read `MGPipeHandleIsPublished` and clear the latch with the delete they emit; an
emitter that drops a record for its own reasons (an LRU eviction, a re-mint) calls
`MGPipeNoteHandleUnpublished`. Keyed `{kind, slot, gen}`, so a recycled slot never inherits its
predecessor's answer, and the `ShaderCso` composite band has a table of its own inside the latch.
Never destroyed (`PipeFill.cpp:1018-1024`), for `MGPipeSlots()`' exit-order reason.

### 3.2 The four mints — `PipeMutation.h:281-299`, bodies `PipeFill.cpp:1042-1056`

```cpp
    void MGPipeMintTextureHandle(MG_State::GLState::ITextureObject& texture);
    void MGPipeMintRenderbufferHandle(MG_State::GLState::RenderbufferObject& renderbuffer);
    void MGPipeMintFramebufferHandle(MG_State::GLState::FramebufferObject& framebuffer);
    void MGPipeMintShaderCsoHandle(MG_State::GLState::ProgramObject& program);
```

Pure allocator work — `MGPipeSlots().Acquire(kind, object.GetLifetimeId())` and nothing else — and
**unconditional in a push build**, for `MGPipeMintResourceHandle`'s reason: `MGPSurface::Res`,
`MGPBoundView::Texture` and `MGPImageView::Res` name a texture or renderbuffer handle out of
*other* families, so gating the mint on the family's own bit would make those emit null handles in
exactly the A/B arm that exists to isolate the families. Called from the object's constructor.
A program-pipeline **composite** is not a construction event and is not here: it is minted by C's
resolver through `MGPipeSlotAllocator::AllocateComposite`, the one door into the band.

### 3.3 The nine emissions — `PipeMutation.h:301-358`, bodies `PipeFill.cpp:1058-1114`

```cpp
    // textures and renderbuffers - MG_Impl/Pipe/TextureEmit.h, package B
    void MGPipeEmitTextureResourceCreate(MG_State::GLState::ITextureObject& texture);
    void MGPipeEmitTextureResourceRespecify(MG_State::GLState::ITextureObject& texture);
    void MGPipeEmitTextureParams(MG_State::GLState::ITextureObject& texture);
    void MGPipeNoteTextureLevelDirty(MG_State::GLState::ITextureObject& storageOwner,
                                     Uint32 uploadTarget, Uint32 level);
    void MGPipeEmitRenderbufferResourceCreate(MG_State::GLState::RenderbufferObject& renderbuffer);
    void MGPipeEmitRenderbufferResourceRespecify(MG_State::GLState::RenderbufferObject& renderbuffer);

    // sampler CSOs and sampler views - MG_Impl/Pipe/SamplerEmit.h, package C
    void MGPipeEmitSamplerCsoCreate(MG_State::GLState::SamplerObject& sampler);
    void MGPipeEmitSamplerViewCreate(MG_State::GLState::ITextureObject& texture);

    // programs - MG_Impl/Pipe/ProgramEmit.h, package C
    void MGPipeEmitShaderCsoCreate(MG_State::GLState::ProgramObject& program);
```

Each body is the same three lines (`PipeFill.cpp:1058-1114`): the **gate**, then the **forward**.

### 3.4 THE ENTRY POINTS EACH EMIT HEADER MUST PROVIDE — implement exactly these

Public members of the family's existing emitter class, taking the frontend object by reference,
returning `void`. **The spelling is load-bearing**: `PipeFill.cpp` already calls them, and a family
that sets its wired constant without providing one is a compile error in that family's own commit.

| emitter class | entry point | called from |
|---|---|---|
| `MGPipeTextureEmitter` | `void EmitResourceCreate(MG_State::GLState::ITextureObject&)` | `MGPipeEmitTextureResourceCreate` |
| `MGPipeTextureEmitter` | `void EmitResourceRespecify(MG_State::GLState::ITextureObject&)` | `MGPipeEmitTextureResourceRespecify` |
| `MGPipeTextureEmitter` | `void EmitTextureParams(MG_State::GLState::ITextureObject&)` | `MGPipeEmitTextureParams` |
| `MGPipeTextureEmitter` | `void NoteLevelDirty(MG_State::GLState::ITextureObject& storageOwner, Uint32 uploadTarget, Uint32 level)` | `MGPipeNoteTextureLevelDirty` |
| `MGPipeTextureEmitter` | `void EmitRenderbufferCreate(MG_State::GLState::RenderbufferObject&)` | `MGPipeEmitRenderbufferResourceCreate` |
| `MGPipeTextureEmitter` | `void EmitRenderbufferRespecify(MG_State::GLState::RenderbufferObject&)` | `MGPipeEmitRenderbufferResourceRespecify` |
| `MGPipeSamplerEmitter` | `void EmitSamplerCso(MG_State::GLState::SamplerObject&)` | `MGPipeEmitSamplerCsoCreate` |
| `MGPipeSamplerEmitter` | `void EmitSamplerView(MG_State::GLState::ITextureObject&)` | `MGPipeEmitSamplerViewCreate` |
| `MGPipeProgramEmitter` | `void EmitShaderCso(MG_State::GLState::ProgramObject&)` | `MGPipeEmitShaderCsoCreate` |

`MGPipeFramebufferEmitter` and `MGPipeImageEmitter` need **nothing new**: a framebuffer's record
goes out at the validate point through the existing `EmitFramebufferState`, and the image units'
through `EmitShaderImages`. `MGPipeTextureEmitter::DrainTextureSubData` is unchanged and is still
the only validate-point call in the texture family.

**Each entry point owns its family's handle rule and its own publication.** `PipeFill.cpp` does
*not* resolve the handle for these — deliberately, because the three families disagree about
identity and the contract must not pick for them: a texture's view is identity-addressed off the
texture's lifetime id (D-F2), a sampler CSO is **content-addressed and shared** (D-F1), a program's
CSO is identity-addressed per `ProgramObject` and a composite comes out of the band (D-H7).

> **Open item for C, and it is a real tension in the brief, not a defect in this commit.** D-F1
> content-addresses sampler CSOs at capacity 256 with sharing, while D-I1 makes `~SamplerObject`
> call `MGPipeEmitSamplerCsoDestroyAndFree(m_lifetimeId)`, which resolves
> `FindByLifetimeId(SamplerCso, lifetimeId)`. Two samplers sharing one CSO cannot both own that
> slot's lifetime-id mapping. C must decide which sampler object (if any) owns the mapping, and the
> death helper is already safe when nothing resolves: a null handle emits no delete and frees no
> slot, and the death notice still goes out.

### 3.5 The seam, and why it exists — `PipeFill.cpp:889-937`

```cpp
        template <Uint64 kWired, class Emitter, class Fn>
        constexpr void ForwardWhenWired(Emitter& emitter, Fn&& call) {
            if constexpr (kWired != 0) { call(emitter); } else { (void)emitter; (void)call; }
        }
```

A owns `PipeFill.cpp` for the whole phase and B/C own the five emit headers, so the forwarding had
to be written **now**, against an emitter whose entry point does not exist **yet**. A plain call
would not compile against the stub and a runtime `if` would not link. Passing the family's wired
constant as a **template argument** makes the `if constexpr` condition value-dependent, so the
discarded statement is never instantiated while the constant is `0`; the moment a family sets it,
the statement instantiates and a missing or misspelled entry point is a **compile error in that
family's own commit**. `call` must be a **generic** lambda (`[&](auto& emitter) { … }`) — that is
what defers the body's name lookup — and every call site in `PipeFill.cpp` already is one.

The taken arm is otherwise never instantiated in this tree, so `PipeFill.cpp:917-937` adds a
**positive control**: a local probe emitter driven through the same template with `1ull` and with
`0ull`, and a `static_assert` that exactly one call happened. "Discarded when 0, called when set"
is a checked property of this build, not a claim in a comment.

---

## 4. The other three landed rules

### 4.1 `wants()` as landed — `MG_Impl/Pipe/PipeFill.cpp:2184-2191`

```cpp
        const Uint64 pushMask = MG_Config::Features.PipePush;
        const auto wants = [&](MGPipeDirty bit) {
            const Uint64 subsystem = MGPipeSubsystemForDirty(bit);
            return subsystem != 0 && (pushMask & subsystem) != 0 &&
                   (kMGPipeWiredSubsystems & subsystem) != 0 &&
                   (dirty & MGPipeDirtyBit(bit)) != 0;
        };
```

**Four conditions**: the bit maps to a subsystem, the operator's mask has it, **this build wired
the family**, and the bit fired. The birth hooks' gate (`FamilyIsLive`, `PipeFill.cpp:885-887`) is
the same pair one level in, so the client paths and the validate point cannot disagree.

**Behaviour at `c0b` is unchanged, and it is checkable rather than argued.**
`MGPipeSubsystemForDirty` (`Tracker.h:156-200`) returns exactly eight distinct constants:
`RenderState`, `PixelPack`, `PatchState`, `VertexAttribDefaults`, `VertexInput` — all five present
in `kMGPipeWiredSubsystems` (`PipeFill.cpp:1601-1610`) — and `Programs`, `Framebuffer`, `Samplers`,
whose family constants are all `0` today and whose emitters are all stubs returning 0. So the new
condition removes nothing that emitted and adds nothing that did not.

**What this changes for B and C**: `kMGPipeWiredFramebufferSubsystem`,
`kMGPipeWiredTextureSubsystem`, `kMGPipeWiredSamplerSubsystem` and `kMGPipeWiredProgramSubsystem`
**are** the switch, exactly as their headers say. An emitter with a body and a constant still `0`
is called by nothing and emits nothing; flipping the constant switches the family on. Both
directions of the M3 failure scenario are now closed.

### 4.2 The sampler view's death notice

`PipeFill.cpp:1182-1193`, §2/M4. **For D and E**: a backend twin held per `SamplerViewCso` slot now
gets its `NotifyStateObjectDestroyed(MGPipeKind::SamplerViewCso, lifetimeId)`, like the other five
kinds, and the answer to `SlotTables.h:38`'s "all six re-keyed object classes raise
`NotifyStateObjectDestroyed()`" is now yes for all six.

### 4.3 The allocator's two spaces — API for package F

```cpp
        Uint32 HighWater(MGPipeKind kind) const;   // the ORDINARY space only, every kind
        Uint32 CompositeHighWater() const;         // one past the highest composite slot ever
                                                   //   handed out; == the band base when none was
        Uint32 LiveCount(MGPipeKind kind) const;   // BOTH spaces for ShaderCso (unchanged)
        Uint32 CompositeLiveCount() const;         // the band's share of it
        Uint32 FreeCount(MGPipeKind kind) const;   // BOTH spaces for ShaderCso (unchanged)
        Uint32 CompositeFreeCount() const;         // the band's share of it
```

`SlotAllocator.h:86-119`; bodies `SlotAllocator.cpp:224-254`; `KindState::BandLiveCount` at
`SlotAllocator.h:148-150`, maintained at `.cpp:157-158`, `:201-202`, `:264`.

**`LiveCount` and `FreeCount` deliberately keep summing both spaces.** `CompositeResolverTest.
TheCompositeBandHasExactlyOneDoor` — which is package **C**'s file and must not be edited here —
reads `LiveCount(ShaderCso)` across a composite free and asserts it drops by one. Narrowing it
would have broken a case in a file this round may not touch, and the merged count is also the right
answer on its own terms: a live composite *is* a live `ShaderCso`. Only the **high-water** mark had
to split, because that is the one whose merged form is monotone-and-pinned.

**Package F, `PipeSlotPeek`**: the six new members G8b asks for want a **seventh**,
`PeekPipeCompositeSlotHighWater`, and the composite leak case asserts on that one; the ordinary
`ShaderCso` case keeps asserting on `PeekPipeSlotHighWater(ShaderCso)`, which is now a real
assertion again.

### 4.4 The unit case that proves the marks move — `MG_Test/Pipe/PipeCatalogueTest.cpp:783-843`

`PipeCatalogue.TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace` pins both halves in one
allocator: a composite mint moves `CompositeHighWater()` and **leaves `HighWater(ShaderCso)` where
it was**; an ordinary mint with a composite outstanding moves `HighWater(ShaderCso)` and **leaves
`CompositeHighWater()` where it was**; freeing the composite returns it to the band's free list
(`CompositeFreeCount() == 1`, `FreeCount(ShaderCso) == 1`) and **neither high-water mark comes back
down**, which is what makes them leak witnesses. A visible `GTEST_SKIP` in a pull build, so
`ctest -N` stays name-for-name identical.

It lives in `PipeCatalogueTest.cpp` because that file is A's by C.7 and needs no
`MG_Test/Pipe/CMakeLists.txt` edit. `SlotAllocatorTest.cpp` is the thematic home and C.7 gives it
to **nobody**; a later round may move the case there.

---

## 5. Why the tree is still behaviourally neutral

Three independent mechanisms, each checkable:

1. **Nothing calls the birth hooks.** `git grep` for any of the thirteen names outside
   `PipeMutation.h` and `PipeFill.cpp` is empty; B and C add the call sites.
2. **Every birth hook is gated twice** and both gates are false today:
   `FamilyIsLive(subsystem, wired)` needs the operator's bit **and** a non-zero family constant, and
   all four family constants are `0`. Even with the gate passed, the `if constexpr` seam discards
   the forward.
3. **Nothing latches a publication**, so `MGPipeHandleIsPublished` answers `false` for every handle
   and the five death helpers emit no wire delete — exactly as they did at `c0`, but now for a
   reason a package can change rather than a literal it cannot.

And `wants()`' new condition removes nothing (§4.1). The pull build is untouched: every line added
to `PipeMutation.h` is inside its `#if MOBILEGL_PIPE_PUSH`, and `PipeFill.cpp` / `SlotAllocator.cpp`
are compiled only under it — which G1's 0/0/0/0 confirms.

---

## 6. Verification transcript (on `2cb44039`, working tree clean)

```
$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
symbol-report: before: ~/w7/p4a-before-libMobileGL.so (19114360 bytes on disk)
symbol-report: after : build-linux/libMobileGL.so     (19114360 bytes on disk)
symbol-report: .text 10806323 -> 10806323 (+0, +0.000%)
symbol-report: 27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
### Removed (0) _none_   ### Added (0) _none_
### Resized (0) _none_   ### Renamed only (0) _none_                                 rc=0

$ cmake --build build-linux  -j 12 && ctest --test-dir build-linux  -L unit -j 12   100% 1636/1636
$ cmake --build build-push   -j 12 && ctest --test-dir build-push   -L unit -j 12   100% 1636/1636
$ cmake --build build-verify -j 12 && ctest --test-dir build-verify -L unit -j 12   100% 1636/1636

$ diff <(build-linux names) <(build-push names)          (empty; 2602 == 2602)
$ comm -23 ~/w7/p4a-before-ctest-names.txt <(build-linux names)                     (empty)
$ comm -13 ~/w7/p4a-before-ctest-names.txt <(build-linux names)                     14 added:
  CompositeResolver.TheCompositeBandHasExactlyOneDoor
  {Framebuffer,Image,Program,Sampler,Texture}Emit.TheEmitterIsOneNeverDestroyedProcessSingleton
  PipeCatalogue.{EveryTextureTargetMapsToItsOwnResourceTarget, EveryUnmigratedEmulationIsNamedOnce,
                 TextureParamsNameTheirBuiltinSamplerAndFramebufferStateNamesItsTarget,
                 TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace}          <- c0b's one
  ProgramArtifactsCodec.{RoundTripsAFullyPopulatedArchive, ATruncatedStreamIsRefusedNotGuessed,
                         AVersionMismatchIsRefused, TheTablesVisitEveryMemberExceptTheLiveProgram}

$ ctest --test-dir build-linux  -R TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace   Skipped (pull)
$ ctest --test-dir build-push   -R TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace   1/1 passed
$ ctest --test-dir build-verify -R TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace   1/1 passed

$ python3 scripts/gen_pipe.py --check                                               rc=0
  gen_pipe: 71 calls (11 screen, 60 context), 72 verify payloads, 63 PipeInputs fields,
            69 verbs, 9 classes; inventory 477 rows, 0 UNMAPPED; generated files up to date
$ python3 scripts/gen_pipe.py --self-test                                           rc=0
$ git diff --exit-code -- MobileGL/MG_Pipe/generated                                rc=0
$ python3 scripts/gen_pipe_dirty_surface.py --check                                 rc=0
  dirty-surface: 79 mutators, all mapped; 0 COARSE; 2 UNDECIDED (listed); 26 prose answers
$ python3 scripts/gen_pipe_dirty_surface.py --self-test                             rc=0
$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD                               rc=0

$ python3 scripts/check_include_closure.py --mode both --compiler clang++ \
      --self-test --require-all --expect-probes 4
include-closure: self-test: 6 negative-control trip(s), parser checks OK
include-closure: 4 probes, 0 skipped, 0 problem(s)                                  rc=0

# M6's two controls, both of which used to be green:
$ python3 scripts/check_include_closure.py --mode text --compiler clang++-20 --self-test
::error::compiler not found: clang++-20                                             rc=1   (was rc=0)
$ python3 scripts/check_include_closure.py --mode both --compiler clang++ --expect-probes 9
include-closure: 4 probes, 0 skipped, 1 problem(s)
::error::expected 9 probe(s), ran 4                                                 rc=1   (new)

$ git rev-parse refs/tags/p4a/contract   08192d7266ace31cbba1641ed8078948687cbaaa   (NOT moved)
$ git rev-parse refs/heads/p4a/contract  2cb44039b526abfd9875ea3b16fb57dda4296e26
$ git status --porcelain                 (empty)
```

---

## 7. What the other packages must be told

1. **B and C: implement §3.4's nine entry points verbatim, and call
   `MGPipeNoteHandlePublished(kind, handle)` when a create actually goes out.** Nothing else
   latches it, and without it your kind's destroy never goes out.
2. **`kMGPipeWired*Subsystem` now really is the switch** (§4.1). Emitter bodies are inert until the
   constant is set, in *both* the validate point and the client paths.
3. **The wired constant is also a compile-time contract**: set it and every entry point §3.4 names
   for your family must exist, spelled exactly, or the build fails in your commit.
4. **C: the D-F1/D-I1 sampler-CSO identity tension is open** (§3.4's box) and is C's to resolve.
5. **D and E: `SamplerViewCso` now raises `NotifyStateObjectDestroyed`** like the other five kinds
   (§4.2) — write the twin drop.
6. **F: `PipeSlotPeek` needs a seventh member** for `CompositeHighWater()` (§4.3), and
   `.github/workflows/test.yml:774` must move to `--compiler clang++ --expect-probes 4` or that
   step is now red (§2/M6).
7. **`wire`: M2 (the `MGPSubRegion` tail on `MGPipeApplyResourceSubData`) and m2 are yours**, and
   m1 (`MGPipeTypes.h`'s vacuous drift assert) is unclaimed.
