# c1f-v1 — client producer gates and shared teardown refusal

Implementation commit: `6d86f9d9` (`5682f429..6d86f9d9`). Acceptance execution is recorded below.

Base: `p5/c1@5682f429`. Owned worktree: `/home/swung/w7/p5-c1f`; branch `p5/c1f`.
No edits to the original c1 report or other package worktrees.

## Changes and the control boundary

N-2 is the production change: `RequireClientTablesInstalled` owns the refusal check used by
both `WireTables.cpp` and `EmitTables.cpp`, before session lookup or ring access. Five controls
start a real session, uninstall the wire tables while that session is still live, then call
Clear, DrawArrays (DrawVbo on the wire), ReadPixels, BlitFramebuffer (Blit), and Present through
the emit table. Each requires `Fatal{ClientTablesUninstalled, "<its slot>"}` and SIGABRT.

N-1 adds `PipeCatalogue.FrontendNeverTakesAnApplierAddress` to every unit lane. It scans the
checked-out `MG_Impl` sources, removes comments, and rejects by-address applier references;
the routing tables live outside this directory. Missing source is a failure, never a skip.
The false red-check comment in PipeFill is replaced with this test's name. Its runtime partner
creates and defines a texture through GLImpl, lets an accepting peer reply through the real
reply pool, then observes `ClientWireRecordsEmitted()` across `DeleteTextures`. It does not
set the publication latch or mint the observed delete record.

N-3 controls call the installed ReadPixels emitter over a real ClientSession and server apply
thread. The peer verb sink observes the decoded production `MGPReadbackInfo::DstSize`; it
does not construct that field. Adversarial replies use the decoded sequence and real reply
pool: OK/32 bytes for a 48-byte read on both fast and bounce paths, and ERROR/0 bytes. Each
death requires its named ReadPixels diagnostic. This reaches both production checks and the
production size assignment, rather than merely their helper functions.

N-4 installs an adversarial peer reply sink on the apply thread, invokes the two installed
escape routes, and posts ERROR on their actual sequence. The controls require respectively
`Fatal{ReplyError, "resource_respecify"}` and `Fatal{ReplyError, "map_persistent"}`.

The peer substitutions use test-local explicit member-pointer instantiations to reach the
existing server decoder/backend interfaces. They add no production test knob, modify no
server source, and change no client state being asserted. All member replacement runs on
the real apply thread through `RunOnApplyThread`.

Codex 12 needed a gate, not another production fix: c1's current MakeEGLCurrent already pumps
after every successful non-release call. The new control uses the real client and server
forwarders and a server backend double. It changes the server compute limit from 113 to 227,
repeats make-current, then immediately reads mirror generation and GetIntegeri_v. It does
not call PumpControlPlane or Present to prepare the observation.

N-5's previously missing executable controls now include server-role buffer respecify,
server-role flush, and header-only dynamic-state emission. Respecify runs BufferObject's
producer on the real apply thread; flush observes the resource-op consumer callbacks to
exclude the client-only subdata follow-up. The dynamic-state case submits a valid empty-chunk
input through the installed wire row and observes its emitted ordinal.

M8 has a CPU unit control binding a real 64-byte pack buffer through GLImpl and requesting
offset 16, plus an explicit native GPU control in the same committed test binary:
`RemoteClientTest --c1f-pack-gpu=monolith` / `--c1f-pack-gpu=inproc`. The native control uses
the shipped EGL/GL frontend and real DirectGLES driver; it cannot silently skip. It is an
explicit command because a native driver is not a prerequisite of the unit label.

## Minors and interpretation of the six PARTIAL items

- N-6: B3 now asserts each of the 33 generated routed rows and four escape rows by name,
  in addition to the counts. The compensating wrong-row mutation keeps the count at 33.
- N-7: the shutdown comment now explicitly states that ReadPixels refuses DECLINED as
  `Fatal{ReadbackDeclined, "ReadPixels"}` because it has no complete pixels to return.
- N-8 merge instruction: when merging v1 round 2, keep c1's Wire/CMakeLists.txt foreach
  registration and discard v1's duplicate ServerLoopTest block. No second registration is
  needed. This is a merge instruction, not an unauthorized CMake edit in this package.
- N-9: the gate extracts only `Test #N: ...` lines from unfiltered `ctest -N`, over every
  label. The `Total Tests` footer is never a name.

