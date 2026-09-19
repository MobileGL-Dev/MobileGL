# P4a fable fix round - fablefix-v1 (2026-09-08)

**Branch** `refs/heads/p4a/fablefix`, worktree `~/w7/p4a-fablefix`, base `refs/heads/feat/disaggregated` =
`4678519f` (the complete P4a tree: contract c0..c0g, wire, clientsp v3, clientfb v2, esprytobj v2/v3,
esprytdraw v3 + patches, gates v3). **HEAD `6035c9d7`**, seven commits, **not pushed**, working tree clean.
Integrator grant across package boundaries for this round (ID-45): the files touched are A's `Tracker.h` /
`PipeMutation.h` / `DirtySurface.def` / `scripts/gen_pipe_dirty_surface.py`, B's `TextureObject.cpp` /
`RenderbufferObject.cpp`, D's `Managers.{h,cpp}`, E's `DirectGLES.cpp`, and F's
`MG_IntegrationTest/CMakeLists.txt` (two added lines - one scenario file, one harness TU; section 4). No F
scenario file was edited; the new scenarios live in `Scenarios/P4aSeamAuditScenario.cpp` with their own peek
TU `Harness/P4aSeamPeek.{h,cpp}` (F's `PipeApplyPeek.cpp` is untouched). The census tool
`~/w7/notes/tools/wsl_p4a_refusal_census.sh` is edited in place (not a tree file).

## 0. Verdict in brief

* All four proven seams (F-3, F-1/F-1b, F-2, F-4) and E's SD-4 are fixed, each in its own commit with a
  scenario that was RED on the unfixed tree and is GREEN after (section 2 has the red/green transcript per
  case; every red was taken on `4678519f` + the tests with the eight behavioural files stashed).
* **SD-4 is not a reflection gap - it is F-2.** A probe (2.5) shows `EmitShaderImages` never ran with the
  buffer-image program current: bit 14's plain arm mixed the per-program image-unit COUNTER alone, 0 against
  no program and 0 against the program, so the null -> program transition moved nothing. The 2D/3D kinds
  were rescued by an unrelated frontend generation moving between the bind and the dispatch; a buffer
  texture moves none, which is why only that kind reached E's I2 line. F-2's fix closes it: the five itest
  cases E named no longer carry the line (census 0 lines / 0 tests on the final tree).
* **F-1b's wrong picture reproduces** exactly as the audit described (black quad, 144/144 pixels, Espryt at
  `0x1fff`; Magma and the legacy arms green) once the sequence has a verb between the bind and the switch
  and the first program is in use before the first verb. The F-1 fix alone closes it; E's epoch is NOT
  widened with the bind generation (3.2).
* **F-3 was wrong on BOTH arms**, and the audit's "the legacy arm comes out fresh" held only for a respecify
  that re-mints the driver id. A same-carrier redefinition (`GL_SRGB8` -> `GL_SRGB8_ALPHA8` on a driver that
  widens the first) regenerates mutable storage on the same id, which moves nothing the pre-handle FBO memo
  reads either. Commit 1 fixes the handle arm at the SETTER (the framebuffer aggregate); commit 6 fixes the
  pre-handle arm for textures (an in-place regeneration takes the generation a re-mint takes); commit 7
  compiles that bump under `MOBILEGL_PIPE_PUSH` because G1 (pull library byte-identical to the P4a baseline)
  caught it as 0/0/2/0 - so every arm of a push build has the fix, the pull build keeps the pre-P4a hole and
  the texture cases decline by name there. A renderbuffer re-storage on the pre-handle arm is D-D2's
  documented hole and its case asserts on the handle arm only (2.1, 3.4).
* Gates on `6035c9d7`: G1 0/0/0/0 (`.text` 10806323 -> 10806323), both untouched-region scripts rc 0 (+
  self-test, 8 controls), gen_pipe `--check` up to date, dirty-surface `--check` and `--self-test` rc 0 (27
  controls), include closure rc 0 (4 probes, both invocations), unit 1772 x 3, G2 2863 == 2863, G14 0 removed
  / +275, integration-gpu **1091/1091 at 0x1fff, 0x1ff, 0 and on the pull build** (DirectVulkan lane
  546/546), verify 896/896 with 0 Fatal, retrace 79/79, retrace census 0 on all thirteen needles over 79
  logs, itest default-arm census 0 lines / 0 tests on all ten rows and three stems (section 6).

## 1. Commits (branch `p4a/fablefix`, from `4678519f`)

