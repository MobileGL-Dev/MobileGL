# P4a package C — `clientsp`. Rework result, v3

Branch `refs/heads/p4a/clientsp`, head **`92dffb81`**. **Not pushed.** Working tree clean.
Rebased onto `refs/heads/feat/disaggregated` = **`712c9467`** (c0 + c0b + c0c + c0d + c0e + wire),
then four v3 commits. `git diff --stat 712c9467..HEAD`: **11 files, +3398 / −68**; the v3 work
alone (`820d1a60..HEAD`) is **6 files, +448 / −55**.

The v2 re-review's verdict was REWORK on **one major (C2-M1), three minors and three nits**. All
seven are closed; the rebase turned up **one further defect of its own** (§2.4), fixed in the same
round.

---

## 0. Commits

| # | hash | message (exact, single line, no attribution) |
|---|---|---|
| c1'' | `1534cf37` | `[Feat] (Pipe): content-address sampler states, mint one sampler view per texture and push the three unit sets behind their own content hashes` |
| c2'' | `5a3e9f08` | `[Feat] (Pipe): publish a program's per-stage SPIR-V and reflection archive as a shader CSO and its default uniform block as global constants` |
| c3'' | `bc2aad21` | `[Feat] (Pipe): resolve a program pipeline into one shader CSO out of the reserved composite band and release it exactly once` |
| c4'' | `ccde2906` | `[Test] (Pipe): wire the sampler and program subsystems and pin the padding-proof CSO identity, the program-resolved unit sets, the linked-snapshot stage mask and the composite band's single release` |
| c5' | `b2edca27` | `[Fix] (Pipe): give the sampler family the birth-hook entry points the contract declares, take the publication latch where its creates go out, and reference-count cache entries so an LRU eviction can never take a handle a standing record still names` |
| c6' | `f0f8cab6` | `[Fix] (Pipe): give the program family its birth-hook entry point and publication latch, invalidate the global-constants key whenever a create_shader_state is re-issued, and count a truncated module tail instead of asserting it` |
| c7' | `820d1a60` | `[Fix] (Pipe): stop a make-current disarming the composite resolver's release path - the memo's freshness and the slot's release obligation were one flag, so after the first Reset no signature move ever spoke a delete` |
| **c8** | **`ac09e5b3`** | `[Fix] (Pipe): key the composite resolver's memo on the context as well as the pipeline's GL name - the resolver is a process singleton while GL names are per context, so a make-current released the other context's live composite` |
| **c9** | **`d6e52f75`** | `[Fix] (Pipe): count the sampler cache's unaccountable releases and its referenced evictions rather than asserting them, and state what a shared reference count, an over-capacity cache and the per-unit reconciliation really cost` |
| **c10** | **`79b58198`** | `[Fix, Test] (Pipe): read a forked refusal drive's log from the offset it had instead of unlinking the file - once a case initialises the library the log is already open, so the child was writing into a deleted inode and the parent read nothing` |
| **c11** | **`92dffb81`** | `[Test] (Pipe): drive the sampler cache's two unaccountable-release counters - a handle it never handed out and a second release of a reference only one holder owed` |

c1''..c7'' are v1+v2 replayed. As at v2, **`ccde2906` and `b2edca27` do not build on their own**
(the wired constants and c0b's `if constexpr` seam are set one commit before the entry points
exist); the tree builds again at `f0f8cab6` and at every commit after it. The integrator squashes
or merges the branch anyway.

---

## 1. The rebase onto `712c9467`

Seven commits replayed; **four conflicted, all in `MobileGL/MG_Test/Pipe/`**, and all in the one
replayed commit `ccde2906`. The cause is that the pipe's `ae1a1c50` ("pin every P4a record's
lifecycle…", one of wire's twelve) filled **the same four stub files C owns** with the applier's
half of the same families: `CompositeResolverTest.cpp`, `ImageEmitTest.cpp`,
`ProgramEmitTest.cpp`, `SamplerEmitTest.cpp`. `MG_Impl/Pipe/*` and `MG_State/**` rebased clean —
c0d's `Tracker.h`, c0e's `Named` and wire's `PipeApply.*` are files C does not touch.

