# Disaggregated-mode device perf harness

A/B/C/D performance matrix and profiling harness for MobileGL's transport arms
(`monolith` / `inproc` / `spawn+shm` / `spawn+tcp`), built for the adb-attached
Android bench device. Written from Git Bash on a Windows host; requires
`MSYS_NO_PATHCONV=1` (the scripts set it themselves).

Prerequisites: the disaggregated trace APK installed
(`./gradlew -p android-plugin :app:assembleTraceRelease -Pmobilegl.buildDisaggregated=ON
-Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON -Pmobilegl.debuggableRelease=true`,
then sign with the debug keystore and `adb install -r`), and trace fixtures under
`tools/trace_replay/fixtures/`.

All scripts honor `SER=<adb serial>` (default `2f7cbe2e`) and
`PERF_WORK=<results root>` (default `<repo>/.trace-work/perf-compare`, git-ignored).

## run_matrix.sh — perf matrix driver

```
bash run_matrix.sh [smoke|prep|openra|rd12|all]
```

Interleaved A/B/C/D rounds (monolith -> inproc -> shm -> tcp), N reps per arm
(`OPENRA_REPS` / `RD12_REPS`, default 3). Interleaving is the substitute for
frequency pinning on unrooted devices. Per run it force-stops the app, clears
stale results, starts/stops the tcp server for the tcp arm, verifies the arm
markers in logcat (`Config: MOBILEGL_TRANSPORT=...`, `spawn ARMED`,
`run-ahead ARMED` — a run without them is invalid), and pulls
`result.json` / `benchmark.json` / PipeStats dumps into
`$PERF_WORK/<workload>/<arm>-r<n>/`.

benchmark mode never snapshots, so ssim stays -1; the matrix adds one
non-benchmark ssim round per workload/arm for the golden gate.

## make_report.py — aggregate into REPORT.md

```
python make_report.py [results_dir]
```

Renders raw per-rep tables, best-of-N aggregates with spread, the ssim gate,
per-run arm verification, and merged client+server PipeStats rollups.
Note the spawn-arm traps: client-side `frames=0` is structural (use the server
dump), and logcat `MGPipe stats:` mixes non-counter lines — trust the JSON dumps.

## profile_matrix.sh + run_profile_batch.sh — profiling lane

Same runner with `PROF=1`: adds simpleperf (`perf_event_paranoid=-1` device)
for the client and server processes, ftrace sched capture for exact wakeup
latencies (shell-writable on this device), and the /proc sampler below.

```
bash run_profile_batch.sh          # the canned batch; edit the run list inside
PROF=1 ONE=tcp WL=rd12 REP=1 bash profile_matrix.sh one   # single arm
```

`ONE` / `WL` / `REP` / `EXTRA_ENV` / `EXTRA_SERVER_ENV` select and parameterize
a single run; e.g. `EXTRA_SERVER_ENV=MOBILEGL_IPC_SPIN_US=2000` for the
deep-idle-exit A/B.

## sampler.c — device-side /proc sampler

`sampler <pid> <duration_s> <interval_ms> <out.csv>`. Per-tid blocked-syscall
histogram (`/proc/<tid>/syscall`) + CPU/switch counters. Exists because strace
attach is SELinux-blocked for shell uid on the bench device, while same-uid
`/proc` reads via `run-as` work. Build for the device with the NDK
(`aarch64-linux-android*-clang`), push, run under `run-as`.

## tcp_path_check.sh — run before any cross-host measurement

