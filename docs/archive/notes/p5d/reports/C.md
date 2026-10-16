# P5d round 3 - package C: client verb overhead (image-unit sweep, validate/fill, server stamp)

Worktree `MobileGL-p5d-C`, branch `p5d-C`, base `85362a85`. Not built, not committed.
Patch: `.../scratchpad/p5d/patches/C.patch` (865 lines, 11 files, after the fix round below).

## What changed, per file

### 1. The 32-unit image sweep per draw (brief item 1)

- `MG_State/GLState/TextureState/TextureState.h` - new `NoteImageUnitTouched(Int)` /
  `GetMaxTouchedImageUnit()` and an `Int m_maxTouchedImageUnit = -1` member, all inside
  `#if MOBILEGL_PIPE_PUSH` (the private member joins the two push-only generations already there).
  A **second** mark, not a widening of `m_maxTouchedUnit` (image units are their own array). It
  only ever grows, so a walk bounded by it over-approximates the units that hold a binding - the
  only direction a conservative GPU-write set may fail in.
- `MG_State/GLState/Core.h` - the two `GLContext` pass-throughs, same guard.
- `MG_Impl/GLImpl/Texture/GL_Texture.cpp` - `BindImageTexture` feeds the mark, same guard.
- `MG_Remote/Client/GpuWritePending.cpp` - `MarkWritableImageBufferTextures` walks
  `unit <= GetMaxTouchedImageUnit()` instead of `unit < MAX_TEXTURE_IMAGE_UNITS`. On the profiled
  workload (no image binding anywhere) the mark is -1, so the walk is one compare.
  Profile tie-in: 2.15% self / 3.59% inclusive of the GL thread, `GetImageTextureBinding` 1.37 of it.
- `MG_Impl/Pipe/ImageEmit.h` - its comment claimed no such mark could exist without resizing the
  pull build's object. Updated, not deleted: the mark is push-only, and the emitter deliberately
  keeps its program-derived window (the narrower "which units can a *shader* read").

### 2. `MGPipeValidateForVerb` (brief item 2)

- `MG_Impl/Pipe/PipeFill.cpp` - step 4's seven-term `supplied` conjunction was evaluated 63 times
  per verb; it reads nothing about the verb. It is now a memo (`SuppliedFieldMask`) keyed on the
  four facts it does read: the push mask, `ApplierDerivesRenderStateFields()`,
  `contextValuesWireLive` (passed in, so P5c rv's emission half and fill half still read the ONE
  answer), and `P4aFamilyHasItsConsumer(kMGPipeP4aFamilySubsystems)` - all four families ride that
  one signal by that function's own rule. The walk becomes one bit test per field; the expression
  is moved, not copied.
  Profile tie-in: `MGPipeValidateForVerb` 4.11% self / 10.9% incl.; `P4aFamilyHasItsConsumer` alone
  is a `CapsMirror` read taken 63x/verb for one answer under split.
- `ParsePoisonOmissionKnob` - the per-verb `String == String` (out-of-line even for two empty
  strings, which is every shipping configuration) is now a size compare inline, contents only when
  a non-empty value is involved. Re-parse condition unchanged.
- `MG_Impl/Pipe/PipeFill.h` - `MGPipeResidualFillSuppliesField(field, pushMask, applierDerives,
  contextValuesWireLive)`, exported for the unit gate only, in `MGPipeP4aFamilyEmits`' shape.

### 3. `MGPipeServerStampVerbBoundary` (brief item 3)

- `MG_Backend/MGPipe/PipeInputs.cpp` - the per-verb 63-field `answerable` derivation is now a
  `constexpr` per-verb-class mask table (`AnswerableMaskForClass` / `kAnswerableByClass`), built
  from the same expression. The loop body is `FilledGen[i] = serial & -bit` - no lookups, no calls,
  no branch. Withdrawal semantics byte-identical, including the out-of-range verb
  (`kNoFieldIsAnswerable`, all 63 withdrawn), which is what `FieldIsInVerbClass` answered for it;
  that helper stays, `MGPipeInputUnfreshRead` still uses it. Profile: 1.1% self of the apply
  thread, and on a lockstep frame that is client wait time one-for-one.

## Red-once

