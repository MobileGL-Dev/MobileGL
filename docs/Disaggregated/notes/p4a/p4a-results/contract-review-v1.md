# P4a package A `c0` — adversarial review of the contract (`contract-review-v1`)

Reviewed commit `08192d72` (tag `p4a/contract`), parent/base `37da3c3a`, worktree
`/home/swung/w7/p4a-contract` (read-only; nothing was built there). Every finding below was checked by
opening the file at the commit — the whole `git diff 37da3c3a HEAD` (43 files, +3802/−145) plus the
surrounding unchanged code it lands into. No private worktree was needed: every question turned out to
be answerable by reading, and no experiment would have added evidence.

## Verdict

**ACCEPT WITH MAJORS — conditional.** The *payload* layer is right: every enum, constant, struct edit,
field-list row, generated table and entry-point signature I checked matches the B decision it claims,
`c0` is behaviourally inert in both the push and the verify build, and G1's 0/0/0/0 is credible. The
sixteen declared deviations are, with two exceptions, correctly reasoned.

What is **not** right is the *client-facing API surface*: `c0` landed the death half of the MG_State
coupling and none of the birth half, hard-coded the death helpers' answer to a literal `false` with no
hook any package can supply, left the one var-tail apply signature D-D3 needs without its tail, and — in
the one line three documents rest on — did not actually make `kMGPipeWired*Subsystem` gate emission.
**Packages B and C cannot write their emitters against this tag without editing three files C.7 gives A
for the entire phase**, which is precisely the `bb2a236d` merge shape section E claims was discharged
structurally.

Six **majors** and nine **minors** follow. None of them is an ABI change and none needs the tag re-cut
as such — M1, M2 and M3 are additions and a two-line predicate in A's own files — but **M1, M2 and M3
must land, and be announced to all five packages, before B and C write a line of emitter**, and M4/M5
before D, E and F write the twin tables and `PipeSlotPeek`. M6 is the integrator's.

---

## Findings

### M1 (major) — `c0` landed the DEATH half of the MG_State-facing client API and none of the BIRTH half, and the six death helpers answer a literal `false` with no hook a package can supply

`MobileGL/MG_Impl/Pipe/PipeFill.cpp:899-980` (the six helpers), each of which reads

```cpp
const Bool published = false; // the texture emitter publishes nothing yet
```

at `:902`, `:912`, `:935`, `:947` (implicit `return false`), `:961`, `:976`.

Compare P3a's, in the same file, `:833-841`:

```cpp
MGPipeVertexInputEmitter& emitter = MGPipeVertexInputEmitterInstance();
const Bool published = emitter.RecordIsPublished(handle);
if (published) { ... MGPipeApplyDeleteVertexElements(only); emitter.NoteRecordDestroyed(handle); }
```

The five new emit headers expose **only** `Emit*(GLContext&)` and `Reset()`
(`FramebufferEmit.h:138,146`, `TextureEmit.h:1272-1277` of the diff, `SamplerEmit.h`, `ImageEmit.h`,
`ProgramEmit.h`). There is no `RecordIsPublished`, no `NoteRecordDestroyed` and no destroy emitter, so
nothing B or C can add to *their own* header makes any of the six helpers emit a `delete_*`.

The birth side is missing outright. `MG_Pipe/PipeMutation.h` gains exactly the six death declarations
(`:181-201`); the only mint/emit declarations in the file are P3a's buffer ones
(`:115 MGPipeMintResourceHandle(BufferObject&)`, `:203 MGPipeEmitResourceCreate(BufferObject&)`, …).
There is no `MGPipeMintTextureHandle`, no `MGPipeEmitTextureResourceCreate/Respecify/SubData`, no
`MGPipeEmitTextureParams`, no sampler-CSO mint or emit. And that header is the *only* door:
`grep -rn 'MG_Impl/Pipe' MobileGL/MG_State/` is **empty**, and
`scripts/check_include_closure.py:110-112` pins `MobileGL/MG_Pipe/PipeMutation.h` as the
"mutation-header" probe precisely so it stays that way.

