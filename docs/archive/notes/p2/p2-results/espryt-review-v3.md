# Adversarial review — package `p2/espryt` (brief C.2), result file `espryt-v3.md`

Reviewer: adversarial pass, round 3. Tree `~/w7/p2-espryt`, branch `p2/espryt`, HEAD `994730a7`,
base tag `p2/contract` (`9c6a8a25`). Everything below was re-run by me; nothing was taken on the
result file's word. The tree was left exactly as found (`git status --porcelain` → only the
untracked `build-nolegacy/`, as at the start; no file was edited, no negative control applied).

**Verdict: NOT APPROVED — 3 majors.**

---

## 0. What I re-ran, and what held

Every one of these reproduces what the result file claims. I record them so the majors below are
read as *what the green does not cover*, not as a dispute about the green.

| gate | command (from `~/w7/p2-espryt`) | observed |
|---|---|---|
| build | `cmake --build {build-linux,build-push,build-verify,build-nolegacy} -j 12` | all `rc=0` |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `0 added, 0 removed, 4 resized, 0 renamed`; resized = `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 |
| **G1 attribution** | same with `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed` — package C's pull-build delta really is zero |
| **G5** | `git show {48268068,9c6a8a25,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…220efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (`rc=1`) |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `python3 scripts/gen_pipe.py --check` / `--self-test` | `rc=0` / `rc=0` |
| **G13 stdio** | stdio grep over the seven touched/new sources | 9 hits, all the English word "puts" in comments |
| **G2** | `ctest -N \| sed -n 's/^ *Test  *#[0-9]*: //p' \| sort` × 4 dirs, then `diff` | `build-linux` 2378, `build-push` 2378, `build-nolegacy` 2378; both diffs empty. `build-verify` 3196 |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | empty. Added: 4 contract placeholders + **11** `DirectGLESSlotTable.*` |
| unit × 4 | `ctest --test-dir <d> -L unit --no-tests=error -j 6` | `100% tests passed … out of 1500` on all four dirs |
| slot cases | `ctest -R DirectGLESSlotTable` | 11/11 on push / nolegacy / verify; 11 present and `(Skipped)` on build-linux |
| sanity | `./build-push/…/SanityTest` / `MOBILEGL_PIPE_PUSH=0 …` | `[ PASSED ] 93` / `[ PASSED ] 91` + 2 skipped |
| **round-2 MAJOR 1** | `gdb … break MGPipeSlotAllocator::Acquire … --gtest_filter=-DirectGLESSlotTable.*` | `build-push` **4 hits**, `build-verify` **4 hits**, `ResolveEsprytSlotTablesArm` 1 hit each. Round 2's `0` is genuinely fixed |
| integration | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **446/446** |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | **446/446** |
| integration, no legacy compiled | `ctest --test-dir build-nolegacy …` | **446/446** |
| **G4 half** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **818/818** |
| **G3 (DirectGLES half)** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/rev-espryt -j 4 --only 'DirectGLES$'` | `passed 39 / 39; failed: []` |

Gates the package cannot reach, re-probed and confirmed absent at this tree (all owned by A/B/E,
so **not** counted against C): `ctest -N \| grep -c HandleRecycle` → **0** in all four dirs (G8);
the only `RenderStateSpans` entry is `…PlaceholderUntilTheOwningPackageFillsThisIn` (G6/G7);
`gen_pipe_dirty_surface.py --check` → `error: unrecognized arguments: --check` (G9);
`grep -c CsoContentAddressing` → 0 and `scripts/g7_negative_control.sh` does not exist (G12/G7);
`MGL_RESIDUAL_BLOCK_SIZE 8` is already pinned at `MGPipeTypes.h:546` but the only `Residual` ctest
entries are the contract's `PipeCatalogue.*` (G10). So `1500×4 + 446×3 + 818 + 39` is **not** a P2
acceptance pass, and the result file says so (§3.6) — correctly.

---

## MAJOR 1 — a knob combination this package introduced turns the whole DirectGLES `integration-gpu` lane **green by skipping**, and mis-diagnoses itself as "no GPU"

Commit `d89fb684` added a `std::abort()` to `ResolveEsprytSlotTablesArm()`
(`MobileGL/MG_Backend/DirectGLES/Managers.cpp:213-225`) for the case
`kMGPipeSubsystemEsprytSlots` clear **and** `Features.PipeLegacyMemos` false. `DirectGLES.cpp:10225`
then forces that resolution from inside `InitDisplayAndContext()`, i.e. **inside the EGL bring-up**.

The integration harness pre-flights EGL bring-up in a forked child and converts a child that dies
on a signal into a *skip of every scenario* (`MG_IntegrationTest/Harness/ScenarioFixture.h:83`). So
the new `Fatal{}` never surfaces as a failure, never prints its own message to the parent, and
instead reads as a missing GPU:

```
$ cd ~/w7/p2-espryt
$ export MOBILEGL_BACKEND_TYPE=DirectGLES
$ export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ./build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest \
    --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData
  SKIPPING every scenario: the EGL bring-up ABORTS on this platform: a forked pre-flight child
  died on signal 6 (Aborted). MobileGL asserts rather than returning an error here, so the
  scenarios would have taken the whole test binary down with them
[  SKIPPED ] CrossFrameBufferScenario.VertexBufferSubData
```

and at ctest level the lane is **green**:

```
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario' --no-tests=error
100% tests passed, 0 tests failed out of 13     # …all 13 listed as (Skipped)
```

Control, same binary, same filter, one env var removed:

```
$ MOBILEGL_PIPE_PUSH=0 ./build-push/…/MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData
[  PASSED  ] 1 test.
```

Three things are wrong here, and they compound:

1. **`ROADMAP.md:7` — 每个门必须能因它存在的理由变红.** Under this combination every DirectGLES
   integration entry reports pass-with-skip. A CI job (or an integrator running the D.3 part-3
   A/B with the wrong pair of knobs) gets a green lane that ran nothing. The result file's own
   §3.2 row records exactly this observation — `[ PASSED ] 0 tests`, `[ SKIPPED ] 1` — and reads
   it as "the disabled arm still does not run", i.e. as the guard *working*. It is not the guard
   working; it is the guard being swallowed.
2. **The diagnosis is actively misleading.** The operator is told the platform has no usable
   GPU/display/ICD. Nothing anywhere in the output names `PipeLegacyMemosDisabled`, the two knobs,
   or the arm. That is the opposite of what the abort was added for ("refuse to run an arm the
   operator disabled" — `d89fb684`'s own subject).
3. **It is new.** Before this package the same knob pair simply ran the handle arm; the runtime
   `Features.PipeLegacyMemos` tri-state (`Config.h:375`, `ConfigLoader.cpp:273`) had no fatal. The
   CMake guard at `CMakeLists.txt:476-479` only forces the *compile-time* option on; the runtime
   pair is guarded solely by this abort.

This also silently weakens the D14/D18 A/B the whole Track H measurement rests on: the lever the
integrator will drive `HandleRecycleScenario`'s three arms with is exactly this pair of env vars,
and one wrong combination produces a green, empty run rather than a red one.

**What would settle it** (not applied — this is a review): raise the fatal where the harness can
see it (before EGL bring-up, or as a startup validation the scenario fixture reports as a failure
rather than a skip), or make the skip message name the knob; plus one always-on ctest entry that
asserts the combination is rejected *visibly*.

---

## MAJOR 2 — the D13 "must not break" evidence does not exercise the re-keyed code: four of the five tests C.2 names make **zero** slot acquisitions

C.2's *Must not break* clause is explicit: "the D13 list. **Each item has a named existing test**",
and then names `SanityTest.cpp:2575-2596` (scratch-FBO scrub), `:2650-2665`, `:2757-2790` (the three
context-generation guards on texture / framebuffer / renderbuffer twins) and `:3020-3062`
(sampled-set staleness). The result file's §2 MAJOR-1 section concludes that these now run on the
handle arm: *"All 93 cases pass on the handle arm … together with the scratch-FBO scrub, the three
context-generation guards and the sampled-set staleness walk."*

Measured, per test, on `build-push` (which the §0 probe confirms resolves the handle arm):

```
$ for T in DirectGLESStateGuards.ScratchFBOTextureDeletionForcesFullScrub \
           DirectGLESBackendTexture.DestructorDeletesIdAndScrubsBindingCache \
           DirectGLESBackendFramebuffer.DestructorDeletesIdAndScrubsBindingShadow \
           DirectGLESBackendRenderbuffer.DestructorDeletesId \
           DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture \
           'DirectGLESStateGuards.*'; do
    gdb -q -batch -ex 'break MobileGL::MG_Pipe::MGPipeSlotAllocator::Acquire' -ex 'ignore 1 100000000' \
        -ex run -ex 'info breakpoints' \
        --args ./build-push/MobileGL/MG_Test/SanityTest --gtest_filter="$T"; done
```

| test (HEAD line) | `MGPipeSlotAllocator::Acquire` hits |
|---|---|
| `DirectGLESStateGuards.ScratchFBOTextureDeletionForcesFullScrub` (`SanityTest.cpp:2623`) | **0** |
| `DirectGLESBackendTexture.DestructorDeletesIdAndScrubsBindingCache` (`:2681`) | **0** |
| `DirectGLESBackendFramebuffer.DestructorDeletesIdAndScrubsBindingShadow` (`:2795`) | **0** |
| `DirectGLESBackendRenderbuffer.DestructorDeletesId` (`:2823`) | **0** |
| `DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture` (`:3047`) | 2 |
| `DirectGLESStateGuards.*` (whole suite) | **0** |

The reason is visible in the sources: those cases build their twins with
`MakeShared<TextureImpl::BackendTextureObject>()` directly (`SanityTest.cpp:2696`, `:2711`) or drive
`ScratchFBOImpl` under `ScopedStateGuardMocks` (`:2623-2645`) — they never go through
`StateBackendObjectRegistry`, so they compile and pass **identically on both arms** and cannot go
red for a defect in `BackendSlotTable`. In the whole binary the registry's `GetOrCreate` is reached
from exactly two call sites (`SanityTest.cpp:369`, `:413`) plus the two inside
`SyncTextureObjectToBackend` at `:3047` — the four `Acquire` hits the result file cites.

Consequences that matter for "complete":

* **All four acquisitions are kind `Texture`.** The switch-over of the other five kinds —
  `Framebuffer`, `Renderbuffer`, `SamplerCso`, `ShaderCso`, `VertexElementsCso`
  (`Managers.cpp:2837`, `:5165`, `:5918`, `:6229`, `:8747`, `:8859`) — has **no unit coverage at
  all** on the handle arm. The 11 `DirectGLESSlotTable.*` cases exercise `BackendSlotTable`
  directly on the throwaway kinds `Query`/`Fence` (`SanityTest.cpp:3213-3220`), i.e. they never
  test that `StateBackendObjectRegistry` dispatches to it for a real kind, and they would pass
  unchanged if the six registries had never been re-keyed. The single case that does test the
  dispatch, `TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm` (`:3520`), asserts only that the
  *arm flag* is set.
* **The `ScopedDirectGLESTextureBindings` save half is never exercised populated.** The fixture is
  used at `:337`, `:388`, `:2683`, `:3049`; in each the *outer* `g_backendTextureObjects` is empty
  at construction, so `previousRegistry` (`:208`, `:243`) is always a copy of an empty slot table
  and the restore at `:226` always restores nothing. D13 lists that fixture as a must-not-break
  item precisely because "the slot table needs the same copy-assign-and-restore shape"; the shape
  that is actually at risk (a populated table saved, reset, repopulated, restored — which is the
  case debt #2 in the result file says is *broken*, slot reuse between reset and restore) is the
  one nothing drives.
* Round-2 MAJOR 1 was "the D13 pins do not execute the `{slot, gen}` arm". `3e59a856` fixed the
  *arm selection*; it did not make the named pins touch the arm. The result file presents the
  4 == 4 `EnsureProcessTeardownSentinel`/`Acquire` equality as proof that "every one of the four
  twin-registry entries the must-not-break flows make … went down the `{slot, gen}` arm". That
  sentence is true and irrelevant: there are only four twin-registry entries in the binary and
  none of them is made by the scratch-FBO scrub or by any of the three context-generation guards.

The integration lane (446/446 × 3 arms) and the 39 retraces *do* exercise all six kinds on the
handle arm, and that is real coverage — but it is end-to-end coverage of a monolith where both
arms are behaviourally equivalent by construction, with no assertion anywhere that a
`{slot, gen}` miss/stale/ABA case behaves as designed for the five uncovered kinds. G8
(`HandleRecycleScenario`, the one gate that would have covered exactly this) does not exist at
this tree, so nothing else fills the hole either.

---

## MAJOR 3 — `e2` is landed after `e3`, for two kinds out of six, and the garbage collector D13 retires is still the handle arm's primary death signal

Brief §E, risk row *"A slot table has no garbage collector …"*, mitigation, verbatim: **"C's `e2`
lands the explicit `delete_*` notification *before* `e3` switches the tables over, and `e3` is not
merged without it."** C.2 orders the steps `e1 → e2 → e3 → e4 → e5` for the same reason.

Landed order (`git log --oneline p2/contract..HEAD`, oldest last):

```
994730a7 [Fix] (Espryt): pick the unit-bindings debounce by the runtime arm …
103cafed [Fix] (Espryt): tell the backend when a frontend object dies …      <- e2
3e59a856 [Test] (Espryt): run the sanity binary on the twin arm …
5f245ac7 [Test] (Espryt): pin the churn-driven sweep cadence …
d89fb684 [Fix] (Espryt): sweep the twin table on object churn again …
d714600a [Fix] (Espryt): do not return a reference through a null slot pointer …
1e0f4d6d [Test] (Espryt): pin the twin table identity contract …
fbdaeef3 [Refactor] (Espryt): key every backend twin on {slot, gen} …        <- e3
dc9b7237 [Feat] (Espryt): give the backend a dense {slot, gen} twin table …  <- e1
```

`e2` is commit 8 of 9; `e3` is commit 2. And `e2` covers two of six kinds:

```
$ grep -rn 'NotifyStateObjectDestroyed' MobileGL/MG_State
MG_State/GLState/ProgramState/ProgramObject.cpp:38          (kind ShaderCso)
MG_State/GLState/RenderbufferState/RenderbufferObject.cpp:38 (kind Renderbuffer)
```

`Texture`, `Framebuffer`, `SamplerCso` and `VertexElementsCso` still discover death only through
the sweep, and all seven `CollectGarbageIfNeeded` call sites D13's replacement table says are
retired are still there:

```
$ grep -n 'CollectGarbage' MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
1275, 1715, 2080, 2081, 2082, 2910, 2911
```

What actually shipped on the *handle* arm is not "a slot table with no GC and an explicit destroy";
it is a slot table that carries **two** garbage-collection drivers of its own — the draw tick
(`SlotTables.h:260-266`, `kGCInterval = 1024`, `:317`) and a creation tick
(`:127-130`, `kCreationGCInterval = 64`, `:321`) — plus a `std::weak_ptr` to the **frontend** state
object in every entry (`SlotTables.h:100`) as the liveness signal. That is the mechanism D13 and
`ARCHITECTURE.md:363` say Track H deletes, re-implemented on the new key. Under a real split the
server side cannot hold a `weak_ptr` to a client object at all, so this is not a shape that
survives P5+; `ForEachLive` (`SlotTables.h:284-292`) even hands a frontend `SharedPtr` back to
backend code at `DirectGLES.cpp:6659-6664`.

I accept the engineering argument in `SlotTables.h:50-57` (dropping the sweep before the notice
covers all six kinds would have made memory behaviour strictly worse than the map it replaces) —
it is the right call *given* that `e2` was not finished. But the brief's §E rule is a decision, and
"where the brief and the tree disagree … the brief wins for decisions". As landed, `ROADMAP.md:18`'s
P2 deliverable "删 `TwinLookupMemo`×3 / `OwnerEquals` / `g_fbSlotCache` / **GC**" is delivered for
the first three and not for the fourth, and the four missing destructor calls sit in files C.5
gives to packages B and D — so this cannot be closed inside package C at all. It is an integrator
decision with a cross-package dependency, which is precisely why §E made it a merge rule. It has to
be raised as a major and waived deliberately, not carried as a footnote.

---

## Minors

1. **`SlotTables.h:331-336`'s justification for the one-entry memo is factually wrong for two of
   the three paths it names.** It claims "the three per-draw resolution paths (`ResolveVaoTwin`,
   `SyncCurrentProgram`, `BindCurrentFBO`) ask the SAME table for the SAME object every draw".
   `BindCurrentFBO` is called for **both** targets in one frame (`DirectGLES.cpp:6276-6277`, and
   again at `:7021`, `:7116`, `:9347`), and `ResolveUnitSamplerBackend` (`:3384-3410`, called per
   texture unit from `:3456` and `:3859`) asks the sampler table for a *different* object per unit.
   A single-entry `lifetimeId → handle` memo (`SlotTables.h:190-194`, `:345-346`) thrashes in both,
   and each miss is an `UnorderedMap<Uint64,Uint32>` probe (`SlotAllocator.cpp:97-105`) that P1 did
   not pay: `ResolveUnitSamplerBackend`'s pre-P2 fast path was a pure `OwnerEquals` on a *per-unit*
   memo with no probe at all, and `BindCurrentFBO`'s was a direct-mapped 6-slot array. Measured on
   `*CrossFrameBufferScenario.*` (`build-push`, gdb breakpoint counts): `FindByLifetimeId` **42**,
   `ResolveVaoTwin` 64, `BindCurrentFBO` 156 — small here, but the shape scales with bound units,
   and G11 (the gate that would catch it) is device-only and owed. Either fix the comment or make
   the memo per-unit/per-target.
2. **`HandleOf` caches negative answers.** `SlotTables.h:191-194` calls `RememberHandle(lifetimeId,
   handle)` unconditionally, including when `FindByLifetimeId` returned `kMGPipeNullHandle`, so the
   memo can hold "this object has no handle" and keep answering that until another lifetime id
   displaces it. It is safe today only because `GetOrCreate` refreshes the memo at `:160`; nothing
   pins that, and none of the 11 cases drives the negative-cache path.
3. **Cadence divergence between the arms, contradicting `SlotTables.h:319-321`** ("Same value the
   map arm uses, so the two arms sweep at the same cadence under the same workload"):
   `BackendSlotTable::CollectGarbageIfNeeded` zeroes `m_creationTick` (`:264`) and
   `CollectGarbageNow` does too (`:269`); the legacy `StateBackendObjectRegistry::CollectGarbageIfNeeded`
   (`Managers.h:455-467`) zeroes only `m_gcTick`. The value is the same; the cadence is not.
4. **`GetOrCreate(nullptr)` is not arm-equivalent.** The handle arm does `m_nullTwin.reset()` on
   *every* null call (`SlotTables.h:113-121`), so a second null call destroys the twin the first one
   handed back; the legacy map kept the null-keyed entry until a sweep.
   `DirectGLESSlotTable.GetOrCreateToleratesANullStateObject` (`SanityTest.cpp:3441`) only makes one
   call, so the difference is untested. (Note also §4.9's correction is right: the shipping path
   still asserts on null at `Managers.h:319`, so the case pins the table's contract, not
   production's.)
5. **`ForEachLive` iterates with a range-for while `fn` runs arbitrary backend code**
   (`SlotTables.h:286-291`); a nested `GetOrCreate` on the same table would `resize` `m_slots`
   (`:304`) and invalidate the iterator. The only caller (`DirectGLES.cpp:6655-6664`) happens not to
   insert. The legacy walk had the same hazard, so this is carried over rather than introduced —
   but the re-key was the moment to close it, and `ReclaimDeadSlots` (`:202-222`) shows the index
   loop that does close it.
6. **The backend mints client handles and still depends on frontend object lifetime.**
   `SlotTables.h:132-133`, `:187-195`, `:218`, `:255` call `MG_Pipe::MGPipeSlots()` from inside
   `MG_Backend/`, against `MGPipeHandles.h:13-16` ("minted by the CLIENT and never by the server"),
   and `SlotTables.h:15` pulls the client header `MG_Impl/Pipe/SlotAllocator.h` into a backend
   header. `check_include_closure.py` probes only the value/artifacts/mutation/wire headers
   (re-run: `4 probes`), so nothing gates a second instance. The result file declares this
   (`SlotTables.h:59-66`, debt #4) and D13 arguably sanctions the file layout, so it is a minor —
   but it should be an explicit P3 entry in `MEASUREMENTS.md`, not only a header comment.
7. **The two cross-package slot hazards are declared but not pinned in the direction that matters.**
   Debt #2 (a table reset between save and restore leaks its slots, and an intervening `Acquire`
   can hand the same slot out at a higher `Gen`) and debt #3 (two holders of one kind orphan each
   other's entry) are both correctly analysed in `espryt-v3.md` §6, and
   `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin` (`SanityTest.cpp:3341`) documents #3 in a
   comment — but no case asserts the failure mode, so a later change that makes it *worse* is not
   detectable. Package D's subsystem 4 keys `VaoDrawMemo` out of the same `VertexElementsCso` kind
   (D12.4), so the integrator must decide this, not defer it.
8. **`EnsureProcessTeardownSentinel()` did not move where D13 says.** D13: "The arming site moves to
   the slot table's first insertion." It is still in `StateBackendObjectRegistry::GetOrCreate`
   (`Managers.h:326`), ahead of the arm dispatch — so it also arms on a null-object call, and a
   `BackendSlotTable` used outside the registry (as the 11 unit cases do) never arms it. Harmless
   today, and arguably better than what D13 asked for, but it is an undeclared deviation.
9. **`EsprytSlotArmEnvironment` duplicates ConfigLoader's default rather than calling it**
   (`SanityTest.cpp:74-87` vs `ConfigLoader.cpp:254`). If `kMGPipeSubsystemsMigratedAtP2` or the
   push-build default changes in P3, the two silently diverge and `SanityTest` goes back to
   testing an arm nothing ships. One assertion that the two agree would close it.
10. **G4 is half-run.** The package ran `ctest -L integration-verify` (818/818) but not the verify
    *retrace* half of G4 (`MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … build-verify/libMobileGL.so`,
    then the two `grep`s over `mobilegl.log`). §3.6 does not list G4 among the unreachable gates, so
    the omission is undeclared. It is cheap from this tree and would have closed the
    "every case armed" half.
11. **`retrace_gate.py` really has no `--backend`/`--ssim`** (`--help`: `--tree --lib --out -j
    --only`), confirming §4.8; section A's G3 row and C.2/C.3's verification blocks need the fix.
12. **G1's admitted resize set must be widened by `_GLOBAL__sub_I_DirectGLES.cpp` (−9)**, which is
    the contract's, not C's — reproduced above (contract→HEAD is `0 resized`). D15 point 4 and C.0's
    verification block name only three symbols; the integrator has to record the fourth or G1 reads
    as failed after every merge.
13. **Four `MG_State/GLState/` files outside C.2's *Files* line** (`ProgramObject.cpp`,
    `RenderbufferObject.{h,cpp}`, new `StateObjectDeathNotice.h`). C.5 does give them to nobody, so
    no ownership line is crossed and the departure is declared (§4.1) — recorded here only so the
    integrator confirms the reading rather than discovering it at rebase.
14. **The `#if MOBILEGL_PIPE_PUSH` nested inside `#if MOBILEGL_PIPE_PUSH`** in
    `ScopedDetachedTextureFramebufferAttachments` (`DirectGLES.cpp:6656` inside the block opened at
    `:6650`) is dead nesting; harmless, but it makes the three-arm reading of that function harder
    than it needs to be.

---

## Things the result file claims that I checked and found accurate

Recorded so the integrator does not re-litigate them: §3.1's four G1 resizes and the zero
contract→HEAD delta; §3.1's G5 triple-hash equality; the legacy-residue claim (I read every hit of
`OwnerEquals|TwinLookupMemo|g_fbSlotCache` in `DirectGLES.cpp` — 69, 89, 93, 128-137, 152-184,
1255-1263, 2964-2972, 3084-3089, 3406 are all inside `#if MOBILEGL_PIPE_LEGACY_MEMOS`, the `#else`
of `#if MOBILEGL_PIPE_PUSH`, or `#if !MOBILEGL_PIPE_PUSH || MOBILEGL_PIPE_LEGACY_MEMOS`;
`GetFramebufferBindingSlotFast` has 0 hits tree-wide); §5.3's `ctest -N` extraction bug (`grep -E
'^\s+Test #'` does drop four-digit entries — I used the `sed` form throughout); §4.6 (only three
`pDefaultFramebufferInfo->defaultFBO` compares remain, at `:2120`, `:3070`, `:3114`; the fourth the
brief lists at `:6420` was already `IsDefaultFramebuffer()` at the contract, so the brief was wrong
about it, not the tree); and §4.5's equivalence argument for the lifetime-id snapshot
(`LifetimeIdOf(nullptr) == 0` and lifetime ids are never reused, so it is at least as strong as
`OwnerEquals`).

---

## Bottom line

The package builds clean on four configurations, holds G1/G2/G5/G13/G14 exactly, is green on
1500×4 unit, 446×3 integration, 818 integration-verify and 39 retraces, and round 2's MAJOR 1 is
genuinely fixed (4/4 `Acquire`). What it does not have is (1) a knob path that fails loudly instead
of greening a whole lane, (2) any test that can go red for the switch-over of five of the six
re-keyed kinds, and (3) the `e2` deliverable the brief makes a precondition of `e3`. Those are the
three majors. Approve after they are addressed or explicitly waived by the integrator with the
cross-package work (B's and D's four destructor calls, E's `HandleRecycleScenario`) scheduled.
