# P4a package D — `p4a/esprytobj3`, the VERIFICATION ROUND on the integrated tree

Branch `refs/heads/p4a/esprytobj3`, worktree `/home/swung/w7/p4a-esprytobj3`, base
`refs/heads/feat/disaggregated` = **`17396216`** (contract c0..c0e + wire + clientsp v3 + clientfb v2 +
esprytobj v2's thirteen replayed commits). **HEAD `56453191`**, three commits, **not pushed**.
Files touched: `MG_Backend/DirectGLES/{Managers.h, Managers.cpp}` and `MG_Test/SanityTest.cpp` — the
same three, nothing else. `DirectGLES.cpp` is package E's and is **byte-identical to the base**
(one temporary experiment, run and reverted — §4.1).

**Headline.** The default arm reaches **485 / 491** on the DirectGLES lane, the six failures are the
armless `.Handles` aborts that package F's pin move closes, and the eight-family refusal census over
the whole lane is **ZERO for all eight**. Four seam defects were found; two are fixed here, one needs
one line from package E (written out verbatim in §4.1, and proved to close the seam), and one is a
**phase-level defect the integrator must rule on: at the default mask the DirectVulkan backend has no
texture-upload consumer, so 66 of its 475 integration cases and 40 of 40 of its retrace cases are
red** (§4.2).

---

## 1. The three commits

| # | hash | what |
|---|---|---|
| v1 | `84c0edbd` | `[Fix] (Espryt): close the nine re-review minors - the framebuffer refusal now claims only what it can, the adoption refusals have a reachable caller, the carried region offsets and the sub-data resource byte are cross-checked, the remint-pull marker fires only when a level is owed, and a record-empty point the frontend still holds is refused` |
| v2 | `aec41e7d` | `[Fix] (Espryt): take the sampler CSO record from the handle the caller carries - a content-addressed record can never be named by the twins own identity handle, so every bound sampler objects parameters were refused on the integrated tree` |
| v3 | `56453191` | `[Fix] (Espryt): stop calling a texture that was never glTexParameter-ed a malformed record - ParamsSerial 0 means no set_texture_params was ever applied, so there is no built-in sampler to push` |

The integrator fast-forwards `p4a/esprytobj3` (its parent IS `17396216`; no rebase, no cherry-pick,
no conflict — the ID-34 recipe was for the pre-integration replay and is spent).

---

## 2. N-1, N-2 and the seven minors

### N-1 — the refusal line said something it cannot know (`Managers.cpp:9244`)

The message and the block comment above it are rewritten. What the reorder actually buys is stated
instead: **the refusal itself has no driver effect** — the twin no longer mints and binds a driver
FBO as a side effect of declining — and the comment now names the two callers that bind anyway
(`SyncAndBindFramebufferObject` = `SyncToBackend` then an unconditional `Bind(target)`,
`DirectGLES.cpp:3292-3293`, the path all five DSA entry points take; and `BindCurrentFBO`, which binds
whether or not `SyncCurrentFBO` synced). New text:

```
MGPipe: framebuffer %u has no applier record on the handle arm, so it is not configured here;
nothing is bound by this refusal, but the caller's own bind still targets it (handle {%u, %u})
```

The common stem `no applier record on the handle arm` is preserved, so every grep written against
the family keeps matching. Package F's dependency-refusal matcher is unaffected (it matches the
`REFUSING the dependent bit` sentence, which did not change).

### N-2 — the census is EIGHT families, and it was run as eight

`esprytobj-v2.md` §4 named six; the review found two more. All eight, at HEAD:

| # | site (at HEAD) | sentence (stem) |
|---|---|---|
| 1 | `Managers.cpp:6381` | `texture %u … its storage and uploads cannot be driven from the pushed descriptor` |
| 2 | `Managers.cpp:7675` | `texture %u … its built-in sampler cannot be pushed` |
| 3 | `Managers.cpp:7906` | `texture %u … its parameters cannot be pushed` |
| 4 | `Managers.cpp:9021` | `framebuffer %u … its read buffer cannot be pushed` |
| 5 | `Managers.cpp:9244` | `framebuffer %u … it is not configured here` (N-1's new wording) |
| 6 | `Managers.cpp:12086` | `program %u has no shader-CSO applier record …` |
| 7 | `Managers.cpp:12354` | `sampler %u … its parameters cannot be pushed` |
| 8 | `Managers.cpp:12630` | `renderbuffer %u … its storage cannot be allocated from the pushed descriptor` |

Seven share the stem `no applier record on the handle arm`; `#6` needs its own pattern
(`no shader-CSO applier record`). §3.2 has the measured counts.

### The seven minors

| # | disposition |
|---|---|
| **N-3** `AdoptTwinByHandle`'s refusals unreachable | **Fixed by splitting the helper.** `PipeTwinHandleIsAdoptable(registry, handle, kindName)` (`Managers.cpp:3562`) now carries the two loud refusals and `AdoptTwinByHandle` (`:3588`) is `PipeTwinHandleIsAdoptable` + `GetOrCreateByHandle`. Both call sites (`SyncAttachmentSurface`, texture and renderbuffer arms) call the predicate **before** consulting the frontend, because the record's handle is the untrusted input and `HandleOf(frontendObject)` is the corroboration — v2 asked them the other way round, which made the slot bound and the backward-generation refusal unreachable by construction. Asking first is also side-effect free, which matters for the same reason N-1 does: `GetOrCreateByHandle` can reset an incumbent twin and a handle the predicate rejects must not reach it. |
| **N-4** the remint-pull marker on the *prevention* path | **Fixed.** `MGPipeUnmigratedEmulation("texture-remint-pull")` moved out of `RequireImageBindableStorage`'s prologue and into the level loop, behind a `markedRemintPull` latch so it is raised **once per transition, only when a level is actually owed** (`Managers.cpp:5302`, `:5327`). A mask-only respecify that arrives before any level is defined — exactly what `ImageBindableHint` exists to make happen — no longer counts (and, under P9, no longer aborts on) the emulation it prevents. Still one `MGPipeUnmigratedEmulation` occurrence in the file, so E's G13b census (`DirectGLES.cpp:4` + D's 1 = 5) is unchanged. |
| **N-5** raw `SrcOffset` | **Fixed, symmetrically with the strides.** `Managers.cpp:7103-7131`: every region's `SrcOffset` is compared against the number `rectShadowPtr` derives from its own origin and the level's two pitches; on disagreement the carried offsets are **dropped** (every use site falls back to the derived pointer, which is what the legacy arm sends) and one named `MGLOG_E_ONCE` says so. `MGB_RECT_SRC_PTR` consults the same `pendingOffsetsAgree`. The comment also **corrects v2's m-1**: there are three use sites and the FIRST is unreachable (`dirtyRectCount` is forced to 0 whenever `UnpackRingAvailable()`, and the staging loop requires `ringUsable && dirtyRectCount >= 2`); that shape is pre-existing, so the count is corrected rather than the loop deleted underneath it. |
| **N-6** the silent empty-surface arm | **Made loud.** `Managers.cpp:8763-8782`: `SyncAttachmentSurface`'s empty arm now refuses when the record says the point is empty and `!attachmentObject.IsEmpty()`. That was the one direction in which neither the frontend-decided detach nor the record-decided attach fired, and the caller then stamped the point's synced version with the previous owner's image still on the driver. Every other record-vs-frontend disagreement in that function was already loud. No false-positive risk: B's `MGPipeBuildSurface` returns the empty surface **only** when `attachment.IsEmpty()` (`FramebufferEmit.h:115-118`), i.e. exactly the case this refusal excludes — and it fired **zero** times across the whole lane (§3.2). |
| **N-7** high byte vs packed key | **Resolved in D's favour, with the reason, plus a cross-check.** `Managers.cpp:3800-3826` states it: D's question is per `(uploadTarget, level)` because that is the granularity its upload loop iterates and the only key its callers hold, while wire's `AccumulatePendingUpload` keys on the whole packed field because that is what arrived. Widening D's key would require its callers to invent a resource target; instead the low byte is **cross-checked** at all three match sites (find, consume, re-arm) against `record.Desc.Target`, which B fills from exactly the expression the sub-data's low byte comes from (`TextureEmit.h:223` / `:517` against `:1037`). A disagreement is one named line. This is also **`MGPResourceDesc::Target`'s first reader** in the package (v2 §2(6) recorded it as having none). It fired zero times. |
| **N-8** `HasDefinedContent` unread and unlisted | **Stated, with the argument, at `Managers.cpp:6298-6310`.** It is deliberately not read on the texture arm: a buffer's content is ONE fact about the resource (and the buffer family reads it three times), a texture's is per `(uploadTarget, level)` and its authority is the pending set, which survives a bail. A whole-resource "content is undefined" would say nothing the per-level loops do not already answer by finding no level to upload, and acting on it would mean throwing away levels the set still owes. It stays a respecify CLASSIFIER (it is storage-defining, so wire refuses to call such a respecify metadata-only) and nothing more on this arm. |
| **N-9** the G9 probe covers in-function deferral only | **Recorded in the case's own comment** (`SanityTest.cpp:3185-3191`): the probe drives `SyncTextureParamsToBackend` directly, so a regression that gates the CALL on a sampler view (in `SyncNeccessaryTextures`, or in E's per-unit walk) leaves it green; the caller-level half is F's G9 scenario. |

`MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED` (`Managers.h:2011`) **stays 0**: package E is not on
the integrated tree at all — `InvalidateFramebufferHandleArmMemos` does not exist in
`DirectGLES.cpp` at `17396216` (`grep`: zero hits). The flip is one line and belongs in the round
that follows E's integration; flipping it now does not link, which is the property the handshake was
designed to have.

---

## 3. THE FIRST REAL-PATH RUN

`ctest --test-dir build-push -L integration-gpu -j 8 --no-tests=error -R DirectGLES`, under
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, at HEAD.

| mask | result | note |
|---|---|---|
| **default `0x1fff`** | **485 / 491** | the six armless `.Handles` aborts, and nothing else |
| `0x1ff` | 485 / 491 | same six |
| `0` | 485 / 491 | same six |
| `0x3ff` | 485 / 491 | same six |
| `0x7ff` | **438 / 491** | the six + **47**: bit 10 set with bit 11 clear — §4.3 |
| `0xfff` | 485 / 491 | same six |
| DirectVulkan, default | **409 / 475** | **§4.2 — a phase-level defect, not D's** |
| DirectVulkan, `0x1ff` / `0x3ff` | 475 / 475 | |
| DirectVulkan, `0x5ff` | 409 / 475 | isolates the cause to bit 10 |

For reference, the same six masks on the **base** `17396216` (measured first, before any edit of
mine) were `483 / 485 / 485 / 485 / 438 / 483`: the default and `0xfff` arms each had **two extra
failures**, `SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture`
and `IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler`. Those two
are seam defect **S-1** and are closed by `aec41e7d`.

### 3.1 Classification of every default-arm failure

**(a) six armless `.Handles` aborts — package F's, expected until F lands.** Identical set at every
mask, in `build-push` and `build-verify` alike:

```
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin
DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.DestroyedVertexArraysReturnTheirVertexElementsSlots
    (Subprocess aborted)
```

Now proved from the log rather than by mechanism: each aborts with exactly one

```
FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, "MOBILEGL_PIPE_PUSH leaves
kMGPipeSubsystemTextureResources (bit 10) clear (or refuses it) and MOBILEGL_PIPE_LEGACY_MEMOS=0
disables the pre-handle texture cheap-gate trio, so there is no arm to run"}
```

which is the ctest `ENVIRONMENT` pin `MGL_ITEST_HANDLES_ARM_KNOBS = "MOBILEGL_PIPE_LEGACY_MEMOS=0"
"MOBILEGL_PIPE_PUSH=0x1ff"` (`MG_IntegrationTest/CMakeLists.txt:949`) doing exactly what D-K3 says it
must. F's pin move `0x1ff → 0x1fff` at `:949, 1065, 1075, 1149, 1183, 1188, 1219` and
`0x80000000000001ff → 0x8000000000001fff` at `:1070, 1080` closes all six.

**(b) NAMED REFUSALS of the eight families — ZERO.** §3.2.

**(c) a wrong picture with no refusal — none on the DirectGLES lane.** One exists on the
DirectVulkan lane and it is §4.2.

**(d) anything else — none.**

### 3.2 The eight-family census, and how it was taken

The console log sink is compiled out of these builds, so ctest's captured output carries no `MGLOG`
lines at all (I checked: `grep -c 'MGPipe' LastTest.log` = 0 over 491 tests). The library opens
`MOBILEGL_LOG_FILE_PATH` with `fopen(path, "w")`, so a `-j 8` lane sharing one path clobbers itself.
The census therefore runs **one ctest invocation per test name, `-P 8`, each with its own log file**
(`~/w7/notes/tools/wsl_p4a_refusal_census.sh`, left durable — E's round, F's round and the final
review all owe this measurement).

Default arm, 491 names, **477 logs** (the 14 without one are ctest `ENVIRONMENT`-pinned scenario
variants — `CsoContentAddressing.{On,Off}`, `PointSizeDemotion.*`, `UnlocatedIoBlocks.*`,
`ResourceSubsystemControl.{On,Off}`, `MapPersistentRoundtrips.*` — whose own property replaces
`MOBILEGL_LOG_FILE_PATH`; all 14 pass in the lane).

```
[0 lines / 0 tests] texture ... its storage and uploads cannot be driven from the pushed descriptor
[0 lines / 0 tests] texture ... its built-in sampler cannot be pushed
[0 lines / 0 tests] texture ... its parameters cannot be pushed
[0 lines / 0 tests] framebuffer ... its read buffer cannot be pushed
[0 lines / 0 tests] framebuffer ... it is not configured here
[0 lines / 0 tests] program ... no shader-CSO applier record
[0 lines / 0 tests] sampler ... its parameters cannot be pushed
[0 lines / 0 tests] renderbuffer ... its storage cannot be allocated from the pushed descriptor

stem 'no applier record on the handle arm' : 0
stem 'no shader-CSO applier record'        : 0
```

**Every `ERROR]: MGPipe` / `FATAL]: MGPipe` line in the entire 477-log corpus, deduped:**

```
  6  FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, ...}          <- (a), F's pins
  2  ERROR]: MGPipe: sampler N was synced without its unit's SamplerCso handle, so the
                     content-addressed record cannot be named and the object's own parameters
                     are used - the caller must pass MGPipeApplier().BoundSamplerStates[unit]
```

and nothing else. The 960 `FATAL]` lines the raw count reports are 477 × 2 ambient EGL
entry-point loads under llvmpipe (`eglSwapBuffersWithDamageEXT`, `eglUnlockSurfaceKHR`) plus those
six — the same ambient pair the re-review warned about.

The two remaining `ERROR` lines are **S-1's residual half**, which is package E's one-line call site
(§4.1) and is loud by design until E carries it.

**Counters, read off the same corpus:**

* `StaleFramebufferRecordLookups` — **0**. The counter is incremented only beside
  `PipeApply.cpp:2258`'s `MGLOG_E_ONCE("MGPipe: the framebuffer record at slot %u is generation %u
  and the lookup named generation %u…")`, and that sentence does not occur once. (Checklist item 3's
  assertion, taken over 477 tests rather than one scenario.)
