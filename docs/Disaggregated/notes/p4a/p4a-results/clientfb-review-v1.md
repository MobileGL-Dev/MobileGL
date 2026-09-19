# P4a package B — `clientfb`, adversarial review v1

Reviewed: `refs/heads/p4a/clientfb` @ `f7de87b5` (`51d1760f` / `a78eaf93` / `f7de87b5`) against the tag
`p4a/contract` = `08192d72`, and against `refs/heads/feat/disaggregated` @ `17db7598` (= c0 + c0b + c0c) and
`refs/heads/p4a/wire` for the seams the rework has to meet. 13 files, +2647 / −55.

**Method.** Read-only. I did not create a worktree and did not re-run B's gates — the report's transcript is
internally consistent, the file list matches C.7 exactly, and every finding below is decided by reading code
across four refs rather than by a build. Where a finding needed a fact I could not read off B's branch (the
wire applier's arity, c0b's declarations, C's cache seam) I read it out of the branch that owns it. Every
finding carries the refutation I tried.

---

## Verdict: **REWORK**

Not because the package is weak — it is the most carefully argued of the client packages, its eleven
deviations are honest, and D4 (the blocked texture flip) is a real find that D.1's ordering justifies after
the fact. It is REWORK because three of the brief's own named properties are not actually delivered:

- **M1** the `MGPSubRegion[]` tail is built and never handed to the applier, so D-D6's "the SERVER picks the
  upload shape" is unimplemented and, after the rebase onto wire, becomes a record that declares N regions
  and supplies none;
- **M2** no emission site covers the thirteenth `MGP_NOTE_AGGREGATE(TextureParams)` site — the built-in
  sampler's own choke point — so `MGPTextureParams::{MinLod, MaxLod, LodBias}` go stale on
  `glTexParameterf(GL_TEXTURE_MIN_LOD)`, and it is the exact hook ID-14's re-emit needs;
- **M3** the client clears its own dirty flag on *dispatch*, not on *acceptance*, which is the one thing
  D-D5 step 1 spells out and the one thing its unit case claims to pin;
- plus **M4**, the sticky `BindMask` / `ImageBindableHint` never reaching the applier for the canonical
  immutable-storage order.

The mandatory c0b / c0c / ID-14 reconciliation is by itself a rework of comparable size, so folding these in
costs one round rather than two.

---

## 1. Findings

### MAJOR

#### M1 — `RegionCount` is emitted; the regions are not. `MG_Impl/Pipe/TextureEmit.h:939-941`

```cpp
if constexpr (MGPipeTextureRecordsReachTheApplier()) {
    MGPipeApplyResourceSubData(m_lastSubData, shadow);      // <- two arguments
}
```

`m_regions` is filled at `:909-914`, its size is written into `m_lastSubData.RegionCount` at `:928`, it is
exposed to unit cases through `LastRegions()` at `:778` — and it is never passed to anybody. On the tag the
applier's signature is two-argument (`MG_Pipe/PipeApply.h:585`), so today the omission is unobservable and
compiles. On `refs/heads/p4a/wire` W1 closed contract-review M2 by giving that call a defaulted trailing
`const MGPSubRegion*` (`PipeApply.h:628`) — **and B still passes nothing**, so after the rebase the applier
receives `RegionCount = N` with a null tail on every scattered upload.

