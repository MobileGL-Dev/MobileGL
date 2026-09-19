# P4a package D — `p4a/esprytobj`, rework v2

Branch `refs/heads/p4a/esprytobj`, worktree `/home/swung/w7/p4a-esprytobj`, **HEAD `ef6beb28`**,
**not pushed**. Files touched by the rework: `MG_Backend/DirectGLES/{Managers.h, Managers.cpp}` and
`MG_Test/SanityTest.cpp` — the same three v1 touched, nothing else. Every line number below is a
line number **at HEAD**.

---

## 0. The base: what was rebased, what was merged, and the one commit the integrator must drop

```
git rebase refs/heads/feat/disaggregated      # 9ea44389 = c0 + c0b + c0c + c0d, clean, six commits replayed
git merge --no-edit refs/heads/p4a/wire       # 70e742a3  -> merge af5dca4c
git merge --no-edit refs/heads/p4a/c0e        # 11446f35  -> merge 815dd6d3
```

**Merged heads: `refs/heads/p4a/wire` = `70e742a3` and `refs/heads/p4a/c0e` = `11446f35`.**
`p4a/clientfb` and `p4a/clientsp` were NOT merged (their v2 rounds are in flight; D's verification
round on the integrated tree follows theirs). The rebase replayed all six v1 commits with no
conflicts: `7aa598fa c1d6b41c 49ff8d91 7e400454 a69819e4 9bb95335` →
`b1983e2c b8d0ef1b 42f1379f 308c80cb 107658f7 8338c18f`.

### `cce81d19` — a TEMPORARY BASE COMMIT, and the integrator drops it with the two merges

`[Fix] (Pipe): TEMPORARY BASE COMMIT the integrator drops with the two merges - retire wire v3's
stand-in Named framebuffer target constant, which c0e's enumerator makes a firing static_assert`

wire v3 left `kMGPipeFramebufferTargetNamed = 3` in `PipeApply.h` behind
`static_assert(MGPipeFramebufferTarget::Count == 3)` as the trip wire that makes retiring it
impossible to forget (wire-v3.md §7.2). c0e makes `Count` 4, so **the merged base does not
compile** until that constant is retired. The commit performs exactly the retirement wire-v3.md
§7.2 specifies — delete the constant and its assert, spell
`static_cast<Uint8>(MGPipeFramebufferTarget::Named)` at its six readers (`PipeApply.h:595`, `:965`,
`PipeApply.cpp` target gate and comment, `FramebufferEmitTest.cpp` ×3) — and nothing else. It is
`PipeApply.{h,cpp}` and a wire test file, all package A's, so it is **not** part of D's deliverable:
ID-24 assigns that edit to the integrator in the wire tree after wire's rebase. If the integrator's
own version is textually identical the patch-id matches and the rebase skips it; otherwise drop it.

---

## 1. The seven rework commits

| # | hash | items closed |
|---|---|---|
| r1 | `ac6757e6` | **CRITICAL / ID-19(d)**, E's item (4) |
| r2 | `a8c8824e` | **ID-12 DV-3** (the packed decode), DV-2, DV-4, **ID-15** (D-K2's fourth row), m-4 |
| r3 | `b6169dba` | **M-2** (storage from `Desc` + the metadata respecify), **M-1** (the server's own re-dirty) |
| r4 | `244224bc` | **M-3** (`SyncAttachmentObject` + the four masks from `MGPSurface`), **M-4** |
| r5 | `972384c2` | m-1, m-2, m-3, m-7 |
| r6 | `15787e7e` | **the G9 white-box probe (R1)**, m-8, m-9 |
| r7 | `ef6beb28` | three `#undef`s for macros never defined, and one stale ownership comment (both found by my own audit) |

Full messages:

```
ac6757e6 [Fix] (Espryt): resolve the framebuffer record by the handle it names and refuse before
         binding, so a DSA blit or clear configures the framebuffer it is about to write instead
         of one that never got its attachments
a8c8824e [Fix] (Espryt): decode the sub-data target's upload half through the contract's accessor,
         read the depth-stencil aspect and surface-kind constants c0c minted, and refuse bit 10
         without bit 11
b6169dba [Fix] (Espryt): allocate texture storage from the pushed descriptor, honour a
         metadata-only respecify's sticky mask, and arm the server's own re-dirty in the applier's
         pending set instead of a model the handle arm never reads
244224bc [Refactor] (Espryt): resolve framebuffer attachments and the four cross-object masks from
         the record's surfaces, adopting each twin by the handle the record carries, and drop the
         registry release helper nothing calls
972384c2 [Fix] (Espryt): consume the carried region offset, refuse a record whose regions disagree
         about the level pitch, stop reading the frontend border form on the handle arm, and drop a
         nested guard that repeats its parent
15787e7e [Test] (Espryt): assert an attachment-only texture's parameters reach the driver with no
         sampler view, prove every P4a kind's slot returns at a moved generation, and stop one
         refusal message claiming what it cannot tell
ef6beb28 [Fix] (Espryt): drop three #undefs for macros that were never defined and correct the
         attachment-version note that still called the record-driven walk another package's work
```

---

## 2. Item by item

### (1) CRITICAL / ID-19(d) — the record is keyed by the framebuffer handle, and the refusal happens BEFORE the bind

`Managers.cpp:8803` `PushedFramebufferRecord(MGPipeHandle fbo)` is now one call —
`MG_Pipe::MGPipeApplier().FramebufferRecordFor(fbo)`. The `asTarget` parameter and the
`record.Fbo == fbo` test are **gone**: the table is keyed by the handle, so a record that comes back
is that framebuffer's by construction and it comes back whether the object is bound to Draw, to
Read, to both or to neither. Declared at `Managers.h:1879`.

**The bound-target question is now a different function**: `PushedFramebufferIsBoundTo(target, fbo)`
(`Managers.cpp:8812`, declared `Managers.h:1886`) = one compare against
`MGPipeApplier().BoundFramebuffer[Draw|Read]`, exactly ID-19(d). It is the only correct spelling now
that a `Named` record describes an object without claiming any binding for it.

**`SyncToBackend` no longer binds and returns.** The record resolution block moved ABOVE
`Bind(asTarget)` (`Managers.cpp:9054-9091`, the `Bind` at `:9092`): on a missing record the twin logs
one `MGLOG_E_ONCE` naming the handle and returns with **nothing bound**, so the driver's binding is
where the caller left it rather than pointing at a freshly minted, attachment-less driver FBO. The
record's own `Target` is no longer compared against `asTarget` (a `Named` record legitimately names
no binding); what stays gated on `asTarget` is what reaches the DRIVER's bound target — `glDrawBuffers`
and the four cross-object masks stay Draw-only, `glReadBuffer` stays Read-only, and the OIT
argument the old comparison protected is unchanged because the object is bound as `asTarget` at that
point.

**Every DSA entry point is covered without touching E's file.** `BlitNamedFramebuffer`
(`DirectGLES.cpp:6474`, `:6475`) and the four `ClearNamedFramebuffer*` (`:7960`, `:7980`, `:7997`,
`:8017`) all reach the twin through `SyncAndBindFramebufferObject` →
`BackendFramebufferObject::SyncToBackend(framebuffer, target)`, which now resolves **that**
framebuffer's record by its own handle. No `DirectGLES.cpp` edit was needed and none was made.

`SyncReadBufferToBackend` (`Managers.cpp:8838`) asks the same question the same way and its refusal
line dropped the word "read-target": the record is the OBJECT's, and `ReadSurface` is resolved from
that framebuffer's own read buffer under every `Target`, `Named` included (c0e's comment).

