# P3a integrator decisions (feat/disaggregated, 2026-09-08)

## ID-1 The base ref is `44c2b5cf`, not the brief's `55d2af9b`
`BRIEF-P3A.md` was verified at `55d2af9b`. P2 landed four more commits before P3a started: `c73ae7d4` (pins
`MOBILEGL_PIPE_PUSH=0x7f` in the itest Handles lanes: `MG_IntegrationTest/CMakeLists.txt`), `a9778eaa` + `738b289d`
(the ABA generation-coverage fix: `MG_Backend/DirectVulkan/Renderer/{MagmaPipeArms.h, VertexInputStateFactory.cpp,
VulkanRenderer.cpp}`, `MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` (comment), `MG_Test/Pipe/CMakeLists.txt`
+ new `MG_Test/Pipe/MagmaPipeIdentityTest.cpp`), and `44c2b5cf` (docs only). **Every P3a tree branches from `44c2b5cf`**
and `$BASE` = `44c2b5cf` in every command the brief writes as `55d2af9b`. Brief line numbers are still exact for every
file except those seven; a package touching `HandleRecycleScenario.cpp`, `MG_IntegrationTest/CMakeLists.txt` or
`MG_Test/Pipe/CMakeLists.txt` re-opens the file before editing. The `~/w7/p3a-before-*` baselines were captured at
`44c2b5cf` (2482 ctest names; pull lib; `RenderStateImpl` sha `d8fd1c48...`).

## ID-8 Landed so far (feat/disaggregated in ~/w7/pipe, pushed to origin)
- contract c0: `39722687` (tag `p3a/contract`, packages branch here) rebased onto the P2 docs commits as
  **`e01c0ccc`** — pull symbols 0/0/0/0, unit 1571 x 3, +5 test names. Package branches rebase onto
  `refs/heads/feat/disaggregated` (spell branch refs fully: the tag makes `p3a/contract` ambiguous);
  the duplicate c0 patch drops out of the rebase automatically.
- contract review v1: ACCEPT WITH MINORS; M1 (Coverage.def target split omits DispatchIndirect/Query) and
  item 9 (SetVertexBuffers Start+Count policing) go to wire's rework; M2 (GetOrCreate(MGPipeHandle) adopts a
  stale generation) goes to espryt's e1; item 11 (0x7f-pinned itest lanes need a 0x1ff arm) goes to gates.

- wire v1: db0c93bb / cede041d / e9b4a263 on p3a/wire; review v1 = REWORK (C1 records wiped on every
  make-current via Tracker.h:192-195's Reset; C2 serials zeroed on reset; M1; c2 zero test coverage; the
  PipeLiveHostWrites control; RecordAt bound; Blob rule). Rework in flight with a **granted waiver** to touch
  Tracker.h:192-195 minimally if the fix needs it (B rebases across it).
- espryt v1: 9ae4ec09 / de13c588 / 855272e5 / 469f78b8 / a9a1d0eb / 203120fa on p3a/espryt; legacy arms
  461/461, default arm 202/461 red all attributed to the stub applier (expected until wire+client land);
  review v1 in flight. Accepted-in-principle deviations: g_pendingFetchBaseInstance/ScopedFetchBaseInstance
  stay under MOBILEGL_PIPE_LEGACY_MEMOS (deleting them is a G1 break, so C.2's "grep empty" line is not
  achievable in P3a - D.5 must say so); MGPipeSetResourceOps installed from RegisterBufferBackendOps;
  the fp64 stream's SyncGpuWrites stays frontend-side (D-N) - DoublePrecisionScenario is a post-rebase watch item.
  espryt review v1 = REWORK: C-1 the draw-clean test replaced IsMapped() with HasLiveHostWrites (pinned false
  => mapped non-adopted buffers never resync), C-2 a cached raw hostBytes base dangles after adoption/resize
  (fp64 narrowing UAF, undeclared pool gate), majors: contract M2 unaddressed, attribute walk bounded by
  AttributeCount drops the disable arm, bit 8 without bit 7 advertised but non-functional, PipeLegacyMemos
  ignored by the new arms, memo safety silently depends on wire's serial rule. Rework in flight (rebases onto
  5cb826b0 first, ID-9/ID-10/ID-11 applied).

