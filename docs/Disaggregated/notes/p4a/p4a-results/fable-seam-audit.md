# P4a seam audit (fable research agent), 2026-09-08

**Tree audited:** `~/w7/pipe` = `feat/disaggregated` @ **`41b79050`** (E's verification round landed: the
S-1 call-site line + SD-0's `Tracker.h` bind-generation mix), i.e. c0..c0g + wire + B + C + D + E. Read-only;
no tree was edited. Runs: the Iris retrace set on `41b79050`, on E's pre-landing lib `2e03b82a` (D's
landing without E's last commit), and a three-point bisect of D's landing commits in a private worktree
`~/w7/fable-bisect` (results in §B). Retrace outputs under `~/w7/retrace-out/fable-*`.

**Method of the sweeps (§A):** the P4a record catalogue (`PipeCalls.def`, `PipeFields.def`, `MGPipeTypes.h`)
was walked field by field against (i) the emitter that fills the field (`MG_Impl/Pipe/*Emit.h`), (ii) the
dirty bit / birth hook that triggers the emitter (`PipeFill.cpp:2457-2517`, `Tracker.h:283-524`), (iii) the
frontend setter that changes the field's source (`grep` over `MG_State` for every `MGP_NOTE_AGGREGATE`,
`Bump*Generation`, `NoteUnitTouched`, `++m_*Version` site; `DirtySurface.def` rows), and (iv) every server
reader of the field (`grep 'st\.\|MGPipeApplier()' DirectGLES.cpp`; `grep 'surface\.\|pushedRecord->\|record->'
Managers.cpp`). Handle minting was checked per kind on both sides (`MGPipeSlots().Acquire/Allocate/AllocateFor`
vs `TwinRegistry`/`BackendSlotTable::Acquire`, `SlotTables.h:232`). Every client-side clear/evict/release was
listed with its consumer guarantee, every process singleton in `MG_Impl/Pipe` with its per-context state,
every serial with the memo that reads it. The retrace's MobileGL log (`<case>/DirectGLES/output/mobilegl.log`,
the file sink - `ctest -V`/stdout is a false zero, E §6.0) was censused for refusal and seam lines on every run.

---

## 0. Verdict in brief

