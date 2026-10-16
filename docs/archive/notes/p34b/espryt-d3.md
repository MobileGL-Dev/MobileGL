# espryt D3 — the Espryt (DirectGLES) stream, part 3

The defect package D2 found and could not fix, plus the two D-K2 loose ends D2 left for
whoever owned `MG_Backend/DirectGLES/` next. Base `p7/espryt-d2 @ a7c06f4f`, worktree
`~/w7/p7-espryt-d3`, branch `p7/espryt-d3`.

Per-slice red-once notes (R-16 / rule J). A slice is not done until the case it turns on has
been seen RED against the tree without that slice's fix and GREEN with it, on the arms the fix
is for — and for this package that means the two-process arms, not only inproc.

Four commits, in cherry-pick order:

| # | files | what is true after it |
|---|---|---|
| 1 | `MG_Test` | the pull build compiles again — `SubsystemDepsTest`'s server half is `MOBILEGL_PIPE_PUSH`-only |
| 2 | `MG_Backend`, `MG_IntegrationTest`, `MG_Remote`, `docs` | an owner-side upload shows through a sampled texture view, because the split clean gate resolves to the storage record |
| 3 | `MG_Backend`, `MG_Pipe`, `MG_Impl` | D-K2's dependency rule has one statement, and every reader reads it |
| 4 | `docs` | this note, and the census row the fix falsified |

Commit 1 is not a deliverable and is first for a reason: without it `ninja -C build-linux` exits
1 at D2's head, so every later commit would be measured on a tree whose pull build does not
build.

---

## slice 0 — the pull build compiles again

Not a deliverable; a prerequisite for this package's own G1, and worth stating first because it
means D2's head does not pass gate C as it stands.

D2's new `MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp` guards its CLIENT case with
`#if MOBILEGL_PIPE_PUSH` + `GTEST_SKIP` and does not guard its two SERVER cases, which take the
addresses of `ResolveFramebufferSubsystemArm` / `ResolveTextureResourceSubsystemArm` /
`ResolveSamplerSubsystemArm` / `ResolveProgramSubsystemArm`. `Managers.h` declares all four
inside `#if MOBILEGL_PIPE_PUSH` and `Managers.cpp` defines them inside it, so with the pull
flags the file does not compile:

```
../MobileGL/MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp:200:11:
  error: use of undeclared identifier 'ResolveFramebufferSubsystemArm'
... 9 errors generated.
ninja: build stopped: subcommand failed.
```

`libMobileGL.so` has already linked by then, so G1's `.text` and symbol numbers are still
readable — but `~/w7/logs/p7-gate-c-inner.sh` reports `=== linux build rc= ===` and it was 1.
The two cases get the same guard and the same skip the client case already carries. Nothing
else moves; `ninja -C build-linux` rc 0.

---

## slice 1 — an owner-side upload is visible through a sampled view

### The hole

`TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary`, registered by D2 and
excluded by name from the three split arms with the cause in the CMake comment: once a
`glTextureView` has been SAMPLED, a later `glTexSubImage` through the OWNER's own name never
becomes visible through the view. Red on inproc, spawn and tcp; green under monolith.

The mechanism, end to end:

* A view owns no texels, and the client keeps **one emission cursor per storage**. An upload
  through a view's own name is remapped onto its storage owner before it is emitted
  (`MG_Impl/Pipe/TextureEmit.h`, `MGPipeTextureEmitter`; `TextureEmitTest.cpp`'s
  `AnUploadThroughAViewKeysOnTheStorageOwner` pins it). So every `resource_subdata` for either
  name is keyed on the OWNER.
* `MG_Pipe/PipeApply.cpp`'s `ApplyTextureUpload` accumulates into that record and does
  `++stored->Serial` on it — the owner's. Nothing moves the view's.
* `BackendTextureObject::IsDrawSyncCleanByRecord` (`MG_Backend/DirectGLES/Managers.cpp`) read
  `record.Serial` and `record.PendingUploads` **of the record it was handed**, which for a bound
  view is the view's. A view's `Serial` is moved by nothing an upload does, so after the view's
  first sample the gate answered CLEAN forever and `SyncTextureViewToBackendByRecord` — the only
  thing that drains the owner's pending uploads on a view's behalf, because it syncs the storage
  twin by handle at its head — never ran again.
* Under monolith the same gate reads `GetContentVersion()` THROUGH the view, which
  `TextureObjectView` forwards to its owner (`TextureObjectView.cpp:100-102`). The split gate
  had no equivalent of that forwarding.
