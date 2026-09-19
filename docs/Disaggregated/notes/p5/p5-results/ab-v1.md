# ab — P5 paired device A/B on Redmi `2f7cbe2e`

## Outcome

The reboot-clean, pinned, actively-cooled session ran 2026-09-16 10:31:20–11:10:05
UTC−04 at runner head `eec0e836`. Performance was recorded against pull and never
gated. Of 40 case/backend/arm groups, 27 completed and 13 failed after exactly one
retry. Every `split` (`MOBILEGL_TRANSPORT=inproc`) group aborted before producing a
benchmark, so **barrier tax is unavailable for every case**; no value is inferred.

This is a concrete P5 blocker. DirectGLES split reached the real session/caps path,
then aborted on `Fatal{UnmigratedVerb, "DrawElementsInstancedBaseVertex"}` for
improved-transparency and `Fatal{UnmigratedVerb, "DrawElements"}` for the other four
cases. DirectVulkan split is outside P5's supported scope (BRIEF-P5 risk 9 says it is
P7), but was run because this package required the P4a backends; it failed on the same
unmigrated verbs. Separately, rd12/DirectVulkan reproduced the known
`scudo::reportMapError` crash in pull, push and splitctl.

`splitctl` did preserve the split APK's monolith path: wherever push and splitctl both
completed, frame-p50 differences were −0.1% to +1.6%, and frame-p99 differences were
−3.0% to +1.4% (the widest tails were the already noisy shader-heavy cases). That is
consistent with measurement noise; there is no evidence that the split build moved the
monolith path.

## Artifacts and install decision

- Driver: `~/w7/notes/tools/p5_ab_redmi.sh`
- Reducer: `~/w7/notes/tools/ab_reduce4_p5.py`
- Runner adapter: `~/w7/notes/tools/p5_runner_adapter.py`
- Raw session root: `C:\Users\GEEKER~1\AppData\Local\Temp\claude\C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL-disagg\0614b2e9-9ce2-49e4-a44c-6d023a133184\scratchpad\p5-ab-redmi\2f7cbe2e\`
- Full dry-run list: `dry-run-commands.txt` in that root (142 lines).
- Tables-only WSL copy: `~/w7/notes/p5/p5-results/ab-v1-tables.md`.

Badging gives three distinct, side-by-side application IDs:
`top.mobilegl.plugin.p5pull.trace`, `top.mobilegl.plugin.p5push.trace`, and
`top.mobilegl.plugin.p5split.trace`. No install-by-turn collision workaround was
needed. The stock Python wrapper hardcodes `top.mobilegl.plugin.trace`, so the adapter
sets the real per-arm package for all install, activity, force-stop and `run-as`
operations. `split` and `splitctl` both use the split ID/APK; only `split` receives
`MOBILEGL_TRANSPORT=inproc`.

## Dry run and exact rerun

The script and reducer passed shell/Python syntax checks. The dry run executed no
commands and printed the guarded reboot/fan/pin lifecycle plus 40 interleaved runner
invocations in P4a order (`pull`, `push`, `split`, `splitctl`) for each backend/case.
Representative invocation:

```text
ADB=<OUT>/adb-2f7cbe2e.sh MOBILEGL_TRACE_PACKAGE=top.mobilegl.plugin.p5split.trace \
MOBILEGL_TRACE_APK=//wsl.localhost/Arch/home/swung/w7/notes/p5/apk/trace-split.apk \
python //wsl.localhost/Arch/home/swung/w7/notes/tools/p5_runner_adapter.py \
  --case improved-transparency-minecraft-26.3 --backend DirectGLES --benchmark \
  --benchmark-no-finish --benchmark-repeats 3 --benchmark-tail-frames 200 \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120 \
  --env MOBILEGL_TRANSPORT=inproc
