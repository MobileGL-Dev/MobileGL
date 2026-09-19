# P4a package E — `esprytdraw`. Result, v1

Branch `refs/heads/p4a/esprytdraw`, five commits on top of the tag `refs/tags/p4a/contract`
(`08192d72`). **Not pushed.** Working tree clean. One file touched:
`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`, **+893 / −38**. `Utils.{h,cpp}` and
`MultiDraw.cpp` are **untouched** (§5, DEV-10).

A safety ref `refs/heads/p4a/esprytdraw-backup` points at HEAD; the integrator may delete it after
the merge.

| step | sha | message (exactly as C.4 specifies, single line, no attribution) |
|---|---|---|
| `e1` | `98bdfa3e37d737c5131280d55829671dff4d4970` | `[Refactor] (Espryt): resolve the current framebuffers from the pushed records and keep the shadowed default bind and the missing fast path exactly as they are` |
| `e2` | `c0dc679ea3099d4ff04736adf55d3339927de6ca` | `[Refactor] (Espryt): drive the per-draw texture sync from the pushed sampler-view set and give a read-only attachment its texture parameters` |
| `e3` | `f23fec0ea652bc19543cd93dfa982cd8abcb4398` | `[Refactor] (Espryt): bind shader images from the pushed set and leave the format recast and the split view where they are` |
| `e4` | `45810af0903a6c23c7f8975e8a82dd00e991b55d` | `[Refactor] (Espryt): bind samplers and the program's resources from the applier's resolved sets and keep the raw-depth-fetch substitution on the server` |
| `e5` | `0b00f7f0baae89dba37af66896bc7fd0062623e7` | `[Refactor] (Espryt): name every emulation that cannot survive a split and take the format decisions from the descriptor` |

---

## 0. The one finding the other Espryt packages need before they write a line

**`MGPipeApplierReset()` ADVANCES every P4a working-state serial unconditionally**
(`PipeApply.cpp:686-690`, `:716-720`: `++FramebufferSerial; ++SamplerViewsSerial; …;
++ProgramBindingSerial`). It is a make-current, not a teardown, and the design's own argument for
advancing rather than zeroing is correct — but the consequence is that **a P4a working-state serial
is NEVER the test for "has the client ever emitted this set"**. It is non-zero after the first
make-current on a tree where nothing emits at all.

This was found the expensive way. `e1`'s first landing gated the framebuffer handle arm on
`FramebufferSerial != 0`, took that arm with two all-zero records, skipped every framebuffer sync
and turned **55 of 491** `integration-gpu` cases red on the default (`0x1fff`) arm — a failure that
looks exactly like "the applier is a stub, which the brief says to expect", and would have been
classified as expected rather than as mine. It is not: the same tree at the tag is **491/491 green
on the default arm**, which is the control that separated the two (§7 records that run).

The tests that hold instead, and that every handle arm in this package uses:

* a **record's own handle** (`MGPFramebufferState::Fbo`, `MGPBoundView::View`, `MGPImageView::Res`)
  — null means "no record";
* a var-tail set's **`Count`** (`SamplerViewCount`, `SamplerStateCount`, `ShaderImageCount`) — 0
  means "this set has never arrived".

Package D's `esprytobj` will meet the same trap in `d4`'s `FramebufferSerial` compare and in `d2`'s
per-record gates. **Recommend the integrator hand this paragraph to D before its rework round.**

---

## 1. The controlling invariant of this package

**Every handle arm DECLINES to the pre-handle arm when the records that arm needs have not arrived,
and a decline is a full fallback rather than a partial run.** That is P3a's
`SyncVaoAttributeBuffersByHandle` discipline ("no live record is a miss, never a hit") applied to
five more families, and it is what makes this package landable ahead of `wire`, `clientfb`,
`clientsp` and `esprytobj`:

* **the default (`0x1fff`) arm is green on this tree, 491/491**, with no expected-failure set to
  argue about — because with an empty applier every arm declines and the tree runs exactly the code
  it ran at the tag;
* when B/C/`wire` land, each arm engages on its own record's arrival, independently of the others;
* a **seam check** sits in front of the two arms that could otherwise drive the driver from a record
  that describes something else (§4, e1 and e3), so a mis-keyed emission declines loudly instead of
  producing wrong pixels.

---

## 2. Per-step summary: every memo re-keyed, and every frontend read replaced (Track H census input)

### `e1` — the framebuffer sync and bind path (+347 / −18)

**Memos re-keyed**

