# P4a package C — `clientsp`. Focused re-review of the rework, v2

Target: `refs/heads/p4a/clientsp` = **`00a86d2a`** — the three rework commits `02475fbb` / `5f577d16` /
`00a86d2a` on top of the four v1 commits replayed onto `17db7598` (c0 + c0b + c0c).
Sources cross-read: `clientsp-review-v1.md`, `clientsp-v2.md`, `INTEGRATOR-DECISIONS.md`
(ID-14 / ID-17 / ID-18 / ID-22 / ID-25 / ID-26), `contract-v2.md` §3, `contract-v4.md` (c0d).

**Work done:** every file opened at HEAD, the three rework diffs read hunk by hunk, and — unlike v1 —
a **private worktree was built and run**: `/home/swung/w7/p4a-rereview-clientsp` at `00a86d2a`,
`build-push` (`MOBILEGL_PIPE_PUSH=ON`, integration tests off), `-j 8`, `CCACHE_BASEDIR=/home/swung/w7`.
The tree **builds rc 0** and `CompositeResolverTest` / `SamplerEmitTest` / `ProgramEmitTest` link and
pass, which independently confirms the report's §6 build rows for these three targets. The worktree
has been removed. **Not done:** C's full gate set (G1, the three ctest lanes, integration-gpu,
integration-verify, retrace) — the base moves at integration anyway and §5 below says what to re-run.

---

## Verdict — **REWORK (one item)**

Every v1 item is closed, and the two hardest ones are closed *well*: C-M2 is exactly the ruled
one-line invalidation with an honest case and two written-down corrections to the review's own
scenario, and C-M3 is ID-17 implemented as ruled — `Acquire` takes a reference, `Release` drops one,
the LRU skips pinned entries, over-capacity minting is counted, and the emitter's own bound set holds
its pins. The c0b reconciliation is complete and the ten minors are each taken or defensibly declared,
including one (**C-m7**) that is correctly **refuted**: the `EmitterScope` the v1 review said was
missing was already there in v1, and the diff proves it (the line is unchanged context at
`SamplerEmitTest.cpp:456`). The B-facing API in §3 of the report matches the code name for name,
line for line.

The rework nevertheless **over-corrects C-M1** and introduces a defect the v1 tree did not have:

* **C2-M1** — with `Live` no longer cleared by `Reset()`, the first `Observe` from **another context**
  on a pipeline that happens to carry the same GL name releases the *first* context's **live**
  composite: `delete_shader_state` goes out on the wire, the publication latch is cleared and the slot
  is freed, while the frontend `ProgramObject` is still alive. **Demonstrated red on `00a86d2a`**, and
  **green again** when v1's `Reset()` is restored — so it is this round's, not v1's.

The fix is small and local to `CompositeResolver.h` (§2 names it). If the integrator would rather not
spend a third round on C, it is an integrator-side edit in C's tree plus one case; but it cannot be
*declared*, because the file's own header (`CompositeResolver.h:130-142`) states the opposite property
as the reason `Reset()` does not release, and that sentence is now false.

---

## 1. What v1 asked for, and whether it happened

| v1 item | ruling | closed? | evidence |
|---|---|---|---|
| **C-M1** `Reset()` disarms the release path | fix + a case that drives release **through** the resolver after a `Reset` | **YES, but over-corrected — see C2-M1** | `Live` (`CompositeResolver.h:181`) is now the release obligation, cleared only by `ReleaseEntry` (`:195`); `Fresh` (`:183`) is the memo, cleared only by `Reset()` (`:143-145`); the reuse branch re-arms `Fresh` and leaves `Live` (`:105-113`). Case `ASignatureMoveAfterAMakeCurrentStillReleasesThroughTheResolver` (`CompositeResolverTest.cpp:346`). **I ran the stated negative control**: restoring `entry.Live = false` in `Reset()` turns it red on `Releases == 1`. The case does go red on the old code. |
| **C-M2** the `(Cso, Version)` latch survives a re-issue | invalidate on **every** re-issue | **YES** | `ProgramEmit.h:268-271`, on the branch that actually emitted, after `MGPipeApplyCreateShaderState`. Every re-issue reaches it (the early-out at `:209-211` is the only skip and it returns before the emit). `EmitGlobalConstants` acquires at `:172` **before** it consults the latch at `:176`, so an invalidation in the same call is seen. Accessors `:378-379`. Case `ProgramEmitTest.cpp:318`; deleting the four lines makes it red on `EXPECT_TRUE(MGPipeHandleIsNull(GlobalConstantsCso()))`. |
| **the failed-relink case** | — | **correctly re-derived, and the report is right** | `ProgramObject::Link()`'s prologue assigns `m_spirv = {}` (`ProgramObject.cpp:532`) and `GetUBOSize()` reads `Spirv().globalUboScratch.size()` (`ProgramObject.h:736`), so after a *failed* relink the size is 0 and `EmitGlobalConstants` early-outs at `:169`. v1's "the client's scratch still holds the live values" is true of GL and false of this frontend. **This is frontend behaviour, not a push/pull divergence** — the pull path reads the same two getters — so it is not C's to fix, and the fix is still required for the general re-issue. Report §1's correction 1 is also right: `BumpLinkObservableVersions` calls `MarkUBOContentDirty()` beside `++m_linkVersion` (`ProgramObject.cpp:357-358`), so the *behavioural* half is not the discriminator and the case says so. |
| **C-M3 / ID-17** reference counts | cache keeps a per-entry ref count; `Acquire` takes, `Release` drops, LRU evicts only unreferenced, over-capacity minting counted; B's Acquire/Release spelled for B | **YES, per ruling** | see §3 |
| **the c0b list, items 1–8** | three entry points, four latch calls, two comments, one sentence for D, one verify | **YES, all eight** | see §4 |
| **the ten minors + two nits** | fix or declare with a reason | **YES** | see §6 |