* `RefusedResourceCalls` / `RefusedObjectCalls` — the applier's refusal paths are all loud; none of
  their sentences occurs. Wire's rule that `RefusedObjectCalls` stays 0 across every framebuffer call
  holds.
* D's own new cross-checks (`Res` vs `HandleOf`, `PushedSurfaceTextureTarget`'s
  `kMGPipeSurfaceNoTextureTarget`, the region stride and offset disagreements, the sub-data low-byte
  mismatch, N-6's record-empty point) — **all zero**. Every one of them is a seam assertion that has
  now been exercised against real records and stayed silent.

---

## 4. The seam defects

### 4.1 S-1 (FIXED here + one line owed by package E) — the sampler family could never find its record

**Evidence.** On the base, two scenarios failed and both logged
`MGPipe: sampler 1 has no applier record on the handle arm, so its parameters cannot be pushed
(handle {8, 0})` / `(handle {6, 0})`. `IntegerBorderColorScenario.SamplerParameterIivBorderColour…`
failed as `glSamplerParameterIiv component 3 returned 1065353216 instead of 3` — `0x3F800000`, i.e.
the FLOAT border colour: the driver sampler had never been programmed at all.

**The mismatch.** Reading side: `BackendSamplerObject::SyncToBackend` resolved the record with
`g_backendSamplerObjects.HandleOf(stateSamplerObject.get())` — the twin registry's own handle, minted
by `SlotTables.h:232` as `MGPipeSlots().Acquire(MGPipeKind::SamplerCso, stateObj->GetLifetimeId())`,
i.e. **identity-addressed**. Emitting side: C's cache allocates sampler CSOs **by content**,
`MGPipeSlots().Allocate(MGPipeKind::SamplerCso)` (`SamplerEmit.h:426`), and `create_sampler_state`
is only ever emitted at those handles. The two are different slots in the same kind band, so the
lookup could not hit — for any sampler object, ever. (`MGPipeEmitSamplerCsoCreate` exists in the
contract and in `PipeFill.cpp:1097` but has **no caller**: clientsp-v3 §8.7 declined the
constructor-time call site. Wiring it would not have helped, because `EmitSamplerCso` also acquires
from the content-addressed cache.) D's own comment three lines below the broken lookup already said
the record is content-addressed — the defect is that the handle did not follow.

