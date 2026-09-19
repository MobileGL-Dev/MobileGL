# P4a package A — `wire`, rework round. Result, v2

Rework of `wire-review-v1` (verdict REWORK: **C1** plus minors **m1**–**m5**), accepted by the integrator as
**ID-15**. Worktree `/home/swung/w7/p4a-wire`, branch `refs/heads/p4a/wire`, **not pushed**.

## 0. Rebase

`p4a/wire` was rebased onto `refs/heads/feat/disaggregated` = **`2cb44039`** (c0 `08192d72` + c0b
`32033d69`/`2cb44039`). **No `c0c` had landed** on `feat/disaggregated` or on `refs/heads/p4a/contract` when this
round started (both were at `2cb44039`), so the base is c0 + c0b and `MGPipeSubDataUploadTargetOf` /
`MGPipeSubDataResourceTargetOf` / `MGPipePackSubDataTarget` are **not available** — see m3 for what this package
did instead and what c0c retires.

The rebase was **clean, four commits, no conflicts** (c0b's `PipeFill.{h,cpp}`, `PipeMutation.h`,
`SlotAllocator.*` do not overlap `PipeApply.{h,cpp}`, which is wire's after the tag). The four v1 commits are now:

| v1 | after rebase | message (unchanged) |
|---|---|---|
| `86835b50` | `686ad38c` | `[Feat] (Pipe): apply the framebuffer record per bound target and the texture and renderbuffer resource calls into their own slot-indexed records` |
| `bc9abfbd` | `e5038c3d` | `[Feat] (Pipe): apply sampler states, sampler views and the three unit sets into the server's own working state` |
| `0f990633` | `59bceb4c` | `[Feat] (Pipe): apply the shader CSO's artefacts and the default uniform block without ever re-linking on the server` |
| `3c07c8c3` | `529cb262` | `[Test] (Pipe): pin every P4a record's lifecycle, its bounds gate and what a make-current does and does not clear` |

## 1. The five new commits

**HEAD = `6f263284`.** Only `MG_Pipe/PipeApply.{h,cpp}` and three `MG_Test/Pipe/*EmitTest.cpp` are touched;
no other file in the tree moved, and nothing outside package A's own C.7 set was opened.

| # | hash | items | message |
|---|---|---|---|
| 1 | `3c5b2d01` | **C1**, **m4**, **m3** (+ n1) | `[Fix] (Pipe): scope a respecify's pending-upload clear to the level it redefines, answer the emitter whether a sub-data record was accepted, and refuse a resource target that names no texture` |
| 2 | `026d2c48` | **m1** | `[Fix] (Pipe): release the framebuffer records and the three unit windows with the object records whose handles they hold` |
| 3 | `9f75fbbb` | **m5** | `[Fix] (Pipe): give unmap_persistent's kind check the verdict resource_destroy's has, and say why the other three buffer-only calls have nothing to check` |
| 4 | `aaad9691` | **m2**, **n3** | `[Docs] (Pipe): correct the refusal counter's family list - set_framebuffer_state resolves no record and can only fault - and warn that a texture's built-in sampler handle may outlive its CSO` |
| 5 | `6f263284` | the four new cases + the narrowing | `[Test] (Pipe): pin a respecify's per-level scope, the sub-data acceptance signal, the upload targets that name no texture and what a release of the object records clears` |

The three concerns folded into commit 1 all live on the same call path (the respecify's clear, the sub-data
dispatcher and the sub-data return type share hunks in both files); n1's sentence is adjacent to C1's in
`PipeApply.h` and rides with it. Everything else is one item per commit.

---

## 2. Review item by review item

### C1 — the respecify's pending-upload clear is now scoped to the level it redefines

**Fixed exactly as the review specifies**, with the trailing defaulted pointer.

- `PipeApply.h:641-653` — new `struct MGPRespecifiedLevel { Uint16 UploadTarget, Level; }`. It is an
  **applier-side argument, not a wire record**: not in `PipeFields.def`, no payload, no `MGP_ASSERT_POD`. Its
  `UploadTarget` is `MGPSubData::Target` **verbatim** — the whole packed field — because that is what
  `MGPipeResourceRecord::PendingUpload::UploadTarget` holds (`AccumulatePendingUpload` stores `record.Target`),
  so the two keys are the same value or they match nothing. This is stated at the declaration.
- `PipeApply.h:655-680` — `MGPipeApplyResourceRespecify(const MGPResourceDesc&, const void*, const
  MGPRespecifiedLevel* level = nullptr)`. Trailing and defaulted, M2/W1's shape: `gen_pipe.py` never parses this
  header, the generated wire path calls no `MGPipeApply*`, `PipeCalls.def` is untouched, and P3a's one call site
  (`MG_Impl/Pipe/PipeFill.cpp:691`) plus every existing case compile unchanged. **`git diff -- MobileGL/MG_Pipe/generated`
  is empty and `gen_pipe.py --check` is rc 0**, so the wire path did not move.
- `PipeApply.cpp:1510-1511` (signature), `1531-1562` (the argued clear). `nullptr` → clear all, exactly as
  before, for `glBufferData` / `glBufferStorage` / `glTexStorage*` / a texture view. Non-null → erase the single
  matching `(UploadTarget, Level)` and keep the rest; the keys are unique by `AccumulatePendingUpload`'s
  construction, so it erases at most one entry and stops.
- `PipeApply.h:241-247` — the pending set's own comment now says the keys are independent and why.

**Deviation W7 is withdrawn and replaced** — see §3.

**The case.** `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers`
(`MG_Test/Pipe/TextureEmitTest.cpp:473`) drives the canonical sequence the review names: define level 0, accept
its upload, add a second **cube face** of level 0 (keyed the packed way, `(1 << 8) | Tex2D`, so that the level
number alone cannot be what matched), then respecify level 1 — and asserts **both** level-0 entries survive with
their boxes intact. It then proves the other half: the key the respecify **does** name is dropped.
`TextureEmit.ARespecifyDropsThePendingUploadsAgainstTheStorageItReplaces` (`:445`) is **narrowed to the
whole-resource arm it actually proves** — it now accumulates two levels and passes a null scope, and its comment
says so and points at its twin.

**The mutation** (§4, MU1): restoring the old code — the unconditional `PendingUploads.clear()` — makes exactly
that case fail, and nothing else.

### m4 — the acceptance signal (implemented, not declared)

`MGPipeApplyResourceSubData` now returns `Bool`.

- `PipeApply.h:692-709` — the contract: **true** when the record was stored (the buffer half landed its range,
  or the texture half accumulated the shape), **false** when it was refused, and the paragraph says in as many
  words that B's dirty-flag clear is gated on it (D-D5 step 1) and why neither refusal path is otherwise visible
  (a dead handle is a counted no-op; a corrupt record is a Fatal that deliberately moves **no** counter).
- `PipeApply.cpp:811` `ApplyBufferWrite` and `:863` `ApplyTextureUpload` both return `Bool`; `:1579`
  `MGPipeApplyResourceSubData` forwards it.
- **An unregistered or partial op table still returns true** (`PipeApply.cpp:839-843`, argued there): whether a
  backend registered `MGPipeResourceOps`, and whether it implements the optional resident hook, is a property of
  the **build**, not of the record. An emitter that read "not accepted" off an unregistered table would re-send
  a write the applier has already taken responsibility for.
- `MGPipeApplyBufferSubDataResident` keeps its `void` return — it is not on D-D5's dirty-flag path and no
  package asked for it.

Case: `TextureEmit.TheSubDataCallAnswersWhetherTheRecordWasAcceptedSoTheClientCanClearItsFlag`
(`TextureEmitTest.cpp:531`) — an accepted texture upload answers true, a **stale generation** (the refusal that
is *not* a Fatal, so the only one the return can carry) answers false and moves `RefusedResourceCalls`, and the
buffer half answers on the same terms with no backend table registered. Mutation MU2.

### m3 — the texture half no longer accepts a resource target that names no texture

`PipeApply.cpp:1591-1608`. After the buffer branch, the record's **resource-target half** must name a texture:
`Buffer`, `Renderbuffer` and anything at or above `MGPipeResourceTarget::Count` are
`Fatal{ProtocolCorruption}` — *"the record's resource target names no texture to upload into
(target=%u, resource target=%u)"* — rather than an accumulation onto whatever texture holds that slot. It is the
mirror of `ResourceTableForTarget`'s null-on-unknown and its argument, and it is a Fatal and not a counted
refusal for the reason `resource_destroy`'s dispatch Fatal is.

**The value-space pin, and where it went.** ID-12 rules `MGPSubData::Target` **packed** (low byte
`MGPipeResourceTarget`, high byte `TextureUploadTarget`) and says wire matches the field verbatim, which the
buffer test still does (`SubDataNamesABuffer`, `PipeApply.cpp:511-513`, whole field `== 0`). The new gate needs
the low byte, so `PipeApply.cpp:515-530` carries `SubDataResourceTargetOf` — **the local decode, with the
encoding written out beside it**, because c0c is not on this base. It is one function to retire the day c0c
lands; §5 says so. **The sentence beside `MGPSubData` in `MGPipeTypes.h` is DECLARED, not written**: that file
is the contract's and c0c is in flight on exactly it (it mints the three pack/unpack helpers, which *are* the
value-space pin). Writing a comment there would collide with c0c for no gain. The pin therefore lives at
`PipeApply.h:700-707` and `PipeApply.cpp:515-530`, both of which name ID-12.