```

Exact full rerun from Windows Git Bash:

```bash
//wsl.localhost/Arch/home/swung/w7/notes/tools/p5_ab_redmi.sh
```

The adapter also preserves `mobilegl-runN.log`, `retrace-runN.log`, result JSON and
benchmark JSON for every repeat. Its adb shim executes only
`adb -s 2f7cbe2e ...`; neither the driver nor nested runner enumerates/touches any
other serial. A first preflight session found and preserved a host-only UNC conversion
failure under `preflight-unc-path-failure/`; no workload ran in that preflight. Its trap
was verified before the corrected reboot-clean session began.

## Session facts

- Guard: `adb -s 2f7cbe2e get-state` returned `device` before each real session.
- Reboot-clean; root returned; wake/unlock/stay-awake applied.
- Shared device lock held for the complete foreground session.
- Pin succeeded on attempt 1. Every one of the 40 before/after checks was `P/P`
  (`pin-rc.txt` = `0/0`).
- Fan was level 2 throughout; initial real speed was 15,320 rpm.
- Initial pre-pin `cpuss-0-0` temperature was 38.7 °C. Per-arm gate samples follow.
- EXIT trap verification: `session-unpin-rc.txt` = 0; verdict `UNPINNED`;
  `session-fan-final.txt` = 0; a direct post-run
  `adb -s 2f7cbe2e shell "su -c 'cat .../target_level'"` also returned 0.

| case | backend | pull °C | push °C | split °C | splitctl °C |
|---|---|---:|---:|---:|---:|
| improved-transparency-26.3 | DirectGLES | 37.2 | 37.2 | 38.0 | 39.5 |
| improved-transparency-26.3 | DirectVulkan | 38.7 | 38.4 | 38.4 | 38.7 |
| rd12-odinlite | DirectGLES | 39.1 | 39.9 | 39.5 | 39.5 |
| rd12-odinlite | DirectVulkan | 39.9 | 39.5 | 39.9 | 39.9 |
| fabric-sodium | DirectGLES | 39.9 | 39.5 | 39.9 | 39.5 |
| fabric-sodium | DirectVulkan | 39.9 | 39.9 | 39.9 | 39.5 |
| 1.21.4-in-world | DirectGLES | 39.9 | 39.9 | 39.9 | 39.9 |
| 1.21.4-in-world | DirectVulkan | 39.9 | 39.9 | 39.9 | 39.5 |
| fabric-iris-bsl | DirectGLES | 39.9 | 39.5 | 39.5 | 39.9 |
| fabric-iris-bsl | DirectVulkan | 39.5 | 39.5 | 39.9 | 39.9 |

## Failures (verbatim signatures)

- improved-transparency, both split backends:
  `MGPipe: Fatal{UnmigratedVerb, "DrawElementsInstancedBaseVertex"}`.
- rd12, fabric-sodium, vanilla and iris-BSL, both split backends:
  `MGPipe: Fatal{UnmigratedVerb, "DrawElements"}`.
- rd12/DirectVulkan pull, push and splitctl:
  `scudo::reportMapError(unsigned long)` followed by `Fatal signal 6 (SIGABRT)`;
  this is the previously recorded rd12/Magma defect and reproduced across builds.

Each failure's complete stdout/stderr, retrace log, MobileGL log and logcat remain in
its `attempt-1/` and `attempt-2/` directory. No group was retried more than once.

## Reduced tables and raw numbers

# P5 Redmi four-arm A/B tables

Recorded against pull; no performance threshold is applied. Values are the runner's lowest-mean-wall-time repeat of three, reduced over its trailing 200 frames.

## improved-transparency-minecraft-26.3

| backend | arm | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms | pmap | mpr | rsp | peak RSS MiB | mapped MiB | pins | rc |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| DirectGLES | pull | 10.535 | 25.415 | 10.447 | 25.213 | 0 | - | - | - | - | P/P | 0 |
| DirectGLES | push | 12.106 | 26.973 | 12.011 | 26.775 | 0 | 8 | 0 | - | - | P/P | 0 |
| DirectGLES | split | - | - | - | - | - | - | - | 204.9 | 112.5 | P/P | 1 |
| DirectGLES | splitctl | 12.235 | 27.131 | 12.135 | 26.936 | 0 | 8 | 0 | - | - | P/P | 0 |
| DirectVulkan | pull | 10.742 | 25.031 | 10.662 | 24.859 | 0 | - | - | - | - | P/P | 0 |
| DirectVulkan | push | 11.678 | 26.134 | 11.579 | 25.959 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | split | - | - | - | - | - | - | - | 201.7 | 112.5 | P/P | 1 |
| DirectVulkan | splitctl | 11.837 | 26.476 | 11.756 | 26.195 | 0 | 0 | 0 | - | - | P/P | 0 |

| backend | paired delta | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms |
|---|---|---:|---:|---:|---:|
| DirectGLES | push - pull | +1.571 (+14.9%) | +1.558 (+6.1%) | +1.564 (+15.0%) | +1.562 (+6.2%) |
| DirectGLES | **barrier tax: split - push** | - | - | - | - |
| DirectGLES | splitctl - push | +0.129 (+1.1%) | +0.158 (+0.6%) | +0.124 (+1.0%) | +0.161 (+0.6%) |
| DirectVulkan | push - pull | +0.937 (+8.7%) | +1.103 (+4.4%) | +0.917 (+8.6%) | +1.100 (+4.4%) |
| DirectVulkan | **barrier tax: split - push** | - | - | - | - |
| DirectVulkan | splitctl - push | +0.159 (+1.4%) | +0.342 (+1.3%) | +0.177 (+1.5%) | +0.236 (+0.9%) |

## minecraft-1.21.4-fabric-iris-bsl-in-world

| backend | arm | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms | pmap | mpr | rsp | peak RSS MiB | mapped MiB | pins | rc |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| DirectGLES | pull | 8.309 | 326.025 | 1.600 | 321.087 | 0 | - | - | - | - | P/P | 0 |
| DirectGLES | push | 8.320 | 322.547 | 1.928 | 318.752 | 0 | 2 | 0 | - | - | P/P | 0 |
| DirectGLES | split | - | - | - | - | - | - | - | 205.2 | 112.5 | P/P | 1 |
| DirectGLES | splitctl | 8.316 | 327.215 | 1.944 | 321.839 | 0 | 2 | 0 | - | - | P/P | 0 |
| DirectVulkan | pull | 0.741 | 465.536 | 0.738 | 459.608 | 0 | - | - | - | - | P/P | 0 |
| DirectVulkan | push | 0.789 | 466.818 | 0.788 | 461.340 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | split | - | - | - | - | - | - | - | 202.4 | 112.5 | P/P | 1 |
| DirectVulkan | splitctl | 0.790 | 455.803 | 0.789 | 451.247 | 0 | 0 | 0 | - | - | P/P | 0 |

| backend | paired delta | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms |
|---|---|---:|---:|---:|---:|
| DirectGLES | push - pull | +0.011 (+0.1%) | -3.478 (-1.1%) | +0.328 (+20.5%) | -2.335 (-0.7%) |
| DirectGLES | **barrier tax: split - push** | - | - | - | - |
| DirectGLES | splitctl - push | -0.004 (-0.0%) | +4.668 (+1.4%) | +0.016 (+0.8%) | +3.087 (+1.0%) |
| DirectVulkan | push - pull | +0.048 (+6.5%) | +1.282 (+0.3%) | +0.050 (+6.8%) | +1.732 (+0.4%) |
| DirectVulkan | **barrier tax: split - push** | - | - | - | - |
| DirectVulkan | splitctl - push | +0.001 (+0.1%) | -11.015 (-2.4%) | +0.001 (+0.1%) | -10.093 (-2.2%) |

## minecraft-1.21.4-fabric-sodium-in-world

| backend | arm | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms | pmap | mpr | rsp | peak RSS MiB | mapped MiB | pins | rc |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| DirectGLES | pull | 8.314 | 9.573 | 1.320 | 2.504 | 0 | - | - | - | - | P/P | 0 |
| DirectGLES | push | 8.311 | 9.701 | 1.468 | 2.509 | 0 | 1 | 0 | - | - | P/P | 0 |
| DirectGLES | split | - | - | - | - | - | - | - | 204.8 | 112.5 | P/P | 1 |
| DirectGLES | splitctl | 8.303 | 9.486 | 1.478 | 2.560 | 0 | 1 | 0 | - | - | P/P | 0 |
| DirectVulkan | pull | 0.508 | 1.145 | 0.475 | 1.145 | 0 | - | - | - | - | P/P | 0 |
| DirectVulkan | push | 0.533 | 1.184 | 0.503 | 1.147 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | split | - | - | - | - | - | - | - | 201.7 | 112.5 | P/P | 1 |
| DirectVulkan | splitctl | 0.534 | 1.167 | 0.507 | 1.152 | 0 | 0 | 0 | - | - | P/P | 0 |

| backend | paired delta | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms |
|---|---|---:|---:|---:|---:|
| DirectGLES | push - pull | -0.003 (-0.0%) | +0.128 (+1.3%) | +0.148 (+11.2%) | +0.005 (+0.2%) |
| DirectGLES | **barrier tax: split - push** | - | - | - | - |
| DirectGLES | splitctl - push | -0.008 (-0.1%) | -0.215 (-2.2%) | +0.010 (+0.6%) | +0.051 (+2.0%) |
| DirectVulkan | push - pull | +0.025 (+5.0%) | +0.039 (+3.4%) | +0.028 (+5.9%) | +0.002 (+0.2%) |
| DirectVulkan | **barrier tax: split - push** | - | - | - | - |
| DirectVulkan | splitctl - push | +0.001 (+0.2%) | -0.017 (-1.4%) | +0.004 (+0.8%) | +0.005 (+0.4%) |

## minecraft-1.21.4-in-world

| backend | arm | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms | pmap | mpr | rsp | peak RSS MiB | mapped MiB | pins | rc |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| DirectGLES | pull | 8.291 | 9.978 | 2.365 | 5.235 | 0 | - | - | - | - | P/P | 0 |
| DirectGLES | push | 8.292 | 10.085 | 2.879 | 5.709 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectGLES | split | - | - | - | - | - | - | - | 204.9 | 112.5 | P/P | 1 |
| DirectGLES | splitctl | 8.326 | 9.778 | 2.909 | 5.785 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | pull | 1.043 | 2.293 | 1.030 | 2.273 | 0 | - | - | - | - | P/P | 0 |
| DirectVulkan | push | 1.159 | 2.404 | 1.150 | 2.367 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | split | - | - | - | - | - | - | - | 202.0 | 112.5 | P/P | 1 |
| DirectVulkan | splitctl | 1.179 | 2.426 | 1.167 | 2.408 | 0 | 0 | 0 | - | - | P/P | 0 |

| backend | paired delta | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms |
|---|---|---:|---:|---:|---:|
| DirectGLES | push - pull | +0.001 (+0.0%) | +0.107 (+1.1%) | +0.514 (+21.8%) | +0.474 (+9.1%) |
| DirectGLES | **barrier tax: split - push** | - | - | - | - |
| DirectGLES | splitctl - push | +0.034 (+0.4%) | -0.307 (-3.0%) | +0.030 (+1.0%) | +0.076 (+1.3%) |
| DirectVulkan | push - pull | +0.116 (+11.1%) | +0.111 (+4.8%) | +0.120 (+11.7%) | +0.094 (+4.1%) |
| DirectVulkan | **barrier tax: split - push** | - | - | - | - |
| DirectVulkan | splitctl - push | +0.019 (+1.6%) | +0.022 (+0.9%) | +0.017 (+1.5%) | +0.041 (+1.7%) |

## minecraft-1.21.4-rd12-odinlite-in-world

| backend | arm | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms | pmap | mpr | rsp | peak RSS MiB | mapped MiB | pins | rc |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| DirectGLES | pull | 7.999 | 22.089 | 7.921 | 21.878 | 0 | - | - | - | - | P/P | 0 |
| DirectGLES | push | 11.105 | 24.865 | 11.011 | 24.686 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectGLES | split | - | - | - | - | - | - | - | 205.5 | 112.5 | P/P | 1 |
| DirectGLES | splitctl | 11.261 | 24.771 | 11.168 | 24.575 | 0 | 0 | 0 | - | - | P/P | 0 |
| DirectVulkan | pull | - | - | - | - | - | - | - | - | - | P/P | 1 |
| DirectVulkan | push | - | - | - | - | - | - | - | - | - | P/P | 1 |
| DirectVulkan | split | - | - | - | - | - | - | - | 202.2 | 112.5 | P/P | 1 |
| DirectVulkan | splitctl | - | - | - | - | - | - | - | - | - | P/P | 1 |

| backend | paired delta | frame p50 ms | frame p99 ms | thread CPU p50 ms | thread CPU p99 ms |
|---|---|---:|---:|---:|---:|
| DirectGLES | push - pull | +3.106 (+38.8%) | +2.776 (+12.6%) | +3.089 (+39.0%) | +2.808 (+12.8%) |
| DirectGLES | **barrier tax: split - push** | - | - | - | - |
| DirectGLES | splitctl - push | +0.156 (+1.4%) | -0.094 (-0.4%) | +0.157 (+1.4%) | -0.111 (-0.4%) |
| DirectVulkan | push - pull | - | - | - | - |
| DirectVulkan | **barrier tax: split - push** | - | - | - | - |
| DirectVulkan | splitctl - push | - | - | - | - |

## Raw repeats and log counters

### improved-transparency-minecraft-26.3 / DirectGLES / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 10.595 | 25.479 | 10.502 | 25.307 | 11.748 |
| 2 | 10.535 | 25.415 | 10.447 | 25.213 | 11.673 |
| 3 | 10.569 | 25.454 | 10.470 | 25.228 | 11.714 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:33:00] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=87495 acc/draw=9.57 bytes/f[buf=34962.67 tex=306.33 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=2 box=2 rect=0 jobs=2] gates[ers=9088/453 etl=630/8911 eub=535/9006 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:01] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=103336 acc/draw=10.50 bytes/f[buf=96267.73 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=9532/908 etl=840/9600 eub=840/9600 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:02] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=103470 acc/draw=10.50 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=9555/900 etl=840/9615 eub=840/9615 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:03] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=103250 acc/draw=10.50 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=9533/900 etl=840/9593 eub=840/9593 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:04] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=103260 acc/draw=10.50 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=9534/900 etl=840/9594 eub=840/9594 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:05] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=105706 acc/draw=10.49 bytes/f[buf=82582.80 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=9769/908 etl=840/9837 eub=840/9837 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:06] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=114322 acc/draw=10.67 bytes/f[buf=105143.08 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=10360/996 etl=891/10465 eub=887/10469 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:08] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=587791 acc/draw=9.14 bytes/f[buf=423253.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=62787/2809 etl=53411/12185 eub=53050/12546 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:09] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=519050 acc/draw=9.19 bytes/f[buf=325797.58 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=55075/2699 etl=46954/10820 eub=46594/11180 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1336220 acc/draw=8.44 bytes/f[buf=332631.20 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=156925/2791 etl=148606/11110 eub=148246/11470 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1347470 acc/draw=8.44 bytes/f[buf=295586.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=158155/2799 etl=149578/11376 eub=149218/11736 mfp=0/0 mpm=0/0 mdt=0/0]
[22:33:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=668777 acc/draw=8.39 bytes/f[buf=301156.88 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0]
```

### improved-transparency-minecraft-26.3 / DirectGLES / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 12.133 | 27.106 | 12.037 | 26.889 | 13.317 |
| 2 | 12.177 | 27.084 | 12.081 | 26.913 | 13.442 |
| 3 | 12.106 | 26.973 | 12.011 | 26.775 | 13.290 |