**How they were resolved.** Not by hand-picking hunks: both sides are *additions to the same
stub*, so the resolution is a three-way merge that replays each side's insertions at its own base
anchor. A script took `:1`/`:2`/`:3` from the index, computed both opcode lists against the base,
**asserted that no base range is touched by both sides**, and emitted the union in base order
(the applier block first, then C's `#if !MOBILEGL_PIPE_PUSH` pull-skip list and its `#else`
namespace, then `main()`). The overlap assertion passed on all four files. One duplicated
`#include <MG_Pipe/PipeApply.h>` in `CompositeResolverTest.cpp` was removed by hand.

**Nothing of wire's was lost.** Every `TEST(` name present in `712c9467`'s version of the four
files is present at HEAD: 4 / 5 / 7 / 8 wire cases, **0 lost**. The pull-skip `MGL_*_TEST_LIST`
macros carry only C's push-only cases, which is correct — wire's cases compile in both builds and
`GTEST_SKIP()` internally — and G2 (pull and push ctest name sets identical) holds at 2683 == 2683.

---

## 2. C2-M1 — the composite resolver's key

### 2.1 What changed

| what | where |
|---|---|
| `Entry` gains a context half | `CompositeResolver.h:230` `Uint64 ContextId`, beside `:231` `Uint PipelineName` |
| `Find` requires **both** halves | `:242-247` |
| `Observe` takes the context id | `:132-133` `Observe(Uint64 contextId, const ProgramPipelineObject&, const ProgramObject&, MGPipeHandle)` |
| the one production call site supplies it | `ProgramEmit.h:118-126`, inside `EmitShaderState(GLContext& ctx)`: `Observe(ctx.GetTextureContextId(), *pipeline, *drawProgram, drawCso)` |
| `Fresh` and `HandleFor()` deleted | (C2-m1) — see §2.3 |
| `Reset()` reclaims instead of invalidating | `:188-199`, counted by `Counters::Sweeps` `:120` |
| the file header states the key, the identity and the death path | `:42-69` |

The mismatch arm is now reachable only for **this** context's own entry: another context's
pipeline of the same name is not found, therefore not matched and not released, and its obligation
stays owed to the context that took it (`:147-151`).

### 2.2 The context identity, and why this one

**`GLContext::GetTextureContextId()`** (`Core.h:156-159` → `TextureState.h:135-141`,
`TextureState::AllocateContextId`, a process-wide atomic). It is not a new notion:

* it is already **the pipe's** context identity — `PipeFill.cpp:236-237` copies it into
  `PipeInputs::m_textureContextId` at seven fill points (`FillPoints.def`, `kDraw` / `kDispatch` /
  `kClear` / `kBlitOrCopy` / `kTextureOp` / `kReadback` / `kProgramOp`), and `Coverage.def:229`
  describes it in exactly those words: *"a context identity the backend keys its own tables on"*;
* it is what the backend's own per-context memos key on today (`DirectGLES.cpp`'s
  `g_observedUnitBindingsContextId`, `g_unitTextureSyncListContextId`, `g_imageSweepContextId`,
  `g_unitSamplerWalkContextId`, `Managers.cpp`'s `m_syncedShapeContextId`).

**Deliberately NOT the `GLContext` address**, which is the other candidate in reach —
`MGB_CTX_IDENTITY` (`PipeInputsSwitch.h:21` / `PipeInputs.h:191`) and `MGPipeTracker::m_context`
(`Tracker.h:271`) both compare it. `Core.h:156-158` says why that would be wrong here, in its own
words: *"both restart at 0 in a new context, and a recreated context can land on the old heap
address"* — so an address key would put this same defect back one context destroy-and-recreate
later, with entries of a dead context inherited by its successor.

The id travels as a `Uint64` argument rather than a `const GLContext&` for two reasons: the
resolver keeps its "depends on nothing of the family's" include set (no `Core.h`), and a unit case
can drive two contexts in a process that only ever has one. The header says in capitals that the
argument is `GetTextureContextId()` **and nothing else**, and there is exactly one production
caller.

### 2.3 Where a destroyed context's entries are released — stated

**In `ProgramObject::~ProgramObject`, and nowhere else.** Destroying a `GLContext` drops
`m_programPipelines`, which drops each `ProgramPipelineObject`, which drops the composite it
cached in its one-slot draw-program cache; `~ProgramObject` then runs
`MG_Pipe::MGPipeEmitShaderCsoDestroyAndFree(m_lifetimeId)` (`ProgramObject.cpp:58`) — **D-H7's
second release path** — which emits `delete_shader_state`, drops the publication latch and frees
the band slot, **exactly once**.

