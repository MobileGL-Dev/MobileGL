# P4a package A — `w1`-`w4`, adversarial review of the applier bodies (`wire-review-v1`)

Reviewed `86835b50` / `bc9abfbd` / `0f990633` / `3c07c8c3` on `refs/heads/p4a/wire`, parent `08192d72`
(tag `p4a/contract`), worktree `/home/swung/w7/p4a-wire` (**not modified**). Everything below was checked by
opening the file at `3c07c8c3`; the brief, `contract-v1.md`, `contract-review-v1.md` and `contract-v2.md` were
re-read at their landed text. No private worktree was needed: every claim I re-derived is a source or a
structural one.

## Verdict

**REWORK** — one critical defect, small and fully specified, plus five minors of which two were explicitly
assigned to this package by the integrator and not taken.

The package is otherwise very good, and better than P3a's `wire` was at the same point. The three-table split
is right and is argued at the place it matters; the two verdicts (counted no-op vs `Fatal{ProtocolCorruption}`)
are applied consistently and the boundary between them is drawn where the brief draws it; the sub-data split
into a buffer half and a texture half is the right call and its "deliberately NOT checked" list is the most
useful comment in the file; `ApplyUnitWindow` is one body for three sets with the overflow written in `Uint64`;
the composite band split is correct at both ends and is coherent with `c0b`'s `CompositeHighWater` split (no
w4 case reads `HighWater`, only `LiveCount(ShaderCso)`, which `c0b` deliberately left summing both spaces —
§4.3 of `contract-v2`); and the verify-only codec pin is live code with a gate, in the right place in the body,
under the right guard, with the `Fatal{PipeVerifyDiffer` marker G4 greps for. Behaviour neutrality is
structural and I verified it independently: all four `kMGPipeWired*Subsystem` constants are still `0`
(`FramebufferEmit.h:40`, `TextureEmit.h:36`, `SamplerEmit.h:41`, `ProgramEmit.h:46`) and the only mentions of
the fifteen entry points outside `MG_Pipe/PipeApply.{h,cpp}` and `MG_Test/Pipe/*EmitTest.cpp` are two prose
comments (`MG_Impl/Pipe/ProgramEmit.h:33`, `MG_State/GLState/ProgramState/ProgramArtifactsCodec.h:26`).

What forces a rework round is **C1**: `resource_respecify` clears the *whole* pending-upload set, which throws
away accepted uploads for levels and cube faces the respecify never redefined — after the client has already
cleared those levels' dirty flags. That is the exact class of loss D-D5's server-side set exists to prevent,
it is reachable on a canonical mip-building sequence, and it is invisible to every gate P4a has. It is
declared as deviation **W7**, whose stated justification is factually wrong about the frontend.

Nothing here needs the tag re-cut. C1's fix is ~15 lines in A's own file plus one case; the minors are
one-liners. G1, G13 and the closure gate hold and hold structurally (`PipeApply.{h,cpp}` are push-only
translation units, so the pull build cannot move).

---

## Findings

### C1 (critical) — `resource_respecify` drops the pending uploads of every level, not of the level it redefines, and the client has already cleared those levels' dirty flags

`MG_Pipe/PipeApply.cpp:1478-1485` (the clear and its comment), `PipeApply.h:220-238` (the set and its
contract), `MG_State/GLState/TextureState/TextureObject.h:245` and `:252` (the frontend's per-level
granularity), `MG_State/GLState/TextureState/MipmapStorage.h:53,118` (`m_isDirty` is a per-level vector),
brief D-D1 (respecify is emitted from `TexImage{1,2,3}D_State` — i.e. per GL call, per level) and D-D5.

```cpp
// PipeApply.cpp:1485
record->PendingUploads.clear();
```

The pending set is keyed `(UploadTarget, Level)` and holds every emission the applier has accepted and the
backend has not yet consumed. A respecify wipes all of them.

**Why the deviation's argument does not carry.** W7 says *"nothing is lost because the frontend entry points
that respecify a texture re-mark the levels they define (`AllocateStorage` then `MarkStorageDirty`)"*. Both
frontend entry points are **per (uploadTarget, level)**:

```cpp
// TextureObject.h:245, :252
virtual void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) = 0;
virtual void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty = true) = 0;
```

