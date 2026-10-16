# P4a package B — `clientfb`, rework v2

Branch `refs/heads/p4a/clientfb`. Rebased onto `refs/heads/feat/disaggregated` = **`9ea44389`** (c0 + c0b +
c0c + c0d), then three merges (see §1). Addresses `clientfb-review-v1.md` (M1–M4, nine minors) and the
integrator rulings ID-8, ID-12, ID-14, ID-17, ID-18, ID-19(b)(c), ID-21, ID-22.

**Read §7 first.** The gates are green everywhere except one arm, and that arm's failure is a *proved*
consequence of D4's flip landing without package D — not a defect in this package, and the integrator has
an ordering decision to make because of it.

---

## 1. Commits and merged heads

| commit | what |
|---|---|
| `8b910b8c` | merge `refs/heads/p4a/wire` @ **`70e742a3`** (the applier: the `MGPSubRegion` tail, the three acceptance `Bool`s, the per-object `FramebufferRecords` table, the metadata respecify) |
| `e12dab98` | merge `refs/heads/p4a/c0e` @ **`11446f35`** (`MGPipeFramebufferTarget::Named = 3`, the per-object record comment, `MGPSurface::TextureTarget`'s field set) |
| `8534abf7` | merge `refs/heads/p4a/clientsp` @ **`00a86d2a`** — **a fourth head, not in the base recipe; see §1.1** |
| `549ef30b` | `[Fix] (Pipe)` retire wire v3's local `kMGPipeFramebufferTargetNamed` for c0e's enumerator (merge seam) |
| `a390bb1a` | `[Fix] (Pipe, State)` the contract's birth hooks, the D4 flip, the region tail, acceptance-gated flag clears, the metadata respecify, the minors |
| `d013b7ec` | `[Fix] (GLImpl, Pipe)` the two granted files: M2's `set_texture_params` republish and ID-19(c)'s Named records |
| `4ec43194` | `[Test] (Pipe)` twelve new cases and the six that had to change |
| `d1215fbc` | `[Docs] (Pipe)` refresh the one comment the flip made stale — **HEAD** |

Three merges are the base recipe; the fourth is §1.1. **The integrator drops all four** when it rebases this
branch onto the pipe that already contains them, and `549ef30b` goes with them (it is a merge seam, and
ID-21 already assigns that retirement to wire's verification round).

**The heads move under this branch.** At the moment this round closed, `refs/heads/feat/disaggregated` had
advanced to `92dffb81` (from the `9ea44389` this branch is rebased on) and `refs/heads/p4a/wire` to
`712c9467` (from the `70e742a3` merged here); `p4a/esprytobj` is at `ef6beb28` and `p4a/gates` at
`ecf9cb45`. The four hashes in the table above are therefore snapshots of what this round was BUILT and
GATED against, not a claim about the current pipe.

### 1.1 Why `p4a/clientsp` had to be merged, and what it costs

ID-14/ID-17 and this rework's item (4) require `MGPTextureParams::BuiltinSampler` to come from **C's**
content-addressed cache — `MGPipeSamplerCsoCacheInstance().Acquire(params, payloadBytes)` /
`Release(handle)`, spelled in `clientsp-v2.md` §3. On the base recipe's tree
`MG_Impl/Pipe/SamplerEmit.h` is still the **76-line contract stub**: it declares no cache, so the call
does not compile, and c0b's `ForwardWhenWired` idiom cannot help (the cache is reached by a *free function
name*, which is non-dependent and is therefore looked up even in a discarded `if constexpr` arm). There is
no way to write the required call against the stub, so the head that declares it was merged.

The merge conflicted in four files — `MG_Test/Pipe/{CompositeResolver,ImageEmit,ProgramEmit,SamplerEmit}Test.cpp`
— because wire and clientsp both **append** at the same anchor. Resolved by **union**, verified exact:
re-run with `merge.conflictStyle=diff3 -X diff-algorithm=histogram` every conflict region has an **empty
base**, i.e. both sides are pure additions with nothing removed, so dropping the four marker lines is the
whole resolution. Checked afterwards: 0 duplicate `TEST(` names, 0 duplicate `#define`s, one
`MGL_*_TEST_LIST` per file, and G2 (pull == push name sets) is 0 diff lines.

**Consequence the integrator must decide:** ID-19's order is `B v2 -> C v2`. B's landing commit does not
compile before C's, so either the two land together, the order swaps, or B lands with D10 still
identity-addressed and takes the cache at C's verification round (the review's §3.4 option (a)). The code
here assumes they are one landing.

---

## 2. Item by item

### (1) c0b reconciliation — the two-TU deviation is gone

`MG_Impl/Pipe/TextureEmit.h`

* **Deleted** all eight free functions and both destructor call sites (`TextureEmit.h`, the block that was
  `:986-1082`). MG_State now calls only what `MG_Pipe/PipeMutation.h` declares, and **no MG_State
  translation unit includes an emitter header** — the `#include <MG_Impl/Pipe/TextureEmit.h>` is gone from
  `TextureObject.cpp:22` and `RenderbufferObject.cpp:17`, and the second is replaced by
  `#include <MG_Pipe/PipeMutation.h>` (`RenderbufferObject.cpp:12`).