A side effect worth naming: the gate also closes the hole DV-3 opened at the *other* end — a record with the
low byte `Buffer` and a non-zero upload-target half used to fall through the whole-field buffer test into
`TextureResources`. It is now refused.

Case: `TextureEmit.ASubDataRecordWhoseResourceTargetNamesNoTextureIsRefusedRatherThanRouted`
(`TextureEmitTest.cpp:580`) — all three arms (Renderbuffer, `Count`, packed-Buffer), each naming the refusal,
each proving nothing was accumulated, no serial moved and `RefusedObjectCalls`/`RefusedResourceCalls` did not
move. Mutation MU3.

### m1 — `MGPipeApplierReleaseObjectRecords` now releases everything that names an object record

`PipeApply.cpp:1160-1180`. It clears `DrawFramebuffer`, `ReadFramebuffer` and all three unit windows
(`BoundSamplerViews`/`Start`/`Count`, `BoundSamplerStates`/…, `BoundShaderImages`/…) alongside the three
program handles it already cleared. The argument is written there: a window left standing after the tables are
emptied is a set of handles into empty tables, which the next resolve either refuses and counts (a teardown-time
refusal storm nothing asked for) or — on a slot the next context re-mints — resolves onto somebody else's
record. **Contract review m2 is discharged by fixing, not by declaring.**