---

## 2. C2-M1 [MAJOR, introduced by this round] the resolver now emits `delete_shader_state` for another context's **live** composite

`MG_Impl/Pipe/CompositeResolver.h:102-115` (`Find` + the mismatch arm), `:143-145` (`Reset`),
`:186-191` (`Find`, keyed on the GL name alone), `:193-200` (`ReleaseEntry`), against its own
`:130-142` and `:164-175`.

```cpp
Entry* entry = Find(key);            // key = pipeline.GetExternalIndex(), :101-102
if (entry != nullptr) {
    if (entry->Signature == signature && entry->Handle == handle) { … return handle; }
    ReleaseEntry(*entry);            // :115  <-- now really runs after a Reset()
}
…
void Reset() { for (Entry& entry : m_entries) entry.Fresh = false; }   // :143-145, Live untouched
```

**Failure scenario.** `GLContext` owns `m_programPipelines` **and its own name generator**
`m_programPipelineNames` (`MG_State/GLState/Core.h:658-659`), so pipeline GL name *N* can be live in
two contexts at once, naming two different objects. `MGPipeCompositeResolverInstance()` is a
**process** singleton and `Find` keys on nothing but that name. So:

1. Context A binds its pipeline *N*; `EmitShaderState` mints composite A's handle H<sub>A</sub>,
   publishes `create_shader_state`, and `Observe` records `{name N, sig A, H_A, Live}`.
2. A make-current to context B: `PipeFill.cpp:2227-2229` runs
   `MGPipeProgramEmitterInstance().Reset()` → the resolver's `Reset()`, which now clears **only**
   `Fresh`.
3. Context B binds *its* pipeline *N* and draws. `Observe` finds the entry, the signature and the
   handle both mismatch (two contexts hold two different composite `ProgramObject`s, so the handles
   differ **even when the stage programs are share-group-shared and the signature is identical**),
   and falls into `ReleaseEntry` — which, unlike in v1, is armed. It calls
   `MGPipeEmitShaderCsoDestroyAndFree(A's composite lifetime id)`. Composite A is **alive**, so that
   id still resolves: `EmitDeleteIfPublished` emits `delete_shader_state`, clears the publication
   latch, `NotifyStateObjectDestroyed` fires and the slot goes back to the free list.

This is precisely what `:130-142` says must not happen (*"releasing them would emit a delete for a
live program"*) and what `:164-175`'s benign-recycling argument assumes cannot happen (*"that release
resolves NOTHING — the allocator erases the lifetime-id mapping on Free"* — true when the predecessor
is **dead**, false here).

**Demonstrated, not argued.** A scratch probe in the private worktree drove the resolver exactly as
`EmitShaderState` does — two `ProgramPipelineObject`s built with the same external index, two
composites, `Observe` / `Reset` / `Observe` — on the landed head:

```
CompositeResolver.ScratchProbe_CrossContextPipelineNameAliasing
  Releases                                              : 1 (expected 0)
  MGPipeSlots().IsLive(ShaderCso, hA)                   : false (expected true)
  MGPipeHandleIsPublished(ShaderCso, hA)                : false (expected true)
      -> delete_shader_state went out for a LIVE composite
[  FAILED  ]
```

and with **v1's `Reset()` restored** (`entry.Live = false` put back), the probe is **green** and C's
own new case is the one that goes red. The two cases are in direct tension: with a single
`Live`/`Fresh` pair the resolver cannot tell *"my own entry after a make-current"* from *"another
context's entry"*, and exactly one of the two properties can hold at a time. That is the defect,
stated as sharply as it can be.