The review explicitly labels B1, M2, M3, M4 and M8 PARTIAL. Its stated total of six does not
identify a sixth separate PARTIAL heading. The scoped codex-4 gap is N-2 and is handled here.
Codex-7's fill-side barrier check remains the accepted p1-owned request; M1's production
arming remains the integrator's/v1's ScopedApplierEntry hunk. Neither is silently claimed
as a c1f production change. The review expressly accepted the old m1/m3/m4/m5 deferrals.

## Corrections to c1-v3's unsupported statements

1. The old B1 grep gate did not exist. The old purported B1 red-once was not executed; the
   review showed its revert kept 2030 tests green. The committed source and wire gates here
   replace that unsupported claim.
2. “Every gate ships its red-once line” was false for B1, escape M4, M5, codex 12 and
   BlobMissing. Only the executed controls listed in this report are claimed here.
3. A shared tight-size helper did not gate `info.DstSize = tight`. The new peer observes
   the emitted field, and the exact `tight + 16` production mutation is tested.
4. The old M8 `if (true)` perturbation demonstrated reachable Fatal code, not detection
   of a real pack binding. The new controls bind a 64-byte PBO and use offset 16.
5. c1's 14/21 run was its own head plus the Init hunk, not a joint with v1 round 2.
   This report identifies the exact scratch merge used for its joint result.
6. The old codex-12 line supplied no executed before/after gate. The new repeated-current
   test measures the generation and public answer without a later pump.
7. Installation follows an attempted initial caps pump; Start can warn about a missing
   snapshot and continue. ClientWireRecordsEmitted counts routed records; the integration
   harness still arms on the encoder ordinal. Its new test reader does not change that.
8. The supplied before-name list contains 2,902 total entries, including 1,117 GPU entries.
   Therefore 1,128 current GPU names cannot be literally identical to that list; report
   zero removals and the pre-existing additions separately.

## Executed mutations and acceptance

Committed runner: `MobileGL/MG_Test/Wire/c1f_redcheck.py`. Command from the worktree with links
on: `python3 MobileGL/MG_Test/Wire/c1f_redcheck.py`. Final log:
`/home/swung/w7/p5-c1f-redcheck-final.log`, **exit 0, 15/15 RED, 15 restored GREEN,
`FAILED_CONTROLS=[]`**. It requires a successful build, nonzero test status, and each selected
test's own `[  FAILED  ] Suite.Name (` line. Every baseline and restored baseline must contain
each own OK line. An empty selection fails. The verdict self-test is **5/5**: unrelated crash,
all-green output, build failure, forged failure text with exit zero, and genuine own failure.

Each row below is an executed “I made it red once by doing X” line; the right column is
verbatim from the final log. Full gtest failures and private Fatal output are in that log.

| Item / perturbation | Executed red line |
|---|---|
| N-1/B1: revert the texture destroy address to `&MGPipeApplyResourceDestroy`; committed source scan | `B1-source: RED build=0 run=1` |
| N-1/B1: same revert; observe DeleteTextures wire ordinal | `B1-wire: RED build=0 run=1` |
| N-2: delete the class-B shared refusal call; all five named family tests fail | `N2-five-verbs: RED build=0 run=1` |
| N-3/M2: reviewer's exact deletion of both `RequireReadbackReplyComplete` calls; short fast, short bounce, ERROR all fail | `M2-production-deleted: RED build=0 run=1` |
| N-3/M2: delete only fast-path check | `M2-fast-deleted: RED build=0 run=1` |
| N-3/M2: delete only bounce-path check | `M2-bounce-deleted: RED build=0 run=1` |
| N-3/M3: reviewer's exact production `info.DstSize = tight + 16` | `M3-production-plus16: RED build=0 run=1` |
| N-3/M3: add 16 inside the shared tight-size function | `M3-shared-size: RED build=0 run=1` |
| N-4/M4: delete both escape `status == 2` Fatal blocks; each escape test fails | `M4-escape-errors-deleted: RED build=0 run=1` |
| N-5/M5: delete role guards; server respecify fails | `M5-server-role-guards-deleted: RED build=0 run=1` |
| N-5/M5: delete only flush role guard; extra subdata observed | `M5-server-flush-guard-deleted: RED build=0 run=1` |
| M8: disable real pack-binding detection; offset-16 control loses its named refusal | `M8-real-pack-binding: RED build=0 run=1` |
| Codex 12: restore the skip of the post-make-current pump/refresh block; generation/getter control fails | `codex12-repeat-skip: RED build=0 run=1` |
| N-5/BlobMissing: change optional dynamic-state staging back to mandatory | `BlobMissing-optional-to-required: RED build=0 run=1` |
| N-6/B3: replace the ResourceDestroy assignment with FenceCreate; same count, wrong identity | `N6-compensating-wrong-row: RED build=0 run=1` |

