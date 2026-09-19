# P2 package C — `p2/espryt` (Espryt 0b, the first Track H slice) — result v2 (rework round 2)

Tree: `~/w7/p2-espryt`, branch `p2/espryt`, base tag `p2/contract` (`9c6a8a25`).
Build dirs now present: `build-linux` (pull, the G1 build), `build-push`, **`build-verify`**
(`-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`), **`build-nolegacy`**
(`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`) and **`build-bench`**
(`-DMOBILEGL_BUILD_BENCHMARK=ON -DMOBILEGL_PIPE_PUSH=ON`, `DriverBench` only).
The last three are new in this round and exist to answer review minors 9 and 12 and MAJOR 4.

`build-nolegacy/` shows up as an untracked directory in `git status`; it is a build dir, not
content, and is not committed.

## 1. Commits

Rework round 2 added two commits on top of the v1 four:

| sha | subject |
|---|---|
| `dc9b7237` | `[Feat] (Espryt): give the backend a dense {slot, gen} twin table beside the address-keyed registry` |
| `fbdaeef3` | `[Refactor] (Espryt): key every backend twin on {slot, gen} instead of the frontend object heap address` |
| `1e0f4d6d` | `[Test] (Espryt): pin the twin table identity contract - a reclaimed slot is a new handle and the stale one resolves to nothing` |
| `d714600a` | `[Fix] (Espryt): do not return a reference through a null slot pointer, and stop a comment claiming a memo that is no longer there` |
| **`d89fb684`** | **`[Fix] (Espryt): sweep the twin table on object churn again, refuse to run an arm the operator disabled, and give the hot lookups back their array probe`** |
| **`5f245ac7`** | **`[Test] (Espryt): pin the churn-driven sweep cadence, the null tolerance, and the one slot per object the two Track H slices share`** |

Files touched across all six — still exactly package C's rows of the C.5 ownership table:
`MG_Backend/DirectGLES/{SlotTables.h (new), Managers.h, Managers.cpp, DirectGLES.cpp}` and
`MG_Test/SanityTest.cpp`. No file owned by another package was edited in this round either.

## 2. What the two new commits do, review item by review item

### MAJOR 1 — the churn-driven sweep is back (FIXED)

`BackendSlotTable` now carries **both** sweep drivers, exactly as the map arm does:
`kGCInterval = 1024` on the draw-path tick and **`kCreationGCInterval = 64` on first-time
insertions**, swept at the top of `GetOrCreate` before the `Entry&` exists (same reason the map
arm sweeps there: `EntryAt` can grow `m_slots` and move every element). `CollectGarbage{IfNeeded,
Now}` reset the creation tick when they sweep, so the two drivers do not double-count.

Pinned by a new case that never calls `CollectGarbage*` at all:
`DirectGLESSlotTable.ObjectChurnAloneDrivesTheSweep` churns 256 objects through `GetOrCreate`,
letting each die immediately, and asserts (a) at most `interval + 2` live entries ever coexist,
(b) the table ends at most that big, (c) the allocator's `HighWater` for the kind grew by at most
`interval + 2` rather than by 256.

**Negative control run** (`ObjectChurnAloneDrivesTheSweep` must be able to go red for the reason
it exists): with `if (m_creationTick >= kCreationGCInterval)` changed to `if (false && …)` and
`build-push` rebuilt, `ctest -R DirectGLESSlotTable` gives

```
The following tests FAILED:
	 88 - DirectGLESSlotTable.ObjectChurnAloneDrivesTheSweep (Failed) unit
```

— it is the only one of the seven that fails — and after `cp /tmp/slot.memo …; cmake --build`,
`7/7 Passed` again. (A first attempt at this control raised `kCreationGCInterval` to `0xFFFFFFFF`
instead; that made the *test itself* loop 2^32 times, so the churn count is now a fixed `256`
with an `ASSERT_LT(interval, kChurn)` guard, and the control fails fast instead of hanging.)

**v1 §4.1 point 2 was wrong and is retracted.** The memory behaviour of the handle arm is now the
same as the map arm's, not worse; what is still missing is the *improvement* e2 would bring
(death announced rather than discovered) — see MAJOR 3.

### MAJOR 2 — `Fatal{PipeLegacyMemosDisabled}` now stops, and stops early (FIXED)