**Refutations attempted, and how far each gets.**
(a) *The reuse branch saves it — same signature, same handle.* No: the handle is minted off the
composite object's own lifetime id (`ProgramEmit.h:398-412`), and two contexts have two composite
objects, so `entry->Handle == handle` is false even when the signature matches exactly.
(b) *The old composite is dead by then.* No — that is the single-context case, where the pipeline's
one-slot cache drops the last `SharedPtr` before `Observe` runs. Across a make-current context A's
pipeline **and** its cached composite are alive; that is the stated reason `Reset()` does not release.
(c) *`MGPipeApplierReset` covers it.* It clears `DrawProgram` / `DispatchProgram` / `BoundShaderCso`
(`PipeApply.cpp`, the P4a working-state block) but deliberately **not** the object records — so the
record the delete destroys is exactly the one that was supposed to survive the switch.
(d) *MobileGL is single-context.* `MGPipeApplierReset`'s own comment describes *"a make-current BACK
to a context that is still alive"*, `FreshlyPrimed` exists for it, and v1's C-M1 scenario needs it too.
(e) *Unreachable anyway.* The opposite: **c0d makes it reachable.** Before c0d, bits 6/7/8 read
`GetCurrentProgram()`, null under SSO, so `EmitShaderState` — and therefore `Observe` — essentially
never ran for a bound pipeline (that was C-M5). c0d's `else if (pipeline)` arm
(`contract-v4.md` §2.2, §6 item 2) puts the composite resolver on the hot path *for the first time*.
This finding lands the moment C is rebased onto `e8502a61`.
(f) *It self-heals, so it is cosmetic.* It does self-heal — context A's next `EmitShaderState` misses
`FindByLifetimeId`, allocates a fresh band slot and re-issues `create_shader_state`, so pixels stay
right. What it costs is a **death notice and a backend twin destruction for a live frontend object**
(the D-I1 / P3a C-1 shape), a composite program **rebuild on the server per context switch** — which
for Espryt is the glslang + SPIR-V + spirv-opt rebuild `CompositeResolver.h:42-45` says the whole
signature design exists to avoid — and a window in which A's `ProgramObject` is alive with its band
slot already re-handed out at `gen + 1`. It is not cosmetic.

**Fix.** Give `Entry` a context key beside `PipelineName` and make `Find` require both — the
`GLContext*` the emission is running under, or a per-context serial. Then an entry from another
context is simply *not this pipeline's entry*: it is not found, not matched and not released, its
obligation stays owed to its own context, and both C's new case and this probe are green together.
`Observe` already has everything it needs at the call site (`ProgramEmit.h:116-120` is inside
`EmitShaderState(GLContext& ctx)`); the signature gains one argument. Add a case that pins it: two
pipeline objects with one external index, a `Reset()` between them, `Releases == 0` and the first
composite's slot still live.
**Do not fix it by re-clearing `Live` in `Reset()`** — that is v1's C-M1 back, as run 2 of the control
proves.

---

## 3. C-M3 / ID-17 — the reference count, checked against the ruling

Every clause of ID-17 is implemented and correct.

| ID-17 clause | where | verdict |
|---|---|---|
| the cache keeps a reference count per entry | `SamplerEmit.h:331` | ✔ |
| `Acquire` takes a ref (hit **and** mint) | `:354` / `:410` | ✔ — the mint counting as an acquire is right, and the comment says why |
| `Release` drops one | `:267-275` | ✔ (see C2-m2 for the one thing it should also do) |
| LRU evicts only unreferenced entries | `:361-374` | ✔ — the victim search starts at `m_entries.size()` as "none found" and skips `RefCount != 0`, so a fully pinned cache falls through correctly rather than picking index 0 |
| every entry pinned ⇒ mint beyond 256 and count it | `:376-381`, `Counters::OverCapacityMints` `:223` | ✔ |
| bound sampler states hold a ref while bound, released on unbind | `EmitSamplerStates` `:771-782`, `m_stateRefs` `:996` | ✔ — I walked the reconciliation for all five cases (same handle, moved handle, unit leaving the window, two units sharing one CSO, a unit with no sampler) and it is exactly one reference per binding in every one; `Release(null)` is a no-op so the out-of-window units are free |
| released at a make-current | `Reset()` `:952-959` | ✔ — and it is right that it goes there: `MGPipeApplierReset` clears `BoundSamplerStates` immediately before (`PipeFill.cpp:2224` then `:2227`), so no standing set names those CSOs afterwards |
| B's Acquire/Release spelled for B | report §3 | ✔ — matches the code exactly: `MGPipeSamplerCsoCacheInstance()` `:469`, `Acquire` `:244`, `Release` `:267`, `RefCountOf` `:279`, `RecordIsPublished` `:294`; `Acquire` really does take a **non-const `Uint64&`**, so B needs a real lvalue; both names are in `MobileGL::MG_Pipe`; the include is `<MG_Impl/Pipe/SamplerEmit.h>` and `ImageEmit.h:36` already precedents it |

**The ABA hazard is closed.** A handle a standing record names has `RefCount >= 1`; `Mint`'s victim
search cannot pick it; `Evict` is the only path that frees a `SamplerCso` slot
(`MGPipeEmitSamplerCsoDestroyAndFree` resolves nothing for a content-addressed CSO, confirmed at
`PipeFill.cpp:1265-1272` against `MGPipeSlots().Allocate(SamplerCso)` at `SamplerEmit.h:383`, which
takes no lifetime id). So a texture's params record can never name an evicted handle **provided B
owes exactly one Release per Acquire** — which is what report §3's four rules say, and which the
reviewer of B must check.

