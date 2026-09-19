# P4a package D — `p4a/esprytobj` (the twins and the slot tables), v1

Branch `refs/heads/p4a/esprytobj`, worktree `~/w7/p4a-esprytobj`, based on the tag `p4a/contract`
(`08192d72`). Files touched: `MG_Backend/DirectGLES/{Managers.h, Managers.cpp}` and
`MG_Test/SanityTest.cpp` — **nothing else**. `SlotTables.h` needed no edit: `GetOrCreate(handle)`,
`FindByHandle`, `ReleaseByHandle` and `LiveGenAt` were already there from P3a, which is what C.3's
`d1` is written against.

---

## 1. Commits

| step | hash | message |
|---|---|---|
| d1 | `7aa598fa` | `[Refactor] (Espryt): key the texture, renderbuffer, framebuffer, sampler and program twins on the client-minted handle instead of the frontend object` |
| d2 | `c1d6b41c` | `[Refactor] (Espryt): drive texture storage, parameters and uploads from the pushed descriptors and take the upload strides from the record` |
| d3 | `49ff8d91` | `[Refactor] (Espryt): allocate renderbuffer storage from the pushed descriptor and keep the deferred out-of-memory report where it is` |
| d4 | `7e400454` | `[Refactor] (Espryt): sync the driver framebuffer from the pushed record and answer the read buffer from the resolved read surface` |
| d5 | `a69819e4` | `[Refactor] (Espryt): take the sampler parameters and the program artefacts from the applier instead of the frontend objects` |
| d6 | `9bb95335` | `[Test] (Espryt): drive the sampler-view table through the handle arm and prove every P4a death notice is idempotent` |

C.3 gives no message for step 6; DV-10 records the one used.

---

## 2. Per-step summary — every memo and member re-keyed, retired or added

This section is the Track H census input. **Nothing is deleted.** Every pre-handle member stays
beside its replacement under `MOBILEGL_PIPE_LEGACY_MEMOS`, which `ARCHITECTURE.md:369` keeps
compiled through P3a/P4a and which is what makes the four subsystem bits a real A/B rather than a
bring-up-order question.

### d1 — the handle-keyed API, the sixth table, the four arm resolvers

**Added to `StateBackendObjectRegistry` (push-only, so the pull build's mangled names are the
pre-P2 two-argument ones):**

| member | what it is |
|---|---|
| `GetOrCreateByHandle(MGPipeHandle)` | resolve-or-create by the handle the CALL carried. Returns a POINTER, not the reference the minting overload returns, because it has three ways to decline: the legacy arm is running, the slot is past `kMaxHandleSlot`, or the generation is BEHIND the live entry's |
| `LiveGenAt(Uint32 slot)` | the live entry's generation, or 0 — so a release build, where `MOBILEGL_ASSERT` is inert, can DIAGNOSE the refusal the table performs silently |
| `ReleaseByHandle(MGPipeHandle)` | hands the twin OUT rather than destroying it in place; the SLOT is not freed, the client frees it after the destroy returns |

**Added at `DirectGLES` namespace scope (push-only):**

- `AdoptTwinByHandle(registry, handle, kindName)` — one anonymous-namespace template holding the
  release-build VOICE for the two silent refusals, shared by the five kinds so the wording cannot
  drift between them. It is P3a's `GetOrCreateBufferResourceForHandle` shape lifted into one place.
- `ResolveFramebufferSubsystemArm` / `ResolveTextureResourceSubsystemArm` /
  `ResolveSamplerSubsystemArm` / `ResolveProgramSubsystemArm`, plus the four inline latches
  `FramebufferSubsystemEnabled()` / `TextureResourceSubsystemEnabled()` /
  `SamplerSubsystemEnabled()` / `ProgramSubsystemEnabled()`. Lazy at first use, never at bring-up
  (a forked pre-flight child dying on a signal makes a whole lane SKIP green). All four reuse
  `BufferImpl::ClassifyPipeSubsystemArm` / `StopOnArmlessPipeSubsystem` unchanged and all four pass
  `legacyArmSurvivesLegacyMemos = false`, which is D-K3's table verbatim.
- The three dependency refusals, each one `MGLOG_E` naming both bits and then the legacy arm:
  **bit 11 requires bit 10**, **bit 9 requires bit 10**, **bit 10 requires bit 7**. The mirror
  pairs are stated as fine in the source, because an unreachable branch that says something
  different is how the reachable one drifts. Bit 12 depends on nothing and says so.
- **D-C3's `MaxColorAttachments > 8` refusal**, on `ResolveFramebufferSubsystemArm`: reads
  `g_GLESCapabilities.MaxColorAttachments` (the driver's RAW ES cap, which
  `BackendObject_DirectGLES.cpp:1518` publishes verbatim and which is NOT clamped to 8 on this
  path), refuses bit 9 naming the cap, and runs the legacy arm.

**The sixth table — `namespace SamplerViewImpl` (push-only):**

