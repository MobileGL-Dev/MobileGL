# P14 perf pass: handoff (closed 2026-10-07)

The pass is closed at `781a7446`, plus this docs commit. Results are in [BENCH.md](BENCH.md) and
[README.md](README.md). The next phase is the per-(VAO, program) draw-packet cache
([NEXT-draw-packet-cache.md](NEXT-draw-packet-cache.md)). Its first item is an off-CPU profile of
FCL Magma monolith (README "Headline numbers").

## Where things stand (rd12, pinned, FINISH=false, best of 3)

| arm | Espryt | Magma | dev Espryt / Magma |
|---|---|---|---|
| monolith | 91.1 | 83.8 | 115.2 / 87.0 |
| inproc | 111.9 | 110.0 | |
| shm | 87.8 | 95.6 | |
| tcp | 68.7 | 69.9 | |

FCL MC in world:
- dev: Espryt 228.6, Magma 231.8;
- feat monolith: Espryt 198.3, Magma 171.4;
- feat inproc: Espryt 241.5, Magma 194.8.

All arms are CPU-bound (GPU 19-39 %).

## Open items (DEBTS.md has the rows)

1. FCL Magma monolith waits about 3 ms/frame. The trace does not show it. Profile it off-CPU
   first. Both close-out attempts caught no game frames, because FCL start-up ran past the
   60 s warm-up.
2. The draw-packet cache (design sketch). Prototype the client half on Espryt monolith first.
3. The Espryt driver +0.33 ms is a cache-pressure inference. Check it with L1D refills
   before and after (2).
4. Native TLS, minSdk 29 vs 26, is the user's call: +1.7 % Espryt, noise on Magma.

## Harness

Session scratchpad scripts:
- `build_trace.sh W:/perf .p14X`: trace APK, package `top.mobilegl.plugin.p14X.trace`.
- `sign_install.sh <apk> <name>`.
- `devjob.sh <log> <cmd>`: runs a device job under a mkdir lock. Always use it; two concurrent
  device jobs contaminate each other, and killing a job leaves its children running.
- `incl.py` / `children.py` / `lines.py` / `layers2.py` / `drvcall.py` / `offcpu.py`:
  simpleperf analyses. The tail window comes from the render thread's `retraceCall` samples.
- `profile_offcpu.sh`: `--trace-offcpu` variant of `profile_p14.sh`.
- `stat_p14.sh`: `simpleperf stat` counters.
- `fcl_offcpu.sh`: FCL profile plus restore.

Then:

```
cd tools/device_bench/disagg
FIXTURES=<main checkout>/tools/trace_replay/fixtures PREP_PKGS=top.mobilegl.plugin.p14X.trace bash matrix_p14.sh prep
ARMS="monolith@p14a monolith@p14b" BACKENDS="DirectGLES DirectVulkan" WLS=rd12 REPS=3 TAG=ab bash matrix_p14.sh run
python p14_report.py ../../../.trace-work/perf-p14/ab
ARMS="monolith@p14X inproc@p14X shm@p14X tcp@p14X" REPS=1 TAG=ssim bash matrix_p14.sh ssim   # SSIM gate
ARM=monolith BACKEND=DirectGLES WL=rd12 TAG=prof PKG_FEAT=... SYMDIR=<unstripped libs> bash profile_p14.sh
```

The FCL library comes from `:app:assemblePluginRelease`. Unzip `lib/arm64-v8a/libMobileGL.so` from
`app/build/outputs/apk/plugin/release/`; the unstripped copy is under
`intermediates/merged_native_libs/pluginRelease`. Run `fcl_p14.sh run`, then `restore`
(md5-checked).

Gradle's `packageTraceRelease` sometimes fails on the first try with
`IncrementalSplitterRunnable`; a rerun succeeds.

Host:
- Unit tests: WSL archlinux `~/mgl-perf-bld` (clang, split + inproc).
  - ServerSpawnTest, FuzzArm2 and PeerLatchTest fail there with `Refuse{BuildFingerprint}`;
    that is environmental.
  - The google-benchmark targets don't build with clang 22; also environmental.
- CI-shaped integration tree: `~/mgl-perf-ci31`, configured with CMake 3.31.10 from the
  `~/mgl-perf-tools` venv.
  - The system CMake 4.x joins `gtest_discover_tests` ENVIRONMENT lists into one variable, and
    every entry then skips. Use 3.31.10.
  - lavapipe ICD; spawn/tcp lanes are environmental there.
  - To run the syncval gates: `scripts/ci/magma_runahead_checks.py validation --build-dir ~/mgl-perf-ci31 --execute`,
    with `GIT_DIR=.../.git/modules/MobileGL/worktrees/perf` set for the cache script.
- `~/mgl-perf-ci` is a CMake 4 tree. Use it only through the scratchpad's `runentry.sh`, which
  splits the joined environment.

## Device state

- HA27Q3LQ CPU/GPU clocks are still PINNED: the next phase wants them pinned.
  `bash pin_clocks.sh restore` undoes it; ask before unpinning.
- FCL is restored: lib md5 matches `/data/local/tmp/p14-fcl-orig.so`, config.json matches its
  saved copy, and the `debug.mobilegl.*` properties are deleted.
- Trace APKs `top.mobilegl.plugin.p14*.trace` are installed; `p14fin` is the close-out build.

## CI

Dispatched runs (workflow_dispatch, not cancelled by later pushes):
- 86ab2835: Test 37702990844, APK 37702995519.
- 781a7446: Test 37707221661.
- The close-out docs head: see the final report.

There is only one self-hosted GPU runner (`minipc-8845-arch-wsl-gpu`), so GPU jobs queue behind it.