*Failure scenario.* Atlas traffic (`rei`, `xaero-*`, `journeymap`, `modernui` — the fixtures D-D5 names) is
the scattered case: two far-apart sprite writes produce `RegionCount = 2` and a null list. Either the applier
faults on the mismatch (the loud direction, and the best case), or it stores a pending set with two declared
regions it cannot read and Espryt picks the box — which silently discards D-D6's entire measured argument
(635 KB/frame box vs 40 KB/frame rects, 16×, and Mali's per-job +6 ms/frame). Nothing in the tree can see the
difference: SSIM is blind to upload shape, so the retrace's 79/79 proves nothing here, and the four unit cases
that touch regions all read `LastRegions()` — the emitter's own staging vector — rather than what the applier
was handed.

*Refutation attempted, and it fails.* (a) "The record is not sent at all today, so it cannot matter" — true
for `resource_subdata` under `kMGPipeWiredTextureSubsystem = 0`, but §9's one-line flip is made *in the same
commit as the rebase*, which is precisely when both halves become live at once. (b) "RegionCount 0 is the
common case" — `MipmapStorage::MarkDirtyRegion` seeds the rect list from the union box and inserts, so a
*single* `glTexSubImage2D` on a clean level already produces `RegionCount = 1`; zero is the *whole-level*
case, not the common one. (c) "The applier could recover the regions from the box" — that is exactly the
information the box does not carry.

**Fix**: pass `m_regions.data()` (and keep passing `nullptr` when the list is empty) at the rebase, and change
`AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero` to assert on what the applier received rather than on
`LastRegions()`.

---

#### M2 — the built-in sampler's parameter mutators publish no `set_texture_params`

C.1's MODIFY row asks for *"`set_texture_params` from the thirteen `MGP_NOTE_AGGREGATE(TextureParams)` sites"*.
There are exactly thirteen in the tree:

| where | count | hooked by B? |
|---|---|---|
| `MG_State/GLState/TextureState/TextureObject.cpp` :174 :208 :226 :244 :297 :308 :329 :347 :371 | 9 | yes (`PipePublishParams` at :181 :210 :228 :246 :299 :310 :335 :350 :378) |
| `TextureObject.h:192` (`SetDepthStencilTextureMode`) | 1 | yes (`:197`) |
| `TextureObject.cpp:417` (`SetSamples`), `:430` (`SetFixedSampleLocations`) | 2 | **no** — descriptor only, and that is *defensible*: neither field exists in `MGPTextureParams` |
| **`MG_State/GLState/SamplerState/SamplerObject.cpp:53`** (`SamplerObject::BumpVersion`, whose own comment calls it *"the one choke point every setter reaches"*) | 1 | **NO — and this one is a defect** |

`MGPipeBuildTextureParams` reads three of its ten fields off that object
(`TextureEmit.h:346-351`: `MinLod`, `MaxLod`, `LodBias` from `texture.GetSamplerObject()`). Every
`glTexParameter*` that writes them lands on the `SamplerObject` and on nothing B watches —
`GL_Texture.cpp:1265-1276` (`GL_TEXTURE_MIN_LOD` / `MAX_LOD` → `SetLodRange`), `:1313` and `:1391`
(`GL_TEXTURE_LOD_BIAS` → `SetLodBias`), and their DSA twins at `:2038-2087` and the `*fv` families at
`:3062-3392`.

*Failure scenario.* `glTexStorage2D(…); glTexParameterf(GL_TEXTURE_MIN_LOD, 2.0f); draw;` — no
`set_texture_params` is emitted, the applier's record still says `MinLod = 0`, and Espryt's
`SyncTextureParamsToBackend` pushes the wrong LOD clamp. Wrong pixels, not a lost optimisation.

*And this is the ID-14 hook.* ID-14 requires B to *"re-emit `set_texture_params` when the sampler object's
parameters change"*, because C's cache is content-addressed and the handle moves with the content. Judged as
the review was asked to: **`SamplerResync` does not cover it** — `MGPipeBuildTextureParams:345` writes
`params.SamplerResync = 0` unconditionally, D-E2 makes both resync bytes the *server's* to set, and a resync
byte on a record that is never emitted is inert anyway. There is today **no path at all** from a sampler
parameter change to a `set_texture_params`.

*Refutation attempted.* (a) "Some other setter publishes afterwards" — `SetInternalFormat` publishes both
records, but no `glTexParameter*` touches the format; `SetBaseLevel` / `SetMaxLevel` are different pnames.
(b) "It is C's file, so it is C's problem" — `MG_State/GLState/SamplerState/**` is C's after the tag, so B
genuinely cannot hook `BumpVersion`; but the *record* is B's and C.7 grants B
`MG_Impl/GLImpl/Texture/GL_Texture.cpp` under exactly this rule (*"if an implementer finds a path that does not
go through them, the grant is one call site per path and it is recorded as a deviation"*). B did not take the
grant and §6 records it as "not needed", which is wrong for this class. (c) "The wired bit is 0, so nothing
breaks" — again, the flip is the integrator's next commit.

**Fix**, and it lands with ID-14 rather than beside it: latch `(m_textureParamsVersion,
sampler->GetVersion())` per texture in the emitter, and re-publish from the granted `GL_Texture.cpp`
`glTexParameter*` sites (or, if A/C prefer, have `SamplerObject::BumpVersion` call a B-provided hook — that is
a contract change and should be raised, not assumed).

---

#### M3 — the client clears its own dirty flag on *dispatch*, not on *acceptance*. `TextureEmit.h:947-951`

```cpp
if constexpr (MGPipeTextureRecordsReachTheApplier()) { MGPipeApplyResourceSubData(...); }
++m_subDatas;
...
mipmap->MarkStorageDirty(uploadTarget, level, false);   // unconditional
```

D-D5 step 1 is explicit: *"the client clears its own `m_isDirty`/`m_dirtyRects`/`m_dirtyRegions` for the level
at emission, and **only for levels whose record the applier accepted** (the emit helper returns 'applied')"*.
`MGPipeApplyResourceSubData` returns `void` on the tag and on wire — the wire review's own m4 says no
acceptance signal exists — so B clears whatever happened. Worse, the clear runs **even when the record was
never dispatched at all**: with the texture bit still 0 the `if constexpr` discards the call and line 950 still
fires, which is exactly the state every `ArmForTest(true)` unit case runs in.

*Failure scenario, post-flip.* Any applier refusal on a `resource_subdata` — a stale generation, a box that
fails `SubDataBoxFault`, a resource whose record the applier does not hold (which is the *normal* state for a
texture born while the bit was clear until the self-healing create at `:601-608` catches it) — leaves the
server with nothing and the client with a clean flag. `MG_Impl` contains no reader of the texture's dirty
state, so nothing ever notices: **the level stops updating for the life of the texture**. This is the exact
"a texture that stops updating" the review brief asks about, and yes, it is reachable.

*The unit case pins the wrong property.* `TextureEmit.ABailedLevelStaysDirtyAndStaysOnTheDrainList`
(`TextureEmitTest.cpp`, second half) asserts `EXPECT_FALSE(good->IsStorageDirty(...))` with the message
*"the client must clear its own flag for a level whose record the applier accepted"* — on a build where the
applier was never called. It is green for the wrong reason, and it would stay green under a refusal.

*Refutation attempted.* (a) "There is no acceptance signal to check, so B did the only thing possible" —
partly true, and it is why this is shared with wire rather than B's alone; but B's *report* declares neither
the deviation nor the risk, and the honest minimum is a declared deviation plus a `MGLOG_E_ONCE` on the
non-dispatched path. (b) "The applier's pending-upload set is the safety net (D-D5 step 2)" — the net catches
Espryt's bails, which is a different failure; it cannot catch a record the applier refused.

**Fix**: either wire adds the return value (one line in `PipeApply.{h,cpp}`, and it is m4 of its own review),
or B clears only inside the `if constexpr` arm and logs once on the other. The first is correct; take it to
the integrator as a joint item.

---

#### M4 — the sticky `BindMask` / `ImageBindableHint` never reach the applier for immutable storage

`NoteTextureBoundAs` (`TextureEmit.h:482-494`) ORs the bit into the client entry and returns. Nothing
republishes. The comment at `FramebufferEmit.h:406-408` states the consequence as if it were the design:
*"the mask is republished on the resource's next respecify"*.

*Failure scenario.* `glTexStorage2D(…)` → one respecify with `BindMask = 0`, `ImageBindableHint = 0`. Then
`glBindImageTexture(…)` (or an FBO attachment) sets the bit client-side. An **immutable** texture has no next
respecify — that is what immutable means — so the applier's record keeps `ImageBindableHint = 0` forever. The
hint is described in B's own comment as *"the PREVENTION half of the texture-remint stall class — a texture the
server knows may be image-bound is allocated image-bindable up front, so the re-mint that would have to pull
its texels back never happens"*. For the canonical order (allocate, then bind) it never fires, so the
prevention half is a no-op for exactly the textures it was written for. The same is true of `RENDER_TARGET` /
`DEPTH_STENCIL`, whose only producers are `FramebufferEmit.h:409-421`.

*The unit case hides it.* `AnImageBoundTextureCarriesTheImageBindableHintForever` calls
`NoteTextureBoundAs(handle, kMGPipeBindShaderImage)` **before** `AllocateStorage`, i.e. in the one order in
which the mask does reach the wire. Reverse the two statements and the case goes red.

*Refutation attempted.* (a) "D-A4 only says the mask is *emitted on* create and respecify" — it does, and B
satisfies the letter; but D-A4 also says `ImageBindableHint` *"is the prevention half of §8.4's four
mitigations"*, and a value that cannot reach the server before the allocation it is meant to steer prevents
nothing. (b) "A mutable texture will respecify eventually" — true and irrelevant; the atlas and image-load/store
textures this targets are `glTexStorage`'d. (c) "The applier could re-derive it from `MGPImageView::Res`" —
that is C's family and a different bit; the descriptor field would still be wrong.

**Fix**: make `NoteTextureBoundAs` / `NoteRenderbufferBoundAs` republish the descriptor when the mask actually
*changes* (the memcmp dedupe at `:596` already makes the no-change case free), and add the reversed-order
assertion to the case.

---

### MINOR

| # | file:line | finding | scenario | refutation tried |
|---|---|---|---|---|
| m1 | `FramebufferEmit.h:83-91` | `MGPipeResolveAttachmentUploadTarget` falls back to `texture->GetUploadTargets()[0]` when the attachment carries `Unknown`. For a **cube map** that is six faces and `[0]` is `CubeMapPositiveX`. | `glFramebufferTexture(GL_COLOR_ATTACHMENT0, cube, 0)` (layered, no face token) emits a *resolved* record naming +X. Espryt reads `MGPSurface::UploadTarget` to pick the face; with `Layered = 1` it should ignore it, but the record now asserts a face that is not the truth. | The cited precedent (`FramebufferAttachmentObject::GetSize`, `FramebufferObject.cpp`) applies the same fallback — but it only needs an **extent**, which is identical across the six faces. Face *identity* is not. The precedent does not transfer. |
| m2 | `FramebufferEmit.h:136-143`, refusal loop `:345-357` | D-C3's over-wide refusal scans **attachments** only. A draw-buffer token may name a colour point ≥ 8 with nothing attached there; `MGPipeDrawBufferIndex` then returns 8..31 into an 8-wide `Color[]`. | `glDrawBuffers(1, {GL_COLOR_ATTACHMENT10})` with no attachment at 10 (legal state, incomplete for draw) → `DrawBuffers[0] = 10`. The server indexes `Color[10]` out of bounds, or must invent a bound the record does not carry. | The FBO would be draw-incomplete, so `Complete = 0` and a well-behaved server would not draw — but the record still carries an out-of-range index, and "truncating silently is the bug class this phase is closing" is the same argument that produced the attachment refusal. One extra test in the same loop. |
| m3 | `TextureEmit.h:564` vs `:569` vs `:609` | `Entry& entry = EntryFor(m_textures, handle);` is taken, then `AcquireTexture(owner…)` may `resize()` the same `Vector`, then `entry.LastDesc = desc` writes through the reference. | Dangling reference / heap corruption if the owner's slot exceeds the table's size. | **Refuted for production**: every texture registers its slot in `TextureObjectBase`'s constructor, so the table always already covers the owner. **Not refuted after `ResetForTest()`**, which clears `m_textures` while the allocator keeps its slots — an owner allocated at a higher slot than its view then resizes mid-function. Make `EntryFor` return an index, or re-fetch. |
| m4 | `TextureEmit.h:1060-1070`, `:442-448`, `:865-869` | `MGPipeEmitTextureResourceDestroy` returns **before** `NoteTextureRecordDestroyed` when the record was never published, so `Retire()` never runs; and `AcquireTexture` sets only `Texture` and `Gen`, leaving `BindMask`, `ForceParamsResync`, `HasLastDesc`, `LastDesc`, `DrainKeys` from the previous occupant. | Live **today**: the texture bit is 0 (nothing publishes) while the framebuffer bit is wired and `SurfaceOf` ORs `RENDER_TARGET`/`DEPTH_STENCIL` into those entries. A recycled slot's new texture inherits the dead one's sticky mask, hence a wrong `BindMask`/`ImageBindableHint` on its first descriptor. | `LastDesc` inheritance is harmless (it embeds `Resource`, whose `Gen` differs, so the memcmp cannot false-hit). The mask inheritance is not. c0b's `{kind, slot, gen}` latch fixes the publication half; the Entry reset is still B's. |
| m5 | `FramebufferEmit.h:240-241`, `:379` | A `Target = Draw` record's `ReadSurface` is resolved from the **read** framebuffer, so it describes a surface that is not part of the framebuffer its own `Fbo` names — and a `glReadBuffer` on the read FBO moves the *draw* record's `ContentHash` and forces a redundant draw emission. | Correct per D-C2's letter (*"resolved from the READ framebuffer's own read buffer"*), muddled in substance. Cost is one extra 304-byte record per `glReadBuffer`. | Not a defect, but the server must be told to read `ReadSurface` only off a `Read`/`Both` record; say so in the payload comment so E's `SyncCurrentFBO` cannot read it off the draw record. |
| m6 | `FramebufferEmit.h:122`, test `FramebufferEmitTest.cpp` empty-point assertion | `MGPSurface::UploadTarget` is written `0` on renderbuffer and empty points, and `0 == TextureUploadTarget::Texture1D` — the *same* enumerator collision ID-12 ruled on for `MGPSubData::Target`. | A consumer that reads `UploadTarget` without first testing `Kind` reads "1D texture". | `Kind` disambiguates, so it is currently unreachable; but c0c's `MGPSurface::TextureTarget` explicitly takes `0xFFFF = Unknown` on non-texture points, and `UploadTarget` should follow the same rule in the same rework. The unit case pins the ambiguous value and must move with it. |
| m7 | `FramebufferEmit.h:56` + `TextureEmit.h:98` | The branch ships **bit 9 wired, bit 10 unwired** — the arm D-K2 forbids (*"bit 9 (framebuffer) requires bit 10"*), because every `MGPSurface::Res` names a handle the applier has no record for. | Inert today (nothing consumes `set_framebuffer_state` before Espryt lands), and §9 already makes the flip mandatory in the rebase commit. Recorded so it cannot be dropped: **the branch must never be integrated with only one constant flipped.** | Refuted as a live defect; kept as an integration hard constraint. |
| m8 | `TextureObject.cpp:263` (`SetInternalFormat` → `PipePublishDescriptor`) | The first `resource_respecify` for a texture is emitted from `SetInternalFormat`, before any storage exists, carrying `Width = Height = 0`, `Levels = 0`, `HasDefinedContent = 0` — a "storage definition" that defines none. | Extra record per texture; a strict applier could count it as a respecify to a zero extent. | Harmless as long as the applier tolerates a zero-extent respecify (P3a's buffer bodies do). Declare it, or gate the publish on `storageDefined`. |
| m9 | `FramebufferEmitTest.EveryAttachmentFieldSurvivesTheSurfaceConversion` | C.1 asks G6 to be driven *"through the texture, the DSA-named, the renderbuffer and the layered entry points"*; the case drives `AttachTexture` / `AttachRenderbuffer` / the layered flag but not a DSA-named path. | Weaker coverage than the brief asks. | All four GL entry points funnel into `FramebufferObject::AttachTexture`, so a state-level unit case genuinely cannot distinguish them. Declare it rather than fake it. |

---

## 2. The declared deviations, judged

| dev | judgement |
|---|---|
| **D1** MG_State reaches the emitter through two TUs | **Superseded by c0b (ID-13).** The reasoning was right at the tag and is now moot: `PipeMutation.h:281-358` declares four mints and nine emissions and `PipeFill.cpp:1042-1114` forwards them through `ForwardWhenWired<kWired>`. The rework deletes B's six free functions and re-points the MG_State call sites. Full site list in §3. |
| **D2** `MGPSubData::Target` PACKED | **ADOPTED (ID-12 DV-3).** B's spelling wins. c0c has moved `MGPipePackSubDataTarget` / `…ResourceTargetOf` / `…UploadTargetOf` into `MGPipeTypes.h` under the same names with `Uint32` arguments; B's copies at `TextureEmit.h:162-176` must be deleted (they are `inline constexpr` in the same namespace — leaving them is a redefinition). |
| **D3** step 1 emitted from the destructor | **Superseded by c0b (ID-13).** `MGPipeEmitTextureDestroyAndFree` / `…RenderbufferDestroyAndFree` now read `MGPipeHandleIsPublished` and emit `MGPipeApplyResourceDestroy` themselves (contract-v2 §M1(b), `PipeFill.cpp:1199-1200` / `:1235-1236`). B's `MGPipeEmitTextureResourceDestroy` / `…RenderbufferResourceDestroy` and both destructor call sites are deleted. |
| **D4** `kMGPipeWiredTextureSubsystem` stays 0 | **ACCEPTED, and it is the report's best work.** The `Fatal{ProtocolCorruption} … the buffer half carries a mip level` was found, not argued, and it is direct evidence for D.1's stated order. Verified independently: `PipeFill.cpp:1292-1310` ORs the constant into `kMGPipeWiredSubsystems` and `static_assert`s it is 0-or-its-own-bit, and `:1934` gates `DrainTextureSubData` on that OR — so the one-line flip really is the whole switch, and nothing else moves. **The flip claim checks out.** |
| **D5** drain flag on the client, not on `MipmapStorage` | **ACCEPTED.** Better than D-D4's letter: no MG_State member, so G1's admitted-resize set stays empty, and the ≤96-entry scan sits on a path that has just memcpy'd texels. |
| **D6** descriptor deduped on its own bytes | **ACCEPTED.** An 88-byte memcmp against the emission it avoids is the right trade and it is what keeps `glRenderbufferStorage`'s three setters one record. |
| **D7** `MGPSurface::Kind` reuses `MGPipeKind` | **ADOPTED (ID-12 DV-4).** c0c mints `kMGPipeSurfaceKindNone/Texture/Renderbuffer`; B's copies at `FramebufferEmit.h:73-77` are deleted. |
| **D8** `DrawBuffers[]` narrowing of the four default tokens | **ACCEPTED with m2.** The stereo argument is right (`FramebufferObject`'s constructor seeds `BackLeft` and nothing writes another). The narrowing is not the problem; the *widening* above the array bound is (m2). |
| **D9** `DepthStencilMode` numbered | **ADOPTED (ID-12 DV-2).** c0c mints `kMGPipeDepthStencilModeDepth/Stencil`; B's copies at `TextureEmit.h:182-183` are deleted, `MGPipeDepthStencilModeByte` stays B's. |
| **D10** `BuiltinSampler` identity-addressed | **REWORK (ID-14).** See §3. B's own §9 flagged it first and correctly; ID-14 rules the direction. Note the ordering problem in §3.4 — B cannot do this on its own branch. |
| **D11** one `SamplerCso` slot per texture not released | **Dissolved by ID-14.** After the rework B mints no `SamplerCso` at all, so there is nothing to leak; C's LRU eviction is the only `delete_sampler_state` emitter. Delete the deviation rather than carry it. |

**On C1 (wire), asked directly: does B's emission order make the loss reachable today?** **Yes, and by the
ordinary path.** `glTexImage2D(level 0, data)` → `AllocateStorage` → respecify → mark dirty → validate →
`resource_subdata` accepted into `PendingUploads[(target,0)]` **and the client's flag cleared** → Espryt bails
on the incomplete texture and keeps the pending set → `glTexImage2D(level 1, …)` → `AllocateStorage` →
**another respecify**, which under `PipeApply.cpp:1485` clears the *whole* `PendingUploads` → level 0's texels
exist nowhere. `glGenerateMipmap` is worse: `GL_Texture.cpp:524-536` emits one respecify per generated level
plus a `TruncateMipmapLevels`. So wire's C1 fix (scope the clear to the redefined level) is a **hard
prerequisite for the texture flip**, not a tidy-up, and M3 above is the client half of the same hole.

---

## 3. The exact rework list

### 3.1 c0b reconciliation (ID-13) — every site that changes

`MG_Impl/Pipe/TextureEmit.h`, emitter class — **rename to contract-v2 §3.4's exact spellings** (the wired
constant makes a mismatch a compile error in B's own commit):

| now | must become |
|---|---|
| `EmitTextureCreate` / `EmitTextureCreateFromBase` | one `void EmitResourceCreate(MG_State::GLState::ITextureObject&)`. **Keep the target-derived storage kind** — the hook is still called from `TextureObjectBase`'s constructor, so `GetStorageType()` is still pure-virtual there and `MGPipeTextureStorageKindForTarget` is still the only legal source. Bind the reference to `*this`; read only non-virtual members. |
| `EmitTextureRespecify` | `void EmitResourceRespecify(MG_State::GLState::ITextureObject&)` |
| `EmitTextureParams` | unchanged name, already matches |
| `NoteLevelDirty(ITextureObject&, MobileGL::TextureUploadTarget, Uint)` | `NoteLevelDirty(ITextureObject& storageOwner, Uint32 uploadTarget, Uint32 level)` — parameter types change |
| `EmitRenderbufferCreate` / `EmitRenderbufferRespecify` | unchanged names, already match |

Free functions in `TextureEmit.h` — **delete all eight** and repoint the MG_State call sites:

| delete | MG_State site now calling it | calls instead |
|---|---|---|
| `MGPipeMintAndCreateTexture` (`:992`) | `TextureObject.cpp:248` (ctor) | `MGPipeMintTextureHandle(*this)` **and** `MGPipeEmitTextureResourceCreate(*this)` |
| `MGPipeEmitTextureRespecify` (`:1002`) | `TextureObject.cpp:216` (`PipePublishDescriptor`) | `MGPipeEmitTextureResourceRespecify` |
| **`MGPipeEmitTextureParams` (`:1007`)** | `TextureObject.cpp:220` | `MGPipeEmitTextureParams` — **name collision**: `PipeMutation.h:301-358` declares this exact signature and `PipeFill.cpp` defines it. B's `inline` definition is a second definition of the same function and **will not compile after the rebase.** |
| `MGPipeNoteTextureLevelDirty` (`:1012`, 4 params incl. `Bool dirty`) | `TextureObject.cpp:225` | see §3.2 — the contract's hook has **no clean arm** |
| `MGPipeMintAndCreateRenderbuffer` (`:1023`) | `RenderbufferObject.cpp:67` (ctor) | `MGPipeMintRenderbufferHandle` **and** `MGPipeEmitRenderbufferResourceCreate` |
| `MGPipeEmitRenderbufferRespecify` (`:1032`) | `RenderbufferObject.cpp:131` | `MGPipeEmitRenderbufferResourceRespecify` |
| `MGPipeEmitTextureResourceDestroy` (`:1060`) | `TextureObject.cpp:209` | **delete the call** — c0b's helper emits the destroy itself |
| `MGPipeEmitRenderbufferResourceDestroy` (`:1072`) | `RenderbufferObject.cpp:88` | **delete the call** |

Publication latch — **deviation D17 makes the latch A's, not B's**:

- delete `Entry::Published`, `TextureRecordIsPublished`, `RenderbufferRecordIsPublished`,
  `NoteTextureRecordDestroyed`, `NoteRenderbufferRecordDestroyed`, `PublishedIn`, `Retire`
  (`TextureEmit.h:503-517`, `:860-869`);
- call `MGPipeNoteHandlePublished(MGPipeKind::Texture|Renderbuffer, handle)` in `EmitResourceCreate` /
  `EmitRenderbufferCreate` **and** in the self-healing create branch at `:601-608` / `:676-682`;
- keep the self-heal — it is still needed and c0b does not provide it.

Also: `MGPipeMintTextureHandle` is pure allocator work and does **not** populate `Entry::Texture`, which
`ResolveTexture` needs at drain time. Confirm every path that can put a level on the drain list has first run
through `NoteLevelDirty`'s own `AcquireTexture` (it does today), or populate the entry from the mint.

Finally, `MGPipeTextureEmitter::ArmForTest` becomes redundant once the constant is flipped — `PipeFill.cpp`'s
`FamilyIsLive(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem)` gate does not consult it, so
after the rebase the two suites' `ArmForTest(true)` no longer arms anything the frontend drives. **Delete
`ArmForTest` and `m_armed` in the same commit as the flip**, and let `MGPipeTextureSubsystemEnabled()` be the
config-bit test alone; leaving it is a latch that lies.

### 3.2 The one thing c0b does not give B

`MGPipeNoteTextureLevelDirty(ITextureObject&, Uint32, Uint32)` has **no `dirty = false` arm**, but B needs one:
`TextureObjectWithOneMipmap::MarkStorageDirty(t, l, false)` and `GL_Texture.cpp:529`'s generated-mip clean both
have to reach `NoteLevelClean`. Options, in preference order: (a) ask A to widen the hook to carry the flag
(one parameter, and it is the honest shape); (b) keep a second B-owned entry point and record it as a fresh
deviation. **Do not** silently drop the clean arm — the drain list then keeps keys for levels that are no
longer dirty and `EmitOneLevel`'s `IsStorageDirty` early-out becomes the only thing collecting them.

### 3.3 c0c helper deletions and the `TextureTarget` fill (ID-12)

- delete `TextureEmit.h:162-176` (`MGPipePackSubDataTarget` / `…ResourceTargetOf` / `…UploadTargetOf` and the
  `static_assert`) — they are now `MGPipeTypes.h`'s, same names, `Uint32` arguments;
- delete `TextureEmit.h:182-183` (`kMGPipeDepthStencilModeDepth/Stencil`), keep `MGPipeDepthStencilModeByte`;
- delete `FramebufferEmit.h:73-77` (`kMGPipeSurfaceKind*` and the `static_assert`);
- **fill `MGPSurface::TextureTarget`** (was `Pad0`) in `MGPipeBuildSurface`
  (`FramebufferEmit.h:101-124`): `static_cast<Uint16>(texture->GetTarget())` on the texture arm, `0xFFFF` on
  the renderbuffer arm **and on the empty-attachment early return at `:104`** (a zero-initialised `MGPSurface`
  would otherwise say `TextureTarget = 0`, not Unknown — check what c0c's field default requires);
- **add it to the hash**: `MGPipeCopySurfaceForHash` (`:164-172`) must copy the new field, or a texture-target
  change alone is suppressed. It is now a `PipeFields.def` row, so the field-wise copy is mandatory;
- apply m6 in the same edit: `UploadTarget = Unknown` rather than `0` on non-texture points, and move
  `FramebufferEmitTest`'s `Color[1].UploadTarget == 0u` assertion with it.

### 3.4 ID-14 — the built-in sampler acquisition, and an ordering problem

`TextureEmit.h:636-637` must stop calling `MGPipeSlots().Acquire(MGPipeKind::SamplerCso,
sampler->GetLifetimeId())` and instead obtain the handle from C's content-addressed cache
(`MGPipeSamplerCsoCache::Acquire(params, payloadBytes)`, `SamplerEmit.h` on `p4a/clientsp`); `TextureEmit.h`
may include `SamplerEmit.h`, and `check_include_closure.py --mode both --require-all` must be re-run.
Re-emit `set_texture_params` whenever the built-in sampler's parameters move — **which is M2**, so the two are
one edit. Drop the per-texture `SamplerCso` death path (there is none today; keep it that way) and delete D11.

**Ordering problem the integrator must resolve.** `SamplerEmit.h` on `p4a/clientfb` is still the contract stub
(76 lines, no cache), and D.1's order integrates `clientfb` **before** `clientsp`. So B cannot make this edit
on its own branch. Either (a) land B's rework with D10 still identity-addressed and a `TODO`-free declared
deviation, then make the acquisition a first-class item of **C's** verification round on the integrated tree
(with `TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` as its gate — it SKIPs today with the reason
printed, which is the right shape), or (b) swap the two packages' integration order. (a) is cheaper and is
what §9 already anticipates; say which, in writing, before B's rework starts.

### 3.5 The wired-bit flip

`TextureEmit.h:98`: `= 0` → `= kMGPipeSubsystemTextureResources`, **in the same commit as the rebase**, per
§9 and per m7. Verified: `PipeFill.cpp:1292-1310` and `:1934` make this the entire switch; the four
`static_assert`s and the `EmittedCallSuppliesTheWholeField` rows are untouched by it.

### 3.6 The findings

M1 (pass `m_regions.data()`), M2 (§3.4), M3 (acceptance signal — joint with wire), M4 (republish on a mask
change), m1, m2, m3, m6 (folded into §3.3). m5, m7, m8, m9 may be **declared** rather than fixed.

---

## 4. What I checked and did not fault

- **Handle minting and death (D-A/D-B, ID-8).** One handle per texture object including views, buffer
  textures and cube maps (the cube's six faces are six blobs on one handle, `TextureEmit.h:220-224` — right).
  Stale-generation refusal is present and doubled in `ResolveTexture` (`:466-473`: entry gen **and**
  `MGPipeSlots().GenOfSlot`). `MGPipeSlots().Acquire` is `FindByLifetimeId`-then-allocate
  (`SlotAllocator.h:73-74`), so the repeated `AcquireTexture` on the respecify / params / dirty paths is
  idempotent and mints nothing — I checked this specifically, because a mint-per-call would have been a slot
  leak on the hottest path in the package. Destroy-then-notify-then-free order is exactly `PipeMutation.h`'s.
  The `ObjectLifetimeIdTest` extension is a genuine 64-round churn check under push. No leak-test gap found;
  the DirectVulkan lane matters here only through the death helpers, which are backend-neutral by
  construction (no op table involved at all — D-B1).
- **Exit-order rule (ID-8).** Both emitters are `new`'d singletons, never destroyed
  (`TextureEmit.h:971-978`, `FramebufferEmit.h:466-472`), with the right reason stated
  (`~TextureObjectBase` reads *and writes* the emitter's tables through the death path). `Entry::Texture` is a
  **raw** pointer — no new static holder of a frontend `SharedPtr` anywhere in the package. Clean.
- **MGLOG discipline.** Exactly two log statements, both `MGLOG_E_ONCE`, both on real error paths. No
  `MGLOG_I`, no `printf`, no `TODO`/`FIXME`, no macros defined (so no `#undef` hygiene question).
- **G1 mechanism (dimension 8).** Not macros and not separate TUs: *every* MG_State edit is a statement (or a
  declaration) inside the file's existing `#if MOBILEGL_PIPE_PUSH`, including the three
  `TextureObjectBase::PipePublish*` **declarations** in `TextureObject.h:446-472`, the inline body inside
  `SetDepthStencilTextureMode` (`:190-198`), `RenderbufferObject::PipePublishDescriptor`'s declaration
  (`RenderbufferObject.h:145-151`), and `TextureObjectBuffer::SetBufferRange`'s call (a header inlined into
  many TUs). **No new data member and no new virtual anywhere** — so the pull build's preprocessed text is
  unchanged and P4a's admitted-resize set stays empty. The report's 0/0/0/0 with `.text` 10806323 → 10806323
  is consistent with what the diff can do. P3a's eleven untouched: nothing in the file list can reach them.
- **The 22 emitter cases + pull==push names (dimension 7).** 14 `TextureEmit` + 8 `FramebufferEmit`; the two
  `MGL_*_CLIENT_TEST_LIST` pull-skip macros name exactly the same 14 and 8, so pull and push register
  identical names and the report's empty `diff` is structurally guaranteed. Reading each body against its
  reason: 20 of 22 can go red for the reason they claim. The two that cannot are called out above —
  `ABailedLevelStaysDirtyAndStaysOnTheDrainList`'s second half (M3: it says "accepted" and tests "dispatched")
  and `AnImageBoundTextureCarriesTheImageBindableHintForever` (M4: it tests the order in which the bug does
  not occur). `MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero` reads the storage's own answer as
  the oracle before the drain clears it, which is the right shape.
- **`ArmForTest` gating.** Both scopes arm `MG_Config::Features.PipePush` explicitly and restore it, and both
  call `ResetForTest()` on entry *and* exit, so no case leaks an armed emitter into the next. The report's
  finding 2 (`Features.PipePush` defaults to 0, so an unarmed case is green for the wrong reason) is real and
  correctly handled. See §3.1 for why `ArmForTest` must not survive the flip.
- **`EmittedCallSuppliesTheWholeField`.** `PipeFill.cpp` untouched, all six P4a rows in the `false` arm.
  `GetFramebufferBindingSlot` = **NO** is right and the reason is exactly P3a's `GetBoundVertexArray`
  precedent: the field's storage is a frontend heap reference and the call carries handles. The consequence B
  draws — this package retires no pull at all, so the 79-case verify retrace reports zero divergences rather
  than "no divergence we looked for" — is honest and worth keeping in the record.
- **File list vs C.7.** 13 files: `MG_Impl/Pipe/{Framebuffer,Texture}Emit.h` (B's), `MG_State/GLState/`
  `{FramebufferState,TextureState,RenderbufferState}/**` (B's), `MG_Test/Pipe/{Framebuffer,Texture}EmitTest.cpp`
  (B's `b3` half), and the one granted `MG_Test/State/ObjectLifetimeIdTest.cpp`. **Nothing outside.** The two
  GLImpl grants were declined — correctly for `GL_Framebuffer.cpp`, **incorrectly for `GL_Texture.cpp`**
  (M2 needs it).
- **The LFS trap** (§8's retrace note) is already ID-13's ruling; every reviewer worktree runs
  `git lfs checkout` first. I ran no retrace.

---

## 5. What the integrator must re-run on the integrated tree

B's §9 order is right as far as it goes. Add to it:

1. **Before anything**: confirm wire's C1 fix (scoped `PendingUploads` clear) is in the tree. The texture flip
   without it is the mipmap-chain data loss in §2. This is a **gate**, not a preference.
2. `ctest -L unit` in **build-verify**, alone and first. Named expected-failing-before set, re-stated
   unchanged from B's §9: `TextureEmit`'s four drain cases
   (`TheUnionBoxAndTheRegionListDescribeTheSameTexels`, `AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero`,
   `AWholeLevelUploadCarriesZeroStrides`, `MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero`) plus
   `TextureTest.GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined` — five, and they must be **green after**
   the flip on a tree carrying `w1`. Any of them still red means `w1` did not do what its report says.
3. `ctest -L integration-gpu` on build-push at the **default** mask and at **`0x1ff`**. Also add a third arm:
   the D-K2 dependency refusals must fire — `0x9ff`-shaped (framebuffer without textures) and `0x5ff`-shaped
   (textures without samplers, ID-15's H1) must be **refused with one `MGLOG_E`**, not half-run. That is
   package F's scenario, but the integrated tree is the first place it is answerable.
4. `ctest -L integration-verify` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, for
   `~TextureObjectBase` and `~RenderbufferObject` reaching a live applier — **on both lanes**, DirectVulkan
   included (ID-8's per-kind leak test).
5. The 79-case verify retrace (`git lfs checkout` first). It is the only thing that drives the drain list on
   real atlas traffic, and after M1 is fixed it is the only place the region tail is exercised at volume.
   Compare `PipeStats::ClientTextureUploadEmissions` against the server-side
   `TextureUpload{Emissions,Box,Rect,Jobs}` — a rect count of zero on `rei`/`xaero-*` after M1's fix means the
   tail is still not arriving.
6. **G1** after the flip, because it moves a `constexpr` in a push-only header.
7. `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` after §3.4 adds
   `SamplerEmit.h` to `TextureEmit.h`'s closure.
8. **The residual-record sweep** ID-12 names: the 183 "no applier record" refusals on the default arm must go
   to **zero** once B and C have landed. A residual one on a texture or renderbuffer handle is a B seam defect.
9. **New, and it is the check M4 exists for**: a scenario (or a unit case) that allocates immutable storage
   **and then** binds the texture as an image / attaches it, and asserts the applier's record carries
   `ImageBindableHint = 1` and the right `BindMask`. Without it M4's fix is unverified.

---

*Reviewer note.* Everything above is from reading `p4a/clientfb`, `p4a/wire`, `feat/disaggregated` and the
frontend at `08192d72`. No build, no device, no worktree created; nothing on the shared box was modified.