so a `glTexImage2D(level = 1)` on a mutable texture re-marks **level 1 and nothing else**, while the applier's
clear drops the pending entries for levels 0, 2, 3 … and for the other five cube faces. Those levels' client
dirty flags were cleared at *their* emission (D-D5 step 1), so nothing anywhere still owes them.

**Failure scenario**, and it is the canonical one:

1. `glTexImage2D(level 0, data)` → respecify (GL-call time), `MarkStorageDirty(0)`.
2. `glDrawArrays` → the drain emits `resource_subdata(level 0)`; the applier accumulates it; the client
   clears its level-0 dirty flag.
3. Espryt **bails** — the texture is not mipmap-complete for its min filter, which is the
   `Managers.cpp:5734-5738` early return D-D5 names by name — so the applier's entry survives, exactly as
   designed.
4. `glTexImage2D(level 1, data)` → respecify → **`PendingUploads.clear()` destroys level 0's entry.**
5. `glDrawArrays` → the drain emits level 1 only. Level 0's texels never reach the driver, in any build,
   with no counter, no log line and no gate that can see it. The same happens for
   `glTexImage2D(0); draw; glGenerateMipmap(...)` — the generated-mip storage grow at `GL_Texture.cpp:528-539`
   is a respecify emission C.7 explicitly grants to B — where the whole generated chain is then built from
   whatever the driver had in level 0.

**Refutations attempted, and why each fails.**

- *"Both respecifies happen before the drain, so nothing is pending yet."* True only when no verb separates
  them. The loss needs one intervening verb, which is what step 2 is, and streaming a mip chain while drawing
  is the ordinary shape.