Case: `FramebufferEmit.AReleaseOfTheObjectRecordsAlsoClearsTheWorkingHandlesThatCouldNameThem`
(`FramebufferEmitTest.cpp:348`) — a framebuffer record whose `Color[0].Res` names a texture, plus one entry in
each of the three sets at a non-zero `Start`, then the release, then eleven assertions. Mutation MU4.

### m2 — the header no longer promises a framebuffer refusal

`PipeApply.h:445-465`. "framebuffer" is out of `RefusedObjectCalls`' family list (now four families, "one
counter rather than four"), and a paragraph states positively that `set_framebuffer_state` **resolves no
record** (D-I2: a handle and no wire lifetime), that `MGPSurface::Res` is deliberately left unresolved (D-I3),
that its only verdict is `Fatal{ProtocolCorruption}` on a malformed record, and that **the counter must stay at
0 across every framebuffer call in every build**. That last sentence is the assertable form of W4, in the file
`MG_Test`, D and E read.

### m5 — `unmap_persistent`'s kind check is a Fatal

`PipeApply.cpp:1762-1780`. `MOBILEGL_ASSERT` is replaced by `Fatal{ProtocolCorruption}` — *"the persistent
donation is the buffer family's and the handle names another kind (%u)"* — with the reason written out: the
assert compiles out at INFO, which is what all three gate builds and every shipped build are, so a mistyped
record used to walk into `ResolveResource` and alias whatever **buffer** holds that slot. No counter moves; it
is the corrupt-record verdict, exactly as `resource_destroy`'s.

`PipeApply.cpp:1618-1626` carries the note the review asks for beside the other three: `resource_flush_range`,
`resource_readback` and `map_persistent` are buffer-only **by catalogue and not by check** — `MGPFlushRange` and
`MGPReadback` carry no kind at all and `map_persistent`'s `MGPHandleOnly` is not asked for one — so there is
nothing there to compare, and if a later phase gives one of them a kind the gate belongs beside that field.

