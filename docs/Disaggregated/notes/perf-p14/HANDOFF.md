# P14 perf pass: handoff (checkpoint 2026-10-07)

Worktree `.claude/worktrees/perf` (branch `perf`, pushed to `origin/feat/disaggregated`). Estimate:
[ESTIMATE.md](ESTIMATE.md). BENCH.md / README.md are not written yet (see "Next").

## Head and running totals

Head: `94705adc` (plus this handoff commit). rd12, Lenovo Y700 (Adreno 750), clocks pinned, no
per-frame glFinish (`FINISH=false`, the primary mode; finish=true serialises the split arms on one
FenceWait round trip per frame), best of 3:

| arm | Espryt start -> now | Magma start -> now | dev (pull) Espryt / Magma |
|---|---|---|---|
| monolith | 69.8 -> 88.6 | 45.4 -> 69.5 | 115.1 / 86.1 |
| inproc | 71.1 -> ~107 | 63.2 -> ~99 | |
| shm | 62.8 -> ~82 | 58.6 -> ~85 | |
| tcp | 50.0 -> ~64 | 45.6 -> ~61 | |

(split-arm "now" values are from the p14p A/B, before the last two monolith commits; re-measure.)
GPU busy 22-33 % everywhere: every arm is CPU-bound on one thread.

## Latest layer split (rd12 monolith, render thread ms/frame, simpleperf, head `94705adc` = p14r)

| layer | Espryt feat | Espryt dev | Magma feat | Magma dev |
|---|---|---|---|---|
| driver | 1.56 | 1.40 | 2.15 | 1.98 |
| backend | 1.21 | 0.59 | 1.95 | 2.12 |
| applier | 0.07 | 0 | 0.13 | 0 |
| client half | 1.96 | 0.36 | 2.01 | 0.69 |
| harness | 6.46 | 6.27 | 6.50 | 6.25 |
| total CPU | 11.27 | 8.62 | 12.77 | 11.05 |

Client half top: validate self 0.22-0.26, verb port (MG_Record) 0.15-0.18, Tracker::Update 0.14-0.17,
CopyField 0.07-0.09, VAO memo probe 0.07-0.09. Espryt backend top: PrepareForDraw 0.10,
AdoptUnchangedBufferSets 0.10, PerNativeContext<...>::Current 0.084, SyncTextureToBackendByHandle
0.066, IsBufferDrawCleanByHandle 0.058. Magma backend top: SetupWireDraw self 0.35,
BindDescriptorSetDeduped 0.23 (dynamic UBO offset moves every chunk draw), AcquireWireSlice 0.19.

Per-draw counts (instrumented build, `p14count2.patch` in the scratchpad, not committed): rd12 1471
draws/frame, 3.3 GL calls/draw; after the protocol change 66.8 B/validate (bind 16 B + uniform span
~47 B), VertexBuffers/IndexBuffer 100 % no-op.

## Commits this pass (newest first)

94705adc inline applier / fill plan / word-wise spans / memo-answered draw bindings (+6 % Espryt, +3.7 % Magma);
81bdfb1f Espryt twin lookups w/o SharedPtr copy + one TLS block (+3.8 %);
032dcd24 Magma vertex-input layout kept on its record + slot-indexed buffer front;
c1497bb8 VAO-kept bindings, uniform spans, context-values gate (split arms +5-10 %; stream span merge fixes a tcp regression);
0158a115 Magma pass-continuation memo; 8883e496 fill pins + VAO hash memo; 80dc9d45 VAO memo;
5788a602 Magma descriptor rebind; afcf8d96 Espryt AdoptUnchangedBufferSets; e6281b26, 4ef9705e Magma
lookups/sampler memo; b556ec31, a9108e1a Espryt; 549a34bb server fault latch; 8346d809 slot allocator
front; 267211e8 Magma hash memo + scratch.

## In flight / next

1. Espryt backend lookups: `PerNativeContext<T>::Current` (TLS cache per T + non-inline serial/epoch
   calls), `IsBufferDrawCleanByHandle`, `AdoptUnchangedBufferSets` (compares the window every draw
   because the bind re-apply bumps VertexBuffersSerial), `SyncTextureToBackendByHandle`.
2. Tracker::Update: 18 shutters evaluated per draw, 4 fire. Needs a cheap "nothing but the VAO and
   the uniform moved" signal; no central frontend mutation epoch exists yet.
3. Residual fill: now a precomputed walk, but still ~11 CopyField per draw.
4. Magma: SetupWireDraw self, per-draw dynamic-offset descriptor bind.
5. Final full matrix (dev + 4 arms x 2 backends x openra/rd12, REPS=3), then BENCH.md, README.md,
   DEBTS.md rows (Magma 16 %, W9: closed with readings - render-state CSO cache mints 0/frame, Magma
   pipeline memo 99.0 % hit on rd12, 92.7 % on openra), FCL re-run on the final lib.

## Harness

Scratchpad scripts (session-local): `build_trace.sh W:/perf .p14X` (trace APK, suffix = package
`top.mobilegl.plugin.p14X.trace`), `sign_install.sh <apk> <name>`. W: is `subst` of
`.claude/worktrees` (MAX_PATH). Then:

```
cd tools/device_bench/disagg
FIXTURES=<main checkout>/tools/trace_replay/fixtures PREP_PKGS=top.mobilegl.plugin.p14X.trace bash matrix_p14.sh prep
ARMS="monolith@p14a monolith@p14b" BACKENDS="DirectGLES DirectVulkan" WLS=rd12 REPS=3 TAG=ab bash matrix_p14.sh run
python p14_report.py ../../../.trace-work/perf-p14/ab
ARMS="monolith@p14X inproc@p14X shm@p14X tcp@p14X" REPS=1 TAG=ssim bash matrix_p14.sh ssim   # SSIM gate
ARM=monolith BACKEND=DirectGLES WL=rd12 TAG=prof PKG_FEAT=... SYMDIR=<unstripped libs> bash profile_p14.sh
python layers.py <perf.data> <binary_cache> MobileGLTraceRe <cpu ms/frame> --tail-ms=N [--detail=backend]
```

FIXTURES must point at a checkout with real LFS goldens (worktrees have pointer files). Host tests:
WSL archlinux `~/mgl-perf-bld` (clang, split+inproc, tests on); ServerSpawnTest / FuzzArm2 /
PeerLatchTest fail there with Refuse{BuildFingerprint} (no build stamp in that tree), environmental.

## Device state

HA27Q3LQ: CPU/GPU clocks PINNED (`bash pin_clocks.sh restore` to undo). FCL lib/config/props restored
(md5 checked; `debug.mobilegl.*` props unset). Trace APKs `top.mobilegl.plugin.p14*.trace` installed.

## CI

No run has completed since f8a9466b (each push cancelled the previous). workflow_dispatch runs on
the checkpoint head: see the milestone message / below.