**Fix, D's half (`aec41e7d`).** `SyncToBackend` takes the CSO handle from the caller:

```cpp
#if MOBILEGL_PIPE_PUSH
    void SyncToBackend(const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject,
                       MG_Pipe::MGPipeHandle pushedCso = MG_Pipe::kMGPipeNullHandle);
#else
    void SyncToBackend(const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject);
#endif
```

(push-only spelling: a defaulted parameter is still part of the signature, and widening it
unconditionally renames the symbol in the PULL build, whose admitted-change set is EMPTY.) With a
handle: resolve, serial-gate, refuse loudly if the record is missing — that is now a true seam
defect. Without one, two shapes, told apart by whether the object is in the twin registry:

* **not registered** → a sampler this backend minted for itself (`GetRawDepthFetchSampler`,
  `DirectGLES.cpp:207-218`), which the client has never seen and for which no record can ever exist.
  The object is the authority. Silent, permanent, correct.
* **registered** → an application sampler whose caller did not carry the handle: **one named line**,
  and the object's own parameters are used, which in monolith are the very values the client
  content-addressed — so the picture is right while the gap is visible rather than silent.

Both scenarios go green with that, and the census's two remaining `ERROR` lines are exactly this
named gap.

**The line package E owes (`DirectGLES.cpp:4021`).** Proved: applied in the tree, built, run,
**reverted** (`git status` clean, `git diff` against the base for that file is empty, rebuilt). With
it, both scenarios pass **and the sampler ERROR line disappears entirely** — the record path resolves
end to end. The call site is NOT inside a `#if MOBILEGL_PIPE_PUSH` block, so the patch needs the
guard:

```cpp
#if MOBILEGL_PIPE_PUSH
                                backendSampler->SyncToBackend(
                                    samplerObject,
                                    static_cast<SizeT>(unit) < MG_Pipe::MGPipeApplier().BoundSamplerStates.size()
                                        ? MG_Pipe::MGPipeApplier().BoundSamplerStates[static_cast<SizeT>(unit)]
                                        : MG_Pipe::kMGPipeNullHandle);
#else
                                backendSampler->SyncToBackend(samplerObject);
#endif
```

`GetRawDepthFetchSampler`'s call at `:217` stays one-argument and must: it has no unit and no record.

### 4.2 S-2 (NOT D's — the integrator must rule) — bit 10 breaks the DirectVulkan backend

**Evidence, three independent measurements:**

| lane | default `0x1fff` | `0x3ff` | `0x5ff` (adds bit 10) |
|---|---|---|---|
| integration-gpu `-R DirectVulkan` | **409 / 475** | 475 / 475 | 409 / 475 |
| `integration-verify` (whole label) | 778 / 844, and **all 66 failures are `DirectVulkan.Verify.*`** | — | — |
| retrace (`retrace_gate.py`) | **39 / 79** — DirectGLES **39 / 39**, DirectVulkan **0 / 40** | `minecraft-1.21.4-main-menu.DirectVulkan` **PASSES**, ssim 0.997 | — |

The failing set is the texture-upload shape clientfb-v2's appendix lists (`SwizzleAccessRoutine`,
`CopyImage*`, `ImageTargetKind.Loads*`, `NonCoreImageFormat`, `FormatlessImageBake`,
`PackedWordReadback`, `FramebufferChurn`): the texels never reach the driver.

**Cause**, and it is exactly ID-35 §5's ruling extended one backend further. Bit 10's client half is
**destructive**: `DrainTextureSubData` clears the frontend per-level dirty flag as soon as the
applier accepts the record, so the texels live only in `MGPipeResourceRecord::PendingUploads`, which
only a P4a server-side consumer reads. D wrote that consumer for **DirectGLES only**;
`MG_Backend/DirectVulkan` still asks `IsStorageDirty()`, finds it clear and uploads nothing.
`MG_Config::Features.PipePush` is a process-wide config bit with no backend dimension, and
`FamilyIsLive` (`PipeFill.cpp:885`) is `(Features.PipePush & subsystem) != 0 && (wired & subsystem) != 0`
— it cannot ask which backend is running.

