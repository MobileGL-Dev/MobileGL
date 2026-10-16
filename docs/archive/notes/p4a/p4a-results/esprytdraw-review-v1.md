# P4a package E — `esprytdraw`. Adversarial review, v1

Reviewed: `refs/heads/p4a/esprytdraw` `98bdfa3e..0b00f7f0` (five commits) against the tag
`refs/tags/p4a/contract` (`08192d72`). One file, `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`,
+893/−38. Read-only; no experiment worktree was needed (every question below was answerable by
reading the tree, the contract headers and package D's `Managers.{h,cpp}` in `~/w7/p4a-esprytobj`).
All line numbers are **HEAD (`0b00f7f0`) line numbers in `DirectGLES.cpp`** unless a path is given.

## Verdict: **REWORK** (three bounded code fixes in E's own file + one item to hand to D)

The package is strong: the arm shapes are right, the memo re-keying is argued rather than asserted,
the two byte-identical D-N regions and P2's `RenderStateImpl` sha reproduce exactly (I recomputed
them, §7), the macro/`#undef` hygiene is clean, and §0's `MGPipeApplierReset` finding is real and
load-bearing for D. What forces a rework round is three places where the handle arm **narrows what
reaches the driver** in the one direction the package's own DEV-3 argument forbids, plus two seam
checks that are silent and therefore cannot be policed by the check §8 itself mandates.

Everything ID-15 called out — the silent declines — is *specified* below rather than counted as a
rework item, because ID-15 assigns those flips to E's verification round. The decline table (§2) is
the deliverable for that round.

---

## 1. Findings by severity

### MAJOR-1 — the image sweep's membership can be smaller than the set of units the driver holds

`SyncImageTextureBindings`, **2519-2537**. The handle arm replaces
`for (unit = 0; unit < unitCount; ++unit)` with `for (unit = ShaderImageStart; unit < min(Start +
Count, unitCount); ++unit)` and then `return`s, so **no unit outside the received window is visited
at all**.

Failure scenario. The sweep exists for exactly one reason, written at 2544-2556 and 2601-2612: an
image unit is established *eagerly* by `glBindImageTexture` and is never revisited, so a texture
re-minted under it (`RecreateBackendTexture`) leaves the unit pointing at a **deleted driver texture
name**. Program P binds an image at unit 5 and dispatches; program Q, which declares only unit 0,
is then used. If `set_shader_images` is the *program-resolved* set — which is what E's own text calls
its sibling at 1828 ("the resolved per-unit view set") and what makes the ContentHash suppressor
worth having — Q's window is `[0,1)` and unit 5 is skipped by the sweep. A re-mint between the two
dispatches then leaves unit 5 bound to a dead name; on Adreno a stale image binding is a write
through a freed allocation, and on Mali the dispatch is rejected with `GL_INVALID_OPERATION`. The
pre-handle sweep bound *every* unit precisely so this could not happen. The same code is reached
unconditionally from `PrepareForCompute` (**5267**), which is the dispatch path where image units
matter most.

Refutation attempted, and why it fails. (a) *"D-J2 says the var-tail window IS the bound."* D-J2 and
`PipeApply.h:461-464` are a rule about **record retention** — entries outside the window are not
cleared *in the applier*. They say nothing about which **driver** units a re-issue sweep must visit;
the correct membership for that is "every unit the driver currently has an image on", which is
`g_imageUnitHighWaterMark` and is server-owned by construction. (b) *"The client emits Start = 0 and
Count = the image high-water mark + 1"* (the comment at 2525). That is an assumption about package C
that is written nowhere in the contract, is not listed among E's own seven assumptions in §3 of the
result, and is not checkable on this tree. (c) The decisive one: this is the **identical failure
direction** E refused to accept for `keys.maxTouchedUnit` in DEV-3 — "a window that is larger costs a
walk over provably-empty units, while one that is SMALLER silently drops [work] … which no gate on
this tree can see". DEV-3's reasoning applies verbatim here and was not applied.