* `CONTRACT-P5E.md` §5.2 stated the split gate in full, `rec->Serial` and all, so the code was
  faithful to a wrong contract. **The contract is amended, not restated.**

The client half was never at fault, and the red run proves it rather than arguing it: the
case's own `MGPipeTextureEmitter::SubDataCount()` / `RefusedSubDataCount()` / `DrainListSize()`
assertions PASS on the red run. Only the pixel channel is red.

### The decision: resolve the predicate, not index the reverse edge

**Chosen (a): the gate's two STORAGE clauses resolve through `Desc.ViewOf` to the storage
record.** `PipeTextureStorageRecordForRecord` (new, `Managers.cpp`, beside
`PipeTextureRecordForHandle`) answers with the record itself for a texture that owns its texels
and with the record `Desc.ViewOf` names for a view. `IsDrawSyncCleanByRecord` asks it for
`Serial` and `PendingUploads`; the three PARAMETER clauses and `Desc.StorageKind` stay the
view's own record's, because a view has its own texture parameters and its own built-in sampler
and that is the whole reason the second name exists. `SyncTextureViewToBackendByRecord` stamps
that same storage serial at both its arms — the steady one and the one after the native view
call — so the stamp and the gate name one quantity rather than two.

**Rejected (b): a reverse index owner → views, so an owner-side `resource_subdata` bumps every
viewing record's `Serial`.** Three reasons, in increasing order of how much they would have
cost to discover later:

1. It puts new mutable state in `MG_Pipe`'s applier, which BOTH backends read, to answer a
   question one backend's gate asks. Magma does not need it (below).
2. It makes `ApplyTextureUpload` write a record other than the one the call names. The
   applier's serial discipline is stated in that function's own comment — the twin stamps its
   synced serial from inside the sync that reads THIS record — and a bump arriving from a
   different record's apply is exactly the shape that comment exists to forbid.
3. **Deletion ordering, which is what actually decided it.** The index has to be pruned when a
   VIEW dies, or an owner upload bumps a slot that has since been recycled to an unrelated
   texture — a silent cross-texture serial skew with no assertion anywhere that would catch it.
   The resolve direction needs no pruning at all, and it is checked at every use:
   `PipeTextureRecordForHandle` tests `Live` and `Gen`, so a recycled slot answers null rather
   than a stranger's record.

**Deletion ordering for the chosen shape: the walk's `Live`/`Gen` test is the guarantee, not
the emit order** (the review corrected an earlier draft of this paragraph that claimed the
ordering alone was enough). A view may outlive its owner's GL NAME — `glDeleteTextures(owner)`
while the view is alive is legal and the storage must survive — and it cannot outlive the owner's
STORAGE: `TextureObjectView::m_storageOwner` is a strong `SharedPtr`, and
`MGPipeEmitTextureDestroyAndFree` is called from `TextureObjectBase`'s DESTRUCTOR, whose own
comment says why — *"the last SharedPtr to this object dropping, not the glDelete* that only
marks the name and leaves a still-bound object very much alive"* (`TextureObject.cpp:63`). But
`m_storageOwner` is a member of the derived class and there is no user destructor on the view,
so when the view holds the owner's last reference C++ destroys the member first: the wire
carries `resource_destroy(owner)` and then `resource_destroy(view)`, and for exactly one apply
the view record is live with `Desc.ViewOf` naming a freed slot. No draw can land between two
destroys from one destructor chain, and the walk tests `Live`/`Gen`, so that window answers
null — and **a null answer is NOT clean**, the direction that re-syncs, never the direction that
shows stale texels. It is not a refusal and raises nothing (rule I: no new abort site, no new
`Fatal` family word).

**Views of views resolve transitively.** One hop always reaches storage in practice —
`glTextureView` composes a view-of-a-view onto the ROOT at creation, which is what the spec's
additive min-level rule means; the client's emitter says so in as many words
(`TextureEmit.h EmitResourceRespecify`) and `TextureObjectView`'s own invariant is that its
owner "is never a view itself". The walk is written transitively anyway and BOUNDED, because
this side may not depend on a client invariant to terminate: a chain the wire could forge into a
cycle ends as a null answer rather than as a hang.

**The tree already agreed with (a), on the other backend.** `VkTextureManager::
ResolveWireTextureStorage` (`MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp:2603`) is the
same walk — follow `Desc.ViewOf`, bound the hops, check `Live`/`Gen`, return null on failure —
and its comment states the same two reasons: *"Production views point directly to their root.
The bound also makes a corrupt or future multi-hop record graph terminate without ever following
client data."* Magma therefore never had this defect: it resolves the storage at every use
instead of memoising a per-view serial. Picking (b) would have given the two backends two
different answers to one question.