## ID-8b wire integrated: feat/disaggregated = `12e6bfcf` (pushed)
wire v2 (six commits: c1 45c8f1a8, c2 6eb0e675, c3 355c60b9, then the rework 56366331 / e6452ce9 / 12e6bfcf)
rebased onto 5cb826b0 and fast-forwarded; pull symbols 0/0/0/0 against the 5cb826b0 baseline, unit 1599 x 3,
G2 0, G14 0 removed / +17. Applier rule from here on: object records survive make-current; only
MGPipeApplierReleaseObjectRecords (unwired in monolith) drops them; refusals are counted; serials only advance;
one Blob rule (0 = not declared). Client's rework runs against this tree.

## ID-8d client integrated: feat/disaggregated = `31e370be` (pushed)
Between wire and client two version bumps landed on the branch (42d43af2 gradle 26.09, b9eaa474 CoreVersion
26.9), so the G1/G14 baseline was re-captured at b9eaa474 before the merge. client v2 (eight commits, rework
d9bde131 / f11e78b0 / 0c555605 / 31e370be) fast-forwarded; pull symbols 0/0/0/0, unit 1617 x 3, G2 0,
G14 0 removed / +18; client's own tree had integration-gpu 920 x 3 arms, integration-verify 832, retrace
79/79 verify-armed zero divergence. espryt v3 (verification round on this tree) in flight; gates integrates
after espryt.

## ID-8e all four packages integrated: feat/disaggregated = `3e298c9a` (pushed)
espryt v3 (`3c55e027` + the integrator's BufferTest fixture commit `83302ca2` — ID-14 below) then gates v2
rebased and fast-forwarded (`3e298c9a`). On the finished tree: pull symbols 0/0/0/0 vs the b9eaa474 baseline,
G5 ten functions byte-identical vs 5cb826b0 AND vs 44c2b5cf, G5 self-test, unit 1619 x 3, G2 0, G14 0 removed
/ +58, HandleRecycle (verify) 60/60, mpr + ResourceSubsystemControl (push) 34/34, G7 tripped naming IsBgra.
espryt v3's real-path round found and fixed three seam defects (vertex buffer resolved by GL binding point
instead of attribute index; a stale descriptor consulted for the shadow on full re-upload; the shadow base not
published for a lazily twinned store) - the verification round was load-bearing. In flight: the full
five-part gate (three retrace sweeps) on ~/w7/pipe, the final whole-diff review (5cb826b0..3e298c9a), the
pull/push trace APKs for the D.4.2 device A/B, and the P3a docs.

## ID-14 Ownership waiver: `MG_Test/Buffer/BufferTest.cpp`'s fixture
C.5 gives that file to nobody. Under a push build the backend installs both `BufferBackendOps` and
`MGPipeResourceOps` at bring-up, so `ScopedBackendOps` (which scoped only the former) let 26 of the 86
BufferBackendOps dispatch cases route into the pipe. The integrator landed the one-scope fix espryt-v3 §7
measured (`83302ca2`): the fixture now saves/nulls/restores the pipe table exactly as ResourceEmitTest's
ApplierGuard scopes the applier. It weakens nothing (the pipe-side dispatch has ResourceEmitTest's own
coverage). Follow-up for a later phase: a pipe-shaped mock so the same 86 assertions run over the handle path.