| before | after (handle arm) |
|---|---|
| `g_fboSyncedSlotVersions[t]`, `g_fboSyncedObjectVersions[t]`, `g_fboSyncedObjects[t]` — three answers to "has the state the client would describe moved" (`Managers.h:1690-1710`) | **one** `Uint64`: `FramebufferImpl::g_fboSyncedSerials[t].serial` vs `MGPipeApplier().FramebufferSerial` |
| `g_fboSyncedBackendIdGenerations[t]` | **kept, per D-B3/D-O**: `g_attachmentBackendIdGeneration` answers "did *I* re-mint a driver texture id", which no client version can answer. Joined by `g_backendContextGeneration`, because driver FBO names die with the ES context |
| `g_broadcastMemo{Fbo, SlotVersion, ObjectVersion}` (`DirectGLES.cpp:3037-3057`) | `g_broadcastMemoContentHash` vs `DrawFramebuffer.ContentHash` — a **complete** key by D-C4's construction: the hash covers `Fbo` (so a recycled framebuffer handle cannot be suppressed against its predecessor) and `DrawBuffers[8]` (so an unchanged hash provably means the broadcast count did not move) |

The memo stays **per target**, because `ForceBindCurrentFBO` syncs one binding and must be able to
say so; what collapsed is the three-value question into one serial, not the two targets into one.

**Frontend reads replaced**

| read | site | replaced by |
|---|---|---|
| `GetFramebufferBindingSlotChecked(t).GetVersion()` ×2 | `SyncCurrentFBO`'s memo | the record's serial |
| `currentFBO->GetObjectVersion()` ×2 | same | same |
| `currentFBO.get()` as identity ×2 | same | `record.Fbo` through `FindByHandle` |
| `currentFBO == pDefaultFramebufferInfo->defaultFBO` ×2 | `SyncCurrentFBO`, `BindCurrentFBO` | `record.IsDefault` — one of the four identity comparisons `MGPipeHandles.h:69-71` names. Retained **only** as the consistency guard's comparand (§4) |
| `currentFBO.get() == lastUpdatedFBO` ("same FBO as draw") | `SyncCurrentFBO:2286` | `record.Target == Both`, and the read buffer is then applied from the record's **own** `ReadSurface`. The hazard `Managers.cpp:7424-7426` describes becomes unrepresentable rather than merely fixed |
| **the whole of `GetFramebufferBindingSlotChecked(t)`** | `BindCurrentFBO` | the record. This is the one of the five call sites that is **entirely retired** on the handle arm |
| `drawFBO->GetDrawBuffers()` walk | `SyncCurrentProgram`'s fragColor derivation | `record.DrawBuffers[8]` (`>= 0` is "not None") |

**Kept exactly as they are, and each says so in a comment**: `BindCurrentFBO` has **no fast path on
any version** (`KHR-GL32.packed_pixels`, D-O); the **default framebuffer is bound through the
shadow**, never raw (`readback-state-guards`, D-O); the widened/integer draw-buffer masks are reset
on the default-FBO path for the DRAW target only; `SyncAndBindFramebufferObject`'s default-FBO early
return is untouched.

**New invalidation**: `FramebufferImpl::InvalidateFramebufferHandleArmMemos()` is called beside all
**six** `InvalidateFramebufferBindingCache()` call sites in this file, and `InvalidateBroadcastMemo()`
clears the handle-arm broadcast keys too.

### `e2` — the per-draw texture sync (+265 / −4)

**Memos re-keyed**

| before | after (handle arm) |
|---|---|
| `CurrentUnitBindingsEpoch`'s derivation: `CaptureUnitBindings` / `UnitBindingsUnchanged` — a per-unit walk over every binding slot **and** the unit's sampler object, re-run whenever `GetTextureBindGeneration()` moves (26.2 moves it around every texture-unit switch) | `Mix(SamplerViewsSerial, SamplerStatesSerial)` — **two `Uint64` reads, no walk, no accessor call**. Sound because both sets go out through a `ContentHash` suppressor over every tail entry, so a serial moves iff the resolved set moved |
| `g_fboTextureSyncListSlotVersion` + `…ObjectVersion` | `g_fboTextureSyncListContentHash` vs `DrawFramebuffer.ContentHash`. **The identity half stays a pointer compare** and deliberately so — the list borrows the attachment's own `SharedPtr` slots, and "is the object I borrowed from the one bound now" has no handle in it until D re-keys the twin tables |

**Transitively re-keyed with no edit of their own** — every consumer of `keys.unitBindingsEpoch`,
which is now the records' serials on the handle arm: `g_unitTextureSyncList*` (the unit texture sync
list), `ResolvedTextureBindingMemo` (the per-program resolved texture bindings), `g_unitSamplerWalk*`
(the sampler walk), and `BackendProgramObjectImpl::SamplerPassMemo`. **Four memos re-keyed by one
substitution** — this is the single largest item in the census and it costs no edit at any of the
four sites.