Best-repeat aggregate: pmap=0 mpr=8 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:34:34] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=68695 acc/draw=7.52 bytes/f[buf=34962.67 tex=306.33 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=30.20 csob-blob=7.50] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=6 csob=428 mpr=4] emit[fbe=1197 sve=9143 sse=111 sie=0 ctu=11464 trp=0 rsp=0] gates[ers=9088/453 etl=396/9145 eub=9541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:35] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=82936 acc/draw=8.43 bytes/f[buf=96267.73 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=3 csob=688 mpr=0] emit[fbe=2300 sve=9840 sse=528 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9532/908 etl=600/9840 eub=10440/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:36] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=83040 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9855 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9555/900 etl=600/9855 eub=10455/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:37] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=82864 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9833 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9533/900 etl=600/9833 eub=10433/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:38] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=82872 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9834 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9534/900 etl=600/9834 eub=10434/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:39] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=84832 acc/draw=8.42 bytes/f[buf=82582.80 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=682 mpr=0] emit[fbe=2307 sve=10077 sse=521 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9769/908 etl=600/10077 eub=10677/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:40] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=92142 acc/draw=8.60 bytes/f[buf=105143.08 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=66.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=13 csob=748 mpr=2] emit[fbe=2458 sve=10706 sse=546 sie=0 ctu=3 trp=0 rsp=0] gates[ers=10360/996 etl=643/10713 eub=11356/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:41] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=457791 acc/draw=7.12 bytes/f[buf=423253.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=187.27 csob-blob=0.83] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=6 csob=2340 mpr=2] emit[fbe=4487 sve=12423 sse=1206 sie=0 ctu=2 trp=0 rsp=0] gates[ers=62787/2809 etl=53171/12425 eub=65596/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=404702 acc/draw=7.17 bytes/f[buf=325797.58 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=179.93 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2256 mpr=0] emit[fbe=4383 sve=11060 sse=1131 sie=0 ctu=0 trp=0 rsp=0] gates[ers=55075/2699 etl=46714/11060 eub=57774/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:44] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1017988 acc/draw=6.43 bytes/f[buf=332631.20 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.07 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2344 mpr=0] emit[fbe=4498 sve=11350 sse=1218 sie=0 ctu=0 trp=0 rsp=0] gates[ers=156925/2791 etl=148366/11350 eub=159716/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:45] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1026762 acc/draw=6.43 bytes/f[buf=295586.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.60 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2348 mpr=0] emit[fbe=4482 sve=11615 sse=1221 sie=0 ctu=1 trp=0 rsp=0] gates[ers=158155/2799 etl=149338/11616 eub=160954/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:34:46] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=508723 acc/draw=6.39 bytes/f[buf=301156.88 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=185.36 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=1157 mpr=0] emit[fbe=2213 sve=5069 sse=599 sie=0 ctu=0 trp=0 rsp=0] gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### improved-transparency-minecraft-26.3 / DirectGLES / split
FAILED rc=1

```text
09-16 22:35:34.313  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:35:34.313  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:35:34.313  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:35:34.318  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.321  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=16, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=193522, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1400208}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.321  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=12, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=119188, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1281044}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.334  3154  3294 I libc    : kill: send 9 to pid -19536
09-16 22:35:34.334  3154  3294 I libc    : debug pid reuse, real pid -19536
09-16 22:35:34.355  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:35:34.390  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.390  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.390  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:34.390  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:35.009  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=24, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=241114, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1638536}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:35.010  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=16, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=193522, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1400208}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:35.010  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=12, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=119188, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1281044}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:35:35.012  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 22:35:35.012  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:35 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:35 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:35 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:35 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 14450
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:35 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:35 ..
-rw-r--r-- 1 u0_a290 u0_a290   368306 2026-09-16 22:35 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 14404890 2026-09-16 22:35 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 50
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:35 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:35 ..
-rw------- 1 u0_a290 u0_a290 40953 2026-09-16 22:35 mobilegl.log
-rw------- 1 u0_a290 u0_a290   921 2026-09-16 22:35 retrace.log
improved-transparency-minecraft-26.3 / DirectGLES run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:35:33] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=214441984 currentRss=214441984 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:35:33] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=214818816 currentRss=214818816 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### improved-transparency-minecraft-26.3 / DirectGLES / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 12.296 | 27.120 | 12.207 | 26.884 | 13.513 |
| 2 | 12.235 | 27.131 | 12.135 | 26.936 | 13.449 |
| 3 | 12.261 | 27.103 | 12.152 | 26.921 | 13.454 |

Best-repeat aggregate: pmap=0 mpr=8 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:36:04] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=68695 acc/draw=7.52 bytes/f[buf=34962.67 tex=306.33 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=30.20 csob-blob=7.50] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=6 csob=428 mpr=4] emit[fbe=1197 sve=9143 sse=111 sie=0 ctu=11464 trp=0 rsp=0] gates[ers=9088/453 etl=396/9145 eub=9541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:05] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=82936 acc/draw=8.43 bytes/f[buf=96267.73 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=3 csob=688 mpr=0] emit[fbe=2300 sve=9840 sse=528 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9532/908 etl=600/9840 eub=10440/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:06] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=83040 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9855 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9555/900 etl=600/9855 eub=10455/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:07] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=82864 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9833 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9533/900 etl=600/9833 eub=10433/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:08] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=82872 acc/draw=8.43 bytes/f[buf=81064.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=2300 sve=9834 sse=520 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9534/900 etl=600/9834 eub=10434/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:09] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=84832 acc/draw=8.42 bytes/f[buf=82582.80 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=682 mpr=0] emit[fbe=2307 sve=10077 sse=521 sie=0 ctu=0 trp=0 rsp=0] gates[ers=9769/908 etl=600/10077 eub=10677/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=92142 acc/draw=8.60 bytes/f[buf=105143.08 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=66.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=13 csob=748 mpr=2] emit[fbe=2458 sve=10706 sse=546 sie=0 ctu=3 trp=0 rsp=0] gates[ers=10360/996 etl=643/10713 eub=11356/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=457791 acc/draw=7.12 bytes/f[buf=423253.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=187.27 csob-blob=0.83] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=6 csob=2340 mpr=2] emit[fbe=4487 sve=12423 sse=1206 sie=0 ctu=2 trp=0 rsp=0] gates[ers=62787/2809 etl=53171/12425 eub=65596/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=404702 acc/draw=7.17 bytes/f[buf=325797.58 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=179.93 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2256 mpr=0] emit[fbe=4383 sve=11060 sse=1131 sie=0 ctu=0 trp=0 rsp=0] gates[ers=55075/2699 etl=46714/11060 eub=57774/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:14] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1017988 acc/draw=6.43 bytes/f[buf=332631.20 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.07 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2344 mpr=0] emit[fbe=4498 sve=11350 sse=1218 sie=0 ctu=0 trp=0 rsp=0] gates[ers=156925/2791 etl=148366/11350 eub=159716/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:16] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1026762 acc/draw=6.43 bytes/f[buf=295586.63 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.60 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2348 mpr=0] emit[fbe=4482 sve=11615 sse=1221 sie=0 ctu=1 trp=0 rsp=0] gates[ers=158155/2799 etl=149338/11616 eub=160954/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:36:17] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=508723 acc/draw=6.39 bytes/f[buf=301156.88 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=185.36 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=1157 mpr=0] emit[fbe=2213 sve=5069 sse=599 sie=0 ctu=0 trp=0 rsp=0] gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### improved-transparency-minecraft-26.3 / DirectVulkan / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 10.742 | 25.031 | 10.662 | 24.859 | 11.634 |
| 2 | 10.761 | 25.066 | 10.681 | 24.868 | 11.669 |
| 3 | 10.772 | 25.124 | 10.680 | 24.944 | 11.640 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:36:55] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=61420 acc/draw=6.72 bytes/f[buf=239026.13 tex=289550.00 ubog=0.00 ubon=11143.87 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=8680 box=8679 rect=1 jobs=8697] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9138 mpm=8719/419 mdt=8831/307]
[22:36:55] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=81088 acc/draw=8.24 bytes/f[buf=95091.73 tex=9525.60 ubog=0.00 ubon=12005.33 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=2597 box=2597 rect=0 jobs=2597] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9840 mpm=8412/1428 mdt=9212/628]
[22:36:56] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=81050 acc/draw=8.22 bytes/f[buf=79888.00 tex=1693.63 ubog=0.00 ubon=12018.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=155 box=155 rect=0 jobs=155] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9855 mpm=8435/1420 mdt=9235/620]
[22:36:56] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=80918 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11991.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9833 mpm=8413/1420 mdt=9213/620]
[22:36:56] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=80924 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11992.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9834 mpm=8414/1420 mdt=9214/620]
[22:36:56] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=82569 acc/draw=8.19 bytes/f[buf=81406.80 tex=0.00 ubog=0.00 ubon=12284.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10077 mpm=8645/1432 mdt=9450/627]
[22:36:57] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=87626 acc/draw=8.18 bytes/f[buf=2343393.95 tex=153.60 ubog=0.00 ubon=13083.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=3 box=3 rect=0 jobs=3] gates[ers=0/0 etl=0/0 eub=0/0 mfp=9/10709 mpm=9201/1508 mdt=10038/680]
[22:36:58] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=440209 acc/draw=6.85 bytes/f[buf=1737307.00 tex=273.07 ubog=0.00 ubon=128592.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=2 box=2 rect=0 jobs=2] gates[ers=0/0 etl=0/0 eub=0/0 mfp=6525/57751 mpm=55104/2647 mdt=62478/1798]
[22:36:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=387813 acc/draw=6.87 bytes/f[buf=164892.75 tex=0.00 ubog=0.00 ubon=113544.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=4805/51649 mpm=49130/2519 mdt=54760/1694]
[22:37:00] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1034106 acc/draw=6.53 bytes/f[buf=172642.40 tex=0.00 ubog=0.00 ubon=331172.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21360/137036 mpm=134421/2615 mdt=156611/1785]
[22:37:02] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1041827 acc/draw=6.53 bytes/f[buf=135597.03 tex=273.07 ubog=0.00 ubon=333744.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=1 box=1 rect=0 jobs=1] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21413/138221 mpm=135594/2627 mdt=157842/1792]
[22:37:02] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=519376 acc/draw=6.52 bytes/f[buf=138530.71 tex=0.00 ubog=0.00 ubon=340328.68 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868]
```

### improved-transparency-minecraft-26.3 / DirectVulkan / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 11.678 | 26.134 | 11.579 | 25.959 | 12.588 |
| 2 | 11.628 | 26.196 | 11.542 | 25.997 | 12.589 |
| 3 | 11.663 | 26.240 | 11.582 | 25.975 | 12.628 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:37:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=61420 acc/draw=6.72 bytes/f[buf=239026.13 tex=289550.00 ubog=0.00 ubon=11143.87 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=30.20 csob-blob=0.00] tex[emit=8680 box=8679 rect=1 jobs=8697] cso[csom=6 csob=428 mpr=0] emit[fbe=424 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9138 mpm=8719/419 mdt=8831/307]
[22:37:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=81088 acc/draw=8.24 bytes/f[buf=95091.73 tex=9525.60 ubog=0.00 ubon=12005.33 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=2597 box=2597 rect=0 jobs=2597] cso[csom=3 csob=688 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9840 mpm=8412/1428 mdt=9212/628]
[22:37:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=81050 acc/draw=8.22 bytes/f[buf=79888.00 tex=1693.63 ubog=0.00 ubon=12018.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=155 box=155 rect=0 jobs=155] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9855 mpm=8435/1420 mdt=9235/620]
[22:37:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=80918 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11991.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9833 mpm=8413/1420 mdt=9213/620]
[22:37:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=80924 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11992.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9834 mpm=8414/1420 mdt=9214/620]
[22:37:50] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=82569 acc/draw=8.19 bytes/f[buf=81406.80 tex=0.00 ubog=0.00 ubon=12284.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=682 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10077 mpm=8645/1432 mdt=9450/627]
[22:37:50] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=87626 acc/draw=8.18 bytes/f[buf=2343393.95 tex=153.60 ubog=0.00 ubon=13083.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=66.40 csob-blob=0.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=13 csob=748 mpr=0] emit[fbe=669 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=9/10709 mpm=9201/1508 mdt=10038/680]
[22:37:52] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=440209 acc/draw=6.85 bytes/f[buf=1737307.00 tex=273.07 ubog=0.00 ubon=128592.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=187.27 csob-blob=0.00] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=6 csob=2340 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=6525/57751 mpm=55104/2647 mdt=62478/1798]
[22:37:52] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=387813 acc/draw=6.87 bytes/f[buf=164892.75 tex=0.00 ubog=0.00 ubon=113544.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=179.93 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2256 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=4805/51649 mpm=49130/2519 mdt=54760/1694]
[22:37:54] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1034106 acc/draw=6.53 bytes/f[buf=172642.40 tex=0.00 ubog=0.00 ubon=331172.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.07 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2344 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21360/137036 mpm=134421/2615 mdt=156611/1785]
[22:37:55] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1041827 acc/draw=6.53 bytes/f[buf=135597.03 tex=273.07 ubog=0.00 ubon=333744.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.60 csob-blob=0.00] tex[emit=1 box=1 rect=0 jobs=1] cso[csom=0 csob=2348 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21413/138221 mpm=135594/2627 mdt=157842/1792]
[22:37:56] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=519376 acc/draw=6.52 bytes/f[buf=138530.71 tex=0.00 ubog=0.00 ubon=340328.68 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=185.36 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=1157 mpr=0] emit[fbe=719 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868]
```

### improved-transparency-minecraft-26.3 / DirectVulkan / split
FAILED rc=1

```text
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:39:24.044  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:39:24.046  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=44, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=470843, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2234356}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.048  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=36, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=430626, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1996028}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.048  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=32, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=355331, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=1876864}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.068  3154  3294 I libc    : kill: send 9 to pid -25767
09-16 22:39:24.068  3154  3294 I libc    : debug pid reuse, real pid -25767
09-16 22:39:24.091  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:39:24.126  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=44, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=470843, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2234356}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.126  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=44, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=470843, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2234356}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.126  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=44, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=470843, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2234356}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:39:24.126  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=44, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=470843, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2234356}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:39 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:39 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:39 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:39 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 14450
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:39 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:39 ..
-rw-r--r-- 1 u0_a290 u0_a290   368306 2026-09-16 22:39 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 14404890 2026-09-16 22:39 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 42
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:39 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:39 ..
-rw------- 1 u0_a290 u0_a290 29332 2026-09-16 22:39 mobilegl.log
-rw------- 1 u0_a290 u0_a290   123 2026-09-16 22:39 retrace.log
improved-transparency-minecraft-26.3 / DirectVulkan run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:39:23] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=211152896 currentRss=211152896 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:39:23] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=211472384 currentRss=211472384 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### improved-transparency-minecraft-26.3 / DirectVulkan / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 11.862 | 26.436 | 11.765 | 26.242 | 12.829 |
| 2 | 11.837 | 26.476 | 11.756 | 26.195 | 12.825 |
| 3 | 11.979 | 26.525 | 11.877 | 26.361 | 12.965 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:39:57] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=9138 draws/f=76.15 acc=61420 acc/draw=6.72 bytes/f[buf=239026.13 tex=289550.00 ubog=0.00 ubon=11143.87 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=30.20 csob-blob=0.00] tex[emit=8680 box=8679 rect=1 jobs=8697] cso[csom=6 csob=428 mpr=0] emit[fbe=424 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9138 mpm=8719/419 mdt=8831/307]
[22:39:57] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=9840 draws/f=82.00 acc=81088 acc/draw=8.24 bytes/f[buf=95091.73 tex=9525.60 ubog=0.00 ubon=12005.33 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=2597 box=2597 rect=0 jobs=2597] cso[csom=3 csob=688 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9840 mpm=8412/1428 mdt=9212/628]
[22:39:58] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=9855 draws/f=82.13 acc=81050 acc/draw=8.22 bytes/f[buf=79888.00 tex=1693.63 ubog=0.00 ubon=12018.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=155 box=155 rect=0 jobs=155] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9855 mpm=8435/1420 mdt=9235/620]
[22:39:58] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=9833 draws/f=81.94 acc=80918 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11991.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9833 mpm=8413/1420 mdt=9213/620]
[22:39:58] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=9834 draws/f=81.95 acc=80924 acc/draw=8.23 bytes/f[buf=79888.00 tex=0.00 ubog=0.00 ubon=11992.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=680 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/9834 mpm=8414/1420 mdt=9214/620]
[22:39:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=10077 draws/f=83.98 acc=82569 acc/draw=8.19 bytes/f[buf=81406.80 tex=0.00 ubog=0.00 ubon=12284.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=60.53 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=682 mpr=0] emit[fbe=600 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10077 mpm=8645/1432 mdt=9450/627]
[22:39:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=10718 draws/f=89.32 acc=87626 acc/draw=8.18 bytes/f[buf=2343393.95 tex=153.60 ubog=0.00 ubon=13083.60 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=66.40 csob-blob=0.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=13 csob=748 mpr=0] emit[fbe=669 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=9/10709 mpm=9201/1508 mdt=10038/680]
[22:40:00] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=64276 draws/f=535.63 acc=440209 acc/draw=6.85 bytes/f[buf=1737307.00 tex=273.07 ubog=0.00 ubon=128592.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=187.27 csob-blob=0.00] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=6 csob=2340 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=6525/57751 mpm=55104/2647 mdt=62478/1798]
[22:40:01] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=56454 draws/f=470.45 acc=387813 acc/draw=6.87 bytes/f[buf=164892.75 tex=0.00 ubog=0.00 ubon=113544.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=179.93 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2256 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=4805/51649 mpm=49130/2519 mdt=54760/1694]
[22:40:03] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=158396 draws/f=1319.97 acc=1034106 acc/draw=6.53 bytes/f[buf=172642.40 tex=0.00 ubog=0.00 ubon=331172.80 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.07 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2344 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21360/137036 mpm=134421/2615 mdt=156611/1785]
[22:40:04] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=159634 draws/f=1330.28 acc=1041827 acc/draw=6.53 bytes/f[buf=135597.03 tex=273.07 ubog=0.00 ubon=333744.40 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=186.60 csob-blob=0.00] tex[emit=1 box=1 rect=0 jobs=1] cso[csom=0 csob=2348 mpr=0] emit[fbe=1440 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=21413/138221 mpm=135594/2627 mdt=157842/1792]
[22:40:05] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1379 window=59 draws=79668 draws/f=1350.31 acc=519376 acc/draw=6.52 bytes/f[buf=138530.71 tex=0.00 ubog=0.00 ubon=340328.68 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=185.36 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=1157 mpr=0] emit[fbe=719 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.334 | 599.112 | 1.771 | 588.772 | 36.991 |
| 2 | 8.287 | 328.816 | 1.625 | 324.691 | 27.527 |
| 3 | 8.309 | 326.025 | 1.600 | 321.087 | 27.114 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:04:41] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=58593 acc/draw=21.04 bytes/f[buf=32605.07 tex=8768.80 ubog=1813.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=3 box=3 rect=0 jobs=3] gates[ers=1958/1843 etl=722/3079 eub=722/3079 mfp=0/0 mpm=0/0 mdt=0/0]
[23:04:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=7248 acc/draw=40.95 bytes/f[buf=9404216.00 tex=0.00 ubog=22933.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.333 | 603.181 | 1.952 | 592.245 | 36.863 |
| 2 | 8.320 | 322.547 | 1.928 | 318.752 | 27.021 |
| 3 | 8.294 | 324.362 | 1.928 | 319.776 | 27.473 |

Best-repeat aggregate: pmap=0 mpr=2 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:05:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=52211 acc/draw=18.75 bytes/f[buf=32605.07 tex=8768.80 ubog=1813.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=124.27 csob-blob=1890.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=22 csob=1796 mpr=2] emit[fbe=1746 sve=3407 sse=89 sie=0 ctu=50 trp=0 rsp=0] gates[ers=1958/1843 etl=458/3343 eub=3800/2 mfp=0/0 mpm=0/0 mdt=0/0]
[23:05:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=6916 acc/draw=39.07 bytes/f[buf=9404216.00 tex=0.00 ubog=22933.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=370.67 csob-blob=22933.33] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=1 csob=122 mpr=0] emit[fbe=141 sve=193 sse=32 sie=0 ctu=0 trp=0 rsp=0] gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / split
FAILED rc=1

```text
09-16 23:06:12.987  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 23:06:12.987  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 23:06:12.987  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=196, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2079787, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6762588}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:12.990  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=188, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2037899, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6524260}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:12.990  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=184, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1988919, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6405096}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:13.010  3154  3294 I libc    : kill: send 9 to pid -15697
09-16 23:06:13.010  3154  3294 I libc    : debug pid reuse, real pid -15697
09-16 23:06:13.035  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 23:06:13.061  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=196, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2079787, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6762588}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:13.061  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=196, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2079787, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6762588}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:13.062  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=196, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2079787, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6762588}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:06:13.062  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=196, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2079787, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6762588}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 23:06 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 23:06 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:06 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:06 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 13546
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 23:06 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 23:06 ..
-rw-r--r-- 1 u0_a290 u0_a290   606400 2026-09-16 23:06 alternate-golden.png
-rw-r--r-- 1 u0_a290 u0_a290   602071 2026-09-16 23:06 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 12635537 2026-09-16 23:06 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 86
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 23:06 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 23:06 ..
-rw------- 1 u0_a290 u0_a290 74416 2026-09-16 23:06 mobilegl.log
-rw------- 1 u0_a290 u0_a290   981 2026-09-16 23:06 retrace.log
minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[23:06:12] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=214753280 currentRss=214753280 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[23:06:12] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=215171072 currentRss=215171072 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.328 | 601.612 | 1.978 | 591.719 | 37.045 |
| 2 | 8.334 | 329.371 | 1.904 | 324.370 | 27.423 |
| 3 | 8.316 | 327.215 | 1.944 | 321.839 | 27.068 |

Best-repeat aggregate: pmap=0 mpr=2 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:06:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=52211 acc/draw=18.75 bytes/f[buf=32605.07 tex=8768.80 ubog=1813.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=124.27 csob-blob=1890.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=22 csob=1796 mpr=2] emit[fbe=1746 sve=3407 sse=89 sie=0 ctu=50 trp=0 rsp=0] gates[ers=1958/1843 etl=458/3343 eub=3800/2 mfp=0/0 mpm=0/0 mdt=0/0]
[23:06:50] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=6916 acc/draw=39.07 bytes/f[buf=9404216.00 tex=0.00 ubog=22933.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=370.67 csob-blob=22933.33] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=1 csob=122 mpr=0] emit[fbe=141 sve=193 sse=32 sie=0 ctu=0 trp=0 rsp=0] gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.735 | 472.288 | 0.734 | 466.863 | 23.852 |
| 2 | 0.742 | 464.484 | 0.741 | 459.244 | 23.506 |
| 3 | 0.741 | 465.536 | 0.738 | 459.608 | 23.413 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:07:35] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=31347 acc/draw=11.26 bytes/f[buf=312538.00 tex=256157.10 ubog=1813.60 ubon=0.00 vtxc=1721.40 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=21 box=20 rect=1 jobs=24] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2785 mpm=1890/895 mdt=1573/1212]
[23:07:36] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=2523 acc/draw=14.25 bytes/f[buf=9404216.00 tex=612010.67 ubog=22933.33 ubon=0.00 vtxc=29592.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=2 box=2 rect=0 jobs=2] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.787 | 472.421 | 0.785 | 467.040 | 24.476 |
| 2 | 0.789 | 466.818 | 0.788 | 461.340 | 23.205 |
| 3 | 0.795 | 465.443 | 0.793 | 460.580 | 23.704 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:08:15] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=31347 acc/draw=11.26 bytes/f[buf=312538.00 tex=256157.10 ubog=1813.60 ubon=0.00 vtxc=1721.40 idxc=0.00 icmd=0.00 pmap=0.00 resid=124.27 csob-blob=0.00] tex[emit=21 box=20 rect=1 jobs=24] cso[csom=22 csob=1796 mpr=0] emit[fbe=496 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2785 mpm=1890/895 mdt=1573/1212]
[23:08:16] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=2523 acc/draw=14.25 bytes/f[buf=9404216.00 tex=612010.67 ubog=22933.33 ubon=0.00 vtxc=29592.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=370.67 csob-blob=0.00] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=1 csob=122 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96]
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / split
FAILED rc=1

```text
09-16 23:09:19.850  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 23:09:19.850  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 23:09:19.851  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.855  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=208, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2211440, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7120080}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.855  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=204, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2163417, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7000916}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.873  3154  3294 I libc    : kill: send 9 to pid -21558
09-16 23:09:19.873  3154  3294 I libc    : debug pid reuse, real pid -21558
09-16 23:09:19.899  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 23:09:19.940  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.940  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.940  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:19.940  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:20.561  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=216, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2266652, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7358408}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:20.561  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=208, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2211440, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7120080}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:20.561  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=204, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=2163417, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=7000916}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:09:20.578  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 23:09:20.578  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 23:09 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 23:09 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:09 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:09 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 13546
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 23:09 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 23:09 ..
-rw-r--r-- 1 u0_a290 u0_a290   606400 2026-09-16 23:09 alternate-golden.png
-rw-r--r-- 1 u0_a290 u0_a290   602071 2026-09-16 23:09 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 12635537 2026-09-16 23:09 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 62
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 23:09 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 23:09 ..
-rw------- 1 u0_a290 u0_a290 51700 2026-09-16 23:09 mobilegl.log
-rw------- 1 u0_a290 u0_a290   183 2026-09-16 23:09 retrace.log
minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[23:09:19] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=212008960 currentRss=212008960 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[23:09:19] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=212267008 currentRss=212267008 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.787 | 475.862 | 0.787 | 470.840 | 23.709 |
| 2 | 0.796 | 461.005 | 0.795 | 456.610 | 23.739 |
| 3 | 0.790 | 455.803 | 0.789 | 451.247 | 23.377 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:09:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2785 draws/f=23.21 acc=31347 acc/draw=11.26 bytes/f[buf=312538.00 tex=256157.10 ubog=1813.60 ubon=0.00 vtxc=1721.40 idxc=0.00 icmd=0.00 pmap=0.00 resid=124.27 csob-blob=0.00] tex[emit=21 box=20 rect=1 jobs=24] cso[csom=22 csob=1796 mpr=0] emit[fbe=496 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2785 mpm=1890/895 mdt=1573/1212]
[23:10:00] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=123 window=3 draws=177 draws/f=59.00 acc=2523 acc/draw=14.25 bytes/f[buf=9404216.00 tex=612010.67 ubog=22933.33 ubon=0.00 vtxc=29592.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=370.67 csob-blob=0.00] tex[emit=2 box=2 rect=0 jobs=2] cso[csom=1 csob=122 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.313 | 9.380 | 1.316 | 2.398 | 8.402 |
| 2 | 8.311 | 9.716 | 1.288 | 2.380 | 8.352 |
| 3 | 8.314 | 9.573 | 1.320 | 2.504 | 8.319 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:51:07] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=35894 acc/draw=13.73 bytes/f[buf=63389.93 tex=8768.53 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=4 box=4 rect=0 jobs=4] gates[ers=1769/1720 etl=632/2857 eub=632/2857 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:08] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=27263 acc/draw=16.04 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1199/1341 eub=1199/1341 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:09] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=27273 acc/draw=16.03 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2301 etl=1200/1341 eub=1200/1341 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=27273 acc/draw=16.03 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2301 etl=1200/1341 eub=1200/1341 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:14] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:15] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:16] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=27637 acc/draw=15.98 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2329 etl=1200/1369 eub=1200/1369 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:17] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=28833 acc/draw=15.83 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2421 etl=1200/1461 eub=1200/1461 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:18] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=28027 acc/draw=15.93 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2359 etl=1200/1399 eub=1200/1399 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:20] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:21] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:22] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=27273 acc/draw=16.03 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2301 etl=1200/1341 eub=1200/1341 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:24] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:25] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:26] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=27260 acc/draw=16.04 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=240/2300 etl=1200/1340 eub=1200/1340 mfp=0/0 mpm=0/0 mdt=0/0]
[22:51:27] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=10277 acc/draw=15.64 bytes/f[buf=29445.60 tex=0.00 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=81/859 etl=405/535 eub=405/535 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.322 | 9.514 | 1.462 | 2.505 | 8.401 |
| 2 | 8.311 | 9.701 | 1.468 | 2.509 | 8.317 |
| 3 | 8.317 | 9.548 | 1.463 | 2.529 | 8.320 |

Best-repeat aggregate: pmap=0 mpr=1 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:52:14] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=29544 acc/draw=11.30 bytes/f[buf=63389.93 tex=8768.53 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=114.67 csob-blob=1266.33] tex[emit=4 box=4 rect=0 jobs=4] cso[csom=20 csob=1695 mpr=1] emit[fbe=922 sve=3110 sse=4 sie=0 ctu=46 trp=0 rsp=0] gates[ers=1769/1720 etl=345/3144 eub=3488/2 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:15] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=23623 acc/draw=13.90 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=719/1821 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:16] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:17] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1958.80] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:18] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:20] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:21] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:22] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=23939 acc/draw=13.85 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=155.27 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2189 mpr=0] emit[fbe=740 sve=1829 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2329 etl=720/1849 eub=2569/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:24] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=24951 acc/draw=13.70 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=161.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2280 mpr=0] emit[fbe=741 sve=1920 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2421 etl=720/1941 eub=2661/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:25] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=24269 acc/draw=13.80 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=157.27 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2219 mpr=0] emit[fbe=740 sve=1859 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2359 etl=720/1879 eub=2599/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:26] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:27] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:28] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:29] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:30] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:31] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:32] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:33] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:52:34] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=8880 acc/draw=13.52 bytes/f[buf=29445.60 tex=0.00 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=2322.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=2 csob=811 mpr=0] emit[fbe=251 sve=689 sse=0 sie=0 ctu=3 trp=0 rsp=0] gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / split
FAILED rc=1