Connects from WSL to a closed port on the phone. End to end, the phone refuses
it; if the connect is accepted, something on the host (here v2rayN's `xray_tun`,
WSL's default route) is terminating TCP and relaying it. That fakes sub-ms RTTs,
and on 2026-09-28 it stalled the MobileGL control stream. The stall read as a
server hang at eglMakeCurrent. Exit 0 = end to end, 1 = middlebox,
2 = filtered. Fix: `ip route replace <phone>/32 via <LAN gateway> dev eth0`
as root in WSL (temporary; delete it afterwards).

## fcl_accept.sh — P12 exit gate (a)

FCL's Minecraft renders into the Render Server's on-screen window on the same phone, then
`fcl_accept.sh <backend> kill` kills the server and checks the client latches DEVICE LOST and FCL
survives. Needs the FCL fork's version setting "keepRunningInBackground" ON in `config.json`
(FCL pauses the game the moment its Activity leaves the foreground). The server is started through
the Render Server screen's documented extras. Results: `docs/Disaggregated/notes/p12/FCL-ACCEPTANCE.md`.

## crosshost_accept.sh + crosshost_report.py — P12 exit gate (b)

A WSL client replays the openra and rd12 traces against the phone's render server over
TCP + stream (ssim gate, then `PRESENT_CREDIT` 1/2/3 benchmarks). Adds a temporary /32 route
around the host's TUN proxy, checks the path with `tcp_path_check.sh`, and removes the route and
stops the server on exit. `crosshost_report.py` reduces the output to a table. Results and the
P6.5 link numbers: `docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md`.

## netbench.sh — Wi-Fi TCP throughput baseline

**Understates the link**: the `toybox nc` sink capped at 18-22 MiB/s while the render server
read 52 MB/s on the same Wi-Fi. Use the server's `P65ServerFrame` read rate as the throughput number (enable it with `MOBILEGL_SERVER_FRAME_STATS=1` or `setprop debug.mobilegl.server_frame_stats 1`; it is off by default).

WSL -> phone bulk transfer over `toybox nc` sink, 3 reps; refuses to run
unless `tcp_path_check.sh` passes. The `nc` listener must keep stdin open or it
exits on EOF and RSTs the connection, and `pkill` must be anchored
(`'^toybox nc'`) or it kills the adb shell that is about to start the listener.

## phone_strace.sh — session-child strace

Attaches the device `/system/bin/strace` to the server's session child as the
app uid via `run-as` (shell-uid ptrace is blocked).

## P15 FCL probes: fcl_probe.sh, fcl_count.sh and their reducers

`fcl_probe.sh <name> <lib> <backend> <transport> <outdir> [simpleperf=1]` swaps a library into
FCL (rooted device), launches it, waits for steady in-world frames (MC's `Time elapsed:` line,
then four fps-log windows of more than 150 frames each, relaunching on the known start-up death),
then records:
- 4 s of ftrace: sched, kgsl cmdbatch/waittimestamp, and atrace gfx for the app;
- schedstat and `gpu_clock_stats` bracketing that window;
- with the last argument at 1, 6 s of `simpleperf --trace-offcpu -p <game pid>`.

The caller restores FCL with `fcl_p14.sh restore`. `fcl_count.sh` does the same launch for a
`p15count.patch` build (per-draw (VAO, program) pair and between-draw entry statistics, armed by
`debug.mobilegl.p15count=1` or `MOBILEGL_P15_COUNT=1`, window `MOBILEGL_P15_COUNT_WINDOW`).

Reducers, all reading a probe directory:
- `snapdiff.py`: per-thread CPU and runqueue wait;
- `offwait.py <trace> <tid> <present-slice-regex>`: sleep and runnable time per frame, keyed by the
  atrace slices open at switch-out and the waker, plus GPU busy;
- `sfalign.py`: SurfaceFlinger `setTransactionState` duration and phase against `flushTransactions`;
- `slices.py`: atrace slice totals per thread;
- `fcl_split.py`: layer split, on- vs off-CPU;
- `fcl_incl.py`: inclusive libMobileGL functions;
- `fcl_drvcaller.py`: driver time by entry and MobileGL caller;
- `fcl_stacks.py`: top stacks.

The simpleperf reducers need `SIMPLEPERF_DIR` and a `binary_cache` from `binary_cache_builder.py`.
Findings: `docs/Disaggregated/notes/perf-p15/PLAN-P15.md` section 2.

## Clocks, bench sessions and run validity (P15)

Every performance number carries the device state it was measured under.

- **`pin_clocks.sh`** pins CPU and GPU to a named profile (`PROFILE=sustained|gpumax|gpulow|cpulow|cpuhunt|gpuhunt`,
  see the script header; `sustained` is the default) and records it on the device.
- **`bench_session.sh start|stop`**:
  - `start` records, then stops, the services that move frequency caps (`thermal-engine`, the QTI perf HAL,
    `vendor.perfservice`, Lenovo's `performance` and `hyperschedule_hal_service`). It releases their leftover
    requests in `/sys/kernel/msm_performance/parameters/cpu_{max,min}_freq`, which otherwise clamp
    `scaling_max_freq` under any pin, and then pins.
  - `stop` restarts every service that was running and verifies it is running again.
  - Wrap every device job so `stop` always runs (trap on EXIT/INT/TERM).
- **`freq_sampler.sh`** runs on the device for each measurement window and samples every 2 s: CPU cur/max per
  policy, GPU cur, kgsl thermal level, busy and temperatures.
- **`freqcheck.py`** decides VALID/INVALID against the intended pin (one OPP of tolerance, GPU checked while
  busy). Invalid runs are discarded and re-run after cool-down.
- **`devstate.sh <outdir> [trace]`** writes the per-run `state.txt` line: profile and clocks, validity with the
  observed min/max frequencies and peak temperatures, daemons, MC version, swap interval and anland on/off.
  `fcl_probe.sh` and `matrix_p14.sh` (benchmark mode) call it.
