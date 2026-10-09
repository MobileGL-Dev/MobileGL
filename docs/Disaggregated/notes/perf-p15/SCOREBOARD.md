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
| lto ~17:30 (10-09) | o = ca0a3b74 + 2a/2b, -Pmobilegl.enableLto=OFF; l = the same, LTO ON (default) | 314.1 (1.71) | o 0.794 (2.99); l 0.800 (2.90) | o 0.850 (2.52); l 0.907 (2.36) | - | - |
| api29 ~19:00-21:30 (10-09) | a26 = 0650608d + e543e289 (default, API 26); a29 = the same tree, -Pmobilegl.androidApi=29 (42922d21); both LTO ON | 324.6 (1.67) | a26 0.771 (2.87); a29 0.777 (2.85) | a26 0.874 (2.33); a29 0.910 (2.26) | a26 0.998 (1.27 / 1.53); a29 1.013 (1.22 / 1.50) | a26 1.088 (1.23 / 1.16); a29 1.136 (1.15 / 1.09) |

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
- **lto:** ThinLTO + -O3 + ICF/gc/no-semantic-interposition (190580f2) takes GL CPU from 2.52 to 2.36 ms on
  Magma (fps 267 -> 285) and from 2.99 to 2.90 on Espryt. All runs VALID.
  - Both backends load and render in FCL (one screenshot each, runs/lto-shot).
  - Exported GL/EGL/JNI/vk symbols are identical (2973). LTO drops 3323 internal 3rdparty C++ exports.
  - libMobileGLServer.so links and its only libMobileGL import (mobilegl_server_main) is exported.
- **api29 (cpuhunt, vanilla 1.21.5 frozen world, 3 interleaved reps, one bench session per rep with the arm order rotated, 27 of 27 runs VALID):**
  API 26 (shipped, emulated TLS) against API 29 (separate artifact, native ELF TLS), the same tree, both LTO ON.
  - Libs: api26 `b843b121...`, 0 TLSDESC relocs, 98 `__emutls_v.*` symbols; api29 `1f2ec4a0...`, 95 TLSDESC relocs, a PT_TLS segment, 3 `__emutls_v.*` (libc++abi). Both load and render in FCL.
  - fps, mean of 3 (api26 -> api29, ratio) and GL / apply thread CPU ms/frame:

    | arm | fps 26 | fps 29 | 29/26 | GL cpu 26 -> 29 | apply cpu 26 -> 29 |
    |---|---|---|---|---|---|
    | Magma monolith | 283.7 | 295.5 | 1.041 | 2.33 -> 2.26 (0.970) | - |
    | Magma inproc | 353.1 | 368.6 | 1.044 | 1.23 -> 1.15 (0.930) | 1.16 -> 1.09 (0.945) |
    | Espryt monolith | 250.2 | 252.2 | 1.008 | 2.87 -> 2.85 (0.994) | - |
    | Espryt inproc | 324.0 | 328.7 | 1.015 | 1.27 -> 1.22 (0.966) | 1.53 -> 1.50 (0.978) |
    | MobileGlues (control) | 324.6 (1.67 ms) | | | | |

  - Per-rep fps: Magma mono 277.5 / 291.6 / 282.2 vs 298.5 / 286.2 / 301.9 (rep 2, where api29 ran first, is the one rep it lost by 1.9 %); Magma inproc 351.9 / 354.9 / 352.4 vs 365.3 / 374.6 / 365.9 (api29 ahead in every rep); Espryt mono 251.9 / 249.1 / 249.4 vs 253.6 / 248.0 / 255.1; Espryt inproc 327.1 / 314.9 / 329.9 vs 324.1 / 326.6 / 335.3.
    Spread (max-min)/mean is 0.8-5.3 %: the Magma inproc gain (+4.4 %, spread 0.8 / 2.5 %) is outside it, Magma monolith (+4.1 %, spread 5 %) is about at it, both Espryt gains are inside it.
  - Device state, every run: CPU 1497 (cpu2-6) / 1248 (cpu0) / 1478 (cpu7) MHz, GPU 903 MHz, samples inside the measure windows only; tpl 0, throttling 0, no "max capped"; peak over the 27 runs CPU 62.8 C, GPU 56.2 C, skin 43.9 C; daemons stopped during a session and restored after (5 running again, max clocks back to stock); swap=false; anland off; FCL pinned to cpu2-6 (taskset 7c); cooldown to battery <= 33.0 C before each session.
  - Simpleperf, one run per cell (monolith render thread, 6 s, cpu-clock 4000 Hz; `runs/api29ab-sp`). The share of TLS access in the render thread's on-CPU time:

    | cell | on-CPU ms/frame | TLS symbol | ms/frame | share |
    |---|---|---|---|---|
    | Magma api26 | 2.546 | `__emutls_get_address` | 0.052 | 2.1 % |
    | Magma api29 | 2.501 | `tlsdesc_resolver_dynamic` (linker64) | 0.051 | 2.1 % |
    | Espryt api26 | 3.016 | `__emutls_get_address` | 0.058 | 1.9 % |
    | Espryt api29 | 3.259 | `tlsdesc_resolver_dynamic` (linker64) | 0.047 | 1.4 % |

  - Reading:
    - API 29 does not remove the TLS cost of the monolith render thread. MobileGL is dlopen'ed, so bionic resolves each TLSDESC through the linker's dynamic resolver (`tlsdesc_resolver_dynamic`), a call that costs about what `__emutls_get_address` did (0.05 ms/frame, about 2 % of the thread). The native-TLS switch is therefore not the source of the Magma gain; whatever it is (code layout and codegen of a different link, or the android-29 platform), this A/B does not isolate it.
    - Untested lead for a real TLS saving: a static TLS model (initial-exec) for the hot variables. Bionic's room for static TLS in a dlopen'ed library is limited, so it needs its own measurement.
    - The Espryt monolith profile run read 242.6 / 245.4 fps against 252.2 / 254.0 for api26, opposite to the 3-rep means; it is one run, the 3 interleaved reps decide.
