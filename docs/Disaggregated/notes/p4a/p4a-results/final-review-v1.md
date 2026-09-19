# P4a final whole-diff review — v1

Reviewer: final whole-diff review (fable). Tree reviewed: `~/w7/pipe` at `a38bdab4` = code head
`6035c9d7` (+ the tools-only `MOBILEGL_TRACE_SKIP_INSTALL` commit). Phase diff read in full:
`git diff 37da3c3a 6035c9d7` (81 files, every hunk, including the tests, the harness, the scripts and
the workflow). Experiments ran in the throwaway worktree `~/w7/p4a-finalreview` (@ `6035c9d7`,
`build-push` / `build-linux`, base tree `~/w7/p4a-finalreview-base` @ `37da3c3a`) after
`~/w7/p4a-bench.done` appeared; the worktrees are removed at the end of this round (§9). Nothing was
committed to `pipe`.

Reading order, as instructed: `INTEGRATOR-DECISIONS.md` (ID-1..49), `BRIEF-P4A.md` A/D/E/F,
`fable-seam-audit.md`, `fablefix-v1.md`, then contract-v7, wire-v3 + review-v2, clientfb-v2 +
review-v2, clientsp-v3 + review-v2, esprytobj-v3 + review-v2, esprytdraw-v3, gates-v3, and the three
docs. Findings below were refuted against the code before being written; the refutation attempted is
stated under each.

---

## 0. Verdict

**REWORK** — two critical defects in the texture-resource family's CLIENT half (package B's
`TextureEmit.h`), both structural, both invisible to every gate that shipped, both with a small and
well-bounded fix:

* **C-1** — `TextureEmit.h` never passes wire's `MGPRespecifiedLevel`, so every per-level
  `glTexImage*D` / `glCopyTexImage2D` / `glGenerateMipmap` grow clears the applier's WHOLE
  pending-upload set. A level accepted at one verb and defined-around at the next is uploaded by nobody;
  the client's flag is already clear (D-D5 step 1). Silent, no counter, no log line.
* **C-2** — a dead-but-not-recycled texture handle still resolves to the freed `ITextureObject*` inside
  the emitter, and the drain list is not emptied at death. `glTexImage2D` → `glDeleteTextures` → next
  verb walks freed memory (virtual `GetStorageType()` on it), and keeps walking it once per verb until the
  slot is recycled.

Both were confirmed by probes in the review worktree (§2), and C-1's shape was documented as B's
obligation by wire-v3 §5 item 5 and never checked by clientfb-review-v2. Everything else in the phase
is CLOSE-quality: the contract as landed is consistent (one encoding per field, six death helpers in one
fixed order, c0f/c0g complete and non-transitive on both sides, the shutter table complete after the
fable round), the applier is a per-object table with loud stale-generation refusals, Espryt's handle arms
refuse loudly at every one of the 27 decline sites I re-walked, the exit-order rule holds, no debug or
temporary-base artefact leaked, and every gate I mutated went red for the reason it exists (§4) except
the one declared limitation in §4.3.

The rework is one package (B) plus one contract signature (A), ≈ 6 files + 2 tests, and does not
reopen D/E/F. After it lands: re-run the list in §8. The recorded items (§5) and the D.5 docs checklist
(§6) are unchanged by the rework.

---

## 1. Findings

### CRITICAL

#### C-1 — the client never passes the level scope; a per-level respecify drops every pending upload

**Where.** `MobileGL/MG_Impl/Pipe/TextureEmit.h:955-960`:

```cpp
static Bool ApplyRespecify(const MGPResourceDesc& desc) {
    if constexpr (MGPipeTextureRecordsReachTheApplier()) {
        return MGPipeApplyResourceRespecify(desc, nullptr);   // third parameter defaulted: level == nullptr
    }
```

`MGPipeApplyResourceRespecify(desc, initialBytes, const MGPRespecifiedLevel* level)`
(`MG_Pipe/PipeApply.cpp:1612-1613`) then takes the whole-resource arm at `:1684-1685`:
`else if (level == nullptr) { record->PendingUploads.clear(); }`. The non-test tree contains exactly
three mentions of `MGPRespecifiedLevel`: `PipeApply.h`, `PipeApply.cpp`, and a comment in
`Managers.cpp`. No emitter constructs one. The hook the frontend calls
(`MG_State/.../TextureObject.cpp:68-69` `PipePublishDescriptor()` → `MGPipeEmitTextureResourceRespecify(*this)`,
`PipeMutation.h:320`) carries no level at all, although every caller of it — `AllocateStorage`
(`TextureObject.cpp:527`, `TextureObject2DCube.cpp:68`) — is per `(uploadTarget, level)`.

**Why the descriptor moves on a level-1 definition** (so the metadata-only arm does not save it):
`TextureEmit.h:239` `desc.Levels = mipmap->GetMipmapLevelCount()`, and `GetMipmapLevelCount()` is
`m_textureStorage.GetLevelCount()` (`TextureObject.cpp:471-473`), which grows as levels are defined.
`RespecifyRedefinesNoStorage` (`PipeApply.cpp:558-568`) compares `Levels`, so the call is classified as a
redefinition and the clear runs.

