# P2 package C — `p2/espryt` (Espryt 0b, the first Track H slice) — result v5 (rework round 5)

Tree `~/w7/p2-espryt`, branch `p2/espryt`, base tag `p2/contract` (`9c6a8a25`), HEAD **`0f29fa58`**.
Build directories: `build-linux` (pull, the G1 build), `build-push`, `build-verify`
(`-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`) and `build-nolegacy`
(`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`, untracked, not content).
`git status --porcelain` at the end of the round: only `?? build-nolegacy/`.

The round-4 **major** (nine files outside C.5's ownership) is closed by waiver: **ID-1** in
`~/w7/notes/p2/INTEGRATOR-DECISIONS.md` records the grant and **ID-2** moves espryt before
tracker and magma in the integration order. Those edits are untouched in this round and are not
re-argued here. This round closes the six round-4 minors the rework instruction named as real
defects — (a) the two-holder hazard, (b) the caller-less "backstop", (c) the call-site pin,
(d) the armless case's fixed path and leaked env / config, (e) the sentinel's arming site, and
(f) `HandleOf`'s negative memo — and re-runs the package's verification.

---

## 1. Commits

Two new on top of the thirteen from rounds 1–4 (15 total from `p2/contract`).

| sha | subject |
|---|---|
| **`6507cfae`** | `[Fix, Test] (Espryt): deliver a death notice to every holder of a kind, retire the twin table's last sweep, and pin the bring-up call site under the armless knob pair` — (a), (b), (c), (d), (e), (f) |
| **`0f29fa58`** | `[Test] (Espryt): pin that the armless cases put the operator's log path and MG_Config::Features back as they found them` — (d)'s own gate |

Files touched this round: `MG_Backend/DirectGLES/{SlotTables.h, Managers.h, Managers.cpp}` and
`MG_Test/SanityTest.cpp` — all inside C.2's file list. **No file outside C.2 + ID-1 was edited.**
`git diff --summary refs/tags/p2/contract..HEAD` prints only the two `create mode 100644` lines
(`SlotTables.h`, `StateObjectDeathNotice.h`) — **no mode change**.
`git log refs/tags/p2/contract..HEAD --format=%B | grep -ci 'co-authored\|claude'` → `0`.

---

## 2. The six minors, one by one

### (a) The two-holder hazard — FIXED (`6507cfae`)

**Restated.** `OnFrontendStateObjectDestroyed` told one registry per kind, and
`BackendSlotTable::DestroyByLifetimeId` returned `false` without freeing when *this* table did not
hold the slot. With `CollectGarbageIfNeeded` empty and `CollectGarbageNow` uncalled, any other
holder of the kind kept a live entry — and its twin's driver storage — for the life of the process.
The real second holder today is the `ScopedDirectGLESTextureBindings` fixture's saved copy of the
Texture registry. It is made worse, not better, by the allocator: `MGPipeSlotAllocator::Free`
**erases the `lifetimeId → slot` mapping** (`SlotAllocator.cpp:113-131`), so a notice delivered to
one holder and then resolved by the next would find nothing to resolve.

**The fix — one notice, every holder, one free, last.** `BackendSlotTable` links every instance
into a per-table-type intrusive doubly-linked holder list (constant-initialised head, two pointer
writes per link/unlink, no allocation; every constructor — default, copy, move — links, the
destructor unlinks, copy/move-assign carry entries and memo but never the links).
`OnFrontendObjectDestroyed(lifetimeId)` is **static**: it resolves the handle **once** through
`FindByLifetimeId`, calls the private `ReleaseTwinAt(handle)` on every holder (twin moved out of
the vector before its destructor runs, memo forgotten whenever it names that slot), and then calls
`Free` once. The slot is returned **whether or not any holder still had an entry** — the lifetime
id is dead and MG_State never re-issues one, so a slot that a `table = {}` reset had orphaned no
longer stays allocated for the life of the process (round-4 debt #2's test-only leak is gone too).
`StateBackendObjectRegistry::DestroyByLifetimeId` is static as well and forwards to it; the
dispatcher in `Managers.cpp:184-214` keeps its per-kind spelling and says in its comment that
naming one instance is a spelling, not a choice of holder.

**Magma and `VertexElementsCso` — a correction to the finding, stated rather than hidden.** The
rework instruction says the kind "is shared with magma's m3 inside P2". I checked package D's
tree: `grep -rn 'SlotAllocator\|MGPipeSlots' ~/w7/p2-magma/MobileGL/MG_Backend/DirectVulkan/` hits
only a *comment* (`MagmaPipeArms.h:170`, "nothing in P2 can call its Free"). Magma's
`MagmaPipeIdentityTable` mints out of its **own per-renderer allocator** with age-based
reclamation and never touches `MGPipeSlots()`. So the two backends share the *name* of the kind
and nothing else; there is no cross-backend holder to notify and no double-free ordering between
two consumers to design for. The holder list is per table type; each of the six kinds has exactly
one table type in this backend, and `SlotTables.h`'s header says so. The hazard the review named
is real for the fixture's copy (and for any future second table of a kind) and that is what is
fixed and tested.

**Tests.** `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` (kind `Fence`): two independent
tables plus a by-value copy of one hold the object, a fourth table holds a different object;
**one** notice while the object is still alive → all three `FindByHandle` null, both twins'
`weak_ptr`s expired, the kind's `LiveCount` down by exactly one, a second notice answers `false`,
the bystander untouched. `ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice` does it through
the **real** `TextureImpl::g_backendTextureObjects`, a copy taken exactly as the fixture takes it,
and the **real** `~TextureObjectBase` notice. `AWholeTableSavesResetsAndRestores` now also pins
`HolderCount()` across copy / reset / restore and that the saved copy drops the twin on the
object's death. **Negative control NC8** (§3.4): release only the first holder → exactly those
three cases fail, naming the holder.

### (b) The caller-less "backstop" — the claim and the code are DELETED (`6507cfae`)

`ReclaimDeadSlots()`, `CollectGarbageNow()`, `CollectGarbageIfNeeded()` and `m_isCollecting` leave
`BackendSlotTable`. `grep -rn 'ReclaimDeadSlots' MobileGL/` → no hits. The header no longer claims
a backstop; it says what is true: a notice dropped by `InProcessTeardown()` leaves a twin that is a
**deliberate leak** (the process is exiting, the driver reclaims the object, and a twin destructor
must not call into a driver that may already be unloaded), not garbage awaiting a collection. The
per-entry `weak_ptr` stays for exactly one reason — `ForEachLive()`'s strong reference to the
callee — and the comment says so. `StateBackendObjectRegistry::CollectGarbageNow` is pre-P2 API
(`p2/contract` `Managers.h:364`, no caller then either) and is kept for the legacy arm; on the
handle arm it returns. `CollectGarbageIfNeeded` on the handle arm is the predicted branch plus a
return with nothing to forward to.

### (c) The call-site pin — ADDED (`6507cfae`)

`EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping` runs the **real bring-up entry point**
under the pair. `InitDisplayAndContext` is file-static, but its two public callers are not;
`InitPbufferSurface(1, 1)` reaches it, and its twin-arm call is the first statement after
`DestroyEGLContext()`, *ahead of* `eglGetDisplay`. So in a forked death-test child the case sets
the knob pair, installs an EGL table whose `eglGetDisplay` answers `EGL_NO_DISPLAY`
(`SetEGLFuncsTable`), calls `InitPbufferSurface`, and `std::exit`s with a code that says "returned
`false`". No display, no GPU, no skip: the predicate is `ExitedWithCode(0x51)`. If the site is
edited back to `ResolveEsprytSlotTablesArm()` the child dies of `SIGABRT` and the case **fails**
with a message naming `MOBILEGL_PIPE_PUSH` / `kMGPipeSubsystemEsprytSlots` and
`MOBILEGL_PIPE_LEGACY_MEMOS=0` — and it additionally reads the child's log and asserts the
diagnosis line is there and no `Fatal{` is. It runs in all three push builds (in `build-nolegacy`
the verdict is `Handles` and bring-up must *still* return — it does not skip there) and skips only
in the pull build, where the twin table does not exist (G2 parity).

**Negative control NC10** (§3.4) applied exactly that edit at `DirectGLES.cpp:10238`: the case fails
("Terminated by signal 6", both knobs named, the `Fatal{PipeLegacyMemosDisabled …}` line quoted)
while, in the same build, `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest -L integration-gpu
-R CrossFrameBufferScenario` reports **`100% tests passed, 0 tests failed out of 13`, all
`***Skipped`** — i.e. the lane hides precisely what the pin catches.

### (d) The armless case's fixed path and leaked state — FIXED (`6507cfae`, gate in `0f29fa58`)

Two RAII guards in `SanityTest.cpp`'s anonymous namespace: `ScopedLogFileRedirect` (closes the log,
saves the operator's `MOBILEGL_LOG_FILE_PATH` — value or absence —, sets the redirect; on
destruction closes the log again, puts the previous value back or unsets, removes the file) and
`ScopedArmlessKnobPair` (saves and restores `Features.PipePush` / `PipeLegacyMemos`).
`UniqueScratchLogPath()` builds `<tmp>/<stem>-<pid>-<counter>-<steady ticks>.log`, so two
`SanityTest` processes on one host and two cases in one process cannot collide.
`AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane` uses both guards; its `ASSERT_EQ(…, NoArm)`
now sits *after* the knob guard is constructed, so a failure there restores too.
`TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem` (`0f29fa58`) pins the restore byte for
byte with a pre-set operator path and with none. `ls /tmp | grep -c mobilegl-espryt` after the
whole binary → `0`.

### (e) `EnsureProcessTeardownSentinel` — MOVED to the slot table's first insertion (`6507cfae`)

`BackendSlotTable::GetOrCreate` arms it on the non-null path before `Acquire`; on the handle arm
`StateBackendObjectRegistry::GetOrCreate` dispatches to the table *before* its own arming call, so
the registry arms only on the legacy arm. D13's sentence is now literally true. Round-4 §6.9's
objection — "a `BackendSlotTable` used outside the registry would arm it" — was a reason not to
bother, not a reason it is wrong: the atexit handler only latches `g_processTeardown`, which no
unit binary reads to any effect. **Pull-build codegen is unchanged** (the dispatch is under
`#if MOBILEGL_PIPE_PUSH`; in the pull build the sequence is still assert → sentinel → map), and
G1 confirms it: contract → HEAD `0 added, 0 removed, 0 resized, 0 renamed`, `.text +0`.
`SlotTables.h` re-declares the function because it is included from `Managers.h` above line 61.

### (f) `HandleOf`'s negative memo — REMOVED (`6507cfae`)

`if (!MGPipeHandleIsNull(handle)) RememberHandle(…)`. The reason is the one the review gave: the
memo is per table, the allocator per kind, so once another holder can be the one that acquires, a
cached "no handle" here would outlive the twin's creation over there and nothing on *this* table's
acquire path would ever refresh it. `ReleaseTwinAt` also forgets the memo whenever it names the
freed slot, *before* checking whether this table has an entry there, so a positive memo learned
from the allocator for another holder's twin cannot survive the slot's next handout either.
`RepeatedLookupsOfALiveObjectKeepOneHandle` lost the sentence that pinned the old behaviour;
`ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire` is the new gate (first table asks and
misses, second table acquires, first table must now answer the same non-null handle).
**Negative control NC9** (§3.4): memoise unconditionally again → exactly that case fails.

---

## 3. Verification — every command and what it printed

All from `~/w7/p2-espryt` at `0f29fa58` (the test-only second commit; the library gates were run at
`6507cfae` and the pull library is byte-identical between the two — §3.1 last row),
`CCACHE_BASEDIR=/home/swung/w7`, with `MOBILEGL_BACKEND_TYPE=DirectGLES` and
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json` for the GPU lanes.
Summary of the long lane: `~/w7/p2r5-espryt-summary.txt`.

### 3.1 Build and static gates

| gate | command | result |
|---|---|---|
| build | `cmake --build {build-linux,build-verify,build-nolegacy,build-push} -j 12` | all `rc=0`, 0 warnings in the push build log |
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`, `.text +160` — the four are ID-3's admitted set |
| **G1 attribution** | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed`, `.text 10792739 -> 10792739 (+0)` — re-run after `0f29fa58`: identical |
| **G5** | `git show {48268068, refs/tags/p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48716056c5…`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty, `rc=1` |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, `rc=0` |
| **G13** | `gen_pipe.py --check` / `--self-test` | `rc=0` / `7 negative-control trip(s), positive control OK` |
| **G13 stdio** | the `pipe-gates` alternation (`test.yml:1515-1522`) over `MG_Backend` + `MG_State` | **0 hits** |
| C.2 grep | `grep -n 'OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|CollectGarbageIfNeeded' DirectGLES.cpp` | 37 hits, unchanged from v4 (`DirectGLES.cpp` is not touched this round); all inside `MOBILEGL_PIPE_LEGACY_MEMOS` arms as verified in rounds 3–4 |
| **G2** | ID-4's `ctest -N` extraction × 4, `LC_ALL=C sort`, `diff` | `build-linux` **2385**, `build-push` **2385**, `build-nolegacy` **2385** — both diffs IDENTICAL. `build-verify` 3203 (its verify-only lanes) |
| **G14** | `LC_ALL=C comm -23 <sorted ~/w7/p2-before-ctest-names.txt> <pull names>` | **0** removed. Added 22: the contract's 4 placeholders + **18** `DirectGLESSlotTable.*` |

### 3.2 Tests

| lane | command | result |
|---|---|---|
| unit × 4 | `ctest --test-dir <d> -L unit --no-tests=error -j 6` | `100% tests passed, 0 failed out of 1507` on **build-linux, build-push, build-verify, build-nolegacy** |
| slot × 4 | `ctest --test-dir <d> -R DirectGLESSlotTable --no-tests=error` | `18/18` in all four; 0 skipped in `build-push` / `build-verify`; **2** skipped in `build-nolegacy` (`AnArmlessKnobCombination…` only — the bring-up pin runs there); all 18 present and skipping in `build-linux` |
| sanity, handle arm | `./build-push/MobileGL/MG_Test/SanityTest` | `[ PASSED ] 100 tests` |
| sanity, legacy arm | `MOBILEGL_PIPE_PUSH=0 …/SanityTest` | `[ PASSED ] 96`, 4 skipped (the four that are about the handle arm, incl. the new `ASavedCopyOfARealRegistry…`) |
| sanity, order independence | `--gtest_shuffle --gtest_random_seed={1,2,3}` | `100/100` × 3 |
| armless hygiene | whole binary, then `ls /tmp \| grep -c mobilegl-espryt` | `0` |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **446/446**, 100 (Skipped) |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | **446/446**, the same 100 skips |
| integration, **pull build** | `ctest --test-dir build-linux -L integration-gpu -j 4 -R DirectGLES` | first run **444/446** (see §5.1), rerun of the two alone `2/2`, full lane rerun **446/446** |
| integration, no legacy compiled | `ctest --test-dir build-nolegacy …` | **446/446** |
| integration, verify build | `ctest --test-dir build-verify -L integration-gpu -j 4 -R DirectGLES` | **855/855** |
| **G4 (a)** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **818/818**; `grep -c 'Fatal{'` over the 10 lane logs = `0` each |
| **armless red proof** | `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario'` | **`0% tests passed, 13 tests failed out of 13`**, 13 × `Subprocess aborted` (log `~/w7/p2r5-espryt-armless.log`) |
| **G3** (DirectGLES half) | `retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/v5-espryt -j 4 --only 'DirectGLES$'` | `rc=0`, **`passed 39 / 39; failed: []`** |
| **G4 (b)** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … --lib $PWD/build-verify/libMobileGL.so --out ~/w7/retrace-out/v5-espryt-verify …` | `rc=0`, **39/39**; of the 39 `*/output/mobilegl.log`: **0** `Fatal{`, **0** `PipeVerifyDiffer\|UnmigratedPipeInput\|PipeResidualDiverged`, **0** unarmed (`grep -L 'MGPipe: verify armed'`) |

The two retrace output trees (1.4 GB each) were deleted after the counts above were taken.

### 3.3 Negative controls — each new gate goes red for the reason it exists

Each perturbation applied alone, built, run, restored from a `/home/swung/w7` copy (no `git stash`),
then a clean rebuild and `18/18`; `git status --porcelain` afterwards only `?? build-nolegacy/`.
Log: `~/w7/p2r5-espryt-nc.log`.

| # | perturbation | result |
|---|---|---|
| NC8 | `OnFrontendObjectDestroyed` releases only the first holder (`holder = nullptr` after one) | **only** `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` ("holder a kept the twin", "holder b kept the twin"), `AWholeTableSavesResetsAndRestores` and `ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice` fail; 14 pass |
| NC9 | `HandleOf` memoises unconditionally again | **only** `ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire` fails ("the first holder kept answering the null handle it cached before the second holder acquired"); 16 pass |
| NC10 | `DirectGLES.cpp:10238` → `(void)ResolveEsprytSlotTablesArm();` | **only** `EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping` fails: `Result: died but not with expected exit code: Terminated by signal 6`, the message names both knobs, and the log quote is the `Fatal{PipeLegacyMemosDisabled, …}` line. In the same build the integration lane under the pair prints `100% tests passed, 0 tests failed out of 13`, all `***Skipped` |

(NC5–NC7 from v4 were not re-run; the classifier, the per-kind walk and the destructor notices are
unchanged this round except that the walk's death step now goes through the static notice.)

### 3.4 Gates this package cannot reach from its own tree

Unchanged from v4 §3.5: G6/G7 (package A's spans, E's `g7_negative_control.sh`), G8
(`HandleRecycleScenario`, E), G9 (`gen_pipe_dirty_surface.py --check`), G10, G11, G12, G14's
CI-matrix half. DriverBench and the two-device runs (D.4.2/D.4.3) remain owed; this round's
per-draw delta is a removal (the registry's handle-arm `CollectGarbageIfNeeded` no longer makes
a call at all) plus one `MGPipeHandleIsNull` test on `HandleOf`'s miss path.

---

## 4. Deviations from the brief and from the rework instruction

1. **ID-1 / ID-2** — the six destructor sites stay in the B/D-owned files under the recorded
   waiver; not re-argued.
2. **The "shared with magma's m3" premise of (a) is not what D's tree does** (§2(a)): Magma
   never calls `MGPipeSlots()`. The fix is made and tested for the holders that do exist (the
   fixture's copy, any future second table of a kind); no cross-backend consumer ordering was
   built because there is nothing to order.
3. **(c) is a unit-binary case, not a ctest lane entry.** C.2 owns no CMake and E owns
   `MG_IntegrationTest/**`, so the "lane-level" check is a `SanityTest` case that runs the real
   bring-up entry point under the pair in a forked child. It never skips in a push build and it
   cannot skip on the pair by construction (no GPU is needed); it skips only in the pull build,
   where the table does not exist, for G2 parity.
4. **(e)** moved as D13 says; the objection recorded in v4 §6.9 is withdrawn (§2(e)).
5. Carried unchanged from v4 §4: 4.2 (first-use Fatal instead of startup Fatal), 4.3 (GC deleted
   on the handle arm only), 4.4 (e2-before-e3 satisfied in content, ordering deviation on
   squash-merge), 4.5.

## 5. Where the tree contradicts the brief, or is flaky

### 5.1 **NEW** — two E-owned integration lanes are flaky under `-j 4` in the pull build

First full pull-lane run: `DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn`
and `DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`
`***Failed` (`~/w7/p2r5-espryt-it-linux.log`); both pass alone and the full rerun is 446/446
(`~/w7/p2r5-espryt-it-linux2.log`). Both passed in all four other lane runs this round. The pull
library is byte-identical to the contract's (G1), so this is not package C's code. Likely cause,
for E: `MG_IntegrationTest/CMakeLists.txt:368-387` gives each of those lanes **one fixed**
`MOBILEGL_LOG_FILE_PATH` (`unlocated-io-blocks.log`, `point-size-demotion-gles.log`), the
arming assertion reads that file, and under `-j 4` the lane's other cases open the same file with
`"w"` in parallel — the same fixed-path collision shape as this package's review minor 5.

### 5.2 Carried from v4 §5

`retrace_gate.py` has no `--backend` / `--ssim` (ID-4 records it); G4's `MGPipe verify:` grep
and `<out>/*/mobilegl.log` glob are vacuous as written (ID-4 records the arming line; the path is
`<out>/<case>/<backend>/output/mobilegl.log`); `_GLOBAL__sub_I_DirectGLES.cpp` is in ID-3's
admitted set; section A's `^\s+Test #` drops four-digit ids (ID-4); `git stash -u` destroys the
copied fixtures; `p2/contract` is an ambiguous refname here (the tag wins).

---

## 6. Unfinished, and debts carried

1. **The death-notice ops table is still a single global pointer**
   (`StateObjectDeathNotice.h`). No second consumer exists in P2 (§2(a): Magma has its own
   allocator and no notice), so this is a P3 precondition, not a P2 defect; the fix is a small
   fixed-size consumer list. Still the top cross-package item.
2. **Slot reuse between a table's reset and restore** (v4 debt #2) is now moot in its leaking
   form — a dead object's slot is returned whether or not a holder has an entry — but the
   *semantic* hazard (a restore reinstating an entry for a slot the allocator re-handed while it
   was reset) is still only avoided because nothing acquires between the fixture's reset and
   restore. A refcounted ownership token is still the right shape if that ever changes.
3. **Debt #3 (two holders) is closed** by the holder list; what remains is that the list is per
   table *type*, so a second type of the same kind in this backend would need to join it.
4. **The backend still mints client handles** (`SlotTables.h` DEBT block; P3+, belongs in
   `MEASUREMENTS.md`).
5. **The one-entry resolution memo** is the wrong shape for `BindCurrentFBO` and
   `ResolveUnitSamplerBackend` (per-unit / per-target memo is the fix; G11 prices it).
6. **Device measurement** (D.4.2 / D.4.3, G11) and **DriverBench** are owed.
7. **G6–G12** not exercisable from this tree.
8. **No `PipeStats` counter** for this subsystem (package A's file).
9. Round-4 minors left open on purpose: minor 3 (`build-nolegacy` still instantiates the legacy
   `CollectGarbage()` — nothing ticks it; the sentence in v4 was wrong, the mechanism is not),
   minor 6 (the D13 "must not break" pins make no acquisitions; coverage lives in the per-kind
   walk), minor 7 (the populated fixture save/reset/restore is covered by `AWholeTableSavesResetsAndRestores`
   on a fake table, now including the death path, not by the fixture itself), minor 10 (the
   registry still asserts non-null ahead of the arm dispatch), minor 11 (the out-of-line
   destructors are push-only), minor 13 (`kMGPipeSubsystemsMigratedAtP2` duplicated in the test
   environment), minor 14 (device measurement).

## 7. Logs kept under `~/w7` (everything else of rounds 1–5 deleted)

`p2r5-espryt-summary.txt` (the long lane, verbatim), `p2r5-espryt-nc.log` (NC8–NC10 builds),
`p2r5-espryt-armless.log` (the red proof), `p2r5-espryt-it-linux.log` / `p2r5-espryt-it-linux2.log`
(§5.1). Deleted this round: `armless.log`, all `e2-*` and `e3-*`, `p2-espryt-*.log`,
`p2-espryt-verify/`, `rev-espryt/`, both `retrace-out/v5-espryt*` trees, and every other
`p2r5-*` file this package wrote. `rt-restore2-build.log` (487 bytes, 2026-09-06, a test-target
build log of uncertain origin) was left in place. Other packages' `p2r5-*` files were not touched.