- `BackendSamplerViewObject` — `{MGPSamplerView View; Uint64 SyncedSerial; Uint64
  SyncedSamplerSerial; Bool NeedsRawDepthFetchSampler;}`. It owns **no driver id**: there is
  nothing in ES to create for a view, the id the unit binds is the texture's, and what this twin is
  is the server's memo of one resolved view plus Espryt's raw-depth-fetch substitution decision —
  one of the two backend post-processings `ARCHITECTURE.md:206` keeps on the server. No destructor
  is declared, and that is deliberate: nothing here owns a GPU object, so the teardown sentinel's
  whole reason does not apply and the defaulted one is correct in every teardown order.
- `BackendSamplerViewTable` = `BackendSlotTable<ITextureObject, BackendSamplerViewObject,
  SamplerViewCso>` and the global `g_backendSamplerViews`. Handle-keyed only, exactly like P3a's
  `BackendBufferResourceTable`; the `StateObject` parameter names `ITextureObject` because the view
  is minted off the TEXTURE's lifetime id (D-F2), which is what makes `HandleOf` resolve at all.
- `GetOrCreateSamplerViewForHandle` / `FindSamplerViewForHandle` /
  `HandleOfSamplerViewForTexture` (monolith glue, named as such).
- **`OnFrontendStateObjectDestroyed` gains a `SamplerViewCso` arm.** §3 below is its idempotence
  argument.

### d2 — the texture twin

| retired as a KEY | replaced by | member kept? |
|---|---|---|
| `m_syncedContentVersion` (`Managers.h:1468`) | `m_syncedResourceSerial` = the resource record's `Serial` | yes, legacy arm |
| `m_syncedShapeContextId` / `m_syncedShapeGeneration` / `m_syncedShapeParamsVersion` (`:1481-1483`) — the cheap-gate trio | the same one `Serial` compare, plus "the pending-upload set is empty" | yes, legacy arm |
| `m_syncedTextureParamsVersion` (`:1503`) | `m_syncedParamsSerial` = the record's `ParamsSerial` | yes, legacy arm |
| `m_forceTextureParamsResync` (`:1507`) | `MGPTextureParams::ForceResync` on the wire — READ, acted on, and **never written back** (D-E2); the twin's own flag is still cleared here exactly as before | yes, both |
| `m_syncedSamplerVersion` (texture-owned, `:1502`) | `m_syncedBuiltinSampler` + `m_syncedBuiltinSamplerSerial` — the HANDLE is part of the key, not just the serial, because sampler CSOs are content-addressed and a texture whose sampling diverges is re-pointed at a different CSO whose serial may be SMALLER | yes, legacy arm |
| `m_forceSamplerResync` (`:1516`) | `MGPTextureParams::SamplerResync` — the byte that had no wire spelling at all before P4a | yes, both |
| the frontend dirty model (`IsStorageDirty` / `MarkStorageDirty` / `GetStorageDirtyRegion` / `GetStorageDirtyRects`) | the applier record's `PendingUploads` set, read through `FindPipeTextureUpload` and cleared through `ConsumePipeTextureUpload` **only where the level actually uploaded** | yes, legacy arm |

**New members (all `#if MOBILEGL_PIPE_PUSH`):** `BackendTextureObject::{m_syncedResourceSerial,
m_syncedParamsSerial, m_syncedBuiltinSamplerSerial, m_syncedBuiltinSampler}`.
**New members functions:** `ResolvePushedBuiltinSampler`, `ResolvePushedTextureParams`.

**The upload shape.** The DECISION stays on the server verbatim — `subRectEligible`, the
`UnpackRingAvailable()` "one box, one job" collapse, and the three staging shapes are unchanged.
What moves is the SOURCE: the union box comes from `PendingUpload::UnionBox`, the rect list from
`PendingUpload::Regions`, and the **strides are carried, never inferred** —
`MGPSubRegion::SrcRowStride` / `SrcSliceStride`, with 0 meaning tightly packed, which is what a
whole-level region sends. `StageBlocksIntoUnpackRing`, `UnpackRingAvailable` and
`UnpackRingAllocate` are byte-identical (§4).

**`MGPipeUnmigratedEmulation("texture-remint-pull")`** is planted at
`RequireImageBindableStorage`'s re-dirty of already-uploaded levels — the one D-M site that lives
in this package's files. The other four are `DirectGLES.cpp`'s and are package E's.

### d3 — the renderbuffer twin

| retired as a KEY | replaced by |
|---|---|
| `m_cacheInternalFormat` / `m_cacheWidth` / `m_cacheHeight` / `m_cacheSamples` (`Managers.h:2251-2254`) as the four-field GATE | `m_syncedResourceSerial` = the record's `Serial` |

The four members stay and are still written, because they are also what the legacy arm compares and
what the twin reports about the storage it actually holds. The four allocation values now come from
`Desc.InternalFormat/Width/Height/Samples`. **The `GL_OUT_OF_MEMORY` probe and its `RecordError`
are untouched**, and the serial is stamped AFTER the allocation so a refused allocation leaves the
twin describing what it holds. D-D2's publication hole (`SetInternalFormat` / `AllocateStorage` /
`SetSamples` bump no version and raise no notice, so re-storaging an ALREADY-ATTACHED renderbuffer
was invisible) is closed by the client emitting from those three mutators; this side needs only the
one serial compare, which is what it now has.

### d4 — the framebuffer twin