`MGLOG_F(...)` is followed by `std::abort()` (`Managers.cpp`), matching the codebase's own
Fatal convention (`MG_Impl/Pipe/PipeFill.cpp`'s `BadKnob` and its verify trap). The resolution is
also forced at **backend context creation** (`InitDisplayAndContext`, first statement after
`DestroyEGLContext()`), so a process that never twins anything still learns its knobs left it
with no arm.

Reviewer's exact reproduction, re-run:

| command | v1 (reviewer) | v2 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData` | `exit=0`, `[ PASSED ] 1 test`, Fatal at line 317/319 | `exit=0`, **`[ PASSED ] 0 tests`**, `[ SKIPPED ] 1`, harness prints *"the EGL bring-up ABORTS on this platform: a forked pre-flight child died on signal 6 (Aborted)"*; the Fatal is the **last line** of the log (69/69), i.e. at context creation |
| `… ctest -L integration-gpu -R 'DirectGLES.CrossFrameBuffer'` | `100% tests passed … out of 13` | **all 13 `(Skipped)`** |
| same + `MOBILEGL_ITEST_REQUIRE_GPU=1` (what `build-verify` is configured with) | — | **13 `(Failed)`**, `Errors while running CTest` |

So the arm the operator disabled no longer runs, and the A/B lever D18 depends on can no longer
be mis-set silently. The residual sharp edge is the harness's own: without
`MOBILEGL_ITEST_REQUIRE_GPU=1`, a whole-suite abort presents as skips, which the harness itself
warns about in the same breath (*"a run that skipped everything is otherwise indistinguishable
from one that passed"*). That is package E's lever and it already exists.

### MAJOR 3 — step `e2` is still absent (NOT FIXED — still a blocking, integrator-owned deviation)

Unchanged from v1 and for the reason the reviewer accepted: `e2` has to be installed in
`MG_State/GLState/{Texture,Framebuffer,Renderbuffer,Sampler,Program,VertexArray}State/*`, and
C.5 gives every one of those files to package B (or, for `VertexArrayObject.h`, to D); C.2's own
**Files** line grants package C none of them. The standing instruction is to stop and record a
blocking deviation rather than cross an ownership line, so that is what this is, again.

What changed is the damage: with MAJOR 1 fixed, `e3` landing without `e2` is now a **no-op** on
twin lifetime rather than a regression. `ARCHITECTURE.md:363`'s census still cannot count the six
registries' GC as deleted, and the seven `CollectGarbageIfNeeded` call sites
(`DirectGLES.cpp:1273, 1660, 2025, 2026, 2027, 2857, 2858` at HEAD) still stand, now driving
`ReclaimDeadSlots()`.

Integrator decision required, exactly as the review says: re-assign `e2` to package B, or land B
before C and re-open C for one commit. Nothing in the table needs to change when it lands —
`ReclaimDeadSlots()` becomes the fallback path of an explicit `Destroy(handle)`.

### MAJOR 4 — the hash probe, and a cost datum that now exists (FIXED, with an honest caveat on the desktop noise floor)

Two code changes, then three measurements.

**(a) `EsprytSlotTablesEnabled()` is now inline** (`SlotTables.h`) over an out-of-line
`ResolveEsprytSlotTablesArm()` (`Managers.cpp`). It was a cross-TU call with no LTO, consulted on
every `Find`/`GetOrCreate`/`HandleOf`/`ForEachLive`/`CollectGarbage*`.

**(b) `HandleOf` keeps a one-entry `lifetimeId -> handle` memo.** The three per-draw resolution
paths ask the same table for the same object every draw, so the steady state is an integer
compare plus an array index again — which is what D13 traded the three `TwinLookupMemo`s for. It
cannot serve a stale answer, by two independent arguments: a lifetime id is never handed out
twice (so a recycled heap address cannot hit it, which is what the deleted address-keyed memos
needed a `WeakPtr` for), and every consumer resolves through `FindByHandle`, which compares `Gen`.
It is cleared anyway when the sweep frees the memoised slot.

**Measurement 1 — call counts under gdb** (the reviewer's own instrument; noise-free).
`CrossFrameBufferScenario.VertexBufferSubData` on `build-push`, ctest environment, no
`MOBILEGL_PIPE_PUSH` override:

| breakpoint | memo OFF (negative control) | memo ON (HEAD) |
|---|---|---|
| `MGPipeSlotAllocator::FindByLifetimeId` | **24** | **6** |
| `MGPipeSlotAllocator::Acquire` | 5 | 5 |
| `MGPipeSlotAllocator::Allocate` | 5 | 5 |
| `MGPipeSlotAllocator::Free` | 0 | 0 |
| the arm selector (out-of-line body) | 1 | **1** (was 92 `EsprytSlotTablesEnabled()` calls in v1) |

`Acquire` itself calls `FindByLifetimeId` (`SlotAllocator.cpp:107-111`), so 5 of the 6 remaining
probes are inside object *creation*. The **resolution** path went from 19 hash probes to 1 (the
cold first lookup that fills the memo). `Free` is 0 in this scenario because it churns 5 objects
and none of them dies — the sweep cadence is pinned by the unit case above instead.

**Measurement 2 — DriverBench, desktop, `~/w7/p2-espryt/build-bench`** (D.4.3's instrument).
`run_driver_bench.sh espryt <lib>`, `EGL_PLATFORM=surfaceless`, Mesa/llvmpipe, `taskset -c 20-23`,
9 repeats per arm, arms interleaved round-robin so drift is shared. Four arms: `pull` =
`build-linux/libMobileGL.so`; `push_default` = `build-push` with the default bitmask;
`push_bit0` = `build-push` with `MOBILEGL_PIPE_PUSH=0`; `push_no_espryt` = `build-push` with
`MOBILEGL_PIPE_PUSH=0x5f` (this slice's bit clear, everything else as default).

| case | T1 min / med | T2 min / med | **arm A/B (0x7f − 0x5f), same binary** min / med |
|---|---|---|---|
| `draw_tiny` | +441 / +1294 | +498 / +1408 | **−66 / +481** |
| `draw_multi_vao` | +506 / +1583 | +549 / +2011 | **+28 / −1003** |
| `program_pingpong` | +637 / +1864 | +550 / +509 | **+183 / −41** |
| `tex_pingpong` | +363 / +1849 | +358 / +1278 | **+193 / −1802** |
| `mc_vanilla_draw` | +510 / +1813 | +569 / +801 | **−17 / −354** |
| `mc_state_toggle` | +750 / +4489 | +1130 / +10222 | **+40 / +987** |

(ns/op; op = draw for `mc_vanilla_draw`, 5495 per frame. Raw rows: `~/w7/e2-bench2.csv`.)

**What this does and does not say.** The host is llvmpipe and was running at load average
29–51 on 28 cores (the other P2 packages were building and testing on it throughout), so the
within-arm spread of the *pull* arm alone is 65–95 % of its own median. On that noise floor:

- T1 ≈ T2 on every case by the min statistic (+363…+750 vs +358…+1130 ns/op), i.e. essentially
  all of the pull→push cost is **P1's residual fill**, and P2 adds nothing measurable on top.
- The same-binary arm A/B, which differs **only** in this slice's arm, is **−66…+193 ns/op** by
  the min statistic and changes sign by the median. It is inside the noise; what can be said is
  that no several-hundred-ns per-draw regression from this slice exists on this host.
- The brief's ceiling (`T1 ≤ 45 ns/draw` on Adreno 830, `≤ 60` on Mali) is a **device** number
  and this host cannot resolve it. D.4.2/D.4.3 on the two devices remain owed, and this slice
  should be measured there with the same `0x7f` vs `0x5f` arm A/B, which is the cheapest
  attribution available (one binary, one lock hold).

Recorded honestly rather than dressed up: v1 produced no cost datum at all, v2 produces a
call-count datum that is decisive about the mechanism and a timing datum that only bounds the
magnitude on a contended software rasteriser.

### Minors

| # | review item | disposition |
|---|---|---|
| 1 | `TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` cannot fail | **fixed.** Replaced by `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin`, which asserts the allocator's `LiveCount(kind)` grows by exactly **one** however many tables hold a twin of the object (a per-table allocator passes the old pure compare and fails this), and that the shared handle still addresses each table's own twin. |
| 2 | `GetOrCreate(nullptr)` derefs null in release | **fixed.** Returns a per-table parking `BackendPtr`, never live, never swept, never handed a handle — the map arm's documented tolerance. The `MOBILEGL_ASSERT` was *removed* from that path on purpose: a DEBUG build must not trap where release quietly does the defined thing. Pinned by `GetOrCreateToleratesANullStateObject`. |
| 3 | latent UAF in `ReclaimDeadSlots` | **fixed.** The twin is `std::move`d into a local, the entry is finished with inside a scope, the slot is freed, and only then is the local released — so a twin destructor that re-entered `GetOrCreate` and grew `m_slots` cannot make the writes land in freed memory. |
| 4 | nothing returns slots to the allocator on destruction/reset | **argued, not fixed.** D13 requires the table to keep `ScopedDirectGLESTextureBindings`' copy-assign-and-restore shape, so two live table values can name the same slot (the fixture's `saved` and the working table do exactly that). A destructor or reset that called `Free` would therefore be a double free through a copy — the double free is real and reachable: `Free` on the same `{slot, gen}` twice pushes the slot on the free list twice and two later `Allocate`s hand out the same slot. Returning slots has to be owned by the explicit-destroy path (`e2`), which is blocked. Recorded as debt; the new two-table case carries a comment saying why it deliberately does not sweep. Consequence the integrator inherits: `MGPipeSlotAllocator::HighWater`/`LiveCount` are still not a process-wide health signal from a test process. |
| 5 | the backend mints client handles | **recorded, not fixed.** A P3+ DEBT block at the top of `SlotTables.h` names `MGPipeHandles.h:13-16`, says the minting and the `lifetimeId -> handle` resolution both belong on the client, says the backend should receive the handle in the verb payload under split, and says `check_include_closure.py` does not probe `MG_Backend` headers so nothing catches it. It is monolith glue and is **not** "Track H done". |
| 6 | `MOBILEGL_PIPE_PUSH=0` is no longer the faithful P1 control | **fixed.** `g_fbSlotCache` is compiled under `#if !MOBILEGL_PIPE_PUSH \|\| MOBILEGL_PIPE_LEGACY_MEMOS` and `GetFramebufferBindingSlotChecked` uses it when `!EsprytSlotTablesEnabled()`. So the poison bypass is closed on the arm that ships and `MOBILEGL_PIPE_PUSH=0` reproduces P1 at those five sites too, which is what `ConfigLoader.cpp:251-254` promises. `build-nolegacy` (no legacy arm) takes the `#else`, an unconditional checked read. |
| 7 | `MGB_TWIN_KIND_PARAM`/`ARG` leak into every TU | **fixed.** `MGB_TWIN_KIND_ARG` is gone; the twelve declaration and definition sites name a `TwinRegistry<S, B, kKind>` alias template that swallows the kind in the pull build (an alias template may have an unused parameter and emits no symbol, so the pull mangling is unchanged — confirmed by G1 below). `MGB_TWIN_KIND_PARAM` is `#undef`'d immediately after the class. |
| 8 | shadowing at `BindCurrentFBO` | **fixed.** The twin-table lookup is `twinEntry`; the framebuffer binding slot keeps the name `slot`. |
| 9 | the `build-nolegacy` row was unverifiable | **fixed.** `build-nolegacy` exists and was built and run; see §3. |
| 10 | §5.3's cold-cache flake could not be reproduced | **root-caused, and v1's explanation was wrong.** See §5.2 — it is a ctest **scheduling** flake, not a cold shader cache. |
| 11 | §4.1 point 2 ("unchanged") is factually wrong | **retracted**, and the underlying defect fixed (MAJOR 1). It does not appear in this file except as a retraction. |
| 12 | no `build-verify`, so the verify read-hook was never run against the handle arm | **fixed.** `ctest --test-dir build-verify -L integration-verify -j 4` → **818/818 passed**, and `grep -l 'Fatal{'` over the six `pipe-verify-*.log` lanes is **empty**. The five converted `GetFramebufferBindingSlotChecked` sites now go through the checked accessor on the handle arm and produce no divergence. |
| 13 | the reported G3 SSIM minimum is wrong | **fixed and confirmed.** The reviewer's figures reproduce exactly; see §3. |

## 3. Verification — commands run and what they printed

All from `~/w7/p2-espryt`, `CCACHE_BASEDIR=/home/swung/w7`. Logs under `~/w7/e2-*`.

| gate / check | command | result |
|---|---|---|
| build | `cmake --build {build-linux,build-push,build-verify,build-nolegacy,build-bench} -j 12` | all `rc=0` |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; resized = `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — **all four are the contract commit's** |
| **G1 attribution** | same script, `--before ~/w7/p2-contract/build-linux/libMobileGL.so --after build-linux/libMobileGL.so` | `.text +0`, `0 added, 0 removed, 0 resized, 0 renamed` — **package C's pull-build delta is still exactly zero after this round**, including the `TwinRegistry` alias |
| **G2** | `ctest -N \| sed -n 's/^ *Test  *#[0-9]*: //p' \| sort` on all four dirs, `diff` | `build-linux` 2374, `build-push` 2374, `build-nolegacy` 2374 — both diffs **empty**. (`build-verify` lists 3192: it adds the verify-only lanes, which is by design.) |
| **G5** | `git show {48268068, p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, rc 0 |
| **G13** | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| **G13** | stdio grep over the four touched backend files | 9 hits, **all the English word "puts" in comments** (the base ref has 6 of the same in `DirectGLES.cpp` alone); no `printf`/`cout`/`cerr` |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | **empty**. Added: the contract's 4 `*.PlaceholderUntilTheOwningPackageFillsThisIn` and this package's **7** `DirectGLESSlotTable.*` (5 from v1, one of which was renamed, plus 3 new) |
| legacy residue | preprocessor-stack walker over `DirectGLES.cpp` for `OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|GetFramebufferBindingSlotFast` | 24 hits, **`UNGUARDED: 0`** — each is inside `#if MOBILEGL_PIPE_LEGACY_MEMOS`, the `#else` of `#if MOBILEGL_PIPE_PUSH`, or the new `#if !MOBILEGL_PIPE_PUSH \|\| MOBILEGL_PIPE_LEGACY_MEMOS`. `GetFramebufferBindingSlotFast` is gone entirely |
| unit | `ctest -L unit --no-tests=error -j 6` on all four dirs | `100% tests passed, 0 failed out of 1496` × 4 |
| slot-table cases | `ctest --test-dir build-push -R DirectGLESSlotTable --output-on-failure` | `7/7 Passed` |
| slot-table cases, pull | same on `build-linux` | all 7 present and `(Skipped)` — a visible skip, as C.4 wants |
| **churn negative control** | neuter the creation sweep, rebuild `build-push`, `ctest -R DirectGLESSlotTable` | **only** `ObjectChurnAloneDrivesTheSweep` fails; `7/7` again on restore |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 -R DirectGLES` | `446/446` (run 1 and run 2) |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | `446/446` |
| integration, no legacy arm compiled | `ctest --test-dir build-nolegacy … -R DirectGLES` | run 1 `444/446` (the scheduling flake, §5.2), run 2 `446/446` |
| integration, verify build | `ctest --test-dir build-verify … -R DirectGLES` | run 1 `853/855` (same two), run 2 `855/855` |
| **integration-verify (G4 half)** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **`818/818 passed`**; `grep -l 'Fatal{' build-verify/…/pipe-verify-*.log` **empty** |
| **Fatal{PipeLegacyMemosDisabled}** | reviewer's two commands, verbatim | see the table in §2 — the disabled arm no longer runs |
| **G3 (DirectGLES half)** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/e2-espryt -j 4 --only 'DirectGLES$'` | rc 0, **`passed 39 / 39; failed: []`**. Five lowest SSIM: **0.993475** `minecraft-1.21.11-main-menu`, 0.995426 `…iris-makeup-ultrafast-in-world`, 0.995920 `…iris-iterationt-nodsa-in-world`, 0.996197 `…iris-derivative-main-d24.4.14-in-world`, 0.996301 `…iris-sundial-lite-in-world`. **The reviewer's figures are the correct ones; v1's "lowest 0.997009" was wrong.** All 39 ≥ 0.99 |
| **DriverBench T1/T2 + arm A/B** | §2 MAJOR 4, raw rows `~/w7/e2-bench2.csv` | produced; noise-bounded |
| gdb call counts | §2 MAJOR 4 | `FindByLifetimeId` 24 → 6; arm selector 92 → 1 |

## 4. Deviations from the brief (v2)

### 4.1 BLOCKING — step `e2` is not implemented (unchanged from v1)

See §2 MAJOR 3. The brief contradicts itself: C.2 step `e2` requires files that C.2's own
**Files** line does not grant and that C.5 gives to B and D. Integrator decision.

### 4.2 The `TwinLookupMemo`s are replaced by a table-local one-entry memo, not by a bare array index

D13's "direct slot indexing" is true of the *table* (`FindByHandle` is a bounds check plus an
index) but not of resolving a frontend object to its handle, which goes through the allocator's
`lifetimeId -> slot` hash. v1 left that hash on the three hottest paths; v2 puts a one-entry
`lifetimeId -> handle` memo in front of it (§2 MAJOR 4). This is the mitigation the review names
("restoring the memo keyed on `{slot, gen}`"), except that the key is the lifetime id — which is
strictly safer than `{slot, gen}` would be, because a lifetime id is never reused at all while a
slot is, and the `Gen` compare in `FindByHandle` remains as the second line of defence.

### 4.3 `UnitBindingsSnapshot` holds lifetime ids, not `{slot, gen}` (unchanged from v1)

A texture that is bound but never synced has no twin and therefore no handle, so a handle-keyed
snapshot would read two never-synced textures as equal. The reviewer independently agreed.

### 4.4 The four `pDefaultFramebufferInfo->defaultFBO` compares are not retired (unchanged from v1)

The default framebuffer is never registered in the twin table, so `HandleOf` answers the null
handle for it; somebody has to own minting and installing `kMGPipeDefaultFramebuffer` first. The
reviewer agreed this is not actionable as written.

### 4.5 Some deletions are arm-split rather than outright (unchanged from v1)

`ScopedDetachedTextureFramebufferAttachments`' walk and `UnitBindingsSnapshot` keep the pull text
verbatim under `#else`, because the shared form moved pull-build codegen and G1 admits no resize
beyond the contract's.

### 4.6 `retrace_gate.py` has no `--backend` or `--ssim` flag (unchanged from v1)

It takes `--tree --lib --out -j --only`; the SSIM threshold comes from the reference
`build-retrace` `CTestTestfile.cmake` it parses. C.2's and C.3's verification blocks and G3's row
in section A spell flags that do not exist. Tree wins on the fact.

### 4.7 New in v2: the `MOBILEGL_ASSERT` on a null state object was deliberately removed

On the handle arm only. Null is now a *defined* answer, so trapping in a DEBUG build while
release quietly parks the twin would be the worst of both. The map arm keeps its assert.

### 4.8 New in v2: one v1 test name was changed

`DirectGLESSlotTable.TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` →
`…ShareOneSlotAndKeepTheirOwnTwin`. G14 is untouched (the old name was added by this package on
this branch and is not in `~/w7/p2-before-ctest-names.txt`), but the integrator should know the
name moved between review rounds.

## 5. Where the tree contradicted the brief

### 5.1 The contract commit resizes a fourth pull-build symbol (unchanged from v1, re-confirmed)

`_GLOBAL__sub_I_DirectGLES.cpp` is 1340 → 1331 bytes (−9) at `p2/contract` with zero package-C
changes; contract→HEAD is a byte-identical `.text`. G1's admitted set (D15 point 4, C.0's
verification block) must be widened by that symbol, and the attribution belongs to package A's
D3.1, not to C. Re-verified this round after the `TwinRegistry` alias landed.

### 5.2 §5.3 of v1 was wrong about the flake: it is a ctest **scheduling** flake, not a cold cache

`DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn`
and `DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`
fail on the **first** `ctest -L integration-gpu -R DirectGLES -j 4` of a build directory and pass
on every run after. v1 guessed "cold shader/translation cache". That is refuted:

```
$ rm -f build-push/Testing/Temporary/CTestCostData.txt     # warm binaries, unchanged library
$ ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES --output-on-failure
99% tests passed, 2 tests failed out of 446
	2336 - DirectGLES.UnlocatedIoBlocks…TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn (Failed)
	2369 - DirectGLES.PointSizeDemotion…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn (Failed)
```

Deleting **only** ctest's cost data — nothing else changes, same library, same build — reproduces
it exactly; the next run (cost data regenerated) is `446/446`. It reproduced on three separate
build directories this round (`build-push` after the cost-data removal, first runs of
`build-nolegacy` and `build-verify`) and on both arms, so it is not attributable to this package:
the library is byte-identical between the failing and the passing run of the same directory.

The failure itself is the log-window assertion:

```
PointSizeDemotionScenario.cpp:512: Failure
Expected: (appended.find("demoted to an ordinary varying")) != (std::string::npos)
```

`LibraryLogSize()` / `LibraryLogSince(before)` read a byte window of
`${CMAKE_CURRENT_BINARY_DIR}/point-size-demotion-gles.log`, which every entry of that lane shares
(`MG_IntegrationTest/CMakeLists.txt:387, 370`). Running the lane alone under `-j 4` is green, so
the losing interleaving needs the full 446-test schedule; ctest's cost-data ordering is what
decides whether it happens. **A clean CI runner never has cost data**, so D.3 will meet this
every time. Fix belongs to package E (the lane's entries need `RUN_SERIAL` or a `RESOURCE_LOCK`,
or the assertion needs a per-test log path); it is not fixable from package C's file set.

### 5.3 The G2 / G14 `ctest -N` extraction in section A silently drops most tests (unchanged from v1)

`grep -E '^\s+Test #'` misses `  Test   #83:` once the total is four digits. Working form used
throughout: `ctest --test-dir <dir> -N | sed -n 's/^ *Test  *#[0-9]*: //p' | sort`. Section A
(G2, G14) and D.1/D.3 need the fix. The reviewer confirmed this independently (1373 of 2372).

### 5.4 `git stash -u` in a P2 worktree destroys the copied trace fixtures (unchanged from v1)

`wsl_p2_tree.sh` copies the 803 MB fixture set in and marks it `--assume-unchanged`; `git stash`
resets through that bit and leaves LFS pointer files. Not hit this round (the negative controls
used `cp` to `/tmp`, never `git stash`), and the fixture set is intact (`du -sh` = 803M).

### 5.5 `p2/contract` is an ambiguous refname in this worktree

`git show p2/contract:…` warns `refname 'p2/contract' is ambiguous` (a tag and a branch-shaped
path both match). Harmless — it resolves to the tag — but the integrator will see the warning in
every G5 run.

## 6. Unfinished

1. **Step `e2`**, the explicit-destroy hook — blocked on file ownership (§4.1). The one scoped
   deliverable of C.2 that is not present. With MAJOR 1 fixed it is no longer a regression, but
   `ARCHITECTURE.md:363`'s census still cannot count the six registries' GC as deleted, and the
   seven `CollectGarbageIfNeeded` call sites still stand.
2. The four `pDefaultFramebufferInfo->defaultFBO` compares are not replaced by
   `kMGPipeDefaultFramebuffer` (§4.4) — needs an owner for the reserved handle.
3. **Slots are never returned to the allocator on table destruction or reset** (minor 4). Argued
   rather than fixed: two table *values* can name one slot under the copy-and-restore fixture
   shape D13 mandates, so a `Free` on discard is a reachable double free. The release belongs to
   `e2`. `HighWater`/`LiveCount` therefore remain unusable as a process health signal in tests.
4. **The backend still mints client handles** (minor 5). Recorded as P3+ debt in `SlotTables.h`;
   it must not be counted as "Track H done", and `check_include_closure.py` does not probe
   `MG_Backend` headers so nothing will catch a second instance.
5. **Device measurement.** The desktop DriverBench numbers exist now but the host (llvmpipe,
   load average 29–51) cannot resolve a 45 ns/draw ceiling. D.4.2's two-device paired A/B and
   D.4.3's T1/T2 on device are still owed, and this slice should get the `MOBILEGL_PIPE_PUSH=0x7f`
   vs `0x5f` arm A/B there — one binary, one lock hold, exact attribution.
6. **The `integration-gpu` first-run flake** (§5.2) is root-caused but not fixed; the fix is in
   package E's files. D.3 will hit it on every clean runner.
7. No `HandleRecycleScenario` (package E owns it), no `PipeStats` gate for this subsystem
   (package A owns `PipeStats.{h,cpp}`), so the integrator's A/B still has no counter that says
   "N twins resolved through the slot table this frame" — only the gdb counts above.