## ID-15 Final whole-diff review = REWORK; C-2 ruling supersedes ID-13's "outside any #if" sentence
Full five-part gate on 3e298c9a: all green (G1 0/0/0/0, G5 byte-identical vs 44c2b5cf and 5cb826b0 + self-test,
HandleRecycle 60/60, unit 1619 x 3, G2 0, G14 0/+58, integration-gpu 958/958 x {0x1ff, 0, 0x7f} + the
ESPRYT_DISABLE_INVALIDATE_FLUSH arm, buffer/VAO family 202/202, g7 + G7 tripped, CsoContentAddressing +
ResourceSubsystemControl 10/10, controls 64/64, integration-verify 842/842 zero Fatal, retrace verify 79/79
armed zero Fatal, retrace push 79/79). The G3b named sub-sweep matched 0 cases because `retrace_gate.py
--only` is a regex and the gate script passed a comma list (fixed in wsl_p3a_gate.sh; the five cases passed
inside the full sweeps). final-review-v1: C-1 the client-minted VertexElementsCso slot has no backend-neutral
death path (Magma installs no death ops: leak per VAO at 0x1ff, Fatal past 65536 slots); C-2 FlushPendingRangesNow
sits inside the pull-only #else, so push builds compile only FlushPendingRangesFrom and G5's text hash does
not cover the shipping ladder. **Ruling for C-2:** FlushPendingRangesFrom becomes the ELEVENTH G5 row with
its own baseline sha (captured at 3e298c9a); FlushPendingRangesNow stays byte-identical where it is (G1 is
what protects the pull build); the two-arm shape is stated at Managers.cpp:1127-1138. M-3 (the fp64 stream's
dropped SyncGpuWrites site) is a RULED deviation for P3a. The rework runs on ~/w7/p3a-final (branch p3a/final
from 3e298c9a); it lands as the last P3a commits. The device A/B and the bench are recorded against the
3e298c9a APKs (the rework is lifecycle/seam correction, not a cost change) - the docs say so.

## ID-9 dev merged mid-phase: feat/disaggregated = `5cb826b0` (pushed), baseline re-captured there
`dev@9eae9858` (two commits past our P2 base: `d7655247` rebinds VAOs when an adopted store is respecified -
a 5-line fix in Managers.cpp's immediate retire path - and `9eae9858` adds the persistent-buffer ordering
POST probe) was merged into `~/w7/pipe` as `5cb826b0` right after the contract, so every remaining package
rebases across it once. **The `~/w7/p3a-before-*` baseline is now the pull build of `5cb826b0`** (2502 ctest
names, 1582 unit); G1 for every later package is measured against that. Consequences: espryt's rebase
conflicts in Managers.cpp and the d7655247 fix must be present in BOTH the legacy and the handle arm of the
respecify/retire path (the handle arm duplicates that core); gates' rebase conflicts in
LargeArenaAdoptionScenario.cpp (dev added cases there, gates added the mpr assertions and the second arena) -
resolve by union, keep every test name from both sides. wire and client do not touch those files.

## ID-10 Ownership grant: the three `*BaseInstance` draw entry points in `MG_Impl/GLImpl/GL_Drawing.cpp`
The brief's ownership table gives `MG_Impl/GLImpl/**` to nobody, yet D-H2 needs `MGPipeSetPendingBaseInstance`
called from the three `glDraw*BaseInstance` entry points (client-v1 finding 1: no call sites, so the emitted
BaseInstance is always 0). Package B's rework is GRANTED those three one-line call sites (and nothing else in
GL_Drawing.cpp); the reset in `MGPipeLeaveVerb` stays B's. Until they land, espryt's handle arm reads a
`VertexFetchBaseInstance` of 0 - the DrawParameters / VertexAttribBinding base-instance scenarios are the gate
that must go red without the call sites and green with them, and the integrator checks both directions once
on the finished tree. client v1: 73bab3d6 / 1ae6b4f4 / 71805d59 / a0ac79cc on p3a/client; review v1 in flight.