**Behaviour deliverable — D-E3, `ROADMAP.md:20`'s 落地前必须红 item.** A new push-only list,
`g_readFboTextureSyncList`, walks the **READ** framebuffer's texture attachments and gives each one
`SyncTextureParamsToBackend` + `SyncBuiltinSamplerToBackend` + `SyncMipmapsToBackend`. Today a
read-only attachment reaches `SyncAttachmentObject`, which calls `SyncMipmapsToBackend` **and
nothing else** (`Managers.cpp:7161`), because the list that pushes parameters for an attachment reads
the DRAW slot alone (`DirectGLES.cpp:1944`). One object bound to both bindings is walked once (a
pointer compare against the draw framebuffer). The list carries **both** key shapes, exactly like
the draw list.

**Memos retired**: none. `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` /
`PairingsIntact` are all **kept**; their deletion is the ~175-line backend debounce removal that
`ROADMAP.md:23` gives to P3b/P4b, and `ARCHITECTURE.md:369` keeps the pre-handle arm compiling
through P4a.

### `e3` — the image units (+93 / −6)

* `SyncImageTextureBinding` takes **`Level`, `Layer`, `Layered` and `InternalFormat`** from
  `MGPImageView` when `ResolveShaderImageRecord` says the record names the same texture the unit
  holds. Substituted through four `#define`/`#undef` pairs in the `MGB_UNIT_BINDINGS_HANDLE_ARM`
  shape this file already uses, so the **pull build's preprocessed text is unchanged** (D-P).
* `SyncImageTextureBindings`' sweep membership: `min(192, MaxImageUnits)` → `[ShaderImageStart,
  ShaderImageStart + ShaderImageCount)`, clamped to what ES exposes. That is D-J2's rule — *the
  var-tail window IS the bound and entries outside it are not cleared*.
* **`Access` is deliberately not taken from the record** — see DEV-6.
* **D-G4's two invariants are kept and now say so in the source**: the
  `g_imageUnitHighWaterMark == 0` early-out is asked before anything reads a record, and the sweep's
  gate stays the **frontend** sampling-resolution generation and is explicitly *not* re-keyed onto a
  server-owned serial, because a texture bound only to an image unit is re-minted **inside** the
  sweep. The bind-format recast and the buffer split view are untouched.

### `e4` — the sampler walk and the program's resources (+159 / −13)

* **`BindCurrentUnitSamplers`' walk reads `MGPipeApplier().BoundSamplerStates[unit]`** instead of
  `MGB_CTX->GetTextureUnitObject(unit).GetSamplerObject()`, and binds through `FindByHandle` — so on
  that path `UnitSamplerLookupMemo` is **bypassed entirely**: the handle *is* the lookup, and
  `SlotTables.h:556-561`'s recorded single-entry-memo thrash does not happen. A null handle unbinds;
  a handle whose twin does not exist yet is left alone and **never cached as a miss**, for the reason
  `ResolveUnitSamplerBackend` gives (the program pass creates the twin later in the same draw).
* **`BindCurrentProgramWithResources`' global UBO takes its version and its block image from the
  `ShaderCso` record** — `GlobalConstantsVersion` and `GlobalConstants` replace
  `GetUBOContentVersion()` and `MapUBO()`. `ResolveGlobalConstantsRecord` requires four things
  before it will answer: a live record at the program's handle, the record's own `Desc.Cso` naming
  that same handle (the seam check), a version that is not the `~0u` never-uploaded sentinel, and a
  block image at least `GetUBOSize()` long.
* `FindShaderCsoRecord` is the **one** reader that knows the composite band exists
  (`MGPipeIsCompositeShaderSlot` → `CompositeShaderCsos[slot − base]`), so D-H7's "the server never
  learns it is a composite" stays true of every other reader.
* **The UBO ring is untouched** — the `{contentVersion, ringGeneration, frameSerial, offset}`
  allocation key, the `glBufferSubData` fallback and the `ByteClass::StageUboGlobal` accounting are
  all exactly what they were. Named UBOs (P4b) are untouched.
* **The raw-depth-fetch substitution stays on the server** and now carries the D-F3 paragraph naming
  it as *the largest surviving `MG_State`-type usage under `MG_Backend/DirectGLES`* with its owner
  (P3b/P4b) written down, so P13's include-graph gate meets a known item.

### `e5` — the named emulations (+32)

Four `MGPipeUnmigratedEmulation` call sites, each planted where the emulation **commits to running**
(after the cheap declines), so the name means "this ran" rather than "this was considered". §6 lists
them.

---

## 3. What this package assumes of package D, and where the rebase will touch it

Every assumption below is written into the source at the site that makes it.

| # | assumption | what changes at the rebase |
|---|---|---|
| **A1** | `StateBackendObjectRegistry` (`Managers.h:303-540`) forwards `HandleOf` and `FindByHandle` but **not** `GetOrCreate(MGPipeHandle)`. `e1` resolves the FBO twin with `FindByHandle(record.Fbo)` and falls back to `GetOrCreate(currentFBO)` | after `d1`, the fallback becomes `GetOrCreate(record.Fbo)` — one line, marked "MONOLITH GLUE" in the source |
| **A2** | `BackendFramebufferObject::SyncToBackend(const SharedPtr<FramebufferObject>&, FramebufferTarget)` and `SyncReadBufferToBackend(const SharedPtr<FramebufferObject>&)` keep their signatures through `d4`; only their bodies re-key onto `MGPFramebufferState` | if `d4` changes the signature, `SyncCurrentFBOByRecord`'s two call sites take the record instead of the object, and the "resolve the bound object" block above them disappears with the consistency guard (§4) |
| **A3** | `BackendTextureObject::{SyncTextureParamsToBackend, SyncBuiltinSamplerToBackend, SyncMipmapsToBackend, IsDrawSyncClean}` keep their frontend-object signatures through `d2` | the three sync lists' entries become `{handle, twin}` instead of `{slot, texture, twin}`, and DEV-4 (the unit list's membership) can then move to the record |
| **A4** | D-K3's four `Resolve<Family>SubsystemArm()` latches do **not** exist at `c0`; contract-v1 §6 gives them to D | the four local latches in this file (`EsprytDraw{TextureResource,Framebuffer,Sampler,Program}HandlesEnabled`) become calls to D's, which add the `MGLOG_E` naming both bits and the `Fatal{PipeLegacyMemosDisabled}` stop. **The dependency directions are already copied here** (11→10, 9→10, 10→7, 12→nothing) so a half-running mask is refused in the meantime |
| **A5** | `SamplerPassMemo::rows` stays `Array<BackendSamplerObject*, 16>` and `g_boundSamplersCache` stays a raw-pointer shadow (`Managers.h`, D's) | nothing here; see DEV-5 for why no handle belongs in either |
| **A6** | `MGPImageView::Access` is a `Uint8` with **no documented encoding** at `c0` | `e3` starts reading it once package C writes the encoding down; one `#define` in this file |
| **A7** | `MGPipeApplierReset` advances all P4a working-state serials (§0) | nothing here — every arm already uses handles and counts |

