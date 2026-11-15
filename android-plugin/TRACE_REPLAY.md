# MobileGL trace replay APK

The Android plugin app has two flavor dimensions:

- `backend`: `espryt` uses `DirectGLES`, `magma` uses `DirectVulkan`.
- `profile`: `plugin` keeps the original FCL plugin APK behavior, `trace` adds the standalone trace runner Activity.

Useful debug builds:

```sh
./gradlew -p MobileGL/android-plugin :app:assembleEsprytPluginDebug
./gradlew -p MobileGL/android-plugin :app:assembleEsprytTraceDebug
./gradlew -p MobileGL/android-plugin :app:assembleMagmaTraceDebug
```

The trace profile keeps the existing plugin manifest metadata and adds:

```text
action: top.mobilegl.plugin.TRACE_REPLAY
activity: top.mobilegl.plugin.trace.TraceReplayActivity
```

Intent extras:

```text
trace_path    absolute path to the apitrace file
golden_path   optional absolute path to a golden PNG
output_dir    directory for result.json and actual.png
diff_path     optional absolute path for a golden-difference PNG
backend       DirectGLES or DirectVulkan; defaults to the backend flavor
target_frame  target frame index, or -1
target_call   target call number, or -1
width         optional replay surface width override
height        optional replay surface height override
ssim_threshold minimum SSIM required to pass; default is 0.99
crop_x        optional compare crop x
crop_y        optional compare crop y
crop_width    optional compare crop width
crop_height   optional compare crop height
use_angle     optional boolean; DirectGLES uses packaged ANGLE when true
use_pbuffer   optional boolean; DirectGLES uses an offscreen EGL pbuffer when true
angle_variant required with use_angle; signed packaged ANGLE short hash (`ec889e6ea831` or `90a62123d794`)
```

Implementation notes:

- The trace APK builds independently from FCL and can be launched with `adb shell am start`.
- The native runner validates inputs, sets `MOBILEGL_BACKEND_TYPE`, loads `libMobileGL.so`, runs apitrace GL retrace, writes `actual.png`, and writes `result.json`.
- Snapshot capture stays entirely in the retrace layer: apitrace selects the target call, obtains drawable/read-buffer state, calls MobileGL's public `glReadPixels`, and encodes the returned pixels to PNG. MobileGL has no trace-call or output-path hooks.
- The runner uses a MobileGL-backed EGL window-system shim. GLX calls in PC traces are consumed by apitrace's GLX retrace frontend and mapped onto this EGL shim; the Android runner does not require or call a MobileGL GLX implementation.
- `DirectGLES` and `DirectVulkan` replay on the Activity `SurfaceView` by default. DirectGLES can still use the old offscreen EGL pbuffer path by passing `use_pbuffer=true`.
- Golden comparison is implemented in native C++ with libpng RGBA decode and SSIM validation. The Java Activity only passes arguments and displays the native result, so the replay/compare core is not tied to Android UI or Bitmap APIs and can be ported to Linux.
- The plugin profile still excludes `libtrace_replay_runner.so`; normal plugin APK behavior is preserved.
- Set `MOBILEGL_ESPRYT_USE_ANGLE=1` and `MOBILEGL_TRACE_ANGLE_VARIANT=<short-hash>` when running `trace-replay-ci.sh` for DirectGLES. The trace APK contains both allowlisted ANGLE builds with short-hash filenames and SONAMEs; MobileGL resolves its signed native library directory and loads the selected pair by absolute path. Set `MOBILEGL_RETRACE_USE_PBUFFER=1` or pass `--use-pbuffer` to keep DirectGLES offscreen.

Example core-profile trace smoke command for a debug trace APK:

```sh
adb push app.trace /data/local/tmp/mobilegl_app.trace
adb push app.golden.png /data/local/tmp/mobilegl_app_ref.png
adb shell run-as top.mobilegl.plugin.espryt.trace mkdir -p files/trace-replay/input files/trace-replay/output
adb shell run-as top.mobilegl.plugin.espryt.trace cp /data/local/tmp/mobilegl_app.trace files/trace-replay/input/app.trace
adb shell run-as top.mobilegl.plugin.espryt.trace cp /data/local/tmp/mobilegl_app_ref.png files/trace-replay/input/app.golden.png
adb shell am start -a top.mobilegl.plugin.TRACE_REPLAY \
  -n top.mobilegl.plugin.espryt.trace/top.mobilegl.plugin.trace.TraceReplayActivity \
  --es trace_path /data/user/0/top.mobilegl.plugin.espryt.trace/files/trace-replay/input/app.trace \
  --es golden_path /data/user/0/top.mobilegl.plugin.espryt.trace/files/trace-replay/input/app.golden.png \
  --es output_dir /data/user/0/top.mobilegl.plugin.espryt.trace/files/trace-replay/output \
  --es diff_path /data/user/0/top.mobilegl.plugin.espryt.trace/files/trace-replay/output/app-diff.png \
  --es backend DirectGLES \
  --ez use_angle true \
  --es angle_variant ec889e6ea831 \
  --el target_call 31249 \
  --es ssim_threshold 0.99
adb shell run-as top.mobilegl.plugin.espryt.trace cat files/trace-replay/output/result.json
```

