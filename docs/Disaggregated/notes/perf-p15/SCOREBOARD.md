# P15 scoreboard: FCL MC 1.21.5 vanilla at cpuhunt

One row per bench session. MobileGlues (plugin `com.fcl.plugin.mobileglues`) is the only control
measured in every session. Results are reported as fps ratio to MobileGlues. "vs dev" is derived
through the fixed dev reference below: `vs dev = (arm / MobileGlues) / (dev / MobileGlues)`.

Targets (PLAN-P15 §0.1):
- Monolith at or above dev, then on par with MobileGlues (ratio 1.00).
- Inproc substantially above MobileGlues.

## Dev reference (fixed)

Re-measure only when the scene, clock profile, MC version, FCL build or harness changes.

Measured in session safter (2026-10-08 ~22:00) on harness H2 at cpuhunt (CPU 1.50/1.25 GHz, GPU 903 MHz). All
15 runs were VALID, with no throttling (tpl=0 thr=0) and peak CPU 63 C. dev = 08124c99 (perf-dev worktree, `libs/devsym`). The table gives
3 interleaved reps per arm; CPU is the GL thread's schedstat time.

| arm | fps (r1 / r2 / r3) | mean fps | ratio to MG | CPU ms/frame |
|---|---|---|---|---|
| MobileGlues | 318.8 / 325.1 / 320.5 | 321.5 | 1.000 | 1.70 |
| dev Espryt monolith | 297.1 / 277.3 / 281.0 | 285.1 | **0.887** | 2.38 |
| dev Magma monolith | 313.8 / 294.5 / 308.4 | 305.5 | **0.950** | 2.01 |

- **vs dev for a session row:** Espryt = ratio / 0.887, Magma = ratio / 0.950.
- **CPU reference:** dev Espryt 2.38 ms/frame (1.40 of MG), dev Magma 2.01 ms/frame (1.18 of MG).
- **Dev reps spread more than the other arms** (6-7 % vs 1-3 %): dev's warmup needed 2-5 windows to settle where p15s and MobileGlues needed 2. The mean of the three reps is the reference.
- **Function-level baseline:** one symbolized simpleperf profile per dev backend, taken after the measurement windows in the same runs: `runs/safter/dev-Direct{Vulkan,GLES}-r{1,2,3}/perf.data`. See [MONOLITH-DIFF.md](MONOLITH-DIFF.md).

## Harnesses

- **H1 (old probe, through fifi):**
  - Not pinned: the scheduler gave MobileGL's monolith render thread the X4 core 72-90 % of the
    time and MobileGlues' 47-60 %. MobileGlues' fps tracked its X4 share (fifi: 60 % -> 378, 47 % ->
    344).
  - fps sources differ by arm: MobileGL arms used the MobileGL fps log before tracing; MobileGlues
    used the ftrace window.
  - Unfrozen world: daylight cycle on, and time advanced between runs through autosaves.
  - 2 reps per arm.
- **H2 (`fcl_bench.sh`, from safter):**
  - FCL is pinned to cpu2-6 (the A720 cluster) after world entry.
  - fps for every arm comes from the SurfaceView BLAST layer's frame number in SurfaceFlinger.
  - The same warmup for every arm: 20 s, then 5 s windows until two consecutive ones agree within
    3 %.
  - Measurement: two 5 s windows with no tracer running. Per-thread CPU comes from schedstat over
    those windows.
  - Frozen save: no daylight, weather or mob-spawn cycle, random ticks off, DayTime 6000.
  - 3 interleaved reps per arm.
  - H2 absolute fps is lower than H1 because nothing runs on X4. Compare H2 rows only with H2
    rows.

## Sessions

Each cell is the fps ratio to MobileGlues, with presenting-thread CPU in ms/frame in brackets.
- Monolith: the CPU is the GL thread.
- Inproc: the CPU is the apply thread (it presents). The H1 rows use the MobileGL fps log's
  "presenting thread CPU".
- MobileGlues (MG) cells give fps and CPU.
- A dash means the arm was not in that session.