Fix (E's file, ~2 lines): sweep `[min(Start, 0), max(Start + Count, g_imageUnitHighWaterMark))`
clamped to `unitCount` — i.e. union the record's window with the backend's own high-water mark — or
drop the record window here entirely and keep the record for the *values* only (which is `e3`'s real
deliverable). Add A8 to §3 naming C's `Start`/`Count` semantics for `set_shader_images` so the rebase
has something to reconcile against.

### MAJOR-2 — the sampler walk leaves a stale `glBindSampler` on every unit outside the window

`BindCurrentUnitSamplers`, **4357-4363**. On the record arm a unit in `[0, maxTouchedUnit]` but
outside `[SamplerStateStart, SamplerStateStart + SamplerStateCount)` is `continue`d: neither bound
nor unbound.

Failure scenario. The pre-handle arm's `else { SamplerImpl::UnbindSampler(unit); }` (**4390-4394**)
exists with its reason in the source: *"a sampler object left on the unit by an earlier draw keeps
being applied, and on a multisample texture — which takes no sampler object at all — the draw is
rejected outright."* Draw 1 binds sampler S to unit 3 (window `[0,4)`); draw 2 uses a program whose
resolved window is `[0,1)`; unit 3 keeps S bound. If unit 3 then holds a multisample texture the
draw is rejected by the driver; otherwise it silently samples with the wrong filter/wrap/compare.
This is the same defect class the `else` branch was added to close, reintroduced on the new arm.

Refutation attempted. Same three as MAJOR-1, and additionally: *"a `Start` above 0 is the client
saying it is describing a sub-range."* Even granting that, the units **below** `Start` are then
undescribed and keep whatever an earlier draw left, which is the same hole. The only shape that is
safe is "the window is always `[0, maxTouched+1]`", and if that is the contract it should be asserted
here rather than assumed.

Fix (E's file, ~3 lines): `UnbindSampler(unit)` for units outside the window instead of `continue`,
which is what the record's absence means for a *driver* binding; or refuse the arm when the window
does not cover `[0, maxTouchedUnit]` and fall through to the frontend walk (a full decline, per the
package's own §1 invariant).

### MAJOR-3 — two of the three seam checks are silent, so §8's mandated grep cannot see them

The framebuffer seam check logs (**2713-2717**, `"A framebuffer record does not describe the binding
it names"`). The other two do not:

* **image**, `ResolveShaderImageRecord` **2369**: `if (view.Res != HandleOf(boundTexture)) return
  nullptr;` — the result's §4 calls this check *"not optional"*, and it is the one guarding the eager
  `glBindImageTexture` funnel against binding one texture with another's level/layer/format. It
  returns null in silence, which is indistinguishable from "the client half has not landed".
* **program**, `ResolveGlobalConstantsRecord` **3699**: `if (record->Desc.Cso != handle) return
  nullptr;` — described in the source as *"the seam where a mis-keyed emission becomes visible
  instead of becoming a wrong upload"*. It is not visible; it is a silent fallback to `MapUBO()`.

The package's own post-rebase item §8.2 says *"`grep 'does not describe the binding it names'` in the
itest and retrace logs must be empty; a hit means a client emission is mis-keyed and is a finding
against B/C"*. As built, that grep can only ever see the framebuffer seam. Two of the three seams are
unpoliceable, and a mis-keyed `set_shader_images` or `set_global_constants` from B/C would land as
"the arm did nothing", i.e. as a perf regression, not as a defect.

Refutation attempted: *"the checks still prevent the wrong pixels, which is their job."* True, and
that is why this is MAJOR and not CRITICAL — but a check whose whole purpose is to surface a
cross-package seam defect, in a phase whose integration plan is "each package gets a real-path
verification round on the integrated tree", must be observable. The framebuffer one shows the right
shape; the other two just need it.

Fix: one `MGLOG_E_ONCE` each with the same wording stem so one grep covers all three.

### MAJOR-4 (hand to D) — `InvalidateFramebufferHandleArmMemos()` is called *beside* the six call sites, not *inside* `InvalidateFramebufferBindingCache`

E adds the new call next to all six `FramebufferImpl::InvalidateFramebufferBindingCache()` sites in
`DirectGLES.cpp` (6402, 6920, 7022, 10979, 11426, 11962 — I verified all six are paired). But
`InvalidateFramebufferBindingCache` is defined in `Managers.cpp:7107` and has **three further
callers E cannot reach**: `MobileGL/MG_Test/SanityTest.cpp:2530` (`ScopedStateGuardMocks::
ResetShadows`), `:2778` and `:2799` (`ScopedBackendTwinMocks` ctor/dtor). Those three now clear the
pre-handle trio and leave `g_fboSyncedSerials` and `g_fboRecordsTrusted` stale across a GLES function
table swap — exactly the state a fixture installs mocks to control.

Harmless on E's tree (every arm declines). On the integrated tree with bit 9 set, a `SanityTest` case
that swaps the funcs table mid-test can be entered with `g_fboRecordsTrusted == true` and a memo
claiming a target is synced at the current serial, so `BindCurrentFBO` binds from a record while the
mock table is installed. Refutation attempted: *"`ScopedBackendTwinMocks` also clears on the way
out."* It clears the wrong half both times.

Fix belongs in D's file: call `InvalidateFramebufferHandleArmMemos()` from **inside**
`InvalidateFramebufferBindingCache` so no caller can forget. Until D lands, E should at minimum note
it in §3 as an eighth assumption. (E cannot fix this itself — `Managers.cpp` is D's for the phase.)

### MINOR-1 — the source claims the read buffer is applied "from the record's own `ReadSurface`"; it is not

**2801-2815**. The comment says *"the read buffer … is applied from the record's OWN ReadSurface.
That is what makes the read-buffer-shared-FBO defect class unrepresentable rather than merely
fixed"*. The code calls `backendObj->SyncReadBufferToBackend(currentFBO)` — the **frontend object**.
`MGPFramebufferState::ReadSurface` is read nowhere in the diff (I grepped the whole change: E reads
only `Fbo`, `IsDefault`, `Target`, `DrawBuffers[]` and `ContentHash`).

What the change actually achieves is real and worth having: the `lastUpdatedFBO` *pointer compare*
becomes a `record.Target == Both` **field test**, so the skip can no longer be produced by an
accident of loop order. That is "the defect class is now expressed as a field" — not
"unrepresentable". The distinction matters because the brief and ID-12 both lean on the structural
claim. Rewrite the comment, or read `ReadSurface` (which is the D-C1 endpoint anyway).

### MINOR-2 — ID-12's "E's `SyncAttachmentObject` reads `MGPSurface::TextureTarget`" is misassigned

ID-12 (DV-5) says *"B's rework fills it; D's rework moves the four cross-object masks onto it; **E's
`SyncAttachmentObject` reads it**"*. `SyncAttachmentObject` is `Managers.cpp:7142` (tag) /
`:7947` (esprytobj) — **package D's file**, which C.7 gives to D for the whole phase, and E's own
DEV-1 says so. E's diff contains no `MGPSurface` read at all. Package D's own source agrees the
ownership is confused: `~/w7/p4a-esprytobj/MobileGL/MG_Backend/DirectGLES/Managers.cpp:8603` says
*"the array itself retires with the frontend attachment objects, which is **E's**
`SyncAttachmentObject` work"*.

Integrator action: reassign the `MGPSurface::TextureTarget` reader to D explicitly, or grant E
`Managers.cpp`'s `SyncAttachmentObject` for one commit. As things stand nobody owns it and the c0c
field would land unread.

### MINOR-3 — `ForceBindCurrentFBO` stamps a record-keyed memo from a path that never consults the record

**4028-4035**. `StampSyncedFramebufferSerial(target, FramebufferSerial)` runs whenever bit 9 is set,
after `SyncAndBindFramebufferObject(fbo, target)` synced the **frontend object**. The memo it writes
means "this target reflects the applier's record as of this serial", and nothing here checked that
the record describes this binding.

Failure scenario I could construct, and its refutation: draw with FBO Y at serial S (record@S = Y,
memo stamped); `glBindFramebuffer(X)`; a DSA clear runs `ForceBindCurrentFBO(Draw)`, syncs X, and
re-stamps DRAW at S; `glBindFramebuffer(Y)`; next draw — the client suppresses the re-emission
(ContentHash unchanged), so the serial is still S, `FramebufferRecordMatchesBinding` passes (record Y
== binding Y) and `SyncedFramebufferSerialIsCurrent` skips Y's sync. **I could not make this go
wrong**: Y's driver FBO state was correct from the earlier sync at S, and any edit to Y would move
its ContentHash and hence the serial. What *is* wrong at that point is
`g_alphaWidenedDrawBufferMask`, which now describes X — but the pre-handle arm has the same hole, so
it is not a regression.

Recorded because it is a real invariant break with no demonstrable failure: the safe and cheaper
form is `g_fboSyncedSerials[target].valid = false` (one extra sync at worst) rather than a stamp that
claims something this path did not verify.

### MINOR-4 — the `PipeStats` gate cannot distinguish the two arms, so §8.1's "hit rate → 100%" is not measurable

**1869-1872** ticks `Gate::EsprytUnitBindingsEpoch` with `hit=true` when the records answer, and then
`return`s. The decline path falls through to the walk arm, which ticks **the same gate** with its own
`hit=true` (1887) / `hit=false` (1892). So a fully declining tree still reports a high hit rate on
that gate, and §8's *"`Gate::EsprytUnitBindingsEpoch` hit rate should go to 100%"* cannot separate
"the record arm engaged" from "the walk arm's memo hit". Tick `hit=false` on the record-arm decline
before falling through (or add a distinct gate) so the verification round has a real signal.

### MINOR-5 — the four local arm latches carry a D-K2 mirror sentence that ID-15 has since refuted

**221-250**. `EsprytDrawTextureResourceHandlesEnabled()` is `bit 10 && bit 7` and its comment states
the dependency directions "verbatim". ID-15 adds a **fourth** D-K2 row — **bit 10 requires bit 11** —
because `MGPTextureParams::BuiltinSampler` is a SamplerCso handle, only bit 11 mints sampler CSOs,
and a null there is Fatal. D's mirror in `esprytobj/Managers.h:631-636` still says the opposite out
loud (*"The mirror pairs (10 without 11, 10 without 9, 7 without 10) are all FINE and are said so out
loud"*), and D's rework has to change that sentence. E's four latches will be **deleted** at the
rebase (A4 / §8.7), so the missing row is not a runtime defect in E — but the comment must not be
copied forward, and the reviewer of the rebase should check that the deletion actually happened
rather than the latches being "kept and updated".

E's other three directions match D's mirror exactly: bit 11 → bit 10 (`EsprytDrawSamplerHandles`
238-246), bit 9 → bit 10 (`EsprytDrawFramebufferHandles` 230-236), bit 12 → nothing
(`EsprytDrawProgramHandles` 248-252). ✓

### Refuted — things I chased and could not turn into findings

* **`g_fboRecordsTrusted` staleness.** `BindCurrentFBO`'s handle arm trusts a latch set by the last
  `SyncCurrentFBO`. I checked **all ten** `BindCurrentFBO` call sites (4091, 5323, 7256, 7257, 8010,
  8105, 8708, 8754, 8770, 8783, 10359) and every one is preceded by `FramebufferImpl::
  SyncCurrentFBO()` in the same function, with no GL entry point in between. `BindCurrentFBO` has no
  caller outside `DirectGLES.cpp`. The claim holds.
* **Out-of-bounds `st.BoundSamplerStates[index]` at 4364** (no size check, unlike
  `ResolveShaderImageRecord`'s at 2367). Refuted by contract: `PipeApply.h:461-464` — *"Start + Count
  above the bound is `Fatal{ProtocolCorruption}`"* — and `kMGPipeMaxTextureUnits = 192` equals
  `TextureState::MAX_TEXTURE_IMAGE_UNITS = 192`, so `maxTouchedUnit <= 191`. Safe. The asymmetry with
  2367 is cosmetic.
* **`DrawBuffers` decode.** `record.DrawBuffers[i] >= 0` at **3739-3743** is correct: the field is
  `Int8 DrawBuffers[8]; // attachment index, -1 = NONE` (`MGPipeTypes.h:556` region). Had it been
  `Uint8` the broadcast count would have pinned at 8 on every draw. It is not.
* **Dangling `slot` pointers in the new `g_readFboTextureSyncList`.**
  `FramebufferObject::GetAllAttachmentObjects()` returns `const FramebufferAttachmentObjectArray&`
  (`MG_State/GLState/FramebufferState/FramebufferObject.h:131`), so `&textureObject` is stable, and
  `PairingsIntact` is applied to the new list exactly as to the two existing ones. No finding.
* **Exit-order (ID-8).** The new statics are `Array<SyncedFramebufferSerialMemo,…>` (POD), four
  `Bool`/`Uint64` scalars, and `Vector<UnitTextureSyncEntry>`; `UnitTextureSyncEntry` holds three
  **raw** pointers, so no new static holder of a frontend `SharedPtr` is introduced. Observation only:
  E's new D-F3 paragraph at **87-101** formally names the pre-existing
  `g_rawDepthFetchSamplerState` file-static `SharedPtr<MG_State::GLState::SamplerObject>` as the
  largest surviving `MG_State` usage under `MG_Backend/DirectGLES`. That *is* ID-8 "leak-at-exit
  storage" by the letter of the rule; it is pre-existing at 37da3c3a and owned by P3b/P4b, but now
  that it is written down the integrator should confirm P4a's `tcache_count=0` lanes still show zero
  Fatal rather than letting the new comment imply it was audited.
* **The unit texture sync list keyed on a serial that can miss a frontend rebind.**
  `keys.unitBindingsEpoch` becomes `Mix(SamplerViewsSerial, SamplerStatesSerial)`, which moves only
  when the *resolved* set moves, while the list's **membership** is still the frontend binding walk
  (DEV-4). I constructed: unit 5 empty → texture T bound to unit 5, program samples only unit 0, so
  the resolved set and hence the serial do not move, `maxTouchedUnit` unchanged, `PairingsIntact`
  sees no entry for unit 5 → the list is replayed without T. Refuted as harmless: T is not sampled by
  this draw, and any later program that samples unit 5 moves the resolved set and rebuilds the list.
  `PairingsIntact` (1938-1943) catches every *swap* on an existing entry, and
  `g_unitTextureSyncListMaxUnit` is still frontend-derived (DEV-3), so the two escapes the walk arm
  covered are both covered here. **Put it on the verification list anyway** (§5.4) — it is the one
  place where the four "free" re-keyings are not obviously equivalent.
* **A missing-record partial run in `SyncCurrentFBOByRecord`.** The `!currentFBO` branch at
  **2752-2756** `continue`s with `g_fboRecordsTrusted` already true and then `return true`, which
  violates §1's "a decline is a full fallback". Refuted as unreachable: `FramebufferRecordMatchesBinding`
  (2674-2681) maps `!bound` to `boundIsDefault = true`, so a non-default record with no bound FBO is
  already rejected at 2713. Left as a decline-table flip (D4) rather than a finding.

---

## 2. The decline-site table, and the verification-round flip for each

Classification: **M** = "the client has not switched this family on" (the mask says so) — legitimate,
stays silent. **S** = seam defect once the bit is on and B/C have landed — must become LOUD.
**K** = a memo-key selection, not a behavioural decline — stays silent.

| # | site (`DirectGLES.cpp`) | condition | today | class | verification-round flip |
|---|---|---|---|---|---|
| F1 | 2800 | `EsprytDrawFramebufferHandlesEnabled()` false | silent | **M** | none; becomes `FramebufferSubsystemEnabled()` (D) |
| F2 | 2709-2711 | `DrawFramebuffer.Fbo` **or** `ReadFramebuffer.Fbo` null | **silent** | **S** | `MGLOG_E_ONCE` naming *which* target is unrecorded, then decline. A half-described applier (one target only) must be loud too — it is the shape a partially-landed emitter produces |
| F3 | 2713-2717 | seam: record vs binding mismatch | **loud** ✓ | S | keep verbatim; it is the wording §8.2 greps |
| F4 | 2752-2756 | non-default record, nothing bound | loud, but `continue`s | S | `g_fboRecordsTrusted = false; return false;` — a full fallback, per §1 |
| F5 | 3906 | `record.Fbo` null under `g_fboRecordsTrusted` | silent | — | unreachable (trust ⟹ both handles non-null); make it `MOBILEGL_ASSERT` or delete |
| F6 | 3910-3915 | twin missing for a trusted record | loud | S | keep; parity with the pre-handle arm at 3969-3971 (both bind nothing) |
| F7 | 3735 | as F5, in the fragColor derivation | silent | — | unreachable; same treatment |
| F8 | 2067-2071, 2211-2215 | `recordKeyed` false → version key | silent | **K** | none — both key shapes are correct and are held in separate fields |
| F9 | 4032 | `ForceBindCurrentFBO` stamps unconditionally | n/a | — | MINOR-3: invalidate instead of stamp |
| T1 | 1864 | `EsprytDrawSamplerHandlesEnabled()` false | silent | **M** | none; becomes `SamplerSubsystemEnabled()` (D) |
| T2 | 1852 | `SamplerViewCount == 0 && SamplerStateCount == 0` | **silent** | **S** | `MGLOG_E_ONCE` + tick the gate `hit=false` (MINOR-4) before falling through |
| I1 | 2363 | latch false | silent | **M** | none |
| I2 | 2365 | `ShaderImageCount == 0` | **silent** | **S** *(when the program declares images)* | loud-once; a program with images and no pushed set is a seam defect |
| I3 | 2366 | unit outside `[Start, Start+Count)` | silent | **M** for the eager funnel | none here — but see MAJOR-1 for the *sweep* |
| I4 | 2367 | `unit >= BoundShaderImages.size()` | silent | — | unreachable by `PipeApply.h:461-464`; keep as defence |
| I5 | 2369 | **image seam**: `view.Res != HandleOf(bound)` | **silent** | **S** | **MAJOR-3**: `MGLOG_E_ONCE` with the same wording stem as F3 |
| I6 | 2529 | `ShaderImageCount == 0` → full sweep | silent | **M** | safe direction; keep silent |
| I7 | 2532-2535 | sweep window narrower than the driver's | silent | — | **MAJOR-1** |
| S1 | 4354 | latch false | silent | **M** | none |
| S2 | 4356 | `SamplerStateCount == 0` | **silent** | **S** | loud-once, then the frontend walk |
| S3 | 4361-4363 | unit outside the window → `continue` | silent | — | **MAJOR-2** |
| S4 | 4369-4372 | twin missing at a live handle | silent | **M** | none — parity with `ResolveUnitSamplerBackend`'s null path, and deliberately not memoised as a miss |
| P1 | 3695 | latch false / `program == nullptr` | silent | **M** | none; becomes `ProgramSubsystemEnabled()` (D) |
| P2 | 3672 | `index >= table->size()` | silent | **S** | loud-once, and distinguish the composite band (an out-of-range composite index is a contract bug, not a missing record) |
| P3 | 3674 | `!record.Live \|\| record.Gen != handle.Gen` | silent | **S** | loud-once — this is ID-8's stale-generation refusal and it is currently indistinguishable from "no record" |
| P4 | 3698 | `record == nullptr` | silent | **S** | folded into P2/P3 |
| P5 | 3699 | **program seam**: `Desc.Cso != handle` | **silent** | **S** | **MAJOR-3**: `MGLOG_E_ONCE`, same stem as F3 |
| P6 | 3700 | `GlobalConstantsVersion == ~0u` | silent | **M**, then **S** | legitimate before the first upload; once bit 12 is on and `GetUBOSize() > 0 && HasGlobalUboBlock()`, a never-uploaded record at a draw is a seam defect → loud-once |
| P7 | 3701 | `GlobalConstants.size() < GetUBOSize()` | **silent** | **S** | a short block image is protocol corruption, not a missing record — this one should be `Fatal{ProtocolCorruption}`-shaped, or at minimum loud-once and never silently fall back to `MapUBO()` |

Also outside the table: the D-E3 read-attachment sync (**2078-2115**, called at **2231**) is
deliberately **ungated** (DEV-2), so it runs on the push build at mask 0 as well. Accepted as
declared — but it means the object A/B's `MOBILEGL_PIPE_PUSH=0` arm is no longer byte-equivalent to
the pull build's behaviour. G1 is unaffected (pull symbols 0/0/0/0 verified).

---

## 3. The seven assumptions, cross-checked against D's actual API

Read from `~/w7/p4a-esprytobj/MobileGL/MG_Backend/DirectGLES/Managers.{h,cpp}` (package D v1,
`9bb95335`), read-only.

| # | E's assumption | what D actually provides | reconciliation at the rebase |
|---|---|---|---|
| **A1** | registry forwards `HandleOf`/`FindByHandle` but **not** `GetOrCreate(MGPipeHandle)` | **Correct at c0.** D **adds** `BackendPtr* GetOrCreateByHandle(MG_Pipe::MGPipeHandle)` — `Managers.h:453` — returning a **pointer**, not the reference `GetOrCreate(StatePtr)` returns, and D says the pointer form is deliberate | Not the one-liner §3 predicts. `SyncCurrentFBOByRecord`'s `GetOrCreate(currentFBO)` (2779-2786) becomes `GetOrCreateByHandle(record.Fbo)` **plus a null check** — and once it takes the handle, the `slot.GetBoundObject()` resolve above it and the seam check can go, which is what A2 anticipates |
| **A2** | `BackendFramebufferObject::SyncToBackend(const SharedPtr<FramebufferObject>&, FramebufferTarget)` and `SyncReadBufferToBackend(const SharedPtr<FramebufferObject>&)` keep their signatures through `d4` | **Confirmed unchanged**: `esprytobj/Managers.h:1754` and `:1759` are textually the tag's `:1548` / `:1553`. D re-keys only the bodies (`m_syncedRecordHashes` / `m_syncedFrontendAttachmentVersions`, `Managers.cpp:8590-8608`) | Nothing to do. The `#else` half of A2 does not fire |
| **A3** | `SyncTextureParamsToBackend` / `SyncBuiltinSamplerToBackend` / `SyncMipmapsToBackend` / `IsDrawSyncClean` keep their frontend-object signatures through `d2` | **Confirmed**: `esprytobj/Managers.h:1501, 1514, 1515, 1550` == tag `:1350, 1363, 1364, 1399`. **But** D adds record-keyed members beside them — `:1709` *"the record's `ParamsSerial` at the last `SyncTextureParamsToBackend`"* and `:1715` *"the BuiltinSampler CSO record's `Serial` at the last `SyncBuiltinSamplerToBackend`"* | Signatures hold, so E's new read-FBO list compiles unchanged. **New**: once D's cheap gates key on record serials, `IsDrawSyncClean` for a **read-only** attachment answers from a `ParamsSerial` that nothing on the read path ever advanced — re-verify the new list still does work after `d2`, or it becomes a no-op |
| **A4** | D-K3's four `Resolve<Family>SubsystemArm()` latches do **not** exist at c0 | **Correct at c0; D has them.** `esprytobj/Managers.h:644-647` declares `ResolveFramebufferSubsystemArm` / `ResolveTextureResourceSubsystemArm` / `ResolveSamplerSubsystemArm` / `ResolveProgramSubsystemArm`, and `:649-663` wraps them as **`FramebufferSubsystemEnabled()`**, **`TextureResourceSubsystemEnabled()`**, **`SamplerSubsystemEnabled()`**, **`ProgramSubsystemEnabled()`** | Delete E's four (221-252) and call D's four **wrappers** (not the resolvers — the wrappers hold the `static const` latch). Note the **names differ** from E's guess. D's version adds the `MGLOG_E` refusal and `StopOnArmlessPipeSubsystem`. Carry ID-15's fourth row (bit 10 requires bit 11) — D's mirror comment at `Managers.h:631-636` still asserts the opposite and is D's rework item |
| **A5** | `SamplerPassMemo::rows` stays `Array<BackendSamplerObject*, 16>`, `g_boundSamplersCache` stays a raw-pointer shadow | **Confirmed**: `esprytobj/Managers.h:2158-2169`, `kMaxEntries = 16`, `Array<SamplerImpl::BackendSamplerObject*, kMaxEntries> rows{}` | Nothing. DEV-5 stands, and D's own arm-census at `Managers.h:627-628` lists these rows as a *legacy* memo, which is consistent |
| **A6** | `MGPImageView::Access` is a `Uint8` with no documented encoding at c0 | **Confirmed** — `MGPipeTypes.h`'s `MGPImageView` is `{Res, Unit, InternalFormat(Uint32), Layer(Uint32), Level(Uint16), Layered(Uint8), Access(Uint8)}` with no encoding note | Unchanged by esprytobj. Stays a C item (ID-12 did not rule it). §8.5 is the right home |
| **A7** | `MGPipeApplierReset` advances all P4a working-state serials | **Confirmed by reading `PipeApply.cpp`**: `++FramebufferSerial; ++SamplerViewsSerial; ++SamplerStatesSerial; ++ShaderImagesSerial; ++ProgramBindingSerial`, and the records/windows are zeroed in the same function. E's §0 is correct and should go to D before its rework, as the result recommends | Nothing here |
| **A8** *(new — the review adds it)* | *unstated*: that `set_shader_images` / `bind_sampler_states` windows cover every unit the **driver** holds, not just the program-resolved ones | The contract says only *"the record is the last set as received"* and *"Start + Count above the bound is `Fatal{ProtocolCorruption}`"* (`PipeApply.h:461-464`). Nothing pins the window's membership | MAJOR-1 / MAJOR-2. Either C documents it in `ImageEmit.h` / `SamplerEmit.h` and E asserts it, or E stops narrowing the driver-side sweep and walk |

The two extra API facts worth carrying: D's `HandleOf`/`FindByHandle` both return
null/`kMGPipeNullHandle` when `EsprytSlotTablesEnabled()` is false (`Managers.h:415-429`), so every
one of E's identity checks degrades to "decline" rather than to "false match" if bit 7 is ever clear
— that is a genuine safety property of the current code and should survive the rebase. And
`Managers.h:381-385` states a pointer-invalidation contract for handle-arm results ("invalidated by
the next `GetOrCreate`/`Find`/`CollectGarbage`"); E's `twinSlot ? *twinSlot :
GetOrCreate(currentFBO)` at 2794-2796 short-circuits correctly, and the `backendTexture` reference
held across `ResolveShaderImageRecord`'s `HandleOf` call at 2398 is safe because `HandleOf` neither
grows nor sweeps. Both should be re-checked after A1's rewrite, since `GetOrCreateByHandle` *can*
grow.

---

## 4. D-E3, and whether G9's mandated red-before is obtainable (R1 of the gates review)

**The closure itself is correct and is in the right place.** `SyncReadFramebufferTextureAttachments`
(**2078-2116**, called at **2231**) walks the READ framebuffer's texture attachments and gives each
`SyncTextureParamsToBackend` + `SyncBuiltinSamplerToBackend` + `SyncMipmapsToBackend`, dedupes a
framebuffer bound to both bindings with a pointer compare against the draw FBO, carries both key
shapes exactly like the draw list, and reuses `PairingsIntact`. DEV-1's ownership argument holds:
`SyncAttachmentObject` is `Managers.cpp:7142` and pushes `SyncMipmapsToBackend` **and nothing else**
(I read the whole body), while the list that pushes parameters reads the DRAW slot alone. Closing it
in `SyncNeccessaryTextures` is both inside E's ownership and closer to the cause.

**Completeness.** Attachment-only textures: covered (either framebuffer). Image-only textures: *not*
covered and correctly so — an image-only texture goes through `SyncTextureObjectToBackend(…, true)`
in `SyncImageTextureBinding` (2394), which is the params path for that family. `CopyImage`-endpoint
textures: *not* covered — `ResolveCopyImageEndpoint` (8418-8425) syncs the endpoint itself and does
not depend on this list. So the closure is complete for the gap D10 names.

**Is G9's red-before obtainable through public GL? No — the gates package's "no" stands, and I can
say exactly why.** I traced all four parameter families the comment at **2020-2024** names:

* `GL_DEPTH_STENCIL_TEXTURE_MODE` affects **texture fetch only**. For a texture that is only a READ
  attachment it can be observed in exactly one place: MobileGL's own depth/stencil readback
  emulation, which *samples* the attachment. That path **sets the mode itself** immediately before
  sampling — `DirectGLES.cpp:9293` in `DepthStencilSamplingReadImpl`, and `:6954`, `:6967`, `:6987`
  in the replicate-blit path — so the application's value is overwritten and cannot be observed.
* Swizzle, LOD clamp and border colour are likewise fetch-only. `glReadPixels`, `glBlitFramebuffer`
  and `glCopyTexSubImage` read the attachment **image**, not through the sampler.
* The remaining route would be "the texture is later sampled, and a stale clean-stamp suppresses the
  params push". Refuted by reading `IsDrawSyncClean` (`Managers.h:1399-1417`): it requires
  `m_syncedTextureParamsVersion == t->GetTextureParamsVersion()` **and**
  `m_syncedSamplerVersion == samplerObject->GetVersion()`, neither of which `SyncMipmapsToBackend`
  advances. So the unit list pushes the parameters the first time the texture is sampled, and there
  is no window in which the gap is visible.

Concretely: **no public-GL case goes red on the tag and green here.**
`AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` as named cannot be written against the
GL surface. What *is* demonstrable, and what package F's G9 must become, is a **white-box** assertion:
an `MG_Test`/integration case that attaches a texture to the READ framebuffer only, sets
`GL_DEPTH_STENCIL_TEXTURE_MODE`/a swizzle, issues a `glReadPixels`, and asserts the **driver**
received the `glTexParameteri` — via a recording `GLESFunctionsTable` (the `ScopedBackendTwinMocks`
shape already in `SanityTest.cpp:2772+`) or by asserting on `BackendTextureObject`'s synced params
version. E's §8.3 should be rewritten to say that; as written it asks the integrator to run a green
against a red-before log that F cannot record. Flag to F and to the ROADMAP owner: `ROADMAP.md:20`'s
"落地前必须红" cannot be honoured for this item with a black-box test, and pretending otherwise
produces a vacuous green.

---

## 5. The other dimensions

**SyncCurrentFBO on the record (dimension 2).** Draw/Read/Both handled at 2757-2818; the `Both`
skip is now a field test (MINOR-1 on the comment). The `{0,1}` reserved-handle default path is
compared through `IsDefault` rather than `Fbo` (2764, and 2674-2681 says why) and resets the widened
and integer draw-buffer masks for the DRAW target only — matching the pre-handle arm at 2839-2849 and
`SyncAndBindFramebufferObject` at 3987-3999. `g_fboSynced{SlotVersions,ObjectVersions,Objects}` keep
their only readers on the pre-handle arm and are stamped only there; `g_fboSyncedBackendIdGenerations`
is *not* replaced — `g_attachmentBackendIdGeneration` and `g_backendContextGeneration` ride inside
`SyncedFramebufferSerialMemo` (2652-2664), which is the D-B3/D-O requirement and is right. The two
memo families never share a field, so a stale half can never be compared against a live one. One
behavioural divergence, benign and worth recording: with **nothing** bound, the pre-handle arm logs
`"No FBO is currently bound"` and skips (2834-2837), while the handle arm treats it as the default
framebuffer (2674-2677 maps `!bound` to default) and binds 0 — which is what `BindCurrentFBO`
(3943-3979) and `SyncAndBindFramebufferObject` (3987) already do, so the handle arm is the *more*
consistent of the two.

**The unit walks (dimension 3).** The epoch substitution is sound for three of its four consumers
(`ResolvedTextureBindingMemo`, `g_unitSamplerWalk*`, `SamplerPassMemo` — all program-resolved, which
is exactly what `set_sampler_views` covers) and approximately sound for the fourth
(`g_unitTextureSyncList`, whose membership is still the frontend walk — see §1 Refuted). No memo
fails to invalidate on a real change that I could construct. `maxTouchedUnit` is carried separately
in both the list key (1948, `g_unitTextureSyncListMaxUnit`) and the sampler-walk key
(`g_unitSamplerWalkMaxUnit`), so dropping it from the epoch loses nothing. The raw-depth-fetch
substitution stays server-side and is only re-*read* from the resolved view/CSO (87-101, `e4`); the
LOD-bias fallback and `EmulateTextureLodBias` are untouched, as §6 claims — I confirmed no diff hunk
reaches them.

**SyncCurrentProgram / the nine-clause rebuild (dimension 4).** `e4` touches only the fragColor
broadcast derivation (3727-3757) and the global-UBO source (4581-4656); the nine-clause condition and
`GetSyncedShaderCsoSerial()` are **not** re-keyed by this package — which is consistent with C.4 but
means dimension 4's "nine-clause rebuild condition reading `GetSyncedShaderCsoSerial()`" is *not* a
deliverable E landed. `FindShaderCsoRecord` (3658-3676) is the single composite-band reader, as
claimed, and `MGPipeIsCompositeShaderSlot` / `kMGPipeShaderCsoCompositeSlotBase` exist at
`MGPipeHandles.h:100-107`. Program deletion while bound is handled by P3 (`!record.Live ||
record.Gen != handle.Gen`), which is ID-8's stale-generation refusal — correct, but silent (table
row P3). Pipeline composites reach `ResolveGlobalConstantsRecord` through the same handle and are
indistinguishable to every other reader, which is D-H7 satisfied.

**G1 / G5 mechanism (dimension 7) — I re-verified, not just re-read.**
`awk '/^    namespace DepthStencilSamplingReadImpl \{/,…' | sha256sum` gives
`dfefc9964c82f23cb6b89292b2f155ebd2ac22ae442c18a959d38d5bc9063f53` at **both** `37da3c3a` and HEAD.
`RenderStateImpl` gives `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` at both,
== `~/w7/p4a-before-syncrenderstate.sha`. `git diff --stat 37da3c3a HEAD -- Utils.{h,cpp}
MultiDraw.cpp` is empty. `bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD` → rc 0. The pull-text
mechanism is macros, not duplicated functions, and every one is defined in **both** arms and
`#undef`ed: `MGB_IMAGE_{LAYERED,LAYER,LEVEL,FORMAT}` (2400-2408 / 2473-2476) and `MGB_UBO_BYTES`
(4596-4600 / 4656). `MGB_UNIT_BINDINGS_HANDLE_ARM` (1709-1770) is P3a's and is untouched. No macro
escapes its function. G1's 0/0/0/0 claim is mechanically credible; the integrator still re-runs
`symbol_report.py` on the integrated tree (§6).

**Emulations (dimension 8).** Four `MGPipeUnmigratedEmulation` sites, at 7534, 8185, 8480, 10106 —
`generate-mipmap-storage`, `generate-mipmap-cpu-fallback`, `copy-image-shadow-mirror`,
`get-tex-image-shadow`. Each is placed **after** the cheap declines, so the name means "this ran"
(7524-7534: after the `existingLevelCount == 0` return; 8172-8185: after the format and mipmap
declines; 8418-8480: after every shape decline; 10093-10106: after the channel-mapping and
zero-extent declines). D-M's fifth (`texture-remint-pull`) is not in this tree and is D's, as §6
says. Nothing in the D-M census is silently un-emulated: the fragColor broadcast, D24S8 dual-mode
sampling (byte-identical, verified), blit LOD/layered/multisample-replicate, draw-buffer `None`,
layered attachment shapes, the image format bake/widen/split, the LOD bias and the raw-depth-fetch
substitution are all still present and reachable. ✓

**Discipline (dimension 9).** Exit order: no new frontend-`SharedPtr` static (§1 Refuted). Logging:
five new `MGLOG_` lines — three `MGLOG_E_ONCE` for genuine seam/refusal conditions and one `MGLOG_D`
that mirrors the pre-handle arm's wording; nothing at `MGLOG_I`. No `TODO`/`FIXME`/`printf`/debug
leftovers in the diff (grepped). No new `MGB_CTX` read beyond the declared DEV-1/DEV-3/DEV-9 sites.
Working tree clean; `git status --porcelain` empty.

**Deviations judged.** DEV-1 ✓ (ownership argument correct, see §4). DEV-2 ✓ declared, with the A/B
caveat in §2. DEV-3 ✓ — and it is the reasoning MAJOR-1/MAJOR-2 ask to be applied consistently.
DEV-4 ✓ (structural, `Entry::stateRef` is empty on handle-keyed entries). DEV-5 ✓ (confirmed against
D's `Managers.h:2158-2169`). DEV-6 ✓ (confirmed: no encoding at c0 or c0c). DEV-7 ✓ but see MINOR-5.
DEV-8 ✓. DEV-9 ✓ (marked as monolith glue with a removal condition). DEV-10 ✓ (verified: the three
files are byte-untouched, and the `e5` message's second clause has no work behind it — accept, but
the integrator should know one of the five commit messages over-promises). DEV-11 ✓ (I reproduced
the shas by hand, same method). DEV-12 ✓. **DEV-13 is the one the integrator must act on before
anything else**: `refs/heads/p4a/esprytdraw` was never rebuilt by `wsl_tree.sh`, and re-running
`wsl_tree.sh p4a esprytdraw p4a/contract` would reset the branch and take all five commits.
`refs/heads/p4a/esprytdraw-backup` (`0b00f7f0`) is the net; do not delete it before the merge.

---

## 6. Rework list (bounded — all but the last are in E's own file)

1. **MAJOR-1** `SyncImageTextureBindings` 2527-2537: union the record window with
   `g_imageUnitHighWaterMark` (or drop the window here and keep the record for the values only).
   Add A8 to §3.
2. **MAJOR-2** `BindCurrentUnitSamplers` 4361-4363: `UnbindSampler(unit)` outside the window, or
   decline the whole arm when the window does not cover `[0, maxTouchedUnit]`.
3. **MAJOR-3** add `MGLOG_E_ONCE` to the image seam (2369) and the program seam (3699), with the
   same wording stem as 2715 so one grep covers all three.
4. **MINOR-1** correct the `ReadSurface` comment at 2801-2815 (or read the field).
5. **MINOR-3** `ForceBindCurrentFBO` 4028-4035: invalidate rather than stamp.
6. **MINOR-4** tick `Gate::EsprytUnitBindingsEpoch` `hit=false` on the record-arm decline at 1852.
7. **MINOR-5** delete the D-K2 mirror sentence at 221-229 (or add ID-15's fourth row) so the stale
   claim is not carried into the rebase.
8. **MAJOR-4 → package D**: move `InvalidateFramebufferHandleArmMemos()` **inside**
   `InvalidateFramebufferBindingCache` (`Managers.cpp:7107`); three `MG_Test/SanityTest.cpp` callers
   (2530, 2778, 2799) currently clear only half the framebuffer memo state.
9. **→ integrator**: ID-12's DV-5 reader assignment ("E's `SyncAttachmentObject`") is wrong;
   `SyncAttachmentObject` is `Managers.cpp` and E's diff reads no `MGPSurface` field. Reassign to D
   or grant E the function for one commit, or the c0c `TextureTarget` field lands unread.
10. **→ package F / ROADMAP**: G9's `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver`
    cannot be written against public GL (§4). Re-specify it as a white-box assertion or drop the
    red-before requirement for D-E3.

Not asked for and not required: the decline-table flips (§2). Those are ID-15's verification-round
work and should land there, not in this rework.

---

## 7. What the integrator must re-run on the integrated tree

0. **Before touching the tree**: do not run `wsl_tree.sh p4a esprytdraw p4a/contract` (DEV-13).
   `git lfs checkout` in every worktree before any retrace (ID-13).
1. Rebase onto `refs/heads/feat/disaggregated` (c0 + c0b + c0c) + wire + clientfb + clientsp +
   esprytobj, then take §3's reconciliations **first**: A1 (`GetOrCreateByHandle`, pointer + null
   check), A4 (delete the four latches, call D's `FramebufferSubsystemEnabled()` /
   `TextureResourceSubsystemEnabled()` / `SamplerSubsystemEnabled()` / `ProgramSubsystemEnabled()`),
   A3's new record-keyed cheap gates.
2. **Apply the §2 flips**, then run with the default mask and assert **every** handle arm engages:
   `SyncCurrentFBOByRecord` returns true, `UnitBindingsEpochFromRecords` answers,
   `ResolveShaderImageRecord` returns non-null, `BindCurrentUnitSamplers` sets `walkedFromRecords`,
   `ResolveGlobalConstantsRecord` answers. A silently all-declining tree passes every gate below and
   means the package did nothing — and after the flips it is now *loud*, which is the point.
3. **Zero seam-check hits.** `grep 'does not describe the binding it names'` across the itest and
   retrace logs must be empty for **all three** seams (framebuffer, image, program). A hit is a
   finding against B/C, never a reason to relax the check.
4. **MAJOR-1/MAJOR-2 regression evidence**: with the client emitting, log
   `(ShaderImageStart, ShaderImageCount)` against `g_imageUnitHighWaterMark` and
   `(SamplerStateStart, SamplerStateCount)` against `maxTouchedUnit` for a Minecraft frame. If they
   agree everywhere, DEV-3's `keys.maxTouchedUnit` can also move to the record (§8.4) and A8 can be
   written down as a contract guarantee; if they do not, the fixes in §6.1-2 are load-bearing.
5. **The unit-list equivalence** (§1 Refuted, last item): compare `g_unitTextureSyncList` rebuild
   counts between the walk arm (`MOBILEGL_PIPE_PUSH=0x1ff`) and the record arm (`0x1fff`) over one
   frame. A large drop means the epoch is missing rebuilds the walk arm took.
6. **The full C.4 verification**: `symbol_report.py --threshold 0` (G1),
   `p3a_untouched_regions.sh`, **`p4a_untouched_regions.sh`** — note `DepthStencilSamplingReadImpl`
   is a **namespace**, so F's script needs the namespace shape for that row — `ctest -L unit` ×3,
   `-L integration-gpu` on all three arms, the C.4 family regex, G13's two greps
   (`pGLContext` = 1 pre-existing, `MGPipeUnmigratedEmulation` = 4 in `DirectGLES.cpp` + 1 in D's
   `Managers.cpp`), and `retrace_gate.py` on `build-push` **and** `build-verify` (G3/G3b/G4 could not
   run on E's tree — no `build-verify`).
7. **Named traces**: `photon-v1.3b` on llvmpipe desktop retrace is the canary for `e3`'s image-binding
   semantics (and is the one that would catch MAJOR-1); `improved-transparency-minecraft-26.3` is the
   draw-buffer-`None` net for `e1`.
8. **`MOBILEGL_PIPE_LEGACY_MEMOS=0`** configure+build (E did this once and removed the dir) plus the
   `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exit-order lanes on both backends — the latter now
   has a named item to confirm (`g_rawDepthFetchSamplerState`, §1 Refuted).
9. **Track H census input for `MEASUREMENTS.md`**: 7 memos re-keyed (3 direct, 4 transitive), 0
   retired, 1 bypassed (`UnitSamplerLookupMemo`), 1 new (`g_readFboTextureSyncList`). Verified
   against the source; the census is accurate as written.