**The three cases are honest and the negative controls are real.**
`AReferencedCsoIsNeverTheLruVictim` (`SamplerEmitTest.cpp:526`) drives 512 distinct unreferenced
values through the 256-entry cache, asserts `Evictions > 0` (so the LRU really ran) and
`OverCapacityMints == 0`, and its bookkeeping is right — the two trailing `Release(pinned)` calls
balance the initial `Acquire` and the re-acquire that proves the hit.
`AFullyPinnedCacheMintsBeyondItsCapacityAndCountsIt` (`:559`) pins exactly the capacity and asserts
`Size() == 257 / Evictions == 0 / OverCapacityMints == 1`.
`ABoundSamplerStateHoldsItsCsoUntilTheUnitMoves` (`:494`) pins "one binding is one reference, not one
per pass", which is the clause that would otherwise leak pins for the life of the process.
`AHashCollisionDoesNotAliasTwoSamplerStates` (`:290`) now really can go red for its own name: the
forced-hash seam puts two different values on one hash and the case asserts the handles differ,
`Collisions == 1`, and — the part v1 said was unreachable — that the probe **keeps going**, so
re-asking for the first value is a `Hits == 1` and not a third mint.

---

## 4. The c0b reconciliation — all eight items, verified at the source

| # | claim | verified |
|---|---|---|
| 1 | `MGPipeSamplerEmitter::EmitSamplerCso(SamplerObject&)` | ✔ `SamplerEmit.h:888-892`; forwarded from `PipeFill.cpp:1097-1101`. Spelling exact. |
| 2 | `MGPipeSamplerEmitter::EmitSamplerView(ITextureObject&)` | ✔ `SamplerEmit.h:900-905`; `PipeFill.cpp:1103-1107`. Resolves the texture handle the same way `EmitSamplerViews` does (`:721` vs `:902-903`). |
| 3 | `MGPipeProgramEmitter::EmitShaderCso(ProgramObject&)` | ✔ `ProgramEmit.h:292-295`; `PipeFill.cpp:1109-1113`. |
| — | **compile-time contract really is armed** | ✔ Both wired constants are non-zero (`SamplerEmit.h:84`, `ProgramEmit.h:63`), so `ForwardWhenWired<kWired>` (`PipeFill.cpp:908`) instantiates all three forwards. My `build-push` **rc 0** is the proof; a misspelling would not have linked. |
| 4 | the four latch calls, and nothing else | ✔ `MGPipeNoteHandlePublished(SamplerCso)` `SamplerEmit.h:419` (immediately after `MGPipeApplyCreateSamplerState`), `NoteHandleUnpublished` in `Evict` `:450` **and** in `ResetForTest` `:313`, `NoteHandlePublished(SamplerViewCso)` `:855`, `NoteHandlePublished(ShaderCso)` `ProgramEmit.h:255`. Grepped: no fifth call site. Both headers now include `<MG_Pipe/PipeMutation.h>` directly (`SamplerEmit.h:47`, `ProgramEmit.h:44`). |
| 5 | `RecordIsPublished` / `NoteRecordDestroyed` comments rewritten; the bound-mirror clearing explained | ✔ `SamplerEmit.h:287-293`, `:907-916`, `:925-930`; `ProgramEmit.h:297-308`, `:318-327`. The "why it self-heals" the review asked for is at `ProgramEmit.h:322-327` and it is **correct**: `RecordGen == handle.Gen` refuses a stale latch and the three mirrors hold a generation that can never be handed out again. |
| 6 | the sentence for D | ✔ `SamplerEmit.h:201-205`, and repeated in report §3. It matches what `PipeFill.cpp:1265-1272` actually does. |
| 7 | the two false "it is NOT the emission gate" comments | ✔ rewritten at `SamplerEmit.h:67-83` and `ProgramEmit.h:55-62`, and they are now **accurate**: `wants()` really does consult `kMGPipeWiredSubsystems`, and `FamilyIsLive` (`PipeFill.cpp:885`) is the same pair one level in, as the birth-hook bodies at `:1097-1113` show. |
| 8 | `TheCompositeBandHasExactlyOneDoor` unchanged and green | ✔ unchanged in the diff; **I ran it** — green. |

**The three birth hooks having no call site is correctly declared, and the reasons hold.**
Grepped the whole tree: `MGPipeEmitSamplerCsoCreate` / `MGPipeEmitSamplerViewCreate` /
`MGPipeEmitShaderCsoCreate` are called from nothing outside `PipeFill.cpp` — and neither are
`MGPipeMintTextureHandle` / `MGPipeMintShaderCsoHandle`, which are B's and A's to wire. So **the
emitters are reachable only through the hooks and the validate point**, which is what §4 of the task
asked. D-H4 does put `create_shader_state` at the validate point; `TextureObject`'s constructor is B's
file; and the `SamplerObject` one-liner would make `DirectGLES.cpp:209-214`'s backend-side
`SamplerObject(0)` emit into the pipe from inside `MG_Backend`, which is a real objection. The offer
to add it on request is the right disposition.