### Red-once (rule J)

At D2's head `a7c06f4f`, with the CMake exclusion lifted and **nothing else changed**:

```
$ ctest -R 'DirectGLES\.(Split|Spawn|Tcp)\.TextureViewAliasScenario' -j 3
79% tests passed, 3 tests failed out of 14

The following tests FAILED:
   4042 - DirectGLES.Split.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary (Failed) integration-gpu integration-split
   4046 - DirectGLES.Spawn.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary (Failed) integration-gpu integration-spawn
   4050 - DirectGLES.Tcp.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary (Failed) integration-gpu integration-tcp
```

and the failure itself, verbatim from the Split entry:

```
../MobileGL/MG_IntegrationTest/Scenarios/TextureViewAliasScenario.cpp:328: Failure
the first owner write, through the view: 11408 of 11408 pixels disagree;
  first at (2, 2) is rgba(50,160,30,255), expected rgba(0,0,255,255) +/- 2
../MobileGL/MG_IntegrationTest/Scenarios/TextureViewAliasScenario.cpp:328: Failure
the second owner write, through the view: 11408 of 11408 pixels disagree;
  first at (2, 2) is rgba(50,160,30,255), expected rgba(255,255,0,255) +/- 2
```

`rgba(50,160,30,255)` is the LAYER SEED — what the view was minted holding. Both writes are
lost, not one, which is the signature of a gate that latched rather than of a record that got
dropped. The drain's counter assertions are silent in the failure list: they passed.

The MONOLITH control in the same tree is 4/4:

```
$ ctest -R 'DirectGLES\.TextureViewAliasScenario'
100% tests passed, 0 tests failed out of 4
```

With the fix, the same invocation on the three arms:

```
$ ctest -R 'DirectGLES\.(Split|Spawn|Tcp)\.TextureViewAliasScenario' -j 3
100% tests passed, 0 tests failed out of 14
```

Rule J is satisfied on spawn AND tcp, not only inproc: entries 4046 and 4050 are the
two-process arms and both went red and then green.

---

## slice 2 — D2's D-K2 loose ends, now in-partition

D1 has landed on `feat/disaggregated`, so `MG_Backend/DirectGLES/` is free and the two items
D2 listed as the integrator's are taken here.

### The stale prose in the header of the file that implements the refusal

`MG_Backend/DirectGLES/Managers.h`'s resolver block said **"THREE OF THEM CARRY A DEPENDENCY"**
(there are six rows; bits 8 and 13 were the two it did not know about) and listed
**"10 without 11"** among the mirror pairs that are FINE — when 10-without-11 is precisely
D-K2's fourth row (ID-14/ID-15), refused twenty lines below by
`ResolveTextureResourceSubsystemArm` and withheld by the client. `MGPipe.h` lost the identical
pair of defects with R-5; this copy was left behind because the file belonged to a package in
flight. It does not restate the rows again: the file that states them is the file the code
reads.

### Both readers read the table

More than the two one-line changes D2 estimated, because the rule had **six** executable and
prose statements and the estimate counted two:

| reader | was | now |
|---|---|---|
| `MG_Impl/Pipe/PipeFill.cpp` `P4aFamilyDependencyBits` | its own `kMGPipeP4aFamilyDependencies[]`, four rows | `MGPipeSubsystemRequires()`; the local table is deleted |
| `MG_Impl/Pipe/PipeFill.cpp` `P5eFamilyIsLive` | hand-coded `(pushMask & kMGPipeSubsystemResources) == 0` | `MGPipeSubsystemDependenciesAreSet(bit 13, mask)` |
| `MG_Backend/DirectGLES/Managers.cpp` `PipeSubsystemDependencyMissing` | took a literal dependency BIT per call site | takes the FAMILY and asks the table |
| …its `Resolve<Family>SubsystemArm` call sites | four calls with four hand-written sentences (`ResolveProgramSubsystemArm` never called it) | three calls, one per family that has a row, naming only the family |
| `MG_Backend/DirectGLES/Managers.cpp` `ResolveVertexInputSubsystemArm` | hand-rolled `bitSet && !resourcesBitSet` | the table |
| `MG_Backend/DirectGLES/DirectGLES.cpp` `ResolveBufferBindingSubsystemArm` | hand-rolled `bitSet && !resourcesBitSet` | the table |

`MGPipe.h` gains one accessor, `MGPipeSubsystemDependencyWhy()`, so a reader that refuses
prints the ROW'S OWN sentence instead of a copy of it — the `.def` has carried a `Why` column
since R-5 and nothing read it.