**Failure scenario** (any of these, application-side, all legal GL):

1. `glTexImage2D(L0, data)` → any verb that does not reach the texture (a draw with another texture, a
   `glClear`, `glReadPixels`, `glGetTexImage` on another texture, `glBindImageTexture` — every
   `MGP_FILL` point drains) → `glTexImage2D(L1, data)` → draw sampling the texture.
   At the first verb the drain emits L0, the applier accepts it, the client clears L0's flag
   (`TextureEmit.h:1097`). Espryt does not sync the texture (bound nowhere / not sampled), so the
   applier's entry is the only owner of those texels. The L1 definition respecifies with `level ==
   nullptr` → `PendingUploads.clear()`. At the sampled draw the handle arm allocates from the descriptor
   (`MGB_STORAGE_*`, `Managers.cpp:6315-6333`) and uploads only what `FindPipeTextureUpload` returns
   (`MGB_LEVEL_NEEDS_UPLOAD`, `:6262-6265` — no frontend fallback on the handle arm): L1 only. Level 0
   is allocated undefined. No refusal, no counter, no line.
2. The same with `glGenerateMipmap`: `GL_Texture.cpp:6782-6783` runs
   `EnsureGeneratedMipmapStorageAllocated` (one `AllocateStorage` → one respecify per level, `:505-550`)
   BEFORE `GenerateMipmap_Backend` (`MGP_FILL(GenerateMipmap)` at `:1675`). A texture whose level 0 was
   accepted-but-unconsumed loses it inside the generate; the driver builds the chain from garbage.
3. Cube maps: six faces of level 0 defined across verbs — each face's `AllocateStorage` respecifies and
   clears the other faces' pending uploads.
4. Texture views: `TextureObjectView::AllocateStorage` forwards to the owner (`TextureObjectView.cpp:272`),
   so a per-level definition through a view clears the OWNER's set.

**Evidence.**
* Applier+client probe (worktree, `TextureEmitTest` with the real frontend `AllocateStorage` /
  `MarkStorageDirtyRegion` and `DrainTextureSubData`): `FinalReviewALevelDefinedAfterAnEmittedButUnconsumedUploadKeepsThatUpload`
  → **RED**: after the level-1 definition, level 0 is no longer pending and its client flag is CLEAR;
  after the next drain only level 1 is pending (`~/w7/p4a-finalreview-exp.log`).
* End-to-end (worktree itest `FinalReviewC1Scenario`, DirectGLES, llvmpipe): **cases 1 and 4 read a
  100 % black level 0 on the handle arm (`0x1fff`) and red on the legacy arm (`0x1ff`); the three
  controls are red on both** (§2.1).

**Refutation attempted.**
* "Espryt's incomplete-texture bail keeps the level pending until the chain is complete": the bail at
  `Managers.cpp:6480` is `!IsComplete() && !HasAnyDefinedMipmapLevel(...)` — it only bails when NO level is
  defined. So when the texture IS reached, L0 is consumed at the first sync and the hazard closes; the
  hazard needs the texture to be unreached between the two definitions. That is the ordinary loader
  shape (progress bars, one-level-per-tick uploaders, atlas stitchers that upload level 0 and mip later).
* "The client re-dirties": nothing in MG_Impl re-marks a level; the only re-dirty is
  `RearmPipeTextureLevelUpload` on a backend bail, which needs the set to still hold the entry.
* "glTexStorage / glBufferData are whole-resource anyway": yes, and those are the calls that SHOULD pass
  `nullptr` (wire-v3 §5 item 5 lists them). The bug is that everything passes `nullptr`.
* "The applier could infer the scope": it cannot — a `Levels` move from 1 to 2 and a level-0
  redefinition at a new format both change the descriptor; only the caller knows which level moved.

**Why no gate saw it.** `TextureEmitTest.cpp:1567-1645` and `:1647-…` (wire's C1 cases and gates'
MINOR-1 cube case) drive `MGPipeApplyResourceRespecify(..., &levelZero)` DIRECTLY — they prove the applier's
arm and assume B passes the pointer (`:1584-1591` even says so in prose). No unit case drives
`AllocateStorage` twice around a drain. clientfb-review-v2 verified M1 (the region tail) at
`TextureEmit.h:1068-1069` and did not verify wire-v3 §5 item 5. No itest or retrace fixture defines two
levels of one texture around a verb without sampling it in between (the itest textures are drawn
right after their upload; MC defines every level with NULL data in one burst and sub-images later).

**Fix path** (B + A, one commit, ≈ 6 files):
`PipeMutation.h:320` `MGPipeEmitTextureResourceRespecify(ITextureObject&, const MGPRespecifiedLevel* = nullptr)`;
`TextureObject.h:235` / `TextureObject.cpp:68` `PipePublishDescriptor(TextureUploadTarget, Uint level)`
called from `AllocateStorage` with the level and from `TruncateMipmapLevels` / the view / immutable
paths with none; `PipeFill.cpp:1258` forwards; `TextureEmit.h:955` builds the key with
`MGPipePackSubDataTarget(MGPipeResourceTargetForTextureTarget(target), uploadTarget)` — the SAME packed
`Target` the drain uses at `:1037-1039` (wire-v3 §5 item 5's exact requirement), and `RepublishMask`
keeps `nullptr`. Tests: the applier+client probe above becomes a permanent `TextureEmitTest` case; the
itest in §2.1 (cases 1 and 4 with their controls) becomes a lane.

#### C-2 — a dead texture handle resolves to freed memory in the drain; the drain list survives the death

**Where.** `TextureEmit.h:446-453`:

```cpp
ITextureObject* ResolveTexture(MGPipeHandle handle) const {
    ... if (entry.Texture == nullptr || entry.Gen != handle.Gen) return nullptr;
    if (MGPipeSlots().GenOfSlot(MGPipeKind::Texture, handle.Slot) != handle.Gen) return nullptr;
    return entry.Texture;
```

`MGPipeSlotAllocator::Free` (`SlotAllocator.cpp:184-207`) sets `Live = false` and pushes the slot on the
free list; the generation moves only in `AllocateFor` (`:90` `++entry.Gen`) at the NEXT acquire. So
between the death and the recycle, `GenOfSlot == handle.Gen`, `entry.Texture` is the freed object, and
`ResolveTexture` hands it back. The death helper (`PipeFill.cpp:1391-1395`
`MGPipeEmitTextureDestroyAndFree`) does not forward to the emitter — `TextureEmit.h:927-929` says so in
its own words ("the death helper is A's ... and does not forward to this emitter") — and
`RetireIfRecycled` (`:931-935`) clears the entry only at re-acquire. Nothing removes the slot's
`DrainKeys` / `m_drain` entries at death.

**Failure scenario.** `glGenTextures; glTexImage2D(data)` (or `glTexSubImage2D`) — the level is on the
drain list — `glDeleteTextures` (the last reference; the frontend destructor runs the helper) — any verb.
`DrainTextureSubData` → `EmitOneLevel` (`:998-1005`) → `ResolveTexture` returns the dangling pointer →
`AsMipmapTexture(texture)` calls virtual `GetStorageType()` (`TextureObject.h:342-346`) on freed memory →
`IsStorageDirty` reads freed `MipmapStorage`. If the allocator has not scribbled the block the read
succeeds, a `resource_subdata` is emitted for a record the wire delete has already dropped, the applier
refuses it (a counted no-op, `:1082-1083`), `EmitOneLevel` returns false and the entry STAYS on the list
(`:1090-1095`, "retried at the next validate point") — so the freed object is dereferenced once per verb
until a new texture recycles the slot. In a process with many texture creates this is short; in one
that deletes a temporary texture and then draws for a long time it is a per-draw UAF read plus one
`MGLOG_E_ONCE` line. `MALLOC_PERTURB_` / ASan / a reused block turns it into a crash.

**Evidence.**
* Emitter probe (worktree, `TextureEmitTest`): `FinalReviewADeadTexturesHandleStillResolvesAndTheDrainWalksIt`
  → `IsLive == false` passes, `ResolveTexture(handle) == 0x5a88f04261f0` (expected `nullptr`), and
  `DrainListSize()` stays 1 after the death (`~/w7/p4a-finalreview-exp2.log`).
* End-to-end (worktree itest, case 6 of `FinalReviewC1Scenario`, 8 create-upload-delete-draw rounds,
  plain and under `MALLOC_PERTURB_=165`, both arms): **SIGABRT `pure virtual method called` on the
  handle arm in the first round, with and without the perturb; the legacy arm passes** (§2.1). This is
  a hard crash on `glTexImage2D; glDeleteTextures; <any verb>` at the shipping default mask.

**Refutation attempted.**
* "The `GenOfSlot` compare is the guard": it guards a RECYCLED slot, not a dead one — the design comment
  at `:442-445` ("the entry exists only between the create the constructor emits and the destroy the
  destructor emits") describes an invariant no code establishes.
* "The applier refuses, so nothing reaches the server": true for the server — the client-side deref is
  the hole, and the refusal path is what keeps the entry alive.
* "No lane saw it, so the sequence is rare": no itest or retrace deletes a texture with a level still on
  the drain list (every itest draws first). CTS negative tests and `packed_pixels` (upload, check
  errors, delete, next case draws) do exactly this; G15 has not run yet.
* The renderbuffer table has the same stale-`Gen` entry but no drain and no pointer, so no deref; the
  framebuffer `NamedEntry` table resolves the live object at the DSA call; sampler CSOs are
  content-addressed with reference counts; sampler views and shader CSOs are re-acquired from live
  objects at the validate point. Only the texture drain defers by handle.

**Fix path** (B + A, same commit as C-1): the texture death helper — or the emitter's own hook from
`NotifyAndFree`'s notice — nulls `entry.Texture`, drops `entry.DrainKeys` and the slot's entries from
`m_drain`; `ResolveTexture` tests `MGPipeSlots().IsLive(kind, handle)` rather than `GenOfSlot`. The
emitter probe above becomes a permanent case; the itest case 6 (with `MALLOC_PERTURB_` in its lane
environment) becomes the negative control.

### MAJOR

#### M-A — no producer of `kMGPipeBindSampler` / `kMGPipeBindShaderImage`; `ImageBindableHint` is dead

`MGPipeTypes.h:256-259` says "P4a is that producer: a texture sets kMGPipeBindSampler,
kMGPipeBindShaderImage, kMGPipeBindRenderTarget and kMGPipeBindDepthStencil". The only caller of
`NoteTextureBoundAs` (`TextureEmit.h:474`) in the non-test tree is `FramebufferEmit.h:577`, with
RenderTarget / DepthStencil. `SamplerEmit.h` and `ImageEmit.h` never call it. Consequences:

* `desc.ImageBindableHint = (BindMask & kMGPipeBindShaderImage) != 0` (`TextureEmit.h:230`, `:520`,
  `:982`) is always 0; the ShaderImage-transition `ForceParamsResync` at `:486` is dead.
* Espryt's `pushedWantsImageBindableStorage` (`Managers.cpp:6408`, read from `Desc.BindMask`) never
  fires from the record; the widening still comes from the legacy trigger
  (`SyncTextureObjectToBackend(texture, /*imageBindable=*/true)`), so the PICTURE is right — but D-A4 /
  D-M's mitigation (the hint arriving BEFORE the first image bind so the level is not re-uploaded) is not
  delivered, and `ROADMAP.md:85` open question 2 (the remint pull rate that decides
  `MOBILEGL_PIPE_TEXEL_RETAIN_MB`) cannot be answered from this tree — nothing counts a remint.
* The metadata-respecify arm (ID-18 M4, `RepublishMask` `:975-986`) has exactly one live trigger
  (framebuffer attachment) and none of the two the ruling was written for.

Refutation attempted: `ResourceTracker.h:97` returns `kMGPipeBindSampler` for `BufferTarget::Texture`
— that is the BUFFER family (a TBO's backing store), not a texture. clientsp-review-v2 §"sampler views"
checks the view's `Target`/`Res` and not the mask.

Fix (C, ≈ 2 lines + 1 unit case): `ImageEmit.h`'s window walk notes each image-bound texture as
`kMGPipeBindShaderImage`; `SamplerEmit.h`'s view acquisition notes `kMGPipeBindSampler`;
`ImageEmitTest`: "a texture bound to an image unit carries `ImageBindableHint = 1` on its next
descriptor". Not blocking on its own — recorded as MAJOR because a design ruling (D-A4) is silently
undelivered and a ROADMAP number depends on it.

### MINOR

* **m-1 F-7 (recorded by the audit, still open): set_texture_params latches BEFORE acceptance.**
  `TextureEmit.h:641-643` sets `HasParamsLatch / ParamsVersion / SamplerVersion` and `:676` clears
  `ForceParamsResync` before the void `MGPipeApplySetTextureParams(params)` at `:679`. A refused record
  (no applier record — the SD-1/SD-3 shape) leaves the latch advanced; the params are not re-sent until
  the next `glTexParameter*` moves the version. `PublishCreate` (`:941-953`) and `NoteRespecified`
  (`:962-968`) got the acceptance rule; params did not. Owner B; make `MGPipeApplySetTextureParams`
  return `Bool` (A) and latch on true.
* **m-2 `Config.h:336-340`** lists 0x200→0x400, 0x400→0x80, 0x800→0x400 and omits the fourth row
  `kMGPipeP4aFamilyDependencies` actually enforces (`PipeFill.cpp:961-…`: bit 10 also requires bit 11).
  The scenario `ASamplerBitWithoutTheTextureBitIsRefusedAndNamed` pins the code; the comment misleads a
  reader of the mask.
* **m-3 `MGPipeTypes.h:1107-1108`** says the stored `BindMask` "is sticky and therefore ORed, never
  replaced"; `PipeApply.cpp:1644` replaces the descriptor whole. Equivalent today only because the
  client's `entry.BindMask` is itself sticky; fix the comment (or OR in the applier).
* **m-4 F-6 (audit class 1, recorded): encodings stated in package headers, not the contract.**
  `MGPipeEncodeImageAccess` lives in `ImageEmit.h`; `MGPSamplerView::Target`'s meaning and the
  `DrawBuffers` narrowing are stated at their emitters. Espryt reads them back with its own copies.
  Owner: contract, P4b.
* **m-5 `DirectGLES.cpp:8687-8754`** (`GenerateThreeChannelFloatMipmapOnCpu`, called at `:8780`):
  marks frontend levels dirty from inside the backend without `RearmPipeTextureLevelUpload`; on the
  handle arm the CPU-built levels reach the driver only at the NEXT verb's drain (self-heals, one verb
  late). The storage grow beside it (`EnsureGenerateMipmapStorageAllocated`, `:8038` / `:8777`) is
  fine — and note it is a SECOND per-level `AllocateStorage` → respecify path inside the backend that
  C-1's fix must pass the level through as well.
* **m-6** G9's dirty-surface check is blind to the two P4a bits on the `UseProgram` row when row AND
  mark are dropped together (declared in `DirtySurface.def:300-306`; measured in §4.3). The behaviour
  is pinned by `TrackerTest.cpp:833-849` and the F-1/F-2 itests, not by the gate.
* **m-7 pull-build text change**: `DirectGLES.cpp:4137/4174` adds a `Bool broadcastCountResolved`
  local and an `if` on the pull path (the only non-`#if` code change the preprocess diff of
  `Managers.cpp` / `DirectGLES.cpp` / `TextureObject.cpp` shows; everything else is macro parentheses,
  `__LINE__` immediates and one `static_assert` string). Benign; the per-symbol size comparison in §3
  is the evidence.