- *"Espryt consumes the entry at the sync between the two calls."* Sometimes. The arms where it does not are
  precisely the bail arms the server-side set was introduced for (D-D5's own words), and the
  incomplete-texture bail persists **until more levels are defined** — i.e. until the very call that wipes the
  set.
- *"A respecify invalidates the level's coordinate system, so keeping a box would upload past the end of a
  shrunken store."* True **for the level being redefined**, which is why the clear is right for that key and
  for a whole-resource redefinition (`glTexStorage*`, which defines every level at once). It is not an
  argument for dropping the other keys.
- *"The verify lane's retain mode would catch it."* It cannot: retain mode compares the *emitted*
  `(UnionBox, RegionCount, Regions[])` against the tracker's pre-clear set. The emission happened and matched;
  what was lost happened afterwards, inside the applier.
- *"It is latent — nothing consumes the set in this tree."* Same answer P3a's wire review gave to the same
  objection: these are the record semantics B's drain and D's consume loop are being written against right
  now, and D.1's order exists so that no package after `contract` runs against a wrong applier.

**Fix.** The clear has to be scoped, and `MGPResourceDesc` names no level, so the scope has to arrive with the
call. The cheapest form is the one W1 already established as legitimate in this file — a trailing defaulted
parameter that the buffer call sites do not pass:

```cpp
// PipeApply.h
struct MGPRespecifiedLevel { Uint16 UploadTarget, Level; };
void MGPipeApplyResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                  const MGPRespecifiedLevel* level = nullptr);
```

`nullptr` = "this respecify redefines the whole resource" (every `glBufferData`/`glBufferStorage`, every
`glTexStorage*`, every `TextureView`) → clear all, as today. Non-null → erase only the matching
`(UploadTarget, Level)` entry and keep the rest. B passes the pair it just allocated, which it has in hand at
every `AllocateStorage` call site. If the integrator would rather not touch the signature a second time, the
fallback is to clear nothing on a respecify and put the shrink clamp on the consume side (D) — but that moves
"the applier owns extent" (D-J1) onto the backend and needs saying out loud in the brief.

Either way the case is `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers`, with the mutation
(delete the scoping) added to the harness, and `TextureEmit.ARespecifyDropsThePendingUploadsAgainstTheStorageItReplaces`
narrowed to the whole-resource arm it actually proves.

---

### m1 (minor) — `MGPipeApplierReleaseObjectRecords` still clears three of the working handles it says it clears (contract review `m2`, assigned to `wire`, not taken and not declared)

`MG_Pipe/PipeApply.cpp:1117-1133`. The comment says *"P4a's five object tables go with them, **and the working
handles they could name go too**"*, and then nulls `DrawProgram`, `DispatchProgram` and `BoundShaderCso` only.
`DrawFramebuffer` / `ReadFramebuffer` — whose eleven `MGPSurface::Res` name texture and renderbuffer records
this function has just dropped — and `BoundSamplerViews` / `BoundSamplerStates` / `BoundShaderImages` — whose
entries name sampler-view, sampler-CSO and texture handles it has just dropped — are left populated.

Latent in the monolith (the function is wired to nothing, `PipeApply.h:556-563`), and teardown-only under
split, which is why this is a minor and not more. What makes it a rework item rather than a note is that
`contract-v2.md` §7 item 7 assigned it to this package in as many words (*"`wire`: M2 … and m2 are yours"*),
the report does not mention it, and D and E will read this function as the definition of what survives a
release. Two lines, or one honest sentence saying why the framebuffer records and the three sets deliberately
outlive the object records they name.

### m2 (minor) — `PipeApply.h`'s `RefusedObjectCalls` comment still promises a framebuffer refusal that W4 correctly says cannot exist

`MG_Pipe/PipeApply.h:422-428`: *"every **framebuffer**, sampler, sampler-view, program and texture-params call
this applier refused"*. `MGPipeApplySetFramebufferState` (`PipeApply.cpp:1953-1959`) resolves no record and can
never touch the counter — which is right, and W4's reasoning from D-I2 is right. But the header is A's own
file, this package owns it, and the fix is deleting one word. Leaving the contradiction in the header and the
correction in a report is the wrong way round: `MG_Test`, D and E read the header. (The brief's D-J3 has the
same five-family list; the integrator amends it at D.5.)

### m3 (minor) — the texture half of `resource_subdata` accepts any non-zero `Target`, including the `Renderbuffer` enumerator, and nothing pins the upload-target value space

`MG_Pipe/PipeApply.cpp:504-511` (`SubDataNamesABuffer`), `:834-837` (the texture half resolves in
`TextureResources`), `MGPipeTypes.h:844` (`Uint16 Target`), brief D-D3 (*"the owner's upload target
(`MGPipeResourceTarget` from D-A3, plus the cube-face upload target …)"*).

`MGPipeResourceTarget::Renderbuffer` is `10` and is therefore a perfectly plausible `MGPSubData::Target`; a
record carrying it is routed to `TextureResources` and, if a live texture holds that slot in the texture slot
space, accumulates a pending upload onto **that texture**. Unreachable from a correct client (renderbuffers
have no sub-data path), which is why it is a minor — but the applier's own argument for `ResourceTableForTarget`
returning null on an unknown enumerator ("acting on the wrong table would … act outside its own storage") is
exactly the argument for refusing this one. The value space of `MGPSubData::Target` is also undocumented on the
record: B is about to encode cube faces into it and nothing says which numbers are free. One `static_assert`
or one sentence beside `MGPSubData` plus a `Target != Renderbuffer` arm closes both.

### m4 (minor) — no acceptance signal reaches the emitter, so D-D5's "clear only for levels the applier accepted" degrades to "clear for levels I emitted"

`MG_Pipe/PipeApply.h:628` / `PipeApply.cpp:1502-1516` return `void`; `ApplyTextureUpload` has two silent exits
(a refused resolve at `:837`, a validator fault at `:849-858`) and neither is observable from the call site.
`RefusedResourceCalls` cannot substitute: the validator's `Fatal{ProtocolCorruption}` path does **not**
increment it, so in a shipped push build a refused upload and an accumulated one are indistinguishable to B's
drain loop, which is about to clear the level's dirty flag on the strength of having emitted.

I could not build a reachable loss out of it and that is why it is a minor rather than a major: the texture
family's create gate and consume gate are the same bit (D-B1 registers no op table, so there is no
register/unregister window of the kind `PipeFill.cpp:653-679` documents for buffers), so the resolve failure
only happens at teardown, where the texels do not matter. But D-D5 step 1 says *"only for levels whose record
the applier **accepted**"*, the signature is open in this very round, and it will not be open again: return
`Bool` (true when the record was stored/accumulated) from `MGPipeApplyResourceSubData` in the same edit that
fixes C1, and let B gate its clear on it.

### m5 (minor) — the three buffer-only entry points police their kind with an inert assertion where `resource_destroy` uses a Fatal

`MG_Pipe/PipeApply.cpp:1524` (`flush_range`), `:1553` (`readback`), `:1639` (`map_persistent`), `:1660-1663`
(`unmap_persistent`). All four resolve through `ResolveResource`, i.e. against `g_applier.Resources` — correct,
because these calls exist only for buffers — but `MGPFlushRange` / `MGPReadback` carry no kind at all, and
`unmap_persistent`'s `MOBILEGL_ASSERT(handle.Kind == Buffer)` compiles out at INFO, which is what every gate
build is. A mistyped record therefore aliases whatever buffer holds that slot instead of being refused, which
is the failure mode `resource_destroy`'s `ResourceTableForKind` Fatal exists to stop. W3's fix is complete for
the four calls the brief names (create / respecify / subdata / destroy); this is the tail of the same question
and one `Fatal` in `unmap_persistent` closes the only half that has the discriminator to close it.

### n1..n4 (nits, no rework required)

- **n1** `SubDataTextureFault` (`PipeApply.cpp:672-709`) checks region *containment* but not *disjointness*,
  and `AccumulatePendingUpload` (`:751`) concatenates lists across emissions, so the accumulated list can hold
  overlapping rects that the frontend's own model (pairwise disjoint behind one level,
  `MipmapStorage.h:62-75`) never produces. Harmless — D's N-rect staging uploads the same texels twice — but
  say it in the header beside `PendingUpload`, because D will read "the frontend's model" and assume disjoint.
