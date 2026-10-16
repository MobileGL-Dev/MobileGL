# P4a contract fix c0e — `MGPipeFramebufferTarget::Named` and the metadata respecify

Branch `p4a/c0e`, worktree `~/w7/p4a-c0e`, based on `refs/heads/feat/disaggregated` = **17db7598** (c0 + c0b + c0c).
**One commit: `11446f35`.**

```
[Fix] (Pipe): key the framebuffer record to the object it names - a fourth Target value for every
DSA entry point that hands a framebuffer over by name, and the metadata-only respecify the sticky
bind mask needs
```

Files: `MobileGL/MG_Pipe/MGPipeTypes.h`, `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` (+160 / −15).
No other file touched — `Tracker.h` and `PipeFill.cpp` are c0d's and were never opened for writing.
`PipeFields.def` is unchanged: nothing was renamed and no member was added, so `gen_pipe` has nothing to
regenerate.

---

## 1. The declaration wire v3 / B v2 / D v2 quote

`MGPipeTypes.h`, the enum immediately above `kMGPipeMaxColorAttachments`:

```cpp
    enum class MGPipeFramebufferTarget : Uint8 {
        Draw = 0,
        Read = 1,
        Both = 2,
        // Describes the framebuffer named by Fbo and changes no binding (ID-19). Emitted
        // ahead of every DSA entry point that hands that framebuffer over by name.
        Named = 3,
        Count,
    };
```

`Count == 4`. Wire's existing validate point (`PipeApply.cpp:2046`, `state.Target >= Count` refuses) therefore
starts accepting 3 with no edit at that line — **wire v3's declared local constant for 3 is now retired** and
`MGPipeFramebufferTarget::Named` is spelled directly.

`MGPFramebufferState` is **unchanged in layout**: `MGP_ASSERT_POD(MGPFramebufferState, 304)` still holds and
`Uint8 Target` is the same byte. `Named` cost nothing on the wire — it is a fourth value of a byte that already
existed, which is why the applier can be given a per-object table without a payload change.

`MGPResourceDesc` is likewise unchanged (`MGP_ASSERT_POD(MGPResourceDesc, 88)`). **No field was added anywhere.**

## 2. The exact comment text (quote verbatim; it is the contract)

### 2.1 Above `enum class MGPipeFramebufferTarget`

```
    // P4a, D-C2/D-C3 and ID-19: THE RECORD DESCRIBES A FRAMEBUFFER OBJECT, and Target says
    // whether it ALSO moves a binding.
    //
    // A `Named` record describes the framebuffer object it names (Fbo) and changes NO
    // binding. `Draw` / `Read` / `Both` records describe the same object AND set the bound
    // handle(s) of the target(s) they name.
    //
    // The applier therefore keeps records PER FRAMEBUFFER OBJECT, keyed by the handle's slot
    // (the generation is checked on lookup and a stale one refuses; a framebuffer has no wire
    // lifetime - D-I2, the catalogue has no framebuffer delete - so a successor's record
    // simply OVERWRITES the slot), plus the two bound handles. And every DSA entry point that
    // hands a framebuffer to the server BY NAME - BlitNamedFramebuffer, the four
    // ClearNamedFramebuffer*, and the DSA attachment / draw-buffer / read-buffer setters at
    // their validate point - is PRECEDED BY A Named RECORD, so that any framebuffer the
    // server is about to receive by name already has one.
    //
    // That last rule is the phase's main correction, not a nicety. With only the two
    // bound-target records, glClearNamedFramebufferfv(fbo) on an unbound fbo made the backend
    // mint a fresh driver framebuffer with NO ATTACHMENTS, find no record for it, decline,
    // and then issue the clear against it anyway - GL_INVALID_FRAMEBUFFER_OPERATION and
    // nothing cleared, where the legacy arm cleared correctly. Writing such an object into
    // the bound-target record instead would have been worse: the applier would then claim it
    // is bound.
    //
    // GL has two independent framebuffer bindings and one record carries one Fbo, so a
    // Draw/Read pair is two records and one object bound to both targets is one record with
    // Both. The draw-buffer array belongs to the OBJECT the record names; it reaches the
    // driver's bound draw framebuffer only for a record whose Target is Draw or Both -
    // Espryt's own comment records the Minecraft 26.x OIT bug where a READ-only sync landed
    // glDrawBuffers on the wrong framebuffer - and a Named record's draw buffers are applied
    // when that object is next configured, never to whatever happens to be bound. ReadSurface
    // is resolved from THAT framebuffer's own read buffer in EVERY record, Named included,
    // which is what makes the read-buffer-shared-FBO defect class unrepresentable rather than
    // merely fixed.
    //
    // Target IS A ContentHash INPUT (the hash covers the whole record), and the emitter's
    // suppressor must be keyed by the framebuffer the record names, not by one global slot:
    // two different objects' Named records in a row must both go out, and a Named record must
    // never be suppressed against the same object's bound record or the reverse.
```

