# Adversarial review — package `p2/espryt` (brief C.2, D13/D14/D18), result file `espryt-v5.md`, round 5

Reviewer: adversarial pass, round 5. Tree `~/w7/p2-espryt`, branch `p2/espryt`, HEAD **`0f29fa58`**,
base tag `p2/contract` (`9c6a8a25`). Read first: `~/w7/notes/p2/INTEGRATOR-DECISIONS.md` — ID-1 waives
the C.5 ownership of the six destructor sites (round-4's only major), ID-2 re-orders integration,
ID-3 fixes the G1 admitted set, ID-4 corrects the section-A commands. None of those is re-raised.

Everything below was re-run by me. The tree was left exactly as found (`git status --porcelain` →
`?? build-nolegacy/` only; `git diff --summary refs/tags/p2/contract..HEAD` → the two
`create mode 100644` lines, **no mode change**). The three source-patching negative controls were
reproduced in a **source copy** (`~/w7/rev5-nc`, deleted afterwards), never in the tree.

**Verdict: APPROVED — 0 majors, 10 minors.**

The six round-4 minors the rework named are closed for the reasons stated, the closure is
reproducible with my own scenario and my own negative controls, and no gate this tree can reach
regressed.

---

## 0. The six round-4 items, re-run

### (a) two-holder hazard — CLOSED, verified with my own scenario

`~/w7/rev5-espryt/twoholder.cpp` (kept, with its output in `twoholder-run.txt`): a standalone
program compiled with `SanityTest.cpp`'s own compile line and linked against `build-push`'s
`libMobileGL_s.a`. It uses the **real** `TextureImpl::g_backendTextureObjects`, real
`TextureObject2D`s, real `BackendTextureObject` twins over a fake GLES table that counts
`glDeleteTextures`, and the **real** `~TextureObjectBase` notice, with a counting wrapper around the
installed `StateObjectDeathOps` so the number of notices is observed, not inferred. Three holders of
kind Texture — the registry global, a by-value copy of it (`previousRegistry`'s shape), and an
independent `BackendSlotTable<ITextureObject, BackendTextureObject, Texture>` — all twin the same
object; `tex.reset()` then:

```
  notice #1 kind=2 lifetimeId=1
  [ok] reg.FindByHandle(h) == nullptr
  [ok] copy.FindByHandle(h) == nullptr
  [ok] other.FindByHandle(h) == nullptr
  [ok] twinGlobal.expired()
  [ok] twinOther.expired()
  [ok] g_deletes == 2                       # both twins ran glDeleteTextures once
  [ok] slots.LiveCount(kKind) == live0      # the slot went back exactly once
  [ok] MG_Pipe::MGPipeHandleIsNull(slots.FindByLifetimeId(kKind, id))
  [ok] Registry::DestroyByLifetimeId(id) == false
```

Also observed: the fixture window (`saved = reg; reg = {}; …; reg = saved`) — a death inside the
window reaches the saved copy; a twin the restore drops without a `Free` still returns its slot on
the object's death (`slots.LiveCount == live0` afterwards); a transient table unlinks itself
(`HolderCount` back by one). The one `[FAIL]` lines in that output are my own wrong expectation
(`g_notices == 1`): every `TextureObject2D` death raises **two** real notices, `kind=2` (Texture)
and `kind=8` (SamplerCso, the texture's embedded `SamplerObject`, `TextureObject.h:206`) — see minor 3.

gdb on the shipped case (`break MGPipeSlotAllocator::Free`, `…::FindByLifetimeId`, `ignore`,
`info breakpoints`): `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` → `Free` hit **2** (one per
successful notice: `object`, then the `uninvolved` cleanup), `ASavedCopyOfARealRegistry…` → `Free`
hit **1**. One `Free` per notice, never one per holder.

**NC8 (my reproduction, source copy)** — `SlotTables.h:311` `holder = next;` → `holder = nullptr;`:
exactly `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` ("holder a kept the twin", "holder b
kept the twin"), `AWholeTableSavesResetsAndRestores`, `ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice`
("the registry global kept the twin") fail; 15 pass. (`~/w7/rev5-espryt/nc-summary.txt`.)

The result file's correction about Magma is right: `grep -rn 'MGPipeSlots' ~/w7/p2-magma/MobileGL/MG_Backend/DirectVulkan/`
hits only the comment at `MagmaPipeArms.h:170`; there is no cross-backend holder. And each of the six
kinds has exactly one `TwinRegistry<…>` type (`Managers.h:954,1275,1366,1884,1984,2011`), so the
per-type holder list is per-kind in this backend.

### (b) caller-less "backstop" — CLOSED

`grep -rn 'ReclaimDeadSlots\|CollectGarbageNow\|CollectGarbageIfNeeded' MobileGL/ --include=*.h --include=*.cpp | grep -v DirectGLES.cpp`
→ `Managers.h:352` (comment), `:473`, `:492` (the registry's own, both `return` on the handle arm
with nothing to forward to), `SanityTest.cpp:3589` (comment). `SlotTables.h` has no
`ReclaimDeadSlots`, no `CollectGarbage*`, no `m_isCollecting`; the header now says the
teardown-dropped twin is a deliberate leak (`:64-68`).

### (c) call-site pin — CLOSED, and it catches exactly what the lane hides

**NC10 (my reproduction, source copy)** — `DirectGLES.cpp:10238` `DiagnoseEsprytSlotArm();` →
`(void)ResolveEsprytSlotTablesArm();`, rebuilt `SanityTest` **and** `MobileGLIntegrationTest`:

```
[  FAILED  ] DirectGLESSlotTable.EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping (11 ms)
    Result: died but not with expected exit code:  Terminated by signal 6
EGL bring-up under MOBILEGL_PIPE_PUSH with kMGPipeSubsystemEsprytSlots clear and MOBILEGL_PIPE_LEGACY_MEMOS=0 did not RETURN: …
bring-up wrote a Fatal{} … [Linux SanityTest/FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, …
-- same NC10 build, integration lane under the pair:
100% tests passed, 0 tests failed out of 13      skipped=13 aborted=0
```

i.e. with the round-3 regression re-applied, the ctest lane is green-by-skipping and the new
unit case is the only thing that goes red. The case runs in `build-push`, `build-verify` and
`build-nolegacy` (verdict `Handles` there, still must return: passed in isolation) and skips only
in `build-linux` (G2 parity).

### (d) fixed log path / leaked env and config — CLOSED

`ls /tmp | grep -c mobilegl-espryt` after the whole binary → `0`, in every arm run below.
`TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem` passes with and without a pre-set
`MOBILEGL_LOG_FILE_PATH`, in isolation and shuffled. Note for the record: running the full binary
with `MOBILEGL_LOG_FILE_PATH=<file>` set never creates `<file>` — no SanityTest case other than the
armless ones writes a log line (`Log.cpp:63-75` opens the file lazily on the first write) — so the
restore is only observable through the pinning case, which is what it is there for.

### (e) sentinel arming site — MOVED as D13 says

`SlotTables.h:235` arms on the non-null `GetOrCreate` path before `Acquire`; `Managers.h:323-336`
dispatches to the table before the registry's own arming call. Pull-build codegen unchanged (G1
below: contract→HEAD `0 resized`, `.text +0`).

### (f) `HandleOf` negative memo — REMOVED

`SlotTables.h:283`: `if (!MG_Pipe::MGPipeHandleIsNull(handle)) RememberHandle(lifetimeId, handle);`.
My scenario B (real type, two holders): `other.HandleOf(tex2)` misses, `reg.GetOrCreate(tex2)`,
`other.HandleOf(tex2) == h2` → ok. **NC9 (my reproduction)** — memoise unconditionally → exactly
`ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire` fails ("the first holder kept answering the
null handle it cached before the second holder acquired"); 17 pass.

---

## 1. Section-A gates, re-run at `0f29fa58` (`CCACHE_BASEDIR=/home/swung/w7`)

All four build dirs were `ninja: no work to do` at HEAD before anything ran.

| gate | command | observed |
|---|---|---|
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`, `.text +160`; resized = `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — exactly ID-3's set |
| **G1 attribution** | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed`, `.text 10792739 -> 10792739 (+0)` |
| **G5** | `git show {48268068, refs/tags/p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all `d8fd1c48…220efe27` = `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty, rc=1 |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, rc=0 |
| **G13** | `gen_pipe.py --check` / `--self-test` / `symbol_report.py --self-test` | rc=0 / `7 negative-control trip(s), positive control OK` / `OK (2 canned transcripts, 5 buckets, 3 gates)` |
| **G13 stdio** | `test.yml:1515-1522` alternation over `MG_Backend` + `MG_State` | 0 hits |
| C.2 grep | `OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|CollectGarbageIfNeeded` in `DirectGLES.cpp` | 37 (unchanged from v4; file untouched this round) |
| **G2** | ID-4 extraction ×4, `LC_ALL=C sort`, `diff` | `build-linux` 2385 = `build-push` 2385 = `build-nolegacy` 2385 (both diffs empty); `build-verify` 3203 |
| **G14** | `comm -23 <baseline> <pull names>` | 0 removed; 22 added = 4 contract placeholders + 18 `DirectGLESSlotTable.*` |
| unit ×4 | `ctest -L unit --no-tests=error -j 6` | `100% tests passed, 0 failed out of 1507` in **all four** |
| integration, push handle | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **446/446**, 50 skipped |
| integration, push legacy | same with `MOBILEGL_PIPE_PUSH=0` | **446/446**, 50 skipped |
| integration, nolegacy | | **446/446** |
| integration, pull | `build-linux` | **446/446** on the first run (v5 §5.1's two flakes did not reproduce, at load ≈45) |
| integration, verify | `build-verify -L integration-gpu -R DirectGLES` | **855/855** |
| **G4 (a)** | `build-verify -L integration-verify -j 4` | **818/818**; `Fatal{` = 0 in all ten lane logs |
| **armless red proof** | `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario'` | **`0% tests passed, 13 tests failed out of 13`**, rc=8, 13 × `Subprocess aborted`, **0 skipped** |
| armless, whole lane | same, `-R DirectGLES -j 4` | `17% tests passed, 369 tests failed out of 446`, 44 skipped (identical to v4) |
| armless, single binary | `MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData` | harness comes up (`renderer: Espryt … llvmpipe`), `[ RUN ]`, exit **134** |
| **G3** | `retrace_gate.py --tree ~/w7/p2-espryt --lib build-push/libMobileGL.so -j 4 --only 'DirectGLES$'` | rc=0, **`passed 39 / 39; failed: []`** |
| **G4 (b)** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … --lib build-verify/libMobileGL.so …` | rc=0, **39/39**; 39 `*/*/output/mobilegl.log`: `Fatal{` 0, divergence 0, unarmed 0 |
| SanityTest | `build-push` / `MOBILEGL_PIPE_PUSH=0` / `build-nolegacy` / `build-verify` / `build-linux` | 100 / 96 + 4 skipped (the four handle-arm cases) / 99 + 1 skipped (`AnArmlessKnob…`) / 100 / 82 |
| SanityTest order | `--gtest_shuffle --gtest_random_seed={7,19,4242}` | 100/100 ×3; each of the eight new/changed slot cases also passes in isolation on push, and the relevant ones on nolegacy / legacy env |
| commits | `git log p2/contract..HEAD --format=%B \| grep -ci 'co-authored\|claude'` | 0; both new subjects `[Type] (Scope): …` + blank + `- ` bullets |
| CR/LF | `grep -c $'\r'` over all 17 touched files | 0 each |

Gates this tree cannot reach (unchanged from v4 and correctly declared in `espryt-v5.md` §3.4):
G6/G7, G8, G9 (`gen_pipe_dirty_surface.py` here accepts only `--summary`), G10, G11, G12, G14's CI
half. So `1507×4 + 446×4 + 855 + 818 + 39 + 39` is not a P2 acceptance pass; it is C's part of it.

On the "tracker gate" items in the round-5 reviewer instruction (synthetic dictionaries for six
negative controls, ten `RenderState` setters against the field-level match rule): those are package
B's deliverables (`gen_pipe_dirty_surface.py --check/--self-test`, G7), live in `~/w7/p2-tracker`
(read-only to this reviewer) and are not claimed by `espryt-v5.md`. Confirmed absent from this tree
(`--self-test` → `unrecognized arguments`); not reviewed here.

---

## 2. Minors (none blocks)

1. **`SanityTest` cannot be put on the armless pair from the environment.** `EsprytSlotArmEnvironment`
   (`SanityTest.cpp:81-97`) reads `MOBILEGL_PIPE_PUSH` only; `MOBILEGL_PIPE_LEGACY_MEMOS=0` is ignored
   because the binary never runs `MG_ConfigLoader`. Observed: `MOBILEGL_PIPE_PUSH=0
   MOBILEGL_PIPE_LEGACY_MEMOS=0 ./build-push/…/SanityTest` → `[ PASSED ] 96` on the legacy arm,
   silently. The armless cases set the knob in-process so they are unaffected; the comment's "the A/B
   is one env var" is true for the bit only.
2. **`GetOrCreate`'s Gen-mismatch branch keeps the discipline `ReleaseTwinAt` was written for out.**
   `SlotTables.h:230-235` holds `Entry& entry` into `m_slots` across `entry.backend.reset()` — a twin
   destructor, i.e. a driver call — then writes `entry.Gen` / `entry.Live` / `entry.stateRef`;
   `ReleaseTwinAt` (`:368-381`) moves the twin out first for exactly that reason. With every holder
   notified the branch is unreachable in practice (my scenario C: no acquisition can land on a reset
   window's slot while the object lives), so this is a consistency defect, not a live one.
3. **A texture death is two notices, not one.** Every `TextureObject2D` destruction raised
   `kind=2` then `kind=8` (the embedded `SamplerObject`, `TextureObject.h:206`, `SamplerObject.cpp:29-39`)
   — two `FindByLifetimeId` probes per texture death, one of which never finds a twin unless the
   sampler registry twins embedded samplers. Correct, but undocumented per-death cost; belongs next
   to the "no draw-path tick" sentence in `SlotTables.h:43-47`.
4. **`HandleOf`'s miss path is now an allocator hash probe every call** (`:281-283`) for an object
   that is bound but never synced; round 4 cached that. Right trade for two holders, unpriced (G11,
   `D.4.2/D.4.3` still owed, as v5 §6.6 says).
5. **`espryt-v5.md` §3.2 says the push handle lane had "100 (Skipped)"**; it has 50 (same as the
   legacy arm, `build-nolegacy`, `build-linux`, and v4's count). Reporting slip only.
6. **G4 (a)'s lane logs: 8 of the 10 `build-verify/MobileGL/MG_IntegrationTest/*.log` carry no
   `MGPipe: verify armed` line** (only `pipe-verify-arming-*.log` do). Each lane has one fixed
   `MOBILEGL_LOG_FILE_PATH` opened with `"w"` per case, so the file is the last case's log — the same
   shape v5 §5.1 reports to E. Package E's lane, not C's code; the brief's arming check is defined
   over the retrace logs, where it is 0 unarmed.
7. **Round-4 minors left open on purpose remain open**: 3 (`build-nolegacy` still instantiates the
   legacy `CollectGarbage()`: `nm -C … | grep -c` → 6), 6, 7, 10, 11, 13, 14. All declared in v5 §6.9.
8. **`SlotTables.h`'s "single-threaded" holder-list claim (`:403-404`) is an assumption, not a gate.**
   It follows `BufferBackendOps::OnDestroy`'s shape (`BufferObject.cpp:38-42`) and I found no
   worker thread that can hold the last `SharedPtr` of a re-keyed object; a debug-only thread-id
   assert in `OnFrontendObjectDestroyed` would make the claim checkable.
9. **v5 §6.2 overstates what is left of debt #2.** With the holder list, a slot minted for a live
   object cannot be re-handed during a reset window (nothing frees it while the object lives), so the
   "semantic hazard" is moot in both forms, not one. Documentation only.
10. **v5 §5.1's two pull-lane flakes did not reproduce** here (446/446 at load ≈45 with another
    package's lane running concurrently). Consistent with E's fixed-path collision reading, but the
    integrator should not treat it as confirmed until E reproduces it.

---

## 3. Claims in `espryt-v5.md` checked and found accurate

§1's two commits, file list (all inside C.2 + ID-1), no mode change, no attribution lines; §2(a)'s
Magma correction; §2(b)'s deletions; §2(c)'s pin mechanism and NC10's lane-skip contrast (reproduced
above); §2(d)'s hygiene; §2(e)'s pull-codegen-unchanged; §2(f)'s NC9; §3.1's G1/G5/G13/G2/G14
numbers; §3.2's 1507×4, 446×4, 855, 818, 39+39, 100/96/99; §3.3's NC8–NC10 (all three reproduced
by me in a copy, with the same failing-case sets); §3.4's unreachable list.

## 4. Logs kept under `~/w7` for this review

`~/w7/rev5-espryt/{lanes-summary.txt, nc-summary.txt, twoholder.cpp, twoholder-run.txt}`. The
source copy `~/w7/rev5-nc`, both retrace output trees and every other intermediate log of this
review were deleted.

## Bottom line

The round-4 hazards are closed by construction (one notice, every holder, one free, last) and the
closure is observable from outside the package's own tests: a reviewer-written three-holder scenario
over the real Texture registry and the real destructor drops both twins and returns the slot once,
and each of the three new gates goes red for its own reason when its guard is removed — including
the call-site pin, which fails on exactly the edit that turns the integration lane green-by-skipping.
Every reachable gate holds on four build configurations with the tree left as found. Approved.