---

## 5. The counted refusal in `5f577d16` — what is refused, and is it right

`ProgramEmit.h:233-240`:

```cpp
const SizeT moduleCount = spirv.generatedSpirv.size();
if (moduleCount > 6) ++m_moduleTruncations;
for (SizeT i = 0; i < moduleCount && i < 6; ++i) { … }
```

**What is refused:** the *tail* of a linked snapshot that carried more SPIR-V modules than
`MGPProgramDesc::Spirv[]` (six) can name. Nothing is refused today — `generatedSpirv` is indexed by
`GetLinkedShaderStages()` and `ShaderStage` has exactly six real stages — so this is a guard against a
seventh stage being added later.

**Is it right?** Yes, and it is the correct reading of D-J3. `MOBILEGL_ASSERT` is live only when
`MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`Defines.h:104-115`), i.e. **off in all three
gate builds and every shipped build**, so the v1 code would have truncated silently in exactly the
builds that run. `TruncatedModuleCount()` (`:382`) is the visible artefact and `ResetCounters()`
zeroes it (`:369`). Truncation, not a drop of the whole descriptor, is the safe direction: `StageMask`
and `Spirv[]` come out of the same snapshot, so a truncated tail is a program the server refuses to
build rather than one it builds wrong.
One asymmetry the same round did **not** apply the same rule to: see C2-n1.

---

## 6. The ten minors and two nits — dispositions checked

| # | claimed | verified |
|---|---|---|
| **C-m1** dispatch-through-a-pipeline window | declared | ✔ defensible; the closure genuinely needs `Tracker.h`, and c0d §2.5 confirms only the tracker half is A's — the compute-pipeline half stays C's, correctly recorded |
| **C-m2** no version-first skip before the acquire | declared, ID-19 | ✔ declared — but the cost is **understated**, see C2-m3 |
| **C-m3** the collision branch's second eviction path | **taken** | ✔ `SamplerEmit.h:341-352`: counts and `continue`s. Both halves close, and the case now covers the branch |
| **C-m4** dead `Forget()`, recyclable key | **taken** | ✔ `Forget()` is gone; the key reason is at `:164-175`. But see C2-m1: `HandleFor()` was left behind |
| **C-m5** one-entry opaque-units memo | declared | ✔ perf only |
| **C-m6** compiled-out assert on the module count | **taken** | ✔ see §5 |
| **C-m7** missing `EmitterScope` | **refuted** | ✔ **the refutation is correct.** The `EmitterScope scope;` line is unchanged *context* in the rework diff at `SamplerEmitTest.cpp:456`, so it was present in v1; the v1 review was wrong. The case also now clears unit 2 on the way out, which matters because `EmitterScope::Clear()` resets the emitter *before* the cache, so a leaked binding could not have leaked a pin either way |
| **C-m8** the collision case cannot go red for its name | **taken** | ✔ `AcquireWithForcedHashForTest` `:256-262` + `SamplerEmitTest.cpp:290-327`. See C2-n3 for the one reservation |
| **C-m9** no case drives the resolver's own release | **taken** | ✔ and its negative control is real (I ran it) |
| **C-m10** no DirectVulkan arm | not C's | ✔ still the integrator's to confirm against F |
| **n1** `Evict`'s copy-assign | **taken** | ✔ `:452-458`, and the sentence is accurate |
| **n2** `SanityTest.cpp:3898` | recorded for D | ✔ |

### Hygiene, exit order, ownership — clean

* **No `MGLOG_*`, `TODO`, `FIXME`, `printf`, `std::cout`, `#if 0` or `pGLContext` in any of the four
  headers** (grepped at HEAD). MGLOG discipline is vacuously satisfied — the headers log nothing.
* **No new static holder of a frontend `SharedPtr`.** The one new member is
  `Array<MGPipeHandle, 192> m_stateRefs` (`SamplerEmit.h:996`) — a POD array of `{slot, gen}` pairs.
  `Entry::RefCount` is a `Uint32`. `CompositeResolver::Entry` is unchanged in kind. All six singletons
  are still `static T* p = new T();` and never destroyed (`SamplerEmit.h:473`, `:630`, `:1013`;
  `ProgramEmit.h:475`; `ImageEmit.h:170`; `CompositeResolver.h:210`).
* **Ownership.** The rework touches six files, all C's: three `MG_Impl/Pipe` headers and their three
  `MG_Test/Pipe` suites. `ImageEmit.h` is untouched by the rework. Nothing under `MG_Backend/`,
  `MG_Pipe/`, `scripts/` or `.github/`.
* **No debug leftovers.** The only test-only surface is `AcquireWithForcedHashForTest` (C2-n3).

---

## 7. Findings of this round

### C2-M1 [MAJOR] — §2 above. Demonstrated red on `00a86d2a`, green on v1's `Reset()`.