```text
09-16 22:53:48.900  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:53:48.900  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:53:48.900  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:53:48.900  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:53:48.901  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=108, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1286159, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4140980}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:48.901  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=104, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1193975, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4021816}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:48.924  3154  3294 I libc    : kill: send 9 to pid -21257
09-16 22:53:48.924  3154  3294 I libc    : debug pid reuse, real pid -21257
09-16 22:53:48.946  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:53:48.980  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=116, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1335699, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4379308}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:48.980  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=116, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1335699, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4379308}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:48.981  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=116, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1335699, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4379308}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:48.981  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=116, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1335699, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4379308}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:49.595  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=116, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1335699, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4379308}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:49.596  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=108, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1286159, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4140980}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:49.596  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=104, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1193975, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4021816}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:53:49.618  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 22:53:49.618  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:53 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:53 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:53 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:53 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 30438
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:53 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:53 ..
-rw-r--r-- 1 u0_a290 u0_a290   174433 2026-09-16 22:53 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 30953189 2026-09-16 22:53 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 86
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:53 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:53 ..
-rw------- 1 u0_a290 u0_a290 74416 2026-09-16 22:53 mobilegl.log
-rw------- 1 u0_a290 u0_a290   981 2026-09-16 22:53 retrace.log
minecraft-1.21.4-fabric-sodium-in-world / DirectGLES run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:53:48] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=214331392 currentRss=214331392 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:53:48] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=214708224 currentRss=214708224 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.303 | 9.486 | 1.478 | 2.560 | 8.402 |
| 2 | 8.311 | 9.738 | 1.478 | 2.582 | 8.403 |
| 3 | 8.306 | 9.658 | 1.477 | 2.566 | 8.403 |

Best-repeat aggregate: pmap=0 mpr=1 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:54:09] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=29544 acc/draw=11.30 bytes/f[buf=63389.93 tex=8768.53 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=114.67 csob-blob=1266.33] tex[emit=4 box=4 rect=0 jobs=4] cso[csom=20 csob=1695 mpr=1] emit[fbe=922 sve=3110 sse=4 sie=0 ctu=46 trp=0 rsp=0] gates[ers=1769/1720 etl=345/3144 eub=3488/2 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=23623 acc/draw=13.90 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=719/1821 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1958.80] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:14] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:15] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:16] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:17] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:18] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=23939 acc/draw=13.85 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=155.27 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2189 mpr=0] emit[fbe=740 sve=1829 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2329 etl=720/1849 eub=2569/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=24951 acc/draw=13.70 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=161.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2280 mpr=0] emit[fbe=741 sve=1920 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2421 etl=720/1941 eub=2661/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:20] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=24269 acc/draw=13.80 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=157.27 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2219 mpr=0] emit[fbe=740 sve=1859 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2359 etl=720/1879 eub=2599/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:21] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:22] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:24] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=23631 acc/draw=13.89 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=1960.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=741 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2301 etl=720/1821 eub=2541/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:25] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:26] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:27] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:28] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=23620 acc/draw=13.89 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=1960.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=740 sve=1800 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=240/2300 etl=720/1820 eub=2540/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:54:28] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=8880 acc/draw=13.52 bytes/f[buf=29445.60 tex=0.00 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=2322.40] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=2 csob=811 mpr=0] emit[fbe=251 sve=689 sse=0 sie=0 ctu=3 trp=0 rsp=0] gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.508 | 1.180 | 0.481 | 1.136 | 0.640 |
| 2 | 0.508 | 1.145 | 0.475 | 1.145 | 0.632 |
| 3 | 0.510 | 1.145 | 0.477 | 1.145 | 0.634 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=28449 acc/draw=10.88 bytes/f[buf=203512.73 tex=268794.70 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=25 box=24 rect=1 jobs=28] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2615 mpm=1839/776 mdt=1496/1119]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=34438 acc/draw=19.92 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1729 mpm=240/1489 mdt=0/1729]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=36462 acc/draw=20.02 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1821 mpm=240/1581 mdt=0/1821]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=35098 acc/draw=19.95 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1759 mpm=240/1519 mdt=0/1759]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:42] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:55:43] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:43] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:43] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:43] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=0 box=0 rect=0 jobs=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:55:43] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=12819 acc/draw=19.51 bytes/f[buf=29445.60 tex=1228.80 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=3 box=3 rect=0 jobs=3] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.538 | 1.201 | 0.507 | 1.201 | 0.659 |
| 2 | 0.533 | 1.184 | 0.503 | 1.147 | 0.654 |
| 3 | 0.538 | 1.149 | 0.508 | 1.149 | 0.660 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=28449 acc/draw=10.88 bytes/f[buf=203512.73 tex=268794.70 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=114.67 csob-blob=0.00] tex[emit=25 box=24 rect=1 jobs=28] cso[csom=20 csob=1695 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2615 mpm=1839/776 mdt=1496/1119]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=34438 acc/draw=19.92 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=155.27 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2189 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1729 mpm=240/1489 mdt=0/1729]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=36462 acc/draw=20.02 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=161.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2280 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1821 mpm=240/1581 mdt=0/1821]
[22:56:12] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=35098 acc/draw=19.95 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=157.27 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2219 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1759 mpm=240/1519 mdt=0/1759]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:56:13] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=12819 acc/draw=19.51 bytes/f[buf=29445.60 tex=1228.80 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=0.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=2 csob=811 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657]
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / split
FAILED rc=1

```text
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:57:09.327  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:57:09.331  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=136, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1536133, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4975128}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.336  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=128, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1485494, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4736800}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.336  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=124, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1455335, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4617636}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.352  3154  3294 I libc    : kill: send 9 to pid -26820
09-16 22:57:09.352  3154  3294 I libc    : debug pid reuse, real pid -26820
09-16 22:57:09.378  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:57:09.404  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=136, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1536133, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4975128}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.404  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=136, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1536133, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4975128}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.404  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=136, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1536133, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4975128}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:57:09.404  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=136, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1536133, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=4975128}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:57 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:57 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:57 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:57 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 30438
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:57 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:57 ..
-rw-r--r-- 1 u0_a290 u0_a290   174433 2026-09-16 22:57 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 30953189 2026-09-16 22:57 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 62
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:57 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:57 ..
-rw------- 1 u0_a290 u0_a290 51700 2026-09-16 22:57 mobilegl.log
-rw------- 1 u0_a290 u0_a290   183 2026-09-16 22:57 retrace.log
minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:57:08] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=211255296 currentRss=211255296 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:57:08] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=211509248 currentRss=211509248 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.536 | 1.191 | 0.510 | 1.191 | 0.664 |
| 2 | 0.540 | 1.148 | 0.508 | 1.148 | 0.660 |
| 3 | 0.534 | 1.167 | 0.507 | 1.152 | 0.658 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2615 draws/f=21.79 acc=28449 acc/draw=10.88 bytes/f[buf=203512.73 tex=268794.70 ubog=1197.33 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=114.67 csob-blob=0.00] tex[emit=25 box=24 rect=1 jobs=28] cso[csom=20 csob=1695 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2615 mpm=1839/776 mdt=1496/1119]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=20148.80 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=16154.27 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16067.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=600 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16232.27 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=720 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18288.80 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=840 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17581.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=960 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=18222.67 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1080 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16697.07 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1200 window=120 draws=1729 draws/f=14.41 acc=34438 acc/draw=19.92 bytes/f[buf=16593.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=155.27 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2189 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1729 mpm=240/1489 mdt=0/1729]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1320 window=120 draws=1821 draws/f=15.18 acc=36462 acc/draw=20.02 bytes/f[buf=17183.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=161.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2280 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1821 mpm=240/1581 mdt=0/1821]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1440 window=120 draws=1759 draws/f=14.66 acc=35098 acc/draw=19.95 bytes/f[buf=17231.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=157.27 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2219 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1759 mpm=240/1519 mdt=0/1759]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1560 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17414.40 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1680 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17420.00 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1800 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16061.47 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=1920 window=120 draws=1701 draws/f=14.18 acc=33822 acc/draw=19.88 bytes/f[buf=18121.73 tex=0.00 ubog=1960.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.40 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1701 mpm=240/1461 mdt=0/1701]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2040 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19013.87 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2160 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=19412.13 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2280 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=16085.73 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2400 window=120 draws=1700 draws/f=14.17 acc=33800 acc/draw=19.88 bytes/f[buf=17211.33 tex=0.00 ubog=1960.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=153.33 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=2160 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/1700 mpm=240/1460 mdt=0/1700]
[22:57:49] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=2440 window=40 draws=657 draws/f=16.43 acc=12819 acc/draw=19.51 bytes/f[buf=29445.60 tex=1228.80 ubog=2322.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=0.00] tex[emit=3 box=3 rect=0 jobs=3] cso[csom=2 csob=811 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657]
```

### minecraft-1.21.4-in-world / DirectGLES / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.291 | 9.978 | 2.365 | 5.235 | 8.314 |
| 2 | 8.314 | 9.925 | 2.351 | 5.238 | 8.316 |
| 3 | 8.336 | 9.946 | 2.352 | 5.226 | 8.317 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:58:20] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=38118 acc/draw=13.31 bytes/f[buf=45847.20 tex=57447.47 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=15 box=15 rect=0 jobs=15] gates[ers=1982/1764 etl=698/3048 eub=709/3037 mfp=0/0 mpm=0/0 mdt=0/0]
[22:58:21] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=95717 acc/draw=9.15 bytes/f[buf=27744.43 tex=635034.53 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=119 box=119 rect=0 jobs=119] gates[ers=9284/2017 etl=10316/985 eub=10477/824 mfp=0/0 mpm=0/0 mdt=0/0]
[22:58:22] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=102034 acc/draw=9.28 bytes/f[buf=13500.13 tex=635225.27 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=185 box=185 rect=0 jobs=185] gates[ers=9257/2577 etl=10538/1296 eub=10720/1114 mfp=0/0 mpm=0/0 mdt=0/0]
[22:58:23] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=103653 acc/draw=9.32 bytes/f[buf=16727.60 tex=643683.33 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=186 box=186 rect=0 jobs=186] gates[ers=9240/2721 etl=10560/1401 eub=10680/1281 mfp=0/0 mpm=0/0 mdt=0/0]
[22:58:24] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=68434 acc/draw=9.40 bytes/f[buf=17197.30 tex=597919.04 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=121 box=121 rect=0 jobs=121] gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-in-world / DirectGLES / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.331 | 10.049 | 2.877 | 5.677 | 8.314 |
| 2 | 8.292 | 10.085 | 2.879 | 5.709 | 8.313 |
| 3 | 8.351 | 10.030 | 2.875 | 5.695 | 8.314 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:59:26] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=31230 acc/draw=10.91 bytes/f[buf=45847.20 tex=57447.47 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=117.60 csob-blob=1454.60] tex[emit=15 box=15 rect=0 jobs=15] cso[csom=19 csob=1745 mpr=0] emit[fbe=943 sve=3296 sse=4 sie=0 ctu=73 trp=0 rsp=0] gates[ers=1982/1764 etl=410/3336 eub=3745/2 mfp=0/0 mpm=0/0 mdt=0/0]
[22:59:27] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=74534 acc/draw=7.12 bytes/f[buf=27744.43 tex=635034.53 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=134.47 csob-blob=16065.60] tex[emit=119 box=119 rect=0 jobs=119] cso[csom=0 csob=1817 mpr=0] emit[fbe=800 sve=1331 sse=0 sie=0 ctu=184 trp=0 rsp=0] gates[ers=9284/2017 etl=9843/1458 eub=11301/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:59:28] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=80424 acc/draw=7.32 bytes/f[buf=13500.13 tex=635225.27 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=16748.40] tex[emit=185 box=185 rect=0 jobs=185] cso[csom=1 csob=2377 mpr=0] emit[fbe=800 sve=1794 sse=0 sie=0 ctu=187 trp=0 rsp=0] gates[ers=9257/2577 etl=9852/1982 eub=11834/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:59:29] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=81891 acc/draw=7.36 bytes/f[buf=16727.60 tex=643683.33 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=181.40 csob-blob=16973.60] tex[emit=186 box=186 rect=0 jobs=186] cso[csom=0 csob=2520 mpr=0] emit[fbe=801 sve=1920 sse=0 sie=0 ctu=186 trp=0 rsp=0] gates[ers=9240/2721 etl=9840/2121 eub=11961/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:59:29] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=54113 acc/draw=7.44 bytes/f[buf=17197.30 tex=597919.04 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=178.31 csob-blob=16016.96] tex[emit=121 box=121 rect=0 jobs=121] cso[csom=2 csob=1719 mpr=0] emit[fbe=570 sve=1323 sse=0 sie=0 ctu=121 trp=0 rsp=0] gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-in-world / DirectGLES / split
FAILED rc=1

```text
09-16 23:00:35.613  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 23:00:35.613  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 23:00:35.613  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 23:00:35.613  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 23:00:35.614  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=148, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1685626, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5332620}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:35.614  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=144, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1629715, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5213456}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:35.634  3154  3294 I libc    : kill: send 9 to pid -32517
09-16 23:00:35.634  3154  3294 I libc    : debug pid reuse, real pid -32517
09-16 23:00:35.663  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 23:00:35.689  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=156, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1742411, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5570948}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:35.689  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=156, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1742411, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5570948}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:35.689  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=156, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1742411, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5570948}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:35.689  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=156, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1742411, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5570948}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:36.305  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=156, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1742411, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5570948}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:36.306  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=148, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1685626, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5332620}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:36.306  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=144, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1629715, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5213456}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:00:36.326  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 23:00:36.326  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 23:00 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 23:00 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:00 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:00 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 47898
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 23:00 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 23:00 ..
-rw-r--r-- 1 u0_a290 u0_a290   296445 2026-09-16 23:00 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 48692730 2026-09-16 23:00 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 86
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 23:00 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 23:00 ..
-rw------- 1 u0_a290 u0_a290 74416 2026-09-16 23:00 mobilegl.log
-rw------- 1 u0_a290 u0_a290   902 2026-09-16 23:00 retrace.log
minecraft-1.21.4-in-world / DirectGLES run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[23:00:35] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=214499328 currentRss=214499328 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[23:00:35] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=214806528 currentRss=214806528 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-in-world / DirectGLES / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.290 | 9.656 | 2.930 | 5.813 | 8.314 |
| 2 | 8.326 | 9.778 | 2.909 | 5.785 | 8.313 |
| 3 | 8.305 | 9.918 | 2.905 | 5.749 | 8.313 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:01:07] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=31230 acc/draw=10.91 bytes/f[buf=45847.20 tex=57447.47 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=117.60 csob-blob=1454.60] tex[emit=15 box=15 rect=0 jobs=15] cso[csom=19 csob=1745 mpr=0] emit[fbe=943 sve=3296 sse=4 sie=0 ctu=73 trp=0 rsp=0] gates[ers=1982/1764 etl=410/3336 eub=3745/2 mfp=0/0 mpm=0/0 mdt=0/0]
[23:01:08] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=74534 acc/draw=7.12 bytes/f[buf=27744.43 tex=635034.53 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=134.47 csob-blob=16065.60] tex[emit=119 box=119 rect=0 jobs=119] cso[csom=0 csob=1817 mpr=0] emit[fbe=800 sve=1331 sse=0 sie=0 ctu=184 trp=0 rsp=0] gates[ers=9284/2017 etl=9843/1458 eub=11301/0 mfp=0/0 mpm=0/0 mdt=0/0]
[23:01:09] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=80424 acc/draw=7.32 bytes/f[buf=13500.13 tex=635225.27 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=16748.40] tex[emit=185 box=185 rect=0 jobs=185] cso[csom=1 csob=2377 mpr=0] emit[fbe=800 sve=1794 sse=0 sie=0 ctu=187 trp=0 rsp=0] gates[ers=9257/2577 etl=9852/1982 eub=11834/0 mfp=0/0 mpm=0/0 mdt=0/0]
[23:01:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=81891 acc/draw=7.36 bytes/f[buf=16727.60 tex=643683.33 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=181.40 csob-blob=16973.60] tex[emit=186 box=186 rect=0 jobs=186] cso[csom=0 csob=2520 mpr=0] emit[fbe=801 sve=1920 sse=0 sie=0 ctu=186 trp=0 rsp=0] gates[ers=9240/2721 etl=9840/2121 eub=11961/0 mfp=0/0 mpm=0/0 mdt=0/0]
[23:01:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=54113 acc/draw=7.44 bytes/f[buf=17197.30 tex=597919.04 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=178.31 csob-blob=16016.96] tex[emit=121 box=121 rect=0 jobs=121] cso[csom=2 csob=1719 mpr=0] emit[fbe=570 sve=1323 sse=0 sie=0 ctu=121 trp=0 rsp=0] gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-in-world / DirectVulkan / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.055 | 2.310 | 1.048 | 2.278 | 1.263 |
| 2 | 1.043 | 2.293 | 1.030 | 2.273 | 1.257 |
| 3 | 1.050 | 2.286 | 1.034 | 2.272 | 1.258 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:01:47] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=30001 acc/draw=10.48 bytes/f[buf=46159.87 tex=261213.90 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=24 box=20 rect=4 jobs=71] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2863 mpm=2085/778 mdt=1710/1153]
[23:01:47] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=83638 acc/draw=8.00 bytes/f[buf=28823.23 tex=41828.13 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=120 box=32 rect=88 jobs=1818] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10461 mpm=9164/1297 mdt=9044/1417]
[23:01:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=94148 acc/draw=8.56 bytes/f[buf=13500.13 tex=39863.47 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=186 box=97 rect=89 jobs=1897] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10994 mpm=9240/1754 mdt=9120/1874]
[23:01:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=96942 acc/draw=8.72 bytes/f[buf=16727.60 tex=40475.87 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=186 box=97 rect=89 jobs=1907] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/11121 mpm=9240/1881 mdt=9120/2001]
[23:01:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=63738 acc/draw=8.76 bytes/f[buf=17197.30 tex=37734.17 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=121 box=64 rect=57 jobs=1224] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350]
```

### minecraft-1.21.4-in-world / DirectVulkan / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.276 | 2.533 | 1.263 | 2.507 | 1.479 |
| 2 | 1.159 | 2.404 | 1.150 | 2.367 | 1.367 |
| 3 | 1.168 | 2.431 | 1.153 | 2.403 | 1.384 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:02:18] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=30001 acc/draw=10.48 bytes/f[buf=46159.87 tex=261213.90 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=117.60 csob-blob=0.00] tex[emit=24 box=20 rect=4 jobs=71] cso[csom=19 csob=1745 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2863 mpm=2085/778 mdt=1710/1153]
[23:02:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=83638 acc/draw=8.00 bytes/f[buf=28823.23 tex=41828.13 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=134.47 csob-blob=0.00] tex[emit=120 box=32 rect=88 jobs=1818] cso[csom=0 csob=1817 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10461 mpm=9164/1297 mdt=9044/1417]
[23:02:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=94148 acc/draw=8.56 bytes/f[buf=13500.13 tex=39863.47 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=0.00] tex[emit=186 box=97 rect=89 jobs=1897] cso[csom=1 csob=2377 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10994 mpm=9240/1754 mdt=9120/1874]
[23:02:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=96942 acc/draw=8.72 bytes/f[buf=16727.60 tex=40475.87 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=181.40 csob-blob=0.00] tex[emit=186 box=97 rect=89 jobs=1907] cso[csom=0 csob=2520 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/11121 mpm=9240/1881 mdt=9120/2001]
[23:02:19] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=63738 acc/draw=8.76 bytes/f[buf=17197.30 tex=37734.17 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=178.31 csob-blob=0.00] tex[emit=121 box=64 rect=57 jobs=1224] cso[csom=2 csob=1719 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350]
```