The per-target memo `m_syncedRecordHashes` **stays per target** and `Managers.h:1836-1849` now says
why: there is one record for the framebuffer, but syncing it as Draw and syncing it as Read do
different work, so "I have already applied this record" is a per-target claim.

### (2) M-1 — the server's own re-dirty arms the applier's pending set

**Wire exposes no helper**, so the re-arm is written twin-side against the applier's own record —
`MGPipeApplier()` returns a non-const `MGPipeApplierState&` and `PendingUploads` is server-side state
by design (D-D5), so this is the applier's set, not a second twin-side one.
`RearmPipeTextureLevelUpload(res, packedTarget, level, wholeLevel)` at `Managers.cpp:3834`
(declared `Managers.h:717`), called from `RequireImageBindableStorage` (`Managers.cpp:5203`) at `Managers.cpp:5277`.

- The entry is **whole-level with no regions** (`RegionCount 0` = "the union box is the whole story",
  D-D3), because a replay owes every texel of the level.
- It **merges** with an entry the client already emitted rather than duplicating it (a whole-level box
  subsumes any scatter behind it), and it decodes the packed target with `MGPipeSubDataUploadTargetOf`
  so its key is indistinguishable from a client-emitted one.
- It writes a **packed** `MGPSubData::Target` (`MGPipePackSubDataTarget(MGPipeResourceTargetForTextureTarget(
  texture->GetTarget()), uploadTarget)`), for the same reason.
- At the applier's `kMGPipeMaxPendingUploads` bound it returns false with a named `MGLOG_E_ONCE`: a
  dropped replay is the bug this exists to stop, so it must not be silent.
- Both models are written (the frontend's `MarkStorageDirty` stays): the legacy arm reads it and the
  client's own `NoteLevelDirty` hook rides on it.

**The alternative the review offered (OR the frontend flag into the per-level question) was not
taken** and the reason is at the definition: it puts two authorities back on the handle arm and
forces D-D5's "the server never clears the client's flag" to grow an exception for the levels the OR
consumed.

**The three server-side `MarkStorageDirty` sites are enumerated in the comment** at
`Managers.cpp:3836-3856`: `Managers.cpp:5203` (`RequireImageBindableStorage`, live, this helper's
caller); `DirectGLES.cpp:7406` (`GenerateThreeChannelFloatMipmapOnCpu` — package E's file, and E's
verification round calls this helper there or states why not); `DirectGLES.cpp:6735` — which is the
**clear** half (`MarkStorageDirty(..., false)`), so nothing is owed and there is nothing to re-arm.

### (3) M-2 — the storage shape comes from `pushedStorage->Desc`

Seven push-only macros at `Managers.cpp:6244-6263`, `#undef`'d with the pair above at `:7543-7552`:
`MGB_STORAGE_FORMAT`, `MGB_STORAGE_BASE_SIZE`, `MGB_STORAGE_LEVELS`, `MGB_STORAGE_SAMPLES`,
`MGB_STORAGE_FIXED_SAMPLE_LOCATIONS`, `MGB_STORAGE_IMMUTABLE`, `MGB_STORAGE_KIND`. Each takes the
object it would otherwise have been spelled on, so the PULL expansion is the pre-P4a expression with
one pair of parentheses round the receiver — DV-9's discipline, and G1 stayed 0/0/0/0 through the
commit. Applied at:

- `currentTextureInfo` and the storage-kind switch (`:6468-6477`) — the allocation memo
  `needsRegeneration` and `canAppendMipmaps` read;
- the mipmap branch's `mipmapCount` (`:6481`, `:6858`) ← `Desc.Levels`;
- `IsImmutable()` at both allocator branches (`:6502`, `:6668`);
- the multisample allocation's sample count and fixed sample locations (`:6617`, `:6635`, `:6642`);
- all 16 `GetFormat()` reads in the mipmap and buffer branches ← `Desc.InternalFormat`.

**Buffer-texture fields** (`:7324-7360`, `:7425-7443`): `Desc.BufferForTexBuffer` names the backing
store (refused loudly when null on the handle arm), the twin resolves it with P3a's
`EnsureBufferResourceForHandle(buffer, pushedTexBuffer)`, the `bufferExternalIndex` memo key becomes
the handle's slot rather than a GL name, and the window comes from `Desc.BufOffset` / `Desc.BufSize`
with `kMGPipeWholeBuffer` resolved live against the buffer's own size (which is what the sentinel is
documented to mean). Written as `#if MOBILEGL_PIPE_PUSH` arms rather than macros because the branch
is structurally divergent; the pull text is byte-identical.