### C2-m1 [MINOR] `HandleFor()` is dead, so `Fresh` has no live reader and `Reset()` is observably a no-op

`CompositeResolver.h:149-154`, `:183`, `:143-145`.

`Fresh` is written by `Observe` (`:111`, `:125`) and `Reset()` (`:144`) and **read in exactly one
place**: `HandleFor` (`:151`), which has **no caller anywhere in the tree** — grepped `MobileGL/**`,
production and tests alike. So the second half of the Live/Fresh split is unobservable scaffolding and
`MGPipeCompositeResolver::Reset()` now changes nothing any caller can see.

**Failure scenario.** None, today — this is dead weight, not a bug. It matters because it is *the same
class as C-m4*, which this very round deleted (`Forget()` had no caller, so out it went), and because
a reader of `:130-145` will believe `Reset()` does something. It also masks C2-M1: if `Fresh` had been
given a real reader, the question "what distinguishes my entry from another context's after a Reset"
would have had to be answered.

**Refutation attempted.** (a) *It is future API for B or D.* No — the resolver is entirely internal to
`ProgramEmit.h`; nothing else includes `CompositeResolver.h` except its own test. (b) *The new case
reads it.* It does not; the case reads `DrawCso()`, `Releases` and `IsLive`. (c) *Deleting `Fresh`
would re-break C-M1.* Also no — with `Live` no longer cleared, `Reset()` could be deleted outright and
every existing case stays green; the correct answer is a context key (§2), which gives `Fresh` a
purpose or removes the need for it.

**Disposition:** fold into the C2-M1 fix — either give `HandleFor` a caller or delete it, and say which
in the header.

### C2-m2 [MINOR] `Release` is silent on a handle it never handed out, and "a double release is a no-op" is true only when nobody else holds the entry

`SamplerEmit.h:263-275`.

```cpp
void Release(MGPipeHandle handle) {
    if (MGPipeHandleIsNull(handle)) return;
    for (Entry& entry : m_entries) {
        if (entry.Cso != handle) continue;
        if (entry.RefCount > 0) --entry.RefCount;
        ++m_counters.Releases;
        return;
    }
}
```

Two things the comment at `:263-266` promises that the code does not deliver.

1. **An unknown handle falls off the end of the loop and is counted nowhere.** `Releases` counts only
   releases that *found* an entry, so a caller releasing a handle this cache never minted — a stale
   handle from before a `ResetForTest`, a `Texture` handle passed by mistake, a handle whose entry was
   already evicted — leaves no trace at all.
2. **A double release does not "not corrupt the count" when the entry is shared.** Entries are
   content-addressed and shared by design: package B's texture params record and this file's own
   `m_stateRefs` will routinely hold the *same* handle. `Release` is per-handle, not per-holder, so a
   second release from B decrements the pin `bind_sampler_states` is holding. The entry becomes
   `RefCount == 0` while a published `MGPTextureParams` **and** a standing `bind_sampler_states` set
   still name it, the LRU may then take it, and we are back at C-M3 — silently, because the applier
   does not resolve `BuiltinSampler` and nothing counts the extra release.

**Failure scenario.** B holds one ref for texture T's built-in sampler H; unit 3 is bound to a sampler
object with the same parameters, so `EmitSamplerStates` holds a second ref on H (`RefCount == 2`). B's
texture-death path releases H, and B's params-change path — which per report §3 rule 3 also releases
"the previous handle" — releases it again because its per-texture latch was not cleared. `RefCount`
is now 0 with unit 3 still bound. The next `Mint` under pressure evicts H, `delete_sampler_state` goes
out, and the applier's standing `BoundSamplerStates[3]` names a slot that has been re-handed out to a
different value. Wrong filtering, no refusal, no counter.

**Refutation attempted.** (a) *That needs a bug in B.* It does — and this API is being handed to
another package with a written four-rule contract, which is exactly when the callee should make a
contract violation visible rather than absorb it. (b) *An assert would do.* No: `MOBILEGL_ASSERT` is
off in every gate and shipped build (`Defines.h:104-115`) — the finding C-m6 just closed. (c) *The
over-capacity path makes it safe.* That path protects against too many pins, not too few.

**Fix (small):** two counters in the same counted-refusal idiom C-m6 adopted —
`Counters::UnknownReleases` incremented when the loop finds nothing, and
`Counters::UnderflowedReleases` incremented when `RefCount` is already 0 — plus one sentence in the
comment saying the count is per *handle*, not per *holder*, so a holder that releases twice steals
another holder's pin. No behaviour change, and B's review gains something to assert on.

### C2-m3 [MINOR] the refcount reconciliation's cost is understated, and c0d makes it land far more often

Report §1 describes the reconciliation as *"one extra pass over the 192-entry unit array"*, and C-m2's
deferral is budgeted against that sentence. The pass is 192 iterations, but each `cache.Release(held)`
on a **non-null** handle is a **linear scan of the whole cache** (`SamplerEmit.h:269-274`), sitting
beside the `Acquire` probe which is already a linear scan (`:339-357`). So a pass with *K* units
holding sampler CSOs costs `2 · K · O(cacheSize)` handle comparisons, not one extra array walk — up to
~98 000 iterations per firing at `K = 192` with a full cache, and unbounded above that because the
over-capacity path lets `m_entries` grow past 256.

