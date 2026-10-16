# P5b phase-close Codex review

Date: 2026-09-16. Reviewed tree: `/home/swung/w7/p5b-integrate-codex`.
Pinned target: **348d22a4b9c15cc571af840dedc4b4edc8c53dbb**; diff base: **37fc4fdb**.
HEAD was rechecked at the end and still matched. This is the phase's one closing review,
not an additional implementation-round review.

**Verdict: 0 blocker, 2 major, 1 minor.** The two new major findings concern the now-live
user-index carrier and the now-live blocking sync call. The minor is the previously named
ReadPixels reply-status gap, which remains present. Findings below are source-confirmed;
no new runtime reproduction, mutation, build, broad test, or device operation was performed.
They do not claim that the four selected Minecraft targets exercise these edge cases.

## Findings

### 1. A user-index span can declare fewer bytes than the draw consumes — major

Locations:

- `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1049`: encoder host-span check.
- `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1966`: decoder host-span check.
- `MobileGL/MG_Remote/Server/PipeApplier.cpp:555`: the sink resolves the span.
- `MobileGL/MG_Remote/Server/PipeApplier.cpp:593`: the ordinary indexed arm hands the
  resolved pointer and independent range count to the backend; the other indexed arms do
  the same at lines 582, 586, 590, 598, 602, 607 and 610.

`CheckHostSpanIsHonest` proves that **the declared** `Offset + Size` lies in a registered
segment. Neither codec direction compares `Size` with `MGPDrawRange::Count * IndexSize`.
The sink casts `range.Count` to GLsizei and passes it on without that check. Once the pointer
is resolved, `span.Size` no longer constrains the backend's reads.

Concrete corrupt-record trigger: one direct indexed draw, `IndexSize=2`, `NumDraws=1`,
`Flags=kDrawHasUserIndices`, range `{Start=0, Count=3, IndexBias=0}`, and a null-Ptr span
`{Seg=SEG_STAGE, Offset=segment_capacity-2, Size=2}`. The existing layout, tail-size and
four honesty checks all accept this shape. The sink then asks the backend to consume six
index bytes from the last two bytes of the segment. Even away from the segment boundary,
this can consume bytes from an adjacent staged allocation rather than the declared run.

This is a newly armed P5b carrier-boundary defect, not the documented P8 host-index mirror
deferral. The ordinary emitter currently stages `count * indexSize`, so a healthy emitter
does not itself produce the mismatch; the missing defense is against corrupted/inconsistent
wire input, which the codec expressly promises to reject before handing the sink validated
arguments. No driver read outside memory was deliberately executed during this review.