## ID-11 G5's byte-identical set is ten functions
The brief's G5 row names nine (`IsPoolable`, `EnrollIntoPool`, `AcquireFromPool`, `TrimBufferPool`,
`ClearBufferPool`, `ProcessDeferredBufferReleases`, `CreateRingStorage`, `RingAvailable`, `RingAllocate`) but its
text (BRIEF-P3A.md:420, :1708) twice calls `FlushPendingRangesNow` G5-protected and names it as the only
mitigation of the tier-drain p99 risk. Decision: **`FlushPendingRangesNow` is the tenth**. The handle arm must
call the untouched function; a copy under `#if MOBILEGL_PIPE_PUSH` is a G5 break espryt's rework restores.
gates v1: 1ef91dec / 588d25e9 / 80f6bf1b; review v1 = REWORK (four majors: three CSO stats lanes still pinned
0x7f, the ResourceSubsystemOn/Off entries unreachable from CI's -R, the G7 script not rebuilding on its
"did not trip" exit, FlushPendingRangesNow missing from G5); rework in flight, rebasing onto 5cb826b0 first
and undoing the second-arena workaround that d7655247 makes unnecessary.

## ID-12 client review v1 = REWORK; cross-package items
B-C1: Tracker.h:373 zeroes m_pendingBaseInstance in Reset() (called on every make-current) - delete the line.
B-C2: no re-publication path for resource records after MGPipeApplierReset - **resolved together with wire's
rework C1**: the applier keeps resource/vertex-elements records across make-current and clears them only at
context destruction; the client therefore needs no re-publication on FreshlyPrimed, but must state that as
the rule (not "absence as policy") and cover context destruction (ResourceDestroy from ~BufferObject is the
only path). The client rework starts after wire-v2.md exists and reads it first.
Majors for the client rework: the three call sites at GL_Drawing.cpp:662/684/713 (ID-10); Acquire mutating the
global slot allocator on Espryt's off-thread SubData path; the sampled BindMask's DSA hole; the absent
UnmapPersistent producer (record it or add it). Cross-package: the `hostBytes` base-vs-base+offset mismatch -
B is brief-conformant, **espryt must change** (goes into espryt's post-rebase verify round, ID-13 when opened).

## ID-13 G1 vs G5 on `FlushPendingRangesNow`: one untouched definition, a separate handle-arm ladder
gates v2 (`2bafb316`, 8 commits on 5cb826b0; rebased clean, D6 undone, M1-M4 closed, G5 = ten functions) reports
G5 rc 2 against espryt (`cdbd5535`): espryt's push arm redefines `FlushPendingRangesNow` as a 3-line forwarder
onto a new `FlushPendingRangesFrom` ladder (Managers.cpp:1051-1127), the original body living only in the
`#else` copy. Decision: **`FlushPendingRangesNow` is defined exactly once, outside any `#if`, byte-identical to
5cb826b0** (that satisfies G1 for the pull build and G5 for the text). The handle arm gets its own
`FlushPendingRangesFrom(...)` under `#if MOBILEGL_PIPE_PUSH` that takes the record/twin instead of the frontend
object; duplicating the tier ladder there is accepted for P3a (the same shape espryt already used for the
respecify core; the legacy copy retires at P13 - ARCHITECTURE.md:367). The pull arm never calls the new one.
A shared template over an accessor interface is NOT acceptable: it resizes the pull symbol (G1). espryt's
post-rebase round applies this; gates' G5 must then be rc 0 against espryt's tree.

## ID-8c espryt v2 = `6a1a9bc7` (16 commits on 5cb826b0), gates v2 = `2bafb316` (8 on 5cb826b0)
espryt v2: rebased onto 5cb826b0 (trap: the three-way merge put d7655247's ++g_bufferBackendIdGeneration into
the legacy arm only; fixed, both arms symmetric), C-1/C-2/M-1..M-5 closed, ID-11/ID-13 satisfied (one
definition of FlushPendingRangesNow, ten byte-identical), legacy arms 463/463, default arm 204/463 red with
the identical failing set before and after (stub-applier attribution still expected until client lands).
Next for espryt: after client integrates, a verify round on the rebased tree (default arm, buffer/VAO family,
DoublePrecision, base-instance scenarios both directions per ID-10, retrace), then a focused re-review.

## ID-2 Integration order: contract -> wire -> client -> espryt -> gates
The brief's D.1 order with `wire` inserted after `contract`: `client`'s emissions land in the applier's records only
once `wire`'s apply bodies exist, and `espryt` reads those records. `wire` is behaviour-neutral on its own (nothing
registers `MGPipeResourceOps` until `espryt`).

