# P4a package C — `clientsp`. Rework result, v2

Branch `refs/heads/p4a/clientsp`, head **`00a86d2a`**. **Not pushed.** Working tree clean.
Rebased onto `refs/heads/feat/disaggregated` = **`17db7598`** (c0 + c0b + c0c), then three
rework commits. `git diff --stat 17db7598..HEAD`: **11 files, +3027 / −52**; the rework alone
(`f5c3364c..HEAD`) is **6 files, +695 / −94**.

## 0. Commits

| # | hash | message (exact, single line, no attribution) |
|---|---|---|
| c1' | `2a076438` | `[Feat] (Pipe): content-address sampler states, mint one sampler view per texture and push the three unit sets behind their own content hashes` |
| c2' | `94daf8ed` | `[Feat] (Pipe): publish a program's per-stage SPIR-V and reflection archive as a shader CSO and its default uniform block as global constants` |
| c3' | `a23a07ad` | `[Feat] (Pipe): resolve a program pipeline into one shader CSO out of the reserved composite band and release it exactly once` |
| c4' | `f5c3364c` | `[Test] (Pipe): wire the sampler and program subsystems and pin the padding-proof CSO identity, the program-resolved unit sets, the linked-snapshot stage mask and the composite band's single release` |
| **c5** | **`02475fbb`** | `[Fix] (Pipe): give the sampler family the birth-hook entry points the contract declares, take the publication latch where its creates go out, and reference-count cache entries so an LRU eviction can never take a handle a standing record still names` |
| **c6** | **`5f577d16`** | `[Fix] (Pipe): give the program family its birth-hook entry point and publication latch, invalidate the global-constants key whenever a create_shader_state is re-issued, and count a truncated module tail instead of asserting it` |
| **c7** | **`00a86d2a`** | `[Fix] (Pipe): stop a make-current disarming the composite resolver's release path - the memo's freshness and the slot's release obligation were one flag, so after the first Reset no signature move ever spoke a delete` |

c1'..c4' are the v1 commits replayed; the rebase was **clean, no conflicts** (c0b/c0c touched
`PipeFill.cpp`, `SlotAllocator.*`, `PipeMutation.h`, `MGPipeTypes.h`, `PipeFields.def`,
`PipeCatalogueTest.cpp` and `check_include_closure.py`, none of which C owns or edits).

**The rework is grouped by file pair, so `f5c3364c` and `02475fbb` do not build.** That is not a
new fact: `f5c3364c` sets both wired constants and c0b's `if constexpr` seam then instantiates
forwards to entry points that did not exist, so the branch has not compiled since the rebase.
The tree builds again at **`5f577d16`** (both entry-point commits landed) and at `00a86d2a`.
Splitting the reconciliation out of the two defect fixes would have meant splitting hunks inside
one file; the integrator squashes or merges the branch anyway.

---

## 1. The three majors

### C-M1 — `MGPipeCompositeResolver::Reset()` disarmed the release path  → FIXED (`00a86d2a`)

`Live` and the memo's freshness were one flag. `Reset()` cleared it, the reuse branch returned
before anything could restore it, and from the first make-current onwards `ReleaseEntry` saw
`!Live` and returned: no `delete_shader_state`, no `Free`, no counter, and the old composite's
handle overwritten out of the resolver.

Split into two flags, each with exactly one meaning:

* `CompositeResolver.h:181` `Bool Live` — **the release obligation**. Set when the entry takes
  responsibility for a composite's slot, cleared **only** by `ReleaseEntry`.
* `CompositeResolver.h:183` `Bool Fresh` — the memo's "does this entry describe the current
  context's pipeline of this name". `Reset()` (`:143-145`) clears **only** this; the reuse branch
  re-arms it (`:105-113`) and `Observe`'s mint path sets both (`:125`). `HandleFor` (`:151`) now
  asks for both, which is the only place `Fresh` is observable.

The reuse branch deliberately does **not** consult `Fresh` before deciding not to release: the
handle is minted off the composite `ProgramObject`'s own lifetime id, so an identical handle *is*
an identical object and there is nothing to release whatever the memo's freshness says. Making
`!Fresh` fall through to `ReleaseEntry` would have emitted a delete for a live composite.