---

## 4. The two seam checks, and why they exist

Both were added in a self-review pass and squashed into their step (DEV-12). Both exist because this
package can be *landed* but not *exercised* against real records on its own tree, and the failure
mode they guard is silent wrong pixels rather than a red test.

**Framebuffer (`e1`).** `FramebufferRecordMatchesBinding(target, record)` requires the record's
`IsDefault` to agree with the bound object's defaultness and, for a non-default record, the
record's `Fbo` to equal `HandleOf(bound)`. A disagreement **declines the whole arm** — both targets,
not one — because the two records are emitted together and one being wrong says nothing good about
the other, and it logs once. `g_fboRecordsTrusted` carries that verdict to `BindCurrentFBO` (whose
whole value is that it does **not** read the binding slot) and to the fragColor broadcast derivation;
both run immediately after `SyncCurrentFBO` in `PrepareForDraw`, so the latch is fresh, and it starts
false and is cleared by every decline and by every binding-cache invalidation, so an unpaired bind
takes the pre-handle arm.

**Image units (`e3`).** `ResolveShaderImageRecord` returns the record only when `view.Res` equals
`HandleOf(the unit's bound texture)`. This one is not optional: `SyncImageTextureBinding` is also the
**eager** funnel `glBindImageTexture` itself runs, and at that moment the newest record describes the
*previous* draw — driving the driver from it would bind one texture with another's level, layer and
format.

Both checks are monolith glue and both disappear when D's twin API takes the record instead of the
object: at that point there is no second answer left to disagree with.

---

## 5. Deviations, each with its reason

**DEV-1 — the READ-attachment parameter gap is closed from `SyncNeccessaryTextures`, not from
`SyncAttachmentObject`.** C.4's `e2` says *"`SyncAttachmentObject` gains the parameter sync for
READ-only attachments"*, but `SyncAttachmentObject` is in `Managers.cpp`, which C.7 gives to **D**
for the whole phase. The gap's actual cause is in this package's file — `SyncNeccessaryTextures`'
attachment list reads `GetFramebufferBindingSlotChecked(Draw)` alone
(`scout-espryt-framebuffer.md` §2.6) — so closing it here is both within ownership and closer to the
cause. Same observable behaviour: an attachment of either framebuffer now gets
`SyncTextureParamsToBackend` + `SyncBuiltinSamplerToBackend`.

**DEV-2 — the READ-attachment sync is NOT gated on the framebuffer subsystem bit.** It is a
*deliverable* of the phase (`ROADMAP.md:20`, `ARCHITECTURE.md:100`), not an *arm* of it. Gating it
would make the object A/B measure the arm **plus** a behaviour change and would leave G9's mandatory
case red on every legacy lane. It is inside `#if MOBILEGL_PIPE_PUSH`, so G1 and the pull build are
untouched, and G9 runs on `build-push`.

