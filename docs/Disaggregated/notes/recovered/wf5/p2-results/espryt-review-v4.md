# Adversarial review — package `p2/espryt` (brief C.2), result file `espryt-v4.md`

Reviewer: adversarial pass, round 4. Tree `~/w7/p2-espryt`, branch `p2/espryt`, HEAD **`a859ff71`**,
base tag `p2/contract` (`9c6a8a25`). Everything below was re-run by me from that tree; nothing was
taken on the result file's word. **No file was edited and no source-patching negative control was
applied**, so NC5–NC7 in `espryt-v4.md` §3.4 are the implementer's evidence, not mine — see §4.
The tree was left exactly as found: `git status --porcelain` → `?? build-nolegacy/` only, and
`git diff --summary refs/tags/p2/contract..HEAD` → the two `create mode 100644` lines and **no mode
change**.

**Verdict: NOT APPROVED — 1 major.**

The three round-3 majors are genuinely closed, each for the reason it was raised, and every gate
this tree can reach still holds (I re-ran all of them). The one major is a spec violation the round-4
fix for MAJOR 3 *introduced*: package C now edits nine files `C.5` assigns to packages **B** and
**D**, seven of which are the exact files `D4`'s aggregate-generation table tells B to edit.

---

## 0. The three round-3 majors, re-run with their own reproduction commands

### MAJOR 1 (round 3) — CLOSED. The armless knob pair now stops the lane instead of skipping it.

v3's command, verbatim, from `~/w7/p2-espryt` with `MOBILEGL_BACKEND_TYPE=DirectGLES` and
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json`:

```
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario' --no-tests=error
 1/13 Test #1514: DirectGLES.CrossFrameBufferScenario.VertexBufferSubData ... Subprocess aborted***Exception
 …
0% tests passed, 13 tests failed out of 13          # v3: 100% tests passed, 13 (Skipped)
rc=8
```

and the single-binary form now brings the harness *up* before it stops:

```
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ./build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest \
    --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData
[itest] pre-flight child: attempting a full EGL bring-up
MobileGL integration scenarios: backend=DirectGLES
  renderer: Espryt (MobileGL Core) (llvmpipe (LLVM 22.1.6, 256 bits), OpenGL ES 3.2)