**What the descriptor does NOT answer**, stated at `:6230-6237`: the PER-LEVEL extent and the level
bytes. `MGPResourceDesc` carries one base extent; the per-level sizes are derived and the texels ride
on sub-data. A level `Desc.Levels` claims and the shadow has not defined reads back `{0,0,0}` and the
per-level loops skip it, which is the arm they already take for a sparse chain.

**The metadata respecify (ID-18 M4)** is honoured at `Managers.cpp:6316-6339`: when
`Desc.ImageBindableHint != 0 || (Desc.BindMask & kMGPipeBindShaderImage)` and the twin has not yet
required image-bindable storage, `RequireImageBindableStorage` is called. That IS the "re-derive the
BindMask-driven flags and recreate only where the backend needs the flag at creation" path — the
image carrier may be channel-widened, which is a different allocation and not a different binding —
and it is idempotent, clears `m_isInitialized`, re-arms every defined level in both dirty models
(M-1), and forces the parameter resync the widening's swizzle needs. Arriving on the CREATE instead,
which is the common case once B publishes the hint, costs nothing: `m_isInitialized` is already false
and there are no levels to replay. No reallocation is triggered by the mask alone.

### (4) M-3 — `SyncAttachmentObject` and the four cross-object masks read `MGPSurface`

**The four masks** are answered from the record at `Managers.cpp:8519-8601`:
`IsSnormFallbackSurface` (`:8550`), `IsUnormFallbackSurface`, `IsAlphaWidenedColorSurface`,
`IsIntegerColorSurface`, over `PushedSurfaceTextureTarget` (`:8539`) which gates on
`Kind == kMGPipeSurfaceKindTexture` and refuses `kMGPipeSurfaceNoTextureTarget` loudly rather than
guessing. `ShouldUseCaveatTextureFormat` / `BackendTextureFormatAddsAlpha` and their renderbuffer
siblings are **untouched** (two are on D-N's byte-identical list, verified below); only what they are
ASKED changed. The one call site — `SyncToBackend`'s draw-buffer mask block — takes a push-only early
branch at `:9163-9192` that resolves `record->Color[index]` from the decoded draw-buffer index and
`continue`s, so the frontend `GetAttachment` below is not even bound on the handle arm.

**The attachment walk**: `PushedSurfaceForAttachment(record, point)` (`:8605`) maps Color0..Color7 →
`Color[i]`, Depth → `Depth`, Stencil → `Stencil`, and answers null for points the record cannot
describe. `SyncAttachmentSurface(glFBOTarget, surface, attachmentObject, glBackendAttachment)`
(`:8631`) is the handle arm of `SyncAttachmentObject`; the call site is `:9290-9322`.

What moves: the TWIN RESOLUTION (`AdoptTwinByHandle(g_backendTextureObjects, surface.Res, "texture")`
and the renderbuffer equivalent — an adoption by the handle the record carried, not a lookup by the
frontend object's address) and the ATTACH SHAPE (`Layered`, `Level`, `Layer`, `UploadTarget`,
`TextureTarget`). What does not move, and is argued at the definition: the twins'
`SyncMipmapsToBackend` / `SyncToBackend` still take the frontend object — they read the level shadow
for the texels, which no record carries, and they have their own handle arms that drive the storage
from the descriptor. The frontend object is also **cross-checked**: a record whose `Res` does not name
the same twin the frontend attachment does is a seam defect and is refused loudly rather than resolved
in favour of either side. A point the record does not describe with a live attachment on it is loud;
an empty one is silently fine (D-C3 refuses bit 9 on a driver reporting more than 8 colour points, so
Color8..31 cannot legally hold one).

**E's READ-only attachment parameter sync stays reachable.** E closed D-E3 through a new
`SyncReadFramebufferTextureAttachments` in `DirectGLES.cpp` that walks the applier's read framebuffer
and calls `SyncTextureParamsToBackend` / `SyncBuiltinSamplerToBackend` per attachment. Nothing here
touches that function, the two twin entry points it calls, or their signatures; the attachment walk
changed here is `BackendFramebufferObject::SyncToBackend`'s, which E's list does not go through.

### (5) M-4 — the adoption helper is wired, one registry method is removed

- `AdoptTwinByHandle` now has **two callers** (`Managers.cpp:8657`, `:8723` — the texture and the
  renderbuffer registry, from `SyncAttachmentSurface`), which is where a handle actually arrives in a
  payload. Its comment (`:3529-3552`) says so and names the other three kinds' resolution sites as
  package E's, so their first adoption is E's to write through the same template.
- `StateBackendObjectRegistry::GetOrCreateByHandle` and `LiveGenAt` are reached through it.
- **`StateBackendObjectRegistry::ReleaseByHandle` is REMOVED** (`Managers.h:470-481` now carries the
  decision instead of the method). Its own precondition — "a kind whose announcement is its own
  destroy CALL" — holds for the BUFFER family and for none of the five kinds this registry serves:
  every one of them dies through `DestroyByLifetimeId`, because P4a adds no server-side destroy arm
  for a texture, a renderbuffer, a framebuffer, a sampler CSO or a shader CSO. The one-line wrapper
  comes back in the commit that gives it a caller. `SlotTable::ReleaseByHandle` underneath is
  untouched and is what `SanityTest.cpp` drives directly.

### (6) ID-12 — the HIGH byte is decoded, and every other `Target` comparison is audited

`FindPipeTextureUpload` (`Managers.cpp:3799`) and `ConsumePipeTextureUpload` (`:3821`) both compare
`MGPipeSubDataUploadTargetOf(pending.UploadTarget)` against the passed half; the re-arm helper does
the same at `:3872`. The three call sites that pass the bare `static_cast<Uint16>(TextureUploadTarget)`
needed no edit. `Managers.h:703-707` states the convention where the two functions are declared.

**Audit**, `grep -n '\.Target\|->Target\|\.UploadTarget'` over `Managers.cpp` at HEAD — eight hits
and every one of them accounted for:

| line | what |
|---|---|
| `:3792`, `:12379` | comments |
| `:3799`, `:3821`, `:3872` | the three decoded comparisons (find, consume, re-arm) |
| `:3888` | the re-arm's WRITE of a packed `MGPSubData::Target` |
| `:8673` | `MGPSurface::UploadTarget` — a different field, a bare `TextureUploadTarget`, no packing |
| `:8902` | the same field, compared surface-to-surface in the read-buffer decode (m-4) |

`MGPResourceDesc::Target` is still **never read** — M-2 reads `Desc.StorageKind`, not `Desc.Target`,
so the storage-kind switch does not go through the resource-target enum at all.
`MGPFramebufferState::Target` is a different field again and, per item (1), is no longer compared.

**DV-2** (`Managers.cpp:7840`): `MGB_TEXPARAM_DS_MODE`'s bare `!= 0` became
`== MG_Pipe::kMGPipeDepthStencilModeStencil`. **DV-4** (`:8877`): the read-surface emptiness test keeps
`MGPipeHandleIsNull(Res)` as the authority (D-C1) and **cross-checks** `Kind` against it, refusing
loudly when the two disagree instead of silently picking a winner.

### (7) ID-15 — the texture family's resolver refuses bit 10 without bit 11

`ResolveTextureResourceSubsystemArm` (`Managers.cpp:3624-3667`) gained a second
`PipeSubsystemDependencyMissing` row after the bit-7 one: **bit 10 requires bit 11**, refusing the bit
and running the legacy arm, in the same voice as the bit-8-without-bit-7 precedent at
`ResolveVertexInputSubsystemArm` (`Managers.cpp:2409-2431`) — same "the handle arm resolves through a
slot table only the other bit populates" shape, same `MGLOG_E`, same fall-back. The reason is stated
at the site: `MGPTextureParams::BuiltinSampler` is a SamplerCso handle, only bit 11 mints sampler CSOs
(c0b's four unconditional mints exclude it), and the applier's verdict for a null one is
`Fatal{ProtocolCorruption}`.

**The mirror pairs, as the source now states them:**

| pair | verdict |
|---|---|
| bit 7 set, bit 10 clear | **fine** — P3a's shipped configuration (unchanged) |
| bit 10 set, bit 7 clear | refused (v1, unchanged) |
| bit 11 set, bit 10 clear | refused (v1, unchanged) |
| **bit 10 set, bit 11 clear** | **refused now** — D-K2's fourth row |
| bit 8 set, bit 7 clear | refused (P3a, unchanged) |
| bit 7 set, bit 8 clear | **fine** (unchanged) |
| bit 12 (programs) | depends on nothing (unchanged) |

`ResolveSamplerSubsystemArm`'s comment said "the mirror pair (bit 10 set, bit 11 clear) is FINE";
that sentence is corrected in place (`Managers.cpp:3692-3699`) — the dependency is now SYMMETRIC, bits
10 and 11 are one arm with two switches, and the only two masks that reach the texture handle arm are
"both set" and "neither set". The two latches are raw bit tests on the mask, never calls to each
other, so they cannot initialise each other.

### (8) E's item (4) — `InvalidateFramebufferHandleArmMemos` inside `InvalidateFramebufferBindingCache`

The call is written at `Managers.cpp:8270-8278`, inside `InvalidateFramebufferBindingCache`, beside
the pre-handle trio it clears. **It is gated**, and this is a declared deviation the integrator must
see:

`InvalidateFramebufferHandleArmMemos` is **`static`** in `DirectGLES.cpp` (E v2, `:2789`) and owns E's
storage (`g_fboSyncedSerials`, `g_fboRecordsTrusted`). An internal-linkage function cannot be called
from `Managers.cpp`, and E is not merged into this base, so there is no definition here to link
against at all. The handshake is two lines and neither side can do it silently:

- `Managers.h:1996-2015` declares the entry point behind
  `#define MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED 0`, with the whole argument at the declaration;
- **E's verification round drops the `static` keyword** (one token);
- **D's verification round flips that constant to 1** (one line, in D's own file).

The constant has no other reader and the declaration has no other definition, so a half-done
handshake does not build. **The three `SanityTest.cpp` callers E named are covered the moment it is
on**: `ScopedStateGuardMocks::ResetShadows` (`SanityTest.cpp:2530`) and `ScopedBackendTwinMocks`'
constructor and destructor (`:2778`, `:2799`) all go through
`FramebufferImpl::InvalidateFramebufferBindingCache()`, which is exactly why the call belongs inside
it rather than at nine call sites.

### (9) The G9 white-box probe (gates review R1)

`SanityTest.cpp:3184` —
`DirectGLESTextureSync.AnAttachmentOnlyTexturesParametersReachTheDriverWithNoSamplerView`.

It covers the half `TextureParamsWithoutASamplerViewScenario` cannot: that scenario's observation is a
SAMPLE, the sample creates the sampler view, and a backend that merely DEFERS the apply to the first
view is green there. This one takes its reading while the texture is still attachment-only.

- A complete 8×8 texture is specified and then released from every unit
  (`BindTexture(GL_TEXTURE_2D, 0)`), so nothing in the test ever binds it for sampling and nothing
  mints a sampler view for it.
- `SetSwizzleParam(Red, Green)` — not any texture's default, so a driver that never hears about it is
  distinguishable from one that does.
- On the handle arm the probe writes the applier records itself (`MGPipeApplyResourceCreate`, a
  `SamplerCso` through `MGPipeApplyCreateSamplerState` because a null `BuiltinSampler` is
  `Fatal{ProtocolCorruption}`, then `MGPipeApplySetTextureParams`) — which is what makes it a
  white-box test: no client emitter exists on this tree, and the subject is Espryt's behaviour GIVEN a
  record.
- It asserts, before the sync, that **no sampler-view twin exists for the texture** — the assertion
  the public scenario cannot make, because making it there would create the view.
- Then it drives `twin->SyncTextureParamsToBackend(texture)` and asserts a
  `glTexParameteri(GL_TEXTURE_SWIZZLE_R, GL_GREEN)` reached the mocked driver.

**Green here on all four arms** (push build at masks `0`, `0x1ff`, `0x1fff`, and the pull build).
**Red on the tag's behaviour** — negative control run and restored: mutating the swizzle block to
require `FindSamplerViewForHandle(...) != nullptr` (the deferral reinstated) makes the case fail with
its own message, and the tree was restored, rebuilt and re-run green
(`git status --porcelain` clean afterwards).

### (10) The ten minors

| # | disposition |
|---|---|
| m-1 | **Fixed, partly.** `MGPSubRegion::SrcOffset` is CONSUMED at all three per-rect sites through `MGB_RECT_SRC_PTR(idx, rect)` (`Managers.cpp:7091`, used at `:7123`, `:7218`, `:7262`), a macro rather than a second lambda for DV-9's measured reason. Every such site is inside `subRectEligible`, which requires `uploadData == mipData`, so the base the offset is relative to is the level shadow itself. **The union-box path still derives its pointer** and cannot do otherwise: `MGPBox` carries no offset, only `MGPSubRegion` does. **`MGPSubData::SourceIsVerbatimLevelShadow` is NOT readable by this package and is DECLARED**: the applier's `MGPipeResourceRecord::PendingUpload` stores `{UploadTarget, Level, UnionBox, Regions}` and does not carry it (`PipeApply.h:283-289`), so `subRectEligible` keeps the `uploadData == mipData` pointer comparison. Carrying it is a wire change; owner = package A at its verification round. |
| m-2 | **Fixed.** `Managers.cpp:6975-7001`: every region's `SrcRowStride`/`SrcSliceStride` is compared against the first; on disagreement the carried pair is DROPPED (the level's extent × bpp is used, which is what the legacy arm computes) and one named `MGLOG_E_ONCE` says so. The assert the review asked for, not a per-region stride loop: a pitch is the level's. |
| m-3 | **Fixed.** `Managers.cpp:8058-8065`: `borderColorForm` is `borderColorReadable ? MGB_TEXPARAM_BORDER_FORM : m_cacheBorderColorForm`, so there is no live frontend read left on the switched-over path, and the value a skipped block sees cannot make the condition true. |
| m-4 | **Fixed.** `Managers.cpp:8888-8912`: the read-buffer decode compares `Res`, `Kind`, `Layered`, `Level`, `Layer` and `UploadTarget`. `InternalFormat` and `TextureTarget` are properties of the RESOURCE rather than of the point and are deliberately left out. |
| m-5 | **DV-9 reworded** — see §3 below. |
| m-6 | **Run.** `cmake -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF` compile-only configure, numbers in §4. |
| m-7 | **Fixed.** `Managers.cpp:217`: the nested `#if MOBILEGL_PIPE_PUSH` around the `SamplerViewCso` case is gone; the dispatcher's own guard at `:182` already opens it. |
| m-8 | **Fixed.** `SanityTest.cpp:4194-4208`: the message no longer claims what the assertion cannot tell, and a comment says which of the two assertions separates a refusal from an adoption (and why the first is kept: a non-null there would be a third and worse outcome). |
| m-9 | **Fixed.** `SanityTest.cpp:4337-4352`: `KindCase` carries the pre-death handle and every one of the five object kinds now gets the ABA leg — the slot came back, and at a MOVED generation, which is what a double free would break by skipping one. |
| m-10 | **Restated.** §4's refusal census names all six shapes; the count is per shape, not per grep. |