**DEV-3 — `keys.maxTouchedUnit` stays `MGB_CTX->GetMaxTouchedTextureUnit()`.** D-G2 makes
`SamplerViewCount` the client's spelling of exactly this number, and an earlier revision of `e2` did
take it off the record. It was **reverted in self-review** because the two failure directions are not
alike: a window larger than the frontend mark costs a walk over provably-empty units, while one that
is *smaller* silently drops the sync of a texture a draw is about to sample — and no gate on this
tree can see the difference, because this tree emits no windows. The design assigns the high-water
mark to the **client** as the `count` argument and says nothing about re-deriving it server-side.
One accessor read per draw; the integrator can move it in one line on a tree where the two can be
compared (§8).

**DEV-4 — the unit texture sync list's MEMBERSHIP stays the frontend binding walk; only its
VALIDITY is record-keyed.** C.4's `e2` asks for the list to be "rebuilt from the applier's
SamplerViews set". It cannot be, yet, and the reason is structural rather than a matter of effort:
an entry is `{const SharedPtr<ITextureObject>* slot, ITextureObject* texture, BackendTextureObject*
backend}` and the three twin calls it replays all take the **frontend object**. `MGPBoundView`
carries a texture *handle* and a unit index but not the binding target, and a handle cannot be turned
back into a frontend object (`Entry::stateRef` is empty by design on handle-keyed entries,
`SlotTables.h:266-269`). The membership move is unlocked by D's `d2`, at which point the entries
become `{handle, twin}`; A3 records it.

**DEV-5 — `SamplerPassMemo`'s rows stay `BackendSamplerObject*`.** C.4's `e4` says they become
handles. Two reasons not to, and the second is the real one: the struct is in `Managers.h`, which is
D's file; and the rows are not an *identity* question at all — they are the memo's proof that **the
driver still holds the bindings this pass left**, compared against `g_boundSamplersCache`, which is a
server-owned driver shadow. A client handle in that array would answer a question nobody asked. The
identity half of this memo family *is* already handle-keyed, by P2:
`UnitSamplerLookupMemo::frontendHandle` (`DirectGLES.cpp:3522-3535`). What `e4` adds is that the
record path does not consult that memo at all.

**DEV-6 — `MGPImageView::Access` is not consumed.** The field is a `Uint8` and a GL access token
(`GL_READ_ONLY` = 0x88B8) is not, so the record carries an **encoding** — and neither
`MGPipeTypes.h:659-668` nor `PipeFields.def:123-124` writes one down at `c0`. Reading it here would
be inventing it. The frontend value answers, and it is the same value by construction.
**Recommend the integrator have package C document the encoding in `ImageEmit.h`**, after which this
is one `#define` in `SyncImageTextureBinding`.

**DEV-7 — four local family arm latches instead of D's `Resolve<Family>SubsystemArm()`.** A4. They
carry the dependency directions verbatim; they do **not** carry the refusal `MGLOG_E` or the
`Fatal{PipeLegacyMemosDisabled}` stop, because two writers of one diagnostic is how the two drift.

**DEV-8 — `SyncTextureObjectToBackend` does not gain an explicit handle parameter.** C.4's `e2` asks
for it. It is already handle-keyed *internally* on the handle arm — `Find(obj)` on a
`BackendSlotTable` is `FindByHandle(HandleOf(obj))` (`SlotTables.h:364-368`) — so an explicit
parameter would only skip a memoised lifetime-id probe, and no caller has a handle to pass until
DEV-4 is unlocked. Adding the overload now would be ~60 duplicated lines with no caller.

**DEV-9 — two consistency guards on the handle arms.** §4. They add frontend reads that a fully
migrated path would not need, which is the opposite direction from the phase's goal; they are marked
in the source as monolith glue with the condition for their removal.

**DEV-10 — `Utils.{h,cpp}` and `MultiDraw.cpp` are untouched.** C.4's `e5` asks for "the
`Utils.{h,cpp}` readers that take a descriptor instead of a frontend object". A grep for `MG_State`,
`ITextureObject`, `FramebufferObject`, `SamplerObject` and `ProgramObject` across all three files
finds **no such reader**: `Utils.h`'s format decisions
(`ShouldUseCaveatTextureFormat`, `GetImageBindableStorageWidening`, `GetImageBindableBufferSplitFormat`,
`GenerateTextureFormatInfo`) already take `TextureInternalFormat` + `TextureTarget` **values**, and
`MultiDraw.cpp`'s only frontend reads are `BufferObject` (P3a's family, and the index host mirror is
P8's). The one Utils function D-N names, `ShouldUseCaveatTextureFormat`, is on the byte-identical
list and stays byte-identical. `e5`'s message therefore lands with only its first clause realised;
the second clause has no work behind it in these files.

