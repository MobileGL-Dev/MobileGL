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
