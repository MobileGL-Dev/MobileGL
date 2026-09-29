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

## netbench.sh — Wi-Fi TCP throughput baseline

WSL -> phone bulk transfer over `toybox nc` sink, 3 reps; refuses to run
unless `tcp_path_check.sh` passes. The `nc` listener must keep stdin open or it
exits on EOF and RSTs the connection, and `pkill` must be anchored
(`'^toybox nc'`) or it kills the adb shell that is about to start the listener.

## phone_strace.sh — session-child strace

Attaches the device `/system/bin/strace` to the server's session child as the
app uid via `run-as` (shell-uid ptrace is blocked).
