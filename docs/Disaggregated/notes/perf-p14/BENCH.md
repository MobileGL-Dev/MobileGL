# P14 perf pass: final bench

**Headline:** FCL Minecraft gives Espryt monolith 198 vs dev 229 and inproc 242; Magma
monolith 171 vs dev 232 and inproc 195. On the rd12 trace, Espryt monolith is 91 vs 115 and
Magma monolith 84 vs 87; inproc is 112 (Espryt) / 110 (Magma). Details below.

Head `781a7446` on `feat/disaggregated` (trace APK `p14fin`) against `dev` at `08124c99` (the pull
monolith, the baseline). Lenovo Y700 (Snapdragon 8 Gen 3, Adreno 750), rooted, CPU and GPU clocks
pinned (`pin_clocks.sh`), no per-frame glFinish (`FINISH=false`), best of 3, steady frames only.
Raw runs: `.trace-work/perf-p14/final-p14/` (`matrix_p14.sh run`, `p14_report.py`).

## Final matrix (fps, best of 3)

| workload | backend | dev (pull) | monolith | inproc | shm | tcp |
|---|---|---|---|---|---|---|
| rd12 | Espryt | 115.2 | 91.1 | 111.9 | 87.8 | 68.7 |
| rd12 | Magma | 87.0 | 83.8 | 110.0 | 95.6 | 69.9 |
| openra | Espryt | 889.2 | 832.5 | 1073.9 | 1063.1 | 602.0 |
| openra | Magma | 293.1 | 747.4 | 1051.6 | 976.0 | 544.5 |

Start of the pass (same harness, `nofinish` tag, `f8a9466b`):

| workload | backend | monolith | inproc | shm | tcp |
|---|---|---|---|---|---|
| rd12 | Espryt | 69.8 | 71.1 | 62.8 | 50.0 |
| rd12 | Magma | 45.4 | 63.2 | 58.6 | 45.6 |
| openra | Espryt | 812.2 | 1067.5 | 953.8 | 529.2 |
| openra | Magma | 356.3 | 446.0 | 394.4 | 412.1 |

So over the pass, rd12 went from 69.8 to 91.1 fps on Espryt monolith (+31%) and from 45.4 to 83.8 on
Magma monolith (+85%). Split arms: inproc +57% / +74%, shm +40% / +63%, tcp +37% / +53%. On openra,
Magma monolith went 356 -> 747 and inproc 446 -> 1052.

## Against the three goals

| goal | rd12 | openra |
|---|---|---|
| push = pull (monolith vs dev) | Espryt 91.1 vs 115.2 (-21%); Magma 83.8 vs 87.0 (-4%) | Espryt 832 vs 889 (-6%); Magma 747 vs 293 (2.5x dev) |
| inproc beats pull | Espryt 111.9 vs 115.2 (-3%); Magma 110.0 vs 87.0 (+26%) | Espryt 1074 vs 889 (+21%); Magma 1052 vs 293 (3.6x) |
| backend parity (feat) | monolith 91.1 vs 83.8; inproc 111.9 vs 110.0 | monolith 832 vs 747; inproc 1074 vs 1052 |

## CPU- or GPU-bound

Every arm on both workloads is CPU-bound: GPU busy is 19-39 % everywhere (`GPU busy` column of the
report), and the arm's top thread runs at 94-99 % of a core on the monolith and inproc arms
(97-99 % for the render thread, 94-95 % for `mgl-srv-apply` where it is the top thread).
- monolith: one thread, `MobileGLTraceRe`, at 99 % on all four cells.
- inproc / shm: two threads at about the same load (client 8.5-8.7, apply 8.5-8.6 ms/frame on rd12),
  so the split arms are bound on whichever of the two is ahead; on rd12 Espryt that is the client
  (harness parse + client half), on rd12 Magma the apply thread.
- tcp: 70-80 % top-thread utilisation, the rest is the socket ping-pong (credit-1 waits), as in P10.
- `dev` Magma on openra is the one cell below 95 %: 63 % utilisation, wide spread (13.6 %) - dev's
  Magma waits on the GPU there, which the feat build no longer does (see "Magma upload wait").

Backend parity therefore means "both CPU-bound at the same CPU cost". On the feat build the two
backends are within 2-11 % of each other on every arm, and the remaining gap is CPU debt (rd12
monolith: Magma backend + driver 3.93 vs Espryt 2.76 ms/frame, client half 1.91 vs 1.78).

## Layer split, rd12 monolith (render thread, ms/frame, simpleperf at 2 kHz, steady tail)

| layer | Espryt feat | Espryt dev | delta | Magma feat | Magma dev | delta |
|---|---|---|---|---|---|---|
| driver | 1.69 | 1.36 | +0.33 | 2.12 | 1.88 | +0.24 |
| backend | 1.07 | 0.62 | +0.45 | 1.81 | 2.56 | -0.75 |
| applier | 0.07 | 0 | +0.07 | 0.13 | 0 | +0.13 |
| client half | 1.78 | 0.39 | +1.39 | 1.91 | 0.70 | +1.21 |
| harness (trace parse) | 6.94 | 6.70 | +0.24 (same code) | 6.79 | 6.86 | -0.07 |
| total CPU | 11.56 | 9.07 | +2.49 | 12.77 | 12.01 | +0.76 |

About 6.7-6.9 ms of every rd12 frame is the replay harness, so the MobileGL part of an Espryt frame
is 4.6 ms on feat against 2.4 ms on dev, and of a Magma frame 6.0 against 5.1.

### What the Espryt monolith gap is made of (as % of an 11.2 ms frame)

