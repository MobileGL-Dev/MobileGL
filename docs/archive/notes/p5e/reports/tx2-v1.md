# tx2 — textures and samplers by handle and window (P5e wave 2)

Branch `p5e/tx2`, head **1c872710**, base **f6cfcbd3** (landed wave 1). Tree `~/w7/p5e-tx2`.
Five commits: 72e79020 (the family by handle), 85959090 (two the lane found), a08b3379 (tests),
99211a48 (the ABA case re-aimed), 1c872710 (a comment fix).

## 1 What changed, per file

**`Managers.h`** — `BackendTextureObject` gains `m_pushedSyncHandle` (§4.1's `m_handle`) +
`NotePushedSyncHandle/PushedSyncHandle` (`:1940-1960`, push-only, so the pull layout does not
move); `IsDrawSyncCleanByRecord` (`:1740-1775`); `RequireImageBindableStorageByHandle`
(`:1675-1695`); `SyncTextureViewToBackendByRecord` (`:1645-1665`); `ResolveOwnRecord` +
`ResolvePushedBuiltinSampler`'s record parameter (`:1895-1910`).

**`Managers.cpp`** — `ResolveOwnRecord` (`:8752-8772`) is the whole of "the handle first, the
client allocator only failing that", and the two prologues lose their `HandleOf` probes; the three
sync bodies tolerate a null frontend object on the handle arm and answer `GetTarget` from
`Desc.Target`, the ~20 `GetExternalIndex` log reads from `Desc.GlNameForDiag`, the storage-kind
backstop from `Desc.StorageKind` (three macros beside the P4a/P5c ones);
`SyncMipmapsToBackend`'s record resolution is hoisted to the top so the VIEW test can read
`Desc.ViewOf` (`:7285-7310`); the buffer arm reads `Desc.BufferForTexBuffer / BufOffset / BufSize`
and passes `nullptr` to `EnsureBufferResourceForHandle` (`:8415-8600`);
`RequireImageBindableStorageByHandle` (`:5879-5930`); `SyncTextureViewToBackendByRecord`
(`:7015-7065`); the two c0e seams bodied (`:14100-14200`).

**`DirectGLES.cpp`** — `UnitTexturesByHandle()` (`:2170-2185`) is the one arm selector
(`Transport != Monolith && SamplerSubsystemEnabled() && TextureResourceSubsystemEnabled()`);
`UnitTextureSyncByHandleEntry` + its list and keys (`:2186-2215`); the unit half of
`SyncNeccessaryTextures` (`:2420-2485`); `CurrentUnitBindingsEpoch`'s decline → refusal
(`:2092-2112`); `ResolveAndBindUnitTextures`' window arm (`:4990-5065`);
`ResolveUnitSamplerBackend` → refusal at the entry (`:5205-5230`); `BindCurrentUnitSamplers`'
decline → refusal (`:5330-5350`); `BindCurrentTextures`' ruling-6 key, entry selection included
(`:5460-5600`); the sampler pass by record (`:5880-5960`); `NeedsRawDepthFetchSampler`'s
values-taking overload (`:286-310`); `GenerateMipmapByRecord` (`:10062-10112`) and
`GenerateMipmap`'s `VerbMipRes` arm (`:10120-10145`); the two mip-shape `HandleOf` sites deleted
in favour of `Generate{Depth,Color}Texture2DMipmapByRecord` (`:9660-9700`);
`ScopedDetachedTextureFramebufferAttachments`' by-handle constructor over the applier's
framebuffer records (`:9215-9290`); both CopyTex destinations synced by handle (`:9780`, `:9900`).

**`MG_State/GLState/TextureState/MipmapStorage.cpp`** — the guard list gains the seven read-only
shape accessors through `RefuseLegacyTextureShapeReadFromApplyThread`, which is keyed on the
server backend being DirectGLES (ruling 12's shape). The mutator rows stay unconditional.

**`MG_Remote/Server/PipeApplier.cpp`** — `CheckUnitWindows(st, verb)` (`:437-495`) called from
`OnDrawVbo` (`:515`) and `OnLaunchGrid` (`:738`).

**Tests** — `RemoteClientTest.cpp` seven new `MGL_TEXTURE_GUARD_TEST` rows; `SanityTest.cpp`
`ARecycledTextureSlotReMintsTheTwinOnTheHandleArm` + its pull-build skip twin.

## 2 The kimi audit's texture/sampler rows (35-67)

RETIRED under a transport: **35, 36, 37** (the snapshot walk is now unreachable: the decline that
led to it is a named refusal) · **38, 39, 40, 42** (the unit list is `st.BoundSamplerViews[]`,
the gate is `IsDrawSyncCleanByRecord`) · **47, 48, 49** on the by-handle entry (the frontend entry
survives as monolith glue and is still reached from fb's three attachment lists) · **50, 51, 52,
53** (`ResolveAndBindUnitTextures` is one pass over the window; the target is the view CSO's; the
unbind walks the fixed `TextureTarget` enum against `g_boundTexturesCache`) · **54, 55, 56, 57,
58** (the identity sampler family) · **59, 60, 62** (`VerbMipRes` + the descriptor) · **63** (the
detach sweep by handle over the framebuffer records) · **66** (the four `HandleOf` probes; they
survive only as the no-handle-noted fallback).

LEFT BARRIERED / trailing: **61** `GenerateThreeChannelFloatMipmapOnCpu` — named
`Fatal{UnmigratedEmulation, "generate-mipmap-cpu-filter"}` at the entry of the arm that would use
it; the pull is P8/P9's · **64** `UpdateTextureBindingAtTarget`'s `GetTextureUnitObject` (CopyTex
is `kWaitApplied`) · **65** already monolith-only at the base.

NOT MINE (fb's rows, untouched): **41, 43, 44, 45, 46, 67**. NOT FOUND: none — every row of 35-67
exists at this head.

## 3 Named behaviour deltas (G1 §)

- **P5e-5, the narrowing.** The unit list syncs the PROGRAM-RESOLVED texture per unit; the
  frontend walk syncs every slot of every touched unit. What stops being synced as a side effect is
  a texture bound to a touched unit that the program does not sample. Coverage is unchanged because
  the union covers it — views ∪ image units ∪ attachment lists ∪ the waited texture ops — **and the
  lane proved the last term is load-bearing rather than rhetorical**:
  `F1WireScenario.CopyTexSubImage2DPixels` went black, because a copy DESTINATION is bound to the
  active unit and sampled by nobody. Both CopyTex endpoints now sync themselves by handle — the
  only case the 111-entry lane found, and the shape any future "bound but not sampled" consumer
  has to copy.
- **Ruling 1 / G1.** Every handle arm is selected by `Transport != Monolith` AND the two family
  bits; the push-monolith build keeps the frontend view test, `RequireImageBindableStorage`, the
  unit walk and the `GenerateMipmap` body token for token. The pull build compiles none of the new
  members, so `BackendTextureObject`'s pull layout does not move and the admitted resize set stays
  empty.
- **`UnitTextureSyncEntry` is NOT re-shaped in place.** A second push-only struct
  (`UnitTextureSyncByHandleEntry`) and a second list are added beside it, because the same struct
  is also the draw-FBO and read-FBO attachment lists' — those are fb's, and one name carrying two
  shapes across two worktrees is the merge trap the contract package exists to avoid. Deviation
  from the brief's "re-shape, push-only"; the observable effect is identical.
- **Scout R4, made explicit in code and here.** `IsDrawSyncCleanByRecord` reads BOTH
  `Params.ForceResync` / `Params.SamplerResync` (the client's bits, read and never written back)
  AND `m_forceTextureParamsResync` / `m_forceSamplerResync` (the server's, set by
  `RecreateBackendTexture` / the image-bindable transition and cleared only by the prologue that
  consumed them). Neither side clears the other's. A gate that read one side would skip the sync
  the other is asking for, and for the sampler half that is not mis-filtering but an INCOMPLETE
  texture sampling (0,0,0,1).
- **Scout R5, resolved.** The buffer-texture arm passes `nullptr` to
  `EnsureBufferResourceForHandle`: that function's three frontend reads are all inside its
  monolith arm (kimi row 29), so `nullptr` is the whole of the split answer and is what §4.2's
  Buffer row prescribes. Whole-vs-range stops asking the buffer for its size — `kMGPipeWholeBuffer`
  IS the question.

## 4 Red-once evidence (executed, quoted, reverted)

1. **Revert the handle arm** (`UnitTexturesByHandle() → false` and `MGB_STAGED_TEXTURE_LIVE →
   false`), `ctest -R DirectGLES.Split.TriangleScenario` → 0/2 passed:
   `MGPipe: Fatal{RoleViolation, "texture-legacy-arm"} - the apply thread called
   TextureObjectMipmap::GetMipmapLevelCount on a frontend object. With an active transport the
   server reads the staged-texture store and the resource descriptor (CONTRACT-P5C §2, rule E); a
   frontend texture's level storage is client memory and this arm is monolith-only`
2. **Revert the unit work list's handle arm**, full strict lane →
   `MGPipe: Fatal{UnmigratedPipeInput, "GetTextureUnitObject@Clear"} [BARRIER-PULLED,
   MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]` on 9 entries —
   byte for byte the BEFORE table below. **Reverting `ResolveAndBindUnitTextures`' window arm
   ALONE changes nothing in the lane** (48/14/13/6/3/2, identical to the after table): with fb and
   vi unlanded, no entry survives to a draw's unit walk, so `GetTextureUnitObject@DrawVbo` cannot
   be any entry's FIRST pulled row. The brief's expected marker is therefore unreachable on this
   tree and the unit-list form above is the one that names the field.
3. **ABA**: drop the forward-Gen reset in `BackendSlotTable::GetOrCreate(handle)` →
   `ARecycledTextureSlotReMintsTheTwinOnTheHandleArm` fails:
   `Expected equality of these values: *secondSlot / Which is: (ptr = 0x592a99c35310, ...) /
   nullptr` and `Value of: MG_Pipe::MGPipeHandleIsNull((*secondSlot)->PushedSyncHandle()) /
   Actual: false / Expected: true`.
4. **Narrow `SamplerEmit.h`'s `Count` by one** → isplit 80% (22 of 111 red):
   `MGPipe: Fatal{ProtocolCorruption, "SetSamplerViews.Count"} - draw_vbo applies with units 0..0
   touched, so set_sampler_views must carry Start=0 and Count >= 1 (CONTRACT-P5E.md §5.3); the
   applied window is Start=0 Count=0, which drops the sync of at least one texture this draw
   samples` — and the dispatch twin, verbatim, with `launch_grid` in place of `draw_vbo`.
5. **Revert `GenerateMipmap`'s `VerbMipRes` arm**, strict on the three split F1 GenerateMipmap
   entries → `MGPipe: Fatal{UnmigratedPipeInput, "GetTextureUnitObject@GenerateMipmap"}
   [BARRIER-PULLED, ...]` in `p5b-f1-GenerateMipmapDepth.log` and
   `p5b-f1-GenerateMipmapPackedFloat.log`; with the arm present those two abort on fb's
   `GetFramebufferBindingSlot@ReadPixels` instead. Plain isplit also goes 2/3 red with the arm
   reverted.

## 5 Gate

| step | result |
|---|---|
| build | rc=0 |
| unit | **2214/2214** (2206 at the base + 8 new: 7 guard rows + the ABA case) |
| `integration-split` | **111/111** |
| gens | all 7 rc=0; the one doc-citation line (`CONTRACT-P5.md:524 Core.cpp:39 ambiguous`) is pre-existing and not mine |
| `one` | 242/242 on `SampledSetStalenessScenario|TextureParamsWithoutASamplerView|F1WireScenario|UnboundImageDescriptorScenario|HandleRecycleScenario` |
| strict | 3 passed / 108 failed, **unchanged from the base** (the lane is red by design until fb, vi, pg and sb land) |

**Strict markers, before and after, measured on THIS tree's own base (f6cfcbd3), 88 private logs:**

| marker | before | after |
|---|---|---|
| `GetFramebufferBindingSlot@Clear` (fb) | 39 | **48** |
| `GetBoundVertexArray@DrawArrays` (vi) | 14 | 14 |
| `GetImageTextureBinding@BindImageTexture` (fb) | 13 | 13 |
| **`GetTextureUnitObject@Clear` (tx2)** | **9** | **0** |
| `GetTextureObject@CopyImageSubData` (P5b debt) | 6 | 6 |
| `GetProgramForDispatch@DispatchCompute` (pg) | 3 | 3 |
| `ValidateProgramName@ShaderStorageBlockBinding` (pg) | 2 | 2 |

The only field that disappeared is mine; the only field that grew is fb's, by exactly the nine
entries that used to abort at mine first. `GetTextureUnitObject` is gone from the lane entirely.

## 6 Seams assumed, and what I coded against

- **`SyncMipmapsToBackendByHandle(handle)`** is bodied as c0e declared it (handle only): it
  resolves the record itself and stamps `m_pushedSyncHandle`, so fb's attachment sync and image
  sweep can call it with nothing but a `MGPSurface::Res` / `MGPImageView::Res`.
  **`SyncTextureToBackendByHandle`** returns `SharedPtr<...>&`; a decline is a reference to a NULL
  twin, so callers must test it. **`ResolveTextureTwin`** (id's) is used unchanged and NOT edited -
  it does not stamp the handle; the two by-handle SYNC entries do.
- **fb's reverse index** was not available, so `ScopedDetachedTextureFramebufferAttachments`'
  handle arm walks `MGPipeApplierState::FramebufferRecordFor()` directly; if fb lands an index,
  that constructor is one call site to re-point.
- **pg's half of ruling 6**: keyed on `{DrawProgram.{Slot,Gen}, ShaderCso.Serial, BindingsSerial}`
  through `PipeShaderCsoRecordForHandle(st.DrawProgram)`; if pg moves `BindingsSerial`'s writer the
  key follows with no edit here. `BindCurrentTextures` still RECEIVES pg's `currentProgram` and no
  longer reads it on the handle arm, entry selection included.

## 7 Rulings I need

1. **Texture VIEWS have no wire carrier for the view window.** `Desc.ViewOf` names the storage
   owner, but `minLevel/numLevels/minLayer/numLayers` are on no record. The STEADY question is
   answered server-side (sync the storage twin by handle, compare
   `m_viewSourceBackendTextureId`, stamp the serial); the CREATION of the view still needs the
   frontend object and takes it when the monolith-glue note reaches one, with a named
   `MGLOG_E_ONCE` when it does not. `TextureViewScenario` is not in `integration-split`, so
   nothing goes red today. Ruling wanted: four `Uint16`s on `MGPResourceDesc` (it has no spare
   pad, so this grows it) in a later package, or the view arm stays barriered and trailing.
2. **`GenerateMipmap`'s `WaitClass` is NOT flipped**, per ruling 14's escape. The unit read is
   gone, but the RGB16F/RGB32F CPU filter still reaches the client's level shadows (kimi 61), so
   the arm is not complete. Trailing item for tx2 or P8.
3. **`CheckUnitWindows` admits one silence**: both counts still 0 means the client's sampler
   subsystem bit is clear and the backend runs its frontend walk, so there is no window to
   check. Narrowing carries a non-zero-but-too-small count and is refused. Confirm this is the
   intended reading of ID-95, or the check has to be armed off the caps/subsystem mask instead.
4. **The widened `MipmapStorage` rows are backend-kind-keyed.** The unconditional widening
   aborted Magma's `BlitNamedFramebuffer` → `ResolveColorBlitBinding` → `IsComplete()` (two
   isplit entries). Ruling 12's shape applied: the new READ rows abort on DirectGLES and are
   silent on DirectVulkan; the MUTATOR rows stay unconditional. Confirm, or Magma's blit path
   opens `MGPipeTextureLegacyArmScope` instead (VulkanRenderer.cpp is not in my scope).

## 8 What I did not do

- **The `PipeSlotPeek` zero-scope-entries-per-frame assertion.** `PipeSlotPeek` reads the CLIENT
  allocator's occupancy; there is no per-frame counter of `MGPipeFrontendKeyedRegistryScope`
  entries and adding one would be hot-path instrumentation the roadmap forbids. The stronger
  observable already exists and is unconditional: once ra lands, ANY frontend-keyed probe from an
  unbarriered apply is `Fatal{RoleViolation, "MGPipeSlots"}`. The family's remaining scope sites
  (`DirectGLES.cpp:1745, :5137, :5223`, `Managers.cpp:5996, :8757`) are all monolith-glue halves
  the transport arms return in front of; the draw path reaches none of them.
- The CopyTex ENDPOINT resolve and `GetTexImage` — barriered and trailing, as the brief scopes
  them. `set_texture_params`' reply and the `resource_subdata` texture half — likewise untouched.
- No measurement of this family's apply-thread share (scout R6): it belongs with the device exit.