### minecraft-1.21.4-in-world / DirectVulkan / split
FAILED rc=1

```text
09-16 23:03:07.776  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 23:03:07.776  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 23:03:07.776  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 23:03:07.776  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 23:03:07.776  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 23:03:07.776  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 23:03:07.793  3154  3294 I libc    : kill: send 9 to pid -9538
09-16 23:03:07.793  3154  3294 I libc    : debug pid reuse, real pid -9538
09-16 23:03:07.818  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 23:03:07.844  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=176, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1894571, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6166768}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:07.844  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=176, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1894571, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6166768}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:07.844  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=176, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1894571, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6166768}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:07.844  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=176, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1894571, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6166768}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:08.459  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=176, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1894571, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=6166768}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:08.459  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=168, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1851651, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5928440}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:08.459  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=164, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1820766, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=5809276}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 23:03:08.478  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 23:03:08.478  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 23:03 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 23:03 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:03 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 23:03 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 47898
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 23:03 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 23:03 ..
-rw-r--r-- 1 u0_a290 u0_a290   296445 2026-09-16 23:03 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 48692730 2026-09-16 23:03 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 62
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 23:03 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 23:03 ..
-rw------- 1 u0_a290 u0_a290 51700 2026-09-16 23:03 mobilegl.log
-rw------- 1 u0_a290 u0_a290   104 2026-09-16 23:03 retrace.log
minecraft-1.21.4-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[23:03:07] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=211566592 currentRss=211566592 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[23:03:07] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=211824640 currentRss=211824640 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-in-world / DirectVulkan / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.176 | 2.490 | 1.171 | 2.451 | 1.399 |
| 2 | 1.179 | 2.426 | 1.167 | 2.408 | 1.386 |
| 3 | 1.185 | 2.453 | 1.175 | 2.416 | 1.393 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[23:03:45] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=2863 draws/f=23.86 acc=30001 acc/draw=10.48 bytes/f[buf=46159.87 tex=261213.90 ubog=1370.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=117.60 csob-blob=0.00] tex[emit=24 box=20 rect=4 jobs=71] cso[csom=19 csob=1745 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/2863 mpm=2085/778 mdt=1710/1153]
[23:03:45] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=10461 draws/f=87.18 acc=83638 acc/draw=8.00 bytes/f[buf=28823.23 tex=41828.13 ubog=16065.60 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=134.47 csob-blob=0.00] tex[emit=120 box=32 rect=88 jobs=1818] cso[csom=0 csob=1817 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10461 mpm=9164/1297 mdt=9044/1417]
[23:03:45] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=360 window=120 draws=10994 draws/f=91.62 acc=94148 acc/draw=8.56 bytes/f[buf=13500.13 tex=39863.47 ubog=16748.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=171.80 csob-blob=0.00] tex[emit=186 box=97 rect=89 jobs=1897] cso[csom=1 csob=2377 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/10994 mpm=9240/1754 mdt=9120/1874]
[23:03:46] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=480 window=120 draws=11121 draws/f=92.68 acc=96942 acc/draw=8.72 bytes/f[buf=16727.60 tex=40475.87 ubog=16976.40 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=181.40 csob-blob=0.00] tex[emit=186 box=97 rect=89 jobs=1907] cso[csom=0 csob=2520 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/11121 mpm=9240/1881 mdt=9120/2001]
[23:03:46] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=563 window=83 draws=7278 draws/f=87.69 acc=63738 acc/draw=8.76 bytes/f[buf=17197.30 tex=37734.17 ubog=16016.96 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=178.31 csob-blob=0.00] tex[emit=121 box=64 rect=57 jobs=1224] cso[csom=2 csob=1719 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=0] gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350]
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / pull
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 8.011 | 21.999 | 7.928 | 21.798 | 8.485 |
| 2 | 7.999 | 22.089 | 7.921 | 21.878 | 8.481 |
| 3 | 7.989 | 21.788 | 7.926 | 21.588 | 8.487 |

Best-repeat aggregate: pmap=0 mpr=None rsp=None; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:40:58] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=1156748 draws/f=9639.57 acc=9384883 acc/draw=8.11 bytes/f[buf=788160.40 tex=5096270.80 ubog=1830535.20 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=855 box=855 rect=0 jobs=855] gates[ers=1145955/19154 etl=1148484/16625 eub=1149352/15757 mfp=0/0 mpm=0/0 mdt=0/0]
[22:40:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=176565 draws/f=1471.38 acc=1426185 acc/draw=8.08 bytes/f[buf=10290.00 tex=357348.93 ubog=281586.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=60 box=60 rect=0 jobs=60] gates[ers=174960/2445 etl=176160/1245 eub=176280/1125 mfp=0/0 mpm=0/0 mdt=0/0]
[22:40:59] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=251 window=11 draws=16185 draws/f=1471.36 acc=130732 acc/draw=8.08 bytes/f[buf=10289.45 tex=342341.82 ubog=281585.45 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=0.00] tex[emit=6 box=6 rect=0 jobs=6] gates[ers=16038/224 etl=16148/114 eub=16159/103 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / push
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 11.111 | 24.841 | 11.014 | 24.623 | 11.599 |
| 2 | 11.117 | 24.880 | 11.027 | 24.703 | 11.612 |
| 3 | 11.105 | 24.865 | 11.011 | 24.686 | 11.595 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:42:10] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=1156748 draws/f=9639.57 acc=7066179 acc/draw=6.11 bytes/f[buf=788160.40 tex=5096270.80 ubog=1830535.20 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=1276.93 csob-blob=1836099.80] tex[emit=855 box=855 rect=0 jobs=855] cso[csom=27 csob=17841 mpr=0] emit[fbe=8307 sve=19118 sse=3 sie=0 ctu=1529 trp=0 rsp=0] gates[ers=1145955/19154 etl=1144429/20680 eub=1165109/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:42:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=176565 draws/f=1471.38 acc=1072815 acc/draw=6.08 bytes/f[buf=10290.00 tex=357348.93 ubog=281586.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=163.00 csob-blob=281586.00] tex[emit=60 box=60 rect=0 jobs=60] cso[csom=0 csob=2280 mpr=0] emit[fbe=765 sve=1560 sse=0 sie=0 ctu=105 trp=0 rsp=0] gates[ers=174960/2445 etl=175680/1725 eub=177405/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:42:11] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=251 window=11 draws=16185 draws/f=1471.36 acc=98340 acc/draw=6.08 bytes/f[buf=10289.45 tex=342341.82 ubog=281585.45 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=162.91 csob-blob=281585.45] tex[emit=6 box=6 rect=0 jobs=6] cso[csom=0 csob=209 mpr=0] emit[fbe=70 sve=143 sse=0 sie=0 ctu=10 trp=0 rsp=0] gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / split
FAILED rc=1

```text
09-16 22:43:03.059  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:43:03.059  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:43:03.059  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:43:03.059  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:43:03.059  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:43:03.059  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:43:03.063  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=64, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=689861, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2830176}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.067  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=56, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=638494, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2591848}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.067  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=52, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=578959, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2472684}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.070  3154  3294 I libc    : kill: send 9 to pid -31815
09-16 22:43:03.070  3154  3294 I libc    : debug pid reuse, real pid -31815
09-16 22:43:03.086  3154  3294 I libc    : kill: send 9 to pid -31815
09-16 22:43:03.086  3154  3294 I libc    : debug pid reuse, real pid -31815
09-16 22:43:03.097  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:43:03.123  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=64, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=689861, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2830176}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.123  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=64, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=689861, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2830176}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.123  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=64, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=689861, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2830176}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:43:03.124  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=64, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=689861, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2830176}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:43 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:43 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:43 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:43 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 15254
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:43 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:43 ..
-rw-r--r-- 1 u0_a290 u0_a290   487045 2026-09-16 22:43 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 15108006 2026-09-16 22:43 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 50
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:43 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:43 ..
-rw------- 1 u0_a290 u0_a290 40934 2026-09-16 22:43 mobilegl.log
-rw------- 1 u0_a290 u0_a290   901 2026-09-16 22:43 retrace.log
minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:43:02] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=215199744 currentRss=215199744 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:43:02] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=215511040 currentRss=215511040 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / splitctl
| repeat | frame p50 | frame p99 | CPU p50 | CPU p99 | mean frame |
|---:|---:|---:|---:|---:|---:|
| 1 | 11.298 | 24.896 | 11.208 | 24.714 | 11.786 |
| 2 | 11.261 | 24.771 | 11.168 | 24.575 | 11.739 |
| 3 | 11.297 | 24.966 | 11.210 | 24.757 | 11.786 |