**Case added** (C-m9's gap): `CompositeResolver.ASignatureMoveAfterAMakeCurrentStillReleases
ThroughTheResolver` (`CompositeResolverTest.cpp:346-403`). It drives the release **through the
resolver** — bind a real pipeline, emit, `MGPipeProgramEmitterInstance().Reset()` (the
make-current, which reaches the resolver's `Reset()`), emit again on the unchanged stage set (the
reuse branch), then `glUseProgramStages` a different fragment stage so the signature really moves
— and asserts `Releases == 1` and that the first composite's slot is no longer live. Both existing
"FreesTheSlotExactlyOnce" cases call the death helper directly and never touch the resolver; this
one does not. **Negative control: restoring `entry.Live = false` in `Reset()` turns it red.**

### C-M2 — the `(Cso, Version)` global-constants latch survived a re-issue  → FIXED (`5f577d16`)

`ProgramEmit.h:259-271`: on the branch of `AcquireShaderCso` that actually re-issues
`create_shader_state`, `m_constantsCso` is reset to `kMGPipeNullHandle` and `m_constantsVersion`
to `kMGPipeGlobalConstantsNeverUploaded` whenever the re-issued handle is the one the constants
key names. Invalidated rather than re-emitted, because `AcquireShaderCso` has no business deciding
when the constants go out — the next `EmitGlobalConstants` sees an unlatched key and sends them.

Two accessors so a case can see it: `GlobalConstantsCso()` / `GlobalConstantsVersion()`
(`ProgramEmit.h:378-379`).

**Case added**: `ProgramEmit.AReIssuedCreateReSendsTheDefaultUniformBlock`
(`ProgramEmitTest.cpp:318-381`). **Negative control: deleting the four-line invalidation turns it
red.**

**Two corrections to the review's scenario, both written into the case's comment.**

1. *The behavioural half is not the discriminator.* `BumpLinkObservableVersions()`
   (`ProgramObject.cpp:346-359`) calls `MarkUBOContentDirty()` beside `++m_linkVersion`, so the
   `Version` half of the key moves on its own for the one re-issue trigger that exists today. The
   case therefore pins the **mechanism** — after a re-issue the emitter must hold no key at all —
   which is the property the contract needs and the half that goes red.
2. *A failed relink does not leave the block populated in this frontend.* `Link()`'s prologue
   assigns `m_spirv = {}` (`ProgramObject.cpp:532`), so `globalUboScratch` is empty after **any**
   link attempt and `EmitGlobalConstants` early-outs on `GetUBOSize() == 0`. The review's
   "the client's scratch still holds the live values" is true of GL and not of this
   implementation. The fix is still right and still required — it is what makes any future
   re-issue trigger that does not happen to move the content version safe, of which a recycled
   slot is one — but the reachable damage today is smaller than the review states.

*Noticed while writing the case, not fixed, recorded for ID-19:* `EmitGlobalConstants` reads
`GetUBOContentVersion()` (`ProgramEmit.h:164`) **before** `GetUBOSize()` (`:168`) joins phase B,
and that join bumps the counter — so the first emission after a link latches a version one behind
the bytes it sent and the next call re-sends identical bytes once. Over-emission, one record per
link, never under-emission. Left alone because reordering the two reads changes when the join
happens on a path the phase has not measured.

### C-M3 — LRU eviction could take a handle a standing record names  → FIXED per ID-17 (`02475fbb`)

Reference-counted entries, exactly as ID-17 rules.

| what | where |
|---|---|
| `Uint32 RefCount` on the entry | `SamplerEmit.h:331` |
| `Acquire(params, payloadBytes)` returns a handle **and takes a reference** | `:244-248` → `AcquireCanonical` `:337-359` (`++RefCount` on a hit `:355`, `RefCount = 1` on a mint `:410`) |
| `Release(handle)` drops one; unknown handle or double release is a no-op, never an underflow | `:267-277` |
| LRU considers **unreferenced entries only** | `:367-374` |
| every entry pinned ⇒ mint beyond 256 and count it | `:376-381`, `Counters::OverCapacityMints` `:223` |
| `Evict` asserts its victim is unreferenced | `:434-437` |
| `RefCountOf(handle)` for diagnostics and cases | `:279-286` |
| the reference a **bound sampler state** holds while bound | `EmitSamplerStates` `:751-783`, `m_stateRefs` `:996` |
| that reference released at a make-current (working state, not a record) | `Reset()` `:944-960` |
| the `static_assert` restated | `:207-209` |

The `static_assert` is kept but no longer claims to be what makes eviction safe. Its message now
says what it really guarantees — a cache that could not hold one whole emission pass would
over-capacity-mint on **every** pass — and the header block at `:167-200` states plainly that
correctness against eviction is the reference count.

`EmitSamplerStates`' reconciliation is one extra pass over the 192-entry unit array: `Acquire` has
already taken a reference for every unit in the new set, so a unit whose handle did not move gives
that duplicate back, a unit whose handle moved gives back the one it used to hold, and a unit that
fell outside the window gives back its own. One binding is one reference, not one per pass.

**Three cases added** (`SamplerEmitTest.cpp`):

* `SamplerEmit.AReferencedCsoIsNeverTheLruVictim` (`:526-557`) — a pinned value survives a
  miniature CTS sampler sweep (512 distinct unreferenced values through a 256-entry cache), the
  LRU really runs (`Evictions > 0`), nothing over-capacity-mints, and re-acquiring the pinned
  value is a **hit** and not a re-mint. **Negative control: dropping the `RefCount != 0` skip in
  the victim choice turns it red.**
* `SamplerEmit.AFullyPinnedCacheMintsBeyondItsCapacityAndCountsIt` (`:559-586`) — 256 pinned
  entries plus one more distinct value: `Size() == 257`, `Evictions == 0`, `OverCapacityMints == 1`.
* `SamplerEmit.ABoundSamplerStateHoldsItsCsoUntilTheUnitMoves` (`:494-524`) — the emitter half:
  one binding is one reference across repeated passes, and the reference goes when the unit stops
  naming the CSO (released is **not** evicted).

---

## 2. The c0b reconciliation (review §6, items 1–8)

| item | disposition |
|---|---|
| **1. `MGPipeSamplerEmitter::EmitSamplerCso(SamplerObject&)`** | **Added**, `SamplerEmit.h:888-892`. Body is the family's handle rule: `MGPipeSamplerCsoCacheInstance().Acquire(sampler.GetAllSamplerParameters(), bytes)`, then `Release` of the reference it just took — a birth is not a standing record, so pinning the default-parameter entry for the life of the process would buy no reader anything. The comment says what the review asked it to say: minting at construction content-addresses the **default** parameters and the cache dedupes, so it is one `create_sampler_state` per distinct default-valued sampler, not one per object. |
| **2. `MGPipeSamplerEmitter::EmitSamplerView(ITextureObject&)`** | **Added**, `SamplerEmit.h:900-905`. A wrapper over `AcquireSamplerView`, resolving the texture handle exactly as `EmitSamplerViews` does and discarding the byte count. |
| **3. `MGPipeProgramEmitter::EmitShaderCso(ProgramObject&)`** | **Added**, `ProgramEmit.h:292-295`. A wrapper over `AcquireShaderCso`. |
| **4. the publication latch, four call sites** | **All four**, and nothing else. `MGPipeNoteHandlePublished(SamplerCso, cso)` at `SamplerEmit.h:419` (immediately after `MGPipeApplyCreateSamplerState`); `MGPipeNoteHandleUnpublished(SamplerCso, …)` in `Evict` (`:450`) **and** in `ResetForTest` (`:313`); `MGPipeNoteHandlePublished(SamplerViewCso, handle)` at `:855`; `MGPipeNoteHandlePublished(ShaderCso, handle)` at `ProgramEmit.h:255`. Both headers now include `<MG_Pipe/PipeMutation.h>` explicitly (`SamplerEmit.h:47`, `ProgramEmit.h:44`) rather than relying on `CompositeResolver.h` pulling it in. |
| **5. `RecordIsPublished` / `NoteRecordDestroyed` stop being the death path's authority** | Kept, comments rewritten: `SamplerEmit.h:288-293` (the cache's), `:907-916` (the view latch's), `:925-930` (`NoteRecordDestroyed`), `ProgramEmit.h:301-308` and `:317-327`. Each says the death helpers read A's latch, and each says why it stays (the version-first skip is the same latch). The **program emitter's bound-mirror clearing keeps its three lines and gains the "why it self-heals" the review asked for**: the slot's `Gen` moves on reuse, so a stale record latch is refused by `RecordGen == handle.Gen` and the three mirrors hold a handle whose generation can never be handed out again, so the next `EmitShaderState` compares against a different handle and re-binds. Clearing them is the cheaper answer, not the load-bearing one. |
| **6. the sampler CSO has no lifetime-id mapping, and D must be told** | **Sentence in the header**, `SamplerEmit.h:201-205`, and §3 below. |
| **7. two now-false header comments** | **Rewritten.** `SamplerEmit.h:67-83` and `ProgramEmit.h:51-62` both now say the constant **is** part of the emission gate (four conditions in `wants()`, the same pair in `FamilyIsLive`) and is additionally a compile-time contract, and that the runtime A/B is still the mask. |
| **8. `CompositeResolver.TheCompositeBandHasExactlyOneDoor` still passes unchanged** | **Verified**, unchanged and green. |

**The three birth hooks still have no call site, deliberately, and this is the one place the
rework declines what the review implies.** The entry points exist and are instantiated (the build
proves it), but `MGPipeEmitSamplerCsoCreate` / `MGPipeEmitSamplerViewCreate` /
`MGPipeEmitShaderCsoCreate` are called by nothing, so no constructor emits. Reasons, per hook:

* **`MGPipeEmitShaderCsoCreate`** — D-H4 and C.7 put `create_shader_state` at the **validate
  point** ("nothing today — `create_shader_state` is emitted from the validate point"). A
  construction-time create would publish an empty archive for a program that has never linked and
  would force a phase-B join at `glCreateProgram`. Adding it would be a design change, not a
  reconciliation.
* **`MGPipeEmitSamplerViewCreate`** — its call site is `TextureObject`'s constructor, which is
  **package B's file**, and D-F2 mints the view at first use behind the version-first skip.
* **`MGPipeEmitSamplerCsoCreate`** — the only honest call site is `SamplerObject`'s constructor
  (C's file, one line). It is *safe* — the cache dedupes to one record for all default-valued
  samplers and the entry point releases its reference — but it is not needed: every consumer of a
  sampler CSO acquires it from the cache at the point of use. It would also make
  `DirectGLES.cpp:209-214`'s backend-side `SamplerObject(0)` (D-F3's raw-depth-fetch sampler)
  emit into the pipe from inside `MG_Backend` during backend init.

**Integrator's call.** If a live birth hook is wanted for this kind, it is one line in
`SamplerObject::SamplerObject` (`MG_State/GLState/SamplerState/SamplerObject.cpp:26`) and C will
add it on request; the entry point behind it is already correct.

---

## 3. THE EXACT API PACKAGE B MUST CALL — quote this

In `MG_Impl/Pipe/TextureEmit.h`, inside `EmitTextureParams`, replacing
`MGPipeSlots().Acquire(MGPipeKind::SamplerCso, sampler->GetLifetimeId())`:

```cpp
#include <MG_Impl/Pipe/SamplerEmit.h>          // add to TextureEmit.h's include block

// ... in EmitTextureParams, where MGPTextureParams::BuiltinSampler is filled:
Uint64 samplerBytes = 0;
const MGPipeHandle builtinSampler =
    MGPipeSamplerCsoCacheInstance().Acquire(sampler->GetAllSamplerParameters(), samplerBytes);
// ... and, when this texture's params record STOPS naming a previously acquired handle
//     (a parameter change that re-acquires, or the texture's own death):
MGPipeSamplerCsoCacheInstance().Release(previousBuiltinSampler);
```

The full seam, all in `namespace MobileGL::MG_Pipe` (no qualification needed inside
`TextureEmit.h`), declared in `MG_Impl/Pipe/SamplerEmit.h`:

```cpp
MGPipeSamplerCsoCache& MGPipeSamplerCsoCacheInstance();                                   // :469

class MGPipeSamplerCsoCache {
    MGPipeHandle Acquire(const SamplerParameters& params, Uint64& payloadBytes);          // :244
    void         Release(MGPipeHandle handle);                                            // :267
    Uint32       RefCountOf(MGPipeHandle handle) const;   // diagnostics / cases           :279
    Bool         RecordIsPublished(MGPipeHandle handle) const;                             // :294
    SizeT        Size() const;
    const Counters& GetCounters() const;   // Mints/Acquisitions/Hits/Collisions/
                                           // Evictions/Releases/OverCapacityMints
};
```

Types: `ITextureObject::GetSamplerObject()` returns `const SharedPtr<SamplerObject>&`
(`TextureObject.h:34`); `SamplerObject::GetAllSamplerParameters()` returns
`const SamplerParameters&` (`SamplerObject.h:64`). **`Acquire` takes a non-const `Uint64&`**, so B
needs a real lvalue; fold `samplerBytes` into whatever B returns as payload bytes rather than
discarding it, or the `csob-blob` accounting under-reports 100 bytes per mint.

**Four rules B must honour.**

1. **Delete B's own `MGPipeSlots().Acquire(MGPipeKind::SamplerCso, …)` line.** Leaving it mints a
   second, record-less slot that `~SamplerObject` then frees.
2. **`Acquire` takes a reference; B owes exactly one `Release` for it.** The reference is what
   stops the LRU taking a handle a published `MGPTextureParams` still names — the applier does not
   resolve `BuiltinSampler`, and an eviction is not a parameter change, so nothing would refuse
   and nothing would re-emit.
3. **Release the previous handle whenever B re-acquires** (the params moved, so the
   content-addressed handle moved) **and when the texture's record stops standing**. If B has no
   texture-death hook of its own — the texture death helper is A's and does not forward to the
   emitter — the pin survives until B's per-texture latch on that slot is overwritten by the next
   texture at that slot. That is bounded by the number of live texture slots, not unbounded, and
   the over-capacity path handles it; but a real release at texture death is better and cheap if
   B's latch already holds the handle.
4. **B's params gate must be keyed on `GetTextureParamsVersion()`**, not on something a
   `glSamplerParameter*` write leaves alone — the review of B must check this. `glTexParameter*`
   moves both `GetTextureParamsVersion()` and `BumpSamplingResolutionGeneration()`
   (`TextureObject.cpp:53`); `glSamplerParameter*` goes through `SamplerObject.cpp:65`.

**Include closure re-run after the seam is added is B's**, but C re-ran the gate on this tree with
the new flag: `--mode both --compiler clang++ --self-test --require-all --expect-probes 4` → rc 0,
4 probes, 0 skipped, 0 problems, 6 negative controls tripped.

**And the sentence for D** (`SamplerEmit.h:201-205`): a content-addressed sampler CSO has **no
lifetime-id mapping**, so `NotifyStateObjectDestroyed(SamplerCso, lifetimeId)` arrives from
`~SamplerObject` with **no handle behind it, every time**. A backend must not key a sampler twin on
a `SamplerObject`'s lifetime id; the twin's life is `create_sampler_state` → the LRU's
`delete_sampler_state` and nothing else.

---

## 4. The ten minors and the two nits

| # | disposition |
|---|---|
| **C-m1** dispatch-through-a-pipeline window | **Declared, not taken.** Closing it means keying the image window and `EmitGlobalConstants` on `GetProgramForDispatch()` as well as `GetProgramForDraw()`, which changes what `set_shader_images` describes for an ordinary graphics draw too (the union of two programs' windows) and needs a dirty-bit shutter that can see the dispatch program — `Tracker.h`, A's file for the phase. Reach: compute through a **program pipeline** only, which no fixture in the phase exercises. Recorded for the integrator. |
| **C-m2** no version-first skip in front of the CSO acquire | **Declared, ID-19 budget item.** The refcount reconciliation adds one more pass over the unit array per firing of bit 13, so the cost the review measured grew rather than shrank. The latch that closes it is, per unit, `(sampler lifetime id, ctx.GetSamplingResolutionGeneration())` — and it is now **sound**, which it was not before: a bound CSO's handle cannot move under the reference count. Not taken in this round to keep the change surface auditable. **The integrator should record it in `MEASUREMENTS.md` under ID-19** — `docs/` is not C's file. |
| **C-m3** the collision branch's second eviction path | **TAKEN**, `SamplerEmit.h:337-359`. On a memcmp rejection the probe now **counts the collision and keeps going** instead of evicting and giving up. Both halves close at once: there is no second eviction path for the reference count to police, and a later entry with the same hash whose bytes really do match is found instead of a duplicate being minted. |
| **C-m4** dead `Forget()`, recyclable key | **TAKEN**: `Forget()` deleted. The key-recycling reason is written into the `Entry` comment (`CompositeResolver.h:164-175`) — a `ProgramPipelineObject` has **no lifetime id** (`ComputeDrawProgramSignature` reads the *stage* programs' ids), so `GetExternalIndex()` is the only available key; a recreated pipeline name inherits the entry, the first `Observe` finds a mismatched signature and releases it, and that release resolves nothing because the allocator erases the lifetime-id mapping on `Free`. It costs one redundant, idempotent death notice — the same shape the composite's own second release path already has. |
| **C-m5** one-entry `MGPipeProgramOpaqueUnits` memo | **Declared, not taken.** A two-entry memo would fix it without giving up the shared instance, and it is the same object C-m1 would have to widen; both belong in one change with a measurement behind it. Perf only. |
| **C-m6** compiled-out `MOBILEGL_ASSERT` on the module count | **TAKEN**, `ProgramEmit.h:233-240`. The assert is gone; `m_moduleTruncations` counts it and `TruncatedModuleCount()` (`:382`) reports it — D-J3's counted refusal, not an assertion that vanishes at INFO. |
| **C-m7** `ARedundantRebindOfTheSameSamplerEmitsNothing` has no `EmitterScope` | **REFUTED.** The case has had one since v1 — `SamplerEmitTest.cpp:459`, the first line of the body. Nothing to fix; a sentence was added saying why the scope is load-bearing. The case also now clears the sampler it bound to unit 2 on the way out, so it cannot leak state into the new reference-count cases. |
| **C-m8** the collision case cannot go red for its name | **TAKEN**, and closed with the `#if`-free test seam the review named as the honest option: `MGPipeSamplerCsoCache::AcquireWithForcedHashForTest(params, forcedHash, payloadBytes)` (`SamplerEmit.h:256-262`) puts two genuinely different values on one hash. `AHashCollisionDoesNotAliasTwoSamplerStates` (`SamplerEmitTest.cpp:290-328`) now asserts the two handles differ, `Collisions == 1`, `Mints == 2`, and — the part the old body could not reach — that the probe **keeps going past the rejected entry** so re-asking for the first value is a hit rather than a third mint. It keeps the one-ULP half as the ordinary-path control. **Negative control: deleting the `memcmp` confirm turns it red.** |
| **C-m9** no case drives the resolver's own release | **TAKEN** — see C-M1's new case. |
| **C-m10** no DirectVulkan arm for the leak cases | **Not C's.** contract-v2 §4.3 assigns the leak cases to package F. The integrator must confirm F's G8b cases run on the DV lane rather than assume it. Recorded, unchanged. |
| **n1** `Evict`'s copy-assignment does not carry padding | **TAKEN**, `SamplerEmit.h:452-458`: the sentence the review asked for, saying the assignment is safe only because every entry's padding was already zeroed by `Mint`'s `memcpy` and `Vector` growth memmoves a trivially copyable type. |
| **n2** `SanityTest.cpp:3898` is a second producer of external index 0 | **Not C's file** (D's). Recorded here so D is told; harmless, it never reaches the emitter, but it is a second producer of the value D5's discriminator reads. |

