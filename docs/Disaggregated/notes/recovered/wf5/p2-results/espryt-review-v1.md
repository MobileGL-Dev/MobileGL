# Adversarial review — package C `p2/espryt` (Espryt 0b, first Track H slice) — v1

Reviewer ran everything below independently in `~/w7/p2-espryt` (branch `p2/espryt`, HEAD
`d714600a`, base tag `p2/contract` = `9c6a8a25`). Nothing in the tree was modified; the two
existing build dirs were rebuilt incrementally (`ninja: no work to do` on both).

**Verdict: NOT APPROVED — 4 majors.**

---

## 0. What I re-ran, and what it actually printed

| gate / claim | my command | my observed result | agrees with `espryt-v1.md`? |
|---|---|---|---|
| build | `cmake --build build-linux -j 12`, `build-push` | `ninja: no work to do`, rc 0 both | yes |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `0 added, 0 removed, 4 resized, 0 renamed`; resized = `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 | yes |
| **G1 attribution** (I did not take this on trust) | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after ~/w7/p2-contract/build-linux/libMobileGL.so --threshold 0` then `--before ~/w7/p2-contract/build-linux/libMobileGL.so --after build-linux/libMobileGL.so` | contract→before: the **same 4** resized. contract→espryt: `0 added, 0 removed, 0 resized, 0 renamed`, `.text +0`. **Package C's pull-build delta is exactly zero.** | yes, and §5.1's attribution is correct |
| **G2** | `for d in build-linux build-push; do ctest --test-dir $d -N \| sed -n 's/^ *Test  *#[0-9]*: //p' \| sort; done; diff` | 2372 names each, `diff` empty | yes |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | empty. Added: 4 contract placeholders + 5 `DirectGLESSlotTable.*` | yes |
| **G5** | `git show {48268068,p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…0efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` | yes |
| **G13** | `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty | yes |
| **G13** | `python3 scripts/check_include_closure.py` | rc 0, `4 probes, 0 skipped, 0 problem(s)` | yes |
| **G13** | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 | yes |
| G13 stdio | grep for `printf/fprintf/cout/cerr/puts` in the 4 touched backend files | empty | yes |
| unit (pull) | `ctest --test-dir build-linux -L unit -j 8` | `100% tests passed … out of 1494` | yes |
| unit (push) | `ctest --test-dir build-push -L unit -j 8` | `100% tests passed … out of 1494` | yes |
| slot-table cases (push) | `ctest --test-dir build-push -R DirectGLESSlotTable` | `5/5 Passed` | yes |
| slot-table cases (pull) | same on `build-linux -V` | all five present and `[  SKIPPED ]` with a message — a visible skip, as C.4 wants | yes |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | `100% tests passed, 0 failed out of 446` | yes |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | `100% tests passed, 0 failed out of 446` | yes |
| dirty-surface gate (G9) | `gen_pipe_dirty_surface.py --check` | **rc 2** — package B owns it and has not landed; not this package's | n/a |
| G6/G7/G8/G10 | — | **unreachable from this tree** (no `build-verify`; the tests/scripts are A/B/E's). The package's green is partial by construction. | consistent with §6 |

**The handle arm really is the arm that runs — I proved it rather than inferring it.** Under gdb on
`build-push`, with the ctest environment (`MOBILEGL_BACKEND_TYPE=DirectGLES`,
`__EGL_VENDOR_LIBRARY_FILENAMES=…50_mesa.json`) and no `MOBILEGL_PIPE_PUSH` override, running
`CrossFrameBufferScenario.VertexBufferSubData`:

```
Breakpoint 1 … EsprytSlotTablesEnabled()            breakpoint already hit 92 times
Breakpoint 2 … MGPipeSlotAllocator::Acquire         breakpoint already hit 5 times
Breakpoint 3 … MGPipeSlotAllocator::Allocate        breakpoint already hit 5 times
Breakpoint 4 … MGPipeSlotAllocator::Free            (0 hits)
$1 = 1        # the latched return value of EsprytSlotTablesEnabled()
```

So "a gate green because it never ran" does **not** apply to the basic path. It does apply to the
sweep (`Free` 0 hits) — see MAJOR 1.

The ctest-name extraction complaint (§5.2) is confirmed: `ctest -N | grep -E '^\s+Test #'` yields
**1373** of **2372** names on this tree; the `sed -n 's/^ *Test  *#[0-9]*: //p'` form yields 2372.
The brief's §A (G2, G14) and §D.1/D.3 commands must be fixed by the integrator.

`retrace_gate.py --only 'DirectGLES$'` under push was re-launched by me and is the one claim I
could not finish inside this review window; the package's own run reported 39/39 with a minimum
SSIM of 0.997009. The integrator should treat G3 as re-verified only when that run completes.

---

## MAJORS

### MAJOR 1 — the churn-driven garbage sweep is silently dropped on the handle arm, so twin memory is *worse* than before P2, not "unchanged"

`StateBackendObjectRegistry::GetOrCreate` has, and has always had, **two** sweep drivers. The
handle arm takes only the first.

`MobileGL/MG_Backend/DirectGLES/Managers.h:326-330` (the new dispatch):

```cpp
            EnsureProcessTeardownSentinel();