| session (2026-10-08) | harness | build | MG fps (cpu) | mono Espryt | mono Magma | inproc Espryt | inproc Magma |
|---|---|---|---|---|---|---|---|
| hunt2 14:20 | H1 | p15l ≈ c8a5abe5 | 335.0 (1.58) | 0.80 (2.19)¹ | 0.79 (2.18)¹ | 0.87 (1.63) | 0.92 (1.60) |
| apab 14:51 | H1 | p15m ≈ 500b63ea | 328.6 (1.55) | 0.86 (2.06) | 0.85 (1.96) | 0.90 (1.65) | 0.91 (1.58) |
| blab 16:17 | H1 | p15n/p15o = c07b8c40 | 354.9 (1.46) | 0.84 (2.02)² | - | 0.95 (1.58) | 0.94 (1.53) |
| nxab 17:04 | H1 | p15p = 61b5eb5a | 375.5 (1.43) | 0.81 (1.95) | 0.84 (1.77) | 0.93 (1.54)³ | 0.89 (1.54) / 0.93³ |
| cens 18:09 | H1 | p15r = e00db8e6 | 321.9 (1.57) | - | - | 0.93 (1.50) / 0.99³ | 0.96 (1.46) / 0.99³ |
| fifm 20:07 | H1 | p15s = 0b96a3ef | 361.4 (1.48) | 0.82 (1.98) | 0.84 (1.84) | - | - |
| fifi 20:47 | H1 | p15s = 0b96a3ef | 361.1 (1.47) | - | - | 0.96 (1.37) | 0.94 (1.39) |

H2 sessions:
- Cells are the fps ratio to MG, then vs dev, then GL-thread CPU in ms/frame.
- Inproc cells add the apply-thread CPU.

| session (2026-10-08) | build | MG fps (cpu) | mono Espryt | mono Magma | inproc Espryt | inproc Magma |
|---|---|---|---|---|---|---|
| safter ~22:00 | p15s = 0b96a3ef | 321.5 (1.70) | 0.746 / 0.84 dev (3.18) | 0.768 / 0.81 dev (2.91) | - | - |
| pt ~23:00 | p15t = 3def5cb2 (p15s + Magma blit cache); inproc Magma also p15s | 319.0 (1.69) | 0.755 / 0.85 dev (3.17) | 0.787 / 0.83 dev (2.83) | - | p15s 1.019 (1.36 / 1.42); p15t 1.032 (1.37 / 1.39) |
| c1 ~00:30 (10-09) | t = p15t; u = p15t + cut 1 (in-frame uploads, local) | 324.8 (1.69) | - | t 0.782 / 0.82 dev (2.80); u 0.769 / 0.81 dev (2.86) | u 0.997 (1.38 / 1.63) | u 1.011 (1.37 / 1.39) |
| c1b ~02:10 (10-09) | t = p15t; v = p15t + 1b (coalesced pass barriers, local) | 320.9 (1.69) | - | t 0.783 / 0.82 dev (2.85); v 0.789 / 0.83 dev (2.85) | - | v 1.048 (1.34 / 1.345) |
| c4 ~02:55 (10-09) | v (as c1b); w = v + cut 4 (Espryt surface-size memo, 66e3b2c1) | 321.6 (1.70) | v 0.757 / 0.85 dev (3.18); w 0.762 / 0.86 dev (3.05) | - | - | - |
| c1c2 ~03:30 (10-09) | t = p15t; y = p15t + 1b + 1c (deferred clears) + cut 4 | 321.3 (1.70) | - | t 0.795 / 0.83 dev (2.83); y 0.785 / 0.82 dev (2.74)⁴ | - | t 1.025 (1.35 / 1.41)⁵; y 1.076 (1.33 / 1.24) |
| fbm ~05:20 (10-09) | y (as c1c2); z = y + FB-state build memo, one entry per target | 321.7 (1.70) | y 0.770 / 0.87 dev (3.04); z 0.775 / 0.87 dev (3.04) | y 0.818 / 0.86 dev (2.65); z 0.831 / 0.87 dev (2.65) | - | - |
| fbm2 ~11:00 (10-09) | y; z (as fbm); h = y + batch h (485dff14 + c85c9439) | 319.7 (1.68) | y 0.763 / 0.86 dev (3.03); h 0.768 / 0.87 dev (3.00) | y 0.805 / 0.85 dev (2.68); z 0.798 / 0.84 dev (2.69); h 0.812 / 0.85 dev (2.62) | - | - |