| retired as a KEY | replaced by |
|---|---|
| `m_syncedFrontendAttachmentVersions[41]` (`Managers.h:1604`) as the attachment walk's KEY | `m_syncedRecordHashes[target]` = `MGPFramebufferState::ContentHash`, which is the hash's SECOND job — "the server's render-pass memo key, and the CLIENT's emission suppressor" |
| `stateFBOObject->GetDrawBuffers()` | `MGPFramebufferState::DrawBuffers[8]`, decoded by `DecodePushedDrawBuffers` |
| `stateFBOObject->GetReadBuffer()` | `MGPFramebufferState::ReadSurface`, matched against `Color[]` to recover the attachment POINT |

**New members / functions (push-only):** `BackendFramebufferObject::m_syncedRecordHashes`,
`FramebufferImpl::PushedFramebufferRecord`, `FramebufferImpl::DecodePushedDrawBuffers`.

- `PushedFramebufferRecord(asTarget, fbo)` matches the applier's per-target working state against
  **this twin's own handle**. A mismatch means the object being synced is not the one currently
  bound to that target — a real sequence (a scratch FBO synced while another is bound) — and is
  answered with null rather than with the other framebuffer's attachments.
- The record's own `Target` is validated against the binding it is being applied to; `Both` is
  legal for either, anything else is refused and named. That is the D-C2 property: the OIT bug at
  `Managers.cpp:7544-7551`, where a READ-only sync landed `glDrawBuffers` on the wrong framebuffer
  and falsely stamped the memo, becomes a one-line test on carried data instead of a call-site
  discipline nobody can check.
- **Survives untouched:** `g_attachmentBackendIdGeneration`, the `~0` re-arm it drives, the
  re-entrancy loop until it is quiescent, `RecomputeBackendColorSlots` (byte-identical), the
  empty-colour-point detach, and the draw-buffer `None -> GL_NONE` handling.
- **Over-firing is free and under-firing is fatal**, so the per-attachment version array stays as a
  narrower SECOND gate underneath the hash: an attachment that moved without the record moving
  still re-attaches.

### d5 — the sampler and program twins

| retired as a KEY | replaced by |
|---|---|
| `BackendSamplerObject::m_syncedSamplerVersion` (`Managers.h:2222`) | `m_syncedSamplerSerial` = the SamplerCso record's `Serial` |
| `BackendProgramObjectImpl::m_syncedLinkVersion` + `m_syncedImageUnitVersion` (`:2354-2355`) | `m_syncedShaderCsoSerial` = the ShaderCso record's `Serial`, exposed as `GetSyncedShaderCsoSerial()` for the draw path |

All four border-colour comparands (float, int, uint, **form**) are still compared on the sampler
side, reading the CSO record's `SamplerParameters` instead of the frontend object — D-F4, and the
reason is unchanged: two integer borders differing above 2^24 share one float, and a Float→Int
transition can leave every number unchanged while needing a different driver entry point.

The frontend/CSO asymmetry that must survive, survives: the texture path resolves its min filter
with `IsAngleLlvmpipeRenderer()` and the sampler-object path with
`ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe()`. Both are server-side choices made from the
same value, so nothing about them moves.

**The program artefacts deliberately do NOT move** — see DV-8. That is D-H3's ruling, not a
shortcut.

### d6 — `SanityTest.cpp`

- `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` gains a **SamplerViewCso** block (the
  five `TwinRegistry` kinds and the Buffer table were already covered): handle minted off the
  texture's lifetime id, the two kinds' slot spaces proven independent for the SAME lifetime id,
  `GetOrCreate` / `FindByHandle` / `HandleOf` / `ReleaseByHandle`, the recycle with a moved `Gen`,
  and the **backward-generation refusal**.
- **New:** `DirectGLESSlotTable.ADeathNoticeForEveryP4aKindIsIdempotent`, driven through the REAL
  consumer the backend installed (`GetStateObjectDeathOps()->OnDestroyed`) rather than a recording
  stub, because what is under test is Espryt's own arm. Skip stub added in the pull arm so G2/G14
  hold (name-for-name identical lists, additions only).
- `ScopedDirectGLESTextureBindings` (`:218-260`, the second live holder of kind Texture),
  `ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice` and
  `PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero` all pass unchanged.

---

## 3. The death-notice idempotence argument, per kind (ID-8)

The rule: **the client emits the wire delete, raises `NotifyStateObjectDestroyed(kind, lifetimeId)`
and frees the slot, in that order**, and the backend notice is the REDUNDANT SECOND PATH.

The mechanism that makes all six idempotent is one property of the allocator plus one of the table:

1. `MGPipeSlotAllocator::Free` **erases the lifetimeId → slot mapping**, so the second resolution of
   the same id finds nothing; and a `Free` on a slot that is no longer live at that generation is a
   proven no-op (`SlotAllocator.cpp:117-119`), with the `Gen` bump riding the next handout rather
   than the free, so a double free cannot skip a generation.
2. `BackendSlotTable::OnFrontendObjectDestroyed` **returns false without touching anything** when
   `FindByLifetimeId` answers the null handle, and `ReleaseTwinAt` returns false for an entry whose
   `Gen` no longer matches — so a notice that arrives after the client's `Free` cannot drop a
   successor's twin.

