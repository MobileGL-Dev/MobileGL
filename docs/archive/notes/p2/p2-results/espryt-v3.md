# P2 package C — `p2/espryt` (Espryt 0b, the first Track H slice) — result v3 (rework round 3)

Tree `~/w7/p2-espryt`, branch `p2/espryt`, base tag `p2/contract` (`9c6a8a25`), HEAD `994730a7`.
Build directories: `build-linux` (pull, the G1 build), `build-push`, `build-verify`
(`-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`), `build-nolegacy`
(`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`) and `build-bench`
(`-DMOBILEGL_BUILD_BENCHMARK=ON -DMOBILEGL_PIPE_PUSH=ON`, `DriverBench` target only — see §5.6).
`build-nolegacy/` is an untracked build directory, not content, and is not committed.

## 1. Commits

Round 3 added three commits on top of the six from rounds 1–2.

| sha | subject |
|---|---|
| `dc9b7237` | `[Feat] (Espryt): give the backend a dense {slot, gen} twin table beside the address-keyed registry` |
| `fbdaeef3` | `[Refactor] (Espryt): key every backend twin on {slot, gen} instead of the frontend object heap address` |
| `1e0f4d6d` | `[Test] (Espryt): pin the twin table identity contract - a reclaimed slot is a new handle and the stale one resolves to nothing` |
| `d714600a` | `[Fix] (Espryt): do not return a reference through a null slot pointer, and stop a comment claiming a memo that is no longer there` |
| `d89fb684` | `[Fix] (Espryt): sweep the twin table on object churn again, refuse to run an arm the operator disabled, and give the hot lookups back their array probe` |
| `5f245ac7` | `[Test] (Espryt): pin the churn-driven sweep cadence, the null tolerance, and the one slot per object the two Track H slices share` |
| **`3e59a856`** | **`[Test] (Espryt): run the sanity binary on the twin arm it was compiled for, and pin that it does`** (MAJOR 1) |
| **`103cafed`** | **`[Fix] (Espryt): tell the backend when a frontend object dies instead of discovering it in a garbage sweep`** (MAJOR 2) |
| **`994730a7`** | **`[Fix] (Espryt): pick the unit-bindings debounce by the runtime arm, and stop two slot cases sharing one kind`** (minors 4, 5, 6) |

Files touched across all nine, versus `p2/contract`:

```
 MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp                        370 ++++-
 MobileGL/MG_Backend/DirectGLES/Managers.cpp                          102 ++-
 MobileGL/MG_Backend/DirectGLES/Managers.h                            161 +++-
 MobileGL/MG_Backend/DirectGLES/SlotTables.h                          350 +++++   (new)
 MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp              16 +-      (new in v3)
 MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp    15 +       (new in v3)
 MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.h       6 +       (new in v3)
 MobileGL/MG_State/GLState/StateObjectDeathNotice.h                    62 +       (new in v3, new file)
 MobileGL/MG_Test/SanityTest.cpp                                      450 +++++
 9 files changed, 1489 insertions(+), 43 deletions(-)
```

The four `MG_State/GLState/` rows are new this round and are **not** in C.2's *Files* line. They are
also not in any other package's row of C.5 — `ProgramState/*`, `RenderbufferState/*` and
`MG_State/GLState/StateObjectDeathNotice.h` (a new file) all fall under C.5's *"everything else:
nobody"*. So no ownership line is crossed; the departure from C.2's *Files* line is declared as
deviation §4.1.

---

## 2. The two majors

### MAJOR 1 — the D13 "must not break" pins now execute the `{slot, gen}` arm (FIXED)

**The finding, restated.** `MG_Config::Features.PipePush` defaults to `0` (`Config.h:334`), which
is the value a *pull* build ships. `SanityTest` never runs `MG_ConfigLoader::Init`, so
`Managers.cpp`'s `bitSet` was false in **every** build directory and
`StateBackendObjectRegistry::GetOrCreate` took the map arm — including in `build-push` and
`build-verify`. Every `ctest -L unit` figure the v2 result file offered as evidence for the D13
list was therefore evidence about code this package did not change.

**The fix** (`3e59a856`, `MG_Test/SanityTest.cpp` only). A gtest global `Environment` seeds
`Features.PipePush` with **ConfigLoader's own push-build default**
(`kMGPipeSubsystemsMigratedAtP2`) before the first test, so the binary runs the arm that *ships in
the build it was compiled for*:

* `build-linux` — the handle arm is not compiled at all; every `DirectGLESSlotTable` case skips
  visibly and the legacy registry keeps its coverage, which is the pre-P2 behaviour;
* `build-push`, `build-verify` — the `{slot, gen}` arm, i.e. the code this package wrote.

`MOBILEGL_PIPE_PUSH` in the environment overrides it, parsed with ConfigLoader's own decimal/`0x`
contract, so `MOBILEGL_PIPE_PUSH=0 ./SanityTest` is the legacy-arm run of the same binary and the
A/B is one env var. It is an `Environment` and not a static initializer because
`MG_Config::Features` has a `String` member and is dynamically initialised, so writing to it from
another TU's static initializer would be an initialisation-order race; `SetUp()` runs inside
`RUN_ALL_TESTS`, after every static initializer and before anything can latch
`EsprytSlotTablesEnabled()`.

This is deliberately stronger than the "second always-on ctest entry" the review suggested: it
needs no CMake change (`MG_Test/CMakeLists.txt` is owned by nobody in C.5, so touching it would
have been a judgement call), it costs no new ctest name, and it moves **all 82** of the binary's
other cases onto the handle arm rather than a hand-picked subset.

**The reviewer's own probe, re-run.** `gdb -batch`, `ignore N 100000000` on each breakpoint,
`--gtest_filter=-DirectGLESSlotTable.*`:

| build dir | `EnsureProcessTeardownSentinel` | `MGPipeSlotAllocator::Acquire` | `ResolveEsprytSlotTablesArm` | result |
|---|---|---|---|---|
| `build-push`, v2 (reviewer) | 4 | **0** | 1 | 82 passed on the map arm |
| `build-push`, **v3** | 4 | **4** | 1 | **82 passed on the handle arm** |
| `build-verify`, v2 (reviewer) | 4 | **0** | 1 | 82 passed on the map arm |
| `build-verify`, **v3** | 4 | **4** | 1 | **82 passed on the handle arm** |

`EnsureProcessTeardownSentinel` is called from `GetOrCreate` on both arms, so 4 == 4 says every one
of the four twin-registry entries the must-not-break flows make (`SanityTest.cpp:321`, `:365`, and
the flows under `:2635` and `:3001`) went down the `{slot, gen}` arm.

**Falsifiable, and made so on purpose.** New always-on case
`DirectGLESSlotTable.TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm` is that gdb probe as a
test. Negative control NC1 in §3.4: put the environment's default back to `0` and it is the **only**
case that fails, naming the arm rather than a symptom.

`ScopedDirectGLESTextureBindings` (`:154-197`) now saves, resets and restores a real
`BackendSlotTable` in `build-push`/`build-verify` — that is the specific fixture the review said
could not be exercised — and passes, together with the scratch-FBO scrub, the three
context-generation guards and the sampled-set staleness walk. All 93 cases pass on the handle arm
and all 91 non-skipped cases pass on the legacy arm (`MOBILEGL_PIPE_PUSH=0`).

### MAJOR 2 — step `e2` now has a landed, tested deliverable (PARTLY FIXED; the residue is named)

**What landed** (`103cafed`):

1. **`MobileGL/MG_State/GLState/StateObjectDeathNotice.h`** (new, frontend, `#if MOBILEGL_PIPE_PUSH`
   only) — `BufferBackendOps`' shape for the other six kinds: an ops table the backend fills in,
   plus `NotifyStateObjectDestroyed(kind, lifetimeId)`. It carries `{kind, lifetimeId}` and not the
   object because by the time the last `SharedPtr` has dropped there is no object to pass, and the
   lifetime id is exactly what the client slot allocator resolves a handle from
   (`ARCHITECTURE.md:40`). One entry point for six kinds rather than six ops tables, because the
   backend's answer is the same for all six.
