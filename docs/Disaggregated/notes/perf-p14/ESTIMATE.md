# P14 perf pass: estimate from the code (written before the benchmark)

Date: 2026-10-07. Code: `feat/disaggregated` at `f8a9466b` ("feat") against `dev` at `08124c99` ("dev",
the pull monolith, the "before" baseline per DEBTS.md). Device for the measurement that follows:
Lenovo Y700 (TB321FU, Snapdragon 8 Gen 3, Adreno 750), rooted, clocks pinned.

**Disclosure.** Before writing this I ran one unpinned pipeline-validation smoke of the new harness
(Magma only, one rep, dev / feat monolith / feat inproc): openra 247 / 178 / 146 fps, rd12
127 / 46 / 33 fps. Those numbers are bigger than the logged 16 % (DEBTS ID-P13-2) and I had seen
them when I wrote the Magma rows below, so the Magma magnitudes are not blind. The mechanism
attribution (which costs, on which thread) is from the code and is what the profile will test;
the Espryt, spawn and tcp rows were written before any run of those arms.

## 1. What each variant does per call and per draw

| | dev pull monolith | feat monolith (record arm) | feat inproc | feat spawn (shm) | feat tcp (localhost) |
|---|---|---|---|---|---|
| GL entry | frontend state only | + `GLStreamScope` (one call + branch) | + process-wide recursive mutex per GL call | same as inproc | same as inproc |
| state calls | frontend state | + mutation notes (`MGP_NOTE_*`), dirty bits | same as monolith | | |
| per draw, client side | none | `MGPipeValidateForVerb`: tracker update (18 shutters), up to 14 emitters (CSO cache probe 64-entry, sampler views/states walks with hash-map slot lookups + XXH64), **`EmitContextValues` + XXH64 every draw**, residual fill of up to 63 fields incl. 3 SharedPtr copies (atomics); verb port: `ReadDrawBindings` hash lookups, persistent-map tracker, `MarkGpuWritesForDraw` | same, plus each record memcpy'd into the SEG_CMD ring and blobs (UBO images, sub-data) into SEG_STAGE | same as inproc | + `sendmsg` flush at every wait / present / fence poll |
| per draw, server side | backend reads `pGLContext` | applier (`MGPipeApply*`, same thread) + backend reads applier records; one extra copy of each changed default-uniform block into the applier | decode + bounds checks + apply + backend on the apply thread; staged bytes copied into server storage | same as inproc, in a second process | + io thread `recv` into mirrors |
| Magma draw setup | `TrySetupDrawFastPath` (4 snapshots keyed on program lifetime id; skips render-pass, descriptor, texture-sync and pipeline work when nothing moved) | `SetupWireDraw` every draw: `PrepareWireTextureResources` (texture sync per sampled binding), attachments resolved twice, `BuildWireVertexInput` rebuilt (Vector pushes + 4 XXH64), descriptor writes rebuilt + FNV over every info + 4-entry reuse memo, ~10 heap Vectors for the render-pass key, `UnorderedMap` intern of the pass-compatibility key, pipeline memo probe, unconditional `vkCmdBindPipeline` | same backend work on the apply thread | | |
| Espryt draw setup | memos keyed on frontend pointers / versions | the same memos re-keyed on applier serials (texture sync list, unit-bindings epoch, FBO lists, resolved-buffers memo, cached `glBindBufferBase/Range`); per-unit record lookups in `ResolveAndBindUnitTextures` | same on the apply thread | | |
| threads that burn CPU | 1 | 1 | 2 (client + apply), run-ahead armed | 2 processes | 2 processes + io threads |
| sync points | none | none | `kWaitReply` / `kWaitApplied` rows (fence waits, queries, readbacks, mipmaps, copy-tex, make-current), present credit 1 | same + socket doorbell when parked | same + credit-1 ping-pong over the socket (P10: 11.2 of 19.4 ms/frame parked) |

The trace APK's library is the split shape (`MOBILEGL_BUILD_DISAGGREGATED=ON`), which also turns on
`MOBILEGL_PIPE_POISON` (a stamp per filled field and a freshness check per backend accessor read).
FCL's library is the push shape without it. So the trace APK's monolith arm carries a little more
than the FCL library does.