* **m-8** the review worktree's `include/ska` submodule prints `fatal: not a git repository` on every
  `git status` — a `wsl_tree.sh`-shaped tree does not get the nested-worktree `.git` pointer right
  for `include/ska`; `git commit -a` fails silently there (it cost this review one false gate reading,
  §4.1). Tooling note for `~/w7/notes/tools/`.

---

## 2. Experiments (review worktree, after the bench)

### 2.1 `FinalReviewC1Scenario` (DirectGLES, llvmpipe, itest binary run directly with the lane's env)

| case | sequence | `0x1fff` handle arm | `0x1ff` legacy arm |
|---|---|---|---|
| 1 PerLevelDefinitionAcrossAnUnrelatedDraw | L0(red); draw(other); L1(red); draw(T) magnified | **BLACK** rgba(0,0,0,0), 11625/11625 px — **FAIL** | red — pass |
| 2 ConsecutiveDefinitionsNoVerbBetween (control) | L0; L1; draw(T) | red — pass | red — pass |
| 3 LevelZeroConsumedBeforeLevelOne (control) | L0; draw(T); L1; draw(T) | red — pass | red — pass |
| 4 GenerateMipmapAcrossAnUnrelatedDraw | L0(red); draw(other); glGenerateMipmap; draw(T) | **BLACK** 11625/11625 px — **FAIL** | red — pass |
| 5 GenerateMipmapImmediately (control) | L0(red); glGenerateMipmap; draw(T) | red — pass | red — pass |
| 6 ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain | 8 × (L0(red); delete; draw(other)) | **SIGABRT** in round 1: `pure virtual method called` / `terminate called without an active exception` (rc 134) | white, 8/8 rounds — pass |
| 6 under `MALLOC_PERTURB_=165` | same | **SIGABRT**, same message | pass |

