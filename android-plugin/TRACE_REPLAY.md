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
- Set `MOBILEGL_USE_ANGLE=1` and `MOBILEGL_TRACE_ANGLE_VARIANT=<short-hash>` when running `trace-replay-ci.sh` for DirectGLES. The trace APK contains both allowlisted ANGLE builds with short-hash filenames and SONAMEs; MobileGL resolves its signed native library directory and loads the selected pair by absolute path. Set `MOBILEGL_RETRACE_USE_PBUFFER=1` or pass `--use-pbuffer` to keep DirectGLES offscreen.

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
