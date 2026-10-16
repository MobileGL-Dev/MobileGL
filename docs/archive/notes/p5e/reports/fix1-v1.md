# P5e `fix1` — the by-handle attachment sync on the monolith arm (report v1)

Branch `p5e/fix1`, head **`66621767`**, base `44f91c74` (P5e wave-2 integration). Tree
`~/w7/p5e-fix1`. One commit. Three files: `MG_Backend/DirectGLES/Managers.cpp`,
`MG_IntegrationTest/CMakeLists.txt`, and a new `MG_IntegrationTest/Scenarios/MonolithAttachmentClearScenario.cpp`.

## 1 Root cause

The push build has TWO server arms and P5e only repaired one of them at this seam. Since P4a
(D-C2) the push build resolves framebuffer ATTACHMENTS from the applier record in **both**
arms — `BackendFramebufferObject::SyncToBackend(const SharedPtr<FramebufferObject>&, target)`
computes `pushedRecord` gated on `FramebufferSubsystemEnabled()` **alone**, with no transport
test — and under `Transport == Monolith` that object overload is what `SyncCurrentFBO` →
`SyncCurrentFBOByRecord` reaches (`DirectGLES.cpp:4304`, the arm below
`FramebufferRecordArmIsMandatory()`), which is exactly the frame the `hs_err` names. Package fb
then dropped `SyncAttachmentSurface`'s frontend attachment parameter and pointed its storage sync
at tx2's `SyncMipmapsToBackendByHandle`, whose contract is "the noted handle routes the body onto
the record arm". That contract is false on the monolith arm: the record arm inside
`SyncMipmapsToBackend` is itself selected by `MG_Config::Transport != Monolith` (the texture-VIEW
test at `Managers.cpp:7397-7412` and `RequireImageBindableStorage`'s re-dirty transition at
`:7458`, both tx2's ruling-1 arms), so with a null `SharedPtr` and monolith transport control fell
through to `stateTextureObject->IsTextureView()` and dereferenced null — the SIGSEGV at
`libMobileGL.so+0x80b57c` in `Lightmap.<init>` → `clearColorTexture` → `glClear`. fb's report §7
enumerates the push-monolith deltas it took deliberately (the two cross-checks, the N-6 refusal)
and this one — the storage sync itself — is not among them; it is an unnamed delta, and a fatal
one. The renderbuffer half of the same function has the same arm error with a milder symptom
(§3.2). **Everything in the task brief's diagnosis is confirmed**, with one correction: the
governing arm selection that is missing is not "the monolith arm must not use the record-shaped
attachment sync" (that is P4a's design and predates ID-81) but "the monolith arm must not use the
by-handle STORAGE sync", which is the thing ID-81 actually governs.

## 2 The fix, and why this one

Three edits, all in `Managers.cpp`.

**(a) `SyncAttachmentSurface` selects the storage sync by transport, and the monolith arm keeps
its frontend object** (`Managers.cpp:10068-10230`). The frontend attachment comes back as a
`const FramebufferAttachmentObject*` parameter — null on the handle arm, `&attachmentObject` from
the object-form walk — used for **nothing but** the storage sync:
`Transport != Monolith` → `SyncMipmapsToBackendByHandle(surface.Res)` / RBO
`SyncToBackendByHandle(surface.Res)` exactly as §5.4 prescribes; `Transport == Monolith` →
`SyncMipmapsToBackend(attachment->GetTexture())` / `SyncToBackend(attachment->GetRenderbuffer())`,
which is `f6cfcbd3`'s text token for token. Which arm is taken is decided by `MG_Config::Transport`
and never inferred from the pointer, so the two statements cannot drift.

This is candidate **(a) of the brief, narrowed to the seam ID-81 owns**, plus candidate **(c)** as
its backstop. I did not take (a) at the FBO sync site (gating `pushedRecord` itself on the
transport): that would revert P4a's record-driven attach shape in the push-monolith build,
silently moving draw-buffer decode, the four cross-object masks and the empty-point test back onto
the frontend for the arm the phone ships on — a far larger behaviour change than the defect, and
one that would reopen review N-6's hole (attach decided by the record, detach by the frontend).
I did not take **(b)** (resolve the frontend object from the registry inside
`SyncMipmapsToBackendByHandle`) because the registry *cannot reliably answer*: `Entry::stateRef`
is written only by the minting `GetOrCreate(StatePtr)`, and this path adopts its twin through
`AdoptTwinByHandle`, which sets no `stateRef` — a texture that reaches the driver first as an
attachment would get a null from `StateForHandle` and we would be back where we started, one
indirection later. The caller *has* the object; asking it is honest and total.

**(b) The null-frontend contract is named and enforced** (`Managers.cpp:6972-7017`).
`RefuseNullFrontendTextureOffTheHandleArm(entry, notedHandle)` is one shared check behind the new
`MGB_TEXTURE_NULL_FRONTEND_REFUSED` macro, spelled by all three sync bodies
(`SyncMipmapsToBackend`, `SyncTextureParamsToBackend`, `SyncBuiltinSamplerToBackend`) in place of
the bare `!obj && MGB_TEXTURE_HANDLE_ARM_OFF(*this)` test. Semantics: no handle noted → decline as
before; handle noted **and** (`Transport != Monolith` **and** `TextureResourceSubsystemEnabled()`)
→ proceed onto the record arm; handle noted on any other arm →
`Fatal{RoleViolation, "texture-handle-arm"}` and `std::abort()`. So the shape becomes **loudly
named wherever it is not impossible**, which is what the brief asked for and what makes the
red-once below a named abort rather than a SIGSEGV. G1: the `#else` branch defines the macro to
`(true)`, so the pull build's preprocessed text is `(!obj && true)` — identical to before, no call
added (the D-P rule `MGB_TEXTURE_HANDLE_ARM_OFF` was written for).

**(c) The renderbuffer arm is arm-selected too**, for a reason that is a behaviour delta rather
than a crash: `BackendRenderbufferObject::SyncToBackendByHandle` dereferences nothing, but it
deliberately drops the `MGB_CTX->RecordError(OutOfMemory, …)` the object form raises, "because on
this arm there is no application on this side of the wire to report it to". On the push-monolith
arm there is one, so the monolith arm keeps the object form.

## 3 The audit of the same class

The class: *a by-handle seam that passes a null/empty frontend object and relies on a record arm
being selected*. I walked every one in `MG_Backend/DirectGLES` and checked each call site's arm
selector by hand.

**3.1 Defective, fixed.** `BackendTextureObject::SyncMipmapsToBackendByHandle`, single call site
`SyncAttachmentSurface` (`Managers.cpp:10102`) — the crash.

**3.2 Same arm error, milder, fixed.** `BackendRenderbufferObject::SyncToBackendByHandle`, single
call site the same function (`Managers.cpp:10201`) — §2(c) above.

**3.3 Checked and correct (transport-gated at every call site).**

| seam | call sites | selector |
|---|---|---|
| `TextureImpl::SyncTextureToBackendByHandle` (passes `kNoFrontendTexture` to all three sync bodies) | `DirectGLES.cpp:2950, 2958` (fb's two attachment lists) | `FramebufferRecordArmIsMandatory()` at `:3333` |
| | `:3230, 3243` (the unit work list) | `UnitTexturesByHandle()` at `:2821` |
| | `:3669` (image bind) | reached only from the record overload, selected at `:3824` |
| | `:10242, 10249` (blit expressed as a copy) | the record overload, selected at `:10303` / `:10378` |
| | `:11453, 11570, 11801` (CopyTex ×2, GenerateMipmap) | each its own `Transport != Monolith` |
| | `Managers.cpp:7125` (a view's storage owner) | inside `SyncTextureViewToBackendByRecord`, itself behind `:7397` |
| `BackendFramebufferObject::SyncToBackendByHandle` / `SyncReadBufferToBackendByHandle` | `DirectGLES.cpp:4265, 4267, 5905` | `FramebufferRecordArmIsMandatory()`; bodies are record-only and hold no frontend object |
| `BackendProgramObjectImpl::SyncToBackendByHandle` | `DirectGLES.cpp:5692` via `SyncCurrentProgramByHandle` | `ProgramHandleArm()`; it is a SEPARATE body from the object head, not the object body fed a null |
| `RequireImageBindableStorageByHandle` | `Managers.cpp:7459` | `:7458`, record-only body |
| `SyncTextureViewToBackendByRecord` | `Managers.cpp:7450` | takes a possibly-null object and guards it: `if (stateTextureObject) SyncTextureViewToBackend(...)` |

**3.4 Checked and correct by a different construction — null-tolerant AND transport-aware bodies.**
vi's and sb's buffer seams are the model this fix follows and they got it right:
`IsBufferDrawCleanByHandle(res, resource, nullptr)` makes its one frontend read conditional on
`askTheObjectWhetherItIsMapped = (Transport == Monolith)` *and* on `frontend != nullptr`, and
`EnsureBufferResourceForHandle(nullptr, res)` returns `resource->hostBytes` under a transport and
`bufferObject->MappedData()` only when it has an object. The record-arm callers are
`VertexInputReadsRecords()`-gated and the push-monolith arm passes the real object
(`DirectGLES.cpp:1172` vs `:1386`); sb's whole walk is behind `ResolveBufferBindingSubsystemArm()`,
which returns false under monolith.

**3.5 One case named rather than changed.** `Managers.cpp:8650`'s `kNoFrontendBuffer` (the
buffer-texture backing on the `texBufferByRecord` arm) is keyed on `pushedStorage != nullptr &&
!MGB_TEXTURE_HANDLE_ARM_OFF(*this)` — on a NOTED HANDLE, not on the transport. It is unreachable
under monolith only because every writer of that note is transport-gated (§3.3), i.e. it is safe
by a chain rather than by a statement. It does not crash (the callee is null-tolerant); what it
would lose on the monolith arm is the live `MappedData()` base for a mapped buffer texture. I left
it alone rather than widen this fix's blast radius. **Ruling wanted**: whether the handle-note
should be re-stated as a transport test there, or whether "a handle is only ever noted under a
transport" is to be made an invariant with a check of its own.

**3.6 Not in this class, checked anyway.** `ScopedDetachedTextureFramebufferAttachments`' handle
arm and `ForEachLive`+`StateForHandle` (`DirectGLES.cpp:11078`) hold no null across a sync;
Magma's `VulkanRenderer.cpp` is out of scope (ID-90 keeps it lockstep) and untouched.

## 4 The coverage hole, and what closes it

**The hole is bigger than "no lane entry clears a texture-attached FBO" (ID-107), and it is
structural.** The gate runs `-L unit` and `-L integration-split`. **Every** `integration-split`
entry exports `MOBILEGL_TRANSPORT=inproc`, which selects `FramebufferRecordArmIsMandatory()` and
therefore `SyncToBackendByHandle` — the arm that never enters the object overload at all. And
every P5-era scenario that *does* clear a texture-attached framebuffer (`F1WireScenario` and its
family) calls `SplitRuntimeSkipReason()` in `SetUp` and **skips itself the moment the runtime is
not split**, so the ambient monolith registration of those files runs zero cases. The push build's
second runtime arm — the one the phone ships on — had no gated entry at all.

Measured on this tree, at HEAD's behaviour: `ctest -L integration-gpu` (the ungated monolith lane)
is **1157 passed / 137 failed of 1294, every failure a DirectGLES SEGFAULT**. The information was
there; nothing the gate runs looked at it.

**What I added.** `Scenarios/MonolithAttachmentClearScenario.cpp`, three cases in the shape the
existing scenarios use (fixture + attach + `glClear` + `glReadPixels` against pinned channel
values): a **mutable** 2D texture attachment (`glTexImage2D`, the Lightmap shape, the arm that
crashed), an **immutable** one (`glTexStorage2D`, so a regression confined to level definition
cannot account for a green), and a **renderbuffer** (the sibling arm of §2(c)). The assertion is
the pixel, not "it did not crash": a clear against a driver framebuffer whose attachment point was
never filled raises `GL_INVALID_FRAMEBUFFER_OPERATION` and writes nothing, which is the silent
form of the same defect and fails here too.

**How it reaches the gate.** A new `DirectGLES.PushMonolithArm.` block in
`MG_IntegrationTest/CMakeLists.txt`, registered inside `if (MOBILEGL_BUILD_DISAGGREGATED)`, with
`MOBILEGL_TRANSPORT=monolith` as a ctest ENVIRONMENT property (review N-6's reason, the
`PersistentMapArm` pair's precedent: a job-level `inproc` export must not be able to turn the
other-arm lane into a fourth copy of the split one) and `LABELS "integration-gpu;integration-split"`.
**The `integration-split` label on a monolith entry is a deliberate reading** and the one thing in
this package an integrator may want to overrule: that label does not mean "runs inproc", it means
"the set the disaggregation gate runs", which is why the block above it argues that a build without
`-DMOBILEGL_BUILD_DISAGGREGATED=ON` must match nothing under it — still true, since my block is
inside that `if`. ID-81 gives the push build two arms; if only one of them is in the gated set,
"the gate is green" keeps meaning "one of the two arms is green". The alternative, which I did not
take because it edits shared tooling outside this tree, is to add `-L integration-gpu` (or a third
label) to `~/w7/notes/tools/p5e_gate.sh`.

`integration-split` is now **116** entries (113 + 3). The scenario is also registered ambiently, so
`DirectGLES.MonolithAttachmentClearScenario.*` and `DirectVulkan.MonolithAttachmentClearScenario.*`
exist in `integration-gpu` (9 entries in total, all green).

### 4.1 Red-once, executed and quoted verbatim

**R1 — the arm selection reverted, the named refusal kept.** `SyncAttachmentSurface`'s
`if (MG_Config::Transport != Monolith)` replaced by `if (true)`; rebuilt.
`ctest -R 'PushMonolithArm.MonolithAttachmentClearScenario'` → **33% tests passed, 2 tests failed
out of 3**:
```
	3457 - DirectGLES.PushMonolithArm.MonolithAttachmentClearScenario.AClearOfAMutableTextureAttachmentReachesItsPixels (Subprocess aborted) integration-gpu integration-split
	3458 - DirectGLES.PushMonolithArm.MonolithAttachmentClearScenario.AClearOfAnImmutableTextureAttachmentReachesItsPixels (Subprocess aborted) integration-gpu integration-split
```
and the abort is named, from the entry's own log (`MOBILEGL_LOG_FILE_PATH`, because `MGLOG_F` does
not reach ctest's captured stdout):
```
[09:41:35] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{RoleViolation, "texture-handle-arm"} - SyncMipmapsToBackend was handed a NULL frontend texture for handle {15, 0} on an arm that cannot answer without one. The by-handle entries pass null on the strength of a noted handle, and the record arm they rely on is selected by Transport != Monolith AND the texture-resource subsystem bit (CONTRACT-P5E §5.8, ID-81); neither holds here, so the body below would read a frontend object that does not exist
```
The renderbuffer case stays green, which is the control: that arm's by-handle form dereferences
nothing.

**R2 — both halves reverted (= HEAD's behaviour), the device's own crash.** The refusal macro
additionally reverted to `MGB_TEXTURE_HANDLE_ARM_OFF`; rebuilt. Running the case directly:
```
2258 Segmentation fault         ./MobileGLIntegrationTest --gtest_filter=MonolithAttachmentClearScenario.AClearOfAMutableTextureAttachmentReachesItsPixels
direct-run exit=139 (139 = SIGSEGV, 134 = SIGABRT)
```
and through ctest:
```
	3457 - DirectGLES.PushMonolithArm.MonolithAttachmentClearScenario.AClearOfAMutableTextureAttachmentReachesItsPixels (SEGFAULT) integration-gpu integration-split
	3458 - DirectGLES.PushMonolithArm.MonolithAttachmentClearScenario.AClearOfAnImmutableTextureAttachmentReachesItsPixels (SEGFAULT) integration-gpu integration-split
```

**R3 — the whole monolith lane, before and after.** Taken at R2's state and again at the restored
head, same build flags, same machine:

| `ctest -L integration-gpu` | result |
|---|---|
| HEAD's behaviour (R2) | **1157 / 1294**, 137 failed — 137 SEGFAULT, 0 Failed, 0 aborted, 0 timeout, all `DirectGLES.` |
| with the fix | **1294 / 1294** |

The 137 span **38 distinct scenarios** (36 pre-existing plus my two new prefixes) —
`OrientationScenario`, `PrimitiveRestartScenario`, `DepthStencilReadbackMatrixScenario`,
`SnormAttachmentScenario`, `ThreeChannelAttachmentScenario`, `SampledSetStalenessScenario`,
`PixelStoreSweepScenario`, `FramebufferChurnScenario`, `P4aSeamAuditScenario`,
`CopyImageLayeredScenario`, `GuiBatchScenario`, … — every one of them a case that renders to or
clears a framebuffer holding a texture attachment. So the
regression was never confined to the Lightmap path; it broke the monolith arm wholesale, and no
gated lane entry touched it.

**R4 — restored, markers clean.** No `RED-ONCE` marker survives in the tree (`grep`, checked
before the commit); the committed diff is the three files in §0.

## 5 Gate

| step | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2250 / 2250** |
| `isplit` (`integration-split`) | **116 / 116** (113 + the 3 new entries) |
| `gens` | all seven rc=0; the one doc-citation line (`CONTRACT-P5.md:524 Core.cpp:39 ambiguous`) is the pre-existing one every package has reported |
| `one` (the new entries) | **9 / 9** on `MonolithAttachmentClearScenario` (3 `DirectGLES.`, 3 `DirectVulkan.`, 3 `DirectGLES.PushMonolithArm.`) |
| `integration-gpu` (not a gate step; measured for §4.1 R3) | **1294 / 1294** |
| `strict` | 7 passed / 109 failed of 116. The 3 new entries pass (they are monolith, so no BARRIER-PULLED row is reachable); the 113 pre-existing entries are unchanged, and cannot be otherwise — every edit in this commit either selects the monolith arm or, under a transport, returns exactly what the pre-fix expression returned. No `Fatal{UnmigratedPipeInput, …}` marker moved. |

## 6 What I could not settle

1. **The `integration-split` label on a monolith lane** (§4). My reading is that the label names
   the gate's set, not the transport, and that ID-81's second arm has nowhere else to live. The
   clean alternative is a gate-script change (`-L integration-gpu`, or a new `integration-arms`
   label) in `~/w7/notes/tools/p5e_gate.sh`, which is outside this worktree and shared with every
   other package, so I did not touch it. **Integrator's call.**
2. **The 137 monolith SEGFAULTs were latent for the whole of wave 2** and no package's gate could
   have seen them, because none of `build`/`unit`/`isplit`/`strict`/`gens` runs the monolith
   integration lane. Whether the phase should adopt `integration-gpu` as a standing gate step —
   it is ~10 minutes at `-j 4` — is a phase-level decision, not fix1's.
3. **`Managers.cpp:8650`'s handle-note-keyed arm** (§3.5): safe by a chain of transport-gated
   writers rather than by its own statement. Left alone deliberately.
4. **No pull-build (G1) compile.** The gate configures the split flavour only, as fb also
   reported. The argument that the pull build's bytes do not move is textual and I state it rather
   than measure it: `SyncAttachmentSurface` lives inside `#if MOBILEGL_PIPE_PUSH` and is not
   compiled there at all; the new refusal is inside the same `#if`, and its `#else` macro is
   `(true)`, so the three guards' preprocessed text in the pull build is the pre-fix1 text token
   for token. A `-DMOBILEGL_PIPE_PUSH=OFF` configure would settle it.
5. **No device run.** I have no adb and did not take one. **Request:** one push-monolith run of
   Minecraft 26.3-rc-3 on the Redmi at `66621767` to confirm the startup crash is gone and the
   frame is correct, and — if it is cheap — one inproc run to confirm the split arm is still the
   140-145 fps / 852-draws-per-frame shape ID-107 recorded, since this commit touches a line that
   both arms read (the arm selection itself).
