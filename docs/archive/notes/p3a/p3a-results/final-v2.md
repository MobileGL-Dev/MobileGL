# P3a final rework — `final-review-v1.md` §6 answered on `p3a/final` (`3e298c9a..680ea633`)

Worktree `/home/swung/w7/p3a-final`, branch `p3a/final` from `3e298c9a`, head **`c20e2f2b`**.
Twelve commits, one per finding, plus three corrections the verification itself forced (two test
registrations and G7's own attribution — see §6 and §8). Nothing pushed.

**Everything the brief asked to be green is green**, including the two things that were not:
C-1's leak case is **red before the fix and green after it, on the DirectVulkan lane**, and G5 now
has **eleven rows** with a non-vacuous control on each of the two flush ladders.

---

## 1. Per-finding disposition

| # | finding | verdict | commit | where |
|---|---|---|---|---|
| C-1 | VAO CSO slot + applier record leak under any backend with no `StateObjectDeathOps` | **FIXED** | `78ff0145` (fix), `f0beefa8` + `680ea633` (tests) | see §2 |
| C-2 | G5's tenth row protects text no push build compiles | **FIXED per ID-15** | `9ba5d7ba` | see §3 |
| M-1 | a `BufferObject` born with the op table unregistered never gets a record | **FIXED + test** | `433f51a0`, `959ca308` | `MG_Impl/Pipe/PipeFill.cpp:637-661`; test `MG_Test/Pipe/ResourceEmitTest.cpp:1807` |
| M-2 | `hostBytes` written off the render thread with no synchronisation | **FIXED** | `afda613b` | `MG_Backend/DirectGLES/Managers.cpp:1949-1966`, `:2014-2019` |
| M-3 | the fp64 stream's dropped `SyncGpuWrites` site | **RULED DEVIATION, recorded at the site** | `83c8101a` | `MG_Backend/DirectGLES/Managers.cpp:4546-4570`; §5 has the text for the docs |
| M-4 | the stale promotion comment, and the promotion not made | **FIXED (now a stop)** | `2a5e0195` | `MG_Backend/DirectGLES/Managers.cpp:2325-2356`, `:2374-2380`, `:2424-2436`, `:3831-3843` |
| M-5 | four cross-package rows closed by nobody | **ALL FOUR CLOSED** | `707bced4` (+ C-1 for row 1) | see §4 |
| §6.6 | re-measure G1/G14 on `3e298c9a` | **not redone** (integrator's, per the brief); **G1 re-run on the final tree**, 0/0/0/0 | — | §6 |
| §6.7 | run G7 once on this tree | **not redone**; re-run anyway because m3 changed its trip detection | — | §6 |
| minors | m1, m3, m4, m6, m8 taken; m2 folded into C-2 | **TAKEN** | `0bff6875`, `9ba5d7ba` | §7 |
| minors | m5, m7 and closure row 12 (`retrace_gate.py` RSS) | **NOT TAKEN**, with reasons | — | §7 |

### The commits, oldest first

```
78ff0145 [Fix] (MG_Impl, MG_State, DirectGLES): give the vertex-elements CSO a backend-neutral death path ...
f0beefa8 [Test] (MG_Test, MG_IntegrationTest): assert a destroyed vertex array returns its CSO slot ...
9ba5d7ba [CI]  (scripts, test.yml, DirectGLES): hash FlushPendingRangesFrom as G5's eleventh row ...
433f51a0 [Fix] (MG_Impl, MG_Test): publish the create a respecify's handle never got ...
afda613b [Fix] (DirectGLES): publish GLESBufferResource::hostBytes under pendingMutex ...
2a5e0195 [Fix] (DirectGLES): stop on an armless P3a subsystem verdict ...
83c8101a [Fix] (DirectGLES): record the fp64 narrowing's dropped SyncGpuWrites as a ruled P3a deviation ...
707bced4 [Fix] (MG_Impl, MG_Pipe, MG_IntegrationTest): close the four cross-package rows nobody claimed ...
0bff6875 [Fix] (MG_Impl, DirectGLES, scripts): take the cheap review minors ...
959ca308 [Test] (MG_Test): declare the new respecify-repair case in ResourceEmitTest pull-skip list (G2)
680ea633 [Test] (MG_IntegrationTest): baseline the CSO slot leak case after two warm-up rounds
c20e2f2b [Fix] (scripts): attribute G7 trip over the failing assertion BLOCKS ...
```

The `ctest -L unit` / `-N` / G1 / G2 / G14 / G5 / integration numbers in §6 were measured at
`680ea633`; `c20e2f2b` touches only `scripts/p3a_vertex_input_negative_control.sh`, which no build
or ctest entry reads, and G7's own end-to-end run at that commit (§8) rebuilt and re-greened
`build-push` as part of its repair step.

---

## 2. C-1 — the backend-neutral death path, and its red/green evidence

### The shape

`~VertexArrayObject` (`MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:44-60`, still inside
`#if MOBILEGL_PIPE_PUSH`) no longer raises the death notice itself. It calls one client helper,
declared at `MG_Pipe/PipeMutation.h:127-141` and defined at `MG_Impl/Pipe/PipeFill.cpp:795-853`,
which performs the whole death in a fixed order:

1. resolve the handle from the lifetime id;
2. **if the applier holds a record** for it, `MGPipeApplyDeleteVertexElements` and forget it;
3. `NotifyStateObjectDestroyed(VertexElementsCso, lifetimeId)` — **unconditional**, exactly as the
   destructor raised it before, so Espryt still drops the driver VAO and the P2 e2 gate
   (`SanityTest.DirectGLESSlotTable.EveryReKeyedObjectClassAnnouncesItsOwnDeath`) still sees this
   class announce itself;
4. `MGPipeSlots().Free(VertexElementsCso, handle)`.

**Espryt's notice is now the redundant second path and the double release is harmless.** Its
`OnFrontendObjectDestroyed` frees the same slot at step 3, so step 4 is a no-op: `MGPipeSlotAllocator::Free`
refuses a slot that is not live at that generation and the `Gen` bump rides the *next handout*, not
the free (`MG_Impl/Pipe/SlotAllocator.cpp:113-130`). That is asserted, not assumed —
`VertexInputEmit.ADoubleReleaseOfAVertexElementsSlotIsHarmless`
(`MG_Test/Pipe/VertexInputEmitTest.cpp:584`) frees by hand first, then runs the client path, and
checks that the successor still gets `{same slot, moved gen}`.

### The one thing the review's shape needed that it did not name

**A slot is not evidence of a record.** DirectGLES mints a `VertexElementsCso` slot from
`BackendSlotTable::GetOrCreate` at every VAO sync, whether or not bit 8 ever asked the client to
emit a create — which is exactly what the shipped `MOBILEGL_PIPE_PUSH=0x7f` A/B control arm runs.
`delete_vertex_elements` on such a handle is a refused call, and the refusal **asserts**
(`PipeApply.cpp`'s `ResolveVertexElements` → `MOBILEGL_ASSERT` → `TRAP`), i.e. it stops a verify
build. So the emitter carries a client-side record latch — `Latch::RecordLive` / `RecordGen`,
`RecordIsPublished` / `NoteRecordDestroyed` at `MG_Impl/Pipe/VertexInputEmit.h:307-345`, set at
`:406` where the create actually reaches the applier. It is deliberately **not** cleared by
`Reset()`: `Reset()` clears the per-context half because a fresh context is a fresh server, and
object records are precisely what `MGPipeApplierReset` does *not* drop.

`Managers.cpp:216-234` now says at the `VertexElementsCso` case that this arm is the second path
and what it is still uniquely for (dropping the driver VAO).

### RED BEFORE / GREEN AFTER

The control restores **only** `VertexArrayObject.cpp` to `3e298c9a` — the pre-C-1 destructor, which
raises the notice and nothing else. The helper stays compiled, so the tests still build and the run
is real rather than stale. (A plain `git revert` of `78ff0145` does *not* work: it breaks the build
and ctest then runs the previous binary and passes — that trap was hit once and is recorded here so
the next person does not repeat it.)

**Unit (`build-verify`, a binary that installs no `StateObjectDeathOps` at all — the Magma shape):**

```
RED-BEFORE  VertexInputEmit.DestroyedVertexArraysReturnTheirCsoSlotsAndRecords ... FAILED
  Expected: (slots.HighWater(VertexElementsCso) - highWaterBefore) <= (3u), actual: 65 vs 3
  Expected: (peakLive) <= (liveBefore + 2u), actual: 64 vs 2
GREEN-AFTER 100% tests passed, 0 tests failed out of 2
```

**Integration (`build-push`, both `HandleRecycle.Handles` lanes) — the decisive one:**

```
RED-BEFORE
  DirectGLES.HandleRecycle.Handles....DestroyedVertexArraysReturnTheirVertexElementsSlots  Passed
  DirectVulkan.HandleRecycle.Handles..DestroyedVertexArraysReturnTheirVertexElementsSlots  ***Failed
  [ HandleRecycle ] backend=DirectVulkan VertexElementsCso live 2 -> 50 (peak 50),
                    high water 3 -> 51 over 48 create/draw/destroy rounds
  "48 vertex arrays were created, drawn with and destroyed and 48 VertexElementsCso slots
   never came back."
GREEN-AFTER
  100% tests passed, 0 tests failed out of 2
```

That is the review's claim reproduced exactly: **green on DirectGLES before the fix** (Espryt
already freed) and **red on DirectVulkan** (nobody did) — 48 slots leaked over 48 rounds, the leak
growing 1:1 with the churn — and both green after.

### The cases and their registration

- `MG_Test/Pipe/VertexInputEmitTest.cpp:527` `DestroyedVertexArraysReturnTheirCsoSlotsAndRecords`
  and `:584` `ADoubleReleaseOfAVertexElementsSlotIsHarmless`, both in the pull-skip list at `:134-135`.
- `MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1015`
  `DestroyedVertexArraysReturnTheirVertexElementsSlots`. `TEST_FILTER "HandleRecycleScenario.*"`
  registers it on **all six** HandleRecycle lanes for free, so it runs on the DirectVulkan Handles
  lane where it failed, and `test.yml:614`'s `-R HandleRecycle|...` picks it up in `build-verify`
  too. It skips on the Legacy/AbaControl lanes (`MOBILEGL_PIPE_PUSH=0`, no allocator to leak from)
  and on a pull build.
- The observable needed a new harness TU, `MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}`
  (registered at `MG_IntegrationTest/CMakeLists.txt:54`), for `BackendCapsPeek.h`'s stated reason:
  the scenario sources include the GL headers with prototypes and MobileGL's umbrella header must
  not meet them in one file. It returns `false` where the allocator is out of reach (pull build,
  Android's hidden-visibility link) and the case **skips** there — "could not look" is never
  reported as "did not leak".
- `680ea633` baselines after **two** warm-up rounds. The first draws in a process mint slots that
  legitimately never come back inside one case (the default vertex array's above all); the second
  round proves the steady state has been reached, since a per-round leak would still be growing.
  The assertions are then exact — `liveAfter == liveBefore` and `highWaterAfter == highWaterBefore`,
  no slack.

---

## 3. C-2 — the eleven-row G5 verdict (ID-15)

`scripts/p3a_untouched_regions.sh`:

- `EXPECTED_FUNCTION_COUNT=11` (`:98`);
- `FUNCTIONS` keeps the pre-P3a ten; `PINNED_FUNCTIONS="FlushPendingRangesFrom"` with
  `PINNED_BASELINE_REF=3e298c9a` and
  `PINNED_SHA_FlushPendingRangesFrom=37fc94ffc5991923d222d585daa3af6511d2352d255623026ce35a3b6963c4a6`
  (`:88-96`), captured at `3e298c9a` as ID-15 directs;
- `SELF_TEST_FUNCTIONS="ClearBufferPool FlushPendingRangesNow FlushPendingRangesFrom"` (`:107`) —
  **both** ladders get a negative control, because the whole point of the eleventh row is that
  there are two bodies and either can drift;
- a new `extract_baseline()` beside `extract_ref()`: the `<ref-b>` side must define all eleven
  exactly once, while the `<ref-a>` side falls back to the pinned sha **only** for a
  `PINNED_FUNCTIONS` name that is absent there — and it re-extracts the pre-P3a ten strictly first,
  so a genuine rename of one of *those* is still exit 2 and never a silently short list. CI passes
  `${BASELINE}` (the P3a base ref) as `<ref-a>`, where `FlushPendingRangesFrom` does not exist, so
  this is the path CI takes.

`FlushPendingRangesNow` is **untouched** and still lives inside the `#else`. The comment at
`Managers.cpp:1139-1166` now states the two-arm shape and that both ladders are hashed from here on
(see §5 for the sentence the docs should carry). `test.yml:1636-1660` says "ELEVEN named bodies",
"six canned controls" and why the eleventh's baseline is pinned rather than read from `BASELINE` —
m2's "nine" wording is gone from both files.

**Verdict, on the final tree:**

```
scripts/p3a_untouched_regions.sh 3e298c9a HEAD   -> rc 0, 11 rows, "byte-identical"
scripts/p3a_untouched_regions.sh 5cb826b0 HEAD   -> rc 0  (the eleventh row takes its PINNED baseline,
                                                    and says so on stderr)
scripts/p3a_untouched_regions.sh --self-test     -> rc 0, six controls:
    positive: 11 bodies extracted / an untouched copy compares equal / an edit outside is invisible
    negative: ClearBufferPool, FlushPendingRangesNow, FlushPendingRangesFrom each reported BY NAME
```

The eleven shas at HEAD are identical to those at `3e298c9a`:

```
ba79b92d…  IsPoolable          15259e54…  ProcessDeferredBufferReleases
6563286e…  EnrollIntoPool      2daaa525…  CreateRingStorage
dc9d3066…  AcquireFromPool     6221df2e…  RingAvailable
12656cfd…  TrimBufferPool      709b6f41…  RingAllocate
c44a1274…  ClearBufferPool     c6557002…  FlushPendingRangesNow
                               37fc94ff…  FlushPendingRangesFrom
```

---

## 4. M-5 — the four open rows

| row | disposition |
|---|---|
| `delete_vertex_elements` producer + VAO CSO slot free (package C) | **This was C-1.** Closed by `78ff0145`; the producer now exists and is the client's. |
| contract-review **m4** — the three pairing `static_assert`s (package B) | **Closed in code**, `MG_Impl/Pipe/PipeFill.cpp:1010-1040`. All three now compare against `SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements)` rather than two of them against the constant, and the `== 0 \|\|` escape hatch is **removed** — it was explicitly conditional on the dirty half being unmapped, and the dirty half is now mapped (`Tracker.h:145-148`). Leaving it would have let a later edit *unmap* one of these bits silently. |
| gates **m10** — no DirectGLES `.AbaControl` positive control (package C) | **NOT registration-only, so written down instead**, `MG_IntegrationTest/CMakeLists.txt:895-914`. `Features.PipeHandleAbaControl` has exactly one consumer in the tree (`DirectVulkan/Renderer/MagmaPipeArms.h:198`), so a DirectGLES `.AbaControl` lane would run with the knob inert, fail to reproduce the corruption it asserts, and go **red in an always-on `integration-gpu` lane** — the exact failure mode this file's own header records having had once. The note states what that costs (on DirectGLES the buffer case proves the re-key does not alias, and nothing proves the reproducer could still see an aliasing reintroduced there) and what would close it (one `if` in Espryt's slot table, the way `MagmaPipeClaimSlotMemos` is Magma's). |
| wire **n6** — which counter G10 asserts against (package D) | **Closed on the record, in code**, `MG_Pipe/PipeApply.cpp:631-645`. G10/G12 read `PipeStats`' process-wide `mpr` out of the lane log through `Harness/PipeStatsWindow.h` — they cannot link the applier symbol at all — so the per-applier `MapPersistentRoundtrips` being zeroed at every make-current cannot silently reset what they measure. This also closes minor **m6**, which asked for exactly that sentence. |

---

## 5. For the integrator's documents

Two things need to leave this file and land in `INTEGRATOR-DECISIONS.md` / `MEASUREMENTS.md`.

### (a) M-3 — the ruled deviation (`docs`, and `MEASUREMENTS.md`'s P3a section)

> **The fp64 vertex-array narrowing drops one of D-N's eleven `SyncGpuWrites` sites on the handle
> arm, and that is RULED for P3a rather than an oversight.** D-N's wording is "no *move* of those
> sites off the frontend"; this arm does not move it, it **cannot make it at all** —
> `SyncFloat64AttributeAsFloat32ByHandle` holds a handle and the call needs a frontend
> `BufferObject`, and the server having no inverse map back to a frontend object is the design
> (ARCHITECTURE.md 4.2), not a gap in it. The two ways to keep the site would each break something
> D-N or D-J protects: a handle→object map is the very thing the split removes, and pulling the
> bytes eagerly on the client at every draw is new behaviour and new cost.
>
> **Blast radius, stated so it can be checked later:** under the default mask, a 64-bit vertex array
> whose *source* buffer was written by a shader and not yet pulled back narrows stale bytes on the
> handle arm and fresh bytes on the legacy arm. That is the whole of it — fp64 vertex arrays, fed by
> a shader-written buffer, read with no intervening explicit readback. No other attribute type reads
> through this path, and a persistently mapped source is excluded separately (the memo never trusts
> one). **P8 closes it** by moving the pull to the client where the object lives. Until then this is
> the one behavioural difference between the two arms under the default mask.

The same text is at the site, `MG_Backend/DirectGLES/Managers.cpp:4546-4570`.

**Package D's `DoublePrecisionScenario` with an adopted source was NOT added.** It is not cheap
under an hour: the scenario would have to drive a compute/XFB write into the fp64 source buffer and
then read the narrowed attribute back, on a lane where an adopted (`≥16 MiB`, persistently mapped)
store is actually taken — and the persistently mapped case is exactly the one the memo excludes, so
the case has to force the *non*-adopted shader-written shape instead, which is a new workload rather
than a parameter on the existing one. It stays package D's, and closure rows 3/4/5 stay open with
that reason. The deviation is now recorded in code, which is what makes it auditable in the
meantime.

### (b) C-2 — the two-arm statement (supersedes ID-13's "outside any `#if`" sentence)

> **ID-15.** `Managers.cpp` carries the three-tier flush drain twice, once per preprocessor arm, and
> exactly one is compiled into any build: `FlushPendingRangesFrom` under `#if MOBILEGL_PIPE_PUSH` is
> what a **push** build runs — both call sites, the legacy one and `Ops_H_Readback`, reach it — and
> `FlushPendingRangesNow` inside the `#else`, byte-identical to `5cb826b0`, is what a **pull** build
> runs. A push build compiles no `FlushPendingRangesNow` at all. No forwarder is possible: G5's
> extractor is preprocessor-blind, so a forwarding `FlushPendingRangesNow` would leave two
> definitions of one name and the gate would exit 2 — a gate that cannot run — rather than compare
> anything. Therefore **both** names are G5 rows: eleven functions, the pull ladder compared against
> the P3a base ref and the push ladder against a sha pinned at `3e298c9a`. Neither ladder may drift,
> and neither may drift away from the other without the gate saying so.

### (c) One more line worth a doc row

`MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}` is new and is the first thing in the integration
module that reads the **client slot allocator** directly. It belongs beside `BackendCapsPeek` in any
list of the module's "looks past the GL API" seams.

---

## 6. Verification transcript (all on `p3a/final` at `680ea633`)

```
### builds x3 ###
BUILD_build-linux=0   BUILD_build-push=0   BUILD_build-verify=0

### ctest -L unit x3 ###
UNIT_build-linux=0    100% tests passed, 0 tests failed out of 1622
UNIT_build-push=0     100% tests passed, 0 tests failed out of 1622
UNIT_build-verify=0   100% tests passed, 0 tests failed out of 1622
                      (1619 at 3e298c9a; +3 = 2 VertexInputEmit + 1 ResourceEmit)

### G1 pull symbols, --threshold 0, vs ~/w7/p3a-before-libMobileGL.so (the b9eaa474 pull build) ###
G1=0   Removed (0)   Added (0)   Resized (0)   Renamed only (same size) (0)
       -> the ~VertexArrayObject edit is push-guarded; the pull build is byte-identical.

### G2 pull vs push ctest names ###
pull=2588  push=2588  G2_DIFF=0

### G14 vs ~/w7/p3a-before-ctest-names.txt ###
REMOVED=0  ADDED=69

### G5 ###
G5_3E=0 (3e298c9a..HEAD)   G5_BASE=0 (5cb826b0..HEAD)   11 rows   G5_SELFTEST=0 (6 controls)

### generators + closure ###
GENPIPE=0   DIRTY=0   DIRTY_SELFTEST=0   CLOSURE=0 (--mode text, 4 probes, 0 forbidden)

### integration-gpu, default mask, build-push, -j 8 ###
IGPU=0      100% tests passed, 0 tests failed out of 966

### integration-gpu, MOBILEGL_PIPE_PUSH=0x7f, build-push, -j 8 ###
IGPU_7F=0   100% tests passed, 0 tests failed out of 966

### build-verify -R 'HandleRecycle|VertexElements|Leak' --no-tests=error ###
VERIFY_R=0  100% tests passed, 0 tests failed out of 76

### integration-verify, build-verify, -j 4 ###
IVERIFY=0   100% tests passed, 0 tests failed out of 844
FATAL_IN_VERIFY=0
```

Notes on the transcript:

- **`ctest -L unit` is 1622 in all three builds and `ctest -N` is 2588 in both**, which is what
  forced `959ca308`: `ResourceEmitTest` keeps an explicit pull-skip list for G2, and a new push-only
  case has to be declared in it or the pull build is one name short. A first pass had `G2_DIFF=1`.
- **One flake seen and not reproduced.** The very first `integration-gpu -j 8` run reported
  `DirectGLES.SnormAttachmentScenario.SignedNormalizedAttachmentsRoundTripTheirChannelValues
  (Subprocess aborted)`. It passed in the `0x7f` run of the same batch, in every later run, and in
  the integrator's own gate on `3e298c9a` (958/958). Unrelated to this diff; llvmpipe under `-j 8`.
- **§6 items 6 and 7 were not redone**, per the brief. G1 *was* re-run here on the final tree
  (above). G7 was re-run anyway, because minor m3 changes its trip detection and a gate whose
  detector I edited has to be shown still tripping — result in §7.

---

## 7. Minors

### Taken

| # | what was done | where |
|---|---|---|
| m1 | the stale "all three still resolve to false today — their dirty bits map to no subsystem" sentence replaced with what is true now (bits 5/9/10 are mapped, all three emitters have bodies, `wants()` answers true under the 0x1ff default on every backend) | `MG_Impl/Pipe/PipeFill.cpp:1588-1595` |
| m2 | the "nine" wording in the G5 script and `test.yml` corrected to eleven, and the control count corrected from "three canned controls" to six | `scripts/p3a_untouched_regions.sh` (header, usage, exit codes, self-test messages), `.github/workflows/test.yml:1636-1660` |
| m3 | G7's trip attribution matched against the **failing assertion's own lines** instead of `grep -q IsBgra` over the whole ctest log; `INT` and `TERM` traps added beside `EXIT`, each repairing and then re-raising with the default disposition so the exit status still reports the signal | `scripts/p3a_vertex_input_negative_control.sh:174-179`, `:230-241` |
| m4 | the extractor reconfigures stdout to `newline='\n'`, so the sha list stays LF under Git Bash / MSYS python and the `awk -v n=` lookup stops missing every row | `scripts/p3a_untouched_regions.sh:269-276` |
| m6 | closed together with wire n6 — see §4 | `MG_Pipe/PipeApply.cpp:631-645` |
| m8 | the `ResolvedDrawBuffers::Entry::resource` invariant written **at the member**: the pointer may dangle, and the rule is that it is only ever dereferenced after the entry's identity has been re-resolved in the same pass (`FindByHandle` on the handle arm, the frontend identity compare on the legacy one); a third consumer that read it straight out of a "valid" memo would be reading freed memory and no compare in the struct would catch it; and why it stays raw | `MG_Backend/DirectGLES/Managers.h:980-999` |

### Not taken, with reasons

| # | why not |
|---|---|
| m5 | G8's DirectGLES `.Handles` arm degrading to a SKIP when `MGPipeResourceOps` is renamed is a **property of the marker mechanism**, not of this lane: all six `mgl_itest_probe_for_symbol` markers behave that way, by design, and the review itself calls it "honest (visible skip with a reason)". Fixing it for one marker only would leave five behaving differently; fixing it for all six is a gates-package change to the probe contract (a probe that must find its symbol, or configure-time red), which is out of scope for a rework round and would go red on any tree where a package has not landed yet — the exact thing the SKIP exists to avoid. |
| m7 | `MGPipeSlots()` being mutated from `BufferObject`'s constructor and destructor is already stated at length where it matters (`MG_Impl/Pipe/PipeFill.cpp:566-580`: the mint happens once, on the GL thread, and the content paths therefore only ever *look up*). Turning "GL thread by construction" into something enforced means a thread assertion on the allocator or a lock inside it — a design change to the allocator, on the hot path, that P3a's brief does not authorise. Carry to the phase that gives the allocator a per-client-context instance (the header already says "under split there is one per client context"), which removes the sharing rather than guarding it. |
| closure row 12 | growing `retrace_gate.py`'s peak-RSS reading is a **tools** change (`~/w7/notes/tools/`, ID-3: the integrator's, not a package's) and it is a device-run prerequisite, not a tree fix. It remains the measurement that would have caught C-1 on its own, and it should be done before the D.4 device runs — but C-1 now has a deterministic gate that does not need it. |

---

## 8. G7 on this tree — and the one thing minor m3 got wrong first

`bash scripts/p3a_vertex_input_negative_control.sh build-push`, run because m3 edited G7's trip
detector and a gate whose attribution logic changed has to be shown still tripping.

**First run: rc 1, `INCONCLUSIVE`.** m3's first attempt matched the field name against single
*lines* carrying `Failure` / `error:` / `Expected`. gtest does not print it there — it prints
`<file>:<line>: Failure`, then the compared **expressions** on their own lines
(`  wire.IsBgra`, `  attrib.IsBgra ? 1 : 0`), then the values. So a real trip was reported as "went
red but never named the field". The gate caught my own edit, which is what it is for.

**Fixed in `c20e2f2b`**: the unit is the failure **block**, not the line — awk prints from a
`: Failure` / `: error:` line until the next blank line, and the field is grepped in that. Validated
against the recorded failing log (16 hits inside the blocks, the same 16 the whole file has, i.e.
the tightening loses nothing real) and then re-run end to end:

```
[p3a-g7] VertexInputEmit\. is green before the patch
[p3a-g7] neutralised 1 assignment(s) to .IsBgra
[p3a-g7] the patched header still compiles, so the record still has the field and still asserts its size
[p3a-g7] restored MobileGL/MG_Impl/Pipe/VertexInputEmit.h; rebuilding build-push from it
[p3a-g7] the tree is restored, rebuilt and green again
[p3a-g7] negative control tripped, naming IsBgra, and the tree is green again
G7_RC=0
```

Same verdict the integrator recorded on `3e298c9a` ("tripped, naming IsBgra"), now on a tree whose
detector is no longer a whole-log substring search. The final commit list is the eleven in §1 plus
`c20e2f2b`; `git status` is clean.