* **Renamed to the contract's spellings**: `EmitTextureCreate` + `EmitTextureCreateFromBase` collapse into
  one `EmitResourceCreate(ITextureObject&)` (`TextureEmit.h:520`); `EmitTextureRespecify` ->
  `EmitResourceRespecify` (`:544`); `NoteLevelDirty(ITextureObject&, Uint32, Uint32)` (`:786`).
  `EmitTextureParams`, `EmitRenderbufferCreate`, `EmitRenderbufferRespecify` already matched.
  `MGPipeEmitTextureParams` was the **name collision** the review predicted; the inline definition is gone.
* **`EmitResourceCreate` reads only what `TextureObjectBase` itself implements.** It is called from that
  class's constructor, so `GetStorageType()` and `GetUploadTargets()` (pure, no body on the base) would be
  undefined behaviour; `GetTarget()`, `GetExternalIndex()` and `GetLifetimeId()` are `override`s **on the
  base**, so they dispatch to the base's own bodies over members the mem-init list has already written.
  The storage kind stays target-derived. Stated at `TextureEmit.h:512-519` and
  `TextureObject.cpp:110-131`.
* **Publication latch**: `Entry::Published`, `TextureRecordIsPublished`, `RenderbufferRecordIsPublished`,
  `NoteTextureRecordDestroyed`, `NoteRenderbufferRecordDestroyed`, `PublishedIn` and `Retire` are all
  deleted. `MGPipeNoteHandlePublished` is called from `PublishCreate` (`TextureEmit.h:1002`) — **only when
  the create was accepted**, because the death helper reads that latch and a latch taken on a refused
  create would emit a `resource_destroy` for a record that does not exist. `MGPipeHandleIsPublished` drives
  the self-healing create at `:566` and `:632`.