**DEV-11 — D-N was checked by hand.** `scripts/p4a_untouched_regions.sh` is package F's and does not
exist yet. §6 gives the sha evidence for the two D-N functions that live in package E's files, taken
the same way the script takes them (an `awk` range for the namespace, a `git diff --stat` for the
file).

**DEV-13 — `~/w7/p4a-trees2.log` never said `REBUILT esprytdraw`, and this package built anyway.**
The task said to wait for that line before the first build. The file was created at 12:25 and was
**still zero bytes** when this round ended, with no `cmake`, `ninja`, `wsl_tree.sh` or
`wsl_integrate.sh` process running at any point. What was checked instead, before touching anything:
`HEAD == refs/tags/p4a/contract` (`08192d72`), a clean working tree, `build-linux` and `build-push`
present with libraries dated after the tree was created, and `ninja -n` in both reporting nothing to
do but a CMake re-run — i.e. the tree was already in the state the line would have announced. **If
the integrator's rebuild script is still pending, it must not be run against this worktree**: a
`wsl_tree.sh p4a esprytdraw p4a/contract` would reset `refs/heads/p4a/esprytdraw` and take the five
commits with it. `refs/heads/p4a/esprytdraw-backup` (`0b00f7f0`) exists for exactly that accident and
should be deleted after the merge.

**DEV-12 — the series was produced with one autosquashed fixup.** The self-review (§4, DEV-3) found
two issues after `e5` had landed; the fix was committed as `--fixup=<e2>` and folded in with
`git rebase --autosquash`, so `e2`..`e5` carry different shas than their first landing and the branch
is exactly the five commits C.4 asks for, with the five messages verbatim. Every commit was
re-verified afterwards (§7).

---

## 6. Emulations: dispositions and the `MGPipeUnmigratedEmulation` sites

Four of D-M's five names are planted in this package's file; the fifth (`texture-remint-pull`,
`Managers.cpp:4769`) is **package D's** — contract-v1 §6 splits the five between D and E, and
`PipeCatalogueTest.EveryUnmigratedEmulationIsNamedOnce` pins all five names.
`grep -rc 'MGPipeUnmigratedEmulation' MobileGL/MG_Backend/DirectGLES/` → `DirectGLES.cpp:4`.

| name | site (this file, at `0b00f7f0`) | planted where |
|---|---|---|
| `"generate-mipmap-storage"` | `EnsureGenerateMipmapStorageAllocated` | after the `existingLevelCount == 0` decline, before the level shadows are read |
| `"generate-mipmap-cpu-fallback"` | `GenerateThreeChannelFloatMipmapOnCpu` | after the format and mipmap-texture declines |
| `"copy-image-shadow-mirror"` | `MirrorCopyImageIntoDestinationShadow` (under `CanMirrorCopyImageShadow`) | after every shape decline, immediately before the shadow rows are copied |
| `"get-tex-image-shadow"` | `GetTexImageViaShadowConversion` | after the channel-mapping and zero-extent declines |

**Emulations that stay where they are, per D-M, and what changed about them:**

| emulation | owner | in this package |
|---|---|---|
| fragColor MRT broadcast (`BroadcastLegacyFragColor`) | server | unchanged as a rewrite; its **input** (`g_fragColorBroadcastCount`) is now derived from the framebuffer record's `DrawBuffers[8]`, keyed on its `ContentHash`, at the same point in the draw as before. Deleting the workaround and `g_broadcastMemo*` is P3b/P4b's |
| D24S8 dual-mode sampling (`DepthStencilSamplingReadImpl`) | server | **byte-identical**, §6.1 |
| blit LOD / layered blit, the multisample replicate path | server | untouched |
| draw-buffer `None` → `GL_NONE`, "apply draw buffers only for the DRAW target" | server (`Managers.cpp`, D's) | untouched here; the record's `Target` is what makes the second rule a field test rather than call-site discipline |
| layered attachment shapes, `SupportsLayeredImageBinding`'s rule | server | untouched; `e3` feeds it `record->Layered` / `record->Layer` and it still asks the **backend** target and still forces `layer` to 0 for a non-layerable one |
| image format bake / widen / split / array remap | server | untouched; `e3` feeds the recast the application's format from the record |
| texture LOD bias (`EmulateTextureLodBias` + the per-draw `glUniform1f`) | server | untouched |
| raw-depth-fetch sampler substitution | server, P3b/P4b to nativise | untouched, and now carries the D-F3 paragraph naming it as the largest surviving `MG_State`-type usage under `MG_Backend/DirectGLES` |

### 6.1 Byte-identical evidence

| item | evidence |
|---|---|
| **D-N, in package E's files**: `DepthStencilSamplingReadImpl` | it is a **namespace**, not a function (`DirectGLES.cpp:8437-8898`), so it is hashed the way P2's G5 hashes `RenderStateImpl`: `awk '/^    namespace DepthStencilSamplingReadImpl \{/,/^    \} \/\/ namespace DepthStencilSamplingReadImpl/' \| sha256sum` → `dfefc996…c9063f53` at `37da3c3a` **and** at HEAD. Package F's `p4a_untouched_regions.sh` will need the namespace shape for this one row |
| **D-N**: `ShouldUseCaveatTextureFormat` (`Utils.cpp:245`) | `git diff --stat 37da3c3a..HEAD -- Utils.cpp Utils.h MultiDraw.cpp` is **empty** — the whole file is untouched |
| **P3a's eleven** (G5) | `bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD` → **rc 0** at every one of the five commits |
| **P2's G5** (`RenderStateImpl` namespace) | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` == `~/w7/p4a-before-syncrenderstate.sha`, at every one of the five commits |

Package E's other D-N obligations (`StageBlocksIntoUnpackRing`, `UnpackRingAvailable`,
`UnpackRingAllocate`, `RecomputeBackendColorSlots`) all live in `Managers.cpp`, which this package
never opens.