### 2.2 Above `struct MGPFramebufferState` — the field-by-field statement (item 3)

```
    // ONE RECORD DESCRIBES ONE FRAMEBUFFER OBJECT - the one named by Fbo - and Target says
    // whether it also moves a binding (P4a, D-C2 as corrected by ID-19; see
    // MGPipeFramebufferTarget above for the failure that forced it).
    //
    // A Named record describes that object and changes NO binding. Draw / Read / Both records
    // describe that object AND set the bound handle(s) of the target(s) they name. The applier
    // keeps these records PER FRAMEBUFFER OBJECT, keyed by Fbo's slot (generation checked on
    // lookup; a framebuffer has no wire lifetime - D-I2 - so a successor's record simply
    // overwrites the slot), plus the two bound handles; every DSA entry point that hands a
    // framebuffer to the server by name is preceded by a Named record.
    //
    // WHAT Target CHANGES, FIELD BY FIELD. NO FIELD IN THIS RECORD REFERS TO "the currently
    // bound framebuffer" - every one of them describes the object named by Fbo - and that is
    // the invariant a reader depends on:
    //
    //   Fbo, Color[], Depth, Stencil, ReadSurface, Width/Height/Layers/Samples,
    //   FixedSampleLocations, IsDefault, Complete
    //       properties of the object named by Fbo, identical in meaning under every Target.
    //       In particular ReadSurface is resolved from THAT framebuffer's own read buffer -
    //       on a Named record too - never from whichever framebuffer is bound to GL_READ.
    //   DrawBuffers[]
    //       a property of the named object; it reaches the driver's bound draw framebuffer
    //       only when Target is Draw or Both. Under Named it is stored with the object and
    //       applied when that object is next configured.
    //   Target
    //       the only binding-specific field: Draw/Read/Both name the binding(s) this record
    //       also sets, Named names none. It is a ContentHash input.
    //   ContentHash
    //       per RECORD, not per object, and the emitter's suppressor is keyed by the
    //       framebuffer named: a Named record must never be suppressed against the same
    //       object's bound record, nor one object's Named record against another's.
```

**Answer to item (3), stated plainly: exactly one field's meaning is binding-specific, and it is `Target`
itself.** Every other member describes the object named by `Fbo` under every `Target`. Two of them needed
their in-struct comments amended because they read as bound-relative before:

```cpp
        // The RESOLVED read surface, not an index, and it is THIS framebuffer's own read
        // buffer under every Target - Named included. This is what structurally closes the
        // read-buffer-shared-FBO defect class.
        MGPSurface ReadSurface;
        // attachment index, -1 = NONE. The named object's array; applied to the bound draw
        // framebuffer only when Target is Draw or Both.
        Int8 DrawBuffers[8];
...
        // MGPipeFramebufferTarget, above (P4a, D-C2; was Pad0). Draw/Read/Both also set the
        // named binding(s); Named sets none (ID-19). The ONLY binding-specific field.
        Uint8 Target;
...
        // ... (existing ContentHash comment) ...
        // Target is one of its inputs, and the suppressor is keyed per framebuffer.
        Uint64 ContentHash;
```