* **Call sites repointed**: `TextureObject.cpp:62` (one helper, not two), `:68` `MGPipeEmitTextureResourceRespecify`,
  `:72` `MGPipeEmitTextureParams` (the contract's), `:76` `MGPipeNoteTextureLevelDirty` with `Uint32`s,
  `:125-131` `MGPipeMintTextureHandle` **and** `MGPipeEmitTextureResourceCreate`;
  `RenderbufferObject.cpp:42-48` the mint plus the create, `:54` one helper, `:148`
  `MGPipeEmitRenderbufferResourceRespecify`.

**Declared deviation (the one thing c0b does not give B): the drain list has no clean arm.** The contract's
`MGPipeNoteTextureLevelDirty` carries no `dirty` flag. Rather than ask A to widen the signature or keep a
second B-owned entry point (which would have kept the two-TU coupling the rework exists to remove), the
clean arm is **retired**: a level that goes clean stays on the list until the next drain, where
`!mipmap->IsStorageDirty(...)` is the *first* test `EmitOneLevel` makes and returns "nothing owed", so the
entry is dropped from both lists there. Cost: one `IsStorageDirty` call per cleaned level per drain; the
list is bounded by the (texture, level) pairs dirtied since the last validate point; nothing clears a dirty
flag on this path, so no texels can be lost. `TextureObject.cpp:501` and `TextureObject2DCube.cpp:68`
become `if (dirty) PipeNoteLevelDirty(...)`; `TextureObject.h:235` loses the parameter. Pinned by
`TextureEmit.ALevelMarkedCleanIsCollectedAtTheNextDrain`.

### (2) c0c helper deletions and `MGPSurface::TextureTarget`

* Deleted `MGPipePackSubDataTarget` / `MGPipeSubDataResourceTargetOf` / `MGPipeSubDataUploadTargetOf` and
  their `static_assert` (`TextureEmit.h`, was `:162-176`), and `kMGPipeDepthStencilModeDepth/Stencil`
  (was `:182-183`). `MGPipeDepthStencilModeByte` stays (GLenum -> byte is frontend knowledge).
  The one call site now widens to the contract's `Uint32` parameters (`TextureEmit.h:1003`).
* Deleted `kMGPipeSurfaceKindNone/Texture/Renderbuffer` and their `static_assert`
  (`FramebufferEmit.h`, was `:73-77`).
* **`MGPSurface::TextureTarget` filled**: `static_cast<Uint16>(texture->GetTarget())` on the texture arm
  (`FramebufferEmit.h:139`), `kMGPipeSurfaceNoTextureTarget` on the renderbuffer arm, on the empty-point
  early return and on the two `SurfaceOf` early returns — through one `MGPipeEmptySurface()` helper
  (`:113-118`) so a zero-initialised record cannot say `TextureTarget = 0` (which is `Texture1D`).
* **Added to the hash**: `MGPipeCopySurfaceForHash` copies it (`:196-201`). It is a `PipeFields.def` row, so
  a field-wise copy that skipped it would suppress a record whose only moved field is the attachment's
  texture target — the field three of the four cross-object masks read.
* **m6 folded in**: `MGPSurface::UploadTarget` is `TextureUploadTarget::Unknown` rather than `0` on every
  non-texture point, the same rule for the same collision. `FramebufferEmitTest`'s
  `Color[1].UploadTarget == 0u` assertion moved with it.

### (3) M1 — the region tail

`TextureEmit.h:1029-1033`: `MGPipeApplyResourceSubData(m_lastSubData, shadow, m_regions.empty() ? nullptr :
m_regions.data())`. Null stays correct for the whole-level shape, where `RegionCount` is 0.
New case `TextureEmit.TheApplierStoresTheRegionListTheEmitterBuiltAndNotAnEmptyOne` asserts on
`MGPipeApplier().TextureResources[slot].PendingUploads[...]` — **what the applier stored**, field by field
(`X`, `Y`, `W`, `H`, `SrcOffset`, `SrcRowStride`, `SrcSliceStride`) — rather than on `LastRegions()`, which
is the emitter's own staging vector and is why v1's four region cases could not see the omission.

### (4) M2 + ID-14/ID-17 — the built-in sampler and the thirteenth aggregate site

* **The hook** (`GL_Texture.cpp` grant, three call sites, §3): every `glTexParameter*` that writes
  `GL_TEXTURE_MIN_LOD` / `MAX_LOD` / `LOD_BIAS` lands on the texture's built-in `SamplerObject` and on
  nothing the texture's own params version watches. The three choke points now call
  `MG_Pipe::MGPipeEmitTextureParams(*textureObject)` after their switch (so the error arms return without
  emitting).
* **The version-first skip reads both counters** (`TextureEmit.h:660-673`): `GetTextureParamsVersion()`
  **and** `SamplerObject::GetVersion()`, plus `ForceParamsResync` as a third input because an
  `ImageBindableHint` transition moves neither. This is clientsp-v2 rule 4 and it is what makes the new
  hook free when nothing moved.
* **The cache** (`:675-700`): `MGPipeSamplerCsoCacheInstance().Acquire(sampler->GetAllSamplerParameters(),
  samplerBytes)`; the previous handle is `Release`d when the content moves it, and the duplicate reference
  is handed straight back when the value did not move, so the entry holds **exactly one** reference.
  `MGPipeSlots().Acquire(MGPipeKind::SamplerCso, ...)` is deleted (rule 1). `samplerBytes` is accumulated
  into `SamplerCsoPayloadBytes()` rather than discarded (rule 3's accounting).
* **Texture death** (rule 3): the death helper is A's and does not forward here, so the reference is given
  back at the only moment this package can see a texture die — `RetireIfRecycled` in `AcquireTexture`
  (`:1050-1063`), when the allocator hands the slot out again. Bounded by live texture slots.
  `ResetForTest` releases every held reference too, so a unit case cannot pin the cache for the process.
* **`SamplerResync` stays 0 and stays the server's** (D-E2): the failure it guards is an incomplete texture
  sampling `(0,0,0,1)` after a driver re-mint, set by `RequireImageBindableStorage` /
  `RecreateBackendTexture` on the twin. The client's one request is `ForceResync` after an
  `ImageBindableHint` transition, and it is unchanged. The re-sync ID-14 needs is the **record itself**
  going out again, not a byte.

Cases: `ALodWriteOnTheBuiltinSamplerRepublishesTheParams` (a write only the sampler object sees republishes;
an unchanged texture does not) and `ATexturesBuiltinSamplerHoldsOneCacheReferenceAndSwapsItWithTheContent`.
`EveryTexturesParamsNameItsBuiltinSamplerCso` now asserts `RecordIsPublished(...)` and
`RefCountOf(...) >= 1` and that **no** `SamplerCso` slot is keyed on the `SamplerObject`'s lifetime id.
`TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` no longer skips (a `static_assert` forbids it).

**Declared:** the three `GL_Texture.cpp` call sites themselves are not pinned by a unit case — a
`MG_Test/Pipe` binary cannot drive `glTexParameterf` without a bound, context-registered texture, and
`MG_Test/Texture/TextureTest.cpp` (which can) is not in C.7's list for this package. The unit case drives
the exact call those sites make; the wiring rides the integration lane's texture family.

### (5) M3 — clear on acceptance, never on dispatch

`TextureEmit.h:1029-1064`. `accepted` comes from `MGPipeApplyResourceSubData`; `dispatched` is the
`if constexpr` itself, so a discarded call never clears either. A refused level **stays dirty and stays on
the drain list**, is counted (`RefusedSubDataCount()`) and is loud once (`MGLOG_E_ONCE` naming the
`{slot, gen}` and the level), because a permanently refused level would otherwise re-emit once per verb
for ever. The same rule is applied to the two calls that DEFINE the storage: `PublishCreate` takes the
latch only on acceptance, and `NoteRespecified` advances the descriptor mirror only on acceptance — a
refused respecify must not leave `LastDesc` claiming the applier holds it, or the next identical call is
deduped against a record that was never stored.

`ARefusedUploadLeavesTheLevelDirtyAndOnTheDrainList` constructs a real refusal
(`MGPipeApplierReleaseObjectRecords()`, i.e. the applier holds no record for the handle — the ordinary
shape of a texture born while the subsystem bit was clear) and asserts the flag survives, the drain entry
survives, and the emission was counted as refused.

**A second finding came out of writing it, and it is fixed here.** The self-heal was keyed on the
publication latch alone, and the latch answers *"did a create for this handle go out"* — which stays true
after `MGPipeApplierReleaseObjectRecords` has dropped every object record, the scope a served context's
teardown takes while the frontend objects live on in the share group. So a texture whose record the applier
had dropped would have had **every** later respecify refused, for ever, with no path back. The applier's
own refusal is now the second, authoritative signal: a refused respecify republishes the create and retries
once (`TextureEmit.h:600-621`, and the renderbuffer twin at `:645-657`). One retry, never a loop.
Pinned by the second half of the same case.

### (6) M4 — a mask change after the allocation is a metadata respecify

`NoteTextureBoundAs` / `NoteRenderbufferBoundAs` (`TextureEmit.h:475-521`) return early when the mask did
not actually move, and otherwise call `RepublishMask` (`:1084-1101`), which re-emits **`entry.LastDesc`
with the new mask and hint** — every storage-defining field byte-identical to what the applier holds, which
is exactly the shape wire applies as a metadata update: the descriptor is replaced, the serial advances, no
reallocation is acked and **no pending upload is dropped**. Gated on the family being live and on the
handle being published (before that, the create or the first respecify carries the mask anyway — both read
`entry.BindMask`).

`AnImmutableTexturesImageBindableHintReachesTheApplierAfterItsAllocation`: `glTexStorage`-shaped immutable
texture, a pending upload standing, then the image bind. Asserts `ImageBindableHint == 1` **on the
applier's record**, `BindMask`, that `Width` did not move, that the serial advanced, and that the pending
upload survived. `AnImageBoundTextureCarriesTheImageBindableHintForever` gained the reversed-order half
(allocate, then bind) that v1 could not fail, plus "a second note of the same bit emits nothing".

### (7) ID-19(c) — a record for every framebuffer handed over by name

`MGPipeFramebufferEmitter::EmitFramebufferByName(const FramebufferObject&)` (`FramebufferEmit.h:299-360`).
`Target` is `Named` **only when the object is bound to neither binding**; a bound object gets
`Draw`/`Read`/`Both` instead, because a record always writes `FramebufferRecords[Fbo.Slot]` and a Named
record on a bound object would leave the record saying "no binding" while `BoundFramebuffer` still resolves
through it. Suppressor: a **per-framebuffer** slot-indexed table with the generation checked
(`m_named`, `NamedEntryFor`), so two different objects' Named records in a row both go out; the two bound
latches are untouched, because they answer the different question "does the server's draw/read binding
already hold this record". `Reset()` clears the Named table (safe direction: this emitter cannot tell
`MGPipeApplierReset` from `ReleaseObjectRecords`).

**The GLImpl sites hooked — all in `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp`, which is the only file
in the tree containing a DSA framebuffer entry point.** One `PipePublishFramebufferByName` helper
(`:648`), sixteen call sites:

| kind | entry point | site | where |
|---|---|---|---|
| consumer | `glBlitNamedFramebuffer` | `:672`, `:673` (both objects) | before `MGP_FILL(BlitNamedFramebuffer)` |
| consumer | `glClearNamedFramebufferfv` | `:688` | before `MGP_FILL` |
| consumer | `glClearNamedFramebufferfi` | `:702` | before `MGP_FILL` |
| consumer | `glClearNamedFramebufferiv` | `:716` | before `MGP_FILL` |
| consumer | `glClearNamedFramebufferuiv` | `:730` | before `MGP_FILL` |
| mutator | `glNamedFramebufferTexture` | `:1461` (detach), `:1490` (attach) | after the mutation |
| mutator | `glNamedFramebufferTexture1D/2D` (shared helper) | `:1516`, `:1546` | after the mutation |
| mutator | `glNamedFramebufferTextureLayer` | `:1603`, `:1708` | after the mutation |
| mutator | `glNamedFramebufferRenderbuffer` | `:1780`, `:1792` | after the mutation |
| mutator | `glNamedFramebufferDrawBuffers` | `:1994` | after the delegate |
| mutator | `glNamedFramebufferDrawBuffer` | `:2011` | after the delegate |
| mutator | `glNamedFramebufferReadBuffer` | `:2023` | after the delegate |

The five **consumers** publish before `MGP_FILL`, so the record precedes the verb that hands the object
over and a later bound-target record for the same object still wins. The ten **mutators** publish after
the frontend mutation because they have **no validate point at all** — `FillPoints.def` has no verb for
any of them, so there is no `MGP_FILL` to sit in front of; "the point at which the object is final for
this call" is the only reading of ID-19(c)'s "at their validate point" that exists for them. Inventing a
fill point would be a contract change (`FillPoints.def` is A's).

Not hooked, with reasons: `glNamedFramebufferTexture3D` is a stub that never resolves a framebuffer;
`glCheckNamedFramebufferStatus`, `glGetNamedFramebufferParameteriv`,
`glGetNamedFramebufferAttachmentParameteriv` are pure queries; `glInvalidateNamedFramebufferData/SubData`
validate and do nothing; `glNamedFramebufferParameteri` writes the no-attachment defaults, which the
record's `FillGeometry` reads only when there is no attachment — hooking it would be harmless and it is
declared rather than added, because it is not an entry point that hands the object to the server.
`GetNamedFramebufferObject_State` and `GetFramebufferObjectForNamedClear` were rejected as hook points:
they run **before** the mutation, so the record would describe the previous state.

Cases: `AFramebufferHandedOverByNameGetsANamedRecordWithoutMovingABinding` (the record exists in
`FramebufferRecordFor`, describes the named object, and neither binding moved) and
`ANamedRecordIsSuppressedPerObjectAndNeverAgainstABoundRecord`.

### (8) The nine minors

| # | verdict | where |
|---|---|---|
| m1 cube face -> `targets[0]` | **fixed** | `MGPipeResolveAttachmentUploadTarget` falls back only when the texture has **exactly one** upload target; anything else keeps `Unknown` (`FramebufferEmit.h:100-108`). Case `ALayeredCubeAttachmentDoesNotAssertAFaceItCannotKnow`. |
| m2 draw-buffer tokens >= 8 | **fixed** | `MGPipeDrawBufferIsInsideTheWireWidth` (`:160-172`) + the second refusal loop in `BuildFramebufferState` (`:436-452`). Case `ADrawBufferTokenAboveTheWireWidthIsRefusedNotTruncated`. |
| m3 dangling `Entry&` | **fixed** | the view owner is acquired **before** any `Entry&` is taken; every reference in `EmitResourceRespecify` is taken after the last call that can `resize()` the table (`TextureEmit.h:544-590`). |
| m4 unpublished entries never retired | **fixed** | `RetireIfRecycled` in both `Acquire*` (`TextureEmit.h:441-460`, `:1050-1063`): a generation change resets the whole `Entry` and releases the built-in sampler's cache reference. Case `ARecycledTextureSlotDoesNotInheritItsPredecessorsBindMask`. |
| m5 a Draw record's `ReadSurface` | **fixed, and by c0e rather than declared** | `MGPipeTypes.h` now states that ReadSurface is resolved from **that framebuffer's own** read buffer under every target, Named included. `BuildFramebufferState` lost its `readFbo` parameter (`FramebufferEmit.h:410-425`); the redundant draw emission on a `glReadBuffer` goes with it. Case renamed `EveryRecordsReadSurfaceComesFromItsOwnFramebuffersReadBuffer` and now asserts the draw record's hash does **not** move when the read framebuffer's read buffer does. |
| m6 `UploadTarget = 0` | **fixed** | see item (2). Case `EveryNonTexturePointCarriesTheUnknownSentinelsRatherThanZero`, which also proves the field is in the hash. |
| m7 bit 9 wired, bit 10 unwired | **dissolved** | the D4 flip. The constraint is restated at `TextureEmit.h:92-96` so it cannot be dropped. |
| m8 `SetInternalFormat`'s zero-extent respecify | **declared** | the first `resource_respecify` for a texture still carries `Width = Height = Levels = 0`. It is not byte-equal to the create (the create leaves `InternalFormat` 0), so it is a real respecify to a zero extent, which every applier body tolerates; the real `glTexStorage` respecify replaces it. Gating the publish on `storageDefined` would mean adding a member to `TextureObjectBase`, which resizes the pull build's object and breaks G1. |
| m9 no DSA-named path in the G6 case | **declared** | all four GL entry points funnel into `FramebufferObject::AttachTexture`, so a state-level unit case cannot distinguish them. Item (7) does now give the DSA paths their own coverage, one level up. |

### (9) D4 — the flip, and the named expected-failing set

`TextureEmit.h:98`: `kMGPipeWiredTextureSubsystem = kMGPipeSubsystemTextureResources`, with the
`0-or-its-own-bit` assert beside it. `ArmForTest` / `m_armed` are **deleted** with it and
`MGPipeTextureSubsystemEnabled()` is the config-bit test alone — `PipeFill.cpp`'s
`FamilyIsLive` never consulted that latch, so after the flip it could only lie. Both suites' scopes now
arm `MG_Config::Features.PipePush` (and reset the applier per case, since the emissions actually land).

**The named expected-failing set is EMPTY.** All five are green in `build-push`:

| case | |
|---|---|
| `TextureEmit.TheUnionBoxAndTheRegionListDescribeTheSameTexels` | pass |
| `TextureEmit.AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero` | pass |
| `TextureEmit.AWholeLevelUploadCarriesZeroStrides` | pass |
| `TextureEmit.MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero` | pass |
| `TextureTest.GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined` | pass |

---

## 3. The granted files, and every site in them

C.7 gives these to nobody; both are needed and both are granted under ID-8's rule ("the grant is one call
site per path and it is recorded as a deviation").

| file | why | sites |
|---|---|---|
| `MG_Impl/GLImpl/Texture/GL_Texture.cpp` | ID-18 M2. `SamplerObject::BumpVersion` is `MG_State/GLState/SamplerState`, which is package C's after the tag, so the hook cannot go there. | **three**, one per path: `TextureParameterObject_State` `:1365`, `TextureParameterObjectf_State` `:1467`, `TexParameterf_State` `:2181` — each after its switch, so the error arms return without emitting. `TexParameteri_State`, the four `TexParameter*v_State` families and the DSA `TextureParameter*` all funnel into these three. One include added (`MG_Pipe/PipeMutation.h`, a declaration only). |
| `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp` | ID-19(c). Every DSA framebuffer entry point in the tree is in this one file. | **sixteen**, listed in §2 item (7), plus one helper and one include (`MG_Impl/Pipe/FramebufferEmit.h` — same layer; this file already includes `MG_Impl/Pipe/PipeFill.h`). |

Every added statement in both files is inside a `#if MOBILEGL_PIPE_PUSH` block, so the pull build's
preprocessed text and symbol set do not move (G1 confirms: 0/0/0/0).

---

## 4. Gate numbers — HEAD `d1215fbc`

| gate | result |
|---|---|
| builds | `build-linux` rc 0, `build-push` rc 0, `build-verify` rc 0 |
| **G1** `symbol_report --threshold 0` vs `~/w7/p4a-before-libMobileGL.so` | `.text` 10806323 -> 10806323 (+0, +0.000%); 27811 -> 27811 defined symbols; **0 added / 0 removed / 0 resized / 0 renamed** |
| `gen_pipe.py --check` | generated files are up to date; `git diff --stat MobileGL/MG_Pipe/generated` = **0 lines**; `--self-test` 7 negative-control trips, positive control OK |
| `gen_pipe_dirty_surface.py --check` | only the two known UNDECIDED rows (D6: `BindVertexArray <- NEW_VERTEX_ELEMENTS`, `UseProgram <- NEW_SHADER`), both still marked with the tool's reason |
| `gen_pipe_dirty_surface.py --self-test` | 27 negative controls, all tripped; positive controls OK |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` | **rc 0**, 4 probes, 0 skipped, **0 problems**; self-test 6 negative-control trips (re-run for `TextureEmit.h`'s new `SamplerEmit.h` include and `GL_Framebuffer.cpp`'s) |
| **G5** `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0** — byte-identical |
| `ctest -L unit` `build-linux` | **1756 / 1756** |
| `ctest -L unit` `build-push` | **1756 / 1756** |
| `ctest -L unit` `build-verify` | 1750 / 1756 — **six failures, all pre-existing on the merged base; see §6** |
| **G2** names pull == push | **0 diff lines** |
| **G14** vs `~/w7/p4a-before-ctest-names.txt` | **0 removed**, **+134 added** |
| emit suites (`TextureEmit|FramebufferEmit|SamplerEmit|ImageEmit|ProgramEmit|ResourceEmit|VertexInputEmit|CompositeResolver`) | **148 / 148** |
| `integration-gpu -R DirectGLES -j 8`, **default arm (0x1fff)** | 444 / 491 — **47 failures, one proved cause; see §5** |
| `integration-gpu -R DirectGLES -j 8`, **`MOBILEGL_PIPE_PUSH=0x1ff`** | **491 / 491** |
| `integration-gpu -R DirectGLES -j 8`, **`MOBILEGL_PIPE_PUSH=0`** | **491 / 491** |
| `integration-verify -j 4` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, **0x1ff** | **844 / 844**, **0 `Fatal{` lines** |
| `integration-verify -j 4`, default arm | 735 / 844 — the same 47 scenarios under the Verify label, same cause; **0 `Fatal{` lines** |
| retrace, **0x1ff** (`git lfs checkout` first, 0 pointers left) | **79 / 79** |
| retrace, default arm | 0 / 79 — same cause |

Per-bit bisect of the integration-gpu arm, which is what isolates the cause
(`0x200` framebuffer, `0x400` textures, `0x800` samplers, `0x1000` programs):

| mask | result |
|---|---|
| `0x1ff` (P3a only) | 491 / 491 |
| `0x3ff` (+ framebuffer, bit 9) | **491 / 491** |
| `0x5ff` (+ textures, bit 10) | **444 / 491** |
| `0x9ff` (+ samplers, bit 11) | **491 / 491** |
| `0x11ff` (+ programs, bit 12) | **491 / 491** |
| `0xdff` (+ textures + samplers) | 444 / 491 |
| `0x1fff` (default) | 444 / 491 |

**Exactly bit 10, and exactly the same 47 every time.** The full list of 47 is at
`~/w7/p4a-clientfb-47.txt` while this round's logs stand; every one of them is a scenario that uploads
texels and reads them back (`SwizzleAccessRoutine`, `CopyImage*`, `PackedWordReadback`, `GuiBatch`,
`ClearTexImageUndefinedLevelZero`, `LayeredAttachment*`, `FormatlessImageBake`, ...), and the failure shape
is uniform: `textureGradOffset(...).w returned 0x0 instead of 0xffffffff at texel 0 (64 of 64 wrong)` —
the texels never reached the driver.

---

## 5. THE ONE THING THE INTEGRATOR HAS TO DECIDE: bit 10 needs package D, not just wire

**Negative control, and it is unambiguous.** On the default arm, with the emission left exactly as it is
and **only** the client-side dirty-flag clear removed (`EmitOneLevel`'s
`mipmap->MarkStorageDirty(uploadTarget, level, false)` put behind a probe environment variable):

| arm | result |
|---|---|
| default, client clear **present** (HEAD) | 444 / 491 |
| default, client clear **removed** | **491 / 491** |

So all 47 integration failures, the 109 integration-verify failures and the 0/79 retrace have **one cause**:
D-D5 step 1's inversion has landed its client half and this tree has no server half.

`DrainTextureSubData` clears the frontend's per-level dirty flag as soon as the applier accepts the record.
The texels then live only in `MGPipeResourceRecord::PendingUploads`, which is **package D's** to consume —
Espryt's texture arm reads the applier's pending set instead of the frontend flags. Neither `p4a/esprytobj`
nor `p4a/esprytdraw` is in this tree, so the base Espryt still reads `IsStorageDirty()`, finds it clear, and
uploads nothing. This is the inversion working exactly as designed with one half present, not a defect in
this package: the client half is required and correct, and it is destructive on its own.

**It is not fixable client-side.** The client cannot ask whether a server-side consumer exists, and keeping
the frontend flag set would double-upload once D lands.

**Recommendation, and it is the integrator's call:**

1. **Land B's D4 flip together with package D's texture arm** — the one line `TextureEmit.h:98` is the whole
   switch, so it can equally be moved into D's commit and B lands with the constant at 0 and everything else
   in place. That keeps every intermediate tree green and keeps a bisect meaningful, which is why it is the
   recommendation.
2. Or land it here and run every gate between B and D at `0x1ff`, accepting that the default arm and the
   retrace are red for that window.

Either way **D-K2 gains a row**, and it is stronger than the two it already has: *bit 10 requires the
server-side texture consumer*, not merely bit 11. Bits 9, 11 and 12 have no such dependency — the bisect
above shows each of them alone is 491/491, because their emissions are records the server may ignore, while
bit 10's emission **takes state away from the legacy path**. That asymmetry is worth recording in the brief
beside D-K2, because it is the only P4a subsystem bit that is not independently switchable.

---

## 6. The six `build-verify` unit failures are not this package's

| case | suite owner |
|---|---|
| `SamplerEmit.ARecordThatDoesNotDescribeItsOwnParametersIsRefusedNamingTheLength` | wire |
| `SamplerEmit.AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated` | wire |
| `ImageEmit.AnImageWindowPastTheImageUnitSpaceIsRefusedRatherThanTruncated` | wire |
| `ProgramEmit.ACreateWithNoArtefactsAnOversizedBlockOrACorruptSlotIsRefusedNamingTheProgram` | wire |
| `ProgramEmit.TheDefaultUniformBlockLandsOnTheProgramsRecordAndTheSentinelIsRefused` | wire |
| `CompositeResolver.ASlotAtTheShaderCsoLimitIsRefusedWhileTheLastBandSlotIsNot` | wire |

All six are wire-authored **applier** refusal cases, verify lane only (`build-push` is 1756/1756), and none
is in either of B's two suites. They fail as `the gate fired without naming what it refused ... log: ` with
an **empty** child log: the forked child does die of the abort, and produces no log text in a
`MOBILEGL_PIPE_VERIFY` build.

**Control run.** `git checkout 8534abf7 -- <the eight library files B changed>` (the four conflicted test
files are untouched by B's four commits — `git diff --stat 8534abf7..HEAD` lists them nowhere), rebuild
`build-verify`, re-run the six: **0 / 6 passed, the same six, identically**. So they predate B's rework and
belong to the `wire v3` x `clientsp v2` seam on the verify lane. The integrator sees them on the integrated
tree whether or not B lands.

---

## 7. What the integrator must re-run, and in what order

1. **Decide §5 first.** Everything below is conditional on it. If the flip moves into D's commit, re-run the
   default arm and the retrace there and expect 491/491 and 79/79; if it stays here, the window between B
   and D is knowingly red on those two and must be gated at `0x1ff`.
2. **The integration order question in §1.1**: B's landing commit does not compile before `p4a/clientsp`.
3. On the integrated tree, the residual-record sweep ID-12 names: the 183 "no applier record" refusals must
   go to zero. **Not answerable here** — the string does not occur once in this tree's integration logs,
   because the arms that emit it are package D's and are not present. Report deferred to D's round.
4. `integration-verify` on the **DirectVulkan** lane for the per-kind leak test (ID-8): this tree's
   `integration-verify` label runs the DirectGLES scenarios; `~TextureObjectBase` and `~RenderbufferObject`
   reaching a live applier is exercised by `TextureEmit.ADestroyedTextureReleasesItsResourceViewAndBuiltinSamplerSlots`
   and by `HandleRecycle`, both green, but the DirectVulkan arm of the leak test is the integrator's.
5. The **G9 white-box assertion** (ID-19/ID-22) and package F's `0x5ff` / `0x9ff` dependency-refusal arms:
   `0x5ff` is 444/491 here for §5's reason and **not** for D-K2's "bit 10 requires bit 11", so F's scenario
   cannot be read off this tree either.
6. The six of §6, once wire's verification round has looked at the verify-lane trip-wire logging.
7. `549ef30b` is a merge seam: drop it with the merges, or keep it if wire's verification round has not yet
   retired `kMGPipeFramebufferTargetNamed` itself.

---

## 8. Deviations declared in this round

| # | deviation | reason |
|---|---|---|
| **D12** | the drain list has **no clean arm** | §2 item (1). The contract's hook carries no `dirty` flag; the collection moves to the next drain, where `IsStorageDirty` is already the first test. No texels can be lost by it. |
| **D13** | `p4a/clientsp` merged as a fourth head | §1.1. The ID-14/ID-17 call does not compile against the contract stub and `ForwardWhenWired` cannot reach a free function name. |
| **D14** | the three `GL_Texture.cpp` call sites are not pinned by a unit case | §2 item (4). A `MG_Test/Pipe` binary cannot drive `glTexParameterf`; the file that can is not in C.7's list for this package. |
| **D15** | the ten DSA **mutators** publish *after* the mutation rather than at a validate point | §2 item (7). They have no validate point — `FillPoints.def` has no verb for any of them, and that file is A's. |
| **D16** | m8 (`SetInternalFormat`'s zero-extent respecify) and m9 (no DSA-named path in the G6 case) declared rather than fixed | §2 item (8). |
| **D17** | the emitter's descriptor dedupe can suppress a respecify the applier no longer holds | The dedupe compares against B's mirror of the accepted descriptor. After `MGPipeApplierReleaseObjectRecords` the mirror is stale and a respecify that moves no field returns before reaching the applier, so the self-heal of §2 item (5) cannot fire for it. The refused **upload** is loud once per handle and the next real storage change heals it. Fixing it would mean asking the applier on every respecify, which is the roundtrip the dedupe exists to avoid. |

*Retired from v1:* D1 (the two-TU deviation — c0b), D2/D7/D9 (adopted into c0c), D3 (c0b's death helpers),
D4 (flipped), D10/D11 (ID-14/ID-17: the cache, no per-texture `SamplerCso` mint, nothing to leak).
D5, D6, D8 stand unchanged.

---

## Appendix — the 47 default-arm integration failures, by name

Identical on every mask containing bit 10 (`0x5ff`, `0xdff`, `0x1fff`); **all 47 pass** with the
client-side dirty-flag clear removed (§5's negative control), and all 47 pass at `0x1ff`.
Every one uploads texels and reads them back.

```
DirectGLES.ClearTexImageUndefinedLevelZeroScenario.AnOrdinaryLevelZeroTextureStillReadsBackCorrectly
DirectGLES.ClearTexImageUndefinedLevelZeroScenario.ReadBackALevelSeparatedFromLevelZeroByAGap
DirectGLES.CopyImageLayeredScenario.ArrayToArrayCopiesEverySlice
DirectGLES.CopyImageLayeredScenario.ArrayToArrayHonoursDifferentLayerOffsets
DirectGLES.CopyImageLayeredScenario.ArrayToVolumeCopiesEverySlice
DirectGLES.CopyImageLayeredScenario.VolumeToArrayCopiesEverySlice
DirectGLES.CopyImageLayeredScenario.VolumeToVolumeAtNonZeroMipLevel
DirectGLES.CopyImageLayeredScenario.VolumeToVolumeHonoursNonZeroZ
DirectGLES.CopyImageLevelRangeScenario.AValidLevelZeroCopyStillMovesPixels
DirectGLES.CopyImagePacked16Scenario.ArrayMipLevelLandsInFlatImageIntact
DirectGLES.CopyImagePacked16Scenario.FlatImageLandsInArrayMipLevelIntact
DirectGLES.CopyImagePacked16Scenario.RenderbufferLandsInArrayMipLevelIntact
DirectGLES.EmptyScissorScenario.AnExplicitlyEmptyScissorBoxClipsEveryFragment
DirectGLES.EmptyScissorScenario.IndexedZeroDimensionScissorBoxesClipEveryFragment
DirectGLES.FormatlessImageBakeScenario.CoreFormatBakedFromTheBoundUnitIsUnaffected
DirectGLES.FormatlessImageBakeScenario.R16uiBakedFromTheBoundUnitStillReachesTheDriver
DirectGLES.FormatlessImageBakeScenario.R8uiBakedFromTheBoundUnitStillReachesTheDriver
DirectGLES.FramebufferChurnScenario.RepeatedFramebufferReadbackStaysExact
DirectGLES.Glsl420DeclarationScenario.SamplerArrayElementsSampleConsecutiveTextureUnits
DirectGLES.GuiBatchScenario.Baseline
DirectGLES.GuiBatchScenario.BlendAndDepth
DirectGLES.GuiBatchScenario.FullFidelityWithRegrowth
DirectGLES.GuiBatchScenario.MeshesBlockLeftUnbound
DirectGLES.GuiBatchScenario.ShortIndices
DirectGLES.GuiBatchScenario.ThreeFramesWithoutRelayout
DirectGLES.GuiBatchScenario.TwoBuildersBaseVertex
DirectGLES.GuiBatchScenario.TwoBuildersShortIndicesBaseVertex
DirectGLES.GuiBatchScenario.TwoBuildersWideIndices
DirectGLES.IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour
DirectGLES.LayeredAttachmentBarrierScenario.BlitBetweenNonZeroColorLayers
DirectGLES.LayeredAttachmentBarrierScenario.ReadPixelsOffRenderedNonZeroLayer
DirectGLES.LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice
DirectGLES.NonCoreImageFormatScenario.NormalizedImageCarriesEveryLayerFaceOfACubeMapArray
DirectGLES.NoViewportArrayEmulation.ViewportArrayScenario.WithoutTheEmulationEveryIndexCollapsesOntoViewportZero
DirectGLES.PackedWordReadbackScenario.ACopiedRgb9E5WordSurvivesInAnR11fG11fB10fDestination
DirectGLES.PackedWordReadbackScenario.ACopyThroughARenderbufferReachesAnRgb9E5Destination
DirectGLES.PackedWordReadbackScenario.AnUploadedPackedWordReadsBackVerbatim
DirectGLES.PipelineFailureScenario.SamplerArrayInAStructDrawsWithoutKillingTheProcess
DirectGLES.PipelineFailureScenario.TheSameDrawRepeatsIdenticallyWithNoPoisonedPipelineCache
DirectGLES.ProgramPipelineScenario.ASamplerUnitRewrittenBetweenDrawsKeepsPaintingTheRightTexture
DirectGLES.SwizzleAccessRoutineScenario.EveryAccessRoutineFetchesTheSameTexelUnderTheIdentitySwizzle
DirectGLES.SwizzleAccessRoutineScenario.EveryAccessRoutineSeesAReversedSwizzle
DirectGLES.SwizzleAccessRoutineScenario.RepeatedProgramBuildsKeepFetchingTheSameTexel
DirectGLES.ViewportArrayScenario.AnIndexedScissorEnableClipsOnlyThatIndex
DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.ArrayMipLevelLandsInFlatImageIntact
DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.FlatImageLandsInArrayMipLevelIntact
DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.RenderbufferLandsInArrayMipLevelIntact
```

The `integration-verify` default-arm residue (109 of 844) is the same scenarios under the
`DirectGLES.Verify.*` name plus their DirectVulkan twins; `0x1ff` is 844/844 with 0 `Fatal{` lines.