[ RUN      ] CrossFrameBufferScenario.VertexBufferSubData
binary exit=134
$ MOBILEGL_PIPE_PUSH=0 ./build-push/…/MobileGLIntegrationTest --gtest_filter=…   # control
[  PASSED  ] 1 test.
```

The mechanism is right, not routed around: `InitDisplayAndContext()` now calls
`DiagnoseEsprytSlotArm()` (`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:10238`), which logs at
ERROR and **returns** (`Managers.cpp:238-246`), so the forked pre-flight child exits 0 and the
harness comes up; the `std::abort()` stays in `ResolveEsprytSlotTablesArm()`
(`Managers.cpp:262-276`), which the inline latch `EsprytSlotTablesEnabled()`
(`SlotTables.h:109-112`) reaches at the first twin lookup, inside a scenario body. That is the
thing that was wrong being made right.

Colour for the integrator: over the *whole* DirectGLES lane the armless pair is red but not
uniformly — `17% tests passed, 369 tests failed out of 446`, 44 skipped, 33 green (the scenarios
that never look a twin up). The lane fails, which is what `ROADMAP.md:7` asks for.

### MAJOR 2 (round 3) — CLOSED. All six re-keyed kinds now have a case that can go red.

My own gdb counts (`-batch`, `break MobileGL::MG_Pipe::MGPipeSlotAllocator::Acquire`,
`ignore 1 100000000`, `info breakpoints`), on `build-push/MobileGL/MG_Test/SanityTest`:

| filter | `Acquire` | `ResolveEsprytSlotTablesArm` |
|---|---|---|
| `DirectGLESSlotTable.EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` | **12** | 1 |
| `-DirectGLESSlotTable.*` (the other 82 cases) | 4 | 1 |
| `DirectGLESStateGuards.ScratchFBOTextureDeletionForcesFullScrub` | **0** | 0 |
| `DirectGLESStateGuards.*` | **0** | 0 |
| `DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture` | 2 | 1 |

12 = six kinds × two objects each, and `ExpectTheHandleArmDrivesThisKind`
(`MobileGL/MG_Test/SanityTest.cpp:3597-3643`, driven at `:3648-3671`) goes through the **real
registry globals** — `TextureImpl::g_backendTextureObjects`, `FramebufferImpl::…`,
`RenderbufferImpl::…`, `SamplerImpl::…`, `PrgramImpl::g_backendProgramObjects`,
`VertexArrayImpl::…` — i.e. through `StateBackendObjectRegistry`'s arm dispatch at
`Managers.h:327-331`. Its first assertion is `ASSERT_FALSE(MGPipeHandleIsNull(firstHandle))`
(`SanityTest.cpp:3608`), and `Managers.h:400-406` makes the legacy arm answer
`kMGPipeNullHandle`, so a kind that fell back to the map fails **by name**. It also pins that the
destructor notice alone frees the slot with no sweep (`:3620-3623`) and that `Gen` moves on reuse
(`:3630-3636`).

I also confirmed the arm the *integration* lane actually runs, which no previous round measured:
gdb on `build-push/…/MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData`
gives `Acquire` **5 hits** / `ResolveEsprytSlotTablesArm` 1 hit by default, and **0** `Acquire`
hits under `MOBILEGL_PIPE_PUSH=0`. The 446 × 3 green really is a two-arm A/B.

Residual (minor 6 below): the five D13 "must not break" tests C.2 names still make **zero**
acquisitions, so C.2's "Each item has a named existing test" is satisfied for the switch-over by
the new case and not by the named ones.

### MAJOR 3 (round 3) — CLOSED in substance. All six kinds announce; both sweep drivers are gone.

```
$ grep -rn 'NotifyStateObjectDestroyed' MobileGL/MG_State
ProgramState/ProgramObject.cpp:43           (ShaderCso)
VertexArrayState/VertexArrayObject.cpp:52   (VertexElementsCso)
SamplerState/SamplerObject.cpp:38           (SamplerCso)
FramebufferState/FramebufferObject.cpp:35   (Framebuffer)
TextureState/TextureObject.cpp:39           (Texture, on ~TextureObjectBase)
RenderbufferState/RenderbufferObject.cpp:40 (Renderbuffer)
GLState/StateObjectDeathNotice.h:55         (the entry point)