| kind | first path | second path | why the second is a no-op |
|---|---|---|---|
| Texture | `~TextureObjectBase` raises the notice; Espryt drops the twin in every holder and frees the slot | the client helper `MGPipeEmitTextureDestroyAndFree` emits `ResourceDestroy`, raises the same notice and frees | whichever gets there first erases the mapping; the other resolves the null handle. The twin's driver id is released exactly once, by `ReleaseTwinAt`, which retires the entry BEFORE running the destructor |
| Renderbuffer | same shape | `MGPipeEmitRenderbufferDestroyAndFree` | same |
| Framebuffer | same shape | `MGPipeEmitFramebufferDestroyAndFree`, which emits **no wire call at all** (D-I2: a framebuffer has a handle but no wire lifetime) — it is notice-then-free only | same |
| SamplerCso | `~SamplerObject` | `MGPipeEmitSamplerCsoDestroyAndFree` | same. A texture's death legitimately raises a SamplerCso notice too, because every `ITextureObject` owns a private `SamplerObject` with its **own** lifetime id (contract D5), so the two notices name two different ids and neither can free the other's slot |
| ShaderCso | `~ProgramObject` | `MGPipeEmitShaderCsoDestroyAndFree`, called **twice** for a composite (the pipeline cache's LRU eviction and the composite's own destructor) | the composite band is a client-side indexing detail; on this side a composite handle is an ordinary ShaderCso handle and both calls resolve through the same table. The second finds nothing |
| **SamplerViewCso** | **nothing raises it today** — the view has no frontend object; the only raiser is the client helper `MGPipeEmitSamplerViewCsoDestroyAndFree`, which fires from `~TextureObjectBase`'s body | the same helper's own `Free` | the notice names the TEXTURE's lifetime id, and it is safe for it to arrive beside the Texture notice for the same id because the two kinds have **separate slot spaces** — the allocator resolves `(kind, lifetimeId)`, so each arm finds its own slot or nothing. The twin owns no driver id at all, so even a double release is a pointer reset |

`ADeathNoticeForEveryP4aKindIsIdempotent` exercises exactly this: for the sampler view, notice →
notice → client `Free` → notice, then a successor acquire proving the slot really came back with a
moved generation (which a double free would break by skipping one); for the five object kinds, the
real destructor followed by two more deliveries.

**Leak-freedom.** No new table has a garbage collector and none needs one: the sampler-view table's
entries leave on the notice, and `SamplerViewImpl`'s twin owns nothing a sweep could reclaim. No new
static holds a frontend `SharedPtr` (the exit-order rule, ID-8): `g_backendSamplerViews` is a
`BackendSlotTable`, whose `Entry::stateRef` is a `weak_ptr` and is never even written on the
handle-keyed overload, and it links itself into the per-type holder list from its own constructor
with no initialisation-order question to answer. It adds **no** `std::atexit` registration of its
own; `EnsureProcessTeardownSentinel()` is still armed by the first insertion of a table whose twin
owns a driver id.

---

## 4. The byte-identical set (G5) — evidence

`scripts/p4a_untouched_regions.sh` is package **F**'s and does not exist on this tree, so the
evidence below was produced with a one-off equivalent that copies the parent script's shape:
mask-first, preprocessor-blind extraction (comments and string/char literals replaced by spaces of
the same length), the definition found as the one `<name> (` whose closing paren is followed by
`{`, the body brace-matched in the masked text and **hashed from the ORIGINAL text**, exactly one
definition per name or exit 2, both arguments git refs.

Four of D-N's seventeen live in this package's files. The other thirteen are P3a's eleven
(`p3a_untouched_regions.sh`, run after every commit, rc 0 every time) plus
`DepthStencilSamplingReadImpl` and `ShouldUseCaveatTextureFormat`, which are in `DirectGLES.cpp`
and `Utils.cpp` — package E's files, untouched here.

```
$ python3 <p4a-dn equivalent> 37da3c3a HEAD
d75537bc1fb9170bf98d8f711c7a86fa229b80a3219f091c487ba56251628b9a  StageBlocksIntoUnpackRing
a0c9994d7647f6d55491767e813bbab07f1ac54b8804bff466e8c53e29356465  UnpackRingAvailable
f56d432fff8d015c08911f846d8c196c3c297261a8301dc8b9c4299d199b95db  UnpackRingAllocate
39a3724922a007509173c330557f6e5f6b938e5b6cd8800817c63ac67ccf4840  RecomputeBackendColorSlots
[p4a-dn] rc=0
```

`ScopedDefaultUnpackState` and its process-wide `s_synced` shadow are untouched, and the ring path
still issues no `glPixelStorei`, so D-O's inherited hazard is neither fixed (`ROADMAP.md:98`) nor
made worse.

---

## 5. Deviations, each with its reason

**DV-1 — d1's message names a re-key the commit only half performs, and that is a C.7 boundary.**
C.3's `d1` says "the five kinds' tables re-keyed onto client-minted handles". The *resolution sites*
for framebuffer, sampler and program are `SyncCurrentFBO`, the lazy sampler mint at
`DirectGLES.cpp:4014-4020` and `SyncCurrentProgram` — all in `DirectGLES.cpp`, which C.7 gives to
**package E**. `SyncTextureObjectToBackend` is likewise defined at `DirectGLES.cpp:1501`. So d1
lands everything the re-key needs on this side — the handle-keyed registry API, the shared refusal
voice, the sixth table and the four arm resolvers — and each family's own resolution moves with its
commit (d2–d5) where it lives in `Managers.cpp`. The brief's exact message is used unchanged per
ID-2; this note is the correction.

