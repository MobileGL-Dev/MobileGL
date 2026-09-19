# P5e package `fb` — framebuffers, attachments, images, blit (report v1)

Branch `p5e/fb`, head **`ca1beb69`**, base `f6cfcbd3`. Tree `~/w7/p5e-fb`. Six commits; the fourth
(`f4675d1a`) is a SCAFFOLD that the sixth (`ca1beb69`) reverts — see §5.

## 1 What changed, per file (ranges at the NEW head)

**`DirectGLES.cpp`** — `:242-264` the doors (`RefuseFramebufferBindingSlotRead`,
`FramebufferRecordArmIsMandatory` = `Transport != Monolith && FramebufferSubsystemEnabled()`,
ruling 1 / §5.8, which selects every arm below). `:2171-2272` the list keys: a new
`FboAttachmentSyncEntry {Res, backend}`, six statics, `SyncRecordAttachmentTextures` — **I did NOT
touch `UnitTextureSyncEntry`**; tx2 owns its re-shape, the attachment lists borrow nothing, and a
type of their own avoids the collision. `:2344-2412` `SyncFramebufferAttachmentTexturesByRecord`
(the core), selected at `:2573`. `:2726-2968` images: `ResolveShaderImageRecord` and
`g_imageRecordSeamIsAuthoritative` DELETED; `IssueImageTextureBind`, `GLAccessForImageView`,
`IsWritableImageBufferView`, `SyncImageTextureBinding(const MGPImageView&)`,
`NoteImageUnitBoundWithoutReadingTheFrontend`. `:2976` the GPU-write marks. `:3055-3180` the sweep
walk and its gate. `:3290` `FramebufferRecordMatchesBinding` monolith-only. `:3428-3543`
`SyncCurrentFBOByRecord`'s loop; `:3571` / `:4781` the two declines; `:4920`
`SyncAndBindFramebufferByHandle`; `:4956` `ForceBindCurrentFBO`; `:6570` `Clear`'s debug
diagnostic. `:8609-8806` the record blit overload + `PushedReadBufferPoint` + `BlitEndpointRecord`,
`:8850` named arm, `:8955` bound arm. `:9409-9484` the detach walk. `:10118` the RBO refusal.
`:10561` `BindImageTexture`.

**`Managers.cpp`** — `:4374` `PipeTextureTargetForHandle`; `:9662` `PushedSurfaceForAttachment`
exported; `:9675-9820` `SyncAttachmentSurface` loses its attachment parameter and both
cross-checks; `:9951-10035` `ApplyReadBufferFromRecord` + `SyncReadBufferToBackendByHandle`;
`:10331` the record-side empty-point test; `:10525-10760` the reverse index +
`BackendFramebufferObject::SyncToBackendByHandle`; `:11228` `BoundImageUnitFormat`;
`:14025-14105` `BackendRenderbufferObject::SyncToBackendByHandle`; the two fb stubs deleted.

**`Managers.h`** — six declarations (§6.4, §6.8). **Tests** — `FramebufferEmitTest.cpp` +2,
`ImageEmitTest.cpp` +1, all three in the X-macro skip lists (G2/G14).

## 2 Kimi-audit rows 68-90

**RETIRED** (no frontend read under a transport): 68 (monolith-only), 69, 70 (`NoteStateForHandle`
deleted), 71, 74, 76, 77, 78, 79, 80, 81, 82, 85, 86, 87, 88, 89, 90.

**LEFT as monolith glue**, unreachable under a transport because `SyncCurrentFBO` / `BindCurrentFBO`
abort before them: 72, 73, 75, 83.