Suggested bounded fix: share a DrawVbo user-index validation helper between encode and
decode. For the supported one-range client-index shape, validate the index width, range
shape and stage carrier, and require the declared run to cover the count-derived extent
(the producer's exact-size contract permits equality). Use checked/widened arithmetic.
Keep the already documented multi-draw client-index refusal. A focused short-span negative
case and its exact-span positive counterpart are sufficient evidence for this finding.

### 2. A legal long ClientWaitSync is cut short by the 30-second transport watchdog — major

Locations:

- `MobileGL/MG_Remote/Client/EmitTables.cpp:1483`: `EmitClientWaitSync` preserves the
  application's Uint64 timeout, then calls `ReadFenceReply` at line 1471.
- `MobileGL/MG_Remote/Server/PipeApplier.cpp:127`: `OnFenceWait` forwards that timeout
  unchanged to the native backend at line 137.
- `MobileGL/MG_Remote/Client/ClientSession.cpp:126`: fixed `kBarrierTimeoutMs = 30000`.
- `MobileGL/MG_Remote/Client/ClientSession.cpp:779`: all reply-owning records use that
  fixed timeout for `WaitForApplied`; line 797 aborts on expiry.
- `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:12114`: the backend synchronously waits
  in native `glClientWaitSync`. DirectVulkan's wait entry at `DirectVulkan.cpp:1045` is
  also reached through the same client deadline.

Concrete trigger: an unsignaled fence and
`glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 60000000000ULL)`, while valid queued
work keeps the native wait outstanding beyond 30 seconds. The server is still carrying out
the application's permitted 60-second wait, so it cannot post the result or advance
appliedSeq yet. At 30 seconds the client aborts with `Fatal{BarrierTimeout,"FenceWait"}`
instead of returning the eventual native wait result. A timeout longer than 30 seconds is
not rejected by frontend GL validation and CONTRACT-P5B's sync appendix preserves the
64-bit timeout. This is different from a wedged ordinary verb: the call explicitly authorizes
the server to remain inside it for that interval.

Suggested minimal fix: derive the **applied/reply** wait budget for `FenceWait` from its
declared timeout, plus the existing transport grace period, leaving command-capacity waits
and all other opcodes unchanged. Round nanoseconds upward, handle overflow and the doorbell's
Uint32-millisecond / wait-forever sentinel deliberately, and retain shutdown wakeup. A budget
helper can be tested with 0, a short wait, 60 seconds and UINT64_MAX without a 30-second sleep.
Alternatively split the server's native wait into bounded pieces while preserving the full
client-visible timeout and final GL result; merely clamping the native timeout changes GL
semantics and does not close this finding. Update the timeout diagnostic to report the actual
selected budget. `FenceWaitServer` is a server-side GPU ordering call and should not inherit
the application's GL_TIMEOUT_IGNORED as a CPU deadline by accident.

### 3. ReadPixels still accepts an unknown reply status when the byte count matches — minor

Locations: `MobileGL/MG_Remote/Client/EmitTables.cpp:863`, used at lines 969 and 982;
the stricter exposed predicate is at line 1792.

`RequireReadbackReplyComplete` rejects ERROR and DECLINED individually, then checks only
the byte count. An exact-sized reply with status 3 (or another non-OK value) passes and is
returned/scattered as pixels. `ReadbackReplyIsComplete` correctly requires status OK, but
the production helper does not invoke that predicate. Reply-slot reading does not reject
unknown status values earlier.

This was already named in the P5 close review's unassigned-debt section and r2's follow-up
table. It is not a newly observed normal server output: the production server currently posts
the expected statuses. Close it by requiring OK after the named ERROR/DECLINED diagnostics,
and drive the actual live readback return path with an exact-sized status-3 answer. Testing
only the exposed stricter predicate would miss the current disconnect again.

## Previous P5 findings 1–13

The inspected integration contains the intended r1/r2 repairs. No additional defect in those
repairs was established during this review. This is source review plus inspection of the
package evidence references, not a claim that their runtime controls were rerun here.

| Prior finding | Reviewed disposition at this target |
|---|---|
| 1 coherent-map server producer re-entry | Backend sync early-outs on the apply role; producer guards precede map reads / serial mutations. r1's client-only suppression report addresses the earlier shared-knob blind spot. |
| 2 frontend framebuffer death | The callback synchronously uses the apply control mailbox before twin destruction/cache edits; reentrant server calls stay on the apply thread. P6 still owns separate-process lifetime transport. |
| 3 PACK_SWAP_BYTES | Client applies component/packed-word swaps on both tight and scatter paths, with the mixed float/depth-stencil type split into two 32-bit words. |
| 4 false RingOverrun on delayed retirement | Staging waits after immediate reclaim; command retry retains both draw tails and accounts for wrap padding. Individually oversized allocations still refuse. |
| 5 census prevents CI controls | Broad inproc status is recorded as debt; hard reduced gates remain; negative controls have `!cancelled()`. |
| 6 pull control poisons following draw-drop | Frozen library backup/EXIT restoration exists; both controls check library symbols, including nm failure. |
| 7 intentional E3 skips and combined diagnostic | E3 selects the four pixel cases; assertions and private logs are checked per selected entry. |
| 8 reclaim count mislabeled as wait | Immediate reclamation does not increment waits; outstanding-retirement blocking does. SmallRing asserts an actual wait as well as wrap. |
| 9 all-skip false green | Baseline failure/nonzero exit/all-skip rejects; CI has the runtime-only nonempty-success tally. |
| 10 optional EGL unit proof | Split unit CI explicitly requires GPU. |
| 11 census zero-run / stale log | Updated external runners clear private logs before launch, reject zero executed gtests, and retain process-status distinctions; these external tools still must accompany the integration. |
| 12 pin self-test bypasses production selection | Both revised self-tests call production `extract_baseline`, rather than directly testing the pin helper alone. |
| 13 obsolete c1f test name | The runner selects the current different-tuple case and checks the identical-tuple companion in baseline/restoration. |

## Scope and limits

Read CONTRACT-P5/P5B, the P5b brief, prior P5 closing review, and the r1/r2/d1/i1/t2/f1,
named-blit, mip and sync reports; inspected the affected emit/codec/sink/lifetime paths and
CI/control changes. Also inspected the new Redmi runner/reducer's exact-head prerequisites,
fresh attempt paths, fixed serial, stage profile and completeness checks. No new general
runner false-green finding was established.

The 256 MiB stage profile is explicit and applies to the four A/B arms; the production
32 MiB default and its large-upload boundary are recorded limits. The named-blit scoped
binding override and mip descriptor's existing identity lookup remain intentional inproc
barrier dependencies. They are not claimed as spawn-safe. Client vertex arrays, resource
identity pulls, P7/P8/P9 readback/query work, rd12, and the known deferred shapes remain their
recorded debts, not newly relabeled blockers in this review.

In particular, i1's old comment claiming the missing copy shadow is always bounded by a
GetTexImage Fatal must be read with the recorded frontend-shadow fallback debt: the frontend
can still select its local GetTexImage fallback. The established texture readback failures
already cover that limitation; this review does not count the same debt as a new finding.

The final host gate, merged census, APK build and Redmi runs were underway concurrently.
Their incompleteness at review time is not a finding, and no successful phase exit or final
binary identity is inferred here. The integrator must append their actual pinned results
and the targeted closure evidence for findings 1–3 before claiming P5b complete. This report
does not request a second full review or a repeated full gate after focused repairs.

## Integrator addendum, 2026-09-16 — pinned results and closure evidence

All concurrently-running evidence named above has since completed; nothing below reruns a
gate or a review, it appends the pinned outcomes the report asked for.

- **Host closing gate (`348d22a4`, `~/w7/p5b-final-host-348d22a4/`)**: the WSL restart left
  31/32 steps at rc=0; the missing `retrace-verify` arm was resumed against the SHA-256
  pinned verify library (`verify-recovery-r1/`), 75 remaining entries serial, exit 0.
  Combined: **retrace-verify 79/79**, retrace-push 79/79, OpenRA 2/2; all unit/gpu/split/
  verify/audit lanes 0 failed. Step-by-step reconciliation in `reconciliation-20260916.md`;
  `exit-code.txt=0`, `complete=true`, `summary-counts.{json,md}` regenerated.
- **Merged census (`348d22a4`, `~/w7/p5b-final-census-348d22a4/`)**: lane **1267 selected =
  811 passed / 203 skipped / 62 aborted / 191 failed**, zero regressions against the c0b
  baseline (1149 retained, 118 added, 0 removed). Trace census **79 = 72 passed / 6 aborted
  / 1 failed** after a three-round resume (resume1: create-indirect DirectVulkan >60 GiB
  RSS guard-killed; resume2: same case guard-killed again = the 1 failed, five cases
  deferred by the memory guard; resume3 on a fresh instance: 4 passed / 1 aborted).
  Non-passing first blockers: rd12 GLES `InitialBytesNotCarried/resource_respecify`, rd12
  Vulkan `BarrierTimeout/Present`, iris-photon GLES / iris-derivative GLES /
  create-indirect GLES `UnmigratedEmulation/texture-remint-pull`, iris-bsl-esc-menu GLES
  `InitialBytesNotCarried/resource_respecify`, create-indirect Vulkan memory-guard kill.
  Census block injected into `joint-codex-v1.md`; `identity.json`/`counts.json`/
  `trace-transitions.json` beside it.
- **Findings 1–3 closure**: fixes `a021e3cc` (finding 1) and `82683d4a` (findings 2–3) keep
  their package evidence (`indexspan-codex-v1.md` 9/9, `wait-codex-v1.md` 7/7). The two
  `PipeWireCodecTest.UserIndexSpan*` failures in the pre-`7cb29d46` quickgate were the
  test log-capture defect fixed by `7cb29d46`; re-evidenced green on `c77831e0`
  (3/3). Final device source: **`82683d4a83b5c8f4f7c375a7a92ebee9a1854646`** (APK phase
  `p5bcodex2`: pull/push/split signed APKs, provenance and proofs in
  `~/w7/notes/p5b/apk/p5bcodex2/`).
- **Redmi exit (device 2f7cbe2e, stage 256 MiB explicit profile)**: correctness matrix
  **8/8** (4 traces x 2 backends, inproc split arm, per-group pin evidence and inproc/
  apply-thread/wire-record proofs; `MOBILEGL_IPC_STAGE_MB=256`). Benchmark four-arm A/B:
  **24/32 groups green with validated 200-frame tails** (all groups of
  improved-transparency-26.3, 1.21.4-in-world, 1.21.4-sodium-in-world; first barrier-tax
  measurement, see `ab-tables.md`). The 8 `iris-bsl-in-world` groups failed validation on a
  fixture limit, not a regression: the trace yields only 123 benchmark frames, below the
  runner's 200-frame-tail rule, on every arm including pull. Their retained 123-frame
  series were reduced separately as a labeled supplement (p99 is shader-compile dominated
  on all arms, as already recorded in the P2 baseline). Note for future campaigns: the
  device no longer holds the vendor GPU ceiling observed 2026-09-11; the deterministic pin
  is now 1100 MHz (pin_device.sh updated 2026-09-16), so these numbers are not
  clock-comparable with a campaign pinned at 1050 MHz.