Best-repeat aggregate: pmap=0 mpr=0 rsp=0; peakRSS=- MiB, currentRSS=- MiB, allRolesMapped=- MiB.

```text
[22:43:47] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=120 window=120 draws=1156748 draws/f=9639.57 acc=7066179 acc/draw=6.11 bytes/f[buf=788160.40 tex=5096270.80 ubog=1830535.20 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=1276.93 csob-blob=1836099.80] tex[emit=855 box=855 rect=0 jobs=855] cso[csom=27 csob=17841 mpr=0] emit[fbe=8307 sve=19118 sse=3 sie=0 ctu=1529 trp=0 rsp=0] gates[ers=1145955/19154 etl=1144429/20680 eub=1165109/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:43:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=240 window=120 draws=176565 draws/f=1471.38 acc=1072815 acc/draw=6.08 bytes/f[buf=10290.00 tex=357348.93 ubog=281586.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=163.00 csob-blob=281586.00] tex[emit=60 box=60 rect=0 jobs=60] cso[csom=0 csob=2280 mpr=0] emit[fbe=765 sve=1560 sse=0 sie=0 ctu=105 trp=0 rsp=0] gates[ers=174960/2445 etl=175680/1725 eub=177405/0 mfp=0/0 mpm=0/0 mdt=0/0]
[22:43:48] [Android MobileGLTraceRe/INFO]: MGPipe stats: frames=251 window=11 draws=16185 draws/f=1471.36 acc=98340 acc/draw=6.08 bytes/f[buf=10289.45 tex=342341.82 ubog=281585.45 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=162.91 csob-blob=281585.45] tex[emit=6 box=6 rect=0 jobs=6] cso[csom=0 csob=209 mpr=0] emit[fbe=70 sve=143 sse=0 sie=0 ctu=10 trp=0 rsp=0] gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0]
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / pull
FAILED rc=1

```text
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (scudo::SizeClassAllocator<scudo::PrimaryConfig<scudo::AndroidNormalConfig>>::popBlocks(scudo::SizeClassAllocatorLocalCache<scudo::SizeClassAllocator<scudo::PrimaryConfig<scudo::AndroidNormalConfig>>>*, unsigned long, unsigned int*, unsigned short)+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (scudo::SizeClassAllocatorLocalCache<scudo::SizeClassAllocator<scudo::PrimaryConfig<scudo::AndroidNormalConfig>>>::refill(scudo::SizeClassAllocatorLocalCache<scudo::SizeClassAllocator<scudo::PrimaryConfig<scudo::AndroidNormalConfig>>>::PerClass*, unsigned long, unsigned short)+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (scudo::Allocator<scudo::AndroidNormalConfig, &scudo_malloc_postinit>::allocate(unsigned long, scudo::Chunk::Origin, unsigned long, bool)+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (scudo_calloc+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (calloc+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:45:37.917  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:45:38.150  3154  3294 I libc    : kill: send 9 to pid -5817
09-16 22:45:38.151  3154  3294 I libc    : debug pid reuse, real pid -5817
09-16 22:45:38.178  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5pull.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a275 u0_a275 3452 2026-09-16 22:45 .
drwxrwxrwx 3 u0_a275 u0_a275 3452 2026-09-16 22:45 ..
drwxrwxrwx 2 u0_a275 u0_a275 3452 2026-09-16 22:45 input
drwxrwxrwx 2 u0_a275 u0_a275 3452 2026-09-16 22:45 output

/data/user/0/top.mobilegl.plugin.p5pull.trace/files/trace-replay/input:
total 15254
drwxrwxrwx 2 u0_a275 u0_a275     3452 2026-09-16 22:45 .
drwxrwxrwx 4 u0_a275 u0_a275     3452 2026-09-16 22:45 ..
-rw-r--r-- 1 u0_a275 u0_a275   487045 2026-09-16 22:45 golden.png
-rw-r--r-- 1 u0_a275 u0_a275 15108006 2026-09-16 22:45 trace.trace

/data/user/0/top.mobilegl.plugin.p5pull.trace/files/trace-replay/output:
total 38
drwxrwxrwx 2 u0_a275 u0_a275  3452 2026-09-16 22:45 .
drwxrwxrwx 4 u0_a275 u0_a275  3452 2026-09-16 22:45 ..
-rw------- 1 u0_a275 u0_a275 26878 2026-09-16 22:45 mobilegl.log
-rw------- 1 u0_a275 u0_a275   107 2026-09-16 22:45 retrace.log
minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / push
FAILED rc=1

```text
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:47:07.912  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:47:07.914  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=76, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=844705, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3187668}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:47:07.914  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=68, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=752028, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=2949340}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:47:08.232  3154  3294 I libc    : kill: send 9 to pid -9666
09-16 22:47:08.233  3154  3294 I libc    : debug pid reuse, real pid -9666
09-16 22:47:08.263  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:47:08.306  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:47:08.306  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:47:08.307  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:47:08.307  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5push.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a283 u0_a283 3452 2026-09-16 22:46 .
drwxrwxrwx 3 u0_a283 u0_a283 3452 2026-09-16 22:46 ..
drwxrwxrwx 2 u0_a283 u0_a283 3452 2026-09-16 22:47 input
drwxrwxrwx 2 u0_a283 u0_a283 3452 2026-09-16 22:47 output

/data/user/0/top.mobilegl.plugin.p5push.trace/files/trace-replay/input:
total 15254
drwxrwxrwx 2 u0_a283 u0_a283     3452 2026-09-16 22:47 .
drwxrwxrwx 4 u0_a283 u0_a283     3452 2026-09-16 22:46 ..
-rw-r--r-- 1 u0_a283 u0_a283   487045 2026-09-16 22:47 golden.png
-rw-r--r-- 1 u0_a283 u0_a283 15108006 2026-09-16 22:46 trace.trace

/data/user/0/top.mobilegl.plugin.p5push.trace/files/trace-replay/output:
total 38
drwxrwxrwx 2 u0_a283 u0_a283  3452 2026-09-16 22:47 .
drwxrwxrwx 4 u0_a283 u0_a283  3452 2026-09-16 22:46 ..
-rw------- 1 u0_a283 u0_a283 27359 2026-09-16 22:47 mobilegl.log
-rw------- 1 u0_a283 u0_a283   107 2026-09-16 22:47 retrace.log
minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / split
FAILED rc=1

```text
09-16 22:48:00.245  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:48:00.245  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:48:00.245  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:48:00.245  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:48:00.246  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.246  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=76, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=844705, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3187668}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.272  3154  3294 I libc    : kill: send 9 to pid -12040
09-16 22:48:00.272  3154  3294 I libc    : debug pid reuse, real pid -12040
09-16 22:48:00.297  2029  2029 W libc    : Access denied finding property "vendor.gpp.create_frc_extension"
09-16 22:48:00.331  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task before=  [TaskKey{id=92, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=987043, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3664324}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.331  4354  4354 I WindowAnimParamsProvider: getClosingWindowAnimParams getClosingShortcutIcon task after=  [TaskKey{id=92, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=987043, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3664324}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.331  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task before=  [TaskKey{id=92, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=987043, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3664324}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.331  4354  4354 I WindowAnimParamsProvider: getUserIdFromRemoteTarget task after=  [TaskKey{id=92, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=987043, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3664324}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.948  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=92, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=987043, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3664324}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.949  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.949  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=76, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=844705, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3187668}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:48:00.968  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
09-16 22:48:00.968  3154  4324 W libc    : Access denied finding property "vendor.video.jpegopt.enable"
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:47 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:47 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:47 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:47 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 15254
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:47 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:47 ..
-rw-r--r-- 1 u0_a290 u0_a290   487045 2026-09-16 22:47 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 15108006 2026-09-16 22:47 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 42
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:47 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:47 ..
-rw------- 1 u0_a290 u0_a290 29313 2026-09-16 22:47 mobilegl.log
-rw------- 1 u0_a290 u0_a290   103 2026-09-16 22:47 retrace.log
minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```

Startup RSS/segment ledger before failure:

```text
[22:47:59] [Android MobileGLTraceRe/INFO]: MG_Remote memory[accept/server]: peakRss=211709952 currentRss=211709952 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[22:47:59] [Android MobileGLTraceRe/INFO]: MG_Remote memory[handshake/client]: peakRss=212021248 currentRss=212021248 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

### minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / splitctl
FAILED rc=1

```text
09-16 22:49:20.922 15430 15430 F DEBUG   :       #38 pc 0000000000082d74  /apex/com.android.runtime/lib64/bionic/libc.so (__pthread_start(void*)+184) (BuildId: 223a572c9f034c6a9337d8b3bedc75f3)
09-16 22:49:20.922 15430 15430 F DEBUG   :       #39 pc 00000000000751f0  /apex/com.android.runtime/lib64/bionic/libc.so (__start_thread+68) (BuildId: 223a572c9f034c6a9337d8b3bedc75f3)
09-16 22:49:20.966 15430 15430 I libc    : tombstoned_notify_completion, socket: 8
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libMobileGL.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_apitrace_main+) ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (mobilegl_trace::RunTraceReplay(mobilegl_trace::Request const&)+) ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  XXlib/arm/libtrace_replay_runner.so (Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay+) ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__pthread_start(void*)+) ()
09-16 22:49:20.981  6756  7080 D DigestGenerator:  /apex/com.android.runtime/lib/bionic/libc.so (__start_thread+) ()
09-16 22:49:20.983  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=100, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5split.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=1067784, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3902652}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:49:20.990  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=84, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5push.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=934707, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3425996}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
09-16 22:49:20.990  4354  5792 D RecentsTaskLoader: reloadTasksData [TaskKey{id=76, stackId=0, baseIntent=Intent { act=top.mobilegl.plugin.TRACE_REPLAY flg=0x10000000 cmp=top.mobilegl.plugin.p5pull.trace/top.mobilegl.plugin.trace.TraceReplayActivity }, userId=0, lastActiveTime=844705, windowingMode=1, isThumbnailBlur=false, isAccessLocked=false, isScreening=false, topActivity=null, mHashCode=3187668}, title=MobileGL, titleDescription=MobileGL, bounds=null, isLaunchTarget=false, isStackTask=true, isSystemApp=false, isDockable=true, baseActivity=null, isLocked=false, mNeedHide=false, hasMultipleTasks=false, cti1Key=, cti2Key=, cti1Task=, cti2Task=] 
trace-replay-ci.sh: app trace-replay files:
/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay:
total 12
drwxrwxrwx 4 u0_a290 u0_a290 3452 2026-09-16 22:49 .
drwxrwxrwx 3 u0_a290 u0_a290 3452 2026-09-16 22:49 ..
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:49 input
drwxrwxrwx 2 u0_a290 u0_a290 3452 2026-09-16 22:49 output

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/input:
total 15254
drwxrwxrwx 2 u0_a290 u0_a290     3452 2026-09-16 22:49 .
drwxrwxrwx 4 u0_a290 u0_a290     3452 2026-09-16 22:49 ..
-rw-r--r-- 1 u0_a290 u0_a290   487045 2026-09-16 22:49 golden.png
-rw-r--r-- 1 u0_a290 u0_a290 15108006 2026-09-16 22:49 trace.trace

/data/user/0/top.mobilegl.plugin.p5split.trace/files/trace-replay/output:
total 38
drwxrwxrwx 2 u0_a290 u0_a290  3452 2026-09-16 22:49 .
drwxrwxrwx 4 u0_a290 u0_a290  3452 2026-09-16 22:49 ..
-rw------- 1 u0_a290 u0_a290 27359 2026-09-16 22:49 mobilegl.log
-rw------- 1 u0_a290 u0_a290   108 2026-09-16 22:49 retrace.log
minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan run 3/3: FAILED (exit 1)
```