| change | test |
|---|---|
| stamp table | `FieldOwnershipTest.AServerStampMakesRecordSuppliedFieldsFreshAndWithdrawsTheRest` (existing) - it recomputes `answerable` independently per field and compares. `TheStickyExemptionIsCancelledByTheServerStamp` covers the withdrawal of the seven forwards. |
| supplied-set memo | `FieldOwnershipTest.TheResidualFillsSuppliedMemoReKeysOnEveryInputThatMovesAnAnswer` (**added**; renamed and extended in the fix round below, which restates this row) - moves each of the three key inputs that can move an answer on its own and reads the answer back in between, in an order where a memo missing that input must return the stale answer. This is the only thing a memo can get wrong that the expression could not; which fields the fill copies has no other observable, because a push build routes every emission straight into the applier, which writes the same storage. |
| image-unit bound | `SplitBufferSet.Row2TheSweepIsBoundedByTheImageUnitHighWaterMark` (**added**) - pins both ends of the bound (mark at -1 walks nothing; a unit at the mark is found). Revert the bound and its second half goes red. |
| the mark's producer | `ImageEmit.TheBindFeedsTheImageUnitHighWaterMark` (**added**) - `GL::BindImageTexture` must move the mark, and an unbind must not shrink it. Drop `NoteImageUnitTouched` from `GL_Texture.cpp` and this goes red at the entry point instead of as a missed GPU write. |
| (adjusted) | `SplitBufferSet.Row2AWritableImageBufferTextureIsMarkedByADraw` now notes the unit, because it writes the binding straight into the slot and never goes through `glBindImageTexture`. |

