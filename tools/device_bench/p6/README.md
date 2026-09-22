# `device_bench/p6` — the device A/B session runner

Two scripts that run a **paired performance comparison on a real device** and turn its evidence into
markdown tables. They exist because the quantity a disaggregation A/B reads on (tenths of a
millisecond of client CPU per frame) is smaller than the drift a hand-driven session accumulates
between the CPU samples, the pin evidence and the library's own counters — and those three come from
three different mechanisms. Collected by hand they are minutes apart; collected by one process they
are one window.

| file | role |
|---|---|
| `ab_session.py` | collects: reboot, pin, launch each arm in order, sample per-thread CPU, pull role logs, verify the arm |
| `render_ab.py` | renders: `session-summary.json` → the markdown tables a report needs |

The split is deliberate. The renderer reads only what the runner recorded, so it cannot influence
what was collected.

## Usage

```bash
# One arm is TRANSPORT[:BACKEND[:CASE[:REPEATS]]]; repeat a spec to interleave it.
python tools/device_bench/p6/ab_session.py \
  --tree  /c/Users/me/mobilegl-snapshot \
  --out   /c/Users/me/evidence \
  --apk   /c/Users/me/evidence/trace-debug-signed.apk \
  --package top.mobilegl.plugin.trace \
  --reboot --fan-level 2 --repeats 3 --session my-session \
  --arm monolith:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm inproc:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3 \
  --arm spawn:DirectGLES:minecraft-1.21.4-rd12-odinlite-in-world:3

python tools/device_bench/p6/render_ab.py /c/Users/me/evidence/my-session/session-summary.json
```

`--tree` is the tree the APK and the runner come from. Point it at a **snapshot**, not the shared
tree, when anything else is editing the repository: a build that picks up a half-written source file
measures nothing, and the failure is invisible in the result. `--apk` must be signed with a key the
device already trusts for that package — the Gradle release build has no signing config locally
(`SIGNING_*` are CI secrets), so it has to be signed by hand first:

```bash
apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android \
  --ks-key-alias androiddebugkey --key-pass pass:android --out signed.apk unsigned.apk
```

`--package` must match the APK's application id. A development build usually needs a suffix
(`-Pmobilegl.applicationIdSuffix=.mine`) so it can sit **beside** an existing install rather than
replacing it: two APKs signed by different keys cannot share an id, and `adb install -r` fails with
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`. The runner then needs `MOBILEGL_TRACE_PACKAGE` set to the same
id (see `run_android_retrace_local.py`'s note beside `BACKENDS`), which `ab_session.py` does itself.

## What the protocol guarantees

The whole point of the script is that these are not optional:

- **Reboot-clean.** `--reboot` reboots, waits for `sys.boot_completed`, and compares the boot id
  before and after. An unchanged id means no reboot happened, which is reported as a failure rather
  than recorded as a clean window.
- **Pinned frequencies, with evidence.** Every arm runs `pin_device.sh <serial> pin` before and
  `... check` after; a non-`PINNED` exit voids the arm (`status: UNPROVEN`), it does not degrade it.
  Un-pinned per-thread CPU is not comparable across arms.
- **One thermal window.** All arms in one session, interleaved by the order the `--arm` flags are
  given, with a cool-down between them so a later arm is not measured hotter than an earlier one.
- **Arm proof.** Each arm is checked against the library's OWN private log: the transport marker, the
  split knobs in the `Config: IPC` line, and for `spawn` the `spawn ARMED - the server role runs in
  pid N` line — which only the client writes after launch, connect and handshake all succeeded. A
  spawn lane that silently fell back to monolith passes every other check and fails this one.

## Evidence layout

```
<out>/<session>/
  session-summary.json          # session metadata: boot ids, fan, APK hash, per-arm summaries
  <NN>-<case>-<backend>-<transport>-r<N>/
    arm-summary.json            # this arm: status, problems, cpu, stats, benchmark runs
    pin-before.txt pin-after.txt
    mobilegl.client.log mobilegl.server.log   # per role
    benchmark-run1..N.json      # per-frame wall + CPU series per repeat
    benchmark.json result.json logcat.txt runner.stdout.txt runner.stderr.txt