- **pt:** the blit view cache gives Magma inproc +1.2 % fps, with apply −0.03 ms/frame.
- **c1:** cut 1 is within noise (−1.7 % fps, +0.06 ms). It engaged (the mid-frame submit is gone) but saves ≤0.04 ms; see MONOLITH-DIFF.md.
- **c1b:** neutral in monolith on CPU and on GPU (busy 29 % in both). Barriers fell from 27.5 to 15.5 per frame, but passes only from 12.6 to 11.5.
- **c1c2:** Magma per frame, from the counting build:
  - passes 11.5 -> 4.3 (dev 4.1);
  - barriers 15.5 -> 9.0 (dev 3.4);
  - Vulkan calls 343.5 (dev 331.6).

  Effect:
  - Monolith GL-thread CPU −0.09 ms/frame (2.83 -> 2.74); fps within noise.
  - Inproc apply thread −0.17 ms/frame (1.41 -> 1.24) and fps +5.0 % (329.5 -> 345.7).
  - GPU busy: monolith 30 -> 27 % (≈1.18 -> 1.07 GPU ms/frame); inproc 35 -> 33 % (≈1.06 -> 0.96).
  - Profile: the `glClear` entry fell 0.286 -> 0.098 ms/frame. Part of that pass begin moved into draws (DrawArrays +0.07).

  ⁴ y has 2 valid reps: r2 never reached the world after 3 startup deaths (signal 34 during bootstrap, a harness flake also seen on MobileGlues and other builds).
  ⁵ t inproc r2 is discarded: a host-side `binary_cache_builder` pulled libraries over adb inside its measurement window.
- **fbm2:** batch h moves GL CPU by -0.06 ms (Magma) and -0.03 ms (Espryt).
  - Batch h: per-framebuffer FB-state memo, handle hints, per-buffer handle reuse, the cached per-verb
    gate, and the Magma hash TLS hoist.
  - The Magma before/after profile (render thread, ms/frame, c1c2 y-r1 -> fbm2 h-r1):
    - MGPipeValidateForVerb 0.328 -> 0.282, BuildFramebufferState 0.058 -> 0.023;
    - FindByLifetimeId + Acquire 0.047 -> 0.003, __emutls_get_address 0.079 -> 0.054;
    - GetOrCreatePipeline 0.026 -> 0.013, MGPipeGetResourceOps 0.007 -> 0.002.
  - The H2 effect is about half of the profile sum. Each piece is below the session noise (+-0.05 ms),
    so the batch is measured as one.
- **c1cbsl (gpuhunt, Iris + BSL, after a cooldown):** 1c does not cost GPU time.
  - t 53.8 fps (18.5 GPU ms/frame) vs y 55.7 (17.9), both at 100 % GPU busy.
  - One VALID pair. The MobileGlues run (56.2) is INVALID: the GPU idled at 231 MHz in two warmup samples.
- **fbm:** the first memo is within noise on both backends (CPU 2.65 / 2.65 Magma, 3.04 / 3.04 Espryt).
  - It engaged only partly: Magma BuildFramebufferState 0.058 -> 0.032 ms/frame, and Espryt kept missing.
  - The build only runs on a binding switch, so one entry per target misses on every A/B/A switch.
    Batch h makes it per framebuffer.
  - Session noise: y Magma r3 read 2.54 against 2.70-2.72 in r1/r2, with nothing in its state to explain it.
- **c4:** Espryt GL-thread CPU fell 0.13 ms/frame (3.18 -> 3.05 in all three reps); fps moved by less than the noise.
  The profile confirms it: QueryCurrentSurfaceSize 0.070 -> 0.012 and SyncRenderState 0.092 -> 0.028 ms/frame.

**Thread state, Perfetto SQL** (c1, H2; `pftstate.py` over `trace_processor_shell` v58.2 on the
`.pftrace` taken after the windows; means of 3 reps; ms/frame):

| arm | fps | GL run | GL woken-wait | GL preempted | GL sleep | apply run | apply sleep | both on-CPU / neither |
|---|---|---|---|---|---|---|---|---|
| MobileGlues monolith | 324.8 | 1.91 | 0.036 | 0.030 | 1.05 | - | - | - |
| Magma monolith (t) | 254.1 | 3.09 | 0.042 | 0.027 | 0.71 | - | - | - |
| Magma monolith (u) | 249.8 | 3.05 | 0.045 | 0.045 | 0.79 | - | - | - |
| Magma inproc (u) | 328.3 | 1.43 | 0.047 | 0.073 | 1.45 | 1.61 | 1.31 | 41 % / 41 % |
| Espryt inproc (u) | 323.8 | 1.39 | 0.057 | 0.088 | 1.51 | 1.95 | 1.02 | 42 % / 34 % |
| c1c2: MobileGlues monolith | 321.3 | 1.95 | 0.039 | 0.019 | 1.05 | - | - | - |
| c1c2: Magma monolith (t) | 255.3 | 3.11 | 0.039 | 0.038 | 0.66 | - | - | - |
| c1c2: Magma monolith (y, 1c) | 252.3 | 3.07 | 0.047 | 0.029 | 0.75 | - | - | - |
| c1c2: Magma inproc (t) | 329.5 | 1.38 | 0.050 | 0.073 | 1.48 | 1.58 | 1.32 | 40 % / 42 % |
| c1c2: Magma inproc (y, 1c) | 345.7 | 1.39 | 0.043 | 0.079 | 1.33 | 1.43 | 1.31 | 39 % / 42 % |