G2/G14 name parity kept: the two new split/push-only names have pull-lane skip twins
(`SplitBufferTest.cpp`'s `MGL_SPLIT_ONLY_OR_SKIP` list, `ImageEmitTest.cpp`'s
`MGL_IMAGE_EMIT_TEST_LIST`, `FieldOwnershipTest.cpp`'s `#if !MOBILEGL_PIPE_PUSH` block).

## G1 / monolith impact

- **Pull build (`MOBILEGL_PIPE_PUSH=OFF`): no change at all.** `PipeFill.{h,cpp}` is appended to
  `SOURCE_FILES` only under `MOBILEGL_PIPE_PUSH` (CMakeLists.txt:519-523); `PipeInputs.cpp`'s stamp
  is inside `#if MOBILEGL_BUILD_DISAGGREGATED`; `GpuWritePending.cpp` is MG_Remote. The two MG_State
  files and the one MG_Impl call site are each `#if MOBILEGL_PIPE_PUSH`. G1 stays 0/0/0/0.
- **Push monolith:** `TextureState` grows 4 bytes (`m_maxTouchedImageUnit`) and `glBindImageTexture`
  gains one compare. Behaviour identical - the member has no reader outside MG_Remote.
  `PipeFill.cpp`'s memo and the knob fast path are behaviour-identical by inspection.
- **Behavioural delta worth naming:** `MG_Remote::Client::ConsumerRefusals()` will count fewer
  refusals, because the residual fill asks `P4aFamilyHasItsConsumer` once per environment instead of
  63 times per verb. It is a diagnostic counter; the one-time `MGLOG_W` per refused subsystem still
  fires, and no test asserts an exact value that the fill contributes to
  (`SanityTest.…CountsTheRefusal` asserts a delta `> 0` from the mint path;
  `RemoteClientTest`/`CapsMirrorTest` drive `ServerConsumes` directly).

## What remains per draw in the validate point / draw emit (brief item 4, surveyed not cut)

- `wants()` still calls `P4aFamilyHasItsConsumer` per emitted gate (~10/verb) **and tests the dirty
  bit LAST**, so a clean bit still pays the consumer read. Reordering the dirty test first is a pure
  win *except* that `ServerConsumes` counts and logs - "a refusal that happened cannot fail to be
  counted" is R-8's stated rule. **Left alone deliberately**; it needs R-8's owner to say whether
  the counter may move.
- `ContextValuesWireLive()` per verb = three loads (`g_active`, `m_started`, one acquire load). Left.
- `RunsAsTheServerRole()` at PipeFill.cpp:3037 is inside the `tracker.FreshlyPrimed()` arm - a
  make-current edge, not per draw. Not hot. (PipeFill.cpp:992's is the respecify path, also not
  per draw.) Package D owns the guard itself.
- `EmitTables.cpp`: `ReadDrawBindings()` re-resolves the element buffer's handle every draw
  (`MGPipeResourceTrackerInstance().Find` -> `MGPipeSlots().FindByLifetimeId`, which under split also
  pays `MGPipeRefuseAllocatorFromApplyThread` - package D's guard). `VertexInputEmit.h:257-274`
  computed the same handle at validate, but **only when bit `NewIndexBuffer` was dirty**, so there is
  no already-computed value to reuse on a clean-index draw; a latch keyed on the VAO's index slot +
  the tracker generation would be needed. **Left** - it is a new latch with its own invalidation
  story and belongs with package B/D's tracker work, not in a cut described as "provably redundant".
  `RequireSession` and the `g_dropDrawEmission` ordering are one global read each and E2's control
  order respectively; nothing else on that path is redundant.

## Risks / not done

- **Not built.** No compiler has seen any of this. The constexpr stamp table
  (`MakeAnswerableMaskTable`, a `constexpr` function with loops over `inline constexpr` generated
  tables) is the most likely place for a toolchain complaint.
- The memo is a file-scope global, like `g_residualDue` / `g_omission` / the verify latches beside
  it; what keeps a single writer on all of them is the verb barrier. If a future change lets two
  roles fill concurrently, this global joins the list that has to move - stated in the code comment.
- The image-unit mark's safety rests on `glBindImageTexture` being the only writer of
  `m_imageTextureBindings` that can *add* a binding. I checked: the other writer is
  `TextureState::MarkTextureObjectForDeletion`, which only unbinds. Tests that poke
  `GetImageTextureBinding(u)` directly must note the unit - one such test existed
  (`SplitBufferTest`) and was updated; `MG_IntegrationTest` has none.
- Brief item 2 also asked about the residual fill's *object-class* copy cost (`GetBindingSlot` from
  `CopyField`, 1.73 self / 45% of it from the draw fill). That cost is the copy itself, not the
  predicate: those rows are BARRIER-PULLED and must be copied until P3b/P4b/P7/P8 give them twins.
  Nothing cut there; it is package E's question.

## Fix round

Same worktree (`MobileGL-p5d-C`), still uncommitted, still not built. Patch regenerated:
11 files, 587/64. Every blocker and major addressed; all seven minors addressed too (six were
one- or two-line corrections, the seventh is the by-value return).

### MAJOR - the memo's fourth key input is untested (`FieldOwnershipTest.cpp:405`)

**The finding is right, and it is stronger than stated: that input is not merely untested, it is
UNTESTABLE through this observable, and the suggested fix cannot work as written.**

The reviewer proposed flipping the consumer signal around `GetFramebufferBindingSlot`,
`GetTextureUnitObject`, `GetProgramForDraw` or `GetImageTextureBinding`. Those are exactly the
five fields whose emitter belongs to a P4a family - and every one of them is a field
`EmittedCallSuppliesTheWholeField()` answers **false** for (PipeFill.cpp:2276-2290: their storage
is a frontend heap reference - a `BindingSlot<FramebufferObject>`, an `ImageTextureBinding`, a
`TextureUnit`, two `SharedPtr<ProgramObject>` - that no payload can carry, so the fill pulls them
whatever the consumer says). So `supplied` is `false` for all five in BOTH consumer states: the
consumer conjunct is **dominated**, an assertion pair over those fields would read
`EXPECT_FALSE` / `EXPECT_FALSE`, and a memo whose key dropped the signal would pass it - vacuous
in the other direction, which is no better.

What went in instead, in three parts:

1. **`PipeFill.cpp` - a compile-time trip wire.** `NoP4aFamilyFieldIsWhollySupplied()`, a
   `constexpr` walk of `kMGPipeFieldEmittedBy` through `SubsystemForEmitter` and
   `EmittedCallSuppliesTheWholeField`, with a `static_assert` over it. It fires on the day
   P3b/P4b/P7/P8 give one of those rows a twin the applier can write - which is the day the
   consumer signal becomes observable - and its message says to go write the pair. This is the
   durable guard; it sits next to the predicate and cannot rot.
2. **`FieldOwnershipTest.cpp` - step 4, which MOVES THE SIGNAL FOR REAL.** Under monolith the
   signal is `MGPipeGetResourceOps() != nullptr` (PipeFill.cpp:1156-1171), and
   `MG_Config::Transport` is a `constexpr` Monolith in every lane but a split one, so
   `MGPipeSetResourceOps(&ops)` / `(nullptr)` moves it in the lane this case runs in, with no new
   include and no MG_Remote dependency. The case collects the WHOLE 63-field answer set in each
   state at `kEverySubsystem` (the only mask at which the fill reads the signal at all), asserts
   the signal really moved, names `GetFramebufferBindingSlot` both ways, and asserts the two
   answer sets are identical - with a failure message saying that a difference is *correct
   behaviour* and the instruction to rewrite the step into the move-and-read-back pair, naming
   `CapsMirrorInstance().Adopt()` and the SanityTest case that already drives it for the split
   arm. It restores the previous op table on the way out.
3. **The name.** `...ReKeysOnEveryInput` becomes **`...ReKeysOnEveryInputThatMovesAnAnswer`**, in
   both the push case and its pull-lane skip twin (G2/G14 parity kept). The finding that the old
   name and the red-once table oversold the coverage is accepted.

Red-once table correction for that row: the memo's case covers **three** of the four key inputs
by moving them; the fourth is covered by a domination assertion plus a `static_assert`, because
no observable distinguishes it today.

### MINOR - the consumer read moved to every verb, counters move the wrong way (both reviewers)

Accepted, and fixed rather than merely documented. `SuppliedFieldMask` now reads the signal only
where the walk could reach it:

```cpp
const Bool p4aConsumer = (pushMask & kMGPipeP4aFamilySubsystems) != 0 &&
                         P4aFamilyHasItsConsumer(kMGPipeP4aFamilySubsystems);
```

At a mask with no P4a bit, `(pushMask & subsystem) != 0` short-circuits every P4a field in the
rebuild and `P4aFamilyHasItsConsumer` returns a constant `true` for every other subsystem, so no
field can reach the mirror - the guard is exact, not conservative. The key stays complete because
`pushMask` is itself in the key, so a mask that gains a P4a bit re-keys on the mask. Net effect:
`ConsumerRefusals()` and `LastRefusedSubsystem()` are now unmoved at the A/B lanes and in the
bring-up window (CallMask 0) where the baseline never touched the mirror, and strictly *fewer*
refusals elsewhere - which makes the report's "will count fewer refusals" line true in both
directions rather than only one. The reasoning is in the code comment at the read.

### MINOR - `SuppliedFieldMask` returned a reference into a memo it rebuilds in place

Accepted. It now returns `MGPipeFieldMask` **by value**; the rebuild fills a local and publishes
it into `g_fillPlan` in one assignment at the end, so no caller can observe a partially rebuilt
mask and no rebuild can zero storage a caller holds. The call site at the walk takes
`const MGPipeFieldMask supplied` (a copy, 16 bytes, read 63 times - cheaper than the indirection
it replaces). The comment that cited `g_residualDue` as precedent now says why that precedent
covers the file-scope-ness but not the width.

### MINOR - the stale-memo failure mode is stated wrong (`PipeFill.h:78`, test header)

Accepted; the reviewer is right. The walk stamps `FilledGen[i] = filled.CurrentVerbSerial`
whether or not it copied, so a wrongly-skipped field reads **FRESH with the previous verb's
value** - a silent stale binding slot, caught only by the verify lane, not
`Fatal{UnmigratedPipeInput}` (poison catches an *unstamped* read, not a stamped-but-uncopied
one). Corrected in the test case header and stated explicitly in `PipeFill.h`, where it is now
the reason the key case is the only guard.

### MINOR - "32 slots" / "32 units wide" (`TextureState.h`, `GpuWritePending.cpp`)

Accepted, in both places. `MAX_TEXTURE_IMAGE_UNITS` is **192**: the replaced sweep was 192
`GetImageTextureBinding` reads per draw, so the measured win is 6x larger than the comment
claimed, not smaller. `GpuWritePending.cpp` now names the constant and the count;
`TextureState.h`'s folding argument now uses real image-unit numbers (a bind of unit 7 pushing
the texture scan out to eight slots, the last advertised unit pushing it to 191) instead of the
invented 32. The report's own prose never quoted 32, so nothing there needed the same fix.