**One behaviour improvement falls out of it rather than being designed in.** The texture family
is the only row with two required bits, and it used to be two guarded calls here, so a mask
missing bit 11 but carrying bit 7 was reported with the bit-7 sentence. One call now computes
`required & ~mask` from the row, so the refusal names whichever bit is actually missing.

**What this does to the anti-drift property, said out loud because it is a real weakening.**
`SubsystemDepsTest` used to compare two COPIES of the rule; it now compares two READERS of one
table, which cannot catch a typo in a row. It is kept, and the `.def`'s prose now says why: the
two sides still fold in reasons of their own — the server's own family bit and D-C3's wire-caps
refusal, the client's consumer and wired conjuncts — and those can still disagree at a mask. The
compile-time half of D2's red-once (drop the fourth row from the `.def` → `MGPipe.h:225`/`:230`
fail) is untouched and is now the load-bearing one.

`SubsystemDepsTest` 5/5 after the switch, including D2's two refusal-lane cases at `0x1fff`
and `0x7ff`.

---

## Gates

Measured on `p7/espryt-d3 @ HEAD`, `build-split` configured by `p7_worktree.sh`, `build-linux`
configured with the same compilers and `DISAGGREGATED=OFF PIPE_PUSH=OFF PIPE_LEGACY_MEMOS=ON`
to match `~/w7/pipe/build-linux`.

### G1 — the pull build is byte-identical

| | value |
|---|---|
| `readelf -S -W build-linux/libMobileGL.so` `.text` | **`0xa52203`** (required `0xa52203`) |
| `nm --defined-only` symbols | 30570 |
| vs `~/w7/p7-before/pull-syms.txt` | **0 added / 0 removed** |
| `nm --defined-only -D` vs `pull-dynsyms.txt` | **0 added / 0 removed** |
| `ninja -C build-linux` | rc 0 (rc 1 at base — slice 0) |

Measured three times: after slice 1, after slice 0, and after slice 2. Identical every time.

**Why nothing this package touches reaches the pull build**, per file:

| file | where the change sits |
|---|---|
| `Managers.cpp` `PipeTextureStorageRecordForRecord`, `PipeSubsystemDependencyMissing`, the four resolvers, `ResolveVertexInputSubsystemArm` | inside `#if MOBILEGL_PIPE_PUSH` |
| `Managers.cpp` `IsDrawSyncCleanByRecord` | inside `#if MOBILEGL_PIPE_PUSH` |
| `Managers.cpp` `SyncTextureViewToBackendByRecord` | inside `#if MOBILEGL_BUILD_DISAGGREGATED` |
| `Managers.h` the declaration and the prose | inside `#if MOBILEGL_PIPE_PUSH` |
| `DirectGLES.cpp` `ResolveBufferBindingSubsystemArm` | inside `#if MOBILEGL_BUILD_DISAGGREGATED` |
| `MGPipe.h` `MGPipeSubsystemDependencyWhy` | `inline constexpr`, unreferenced in the pull build |
| `PipeFill.cpp` | `MOBILEGL_PIPE_PUSH`-only translation unit content |
| `SubsystemDepsTest.cpp`, `MG_IntegrationTest/CMakeLists.txt`, the two `.md` files | not `libMobileGL.so` |

The numbers are the proof; the table is what makes them checkable.

### The three scripts

```
python3 scripts/ci/fatal_census.py                        rc 0
  79 abort sites over 20 files of the split server image, 44 distinct family words,
  3 refusal words, 0 unmarked          (79 is D1's number; this package adds none — rule I)
python3 scripts/link_ratchet.py --build-dir build-split \
  --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone    rc 0
  ratchet: unchanged at 186 symbol(s)
python3 scripts/ci/spawn_lane_parity.py build-split       rc 0
```

### Lanes

All five at 100%, each run on its own so that "The following tests FAILED" belongs to the lane
that printed it:

| lane | base (`a7c06f4f`) | D3 | delta |
|---|---|---|---|
| `unit` | 2418 | **2418 / 2418** | 0 |
| `integration-split` | 263 | **264 / 264** | +1 |
| `integration-spawn` | 179 | **180 / 180** | +1 |
| `integration-tcp` | 182 | **183 / 183** | +1 |
| `integration-magma-split` | 75 | **75 / 75** | 0 |

**Names only grow, and the growth is named.** The base column is a real `ctest -N -L` taken on
this worktree with `MG_IntegrationTest/CMakeLists.txt` reverted to `a7c06f4f` and nothing else,
not an arithmetic guess. Set difference, per lane:

```
unit                     added=0 removed=0
integration-split        added=1 removed=0
    + DirectGLES.Split.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary
integration-spawn        added=1 removed=0
    + DirectGLES.Spawn.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary
integration-tcp          added=1 removed=0
    + DirectGLES.Tcp.TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary
integration-magma-split  added=0 removed=0
```

Three names, one per arm, and they are the un-exclusion. Nothing was removed from any lane.

**The DirectVulkan arms of the same scenario: 30 / 30.** `ctest -R
'DirectVulkan\..*TextureViewAliasScenario'` covers the monolith entry plus the six informational
Magma tiers (`integration-magma-{all,full}-{split,spawn,tcp}`), four cases each, and every one
passes on this head — as they did at base. Magma never had this defect, because
`VkTextureManager::ResolveWireTextureStorage` resolves the storage at every use instead of
memoising a per-view serial. They are informational, not gating: `integration-magma-split` (75)
does not carry this scenario.

**One tcp-lane flake, already on record and not this package's.** The FIRST full gate run had
`integration-tcp` at 182/183; the lane is clean on re-run and clean in the final run above.
`split-scenario-census.md`'s own "`SampledSetStalenessScenario` on tcp" section records the same
shape from D2's measurements and attributes it to the tcp lane serialising through one
supervisor (`RESOURCE_LOCK mobilegl-tcp`) that serves one session at a time. Recorded here so the
count is not quietly a best-of-two.

---

## What remains

**1. `KHR-GL46.texture_view.coherency` has no SPLIT reading.** It is exactly slice 1's test
(`tools/cts/caselists/p7-texture-gl46.txt:1050`), and the device-window baseline confirms D2's
claim case by case — `report-texture.json`'s results map has
`"KHR-GL46.texture_view.coherency": "Pass"`. That is a **monolith** reading; 門 5's split
comparison is the device window's, so on host hardware the integration case is the only thing
standing behind this fix. When the window runs, `texture_view.coherency` is the row to read
first. Two corrections to the citation while it is being read:

* the block is `KHR-GL46`, not `KHR-GL43` as `espryt-d2.md` and `split-scenario-census.md` write
  it. The caselist, the baseline and `CONTRACT-P7.md` §7.3 all say GL46;
* `texture_view` is **1/7 fail** in that baseline (`report-texture.txt:24`) and the failing case
  is `KHR-GL46.texture_view.view_sampling`, NOT `coherency`. It is a pre-existing monolith
  Magma conformance failure, on the list `CTS-base/README.md` calls "monolith Magma 的既有一致性
  状态", and it is not this defect — worth naming so that a 6/7 reading after the fix is not
  mistaken for a regression this package caused.

**2. The Magma arms of `TextureViewAliasScenario` are informational, not gating.** Magma has no
defect to fix, but nothing in `integration-magma-split` watches the property on that backend
either. Promoting the four cases by name into the gating tier is a `test.yml` `require_green`
edit, which is not this package's file.

**3. `SubsystemDepsTest` compares two READERS now, not two copies.** Stated in slice 2 and in the
`.def`, repeated here because it is the one thing D3 made weaker: a typo in a `.def` row can no
longer be caught by that case. The compile-time half of D2's red-once (`MGPipe.h:225`/`:230`) is
what carries the anti-drift weight, and the four `static_assert`s over the table are what stand
behind the rows themselves.

**4. The remaining half of D2's R-3, untouched.** `MGPSamplerView::Target` still has no `Count`
bound, no `...IsValid` predicate and no `Fatal{ProtocolCorruption}` for an out-of-range value,
unlike `MGPipeResourceTarget`. It is an `MG_Pipe/MGPipeValueTypes.h` change and nothing in this
package needed it.

**5. D2's own note is not rewritten.** `espryt-d2.md` still says `Managers.h:690-695` "is NOT
corrected here" and that switching the readers "is two one-line changes for the integrator". Both
were true of D2 and are the record of what D2 did; a package note is not a live statement of the
tree. The tree's own live statements — `SubsystemDeps.def`'s "WHO READS IT", `Managers.h`'s
resolver block and `split-scenario-census.md`'s exclusion row — are the ones corrected, and
`split-scenario-census.md` says so in a quoted block above D2's original text rather than by
overwriting it.

**6. The seven unguarded frontend-pointer-keyed containers under `MG_Backend/DirectVulkan`** that
D2's memo-purity sweep listed are still there. They are wave 2 A/B/C's territory and this package
did not look at them.