No field was added. `MGP_ASSERT_POD(MGPFramebufferState, 304)` unchanged, `MGP_FIELDS_MGPFramebufferState`
unchanged.

### 2.3 Beside `MGPipeResourceRespecifyNeedsAck` — the metadata respecify (item 4, ID-18 M4)

```
    // P4a, ID-18 M4: A RESPECIFY WHOSE STORAGE-DEFINING FIELDS ALL EQUAL THE STORED
    // DESCRIPTOR IS A METADATA UPDATE, NOT A REALLOCATION.
    //
    // MGPResourceDesc::BindMask and ImageBindableHint are STICKY facts the client discovers
    // AFTER allocation - a texture first bound as a shader image, first used as a render
    // target - and they ride resource_create and every resource_respecify. An IMMUTABLE
    // texture never has a later respecify, so without a rule those two would reach the server
    // only by accident, or never; with one, a mask change after allocation emits a
    // resource_respecify that REPEATS the storage the resource already has.
    //
    // The applier and both twins must classify such a record as a metadata update:
    //   - NO reallocation acknowledgement. MGPipeResourceRespecifyNeedsAck above still
    //     answers the per-record question, but a metadata update allocates nothing, so a
    //     record it classifies as metadata is not acked even when that predicate says the
    //     call may require one.
    //   - NO PendingUploads clear - not the whole vector, and not the redefined level either.
    //     This REFINES the level-scoped clear: identical storage fields clear NOTHING. (The
    //     level-scoped rule exists because clearing the whole vector on a level-1 definition
    //     silently dropped level 0's accepted texels; a metadata update must drop neither.)
    //   - The stored descriptor's BindMask and ImageBindableHint ARE updated - BindMask is
    //     sticky and therefore ORed, never replaced - and the twin re-derives its storage
    //     flags from the new mask on its next sync, recreating backend storage only where the
    //     backend actually needs it. The record itself is not a request to recreate.
    //
    // THE STORAGE-DEFINING FIELD SET, named here so that neither side has to guess and a
    // later field cannot join it by silence. It is every MGPResourceDesc member except the
    // three metadata ones and the padding:
    //
    //     Target, StorageKind, InternalFormat, Width, Height, Depth, ArrayLayers, Levels,
    //     Samples, FixedSampleLocations, Immutable, Usage, StorageFlags, HasDefinedContent,
    //     ViewOf, BufferForTexBuffer, BufOffset, BufSize.
    //
    // `Resource` is the identity the stored descriptor is looked up BY, not a comparand. The
    // three fields that may differ on a metadata update are exactly BindMask,
    // ImageBindableHint and GlNameForDiag (diagnostics only, never an identity, never a memo
    // key). HasDefinedContent is storage-defining ON PURPOSE: glBufferData(size, NULL) at an
    // unchanged size is an orphaning reallocation and has to keep clearing, rather than being
    // mistaken for a mask change. A field added to MGPResourceDesc must be placed in one of
    // the two lists in the same commit - PipeCatalogue pins the struct's size for that.
```

A back-pointer sits at the fields themselves so a reader who starts at `MGPResourceDesc` finds the rule:

```cpp
        Uint8 HasDefinedContent;  // false after a NULL-data respecify - STORAGE-DEFINING
        Uint8 ImageBindableHint;  // client-side everImageBound; pre-emptive allocation
        Uint16 Pad0;
        // ImageBindableHint and BindMask above are the two METADATA fields the rule exists
        // for: a respecify that moves only them - every storage-defining field equal to the
        // stored descriptor - is a metadata update, with no reallocation ack and no
        // PendingUploads clear. The rule, the full storage-defining field set and the third
        // field allowed to differ (GlNameForDiag) are stated beside
        // MGPipeResourceRespecifyNeedsAck below (P4a, ID-18 M4), which is where the applier
        // and both twins read them from.
```