**The patch the owning packages need** (D cannot write it; `MG_Impl/Pipe` is B's and `MG_Pipe` is A's):

1. **Contract (A)** — a predicate on the same shape as the existing precedent
   `MGPipeResourceOpsHaveSubDataResident()` (`PipeMutation.h:119`, already used by
   `BufferObject.cpp:439/494` to decide exactly this kind of "may I stop keeping the host copy"
   question): e.g. `Bool MGPipeTextureSubDataHasAServerConsumer()`, answered by something the backend
   registers at bring-up.
2. **clientfb (B)** — gate the acceptance-driven `mipmap->MarkStorageDirty(uploadTarget, level, false)`
   in `TextureEmit.h`'s `EmitOneLevel` on that predicate. Everything else about bit 10 stays; only the
   destructive half becomes conditional. B's own §5 negative control already showed that removing
   just that clear takes the default arm to 491/491.
3. **Interim, if the integrator prefers no contract change in P4a**: ship the default mask at
   `0x3ff` (or run the DirectVulkan lane and the retrace at `0x3ff`) and record bit 10 as
   DirectGLES-only for the phase. D-K2 should then carry the row explicitly: **bit 10 requires a
   server-side texture consumer in the running backend**, which is a stronger statement than the
   "bit 10 requires bit 11" row and is the only P4a bit that is not independently switchable.

This is the one thing on the tree that is a *wrong picture with no refusal* — DirectVulkan emits no
refusal because it has no P4a handle arm at all to refuse from.

### 4.3 S-3 (NOT D's) — D-K2's fourth row refuses server-side, but the client half is already spent

At `0x7ff` (bit 10 set, bit 11 clear) D's `ResolveTextureResourceSubsystemArm` refuses bit 10 and
falls back to the legacy arm exactly as ID-15 specifies — and the lane is **438 / 491**, the same 47
texture-upload failures. The refusal cannot restore a correct picture, because the CLIENT's emission
is gated on the operator's mask alone and has already cleared the frontend dirty flags by the time
the server declines. The same applies to F's `0x5ff` arm.