The sleeps break down as:
- Monolith (both stacks): the app's `waitForever` slice inside queueBuffer, woken by `kgsl-events`, i.e. the GPU fence of the previous frame.
- Inproc GL thread: woken by `mgl-srv-apply`.
- Inproc apply thread: `waitForever` (the GPU fence), as in monolith.

The table below is the same window from the ftrace text (`threadstate.py`). It carries GPU busy, because this Perfetto config does not record the kgsl events.

| arm | GL thread on-CPU | preempted | sleeping (main reason) | apply thread on-CPU | apply sleeping | both on-CPU / neither | GPU busy |
|---|---|---|---|---|---|---|---|
| MobileGlues monolith | 61 % (1.88) | 0.03 | 37 %: present fence `waitForever` 1.13 | - | - | - | 30 % |
| Magma monolith (p15t) | 78 % (3.08) | 0.03 | 20 %: present fence 0.78 | - | - | - | 29 % |
| Magma inproc (u) | 47 % (1.42) | 0.11 | 48 %: woken by the apply thread 1.45 | 52 % (1.59) | present fence 1.30 | 40 % / 41 % | 36 % |
| Espryt inproc (u) | 47 % (1.45) | 0.10 | 48 %: woken by the apply thread 1.47 | 63 % (1.95) | present fence 1.07 | 44 % / 34 % | 30 % |

What the table shows:
- Monolith Magma is CPU-bound: the GL thread is on-CPU 78 % and preemption is negligible.
- MobileGlues finishes its CPU work sooner, then waits on the present fence.
- Inproc is bounded by present-fence latency at credit 1, not by CPU: the apply thread waits on the fence while the GL thread waits on the apply thread, and for 34-41 % of the time neither runs. The GPU is only 30-36 % busy.

Sources:
- `.pftrace` captures for the Perfetto UI are in `runs/c1/*/p15.pftrace`.
- The numbers come from the same window's ftrace text via `threadstate.py`: no trace processor is installed.

H2 spread, measured as (max - min) / mean over the 3 interleaved reps of each arm:
- **sbefore (H1 probe, same day, 3 reps):** Espryt 288.6 / 287.7 / 315.2 (9.3 %), Magma 298.0 / 295.0 / 312.0 (5.6 %), MobileGlues 370.2 / 374.7 / 375.1 (1.3 %).
- **safter (H2):** Espryt 240.2 / 240.8 / 237.9 (1.2 %), Magma 245.3 / 251.3 / 244.1 (2.9 %), MobileGlues 2.0 %, dev 6-7 %.
- **The X4 core hid part of the gap:** on the A720 cluster, p15s is 0.84 (Espryt) and 0.81 (Magma) of dev. In CPU, that is +0.80 and +0.90 ms/frame over dev.

Notes:
1. hunt2 monolith comes from the profiled sub-run (hunt2-prof, simpleperf in the window), so it
   is not the same sub-session as its MobileGlues control. Dev in that sub-run: Espryt 305.3
   (1.64), Magma 302.2 (1.59), that is 0.91 / 0.90 of hunt2's MobileGlues.
2. blab monolith Espryt is p15n (p15o has no monolith arm in blab).
3. Present credit 2 (`MOBILEGL_IPC_PRESENT_CREDIT=2`); the default credit is 1. The FIF sweep
   (fifm/fifi, FIF 2/3/4) showed no significant effect; FIF 3 is the default and is the column
   shown.

H1 spread: two reps of the same arm differ by up to 10 % within a session (fifi MobileGlues 378.4
/ 343.8, apab p15m Magma monolith 289.3 / 266.6). MobileGlues ranged from 322 to 376 across
sessions. H1 ratios therefore carry roughly ±5 %.