## ID-3 Tool ownership
`~/w7/notes/tools/wsl_p3a_gate.sh` and `p3a_ab.sh` are written by the integrator, not by package D; package D's
deliverables are the two `scripts/p3a_*.sh` in the repo, the scenarios, the itest CMake rows and `test.yml`. Tree
creation is `wsl_tree.sh p3a <slug> <base> [verify]`, integration `wsl_integrate.sh p3a <slug>...` (generalised P2
helpers; the phase is a parameter, not a copy of the script).

## ID-4 Models and rounds
Package agents and reviewers run on opus (user rule 2026-09-08: fable only for genuinely hard single items). One
adversarial review per package, at most two rework rounds; declared minors do not block integration.

## ID-5 Durable locations
Brief `~/w7/notes/p3a/BRIEF-P3A.md`; scouts `~/w7/notes/p3a/scout-*.md`; package results and reviews
`~/w7/notes/p3a/p3a-results/<package>-v<N>.md` and `<package>-review-v<N>.md`; A/B artefacts `~/w7/notes/p3a/ab/`.
Package logs under `~/w7/p3a-<slug>-*.log` are deleted by their owner when the round ends.

## ID-6 Performance is recorded, not gated (user, 2026-09-08)
Part 4 of the five-part gate and `ROADMAP.md:19`'s "MC 26.3 在 Adreno 上 p99 不变" are measure-and-publish items
against P2's pull baseline (`MEASUREMENTS.md` §10-§12). Parts 1, 2, 3, 5 stay hard gates.

## ID-7 Working over UNC and WSL
Package agents edit their worktree through `//wsl.localhost/Arch/home/swung/w7/p3a-<slug>/...` with the Read/Edit/Write
tools (file writes work; `mkdir` does not - create directories from a WSL script). Builds and tests run through a script
file: write `<scratchpad>/wsl/<name>.sh` (LF endings) and run
`MSYS_NO_PATHCONV=1 wsl.exe -d Arch -- bash /mnt/c/<scratchpad path>/wsl/<name>.sh`; never pass `~` or `$vars` inline to
`wsl.exe`. A build or test run longer than ~8 minutes is launched with `setsid nohup ... &` inside the script and polled
through its log.

## ID-16 CI reds on 42d43af2 / b9eaa474 / 31e370be are GitHub artifact-download flakes
Every failing job is a `retrace`/`retrace verify` matrix entry whose "Download Linux runtime" / "Download trace
fixture" step died with `digest-mismatch` and `Artifact download failed after 5 retries`; no build or test
step ran red. 12e6bfcf (wire) was green on the same tree family. The failed jobs of the 31e370be run were
re-run; 83302ca2 and 3e298c9a are queued behind them. A red of this shape is not a code signal (the user's
rule: only real retrace reds after a successful download matter).