**Failure scenario.** D-D1 emits `resource_create` from `TextureObject.cpp:56`'s constructor and
`RenderbufferObject.cpp:28`'s; D-D2 emits `resource_respecify` from
`RenderbufferObject::{SetInternalFormat,AllocateStorage,SetSamples}`; D-E1/D-E3 emit
`set_texture_params` from `TextureObject.cpp:125-179`'s parameter mutators; D-F1 mints a sampler CSO
from `SamplerObject`. All four sites are in `MG_State`, and there is nothing declared for them to call.
B and C then have exactly three options, and all three are forbidden: edit `PipeMutation.h` (A's for
the phase, C.7), edit `PipeFill.cpp` (same, and E's risk table names it as the one file the whole
ownership rule exists to keep single-owner), or include `MG_Impl/Pipe/TextureEmit.h` from `MG_State`
(the closure probe's whole reason). Separately, even once the emitters have bodies, every death helper
still returns `false` and emits nothing, so the applier's records stay `Live` on slots the client has
already handed back — the P3a C-1 shape with the sign flipped, and `PipeSlotPeek`'s client-side leak
cases (G8b) cannot see it because the *slot* is returned correctly.

**Refutations attempted, all failed.**
- *"A's `w1`/`w2`/`w3` fill them in."* Their scope is the applier bodies (C.0), and A physically cannot
  call `emitter.RecordIsPublished(handle)` before B and C define those methods — so the fix cannot land
  before its consumers, and nothing in C.0 or D.1 schedules an A commit after them.
- *"Only the sub-data drain runs from the client, and that is at the validate point."* D-B1's own table
  classifies texture storage, texture params, renderbuffer storage and sampler objects as
  **record-only**, emitted "from the GL entry points that cause them" — the wording `TextureEmit.h`
  itself uses ("THE THREE OBJECT CALLS ARE NOT EMITTED FROM HERE'S CALLER … they are emitted from
  MG_State's own mutators"), which states the requirement and supplies nothing that satisfies it.
- *"C.7's explicit grant of `GL_Texture.cpp` to B covers it."* That grant is one anticipated respecify
  call site at `GL_Texture.cpp:528-539`, and it is in `MG_Impl/GLImpl`, not `MG_State`; the constructor
  and parameter emissions are not in it.

**Fix (A, on `p4a/contract` or as an announced amendment before B/C start).** (a) Add P4a's birth
declarations to `PipeMutation.h` in the `MGPipeMintResourceHandle` / `MGPipeEmitResourceCreate` shape,
one set per kind, with bodies in `PipeFill.cpp` that delegate to the family emitter — the same
"declaration here, definition there" split P3a used, so no file is touched twice later. (b) Give each of
the five emitter classes `Bool RecordIsPublished(MGPipeHandle) const`, `void NoteRecordDestroyed(MGPipeHandle)`
and its family's destroy emitter, and make the six helpers read the emitter instead of a literal.

### M2 (major) — `MGPipeApplyResourceSubData` has no region tail, so D-D3's texture sub-data record cannot be applied at all

`MG_Pipe/PipeApply.h:585`:

```cpp
void MGPipeApplyResourceSubData(const MGPSubData& record, const void* bytes);
```

and `MG_Pipe/MGPipeTypes.h:850`, in the record that call takes:

```cpp
Uint32 RegionCount; // MGPSubRegion[] in the variable tail
```

There is no second overload anywhere in the tree. Every *other* var-tail call got an explicit tail
parameter in this very commit (`PipeApply.h`, the three unit sets: `const MGPBoundView* tail`,
`const MGPipeHandle* tail`, `const MGPImageView* tail`), so the omission is not a house convention.

**Failure scenario.** D-D3 emits, per dirty `(owner, uploadTarget, level)`, one `ResourceSubData` with
`UnionBox` **and** `RegionCount` + `MGPSubRegion[]`, and D-D5 has the applier accumulate exactly
`(UnionBox, RegionCount, Regions[])` into `MGPipeResourceRecord::PendingUploads` — which `PipeApply.h`
declares (`struct PendingUpload { … Vector<MGPSubRegion> Regions; }`) and which nothing can ever fill,
because the regions never reach the applier. The consequences are the two the brief cares most about:
verify's **retain mode** (part of G4) has nothing to compare, and D-D6's box-vs-rect choice degrades to
"always the union box" — the +6 ms/frame Mali cliff that SSIM is blind to and that the two published
`TextureUpload*` counters exist to expose.

**Refutations attempted.** *"The buffer path proves one pointer is enough."* A buffer sub-data has one
range and encodes it in the box (`MGPipeSetSubDataBufferRange`); the whole point of the texture record
is that it carries a box **and** a disjoint rect list, "由 server 选上传形状". *"`w1` owns it."* Yes —
and that makes it a **post-tag signature change to a function C.6 explicitly tells B and C to assume**,
which C.0 says must not happen without telling the other five packages.

**Fix.** `void MGPipeApplyResourceSubData(const MGPSubData& record, const MGPSubRegion* regions, const void* bytes);`
now, with the buffer call sites passing `nullptr`; or, if A prefers to keep `w1`'s freedom, an explicit
amendment notice naming the new signature.

### M3 (major) — `kMGPipeWired*Subsystem` does not gate emission; six of the seven P4a emitters are live at `c0`, and three documents say otherwise

`MG_Impl/Pipe/PipeFill.cpp:1865-1869`:

```cpp
const auto wants = [&](MGPipeDirty bit) {
    const Uint64 subsystem = MGPipeSubsystemForDirty(bit);
    return subsystem != 0 && (pushMask & subsystem) != 0 &&
           (dirty & MGPipeDirtyBit(bit)) != 0;
};
```

`kMGPipeWiredSubsystems` is **not** one of the three conditions. `ConfigLoader.cpp:257` now defaults
`pushMask` to `kMGPipeSubsystemsMigratedAtP4a` = `0x1fff`, so bits 9-12 are set. Of the seven new
emitters, six are gated by `wants()` alone (`:1924`, `:1927`, `:1937`, `:1940`, `:1943`, `:1946`); only
`DrainTextureSubData` also tests the wired mask (`:1933-1934`).

So `PipeFill.cpp:1920-1923` — *"all four family bits are absent from kMGPipeWiredSubsystems, so
`wants()` is false for every one of them and this whole block is dead"* — is **false**, and so is the
result file's §4.6 *"each family flips its own constant from `0` to its subsystem bit in the commit that
gives its emitters bodies"*. Today `EmitFramebufferState`, `EmitShaderState`, `EmitSamplerViews`,
`EmitSamplerStates`, `EmitShaderImages` and `EmitGlobalConstants` are **called** at every verb whose bit
fires. They are inert only because their bodies `return 0` — so `c0` is still behaviourally neutral, and
this is a wrong contract rather than a behaviour break.

**Failure scenario for a package.** B lands the framebuffer emitter's body in `b1` and flips
`kMGPipeWiredFramebufferSubsystem` in `b3`, believing (as the header comment, `PipeFill.cpp`'s comment
and the result file all tell it) that the constant is the switch. The emitter is live from `b1`, under
the shipped default mask, and every gate run in `b1`/`b2` measures an arm nobody thinks is on. The
mirror error is worse and will cost a debugging day: C gives `TextureEmit.h` a body, forgets the
constant, and sees **nothing** emitted — because the texture drain is the one emitter the constant
really does gate.

**Refutation attempted.** *"P3a had the same shape and it was fine."* It did, but P3a's emitter bodies
and its wired constant were in the same file and the same commit, so the window did not exist. P4a's
whole ownership design puts the body in B/C's header and the OR in A's file, which is what creates it.

**Fix.** Two lines, in A's own file, behaviour-preserving at `c0`: add
`(kMGPipeWiredSubsystems & subsystem) != 0` as a fourth condition of `wants()` (the P2/P3a bits are all
in that mask, so nothing existing changes), and correct the two comments. That makes the constant mean
what `PipeFill.cpp`, the five emit headers and the result file all already claim it means.

### M4 (major) — `MGPipeEmitSamplerViewCsoDestroyAndFree` skips the death notice, and its stated reason is wrong

`MG_Impl/Pipe/PipeFill.cpp:899-908`:

```cpp
// A sampler view has no frontend object of its own - it is minted off the texture's
// lifetime id - so there is no NotifyStateObjectDestroyed for kind SamplerViewCso to
// raise and step 2 is vacuous here. The slot still goes back, last.
if (!MGPipeHandleIsNull(handle)) MGPipeSlots().Free(MGPipeKind::SamplerViewCso, handle);
```

The reason is factually wrong. `MG_State/GLState/StateObjectDeathNotice.h:56-60` is
`NotifyStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId)` — **one** entry point,
kind-parameterised, deliberately "one entry point for every kind rather than one ops table per kind"
(`:27-28`). `MGPipeHandles.h:34` has `SamplerViewCso`, and the helper's own first line resolves it:
`MGPipeSlots().FindByLifetimeId(MGPipeKind::SamplerViewCso, lifetimeId)` — i.e. the view *is* keyed in
that kind's `ByLifetimeId` map under the texture's id, so `NotifyStateObjectDestroyed(SamplerViewCso,
lifetimeId)` resolves exactly like the other five. Nothing about "no frontend object" prevents the
notice; that is what taking the lifetime id rather than the object was for.

**Failure scenario.** D and E will hold a backend twin per `SamplerViewCso` slot (`SlotTables.h`'s
`BackendSlotTable` shape; `SlotTables.h:38` says "All six re-keyed object classes raise
`NotifyStateObjectDestroyed()`"). With no notice the twin is never dropped: on Espryt it survives until
the slot happens to be recycled and `GetOrCreate(handle)` resets it on a generation change; on
DirectVulkan, which installs no other per-kind free path, it is never dropped at all. That is exactly
the C-1 leak, one kind later, and G8b cannot see it because `PipeSlotPeek` reads the **client**
allocator, where the slot was returned correctly.

**Refutation attempted.** *"It self-heals on reuse."* Only if the slot is reused, and only on a backend
whose handle-keyed `GetOrCreate` resets on a generation change — which is the same overload P3a's
contract review flagged (M2) for adopting a stale generation. D-I1 also states the three-step order is
"fixed and is not a package's choice"; a helper that drops step 2 with a wrong reason is how the rule
erodes.

**Fix.** Raise the notice (one line) and delete the claim; or, if the intent really is that no backend
may keep a `SamplerViewCso` twin, say *that* in `PipeMutation.h:196` and in `SlotTables.h`, because D
and E need the answer before they write the twin tables.

### M5 (major) — `HighWater(ShaderCso)` returns the band top once a composite exists, so G8b's ordinary-ShaderCso leak assertion can no longer go red

`MG_Impl/Pipe/SlotAllocator.cpp:221-231`:

```cpp
if (!state.BandSlots.empty()) {
    return static_cast<Uint32>(kMGPipeShaderCsoCompositeSlotBase + state.BandSlots.size());
}
return static_cast<Uint32>(state.Slots.size());
```

The only consumer is `MG_IntegrationTest/Harness/PipeSlotPeek.cpp:37-40`, and the assertion shape G8b
copies is `HandleRecycleScenario.cpp:1080,1091` — `highWaterAfter == highWaterBefore` over `kChurn = 48`
rounds.

**Failure scenario.** `HandleRecycleScenario` runs the dedicated composite case and the ordinary
`ShaderCso` case in one process. From the first composite mint onward, `HighWater(ShaderCso)` is pinned
at `983040 + N`, and no amount of ordinary-slot growth moves it — so the ordinary case's high-water
assertion is vacuously true for the rest of the process. G8b asks for three assertions per kind; one of
them silently stops existing, for the one kind that has two independent release paths.

**Refutation attempted.** *"The live count still catches it."* It catches a *live* leak. The high-water
half exists to catch the other shape — slots freed but the dense table never shrinking, i.e. exactly the
1.3 KB-per-record growth C-1 produced — and D13's own argument for the change ("a leaked composite slot
moves it") is the mirror of what it costs.

**Fix.** Report the two spaces separately: `Uint32 HighWater(MGPipeKind)` keeps meaning the ordinary
space and a `CompositeHighWater()` (or a `HighWater(kind, Band)` overload) answers the band, with
`PipeSlotPeek` growing a seventh member for it. Package F owns `PipeSlotPeek`'s six new members and
needs this decided before it writes them.

### M6 (major, integrator's) — `check_include_closure.py` exits 0 when the compiler is missing, so G13's closure half is a false green

The result file's D16 records this as *"a tooling note for the integrator … not a tree defect"*. It is
a gate that cannot go red, which `ROADMAP.md:7` forbids: the brief's G13 spells
`--compiler clang++-20`, that binary does not exist in this WSL image, and the script prints
`::error::compiler not found` and **still exits 0** with 0 probes run. Every package that runs the gate
command verbatim gets a green it has not earned. `scripts/check_include_closure.py` is owned by nobody
in C.7, so this is the integrator's or F's.

**Fix.** `wsl_p4a_gate.sh` spells `clang++`; the script exits non-zero when the compiler is absent; and
the gate line checks the probe count is 4 rather than only the exit code.

---

### m1 (minor) — the `kMGPipeResourceTargetBuffer` drift `static_assert` is a tautology

`MG_Pipe/MGPipeTypes.h:185-188`: the constant is *defined as*
`static_cast<Uint8>(MGPipeResourceTarget::Buffer)` and then asserted equal to that same expression. It
can never fail. D-A3(a) wanted a guard against two independently written spellings drifting; either give
the constant its literal `0` (so the assert does work) or drop the assert and say the constant is
derived.

### m2 (minor) — `MGPipeApplierReleaseObjectRecords` clears three working handles and not the others

`MG_Pipe/PipeApply.cpp:693-720`. The comment says "the working handles they could name go too", and
`DrawProgram`, `DispatchProgram` and `BoundShaderCso` are nulled — but `DrawFramebuffer` /
`ReadFramebuffer`, whose `MGPSurface::Res` name texture and renderbuffer records just dropped, and the
three unit arrays, whose entries name sampler-view, sampler-CSO and texture handles, are left populated.
Teardown-only, so latent; but the stated rule is applied to half the fields it names, and D/E will read
this function as the definition of "what survives a release".

### m3 (minor) — the texture death helper discards the view helper's answer

`MG_Impl/Pipe/PipeFill.cpp:928`: `MGPipeEmitSamplerViewCsoDestroyAndFree(lifetimeId);` — return value
dropped, and `:930` returns the texture's own `published`. Once both emit, a texture whose
`ResourceDestroy` was suppressed but whose `DeleteSamplerView` went out reports `false` and the legacy
path runs for both.

### m4 (minor) — D5's destructor-ordering claim is not guaranteed

`MG_Impl/Pipe/PipeFill.cpp:316-317` of the diff: *"`~SamplerObject` runs immediately after
`~TextureObjectBase`'s body — a member's destructor follows its owner's body"*. `m_sampler` is a
`SharedPtr<SamplerObject>` (`TextureObject.cpp:57`, `m_sampler = MakeShared<SamplerObject>(0)`), so
anything that took a reference — a texture unit slot, a sampler-view resolution — delays it
arbitrarily. The **conclusion** (do not free the built-in sampler from the texture's lifetime id) is
right and important; the timing claim beside it is not, and D/E must not build an ordering on it.

### m5 (minor) — the archive's byte stream is not canonical

`ProgramArtifactsCodec.cpp`'s `ArchiveMap` and `ArchiveSet` write arms iterate an unordered container,
so two field-equal archives can encode to different bytes.
`ProgramArtifactsCodec.RoundTripsAFullyPopulatedArchive`'s `EXPECT_EQ(reencoded, bytes)` passes only
because a fresh map populated in the same order probes the same way — true today, not a property. Say in
`ProgramArtifactsCodec.h` that the bytes are not a content hash and must not be used as a cache key
(which matters the moment P5 or the program disk cache wants one).

### m6 (minor) — `PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce` cannot fail for the reason it exists

`MG_Test/Pipe/PipeCatalogueTest.cpp`: the case declares a local `kNames[5]` literal and asserts
`std::size(kNames) == 5u` plus pairwise inequality. Nothing compares it to the tree. G13b's real check
is the grep, which lives in a gate script nobody has written yet. Not wrong — but the "red before"
property belongs to F's script, and D and E must plant exactly those five strings verbatim.

### m7 (minor) — `X(BindProgramPipelineObject, kPulledEveryVerb)` is now coupled to a `PipeFill.cpp` arm with nothing checking the coupling

`MG_Pipe/DirtySurface.def` (D7's row) is correct **only while**
`EmittedCallSuppliesTheWholeField(MGPipeInputField::GetProgramForDraw)` is false
(`MG_Impl/Pipe/PipeFill.cpp:1366-1372`). A later phase that flips that arm makes the row silently wrong,
and `--check` cannot see it. One sentence in the row's comment naming the arm it depends on.

### m8 (minor) — the five `TheEmitterIsOneNeverDestroyedProcessSingleton` cases do not test what they are named

They assert `&Instance() == &Instance()`, which a plain function-local static — the thing the rule
forbids — would satisfy identically. The property is real and enforced by construction
(`static X* p = new X();`); the case name promises an observation it does not make.

### m9 (minor) — `FreeCount(ShaderCso)` sums two spaces

`MG_Impl/Pipe/SlotAllocator.cpp:224-228` of the diff returns `FreeList.size() + BandFreeList.size()`, so
a test reading it cannot tell which space a slot returned to. Harmless today; worth a word in the header
beside `HighWater`'s new caveat.

---

## Judgement of the sixteen declared deviations

| # | judgement |
|---|---|
| **D1** five emit headers created by `c0` | **Correct and necessary.** C.0 asks for the wired constant to live in the family header *and* for no file to be touched twice; only creating the headers at `c0` satisfies both. But the *shape* they landed with is incomplete — see **M1** (no publication/destroy hook) and **M3** (the constant does not gate what the header says it gates). |
| **D2** `TexRect` as a thirteenth enumerator | **Correct, and the reason is right.** `TextureEnum.h:10-24` has `TextureRectangle` at `$BASE`, the table may carry no `default:`, and appending after `TexBuffer` preserves every number D-A3 fixes. The second assert (`Tex2D != TexRect`) is real work. The exhaustiveness walk is sound: `Unknown = -1` sits outside `[0, TextureTargetCount)` so the loop cannot trip on it, and both non-targets are listed rather than defaulted. |
| **D3** `kMGPipeResourceTargetBuffer` moved and narrowed to `Uint8` | **Correct.** `MG_Pipe` may not reach into `MG_Impl` and D-A2's predicate lives in `MGPipeTypes.h`. The narrowing is safe (`MGPResourceDesc::Target` is `Uint8`). See **m1**: the drift assert that came with it is vacuous. |
| **D4** no `using` alias in `ResourceTracker.h` | **Correct.** `using MGPipeBindBit = MGPipeBindBit;` is ill-formed, both files are `namespace MobileGL::MG_Pipe`, and `ResourceTracker.h` includes `MGPipeTypes.h`. All twelve enumerators moved verbatim with unchanged values and underlying type; package B's spellings are unchanged. |
| **D5** six helpers; the texture helper does not release the built-in sampler CSO | **The correction is right; one sub-reason is wrong.** `TextureObjectBase`'s constructor really does `MakeShared<SamplerObject>(0)` with its own lifetime id (`TextureObject.cpp:57`), so freeing it from the texture's id would resolve the wrong slot — D-I1's table is genuinely wrong and this fixes it. But the timing claim is unsound (**m4**), and the *view* half of the same deviation skips the death notice on a false premise (**M4**). |
| **D6** `MGP_DIRTY_SURFACE_UNDECIDED_LIST` is not empty | **Correct and honest.** Both rows carry the tool's own reason verbatim, both bit answers are stated and unchanged, and the two directions are both controlled (18 for a stale mark, new 21 for a mark that stops being needed). Widening the taint rule would have been the wrong trade and the file says why. |
| **D7** `BindProgramPipelineObject → kPulledEveryVerb` | **Correct.** The write set (`m_boundProgramPipeline`, `m_everBound`, the two name tables) and bit 6's read set (`m_currentProgram`, `m_lifetimeId`, `m_linkVersion`) are disjoint, the shutter's `GetCurrentProgram()` choice is deliberate, and `GetProgramForDraw` is in the false arm of `EmittedCallSuppliesTheWholeField` so it really is pulled at every verb. See **m7** for the coupling that is now unguarded. |
| **D8** six emitted rows, four deliberate absences | **Correct.** Each absence has a real reason (a selector no call carries; a backend's own keying question; two frontend shutters replaced by a server-owned `Serial` with a different owner), and the two sticky rows are genuinely storage-less. The rows are also `LC_ALL=C`-sorted, which is P3a's `m1` lesson applied. |
| **D9** seven validate-point emitters, not five | **Correct in shape, wrong in its inertness claim.** Seven bits plus a drain need seven call sites, and adding one after the tag would mean touching `PipeFill.cpp` again. The claim that the block is "dead" at `c0` is false — **M3**. The P4a-before-P2/P3a ordering is defensible (5.4 is code organisation, all `set_*` complete before the verb) and inert while every body returns 0. |
| **D10** the default flip is `ConfigLoader.cpp` only | **Correct, verified.** The root `CMakeLists.txt` carries only `option(MOBILEGL_PIPE_PUSH … OFF)` (`:28`) and no numeric default; the itest pins are at `MG_IntegrationTest/CMakeLists.txt:949, 1065, 1070, 1075, 1080, 1149, 1183, 1188, 1219`, which C.7 gives F. See the packages' list below for the consequence nobody has written down. |
| **D11** `TrackerTest.cpp` edited | **Correct.** `Tracker.h` is A's for the phase, the case that pins its map is A's, the name is unchanged (G14), and the added chain assertions (`P4a ⊇ P3a ⊇ P2`) plus the "no bit names the texture-resource subsystem" walk are real work, not padding. |
| **D12** `SixValueStructsHaveFieldLists` 71 → 72 | **Correct.** Name kept (G14), and the case grew the two things that matter: the `borderColorForm` / `borderColorI` / `maxAnisotropy` field naming and the three-padding-byte control written through a byte pointer. |
| **D13** the composite band gets its own dense table | **Right decision, incomplete reason.** The 236 MB/23 MB arithmetic is correct and the two-table split is the right answer; `EntryOf` is a single resolver so a caller cannot forget the band; `Free` routes band slots to `BandFreeList` and `AllocateComposite` pops from it, so the two spaces stay separate and dense; `Reset` clears both. What the reason does **not** say is the cost: making `HighWater` report the band top takes the ordinary `ShaderCso` high-water assertion away — **M5**. It also does not warn consumers that a composite handle passes the `kMGPipeMaxShaderCsoSlots` bound like any other, which is the point at which an applier that forgets the band allocates the 236 MB the allocator just avoided (see the packages' list). |
| **D14** visited-field count is a test, not a `static_assert` | **Correct.** `VisitFields` needs an instance and `LinkArtifacts` carries `std::string`, vectors, maps and a `std::set`, so no constant expression can walk it. `ProgramArtifactsVisitedFieldCount<T>()` plus the 57/8/14/11/20 pins is the honest substitute, and forcing `link.program = nullptr` on every successful decode is the right belt. |
| **D15** one hand-written codec arm | **Correct.** `TUniformInitializer` has no `VisitFields` table, it is a plain aggregate, the arm is flagged in the source, and a type with no table is a compile error in the `else` branch rather than a `memcpy`. The bound-before-allocate rule in `TakeCount` and the trailing-byte refusal are both real and both exercised. |
| **D16** `clang++-20` does not exist | **Correct, and under-rated.** The finding is right; calling it "a tooling note, not a tree defect" understates it — a gate that exits 0 with zero probes is a gate that cannot go red. **M6**. |

---

## What packages B, C, D, E and F must know that the result file does not say

1. **M1, M2, M3, M4, M5 above** — in particular: *do not start writing emitters against the tag's emit
   headers until A has landed the birth declarations and the publication hook*, and do not believe
   `kMGPipeWired*Subsystem` gates emission today.
2. **Every itest lane is still pinned at `MOBILEGL_PIPE_PUSH=0x1ff`**
   (`MG_IntegrationTest/CMakeLists.txt:949, 1065, 1070, 1075, 1080, 1149, 1183, 1188, 1219`) — i.e. P4a's
   four subsystems **off**. Until F moves them, B/C/D/E's `integration-gpu` and `integration-verify`
   runs exercise the **legacy** arms and will not touch a single P4a emission. A package that reads a
   green itest lane as evidence its emitter works is reading the T2 control.
3. **A composite `ShaderCso` handle passes every bound like an ordinary one.**
   `kMGPipeMaxShaderCsoSlots == kMGPipeShaderCsoSlotLimit`, so slot 983040 is *in range*. Any applier
   body, twin table or peek that indexes by slot must test `MGPipeIsCompositeShaderSlot(slot)` **first**
   and use `MGPipeApplierState::CompositeShaderCsos` (or its own band table). Indexing
   `ShaderCsos[983040]` grows a ~236 MB vector — the exact defect D13 removed from the allocator and now
   the consumer's to avoid. There is no helper for this; write one.
4. **`HighWater(MGPipeKind::ShaderCso)` is not a table size** and is not the ordinary space's high-water
   mark once a composite exists (`SlotAllocator.h`'s rewritten comment says the first half, not the
   second). Size nothing off it.
5. **`MGPipeVerify(SamplerParameters)` exists but nothing reaches it yet.**
   `MGPSamplerDesc::Parameters` is an `MGPBlobRef` (`MGPipeTypes.h:420-424`), not a value, so the new
   field-wise comparator only bites when `w2`'s verify path calls it explicitly on the applier's
   `MGPipeSamplerCsoRecord::Params`. Assuming "the payload-list row makes verify field-wise
   automatically" gets you a byte comparison of a zero-size blob.
6. **Six accessors are deliberately not emitted rows** — `GetActiveTextureUnit`, `GetTextureContextId`,
   `GetTextureBindGeneration`, `GetSamplingResolutionGeneration`, and the sticky `GetTextureObject` /
   `GetProgramObject`. Those fields keep coming through the residual fill at every verb, so Espryt may
   go on reading them; do not "clean them up".
7. **All six new emitted rows land in `EmittedCallSuppliesTheWholeField`'s FALSE arm**
   (`PipeFill.cpp:1366-1372`), so *nothing stops being pulled in P4a*. A package that removes a pull
   because "there is a Coverage.def row now" breaks the push build's mirrors.
8. **`MGPTextureParams` is 40 bytes with `BuiltinSampler` at +8, and a null `BuiltinSampler` is
   `Fatal{ProtocolCorruption}` by contract — but nothing enforces it yet** (the applier is a stub). A
   package that forgets to fill it passes every `c0` gate and detonates when `w1`/`w2` land.
9. **`MGPipeApplierReset` advances five new serials and clears the three unit arrays and the two
   framebuffer records**; the emitters' `Reset()` is called from `PipeFill.cpp`'s `FreshlyPrimed` block.
   Put **only latches** in `Reset()` — a re-publication path there would move a `Serial` for nothing,
   and D-J4 forbids one.
10. **`kMGPipeWiredSamplerSubsystem` lives in `SamplerEmit.h` and covers `ImageEmit.h` too.**
    `ImageEmit.h` defines no constant of its own; C flips one value for both files.
11. **`CompositeResolver.h` does not exist** — C creates it, header-only, inside `#if MOBILEGL_PIPE_PUSH`
    — but `MG_Test/Pipe/CompositeResolverTest.cpp` already exists, is registered, and already owns the
    suite name `CompositeResolver` and the case `TheCompositeBandHasExactlyOneDoor`.
12. **Run the closure gate as `--compiler clang++`, and check it reports 4 probes** — `clang++-20` is
    absent and the script exits 0 anyway (**M6**).
13. **The five `MGPipeUnmigratedEmulation` names are fixed strings** —
    `copy-image-shadow-mirror`, `generate-mipmap-storage`, `generate-mipmap-cpu-fallback`,
    `get-tex-image-shadow`, `texture-remint-pull` — pinned as literals in `PipeCatalogueTest`, with no
    in-tree check that the call sites match. D and E must spell them exactly.
14. **The six death helpers are currently unreferenced.** The five destructors still call
    `NotifyStateObjectDestroyed` directly (`TextureObject.cpp:40`, `RenderbufferObject.cpp:40`,
    `FramebufferObject.cpp:36`, `SamplerObject.cpp:39`, `ProgramObject.cpp:43`). B and C **replace** those
    single lines — edit, never add a destructor (G1).