Two judgement calls consumers should know about, because they decide behaviour:

- **`HasDefinedContent` is storage-defining.** It is not a shape field, but `glBufferData(size, NULL)` at an
  unchanged size is an orphaning reallocation. Classifying it as metadata would have made orphaning stop
  clearing pending uploads.
- **`GlNameForDiag` is not.** It is diagnostics-only by the phase's own rule (a GL name is never an identity,
  never a memo key, never a content-hash input), so a record differing only in it classifies as metadata,
  which is correct — it allocates nothing.
- **No predicate function was minted.** The rule needs the applier's *stored* descriptor as its second
  operand and this header sees only one, so the contract states the field set in prose and the applier owns
  the comparison. If the integrator wants one authority for the comparison, the natural home is wire v3's
  applier, written against the list above verbatim; c0e deliberately did not front-run it while wire v3 was
  in flight.

## 3. Unit test (item 5)

No new test name — the two existing A-owned catalogue cases were extended, so the name inventory is
unchanged (see §4). `MGP_ASSERT_POD` untouched in both directions.

`PipeCatalogue.TextureParamsNameTheirBuiltinSamplerAndFramebufferStateNamesItsTarget` now pins all five
values plus the byte:

```cpp
    EXPECT_EQ(sizeof(MGPFramebufferState), 304u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Draw), 0u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Read), 1u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Both), 2u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Named), 3u);
    EXPECT_EQ(static_cast<Uint8>(MGPipeFramebufferTarget::Count), 4u);
    EXPECT_LE(static_cast<Uint32>(MGPipeFramebufferTarget::Count), 256u);
    EXPECT_EQ(sizeof(MGPFramebufferState::Target), 1u);
```

`Named` is pinned **by value** on purpose: the applier's only validation is `Target >= Count`, so an
enumerator inserted ahead of `Named` would silently re-point every Named record the client emits at `Draw`
or `Read` — and a Draw record for a framebuffer that is not bound is the exact corruption `Named` exists to
prevent. `Count == 4` is pinned for the mirror failure: a wire that still refused 3 would drop every DSA
record on the floor.

`PipeCatalogue.ResourceRespecifyAcksOnlyImmutableStorage` gained the tripwire for the prose rule — the
thing that would make the field list lie is a member added to `MGPResourceDesc` and classified into neither
list:

```cpp
    EXPECT_EQ(sizeof(MGPResourceDesc), 88u);
    EXPECT_EQ(sizeof(MGPResourceDesc::BindMask), 2u);
    EXPECT_EQ(sizeof(MGPResourceDesc::ImageBindableHint), 1u);
    EXPECT_NE(offsetof(MGPResourceDesc, HasDefinedContent),
              offsetof(MGPResourceDesc, ImageBindableHint));
```

## 4. Gates (every number)