**DV-2 — `MGPTextureParams::DepthStencilMode`'s encoding was unstated, and Espryt now defines it.**
The field is a `Uint8`; `ITextureObject::GetDepthStencilTextureMode()` is a `GLenum`
(`GL_DEPTH_COMPONENT` 0x1902 / `GL_STENCIL_INDEX` 0x1901), which does not fit in a byte, and the
contract's payload comment says nothing about how it crosses. **Espryt decodes 0 =
`GL_DEPTH_COMPONENT`, 1 = `GL_STENCIL_INDEX`**, and package B must emit that. The direction is
chosen by one property rather than by the enum's low byte: `GL_DEPTH_COMPONENT` is the GL and ES
default and a texture that never asks for the stencil aspect never emits the call, so **a zeroed
record has to decode to exactly what such a texture already has**. *Owner: the integrator, to amend
`MGPipeTypes.h`'s payload comment; package B, to emit it.*

**DV-3 — `MGPSubData::Target`'s upload-target half was unstated.** D-D3 says `Target` carries "the
owner's upload target … plus the cube-face upload target in `MGPSurface`-style terms" without
fixing the encoding. Espryt reads `static_cast<Uint16>(MobileGL::TextureUploadTarget)`, i.e. the
frontend enumerator value, and the applier's `PendingUpload::UploadTarget` is matched against it.
*Owner: same as DV-2.*

**DV-4 — `MGPSurface::Kind`'s enumerator values are unstated, so Espryt does not read them.** The
comment says `Texture | Renderbuffer | None` and no enum exists. Every test this package needed is
expressible without it, because D-C1 fixes the *other* spelling: "an empty point is
`{Res = kMGPipeNullHandle, Kind = None}`". So the framebuffer arm asks
`MGPipeHandleIsNull(surface.Res)` and never touches `Kind`. Nothing is blocked; recorded so the
integrator can mint the enum in the same amendment as DV-2/DV-3 rather than discovering it in
package E.

**DV-5 — the four cross-object masks still read the frontend attachment objects, and this is a
finding rather than a shortcut.** `IsSnormFallbackAttachment`, `IsUnormFallbackAttachment` and
`IsAlphaWidenedColorAttachment` all reduce to `(format, **TextureTarget**)` for a texture
attachment — `ShouldUseCaveatTextureFormat(format, target)` and
`BackendTextureFormatAddsAlpha(format, target)`. `MGPSurface` carries `InternalFormat` and
`UploadTarget` but **no texture target**, and no `TextureUploadTarget -> TextureTarget` inverse
exists anywhere in the tree. Inventing one would feed `ShouldUseCaveatTextureFormat` — which is on
**D-N's byte-identical list**, so a change to what it is *asked* silently changes what every texture
is allocated as — with a guessed input. D-C1 promises the inline `InternalFormat` makes "the four
cross-object masks fall out at push time with no lookup"; that promise needs one more field.
**Recommended fix: widen `MGPSurface`'s `Pad0` (2 bytes, already there) into `Uint16 TextureTarget`,
which keeps `MGP_ASSERT_POD(MGPSurface, 24)`.** *Owner: the contract (package A) + the integrator.
Until then this package leaves the four masks reading the frontend, unchanged and correct.*

**DV-6 — `MGPFramebufferState::DrawBuffers[]` cannot spell the default framebuffer's tokens, and
that is provably harmless here.** The wire array is an attachment INDEX with -1 for `None` (D-C1),
so `FrontLeft` / `FrontRight` / `BackLeft` / `BackRight` are unrepresentable. Those are the DEFAULT
framebuffer's own tokens; the default framebuffer has **no `BackendFramebufferObject`** (it is
`pDefaultFramebufferInfo->defaultFBO`, which is exactly why `MGPipeHandles.h` reserves `{0,1}` for
it), and the legacy arm's branch for them already says "shouldn't remap". The decode is therefore
total for every object that reaches this twin, and `IsDefault` is what a later phase would test if
that ever changed.