$ grep -n 'kGCInterval\|kCreationGCInterval\|m_gcTick\|m_creationTick' MobileGL/MG_Backend/DirectGLES/SlotTables.h
(none)
```

`BackendSlotTable::CollectGarbageIfNeeded()` is an empty body (`SlotTables.h:276`) and the seven
`CollectGarbageIfNeeded` call sites (`DirectGLES.cpp:1275, 1715, 2080, 2081, 2082, 2910, 2911`)
reach it only after `Managers.h:462-465`'s `if (EsprytSlotTablesEnabled()) { … return; }`. The
consumer is one switch over all six kinds (`Managers.cpp:184-214`) installed from
`ResolveEsprytSlotTablesArm()` (`Managers.cpp:291`). I checked the class hierarchy for holes: every
concrete texture derives from `TextureObjectBase` (`TextureObject1D/2D/3D/2DCube/Buffer/View`, via
`TextureObjectMipmap`/`TextureObjectWithOneMipmap`), and `FramebufferObject`, `SamplerObject`,
`ProgramObject`, `RenderbufferObject`, `VertexArrayObject` have no subclasses — so the firing side
covers every object that can be twinned.

The §E ordering rule (`e2` before `e3`) is still violated in the branch's history (`e3` is commit 2
of 13, `e2` completes at 11). I accept `espryt-v4.md` §4.4's reading: what §E protects against —
`e3` merged with a table that has no GC and no explicit destroy — is false at HEAD, so this is an
ordering deviation on a squash-merge and not a missing mechanism. Recorded for the integrator, not
re-raised.

---

## MAJOR — the round-4 fix for MAJOR 3 puts package C into **nine** files `C.5` assigns to B and D, seven of which are the exact files `D4` tells B to edit

`C.5` states the invariant it exists for, verbatim: *"No two packages edit the same file. … so the
integrator's rebase never sees an edit on both sides of a file."* Its rows give
`MobileGL/MG_State/GLState/{… VertexArrayState/VertexArrayObject.cpp, FramebufferState/*,
TextureState/*, BufferState/*, SamplerState/*}` to **B**, and
`MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h` to **D**. C.2's *Files* line for
this package is `MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.h, Managers.cpp}`, new
`SlotTables.h`, and `MG_Test/SanityTest.cpp`.

Reproduction:

```
$ cd ~/w7/p2-espryt && git diff --name-only refs/tags/p2/contract..HEAD
MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
MobileGL/MG_Backend/DirectGLES/Managers.cpp
MobileGL/MG_Backend/DirectGLES/Managers.h
MobileGL/MG_Backend/DirectGLES/SlotTables.h
MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.cpp     <- B
MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h       <- B
MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp             (nobody)
MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp   (nobody)
MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.h     (nobody)
MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp             <- B
MobileGL/MG_State/GLState/SamplerState/SamplerObject.h               <- B
MobileGL/MG_State/GLState/StateObjectDeathNotice.h                   (new, nobody)
MobileGL/MG_State/GLState/TextureState/TextureObject.cpp             <- B
MobileGL/MG_State/GLState/TextureState/TextureObject.h               <- B
MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.cpp     <- B
MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h       <- D
MobileGL/MG_Test/SanityTest.cpp
```

This is not a theoretical collision. `D4`'s "five aggregate generations" table names, as B's bump
points, `VertexArrayObject.cpp:299,305,311`; `FramebufferObject.cpp:192,203,215`;
`TextureObject.cpp:285,354,365` and `:101,126,140,154,203,210,227,238,258,294,303`;
`TextureObject.h:182`; `SamplerObject.cpp:27-35`. Those are **five of the seven B-owned files C now
edits**, and D's `m4` deletes `SetBackendStateMemo`/`SetBackendAuxMemo`/`SetBackendHashMemo` and
their storage from `VertexArrayObject.h:106-150, :190-197` — the ninth file. `D.1` lands
**contract → tracker(B) → magma(D) → espryt(C) → gates(E)**, so C rebases *onto* B's and D's edits
in all nine, which is precisely the situation C.5 was written to make impossible.

What C actually wrote in those files is minimal and confined — I read the whole diff: one
`#include "…/StateObjectDeathNotice.h"`, one out-of-line destructor under `#if MOBILEGL_PIPE_PUSH`
and its declaration, and for `TextureObject.h:116-127` an `#if/#else` around the existing
`virtual ~TextureObjectBase() = default;`. Nothing else is touched, and G1 is unaffected
(contract→HEAD pull-build delta is `0 added, 0 removed, 0 resized, 0 renamed`, `.text +0`).

`espryt-v4.md` §4.1 declares the departure and asserts that "the rework instruction for this round
explicitly grants those four destructor sites to package C and states that no other package will
touch them in this round", and asks the integrator to confirm. **I cannot verify that grant** — it
is not in `BRIEF-P2.md`, which is the specification I was pointed at, and an assertion inside a
result file is not the specification. Round 3 accepted the analogous departure as a *minor* because
C.5 gave `ProgramState/*` and `RenderbufferState/*` to **nobody**; that argument does not carry to
these nine, and §4.1 itself contradicts `D.1` by recommending B and D land *after* C.

By the standard round 3 applied to §E (a brief decision is a decision, and has to be waived
deliberately rather than carried as a footnote), this is a major. It is cheap to resolve and does
not require code changes: the integrator either (a) confirms the grant on the record and re-orders
`D.1` to **contract → espryt(C) → tracker(B) → magma(D) → gates(E)** so B's and D's rebases see C's
destructors, or (b) moves the six destructors into B's and D's packages. What must not happen is
merging under `D.1`'s stated order on the assumption that C.5 still holds.

---

## Gates I re-ran, and what they printed

All from `~/w7/p2-espryt` at `a859ff71`, `CCACHE_BASEDIR=/home/swung/w7`.

| gate | command | observed |
|---|---|---|
| build | `cmake --build {build-linux,build-push,build-verify,build-nolegacy} -j 12` | all `rc=0`. Cache: `build-verify` shows `MOBILEGL_PIPE_PUSH:BOOL=OFF` but `CMakeLists.txt:469-471` forces it ON for the configure, so it *is* a push build |
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`. The four: `RenderState::RenderState()`, `SetCapability`, `IsCapabilityEnabled`, `_GLOBAL__sub_I_DirectGLES.cpp` |
| **G1 attribution** | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed`, `.text 10792739 -> 10792739 (+0)` — C's pull-build delta is exactly zero, including the six new `MG_State` destructors |
| **G5** | `git show {48268068, refs/tags/p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…220efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty, `rc=1` |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, `rc=0` |
| **G13** | `gen_pipe.py --check` / `--self-test` | `rc=0` / `rc=0` |
| **G13 stdio** | the `pipe-gates` alternation verbatim (`test.yml:1515-1522`) over `MG_Backend` + `MG_State` | **0 hits** (`rc=1`) — v3's nine "puts"-in-a-comment hits are gone |
| **G2** | width-tolerant `ctest -N` extraction + `LC_ALL=C sort`, ×4 dirs, then `diff` | `build-linux` **2380**, `build-push` **2380**, `build-nolegacy` **2380**; both diffs empty. `build-verify` 3198 |
| **G14** | `LC_ALL=C comm -23 <sorted baseline> <pull names>` | **0** removed. Added: 4 contract placeholders + **13** `DirectGLESSlotTable.*` |
| unit × 4 | `ctest --test-dir <d> -L unit --no-tests=error -j 6` | `100% tests passed, 0 failed out of 1502` in **all four** dirs |
| slot cases × 4 | `ctest -R DirectGLESSlotTable --no-tests=error` | 13/13 in `build-push` and `build-verify` (0 skipped); 13 present and all skipping in `build-linux`; 13/13 in `build-nolegacy` with `AnArmlessKnobCombination…` skipped |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **446/446**, 50 (Skipped) |
| integration, legacy arm | same, `MOBILEGL_PIPE_PUSH=0` | **446/446**, the same 50 skips (so the skip set is not arm-dependent) |
| integration, no legacy compiled | `ctest --test-dir build-nolegacy …` | **446/446** |
| integration, verify build | `ctest --test-dir build-verify -L integration-gpu -j 4 -R DirectGLES` | **855/855** |
| **G4 (a)** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **818/818**; over the ten `build-verify/MobileGL/MG_IntegrationTest/*.log`, `Fatal{PipeVerifyDiffer\|UnmigratedPipeInput\|PipeResidualDiverged}` = **0 each**, `Fatal{` = 0 each |
| **G3** (DirectGLES half) | `retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/rev4-push -j 4 --only 'DirectGLES$'` | `rc=0`, **`passed 39 / 39; failed: []`** |
| **G4 (b)** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … --lib $PWD/build-verify/libMobileGL.so --out ~/w7/retrace-out/rev4-verify …` | `rc=0`, **39/39**; of the 39 `*/output/mobilegl.log`: **0** with `Fatal{`, **0** with `PipeVerifyDiffer\|UnmigratedPipeInput\|PipeResidualDiverged`, **0** unarmed |
| sanity | `SanityTest`, and `--gtest_shuffle --gtest_random_seed={11,22,33}` | `[ PASSED ] 95` in all four runs; each of the four new cases also passes in isolation |
| commits | `git log p2/contract..HEAD --format=%B \| grep -i 'co-authored\|claude'` | empty (`rc=1`); the four new subjects are `[Type] (Scope): …` + blank line + `- ` bullets |
| file modes | `git diff --summary refs/tags/p2/contract..HEAD` | two `create mode 100644` lines only, **no mode change** |

Gates the tree cannot reach, re-probed at HEAD and confirmed absent (all owned by A/B/E, so not
counted against C, and `espryt-v4.md` §3.5 declares them correctly): **G6/G7** — the only
`RenderStateSpans` entry is `…PlaceholderUntilTheOwningPackageFillsThisIn` and
`scripts/g7_negative_control.sh` does not exist; **G8** — `ctest -N | grep -c HandleRecycle` is
**0** in all four dirs; **G9** — `gen_pipe_dirty_surface.py --check` → `error: unrecognized
arguments: --check`; **G10** — `MGL_RESIDUAL_BLOCK_SIZE 8` is pinned at `MGPipeTypes.h:546` but the
only `Residual` ctest entries are the contract's `PipeCatalogue.*`; **G11**, **G12**, and G14's CI
half. So `1502×4 + 446×3 + 855 + 818 + 39 + 39` is **not** a P2 acceptance pass.

---

## Minors

1. **`CollectGarbageNow()` has no production caller on either arm, so `SlotTables.h:51-54`'s
   "backstop" is unsupported.** `grep -rn 'CollectGarbageNow\|ReclaimDeadSlots' MobileGL/` outside
   `SlotTables.h` returns only `Managers.h:477,480` (the forwarder itself) and four `SanityTest.cpp`
   sites (`:3238, :3308, :3383, :3384`). The header keeps the per-entry `std::weak_ptr` partly
   because `ReclaimDeadSlots()` "is kept as the body of the EXPLICIT `CollectGarbageNow()` … the
   backstop for the one case the notice cannot cover"; nothing in the library ever asks for that
   collection, so the backstop cannot run. Harmless in P2 (the uncovered case is process teardown),
   but the justification should say "test-only" or a caller should exist.
2. **The two-holder hazard got strictly worse this round, and the test that documents it depends on
   the call production no longer makes.** `OnFrontendStateObjectDestroyed` (`Managers.cpp:184-210`)
   dispatches a death notice to exactly **one** registry per kind, and
   `BackendSlotTable::DestroyByLifetimeId` (`SlotTables.h:248-269`) returns `false` without freeing
   when this table does not hold the slot. Before `e04c5c3e` the other holder's entry was reclaimed
   by its own tick sweep; now `CollectGarbageIfNeeded()` is empty and nothing calls
   `CollectGarbageNow()`, so that holder keeps a live entry — and the twin's driver storage — for
   the life of the process. `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin` itself has to
   call `a.CollectGarbageNow(); b.CollectGarbageNow();` (`SanityTest.cpp:3382-3384`) to get the
   slot back. Today the only second holder is a test fixture, but `C.3`'s `m3` re-keys Magma's
   `VaoDrawMemo` out of the same `VertexElementsCso` kind inside P2. Debt #3 in `espryt-v4.md` §6
   describes the hazard but not this consequence of removing the sweep.
3. **`espryt-v4.md` §2 MAJOR-3's claim that `build-nolegacy` "carries no collector at all" is not
   what the binary says.** Only `CollectGarbageIfNeeded`'s tick body is wrapped
   (`Managers.h:467`); the private `CollectGarbage()` at `Managers.h:488` is unguarded, and
   `nm -C build-nolegacy/libMobileGL.so | grep -c 'StateBackendObjectRegistry<.*>::CollectGarbage()'`
   prints **6**, the same as `build-push`. Nothing ticks it, so "delete the GC" still holds in
   substance; the sentence does not.
4. **The new gate pins the function, not the call site.**
   `AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane` (`SanityTest.cpp:3685-3747`) asserts that
   `DiagnoseEsprytSlotArm()` returns and that `ResolveEsprytSlotTablesArm()` dies naming both knobs.
   Nothing fails if `InitDisplayAndContext()` (`DirectGLES.cpp:10238`) is edited back to call
   `ResolveEsprytSlotTablesArm()`, which is exactly the regression that produced the round-3 major.
   The lane-level property is only re-checkable by hand.
5. **That case mutates process-global state on a fixed path.** It writes
   `fs::temp_directory_path() / "mobilegl-espryt-armless-knobs.log"` (`:3706`), calls
   `MG_Util::Debug::Close()` twice and finishes with `UnsetEnvVar("MOBILEGL_LOG_FILE_PATH")`
   (`:3725`) — so an operator who set `MOBILEGL_LOG_FILE_PATH` for the whole binary loses file
   logging for every case after this one, and two `SanityTest` processes on one host race on that
   filename. Also `ASSERT_EQ(CurrentEsprytSlotArmVerdict(), NoArm)` at `:3704` returns **before**
   the `MG_Config::Features` restore at `:3726-3727`, so a failure there would leave the rest of the
   binary on a mutated config.
6. **The five D13 "must not break" tests C.2 names still make zero slot acquisitions** (measured
   above: `DirectGLESStateGuards.*` → 0, scratch-FBO → 0). The round-3 finding is answered by adding
   a new case rather than by making the named pins touch the arm; C.2's "Each item has a named
   existing test" therefore still reads as satisfied by tests that pass identically on both arms.
   Acceptable, but the integrator should know the coverage lives in one case.
7. **`ScopedDirectGLESTextureBindings`'s populated save/reset/restore is still only covered
   synthetically.** `AWholeTableSavesResetsAndRestores` (`SanityTest.cpp:3315-3330`) drives a
   populated `FakeSlotTable` directly; the fixture itself (`:145-197`) is still used with an empty
   outer registry, and debt #2 (slot reuse between reset and restore) has no case that asserts the
   failure mode.
8. **`HandleOf` still caches negative answers unconditionally** (`SlotTables.h:198-206`:
   `RememberHandle(lifetimeId, handle)` runs even when `FindByLifetimeId` answered
   `kMGPipeNullHandle`). Round 4 closed minor 2 by *pinning* the behaviour
   (`RepeatedLookupsOfALiveObjectKeepOneHandle`, `SanityTest.cpp:3266-3275`) rather than changing
   it. Safe within one table because `GetOrCreate` refreshes the memo at `:171`; not safe as a
   general contract once a second holder of the kind can be the one that acquires.
9. **`EnsureProcessTeardownSentinel()` did not move where D13 says** — still
   `StateBackendObjectRegistry::GetOrCreate` (`Managers.h:326`), ahead of the arm dispatch.
   Declared open in `espryt-v4.md` §6.9; recorded so the integrator sees it as a live D13
   deviation, not a closed one.
10. **`StateBackendObjectRegistry::GetOrCreate` still asserts non-null at `Managers.h:320`**, before
    the arm dispatch, so `BackendSlotTable`'s new null tolerance is unreachable through the shipping
    registry in a debug build and `GetOrCreateToleratesANullStateObject` pins the table's contract
    only. Also, the handle arm now parks the null twin forever (`SlotTables.h:149`) where the map
    arm's null-keyed entry was swept — an arm difference in the opposite direction from the one that
    was fixed.
11. **The out-of-line destructors are a push-only ABI/semantics change to four `MG_State` classes.**
    Declaring `~TextureObjectBase`, `~FramebufferObject`, `~SamplerObject`, `~VertexArrayObject`
    under `#if MOBILEGL_PIPE_PUSH` suppresses their implicit move constructor and move assignment in
    the push build only. It compiles today because these are only ever held by `SharedPtr`, and no
    gate looks at pull-vs-push semantics of `MG_State` (G1 covers the pull build alone).
12. **Brief errata, re-confirmed by me at this tree** (all already raised by `espryt-v4.md` §5):
    `retrace_gate.py --help` lists only `--tree --lib --out -j --only` — no `--backend`, no `--ssim`
    (section A G3, C.2, C.3); the string `MGPipe verify:` that G4 greps for exists **nowhere** in
    `MobileGL/` or `scripts/` (the real line is `MGPipe: verify armed - 63 fields, 69 verbs,
    fatal=1`), so G4's arming check as written is vacuous; G4's glob `<out>/*/mobilegl.log` matches
    nothing (the real path is `<out>/<case>/<backend>/output/mobilegl.log`); G1's admitted resize set
    in D15 point 4 / C.0 needs the contract's fourth symbol `_GLOBAL__sub_I_DirectGLES.cpp`; and
    section A's `grep -E '^\s+Test #'` silently drops four-digit test ids.
13. **`kMGPipeSubsystemsMigratedAtP2` is duplicated in the test environment**
    (`SanityTest.cpp:81-87` vs `ConfigLoader.cpp:254`) — round-3 minor 9, deliberately left open;
    it still means a P3 change to either default can silently put `SanityTest` back on an arm
    nothing ships.
14. **Device measurement is still owed** (G11, `D.4.2`/`D.4.3`), and DriverBench was not re-measured
    this round. Declared in `espryt-v4.md` §3.3; the round's only per-draw change is a removal, so
    the omission is defensible, but the P2 exit still needs it.

---

## Claims in `espryt-v4.md` I checked and found accurate

Recorded so the integrator does not re-litigate them: §1's commit list and `git diff --stat`
(17 files, 1854/43); the "no file-mode change" claim; §3.1's four G1 resizes and the zero
contract→HEAD delta; §3.1's G5 triple-hash equality and the 2380/2380/2380 G2 parity; §3.2's
1502 × 4 unit, 446 × 3 integration, 855 integration-gpu on `build-verify`, 818 integration-verify,
39 + 39 retraces and the `MAJOR 1 red proof`; §2 MAJOR 2's 12-vs-4 gdb split (I reproduced both
numbers); §2 MAJOR 3's six announcing classes and the absence of both sweep drivers; §4.6's renamed
test names (none of the old names is in `~/w7/p2-before-ctest-names.txt`, so G14 is untouched);
§5.2 and §5.3's two new brief errata; and §3.5's list of unreachable gates.

I did **not** re-run NC5–NC7: they require patching the tree, and the instruction for this round is
to leave it exactly as found. What I could check without editing — that
`EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` asserts a non-null handle that the legacy
arm cannot produce (`SanityTest.cpp:3608` vs `Managers.h:400-406`), that it skips rather than passes
on the legacy arm, and that the churn case makes no `CollectGarbage*` call — all holds.

---

## Bottom line

Round 4 does what it says: the armless knob pair now takes the DirectGLES lane red instead of green
(`0% tests passed, 13 tests failed out of 13`, was `100% passed, 13 skipped`), all six re-keyed
kinds are walked through the real registries by one case that cannot pass on the legacy arm
(12 `Acquire` hits, was 0 for five of the six), and the twin table's garbage collector is gone with
every one of the six frontend classes announcing its own death. Every gate this tree can reach —
G1, G2, G3, G4 (both halves), G5, G13, G14 — holds, on four build configurations, with the tree left
as found and no file-mode change.

The one thing blocking approval is not code: `e04c5c3e` put package C into nine files `C.5` assigns
to B and D, seven of which are the exact files `D4` tells B to edit, which voids the invariant
`D.1`'s merge order depends on. Approve once the integrator records the grant and fixes the merge
order (or moves the six destructors to their owners).