**Patch:** the client half of the fourth row. `FamilyIsLive(kMGPipeSubsystemTextureResources, …)` in
`MG_Impl/Pipe/PipeFill.cpp:885` (A's file, the contract owns the enum-coupled block) must also
require `kMGPipeSubsystemSamplers`, so that a mask with bit 10 and without bit 11 emits nothing
rather than emitting and being refused. D's server-side row stays as it is — it is the backstop, and
it is what stops the applier's `Fatal{ProtocolCorruption}` on a null `BuiltinSampler`.

**Note for package F:** its `0x5ff` scenario asserts the refusal *line* and the legacy arm running,
which is still true and still green in principle; but the surrounding lane at that mask is not, and
F's gates-review-v2 F-v2-m2 wording ("the 0x5ff arm goes GREEN") should be read as "the scenario",
not "the lane".

### 4.4 S-4 (FIXED here, mine) — `ParamsSerial == 0` was being read as a malformed record

Three cases logged `MGPipe: texture N's set_texture_params names the null handle as its built-in
sampler CSO; every texture owns a sampler object, so this record is malformed` — a ninth named
refusal shape, outside the eight:
`DepthStencilReadbackMatrixScenario.ReadbackLeavesNoGLStateBehind` (and its
`ForcedDepthStencilEmulation` variant) and
`UnboundImageDescriptorScenario.ABufferTextureWithNoAttachedBufferDoesNotLoseTheDraw`. All three
*passed*, so it was a decline with no visible consequence — and it was D's own misreading, not a
client defect: B never emits a null `BuiltinSampler` (`TextureEmit.h:618-626` drops the record
instead). The null was the value-initialised `MGPTextureParams` of a record for a texture the
application has **never given a parameter to**, so no `set_texture_params` had ever been applied.
`ParamsSerial` is the applier's own "have I ever stored one here". `56453191` reads it first and
declines silently in that state — which is the conclusion `SyncTextureParamsToBackend`'s own gate
already reaches (`m_syncedParamsSerial` starts at 0 too, so its compare skips).

---

## 5. The gate's "HandleRecycle (verify): 6 failed of 70", classified

```
ctest --test-dir build-verify -R HandleRecycle   ->  64 / 70   (6 Subprocess aborted)
ctest --test-dir build-push   -R HandleRecycle   ->  50 / 56   (the SAME 6)
```

They are **the `.Handles` 0x1ff pins of §3.1(a), byte for byte the same six names**, not an ABA
defect and nothing verify-specific: the same six fail identically in `build-push`, at every mask, and
each dies of `Fatal{PipeLegacyMemosDisabled}`. The ABA controls the round added at v2
(`SanityTest.cpp` m-9: every one of the five kinds gets the "slot came back at a MOVED generation"
leg) are in `-L unit` and are **green in all three builds**. Nothing here is D's and nothing needs a
fix; package F's pin move closes all six.

---

## 6. Lanes, gates and every number, at HEAD `56453191`

```
builds                                   build-linux rc 0, build-push rc 0, build-verify rc 0

G1  symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so
      --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0          rc 0
      .text 10806323 -> 10806323 (+0, +0.000%)
      27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
      27072 normalised names, 27072 unchanged

ctest -L unit  build-push     100% passed, 0 failed out of 1761
ctest -L unit  build-linux    100% passed, 0 failed out of 1761
ctest -L unit  build-verify   100% passed, 0 failed out of 1761

G2  names pull == push                          2727 == 2727, 0 diff lines (LC_ALL=C)
G14 vs ~/w7/p4a-before-ctest-names.txt          0 removed, +139 added  (== ID-38's post-merge
                                                number: this round adds no test names)

G5  scripts/p3a_untouched_regions.sh 37da3c3a HEAD                                rc 0
D-N scripts/p4a_untouched_regions.sh 37da3c3a HEAD   (F's script, from ~/w7/p4a-gates)  rc 0
      791b54a35a19b58c82a5b9143cf48aa9ee815c755ecc1d4fa8dae709eeaa20bf  StageBlocksIntoUnpackRing
      1d6f15c983bbf7e26bcb6bdfdc0f9bd940367d928b35480717ef22a4f663ef12  UnpackRingAvailable
      19e614afbed5c0af01517f5ad75926d42f60eae7e23385190989dd47d0132f4f  UnpackRingAllocate
      3ae7e3472c4a730d24549ee8b51b87eacf284dec1266b498cd5f3e056f787188  RecomputeBackendColorSlots
      822ca3b0ece4e662f9b462945f2697f6b14547d841d926ae94c2b35139f16e53  DepthStencilSamplingReadImpl
      266710e0fdc19756f31e27dcee536149c99fc9fe583752042135b3a9cde26366  ShouldUseCaveatTextureFormat
      == esprytobj-v2.md §4 and esprytobj-review-v2.md §4, sha for sha

G13 grep -rc 'pGLContext' MobileGL/MG_Backend    ->  MG_Backend/MGPipe/PipeInputs.h:1  (one file)
    MGPipeResourceOps block ∌ MG_State           ->  0
    MGB_* in any header                          ->  no hits

integration-gpu -R DirectGLES  (six masks)       485 / 485 / 485 / 485 / 438 / 485  of 491
integration-gpu -R DirectVulkan (default)        409 / 475     <- S-2
integration-verify -j 4, default, tcache_count=0 778 / 844, ZERO `Fatal{` lines
                                                 all 66 failures are DirectVulkan.Verify.*  <- S-2
retrace, default (79 cases, -j 4)                39 / 79  = DirectGLES 39/39, DirectVulkan 0/40 <- S-2
retrace, 0x3ff, minecraft-1.21.4-main-menu.DirectVulkan   PASS, ssim 0.997  (the A/B for S-2)

the DSA / named-framebuffer set  (checklist item 3)
  -R 'ClearThenReadPixels|Orientation|FramebufferChurn|DepthStencilReadback'   93 / 94
      the one failure is DirectVulkan.FramebufferChurnScenario (S-2); DirectGLES 100%
  -R 'FramebufferTest.NamedFramebuffer'                                        3 / 3
  StaleFramebufferRecordLookups == 0 over the whole 477-log census

the image-bindable remint set   (checklist item 5)
  -R 'ImageLoadStoreSso|FormatlessImageBake|ImageSizeAfterRespec|ImageTargetKind|
      SampledSetStaleness|TextureParamsWithoutASamplerView'                    59 / 76
      all 17 failures are DirectVulkan (S-2); DirectGLES 100%

the P4a family suites
  -R 'DirectGLESTextureSync|DirectGLESSlotTable|TextureEmit|FramebufferEmit|SamplerEmit|
      ImageEmit|ProgramEmit|ResourceEmit'                                      152 / 152

G9 white-box probe  DirectGLESTextureSync.AnAttachmentOnlyTexturesParametersReachTheDriverWith
    NoSamplerView    push @ 0, @ 0x1ff, @ 0x1fff, and the pull build                 4 / 4 PASSED
```

**ID-8's leak/death half on the DirectVulkan lane**: `integration-verify` ran under
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` with **zero `Fatal{` lines** in the ctest output and in
`LastTest.log`; the DirectVulkan failures are all S-2 pixel comparisons, none is a death-path,
exit-order or idempotence failure, and none aborts. At `0x3ff` the DirectVulkan lane is 475/475.

**G4's retain-mode texture-dirty-rect comparison (checklist item 9): it does not exist to run.**
`MG_Config::Features.PipeTexelRetainMb` is written by `ConfigLoader.cpp:278` from
`MOBILEGL_PIPE_TEXEL_RETAIN_MB` and has **no reader anywhere in the tree** (`grep -rn PipeTexelRetainMb`
→ the declaration and the loader, nothing else). So the consume-and-clear gate the review kept
carrying forward as "still has not run anywhere in this phase" cannot run in P4a as built. What the
verify lane DOES check is the field-by-field record comparison, and that is clean. This should be
recorded against G4 rather than carried again.

---

## 7. What the integrator must re-run, and what is owed

1. **Rule on S-2 (§4.2) before the push.** It is the only thing on the tree that renders wrongly, and
   it renders wrongly on a whole backend. Cheapest interim = default mask `0x3ff`, or gate B's
   dirty-flag clear on a backend predicate.
2. **S-3 (§4.3)**: the client half of D-K2's fourth row, in `PipeFill.cpp:885`. One condition.
3. **Package E owes one line** (§4.1, written out verbatim, guard included). After it lands the
   census's last two `ERROR` lines go to zero. E also then drops the `static` on
   `InvalidateFramebufferHandleArmMemos`, after which **one line in D's file**
   (`Managers.h:2011`, `MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED` → 1) closes DV-13; that flip does
   not link without E's half, which is the point.
4. **Package F**: the six itest pins `0x1ff → 0x1fff` (and the two `0x8000000000000...ff`), which is
   the whole of the default arm's remaining 6/491. F's `ObjectSubsystemControl` scenario does not
   exist on this tree yet, so the `0x5ff` / `0x9ff` dependency arms could not be run here; read F's
   `0x5ff` expectation as "the scenario", not "the lane" (§4.3).
5. **Re-run after E and F land**: the six-mask DirectGLES lane, the eight-family census
   (`~/w7/notes/tools/wsl_p4a_refusal_census.sh default`, ~4 minutes), `integration-verify`, and the
   retrace. Only the retrace and the DirectVulkan lane are affected by S-2.
6. **Checklist items 6 and 7 of `esprytobj-v2.md` §5** are closed by measurement rather than by
   inspection: the metadata respecify runs end to end on this tree (the image-bindable set is 100% on
   DirectGLES, `TextureParamsWithoutASamplerViewScenario` included) and the storage-shape seam was
   already closed by the re-review's own comparison of `TextureEmit.h:183-241` against
   `Managers.cpp`'s `MGB_STORAGE_*`.
7. **Not re-derived here**: DV-9's preprocessed-line count (withdrawn at v2; the property that
   matters, G1's 0/0/0/0 with `.text` +0, is measured above), the device A/B and the bench.

## 8. Deviations added by this round

| # | deviation | reason |
|---|---|---|
| **DV-16** | `BackendSamplerObject::SyncToBackend`'s second parameter exists only in the PUSH build (`#if`/`#else` on both the declaration and the definition) | a defaulted parameter is part of the signature, so an unconditional widening renames the symbol in the pull build and P4a's admitted-change set is EMPTY (D-P / G1). |
| **DV-17** | the twin falls back to the frontend object's parameters when no CSO handle is carried, LOUDLY for a registered sampler and silently for a backend-private one | §4.1. The silent arm is permanent and correct (`GetRawDepthFetchSampler`'s object is server-owned and can never have a record); the loud arm is the E-side gap and disappears with §4.1's one line. |
| **DV-18** | `MGPResourceDesc::Target` now has exactly one reader, and it is a cross-check rather than a decision | N-7. D still keys the pending-upload match on the upload half; the low byte is compared, never acted on. |