| # | hash | what |
|---|---|---|
| 1 | `dcc31e95` | F-3 (handle arm): the framebuffer aggregate moves from `TextureObjectBase::PipePublishDescriptor` / `RenderbufferObject::PipePublishDescriptor`; + the scenario file with the three F-3 cases, its peek TU, two CMake lines, two `TrackerAggregates` unit cases |
| 2 | `0d01405c` | F-1 / F-1b: bit 12 mixes `opaqueUnits` (lifetime id x link version x backend state version; `stageLinks` x per-stage backend state under a pipeline) and the params aggregate; `DirtySurface.def`'s UseProgram row gains NEW_SAMPLER_VIEWS (+ mark); `gen_pipe_dirty_surface.py` control 21 reads the mark list; two `TrackerWalk` unit cases; the F-1 scenario |
| 3 | `e4579ee8` | F-2 / SD-4: bit 14's plain arm mixes `shader` (lifetime id x link version) into `programImages`; UseProgram row gains NEW_SHADER_IMAGES (+ mark); one `TrackerWalk` unit case; the F-2/SD-4 scenario |
| 4 | `ccb7b6b3` | F-4: `SamplerImpl::ResolveSamplerCsoTwin` (Managers.{h,cpp}); the record arm and the program pass bind it (DirectGLES.cpp); `SyncToBackend` accepts a CSO twin (null object + handle); the F-4 scenario |
| 5 | `72b4c91a` | docs: the record-field -> setter -> shutter table at Tracker.h's shutter block; the bit-11 note |
| 6 | `419f9941` | F-3 (pre-handle arm): an attachable object's storage redefined IN PLACE bumps `g_attachmentBackendIdGeneration` (texture twin's regeneration of mutable storage, renderbuffer twin's re-allocation); the renderbuffer case asserts on the handle arm only. Found by the F-3 scenarios on the 0x1ff / 0 / pull lanes of gate 1 |
| 7 | `6035c9d7` | commit 6's two bumps compiled under `MOBILEGL_PIPE_PUSH`; the two texture cases decline by name on the pull build. Found by G1 on gate 2 (`SyncMipmapsToBackend` +16, `BackendRenderbufferObject::SyncToBackend` +13 = 0/0/2/0, rc 1) |

Seven single-line `[Type] (Scope): description` messages; no attribution trailer (`git log --format=%B
4678519f..HEAD | grep -ciE 'co-authored|claude|assistant'` = 0). Commits 6 and 7 are what the gates found
in the round's own work and are kept as their own commits rather than folded into 1, so the history says
who found what.

## 2. Seam by seam: root cause, change, scenario, red/green

The RED column is the run on `4678519f` + the new tests with every behavioural change stashed
(`git stash push -- <8 fix files>`, build-push, `ctest -R 'P4aSeamAudit|Tracker(Aggregates|Walk)'`, stash
pop); the GREEN column is the staged commit's own run and the final tree's (every lane, section 6).

### 2.1 F-3 - an attached object's storage redefinition never reached the framebuffer record (commits 1, 6, 7)