Case: `ResourceEmit.AResourceTargetOrKindTheCatalogueDoesNotNameIsRefusedRatherThanRouted`
(`ResourceEmitTest.cpp:1423-1434`) gains the arm, with a **live buffer at the same slot** so the aliasing it
prevents is actually reachable. Mutation MU5.

### n1, n3 — the two sentences

- **n1**: `PipeApply.h:248-255`, beside `PendingUpload` — the accumulated rect list **may overlap across
  emissions** (the lists concatenate and the gate only checks containment in the union box), so D's N-rect
  staging uploads those texels twice; that is a cost, never a correctness problem, and nothing downstream may
  read "the frontend's model" as "disjoint" once the shapes have been accumulated.
- **n3**: `PipeApply.h:216-223`, beside `Params` — a texture record's `BuiltinSampler` **may name a CSO whose
  record is gone** (`set_texture_params` deliberately does not resolve it; `delete_sampler_state` does not sweep
  the textures that name it), so a consumer must treat it as it treats any stale handle.

### n2, n4 — declared, not taken

- **n2** (`RegionCount == 1` with an all-zero region passes the "no texels at all" test and creates an entry
  describing nothing): **not taken**, deliberately. The record is legal by every rule the applier owns — the
  region is inside the box and the box is encodable — and the only way to refuse it is a rule about region
  *content*, which is the storage owner's arithmetic (the "deliberately NOT checked" list's item (b)). It costs
  one empty staging job on a shape no emitter produces. Left as the review left it: a nit.
- **n4** (contract review m1, the tautological `static_assert` at `MGPipeTypes.h:185-188`): **not claimed.**
  `MGPipeTypes.h` is the contract's file and **c0c is in flight on it**; a one-line deletion from this branch
  would conflict with c0c for no behavioural gain. It stays unclaimed and is the integrator's to fold into c0c
  or to drop.

### H1 — not this package's, and the applier is unchanged

ID-15 rules it: **D-K2 gains a fourth row, "bit 10 requires bit 11"**, refused at the contract dependency rule,
with package D's mirror in the texture family's `Resolve*SubsystemArm` and package F's `0x5ff`-shaped
dependency-refusal scenario. Client-side minting is rejected. **This package changed nothing**: the null
`BuiltinSampler` is still `Fatal{ProtocolCorruption}` and
`TextureEmit.ARecordWithNoBuiltinSamplerCsoIsRefusedNamingTheTexture` still pins it.

---

## 3. Deviations after the rework

W1, W2, W3, W4, W5, W6 and W8 stand exactly as v1 recorded them and as the review accepted them. **W7 is
withdrawn** — it was C1 — and is replaced by:

**W7' — a respecify's pending-upload clear is scoped by a trailing defaulted `const MGPRespecifiedLevel*`, and
the applier's second signature change.** Null means whole-resource and clears everything (`glBufferData`,
`glBufferStorage`, `glTexStorage*`, a texture view); non-null names one `(uploadTarget, level)` and clears only
that. The frontend fact is the opposite of v1's claim: `AllocateStorage` and `MarkStorageDirty`
(`MG_State/GLState/TextureState/TextureObject.h:245,252`) are **per `(uploadTarget, level)`**, so a
`glTexImage2D(level = 1)` re-marks level 1 and nothing else, and every other level's dirty flag was cleared at
its own emission — a blanket clear loses those texels with no counter, no log line and no gate that can see it.