And the firing rate moved under C's feet: **c0d mixes `GetTextureBindGeneration()` into bit 13's
shutter** (`contract-v4.md` §1.2, §6 item 1), so `EmitSamplerStates` now runs on **every texture bind
at any unit**, not only on a parameter change — the contract's own text says C's set-hash suppressor
is *"now load-bearing, not an optimisation"* and that C-m2 *"is worth more after this commit than
before it"*. The suppressor stops the *record* going out; it does not stop the acquire/release walk,
which happens first.

**Refutation attempted.** (a) *K is small.* In Minecraft-shaped workloads K is typically **0** — units
have no `SamplerObject` bound, so `m_states[unit]` is null, `Acquire` is not called and
`Release(null)` returns immediately. This is the honest limit of the finding and it is why it is a
minor and not a major. (b) *A CTS sampler sweep is not a perf lane.* True, but it is a correctness
lane whose wall-clock matters, and it is exactly the workload with large K **and** a large cache.
(c) *ID-19 already carries C-m2.* It does — with the wrong cost model.

**Disposition:** record the corrected model in `MEASUREMENTS.md` under ID-19 beside C-m2, and note
that the per-unit `(sampler lifetime id, GetSamplingResolutionGeneration())` latch C proposes removes
**both** scans, not one.

### C2-n1 [NIT] `Evict`'s `MOBILEGL_ASSERT` is presented as an ID-17 mechanism but is off in every build that runs

`SamplerEmit.h:435-436`. The v2 report's C-M3 table lists *"`Evict` asserts its victim is
unreferenced"* as one of the ruling's mechanisms; `Defines.h:104-115` compiles it out at INFO. It is a
**defensible** debug-only invariant check — the sole caller (`:374-375`) structurally guarantees
`RefCount == 0`, so unlike C-m6 there is no reachable condition being silently swallowed — but it is
the same idiom this round removed from `ProgramEmit.h`, and it is not a mechanism in the shipped
build. Say so in the report row, or make it a counter.

### C2-n2 [NIT] the cache never shrinks after an over-capacity peak

`SamplerEmit.h:361-381`. Once `m_entries.size() > 256`, every subsequent `Mint` evicts one
unreferenced entry and pushes one, so the size stays at its high-water mark for the life of the
process — along with that many `SamplerCso` slots in the allocator and that many `create_sampler_state`
records in the applier. Bounded by the peak simultaneous pin count, so this is memory retention rather
than a leak, and growing was the ruled-safe direction; worth one sentence beside `OverCapacityMints`
so the number is read as "how far the cache permanently grew", not "how often it grew".

### C2-n3 [NIT] `AcquireWithForcedHashForTest` is a public production method, and report §3's "full seam" listing is not full

`SamplerEmit.h:256-262`. It is guarded by nothing but its name and is callable from any translation
unit that includes the header — including package B's `TextureEmit.h`, which is about to. Misuse is
harmless (a value stored under a wrong hash simply mints a duplicate later), and a `#if` seam would
have been worse (the review named this as the honest option), so this is a nit and not a finding. But
report §3 presents a six-member listing as *"the full seam"* while the class has nine public members;
B should be told the three it must not call.

---

## 8. Per-item closure table

