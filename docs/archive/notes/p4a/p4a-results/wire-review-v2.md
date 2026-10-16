# P4a package A — `wire`, adversarial review of rounds v2 + v3 (`wire-review-v2`)

Scope: the **eight** commits after the rebase point on `refs/heads/p4a/wire`
(`7b5b3957 fb709418 52a16a1e be0e032a 22a3e265` = v2, `aff381c7 282325d5 70e742a3` = v3), HEAD `70e742a3`,
against ID-8 / ID-12 / ID-15 / ID-17 / ID-18 M3+M4 / ID-19(b) / ID-21 / ID-22, `wire-review-v1.md`
(C1, m1-m5), `wire-v2.md`, `wire-v3.md`, `esprytobj-review-v1.md` (C-1), `clientfb-review-v1.md` (M3/M4),
`esprytdraw-v2.md` and c0e (**now integrated**: pipe = `e8502a61`, ID-25).

Read-only in `/home/swung/w7/p4a-wire`. Every experiment ran in a private detached worktree
`~/w7/p4a-review-wire2` at `70e742a3` (`build-push`, `-j 8`, `CCACHE_BASEDIR=/home/swung/w7`), which is
**removed** (`git worktree prune` run, log deleted, `p4a-wire` clean, its three build dirs untouched).

---

## Verdict

**ACCEPT WITH MAJORS** — one major, four minors, two nits. Nothing here blocks the merge: every line of
applier behaviour this round changed is correct on the code I read, and the three items the round was launched
for (C1's level scoping, ID-19(b)'s per-object table, ID-18 M3/M4) are implemented as specified and gated by
tests that I re-killed with mutations of my own. The major is a **package-boundary contract gap** — wire changed
what the stored `Target` byte MEANS and told nobody, and package E has a live consumer of it — plus the memo key
E must now use. Both are exactly the class of silent hand-off defect ID-19 exists to close, and neither is
visible to any gate P4a owns.

### What I re-measured rather than took on trust