#if MOBILEGL_PIPE_PUSH
            if (EsprytSlotTablesEnabled()) {
                return m_slotTable.GetOrCreate(stateObj);      // <-- returns here
            }
#endif
```

Everything below that `return` is the legacy arm's **creation-driven** sweep, at
`Managers.h:335-353`:

```cpp
            if (m_creationTick >= kCreationGCInterval) {   // kCreationGCInterval = 64
                m_creationTick = 0;
                CollectGarbage();
            }
            …
            if (m_entries.size() != entryCountBeforeInsert) {
                // "Nothing tells the backend that a texture or renderbuffer was DELETED … and
                //  CollectGarbageIfNeeded is ticked only from the per-draw sync paths, which a
                //  CTS-shaped workload runs about ten times per case. 1024 of those ticks then
                //  span ~100 cases, so ~100 cases' worth of dead (and, for this suite,
                //  gigabyte-sized) objects stay allocated at once. Object CHURN rather than draw
                //  count is what makes the sweep urgent …"
                ++m_creationTick;
            }
```

`BackendSlotTable` has no equivalent. Reproduce:

```
$ grep -n 'm_creationTick\|kCreationGCInterval' \
    MobileGL/MG_Backend/DirectGLES/Managers.h MobileGL/MG_Backend/DirectGLES/SlotTables.h