---

## 7. Verification transcript

All commands in `~/w7/p4a-esprytdraw` with `CCACHE_BASEDIR=/home/swung/w7`.

### 7.1 Per commit (all five)

```
$ for c in 98bdfa3e c0dc679e f23fec0e 45810af0 0b00f7f0; do git checkout --detach $c; ... done

98bdfa3e  build-linux OK  build-push OK  unit(push) OK
          ### Removed (0) ### Added (0) ### Resized (0) ### Renamed only (0)
          p3a-untouched rc=0     RenderStateImpl sha d8fd1c48716056c5
c0dc679e  … identical …
f23fec0e  … identical …
45810af0  … identical …
0b00f7f0  … identical …
```

### 7.2 On the finished branch (`0b00f7f0`)

```
$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0
| before | ~/w7/p4a-before-libMobileGL.so | 19114360 | 10806323 |
| after  | build-linux/libMobileGL.so     | 19114360 | 10806323 |
### Removed (0)  ### Added (0)  ### Resized (0)  ### Renamed only (same size) (0)     <- G1

$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
[p3a-untouched] the 11 pool / deferred-release / ring / flush-drain functions are
[p3a-untouched] byte-identical between 37da3c3a and HEAD                             rc=0

$ awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' \
      MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp | sha256sum
d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27   == p4a-before-syncrenderstate.sha

$ cmake --build build-linux -j 12 && ctest --test-dir build-linux -L unit -j 12
100% tests passed, 0 tests failed out of 1635
$ cmake --build build-push  -j 12 && ctest --test-dir build-push  -L unit -j 12
100% tests passed, 0 tests failed out of 1635

# the legacy arms, and the default arm
$ MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu -j 8 -R DirectGLES
100% tests passed, 0 tests failed out of 491                                          rc=0
$ MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu -j 8 -R DirectGLES
100% tests passed, 0 tests failed out of 491                                          rc=0
$ (default = 0x1fff)       ctest --test-dir build-push -L integration-gpu -j 8 -R DirectGLES
100% tests passed, 0 tests failed out of 491                                          rc=0

# C.4's family subset, all three arms
$ ctest --test-dir build-push -L integration-gpu -j 4 -R '<C.4's 41-name regex>'
default 100% 497/497   |   0x1ff 100% 497/497   |   0 100% 497/497

$ grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'
MobileGL/MG_Backend/MGPipe/PipeInputs.h:1        # pre-existing at $BASE, unchanged   <- G13

$ grep -rc 'MGPipeUnmigratedEmulation' MobileGL/MG_Backend/DirectGLES/
MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4  # + D's one in Managers.cpp = 5      <- G13b

# both compile-time arms, since none of the three standing build dirs has it off
$ cmake -S . -B build-nolegacy … -DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF
configure rc=0 ; ninja -C build-nolegacy MobileGL → rc=0        (dir removed afterwards)
```

**The default-arm control that made §0's finding attributable**: with the working tree stashed, the
same build dirs at the tag ran `default → 100% 491/491`. That is what says "55 red" was this
package's and not the stub applier's.

### 7.3 Not run here, and why