---

## 5. Negative controls (each patch applied, built, run, reverted)

| control | case | result |
|---|---|---|
| `Reset()` clears `Live` again | `CompositeResolver.ASignatureMoveAfterAMakeCurrentStillReleasesThroughTheResolver` | **red** |
| the constants-key invalidation deleted | `ProgramEmit.AReIssuedCreateReSendsTheDefaultUniformBlock` | **red** |
| the LRU stops skipping pinned entries | `SamplerEmit.AReferencedCsoIsNeverTheLruVictim` | **red** |
| the `memcmp` confirm deleted | `SamplerEmit.AHashCollisionDoesNotAliasTwoSamplerStates` | **red** |

All four reverted; tree clean, rebuild rc 0, all 34 emit cases green again.

---

## 6. Gate numbers (head `00a86d2a`, working tree clean)

| gate | result |
|---|---|
| three builds (pull `build-linux` / push `build-push` / verify `build-verify`), `-j 8`, `CCACHE_BASEDIR=/home/swung/w7` | rc 0 / rc 0 / rc 0 |
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **0 added / 0 removed / 0 resized / 0 renamed.** `.text` 10806323 → 10806323 (+0, +0.000%), 27811 → 27811 defined symbols. rc 0 |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (7 negative controls tripped); 71 calls, 72 verify payloads, 63 PipeInputs fields, 69 verbs, 477 inventory rows, 0 UNMAPPED |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0; 79 mutators all mapped, 0 COARSE, **2 UNDECIDED** (the contract's D6, unchanged) |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4` | rc 0, **4 probes**, 0 skipped, 0 problems, 6 negative controls tripped |
| `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, the eleven byte-identical |
| `ctest -L unit -j 8`, `build-linux` | **100% passed, 0 failed out of 1668** |
| `ctest -L unit -j 8`, `build-push` | **100% passed, 0 failed out of 1668** |
| `ctest -L unit -j 8`, `build-verify` | **100% passed, 0 failed out of 1668** |
| ctest names pull == push | `diff` empty, **2634 == 2634** |
| no baseline name removed (`comm -23` vs `~/w7/p4a-before-ctest-names.txt`, sorted) | **0 removed**; **46 added** (2588 → 2634) = 13 c0 + 1 c0b + 2 c0c + 25 C v1 + **5 C v2** |
| the emit suites, `build-push`, `-R 'SamplerEmit\.\|ImageEmit\.\|ProgramEmit\.\|CompositeResolver\.'` | **100% passed, 34/34** (29 at v1 + 5 new) |
| `ctest --test-dir build-push -L integration-gpu -R DirectGLES -j 8`, default arm | **100% passed, 0 failed out of 491** |
| same, `MOBILEGL_PIPE_PUSH=0x1ff` | **100% passed, 0 failed out of 491** |
| `ctest --test-dir build-push -L integration-gpu -j 8` (whole label), default arm | **100% passed, 0 failed out of 966** |
| `ctest --test-dir build-verify -L integration-verify -j 4` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` | **100% passed, 0 failed out of 844**; `grep -c 'Fatal{'` = **0** |
| `python3 ~/w7/retrace_gate.py --tree /home/swung/w7/p4a-clientsp --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-clientsp-v2 -j 4` | **passed 79 / 79**, failed: `[]`, rc 0; 0 logs contain `Fatal{`; every case reported a real `ssim` (1.0 / 0.999x), never `None` |

**The LFS precondition, stated exactly.** `git lfs checkout` in this worktree prints
`Cannot checkout LFS objects, Git LFS is not installed` — `/usr/sbin/git-lfs` (3.7.1) exists but
`git lfs install` has never configured the smudge filter for this user. It does not matter here:
the fixtures are **already real files**, restored from `~/w7/pipe` by C's v1 (deviation D11) and
hidden with `--assume-unchanged`. Measured before the retrace: **94 files, 803 MB,
`find … -size -1k | wc -l` = 0**, `git status --porcelain` empty. The retrace's per-case
`ssim=1.0` confirms it read real archives rather than the `ssim=None` false red.

---

## 7. c0d, and the two cross-package under-fires (review step 7)

**c0d exists but is NOT on this branch's base.** `refs/heads/p4a/contract` is at **`9ea44389`**
(`[Fix, Test] (Pipe): widen the two shutters that cannot see their own subject rather than gate
their emitters on a second bit …`), which is exactly C-M4 and C-M5. `refs/heads/feat/disaggregated`
is still `17db7598`, and the instruction was to rebase onto that and not to wait. So:

* **C-M4** (`glBindSampler` moves bit 12's generation, not bit 13's, so `bind_sampler_states`
  never fires for a sampler bind) and **C-M5** (bits 6/7/8 read `GetCurrentProgram()`, null for
  the whole life of a bound separable pipeline, so a re-composited pipeline never gets a
  `ShaderCso` handle) are **contract defects in `Tracker.h`, not C's**, and c0d has taken them.
* **Nothing was re-run against c0d**, because it is not on this branch's base. C's unit cases are
  indifferent to it by construction — they drive the emitters directly and never go through the
  dirty walk — but the `integration-gpu` arms are not: with the shutters widened,
  `bind_sampler_states` and the whole program family fire in cases where they previously did not,
  which is new emission traffic through C's code on the default and `0x1ff` arms.
* **The integrator must re-run, after rebasing this branch onto a `feat/disaggregated` that
  carries c0d**: the three `integration-gpu` arms, `integration-verify`, and the emit suites; and
  the sampler-bind and SSO scenarios named in review §10.6 (`ProgramPipelineScenario`'s sixteen
  cases, and any scenario that rebinds a *different* sampler object to a unit) are the only things
  that can see C-M4/C-M5 at all.

---

## 8. What the integrator must re-run

1. **§3 first, before anything else** — B's rework must take `BuiltinSampler` from
   `MGPipeSamplerCsoCacheInstance().Acquire(...)`, hold the reference, and delete its own
   `MGPipeSlots().Acquire(SamplerCso, …)`. With `wire` underneath, a null or a stale
   `BuiltinSampler` is `Fatal{ProtocolCorruption}` on **every** `set_texture_params`. Check B's
   texture cases show the handle coming from the cache, and that `grep -c 'Fatal{'` on an
   `integration-verify` run stays 0.
2. **Rebase onto a `feat/disaggregated` that carries c0d (`9ea44389`)** and re-run §6 in full —
   §7 says why the `integration-gpu` arms in particular are not carried over.
3. `ctest --test-dir build-push -R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.'`
   **with `wire` underneath**: on this branch every applier entry point is still a stub, so the
   `Start + Count` bounds Fatal, the `Parameters.Size` Fatal, the `link`/`spirv` non-null Fatal,
   the composite band's table split and the verify build's program-archive round trip all come
   alive for the first time.
4. **A DirectVulkan arm for the death/leak cases** (ID-8, C-m10) — confirm F's G8b cases really
   run there, or say explicitly that they do not.
5. **After `esprytobj` + `esprytdraw`**: `MOBILEGL_PIPE_PUSH=0x5ff` / `0x9ff` (bit 10 without bit
   11) must be **refused at D's texture-family `Resolve*SubsystemArm`** and logged, never
   half-run — C emits `MGPBoundView::Texture` and `MGPImageView::Res` as `Texture` handles
   unconditionally (`SamplerEmit.h:721`, `ImageEmit.h:97-99`), and ID-15 makes bit 10 require
   bit 11.
6. **The two open items C declined and named**: whether `MGPipeEmitSamplerCsoCreate` gets a call
   site in `SamplerObject`'s constructor (§2, one line, C will add it on request), and
   `MEASUREMENTS.md`'s ID-19 entry for C-m2 (the per-unit acquire latch) and for the one extra
   `set_global_constants` per link noted in C-M2.
7. `retrace_gate.py` **after** confirming `tools/trace_replay/fixtures` are not LFS pointers —
   the false red is `passed 2 / 79` with `ssim=None`.