## 2. Predictions

Workloads: openra (light: 128 frames, ~28 draws and ~199 records per frame) and rd12 (Minecraft
1.21.4 RD12: 251 frames, ~7,500 records per frame, several hundred draws per frame). Both are light
on the GPU, so every arm should be **CPU-bound**: GPU busy well under 60 %, with one thread at or
near 100 % for monolith and dev.

Per-draw costs I expect (Y700 big core at ~2 GHz):

- feat validate point + verb port over dev: **~2-4 us per draw** (an XXH64 of the context values, a
  few hash-map lookups, up to 63 field copies, the persistent-map and GPU-write walks).
- Magma `SetupWireDraw` over dev's fast path: **~8-15 us per draw** (about 10 heap allocations, a
  texture sync per sampled texture, two attachment resolves, the vertex-input rebuild and the
  descriptor rebuild). This dwarfs everything else in this table.

| workload / backend | dev | feat monolith | feat inproc | feat shm | feat tcp |
|---|---|---|---|---|---|
| rd12 Espryt | baseline, 1 thread saturated | **10-25 % slower** (validate point + verb port on a ~7-9 ms frame) | **0-20 % faster than feat monolith**, roughly at dev: client and apply threads overlap, total CPU +20-40 % | about inproc (P5/P8 history: shm about inproc) | **3-6x slower than shm**: credit-1 ping-pong and wake-ups, not bandwidth |
| rd12 Magma | baseline; dev Magma at or ahead of dev Espryt (fast path + earlier walk-skip work) | **1.6-2.5x the dev frame time**: SetupWireDraw dominates | apply-thread bound (the same SetupWireDraw), so **at most 10-25 % better than feat monolith and still far behind dev** | about inproc | as Espryt tcp, worse in absolute terms |
| openra Espryt | baseline | within 5 % (few draws; the fixed per-frame cost dominates) | **5-15 % slower** than monolith (handshake, no work to overlap) | 10-15 % slower than monolith | 20-30 % slower than monolith |
| openra Magma | baseline | 15-35 % slower (per-draw gap times only ~28 draws, plus per-frame wire costs) | slower than monolith, as Espryt | | |

## 3. What this says about the three beliefs (to be checked)

1. **"inproc is faster than the pull monolith."** Partly. Only on draw-heavy frames, only where the
   apply thread's backend is cheap enough for the client half to be the critical path (Espryt),
   and the gain is overlap on a second core, not less work: total CPU goes up. Light loads show
   no gain and pay the handshake. Magma inproc stays behind dev until its draw setup is fixed.
2. **"push and pull are on par."** Wrong on Magma (the lost fast path) and wrong by a smaller margin
   on draw-heavy Espryt (the validate point and verb port are per-draw costs pull never paid).
   Close on light loads.
3. **"the two backends are on par."** On dev, roughly yes (Magma ahead on Minecraft-like frames).
   On feat, no: Magma pays its draw setup in full. Both backends are CPU-bound on these scenes,
   so parity means cutting CPU work until both feed the GPU.

## 4. Fix plan the estimate implies, in order

1. A Magma wire draw fast path: snapshot the resolved draw (program lifetime id, applier serials
   for bound views, samplers, FBO and vertex elements, render-pass key, pipeline, descriptor
   signature) and skip the re-resolution while nothing moved. Keys must be monotonic lifetime ids
   or applier generations, never handles or pointers. Also cheaper on a miss: no per-draw heap
   Vectors, one attachment resolve, a cached compatibility id, `vkCmdBindPipeline` deduplicated.
   Expected: most of the Magma gap on rd12.
2. W9: read `MOBILEGL_PIPE_STATS` on both workloads (CSO mints vs binds, pipeline memo misses,
   descriptor reuse misses) and resize where a cache thrashes. Expected: small (<5 %) unless a
   cache is thrashing.
3. Then whatever simpleperf shows on the client half (validate point, `EmitContextValues`,
   residual fill) and on the split arms (ring encode, mutex, waits).