Managers.h:335:            if (m_creationTick >= kCreationGCInterval) {
Managers.h:336:                m_creationTick = 0;
Managers.h:351:                ++m_creationTick;
Managers.h:492:        static constexpr Uint32 kCreationGCInterval = 64;
Managers.h:495:        Uint32 m_creationTick = 0;
                     # SlotTables.h: no hits
```

`SlotTables.h:141-148` is the *only* sweep driver on the handle arm and it is the 1024-tick,
draw-path one (`kGCInterval = 1024`, `SlotTables.h:179`), fed exclusively by the seven surviving
`CollectGarbageIfNeeded()` call sites (`DirectGLES.cpp:1264, 1651, 2016, 2017, 2018, 2846, 2847`).

**Failure scenario, concrete.** The CTS-shaped workload `Managers.h:341-353` describes: ~10
per-draw ticks per case, a handful of gigabyte-sized textures created and deleted per case. On the
legacy arm the 64-creation tick sweeps roughly every 10 cases. On the handle arm nothing sweeps
until 1024 per-draw ticks have accumulated, i.e. ~100 cases — exactly the regression the creation
tick was added to fix. The result file §4.1 point 2 asserts the opposite: *"the ~100 CTS cases'
worth of dead gigabyte-sized objects stay allocated at once is **unchanged**"*. It is not
unchanged; on the arm this package ships by default it is reinstated. Combined with MAJOR 3 (no
explicit destroy), the package's net effect on twin lifetime is **strictly negative** relative to
the base ref.

No test covers sweep cadence: the five `DirectGLESSlotTable.*` cases only ever call
`CollectGarbageNow()` (`SanityTest.cpp:3173, 3235`), never `CollectGarbageIfNeeded()`.

### MAJOR 2 — `Fatal{PipeLegacyMemosDisabled}` does not fail; the arm the operator disabled runs anyway

`Managers.cpp:482-493`:

```cpp
        static const Bool enabled = [] {
            const Bool bitSet = (MG_Config::Features.PipePush & MG_Pipe::kMGPipeSubsystemEsprytSlots) != 0;
#if MOBILEGL_PIPE_LEGACY_MEMOS
            if (!bitSet && !MG_Config::Features.PipeLegacyMemos) {
                MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled, …}");
            }
            return bitSet;      // == false -> the LEGACY arm is entered
```

`MGLOG_F` is a log macro (`MG_Util/Debug/Log.h:88`), not a trap. The codebase's own convention for
a `Fatal{…}` is `MGLOG_F(...)` immediately followed by `std::abort()` —
`MG_Impl/Pipe/PipeFill.cpp:288-291` (`[[noreturn]] BadKnob`), and `PipeFill.cpp:405-409` aborts on
`g_verify.Fatal`. Here there is no abort, and control falls straight into the
`#if MOBILEGL_PIPE_LEGACY_MEMOS` arm that D14 says *"the legacy arm is never entered"*.

Reproduced:

```
$ cd ~/w7/p2-espryt/build-push/MobileGL/MG_IntegrationTest
$ MOBILEGL_BACKEND_TYPE=DirectGLES __EGL_VENDOR_LIBRARY_FILENAMES=…/50_mesa.json \
  MOBILEGL_LOG_FILE_PATH=/tmp/rev-noarm.log \
  MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
  ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData
exit=0
$ grep -n PipeLegacyMemosDisabled /tmp/rev-noarm.log
317:[…/FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, "kMGPipeSubsystemEsprytSlots is clear but MOBILEGL_PIPE_LEGACY_MEMOS=0"}
$ wc -l /tmp/rev-noarm.log   -> 319     # the run continues to a clean teardown after the "Fatal"
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest --test-dir build-push \
    -L integration-gpu -j 4 -R 'DirectGLES.CrossFrameBuffer'
100% tests passed, 0 tests failed out of 13
```

Two things are wrong, not one:

1. It is not a **startup** check. It is latched on the *first twin lookup* — line 317 of a 319-line
   log, i.e. deep inside the first draw. A short-lived process that never twins anything never
   learns.
2. It does not stop anything. An operator who sets `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0`
   believing they have disabled both arms gets a green run measured on the legacy arm. This is
   precisely the "a suppressor that serves a stale/wrong answer" class, and it defeats the lever
   D18's `HandleRecycleScenario` arms are built on: package E is told to drive `.Handles` with
   `MOBILEGL_PIPE_LEGACY_MEMOS=0`, and the only thing that would tell E its arm selection is wrong
   is this check.

Fix is one line (`std::abort();` after the `MGLOG_F`, or hoist the check to config load).

### MAJOR 3 — step `e2` is absent, so `e3` landed under a condition the brief forbids, and the D13 census payment is not made

C.2 step `e2` ("the explicit-destroy hook … **This must land before the tables switch over**") and
the §E risk row (*"C's `e2` lands the explicit `delete_*` notification **before** `e3` switches the
tables over, and **`e3` is not merged without it**"*) are unambiguous. `e3` is merged
(`fbdaeef3`) and `e2` is not present:

```
$ grep -rn 'OnDestroy' MobileGL/MG_State/GLState/ | grep -v BufferState
   (no hits — only BufferObject.h has one)
```

Consequences that the integrator must not inherit silently:

- the seven `CollectGarbageIfNeeded` call sites D13 lists for deletion all survive
  (`DirectGLES.cpp:1264,1651,2016,2017,2018,2846,2847`) — verified by grep;
- `ARCHITECTURE.md:363`'s census is not paid: D.4.6 scores P2 as "11 of the 11 direct deletions",
  and the six registries' GC is one of them;
- with MAJOR 1, the memory half of the deliverable is a net regression rather than a no-op.

I accept that the implementer was boxed in: C.2's own **Files** line does not list any `MG_State`
file, and C.5 gives every one of them to package B / D, and the standing instruction is to record
a blocking deviation rather than cross an ownership line. §4.1 records it correctly and honestly.
It is still a major, because the package does not meet its section-C scope and because the brief
states the merge precondition explicitly. **This is an integrator decision (re-assign `e2`, or
re-order B before C), not something the implementer should have forced.**

### MAJOR 4 — the three deleted per-draw memos are replaced by a *hash probe*, which is the thing they existed to avoid; nothing measured it, on the one metric G11 forbids regressing

D13's replacement column reads, verbatim: *"`OwnerEquals` and the three `TwinLookupMemo`s → direct
slot indexing — the memo existed only to avoid the hash probe"*. What landed is not direct slot
indexing. `BackendSlotTable::Find` (`SlotTables.h:100-103`) is

```cpp
        BackendPtr* Find(StateObject* stateObj) {
            if (stateObj == nullptr) return nullptr;
            return FindByHandle(HandleOf(stateObj));      // HandleOf = a hash probe
        }
```

and `HandleOf` (`SlotTables.h:119-122`) is `MGPipeSlots().FindByLifetimeId(kKind, …)`, which is
`state.ByLifetimeId.find(lifetimeId)` — a hash-map lookup (`SlotAllocator.cpp:97-105`). So on the
three hottest resolution paths — `ResolveVaoTwin` (`DirectGLES.cpp:1235-1240`), `SyncCurrentProgram`
(`:2890-2896`), `BindCurrentFBO` (`:3010-3013`) — the steady-state cost went from *array probe +
owner compare* to *out-of-line call + hash probe + array index*, and the memos that made the fast
path an array probe were deleted in the same commit. The result file admits this in §4.2 and then
declines to make any cost claim.

On top of the hash, `EsprytSlotTablesEnabled()` is an out-of-line, non-inlinable function
(`Managers.cpp:478`, no `inline`, defined in a different TU from every caller, no LTO in this
build) consulted on **every** `Find`/`GetOrCreate`/`HandleOf`/`ForEachLive`/`CollectGarbage*`.
Measured above: **92 calls in a single 20 ms integration scenario**.

Why this is a major and not a "risk noted":

- G11 is a hard gate — *"per-thread CPU p50 and p99 deltas are **not negative** on either device"* —
  and D.4.6 scores this very slice's unit cost. A package that deletes three per-draw memos and
  installs a hash probe in their place is the single most likely source of a negative delta in P2.
- The evidence was cheaply available and was not produced. `DriverBench` is desktop-only and needs
  no device (D.4.3: `run_driver_bench.sh` on `mc_vanilla_draw` / `mc_state_toggle`, pull vs push,
  T1/T2). The tree has no `build-bench` at all (`ls ~/w7/p2-espryt` → only `build-linux`,
  `build-push`), so no local cost datum of any kind exists for this slice.
- The claimed remedy ("under split the handle is what the client already holds and the hash
  disappears") is P3+ work; it does not help the P2 GO/NO-GO, which is what the number is for.

At minimum this package owes a desktop T1/T2 run before D.4; if the number is negative the
mitigation is a per-object cached handle on the frontend object (which is what D12.5 does for
Magma's VAO) or restoring the memo keyed on `{slot, gen}`.

---

## MINORS

1. **`DirectGLESSlotTable.TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` cannot fail.**
   `SanityTest.cpp:3269-3279` asserts `a.HandleOf(o) == b.HandleOf(o)`, but `HandleOf`
   (`SlotTables.h:119-122`) never reads `m_slots` — it is a pure function of `(kKind,
   lifetimeId)`. Two tables must agree for *any* table implementation. The result file §2 leans on
   it (*"the two Track H slices agree by construction, and … pins it"*); it pins nothing about
   Magma's table, which is a different type and is not in the test.
2. **`GetOrCreate(nullptr)` is a null deref on the handle arm in release.** `SlotTables.h:77-80`
   calls `stateObj->GetLifetimeId()` after a `MOBILEGL_ASSERT` that is compiled out; the legacy arm
   inserted a null key instead. `DirectGLES.cpp:7345-7348` explicitly documents that the legacy
   path tolerated it (*"SyncTextureObjectToBackend would register a null state object"*). Callers
   guard today, so this is hardening, not a live bug.
3. **Latent UAF in `ReclaimDeadSlots`.** `SlotTables.h:129-137` holds `Entry& entry =
   m_slots[slot]` across `entry.backend.reset()` and then writes `entry.stateRef`/`entry.Live`. If
   a twin destructor ever re-entered `GetOrCreate` on the same table, `EntryAt`'s `resize`
   (`:174-177`) would move the vector and the three writes would land in freed memory.
   `m_isCollecting` guards re-entrant *sweeps* only. The legacy `CollectGarbage`
   (`Managers.h:499-520`) collected keys into a vector first and was immune to this shape.
4. **Nothing ever returns a table's slots to the allocator on destruction or reset.**
   `ScopedDirectGLESTextureBindings` (`SanityTest.cpp:154-197`) does
   `g_backendTextureObjects = {}` and later restores; the allocator's `Slots`/`ByLifetimeId` rows
   for everything created inside the fixture stay `Live` for the life of the process. Leak only,
   but it means `MGPipeSlotAllocator::HighWater`/`LiveCount` are not usable as a health signal from
   a test process.
5. **The backend now mints client handles.** `SlotTables.h:15` adds
   `#include <MG_Impl/Pipe/SlotAllocator.h>` under `MG_Backend/` and `:79-80` calls
   `MG_Pipe::MGPipeSlots().Acquire(...)`. `MGPipeHandles.h:13-16` states the contract it breaks:
   *"A handle is a {slot, gen} pair minted by the CLIENT and never by the server"*. It also takes a
   frontend `SharedPtr` and calls `GetLifetimeId()`, neither of which exists on the server side of
   a split. `check_include_closure.py` does not probe `MG_Backend` headers so nothing catches it,
   and `MG_Backend/DirectGLES/DirectGLES.cpp:22` already had one `MG_Impl` include, so this is the
   second. Fine as monolith glue; it must be recorded as a P3+ debt, not as "Track H done".
6. **`MOBILEGL_PIPE_PUSH=0` is no longer the faithful P1 control it is documented to be.**
   `ConfigLoader.cpp:251-254` says *"`MOBILEGL_PIPE_PUSH=0` in the environment is the
   all-subsystems-pull control that reproduces P1 exactly"*, but `g_fbSlotCache` /
   `GetFramebufferBindingSlotChecked` is gated on the **compile-time** `MOBILEGL_PIPE_PUSH`
   (`DirectGLES.cpp:151-153`, `:167-177`), not on `kMGPipeSubsystemEsprytSlots`. In a push build the
   five call sites lose the cached slot pointer on **both** arms. This is what e4 asks for, but the
   ConfigLoader/D14 wording is now wrong and the integrator's `MOBILEGL_PIPE_PUSH=0` A/B is not a
   P1 baseline for this path.
7. **`MGB_TWIN_KIND_PARAM` / `MGB_TWIN_KIND_ARG`** are `#define`d at `Managers.h:601-607` (post-diff
   numbering; `Managers.h` near the class) and never `#undef`ed, so they leak into every TU that
   includes `Managers.h`.
8. **Shadowing at `DirectGLES.cpp:3010`**: `auto* slot = …Find(currentFBO.get())` shadows the
   enclosing `auto& slot = GetFramebufferBindingSlotChecked(target)` at `:2990`. Compiles, reads
   badly in a function whose whole subject is which "slot" is meant.
9. **The `build-nolegacy` claim in §3 is unverifiable.** `ls ~/w7/p2-espryt` shows only
   `build-linux` and `build-push`; the directory the row cites does not exist, so
   "`MOBILEGL_PIPE_PUSH=ON` + `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` … unit 1494/1494; integration
   446/446" cannot be re-checked. (Reading the code, that configuration is coherent:
   `EsprytSlotTablesEnabled()` returns an unconditional `true` there, so the `#else return
   nullptr;` at `DirectGLES.cpp:1254` and the empty `else` blocks in `SyncCurrentProgram` /
   `BindCurrentFBO` are unreachable — but that is an argument, not a run.)
10. **§5.3's cold-cache flake could not be reproduced** in the current (warm) state: two full
    `-L integration-gpu -R DirectGLES -j 4` runs, handle arm and legacy arm, were 446/446. The
    claim that it also happens on `~/w7/pipe` is unverified here. It remains a real risk for D.3 on
    a clean CI runner and the integrator must settle it before the five-part gate.
11. **§4.1 point 2 is factually wrong** ("unchanged") — see MAJOR 1. Whatever the integrator
    decides about `e2`, this sentence must not survive into `MEASUREMENTS.md`.
12. **No `build-verify` on this tree**, so `MOBILEGL_PIPE_VERIFY`'s compare-at-read has never been
    run against the handle arm — and `GetFramebufferBindingSlotChecked` is exactly the change that
    should make the verify read-hook fire at five previously bypassed sites. §6.4 says so; it is
    the single highest-value follow-up and should be run before, not after, the tracker lands.

---

## Tree-vs-brief disagreements I confirmed (facts; the tree wins)

- **§5.2 is right.** `ctest -N | grep -E '^\s+Test #'` returns **1373** of **2372** names on this
  tree because a four-digit test count pads to `  Test   #83:`. `~/w7/p2-before-ctest-names.txt`
  (2363) was captured with a working extraction, so the brief's own G2/G14/D.1/D.3 command would
  report ~1000 spurious removals. Fix section A, D.1 and D.3 to
  `sed -n 's/^ *Test  *#[0-9]*: //p'`.
- **§5.1 is right and I re-derived it.** `_GLOBAL__sub_I_DirectGLES.cpp` −9 is present at
  `p2/contract` with zero package-C changes; contract→espryt is a byte-identical `.text`. G1's
  admitted set (D15 point 4, C.0's verification block) must be widened by that symbol, and the
  attribution belongs to package A's D3.1, not to C.
- **§4.6 is right.** `~/w7/retrace_gate.py` takes `--tree --lib --out -j --only`; it has no
  `--backend` and no `--ssim`. C.2's and C.3's verification blocks and G3's row in section A spell
  flags that do not exist.
- **C.2's Files line contradicts C.2 step e2 and C.5.** e2 cannot be implemented from the file set
  C.2 grants package C; every `MG_State/GLState/*State/*` file it needs belongs to B (or, for
  `VertexArrayObject.h`, to D). This is the root cause of MAJOR 3 and the brief must be repaired,
  not the package.
- **D13's "delete `SyncTextureObjectToBackend`'s by-value copy" is wrong and the package was right
  to ignore it.** The copy at `DirectGLES.cpp:1365` is what keeps the twin alive across the nested
  `SyncTextureViewToBackend`, and on the handle arm a nested `GetOrCreate` can still `resize`
  `m_slots` and move the element (`SlotTables.h:174-177`). Deleting it would have been a UAF. The
  package kept it and re-resolves at the tail (`:1387-1405`); that is correct. Only the *second
  Find-or-create* was removable, and it was removed.
- **D13's four `pDefaultFramebufferInfo->defaultFBO` → `kMGPipeDefaultFramebuffer` rows are not
  actionable as written** (§4.4): the default FBO is never registered in the twin table, so
  `HandleOf` answers the null handle for it. Somebody has to own minting/installing
  `kMGPipeDefaultFramebuffer` (`MGPipeHandles.h:71`) first. Correctly deferred; the integrator owns
  the decision.

---

## What I looked for and did **not** find (so the integrator does not re-run it)

- **A handle re-key that breaks on deletion, reuse or a share group.** Lifetime ids are
  process-wide `std::atomic<Uint64>` counters starting at **1** on every one of the seven classes
  (`TextureObject.cpp:17`, `SamplerObject.cpp:18`, `ProgramObject.cpp:25`,
  `VertexArrayObject.cpp:17`, `FramebufferObject.cpp:18`, `RenderbufferObject.cpp:20`,
  `BufferObject.cpp:19`), so there is no id-0 hole (`SlotAllocator.cpp:88`/`:98` would otherwise
  have skipped `ByLifetimeId` registration and made every lookup miss). Ids are never reused, the
  registries and the allocator are both process-global, and `Gen` moves only through
  `Free`→`Allocate` (`SlotAllocator.cpp:66-76`, `:113-130`), which only `ReclaimDeadSlots` drives
  and only for an expired frontend object. A stale handle cannot resolve to a successor's twin;
  `SanityTest.cpp:3163-3196` pins that and it is the one load-bearing new test.
- **`UnitBindingsSnapshot` holding lifetime ids instead of `{slot, gen}`** (§4.3) is the right
  call: a bound-but-never-synced texture has no twin and therefore no handle, so a handle-keyed
  snapshot would read two never-synced textures as equal. The lifetime-id compare has the same
  never-reused property the `WeakPtr` owner compare had.
- **A stale `UnitSamplerLookupMemo` row.** The push arm (`DirectGLES.cpp:3320-3335`) refuses the
  memo unless the *current* object's handle is non-null and equal, and a reclaimed slot's successor
  always carries a higher `Gen`, so a stale row can never match. "A miss is never cached" survives.
- **`EnsureProcessTeardownSentinel()` losing its arming.** It stays at `Managers.h:321`, ahead of
  the arm dispatch, so both arms arm it on first insertion.
- **`ScopedDetachedTextureFramebufferAttachments`' walk changing meaning.** `ForEachLive`
  (`SlotTables.h:156-163`) skips `!Live`/`!backend` and hands a locked strong reference; the legacy
  walk skipped `stateRef.expired()` and `detachFrom` early-returns on a null twin. Same set, and
  the push arm no longer hands out the raw map key.
- **Reference invalidation regressions.** The handle arm's invalidation set (a `GetOrCreate` that
  grows the vector) is a strict subset of the legacy arm's (any `GetOrCreate`/`Find`/
  `CollectGarbage`), so every caller that was correct before is still correct.
- **Hot-path instrumentation, stdio, or `pGLContext` in `MG_Backend/`.** None added.
- **Legacy residue outside a guard.** I re-ran the audit with a preprocessor-stack walker rather
  than a plain grep: every `OwnerEquals` / `TwinLookupMemo` / `g_fbSlotCache` occurrence in
  `DirectGLES.cpp` is inside `#if MOBILEGL_PIPE_LEGACY_MEMOS`, `#if !MOBILEGL_PIPE_PUSH`, or the
  `#else` of `#if MOBILEGL_PIPE_PUSH`. `GetFramebufferBindingSlotFast` is gone entirely (all five
  call sites converted). §3's row is accurate.

---

## Recommendation

Do not land as-is. In priority order:

1. Add the creation-driven sweep to `BackendSlotTable` (MAJOR 1) — a `m_creationTick` /
   `kCreationGCInterval = 64` in `GetOrCreate`, ticked when a slot is newly `Live`. Small, local,
   and it removes the only *regression* in the package.
2. `std::abort()` after the `Fatal{PipeLegacyMemosDisabled}` and move the check to config load
   (MAJOR 2).
3. Integrator: decide `e2` — hand `MG_State/GLState/*State/*` `OnDestroy` hooks to package B, or
   land B before C and re-open C for one commit (MAJOR 3). Until then §4.1's "unchanged" must be
   corrected to "worse" in every artefact.
4. Produce a desktop `DriverBench` T1/T2 for this slice with `MOBILEGL_PIPE_PUSH=0x20` vs `0`
   before D.4 (MAJOR 4). If the delta is negative, cache the handle on the frontend object rather
   than probing `ByLifetimeId` per lookup.
5. Fix the tautological test (minor 1) and run a `build-verify` pass against the handle arm
   (minor 12).