Notes. 128×96 pbuffer, llvmpipe (LLVM 22.1.6), `MOBILEGL_BACKEND_TYPE=DirectGLES`,
`__EGL_VENDOR_LIBRARY_FILENAMES=…/50_mesa.json`, the itest binary run directly with `--gtest_filter`;
logs `~/w7/p4a-finalreview/.finrev-logs/c1-{handles,legacy,perturb,perturb-legacy}.{log,mglog}`.
The three controls (2, 3, 5) are green on the handle arm, so the mip path and the consumed-first path
are fine and the two black pictures are exactly the accepted-but-unconsumed window C-1 describes.
Case 6's `pure virtual method called` is the dangling-vtable signature of a virtual call on an object
whose destructor has run — `AsMipmapTexture` → `GetStorageType()` from `EmitOneLevel` — and it fires on
the FIRST round without any allocator help; the library log carries no `was refused` line because the
process dies inside that first drain. The legacy arm (no drain, no handle table) passes all six.

### 2.2 First-attempt probes (same worktree, 21:43–22:00, logs `~/w7/p4a-finalreview-exp*.log`)

* `TextureEmit.FinalReviewALevelDefinedAfterAnEmittedButUnconsumedUploadKeepsThatUpload` — RED (C-1 at the
  emitter+applier level, driven through the real `AllocateStorage`).