**DV-7 — the framebuffer attachment WALK still reads the frontend attachment objects.** What moved
is the KEY (DV-5's sibling): `SyncAttachmentObject` drives the texture and renderbuffer twins
*through the frontend objects the monolith hands over*, and resolving them from `MGPSurface::Res`
instead is package E's `SyncAttachmentObject` work — and is blocked on DV-5's missing target field
for the same reason. Recorded so the integrator does not read the ContentHash re-key as the whole
of D-C.

**DV-8 — the program twin still reads the frontend archive, and D-H3 says it must.** "THE ARTEFACTS
ARE NOT HELD HERE IN MONOLITH: … the applier stores the DESCRIPTOR and the identity and the server
reads the frontend's own archive." That is what keeps the codec off the monolith hot path entirely
and out of ID-19's budget. So `BackendProgramObjectImpl::SyncToBackend`'s artefact reads are
correct as they stand, and what d5 moves is the identity and the serial. Recorded because the
step's own message ("take … the program artefacts from the applier") reads the other way.

**DV-9 — the value sites are PREPROCESSOR MACROS, not helper functions or lambdas, and G1 is why.**
This was measured, not assumed. Inserting two lambdas into `SyncMipmapsToBackend` renumbered every
one of its nine existing `ErrorLopper` lambdas — their mangled names are `…::$_N` **by position** —
for **8 symbols added, 8 removed and 4 `std::function` thunks resized** in the PULL build. Routing
`SyncTextureParamsToBackend`'s values through locals resized it **+9 bytes**, and the same shape
cost `SyncBuiltinSamplerToBackend` **-2**. P4a's admitted-resize set is EMPTY, so all three are G1
failures. A macro expands to the pre-P4a expression exactly when `MOBILEGL_PIPE_PUSH` is off, so
the pull build's preprocessed text — and therefore its object code — is unchanged. Every macro is
`#undef`'d immediately after its function. The alternative D-N blesses (a separate
`#if MOBILEGL_PIPE_PUSH` function with its own name) was used where the arms diverge structurally
(`ResolvePushedBuiltinSampler`, `ResolvePushedTextureParams`, `PushedFramebufferRecord`,
`DecodePushedDrawBuffers`) and rejected for `SyncMipmapsToBackend`, which would have meant
duplicating ~950 lines.

**DV-10 — step d6 has no message in C.3.** Used
`[Test] (Espryt): drive the sampler-view table through the handle arm and prove every P4a death
notice is idempotent`.

**DV-11 — the legacy arms are guarded `#if MOBILEGL_PIPE_LEGACY_MEMOS` only inside the push arm.**
`CMakeLists.txt:477-480` forces `MOBILEGL_PIPE_LEGACY_MEMOS` ON whenever `MOBILEGL_PIPE_PUSH` is
OFF, so a pull build always compiles the pre-P4a text; the `#else` branch of each arm switch is
that text verbatim. The `#if !MOBILEGL_PIPE_LEGACY_MEMOS` bodies are unreachable — the arm resolver
stops the process at its first call — and are kept loud, for the reason
`BackendVertexArrayObject::SyncToBackend` keeps its twin.

**DV-12 — `p4a_untouched_regions.sh` does not exist on this tree.** §4 says what was run instead.

---

## 6. Verification transcript

`export CCACHE_BASEDIR=/home/swung/w7` in every shell; commands run from `~/w7/p4a-esprytobj`.

### 6.1 After every commit (G1, unit, P3a's eleven, purity)

Run after **each** of `7aa598fa`, `c1d6b41c`, `49ff8d91`, `7e400454`, `a69819e4`, `9bb95335`:

```
$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0 \
      --fail-on-symbol-set-change --fail-on-added-bytes 0
symbol-report: .text 10806323 -> 10806323 (+0, +0.000%)
symbol-report: 27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
                                                                                  rc=0
$ ctest --test-dir build-linux -L unit --no-tests=error -j 12
100% tests passed, 0 tests failed out of 1635        (1636 from d6 on)
$ ctest --test-dir build-push  -L unit --no-tests=error -j 12
100% tests passed, 0 tests failed out of 1635        (1636 from d6 on)
$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
[p3a-untouched] the 11 pool / deferred-release / ring / flush-drain functions are
[p3a-untouched] byte-identical between 37da3c3a and HEAD                          rc=0
$ grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'
MobileGL/MG_Backend/MGPipe/PipeInputs.h:1     # pre-existing at 37da3c3a, unchanged
$ awk '/struct MGPipeResourceOps \{/,/^    \};/' MobileGL/MG_Pipe/PipeApply.h | grep -c MG_State
0                                             # D-B1: no op-table member added, no MG_State type
```

**One G1 failure was found and fixed inside d2, before the commit**, and it is DV-9's evidence: 8
added / 8 removed / 6 resized, every one of them attributable to the two lambdas and the two value
locals. The committed tree of every step is 0/0/0/0.

### 6.2 D-N byte identity

§4. rc 0 for all four functions this package owns; `p3a_untouched_regions.sh` rc 0 for P3a's eleven
after every commit. `git diff 37da3c3a HEAD -- MobileGL/MG_Backend/DirectGLES/Managers.cpp` lands
no hunk inside any of the four.

### 6.3 The legacy-arm lanes

```
$ MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
99% tests passed, 6 tests failed out of 491
$ MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
99% tests passed, 6 tests failed out of 491
```

**Both lanes fail on the SAME six cases and nothing else**, and the two failing sets are identical:

```
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.DestroyedVertexArraysReturnTheirVertexElementsSlots
    (Subprocess aborted)
```