**Root cause, handle arm.** `set_framebuffer_state` inlines each attachment's `InternalFormat`,
`TextureTarget`, extent, `Samples` and `Complete` at emission (D-C1, `FramebufferEmit.h`
`MGPipeBuildSurface`). The setters that change those - `TextureObjectBase::SetInternalFormat` /
`SetSamples` / `SetFixedSampleLocations`, `TextureObjectWithOneMipmap::AllocateStorage` /
`TruncateMipmapLevels`, the cube, view and buffer-texture variants, and
`RenderbufferObject::SetInternalFormat` / `AllocateStorage` / `SetSamples` - bump the two TEXTURE aggregates
(or, for a renderbuffer, nothing at all) and never the attachment aggregate bit 11 reads. So
`glTexImage2D(RGB-class); attach; draw; glTexImage2D(RGBA-class); draw` left the framebuffer record on the
first format while the resource record moved, and Espryt's handle arm computed its four cross-object masks
(`Managers.cpp` `IsSnormFallbackSurface` / `IsUnormFallbackSurface` / `IsAlphaWidenedColorSurface` /
`IsIntegerColorSurface`, consumed in `BackendFramebufferObject::SyncToBackend`'s Draw-target block) from
the stale surface.

**Root cause, pre-handle arm** (gate 1 on commit 5: the same three cases red at `0x1ff`, `0` and on the pull
build, alpha 1.0 / red 0.25). The FBO twin's memo re-enters `SyncToBackend` on a frontend framebuffer
version or on `g_attachmentBackendIdGeneration`, which `RecreateBackendTexture` bumps when a respecify
RE-MINTS the driver id. A redefinition that keeps the id - `BackendTextureObject::SyncMipmapsToBackend`'s
`needsRegeneration` path regenerating MUTABLE storage in place, `BackendRenderbufferObject::SyncToBackend`
re-allocating behind the same name - moved neither, so the masks of the storage the object was attached
with stayed. The audit's reading that the legacy arm "re-reads the frontend" was true of the re-minting
case only.

**What changed.**
* Commit 1: `MobileGL/MG_State/GLState/TextureState/TextureObject.cpp` `TextureObjectBase::PipePublishDescriptor()`
  (push-only, the one funnel every storage-defining entry point takes - the comment at `AllocateStorage`
  already said so) now also does `MGP_NOTE_AGGREGATE(FramebufferAttachment)`;
  `MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp` `PipePublishDescriptor()` - the same
  bump, the D-D2 paragraph rewritten (its "widening the shutter would fire the framebuffer emission on an
  unrelated renderbuffer write" was the seam); `MobileGL/MG_Pipe/PipeMutation.h` - the
  `FramebufferAttachment` enumerator's comment names the new bump sites. `Tracker.h` bit 11's shutter is
  unchanged (it already reads the attachment aggregate); its "a renderbuffer respecify is still invisible
  here, and deliberately so" paragraph is replaced in commit 5.
* Commit 6: `MobileGL/MG_Backend/DirectGLES/Managers.cpp` - in `SyncMipmapsToBackend`'s
  `if (needsRegeneration)` block, `if (m_isInitialized && !m_backendStorageImmutable)
  ++FramebufferImpl::g_attachmentBackendIdGeneration;` (a first definition is not a redefinition; a
  redefinition that went through `RecreateBackendTexture` has already bumped); in
  `BackendRenderbufferObject::SyncToBackend` before `Bind()` / the allocation, `if (m_isInitialized) ++...`.
  `Managers.h`: the generation's comment now says it counts "did I change something under an attachment
  point that no frontend version can tell the framebuffer about", of which the re-mint is one case.
* Commit 7: both bumps inside `#if MOBILEGL_PIPE_PUSH` (3.4 says why); the two texture cases gate their
  alpha / DST_ALPHA assertion on `PeekEsprytFramebufferHandleArmIsLive` RETURNING true (it returns true
  exactly where it could look, which for a case that already skipped off Espryt means "a push build") and
  decline by name on the pull build, printing the value they read.
* Unit: `TrackerAggregates.ATextureStorageDefinitionMovesTheFramebufferAggregateToo` (both aggregates move,
  the other four do not) and `...ARenderbufferStorageDefinitionMovesTheFramebufferAggregate` (`ExpectOnly`);
  pull skip twins added, G2 stays 0.

**Why the setter and not the shutter.** The task offered "mix the generation that texture/renderbuffer
respecify moves" into bit 11. For textures that would be the content + params aggregates, which fire on
every `glTexSubImage2D` and every `glTexParameter` - MC 26.3 does 185 uploads a frame - and each fire costs
the two 304-byte record builds and hashes in `EmitFramebufferState`; and for renderbuffers there is no
generation at all to mix. Bumping the aggregate the emitter's bit already reads from the one storage funnel
fires bit 11 exactly when an inlined field can have moved, costs nothing per frame, adds no counter to
either object (G1), and is the literal reading of the rule: the setter of a record field moves a shutter the
emitter reads. The dirty-surface derivation is unchanged by it (the aggregate hop is credited to callers
through `MGPipeNoteAggregate`'s switch; no row names these object setters): `--check` rc 0, `--self-test`
all tripped at every stage.

**Scenario** (`P4aSeamAuditScenario`, DirectGLES only - the masks are Espryt's substitution machinery, the
same reason `SnormAttachment` / `ThreeChannelAttachment` skip on Magma): the format pair is `GL_SRGB8` <->
`GL_SRGB8_ALPHA8`, because on llvmpipe `GL_SRGB8` is only renderable through Espryt's three-channel
widening (`GL_RGB8` is native there and would not carry the mask).

| case | sequence | RED on 4678519f (push 0x1fff) | RED on commit 5 at 0x1ff / 0 / pull | GREEN on 6035c9d7 |
|---|---|---|---|---|
| `ATextureRespecifiedWhileAttachedReachesTheFramebufferRecord` | SRGB8 tex, attach, draw a=0.25, respecify SRGB8_ALPHA8 while attached, draw a=0.25, read alpha | `:328` alpha 1.0 != 0.25 (the stale widened mask kept masking alpha off) | alpha 1.0 (in-place regeneration, memo never re-entered) | 0x1fff / 0x1ff / 0 pass asserting; pull passes declining by name, "alpha read 1" |
| `ARenderbufferRestoragedWhileAttachedReachesTheFramebufferRecord` | the same with `glRenderbufferStorage` | `:389` alpha 1.0 | alpha 1.0 | 0x1fff passes asserting; 0x1ff / 0 / pull pass declining by name (D-D2 hole, pre-P4a code) |
| `ATextureRespecifiedToThreeChannelsWhileAttachedReachesTheFramebufferRecord` | SRGB8_ALPHA8 -> SRGB8 while attached, then GL_DST_ALPHA blend of white must read 1.0 | `:462` red 0.25 (the stale not-widened mask let the draw write 0.25 into the carrier) | red 0.25 | 0x1fff / 0x1ff / 0 pass asserting; pull passes declining, "red read 0.25098" |

A decline is a printed `[ P4aSeamAudit ] ... DECLINED ...` line plus `RecordProperty("p4a_seam_white_box",
"declined")`, the shape `TextureParamsWithoutASamplerViewScenario.cpp` argues for; the public half (the draw
lands, green channel 1.0) is asserted on every arm.

### 2.2 F-1 / F-1b - the view set did not follow the program, and E's epoch could not see it (commit 2)

**Root cause.** `EmitSamplerViews` resolves the set for `GetProgramForDraw()` and memoises the resolution
on (lifetime id, link version, backend state version); bit 12's shutter was `Mix(textureContent,
bindGeneration)` - no program input, no params input. `glUseProgram(P2)` moved nothing, so
`BoundSamplerViews` stayed P1's; and E's `UnitBindingsEpochFromRecords` (the two set serials) then never
rebuilt `g_unitTextureSyncList`, so a texture bound to an EMPTY slot of an already-touched unit under P1 was
never `SyncTextureObjectToBackend`'d for P2 - `ResolveAndBindUnitTextures` found no twin and cleared the
native target (probe, 2.5: `resolve unit=1 name=2 twin=0`).

**What changed** (`MobileGL/MG_Impl/Pipe/Tracker.h`): a new walk local `opaqueUnits` = the emitter's memo
key - `Mix(shader, program->GetBackendStateVersion())` on the plain arm, `Mix(stageLinks, stageOpaque)`
under a pipeline where `stageOpaque` mixes each stage program's backend state version (the sampler-unit
assignments reach the composite through the uniform mirror) - and bit 12 = `Mix(Mix(Mix(textureContent,
textureParams), bindGeneration), opaqueUnits)`. The params aggregate is the second F-1 input:
`SamplesAsIncompleteTexture` reads the effective sampler's filters, so a `glTexParameteri(MIN_FILTER)` that
completes a texture fired bit 13 and left the view entry null. `DirtySurface.def`: `X(UseProgram,
NEW_SHADER|NEW_SAMPLER_VIEWS)` with the matching undecided mark (the same taint as bit 6's -
`DestroyProgramSlot` writes a non-`m_` member); `gen_pipe_dirty_surface.py` control 21 now reads the mark
list instead of counting two, so a row that gains a bit is counted (`marked_pairs` must each appear as an
"UNDECIDED answer <bit> for <mutator>" line and the count must match). Unit:
`TrackerWalk.AProgramSwitchAloneFiresTheSamplerViewBit`,
`TrackerWalk.ATextureParameterAloneFiresTheSamplerViewBit`.

**Scenario** `ATextureBoundToAnEmptySlotUnderOneProgramIsSampledByTheNext` (both backends): P1 samples
unit 0 (red), unit 1 is touched through its 3D slot; P1 in use BEFORE the first verb; draw; bind green to
unit 1's EMPTY 2D slot; draw with P1 again (the bind's own bit-12 fire resolves under P1 and suppresses);
switch to P2 (samples unit 1); draw; read back. RED on `4678519f`: `:551` "region should be all green, but
144 of 144 pixels are not; first offender is black" on DirectGLES (Magma green). GREEN after commit 2 on
every lane. Two earlier drafts of this case were green before the fix and are recorded because they say
what the seam needs: with the switch inside the same verb gap as the bind, the bind's fire resolves under
P2; with no program in use at the first verb, the first set is `[null, null]` and P1's later `[red, null]`
is a different set that moves the serial (probe: `views program=0 ... emit=1` then `views program=1 ...
emit=1`).

### 2.3 F-2 / SD-4 - the image window did not follow the program (commit 3)

**Root cause.** Bit 14's plain arm mixed `program->GetImageUnitVersion()` alone - a counter that only
`SetUniformSamplerOrImageUnitIndex` moves for an image uniform, so it is 0 for every program that takes its
image units from `layout(binding)` and 0 against no program. A `glUseProgram` between two such programs, or
from no program to one, moved nothing, and `set_shader_images` kept the previous window. That is also E's
SD-4: the probe shows `EmitShaderImages` running only with `program=0` for `LoadsTextureBuffer` (no `OPQ`
line at all - the resolver was never asked about the buffer-image program) while `LoadsTexture2D` got one
`program=1 maxImageUnit=0 count=1` emission - rescued by a frontend generation the image-bindable re-mint of
a 2D texture moves between the bind and the dispatch and a buffer texture, which has no storage of its own,
does not. The reflection is fine (`isImage=1 unit=0 type=0x9063` for the image kinds that were asked).

**What changed** (`Tracker.h`): `programImages = Mix(shader, program->GetImageUnitVersion())` on the plain
arm - the identity the pipeline arm already mixed as `stageLinks`; `ImageEmit.h`'s invariant paragraph is
untouched (the shutter still reads only frontend counters). `DirtySurface.def`: UseProgram row + mark gain
NEW_SHADER_IMAGES. Unit: `TrackerWalk.AProgramSwitchBetweenEqualImageUnitCountersFiresTheShaderImageBit`.

**Scenario** `AProgramSwitchWithEqualImageUnitCountersMovesTheImageWindow` (both backends; white-box half
declined by name where Espryt's sampler family is not on its handle arm): two BUFFER-image compute programs,
P1 naming unit 0, P2 naming units 0 and 1, both images bound before any dispatch; after P1's dispatch the
applier's window must be Count 1, after P2's Count 2; the public half checks both stores landed (7 and 9).
RED on `4678519f`: `:607` Count 0 != 1 and `:620` Count 0 != 2. GREEN after commit 3. The five itest cases E
named (`ImageTargetKind.Loads/StoresTextureBuffer`, `BufferTexture.AnImageStoreIntoABufferTextureIsVisibleToTheCpu`,
the two `NonCoreImageFormat` buffer cases) no longer carry the I2 line: the default-arm census on the final
tree reads 0 lines / 0 tests for every row (section 6), with all eleven `NonCoreImageFormat` /
`ImageTargetKind` names present in the 545-name list it censused.

### 2.4 F-4 - the record arm's sampler bind could never hit (commit 4)

**Root cause.** `BindCurrentUnitSamplers`' record arm did `g_backendSamplerObjects.FindByHandle(
st.BoundSamplerStates[unit])`: a CONTENT-addressed handle (`MGPipeSlots().Allocate(SamplerCso)` in C's
cache) asked of a table whose every twin was minted off a `SamplerObject`'s lifetime id. One allocator, two
disjoint slot families - a miss on every draw, read by E's S4 comment as a within-draw ordering fact. The
picture stayed right because the pre-handle program pass bound the identity twin (synced from the same
record since S-1); the record arm's only live effect was the unbind of null and out-of-window units. The
audit's white-box reading confirmed it on this tree: `unit 0 driver sampler 1, bind_sampler_states handle
{6, 0} inside window yes, CSO twin 0, identity twin 1`.

**What changed.**
* `Managers.h` / `Managers.cpp` (`SamplerImpl`): `BackendSamplerObject* ResolveSamplerCsoTwin(MGPipeHandle
  cso)` - record first (`PipeSamplerCsoRecordForHandle`; no record is a loud named refusal in the sampler
  family's census shape), then `g_backendSamplerObjects.GetOrCreateByHandle(cso)` (the same slot table: one
  allocator serves both handle families of the kind, so a content slot and an identity slot never coincide;
  a generation behind the live entry is refused loudly), then `SyncToBackend(nullptr, cso)`, serial-gated.
  `SyncToBackend`'s null-object prologue admits a null object beside a live handle (push-only text; the pull
  build's three lines are unchanged token for token), the two log lines name the handle instead of a GL
  name, the legacy body returns on a null object it can never receive.
* `DirectGLES.cpp`: the record arm binds `ResolveSamplerCsoTwin(handle)` (S4 retired); the program pass's
  sampler override binds the SAME CSO twin for a unit inside the received window and takes the pre-handle
  path (identity twin, handle carried to `SyncToBackend`) otherwise, so the two arms cannot ping-pong a unit
  between two driver samplers.
* Death: a CSO twin lives until its slot is recycled (the client's LRU eviction frees the slot and the next
  handout arrives with a moved generation, which `GetOrCreate(handle)` answers by resetting the twin) -
  bounded by the cache's capacity, never by draw count; there is no `delete_sampler_state` hook in the ops
  table to retire it earlier, and none is added (recorded in the header comment).

**Scenario** `ABoundSamplerObjectIsDrivenThroughItsCsoTwinOnTheHandleArm` (both backends): a
`CLAMP_TO_BORDER` white-border sampler object bound over a `REPEAT` red texture, sampled at (1.5, 1.5) - the
public half (white wins) passes on every arm; the white-box half (Espryt handle arm only, via
`P4aSeamPeek`) requires the unit's driver sampler == the twin at the CSO handle, and no identity twin for
the object. RED on `4678519f`: `:706` CSO twin 0, `:710` driver sampler != CSO twin, `:713` identity twin 1.
GREEN after commit 4.

### 2.5 The two probes (temporary `MGLOG_E` lines, never committed)

* SD-4 (`SamplerEmit.h` `For()` + `ImageEmit.h`): `LoadsTextureBuffer` -> 19 x `IMG program=0 link=0
  maxImageUnit=-1 ... count=0`, no `OPQ` line; `LoadsTexture2D` -> `OPQ program=1 loc=0 unit=0 isImage=1
  isTexture=0 isOpaque=1 type=0x9063` then `IMG program=1 link=1 maxImageUnit=0 programWindow=1 count=1`.
* F-1b (`DirectGLES.cpp` sync list / resolve + `SamplerEmit.h` emitters), on the draft that was green:
  `views program=0 count=2 ... emit=1 u0={0,0} u1={0,0}` at the clear, then at the P1 draw after the bind
  `views program=1 count=2 ... emit=1 u0={15,0} u1={0,0}` -> `syncList ... epoch=3042.../1476...` rebuilt ->
  `syncTex name=2`. With P1 in use before the clear the first set is already `u0={15,0}`, the bind's
  re-resolution is suppressed, and the P2 draw hits the list memo: black.

## 3. Judgement calls

### 3.1 F-3 at the setter (2.1, "why the setter and not the shutter").

### 3.2 E's record epoch is NOT widened with `GetTextureBindGeneration()`

The audit's F-1b fix offered two shapes: mix the bind generation into E's epoch, or make the serial move on
the F-1 fix. The first would re-run the program-independent list rebuild on every redundant re-bind (26.2
re-binds a sampler around every texture-unit switch), which is exactly the walk E's e2 removed. With F-1
fixed the two serials move whenever the PROGRAM-RESOLVED set moves - a program switch, a relink, a
sampler-uniform re-assignment, a bind onto a slot the program samples, a completeness flip - and that is
the set of events after which a not-yet-synced texture can be SAMPLED. A texture bound to a slot no program
samples stays unsynced until something samples it, at which point the set moves; the read-attachment path,
the copy-image endpoints and the image sweep have their own sync lists. So the record epoch is narrower
than the walk only for textures nothing can read, and the scenario that pins the audit's sequence is green
through the F-1 fix alone. Recorded, not changed.

### 3.3 The scenario file ships its own peek TU (a second CMake line)

Two of the four seams are correct pictures over a permanent silent fallback (F-2/SD-4: the server's
window/high-water union; F-4: the program pass) and have no public-GL observable, so their cases are
white-box. `PipeApplyPeek.cpp` is F's (gates v3) and this round may not edit it; a scenario TU cannot include
`Managers.h` beside the GL prototype headers (PipeSlotPeek.h's rule). So `Harness/P4aSeamPeek.{h,cpp}` is a
new TU of its own - the task said one added CMake line for the scenario; this is two, one per new source
file, in the same list. Its four entry points (`PeekEsprytSamplerHandleArmIsLive`,
`PeekEsprytFramebufferHandleArmIsLive`, `PeekPipeShaderImageWindow`, `PeekEsprytUnitSampler`) all return
false where they cannot look (pull build, Android, a backend that is not Espryt) and a false teaches the
caller nothing.

### 3.4 What was NOT changed, and why

* **The pull build's in-place-regeneration hole (F-3's pre-handle half on pre-P4a code).** Commit 6's two
  bumps are Espryt code the pull build shares, and G1 caught them as two resized functions (+32 bytes
  `.text`). The charter's G1 is 0/0/0/0 and its meaning is that P4a lands nothing in the pull library, so
  commit 7 brackets them with `#if MOBILEGL_PIPE_PUSH`. The pull build therefore still answers alpha 1.0 /
  red 0.25 to the two texture cases (they say so by name, every run). The right vehicle is a separate
  `[Fix] (Espryt)` commit on dev of exactly those two `if` statements (no P4a dependency; the three F-3
  cases are its regression test once the `#if` is dropped and `p4a-before-libMobileGL.so` re-baselined) -
  an integrator decision, not this round's.
* **A renderbuffer re-storaged while attached, pre-handle arm.** On the 0x1ff / 0 / pull lanes the
  renderbuffer twin is only reached from inside the FBO walk the memo skips, the frontend setters bump no
  version (D-D2) and no framebuffer version sees them, so the commit-6 bump inside `SyncToBackend` is never
  reached before the memo decides; the resource record is what closes it, and the case asserts on the
  handle arm only. Pre-P4a code, D-D2's documented hole; fixing it on the legacy arm means teaching the FBO
  memo a renderbuffer version that does not exist today. Not this round's.
* `ImageEmit.h`'s invariant-2 paragraph ("all three FRONTEND counters"): still true - `programImages` is a
  frontend identity, not a server epoch - so the sentence stands.
* `DirtySurface.def`'s `BindProgramPipelineObject` prose predates c0d (recorded by the audit): prose answer,
  no derivation checks it, not this round's.
* No `delete_sampler_state` ops hook for the CSO twin (2.4).
* The two `.Handles`-style census shapes in `Managers.cpp` are not reworded (the tool is fixed instead,
  section 5).

## 4. Deviations from the charter

* Seven commits rather than five: 6 and 7 are the gates' own findings on the round's work (2.1).
* Two CMake lines rather than one (3.3).
* F-3 fixed at `PipePublishDescriptor` rather than in `Tracker.h`'s shutter (3.1); `Tracker.h` gains the
  comment and the table only.
* `scripts/gen_pipe_dirty_surface.py` control 21 rewritten to read the undecided-mark list (the hard-coded
  "two marks" made the self-test red the moment UseProgram gained a second bit); `DirtySurface.def`'s prose
  for the list updated to match ("Control 21 is what proves every marked row still needs its mark").
* The two F-3 texture cases assert on push builds only (3.4, first bullet); the renderbuffer case on the
  handle arm only.

## 5. The census tool

`~/w7/notes/tools/wsl_p4a_refusal_census.sh` gains the two built-in-sampler-CSO shapes whose "has no
applier record" straddles a string-literal line break in `Managers.cpp` (`ResolvePushedBuiltinSampler`,
"declining the built-in sampler push"; the border-colour block of `SyncTextureParamsToBackend`, "so its
border colour cannot be pushed") - matched on the half of the LOG line unique to each - plus a stem line
for the pair ("built-in sampler CSO (two shapes)") and a "TEN SHAPES" comment. `Managers.cpp`'s D-N regions
are untouched (the two sentences are not in them either, but the tool fix was preferred as asked). `bash -n`
clean; the default-arm run on the final tree is in section 6.

## 6. Gates on `6035c9d7`

Run by one detached script after the commit (`fablefix-gate.sh`; builds `-j 8`, `CCACHE_BASEDIR=/home/swung/w7`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`). The same script ran on `72b4c91a` (gate 1: the F-3 reds on
the legacy lanes) and on `419f9941` (gate 2: G1 0/0/2/0, everything else identical to the numbers below).

| gate | result |
|---|---|
| builds | build-linux / build-push / build-verify rc 0, 0 warnings each |
| G1 `symbol_report --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` vs `p4a-before-libMobileGL.so` | rc 0: `.text` 10806323 -> 10806323 (+0), 27811 -> 27811 symbols: **0 added, 0 removed, 0 resized, 0 renamed** |
| `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, byte-identical |
| `scripts/p4a_untouched_regions.sh 37da3c3a HEAD` | rc 0, depth-stencil-sampling / format-caveat regions byte-identical |
| `scripts/p4a_untouched_regions.sh --self-test` | rc 0, 8 negative controls all tripped and named |
| `scripts/gen_pipe.py --check` | up to date |
| `scripts/gen_pipe_dirty_surface.py --check` | rc 0 |
| `scripts/gen_pipe_dirty_surface.py --self-test` | rc 0, 27 negative controls all tripped, positive controls OK (control 21 now "every P4a undecided mark - 4 of them - is still needed") |
| `scripts/check_include_closure.py --compiler clang++` | rc 0, 4 probes, 0 skipped, 0 problems |
| `check_include_closure.py --mode text --self-test --require-all --expect-probes 4` | rc 0 |
| unit `-L unit` | build-linux 1772/1772, build-push 1772/1772, build-verify 1772/1772 |
| G2 pull-vs-push ctest names | 0 diff lines (2863 == 2863) |
| G14 names vs `p4a-before-ctest-names.txt` | 0 removed, 275 added |
| integration-gpu, build-push, `MOBILEGL_PIPE_PUSH` default (0x1fff) | 1091/1091 |
| integration-gpu, build-push, 0x1ff | 1091/1091 |
| integration-gpu, build-push, 0 | 1091/1091 |
| DirectVulkan lane (`-R ^DirectVulkan`, build-push) | 546/546 |
| integration-gpu, build-linux (pull) | 1091/1091 |
| integration-verify, build-verify, `-j 4` | 896/896, `Fatal{` lines 0 |
| retrace (`retrace_gate.py --lib build-push/libMobileGL.so -j 4`, fixtures re-copied, 0 LFS pointers) | rc 0, **79 / 79**, failed [] |
| retrace refusal census over 79 `mobilegl.log`s | 0 for every needle: the ten shapes ("storage and uploads cannot be driven", "built-in sampler cannot be pushed", "parameters cannot be pushed", "read buffer cannot be pushed", "it is not configured here", "no shader-CSO applier record", "sampler ... has no applier record", "storage cannot be allocated", "declining the built-in sampler push", "border colour cannot be pushed"), "does not describe the binding it names", `Fatal{`, "cannot be adopted on the handle arm"; the deduped `MGPipe:` ERROR/FATAL list is empty |
| itest default-arm census (`wsl_p4a_refusal_census.sh default`, 545 names, 525 logs) | 0 lines / 0 tests on all ten rows; stems "no applier record on the handle arm" 0, "no shader-CSO applier record" 0, "built-in sampler CSO (two shapes)" 0; FATAL lines 1050 = the 2 ambient EGL loads x 525 logs, none real |
| the new scenarios (12 cases: 6 DirectGLES + 6 DirectVulkan) at 0x1fff | 12/12; the three F-3 cases 3/3 at 0x1fff, 0x1ff, 0 and on pull (declines by name as in 2.1) |

Baseline for the integration lanes was 1079/1079; the twelve added are the six new cases on both backends.

## 7. The shutter table

Carried in `Tracker.h` at the shutter block (commit 5, before `// ---- the object-class bits 9..17 ----`)
and reproduced here for the P4b brief:

```
THE RECORD-FIELD -> SETTER -> SHUTTER TABLE FOR THE SEVEN P4a BITS.

THE RULE (P4a fable seam audit, section C.1): every field of every emitted
record names the frontend setter that changes it, and that setter moves a
counter the emitting bit's shutter reads - or the emission is unconditional at
the setter (the resource_* family, set_texture_params). A record field whose
setter moves no shutter input is a stale record with nothing to refuse: c0d
(bit 13 without the bind generation), SD-0 (an image re-bind), F-1 (the
program behind the view set), F-2 (the program behind the image window) and
F-3 (an attached object's storage) were all this one class. DirtySurface.def
cannot catch it - it maps MUTATORS to bits and cannot see that a DERIVED field
depends on a mutator whose row is another family's - so the table lives here,
beside the shutters, and a row is added whenever a record gains a field.

bit 6  create/bind_shader_state, set_draw/dispatch_program (ProgramEmit.h)
       fields: Cso, StageMask, GlobalUboSize, the artefact blob refs, the two
               bound handles
       setters: glUseProgram (m_currentProgram), glLinkProgram (link version),
               glBindProgramPipeline / glUseProgramStages (pipeline name +
               per-stage {lifetime id, link version})
       shutter: lifetime id x link version, or stageLinks under a pipeline
bit 7  the program's bindings (image units, block bindings, uniform write set)
       setters: glUniform1i on an opaque uniform (backend state version, image
               unit version), glUniformBlockBinding / glShaderStorageBlockBinding
               (block binding version), any glUniform* (uniform write set)
       shutter: the four per-program counters, x stageLinks under a pipeline
bit 8  set_global_constants: ShaderCso, Version, the default-block image
       setters: any glUniform* on the default block (UBO content version),
               glUseProgram (lifetime id)
       shutter: lifetime id x UBO content version, or stageState
bit 11 set_framebuffer_state: Fbo, Color[8]/Depth/Stencil/ReadSurface
       (Res, Kind, InternalFormat, TextureTarget, Layered, Level, Layer,
       UploadTarget), DrawBuffers[8], Width/Height/Layers/Samples/
       FixedSampleLocations, IsDefault, Complete, Target
       setters: glFramebufferTexture*/glFramebufferRenderbuffer, glDrawBuffer(s),
               glReadBuffer, glFramebufferParameteri (the attachment
               aggregate); glBindFramebuffer (the two binding slot versions);
               AND a storage redefinition of an ATTACHED texture or
               renderbuffer - glTexImage*/glTexStorage*/glTexBuffer/
               glTextureView/glRenderbufferStorage* - because InternalFormat,
               TextureTarget, the extent, Samples and Complete are INLINED at
               emission (D-C1): those bump the attachment aggregate from the
               object's PipePublishDescriptor (F-3)
       shutter: attachment aggregate x draw bind version x read bind version
bit 12 set_sampler_views: per unit {View, Texture}
       setters: glBindTexture / glActiveTexture (bind generation), a texture's
               or a sampler object's parameters (SamplesAsIncompleteTexture -
               the params aggregate), an upload that defines a level (content
               aggregate), the default texture's image appearing (bind
               generation, TextureObject.cpp); AND the program in use -
               glUseProgram, a relink, glUniform1i on a sampler uniform (which
               unit a uniform's TYPE resolves) - F-1
       shutter: content x params x bind generation x opaqueUnits
bit 13 bind_sampler_states: per unit the sampler CSO handle
       setters: glBindSampler (bind generation, c0d), glSamplerParameter* /
               glTexParameter* (params aggregate + sampling resolution),
               glDeleteSamplers (bind generation)
       shutter: params x sampling resolution x bind generation
bit 14 set_shader_images: per unit {Res, InternalFormat, Layer, Level,
       Layered, Access} over the program's image-unit window
       setters: glBindImageTexture (bind generation, SD-0), a texture's
               content/params, glUniform1i on an image uniform (image unit
               version); AND the program in use - glUseProgram, a relink -
               F-2
       shutter: content x params x bind generation x programImages
               (lifetime id x link version x image unit version)
```

## 8. Logs

Every `~/w7/fablefix-*` log, `~/w7/fablefix-logs/`, `~/w7/fablefix-stage/` and `~/w7/fablefix-census/` are
deleted at the end of the round; `~/w7/retrace-out/p4a-fablefix` keeps only `summary.txt` / `summary.json`.
The worktree `~/w7/p4a-fablefix` stays (branch `p4a/fablefix`, HEAD `6035c9d7`, clean; fixtures are the
copies from `~/w7/pipe` under `assume-unchanged`). The shared scratchpad `wsl` directory is untouched except
for this round's own `fablefix-*` scripts.