* `TextureEmit.FinalReviewADeadTexturesHandleStillResolvesAndTheDrainWalksIt` — RED (C-2 at the emitter
  level).

---

## 3. G1 / G5 mechanics

* **G5** as landed: `p4a_untouched_regions.sh 37da3c3a HEAD` rc 0; `--self-test` 8/8 controls tripped.
  Mutation (§4.1): a `(void)0;` statement committed inside `RingAvailable` → rc 1, the row's sha moves
  and the region is named. (A comment-only mutation also moves the sha by the script's own rule at
  `:63-72`; my first two tries read rc 0 only because `git commit -a` failed on the broken submodule —
  m-8.)
* **G1 spot-check at TU granularity** (pull configuration, `compile_commands.json` of both
  `build-linux` trees, `clang++ -E -P` and `nm -S -C` on the objects):
  - `Managers.cpp.o`: 1410 defined symbols at base and at tree, **0** differing (size, type, name)
    rows; `DirectGLES.cpp.o`: 1004 and 1004, **0** differing. The symbol SET and every symbol's SIZE
    are unchanged in the pull configuration of the two files the phase touched most.
  - Preprocessed bodies: `BackendSamplerObject::SyncToBackend` **identical** (the widened signature is
    `#if MOBILEGL_PIPE_PUSH`-guarded at `Managers.h:2552-2564`, so the pull symbol keeps its name);
    `SyncMipmapsToBackend` and `SyncBuiltinSamplerToBackend` differ only in `__LINE__` immediates
    inside `ErrorLopper::Loop` lambdas and in the extra parentheses the `MGB_*` macros add (no codegen
    effect — the nm rows above are the proof); the `TextureObject.cpp` TU (first attempt) differs only
    in a `static_assert` string. The one pull-path CODE change found is m-7.