```

### Two asymmetries of the `spawn` shape that the renderer handles

Both are structural — the client and the server really are different processes — and both produce a
zero that looks like a measurement if read carelessly:

1. **`srv`/`srvpark` live in the server log.** The apply thread publishes them into its own process's
   PipeStats. Under `inproc` both roles are threads of one process, so the client's summary line
   carries all four numbers; under `spawn` the client prints `srv=0 srvpark=0`, which means "another
   process", not "never waited". `render_ab.py` reads the server pair from `mobilegl.server.log`.

2. **The frame count lives in the server log too.** `frames=` is bumped by `PipeStats::OnPresent`,
   which the backend calls on Present — and under `spawn` present is applied in the server process,
   so the client's `frames=0 window=0`. That zero is the same shape as `srv=0`. The per-frame rates
   therefore divide by whichever log counted the frames, and the rendered table names which one that
   was.

## Building the APK

```bash
gradle --no-daemon -p <tree>/android-plugin :app:assembleTraceRelease \
  -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON \
  -Pmobilegl.pipePush=ON -Pmobilegl.debuggableRelease=true \
  -Pmobilegl.applicationIdSuffix=.mine -Pmobilegl.apkSuffix=mine
```

`-Pmobilegl.debuggableRelease=true` is not optional: without it the release APK is not `run-as`-able,
`trace-replay-ci.sh` cannot pull the private logs, and every arm produces no library log — which is
indistinguishable from a run that produced none.

## Pitfalls this tooling works around

- **`--transport` is ignored in `--benchmark` mode.** `run_android_retrace_local.py` applies
  `--transport` only on its `run_case` path; `run_benchmark_case` passes `args.env` alone. So
  `--benchmark --transport spawn` runs monolith under a spawn label. This script passes the transport
  as an explicit `--env MOBILEGL_TRANSPORT=…` and verifies it from the library's log.
- **`MOBILEGL_RETRACE_USE_PBUFFER` must be in the CI script's environment**, not passed as `--env` —
  `trace-replay-ci.sh` reads it itself to decide the surface shape. Without it the `spawn` arm dies
  with `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`, because an `ANativeWindow*` means
  nothing in the server's process (Rule H).
- **`MOBILEGL_TRACE_SKIP_INSTALL=1`** keeps the script's own `adb install` out of the window. The
  session installs once itself and waits for `dex2oat` to exit first: an `am_kill … due to
  installPackageLI` landing inside an arm measures the installer.
- **A stale `benchmark.json` is worse than none.** The runner's result directory is keyed by case and
  backend, not by arm, so a failed arm can leave the previous arm's file where `read_benchmark` will
  pick it up. The directory is deleted before each arm.

## Known limit

Under `spawn`, the client's **windowed** fields (`frames=`, `window=`, `draws/f`, `wrec/f`,
`bytes/f[…]`) are structurally zero: `PipeStats::OnPresent` runs in the server process (the apply
thread applies the present record), so the spawn client never counts frames. Dividing any counter
by the client's `frames=` is undefined. The renderer therefore takes the frame denominator from the
log that actually counted frames (normally the server's), and names the source in the table.

What is **valid** from the client log is the run-total gauges (`cli`, `clipark`, `maxrec`,
`ringwraps`, …): `PublishGauge` is a store, not an add, so the client's terminal `Shutdown()` line
reports true run totals. Symmetrically, the server log's `cli=0` and the client log's `srv=0` mean
"the other process", not "never waited".

The server's own byte/record/wait classes are available since `PipeStats::Init()` runs in
`ServerMain.cpp` (P6 closeout; before that the server log carried no summary line at all and `srv`
had to be reported as unavailable).