## Emulator shards in CI

The APK workflow replays its cases through `trace-replay-shard.sh`: one emulator boot, one APK
install, then an ordered list of cases for one lane, transport and backend. Each case still runs
through `trace-replay-ci.sh`, with the same arguments, environment and transport proof as a
one-case job.

```sh
sh android-plugin/trace-replay-shard.sh \
  --apk-file MobileGL-plugin-trace-release.apk \
  --backend DirectVulkan --gpu lavapipe \
  --transport inproc --lane inproc-acceptance \
  --cases-file cases.json --deadline-seconds 7200
```

The case list is a JSON array of case objects: the `cases` member of a row of
`trace_cases.py --ci --format github-apk-transport-shard-matrix`, which is what the workflow
passes, or the `case` members of `--format github-apk-transport-matrix` rows. `--dry-run` prints
what each case would be run with and touches no device.

The workflow's matrix is that shard matrix: one row per emulator job, holding some of the cases
of one lane, transport and backend. `--shard-seconds` (`RETRACE_SHARD_SECONDS` in `apk.yml`) sets
how much hinted replay time one shard aims for, and `trace_cases.py` refuses to print a shard
matrix whose shards, taken apart, are not exactly the per-case matrix. The fixtures of a shard's
cases must be in `tools/trace_replay/fixtures` before the runner starts; the workflow restores
them from one cache entry (`.github/scripts/trace-fixture-bundle.sh`).

Unless a manual run sets `android_full_matrix`, the workflow passes `--android-lanes
inproc-full`: the inproc lane, the transport the launcher ships, replays every CI case, and the
monolith and spawn lanes replay the cases `trace_cases.json` lists under `android_smoke`. The
Linux retrace jobs of `test.yml` replay every case on all three transports, so those two
emulator lanes keep the cases that prove the Android side of each arm: the server started from
the APK's native library directory, the pbuffer surface of the spawn lane, the monolith build
of the same commit as a control, and every per-case replay setting at least once. A manual run
with `android_full_matrix` replays every case on all three lanes. A branch that runs only the
monolith lane replays every case on it.

Between cases the runner force-stops the app, checks that no process of the app's uid is left,
clears the app data, removes the case's files from `/data/local/tmp` and from the host, and
checks that the device still answers. It restarts the emulator when it does not, before the
case that follows one that did not pass, and before the single retry of a case that
`trace-replay-ci.sh` charged to the infrastructure (exit 75).
`MOBILEGL_SHARD_RESTART_EMULATOR_EACH_CASE=1` cold boots before every case.

The cases of a shard share the job's time. `--deadline-seconds` stops the runner that long after
it starts, so that the job can still upload what it has: a case that is running then, or has not
started, is recorded as `not-run`. `retrace_shard_budget.py` reads a shard matrix and adds two
fields to every shard: `deadline_seconds`, the time in which every case of the shard could use
its whole time budget (`timeout_seconds` plus 300 s) when the cases before it used all of
theirs, with one infrastructure retry of the longest budget and a cold boot per case on top,
and `timeout_minutes`, the limit of the shard's job. `--cap-seconds`
(`RETRACE_SHARD_DEADLINE_CAP_SECONDS` in `apk.yml`) bounds the deadline, so a shard whose
replays all hang cannot hold a runner for hours; a capped shard loses verdicts only when several
of its replays hang to the end of their budgets.

A failing case does not stop the shard. Every case ends with `<case>-<backend>/case-status.json`
(`passed`, `failed` or `not-run`), the result root gets `shard-status.json`, the job log gets one
`::error` per case that did not pass, and the exit status is non-zero when a case did not pass or
a verdict is missing. `--finalize` writes the summary inputs and a `not-run` verdict for every
case without one, without a device; the workflow runs it after the replay step whatever happened.

`python3 android-plugin/test_trace_replay_shard.py` drives the runner with a fake adb, replay
script and AVD script.
