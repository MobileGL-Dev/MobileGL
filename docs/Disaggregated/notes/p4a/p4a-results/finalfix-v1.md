# P4a final-review fix round - finalfix-v1 (2026-09-09)

**Branch** `refs/heads/p4a/finalfix`, worktree `~/w7/p4a-finalfix`, base `refs/heads/feat/disaggregated` =
`a38bdab4` (= the gated P4a code head `6035c9d7` + the tools-only harness commit). **HEAD `8c458cd5`**, five
commits, **not pushed**, working tree clean. Charter: `final-review-v1.md` C-1 / C-2 / M-A / the minors and
INTEGRATOR-DECISIONS ID-52 (B is past two reworks, so the round is fable's with an integrator grant across
B's files, the contract, the applier, Espryt and one new scenario file). Every fix has a scenario that was
RED on the unfixed tree and is GREEN with the fix, taken on this tree (section 2); the full gate on the head
is in section 4.

## 0. Verdict in brief

* **C-1 fixed.** The storage entry points state WHICH storage a respecify replaces (one level, a chain cut,
  the whole resource); the emitter builds wire's `MGPRespecifiedLevel` with the drain's own packed target
  and passes it; a per-level respecify is never deduped on the descriptor; the applier drops a named level
  whether or not the descriptor moved (a refinement of wire's W11 clause, section 3). The review's two
  llvmpipe scenarios read a 100 % black level 0 before and red after; the three controls are red on every
  arm.
* **C-2 fixed.** The six death helpers forward to the emitters between the wire delete and the slot free
  (ID-8's order): the texture emitter drops the dead handle's drain entries, its raw pointer, the built-in
  sampler's cache reference and its latches; the renderbuffer, framebuffer, sampler-view and shader-CSO
  memos are retired the same way; `ResolveTexture` refuses a dead slot loudly on `IsLive`. `glTexImage2D;
  glDeleteTextures; draw` was SIGABRT (`pure virtual method called`) / SegFault under `MALLOC_PERTURB_` and
  is green on both backends, for every P4a kind and for a recycled slot.
* **M-A fixed.** `kMGPipeBindSampler` is produced by the sampler-view resolution and
  `kMGPipeBindShaderImage` by `glBindImageTexture`'s state setter (so the hint precedes the first sync) and
  by the image walk; both through a new contract door because neither caller may include `TextureEmit.h`.
  `ImageBindableHint` reaches the applier as ID-18 M4's metadata respecify (pinned end to end, kept
  upload included), and ROADMAP open question 2 is measurable: **PipeStats `tex-remint-pulls`, `trp=` on
  the summary line** - one per texture Espryt had already allocated and had to re-mint image-bindable.
* **Minors:** m-1/F-7 done (`MGPipeApplySetTextureParams` returns `Bool`, the emitter latches on
  acceptance), m-2 (Config.h's fourth row), m-3 (the "ORed" comment). Going loud on m-1 surfaced one real
  residual in the retrace census - the context's default textures are born before the backend registers
  its consumer, so their first `glTexParameter*` was refused and silently latched away - fixed by healing
  the record from the params path (commit 5). m-4, m-5, m-6, m-7, m-8 declared (section 5).
* Gate on `8c458cd5` (section 4): G1 0/0/0/0, G5 rc 0 x 4, purity rc 0 x 5, unit **1785 x 3**, G2
  2902 == 2902, G14 0 removed / +314, integration-gpu **1117/1117 at 0x1fff, 0x1ff, 0, 0x9ff, 0x5ff, pull
  and `ESPRYT_DISABLE_INVALIDATE_FLUSH=1`** (DirectVulkan 559/559), verify **920/920 zero Fatal**, retrace
  **79/79**, retrace refusal census **0 on 16 needles**, itest eight-family census **0 lines / 0 tests on
  13 rows**, every negative control tripped.

## 1. Commits (branch `p4a/finalfix`, from `a38bdab4`)

| # | hash | what |
|---|---|---|
| 1 | `173f1dd2` | C-1: the level scope of a texture respecify (contract hook + enum, PipeFill forward, TextureObject{,2DCube} call sites, the emitter's three-scope body, the applier's precedence, W11's test clause, 3 unit cases, the scenario file + peek TU + 5 itest cases) |
| 2 | `a690032f` | C-2: the death path forwards to every emitter; loud dead-slot refusal; gen-stamped mask producers; 6 unit cases (texture x4, framebuffer, program), 6 itest cases + the `MALLOC_PERTURB_=165` lanes |
| 3 | `9f60aadc` | M-A: the two bind-bit producers through `MGPipeNoteTextureBoundAs` / `MGPipeNoteTextureImageBound`; `tex-remint-pulls`; 2 unit cases (sampler, image), 1 itest case, 2 peeks |
| 4 | `c2c6a655` | m-1/F-7 (`Bool` from set_texture_params, latch on acceptance, `RefusedParamCount`), m-2 (Config.h), m-3 (MGPipeTypes.h); 1 unit case |
| 5 | `8c458cd5` | the retrace census's residual once m-1 went loud: set_texture_params heals a record the applier never received (create, storage if any, params); the F-7 case reshaped around a refusal on the merits; 1 unit case |

Five single-line `[Type] (Scope): description` messages, no attribution trailer. Each commit was compiled
and run with ONLY its own tests (the shared test files were trimmed per commit before `git add` and the
trimmed suites run: TextureEmit 41/45/45/46/47 on both builds, the itest lane 10/24/26/26/26), so every
intermediate commit is green on its own.

## 2. Finding by finding: root cause, change, scenario, red/green

### 2.1 C-1 - the client never passed the level a respecify redefines

**Root cause.** `TextureEmit.h`'s `ApplyRespecify` called `MGPipeApplyResourceRespecify(desc, nullptr)` at
every texture respecify; the applier's whole-resource arm then cleared the record's entire pending-upload
set. `AllocateStorage` is per `(uploadTarget, level)` and the descriptor carries only the base extent and
the level count, so a level the applier had ACCEPTED at one verb (client flag already clear, D-D5 step 1)
and that the next verb's `glTexImage2D(level 1)` or `glGenerateMipmap` grow defined AROUND was owed by
nobody. wire-v3 §5 item 5 required B to pass the key; no review checked it.

**What changed.**
* `MG_Pipe/PipeMutation.h:334-352` - `enum class MGPipeTextureRespecifyScope { WholeResource, OneLevel,
  LevelsFrom }` and `MGPipeEmitTextureResourceRespecify(ITextureObject&, scope, Uint32 uploadTarget,
  Uint32 level)`; every caller states its scope, "whole resource" is said, never defaulted.
* `MG_Impl/Pipe/PipeFill.cpp:1258` - the forward.
* `MG_State/GLState/TextureState/TextureObject.h:239-240`, `TextureObject.cpp:87-104` -
  `PipePublishLevelDescriptor(uploadTarget, level)` / `PipePublishTruncatedDescriptor(uploadTarget,
  levelCount)` beside the whole-resource `PipePublishDescriptor()`; `AllocateStorage` (`:517`) and
  `TruncateMipmapLevels` (`:527`) call them; the cube twins at `TextureObject2DCube.cpp:39/:47` (the face
  rides in `uploadTarget`). The view forwards to its owner's `AllocateStorage`, so a per-level definition
  through a view keys the OWNER's level; the backend's own generate-mipmap grow
  (`DirectGLES.cpp:8074`) goes through the same `AllocateStorage` and is level-scoped for free.
* `MG_Impl/Pipe/TextureEmit.h:626-720` - `EmitResourceRespecify(texture, scope, uploadTarget, level)`:
  `OneLevel` builds one key `{MGPipePackSubDataTarget(resourceTarget, uploadTarget), level}` - the SAME
  packed `MGPSubData::Target` the drain writes at the sub-data emission - and is NEVER deduped on the
  descriptor (a non-base level redefined at a new size moves no descriptor field, and the applier's box
  against the old level would be uploaded past the end of the new one); `LevelsFrom` spells the cut as one
  call per removed level `[level, LastDesc.Levels)` (the applier's key is one level; a cut at 0 is the
  whole resource; a cut that removes nothing the applier could hold is deduped like the whole-resource
  form); `WholeResource` keeps the byte dedupe. `RespecifyOnce` (`:1124`) carries the refusal self-heal;
  `ApplyRespecify(desc, level)` (`:1107`) takes the key; `RepublishMask` passes `nullptr` deliberately
  (wire-v3 §5 item 6) and says so; the renderbuffer twin passes `nullptr` (no levels).
* `MG_Pipe/PipeApply.cpp:1691-1703` - precedence: a **named level is dropped whether or not the
  descriptor moved**; a null level with identical storage fields is the metadata update that drops
  nothing; a null level with moved fields clears all. `PipeApply.h:787` and `MGPipeTypes.h:1103-1108`
  state the rule; the W11 test clause is rewritten to the same rule (section 3).

**Scenario and proof** (`MG_IntegrationTest/Scenarios/P4aFinalFixScenario.cpp`, DirectGLES-asserted,
skipped by name on DirectVulkan where no P4a consumer exists; `MG_Test/Pipe/TextureEmitTest.cpp`).

| case | before (a38bdab4 + the tests) | after (173f1dd2) |
|---|---|---|
| `PerLevelDefinitionAcrossAnUnrelatedDraw` (`L0(red); draw(other); L1; draw(T)`) | **black, 11625/11625 px** | red |
| `GenerateMipmapAcrossAnUnrelatedDraw` (`L0(red); draw(other); glGenerateMipmap; draw(T)`) | **black, 11625/11625 px** | red |
| controls `ConsecutiveDefinitionsNoVerbBetween`, `LevelZeroConsumedBeforeLevelOne`, `GenerateMipmapImmediately` | red | red |
| unit `ALevelDefinedAfterAnEmittedButUnconsumedUploadKeepsThatUpload` (`:1300`, the review's probe through the real `AllocateStorage`) | **Failed** (level 0 not pending, flag CLEAR) | Passed: both levels stand after the next drain |
| unit `AChainTruncationKeepsTheSurvivingLevelsPendingUploads` (`:1346`) | **Failed** (the cut took level 0's upload) | Passed: level 0 kept, level 2 gone |
| unit `ARedefinitionOfANonBaseLevelAtANewSizeDropsOnlyThatLevelsPendingUpload` (`:1377`) | **Failed** (deduped: the 8x8 box survived onto a 4x4 level) | Passed: level 1 gone, level 0 kept, serial moved |

The five itest cases run at 0x1fff, 0x1ff, 0 and on the pull build (26/26 each arm at the head).

### 2.2 C-2 - a dead handle resolved to freed memory in the drain

**Root cause.** `ResolveTexture` guarded with `GenOfSlot == handle.Gen`, but the allocator moves a slot's
generation only at the NEXT hand-out; between a death and a recycle the dead handle compared equal, the
emitter still held the freed `ITextureObject*`, and nothing removed the slot's drain entries - the six
death helpers freed the slot and told no emitter. `EmitOneLevel` then called virtual `GetStorageType()`
on the freed object from the next validate point.

**What changed.**
* `MG_Impl/Pipe/PipeFill.cpp:1405/1429/1467/1493/1525` - the sampler-view, texture, renderbuffer,
  framebuffer and shader-CSO helpers forward to their emitter AFTER the wire delete and BEFORE
  `NotifyAndFree` (ID-8: delete, notice, free). The sampler CSO forwards to nothing, stated: content-
  addressed, no per-object entry (ID-17). Unconditional in a push build, like the mints (the entries exist
  whether or not the family bit is set; the 0x1ff / 0 arms run the same path).
* `MG_Impl/Pipe/TextureEmit.h:457-513` - `ResolveTexture` refuses on `!MGPipeSlots().IsLive(...)`,
  counted (`DeadResolveCount()`, `:974`) and `MGLOG_E_ONCE` - reaching it means a death path skipped the
  emitter; `NoteTextureDied(handle)` (`:487`) drops the handle's `m_drain` entries and `DrainKeys`,
  releases the built-in sampler's cache reference (at the death now, no longer first at the recycle),
  resets the entry; `NoteRenderbufferDied` (`:509`); `MaskOf` refuses a stale generation; the two mask
  producers stamp the generation they write under (`:423-425`, `:434-435`, `:545-546`, `:564-565`) so a
  texture born while the family bit was clear has an entry a gen-keyed reader can see. `RetireIfRecycled`
  stays as the belt.
* `MG_Impl/Pipe/FramebufferEmit.h:400-410` - `NoteFramebufferDied` (the per-object Named latch) and
  `NamedRecordIsLatched` for the case. `SamplerEmit.h` / `ProgramEmit.h`'s existing
  `NoteRecordDestroyed` ("no production caller today") now have one; comments say so.
* `MG_IntegrationTest/CMakeLists.txt` - `DirectGLES.MallocPerturb.` / `DirectVulkan.MallocPerturb.`
  registrations of the dirty-then-delete case with `MALLOC_PERTURB_=165` (the review's §8 item 1).

**Scenario and proof** (both backends, every arm).

| case | before | after (a690032f) |
|---|---|---|
| `ADirtyTextureDeletedBeforeAnyVerbIsWalkedByTheNextDrain` (8 x `glTexImage2D; glDeleteTextures; draw(other)`) | **SIGABRT `pure virtual method called`** (DirectGLES); **SegFault** under `MALLOC_PERTURB_=165` | white 8/8 on both backends, both lanes |
| `ATextureRecycledOntoTheDeadSlotDoesNotInheritItsDrainEntry` (ABA) | pass (the recycle's Gen already refused it) | pass |
| `ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact`, `ASamplerObjectDeletedWhileBoundLeavesTheNextDrawIntact`, `AProgramDeletedWhileInUseLeavesTheNextDrawIntact`, `AFramebufferDeletedAfterADsaClearLeavesItsAttachmentIntact` | pass | pass |
| unit `ADeadTexturesHandleResolvesToNothingAndLeavesTheDrainList` (`:1410`, the review's probe) | **SegFault** (the freed pointer resolved) | Passed: null, drain list 0, `DeadResolveCount` 0 |
| unit `ATextureRecycledOntoADeadSlotDoesNotInheritTheDrainEntry` (`:1458`) | **Failed** (drain list still 1 after the death) | Passed: 0 after the death, the successor's own level goes out |
| unit `ADeadRenderbuffersEntryIsRetiredWithItsSlot` (`:1484`) | **Failed** (the dead handle still read its sticky mask) | Passed |
| unit `ADeadTexturesSamplerViewLatchIsRetiredAtItsDeath` (`:1440`), `FramebufferEmit.ADeadFramebuffersNamedRecordLatchIsRetired` (`:782`), `ProgramEmit.ADeadProgramsRecordLatchIsRetiredAtItsDeath` (`:781`) | (added with the fix; the stale latches read as published between death and recycle on the unfixed tree) | Passed |
| `ARecycledTextureSlotDoesNotInheritItsPredecessorsBindMask` gains: the cache reference is back at the DEATH and the dead handle reads mask 0 | - | Passed |

### 2.3 M-A - no producer of `kMGPipeBindSampler` / `kMGPipeBindShaderImage`

**Root cause.** `NoteTextureBoundAs` had one caller (the framebuffer emitter, the two attachment bits).
`SamplerEmit.h` and `ImageEmit.h` are included BY `TextureEmit.h` and could not include it back;
`TextureState.h` is `MG_State` and may include no emit header. Nothing set either bit, `ImageBindableHint`
was always 0, the metadata respecify had no live trigger, and the remint pull was neither prevented nor
counted.

**What changed.**
* `MG_Pipe/PipeMutation.h:379/384`, `MG_Impl/Pipe/PipeFill.cpp:1293-1307` - `MGPipeNoteTextureBoundAs(
  MGPipeHandle, Uint32 bindBit)` and `MGPipeNoteTextureImageBound(ITextureObject&)`, the birth hooks'
  shape (declared in the one door, defined beside the other hooks, forwarded through
  `ForwardWhenWired<kMGPipeWiredTextureSubsystem>`); not gated on `FamilyIsLive` because the mask is
  client state and the emission it causes is gated inside the emitter.
* `MG_Impl/Pipe/SamplerEmit.h:774` - the sampler-view resolution notes SAMPLER on every texture it names
  in an emitted `MGPBoundView` (D-A4's letter). `MG_Impl/Pipe/ImageEmit.h:106` - the image walk notes
  SHADER_IMAGE. `MG_State/GLState/TextureState/TextureState.h:43` - `ImageTextureBinding::Bind` notes
  SHADER_IMAGE at `glBindImageTexture` itself (push-only), so the hint reaches the applier BEFORE the
  texture's first sync - the prevention half's whole point.
* `MG_Util/Metrics/PipeStats.h:166`, `PipeStats.cpp:178/459`, `MG_Backend/DirectGLES/Managers.cpp:5260,
  5338-5340` - `CallClass::TextureRemintPulls` ("tex-remint-pulls", `trp=` on the summary line, push-
  only so the pull enum is unchanged), counted in `RequireImageBindableStorage` once per transition that
  replays a level of storage the backend ALREADY HELD (`hadBackendStorage`); a texture reaching the
  transition uninitialised is allocated image-bindable up front and pulls nothing. **This is the counter
  for ROADMAP.md:85 (open question 2):** read `trp=` off the periodic `MGPipe stats:` line
  (`MOBILEGL_PIPE_STATS=1`) on the MC / Iris fixtures.
* `Harness/P4aFinalFixPeek.{h,cpp}` - `PeekPipeTextureResourceRecord` (by GL name),
  `PeekPipeStatsTextureRemintPulls`, `PeekPipeStatsTextureUploadEmissions` (arm the counters in-process).

**Scenario and proof.**

| case | before | after (9f60aadc) |
|---|---|---|
| unit `SamplerEmit.AResolvedSamplerViewMarksItsTextureAsSamplerBound` (`SamplerEmitTest.cpp:1098`) | **Failed** (mask 0) | Passed; a texture no sampler uniform resolves keeps mask 0 |
| unit `ImageEmit.AnImageBoundTextureIsMarkedShaderImageBoundAtTheBind` (`ImageEmitTest.cpp:528`) | **Failed** (mask 0 after the bind and after the walk) | Passed at the bind and in the emitted set |
| `AnImageBindAfterAllocationReachesTheApplierAsAMetadataRespecify` (DirectGLES; `glTexStorage2D` + red, draw, blue subimage drained by an unrelated draw, `glBindImageTexture`) | **Failed**: `ImageBindableHint 0`, `BindMask & (1<<6) == 0`, serial 5 vs 5 unmoved | Passed: hint 1, bit set, serial moved, picture blue, **`trp` +1** for the texture allocated before its hint and **+0** for a second texture image-bound before its first sync; then the KEPT half on that second texture: a DSA attachment's RENDER_TARGET bit (a Named record, no sync in between) reaches the record as a metadata respecify with the blue upload still standing beside it, and the next draw is blue |

A note on the case's shape: Espryt syncs a texture eagerly inside `glBindImageTexture` and the widening
re-mints its storage, replaying every level from the shadow, so on the first texture the standing upload
is CONSUMED by that regeneration whatever the metadata respecify did - which is why the "kept" property is
proved on the second texture (image-bindable already, a new bit, no remint in the way), with Espryt's own
upload count telling a consumed upload from a dropped one.

### 2.4 The minors

* **m-1 / F-7 (c2c6a655).** `MG_Pipe/PipeApply.{h,cpp}:1010/2482` - `MGPipeApplySetTextureParams`
  returns `Bool` (false on no consumer, no record, null built-in sampler; true on store).
  `TextureEmit.h:739-837` - the latch (`HasParamsLatch` / `ParamsVersion` / `SamplerVersion` /
  `ForceParamsResync`) is written only after acceptance; a refusal is counted (`RefusedParamCount()`,
  `:966`) and logged once. Unit `ARefusedParamsRecordDoesNotAdvanceTheLatch` (`TextureEmitTest.cpp:1511`):
  with the pre-fix latch order the refused record advanced the latch and the record never learned the LOD
  write (`ParamCount` +1 not +2, `ParamsSerial` unmoved, `LodBias` 0 vs 0.5) - **RED**; with the fix
  **GREEN**. The case was reshaped twice during the round (its first shape never emitted the refused
  record - `MakeTexture2D`'s format setter had already latched - and its second shape lost its premise to
  commit 5's heal); the final shape drives the emitter directly under `ScopedNoResourceOps`, where the
  applier's belt refuses both the parameters and the healing create, and its red/green was re-taken with
  the latch-order toggle (transcript in `finalfix-v1-logs/`).
* **The residual m-1 uncovered (8c458cd5).** With the refusal loud, the retrace census found 2 lines
  (`set_texture_params for texture 0 {slot=2, gen=0} was refused`): the context's default textures are
  constructed (`TextureState.cpp:68`) before the backend registers `MGPipeResourceOps`
  (`Managers.cpp:2556`), so their create never went out, and an application's first `glTexParameter*` on
  texture 0 found no record - silently latched away before m-1. `TextureEmit.h:805-831` heals the record
  from the params path the way `EmitResourceRespecify` heals it: a create with no storage for the
  identity, the storage itself if the texture has any (a respecify against the create's descriptor is
  never deduped), then the parameters; `Entry&` is re-fetched across the respecify (the table can grow).
  Unit `ATextureBornBeforeTheConsumerRegisteredGetsItsRecordFromItsFirstParamsPublication` (`:1548`):
  **RED** (refused, no record healed) / **GREEN** (record with its 8x8 storage, `ParamsSerial` 1, the LOD
  bias; and a storage-less texture heals to an identity-only record). The retrace census is 0 on the head.
* **m-2 (c2c6a655).** `Config.h:336-341` - `0x400 ... requires 0x80 AND 0x800`, naming
  `kMGPipeP4aFamilyDependencies` and ID-15.
* **m-3 (c2c6a655).** `MGPipeTypes.h:1110-1114` - the applier replaces the descriptor WHOLE with the one
  the client sent; the mask in it is the client's sticky OR, so no bit can be lost; "ORed, never replaced"
  is gone.

## 3. Judgement calls and deviations from the charter

* **The applier's precedence (C-1).** wire's W11 clause read "a metadata update drops nothing, whatever
  `level` says". The client's only metadata caller (the mask republish) passes `nullptr`, and a
  per-level caller cannot be deduped on the descriptor (a non-base level's extent is not in it), so a
  named level with identical storage fields is now that level's redefinition and drops exactly that key.
  `ARespecifyThatRedefinesNoStorageCarriesTheStickyMaskAndKeepsThePendingUploads` keeps its null-level
  half and gains the named-level half (its immutable store has two levels now so level 1 exists for the
  validator). Contract grant; stated in `PipeApply.h:787`, `PipeApply.cpp:1680-1690`, `MGPipeTypes.h`.
* **A per-level respecify is never deduped.** One applier call per `glTexImage*D` / generated-mip grow
  instead of a byte compare; the sub-data that follows moves the serial anyway, and on the handle arm an
  unchanged descriptor with a moved serial costs Espryt one bind and an empty level loop
  (`Managers.cpp:6470-6520`: `needsRegeneration` compares the descriptor, not the serial).
* **`LevelsFrom` as N calls.** The contract's key is one level; a chain cut is spelled as one call per
  removed level rather than a new contract type. Belt-and-braces in practice: every frontend path that
  cuts a chain has already redefined or is about to redefine the surviving base
  (`DiscardMipmapChainOnBaseRespecification`, `glTexStorage*`'s loop), but the unit case pins the rule.
* **Files outside the listed grant** (ID-52's charter names the mechanisms, the grant list names B, the
  contract, the applier, Espryt and one scenario file): C's `SamplerEmit.h` / `ImageEmit.h` /
  `ProgramEmit.h` (the D-A4 placements and the death forward; 1 + 8 + 14 lines), B's `FramebufferEmit.h`
  (the death forward), A's `PipeStats.{h,cpp}` (the counter; push-only), `MG_State/.../TextureState.h`
  (B's directory; the eager image-bind note), C's `SamplerEmitTest.cpp` / `ImageEmitTest.cpp` /
  `ProgramEmitTest.cpp` and B's `FramebufferEmitTest.cpp` (one case each, with pull twins), and F's
  `MG_IntegrationTest/CMakeLists.txt` (two source lines + the two `MallocPerturb` registrations the
  review's §8 asked for). Every one is additive.
* **A second harness TU** (`Harness/P4aFinalFixPeek.{h,cpp}`, fablefix's precedent): a scenario TU cannot
  include `PipeApply.h` beside the GL prototype headers and F's `PipeApplyPeek.cpp` is not mine to edit.
* **Five commits, not four**: the heal is the census's finding on the round's own m-1 work and is kept as
  its own commit (with the F-7 reshape it necessitated) so the history says who found what.
* **The C-1 / M-A itest cases assert on DirectGLES only** (skipped by name on DirectVulkan, which
  registers no P4a consumer - the handle arm is inert there by design); the C-2 cases run on both
  backends (ID-8: the death path is backend-neutral and the DirectVulkan lane must see it).

## 4. Gates on `8c458cd5` (one detached script, `-j 8`, `CCACHE_BASEDIR=/home/swung/w7`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`)

| gate | result |
|---|---|
| builds | build-linux / build-push / build-verify rc 0, **0 warnings** each |
| G1 `symbol_report --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` vs `~/w7/p4a-before-libMobileGL.so` | rc 0: `.text` 10806323 -> 10806323 (+0); 27811 -> 27811 symbols: **0 added, 0 removed, 0 resized, 0 renamed** |
| `RenderStateImpl` sha | d8fd1c48716056c53675 == before |
| `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` / `--self-test` | rc 0 byte-identical / rc 0 |
| `scripts/p4a_untouched_regions.sh 37da3c3a HEAD` / `--self-test` | rc 0 byte-identical / rc 0, 8 controls tripped and named |
| `gen_pipe.py --check` | up to date, rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0, 27 negative controls tripped, positive OK |
| `check_include_closure.py --compiler clang++` / text self-test | rc 0, 4 probes, 0 problems / rc 0 |
| gate C (`pGLContext` in MG_Backend) / G13 | only `PipeInputs.h` (as before) / `PipeApply.h` :37 :40 :832 :1040 :1041 - forward declarations and prose, outside the ops block (ID-39's class) |
| unit `-L unit` | **1785 / 1785 / 1785** (linux / push / verify) |
| G2 pull == push ctest names | **2902 == 2902**, 0 diff lines |
| G14 vs `p4a-before-ctest-names.txt` | 0 removed, **+314** (was +275: 13 unit twins + 13 itest cases x 2 backends = 39) |
| emitter/CSO unit suites (push) | 185/185 |
| HandleRecycle (verify) / verify controls | 180/180 / 4/4 |
| integration-gpu, build-push, 0x1fff (default) | **1117/1117** (1091 + 26) |
| integration-gpu, build-push, 0x1ff / 0 / 0x9ff / 0x5ff | **1117/1117** each |
| DirectVulkan lane, 0x1fff | 559/559 |
| integration-gpu, `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` | 1117/1117 |
| integration-gpu, build-linux (pull) | 1117/1117 |
| family regex / G9 `TextureParamsWithoutASamplerView` / `TextureUploadShape` (recorded) | 497/497 / 8/8 / 3/3 |
| the new lane `P4aFinalFix` at 0x1fff / 0x1ff / 0 / pull | 26/26 each |
| `g7_negative_control.sh` / `p3a_vertex_input_negative_control.sh` / `p4a_descriptor_negative_control.sh` | rc 0 tripped / rc 0 (IsBgra) / rc 0 (both controls named) |
| CsoContentAddressing + ResourceSubsystemControl + ObjectSubsystemControl / the 0x9ff arm | 24/24 / 14/14 |
| controls (PoisonOmitted/VerifyCorrupted/HandleRecycle, verify) | 184/184 |
| integration-verify, build-verify, `-j 4` | **920/920, `Fatal{` lines 0** |
| retrace (`retrace_gate.py --tree ~/w7/p4a-finalfix --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-finalfix -j 4`, fixtures re-copied from `~/w7/pipe`, 0 LFS pointers) | rc 0, **79 / 79**, failed [] |
| retrace refusal census over the 79 `mobilegl.log`s | **0** for every one of 16 needles: the ten shapes, "does not describe the binding it names", `Fatal{`, "cannot be adopted on the handle arm", **"was refused"** (C-2's and m-1's tells), "is dead but the emitter still holds", "no applier record"; the deduped `MGPipe:` ERROR/FATAL list is **empty** (it held 2 lines on `c2c6a655`, closed by `8c458cd5`) |
| itest eight-family census (`wsl_p4a_refusal_census.sh default`, 558 names, 538 logs) | **0 lines / 0 tests on all 13 rows** (the ten shapes + the three this round's tells), stems 0 / 0 / 0 |

## 5. Declared, with reasons

* **m-4 / F-6** (`MGPipeEncodeImageAccess`, `MGPSamplerView::Target`, `DrawBuffers` narrowing stated in
  package headers): contract work for P4b (R-3); no behaviour to fix this round.
* **m-5** (`GenerateThreeChannelFloatMipmapOnCpu` marks levels dirty without `RearmPipeTextureLevelUpload`,
  self-heals one verb late): a server-side D-M emulation site on E's file; the rearm belongs with R-7's
  audit of the two backend re-dirty sites, not in a client-side round. Its storage grow beside it goes
  through `AllocateStorage` and is level-scoped by C-1 for free.
* **m-6** (G9 blind when a `UseProgram` row and its mark drop together): derivation work, R-4, P4b.
* **m-7** (`DirectGLES.cpp:4137/4174` pull-path `broadcastCountResolved`): benign; G1 0/0/0/0 stands.
* **m-8** (nested-worktree `include/ska` pointer): this worktree, made with the plain `git worktree add
  -b` the charter spelled, shows a clean `git status` in `include/ska`; every commit here used explicit
  `git add` + `git commit -F`, never `-a`, and each was verified with `git log`/`show --stat`. Two tooling
  notes of this round for `notes/tools`: (1) `$(...)` inside a double-quoted `wsl.exe -d Arch -- bash -c
  "..."` string is expanded by the Windows Git Bash BEFORE the string reaches WSL (it reported the FCL
  repo's `dev` head and dirty count as if they were the worktree's); (2) a Bash-tool heredoc containing
  backticks fails to parse - write the `.py` with the Write tool instead.
* **§8 items 6-7** (the two-device Release A/B re-take on the fixed head - C-1 dropped uploads and biased
  the push arm - `test.yml:9-12`'s TEMPORARY trigger, D.5 docs and `check_doc_citations.py`) are the
  integrator's. For D.5: `ARCHITECTURE.md:249-254`'s texture paragraph should carry the C-1 rule ("the
  client passes the level it redefines; a chain cut names its removed levels; whole-resource calls pass
  none; a named level is dropped whatever the descriptor says"), `ARCHITECTURE.md:598` the four
  dependency rows, and `ROADMAP.md:85` the counter name (`tex-remint-pulls`, `trp=`).
* The census tool `notes/tools/wsl_p4a_refusal_census.sh` already lists this round's three tells
  (rows 11-13 in the head's census run); it was not edited by this round.

## 6. Logs

Kept in `~/w7/notes/p4a/p4a-results/finalfix-v1-logs/`: the RED run on the unfixed tree
(`p4a-finalfix-red.log`, `p4a-finalfix-red-itest.log` with the black-pixel counts, the `pure virtual
method called` abort and the M-A assertion values), the GREEN run after each fix (`p4a-finalfix-green-
{c1,c2,ma,minors}.log` + `-{c1,c2,ma,ma2,minors}-itest.log`), the F-7 / heal red-green transcripts
(`p4a-finalfix-f7.log`, `p4a-finalfix-heal.log`), and the gate on `8c458cd5` (`p4a-finalfix-gate.log`,
`-gate-symbols.md`, `-gate-retrace-summary.txt`, `-gate-census.log`, `-gate-iverify.log`). Every other
`~/w7/p4a-finalfix-*` log, `~/w7/finalfix-stage/`, the census output directory and the retrace output
directories (summary.txt / summary.json kept) were deleted at the end of the round. The worktree
`~/w7/p4a-finalfix` stays (branch `p4a/finalfix`, HEAD `8c458cd5`, clean; fixtures are the copies from
`~/w7/pipe` under `assume-unchanged`). The shared scratchpad `wsl` directory is untouched except for this
round's own `finalfix-*` scripts.