* **Proven remaining seams: 4 that need a fix before P4a closes (F-1, F-2, F-3, F-4), 3 recorded (F-5,
  F-6, F-7).** All four are instances of the classes the seven contract fixes already hit - three of them
  class 6 (a setter that changes a record field moves no shutter the emitter reads) and one class 2 (an
  identity-keyed twin table looked up by a content-addressed handle, S-1's twin one loop over). None
  produces a wrong picture on the current itest/retrace corpus, which is exactly why they survived; each has
  a public-GL sequence that does, and none of the 80 scenarios or 79 traces contains that sequence.
* **Iris (17 shader-pack retrace failures at ee31944b): S-1, the D half, and they were NOT silent.** A
  three-point bisect of D's landing reproduces E's 17 exactly at `ee31944b` and at `a632e26e` (3/20, the
  same SSIMs to six decimals) and goes 20/20 at **`1e8cdc59`** (S-1's server half); `588b2277` (S-4) and E's
  `41b79050` line are not needed. The path is the **raw-depth-fetch sampler** (`DirectGLES.cpp:207-278`): a
  backend-minted `SamplerObject` that no client ever describes, which v2's `SyncToBackend` looked up by its
  identity handle and refused - every failing trace's `mobilegl.log` carries `MGPipe: sampler N has no
  applier record on the handle arm, so its parameters cannot be pushed` (17 of 17; the three passing
  controls carry none) - so its NEAREST/no-compare parameters never reached the driver sampler bound over
  every `sampler2D` depth read, which is what a shader pack does all frame. E §7's "zero named refusals
  and zero seam lines" was asserted from the itest census; the retrace census was never taken (the trace
  harness writes `output/mobilegl.log`, not `retrace.log`). On `41b79050` and on `2e03b82a` all 20 pass
  with zero refusal/seam lines. c0g is inert at `0x1fff`.
* **Why the brief missed them:** D-K4's sentence "the shutters are already computed, latched and counted
  today - P4a only emits for them" is the root of c0d, SD-0, F-1, F-2 and F-3 alike: the P2/P3a shutters
  were built for value-class blocks and coarse aggregates, and the P4a records carry *derived* fields
  (program-resolved unit sets, per-program windows, inlined attachment formats) that no existing shutter
  covers and that `DirtySurface.def` cannot see (it maps mutators to bits, never record fields to
  mutators). The brief never tabulated record field -> source setter -> shutter, never tabulated the
  handle rule per kind on BOTH sides (D-F1 content vs D-I1's per-object death row vs D's identity twin
  registries), and stated the dependency/mirror rules as facts about masks rather than about emissions.

---

## A. The sweeps, class by class

Confidence scale: **proven** = code-proven on both sides and refuted every alternative I tried;
**mechanism** = code-proven chain, not yet demonstrated by a run; **latent** = correct today only because
the reader is absent or reads the frontend.

### A.1 Class 1 - an encoding stated nowhere (c0c's class)

Mechanism: every `Uint8/Uint16/Int8` field of the P4a payloads in `MGPipeTypes.h` (grep'd: 31 such
fields) classified as (a) spelled by a `kMGPipe*` constant in the contract, (b) a `static_cast` of one
frontend enum used identically by both sides, (c) an encoding invented in a package header.

| field | where the encoding lives | reader | status |
|---|---|---|---|
| `MGPSubData::Target`, `MGPSurface::Kind`, `DepthStencilMode`, `MGPSurface::TextureTarget` (0xFFFF), `MGPipeFramebufferTarget` | contract (c0c/c0e) | D/E | closed |
| **`MGPImageView::Access`** | `ImageEmit.h:146-159` only (`MGPipeEncodeImageAccess`, 0/1/2); `MGPipeTypes.h:797` has no constant | **none** - E reads the frontend's `imageBinding.Access` on purpose (`DirectGLES.cpp:2492-2500`: "reading it here before that encoding is written down would be inventing it") | **F-6a, latent** |
| `MGPSamplerView::Target` (`Uint8`, `MGPipeTypes.h:441`) | `SamplerEmit.h:900` `static_cast<Uint8>(texture.GetTarget())`, no comment in the contract | D's sampler-view twin (no field decode found) | F-6b, latent (same enum both sides) |
| `MGPFramebufferState::DrawBuffers[8]` (`Int8`, -1 = None, four default-FB tokens -> 0) | `FramebufferEmit.h:146-163` (B's narrowing) | **none** - D's mask loop indexes the record's surfaces by the FRONTEND token `stateDrawBuffers[i]` (`Managers.cpp:9322-9330`), never by `DrawBuffers[]` | F-6c, latent |
| `MGPSurface::UploadTarget` empty-point spelling | `FramebufferEmit.h:102-113` (B's m6: `TextureUploadTarget::Unknown`) | D's `SyncAttachmentSurface` | latent, consistent by shared enum |
| `MGPResourceDesc::StorageKind`, `Swizzle[4]`, `MGPImageView::InternalFormat`, `BindMask` | shared enum casts / contract constants | D | fine |

**F-6 (latent, low):** three encodings are written in a package header rather than the contract and have
no server reader today. Each is the c0c shape one reader away: the first server that decodes `Access`
will write its own table. The fix is documentation-only now (`MGPipeTypes.h` constants for `Access`, a
comment on `MGPSamplerView::Target` and on the `DrawBuffers` narrowing).

### A.2 Class 2 - a handle minted by IDENTITY on one side and by CONTENT on the other (ID-14/17, S-1)

Mechanism: per kind, the client mint (`MGPipeSlots().AllocateFor(kind, lifetimeId)` = identity;
`Allocate(kind)` with no lifetime id = content) against every server lookup of a handle of that kind.

| kind | client | server lookup | status |
|---|---|---|---|
| Texture, Renderbuffer, Framebuffer, SamplerViewCso, ShaderCso (+ composite band) | identity (`TextureEmit.h:616`, `SamplerEmit.h:881`, `FramebufferEmit.h:256`, `ProgramEmit.h:406-419`) | `TwinRegistry`/`BackendSlotTable` identity (`SlotTables.h:232`), records by handle | consistent |
| SamplerCso (built-in sampler in `MGPTextureParams`) | **content** (`SamplerEmit.h:426`) | record by the carried handle (`Managers.cpp:7710`) | fixed (ID-14/17, S-4) |
| SamplerCso (`BoundSamplerStates[unit]`) in `BackendSamplerObject::SyncToBackend` | content | record by the handle the caller carries (`Managers.cpp:12352`) | fixed (S-1 + E's `41b79050` line) |
| the raw-depth-fetch sampler (`DirectGLES.cpp:207-278`, a backend-minted `SamplerObject`, never described by any client) | **no record can exist** | v2: identity `HandleOf(object)` -> "no applier record" refusal on every depth read; `1e8cdc59`: unregistered -> the object is the authority (`Managers.cpp:12361-12372`) | fixed by `1e8cdc59` - **this is the Iris failure (§B)** |
| **SamplerCso in `BindCurrentUnitSamplers`' record arm** | content | **`g_backendSamplerObjects.FindByHandle(sampler)` (`DirectGLES.cpp:4847`)** - a registry keyed by the SamplerObject's lifetime id (`Managers.cpp:12546`, `Managers.h:2533`) | **F-4, proven** |

**F-4 (class 2, proven, latent today).** `st.BoundSamplerStates[index]` is a content-addressed CSO handle
(allocated by the cache with no lifetime id); `g_backendSamplerObjects` mints its slots off
`SamplerObject::GetLifetimeId()`. One allocator, two disjoint slot families: the lookup can never find a
twin, so the S4 branch ("a live handle whose twin does not exist yet is left alone") is taken on **every**
draw for every bound sampler, and E's rationale for S4 - "the program pass creates the twin later in the
same draw" - is wrong: the twin the program pass creates (`DirectGLES.cpp:5296-5311`,
`GetOrCreate(samplerObject)`) sits at the *identity* slot and will never sit at the CSO slot. The record
arm's only live effect is the unbind of null/out-of-window units; every sampler-object bind on the handle
arm is still performed by the pre-handle program pass. This is S-1's confusion one loop over, and
`SamplerEmit.h:201-205` already states the rule D's registry violates ("a backend must NOT key a sampler
twin on a SamplerObject's lifetime id; the twin's life is create_sampler_state -> delete_sampler_state").
*Consequence today:* none visible (the program pass binds). *Failure scenario:* the moment the program pass'
sampler bind is memoised away or retired (P3b/P4b's "175-line debounce removal" names exactly that region),
every `glBindSampler` stops reaching the driver with no refusal. *Lane:* none can see it; a white-box case
("after BindCurrentUnitSamplers' record arm, unit N's driver sampler == the CSO's twin, walkedFromRecords
bound > 0") is red today. *Fix (D):* key the sampler twin table by CSO handle (create the twin at
`create_sampler_state` / on first lookup by handle), and correct E's S4 comment.

### A.3 Class 3 - a record keyed by BINDING while consumers address by OBJECT (ID-19's class)

Mechanism: every applier table vs every reader's key. Framebuffer: per object + two bound handles (wire v3),
DSA readers by handle (`Managers.cpp:8964-8971`) - closed. The three unit sets: keyed by unit, consumed by
unit (`DirectGLES.cpp:2447-2455`, `4831-4835`) - consistent. `set_texture_params`: per texture. Program:
per CSO. **No new instance.** One monolith-only note: D's mask loop reads the *frontend* draw-buffer tokens
to address the record's surfaces (F-6c) - a binding-side input over an object-keyed record; harmless while
the frontend is in-process.

### A.4 Class 4 - a process-wide singleton keyed by a per-context name (C2-M1's class)

Mechanism: every `*Instance()` in `MG_Impl/Pipe` (10: Tracker, CsoCache, SamplerCsoCache,
ProgramOpaqueUnitsShared, SetHashSuppressor, five emitters, CompositeResolver, ResourceTracker) vs the
per-context state it holds and what `FreshlyPrimed` resets (`PipeFill.cpp:2414-2440`).

* Tracker: context pointer check + `Reset()`; SetHashSuppressor: `InvalidateAll()`; the five emitters:
  bound-latch halves only (`SamplerEmit.h:1015-1022` releases the bound-set refs, `ImageEmit.h:133` window,
  `FramebufferEmit.h:404-414` incl. the Named latch, `ProgramEmit.h:363-372`); CompositeResolver keyed on
  `(GetTextureContextId(), pipeline name)` (`ProgramEmit.h:118-126`, C v3) - all correct.
* `MGPipeProgramOpaqueUnits` memo keyed on `(lifetimeId, linkVersion, backendStateVersion)` - lifetime ids
  are process-unique; fine.
* The texture drain list `m_drain` is process-wide, not "per-context" as D-D4 says; it is keyed on
  handles of share-group objects and the applier is one per process, so it is correct in monolith and is
  a P5 item (one drain per client context).
* `FramebufferRecords` survive a make-current although an FBO is per-context (`PipeApply.h:485`); keyed by
  process-unique handles, so two contexts' FBOs cannot collide. Fine.

**No new instance.**

### A.5 Class 5 - a destructive client-side change with no consumer/dependency guarantee (c0f/c0g, ID-17)

Mechanism: every client-side clear/evict/release/latch and what guarantees a consumer: (1) the level dirty
clear `MarkStorageDirty(false)` (`TextureEmit.h:1097`) - gated on acceptance AND on
`FamilyIsLive` = bit + wired + consumer + D-K2 deps (`PipeFill.cpp:1064-1069`, `2470-2475`); (2) sampler CSO
LRU eviction - reference-counted (ID-17); (3) composite release - through the one helper; (4) the drain
list clear - only for accepted levels; (5) `DrainKeys` rebuild; (6) the bound-set reference release on
Reset; (7) every emitter latch.

* **F-7 (class 5/7, minor):** `EmitTextureParams` sets its version latch (`TextureEmit.h:637-643`) BEFORE
  `MGPipeApplySetTextureParams` (void; refuses when the texture has no record or a null
  `BuiltinSampler`, `PipeApply.cpp` set_texture_params:14/22). A texture born in the
  register/unregister window (no create, `PipeFill.cpp` c0f note) that receives `glTexParameter*` before
  any respecify has its params refused and latched; the later self-healing create republishes the
  resource but not the params, so `ParamsSerial` stays 0 and the built-in sampler is never pushed (the S-4
  path, `Managers.cpp:7690-7696`) - the driver keeps its defaults against the application's filters.
  Narrow (needs the teardown window); the fix is to latch only on acceptance (the D-D5 rule applied to
  params) - the same shape as wire's n2 for framebuffer records (`FramebufferEmit.h:311-318` latches after a
  void `Emit`). The kVarTail sets are moot: `ApplyUnitWindow`'s only refusals are Fatal.
* **F-5 (recorded = E's SD-5, still open):** both sides express D-K2 against the *bit*, non-transitively
  (`PipeFill.cpp` c0g: "IT IS THE RUNTIME BIT THAT IS TESTED, NOT THE OTHER FAMILY'S LIVENESS";
  `Managers.cpp:3598-3610`). At `0x7ff` the framebuffer family is live on both sides while the texture
  family refuses itself; the surfaces name texture handles that never get twins and D's adoption refusal
  makes E's F4 decline the whole arm - loud, correct picture, a lane that reports 53 fails to whoever
  sweeps masks. Not a shipped mask. The brief rule: a family's dependency is the *resolved liveness* of the
  family it depends on (transitive closure), stated once and mirrored.
* The structural residue that stays after c0f/c0g: the client's gate is *family*-level, the server's
  declines are *per object* (no record / stale generation -> legacy read of a dirty flag the client has
  cleared). Zero such declines on the itest census (ID-41) and on the 40 retrace logs I censused, so no
  reachable instance; it is the rule a P4b brief must state (a per-object server decline may never fall
  back to a flag the client owns).

### A.6 Class 6 - a tracker shutter that does not move on a change that alters a record field (c0d, SD-0)

Mechanism: the record field -> source setter -> shutter table, built for every P4a emission and checked
against the bump sites (`o`-grep: `MGP_NOTE_AGGREGATE(TextureContent|TextureParams|FramebufferAttachment)`,
`NoteUnitTouched`, `BumpSamplingResolutionGeneration`, `++m_imageUnitVersion`, `UseProgram`).

| emission (bit) | shutter (`Tracker.h`) | record inputs NOT in the shutter | finding |
|---|---|---|---|
| `set_framebuffer_state` (11) | attachment aggregate ⊕ draw bind ⊕ read bind (:487-493) | **the attachments' `InternalFormat`/`TextureTarget`/`Width`/`Height`/`Samples`/`Complete`** (`FramebufferEmit.h:115-144, 589-617`) - a texture or renderbuffer *respecify* moves `TextureContent`/`TextureParams` (`TextureObject.cpp:178-184`, `:410-412`) or nothing (renderbuffers, D-D2), never `FramebufferAttachment` | **F-3** |
| `set_sampler_views` (12) | textureContent ⊕ textureBind (:494-495) | **the current program** (`SamplerEmit.h:742-767`: `GetProgramForDraw()` -> `SamplerTarget[unit]`), the sampler uniform's unit (`glUniform1i`, bit 7), the effective sampler's parameters and the texture's (`SamplesAsIncompleteTexture`, textureParams/sampling generation - bit 13's inputs) | **F-1** |
| `bind_sampler_states` (13) | textureParams ⊕ samplingResolution ⊕ textureBind (:515-517) | none (program-independent walk; `glDeleteSamplers` bumps via `TextureUnit.cpp:39`) | ok |
| `set_shader_images` (14) | textureContent ⊕ textureParams ⊕ **`program->GetImageUnitVersion()`** ⊕ textureBind (:518-520) | **the program's identity**: the plain-program arm mixes a per-program counter that is equal for two programs with the same number of image-unit assignments (`ProgramObject.h:826-848`); the pipeline arm mixes `stageLinks` (:420), the plain arm does not | **F-2** |
| `create/bind_shader_state`, `set_draw/dispatch_program` (6/7) | lifetime ⊕ link (:353); bindings without identity (:354-358) but the emitter runs on 6\|7 | ok |
| `set_global_constants` (8) | lifetime ⊕ UBO content (:359) | ok |
| `set_texture_params` (hook) | version-first latch on (params, sampler) versions + ForceResync (`TextureEmit.h:635-643`) | ok |
| `resource_respecify` / renderbuffer | emitted from the storage entry points (`PipePublishDescriptor`) | ok |

**F-1 (class 6, proven shutter gap; wrong picture deferred to the first server reader).**
`EmitSamplerViews` resolves the set for *the program bound at the validate point* and is called only when
bit 12 fires (`PipeFill.cpp:2477`); `EmitShaderState` neither invalidates the `SetSamplerViews` suppressor
slot nor calls the sampler emitter (`ProgramEmit.h:105-158`). `glUseProgram` moves nothing bit 12 reads
(`Core.cpp:393-395` -> `m_programState`; no `BumpTextureBindGeneration` on that path). Sequence:
`glBindTexture` x N; `glUseProgram(P1)`; draw; `glUseProgram(P2)` where P2 samples a unit/target P1 did not
(or `glUniform1i(sampler, otherUnit)`); draw -> `BoundSamplerViews` still describes P1's set. Same for a
`glTexParameteri(MIN_FILTER)` that flips `SamplesAsIncompleteTexture` (bit 13 fires, bit 12 does not) - the
entry stays null. *Reader today:* only E's epoch (`DirectGLES.cpp:1835-1871`); no code on either side reads
a `BoundSamplerViews` entry (`grep BoundSamplerViews` over `MG_Backend`: none). So the record is
write-only in P4a and the gap is invisible to every lane; the first server that binds from the set
(P3b/P4b's debounce removal, P7's Magma views) renders the previous program's units. *Fix (A,
`Tracker.h:494`):* mix the same program identity the emitter's memo is keyed on (`GetCurrentProgram()`'s
lifetime id ⊕ link version ⊕ backendStateVersion, or `stageLinks` under a pipeline) and `textureParams`
into bit 12 - the c0d/SD-0 shape; the suppressor absorbs the extra fires. Unit case: "a program switch
alone re-emits the view set" (red today on `SamplerEmitTest`/`TrackerTest`).

**F-1b (class 7, mechanism-proven, narrow, wrong picture with no refusal).** E's e2 replaced the
program-independent per-slot walk epoch (`UnitBindingsUnchanged`, every binding slot + sampler of every
touched unit) with the two record serials (`DirectGLES.cpp:1868-1871`) and keys the program-independent
`g_unitTextureSyncList` on it (`:2148-2200`). The serials move only when the *program-resolved* set's hash
moved. A `glBindTexture(target, T)` onto a slot that was EMPTY (no entry for `PairingsIntact` to compare)
on an already-touched unit (`Count` unchanged) while the current program does not sample that unit/target
moves neither serial; a following `glUseProgram(P2)` that samples it fires no bit 12 (F-1); the list is
never rebuilt, T is never `SyncTextureObjectToBackend`'d, and `ResolveAndBindUnitTextures` finds no twin
(`DirectGLES.cpp` ~4630 `Find()` null -> `fullyResolved=false; continue`) and then *clears* the native
target -> P2 samples an unbound unit, no refusal, and `fullyResolved=false` cannot self-heal because the
list memo still hits. Rescued only by a `maxTouched` move, any `glTexParameter*` anywhere
(`samplingGeneration` in the key) or a replacement on a populated slot. *Lane:* none
(`SampledSetStalenessScenario` binds before it switches). *Fix:* E's key should mix the frontend
`GetTextureBindGeneration()` (one accessor read; over-fires only on binds) beside the serials, or the serial
must move on the F-1 fix plus a bind-generation input - the record epoch is a *narrower* shutter than the
walk it replaced, which is the direction `Tracker.h:42-48` forbids.

**F-2 (class 6, proven, degrades loudly-ish).** Plain-program arm: `programImages =
program->GetImageUnitVersion()` (`Tracker.h:360`) - a per-program counter incremented only in
`SetUniformSamplerOrImageUnitIndex` for image uniforms (`ProgramObject.h:846-848`); two programs commonly
carry equal values, so a `glUseProgram` between them (no bind/content/params change) fires no bit 14 and
`set_shader_images`' window stays the previous program's (`ImageEmit.h:82-87`). E's I3 then takes the
pre-handle bind for the units outside the window (MAJOR-1's union, silent by E's table) - correct picture,
a permanent silent fallback, exactly A8's "the image half is permanent". *Fix (A, one line):* mix the
program's lifetime id into `programImages` in the plain arm, as the pipeline arm already mixes
`stageLinks` (`Tracker.h:420`).

**F-3 (class 6, proven divergence between the two arms, uncaught by any lane).** The framebuffer record
inlines each attachment's `InternalFormat` (and `TextureTarget`, extent, samples, `Complete`) at emission -
D-C1's "inline, so the four cross-object masks fall out at push time with no lookup". Nothing bit 11 reads
moves when an *attached* texture is respecified: `TextureObjectBase::SetInternalFormat` bumps
`TextureParams` + shape (`TextureObject.cpp:178-184`), `AllocateStorage`/`BumpContentVersion` bump
`TextureContent` (`:410-412`); no path notifies a framebuffer (`grep FramebufferAttachment|BumpAttachmentVersion
TextureObject*.cpp`: none); a renderbuffer re-storage bumps nothing (D-D2 closed only the *resource*
record). On the server the four masks are answered from the stale surface on the handle arm
(`Managers.cpp:8678-8723`, consumed at `:9328-9353`) while the legacy arm reads the frontend attachment at
the same sync (`:9356-9372`). Both arms DO re-run `SyncToBackend` after the respecify - the re-mint bumps
`g_attachmentBackendIdGeneration`, which both memos carry (`DirectGLES.cpp:2852-2857`, the pre-handle
quartet) - so the legacy arm comes out fresh and the handle arm frozen. Sequence (public GL, both arms
otherwise identical): `glTexImage2D(tex, RGB8)`; `glFramebufferTexture2D(fbo, tex)`; draw;
`glTexImage2D(tex, RGBA8, same size)` (no re-attach); draw; `glReadPixels` -> alpha-widened mask still set:
`IsAlphaWidenedFallbackReadAttachment` forces opaque alpha on the handle arm and not on the legacy arm
(`DirectGLES.cpp:10865`); the mirror (RGBA8 -> RGB8) reads the driver's widened alpha instead of 1.0. The
same for `RGBA8 -> RGBA8_SNORM` (clamp mask, `:11084/11705`) on caveat drivers and for
`glRenderbufferStorage(newFormat)` on an attached renderbuffer. `Width/Height/Layers/Samples/Complete` have
no reader (grep) and only go stale. *Lane:* none - `SnormAttachment`, `ThreeChannelAttachment`,
`RenderbufferBlendFormat` all storage-then-attach; `ImageSizeAfterRespec` respecifies an image texture, not
an attachment. *Fix:* either (D) answer the four masks from the *resource* record's `Desc.InternalFormat`
(`TextureResources[surface.Res.Slot]`, which `resource_respecify` keeps fresh) instead of the inlined copy -
an applier-internal lookup, no frontend - or (A) mix the texture aggregates into bit 11, which fires the
304-byte hash on every texture upload. The first is the right one: the inlining "so no lookup is needed"
is the seam. One scenario pins it (respecify-while-attached, both directions, readback alpha).

Refuted along the way: `SetReadBuffer`/`SetDrawBuffer`/default-size setters all bump the attachment
aggregate (`FramebufferObject.cpp:205-241`); `glDeleteTextures`/`glDeleteSamplers` bump the bind generation
(`TextureState.cpp:142`, `TextureUnit.cpp:39`); `glBindImageTexture` reaches `NoteTextureUnitTouched`
(`GL_Texture.cpp:6742`); `glBindSampler` too (`GL_Sampler.cpp:370`); `DirtySurface.def`'s
`BindProgramPipelineObject = kPulledEveryVerb` prose predates c0d (the shutter now reads the pipeline) but
the pull still holds on every path - stale reasoning, not a defect.

### A.7 Class 7 - a serial that is not an "emitted" test; memo keys too coarse (E §0, ID-27)

Mechanism: every `Serial` in `PipeApply.h` vs every memo in `DirectGLES.cpp`/`Managers.cpp` that reads it.
`FramebufferSerial` (one per family, advanced on Reset; E's memo adds backend-id and context generations -
correct, coarse = recorded perf item); `ParamsSerial` 0 = never applied (S-4, correct now);
`SamplerCso.Serial` + handle as the key (`Managers.cpp:7720-7731` - the handle is in the key because
content addressing can re-point a texture at a CSO with a smaller serial: correct);
`SamplerViews/SamplerStatesSerial` as E's epoch (**F-1b**: moves less often than the walk it replaced);
`ShaderImagesSerial` (not used as a memo). **F-1b is the one new instance.**

### A.8 Class 8 - a clear that is too wide (wire C1)

`PendingUploads` erase per `(uploadTarget, level)` with the cube face in the key (wire v3 + F's case);
`MGPipeApplierReset` clears working state only; `ReleaseObjectRecords` clears all (teardown);
`SamplerEmitter::Reset` releases the bound-set refs only; `FramebufferEmitter::Reset` drops the Named latch
(over-fire, argued). **No new instance.**

### A.9 Class 9/10 - gates that cannot go red; name sets

G7 trips and exits 0 on the integrated tree (gates-v3 §5); G9 is white-box; G2 2846 == 2846. Two gates that
still cannot go red: (i) E's S4/"walkedFromRecords" arm is counted as engaged while it binds nothing (F-4)
- the MINOR-4 gate tick measures an arm that does no work; (ii) E §6.6's "unit-list equivalence between
0x1ff and 0x1fff" was argued from failing sets, and the per-frame rebuild count is not instrumented - F-1b
lives exactly there. The retrace-time refusal census that E §7 asserted ("zero named refusals") was never
taken on trace runs (the trace harness writes `output/mobilegl.log`; `retrace.log` has no MobileGL lines);
I took it: zero on 40 runs (§B).

---

## B. The seventeen Iris retrace failures

Reference: E v3 §7 - 62/79 at the default mask on `ee31944b` without E, 17 failures, 15 Iris packs +
`improved-transparency-26.3` + `1.21.11-main-menu`, SSIM 0.61-0.99.

**Run 1 - current head `41b79050`** (pipe's `build-push`, stamp `GIT@41b7905`; E landed incl. the S-1
line and SD-0): `retrace_gate.py --only '(iris|improved-transparency|main-menu).*DirectGLES'` = 20 cases
(the 17 + `1.17-main-menu-854`, `1.21.4-main-menu`, `iris-mellow`): **20/20 PASS**, SSIM 0.9935-1.0
(`~/w7/retrace-out/fable-audit/`). Log census over the 20 `mobilegl.log`s: **0** lines matching any of the
ten refusal shapes, the three E seam stems or `Fatal{`; the only ERROR/WARN lines are the pre-existing
`NormalizePixelFormat GL_STENCIL_INDEX8` pair, the capability-query and stub warnings and the residual
`SetVertexAttribDefaults` note.

**Run 2 - control `2e03b82a`** (E's branch head before `41b79050`: D's landing `a632e26e`/`1e8cdc59`/
`588b2277` + c0g present, E's S-1 call-site line and SD-0 patch ABSENT; lib `~/w7/p4a-esprytdraw/build-push`,
stamp `GIT@2e03b82`): **20/20 PASS**, SSIMs identical to run 1 to six decimals. Census: identical, and in
particular **zero** occurrences of D's S-1 fallback line for a *registered* sampler (`"sampler N was
synced without its unit's SamplerCso handle"`, `Managers.cpp:12379-12385`) - so no application
`glBindSampler` sampler is synced in these 20 traces, and E's call-site line (`41b79050`) is not what fixed
them. SD-0 is likewise excluded (present in run 1 only; E measured it as `create-indirect` alone). c0g
changes nothing at `0x1fff` (every dependency set; the `TextureScope` fixture edits are test-only).

**So the 17 were fixed by D's landing on `ee31944b`, i.e. by one of `a632e26e` (nine re-review minors),
`1e8cdc59` (S-1's server half) or `588b2277` (S-4).** S-4 cannot change a picture by mechanism: the
pre-fix path returned without pushing the built-in sampler for a texture that was never `glTexParameter`'d,
and `SamplerParameters`' defaults are GL's (`MGPipeValueTypes.h:462-486`: NEAREST_MIPMAP_LINEAR/LINEAR/
REPEAT/LEQUAL/None), so "no push" and "push defaults" are the same driver state; D's own report calls
it a decline with no visible consequence. `a632e26e`'s picture-relevant hunks (N-5 `SrcOffset`
cross-check, N-6 record-empty point) fire on no run. The bisect below settles it.

**Bisect** (private worktree `~/w7/fable-bisect`, same configure line as `wsl_tree.sh`, `build-push`, fixtures
copied from pipe with `--assume-unchanged`, the same 20 cases; log `~/w7/fable-bisect.log`, outputs
`~/w7/retrace-out/fable-bisect-<rev>/`):

| rev | content | result (20 cases) | `mobilegl.log` census |
|---|---|---|---|
| `ee31944b` | E's base (before D's landing) | **3/20 - the 17 fail, SSIMs identical to E §7 to six decimals** (0.610443849 super-duper-vanilla ... 0.994435418 improved-transparency) | 17 x `MGPipe: sampler N has no applier record on the handle arm, so its parameters cannot be pushed (handle {N, N})` + 17 x the S-4 line; every failing case carries the sampler refusal, the three passing controls carry none |
| `a632e26e` | + nine re-review minors | **3/20, identical** | identical |
| `1e8cdc59` | + S-1 (server half: the record is taken from the handle the caller carries; an unregistered, backend-minted sampler is its own authority) | **20/20** | the sampler refusal is gone |

**Narrative.** The 17 are **S-1**, through a path nobody named: `GetRawDepthFetchSampler()`
(`DirectGLES.cpp:207-278`) constructs a frontend `SamplerObject` inside the backend (NEAREST/NEAREST/no
mips/compare None) and syncs it through `BackendSamplerObject::SyncToBackend` before binding it over every
`sampler2D` unit that reads a depth texture (`:5290-5293`). No client ever emits a `create_sampler_state`
for that object. D v2 resolved the record by `g_backendSamplerObjects.HandleOf(object)` - the twin's
identity handle - found nothing, logged the refusal and returned without a push, so the driver sampler
object over every depth read kept GL defaults (NEAREST_MIPMAP_LINEAR on a single-level depth texture is
incomplete: every depth fetch reads 0). Shader packs read depth in every composite pass; vanilla in-world
traces have no `sampler2D` depth reads, which is why the failures clustered on Iris (plus 26.3's
improved-transparency and the 1.21.11 menu, which do). `1e8cdc59` sends an unregistered object down the
"the object IS the authority" branch (`Managers.cpp:12361-12372`), silently and correctly; E's line at
`41b79050` covers the *registered* (application) samplers, which these traces do not use.

Two corrections to E v3 §7 follow: the failures were **category (b) - a loud named refusal in every
failing log** - not category (c), and the "zero refusals" sentence was true of the itest census only; the
retrace census (`<case>/DirectGLES/output/mobilegl.log`, one file per run, no shim needed) had never been
taken. The integrator's post-merge should add it: `grep -c 'no applier record\|does not describe\|declining
the built-in\|cannot be pushed' retrace-out/*/*/DirectGLES/output/mobilegl.log` must be all-zero.

**Classification of the 17 on the current head:** all 17 = "fixed by `1e8cdc59`, loud refusal at
`ee31944b`, no refusal, no seam line and SSIM >= 0.9935 on `41b79050`"; none is E's.

---

## C. Root causes in the brief, and what a P4b/P5 brief must state up front

1. **No record-field -> source-setter -> shutter table.** D-K4's "the shutters are already computed,
   latched and counted today - P4a only emits for them" treated the P2/P3a shutters as complete for records
   they were never designed for. Every class-6 seam - c0d (bit 13 without the bind generation, bits 6/7/8
   under SSO), SD-0 (image re-bind), F-1, F-2, F-3 - is a record field with a source the shutter does not
   read. `DirtySurface.def` cannot catch this class: it maps *mutators* to bits and cannot see that a
   *derived* field (a program-resolved set, an inlined attachment format, a per-program window) depends on a
   mutator whose row is `kPulledEveryVerb`/UNDECIDED (`UseProgram`) or maps to another family's aggregate
   (`SetInternalFormat` -> `TextureParams`). The rule to state: **every field of every emitted record names
   the frontend counter that moves when its source changes, and that counter is in the emitting bit's
   shutter or the emission is unconditional at the setter.** The table is the deliverable of the contract
   package, checked by a unit case per row (set the source, assert the bit fires).
2. **No handle-rule table per kind, for both sides.** D-F1 (content-addressed sampler CSO) and D-E1
   ("BuiltinSampler is a SamplerCso handle") never said who mints it; D-I1's death table gave `SamplerCso`
   a per-object helper emitting `DeleteSamplerState`, contradicting D-F1's LRU-only death; D's twin
   registries (`TwinRegistry`, `SlotTables.h`) mint by lifetime id for every kind with no rule saying a
   content-addressed kind may not be looked up that way. ID-14/17, S-1 (both the bound-sampler and the
   raw-depth-fetch instances, §B) and F-4 are one confusion, found one call site at a time. Rule:
   **per kind: identity or content, who mints, who frees, and every server table that may key on it -
   stated in the contract, with a static check that a content-addressed kind has no lifetime-id twin table.**
3. **"Fine" mirror pairs and gates stated about masks, not emissions.** D-K2's "bit 10 without 11 is fine"
   and D-B1's "a push build with P4a's bits clear registers nothing new" described the server's arms and
   never asked what the *client* had already cleared (c0f, c0g, S-3). Rule: **a family is live on the client
   iff its consumer is registered and every family it depends on is live (transitive), and no client-side
   state is cleared before the applier's acceptance - and the same predicate is evaluated on both sides.**
   F-5 shows the non-transitive table still disagrees with itself at `0x7ff`.
4. **Records keyed by binding.** D-C2 ruled "emitted once per bound target" for a record whose consumers
   address objects by name (ID-19). Rule: **object records are keyed by the object's handle; binding is a
   separate, per-context fact.**
5. **Per-context rule for singletons.** D-H7 keyed the composite resolver on a GL name without saying names
   are per context (C2-M1). Rule: every process singleton lists its per-context state and the context id it
   keys on; `FreshlyPrimed` resets exactly that list.
6. **Serials.** D-J4 said "advance, never zero" but not "a serial is never an emitted test" (E §0) nor "a
   record-serial epoch must not be narrower than the shutter it replaces" (F-1b).
7. **Evidence rules.** "Zero refusals" was asserted from `ctest -V` (a false zero, E §6.0) and never taken
   on trace runs; a record with no reader (`BoundSamplerViews` entries, `MGPSamplerView`, `DrawBuffers[]`,
   `Width/Height/Complete`, `Access`) cannot be verified by any lane. Rule: **the census recipe (file
   sink, per-process log) is in the brief, is run on itest AND retrace, and every emitted field has at least
   one reader or a white-box comparator by the phase's close.**

---

## D. Ranked list for the integrator

**Before P4a closes**

| # | finding | owner file | what | why now |
|---|---|---|---|---|
| 1 | **F-3** framebuffer masks from a stale inlined format | D `Managers.cpp:8678-8723` (answer the four masks from the resource record's `Desc.InternalFormat`, keep `TextureTarget` from the surface) + one scenario (respecify-while-attached, alpha readback both directions) | the only proven arm-divergence on a public-GL sequence; visible on every GLES driver through the alpha-widened mask | wrong picture, no refusal, no lane |
| 2 | **F-1 + F-1b** bit 12 blind to the program; E's epoch narrower than the walk | A `Tracker.h:494` (mix the program identity and `textureParams` into bit 12) + E `DirectGLES.cpp:1868` (mix `GetTextureBindGeneration()` into the record epoch) + a unit case ("program switch alone re-emits the view set") | F-1b is a wrong picture with no refusal on a plain sequence; F-1 is the design's stated invariant for a program-resolved set | the record is write-only today, so the first reader in P3b/P4b would inherit it |
| 3 | **F-4** sampler twins keyed by identity, looked up by content | D `Managers.cpp:12546` / `SlotTables.h` (key `g_backendSamplerObjects` by CSO handle; twin per `create_sampler_state`) + E `DirectGLES.cpp:4840-4850` comment + a white-box case | the record arm's sampler bind is a permanent silent no-op; the P3b/P4b debounce removal retires the pass that hides it | S-1's class, proven |
| 4 | **F-2** bit 14 plain arm blind to the program | A `Tracker.h:360` (mix the lifetime id, as :420 does) | one line; closes A8's "permanent union" for the program-switch half | silent fallback |

**Recorded for later (P4b/P5 brief items, not gates)**

* F-5 (= SD-5): transitive dependency on both sides; F pins `0x7ff` as loud-decline, not as a lane.
* F-6a/b/c: `Access`, `MGPSamplerView::Target`, the `DrawBuffers` default-token narrowing and
  `UploadTarget` Unknown - contract constants and comments before any server reads them.
* F-7: latch-on-acceptance for `set_texture_params` and the framebuffer bound latches (wire n2).
* Records with no reader today: `BoundSamplerViews` entries, `MGPSamplerView` (the twin table exists, no
  field is consumed), `DrawBuffers[]`, `Width/Height/Layers/Samples/Complete`, `Access` - each is a
  verification hole until P4b gives it a reader or a white-box comparator; G4 retain mode (ID-41).
* SD-4 (imageBuffer units absent from `MaxImageUnit`, `SamplerEmit.h:640`) is still open (C / MG_State
  reflection); E's I2 keeps it loud.
* `DirtySurface.def`'s `BindProgramPipelineObject` prose predates c0d; the `d`-tables' "per-context drain
  list" wording (D-D4) vs the process-wide `m_drain`.
* Perf items already recorded: `FramebufferSerial` coarse memo; the two sampler cache scans per pass.

**Not defects, refuted:** `SetReadBuffer`/`SetDrawBuffer`/default-size setters (bump the aggregate);
`glDeleteTextures`/`glDeleteSamplers` (bump the bind generation); the ten singletons' reset lists; the
applier's refusal set for well-formed records (Fatal only, so latch-before-acceptance is moot for the unit
sets); the sampler-view twin table (identity on both sides); the framebuffer-record table across a
make-current (process-unique handles).

---

## E. Runs and artefacts

* `~/w7/retrace-out/fable-audit/` (20 cases, lib `41b79050`), `~/w7/retrace-out/fable-audit-2e03b82a/`
  (20 cases, lib `2e03b82a`), `~/w7/retrace-out/fable-bisect-<rev>/` (bisect), logs `~/w7/fable-bisect*.log`.
* The bisect worktree `~/w7/fable-bisect` was removed after the run (`git worktree remove --force`; its
  three retrace outputs and logs are kept under `~/w7/retrace-out/fable-bisect-*` and
  `~/w7/fable-bisect*.log`). No tree under `~/w7/p4a-*` or `~/w7/pipe` was written to; the retrace
  harness writes only under `--out`.
* Scripts in the shared scratchpad `wsl/fable-audit-*.sh`, `wsl/fable-bisect*.sh` (mine; delete freely).