**NOT MINE:** 84 (`SyncCurrentProgram`'s fragColor-broadcast half). Its record arm exists and is
what a transport takes; its decline arm still reads the binding slot, and that decline is in **pg's**
line range. A framebuffer row inside a program function — named so the integrator can rule whether
pg refuses it or fb widens. Nothing in the split lane reaches it.

**NOT FOUND:** none. Every row 68-90 exists at this head.

## 3 The rulings I was asked to answer

- **ID-94 / ruling 16** — done. `Access` decodes through `MGPipeValueTypes.h`; the stale "the
  encoding does not exist at the contract commit" comment is deleted and corrected in place.
- **ID-86 / ruling 7** — done: `(ShaderImagesSerial, TextureShutterSerial, ContextSerial,
  g_backendContextGeneration)`, in their own three fields beside the frontend pair. The old
  comment's argument is KEPT because it is what decides which values are admissible: an applier
  serial moves when the CLIENT said something, strictly before the sweep the saying provoked, so
  the "re-minted inside this sweep" property survives the re-key. Never a backend re-mint counter.
  The window/high-water union is kept and re-argued in place.
- **`SyncReadFramebufferTextureAttachments` ungated on the framebuffer bit** — the delta is
  SMALLER than S3 feared. The function is still not gated on the bit; what selects the record arm
  is the TRANSPORT plus the bit. So the object A/B moves only in its SPLIT arm, and the
  push-monolith arm is token-for-token what it was. **It still needs re-baselining for the split
  arm** — integrator, with the phase's other re-baselines.
- **The renderbuffer cross-check (base `Managers.cpp:9711`)** — fixed by DELETION, not gating
  (§4.4's instruction). **A latent RULE violation, not a latent wrong-picture bug**: the probe ran
  on the apply thread under split while its texture sibling was transport-gated, but inside
  `MGPipeFrontendKeyedRegistryScope` with the client parked, so its answer was correct. It becomes
  a correctness bug the instant the record is unbarriered — which is why it is deleted, not gated.

## 4 Red-once evidence (executed, quoted verbatim)

**R1 — `SyncAttachmentSurface` takes the frontend attachment again** (reverted
`SyncCurrentFBOByRecord` to `SyncToBackend(currentFBO, target)`, the only way the attachment sync
can be handed one). Full strict lane marker tables: with the arm, no `GetFramebufferBindingSlot`
marker at all; with it reverted, `2 Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@Clear"}`.
The private log's line:
```
MGPipe: Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@Clear"} [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]
```

**R2 — the draw-FBO list key back to the pointer compare**, id's hook armed
(`kMGPipeP5eClientWaitRuleLanded = true`), on
`ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels`:
```
MGPipe: Fatal{RoleViolation, "MGPipeSlots"} - the apply thread called MGPipeSlots().HandleOf. ...
A named exemption scope admits it only while the record being applied is BARRIERED (barriered=0) ...
#3 MGPipeRefuseAllocatorFromApplyThread  #4 BackendSlotTable<ITextureObject>::Find
#5 TextureImpl::SyncTextureObjectToBackend  #6 TextureImpl::SyncNeccessaryTextures
#7 DirectGLES::Clear  #8 ServerVerbSink::OnClear
```
Control (key restored, hook still armed): the entry passes `OnClear` untouched and aborts later at
`#5 VertexArrayImpl::ResolveVaoTwin #6 PrepareForDraw #7 DrawArrays #8 OnDrawVbo` — **vi's** site.
The isolation is by FRAME, because with the hook armed every unlanded family aborts somewhere; it
becomes a plain before/after once vi and tx2 land.

**R3 — the image `Access` mapped 1↔2.** (a) the SHARED table (`WriteOnly = 2, ReadWrite = 1`):
```
../MobileGL/MG_Test/Pipe/ImageEmitTest.cpp:511: Failure
Expected equality of these values:  Which is: '\x2' (2)   Which is: 1
../MobileGL/MG_Test/Pipe/ImageEmitTest.cpp:512: Failure
Expected equality of these values:  Which is: '\x1' (1)   Which is: 2
```
(b) the SERVER decode alone (`GLAccessForImageView`): `Split.UnboundImageDescriptorScenario` 13/13
and the whole split lane 111/111 stay GREEN — **lavapipe does not enforce `glBindImageTexture`'s
access**, so the unit case is the only witness that exists here, which is why the brief asked for
it. `FormatlessImageBakeScenario` / `ImageLoadStoreSsoScenario` are MONOLITH-lane entries with no
split twin: they run the frontend arm and cannot reach the decode at all.

**R4 — the named blit back on the frontend objects: DID NOT GO RED**, and I stopped after two
attempts rather than manufacture one. `Split.NamedBlit.F1WireScenario` 4/4 green with the revert.
Three measured reasons: both endpoints of those cases are BOUND (hence synced) before the blit, so
the twin is already configured and the state note is not load-bearing for them; the
layered-destination substitute is gated on `MG_Util::SelfTest::BlitIgnoresDestinationArrayLayer`,
which lavapipe does not trip, so the aspect plan's body never executes in this lane on EITHER arm;
and with id's hook armed the entry aborts earlier at tx2's unlanded unit walk
(`#5 SyncTextureObjectToBackend #6 SyncNeccessaryTextures #7 Clear`). What IS shown:
`Split.NamedBlit.F1WireScenario` (4) and `Split.CopyImageLayeredScenario` (6) are green with the
record-driven blit — the substitution is behaviour-preserving. **Ruling wanted**: whether a
device-lane witness is required before this arm is accepted.

**R5 — the per-point version memo as a second gate: the proof is that it is ABSENT and the lane is
green.** `SyncToBackendByHandle` has no `m_syncedFrontendAttachmentVersions` at all (the record
hash plus `g_attachmentBackendIdGeneration` are the only gates) and `integration-split` is 111/111,
including `Split.F1.F1WireScenario` (13) and `Split.CopyImageLayeredScenario` (6). The
re-storaged-attached-RBO shape S3 named for `P4aSeamAuditScenario` F-3 is covered by the existing
`FramebufferEmit.ARestoragedAttachedRenderbufferPublishesItsNewExtent`, still green.

## 5 Gate

`build` rc=0 · `unit` **2209/2209** (2206 before the three new cases) · `isplit` **111/111** ·
`strict` 3/111 · `gens` all seven rc=0 · `one` **77/77** on `Split.{F1,NamedBlit,Ct}`,
`Split.CopyImageLayeredScenario`, `Split.UnboundImageDescriptorScenario`, `FramebufferEmit.`,
`ImageEmit.` · seam greps SILENT (the image seam log is deleted; no framebuffer "does not describe
the binding it names" line in any of the 88 private logs). The one `gens` doc-citation complaint
(`CONTRACT-P5.md:524`) is PRE-EXISTING — verified with `--rev f6cfcbd3`.

**Strict markers, base (22 private logs) vs now (88; the slice is four times larger because fb's
field no longer aborts first on those entries):**

| marker | base | now | owner |
|---|---|---|---|
| `GetFramebufferBindingSlot@Clear` | 6 | **0** | fb — retired |
| `GetImageTextureBinding@BindImageTexture` | 0 visible | **0** | fb — surfaced at 13 mid-package, fixed (§6.8) |
| `GetTextureUnitObject@Clear` | 13 | 18 | tx2 |
| `GetTextureUnitObject@GenerateMipmap` | 2 | 0 | tx2 |
| `GetBoundVertexArray@DrawArrays` | — | 14 | vi |
| `GetProgramForDispatch@DispatchCompute` | — | 7 | pg |
| `ValidateProgramName@ShaderStorageBlockBinding` | — | 2 | pg (barriered, §7 allowlist) |
| `GetTextureObject@CopyImageSubData` | — | 6 | P7/P9 (barriered) |
| `GetTextureObject@Clear` | — | 39 | **the scaffold's own cost**, gone with it |

**THE HEAD DOES NOT PASS `isplit`, BY CONSTRUCTION.** fb calls tx2's
`SyncTextureToBackendByHandle` / `SyncMipmapsToBackendByHandle` from four places. With c0e's
aborting stubs back, every entry whose framebuffer has a texture attachment dies on
`Fatal{UnmigratedVerb, "TextureImpl::SyncTextureToBackendByHandle"}`. Every number above was taken
at `fcd7c5c8`, where `f4675d1a` bodied the two seams by resolving the frontend texture from the
record's own `Desc.GlNameForDiag` through the `GetTextureObject` sticky forward — correct under
lockstep, deliberately NOT tx2's design, reverted at `ca1beb69`. **fb and tx2 land together, or fb
after tx2.**

## 6 Seams assumed, and deltas to name

1. **`SyncTextureToBackendByHandle(h, imageBindable)` is the by-handle twin of
   `SyncTextureObjectToBackend(obj, …)`** — it does (or memo-skips) params, builtin sampler and
   mipmaps, and returns the twin. The two attachment lists make ONE such call per entry instead of
   the pre-handle arm's three, because by-handle forms of `SyncTextureParamsToBackend` /
   `SyncBuiltinSamplerToBackend` / `IsDrawSyncClean` are tx2's new names and are not declared
   seams. If tx2's `IsDrawSyncCleanByRecord` ends up OUTSIDE that call, fb's lists need a re-look.
2. **`SyncMipmapsToBackendByHandle(h)` drives storage from the descriptor** with no frontend
   object — that is what let `SyncAttachmentSurface` drop its parameter.
3. **`MarkWritableImageBufferTexturesGpuWritten` deleted under a transport** — paired with **sb's**
   deletion of the backend storage-block marks. I gated at the TOP of the function, not at the two
   call sites, so the unit bookkeeping is out of an apply thread's reach too; sb should say whether
   it did the same.
4. **The reverse index is maintained at `SyncToBackendByHandle`**, not at `set_framebuffer_state`
   apply as §5.4's wording suggests. Same set: a framebuffer whose record was never synced has no
   driver attachment to detach, and every path that can have one syncs first. It lives in
   `FramebufferImpl` (`NoteFramebufferTextureAttachments` / `FramebuffersAttachingTexture`) and
   **tx2 calls the second one** if it wants it.
5. **`UnitTextureSyncEntry` untouched** (see §1).
6. **Shared-code delta (G1):** the image bind's format rules are factored into one `static`
   `IssueImageTextureBind` that the PULL build compiles too, because the alternative was the same
   hundred lines of widening/split reasoning twice and a divergence there is an out-of-bounds image
   read, not a wrong pixel. One caller in the pull build, internal linkage, no new symbol. Named
   here rather than left to a G1 diff. Everything else is under `MOBILEGL_PIPE_PUSH` /
   `MOBILEGL_BUILD_DISAGGREGATED`.
7. **Push-monolith delta:** `SyncAttachmentSurface`'s two loud cross-checks and the N-6 refusal go
   on that arm too (no second answer left to check against), and `SyncImageTextureBinding(Uint)` no
   longer takes its four field values off the record there. Both behaviour-identical by
   construction — the client copies the unit's own binding, P4a's own argument — but they ARE token
   moves in the push-monolith build. The pull build is unaffected.
8. **Out of my stated range, taken deliberately:** `DirectGLES.cpp`'s `BindImageTexture` (the
   `bind_shader_image` sink target) was reading `GetImageTextureBinding` on an UNBARRIERED row and
   strict named it on 13 entries; no other package's range names it. It now records the high-water
   mark and defers to the sweep (the sink bumps the texture shutter serial right after, so the
   sweep cannot be suppressed). `Managers.h:717` gained `PipeTextureTargetForHandle` beside
   `PipeTextureRecordForHandle`. **Ruling wanted**: `MGPImageBind::Res` stops at the sink, so that
   funnel cannot resolve a twin at all — should the backend `BindImageTexture` entry eventually
   take the handle, or is deferring the permanent answer?

## 7 Is a blit still barriered?

**No, and it need not be.** `Blit` is `kWaitNone` in §2.2's static column and fb removes the last
reason S3 could see to escalate: both arms resolve every endpoint, aspect and texture from records
and applier state, and the two `ForceBindCurrentFBO` calls that restore the bindings afterwards are
handle-only. The predicate decides and **ra owns the flip** — fb changed no wait class, and asks
for none.

## 8 What I did not do

No device measurement, no adb, no pull-build (G1) compile — the gate configures the split flavour
only. The object A/B is not re-baselined (§3). The resize case has no negative control beyond the
suite's own `AnUnchangedBindingPairEmitsNothing`, which is what stops it passing because the
emitter fires unconditionally. Row 84 left to pg (§2).
