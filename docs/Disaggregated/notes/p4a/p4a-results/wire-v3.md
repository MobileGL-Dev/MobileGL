# P4a package A — `wire`, round v3. Result

The round ID-21 launched: rebase onto c0c, retire v2's local packed-target decode, **ID-19(b)** (the per-object
framebuffer table), **ID-18 M3** (acceptance returns on create and respecify) and **ID-18 M4** (the metadata
respecify). Worktree `/home/swung/w7/p4a-wire`, branch `refs/heads/p4a/wire`, **not pushed**.

**HEAD = `70e742a3`**, three new commits on top of the nine rebased ones. Only
`MobileGL/MG_Pipe/PipeApply.{h,cpp}` and two `MG_Test/Pipe/*EmitTest.cpp` are touched; nothing outside package
A's own C.7 set was opened.

---

## 0. Rebase onto `refs/heads/feat/disaggregated` = `17db7598` (c0 + c0b + **c0c**)

Clean, nine commits, **no conflicts** (c0c touches `MGPipeTypes.h`, `PipeFields.def` and
`PipeCatalogueTest.cpp`; wire owns `PipeApply.{h,cpp}` and three of the emit test files). The mapping:

| v2 | after rebase | |
|---|---|---|
| `86835b50`/`686ad38c` | `395cb841` | w1: the framebuffer + resource records |
| `bc9abfbd`/`e5038c3d` | `ed635ea5` | w2: samplers, views, unit sets |
| `0f990633`/`59bceb4c` | `0ce08d03` | w3: shader CSOs, the default uniform block |
| `3c07c8c3`/`529cb262` | `b2a53c90` | v1's test commit |
| `3c5b2d01` | `7b5b3957` | C1 + m4 + m3 |
| `026d2c48` | `fb709418` | m1 |
| `9f75fbbb` | `52a16a1e` | m5 |
| `aaad9691` | `be0e032a` | m2 + n1 + n3 |
| `6f263284` | `22a3e265` | v2's test commit |

**LFS note (ID-13).** The 92 `tools/trace_replay/fixtures/*` blobs were 132-byte POINTERS in this tree *before*
this round (mtime 12:24, the WSL restart), and `git lfs checkout` fails here — `/usr/bin/git-lfs` exists but the
repo has no `filter.lfs.smudge` configured. They are `assume-unchanged` (`git ls-files -v` → `h`), so I restored
all 92 by copying from `~/w7/base` and the working tree stayed clean (`git status --porcelain` empty,
0 pointers left). No retrace ran this round and none was needed — see §6.

## 1. The three new commits

| # | hash | items | message |
|---|---|---|---|
| 1 | `aff381c7` | **ID-19(b)** | `[Feat] (Pipe): key the framebuffer record by the framebuffer handle and resolve the two bindings through it, so a framebuffer that is named but bound to neither can still be described` |
| 2 | `282325d5` | **c0c retirement**, **ID-18 M3**, **ID-18 M4** | `[Fix] (Pipe): read a sub-data record's target halves through the contract's own accessors, answer the emitter whether a create or a respecify was accepted, and apply a respecify that restates the stored storage as a metadata update rather than a redefinition` |
| 3 | `70e742a3` | the five new cases | `[Test] (Pipe): pin what a Named framebuffer record does and does not move, the stale-generation refusal on the per-object table, the two handles it will not store, and that a respecify which redefines no storage carries the mask without eating the texels` |

`aff381c7` was verified independently (build rc 0, the seven emit suites 69/69) before `282325d5` was applied, so
the history bisects. The three items in commit 2 are one commit because they share hunks: the decode retirement
and the metadata predicate are adjacent blocks in the same anonymous namespace, and the acceptance return and the
metadata arm are the same two function bodies.

---

## 2. Item by item

### (1) c0c's accessors — v2's local decode is gone