### MINOR - the G2 parity count comment is off by one (`SplitBufferTest.cpp:552`)

Accepted; the block declares eighteen names in each lane (diffed, 18 vs 18). Rather than write
`EIGHTEEN` and let it rot a third time - it read `SIXTEEN` against seventeen before this commit
and `SEVENTEEN` against eighteen after - the sentence now drops the count, says the list below is
the authority, and records that the number has never once matched.

### MINOR - the new producer case binds image unit 4 (`ImageEmitTest.cpp:582`)

Accepted. The case now binds **unit 1**, like every neighbouring case, and - because even that
depends on the backend advertising two image units - it `ASSERT`s that the binding actually
landed (`Ctx().GetImageTextureBinding(1).Texture != nullptr`) before it reads the mark. A bind
refused at `unit >= GetAdvertisedImageUnitCount()` (GL_Texture.cpp:6694) now says so by name
instead of reading as a missing `NoteImageUnitTouched`.

### Not changed, and why

Nothing was disputed on the merits. The reviewers' "checked and cleared" sections (stamp-table
equivalence, memo key completeness term by term, single-writer / no new concurrency hazard, the
image-unit mark's writers, G1 = 0 in the pull build, the new tests' typing,
`ParsePoisonOmissionKnob`'s equivalence) are unaffected by this round: the stamp table, the knob
fast path, the image-unit mark and every `#if` guard are byte-identical to the reviewed patch.
The only disagreement is the one recorded under the major, and it is a disagreement about the
proposed FIX, not about the finding.

### Risks, restated

- **Still not built.** This round adds one more `constexpr` function with a loop
  (`NoP4aFamilyFieldIsWhollySupplied`) beside the stamp table's `MakeAnswerableMaskTable`, in the
  same standard-conforming shape; no toolchain has seen either.
- The new test step depends on `MG_Config::Transport == Monolith` to move the consumer signal and
  skips its body otherwise, so in a split-transport lane step 4 does not run. That is the right
  trade today: the assertion it would make there is the same dominated one, and the
  `static_assert` covers every lane.