**Explanation, and it is a cross-package sequencing artefact the brief itself predicts — not a
defect in this package, and not a missing applier record.** Those six cases are the `.Handles` arm,
whose ctest `ENVIRONMENT` property pins `MGL_ITEST_HANDLES_ARM_KNOBS =
"MOBILEGL_PIPE_LEGACY_MEMOS=0" "MOBILEGL_PIPE_PUSH=0x1ff"` at
`MobileGL/MG_IntegrationTest/CMakeLists.txt:949`. A ctest `ENVIRONMENT` property **replaces rather
than appends and overrides the job env**, which is why both outer lanes see the identical failure
set. Under that pinned pair, all four P4a families classify as `NoArm` — the bits are clear AND the
operator asked for the pre-handle arms to be gone — and `StopOnArmlessPipeSubsystem` aborts, which
is **exactly what D-K3 rules must happen** ("all four can reach `NoArm` and all four stop with the
named `Fatal{PipeLegacyMemosDisabled, …}` rather than skipping green").

The fix is the pin move D-K1 already mandates and C.7 already assigns: **`0x1ff` → `0x1fff` at
`MG_IntegrationTest/CMakeLists.txt:949, 1065, 1075, 1149, 1183, 1188, 1219` and
`0x80000000000001ff` → `0x8000000000001fff` at `:1070, 1080`, which contract deviation D10 confirms
is package F's** — "pinning it at `0x1ff` after P4a would leave the `Handles` arm asserting the P3a
shape while the handles it is supposed to be about stayed switched off" (`CMakeLists.txt:942-947`'s
own argument). **Until F lands, or the integrator moves those pins when merging `esprytobj`, these
six cases are red on the integrated tree.** Softening the stop would be re-deciding D-K3 inside a
package, which ID-8 forbids, and would make the whole four-family A/B silently unmeasurable.

Everything else in both lanes passes; the skip list is the pre-existing one (the seven
`DirectGLES.*Scenario` entries that skip without an arm selector, the two
`LargeArenaAdoption` capability skips, the forced-D24S8 and async entries).

**Proof, run standalone so the abort's own log line survives** — ctest's ENVIRONMENT property
replaces the job env, so `MOBILEGL_LOG_FILE_PATH` cannot be injected into the lane, and the Fatal
goes to the log rather than to stdout:

```
$ MOBILEGL_LOG_FILE_PATH=/tmp/p4a-d.log MOBILEGL_BACKEND_TYPE=DirectGLES \
    MOBILEGL_PIPE_LEGACY_MEMOS=0 MOBILEGL_PIPE_PUSH=0x1ff \
    build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest \
    --gtest_filter='IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour'
Aborted                                                                       exit=134 (SIGABRT)
[FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, "MOBILEGL_PIPE_PUSH leaves
         kMGPipeSubsystemTextureResources (bit 10) clear (or refuses it) and
         MOBILEGL_PIPE_LEGACY_MEMOS=0 disables the pre-handle texture cheap-gate trio,
         so there is no arm to run"}

$ ...same, with MOBILEGL_PIPE_PUSH=0x1fff (what package F's pin move produces)
exit=1   # no abort; an ordinary scenario failure
[ERROR]: MGPipe: texture 1 has no applier record on the handle arm, so its parameters
         cannot be pushed (handle {1, 0})
```

That second run is the whole argument in two lines: **with the phase mask moved, the armless stop
disappears and what is left is §6.5's missing-record set**, which the client packages close.

### 6.4 The default arm, and every failure classified

```
$ ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
62% tests passed, 189 tests failed out of 491
```

**189 = 183 + 6, and both terms are accounted for.**

- **6** are §6.3's armless stop, unchanged from the other two lanes and owned by package F's pin
  move. They are the only cases that ABORT rather than fail.
- **183** are the missing-applier-record set, and the attribution is a log line rather than an
  argument. Running one of them standalone with the log captured:

```
$ MOBILEGL_LOG_FILE_PATH=/tmp/p4a-c.log MOBILEGL_BACKEND_TYPE=DirectGLES \
    build-push/.../MobileGLIntegrationTest \
    --gtest_filter='IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour'
[  FAILED  ] IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour
$ grep -c 'no applier record' /tmp/p4a-c.log
3
[ERROR]: MGPipe: texture 1 has no applier record on the handle arm, so its parameters cannot be pushed (handle {1, 0})
[ERROR]: MGPipe: texture 1 has no applier record on the handle arm, so its built-in sampler cannot be pushed (handle {1, 0})
[ERROR]: MGPipe: texture 1 has no applier record on the handle arm, so its storage and uploads cannot be driven from the pushed descriptor (handle {1, 0})
```

Three refusals, one per switched-over texture path, each naming the handle it could not resolve —
`{1, 0}`, a handle the backend's own minting overload produced and for which the applier holds no
record because nothing has emitted one. **No case fails for any other reason**, and no handle arm
reads `MGB_CTX` or the frontend object to make a lane pass: every decline is a named
`MGLOG_E_ONCE` and a `return`.

The shape of the 183, stated so it can be re-stated unchanged after the rebase (P3a's espryt v2
discipline): every scenario whose draw needs a texture uploaded, a texture parameter pushed, a
built-in sampler pushed, a renderbuffer allocated, a framebuffer synced or a program's serial
stamped. The three lanes that carry their own phase pin (`ForcedDepthStencilEmulation`,
`WidenedPacked16`, `NoViewportArrayEmulation`) are in it for the same reason; the pure buffer,
clear, scissor, viewport and XFB scenarios are not, which is why 302 still pass.

### 6.5 What this tree cannot show, and why