**W9 — `MGPipeApplyResourceSubData` returns `Bool` instead of `void`.** m4, implemented rather than declared.
Source-compatible (P3a's call site at `PipeFill.cpp:705` discards it), and the answer is D-D5 step 1's
"accepted".

---

## 4. Mutation evidence for this round

Five mutations, five red, measured in `build-push` at HEAD, each applied to
`MobileGL/MG_Pipe/PipeApply.cpp`, the tree rebuilt, the family's suite run, the file restored from the commit.
**The verdict is `ctest`'s exit code**, not a grep for `(Failed)` — v1's §7.1 lesson.

| # | mutation | verdict |
|---|---|---|
| MU1 | **C1 restored to the old code**: `if (level == nullptr)` → `if (true)`, i.e. the unconditional `PendingUploads.clear()` | `rc=8`, 1/10 failed — `TextureEmit.ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` and only it |
| MU2 | **m4**: `ApplyTextureUpload`'s refused-resolve returns `true` | `rc=8`, 1/10 — `TextureEmit.TheSubDataCallAnswersWhetherTheRecordWasAcceptedSoTheClientCanClearItsFlag` |
| MU3 | **m3**: the resource-target gate disarmed | `rc=8`, 1/10 — `TextureEmit.ASubDataRecordWhoseResourceTargetNamesNoTextureIsRefusedRatherThanRouted` |
| MU4 | **m1**: the eleven working-handle clears deleted from `MGPipeApplierReleaseObjectRecords` | `rc=8`, 1/6 — `FramebufferEmit.AReleaseOfTheObjectRecordsAlsoClearsTheWorkingHandlesThatCouldNameThem` |
| MU5 | **m5**: `unmap_persistent`'s kind Fatal disarmed | `rc=8`, 1/29 — `ResourceEmit.AResourceTargetOrKindTheCatalogueDoesNotNameIsRefusedRatherThanRouted` |

MU1 is the review's requirement stated exactly: the new case is red on the old code and green on the new. Each
mutation kills **one** case, which is also evidence the four new cases are not duplicating an existing one.
After the sweep the tree was restored, rebuilt and re-run: `git status --porcelain` empty, 1673/1673.

---

## 5. What B, C, D, E and F must know (delta on v1's §5)

1. **`MGPipeApplyResourceRespecify` has a third parameter.** `const MGPRespecifiedLevel* level = nullptr`.
   **Package B must pass it at every per-level respecify** — every `glTexImage{1,2,3}D` on a mutable texture, and
   the generated-mip storage grow at `GL_Texture.cpp:528-539` — with `UploadTarget` set to **the same packed
   `MGPSubData::Target` value that level's sub-data emission uses** and `Level` to the level index. It has both
   halves in hand at the `AllocateStorage` call site. Passing `nullptr` (or the wrong pair) does not fail
   loudly — it silently returns to C1's behaviour for that call, so this is the one line of B's texture path a
   reviewer should read twice. `glTexStorage*`, `glBufferData`, `glBufferStorage` and texture views pass
   `nullptr`, which is the default, so B's immutable paths and every P3a buffer path need no edit.
2. **`MGPipeApplyResourceSubData` returns `Bool`.** B's drain must clear the level's
   `m_isDirty`/`m_dirtyRects`/`m_dirtyRegions` **only when it is true** (D-D5 step 1). Discarding it compiles.
3. **A texture sub-data record's resource-target half is now policed.** `Buffer` (with a non-zero upload-target
   half), `Renderbuffer` and anything `>= MGPipeResourceTarget::Count` are Fatal. B's packing must put the
   **resource** target in the low byte, per ID-12.
4. **`MGPipeApplierReleaseObjectRecords` now clears the framebuffer records and the three unit windows.** D and
   E must not read this function as "the object records go and the bindings stay"; after it, nothing is bound.
5. **`unmap_persistent` on a non-buffer kind is Fatal.** D's death paths must not route another kind's retire
   through it.
6. **`RefusedObjectCalls` covers four families, not five.** `set_framebuffer_state` can only ever Fatal; a
   framebuffer call that moved this counter would be a defect. F may assert it stays 0 across the framebuffer
   family.
7. **The accumulated rect list may contain overlaps** (n1). D's staging must tolerate duplicate texels.
8. **A texture record's `BuiltinSampler` may name a dead CSO** (n3).
9. **`SubDataResourceTargetOf` (`PipeApply.cpp:515-530`) is a local decode that c0c retires.** When c0c lands
   `MGPipeSubDataResourceTargetOf` in `MGPipeTypes.h`, this one function forwards to it or goes away; the
   applier's behaviour does not change either way.

---

## 6. Gate transcript (HEAD `6f263284`, working tree clean)

Run in `/home/swung/w7/p4a-wire` with `CCACHE_BASEDIR=/home/swung/w7`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, `--parallel 8` / `-j 8`.