* D-N regions: the 17 rows are the same 17 the brief lists (15 Managers.cpp functions,
  `DepthStencilSamplingReadImpl` namespace, `ShouldUseCaveatTextureFormat`), `FlushPendingRangesFrom`
  consulted against the pinned sha unconditionally (D-N/2, declared in the script header).

---

## 4. Gates — can each go red?

### 4.1 Mutations this round (worktree; every edit restored, tree back at `6035c9d7`, `git status` clean of tracked changes)

| gate | mutation | result |
|---|---|---|
| G5 `p4a_untouched_regions.sh` | `(void)0;` inside `RingAvailable`, committed | **rc 1**, `RingAvailable` named, sha row moves |
| G9 `gen_pipe_dirty_surface.py --check` | `SetBlendFunc` row drops `NEW_PIPELINE_STATE` | **rc 1** "MISSING publisher NEW_PIPELINE_STATE for SetBlendFunc" |
| G9 same | `UseProgram` row drops `NEW_SAMPLER_VIEWS|NEW_SHADER_IMAGES`, marks kept | **rc 1** "STALE undecided mark" ×2 |
| G9 same (first attempt) | row AND undecided marks dropped | **rc 0** — blind (m-6, declared) |
| G7 `p4a_descriptor_negative_control.sh build-push` | its own two field drops | **rc 0**: `Layered` (FramebufferEmit.h, 3 assignments neutralised) tripped, `borderColorForm` (SamplerEmit.h) tripped, both suites green again after restore; tree clean |

### 4.2 Mutations from the first attempt (same worktree, `~/w7/p4a-finalreview-exp.log`)

| gate | mutation | result |
|---|---|---|
| P4aSeamAudit F-3 | delete `MGP_NOTE_AGGREGATE(FramebufferAttachment)` at `TextureObject.cpp:83` | `ATextureRespecifiedWhileAttachedReachesTheFramebufferRecord` **red** |
| G9 white-box | delete `SyncReadFramebufferTextureAttachments(...)` at `DirectGLES.cpp:2309` | `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` **red** (6402 ≠ 6401) |
| G12 / D-K2 | `PipeSubsystemDependencyMissing` returns false | `ObjectSubsystemControl.Refused.…ASamplerBitWithoutTheTextureBitIsRefusedAndNamed` **red**, names 0x9ff and the two bits |

### 4.3 Standing observations on the gates

* `.github/workflows/test.yml:9-12` still carries the **TEMPORARY** `feat/disaggregated` trigger — left
  in place as instructed; the integrator removes it at the `dev` merge. `BASELINE: "37da3c3a"` at
  `:1599`; `--expect-probes 4` at `:802` pinned with `check_include_closure.py`'s PROBES list.