The resolver never speaks a second release for such an entry, and could not: no future `Observe`
can carry a dead context id, and `MGPipeSlotAllocator::Free` has already erased the lifetime-id
mapping, so a release would resolve nothing. What the resolver does instead is **drop** the
stranded entry at the next `Reset()` — the make-current, i.e. the one moment
`PipeFill.cpp:2229`'s `FreshlyPrimed` arm tells the client a context boundary was crossed. An
entry is dropped when `!Live` or when `MGPipeSlots().IsLive(ShaderCso, entry.Handle)` is false,
which is precisely "the obligation was discharged elsewhere". This also restores the bound the
name-only key used to have: the vector is now bounded by the (context, pipeline name) pairs whose
composite slot is **actually live**, not by every pair the process ever used.

`Fresh` and `HandleFor()` go with it (**C2-m1**, disposition "give `HandleFor` a caller or delete
it, and say which in the header"): `Fresh` existed only because a name-only key could not tell
"my own entry after a make-current" from "another context's entry", and `HandleFor` was its only
reader and had no caller anywhere in the tree. Both are deleted and `Reset()`'s comment
(`:170-177`) and `Size()`'s (`:203-206`) say so, the latter adding why a `HandleFor(name)`
accessor must not come back.

### 2.4 The cases, and the mutations

Two new cases, both in `CompositeResolverTest.cpp` and both in the pull-skip list:

* **`CompositeResolver.TwoContextsHoldingOnePipelineNameKeepTheirOwnComposites`** (`:660`) — the
  reviewer's probe, as a case. Two `ProgramPipelineObject`s built with **the same external index**,
  two composites (`MakeShared<ProgramObject>(0u)`, external index 0, the shape `Core.cpp` builds),
  two band handles with real `create_shader_state` records and publication latches, then
  `Observe(A)` → `MGPipeProgramEmitterInstance().Reset()` (the make-current) → `Observe(B)`.
  Asserts `Releases` unmoved, and that **A's slot is still live and still published**; then goes
  back to A and asserts the unmoved signature is a `Reuses` and still releases nothing.
  It is driven at the resolver because one test process has one `GLContext`; every value it
  supplies is what the single production call site supplies.
* **`CompositeResolver.ADestroyedContextsEntryIsDroppedRatherThanReleasedASecondTime`** (`:716`) —
  §2.3 as a case: `Observe`, then drop the composite's last `SharedPtr`, and assert the slot went
  back exactly once with the latch, that the resolver spoke **no** release, and that the next
  `Reset()` drops the stranded entry (`Sweeps + 1`, `Size()` back to where it was, `Releases`
  unmoved).

| mutation | case that goes red | result |
|---|---|---|
| `Find` drops the `ContextId` half of the key (v2's key) | `TwoContextsHoldingOnePipelineNameKeepTheirOwnComposites` | **red**, and only that one (11/12) |
| `Reset()` stops reclaiming stranded entries | `ADestroyedContextsEntryIsDroppedRatherThanReleasedASecondTime` | **red**, and only that one (11/12) |
| `Release` stops counting a handle it never handed out | `SamplerEmit.AReleaseThisCacheNeverHandedOutIsCountedRatherThanAbsorbed` | **red**, and only that one (21/22) |

All three applied, built, run and reverted; the tree is clean and 12/12 + 22/22 green again.

---

## 3. The three minors and the three nits

| # | disposition |
|---|---|
| **C2-m1** `HandleFor()` dead, `Fresh` unobservable, `Reset()` a no-op | **TAKEN**, folded into the C2-M1 fix exactly as ruled: both deleted, `Reset()` given a real job (§2.3), and the header says which was chosen and why (`CompositeResolver.h:170-177`, `:203-206`). |
| **C2-m2** `Release` silent on an unknown handle; "a double release is a no-op" is only true when nobody else holds the entry | **TAKEN**, in the counted-refusal idiom C-m6 adopted. `Counters::UnknownReleases` (`SamplerEmit.h:240`) when the probe finds no entry, `Counters::UnderflowedReleases` (`:241`) when the count is already 0, both incremented in `Release` (`:305-320`). The comment above it (`:288-304`) now says in capitals that **the count is per HANDLE, not per holder**, that B's params record and this file's `bind_sampler_states` routinely name the same handle, and that a holder releasing twice therefore *steals another holder's pin* rather than harmlessly repeating itself. New case + mutation (§2.4). |
| **C2-m3** the reconciliation's cost is understated and c0d makes it land far more often | **TAKEN as a code correction**; the `MEASUREMENTS.md` half stays the integrator's (`docs/` is not C's file). `SamplerEmit.h:807-821` now states the real model: each `Release` of a non-null handle is a linear scan of the whole cache beside an `Acquire` probe that is another, so a pass with *K* units holding sampler CSOs costs `2 · K · O(cacheSize)` comparisons — **up to ~98 000 at K = 192 with a full cache, unbounded above that** because the over-capacity path lets the cache grow past 256 — that Minecraft-shaped K is typically 0 and a CTS sampler sweep is the workload with both a large K and a large cache, that **c0d's bit-13 shutter now mixes `GetTextureBindGeneration()` so it runs on every texture bind**, that the set-hash suppressor stops the record but not the two scans, and that C-m2's per-unit latch removes **both**. |
| **C2-n1** `Evict`'s `MOBILEGL_ASSERT` is presented as an ID-17 mechanism but is off in every build that runs | **TAKEN, made a counter** (the reviewer's second option). `Counters::ReferencedEvictions` (`:245`), incremented at `Evict` (`:483-484`); the comment says the sole caller structurally guarantees 0, that the eviction still proceeds (refusing would leave `Mint` without the room it asked for), and why an assert could not carry it. The new case asserts it is 0. |
| **C2-n2** the cache never shrinks after an over-capacity peak | **TAKEN**, one sentence beside `OverCapacityMints` (`:224-232`): read it as *how far the cache permanently grew*, not how often; the size stays at its high-water mark for the life of the process along with that many `SamplerCso` slots and that many applier records; bounded by the peak simultaneous pin count, so memory retention and not a leak. |
| **C2-n3** `AcquireWithForcedHashForTest` is a public production method and §3's "full seam" is not full | **TAKEN**, `:279-285`: the three members a consumer must **not** call are named — `AcquireWithForcedHashForTest`, `ResetForTest` and `ResetCounters` — and the sentence says the class is public because it is header-only, not because all nine members are for callers. B's seam, restated in §5 below, is the other six. |

---

## 4. The defect the rebase itself turned up (`79b58198`)

Not in any review; found by the gate, and it is C's to fix because it exists only where the two
halves meet. **`ctest -L unit` in `build-verify` failed 6 of 1716** immediately after the rebase —
all six of them *wire's* refusal cases, in the four merged files:

```
SamplerEmit.ARecordThatDoesNotDescribeItsOwnParametersIsRefusedNamingTheLength
SamplerEmit.AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated
ImageEmit.AnImageWindowPastTheImageUnitSpaceIsRefusedRatherThanTruncated
ProgramEmit.ACreateWithNoArtefactsAnOversizedBlockOrACorruptSlotIsRefusedNamingTheProgram
ProgramEmit.TheDefaultUniformBlockLandsOnTheProgramsRecordAndTheSentinelIsRefused
CompositeResolver.ASlotAtTheShaderCsoLimitIsRefusedWhileTheLastBandSlotIsNot
```

**Root cause.** `ExpectRefusedNaming`'s poison/verify arm forks a child, expects `SIGABRT` and
then asserts the Fatal line was written to the log. `RunInChild` prepared for that by
`std::filesystem::remove(g_logPath)` before the fork — correct while the library's log file was
still closed, which it was in wire's own files. **C's `main()` calls `MobileGL::Initialize()`**
(its cases need a `GLContext`), and `Log.cpp:63-77` opens the log file with `std::fopen(path,"w")`
once and caches the `FILE*`. `fork()` duplicates it, so after the `remove()` the child wrote its
Fatal line into a **deleted inode** while the parent read a path that no longer existed: the child
aborted correctly (`DiedOfAbort` passed) and `child.Log` was empty, so only the assertion on *what*
it refused failed. It reproduced in `build-verify` only, because the push build's arm reads the log
in-process and never unlinks it; and only in the four files C owns, because they are the only ones
with both `MobileGL::Initialize()` and a forked drive.

**Fix** (all four files, identical): `RunInChild` remembers the log's end (`LogEnd()`,
`SamplerEmitTest.cpp:96`) instead of unlinking the path, and `ReadLog(from)` reads back only what
the child appended (`:133`). The comment at the call site says why the unlink cannot come back.
6/6 go green; `build-verify` unit is 100%.

This is worth the wire reviewer's eye as a **contract between the two halves of these files**: any
future forked drive in `MG_Test/Pipe` must assume the library's log is already open.

---

## 5. The B-facing API — unchanged from v2, with C2-n3's addition

Nothing about B's seam moved in v3. Restated so v2's §3 need not be re-read:

```cpp
#include <MG_Impl/Pipe/SamplerEmit.h>          // add to TextureEmit.h's include block

Uint64 samplerBytes = 0;
const MGPipeHandle builtinSampler =
    MGPipeSamplerCsoCacheInstance().Acquire(sampler->GetAllSamplerParameters(), samplerBytes);
// ... and, when this texture's params record STOPS naming a previously acquired handle:
MGPipeSamplerCsoCacheInstance().Release(previousBuiltinSampler);
```

The six members that are B's: `Acquire` (`SamplerEmit.h:281`), `Release` (`:305`), `RefCountOf`
(`:322`), `RecordIsPublished` (`:340`), `Size`, `GetCounters` — the counter set has grown to
`Mints / Acquisitions / Hits / Collisions / Evictions / Releases / OverCapacityMints /
UnknownReleases / UnderflowedReleases / ReferencedEvictions`. The three B must **not** call are
`AcquireWithForcedHashForTest`, `ResetForTest` and `ResetCounters` (C2-n3).

The four rules are unchanged, and **C2-m2 sharpens rule 2**: B owes *exactly* one `Release` per
`Acquire`, and a second one now shows up as `UnderflowedReleases` (or `UnknownReleases`) rather
than silently stealing `bind_sampler_states`' pin. B's review has a number to assert on.

---

## 6. Gate numbers (head `92dffb81`, working tree clean)

`CCACHE_BASEDIR=/home/swung/w7`, `-j 8`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported.

| gate | result |
|---|---|
| three builds (pull `build-linux` / push `build-push` / verify `build-verify`) | rc 0 / rc 0 / rc 0 |
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **0 added / 0 removed / 0 resized / 0 renamed**, rc 0. `.text` 10806323 → 10806323 (+0, +0.000%); 27811 → 27811 defined symbols, 27072 normalised names unchanged |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (7 negative controls tripped) |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0; **2 UNDECIDED** (the contract's D6, unchanged by c0d); 27 negative controls all tripped |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4` | rc 0, **4 probes**, 0 skipped, 0 problems, 6 negative controls tripped |
| `scripts/p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, the eleven byte-identical |
| `ctest -L unit -j 8`, `build-linux` | **100% passed, 0 failed out of 1717** |
| `ctest -L unit -j 8`, `build-push` | **100% passed, 0 failed out of 1717** |
| `ctest -L unit -j 8`, `build-verify` | **100% passed, 0 failed out of 1717** |
| ctest names pull == push | `diff` 0 lines, **2683 == 2683** |
| no baseline name removed (`LC_ALL=C comm -23` vs `~/w7/p4a-before-ctest-names.txt`) | **0 removed**; **+95** (2588 → 2683). Of those 95, 62 are the pipe's own between `37da3c3a` and `712c9467` (ID-28), so **33 are C's** |
| the emit suites, `-R 'SamplerEmit\.\|ImageEmit\.\|ProgramEmit\.\|CompositeResolver\.'` | **57/57** in `build-push`, `build-verify` **and** `build-linux` (all three 100%) |
| `TrackerWalk.*` (A's c0d cases, `build-push`) | **18/18** |
| `ctest --test-dir build-push -L integration-gpu -R DirectGLES -j 8`, **default arm** | **100% passed, 0 failed out of 491**; `grep -c 'Fatal{'` = **0** |
| same, **`MOBILEGL_PIPE_PUSH=0x1ff`** | **100% passed, 0 failed out of 491**; `grep -c 'Fatal{'` = **0** |
| `ctest --test-dir build-push -L integration-gpu -j 8` (whole label), default arm | **100% passed, 0 failed out of 966** |
| `ctest --test-dir build-verify -L integration-verify -j 4` | **100% passed, 0 failed out of 844**; `grep -c 'Fatal{'` = **0** |
| `python3 ~/w7/retrace_gate.py --tree /home/swung/w7/p4a-clientsp --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-clientsp-v3 -j 4` | **passed 79 / 79**, failed `[]`, rc 0; **0** logs with `Fatal{`, **0** with `ssim=None` |

**Measurement points.** The three `integration-gpu` runs, `integration-verify` and the retrace were
measured at `79b58198`. The only change since is `92dffb81`, one unit case in
`MG_Test/Pipe/SamplerEmitTest.cpp`: `build-linux/libMobileGL.so` is **byte-identical** across it
(sha256 prefix `2625db7a19a54d3a` before and after) and G1, `p3a_untouched_regions`, unit ×3, the
emit suites, the name pair and all three static scripts **were** re-run at `92dffb81`. Nothing that
those five lanes exercise changed.

**The LFS precondition, stated exactly.** `git lfs checkout` in this worktree still prints
`Cannot checkout LFS objects, Git LFS is not installed` — `git-lfs` exists but `git lfs install`
has never configured the smudge filter for this user. It does not matter: the fixtures are the
real files restored by C's v1 (deviation D11) and hidden with `--assume-unchanged`. Measured
immediately before the retrace: **94 files, 803 MB, `find … -size -1k | wc -l` = 0**,
`git status --porcelain` empty. Every case reported a real `ssim` (1.0 / 0.999x), never `None`,
so the `passed 2 / 79` false red did not occur.

---

## 7. The c0d / c0e re-run list, and the deltas

The re-review's §9 said the numbers do not carry over from v2 because c0d changes *how often* C's
code is entered: bit 13's shutter now mixes `GetTextureBindGeneration()` (so `EmitSamplerStates`
runs on every texture bind at any unit), and the `else if (pipeline)` arm drives bits 6/7/8/14
under a bound separable pipeline (so `EmitShaderState` and the composite resolver are exercised for
the first time). With c0d **and** wire on the base, everything named was re-run:

| lane | v2 (base `17db7598`, stub applier) | v3 (base `712c9467`, real applier + c0d + c0e) | delta |
|---|---|---|---|
| emit suites, `build-push` | 34 / 34 | **57 / 57** | +23 = 20 wire applier cases + 3 of C's own |
| emit suites, `build-verify` | (not reported apart) | **57 / 57** | — |
| `TrackerWalk.*` | not run (c0d not on base) | **18 / 18** | new |
| `ctest -L unit` ×3 | 1668 | **1717** | +49 |
| ctest names pull == push | 2634 | **2683** | +49 |
| names added vs baseline | +46 | **+95** | +49 (62 pipe − 16 already counted + 3 C) |
| `integration-gpu -R DirectGLES`, default | 491 / 491 | **491 / 491**, 0 Fatal | unchanged |
| `integration-gpu -R DirectGLES`, `0x1ff` | 491 / 491 | **491 / 491**, 0 Fatal | unchanged |
| `integration-gpu`, whole label, default | 966 / 966 | **966 / 966** | unchanged |
| `integration-verify -j 4` | 844 / 844, 0 Fatal | **844 / 844**, 0 Fatal | unchanged |
| retrace | 79 / 79 | **79 / 79**, 0 Fatal, 0 `ssim=None` | unchanged |
| G1 | 0/0/0/0 | **0/0/0/0** | unchanged |

(The emit-suite arithmetic, checked at the source: the four stub files each carried one case at
`17db7598` and carry 4 / 5 / 7 / 8 at `712c9467`, so wire added **20**; C's own three are the two
composite-resolver ones and the sampler release-counter one. 34 + 20 + 3 = 57. The same 46 names
the pipe gained between the two bases plus C's 3 account for the +49 in the unit and name rows.)

**The scenarios the re-review named as the only things that can see the paths c0d opened**, all on
the default arm of `build-push`:

| scenario set | result |
|---|---|
| `-R 'ProgramPipeline'` (the sixteen SSO conformance cases and their siblings) | **22 / 22** |
| `-R 'ImageLoadStoreSso\|RelinkStageSet\|PostLinkAttach'` | **16 / 16** |
| `-R 'IntegerBorderColor\|SampledSetStaleness\|PixelStoreSweep'` (a different sampler object rebound to a unit) | **18 / 18** |
| `-R 'CsoContentAddressing\|ObjectSubsystemControl\|ResourceSubsystemControl'` | **10 / 10** |

**What this means, stated honestly.** On `712c9467` the applier is real but **Espryt is still v1**,
so there are no Espryt handle twins at all: the default arm's shape is *"the client emits, the
server pulls"* — every record C sends lands in `MGPipeApplier()` and is validated there (the
`Start + Count` bounds, the `Parameters.Size` gate, the `link`/`spirv` non-null gate, the composite
band's table split, the verify build's program-archive round trip **all ran for the first time**,
which is what the 6 refusal cases of §4 exercise), while the backend still reads the pulled
`PipeInputs` block for its drawing. So a green `integration-gpu` here proves *the records are
well-formed and nothing the client emits perturbs the pull path*; it cannot yet prove Espryt
consumes them. **No Fatal on any arm, 0 in `integration-verify`, and no refusal counter moved.**

---

## 8. What the integrator must re-run

1. **B's seam first** (§5, ID-14 / ID-17): B takes `BuiltinSampler` from
   `MGPipeSamplerCsoCacheInstance().Acquire(...)`, holds the reference, releases the previous
   handle on re-acquire and at texture death, and **deletes** its own
   `MGPipeSlots().Acquire(SamplerCso, …)`. With wire underneath, a null or stale `BuiltinSampler`
   is `Fatal{ProtocolCorruption}` on **every** `set_texture_params`. B's review should now also
   check `UnknownReleases == 0 && UnderflowedReleases == 0` after its texture cases — C2-m2 made
   that assertable.
2. **`check_include_closure.py … --expect-probes 4` again after B adds `#include
   <MG_Impl/Pipe/SamplerEmit.h>` to `TextureEmit.h`** — the probe count moves.
3. **`MEASUREMENTS.md` under ID-19**, which is `docs/` and therefore not C's file: C-m2's per-unit
   `(sampler lifetime id, GetSamplingResolutionGeneration())` acquire latch **with the corrected
   cost model** (C2-m3 — `2 · K · O(cacheSize)`, not one array walk, and c0d's bit-13 shutter makes
   it fire on every texture bind), and the one extra `set_global_constants` per link that C-M2's
   case turned up.
4. **The wire reviewer should see §4** — the forked-drive/log-file contract between wire's cases
   and C's `MobileGL::Initialize()`. The fix is in C's files; the rule belongs to both halves.
5. **A DirectVulkan arm for the death/leak cases** (ID-8, C-m10) — still F's, still to be confirmed
   rather than assumed.
6. **After `esprytobj` + `esprytdraw`**: `MOBILEGL_PIPE_PUSH=0x5ff` / `0x9ff` must be **refused** at
   D's texture-family `Resolve*SubsystemArm` (ID-15's fourth D-K2 row), never half-run — C emits
   `MGPBoundView::Texture` (`SamplerEmit.h`) and `MGPImageView::Res` (`ImageEmit.h:97-99`) as
   `Texture` handles unconditionally.
7. **The open question C named and still declines**: whether `MGPipeEmitSamplerCsoCreate` gets its
   one-line call site in `SamplerObject`'s constructor. C's reasons for declining are in
   clientsp-v2 §2 and are unchanged; if the answer is no, write it into ID-26 rather than leave it
   as a loose end.
8. **`ProgramPipelineScenario` and the sampler-rebind scenarios on the integrated tree once Espryt
   v2 lands** — §7 says why a green run here is necessary but not sufficient.

---

## 9. Per-item closure table

| item | ruling | v3 disposition |
|---|---|---|
| **C2-M1** | key `Entry` on (context identity, pipeline name) using the identity the pipe client already uses; say where a destroyed context's entries are released; add the probe as a case + a mutation | **CLOSED** — `GetTextureContextId()`; `~ProgramObject` (§2.3); 2 cases, 2 mutations |
| **C2-m1** | fold into C2-M1: give `HandleFor` a caller or delete it, and say which | **CLOSED** — deleted with `Fresh`, said in the header |
| **C2-m2** | two counters + one sentence | **CLOSED** — `UnknownReleases` / `UnderflowedReleases`, the per-handle-not-per-holder sentence, a case and a mutation |
| **C2-m3** | record the corrected cost model | **CLOSED in code**; the `MEASUREMENTS.md` half is the integrator's (§8.3) |
| **C2-n1** | say so in the report row, or make it a counter | **CLOSED** — made a counter (`ReferencedEvictions`) |
| **C2-n2** | one sentence beside `OverCapacityMints` | **CLOSED** |
| **C2-n3** | tell B the three it must not call | **CLOSED** — in the header and in §5 |
| rebase onto `712c9467` | resolve faithfully | **DONE** — §1, 0 of wire's 24 cases lost, overlap-checked union |
| the forked-drive log defect | not in any review | **FIXED** — §4, `79b58198` |