- Validate point, 0.92 ms (none of it exists in pull): EmitGlobalConstants 0.17 (1.5 %),
  EmitVertexElements 0.13 (1.1 %, includes the inline apply of the bind), Tracker::Update 0.12
  (1.1 %), validate self 0.10 (0.8 %), CopyField 0.10 (0.9 %), EmitVertexBuffers 0.07 (0.6 %),
  thread-local and atomic overhead about 0.07.
- Verb port, about 0.2 ms (1.8 %): EmitDrawRecord, ReadDrawBindings 0.06, MarkGpuWritesForDraw 0.05.
- Backend +0.45: SyncNeccessaryTextures 0.23 vs 0.10, SyncNeccessaryBuffers 0.16 vs 0.08,
  AdoptUnchangedBufferSets 0.09, BindCurrentProgramWithResources 0.18 vs 0.11,
  SyncCurrentProgramByHandle 0.09 vs 0.04.
- Driver +0.33 (2.9 %): inside glDrawElements itself, with the same GL call mix as dev. A
  `simpleperf stat` over the whole run (render thread) has feat executing 16.6 % more instructions
  than dev with 56 % more L1D and 23 % more L2D refills (IPC 3.73 vs 3.86): the record arm's larger
  per-draw working set (records, applier state, slot tables, twins) evicting the driver's state is
  the best reading, inferred from those counters, not measured inside the driver.

Each item is under 2 % of the frame except the driver term. Why the sum is structural on the record
arm: pull's backend reads frontend objects by pointer at draw time; push has to detect what moved
(tracker), serialise it (emitters, uniform spans), apply it (applier), then let the backend resolve
it again by handle through slot tables, with per-bound-object clean checks because the backend no
longer sees the frontend's dirty bits. Each step is tens of ns per draw on cache lines no other
step shares, times 1471 draws a frame.

Options left (none below the 2 % bar on its own, or each with a correctness risk the bar does not
pay for):
- a frontend mutation epoch (bumped in every `DECLARE_GL_FUNCTION_END` entry except draws,
  glBindVertexArray and float glUniform*) so a validate with only VAO and uniform movement skips
  the context-global shutters (at most 0.07 ms) and, after an audit of which residual fields
  depend on the VAO/program bindings, the residual copies (0.15-0.25 ms together, 1.5-2 %);
- a per-(VAO, program) draw packet that keeps tracker, emitter, applier and backend state for a
  draw on a few cache lines, keyed on lifetime ids - the structural answer to the cache-pressure
  term, a redesign rather than a pass item;
- native TLS (minSdk / ANDROID_PLATFORM 29 instead of 26, the user's call): measured +1.7 %
  Espryt, within noise on Magma (rd12 monolith, best of 3; whole APK, harness included).

## Pass findings worth keeping

- **Magma upload wait (553eb86d).** Every texture level upload flushed the recording and then
  waited on the CPU for every submission to complete (`FlushWirePendingCommandsForTextureUpdate`,
  added in P7 B4). The upload batch is submitted on the same queue after the flush and its first
  barrier takes its source scope from the image's tracked layout, so the wait bought nothing; on
  rd12 it was 1.8 ms/frame of fence stall (Minecraft updates its lightmap every frame). Removing it:
  rd12 Magma monolith 70.4 -> 83.4, openra 367 -> 745. The preserve path keeps its wait. Proof: a
  new between-draws upload scenario under the synchronization validator (red on a broken barrier),
  sync validation of the texture/remint/copy scenarios (no new hazards), SSIM 16/16.
- **Sync validation needs submit-time validation on.** The Khronos layer (1.4.350) does not report
  a hazard between two queue submissions unless `VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION`
  is set; the run-ahead and cache gates now set it (781a7446), and the run-ahead gate goes red on
  the broken-barrier build where it used to stay green.
- **GPU-fault latch regression (549a34bb -> fixed in c24b3428).** Polling the reset status every
  256 records let a record published right after the previous acknowledgement run unpolled on a
  guilty context. The drain now polls before any record the last poll could not have seen
  (e058a450 hands the apply loop's poll to the drain so a woken drain does not poll twice).
- **Record-publish batching, measured and dropped.** Publishing an unbarriered validate's state
  records with the draw record behind them was within +-2 % on every arm and opens a deadlock class
  (a staging-retirement wait on a record that is written but not published - it hung rd12 Espryt
  split until the stage wait published owed records). Not worth the surface; the patch and its
  stress scenario are not in the tree.

## W9 (cache capacities), closed with readings

`MOBILEGL_PIPE_STATS` on rd12 and openra: the render-state CSO cache mints 0 per frame in the steady
state, and Magma's pipeline memo hits 99.0 % on rd12 and 92.7 % on openra. No cache thrashes; the
P8 capacities stand.

## FCL (Minecraft in world, the representative app check)

FCL's own build of the library (plugin flavor, no POISON), swapped into FCL. Readings come from
MobileGL's fps log: mean of two launches × three steady windows. Clocks pinned. Runs:
`.trace-work/perf-p14/fcl-final/`.

| backend | dev (pull) | feat monolith | feat inproc | presenting-thread CPU, monolith feat / dev |
|---|---|---|---|---|
| Espryt | 228.6 | 198.3 (-13 %) | 241.5 (+6 %) | 2.43 / 1.85 ms/frame |
| Magma | 231.8 | 171.4 (-26 %) | 194.8 (-16 %) | 2.80 / 1.64 ms/frame |

FCL Magma does not match the trace: the presenting thread is busy only about 2.8 ms of a 5.8 ms
frame, so about 3 ms is a wait the trace does not have. That is the next phase's first item: an
off-CPU profile, the method that found the trace's upload stall.