2. **`BackendSlotTable::DestroyByLifetimeId`** (`SlotTables.h`) — drops the twin and returns the
   slot immediately, with the same "nothing that outlives the twin destructor may be a reference
   into `m_slots`" ordering rule `ReclaimDeadSlots` uses. It returns the slot **only when this
   table holds it**, which is not defensive: two holders of one kind already exist
   (`ScopedDirectGLESTextureBindings`; Magma's subsystem-4 table shares `VertexElementsCso`).
3. **`StateBackendObjectRegistry::DestroyByLifetimeId`** (`Managers.h`) — arm dispatch. The legacy
   arm cannot answer a notice at all (its key is the frontend heap address, gone by the time a
   destructor speaks), so there it is a no-op and the sweep stays its only death signal. That
   asymmetry *is* the announced-versus-discovered half of the A/B `MOBILEGL_PIPE_LEGACY_MEMOS`
   exists for (`ARCHITECTURE.md:365-369`).
4. **The dispatcher and its registration** (`Managers.cpp`) — one `switch` over the six kinds,
   registered from `ResolveEsprytSlotTablesArm()`, i.e. exactly once per process and exactly when
   the arm that can answer a notice is the arm that runs. A notice arriving after `exit()` has
   begun is dropped (`InProcessTeardown()`), because past that point a twin destructor must not
   call the driver.
5. **The firing side, for the two of the six classes this package may touch** —
   `ProgramObject::~ProgramObject` (kind `ShaderCso`) and a new
   `RenderbufferObject::~RenderbufferObject` (kind `Renderbuffer`), both `#if MOBILEGL_PIPE_PUSH`
   so the pull build is untouched (G1 confirms: contract→HEAD is `0 resized`). The destructor, not
   `glDelete*`, is the right site: a still-bound object goes on living
   (`TextureState.cpp:118-155`), which is what D13 says.

**What is still owed, and by whom.** Four one-line destructor calls, in
`MG_State/GLState/{TextureState,FramebufferState,SamplerState}/*` and
`VertexArrayState/VertexArrayObject.*` — C.5 gives those to packages B and D. Until they land, the
four remaining kinds still discover death in the sweep, so `ARCHITECTURE.md:363`'s census cannot
yet count all six registries' GC as deleted and the seven `CollectGarbageIfNeeded` call sites
(`DirectGLES.cpp:1275, 1662, 2027, 2028, 2029, 2857, 2858`) still stand as the fallback. The merge
rule in section E ("`e3` is not merged without `e2`") is now a question about **four lines in
another package's files**, not about a missing mechanism: the notice exists, the consumer exists,
the dispatcher covers all six kinds, and two kinds fire it end to end.

**Tested, and each half separately falsifiable** (negative controls NC2–NC4, §3.4):

| case | what it pins |
|---|---|
| `AnAnnouncedDeathReturnsTheSlotWithoutASweep` | the slot comes back and the twin goes **while the object is still alive**, so no sweep can be responsible; the notice is idempotent; a table that does not hold the slot says so instead of freeing another holder's slot |
| `AProgramAndARenderbufferAnnounceTheirOwnDeath` | both classes raise the notice, with the right kind and lifetime id, when the last `SharedPtr` drops — and **not before** |
| `TheHandleArmInstallsTheDeathNoticeConsumer` | the backend actually registers a consumer, rather than the two halves each being fine on their own |

---

## 3. Verification — every command and what it printed

All from `~/w7/p2-espryt`, `CCACHE_BASEDIR=/home/swung/w7`.

### 3.1 Build and static gates

| gate | command | result |
|---|---|---|
| build | `cmake --build {build-linux,build-push,build-verify,build-nolegacy} -j 12` | all `rc=0` |
| build | `cmake --build build-bench --target DriverBench -j 12` | `rc=0` (the whole-tree `build-bench` build fails in a fetched third party — §5.6) |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `0 added, 0 removed, 0 renamed`; **4 resized**, all the contract's: `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 |
| **G1 attribution** | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed` — **package C's pull-build delta is still exactly zero**, including the new `MG_State` destructor and the new header |
| **G5** | `git show {48268068, p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…220efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, `rc=0` |
| **G13** | `gen_pipe.py --check` / `--self-test` | `rc=0` / `rc=0` |
| **G13 stdio** | `grep -nE '\b(printf\|fprintf\|std::cout\|std::cerr\|puts\|putchar)\b'` over the seven touched/new sources | 9 hits, **all the English word "puts" in comments**; the four new `MG_State` files and `SlotTables.h` have **zero** |
| legacy residue | preprocessor-stack walker over `DirectGLES.cpp` for `OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|GetFramebufferBindingSlotFast\|legacySlotObjects\|legacySamplerObject` | 34 hits, **1 "unguarded"** and it is a *comment line* (`:1233`); every code hit is inside `#if MOBILEGL_PIPE_LEGACY_MEMOS`, the `#else` of `#if MOBILEGL_PIPE_PUSH`, or `#if !MOBILEGL_PIPE_PUSH \|\| MOBILEGL_PIPE_LEGACY_MEMOS`. `GetFramebufferBindingSlotFast`: 0 hits in the whole tree. Corroborated structurally by `build-nolegacy` compiling, linking and passing |
| **G2** | `ctest -N \| sed -n 's/^ *Test  *#[0-9]*: //p' \| sort` × 4 dirs, then `diff` | `build-linux` 2378, `build-push` 2378, `build-nolegacy` 2378 — both diffs **empty**. `build-verify` 3196 (its extra verify-only lanes, by design) |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | **empty**. Added: the contract's 4 `*.PlaceholderUntilTheOwningPackageFillsThisIn` and this package's **11** `DirectGLESSlotTable.*` |

### 3.2 Tests

| lane | command | result |
|---|---|---|
| unit × 4 dirs | `ctest --test-dir <d> -L unit --no-tests=error -j 6` | `100% tests passed, 0 failed out of 1500` on **build-linux, build-push, build-nolegacy, build-verify** |
| slot cases | `ctest -R DirectGLESSlotTable` | `11/11` on `build-push`, `build-nolegacy`, `build-verify`; all 11 present and `(Skipped)` on `build-linux` |
| sanity, handle arm | `./build-push/…/SanityTest` | `[ PASSED ] 93 tests` |
| sanity, legacy arm | `MOBILEGL_PIPE_PUSH=0 ./build-push/…/SanityTest` | `[ PASSED ] 91`, 2 skipped (the two cases that are about the handle arm) |
| sanity, order independence | `--gtest_shuffle --gtest_random_seed={1..5}` whole binary, `{7,8,9}` slot suite | `93/93` × 5 and `11/11` × 3 — review minor 5 |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **446/446** |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | **446/446** |
| integration, no legacy compiled | `ctest --test-dir build-nolegacy …` | **446/446** |
| integration, verify build | `ctest --test-dir build-verify -L integration-gpu -j 4 -R DirectGLES` | **855/855** |
| **G4 half — integration-verify** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **818/818**; `grep -c 'Fatal{'` over all six `pipe-verify-*.log` = **0 each** |
| `Fatal{PipeLegacyMemosDisabled}` | `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData` | `[ PASSED ] 0 tests`, `[ SKIPPED ] 1` — the disabled arm still does not run |
| **G3 (DirectGLES half)** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/e3-espryt -j 4 --only 'DirectGLES$'` | `rc=0`, **`passed 39 / 39; failed: []`**. Five lowest SSIM **0.993475 / 0.995426 / 0.995920 / 0.996197 / 0.996301** — bit-identical to v2 and to the reviewer's own run |

### 3.3 The gdb probe

See the table in §2 MAJOR 1. `Acquire` 0 → 4 in `build-push` and `build-verify`.

### 3.4 Negative controls — each new gate can go red for the reason it exists

Each was applied alone, `SanityTest` rebuilt, the suite run, then the file restored from a `/tmp`
copy (`git status` clean afterwards; **no `git stash`**, see §5.4).

| # | perturbation | result |
|---|---|---|
| NC1 | `EsprytSlotArmEnvironment`'s default goes back to `0` | **only** `TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm` fails (`TheHandleArmInstallsTheDeathNoticeConsumer` correctly skips); 9 pass |
| NC2 | `BackendSlotTable::DestroyByLifetimeId` returns `false` at once | **only** `AnAnnouncedDeathReturnsTheSlotWithoutASweep` fails; 10 pass |
| NC3 | `~RenderbufferObject` stops raising the notice | **only** `AProgramAndARenderbufferAnnounceTheirOwnDeath` fails; 10 pass |
| NC4 | `Managers.cpp` never calls `SetStateObjectDeathOps` | **only** `TheHandleArmInstallsTheDeathNoticeConsumer` fails; 10 pass |
| restore | files back, rebuild | `11/11 PASSED` |

(The v2 round's control on the creation-driven sweep — neuter `kCreationGCInterval` and only
`ObjectChurnAloneDrivesTheSweep` fails — still holds; it was not re-run this round because nothing
in that path changed.)

### 3.5 DriverBench — the same-binary arm A/B, re-measured

`build-bench`'s `DriverBench` against `build-push/libMobileGL.so`, `EGL_PLATFORM=surfaceless`,
Mesa/llvmpipe, `taskset -c 20-23`, arms interleaved round-robin, `MOBILEGL_PIPE_PUSH=0x7f` (this
slice on) vs `0x5f` (this slice off, everything else as default) — one binary, so the delta is this
slice and nothing else. Raw rows `~/w7/e3-bench.csv` (5 reps) and `~/w7/e3-bench2.csv` (9 reps).

| case | 5-rep min / median delta | **9-rep min / median delta** | within-arm spread of the `0x5f` arm alone |
|---|---|---|---|
| `draw_tiny` | +40 / +139 | **−258 / −118** | 957 ns/op = 13 % of its own median |
| `mc_state_toggle` | +794 / +1307 | **−3022 / −1399** | 6455 ns/op = 19 % of its own median |

**Read it as a bound, not a result.** The delta changes sign between two runs of the *same two
binaries* an hour apart, and the within-arm spread is 13–19 % of the median (host load average
28–39 throughout: the other P2 packages were building and testing on it). What can honestly be
said: no several-hundred-ns-per-draw regression from this slice is visible on this host, in either
direction, and round 3's only per-draw addition is one inlined `EsprytSlotTablesEnabled()` (a
guard-variable load plus a perfectly-predicted branch) per `CaptureUnitBindings` /
`UnitBindingsUnchanged` call, i.e. at most twice per draw. The brief's ceiling
(`T1 ≤ 45 ns/draw` Adreno 830, `≤ 60` Mali) is a device number this host cannot resolve; D.4.2/D.4.3
on the two devices remain owed, and this slice should be measured there with the same `0x7f` vs
`0x5f` A/B — one binary, one lock hold, exact attribution.

### 3.6 Gates this package cannot reach from its own tree — per gate

Recorded so `1500 × 4` + `446/446` + `855/855` + `818/818` + `39/39` is **not** read as a P2
acceptance pass (review minor 7):

| gate | why not reachable here | probe I ran |
|---|---|---|
| **G6 / G7** (setter consistency + its negative control) | package A owns `MGPipeRenderStateSpans.*`; package E owns `scripts/g7_negative_control.sh` | `ctest -N \| grep RenderStateSpans` → the single entry `RenderStateSpans.PlaceholderUntilTheOwningPackageFillsThisIn` |
| **G8** (`HandleRecycleScenario`, three arms) | package E owns `MG_IntegrationTest/**` | no ctest name matching `HandleRecycle` exists in any of the four directories |
| **G9** (`gen_pipe_dirty_surface.py` as a gate) | package B owns the script | `--check` → `error: unrecognized arguments: --check`; only `--summary` exists |
| **G10** (residual tripwire + device `resid=` window) | package A owns the ratchet, E owns the device lane | `MGL_RESIDUAL_BLOCK_SIZE` is already `8` (`MGPipeTypes.h:546`), but the only `Residual` ctest entries are the contract's own `PipeCatalogue.ResidualBlockSizeIsPinned` / `…IsExactlyItsTwoValueStructsPlusPatchTail`; there is no device window from a Linux tree |
| **G11** (two-device paired A/B) | needs the two devices and E's `frameCpuTimesMs[]` | not attempted; §3.5 is the desktop bound |
| **G12** (blend-toggle microbench + CSO negative control) | package E / A | no `CsoContentAddressing` ctest name exists |
| **G14 (CI matrix half)** | `gh workflow run test.yml` is the integrator's | the local half (no test name removed) is green, §3.1 |

---

## 4. Deviations from the brief

### 4.1 NEW — four files outside C.2's *Files* line were edited, none of them owned by another package

C.2's *Files* line grants `MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.h, Managers.cpp}`, the
new `SlotTables.h` and `MG_Test/SanityTest.cpp`. Landing any part of `e2` needs a frontend
destructor, and C.2 step 2 asks for exactly that. I took the two object classes whose files C.5
assigns to **nobody** — `ProgramState/*` and `RenderbufferState/*` — plus one new frontend header,
`MG_State/GLState/StateObjectDeathNotice.h`, which nothing else in the tree names. The four classes
C.5 *does* give to B and D (`TextureState`, `FramebufferState`, `SamplerState`,
`VertexArrayState/VertexArrayObject.*`) were **not** touched. So the standing instruction ("do not
edit a file the ownership table gives to another package") is honoured, and the departure is from
C.2's *Files* line only. If the integrator would rather C had touched nothing outside its five
files, reverting `103cafed`'s three `MG_State` source edits leaves the mechanism intact and unfired.

### 4.2 `e2` is landed as a mechanism, not as the retirement of the sweep (was BLOCKING; now scoped)

Four one-line destructor calls in B's and D's files remain, and until they land the seven
`CollectGarbageIfNeeded` call sites stay as the fallback and `ARCHITECTURE.md:363`'s census cannot
count all six registries' GC as deleted. See §2 MAJOR 2. Integrator decision, but a much smaller
one than in v2.

### 4.3 The `TwinLookupMemo`s are replaced by a table-local one-entry memo, not by a bare array index

Unchanged from v2. D13's "direct slot indexing" is true of the table (`FindByHandle` is a bounds
check plus an index) but not of resolving a frontend object to its handle, which goes through the
allocator's `lifetimeId → slot` hash; a one-entry `lifetimeId → handle` memo sits in front of it.
The key is a lifetime id (never reused at all), and `FindByHandle`'s `Gen` compare is the second
line of defence.

### 4.4 NEW (declared per review minor 3) — `SyncTextureObjectToBackend`'s by-value copy and second `Find` are **not** deleted on the handle arm

D13 says they "are deleted in the same change, not left as harmless". They are not: the copy
survives at `DirectGLES.cpp:1377` and a re-resolving `Find` at `:1403`. The reason is in the code at
`:1369-1373` and the reviewer accepted it — a nested `GetOrCreate` (the `glTextureView` path syncs
the viewed texture first) can grow `Vector<Entry> m_slots` and invalidate the reference. What
*changed* is their job: the copy is now a keep-alive across the nested sync rather than a defence
against the map's erase-inside-`Find`, and the tail is a single re-resolve rather than a
find-or-create-and-repair (the legacy arm's three-branch repair is still there, under `#else`). The
v2 result file did not declare this; it is declared now.

### 4.5 `UnitBindingsSnapshot` holds lifetime ids on the handle arm, and P1's `WeakPtr`s on the legacy arm

The identity choice is unchanged from v2 and the reviewer agreed with it: a texture that is bound
but never synced has no twin and therefore no handle, so a handle-keyed snapshot would read two
never-synced textures as equal; the debounce needs an identity that exists before the twin does.
What round 3 changed (review minor 4) is the **split**: it was `#if MOBILEGL_PIPE_PUSH`, so a push
build ran P2's debounce on the `MOBILEGL_PIPE_PUSH=0` arm too. It is now a runtime
`EsprytSlotTablesEnabled()` branch with P1's `WeakPtr` fields kept beside the ids whenever the
legacy arm is compiled — the same shape `g_fbSlotCache` got in v2. Semantics are identical either
way (`OwnerEquals` on two empty pointers is true and `LifetimeIdOf(nullptr) == 0 == 0`; a
live-versus-expired control block and two distinct lifetime ids both compare unequal), so this is
D14 A/B fidelity, not a behaviour change. A build with no legacy arm carries neither the extra
fields nor the branch.

### 4.6 The four `pDefaultFramebufferInfo->defaultFBO` compares are not retired

Unchanged from v1/v2, and the reviewer agreed it is not actionable as written: the default
framebuffer is never registered in the twin table, so `HandleOf` answers the null handle for it.
Somebody has to own minting and installing `kMGPipeDefaultFramebuffer` first.

### 4.7 Some deletions are arm-split rather than outright

`ScopedDetachedTextureFramebufferAttachments`' walk keeps the pull text verbatim under `#else`,
because the shared form moved pull-build codegen and G1 admits no resize beyond the contract's.

### 4.8 `retrace_gate.py` has no `--backend` or `--ssim` flag

It takes `--tree --lib --out -j --only`; the SSIM threshold comes from the reference
`build-retrace` `CTestTestfile.cmake` it parses. C.2's and C.3's verification blocks and G3's row in
section A spell flags that do not exist. Tree wins on the fact; `--only 'DirectGLES$'` is the
working form.

### 4.9 The `MOBILEGL_ASSERT` on a null state object was removed from `BackendSlotTable::GetOrCreate` only

Corrected from v2 (review minor 1). v2 said it "was removed from that path on purpose"; that is
true of `BackendSlotTable::GetOrCreate` (`SlotTables.h:109-119`) and **false of the shipping call
path**. `StateBackendObjectRegistry::GetOrCreate` (`Managers.h:353`) still opens with
`MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null")` before dispatching to
either arm, so a DEBUG build still traps on null exactly where pre-P2 did, and
`DirectGLESSlotTable.GetOrCreateToleratesANullStateObject` therefore exercises the *table's* own
contract rather than a path production takes. That assert was deliberately left where it is: it is
pre-P2 behaviour on a D13 "must not break" path and removing it would be a change nobody asked for.
The table below it must still be defined on null because a release build compiles the assert out —
which is what the case pins. No code change; the claim is corrected.

### 4.10 One v1 test name was changed between rounds

`…TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` → `…ShareOneSlotAndKeepTheirOwnTwin` (v2). G14 is
untouched — the old name was added by this package on this branch and is not in
`~/w7/p2-before-ctest-names.txt` — but the integrator should know the name moved.

---

## 5. Where the tree contradicted the brief

### 5.1 The contract commit resizes a fourth pull-build symbol

`_GLOBAL__sub_I_DirectGLES.cpp` is 1340 → 1331 bytes (−9) at `p2/contract` with zero package-C
changes; contract→HEAD is a byte-identical `.text`. G1's admitted set (D15 point 4, C.0's
verification block) must be widened by that symbol and the attribution belongs to package A's D3.1.
Re-verified this round after the `MG_State` destructor and the new header landed.

### 5.2 The `integration-gpu` first-run scheduling flake — not reproduced this round

v2 reported that
`DirectGLES.{UnlocatedIoBlocks,PointSizeDemotion}…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`
fail on the **first** `-j 4` run of a build directory and pass afterwards, and root-caused it to
ctest's cost-data ordering plus a shared per-lane log file
(`MG_IntegrationTest/CMakeLists.txt:387, 370`). The reviewer could not reproduce it (warm cost
data) and **neither could I this round** — every integration run above was green first time, on
warm cost data. So the mechanism stands as an explanation but the observation is not reproduced in
v3; the fix, if wanted, is package E's (`RUN_SERIAL` / `RESOURCE_LOCK`, or a per-test log path).

### 5.3 The G2 / G14 `ctest -N` extraction in section A silently drops most tests

`grep -E '^\s+Test #'` misses `  Test   #83:` once the total is four digits. Working form used
throughout: `ctest --test-dir <dir> -N | sed -n 's/^ *Test  *#[0-9]*: //p' | sort`. Section A (G2,
G14) and D.1/D.3 need the fix. Confirmed independently by the reviewer.

### 5.4 `git stash -u` in a P2 worktree destroys the copied trace fixtures

`wsl_p2_tree.sh` copies the 803 MB fixture set in and marks it `--assume-unchanged`; `git stash`
resets through that bit and leaves LFS pointer files. Every negative control this round used
`cp`-to-`/tmp` and never `git stash`; the fixture set is intact.

### 5.5 `p2/contract` is an ambiguous refname in this worktree

`git show p2/contract:…` warns `refname 'p2/contract' is ambiguous` (a tag and a branch-shaped path
both match). Harmless — it resolves to the tag — but the integrator will see it in every G5 run.

### 5.6 NEW — a whole-tree `build-bench` build fails in a fetched third party, unrelated to P2

`cmake --build build-bench` fails compiling **google-benchmark's own sources**
(`_deps/benchmark-build/src/…/benchmark_name.cc.o` and five siblings):
`benchmark/benchmark.h:1461: error: '__COUNTER__' is a C2y extension [-Werror,-Wc2y-extensions]`
under this Arch clang. Nothing in MobileGL is involved and no file this package touched is in the
failing translation units. `cmake --build build-bench --target DriverBench` is `rc=0` and is what
§3.5 used. The integrator will hit this on any `-DMOBILEGL_BUILD_BENCHMARK=ON` tree on this host;
the fix is a `-Wno-c2y-extensions` on the fetched target and belongs to whoever owns the benchmark
CMake.

---

## 6. Unfinished, and debts carried

1. **`e2`'s last four destructor calls** — `MG_State/GLState/{TextureState,FramebufferState,
   SamplerState}/*` and `VertexArrayState/VertexArrayObject.*`, owned by B and D. Until they land
   those four kinds still discover death in the sweep and the seven `CollectGarbageIfNeeded` sites
   stay. §2 MAJOR 2 / §4.2.
2. **Slots are still not returned when a *table* is destroyed or reset.** Review minor 2 was right
   that v2's stated blocker was false — `SlotAllocator.cpp:113-130` opens with
   `if (!entry.Live || entry.Gen != handle.Gen) return;`, so `Free` is idempotent and
   generation-safe and a double `Free` is a no-op. **The real blocker is slot REUSE between reset
   and restore**, and it is concrete: `ScopedDirectGLESTextureBindings` does
   `saved = g_backendTextureObjects; g_backendTextureObjects = {}; …; g_backendTextureObjects =
   saved;`. If the copy-assign at step 2 released the old contents' slots, slot 3 (gen 1) goes on
   the free list; the test body then creates a texture and `Acquire` hands slot 3 back as **gen 2**;
   the restore at step 4 reinstates an entry claiming `{3, 1}`, and from then on `HandleOf` resolves
   `{3, 2}` while the entry says gen 1, so `FindByHandle` misses and the *pre-existing* texture's
   twin is unreachable and its driver texture leaks. Idempotence does not help, because the second
   `Free` is not the problem — the intervening `Allocate` is. The fix that does work is a refcounted
   slot-ownership token shared by table copies (released only when the last copy dies), and it wants
   an owner who holds both holders — the same owner minor 6 below needs. Deferred, with the reason
   now stated correctly. Cost today: a small, **test-only** slot and `ByLifetimeId` leak on table
   reset, so `MGPipeSlots().HighWater`/`LiveCount` are not a process-wide health signal from a test
   process. Real (non-test) tables are process-lifetime globals and are never destroyed.
3. **Two holders of one kind is a cross-package hazard.** `ScopedDirectGLESTextureBindings` already
   makes a second live table of kind `Texture`, and package D's subsystem 4 re-keys `VaoDrawMemo` out
   of the same per-kind allocator. Whichever holder frees the slot first (sweep or death notice)
   leaves the other naming a stale handle: **safe** — `Free` is generation-guarded and
   `FindByHandle` compares `Gen`, so a stale handle is a miss, not a wrong twin — but if the object
   is still alive the next resolution re-`Acquire`s it onto a **new** slot and orphans the first
   table's entry until its own sweep. `DestroyByLifetimeId` is written so it cannot make this worse
   (it frees only a slot this table holds), and the hazard is now written into
   `SanityTest.cpp`'s two-holder case instead of the comment that said it "cannot arise". Flagged
   for the integrator.
4. **The backend still mints client handles.** `SlotTables.h` calls `MGPipeSlots().Acquire()` off a
   frontend `SharedPtr`'s `GetLifetimeId()`, and `MGPipeHandles.h:13-16` says a handle is minted by
   the client and never by the server. A P3+ DEBT block at the top of `SlotTables.h` names it. It is
   monolith glue, it is **not** "Track H done", and `check_include_closure.py` does not probe
   `MG_Backend` headers so nothing catches a second instance.
5. **Device measurement.** §3.5 bounds this slice on a contended llvmpipe host and no more. D.4.2's
   two-device paired A/B and D.4.3's T1/T2 on device are owed, with the `0x7f` vs `0x5f` arm A/B.
6. **Gates G6–G12** are not exercisable from this tree; §3.6 says which and why, per gate.
7. **No `PipeStats` counter for this subsystem** (package A owns `PipeStats.{h,cpp}`), so the
   integrator's A/B still has no counter saying "N twins resolved through the slot table this
   frame" — only the gdb counts.