Representative verbatim diagnostic from the M5 red:
`MGPipe: Fatal{InitialBytesNotCarried, "resource_respecify"} - the respecify crossed with initialBytes = nullptr (R-13.3) and the resource_subdata records that were supposed to follow it emitted NOTHING, so 64 bytes of initial content exist on no side of the wire`.

The initial development campaign (`/home/swung/w7/p5-c1f-redcheck.log`) intentionally remains
cited as an audit trail: it exited **1**, correctly refusing to count a missing-source baseline,
a missing-include build break, and an incompatible-pointer mutation as verified reds. The
source-root lookup, include, and mutation were corrected before the full successful rerun.

Native monolith pack control already executed:
`C1F_PACK_MONOLITH bytes at offset 16: 0 255 0 255` and
`C1F_PACK_MONOLITH: PASS (exited 0)` in `/home/swung/w7/p5-c1f-pack-monolith.log`.

## Acceptance numbers

Package head `6d86f9d9`, **all four configure/builds exit 0**. Driver-independent lanes and
GPU lanes ran with `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`.

| Gate | Executed result |
|---|---|
| G1 `symbol_report.py --before ~/w7/p5-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 resized / 0 renamed**; 27,814 symbols unchanged |
| G1 `.text` | **10,806,611 → 10,806,611 (+0)** |
| G1 `.data / .bss / .rodata` | **76,840 / 1,296,872 / 1,536,602**, each **+0** |
| G1 total / on-disk library | **17,227,007 → 17,227,007** / **19,114,800 → 19,114,800** |
| Pull / push / verify `ctest -L unit` | **1,817/1,817 each**, 0 failures; baseline 1,816 + the new source gate |
| Split `ctest -L unit` | **2,048/2,048**, 0 failures; baseline 2,030 + 17 live controls + source gate |
| Push `integration-gpu`, monolith | **1,128/1,128**, 0 failures |
| Split `integration-gpu`, monolith invocation | **1,149/1,149**, 0 failures; the 21 Split entries force inproc and skip without the local Init hunk, as on the reviewed base |
| G5 p3a / p4a, `ff2994d9 HEAD` | **both exit 0, byte-identical**; the existing ID-41 note is not a new verdict |
| `gen_pipe.py --check` | **exit 0**, up to date |
| `gen_pipe.py --self-test` | **9 negative controls + positive control**, exit 0 |
| field ownership `--check` | **exit 0**, up to date |
| include closure | **4 probes, 0 skipped, 0 problems** |
| Full unfiltered CTest names, before / pull / push / split | **2,902 / 2,945 / 2,945 / 3,197** |
| Before → push names | **0 removed / 43 added** (42 pre-existing additions + new source gate) |
| Push → split names | **0 removed / 252 added** (235 prior additions + 17 new live controls) |
| Pull → push names | **0 removed / 0 added** |
| GPU before → push names | **0 removed / 11 added**, all 11 already on the reviewed base; exact names in the delta artifact |
| Red-check campaign | **15/15 RED**, all restored GREEN, **exit 0**; verdict meta-controls **5/5** |

Main gate log: `/home/swung/w7/p5-c1f-gate.log`, ending `C1F_GATE_COMPLETE`, **exit 0**.
The symbol tool itself describes the requested invocation as informational (no `--gate` flag);
the actual measured changes are all zero, as reported above.

## Scratch joint, native PBO control, and a new integration finding

Scratch branch `p5/c1f-joint-check`, merge commit **`8a92bbe2`** = `6d86f9d9` + pinned
v1 round-2 head **`ca92308b`**. N-8's sole CMake conflict was resolved with c1's foreach loop.
The two integration changes were applied **locally, not committed**: Init's direct remote
construction and the ScopedApplierEntry bracket in PipeApplier. The saved v1 diff's applier
context was stale against round 2; the equivalent local patch is preserved at
`/home/swung/w7/notes/tools/p5-c1f-joint-applier.diff`.

- Joint configure/build **exit 0**.
- Joint `MOBILEGL_TRANSPORT=inproc ctest -L integration-split`: **21/21**, 0 failures,
  **19 ran / 2 PersistentMapArm counting entries skipped**.
- Native monolith PBO command: **exit 0**, observed bytes **0 255 0 255 at offset 16**.
- Native inproc PBO command: control **exit 0**, child **SIGABRT (6)**, exact diagnostic
  `MGPipe: Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}`. It does not return or dereference 16.
- Additional joint unit check: **2,055/2,056**, **one failed** — the new repeated-make-current
  control. This is deliberately reported, not hidden behind the green 21-case integration lane.

**Integrator request / codex-12 joint closure remains OPEN.** v1 round 2 changed
`ServerMakeEGLCurrent` to return on `!outcome.boundNatively`, before `PublishCapsSnapshot`.
An identical successful make-current therefore publishes no new snapshot. The c1 client
correctly pumps, but there is nothing to adopt: the control exits 90 with the mirror still at
generation 2 and the old public getter answer. Verbatim failure:
`[  FAILED  ] RemoteClientControls.RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent (0 ms)`.

This is a real cross-package behavior difference from c1's base, not a missing client gate.
Per ID-64 and the ownership restriction, c1f does **not** change ServerLoop or weaken the
control. Requested ruling/fix: retain ID-54's no-native-rebind behavior, but publish caps on
every successful **non-release** make-current, including an identical tuple, independently
of whether a native bind occurred. If only real binds should publish instead, that is a
contract/control change requiring the integrator's ruling against this assignment's explicit
repeat-current requirement. Reading the subsequently advanced `p5/v1@13043e3f` source found
the same `!outcome.boundNatively` early return; **that newer head was not executed here**.

Thus codex 12 is client-gated and green on `p5/c1f`, but is **not claimed closed on the joint**.
The new gate has already caught the behavior the older joint's 21/21 did not observe.

Both local hunks were reverted after execution and the worktree returned to `p5/c1f`.
The restored package build exited **0** and its **18/18 new cases passed**. Final git status
is clean, submodule links are **off**, and the committer is
`Swung0x48 <swung0x48@outlook.com>`. The joint caps finding above is a **major integration
finding requiring a v1/integrator ruling**, not a waived or skipped test.

## Retained evidence and reproducibility

All root-level evidence below has the `/home/swung/w7/p5-c1f-` prefix. Other temporary
development logs are removed. The scratch branch is retained for the pinned merge identity;
it does not contain either local integration hunk.

- `gate.log`, `symbols.log`, `p3a_untouched_regions.sh.log`, `p4a_untouched_regions.sh.log`.
- `build-linux.log`, `build-push.log`, `build-verify.log`, `build-split.log`.
- `unit-linux.log`, `unit-push.log`, `unit-verify.log`, `unit-split.log`.
- `igpu-push.log`, `igpu-split.log`.
- `redcheck.log` (rejected development campaign), `redcheck-final.log` (successful full campaign).
- `names-before.txt`, `names-linux.txt`, `names-push.txt`, `names-split.txt`.
- `names-before-push-removed.txt`, `names-before-push-added.txt`,
  `names-push-split-removed.txt`, `names-push-split-added.txt`,
  `names-linux-push-removed.txt`, `names-linux-push-added.txt`.
- `gpu-before.txt`, `gpu-push.txt`, `gpu-name-delta.txt` (the 11 exact pre-existing additions).
- `joint.log`, `joint-build.log`, `joint-integration.log`, `joint-unit.log`.
- `pack-monolith.log`, `pack-inproc.log`.
- `final-build.log`, `final-controls.log` (restored c1f build and its 18 new cases).
- `submodlinks.sh` (owned-tree link toggle, uses no git command while links are on).

The scripts under `/home/swung/w7/notes/tools/` are `p5-c1f-build.sh`, `p5-c1f-gate.sh`,
`p5-c1f-joint.sh`, `p5-c1f-joint-run.sh`, and `p5-c1f-joint-applier.diff`. The original joint
log retains the stale-patch application failure before the corrected local application;
the build and run logs are from the corrected form. The joint unit failure stops the joint
script nonzero; the two native pack commands and cleanup were then executed separately.