Per ID-2 this package lands **after** `wire`, `clientfb` and `clientsp`. On this tree the applier's
fifteen P4a entry points are still `c0`'s stubs and **no client emitter has a body**, so
`MGPipeApplier().TextureResources` / `RenderbufferResources` / `SamplerCsos` / `SamplerViewCsos` /
`ShaderCsos` and both framebuffer records are permanently empty. Every switched-over path therefore
takes its named refusal (`MGLOG_E_ONCE`, one per family per texture/framebuffer/sampler/program) and
declines, which is by construction: a fall-back to the frontend would hide a missing record behind a
picture that still looks right, and that is precisely what the subsystem A/B exists to expose
(`MarkBufferGpuWritten`'s note, P3a). **No handle arm re-reads `MGB_CTX` or the frontend object to
make a lane pass.**

The expected failing set on the default arm is therefore "every case that draws through a texture,
a framebuffer, a sampler or a program with a P4a bit set" — measured at **183 of 491** in §6.4,
with its log evidence — and it is stated there **by shape** so it can be re-stated unchanged after
the rebase, exactly as P3a's espryt v2 did.

**The one thing to check first after the rebase is that this number goes to zero rather than
down**: a residual "no applier record" line for one kind after the client packages land is a seam
defect (a handle minted on one side and looked up on the other), not a leftover.

---

## 7. Post-rebase verification list (to run on `~/w7/pipe` after wire + clientfb + clientsp)

This is the list to run when the integrator says the client packages have landed — the first time
real records exist. Every line is C.3's, plus the two this round could not run.

1. `cmake --build build-linux && python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` → 0/0/0/0.
2. `grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'` → only `PipeInputs.h:1`.
3. `awk '/struct MGPipeResourceOps \{/,/^    \};/' MobileGL/MG_Pipe/PipeApply.h | grep -c MG_State` → 0.
4. `bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD` → rc 0.
5. `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` → rc 0 **once package F has landed it**; before that, §4's equivalent.
6. `cmake --build build-push && ctest --test-dir build-push -L unit --no-tests=error --output-on-failure`.
7. `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES` — **the real-path round**. This is the one that matters: P3a's equivalent found three seam defects no package tree could see. The three seams to look at first, in order of risk:
   - **the pending-upload set** (D-D5): does the client clear its own dirty flags only for records the applier accepted, and does `ConsumePipeTextureUpload` fire exactly where a level uploaded? A level cleared client-side but never consumed server-side is a texture that stops updating; the retain-mode comparison in the verify lane is the gate.
   - **the `DepthStencilMode` and upload-target encodings** (DV-2, DV-3): if package B emitted a different encoding than §5 states, `GL_DEPTH_STENCIL_TEXTURE_MODE` is pushed backwards and cube faces resolve to the wrong pending entry. `TextureParamsWithoutASamplerViewScenario`'s depth-stencil case is the behavioural gate.
   - **`PushedFramebufferRecord`'s `Fbo` match**: if the client emits `Target = Both` where Espryt expects a per-target record, or emits for a framebuffer other than the one being synced, the twin declines and the framebuffer is not synced at all.
8. `MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES` → green **only after the §6.3 pin move**.
9. `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES` → same.
10. `MOBILEGL_PIPE_PUSH=0x9ff ctest --test-dir build-push -R 'ObjectSubsystemControl'` — the sampler-bit-without-the-texture-bit refusal, once package F has written that scenario. This package's half (the `MGLOG_E` and the legacy fall-back) is already in.
11. `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-esprytobj -j 4` — not run this round: with empty records the corpus can only reproduce the refusal set, so the number would mean nothing.
12. `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4`, and the **retain-mode** texture-dirty-rect comparison of G4, which is the only way a consume-and-clear set can be checked at all.
13. `ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView'` — G9's mandatory scenario. Its red-before case
    `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` is closed on the **E** side
    (`SyncAttachmentObject` gains the parameter sync for READ-only attachments); this package
    supplies the half that makes it possible, namely that `set_texture_params` is addressed by
    resource and `SyncTextureParamsToBackend` no longer needs a sampler view to have a record.
14. `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported for 6, 7 and 12 (ID-18/ID-21).

---

## 8. Open items handed on

| item | owner |
|---|---|
| DV-5's `MGPSurface` texture-target field (or the inverse mapping), which unblocks the four cross-object masks and E's `SyncAttachmentObject` | contract (A) + integrator |
| DV-2 / DV-3 / DV-4: the `DepthStencilMode` encoding, the upload-target encoding, the `MGPSurface::Kind` enum | contract (A) + integrator; package B must emit DV-2/DV-3 as stated |
| the itest phase-mask pins `0x1ff → 0x1fff` and `0x80000000000001ff → 0x8000000000001fff` | package F (contract D10); **blocks §6.3** |
| `scripts/p4a_untouched_regions.sh` | package F |
| the four remaining `MGPipeUnmigratedEmulation` sites (`copy-image-shadow-mirror`, `generate-mipmap-storage`, `generate-mipmap-cpu-fallback`, `get-tex-image-shadow`) | package E |
| the four `g_fboSynced*` arrays' READERS, in `SyncCurrentFBO` | package E |
| the nine-clause rebuild condition reading `GetSyncedShaderCsoSerial()` | package E |
| the sampler-view twin's first consumer (the resolved-set walk and the raw-depth-fetch substitution keyed on it) | package E |