| item | ruling | v2 disposition | this review |
|---|---|---|---|
| C-M1 | fix + case through the resolver | Live/Fresh split + case + control | **closed, but over-corrected → C2-M1** |
| C-M2 | invalidate on every re-issue | `ProgramEmit.h:268-271` + case + control | **CLOSED, correct** |
| C-M3 | ID-17 ref counts | ref count + 3 cases + control | **CLOSED, correct** |
| C-M4 / C-M5 | not C's → c0d | not re-fixed | **correct — c0d owns them; C must not touch `Tracker.h`** |
| c0b item 1 | `EmitSamplerCso` | added `:888` | **CLOSED** (build proves the spelling) |
| c0b item 2 | `EmitSamplerView` | added `:900` | **CLOSED** |
| c0b item 3 | `EmitShaderCso` | added `ProgramEmit.h:292` | **CLOSED** |
| c0b item 4 | four latch calls | `:419`, `:450`, `:313`, `:855`, `ProgramEmit.h:255` | **CLOSED** (four sites + the `ResetForTest` one the review asked for) |
| c0b item 5 | comments rewritten, mirrors explained | done | **CLOSED** |
| c0b item 6 | the sentence for D | `:201-205` | **CLOSED** |
| c0b item 7 | two false comments | rewritten | **CLOSED, and now accurate** |
| c0b item 8 | one-door case still green | verified | **CLOSED — I ran it** |
| ID-14 §7 | B's exact call | report §3 | **CLOSED, matches the code** |
| C-m1 / C-m2 / C-m5 / C-m10 | declare | declared | **accepted** (C-m2's cost model corrected — C2-m3) |
| C-m3 / C-m4 / C-m6 / C-m8 / C-m9 / n1 | fix | fixed | **accepted** (C-m4 left `HandleFor` — C2-m1) |
| C-m7 | fix | **refuted** | **refutation upheld — the v1 review was wrong** |
| n2 | tell D | recorded | **accepted** |

---

## 9. What the integrator must re-run, in order

C's tree is at `17db7598`; the pipe is at **`e8502a61`** (c0d `9ea44389` + c0e). **Nothing C owns has
to change because of c0d or c0e** — `wants()`'s map, the emitter entry points and the two subsystem
constants are untouched, and c0e's `MGPipeFramebufferTarget::Named = 3` is not in C's reach. What
changes is *how often C's code is entered*, so the numbers do not carry over.

1. **Fix C2-M1 first** (§2), or record an explicit ruling that concurrent contexts are out of P4a's
   scope **and correct `CompositeResolver.h:130-142`**, which currently claims the opposite property.
   Whichever way, add the two-pipelines-one-name case.
2. **B's seam before anything else** (report §3 / ID-14 / ID-17): B takes `BuiltinSampler` from
   `MGPipeSamplerCsoCacheInstance().Acquire(...)`, holds the reference, releases the previous handle on
   re-acquire and at texture death, and **deletes** its own `MGPipeSlots().Acquire(SamplerCso, …)`.
   With wire underneath, a null or stale `BuiltinSampler` is `Fatal{ProtocolCorruption}` on **every**
   `set_texture_params`. B's review must also confirm B's params gate is keyed on
   `GetTextureParamsVersion()`, and that B owes **exactly one** `Release` per `Acquire` (C2-m2 says
   why a duplicate is silent).
3. **Rebase onto `e8502a61` and re-run C's whole §6 gate set** — three builds, G1 0/0/0/0,
   `gen_pipe.py --check/--self-test`, the generated-file diff, `gen_pipe_dirty_surface.py`,
   `check_include_closure.py --mode both --compiler clang++ --self-test --require-all
   --expect-probes 4` (again after B adds its include), `p3a_untouched_regions.sh 37da3c3a HEAD`,
   `ctest -L unit` ×3, the pull/push name-parity pair and the 0-removed check.
4. **The emit suites**, `build-push`,
   `-R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.'` — 34/34 at this head, plus
   whatever the C2-M1 fix adds. With `wire` underneath these reach a real applier for the first time.
5. **`TrackerTest`** (A's, c0d's own cases) — not C's, but it is the shutter both of C's newly
   hot paths hang off.
6. **The three `integration-gpu` arms (default / `0` / `0x1ff`) and the whole label, and
   `integration-verify` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` with
   `grep -c 'Fatal{'` = 0.** These are the rows c0d invalidates, and it is worth saying exactly why:
   bit 13 now fires on **every texture bind**, so `EmitSamplerStates`' 192-unit walk *and* the new
   refcount reconciliation run on batches where only a texture moved; and bits 6/7/8/14 now fire
   **under a bound pipeline**, so `EmitShaderState` and the composite resolver are exercised for the
   first time. C's own unit cases are indifferent to c0d by construction (they drive the emitters
   directly); these arms are not.
7. **`ProgramPipelineScenario`'s sixteen cases and any scenario that rebinds a *different* sampler
   object to a unit** — the only things that can see the paths c0d opened, and the only place C2-M1
   could show up outside a unit case.
8. **A DirectVulkan arm for the death/leak cases** (ID-8, C-m10) — confirm F's G8b cases run there or
   say plainly that they do not.
9. **After `esprytobj` + `esprytdraw`**: `MOBILEGL_PIPE_PUSH=0x5ff` / `0x9ff` must be **refused** at
   D's texture-family `Resolve*SubsystemArm` (ID-15's fourth D-K2 row), never half-run — C emits
   `MGPBoundView::Texture` (`SamplerEmit.h:721`) and `MGPImageView::Res` (`ImageEmit.h:97-99`) as
   `Texture` handles unconditionally.
10. **`retrace_gate.py`** after confirming `tools/trace_replay/fixtures` are real files — the false
    red is `passed 2 / 79` with `ssim=None`. Any fresh worktree needs `git lfs checkout` first
    (ID-13); note that in C's tree `git lfs install` has never configured the smudge filter, so the
    fixtures are the copied-in real files and `git status` must stay clean.
11. **`MEASUREMENTS.md` under ID-19**: C-m2's per-unit acquire latch **with the corrected cost model**
    (C2-m3), and the one extra `set_global_constants` per link C found while writing the C-M2 case.
12. **The open question C named**: whether `MGPipeEmitSamplerCsoCreate` gets its one-line call site in
    `SamplerObject`'s constructor. C's reasons for declining are good; if the answer is no, the
    entry point stays instantiated-but-uncalled and that should be written into ID-26 rather than
    left as a loose end.