* **G3 / G3b / G4** (`retrace_gate.py`, the verify lane) — `build-verify` does not exist in this
  worktree (`wsl_tree.sh p4a esprytdraw p4a/contract` was created without the `verify` argument, per
  C's tree-creation list). They belong to the integrated tree.
* **G6 / G7** (`*EmitTest`, `p4a_descriptor_negative_control.sh`) — packages B/C/F.
* **G8 / G8b / G9 / G12** (`HandleRecycleScenario`, `TextureParamsWithoutASamplerViewScenario`,
  `ObjectSubsystemControlScenario`) — package F's scenarios; none exists on this tree, so **D-E3's
  closure is implemented but not yet demonstrated** (§8).
* **G5's script** — package F's (DEV-11).

---

## 8. Post-rebase verification list

For the integrator, after rebasing this branch onto `wire` + `clientfb` + `clientsp` + `esprytobj`.
The first three are the ones that can only be answered on the integrated tree.

0. **Re-read §3's assumptions against `contract-v2`.** This branch is based on the **tag**
   `refs/tags/p4a/contract` (`08192d72`), as ID-2 requires, but `refs/heads/p4a/contract` has since
   moved to `2cb44039` (the contract rework). Anything the rework settled that §3 lists as an
   assumption — most likely **A6** (`MGPImageView::Access`'s encoding) and **A4**
   (`Resolve<Family>SubsystemArm()`'s home) — is a one-line change here rather than a design
   question, and should be taken before the rest of this list.
1. **The arms actually engage.** With the default mask, confirm each handle arm is taken rather than
   declining: `SyncCurrentFBOByRecord` returns true, `UnitBindingsEpochFromRecords` answers,
   `ResolveShaderImageRecord` returns non-null, `BindCurrentUnitSamplers` walks from records, and
   `ResolveGlobalConstantsRecord` answers. A silent all-declining tree passes every gate below and
   means this package did nothing. Recommend one temporary `MGLOG_D` per arm for the round, or the
   `PipeStats` gate counters (`Gate::EsprytUnitBindingsEpoch` hit rate should go to 100%).
2. **Neither seam check fires.** `grep 'does not describe the binding it names'` in the itest and
   retrace logs must be empty; a hit means a client emission is mis-keyed and is a **finding against
   B/C**, not a reason to relax the check.
3. **G9** — `ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView'`, and specifically
   `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` **green**, against package F's
   recorded red-before log. This is the one deliverable of this package that has no gate on its own
   tree.
4. **DEV-3, reconsidered with evidence**: on a tree that emits, compare
   `MGB_CTX->GetMaxTouchedTextureUnit()` against `SamplerViewCount - 1` for a frame; if they agree
   everywhere, `keys.maxTouchedUnit` can take the record in one line.
5. **DEV-6, reconsidered**: once package C documents `MGPImageView::Access`'s encoding, `e3` reads it
   (one `#define`), and `ImageEmitTest.AnAccessModeChangeAloneStillEmitsTheSet` becomes the gate on
   both sides.
6. **DEV-4 / A3**: once `d2` has re-keyed the twin's sync calls onto the record, move the unit
   texture sync list's membership to `BoundSamplerViews` and drop the frontend binding walk.
7. **A1 / A2 / A4**: replace the `GetOrCreate(currentFBO)` fallback with `GetOrCreate(record.Fbo)`,
   the four local arm latches with D's `Resolve<Family>SubsystemArm()`, and (if `d4` changed them)
   the two `SyncToBackend` call signatures. Then re-run §7.2 whole.
8. **The full C.4 verification**: G1, `p3a_untouched_regions.sh`, **`p4a_untouched_regions.sh`**
   (F's, including this file's two rows — note `DepthStencilSamplingReadImpl` is a **namespace**,
   §6.1), `ctest -L unit` ×3, `-L integration-gpu` on all three arms, the C.4 family regex, G13's
   greps, and `retrace_gate.py` on `build-push` and `build-verify`.
9. **Named traces**: `photon-v1.3b` on **llvmpipe desktop retrace** is G3b's canary for exactly the
   image-binding semantics `e3` moves, and `improved-transparency-minecraft-26.3` is the draw-buffer
   `None` net for `e1`. Neither can run here.
10. **The Track H census** (§2) is the input the phase owes `MEASUREMENTS.md`: **7 memos re-keyed**
    (3 directly, 4 transitively through the epoch substitution), **0 retired** (the pre-handle arms
    stay through P4a per `ARCHITECTURE.md:369`), **1 memo bypassed** on its record path
    (`UnitSamplerLookupMemo`), **1 new** (`g_readFboTextureSyncList`, D-E3), and the frontend reads
    listed per step.

---

## 9. Intermediate logs

Deleted, per ID-5 ("package logs `~/w7/p4a-<slug>-*.log` are deleted by their owner when the round
ends"): the five `~/w7/p4a-esprytdraw-*.log` tree-creation and first-build logs are gone, and so is
every `/tmp/p4ae-*` build, ctest and arm log this round produced. `~/w7/p4a-esprytdraw` itself holds
only `build-linux` and `build-push` (the `build-nolegacy` dir of §7.2 was removed after its one
compile). Nothing was written under `~/w7/notes/` except this file. The scratchpad's
`wsl/p4a-esprytdraw-*.sh` command scripts remain in the shared `wsl` directory, which ID-7 says
never to delete.