```
build-linux / build-push / build-verify                      rc=0 / rc=0 / rc=0

symbol_report.py --before ~/w7/p4a-before-libMobileGL.so
   --after build-linux/libMobileGL.so --threshold 0
   --fail-on-symbol-set-change --fail-on-added-bytes 0
   ### Removed (0)   ### Added (0)   ### Resized (0)   ### Renamed only (0)     rc=0

ctest --test-dir build-linux  -L unit   100% passed, 0 failed out of 1673       rc=0
ctest --test-dir build-push   -L unit   100% passed, 0 failed out of 1673       rc=0
ctest --test-dir build-verify -L unit   100% passed, 0 failed out of 1673       rc=0

ctest --test-dir build-push -R 'ResourceEmit\.|FramebufferEmit\.|TextureEmit\.|
     SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.|HandleRecycle'
                                        100% passed, 0 failed out of 125        rc=0

bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
   the 11 pool / deferred-release / ring / flush-drain functions are
   byte-identical between 37da3c3a and HEAD                                     rc=0

python3 scripts/check_include_closure.py --mode both --compiler clang++
        --self-test --require-all
   4 probes, 0 skipped, 0 problem(s); self-test 6 negative-control trips        rc=0

python3 scripts/gen_pipe.py --check                                             rc=0
python3 scripts/gen_pipe.py --self-test   7 negative-control trips              rc=0
python3 scripts/gen_pipe_dirty_surface.py --check                               rc=0
python3 scripts/gen_pipe_dirty_surface.py --self-test  27 negative controls     rc=0
git diff --exit-code -- MobileGL/MG_Pipe/generated                              rc=0
```

**ctest name sets** (`LC_ALL=C` sort, `ctest -N`):

```
build-linux vs build-push                          identical, 0 differing lines   (G2)
vs ~/w7/p4a-before-ctest-names.txt                 0 removed, 51 added
```

51 = the contract's 13 + wire v1's 33 + c0b's 1 + **this round's 4**. Every new case is a visible SKIP in a pull
build, so the pull and push name sets stay identical name for name. The four:

| suite | case |
|---|---|
| `TextureEmit` | `ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` |
| `TextureEmit` | `TheSubDataCallAnswersWhetherTheRecordWasAcceptedSoTheClientCanClearItsFlag` |
| `TextureEmit` | `ASubDataRecordWhoseResourceTargetNamesNoTextureIsRefusedRatherThanRouted` |
| `FramebufferEmit` | `AReleaseOfTheObjectRecordsAlsoClearsTheWorkingHandlesThatCouldNameThem` |

Unit count moved 1668 (v1's base, c0 only) → **1673**: c0b's one test plus these four.

**Not run this round, and why**: the integration/verify/retrace/APK lanes and the device arms. This package is
behaviour-neutral (all four `kMGPipeWired*Subsystem` are still 0, nothing consumes a P4a record on this tree),
G1 is 0/0/0/0, and the review confirmed structurally that `PipeApply.{h,cpp}` are push-only translation units so
the pull build cannot move. The `git lfs checkout` of ID-13 was therefore not needed either — no retrace ran.

---

## 7. What the integrator must re-run on the integrated tree

1. **The full five-part gate of D.3**, unchanged — nothing here is a substitute for it.
2. **`symbol_report --threshold 0` after the merge** (P4a's admitted set is EMPTY, so 0/0/0/0 again), and
   `p3a_untouched_regions.sh 37da3c3a HEAD` on the merged head.
3. **When c0c lands**: re-run `check_include_closure.py` and the unit lanes, and decide whether
   `SubDataResourceTargetOf` (`PipeApply.cpp:515-530`) forwards to `MGPipeSubDataResourceTargetOf` or is
   deleted. Behaviour does not change either way; this is hygiene, and it is one function.
4. **After package B lands**: the seam that C1 exposed is now B's to hold up — a per-level respecify that
   forgets its `MGPRespecifiedLevel*` is silently the old bug. Worth a targeted read of B's `AllocateStorage`
   emission path, and worth checking that B gates its dirty-flag clear on the new `Bool`. Neither is visible to
   any gate P4a has; both are visible to a reviewer in ten lines.
5. **After D lands**: `MGPipeApplierReleaseObjectRecords` now clears the bindings as well as the records —
   check D's teardown path does not read a binding after it.
6. **D-K2's fourth row** (ID-15's H1 ruling) is still owed by the contract and by D/F. The applier's Fatal on a
   null `BuiltinSampler` is unchanged and correct; the `0x5ff` lane still aborts until that row exists.
7. **The brief's D-J3** still lists five refusal families including framebuffer; the header no longer does.
   ID-15/D.5 amends the brief.
