# Adversarial review — `p2/espryt` (package C, Espryt 0b) — round 2

Reviewer ran everything below itself in `~/w7/p2-espryt` at `5f245ac7` (tag `p2/contract` = `9c6a8a25`).
Nothing in the tree was modified: the four build directories were only re-built incrementally
(`ninja: no work to do` on all four, so the artefacts under test are the ones the implementer
measured), and every probe is a read, a `ctest`, or a `gdb -batch` counting run.

Verdict: **NOT APPROVED — 2 majors.**

---

## 0. What reproduces, exactly as claimed

I re-ran the package's own evidence before hunting. These all hold:

| claim | my command | my result |
|---|---|---|
| G1 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; resized = `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 |
| G1 attribution | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `.text +0`, `0 added / 0 removed / 0 resized / 0 renamed` — **package C's pull-build delta is exactly zero** |
| G5 | `git show {48268068, p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…0efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| G2 | `ctest -N \| sed -n 's/^ *Test  *#[0-9]*: //p' \| sort` × 4 dirs | `build-linux` 2374, `build-push` 2374, `build-nolegacy` 2374, diffs **empty**; `build-verify` 3192 (+818 verify lanes) |
| G14 | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | **empty** (2363 before). Added: 4 `*.PlaceholderUntilTheOwningPackageFillsThisIn` + 7 `DirectGLESSlotTable.*` |
| G13 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| G13 | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, rc 0 |
| G13 | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| G13 stdio | `grep -nE '\b(printf\|fprintf\|std::cout\|std::cerr\|puts\|putchar)\b'` over the four touched backend files | 9 hits, all the English word "puts" in comments |
| unit | `ctest -L unit -j 6` on all four dirs | `100% tests passed … out of 1496` × 4 |
| slot cases | `ctest -R DirectGLESSlotTable` | `7/7` on `build-push` and `build-nolegacy`; 42 `Skipped` lines on `build-linux` |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | `446/446` |
| integration, legacy arm | same, `MOBILEGL_PIPE_PUSH=0` | `446/446` |
| integration, no legacy compiled | `build-nolegacy`, same | `446/446` |
| **integration-verify** | `ctest --test-dir build-verify -L integration-verify -j 4` | **`818/818`**; `grep -c 'Fatal{'` over all six `pipe-verify-*.log` = 0 each |
| **G3 (DirectGLES half)** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib …/build-push/libMobileGL.so --out ~/w7/retrace-out/rev-espryt -j 4 --only 'DirectGLES$'` | `passed 39 / 39; failed: []`; five lowest SSIM **0.993475 / 0.995426 / 0.995920 / 0.996197 / 0.996301** — identical to §3 of the result file |
| `Fatal{PipeLegacyMemosDisabled}` | `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData` | `exit=0`, `[ PASSED ] 0 tests`, `[ SKIPPED ] 1`, harness prints *"a forked pre-flight child died on signal 6 (Aborted)"* — the disabled arm does not run |
| MAJOR-4 mechanism | `gdb -batch`, `break MGPipeSlotAllocator::{Acquire,Free,FindByLifetimeId}` on `CrossFrameBufferScenario.VertexBufferSubData` (`build-push`) | `Acquire` 5, `FindByLifetimeId` 6, `Free` 0 — the implementer's Measurement 1 reproduces exactly |
| churn sweep is real | `gdb -batch`, `break MGPipeSlotAllocator::{Free,Allocate}` on `DirectGLESSlotTable.ObjectChurnAloneDrivesTheSweep` | `Free` **192** hits, `Allocate` 256 hits over 256 churned objects — the creation-driven sweep fires ~3× and slots are recycled, so the case is genuinely falsifiable |

Also confirmed independently, refuting a worry I had on first read: `CMakeLists.txt:466-471`
makes `MOBILEGL_PIPE_VERIFY` imply `MOBILEGL_PIPE_PUSH`, so `build-verify` (whose `CMakeCache`
says `MOBILEGL_PIPE_PUSH:BOOL=OFF`) really does compile `-DMOBILEGL_PIPE_PUSH=1`
(`compile_commands.json`), and `gdb` shows `MGPipeSlotAllocator::Acquire` hit 5 times in
`build-verify`'s integration binary. Minor 12's fix is real.

And `ConfigLoader.cpp:254` + `MGPipe.h:85` (`kMGPipeSubsystemsMigratedAtP2 = 0x7f`) do set bit 5
by default, so the 446 push-arm integration entries really are on the handle arm.

---

## MAJOR 1 — the D13 "must not break" pins never execute the handle arm in any build directory the brief defines

C.2 says, verbatim:

> **Must not break** — the D13 list. Each item has a named existing test: `SanityTest.cpp:2575-2596`
> (scratch-FBO scrub), `:2650-2665`, `:2757-2790` (context-generation guards on
> texture/framebuffer/renderbuffer twins), `:3020-3062` (sampled-set staleness) …

and D13 additionally names the whole-registry save/reset/restore fixture
`ScopedDirectGLESTextureBindings` (`MG_Test/SanityTest.cpp:145-197`, unchanged at HEAD:
`:154-197`) as something the slot table "needs the same copy-assign-and-restore shape" for.

**Those cases run against the legacy `UnorderedMap` arm, not against `BackendSlotTable`, in
`build-linux`, `build-push` and `build-verify`.** The reason is
`MobileGL/Config.h:334` — `Uint64 PipePush = 0;` — and
`MG_Backend/DirectGLES/Managers.cpp:517` — `const Bool bitSet = (MG_Config::Features.PipePush & kMGPipeSubsystemEsprytSlots) != 0;`.
`SanityTest` never runs `ConfigLoader`, so `Features.PipePush` stays at its static default of 0,
`ResolveEsprytSlotTablesArm()` returns **false**, and `StateBackendObjectRegistry::GetOrCreate`
(`Managers.h:364-366`) takes the map arm.

Reproduction (`build-push`):

```
$ gdb -batch -x /tmp/gdb6.txt build-push/MobileGL/MG_Test/SanityTest      # --gtest_filter=-DirectGLESSlotTable.*
[  PASSED  ] 82 tests.
1  breakpoint  keep y  <MobileGL::MG_Backend::DirectGLES::EnsureProcessTeardownSentinel()>
        breakpoint already hit 4 times
2  breakpoint  keep y  <MobileGL::MG_Pipe::MGPipeSlotAllocator::Acquire(MGPipeKind, unsigned long)>
                                                     # <-- no "already hit" line: ZERO hits
3  breakpoint  keep y  <MobileGL::MG_Backend::DirectGLES::ResolveEsprytSlotTablesArm()>
        breakpoint already hit 1 time
```

`EnsureProcessTeardownSentinel` is called from `StateBackendObjectRegistry::GetOrCreate`
(`Managers.h:360`) on **both** arms, so 4 hits proves the twin registry *was* entered four times
(`SanityTest.cpp:321`, `:365`, and the flows under `:2635` and `:3001`); `Acquire` at 0 proves
every one of them went down the map arm. `build-verify` behaves identically (`Acquire`: no hits).
Only `build-nolegacy` — where `MOBILEGL_PIPE_LEGACY_MEMOS` is undefined and
`Managers.cpp:540` returns `true` unconditionally — reaches the new code (`Acquire` **4** hits,
82 tests pass).

Why this is a major and not a nitpick:

1. `build-nolegacy` is **not one of the build directories the brief defines** (section C's tree
   recipe, brief lines 485-500: `build-linux`, `build-push`, `build-verify`, `build-bench`). The
   integrator's D.1 per-merge loop and D.3 five-part gate never configure it, and package C ships
   no CMake entry, ctest label or CI step that would (E owns `test.yml`). So after integration
   nothing in any defined lane keeps these invariants holding on the arm that ships.
2. §3 of the result file offers `ctest -L unit … 100% tests passed … out of 1496 × 4` as the
   evidence for the "must not break" list. For three of those four directories it is evidence
   about the code the package *did not change*.
3. The gap is not hypothetical for this package: the one behaviour the fixture exercises that
   only the handle arm has — `g_backendTextureObjects = {}` followed by `= previousRegistry`
   dropping the table's slots **without returning them to `MGPipeSlots()`** (the package's own
   minor 4 debt) — is exactly what `ScopedDirectGLESTextureBindings` would surface, and in
   `build-push`/`build-verify` it cannot.

`MG_Test/SanityTest.cpp` is package C's own file per C.5, so this is fixable inside the package
(e.g. a second always-on ctest entry that runs `SanityTest` with the arm forced on before the
`EsprytSlotTablesEnabled()` latch, in the shape `MG_IntegrationTest` already uses for
environment-selected arms). It is not an ownership blocker.

## MAJOR 2 — step `e2` is still absent, and the brief forbids merging `e3` without it

Unchanged from v1 and correctly declared by the implementer (§2 MAJOR 3, §4.1, §6.1), but it is
still a missing scoped deliverable of C.2 and the brief is explicit that it blocks:

- C.2 step 2: *"`e2`: the explicit-destroy hook … **This must land before the tables switch
  over**, because a slot table has no garbage collector"*.
- Section E, risk row *"A slot table has no garbage collector…"*: *"C's `e2` lands the explicit
  `delete_*` notification **before** `e3` switches the tables over, and **`e3` is not merged
  without it**."*

`e3` and `e4` are landed; `e2` is not. Verified at HEAD: the seven
`CollectGarbageIfNeeded()` call sites the brief asks to delete are all still there —
`DirectGLES.cpp:1275, 1662, 2027, 2028, 2029, 2857, 2858` — now driving
`BackendSlotTable::ReclaimDeadSlots()` (`SlotTables.h:224-235`). Consequently
`ARCHITECTURE.md:363`'s census cannot count the six registries' GC as deleted, which is the unit
D.4.6 measures the phase against.

I accept the implementer's ownership argument on the merits (C.5 gives
`MG_State/GLState/{Texture,Framebuffer,Renderbuffer,Sampler,Program,VertexArray}State/*` to B and
`VertexArrayObject.h` to D), and I confirm the mitigation is real: the creation-driven sweep
(`SlotTables.h:125-128, 145-157`) restores parity with the map arm, and my gdb run shows it
firing (192 `Free`s over 256 churned objects). But "the damage is now a no-op instead of a
regression" is not the same as "the deliverable is present", and the brief's merge rule is not
a package-level decision to make. This stays a major until the integrator re-scopes `e2`.

---

## Minors

1. **§2 minor-2's claim is wrong about the shipping path.** The result file says *"The
   `MOBILEGL_ASSERT` was **removed** from that path on purpose: a DEBUG build must not trap where
   release quietly does the defined thing."* It was removed only from
   `BackendSlotTable::GetOrCreate` (`SlotTables.h:109-119`). Every production call goes through
   `StateBackendObjectRegistry::GetOrCreate`, which still opens with
   `MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null")` (`Managers.h:353`)
   **before** dispatching to the slot table. So a DEBUG build still traps on null on the shipping
   path, and `DirectGLESSlotTable.GetOrCreateToleratesANullStateObject` exercises a path
   production never takes. The behaviour is unchanged from pre-P2, so this is documentation, not
   a defect — but the claim as written is not true.

2. **Minor 4's stated blocker does not exist.** §2 minor 4 and §6.3 argue that returning slots on
   table destruction/reset cannot be done because *"`Free` on the same `{slot, gen}` twice pushes
   the slot on the free list twice and two later `Allocate`s hand out the same slot"*. That is
   refuted by the allocator itself: `MG_Impl/Pipe/SlotAllocator.cpp:113-130` —
   `if (!entry.Live || entry.Gen != handle.Gen) return;` — makes `Free` idempotent and
   generation-safe, so a second `Free` of the same `{slot, gen}` is a no-op and a `Free` of a
   stale handle whose slot was re-handed is also a no-op. The debt may still be worth deferring
   to `e2`, but it should be deferred for a reason that is true; as it stands the package carries
   a real (if small and test-only) slot/`ByLifetimeId` leak on table reset that it believes it
   cannot fix.

3. **D13's "`SyncTextureObjectToBackend`'s by-value copy and second `Find` … are deleted in the
   same change, not left as harmless" is not honoured, and the deviation is not declared.** Both
   survive on the handle arm: the copy at `DirectGLES.cpp:1377`
   (`const SharedPtr<BackendTextureObject> backendObj = backendSlot;`) and a re-resolving `Find`
   at `:1403`. The code's own comment (`:1369-1373`) gives a sound reason — a nested
   `GetOrCreate` can reallocate `Vector<Entry> m_slots` and invalidate the reference, so the copy
   is now a keep-alive and the `Find` is a re-resolve rather than a repair — so I do not dispute
   the engineering. But §4 of the result file lists five deviations and this is not one of them,
   and C.2/D13 is worded as an outright deletion.

4. **`UnitBindingsSnapshot` is split by `#if MOBILEGL_PIPE_PUSH`, not by the runtime arm, so
   `MOBILEGL_PIPE_PUSH=0` is still not a faithful P1 control there.** `DirectGLES.cpp:1445-1493`
   replaces the `WeakPtr` snapshot with lifetime ids for the whole push build, including the
   `MOBILEGL_PIPE_PUSH=0` legacy arm. This is the same complaint the implementer accepted and
   fixed for `g_fbSlotCache` (minor 6, `DirectGLES.cpp:171-186`, `#if !MOBILEGL_PIPE_PUSH || MOBILEGL_PIPE_LEGACY_MEMOS`);
   it was not applied here. I checked the semantics and they are equivalent (`OwnerEquals` on two
   empty pointers is true, and `LifetimeIdOf(null) == 0 == 0`; a live-vs-expired control block and
   two distinct lifetime ids both compare unequal), so this is A/B fidelity against D14
   (*"`MOBILEGL_PIPE_PUSH=0` … reproduces P1's behaviour exactly"*), not correctness. §4.5
   mentions the file but frames it as a G1 concern only.

5. **The new unit cases share process-global allocator state with no ordering pin.**
   `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin` (`SanityTest.cpp:3316-3345`)
   deliberately never sweeps and lets both `FakeSlotTable`s die holding a live `MGPipeKind::Query`
   slot, permanently raising `MGPipeSlots().LiveCount(Query)`;
   `ObjectChurnAloneDrivesTheSweep` (`:3353-3382`) then reads `HighWater(Query)` and
   `LiveCount(Query)` from the same singleton. Both use deltas so they pass today (and gtest keeps
   registration order within a suite), but nothing pins that, and `--gtest_shuffle` or a later
   case that also uses kind `Query` would make them interact.

6. **The comment that justifies (5) will stop being true at integration.**
   `SanityTest.cpp:3342-3344`: *"The six shipping registries are one table per kind, so that
   cannot arise outside this case."* `ScopedDirectGLESTextureBindings` already creates a second
   live table value of kind `Texture` for the duration of a test, and package D's subsystem 4
   re-keys `VaoDrawMemo` on `MGPipeHandle` out of the same per-kind allocator. Worth flagging to
   the integrator: two holders of one kind, where whichever sweeps first frees the slot the other
   still names, is a cross-package hazard, not a test-only one. (I checked the fallout and it is
   contained — `Free` is generation-guarded, and `FindByHandle`'s `Gen` compare turns a stale
   handle into a miss — but a live object whose `ByLifetimeId` row was erased by another table's
   sweep would be re-`Acquire`d onto a *new* slot, orphaning the first table's entry.)

7. **Gates outside this package were not verifiable from it, and the result file's §3 does not say
   so per-gate.** From `~/w7/p2-espryt` I could not exercise **G6/G7** (`RenderStateSpans.*` is
   still package A's placeholder — `RenderStateSpans.PlaceholderUntilTheOwningPackageFillsThisIn`
   is the only entry), **G8** (`HandleRecycle` — no such ctest name exists in any of the four
   dirs; package E), **G9** (`scripts/gen_pipe_dirty_surface.py` still has only `--summary`:
   `--check` and `--self-test` both exit 2 with *"unrecognized arguments"*; package B),
   **G10** (`MGL_RESIDUAL_BLOCK_SIZE` is already 8 at `MGPipeTypes.h:546`, but there is no
   `Residual` ctest entry and no `resid=` device window) or **G12** (`CsoContentAddressing`;
   package E). None of these is C's fault; they are recorded so the integrator does not read
   "446/446 + 818/818 + 39/39" as a P2 acceptance pass.

8. **Declared deviations I re-checked and agree with**, listed here only so the integrator does
   not have to: §5.1 (`_GLOBAL__sub_I_DirectGLES.cpp` −9 belongs to the contract — confirmed, my
   contract→HEAD run is `0 resized`); §4.3 (`UnitBindingsSnapshot` holds lifetime ids because a
   bound-but-never-synced texture has no twin — correct, and `s_nextTextureLifetimeId` starts at 1
   at `TextureObject.cpp:17`, so the `0 == nothing bound` sentinel is sound); §4.4 (the four
   `defaultFBO` compares need an owner for `kMGPipeDefaultFramebuffer`); §4.6 (`retrace_gate.py`
   has no `--backend`/`--ssim`; confirmed, `--only 'DirectGLES$'` is the working form);
   §5.3 (`grep -E '^\s+Test #'` drops four-digit entries — confirmed, `sed -n 's/^ *Test  *#[0-9]*: //p'`
   is the working form). I did not hit §5.2's first-run scheduling flake in any of my runs
   (the cost data was already warm), so I neither confirm nor refute it.

---

## What I hunted for and did **not** find

- **A stale answer from the new `lifetimeId -> handle` memo** (`SlotTables.h:185-193, 272-279`).
  I could not construct one. The key is a per-class monotone counter that starts at 1 and is
  never reused (`TextureObject.cpp:17,24-26` and the same shape in `Buffer/Program/VertexArray/
  Sampler/FramebufferObject`), `ITextureObject`'s only implementation family is
  `TextureObjectBase` so one counter covers the whole `Texture` kind, `ReclaimDeadSlots` clears
  the memo when it frees the memoised slot (`:218`), and every consumer resolves through
  `FindByHandle`'s `Gen` compare (`:175-181`). A cached *miss* is overwritten by the next
  `GetOrCreate` on the same table.
- **A `BackendPtr*` invalidated behind a caller's back.** `Find` never mutates
  (`SlotTables.h:166-181`), and the only mover is a `GetOrCreate` that grows `m_slots`; I walked
  every `g_backend*Objects.{Find,GetOrCreate}` site in `DirectGLES.cpp` and `Managers.cpp` and
  found none that holds a reference across an insertion into the *same* table without the
  re-resolve at `:1403`.
- **A transcription error in the `detachFrom` lambda.** I diffed
  `DirectGLES.cpp:6598-6653` (the shared lambda) against the pull-arm walk retained at
  `:6676-6727` line by line: identical, with the `stateRef.expired()` test correctly hoisted to
  the two callers.
- **`ResolveUnitSamplerBackend`'s "a miss is never cached" contract.** Preserved on the handle
  arm (`DirectGLES.cpp:3335-3350`), and the null-sampler case is unreachable (`:3805` guards with
  `else if (samplerObject)`).
- **Unguarded legacy residue.** Rather than re-implement the implementer's preprocessor walker I
  used the stronger proof: `build-nolegacy` (`-DMOBILEGL_PIPE_PUSH=1` with
  `MOBILEGL_PIPE_LEGACY_MEMOS` undefined) compiles and links `rc=0` and passes 1496 unit + 446
  integration entries, which is only possible if every `OwnerEquals` / `TwinLookupMemo` /
  `g_fbSlotCache` reference is inside a guard. `GetFramebufferBindingSlotFast` is gone from the
  whole tree (`grep -rn` over `MobileGL/`: no hits).
- **Hot-path instrumentation.** None added; `CreationTickForTest()` is a const accessor and the
  only new logging is `MGLOG_D`/`MGLOG_F` inside the once-per-process
  `ResolveEsprytSlotTablesArm()`.
- **A double free through the copy-assignable table.** `MGPipeSlotAllocator::Free`
  (`SlotAllocator.cpp:113-130`) is generation-guarded and idempotent — which is also what makes
  minor 2 above a real finding.

---

## To clear the review

1. Land a lane that runs `SanityTest`'s twin-registry cases on the `{slot, gen}` arm inside one of
   the brief's four build directories (MAJOR 1) — package C owns `MG_Test/SanityTest.cpp`, so no
   ownership line is crossed.
2. Integrator decision on `e2` (MAJOR 2): re-assign it to package B, or land B first and re-open
   C for one commit. Until then `e3` is merged in violation of section E's own rule.
3. Minors 1-4 are cheap: correct the two claims (1, 2), declare the `SyncTextureObjectToBackend`
   deviation (3), and either arm-split `UnitBindingsSnapshot` the way `g_fbSlotCache` was or say
   in §4 that `MOBILEGL_PIPE_PUSH=0` is knowingly not P1-faithful there (4).