- **bsl3 (gpuhunt, stopped on a CPU max cap in rep 2):** r1 Magma 55.6, Espryt 52.8, MobileGlues 57.2 fps.
  That is Magma 0.97 and Espryt 0.92 of MobileGlues.
- **c1cbsl (gpuhunt, Iris + BSL, after a cooldown, 3 reps):** 1c does not cost GPU time.
  - VALID runs: t 53.8 / 53.9 fps vs y 55.7 / 55.5 (+3.4 %), all at 100 % GPU busy (18.5 -> 17.9 GPU ms/frame).
  - MobileGlues read 56.2 / 55.7 / 56.1, which puts Magma y at about 0.99 of it. All three MobileGlues
    runs (and t r2, y r2) are INVALID for one reason: the freq sampler's first sample, about 40 s
    before the windows, catches the idle GPU at 231 MHz. Every in-window sample of every run is at
    the 578 MHz pin.
  - A window-scoped validity check (freqcheck over the measure_raw.txt span) would admit them. That
    is a harness change, not yet made.
- **ebbsl (gpuhunt, Iris + BSL, Espryt monolith, 3 reps, one arm per cooldown, all VALID):** the two parked
  Espryt GPU items (native depth glCopyImageSubData, attachment shadow) do not help. Builds: x = origin
  9f610167 + the items, b = origin 9f610167, both LTO; MobileGlues is the plugin.
  - fps r1 / r2 / r3: x 51.4 / 51.3 / 51.5 (mean 51.4), b 52.7 / 51.8 / 51.0 (51.9), MobileGlues 56.5 / 55.6 / 55.3 (55.8).
    Ratio to MobileGlues: x 0.921, b 0.930 (x vs b -1.0 %, noise). Target 0.95 not reached.
  - GPU: busy 100 % in every run; union of kgsl cmdbatch intervals per frame (x / b / MobileGlues)
    19.45 / 19.29 / 17.88 ms, 1.04 cmdbatches per frame in all three.
  - Device state: gpuhunt (CPU 2380/1689 MHz, GPU 578 MHz; in-window min = max for every policy), tpl=6
    thr=0, peak CPU 75.5-79.4 C, GPU 63.2-65.9 C, skin 42.4-44.5 C, daemons stopped(5), swap=false, anland off.
  - Engagement (one simpleperf run each, render thread): `BlitDepthTexture2D` 0.100 ms/frame in b, gone in x;
    Espryt `CopyImageSubData` 0.115 -> 0.072; `SyncToBackendByHandle` 0.178 -> 0.162.
  - Vanilla cpuhunt (1500/1250/903 MHz, 3 reps, all VALID, peak CPU 58 C): Espryt monolith GL CPU x 2.87,
    b 2.83 ms/frame; fps x 249.9 / 251.4 / 244.2 (248.5), b 250.1 / 256.5 / 255.2 (254.0), GPU busy 17 %.
  - Not landed: wip/espryt-depthcopy-measured. The BSL frame is GPU-bound and its GPU time did not move.
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