* G2 (pull == push test names): not re-taken here (the review's pull tree has no test binaries);
  gates-v3 §6 row 2 is the standing evidence.
* The subdata-refusal line (`resource_subdata for texture {…} level N was refused`,
  `TextureEmit.h:1092`) is not one of the ten census shapes esprytdraw §6.1 greps, and no itest reads
  `RefusedSubDataCount`. Add it to the post-merge census (it is C-2's tell).
* `ctest` re-runs on the final tree this round (`build-push`, llvmpipe/lavapipe):
  `-R 'ObjectSubsystemControl|TextureParamsWithoutASamplerView|TextureUploadShape|P4aSeamAudit|HandleRecycle'`
  **181/181**; `-L unit -R 'TextureEmit|FramebufferEmit|SamplerEmit|ImageEmit|ProgramEmit|Tracker|CompositeResolver|PipeCatalogue|ResourceEmit|Sanity|ProgramArtifacts'`
  **304/304**. Green — and, as §1 shows, green over both criticals: none of the 181 defines a level
  around a verb or deletes a texture with a level on the drain list.

---

## 5. Recorded for later (owner, phase)

| # | item | owner | phase |
|---|---|---|---|
| R-1 | M-A producers for `kMGPipeBindSampler` / `kMGPipeBindShaderImage` + the remint counter `ROADMAP.md:85` asks for | C | before D.5 docs if cheap, else P3b/P4b |
| R-2 | m-1 F-7: `MGPipeApplySetTextureParams` returns `Bool`; latch on acceptance | A + B | with the C-1/C-2 rework (same files) |
| R-3 | m-4 F-6: image-access / sampler-view target / draw-buffer encodings stated once in the contract | contract | P4b |
| R-4 | m-6: G9 derivation for `UseProgram` / `BindVertexArray` (the `attachedShaders` / `Access` undeclared-member taint) | A | P4b |
| R-5 | SD-5: `0x7ff` half-run — c0g's table now refuses it on the client; the server's table (`ResolveTextureResourceSubsystemArm`) is the same four rows; keep both lists in one place | D | P4b |
| R-6 | `g_rawDepthFetchSamplerState` (a frontend `SamplerObject` inside `MG_Backend`, synced through the "object is the authority" branch `Managers.cpp:12361-12372`) — the one place the handle arm reads a frontend object on purpose (S-1, audit §B) | D | P3b/P4b (`ROADMAP.md:23`) |
| R-7 | m-5: `GenerateThreeChannelFloatMipmapOnCpu` rearm | D | P4b |
| R-8 | `ProgramArtifacts.h:563-565` NDK/libc++ size pin still inert (brief D.5 chore) | integrator | D.5 |
| R-9 | Post-merge census must include the retrace logs (`grep -c 'no applier record\|does not describe\|declining the built-in\|cannot be pushed\|was refused' retrace-out/*/*/DirectGLES/output/mobilegl.log`) — audit §B's correction plus C-2's line | integrator | D.3 re-run |
| R-10 | `HandleRecycle.AbaControl` for the six P4a kinds is inert on both backends (no `PipeHandleAbaControl` consumer over the object slot tables; the cases say so and assert the correct pixels) — a real control needs one `if` in `GetOrCreateByHandle` | D | P4b |
| R-11 | `TextureUploadShapeScenario` recorded, not gated (D-D4) — becomes a gate with the Mali delta | F | P3b/P4b |
| R-12 | m-8 tooling: nested-worktree `include/ska` `.git` pointer in `wsl_tree.sh`-shaped trees | tools | now |

---

## 6. D.5 docs checklist (state at `6035c9d7` / `a38bdab4`)

All D.5 placeholders are still unfilled; `scripts/check_doc_citations.py docs/Disaggregated/*.md` must
be run after each.

| doc line | brief asks | state |
|---|---|---|
| `README.md:3` | status → P4a landed + hash; next P3b/P4b, P5 | not updated |
| `ROADMAP.md:3` | — | still "P3a 已落地 … 下一步 P4a" |
| `ROADMAP.md:20` | ✅ + the real numbers (15 calls, 6 kinds, memos retired, two red-before logs, CTS blocks, T3) | not marked |
| `ROADMAP.md:23` | what P4a left to P3b/P4b (D-Q table) | not updated |
| `ROADMAP.md:67` | 39-day checkpoint: actual elapsed days per package | "P4a > 39 天 | 同上" unfilled |
| `ROADMAP.md:85` (Q2) | texture-remint pull rate | not updated — and NOT MEASURABLE on this tree (M-A) |
| `ROADMAP.md:91` (Q8) | one reflection archive, Espryt half | not updated |
| `ROADMAP.md:100` (Q17) | `create-indirect` state at exit + fix the `:7`→`:98` citation | not updated (SD-0 fixed it; say so) |
| `ARCHITECTURE.md:63` | [deviation] D-F2 identity-addressed sampler views for Espryt | not updated |
| `ARCHITECTURE.md:100` | read-attachment-only parameter gap closed, scenario red-first | not updated |
| `ARCHITECTURE.md:134` | [deviation] D-E1 `MGPTextureParams` 40 bytes + built-in sampler CSO | not updated |
| `ARCHITECTURE.md:136` | `MGPFramebufferState::Target`, `Complete` = `CheckCompleteness()` (D-C3); add `Named` (ID-27/D-I2) | not updated |
| `ARCHITECTURE.md:212` | `CompositeResolver` file + two release paths | not updated |
| `ARCHITECTURE.md:249-254` | drain list is P4a's, cursor is P3b/P4b's, `TextureUploadShapeScenario` built/gated | not updated |
| `ARCHITECTURE.md:261` | `ProgramArtifactsCodec.{h,cpp}`, verify-only | not updated |
| `ARCHITECTURE.md:294-295` | `kNeedsAck` narrowed to the buffer target (D-A2) | not updated |
| `ARCHITECTURE.md:318/:321` | P4a's six byte-identical rows beside P3a's eleven (now 17 rows in `p4a_untouched_regions.sh`) | not updated |
| `ARCHITECTURE.md:323` | `g_rawDepthFetchSamplerState` still a frontend object inside the backend | not updated |
| `ARCHITECTURE.md:369` | `MOBILEGL_PIPE_LEGACY_MEMOS` P4a arm + the +1 day | the only P4a mention in the file; the day is not recorded |
| `ARCHITECTURE.md:598` | push default `0x1fff`, bits 9-12, three dependency refusals (there are four: add 10→11) | not updated |
| `MEASUREMENTS.md` | new numbered P4a section (D.4.2 table with the acc/draw caveat above it, D.4.3 headline numbers, T1/T2/T3 + `mc_state_toggle`, six counters, four suppressors, per-bit fire rates, two red-before logs, Track H census, CTS blocks, peak-RSS hedge) | no P4a section; DriverBench rows exist in `~/w7/p4a-bench.log` (e.g. magma `mc_pass_switch` pull 518785.6 / push 513177.1 / native 475307.8) — transcribe with the two-device A/B |
| `MEASUREMENTS.md:465` | reading (5) discharged: CSO blob byte class numbers | not updated |
| `MEASUREMENTS.md:519` | `g_uploadRing` reset asymmetry still open + `ScopedDefaultUnpackState::s_synced` (D-O) beside it | not updated |
| `ProgramArtifacts.h:563-565` | NDK size pin (chore) | inert |

Two additions the brief did not foresee: (a) the C-1/C-2 rework and the `MGPRespecifiedLevel` rule
belong in `ARCHITECTURE.md:249-254`'s texture paragraph ("the client passes the level it redefines;
whole-resource calls pass none"); (b) `ARCHITECTURE.md:598` should list the FOUR dependency rows.

---

## 7. Public-GL sequences still uncovered by the retrace / itest lanes (audit classes extended), ranked

1. **Per-level definition around a verb** (C-1): `glTexImage2D(L0)`; verb; `glTexImage2D(L1)` — also the
   `glCopyTexImage2D`, `glCompressedTexImage2D`, cube-face and texture-view forms. No fixture, no itest.
2. **Upload-then-generate around a verb** (C-1/4): `glTexImage2D(L0)`; verb; `glGenerateMipmap`.
3. **Dirty-then-delete** (C-2): a level on the drain list when the last reference drops. CTS negative
   tests and `packed_pixels` do this constantly; G15 is the first place it will run.
4. **Image-unit-only textures across program switches** (M-A/F-2): bound with `glBindImageTexture`,
   never sampled, program changes the window — F-2's itest covers the window; nothing measures the
   remint.
5. **Sampler-object binding churn** (`glBindSampler` on registered samplers across many units) — SD-1's
   two scenarios are the only coverage of the registered-sampler CSO path; no retrace fixture binds
   application samplers (audit §B: zero occurrences in the 20 traces).
6. **Framebuffer death while bound / recycled name with a Named record pending** — HandleRecycle covers
   the bound case; DSA-configured-then-deleted-then-recycled (Named record on a slot whose Gen moved)
   is only covered by `FramebufferRecordFor`'s loud refusal, not by a scenario.
7. **Level-scoped respecify of a cube face while the other faces are pending** — gates' MINOR-1 unit case
   pins the applier; no GL-level case.
8. **Texture views over a mutable owner redefined per level** (class of 1).
9. **`glTexParameter*` refused-then-repeated** (m-1): the latch-before-acceptance shape has no lane
   because the refusal needs a texture without an applier record (S-1/SD-3 shape), which the fixes
   removed; a `MOBILEGL_PIPE_PUSH` mask that arms bit 10 without a consumer (`NoConsumer` lane) is the
   place to add it.
10. The `0x7ff` / `0x5ff` half-masks with a REGISTERED sampler bound (SD-5 was measured without one).

---

## 8. What the integrator must re-run

After the C-1/C-2 rework lands (B + A; and R-2 if taken with it):

1. `-L unit` on pull, push and verify; the two probe cases from §2.2 and the two itest cases from §2.1
   (cases 1, 4, 6) added as permanent tests, with case 6's lane carrying `MALLOC_PERTURB_=165`.
2. `ctest -L integration-gpu` on `build-push` at `0x1fff`, `0x1ff`, `0x9ff`, `0x5ff`, consumer /
   no-consumer, plus `-L integration-verify` — the 1079/1079 answer gates-v3 §6 names.
3. The full retrace at the default mask (79 cases) AND the retrace-log census of R-9 (the ten shapes +
   `was refused` must all be zero).
4. `p4a_untouched_regions.sh 37da3c3a HEAD` + `--self-test`; `p4a_descriptor_negative_control.sh`;
   `gen_pipe_dirty_surface.py --check`; the include-closure step.
5. G1 symbol report (`symbol_report.py --fail-on-symbol-set-change --fail-on-added-bytes 0`) — the
   rework touches `PipeMutation.h` / `TextureObject.h` / `TextureObject.cpp`, all of which must stay
   `#if MOBILEGL_PIPE_PUSH`-guarded on the pull side.
6. The device A/B and DriverBench rows are unaffected by the rework's mechanism but the two-device
   Release A/B must be re-taken on the rework head before the numbers go into `MEASUREMENTS.md`.
7. Remove the TEMPORARY trigger at `test.yml:9-12` in the `dev` merge commit; D.5 docs (§6);
   `check_doc_citations.py`; G15 scheduled after the verdict.

---

## 9. Artefacts and cleanup

* Kept beside this report in `~/w7/notes/p4a/p4a-results/final-review-v1-logs/`: the probe
  scenario `FinalReviewC1Scenario.cpp` (drop-in for `MG_IntegrationTest/Scenarios/`, add one line to
  the CMake list), the two unit probes `finrev-repro.py` / `finrev-repro2.py` (they splice a `TEST`
  into `TextureEmitTest.cpp`), the itest run logs `c1-{handles,legacy,perturb,perturb-legacy}.log`,
  the gate-mutation outputs `g5-*.err` / `g9-*.out` / `g7.log`, the ctest subset logs, the four
  `nm -S` symbol tables of §3, and the first attempt's `p4a-finalreview-exp*.log`.
* Removed: the worktrees `~/w7/p4a-finalreview` and `~/w7/p4a-finalreview-base` (`git worktree
  remove --force` + `prune` from `~/w7/pipe`), their build dirs, `~/w7/finrev-g1/` (42 MB of `.i`)
  and the first attempt's `~/w7/p4a-finalreview-*.log`. `pipe` itself was never checked out, edited or
  committed to (HEAD `a38bdab4`, no tracked change).