| gate | result |
|---|---|
| `symbol_report --threshold 0`, before `~/w7/p4a-before-libMobileGL.so`, after `build-linux/libMobileGL.so` | rc 0 — **0 added / 0 removed / 0 resized / 0 renamed**, 27811 → 27811 defined symbols, 27072 unchanged; `.text` 10806323 → 10806323 (+0, +0.000%); file bytes 19114360 → 19114360 (identical) |
| `ctest -L unit` in `build-linux` (pull) | rc 0 — **100% passed, 0 failed out of 1638** |
| `ctest -L unit` in `build-push` (`-DMOBILEGL_PIPE_PUSH=ON`) | rc 0 — **100% passed, 0 failed out of 1638** |
| test-name inventory (full `ctest -N`, `LC_ALL=C`) | 2604 pull == 2604 push; vs `~/w7/p4a-before-ctest-names.txt`: **0 removed / +16 added** — identical to ID-20's post-c0c number, i.e. c0e adds no name (G14 additions-only trivially holds) |
| `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0** — the 11 pool / deferred-release / ring / flush-drain functions byte-identical |
| `gen_pipe.py --check` | rc 0 — 71 calls (11 screen, 60 context), 72 verify payloads, 63 PipeInputs fields, 69 verbs, 9 classes; inventory 477 rows, 0 UNMAPPED; generated files up to date |
| `gen_pipe.py --self-test` | rc 0 — 7 negative-control trips, positive control OK |
| `gen_pipe_dirty_surface.py --check` | rc 0 — 2 UNDECIDED (the two D6 rows, unchanged), 0 COARSE |
| `gen_pipe_dirty_surface.py --self-test` | rc 0 — 27 negative controls, all tripped; positive controls OK |
| `check_include_closure.py --compiler clang++` | rc 0 — 4 probes, 0 skipped, 0 problems (value 66 / artifacts 65 / mutation 84 / wire 2 headers, 0 forbidden each) |
| `check_include_closure.py --mode both --compiler clang++` (extra) | rc 0 — 4 probes, 0 skipped, 0 problems; all four SELF-CONTAINED OK under `-fsyntax-only` |

Builds: `build-linux` rc 0 (994 targets), `build-push` rc 0 (1007 targets), both Release + Ninja + ccache,
`CCACHE_BASEDIR=/home/swung/w7`, `-j 8`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported for the
build and test lanes.

## 5. Notes for the integrator

1. **Wire v3's local constant for `Target == 3` can be deleted at the rebase.** `MGPipeFramebufferTarget::Named`
   exists and `Count` is 4, so `state.Target >= Count` accepts 3 with no change at the validate point.
2. **What c0e did NOT do, because the files were not granted.** ID-19(a) also asks for the D-K2 fourth row
   ("bit 10 requires bit 11") and G9-as-white-box to be recorded in the brief/docs. `docs/` and
   `BRIEF-P4A.md` are outside my grant (my files were `MGPipeTypes.h` + the A-owned catalogue test), and
   ID-15's own correction puts the fourth row's *implementation* in package D's texture-family
   `Resolve*SubsystemArm`. Both still need an owner; neither is a contract-header edit.
3. **The per-object table's key.** The comment says "keyed by the handle's slot, generation checked on
   lookup, a stale generation refuses, a successor's record overwrites the slot". Wire v3 should read that
   as: the slot is the index, `Gen` is a validity test on read, and there is no removal path — consistent
   with D-I2 (no framebuffer delete call) and with `ReleaseObjectRecords` clearing the whole table plus both
   bound handles (wire m1).
4. **B v2's suppressor is now under-specified by its current shape.** `EmitFramebufferState` feeds one
   global `MGPipeSuppressorSlot::SetFramebufferState` slot with a combined draw/read hash. A Named record
   for an arbitrary framebuffer cannot share that slot: two different objects' Named records in a row would
   suppress each other. The contract comment states the requirement (suppression keyed by the framebuffer
   named); the mechanism is B's.
5. **B v2's `ReadSurface` build changes.** Today `BuildFramebufferState(fbo, readSurfaceSource, target, out)`
   passes the *read* framebuffer as the surface source for a Draw record. Under this contract `ReadSurface`
   is always the record's own framebuffer's read buffer, so the second parameter collapses into the first.

## 6. Housekeeping

Worktree `~/w7/p4a-c0e` **left in place** on branch `p4a/c0e` at `11446f35` (integrator rebases onto c0d and
fast-forwards). Nothing pushed. My build/round logs under `~/w7` (`p4a-c0e-*.log`) and my script directory
`~/w7/c0e-scripts` are removed; the shared WSL scratchpad was not touched. The build directories themselves
(`~/w7/p4a-c0e/build-linux`, `build-push`) are left inside the worktree.
