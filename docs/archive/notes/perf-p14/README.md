# P14: the post-P13 performance pass

P14 measured the record (push) arm against the `dev` pull monolith and cut its CPU cost. It ran on
`feat/disaggregated` from `f8a9466b` (start) to `781a7446` (close-out), on a Lenovo Y700 (Adreno
750) with pinned clocks.

- [ESTIMATE.md](ESTIMATE.md): the code-based estimate, written before the first benchmark.
- [BENCH.md](BENCH.md): the final trace matrix, the layer split, CPU vs GPU binding, and what the
  remaining gap is made of.
- [HANDOFF.md](HANDOFF.md): harness commands, device state, open items.
- [NEXT-draw-packet-cache.md](NEXT-draw-packet-cache.md): the design sketch for the next phase.

## Headline numbers

### Minecraft in FCL (the representative app check)

Setup:
- FCL Minecraft, in world.
- The library is swapped into FCL, which runs it as a plugin build without `MOBILEGL_PIPE_POISON`.
- Readings come from MobileGL's own fps log.
- Two launches × three steady windows of about 2 s each. Dev Espryt is three windows: its second
  launch died at start-up, twice.
- Clocks are pinned. Runs: `.trace-work/perf-p14/fcl-final/`.

| backend | dev (pull) | feat monolith | feat inproc | feat monolith, presenting-thread CPU |
|---|---|---|---|---|
| Espryt | 228.6 | 198.3 (−13 %) | 241.5 (+6 %) | 2.43 ms/frame (dev 1.85) |
| Magma | 231.8 | 171.4 (−26 %) | 194.8 (−16 %) | 2.80 ms/frame (dev 1.64) |

Earlier in the pass, at `94705adc`, the same measurement read:
- Espryt: monolith about 192, inproc about 233;
- Magma: monolith about 169, inproc about 179;
- dev: 209-245.

FCL Magma monolith did not follow the trace's improvement. Its presenting thread is busy only
about 2.8 of the roughly 5.8 ms frame, so the rest is a wait. That is the first item for the next
phase: an off-CPU profile of FCL Magma monolith, the same method that found the upload stall in
the trace. Two attempts at it during close-out caught no game frames (FCL start-up).

### Trace replay, rd12 (Minecraft 1.21.4 RD12, 1471 draws/frame), best of 3, fps

| backend | dev (pull) | monolith | inproc | shm | tcp |
|---|---|---|---|---|---|
| Espryt | 115.2 | 91.1 (start 69.8) | 111.9 (71.1) | 87.8 (62.8) | 68.7 (50.0) |
| Magma | 87.0 | 83.8 (start 45.4) | 110.0 (63.2) | 95.6 (58.6) | 69.9 (45.6) |

openra is in [BENCH.md](BENCH.md). On openra, Magma monolith now runs 2.5x faster than dev
(747 vs 293), and inproc 3.6x (1052 vs 293).

Every arm is CPU-bound: GPU busy is 19-39 %, and the top thread sits at 94-99 % of a core
(70-80 % on tcp).

## What landed

The commits run from `f8a9466b` to `781a7446`. Per-commit numbers are in the commit subjects.

- **Magma wire draw:**
  - no per-draw SPIR-V rehash or allocation;
  - unit sampler memo; unchanged pipeline, vertex and index binds skipped;
  - direct-mapped texture and buffer fronts;
  - descriptor-set rebind with fresh offsets;
  - pass-continuation memo;
  - vertex-input layout kept on its record;
  - **texture level uploads no longer wait for the GPU to drain** (`553eb86d`, the largest
    single gain of the pass: rd12 70 -> 83, openra 367 -> 745);
  - slices stamped from the owner's submit counter.
- **Espryt:**
  - VAO twin re-points only the moved buffers;
  - clean attachments skip resync;
  - unchanged buffer sets are adopted;
  - twin lookups without SharedPtr copies;
  - one thread-local block;
  - one UBO-ring lookup per draw.
- **MG_Pipe client half:**
  - lifetime-id slot fronts;
  - per-VAO memo;
  - fill pins;
  - kept VAO bindings on the server;
  - uniform spans;
  - context-values gate;
  - inline process applier;
  - precomputed residual walk, with split copy and stamp loops;
  - memo-answered draw bindings.
- **MG_Remote:** fault-verdict polling restructured.
  - It first went from every record to every 256 records (549a34bb).
  - The drain now polls before any record the last poll could not have seen (`c24b3428`). This
    fixes the regression where a guilty context ran one more record.
  - The apply loop hands its poll to the drain (`e058a450`).
- **Magma hang watch:** a marker slot is reused only once its submission's fence completes
  (`ec973fba`).
- **Tests and CI:**
  - TextureUploadBetweenDrawsScenario on every arm, plus inproc sync validation (`67f4f311`,
    `86ab2835`);
  - submit-time synchronization validation in the run-ahead and cache gates (`781a7446`);
  - ServerLoopEglLatchTest now holds the race window open.
- **Tools:**
  - pinned-clock matrix with a per-thread CPU and GPU-busy sampler;
  - layer-split and off-CPU profile helpers;
  - FCL harness;
  - opt-in fps log and transport override properties.

## Measured and not landed

- **Record-publish batching:** publishing a validate's state records with the draw record. It came
  in within ±2 % on every arm, and it opens a deadlock class (a staging-retirement wait on a
  written but unpublished record).
- **Native TLS (minSdk / ANDROID_PLATFORM 29):** +1.7 % on Espryt, noise on Magma (rd12 monolith).
  This is the user's call.
- **Tracker / residual mutation-epoch fast path:** estimated at most 0.07 ms from the tracker alone,
  0.15-0.25 ms with the residual copies. Each step is below the 2 % bar, and a missed epoch bump
  renders stale. It is folded into the draw-packet design.