---

## 3. Deviations, updated

**DV-9 — REWORDED (m-5).** The sentence "the pull build's preprocessed text — and therefore its
object code — is unchanged" is **withdrawn**. The review measured it and it is false as worded: at
v1 the pull build's preprocessed text differed in 38 non-semantic lines (23 `line = N` constants in
`ErrorLopper::Loop` lambda captures shifted by the insertions, 13 pure added-parenthesis differences
from the macro expansions, 2 whitespace re-flows) with zero semantic differences. **That count is the
reviewer's, at v1, and this round did not re-derive it** — it can only have grown, because the round
added nine more macros (seven storage, one rect pointer, and DV-2's re-spelling), each of which adds
parentheses to the pull expansion by construction.

What the deviation should have claimed, and what IS verified here, is the CONCLUSION: the pull build's
**symbol set and byte size do not move**, which is the property D-P actually requires and which
`symbol_report --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` measures directly —
**0 added / 0 removed / 0 resized / 0 renamed and +0 bytes of `.text` at every one of the eight
commits of this round** (§4). The macro-over-lambda choice stands for the same measured reason it was
made: a lambda inserted into `SyncMipmapsToBackend` renumbers every `$_N` in it.

**DV-1 — closed by M-4.** The adoption helper and `GetOrCreateByHandle` have callers;
`ReleaseByHandle` is removed rather than left dead.

**DV-5 / DV-7 — closed by M-3.** Both are D's work and both are done. The v1 report's §7 item 13
("closed on the E side") and DV-7's "package E's `SyncAttachmentObject` work" are **wrong** and are
corrected here: `SyncAttachmentObject` is `Managers.cpp`, which C.7 gives to D. E closed D-E3 by a
different route (`SyncReadFramebufferTextureAttachments` in `DirectGLES.cpp`), which is untouched.
The stale comment in v1's source that said the array "retires with the frontend attachment objects,
which is E's `SyncAttachmentObject` work" is rewritten at `Managers.cpp:9250-9256` — see §7.

**DV-2 / DV-3 / DV-4 — retired**: c0c's constants and accessors are read.

**DV-6, DV-8, DV-10, DV-11, DV-12 — unchanged** and accepted as v1 argued them. DV-11's asymmetry note
(the `if (pushedStorage == nullptr)` / `if (FramebufferSubsystemEnabled())` arms get no "loud
unreachable" body) still stands and m-6's compile now exercises the four that do have one.

**NEW DV-13 — the handle-arm framebuffer memo hook is gated.** See item (8). One line each side, both
named at the declaration.

**NEW DV-14 — `MGPSubData::SourceIsVerbatimLevelShadow` is unreachable from this package.** See m-1.
The applier does not carry it into `PendingUpload`; owner = package A.

**NEW DV-15 — the re-arm helper writes the applier's own `PendingUploads`.** `MGPipeApplier()` is
non-const and the set is server-side state by design (D-D5), so this is not a twin-side shadow. G4's
retain-mode comparator compares the EMITTED shape against the tracker's retained one; a server-side
re-arm is a legitimate difference there and the verify lane must be read with that in mind
(`RequireImageBindableStorage` is the only site that produces it).

---

## 4. Gates — every number, at HEAD `ef6beb28`

All in `/home/swung/w7/p4a-esprytobj` with `CCACHE_BASEDIR=/home/swung/w7`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported, `-j 8`.

```
build-linux (pull, PUSH=OFF)                                             rc 0
build-push  (PUSH=ON)                                                    rc 0

G1  symbol_report.py --before ~/w7/p4a-before-libMobileGL.so
        --after build-linux/libMobileGL.so --threshold 0
        --fail-on-symbol-set-change --fail-on-added-bytes 0              rc 0
    .text 10806323 -> 10806323 (+0, +0.000%)
    27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
    RE-RUN AT EVERY COMMIT OF THIS ROUND, base repair included, each rebuilt clean and measured:
      cce81d19 ac6757e6 a8c8824e b6169dba 244224bc 972384c2 15787e7e ef6beb28
      all eight: .text 10806323 -> 10806323 (+0), 27811 -> 27811 symbols,
                 0 added / 0 removed / 0 resized / 0 renamed, 27072 normalised names unchanged

ctest --test-dir build-linux -L unit    100% passed, 0 failed out of 1686   rc 0
ctest --test-dir build-push  -L unit    100% passed, 0 failed out of 1686   rc 0
    (1685 after r5, 1686 from r6's new case on)

bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD                      rc 0
    the 11 pool / deferred-release / ring / flush-drain functions are
    byte-identical between 37da3c3a and HEAD

G13 grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'
    MobileGL/MG_Backend/MGPipe/PipeInputs.h:1                  <- ONE file, unchanged
awk '/struct MGPipeResourceOps \{/,/^    \};/' MG_Pipe/PipeApply.h | grep -c MG_State
    0
grep -rn 'MGB_LEVEL_|MGB_TEXPARAM|MGB_RBO_|MGB_STORAGE_|MGB_RECT_' MobileGL --include=*.h
    (no hits - no macro leaks into a header)
```

### D-N: the four functions in this package's files

Run with **package F's `scripts/p4a_untouched_regions.sh`** (it exists in `~/w7/p4a-gates` and not on
this tree; invoked from this worktree so its `git show` reads this tree's refs — it was NOT copied in
or committed here, `scripts/` is F's):

```
bash /home/swung/w7/p4a-gates/scripts/p4a_untouched_regions.sh 37da3c3a HEAD        rc 0
    [p4a-untouched] the 17 pool / ring / unpack-staging / attachment-permutation /
    [p4a-untouched] depth-stencil-sampling / format-caveat regions are byte-identical

791b54a35a19b58c82a5b9143cf48aa9ee815c755ecc1d4fa8dae709eeaa20bf  StageBlocksIntoUnpackRing
1d6f15c983bbf7e26bcb6bdfdc0f9bd940367d928b35480717ef22a4f663ef12  UnpackRingAvailable
19e614afbed5c0af01517f5ad75926d42f60eae7e23385190989dd47d0132f4f  UnpackRingAllocate
3ae7e3472c4a730d24549ee8b51b87eacf284dec1266b498cd5f3e056f787188  RecomputeBackendColorSlots
822ca3b0ece4e662f9b462945f2697f6b14547d841d926ae94c2b35139f16e53  DepthStencilSamplingReadImpl
266710e0fdc19756f31e27dcee536149c99fc9fe583752042135b3a9cde26366  ShouldUseCaveatTextureFormat
```

**These four shas are NOT v1's** (`d75537bc… a0c9994d… f56d432f… 39a37249…`) and that is a change of
EXTRACTOR, not of content: v1 used a one-off equivalent because F's script did not exist, F's script
hashes the region differently, and both say the same thing — **rc 0, byte-identical between
`37da3c3a` and HEAD**. v1's report printed no sha for P3a's eleven, so there is nothing to compare
them against there; what IS a cross-check is that `p3a_untouched_regions.sh` and F's superset print
the SAME eleven shas on this tree, so the two extractors agree everywhere they overlap.

### Integration-gpu, three arms, `-R DirectGLES`

```
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
  default (0x1fff)          62% passed, 189 failed out of 491     rc 8
  MOBILEGL_PIPE_PUSH=0x1ff  99% passed,   6 failed out of 491     rc 8
  MOBILEGL_PIPE_PUSH=0      99% passed,   6 failed out of 491     rc 8
```

**The 0x1ff and 0 failing SETS are identical, and are exactly the six `.Handles` aborts** (diffed
with `comm`, zero differing lines):

```
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.DestroyedVertexArraysReturnTheirVertexElementsSlots
    (Subprocess aborted)
```

They are the ctest `ENVIRONMENT`-pinned `MGL_ITEST_HANDLES_ARM_KNOBS = "MOBILEGL_PIPE_LEGACY_MEMOS=0"
"MOBILEGL_PIPE_PUSH=0x1ff"` arm (`MG_IntegrationTest/CMakeLists.txt:949`), which replaces the job env
— so both outer lanes see them — and under that pair all four P4a families classify as `NoArm` and
`StopOnArmlessPipeSubsystem` aborts, which is what D-K3 rules must happen. **Package F's pin move
`0x1ff → 0x1fff` at `:949, 1065, 1075, 1149, 1183, 1188, 1219` and `0x80000000000001ff →
0x8000000000001fff` at `:1070, 1080` is what closes them.** Unchanged from v1, reproduced.

### The default arm: 189 = 183 + 6, classified BY SHAPE

- **6** = the same six aborts above, `comm`-verified to be present in all three arms' failing sets and
  the only overlap. **Zero of the other 183 abort** (`grep -c 'Subprocess aborted'` over the 183 = 0).
- **183** = the named-refusal set. `comm -13` (0x1ff failures not in the default set) is EMPTY, so the
  default arm is a strict superset and nothing that passes at 0x1ff was newly broken.

The census, taken with `MOBILEGL_LOG_FILE_PATH` over 17 standalone runs sampled across the failing
suites — **six distinct shapes and no seventh**, which is m-10's correction to v1's "three":

```
MGPipe: texture N has no applier record on the handle arm, so its parameters cannot be pushed (handle {N, N})
MGPipe: texture N has no applier record on the handle arm, so its built-in sampler cannot be pushed (handle {N, N})
MGPipe: texture N has no applier record on the handle arm, so its storage and uploads cannot be driven from the pushed descriptor (handle {N, N})
MGPipe: framebuffer N has no applier record on the handle arm, so it cannot be synced from the pushed record and is left unbound rather than bound-and-unconfigured (handle {N, N})
MGPipe: framebuffer N has no applier record on the handle arm, so its read buffer cannot be pushed (handle {N, N})
MGPipe: program N has no shader-CSO applier record on the handle arm, so its synced serial cannot be stamped (handle {N, N})
```

**Zero `FATAL]: MGPipe` lines** across the sample. The non-MGPipe `ERROR` lines in those runs are the
DOWNSTREAM consequences of the declines and nothing else: `ReadPixels: bound READ FBO is not
complete` and `BlitFramebuffer: the depth/stencil aspect was dropped` (a framebuffer that got no
attachments because its record does not exist), one `glCopyImageSubData failed` (a texture with no
storage for the same reason), and `NormalizePixelFormat: unhandled internalFormat:
GL_STENCIL_INDEX8`, which is **ambient** — the control below shows it in the PASSING run of the same
case.

**Control**, `IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour` standalone:

```
MOBILEGL_PIPE_PUSH=0x1ff  -> [ OK ], 0 'ERROR]: MGPipe' lines, 2 NormalizePixelFormat lines
default                   -> [ FAILED ], the six shapes above, 2 NormalizePixelFormat lines
```

**The framebuffer refusal's wording changed this round** (item (1): "…is left unbound rather than
bound-and-unconfigured"), so a grep written against v1's text will miss it. The stem
`no applier record on the handle arm` still matches all five of the first-family shapes.

### The DirectVulkan lane (ID-8's death/leak half)

```
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectVulkan
    100% passed, 0 failed out of 475        rc 0
```

Under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. Zero failures, so the exit-order and
death-notice rules hold on the backend that installs no death ops.

### m-6: the `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` compile-only configure

The four `#if !MOBILEGL_PIPE_LEGACY_MEMOS` "unreachable but loud" bodies this package added
(`Managers.cpp` texture params, built-in sampler, sampler twin, renderbuffer twin) were compiled in
NO build of the v1 round — `build-linux` and `build-push` both pass
`-DMOBILEGL_PIPE_LEGACY_MEMOS=1`. They are compiled now:

```
cmake -S . -B ~/w7/dobj-nolegacy -G Ninja -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_C_COMPILER=/usr/sbin/clang -DCMAKE_CXX_COMPILER=/usr/sbin/clang++
      -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF          configure rc 0
cmake --build ~/w7/dobj-nolegacy --parallel 8 --target MobileGL_s        build     rc 0
    0 lines matching 'error:'; Managers.cpp compiled in this configuration
```

**The compiler has to be spelled.** A bare configure picks GCC on this box and GCC fails in
`MG_Impl/GLXImpl/GLXImpl.cpp:178` (`-Wchanges-meaning` on a member named `Display`) — a pre-existing,
unrelated toolchain difference, not a P4a defect; both gate builds use `/usr/sbin/clang++`, which is
what the line above passes. The build directory was removed afterwards.

---

## 5. What the verification round must do, in order

This is D's checklist for the round that runs on the integrated tree AFTER `C v2` and `B v2` land
(ID-24's order: c0e → wire → C v2 → B v2 → **D v2** → E verification → **D verification** → F v3).

1. **`MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED` → 1** (`Managers.h:2011`), once E's verification
   round has dropped the `static` on `InvalidateFramebufferHandleArmMemos`. One line. Re-run
   `ctest -L unit` and the `-R HandleRecycle` integration set: the three `SanityTest.cpp` fixtures
   that clear the pre-handle trio now clear the handle-arm memos too.
2. **The residual-refusal check, PER SHAPE and not per count.** With `MOBILEGL_LOG_FILE_PATH` set, the
   integrated tree must show **zero** of all six shapes in §4. A residual of any one of them is a seam
   defect (P3a's lesson), not a leftover. The framebuffer two are the ones the CRITICAL fix would
   masquerade as if B did not emit `Named` records.
3. **The DSA / named-framebuffer set, explicitly**, because that is what the CRITICAL fix buys and it
   cannot be observed on this tree:
   `ctest --test-dir build-push -L integration-gpu -R 'ClearThenReadPixels|Orientation|FramebufferChurn|DepthStencilReadback'`
   plus `ctest --test-dir build-push -R 'FramebufferTest.NamedFramebuffer'`. Also assert
   `MGPipeApplier().StaleFramebufferRecordLookups == 0` — wire made that the framebuffer family's own
   seam counter and 0 is the expected value.
4. **B's `Named` records at all five DSA entry points plus the DSA setters** (ID-19(c), wire-v3 §5
   B.1/B.2). If B emits none, item (1)'s fix produces the framebuffer refusal instead of a wrong
   picture — which is the intended failure mode, and the integrator needs to read it as B's gap.
5. **The image-bindable remint set, for M-1**, one dispatch and one readback apiece:
   `ctest --test-dir build-push -L integration-gpu -R 'ImageLoadStoreSso|FormatlessImageBake|ImageSizeAfterRespec|ImageTargetKind|SampledSetStaleness'`,
   and confirm a CONTENT assertion exists beside the swizzle one in
   `TextureParamsWithoutASamplerViewScenario.AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint`
   — today's case checks the swizzle, which M-1 does not break.
6. **The metadata respecify end to end (M-2 / ID-18 M4)**: B publishes a mask-only respecify on an
   IMMUTABLE texture; the applier must classify it as metadata (no ack, no pending-upload clear) and
   the twin must widen the carrier and replay the levels. The observable is that the first
   `glBindImageTexture` after an upload reads the texels rather than zeroes.
7. **The storage-shape seam (M-2)**: B's `MGPipeTextureExtentOf` clamps `Width/Height/Depth` at 0 and
   derives `ArrayLayers` separately; this package reads `Width/Height/Depth` as the base extent and
   `Levels` as the level count. Ten minutes on `TextureEmit.h:183-241` against
   `Managers.cpp:6224-6263` closes it; a disagreement shows up as `needsRegeneration` firing every
   sync (a re-allocation per draw) rather than as a wrong picture.
8. **The attachment seam (M-3)**: the cross-checks at `Managers.cpp:8643`, `:8649`, `:8710` and `:8717` refuse
   when the record's `Res` and the frontend attachment's handle disagree. It must never fire; if it
   does, B's `MGPSurface::Res` fill and the twin's `HandleOf` disagree about which object is attached.
   Also confirm B fills `MGPSurface::TextureTarget` (c0c) — `kMGPipeSurfaceNoTextureTarget` on a
   texture point is loud here and turns all four masks off.
9. **G4's retain-mode comparison** (`build-verify`, `-L integration-verify`) — it has not run anywhere
   in this phase and it is the only mechanism that can check a consume-and-clear set. Read it with
   DV-15 in mind: `RequireImageBindableStorage`'s server-side re-arm is a legitimate difference
   between the emitted set and the applier's.
10. `MOBILEGL_PIPE_PUSH=0x5ff ctest --test-dir build-push -R 'ObjectSubsystemControl'` — F's
    dependency-refusal scenario for D-K2's fourth row (item (7)). D's half is in and correct; the
    expected log line names both bits and the legacy arm runs.
11. `MOBILEGL_PIPE_PUSH=0x9ff` — the bit-11-without-bit-10 arm, unchanged from v1.
12. The six itest phase pins `0x1ff → 0x1fff` and the two `0x80000000000001ff → 0x8000000000001fff`
    (package F). Until they move, the six `.Handles` aborts are red on the integrated tree.
13. `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib build-push/libMobileGL.so ...` after
    `git lfs checkout` — not run this round (with empty records the corpus can only reproduce the
    refusal set, so the number would mean nothing), and it is the first real reading once records
    exist.
14. `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on the unit, integration and verify runs, and the
    ID-8 leak/idempotence half on the DirectVulkan lane — 475/475 here, and F's
    `HandleRecycleScenario.…ReturnTheirSlots` cases (G8b) are where `SamplerViewCso`, the kind with no
    frontend object, has to be covered; nothing in D or E can test it there.

---

## 6. Open items handed on

| item | owner |
|---|---|
| `InvalidateFramebufferHandleArmMemos`: drop the `static` (one token) | package E, its verification round |
| `MGPSubData::SourceIsVerbatimLevelShadow` is not carried into `MGPipeResourceRecord::PendingUpload`, so D-D6's test is unreachable and `subRectEligible` keeps the pointer comparison (DV-14) | package A (wire), its verification round |
| `MGPipeApplyResourceRespecify`'s per-level pointer with a byte-identical descriptor keeps the level it redefines (wire's own W11) — worth ten lines on B's `AllocateStorage` path | package B / the integrator |
| the itest phase-mask pins `0x1ff → 0x1fff` | package F |
| the four remaining `MGPipeUnmigratedEmulation` sites in `DirectGLES.cpp` | package E |
| G4's retain-mode comparison has still not run anywhere in this phase | the integrator |

---

## 7. Corrections to `esprytobj-v1.md`

- **§7 item 13 is wrong**: G9's red-before case is not "closed on the E side (`SyncAttachmentObject`
  gains the parameter sync for READ-only attachments)". `SyncAttachmentObject` is `Managers.cpp`,
  which C.7 gives to D. E closed D-E3 through a NEW function in its own file
  (`SyncReadFramebufferTextureAttachments`), and G9 itself is now a white-box assertion (ID-19) which
  this round supplies at `SanityTest.cpp:3184`.
- **DV-7 is wrong for the same reason** and DV-5's "owner: the contract (package A) + the integrator.
  Until then this package leaves the four masks reading the frontend" is superseded: c0c minted the
  field and this round moved all four masks onto it.
- **DV-9's wording** is corrected in §3 above (m-5).
- **§6.4's "three refusals"** is corrected to six shapes in §4 above (m-10); the classification claim
  ("no case fails for any other reason") holds and is re-verified over a 17-case sample.
- The source comment that said the per-attachment version array "retires with the frontend attachment
  objects, which is E's `SyncAttachmentObject` work" was stale on both counts and is **rewritten**
  (`Managers.cpp:9250-9256`, commit `ef6beb28`): the walk resolves each attachment from the record
  now, so the versions say only "this point may have moved since I last looked", and they retire with
  the frontend attachment array itself, which is a later phase's and not package E's.