| claim | result |
|---|---|
| `ctest -L unit` = 1680 in all three builds | **1680 / 1680 / 1680** (`ctest -N`, `LC_ALL=C`) |
| name sets `build-linux` == `build-push`, 2646 | **2646 == 2646, 0 differing lines** |
| applier suites = 130 | **130** |
| MU1 (Named arm moves the draw binding) | **rc 8, 1/74** — `FramebufferEmit.ANamedRecordDescribes…` |
| MU4 (metadata arm disarmed) | **rc 8, 1/74** — `TextureEmit.ARespecifyThatRedefinesNoStorage…` |
| MU8 (C1's per-level scoping disarmed) | **rc 8, 1/74** — `TextureEmit.ARespecifyOfOneLevelKeeps…` |
| baseline clean | **74/74, rc 0** |
| **MX3 (mine)** — stale-generation test disarmed (`record.Gen != fbo.Gen` → `false`) | **rc 8, 1/74** — `AFramebufferHandleWhoseGenerationHasMovedOn…` |
| **MX2 (mine)** — the `Level` half of the level-scoped erase key deleted | **rc 8, 1/74** — `ARespecifyOfOneLevelKeeps…` |
| **MX1 (mine)** — the `UploadTarget` half of the same key deleted | **rc 0, 74/74 — GREEN** (MINOR-1) |

`git diff -- MobileGL/MG_Pipe/generated` rc 0; `gen_pipe.py` contains **no** reference to `PipeApply.h`
(its only hit is the string it *emits*, `gen_pipe.py:672`), so §3's default-tail / acceptance-return
source-compatibility claim survives v3 intact. No `[[nodiscard]]`; the four P3a call sites
(`PipeFill.cpp:645, 678, 691, 705`) discard the `Bool` and compile.

---

## Findings

### MAJOR-1 — the stored record's `Target` byte silently changed meaning, and `wire-v3.md` §5's package-E section does not say so (the one consumer is `DirectGLES.cpp:2983`)

**Where.** `PipeApply.h:397-401` (`MGPipeFramebufferRecord`) + `:383-396` (its comment) + `:957-975`
(`MGPipeApplySetFramebufferState`'s contract) + `wire-v3.md` §5 "Package E".
Consumer: `/home/swung/w7/p4a-esprytdraw/MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:2982-2987`.

Before ID-19(b) the applier held **two records, one per binding**, so `record.Target` read out of the
read-binding record was a live statement about the current binding pair: `Target == Both` meant "the object on
the read binding is the same object the draw pass just synced". After ID-19(b) the table is **per object**
(`PipeApply.cpp:2207-2211`, the record is written at `FramebufferRecords[state.Fbo.Slot]` whatever the target
is), so the stored `Target` is a property of **the last emission that named that object, from any target
including `Named`** — not of the object and not of the bindings. c0e's own comment
(`MGPipeTypes.h:596-600`, "the draw-buffer array … reaches the driver's bound draw framebuffer only for a
record whose Target is Draw or Both") reads as an invitation to keep asking the binding question of the field.

E does exactly that:

```cpp
// DirectGLES.cpp:2982
if (target == FramebufferTarget::Read &&
    record.Target == static_cast<Uint8>(MG_Pipe::MGPipeFramebufferTarget::Both)) {
    backendObj->SyncReadBufferToBackend(currentFBO);   // skip the draw pass's work
```

**Failure scenario.** FBO X is bound to both bindings, so its record says `Both`. A DSA call on X —
`glClearNamedFramebufferfv(X, …)`, `glBlitNamedFramebuffer(…, X, …)`, or any of the DSA attachment /
draw-buffer / read-buffer setters ID-19(c) puts a `Named` record in front of — overwrites the record at X's
slot with `Target = Named` (bindings untouched, which is correct). On the next draw the read pass no longer
matches `Both`, falls through to a full `SyncToBackend(currentFBO, Read)` and re-does the whole attachment and
draw-buffer walk the draw pass has just done, every frame that issues such a call. The wrong-direction case —
a stale `Both` on an object that is no longer bound to both, which would make the read pass **skip a
framebuffer that was never synced** — is the one that corrupts, and it is what the field-test shape now
permits in principle.

**Refutation I tried, and it holds for the corrupting direction only.** I read B v2's emitter
(`~/w7/p4a-clientfb/MobileGL/MG_Impl/Pipe/FramebufferEmit.h:219-260`): `EmitFramebufferState` walks **both**
bindings at every validate and rebuilds a `Both`/`Draw`/`Read` record from the current pair, suppressed on the
`(drawHash, readHash)` pair, and `Target` is a `ContentHash` input (`:193`). So any binding change re-emits
both records with the right `Target`, and the stale-`Both` corruption is **not reachable while B keeps that
shape**. What survives the refutation is (a) the `Named` false-negative above — cost, not corruption, but
per-frame cost on exactly the DSA traffic ID-19 added — and (b) that a correctness property now rests on an
undocumented emitter habit rather than on the applier's contract. Wire answers the binding question exactly
and cheaply — `BoundFramebuffer[Draw] == BoundFramebuffer[Read]` (ID-19(d)) — and `wire-v3.md` §5's E section
says only "a reference becomes a pointer", listing the seven `BoundFramebufferRecord` call sites and the
`SyncCurrentFBOByRecord` null test while omitting the one site that reads a field whose meaning moved.

Two further inaccuracies in the same §5-E section, found by reading E at `9b8441a6`: the quoted half-described
test `if (MGPipeHandleIsNull(st.DrawFramebuffer.Fbo) || …) return false;` **no longer exists** — E v2 replaced
it (ID-23 F2) with `drawUnrecorded`/`readUnrecorded` plus a loud one-sided branch at `DirectGLES.cpp:2865-2894`
— and the line reference `:2765` now points at `g_fboSyncedSerials`. The section is written against E v1.

**Fix (wire's half is two sentences).** In `PipeApply.h` beside `MGPipeFramebufferRecord`: *"`State.Target` is
the target of the LAST EMISSION that named this framebuffer, `Named` included; it is not a statement about the
object's current bindings. The binding question is `BoundFramebuffer[t] == fbo` (ID-19(d)) and nothing else."*
In `wire-v3.md` §5-E: add `DirectGLES.cpp:2983` to E's change list with that expression, and re-derive the
quoted shapes from E v2.

---

### MINOR-1 — the `UploadTarget` half of the level-scoped erase key is ungated: a mutation that deletes it is GREEN, and the case that claims to pin it does not

**Where.** `PipeApply.cpp:1646-1652` (the erase loop); `MG_Test/Pipe/TextureEmitTest.cpp:473-545`
(`ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers`), specifically `:503-511`.

The key is `(UploadTarget, Level)` and the code is right:

```cpp
// PipeApply.cpp:1648
if (it->UploadTarget != level->UploadTarget || it->Level != level->Level) continue;
```

The case's own comment at `:503-504` says the second face exists *"so that the level number alone cannot be
what matched"*. **It can.** I applied MX1 — delete the `UploadTarget` clause, leaving
`if (it->Level != level->Level) continue;` — rebuilt and ran the seven emit suites: **rc 0, 74/74**. The
symmetric mutation MX2 (delete the `Level` clause) is red. So exactly half of the key is gated.

**Why the case misses it.** Trace it: the second-face respecify at `:507` is fed `levelDesc(1, 0x8058u)`, which
is **byte-identical to the descriptor the previous respecify stored** — so it is classified metadata-only
(`RespecifyRedefinesNoStorage` true) and takes the third arm, erasing nothing at all, contrary to the comment.
The only real per-level erases in the case name `(kTex2D, 1)`, and `(kTex2D, 1)` is the only entry with
`Level == 1`, so the `Level` half alone decides both of them.

**Failure scenario the gate should hold.** A cube map: `glTexSubImage2D` on face 0 level 1 and on face 1 level 1
both pending behind Espryt's incomplete-texture bail, then `glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 1, …)`
redefines face 0 level 1 only. With a `Level`-only key the loop erases the **first** entry with `Level == 1`,
which is whichever face was accumulated first — face 1's texels are dropped for good (its client dirty flag was
cleared at its own emission, D-D5 step 1). That is C1's bug one axis over.

**Refutation I tried.** The shipped code has the full key, so this is a *test* gap and not a live defect. But
the level-scoped erase is the thing C1's whole rework exists to make right, MU8 is the round's own proof that
the arm is load-bearing, and half of its key has no mutation behind it — which is the condition under which a
later refactor of `AccumulatePendingUpload`'s keying would land green.

**Fix.** In the same case, make the second-face respecify a real redefinition (move one storage-defining field,
e.g. `levelDesc(1, 0x8051u)`) that names `{secondFace, 0}`, and assert `(kTex2D, 0)` survives while
`(secondFace, 0)` goes. Add it to the mutation table as MU10 = MX1.

---

### MINOR-2 — `FramebufferSerial` is one number for the whole family and a `Named` write moves it, so E's per-target memo is invalidated by DSA traffic on framebuffers that are not bound; the table has no per-record serial to key on instead

**Where.** `PipeApply.h:600-603` (`Uint64 FramebufferSerial`), `PipeApply.cpp:2229` (`++` on every applied
record, `Named` included); consumer `DirectGLES.cpp:2758-2765` (`SyncedFramebufferSerialMemo`, `serial`) and
`:2903` (`if (SyncedFramebufferSerialIsCurrent(target, st.FramebufferSerial)) continue;`).

Before ID-19(b) the serial moved only when a **bound** framebuffer was described, so a per-target memo keyed on
it was about as sharp as the four `g_fboSynced*` arrays it retires. Now every `Named` record — one ahead of
each `BlitNamedFramebuffer`, each of the four `ClearNamedFramebuffer*`, and each DSA attachment / draw-buffer /
read-buffer setter (ID-19(c)) — moves the same number, invalidating **both** targets' memos and forcing a full
attachment re-sync of the currently bound draw and read framebuffers on the next draw, for a write that touched
neither.

**Refutation.** Over-invalidation is always safe: the memo is stamped after the sync, so a spurious miss costs
a redundant `SyncToBackend`, never a wrong frame. And P4a is behaviour-neutral today (all four
`kMGPipeWired*Subsystem` are 0 on this tree), so nothing regresses now, and ID-6 records performance rather
than gating it. That is why this is a minor and not a major. It is still a real coarsening introduced by this
round, on the draw path, and the applier is the only place that can undo it.

**What E should key on, and what wire owes it.** The exact question is *"has the record describing the
framebuffer bound to target t changed, or has the binding changed"*. That is
`(BoundFramebuffer[t], <that record's own serial>)`. Wire supplies the first half and not the second:
`MGPipeFramebufferRecord` (`PipeApply.h:397-401`) carries `Gen`, `Live`, `State` and no serial. Three lines fix
it — a `Uint64 Serial` in the record, `++record->Serial` beside `PipeApply.cpp:2229`, and an accessor (or
simply returning the record struct rather than `&record.State` from a second entry point). Until then E's
honest key is `(BoundFramebuffer[t], st.FramebufferSerial)`, which at least stops a *binding* change being
missed but does not stop the over-invalidation.

---

### MINOR-3 — the `kMGPipeFramebufferTargetNamed` retirement recipe in `wire-v3.md` §7.2 is one site short, and c0e has already landed

**Where.** `wire-v3.md` §7 item 2 names "six readers": `PipeApply.cpp:2153`, `FramebufferEmitTest.cpp:276`,
`:443`, `:454`, and prose at `PipeApply.h:595` / `:965`. A full grep of the tree finds **nine** occurrences:
those six plus the declaration `PipeApply.h:173`, its `static_assert` `:174-176`, and **a ninth the recipe
misses — the prose mention at `PipeApply.cpp:2148`**.

This is now live rather than hypothetical: c0e is integrated (ID-25, pipe = `e8502a61`,
`MGPipeFramebufferTarget::Named = 3`, `Count` = 4), so the `static_assert` at `PipeApply.h:174` **fires on the
merged tree** and `PipeApply.h` is included by `Managers.cpp`, `DirectGLES.cpp`, `VulkanRenderer.h` and
fourteen test TUs — the whole tree fails to compile until the retirement lands. ID-24 already schedules that
edit ("the Named-constant retirement edit done by the integrator in the wire tree after its rebase,
unit-tested, one commit"), so the ordering is handled; only the recipe's completeness is not. The trip wire
itself is **well designed and I would keep it** — it is the reason the local constant cannot be forgotten.

**Refutation.** A prose mention left behind compiles fine, so the miss costs a stale comment, not a build. It
is a minor only because the recipe presents itself as exhaustive ("One constant, six sites, no behaviour
change") and the integrator is being asked to execute it verbatim under time pressure.

---

### MINOR-4 — `resource_create`'s reserved-slot refusal is the one refusal class with no observable at all in a shipped build, and the header's contract says there are only two

**Where.** `PipeApply.cpp:1524-1527` (`MOBILEGL_ASSERT` + `return false`); contract text `PipeApply.h:777-789`.

The header states the acceptance-return rationale as *"a dead or stale handle is a counted no-op and a corrupt
record is a `Fatal` that deliberately moves no counter"* — two classes. `resource_create` with
`Slot < kMGPipeFirstAllocatableSlot` is a third: `MOBILEGL_ASSERT` compiles out at INFO (which every gate build
and every shipped build is), nothing is counted, nothing is logged, and before M3 the call was a completely
silent drop. The other two create refusals (`:1533` no resource target, `:1550` slot past
`kMGPipeMaxResourceSlots`) are `MGP_TRIP_WIRE_REPORT` and therefore at least an `MGLOG_E`.

**Refutation.** M3 is precisely the cure: the emitter now sees `false` and must not advance its dedupe or clear
a level's flags, and `TextureEmit.TheCreateAndRespecifyCallsAnswerWhetherTheRecordWasAccepted` (`:757`) pins
it. So the hole M3 was written to close is closed. What remains is that the header's two-class taxonomy is
incomplete where it is used to justify the return — one clause, not a code change.

---

### NIT-1 — `kMGPipeMaxFramebufferSlots` is a unilateral bound with no counterpart in the allocator

`PipeApply.h:156` sets it at `1u << 16`; `MG_Impl/Pipe/SlotAllocator.h` has no per-kind ceiling for
`MGPipeKind::Framebuffer` (only the `ShaderCso` composite band is policed), so nothing on the client side
prevents a handle above the cap. The verdict on one is correct and **loud, not UB**:
`MGP_TRIP_WIRE_REPORT` → `abort()` under `MOBILEGL_PIPE_VERIFY`/`POISON`, `MGLOG_E` + return under push, with
nothing stored, no serial moved, and `AFramebufferRecordThatNamesNoUsableHandleIsRefusedRatherThanStored`
(`:528`) asserting the table did **not** resize. Unreachable in practice — the client frees a framebuffer's
slot at death (`PipeFill.cpp:1260-1261`, `NotifyAndFree(MGPipeKind::Framebuffer, …)`), so the high-water mark
tracks *peak live* FBOs, not lifetime creations, and 65 536 simultaneously live FBOs is not a thing. Identical
in shape to the `VertexElements` and `SamplerCso` bounds v1 accepted. Recorded so F can assert
`HighWater(Framebuffer) < kMGPipeMaxFramebufferSlots` if it wants a gate.

### NIT-2 — contract review `m1` (the tautological `static_assert`, `MGPipeTypes.h:185-188`) is still unclaimed

v2 declined it because c0c was in flight on that file; c0c, c0d and c0e have all landed since and it is still
there. Not wire's file; the integrator's to fold or drop.

---

## What I checked and found correct (so a later round does not re-derive it)

**C1's fix (dimension 1).** The three arms at `PipeApply.cpp:1640-1653` are ordered `metadataOnly` →
`level == nullptr` (whole-resource `PendingUploads.clear()`) → the keyed erase, and the precedence is right:
M4's rule *refines* C1's rather than contradicting it (a call that replaces no storage replaces no level's
coordinate system), and it is the arm that must win, because the mask republish B v2 will emit is exactly the
call that would otherwise eat texels standing behind Espryt's incomplete-texture bail. Both are pinned —
`ARespecifyThatRedefinesNoStorage…` (`TextureEmitTest.cpp:687`) drives the metadata arm **with a level pointer**
and carries its own negative control (move `Width`/`Height` and the same call drops the level), and MU4/MU8 are
red on the right cases. A cube-face respecify erases by the full packed key and leaves the other five faces
standing — correct in code, half-gated (MINOR-1). Keys are unique by `AccumulatePendingUpload`'s construction
(`:813-824` looks for the pair before appending), so `erase(it); break;` is right and no iterator survives it.

**The packed decode (dimension 1).** v2's local `SubDataResourceTargetOf` is gone; `PipeApply.cpp:1698` and
`:1711` go through `MGPipeSubDataResourceTargetOf` / `MGPipeSubDataUploadTargetOf`, `TextureEmitTest.cpp:505`
through `MGPipePackSubDataTarget`. A grep for `0x00FF` / `0xFF00` / `& 0xFF` / `>> 8` across
`PipeApply.{h,cpp}` and the two test files returns **one hit, and it is inside a comment** (`:520`).
`SubDataNamesABuffer` (`:512`) still matches the **whole** field, which is ID-12 as written and is what c0c's
own `static_assert` (`MGPipeTypes.h:962`) keeps exact. `MGPRespecifiedLevel::UploadTarget` is `Uint16` holding
the same packed value `PendingUpload::UploadTarget` stores verbatim — types line up, and the header
(`PipeApply.h:763-775`) states the pairing to B in as many words.

**ID-19(b) (dimension 2).** Table slot-indexed by the handle with the generation checked on lookup
(`PipeApply.cpp:2235-2264`); the three nulls are separated correctly and only the third is loud
(`++StaleFramebufferRecordLookups` + `MGLOG_E_ONCE`, counted **apart** from `RefusedObjectCalls`, `mutable` so
E keeps its `const auto&`); the accessors are defined in the `.cpp` so `MGLOG` stays out of the header's
include closure; `RecordAt` deliberately does **not** apply `kMGPipeFirstAllocatableSlot`, which is what lets
the reserved default framebuffer `{0,1}` be a real record at slot 0 while `MGPipeHandleIsNull` (`Slot == 0 &&
Gen == 0`, `MGPipeHandles.h:75`) still refuses `{0,0}` — both pinned as positive/negative controls at
`FramebufferEmitTest.cpp:528`. Draw/Read/Both set the bound handle(s) and `Named` sets neither
(`:2214-2221`, MU1 red). `Reset` clears `BoundFramebuffer` only and keeps the table (`:1183`, MU7); the
argument — a DSA-only framebuffer has no re-emission trigger, since `InvalidateAll` re-sends the two bound
records — is sound, and the slots come from one process-global allocator so nothing aliases across a switch.
`ReleaseObjectRecords` clears the table **and** the two bound handles **and** the three unit windows
(`:1211-1245`, m1 discharged by fixing, MU6). `ReadSurface` is stored verbatim under every target, which is
what c0e (ID-25) rules and what §5-B.1 tells B to fill.

**M3 (dimension 3).** `Bool` on `Create` (`:1524`) / `Respecify` (`:1576`) / `SubData` (`:1678`); every refusal
path returns `false` (create: reserved slot, null table, slot past the bound; respecify: null table, no live
record at that generation; subdata: the resource-target gate and `AccumulatePendingUpload`'s overflow), every
accepted path returns `true` including a metadata update and including an unregistered `MGPipeResourceOps`
(right: the op table is a property of the build, not of the record). `ResolveResourceIn` still moves
`RefusedResourceCalls` on the stale-generation path — asserted at `TextureEmitTest.cpp:775`.

**M4 (dimension 4), and W10 is now the contract's own ruling.** `RespecifyRedefinesNoStorage`
(`PipeApply.cpp:558-585`) compares field by field (not `memcmp` — `MGPResourceDesc` carries `Pad0`/`Pad1`) and
excludes exactly `BindMask`, `ImageBindableHint`, `GlNameForDiag` and the padding. ID-25 records c0e's ruling
as *"metadata = BindMask, ImageBindableHint, GlNameForDiag; **HasDefinedContent is storage-defining**"* — the
implementation matches the contract **exactly**, including the field ID-21's narrower list omitted. A metadata
respecify therefore cannot flip defined content: `HasDefinedContent` differing makes the call a redefinition.
I chased the one way that could have gone wrong — whether B's builder moves `HasDefinedContent` independently
of the storage fields, which would silently reclassify every mask republish as a redefinition — and it does
not: `TextureEmit.h:272-275` derives it from `mipmap->GetMipmapLevelCount() > 0`, in lockstep with
`desc.Levels` (`:261`), and `Immutable` from `texture.IsImmutable()` (`:268`); `Usage`/`StorageFlags` are never
written for a texture. A respecify that changes only `Levels`, or only `Samples`, is **not** metadata (both are
compared at `:568-570`), and the C1 case now feeds the level counts a real mip build produces so it exercises
the per-level arm rather than the metadata one. `MGPipeResourceRespecifyNeedsAck` is false by construction for
every metadata record because the buffer target is excluded outright (`:559`) — the right shape, since a
second suppression rule is one the transport would have to learn.

**m2 / m3 / m5 (dimension 5).** `RefusedObjectCalls`' comment (`PipeApply.h:507-535`) now names four families,
states positively that `set_framebuffer_state` resolves no record and can only fault, and — new in v3 — that
ID-19(b) does not change that, because the per-object table is written by that call and never looked up by it,
the lookup refusal being counted apart. Three test cases assert `RefusedObjectCalls == 0` across the family.
The `Renderbuffer` enumerator and everything `>= MGPipeResourceTarget::Count` are refused in the texture half
(`:1699-1712`) with **both** halves in the line. `unmap_persistent`'s kind check (`:1876-1882`) is
`Fatal{ProtocolCorruption}`, uncounted — the same verdict `resource_destroy` gives at `:1804-1810`, asked of a
narrower question (Buffer only vs any of the three kinds), which is right for a donation that is buffer-only.

**Exit order and hygiene (dimension 7).** `g_applier` is `MGPipeApplierState& g_applier = *new
MGPipeApplierState{};` (`PipeApply.cpp:396`) — the deliberate never-destroyed singleton P3a ID-18/21 asks for.
`FramebufferRecords` rides inside it and holds only PODs (`MGPFramebufferState`'s surfaces carry `MGPipeHandle`,
never a `SharedPtr`), so this round adds **no** new static holder of frontend `SharedPtr`s and no new
destructor reaching into pipe state. `MGLOG_E_ONCE` is used once, on a genuine seam defect — correct level, and
`MGLOG_D` would have been wrong there. `MGPipeApplierReleaseObjectRecords` still has no production caller
(tests only), as the header says.

**Integration lanes (dimension 6).** The nine mutations are honest — I re-ran three at random and all three
killed exactly the named case, and my two extra mutations behaved as the design predicts (MX2 red, MX3 red).
No DirectVulkan lane applies to this round: the seven emit suites are backend-agnostic unit tests, all four
`kMGPipeWired*Subsystem` are 0, `PipeApply.{h,cpp}` are push-only TUs, and G1 is 0/0/0/0 — the "leak test per
new kind on the DirectVulkan lane" that ID-8 requires belongs to the packages that mint (B and C), not here.

---

## The deviations, judged

| # | verdict |
|---|---|
| **W1–W6, W8** | **Stand.** v1's review accepted them and nothing in v2/v3 disturbed them. |
| **W7′** | **Superseded by W11's third arm, correctly.** The metadata arm precedes the scope arm and refines it; the scope argument is unchanged. |
| **W9** (`MGPipeApplyResourceSubData` returns `Bool`) | **Accept.** Source-compatible, `gen_pipe.py` re-verified as never parsing this header, no `[[nodiscard]]`, P3a's call sites unchanged. Extended cleanly to `Create`/`Respecify` in v3. |
| **W10** (a buffer is never metadata-only; the comparison is wider than ID-21's list) | **Accept, and it is no longer a deviation.** ID-25 records c0e ruling `HasDefinedContent` storage-defining and metadata = exactly the three fields wire excludes, so the implementation and the contract now agree field for field. Both reasons wire gives are right independently: the asymmetry argument (comparing too much costs one dropped upload on a call B does not emit; comparing too little uploads past the end of a new level) picks the safe direction, and excluding the buffer target is what makes "no reallocation ack" true by construction rather than by a second rule. I verified the claim the deviation rests on — that none of the extra fields moves on a mask republish — against B's actual builder (`TextureEmit.h:243-285`); it holds. |
| **W11** (identical storage fields clear nothing, whatever the level pointer says — and its sharp edge) | **Accept as written, and the sharp edge is benign in the direction that matters.** A per-level respecify whose descriptor does not move keeps the level it names. Wire calls this unreachable on B's memcmp dedupe; I add the stronger argument it did not make: even if B did emit one, nothing is lost — the descriptor is unchanged, so that level's coordinate system is unchanged, and the surviving box is re-uploaded from the client's **current** shadow, i.e. correct content over a possibly larger region. The failure W11 warns about would need a redefinition that moves no descriptor field at all, which `HasDefinedContent`, `Levels`, `Immutable` and the extent between them make impossible on B's builder. Honest, well-flagged, and the ten minutes of B-review it asks for is the right ask. |

---

## Rework list

Small, and none of it blocks the merge.

1. **MAJOR-1.** Add the two sentences to `PipeApply.h` beside `MGPipeFramebufferRecord` (`:397`): the stored
   `State.Target` is the last emission's target, `Named` included, and is **not** a binding fact; the binding
   question is `BoundFramebuffer[t] == fbo`. Amend `wire-v3.md` §5-E to list `DirectGLES.cpp:2983` with that
   replacement expression, and re-derive its quoted E shapes from E v2 (`9b8441a6`) — the half-described test
   it quotes no longer exists and `:2765` is now `g_fboSyncedSerials`.
2. **MINOR-1.** Make the second-face respecify in `TextureEmitTest.cpp:503-511` a real redefinition of
   `{secondFace, 0}` and assert `(kTex2D, 0)` survives; add MX1 (delete the `UploadTarget` clause at
   `PipeApply.cpp:1648`) to the mutation table as MU10 and show it red. Fix the comment at `:503-504`, which
   currently claims a property the suite does not hold.
3. **MINOR-2.** Either add `Uint64 Serial` to `MGPipeFramebufferRecord` with `++` beside
   `PipeApply.cpp:2229` and an accessor, so E can key per bound record — three lines, and it is the applier's
   to give — or state in `PipeApply.h:600-603` that the family serial is deliberately coarse and that DSA
   traffic on unbound framebuffers invalidates a bound target's memo, so E's verification round records the
   cost rather than discovering it.
4. **MINOR-3.** Add `PipeApply.cpp:2148` to §7.2's retirement site list (nine occurrences, not six).
5. **MINOR-4.** One clause in `PipeApply.h:777-789`: the reserved-slot create refusal is neither counted nor a
   Fatal, and the return is its only observable.

---

## The integrator's post-merge re-run list

1. **The full five-part gate of D.3** on the merged head, plus `symbol_report --threshold 0` (P4a's admitted
   set is empty) and `p3a_untouched_regions.sh 37da3c3a HEAD`. Nothing in this review substitutes for it; what
   I did re-measure on wire's own tree (unit 1680 × 3, names 2646 == 2646 with 0 differing lines, applier
   suites 130, generated-dir diff rc 0) matches `wire-v3.md` §6 exactly.
2. **The Named-constant retirement is merge-blocking, not a later round.** c0e is integrated (`e8502a61`,
   `Count` = 4), so the `static_assert` at `PipeApply.h:174` fires the moment wire is rebased, and
   `PipeApply.h` is in the include closure of `Managers.cpp`, `DirectGLES.cpp`, `VulkanRenderer.h` and
   fourteen test TUs — the tree does not compile until it is done. ID-24 already schedules it as one commit in
   the wire tree; use the **nine**-site list from MINOR-3, not §7.2's six, and re-run the seven emit suites
   (130) plus `-L unit` afterwards.
3. **After B v2 lands**, the three ten-minute reads `wire-v3.md` §7.3 names — all five DSA entry points plus
   the DSA setters emit a `Named` record; the suppressor is per framebuffer and not one global slot (B v2 at
   `8534abf7` is still **one** slot, `MGPipeSuppressorSlot::SetFramebufferState`, on the `(drawHash, readHash)`
   pair — that is the shape §5-B.2 asks B to change); the mask republish moves **only** `BindMask` /
   `ImageBindableHint`. Add a fourth: B must keep re-emitting **both** bound records at every validate — the
   refutation of MAJOR-1's corrupting direction rests on it, so it is now a load-bearing property and should be
   stated in B's own header rather than left as a habit.
4. **After D v2 lands**: `PushedFramebufferRecord` must lose its `asTarget` parameter and its
   `record.Fbo == fbo` test (`~/w7/p4a-esprytobj/…/Managers.cpp:8216-8227` still has both) or C-1 is only half
   fixed; D's teardown must not read a binding after `MGPipeApplierReleaseObjectRecords`.
5. **At E's verification round**: `DirectGLES.cpp:2983` becomes
   `st.BoundFramebuffer[Draw] == st.BoundFramebuffer[Read]` (MAJOR-1); the seven `BoundFramebufferRecord` call
   sites (`:2127, :2273, :2863, :2864, :2905, :4009, :4179`) take `const auto*` and null-check;
   `FramebufferRecordMatchesBinding` and the one-sided loud branch at `:2865-2894` take pointers;
   `StaleFramebufferRecordLookups` must read **0** on every scenario — a non-zero value is the seam defect
   ID-19 was written to make visible, not traffic.
6. **D-K2's fourth row** ("bit 10 requires bit 11", ID-15's H1) is still owed by D and F; the applier's `Fatal`
   on a null `BuiltinSampler` is unchanged and correct, and the `0x5ff` lane still aborts until that row exists.
7. **Docs**: the brief's D-C2 (record "per bound target") and D-J4 (framebuffer records as working state) are
   both amended by ID-19(b) and by this round; D.5 should carry the correction beside c0e's.
8. **LFS**: `git lfs install --local` in each worktree would stop ID-13's trap recurring after every WSL
   restart — `/usr/bin/git-lfs` exists but no worktree has `filter.lfs.smudge`, which is why wire restored 92
   fixture blobs by copy this round.