- **n2** "the record describes no texels at all" (`:843`) tests the union box and `RegionCount`, so a record
  with `RegionCount = 1` and an all-zero region passes and creates an entry describing nothing.
- **n3** a texture record's `MGPTextureParams::BuiltinSampler` is never swept when the CSO it names is deleted
  (`MGPipeApplyDeleteSamplerState`, `:2035-2053`), by the same deliberate non-resolution as `set_texture_params`
  — correct, but §5 of the report tells D what to read and does not tell it that this handle can be dead.
- **n4** contract review `m1` (the tautological `kMGPipeResourceTargetBuffer` assert, `MGPipeTypes.h:185-188`)
  was offered to `wire` and is still unclaimed. Optional.

---

## Judgement on the eight deviations

| # | verdict |
|---|---|
| **W1** — trailing defaulted `const MGPSubRegion* regions` | **Accept.** It breaks nothing: `gen_pipe.py` never parses `PipeApply.h`, and the generated wire path (`generated/PipeWire.inc`, `MGPipeApplyWireRecord`, `gen_pipe.py:672-685`) is a bounds-checking skeleton that returns "not applied" and calls no `MGPipeApply*` function, so there is no thunk to break and `PipeCalls.def` is untouched. It **does** close M2 fully: `AccumulatePendingUpload` (`PipeApply.cpp:751,762`) fills `PendingUpload::Regions` from the tail, so D-D3's rect list, D-D5's pending set and verify's retain-mode comparison are all reachable, and the region-containment gate is the invariant M2 said nothing could check. The one cost is that a caller who forgets the tail gets `nullptr` instead of a compile error; the record's own `RegionCount != 0 && regions == nullptr` Fatal (`:682`) catches the case where it matters, so the hole is closed by the validator rather than by the type system. Acceptable, and the amendment notice the report gives B/C/D is the right form. |
| **W2** — four new bounds constants | **Accept.** Each is argued at its declaration, each is a number that arrives in a payload, none is allocated by being named. I checked the one that could be wrong: `kMGPipeMaxPendingUploads = 256` is above the 192 distinct `(cube face, level)` keys a legal texture can produce, so no legal record can ever hit that Fatal. |
| **W3** — the folded dispatch-guard defect | **Accept the fold** (the branch was rewritten before any push, the four commits and four exact messages are intact, and the defect was found by the package's own gate, which is the system working). **Complete for the four calls the brief names** — verified at `:1452`, `:1496`, `:1509-1515`, `:1624`, and negatively by `ResourceEmit.NoTextureOrRenderbufferResourceCallReachesTheBackendOpTable`, which also drives the same five calls on a buffer to prove the spy is armed. The tail of the question is m5. |
| **W4** — no refusal counted for `set_framebuffer_state` | **Accept the behaviour, reject the paperwork.** D-I2 gives a framebuffer a handle and no wire lifetime, so there is nothing to resolve and nothing to refuse; not resolving `MGPSurface::Res` is likewise right (D-I3: the keep-alives are the frontend's `SharedPtr`s and enforcement is P5's). But the correction belongs in `PipeApply.h`, not only in the report — m2. |
| **W5** — the create/re-issue serial rule | **Accept.** It follows the contract header and D-J2 verbatim (`a create is not a mutation`, `Serial` 0 so a fresh twin agrees without either side publishing), it names the divergence from `create_vertex_elements` honestly, and the three implementations are consistent with one another (`:2020-2025`, `:2075-2080`, `:2259-2273`). |
| **W6** — a re-issued `create_shader_state` clears the default uniform block | **Accept.** A relink replaces the layout the image was sized to; `~0u` is the sentinel that means "never uploaded"; `++GlobalConstantsSerial` announces the clearing so a twin cannot match what it staged before the relink. Well reasoned and correctly implemented. |
| **W7** — a respecify clears `PendingUploads` | **Reject as written — this is C1.** The clear is right for a whole-resource redefinition and wrong for the per-level one, and the frontend fact the deviation rests on is the opposite of the fact. |
| **W8** — `p4a-trees2.log` never said `REBUILT wire` | **Accept as a process note.** Three complete build logs plus a no-op `cmake --build` returning rc 0 across all three trees before the first edit is adequate evidence that the tree was current; the missing line is the orchestration script's and is worth fixing before the next package waits on it. Not a code matter. |

---

## H1 — is `BuiltinSampler == null` under `0x5ff` a real lane, and whose fix is it?

**It is a real lane.** `0x5ff` = bits 0-8 (P3a's `0x1ff`) plus bit 10 (texture resources). D-K2's table has
three rows — 11→10, 9→10, 10→7 — and `0x5ff` violates none of them, so no refusal fires and the arm runs. Bit
10 carries `set_texture_params` (D-K1), whose record must name a `SamplerCso` handle
(`MGPTextureParams::BuiltinSampler`, D-E1), and `contract-v2` §3.2's four unconditional mints are
`Texture / Renderbuffer / Framebuffer / ShaderCso` — **the sampler CSO is deliberately not among them**,
because it is content-addressed and shared (D-F1) and §3.4 leaves its identity rule to package C. So on that
arm the only thing that can mint the handle sits behind bit 11, the record carries `kMGPipeNullHandle`, and
`PipeApply.cpp:2128-2134` aborts the process in a verify or poison lane and drops every texture's parameters
in a shipped one. The applier is implementing D-E1 exactly as written and is not the defect.

**Assignment: the contract's — add a fourth row to D-K2, "bit 10 (texture resources) requires bit 11
(samplers)", refused in `ResolveTextureResourcesSubsystemArm()` with one `MGLOG_E` naming both bits and a
fallback to the legacy arm** (package D implements it, `ObjectSubsystemControlScenario` gets the mirror of
`ASamplerBitWithoutTheTextureBitIsRefusedAndNamed` at `0x5ff`, and the integrator corrects the brief's
sentence *"the mirror pairs (bit 10 without 11 …) are all fine"* at D.5).

The alternative — have the client mint the built-in sampler's CSO handle unconditionally in a push build, the
§3.2 shape — is worse on two counts. The handle of a **content-addressed** CSO is the cache's answer, so
minting it unconditionally means running the sampler cache's hash-and-confirm in the arm that exists to
exclude the sampler family, which costs the A/B its meaning anyway; and it makes the texture emitter (B) reach
into the sampler emitter (C), across the one package boundary C.7 was drawn to keep. §3.2's argument covers
identity-addressed handles minted at construction, which this is not. The cost of the D-K2 row is that bits 10
and 11 become one arm — real, but bit 11 already required bit 10, the recorded arms are pull / `0x1ff` /
`0x1fff` (D.4.2) and not per-bit sweeps, and a stated coupling beats a lane that aborts.

Either way the answer must be written down before B and C emit, because today the tree has an arm that
`ROADMAP.md:7` would call a gate that cannot go green.

---

## What I checked and found correct (so a later round does not re-derive it)

- **Per-target framebuffer storage.** `Target != Read` writes draw, `Target != Draw` writes read, `Both` writes
  both and bumps the pair's one serial once (`:1960-1970`, pinned at `FramebufferEmitTest.cpp:242-249`). The
  `{0,1}` default framebuffer needs nothing here: `Fbo` is stored verbatim and never resolved. `Target >= Count`
  and every `DrawBuffers[i] ∉ [-1, 8)` are Fatal with the identity in the line (`:1930-1951`).
- **The three tables and the destroy dispatch.** `ResourceTableForTarget` (descriptor) and `ResourceTableForKind`
  (handle kind) both return null on an unknown value and both callers Fatal (`:1415-1422`, `:1595-1602`); slot 7
  is three live objects at once and `ResourceEmit.TheThreeResourceKindsKeepTheirOwnSlotSpaceAndDoNotSeeEachOther`
  proves it end to end including the destroy.
- **The sub-data validator.** Level < 32, encodable box, `RegionCount <= 256`, non-null tail, every region
  encodable **and inside the union box**, "no texels at all", non-null bytes — and the three deliberate
  non-checks are argued rather than omitted. The containment rule is the right one and the reason given for it
  (the server picks the shape from the pair) is the reason.
- **`set_texture_params`.** On the texture's own record, `ParamsSerial` moves and `Serial` does not, both resync
  bytes are carried and never cleared, the named CSO is deliberately not resolved (an ordering fact, not a
  corrupt one) — all as D-E1/D-E2 require.
- **Sampler CSO / view records.** The blob rule (0 or exactly `sizeof(SamplerParameters)`), the by-value store
  including `borderColorForm`, the fresh-vs-re-issue rule, the delete that does **not** sweep the unit window,
  the silent back-pointer write, and the delete that clears the back-pointer only if the texture still names
  this view (`:2100-2106`) — that last guard is right and is the one thing in the family a mutation harness
  would need a new case for (see "test gaps").
- **`ApplyUnitWindow`.** `Uint64{start} + Uint64{count} > destination.size()` cannot overflow; entries land at
  `start + i` and nowhere else; nothing outside the window is cleared; the `Unit` field is stored and not
  policed, with `MGPVertexBuffer::BindingIndex`'s precedent cited; a refused set moves no serial and touches no
  entry; an empty set is applied and still moves the serial. `SamplerEmit.AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated`
  pins the acceptance boundary (`Start + Count == 192`) as well as the refusal, which is the shape the house
  asks for.
- **The composite band.** `MGPipeIsCompositeShaderSlot` is `[base, limit)` (`MGPipeHandles.h:103-105`), so a slot
  at or above the limit falls to the ordinary table and is refused by its `base` bound rather than indexing the
  band's; both tables grow only to their own high-water mark; no `MGPipeShaderCsoRecord` outside the band can
  reach a band slot. `CompositeResolver.ASlotAtTheShaderCsoLimitIsRefusedWhileTheLastBandSlotIsNot` pins both
  sides.
- **Serials only advance.** All five working serials `++` in `MGPipeApplierReset` *and* in
  `MGPipeApplierReleaseObjectRecords` (`:1099-1103`, `:1129-1133`); the per-record serials restart with the
  record, which is safe because `Gen` moves with it.
- **Reset semantics.** Working state cleared, object records untouched — including `Params`, `ParamsSerial`,
  `ViewCso` and `PendingUploads`, which ride on a resource record; four cases pin it, one per family.
- **Exit order.** No new static, no new singleton, no frontend `SharedPtr` anywhere: the only global is P3a's
  `g_applier = *new MGPipeApplierState{}` (`:396`), unchanged. The six test TUs' `String g_logPath` is test-only.
- **G1 / G13 / closure.** `PipeApply.{h,cpp}` compile only under `MOBILEGL_PIPE_PUSH`, so the pull build cannot
  move; `MGPipeResourceOps` is unchanged at nine members with no frontend type; the verify-only
  `#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>` sits inside `#if MOBILEGL_PIPE_VERIFY`
  (`:19-27`) and no closure probe names `PipeApply.*` or forbids `MG_State` inside `MG_Pipe`
  (`scripts/check_include_closure.py:73-137`) — so that include is unguarded by design and the contract header
  is the only thing holding it, which is worth knowing at P13 but is c0's decision, not this package's.
- **The codec pin.** Runs after the bounds gate and before the store, all four faults are real faults, and the
  `decodedLink.program != nullptr` arm is the right paranoia. The tag is `Fatal{PipeVerifyDiffer}`, which is
  what G4 greps.
- **Hygiene.** Four commits, four exact single-line messages, empty bodies, no attribution, not pushed; two
  production files and seven test files, all A's by C.7; every test file is a visible SKIP in a pull build.

## Tests (w4) — 33 cases

Not vacuous. Every case I opened asserts a specific field or counter, most assert an acceptance boundary
*and* its refusal in the same body, and the refusal helper (`ExpectRefusedNaming`) checks the log line's text
in a push build and `SIGABRT` plus the text in a forked child under verify/poison — I confirmed the needle
matches both spellings of `MGP_TRIP_WIRE_TAG` (`PipeApply.cpp:42-52`). The counter discipline is pinned in both
directions: a counted refusal moves `RefusedObjectCalls`, a `Fatal` must **not**
(`TextureEmitTest.cpp:324-325`, `ResourceEmitTest.cpp:1422-1423`), which is the distinction the whole file
rests on. The mutation evidence is credible: I traced M11, M13-M16, M20, M26, M28 and M40 to bodies that do
fail if the named line is removed, and M16's SegFault is exactly what deleting the null-tail gate produces at
`:686`. The report's note that grepping ctest output for `(Failed)` under-reports the two process-killing
mutations is correct and worth carrying into the house recipe.

Gaps, all small and none blocking:

- no case covers the *guard* in `MGPipeApplyDeleteSamplerView` (clear the back-pointer **only if the texture
  still names this view**) — a mutation that drops the condition survives, because the re-issue case re-issues
  on the same handle;
- `TextureEmit.ATexturesParametersLandOnItsOwnRecordAndMoveOnlyTheirOwnSerial:296-299` says "a BUFFER of the
  same slot is not a texture" without ever creating that buffer, so it proves the refusal but not the
  non-aliasing (the real proof is in `ResourceEmit`, so this is a comment fix);
- C1's fix needs `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers`.

---

## Exact rework list

1. **C1** — scope `resource_respecify`'s pending-upload clear to the level the respecify redefines
   (`PipeApply.cpp:1485`), via a trailing defaulted `const MGPRespecifiedLevel*` (null = whole resource) or the
   integrator's chosen alternative; add
   `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` and its mutation, and narrow
   `ARespecifyDropsThePendingUploadsAgainstTheStorageItReplaces` to the whole-resource arm. Rewrite deviation
   W7 with the per-level frontend fact (`TextureObject.h:245,252`) instead of the current claim.
2. **m4** — in the same edit, make `MGPipeApplyResourceSubData` return `Bool` ("stored / accumulated"), and say
   in `PipeApply.h` that B's dirty-flag clear is gated on it (D-D5 step 1). Announce both signature changes to
   B, C, D, E in the v2 report's "what the others must know".
3. **m1** — either clear `DrawFramebuffer` / `ReadFramebuffer` and the three unit windows in
   `MGPipeApplierReleaseObjectRecords` (`PipeApply.cpp:1117-1133`), or replace the comment's claim with the
   reason they survive. Contract review `m2` is discharged either way, and the report must say which.
4. **m2** — delete "framebuffer" from `PipeApply.h:422-428`'s refusal list and state there, once, that
   `set_framebuffer_state` resolves no record and can only ever fault.
5. **m3** — refuse `MGPSubData::Target == MGPipeResourceTarget::Renderbuffer` in the texture half, and pin the
   upload-target value space (which numbers cube faces may use) in one sentence beside `MGPSubData`.
6. **m5** — make `MGPipeApplyUnmapPersistent`'s kind check a `Fatal{ProtocolCorruption}` like
   `resource_destroy`'s, and note beside `flush_range` / `readback` / `map_persistent` that their records carry
   no kind and are buffer-only by catalogue.
7. **n1/n3** — two sentences in `PipeApply.h`: the accumulated rect list may overlap across emissions (D's
   staging must tolerate it), and a texture record's `BuiltinSampler` may name a CSO whose record is gone.
8. **H1** — not this package's. Report it to the integrator as written above (recommended: D-K2 gains
   "bit 10 requires bit 11"), and keep the applier's Fatal exactly as it is.

Optional, and declared minors do not block: contract review `m1` (the tautological `static_assert` at
`MGPipeTypes.h:185-188`), still unclaimed.