## ID-17 P2's device A/B APKs were unoptimised; the Release baseline of record is P3a's pull arm
The P3a pull APK's libMobileGL.so (Release, arm64) is 15.5 MB with a 9.4 MB .text; the P2 A/B's pull APK
(`~/w7/notes/p2/apk/trace-pull.apk`) is 43.2 MB with a 21.8 MB .text, same clang 18.0.4, no .debug_info in
either - a 2.3x larger .text is an unoptimised (Debug-flavour, -O0) build, and it replays 26.3 on the Oppo
at 42 ms/frame CPU where the Release build takes 9.7 ms. So MEASUREMENTS.md §10's absolute numbers and its
+8-18% deltas are -O0 readings (the tracker's inline-heavy code pays most at -O0). Decision: the P3a A/B on the
3e298c9a Release APKs is the two-device baseline of record; its pull arm is symbol-identical to the P2-era pull
path (G1 across P3a; only d7655247's 5 lines differ from 44c2b5cf). To re-derive P2's own cost at Release, a
supplementary arm runs the SAME push APK with `--env MOBILEGL_PIPE_PUSH=0x7f` (P2 subsystems on, P3a off) =
the device-side T2 (`p3a_ab_7f.sh`, nofinish only, its own reboot-clean session). The docs get an erratum on
§10 and the corrected table; the user is told that the "~10% is acceptable" call was made on -O0 numbers.
`wsl_build_trace_apks.sh` prints the lib size so an -O0 APK can never pass unnoticed again (Release .text ~9.4 MB).

## ID-8f final rework integrated: feat/disaggregated = `c20e2f2b` (pushed) = the P3a landing hash
p3a/final (12 commits on 3e298c9a, final-v2.md) fast-forwarded: pull symbols 0/0/0/0 vs b9eaa474, G5 eleven
functions byte-identical vs 3e298c9a AND the ten vs 5cb826b0, unit 1622 x 3, G2 0, G14 0 removed / +69,
HandleRecycle+VertexElements+Leak (verify) 76/76, Magma CrossFrameBuffer verify 13/13 locally. Open: the
CI-only teardown `double free or corruption` on DirectVulkan.Verify.CrossFrameBufferScenario.{Vertex,Index}
CopyBufferSubData (3e298c9a run and its re-run; ASan reproduction in flight on a private worktree) - whether
c20e2f2b's lifecycle fixes change it is answered by c20e2f2b's own CI run. Docs: P3ALANDHASH -> c20e2f2b;
the device A/B and the bench remain recorded against the 3e298c9a APKs (ID-15/ID-17).

## ID-18 The CI-only teardown abort is a static-destruction-order UAF at exit; fix = never-destroyed singletons
asan-doublefree-v1: `exit` -> `PipeInputs::~PipeInputs` (namespace-scope `gPipeInputs` holds a
SharedPtr<VertexArrayObject>) -> `~VertexArrayObject` -> `~BufferObject` -> `MGPipeEmitResourceDestroyAndFree`
-> `MGPipeSlotAllocator::FindByLifetimeId` on a hash table already freed by `~MGPipeSlotAllocator`
(`MGPipeSlots()` is a Meyers singleton constructed at the first buffer, so it dies first). Every case executes
the chain; only when the freed table still resolves does `Free()` write freed vectors and glibc reports
`double free or corruption` - CI's allocator layout did, ours did not (native repro: `GLIBC_TUNABLES=
glibc.malloc.tcache_count=0` makes exactly the two CI cases fail on c20e2f2b and none on the fix). Fix
(p3a/asan, rebased onto c20e2f2b as d54ec57a): `MGPipeSlots()`, `MGPipeResourceTrackerInstance()` and
`g_applier` become never-destroyed (heap-constructed, intentionally leaked at exit); the C-1 rework's second
reaching path (`MGPipeEmitVertexElementsDestroyAndFree` from `~VertexArrayObject`) is covered by the same
change. Integrated after the P3a bench finishes reading pipe's libraries.

## ID-19 P3a DriverBench (desktop, c20e2f2b libs) - recorded, not gated
mc_vanilla_draw ns/draw: Espryt pull 5115 / push(0x1ff) 6162 / push(0x7f) 5463 / push(0) 5693; Magma pull 17099 /
push 17709 / push(0x7f) 17287 / push(0) 17739. T1 = +1048 (+20.5%) / +611 (+3.6%); T2 (0x7f = P2's boundary) =
+349 / +189; **P3a itself = +699 / +422 ns/draw** - the largest boundary cost so far (per-draw vertex-input emission +
applier record + Espryt's record walk; Magma's share is pure client emission until P7). blend-toggle +5.2% / +2.8%;
pass switch ~0; CSO content addressing ~0. Goes into the optimisation-phase list (per-draw suppression of
unchanged vertex-input sets; in-place applier record updates). Device side (Release APKs, 3e298c9a): Oppo 26.3
Espryt +8% / Magma +10%, rd12 +30% / +27%, sodium ~0 (sub-ms frames), create-instancing is a 2-frame fixture
(not a benchmark). 0x7f arms and the Xiaomi pass in flight.

## ID-20 CI Test lane green at c20e2f2b
The lifecycle changes of the final rework moved the allocator layout enough that CI's two teardown aborts
(3e298c9a) no longer surface at c20e2f2b - not a fix, a layout change; the systematic exit-order closure
(ID-18 + the 21 knob-only cases) lands as the last P3a commits on p3a/exit and is proven with
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 on both lanes before the docs push.

## ID-21 exit-order closure integrated: feat/disaggregated = `fde5fda3` (pushed) = the P3a landing hash
p3a/exit (6515c8e6 leak-at-exit storage for the four static PipeInputs holders; fde5fda3 the four remaining
MGPipe singletons never-destroyed) on d54ec57a, fast-forwarded. On pipe: pull symbols 0/0/0/0, G5 eleven
byte-identical, unit 1622 x 3, G2 0, G14 0/+69, and with GLIBC_TUNABLES=glibc.malloc.tcache_count=0
integration-verify 844/844 and push integration-gpu 966/966. Follow-up outside P3a: IntegerBorderColorScenario's
stack-buffer-overflow (pre-existing test bug, ASan). Docs: landing hash -> fde5fda3.

## ID-22 CI at fde5fda3: every build/test job green; the run is red only in "remove artifact clutter"
Its "Delete intermediate Linux retrace artifacts" step died on a GitHub HTTP 504 from `gh api --method DELETE`
(housekeeping after all tests passed). Not a code signal. The Test lane's authoritative jobs (pipe-gates,
include-graph-check, test, integration, build-linux-verify, integration-verify, retrace, retrace-verify,
monolith-symbol-report) all succeeded on the P3a landing hash.

## ID-23 rd12 on Magma on the Xiaomi aborts on every arm (pull included): pre-existing, excluded from the table
scudo map error inside a calloc from libMobileGL (frames 0x818b14 / 0x7e2c44, stripped) seconds after start,
with 6 GB available - a giant/negative allocation on the Adreno 830 + Magma path for this fixture, not P3a's
(the pull build is symbol-identical to the pre-P3a path). The same fixture runs on the Oppo/Magma and on the
Xiaomi/Espryt. Task chip opened (symbolize + fix). Side effect: the GPU pwrlevel pin on the Xiaomi resets after
the crash (pin_device reports "range 2..5"), so the Xiaomi's sodium and create-instancing rows after rd12 carry
a DRIFT verdict on both arms (still paired). The P2 -O0 Xiaomi table did not include rd12, so nothing is lost
against P2.

## ID-24 P3a closed: docs at `37da3c3a` (pushed); device A/B complete on both devices, three arms
Release APKs from 3e298c9a; pull / P2-only (0x7f) / P3a (0x1ff), nofinish primary. P2's boundary at Release =
+6-12% p50 on both devices and backends; P3a adds ~0 on 26.3 and sodium but +17-19 points on rd12 (Espryt
+30%, Magma +27% total) - per-draw vertex-input emission on VAO/buffer-heavy frames; MC 26.3 p99 on Adreno
pull 25.46 -> P3a 26.32 ms (+3.4%). mpr on device: 26.3 = 8 per run, sodium 1. Excluded: Xiaomi rd12/Magma
(pre-existing abort, ID-23), create-instancing (2-frame fixture), sub-ms sodium rows (noise). Artefacts:
~/w7/notes/p3a/ab (60 benchmark.json) and ab7f (15), ab3-final.md; bench ~/w7/notes/p3a/bench/driverbench.md.
Next: P4a (handle wave 2: FBO / texture / sampler / program). Optimisation-phase list carries: the per-draw
vertex-input emission cost, IntegerBorderColorScenario's ASan finding, rd12/Magma/Adreno abort, the
harness pidof single-sample kill, create-indirect on dev.