- **`PipeApply.cpp:515-524`** — v2's `SubDataResourceTargetOf` (the local `& 0x00FF`) is **deleted**. The comment
  that replaces it says the encoding is the contract's, names the three helpers, and states that nothing in this
  file open-codes a half any more.
- **`PipeApply.cpp:1698`** — the m3 gate now reads `MGPipeSubDataResourceTargetOf(record.Target)` and compares
  `Uint8` against `Uint8`.
- **`PipeApply.cpp:1701-1710`** — the refusal line gained the **upload half**, through
  `MGPipeSubDataUploadTargetOf`: *"the record's resource target names no texture to upload into (target=%u,
  resource target=%u, upload target=%u)"*. A reader no longer has to do the arithmetic the encoding exists to
  stop anyone doing by hand, and the second half is what says whether an emitter packed the pair the wrong way
  round. The existing case's needle stops before the parenthesis and is unaffected.
- **`TextureEmitTest.cpp:490`** — the case's hand-packed `(1u << 8) | kTex2D` is now
  `MGPipePackSubDataTarget(kTex2D, 1u)`.

**Unchanged, deliberately:** `SubDataNamesABuffer` still matches the **whole field** against
`kMGPipeResourceTargetBuffer` (ID-12, and c0c's own `static_assert` is what keeps that exact), and
`MGPipeResourceRecord::PendingUpload::UploadTarget` still stores `MGPSubData::Target` **verbatim**. ID-12 gives
the high-byte decode to package D at its two match sites, and `MGPRespecifiedLevel::UploadTarget` is documented
to B as "the same packed value that level's sub-data emission uses" — changing the stored key here would silently
break that pairing. contract-v3 §4 leaves the choice to D; wire stays as ID-12 wrote it.

### (2) ID-19(b) — the framebuffer record is per object

**New in `PipeApply.h`:**

| where | what |
|---|---|
| `:156` | `kMGPipeMaxFramebufferSlots = 1u << 16` — the bounds gate the five object tables already carry |
| `:173` | `kMGPipeFramebufferTargetNamed = 3`, the **declared local** ID-21 asked for, with… |
| `:177-179` | …a `static_assert(static_cast<Uint8>(MGPipeFramebufferTarget::Count) == kMGPipeFramebufferTargetNamed)`. The day c0e lands, `Count` becomes 4 and **this fires**: the retirement cannot be forgotten |
| `:182-186` | `kMGPipeFramebufferBindingCount = 2` + a `static_assert` that `Draw == 0 && Read == 1` |
| `:397-401` | `struct MGPipeFramebufferRecord { Uint32 Gen; Bool Live; MGPFramebufferState State; }` |
| `:504` | `Vector<MGPipeFramebufferRecord> FramebufferRecords` — **beside the five object tables**, not in the working-state block |
| `:597` | `Array<MGPipeHandle, kMGPipeFramebufferBindingCount> BoundFramebuffer{}` (replaces the two `MGPFramebufferState` members) |
| `:614` | `mutable Uint64 StaleFramebufferRecordLookups = 0` |
| `:663-665` | `FramebufferRecordFor(MGPipeHandle) const`, `DrawFramebuffer() const`, `ReadFramebuffer() const`, all returning `const MGPFramebufferState*` |

**Bodies in `PipeApply.cpp`:**

- **`:2144` `MGPipeApplySetFramebufferState`.** The target gate is now `state.Target > kMGPipeFramebufferTargetNamed`
  (so **3 is admitted**). Two new refusals, both `Fatal{ProtocolCorruption}` and neither counted: the **null
  handle** (`{0,0}` is what "nothing is bound" reads as, so a record installed there would be answered to every
  caller asking about an empty binding) and a **slot at or above the bound**. Then: the record is written at
  `FramebufferRecords[state.Fbo.Slot]` **whatever the target is**; Draw/Both sets `BoundFramebuffer[Draw]`,
  Read/Both sets `BoundFramebuffer[Read]`, **Named sets neither**; `++FramebufferSerial` on every applied record,
  Named included.
- **`:2235` `MGPipeApplierState::FramebufferRecordFor`.** Null for a null handle (silent — that is an unbound
  binding, not an error), null for a slot with no record (silent — that is every framebuffer before its first
  emission), and for a **stale generation**: `++StaleFramebufferRecordLookups` and one `MGLOG_E_ONCE` naming both
  generations, then null. Loud, because the only way to reach it is an emitter defect and the consequence on the
  handle arm is a blit or clear into another framebuffer's attachments. Defined in the `.cpp`, not inline in the
  header, so the header's include closure does not grow a logger.
- **`:2266`/`:2270` `DrawFramebuffer()` / `ReadFramebuffer()`** = `FramebufferRecordFor(BoundFramebuffer[t])`.
- **`:1176` `MGPipeApplierReset`** clears **`BoundFramebuffer` only** and keeps the table. A make-current is a
  binding change; the record is an object record. Dropping the table there would lose the record of every
  framebuffer that is addressed only BY NAME, because the client's re-emission on a fresh context is driven by
  `MGPipeSetHashSuppressor::InvalidateAll`, which re-sends the two BOUND records and nothing else.
- **`:1222`/`:1236` `MGPipeApplierReleaseObjectRecords`** clears the table **and** the bound handles (m1
  extended). Here there is nothing to outlive: the context is going away, its framebuffers are container objects
  that go with it, and a table left standing would answer a lookup from the next context on a re-minted slot.

I checked c0e's in-flight `MGPipeTypes.h` in `~/w7/p4a-c0e` (uncommitted, branch still at `17db7598`) and wrote
to match it: `Named = 3`, "describes the framebuffer object it names and changes NO binding", per-object table
keyed by the handle's slot with the generation checked on lookup, a successor's record overwriting the slot.
**Nothing was cherry-picked.**

### (3) ID-18 M3 — acceptance returns on create and respecify

`Bool MGPipeApplyResourceCreate` (`PipeApply.h:800`, `.cpp:1524`) and `Bool MGPipeApplyResourceRespecify`
(`:848` / `:1576`). Contract at `PipeApply.h:786-799`, written once for all three calls: the emitter cannot see
either refusal from its call site, because a dead or stale handle is a counted no-op and a corrupt record is a
`Fatal` that deliberately moves no counter.

- **create** returns false for the reserved slot 0, a target that names no resource kind, and a slot past
  `kMGPipeMaxResourceSlots`; true otherwise, dispatch or no dispatch.
- **respecify** returns false for a target that names no resource kind and for a handle with no live record at
  that generation; true otherwise, **metadata updates included** — the record did move.
- Source-compatible: `PipeFill.cpp`'s P3a call sites discard the value, nothing is `[[nodiscard]]`, and
  `gen_pipe.py` never parses this header (wire review W1) — `git diff -- MobileGL/MG_Pipe/generated` is rc 0 and
  `gen_pipe.py --check` is rc 0.

### (4) ID-18 M4 — the metadata respecify

`RespecifyRedefinesNoStorage(stored, next)` (`PipeApply.cpp:558`), asked **before** the descriptor is replaced
(`:1595`), and its three consequences:

- `record->Desc = desc` — BindMask and ImageBindableHint take their new values (that is the whole call);
- `++record->Serial` — the entire publication; the twin re-derives its storage flags from the new mask at its
  next sync and decides for itself whether the backend needs a recreate;
- `:1640` — the **third arm of the pending-upload clear**: a metadata update drops **nothing**, whatever `level`
  says. This refines C1 rather than contradicting it: C1's rule drops the uploads against the storage a call
  REPLACES, and a call that replaces no storage replaces no level's coordinate system.
- `MGPipeResourceRespecifyNeedsAck` is false for every such record **by construction** (`:1660-1670`), not by a
  second suppression rule — see the deviation below.

**The comparison is field by field, not a memcmp** (`MGPResourceDesc` carries `Pad0`/`Pad1` and neither side is
guaranteed to have written them — the padding coin flip clientsp's sampler cache already paid for once).

### (5) The tests

Five new cases; three existing ones extended to the new shape. Every case is a visible SKIP in a pull build, so
`ctest -N` stays name-for-name identical between the pull and the push trees.

| suite | case | pins |
|---|---|---|
| `FramebufferEmit` | `ANamedRecordDescribesTheFramebufferItNamesWithoutMovingEitherBinding` (`:431`) | the Named record is found **by handle** with its attachments; **neither** binding moved (two *different* framebuffers are bound first, so it is an assertion about values and not about null); the serial moved anyway; the same object can then be bound |
| `FramebufferEmit` | `AFramebufferHandleWhoseGenerationHasMovedOnIsRefusedRatherThanAnswered` (`:488`) | stale gen → null **and** `StaleFramebufferRecordLookups + 1`, in both directions across a recycle; the null handle and an undescribed slot are **not** counted; `RefusedObjectCalls` stays 0 |
| `FramebufferEmit` | `AFramebufferRecordThatNamesNoUsableHandleIsRefusedRatherThanStored` (`:528`) | the default framebuffer `{0,1}` is the positive control; the null handle and a slot at the bound are refused by name; nothing stored, no serial, no resize |
| `TextureEmit` | `ARespecifyThatRedefinesNoStorageCarriesTheStickyMaskAndKeepsThePendingUploads` (`:687`) | the immutable `glTexStorage2D` → `glTexSubImage2D` → `glBindImageTexture` order; mask and hint land, the pending upload survives, the serial moves, `NeedsAck` is false; **with a level pointer too**; and the negative control — move one storage field and the same call drops the level |
| `TextureEmit` | `TheCreateAndRespecifyCallsAnswerWhetherTheRecordWasAccepted` (`:757`) | true on both, false on the reserved slot and on a stale generation (with `RefusedResourceCalls + 1`), and the buffer half on the same terms with no backend table registered |

Extended: `ADrawRecordAndAReadRecordAreKeptApartAndBothWritesBoth` (`:216` — Draw moves Draw only, Both moves
both, **and the two earlier framebuffers keep their own records**); `AMakeCurrentClearsBothRecordsAnd…` (`:334` —
both accessors null, both handles null, **the record survives**); `AReleaseOfTheObjectRecordsAlsoClears…`
(`:371` — `FramebufferRecords` is **empty** afterwards). `ATargetOutsideTheThreeBindings…` (`:270`) now drives
`kMGPipeFramebufferTargetNamed + 1`, because 3 is legal.

`ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` (C1's case, `:473`) was **rewritten to stay a gate**: it
now gives each respecify the descriptor the call it models would carry (`Levels` 1 → 2 as a mip build adds
level 1 — package B's `desc.Levels = mipmap->GetMipmapLevelCount()`, which is also why B's own memcmp dedupe
emits that respecify at all; and a new `InternalFormat` for the redefinition of level 1). Fed the *same*
descriptor three times, as v2 wrote it, it would have been exercising the metadata arm instead of the per-level
one. MU8 below proves C1's gate survived the restructure.

---

## 3. Deviations

**W10 — a BUFFER respecify is never classified as metadata-only, and the comparison is wider than the brief's
list.** ID-21 names the storage-defining set as InternalFormat / Width / Height / Depth / ArrayLayers / Levels /
Samples / FixedSampleLocations / StorageKind / Target / the buffer-texture fields. The implementation compares
**everything except** `BindMask`, `ImageBindableHint`, `GlNameForDiag` and the two padding words — so also
`Immutable`, `HasDefinedContent`, `Usage`, `StorageFlags` and `ViewOf` — and refuses the buffer target outright.
Two reasons, both written at `PipeApply.cpp:526-557`:

1. **Direction of the asymmetry.** Comparing too much can only mis-classify a metadata update as a redefinition,
   whose cost is one level's pending upload dropped by a call B does not emit. Comparing too little
   mis-classifies a REDEFINITION as metadata and keeps boxes in a coordinate system that no longer exists —
   an upload past the end of the new level. None of the extra fields moves on a mask republish (B re-publishes
   the same descriptor with a new mask), so the wider set costs M4 nothing.
2. **`glBufferData` at an unchanged size is the canonical ORPHANING idiom** — a real reallocation whose whole
   point is that the old store is gone — and `glBufferStorage` is the one entry point allowed a synchronous ack.
   Excluding the buffer target is what makes ID-18 M4's *"no reallocation ack
   (`MGPipeResourceRespecifyNeedsAck` false)"* true **by construction** rather than by a second suppression rule
   the transport would later have to learn: the predicate is already false for every non-buffer target. ID-18 M4
   is a statement about a texture's (and a renderbuffer's) sticky mask, and a buffer has no pending upload for
   the arm to protect.

**W11 — "identical storage fields clear nothing, whatever the level pointer says" is implemented literally, and
here is its one sharp edge.** A per-level respecify whose descriptor happens not to move would now keep the very
level it redefines. On the tree as built that is unreachable: B publishes `desc.Levels` from
`MipmapStorage::GetMipmapLevelCount()`, so the glTexImage\*D that defines a new level moves the descriptor, and
B's 88-byte memcmp dedupe means a respecify whose descriptor did **not** move is never emitted at all. The case
at `TextureEmitTest.cpp:687` pins the ruling as written (a metadata update with a non-null level drops nothing)
and the case at `:473` pins the other arm on the descriptors a real mip build produces. **If B's rework ever
emits a per-level respecify with a byte-identical descriptor, that level's pending upload survives the
redefinition** — worth ten lines of a reviewer's attention on B's `AllocateStorage` path, and invisible to every
gate P4a has.

W1–W6, W8 and W9 stand as v1/v2 recorded them and as the reviews accepted them. **W7′ is superseded by W11's
third arm** (the scope argument is unchanged; the metadata arm now precedes it).

---

## 4. Mutation evidence — nine mutations, nine red, each killing exactly one case

Measured in `build-push` at HEAD; each applied to `MobileGL/MG_Pipe/PipeApply.cpp`, the tree rebuilt, the seven
emit suites run (74 tests), the file restored from the commit. **The verdict is `ctest`'s exit code**, not a grep.

| # | mutation | verdict |
|---|---|---|
| MU1 | the Named arm also moves the draw binding (`Target == Draw \|\| Both` → `Target != Read`) | rc 8, 1/74 — `FramebufferEmit.ANamedRecordDescribesTheFramebufferItNamesWithoutMovingEitherBinding` |
| MU2 | the stale-generation test disarmed | rc 8, 1/74 — `FramebufferEmit.AFramebufferHandleWhoseGenerationHasMovedOnIsRefusedRatherThanAnswered` |
| MU3 | the null-framebuffer-handle refusal disarmed | rc 8, 1/74 — `FramebufferEmit.AFramebufferRecordThatNamesNoUsableHandleIsRefusedRatherThanStored` |
| MU4 | the metadata-respecify arm disarmed | rc 8, 1/74 — `TextureEmit.ARespecifyThatRedefinesNoStorageCarriesTheStickyMaskAndKeepsThePendingUploads` |
| MU5 | a refused respecify answers accepted | rc 8, 1/74 — `TextureEmit.TheCreateAndRespecifyCallsAnswerWhetherTheRecordWasAccepted` |
| MU6 | the teardown no longer drops the framebuffer records | rc 8, 1/74 — `FramebufferEmit.AReleaseOfTheObjectRecordsAlsoClearsTheWorkingHandlesThatCouldNameThem` |
| MU7 | a make-current drops the framebuffer records | rc 8, 1/74 — `FramebufferEmit.AMakeCurrentClearsBothRecordsAndAdvancesTheSerialRatherThanZeroingIt` |
| MU8 | **C1's** per-level scoping disarmed (the whole-resource clear again) | rc 8, 1/74 — `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` |
| MU9 | `resource_create` always answers accepted (the reserved-slot arm) | rc 8, 1/74 — `TextureEmit.TheCreateAndRespecifyCallsAnswerWhetherTheRecordWasAccepted` |

After each sweep the tree was restored, rebuilt and re-run: `git status --porcelain` empty, 74/74.

---

## 5. The exact applier API the other packages must call

### Package B — clientfb

1. **Emit a `Named` record at every DSA entry point that hands a framebuffer to Espryt by name** (ID-19(c)):
   `BlitNamedFramebuffer`, the four `ClearNamedFramebuffer*`, and the DSA attachment / draw-buffer / read-buffer
   setters at their validate point. `state.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Named)` once c0e
   lands; **until then `MG_Pipe::kMGPipeFramebufferTargetNamed`** (`PipeApply.h:173`, which B already includes).
   The record's every other field describes the framebuffer named by `Fbo` exactly as a bound record does,
   `ReadSurface` included. The rule is *"any framebuffer Espryt is about to receive by name has a record"*.
2. **The suppressor must be keyed by the framebuffer the record names, not by one global slot.** Two different
   objects' Named records in a row must both go out, and a Named record must never be suppressed against the
   same object's bound record or the reverse. (c0e's comment says this too; the applier cannot see the
   difference, so it is B's to hold up.)
3. **`state.Fbo` may never be the null handle** — it is now `Fatal{ProtocolCorruption}`. `kMGPipeDefaultFramebuffer`
   `{0,1}` for the default framebuffer, a minted `{slot, gen}` otherwise, which is what `HandleFor` already does.
4. **`MGPipeApplyResourceCreate` and `MGPipeApplyResourceRespecify` now return `Bool`.** Together with
   `MGPipeApplyResourceSubData` (v2), that is D-D5 step 1's complete signal: **clear a level's
   `m_isDirty`/`m_dirtyRects`/`m_dirtyRegions`, and advance the per-entry descriptor dedupe, only on true.**
   Discarding compiles.
5. **`MGPipeApplyResourceRespecify`'s third parameter is still `const MGPRespecifiedLevel* level = nullptr`**
   and B must pass it at every per-level respecify, with `UploadTarget` set to **the same packed
   `MGPSubData::Target` value that level's sub-data emission uses** (build it with c0c's
   `MGPipePackSubDataTarget`) and `Level` to the level index. `glTexStorage*`, `glBufferData`,
   `glBufferStorage` and texture views pass `nullptr`.
6. **The mask republish (M4) passes `nullptr` for the level and changes only `BindMask` /
   `ImageBindableHint`.** If any other descriptor field moves in the same emission the applier reads it as a
   redefinition and drops that level's pending upload — see W11.
7. The refusal line for a mis-packed sub-data target now names **both halves**, so a wrong-way-round packing is
   readable straight out of the log.

### Package D — esprytobj

`FramebufferImpl::PushedFramebufferRecord` (`Managers.cpp:8216-8226`) is now one call:

```cpp
const MG_Pipe::MGPFramebufferState* PushedFramebufferRecord(MG_Pipe::MGPipeHandle fbo) {
    return MG_Pipe::MGPipeApplier().FramebufferRecordFor(fbo);   // null = no record / stale gen
}
```

The `asTarget` parameter and the `record.Fbo == fbo` test **go**: the table is keyed by the handle, so a record
that comes back is that framebuffer's by construction, and it comes back whether the object is bound to Draw, to
Read, to both or to neither. That is C-1's fix: `SyncToBackend(fbo, Draw)` for a DSA-named framebuffer now finds
its attachments instead of declining and clearing into an unattached driver FBO.

Where D genuinely needs the **binding** question (rather than the description), it is one array compare:
`MG_Pipe::MGPipeApplier().BoundFramebuffer[static_cast<SizeT>(asTarget == FramebufferTarget::Read
? MG_Pipe::MGPipeFramebufferTarget::Read : MG_Pipe::MGPipeFramebufferTarget::Draw)] == fbo` (ID-19(d)).

Also for D: `MGPipeApplier().StaleFramebufferRecordLookups` is the counter to assert 0 in a scenario;
`MGPipeApplierReleaseObjectRecords` now empties `FramebufferRecords` as well as the bindings.

### Package E — esprytdraw

E's v2 already routed all six reads through one function, and that function is the only thing that changes.
`DirectGLES.cpp:284-287`:

```cpp
static const MG_Pipe::MGPFramebufferState* BoundFramebufferRecord(FramebufferTarget target) {
    const auto& st = MG_Pipe::MGPipeApplier();
    return target == FramebufferTarget::Draw ? st.DrawFramebuffer() : st.ReadFramebuffer();
}
```

— a reference becomes a **pointer**, and `const auto& st` stays const (both accessors are `const`; the stale
counter is `mutable` precisely so E keeps its shape). The seven call sites (`:2127`, `:2273`, `:2863`, `:2864`,
`:2905`, `:4009`, `:4179`) take `const auto*` and null-check. In `SyncCurrentFBOByRecord` (`:2765`) the
half-described test

```cpp
if (MGPipeHandleIsNull(st.DrawFramebuffer.Fbo) || MGPipeHandleIsNull(st.ReadFramebuffer.Fbo)) return false;
```

becomes `if (st.DrawFramebuffer() == nullptr || st.ReadFramebuffer() == nullptr) return false;` — the same
question, and it now also covers "the bound handle's record has a stale generation", which used to be
unrepresentable. `st.FramebufferSerial` is unchanged and still advances on every applied record.

### Package F — gates

`RefusedObjectCalls` must still be **0 across every framebuffer call in every build**; the framebuffer family's
own refusal counter is the separate `StaleFramebufferRecordLookups`, and **0 is the expected value there too** on
an integrated tree — a non-zero one is a seam defect, not traffic.

---

## 6. Gate transcript (HEAD `70e742a3`, working tree clean)

Run in `/home/swung/w7/p4a-wire` with `CCACHE_BASEDIR=/home/swung/w7`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, `-j 8` / `--parallel 8`.

```
build-linux / build-push / build-verify                       rc=0 / rc=0 / rc=0

symbol_report.py --before ~/w7/p4a-before-libMobileGL.so
   --after build-linux/libMobileGL.so --threshold 0
   --fail-on-symbol-set-change --fail-on-added-bytes 0                          rc=0
   .text 10806323 -> 10806323 (+0, +0.000%)
   27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
   27072 -> 27072 normalised names, 27072 unchanged
   ### Removed (0)  ### Added (0)  ### Resized (0)  ### Renamed only (0)

ctest --test-dir build-linux  -L unit    100% passed, 0 failed out of 1680      rc=0
ctest --test-dir build-push   -L unit    100% passed, 0 failed out of 1680      rc=0
ctest --test-dir build-verify -L unit    100% passed, 0 failed out of 1680      rc=0

ctest --test-dir build-push -R 'ResourceEmit\.|FramebufferEmit\.|TextureEmit\.|
     SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.|HandleRecycle'
                                         100% passed, 0 failed out of 130       rc=0

bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD                             rc=0
   the 11 pool / deferred-release / ring / flush-drain functions are
   byte-identical between 37da3c3a and HEAD

python3 scripts/check_include_closure.py --mode both --compiler clang++
        --self-test --require-all                                               rc=0
   4 probes, 0 skipped, 0 problem(s); self-test 6 negative-control trips

python3 scripts/gen_pipe.py --check                                             rc=0
   477 inventory rows, 0 UNMAPPED; generated files are up to date
python3 scripts/gen_pipe.py --self-test    7 negative-control trips             rc=0
python3 scripts/gen_pipe_dirty_surface.py --check                               rc=0
   0 COARSE, 2 UNDECIDED (c0's BindVertexArray / UseProgram rows, unchanged)
python3 scripts/gen_pipe_dirty_surface.py --self-test  27 negative controls     rc=0
git diff --exit-code -- MobileGL/MG_Pipe/generated                              rc=0
```

**ctest name sets** (`LC_ALL=C`, `ctest -N`):

```
build-linux vs build-push            2646 == 2646, 0 differing lines            (G2)
vs ~/w7/p4a-before-ctest-names.txt   0 removed, 58 added, 2588 -> 2646
```

58 = c0's 13 + c0b's 1 + **c0c's 2** + wire v1's 33 + v2's 4 + **v3's 5**. Unit count 1673 (v2, on c0+c0b) →
**1680** = c0c's 2 + this round's 5.

**Not run this round, and why**: the integration / verify-scenario / retrace / APK lanes and the device arms.
This package is still behaviour-neutral — all four `kMGPipeWired*Subsystem` are 0 on this tree, nothing consumes
a P4a record, `PipeApply.{h,cpp}` are push-only translation units and G1 is 0/0/0/0. No retrace ran, so the LFS
restore in §0 was hygiene for the next round rather than a prerequisite for this one.

---

## 7. What the integrator must re-run

1. **The full five-part gate of D.3** on the merged head — nothing here substitutes for it — plus
   `symbol_report --threshold 0` (P4a's admitted set is EMPTY) and `p3a_untouched_regions.sh 37da3c3a HEAD`.
2. **When c0e lands**: the `static_assert` at `PipeApply.h:177` **fires** (its `Count` becomes 4). That is the
   trip wire, not a regression. Retiring it is wire's verification round: delete
   `kMGPipeFramebufferTargetNamed` and its assert (`PipeApply.h:173-176`) and spell
   `static_cast<Uint8>(MGPipeFramebufferTarget::Named)` at its six readers — `PipeApply.cpp:2153` (the target
   gate), `FramebufferEmitTest.cpp:276`, `:443`, `:454`, and the two prose mentions at `PipeApply.h:595`/`:965`.
   One constant, six sites, no behaviour change.
3. **After B lands**, three reads worth ten minutes each: (a) does B emit a Named record at **all five** DSA
   entry points plus the DSA setters, and is its suppressor keyed per framebuffer (§5 B.1/B.2)? (b) does B gate
   its dirty-flag clear on the three acceptance returns? (c) does the mask republish move **only** BindMask /
   ImageBindableHint (W11)? None of the three is visible to any gate P4a has.
4. **After D lands**: `PushedFramebufferRecord` must lose its `asTarget` parameter and its `record.Fbo == fbo`
   test, or C-1 is only half fixed — a Named record would still be refused for not being the bound one. And D's
   teardown path must not read a binding after `MGPipeApplierReleaseObjectRecords`.
5. **After E's verification round**: `StaleFramebufferRecordLookups` should be **0** on the integrated tree's
   scenarios. A non-zero value is the seam defect ID-19 was written to make visible, not traffic.
6. **D-K2's fourth row** (ID-15's H1) is still owed by D and F; the applier's `Fatal` on a null `BuiltinSampler`
   is unchanged and correct, and the `0x5ff` lane still aborts until that row exists.
7. **The brief's D-C2** still describes the record as per bound target and **D-J4** still lists the framebuffer
   records as working state; both are amended by ID-19(b) and by this round. D.5 should carry the correction
   alongside c0e's.
8. **LFS**: this tree's fixtures were restored by copy from `~/w7/base` because `git lfs checkout` reports
   *"Git LFS is not installed"* here — `/usr/bin/git-lfs` exists but the repo has no `filter.lfs.smudge`. A
   `git lfs install --local` in each worktree would stop ID-13's trap recurring after every WSL restart.
