# Trace fixture authoring

## Variables

```sh
export REPO="$PWD"
export WORK="$PWD/.trace-work"
export CASE="case-name"
export WIDTH=854
export HEIGHT=480
export TARGET_FRAME=0
export TARGET_CALL=0
```

## Prerequisites

```sh
git clone --recursive <mobilegl-repo-url> MobileGL
cd MobileGL
git lfs install
git lfs pull
```

Install:

- CMake
- Ninja
- C++ compiler
- Python 3
- Mesa OpenGL/EGL runtime
- Vulkan loader and ICD for `DirectVulkan`
- Pillow or ImageMagick for alpha cleanup
- Android SDK, Android NDK, JDK, Gradle, and `adb` for Android replay

## Build apitrace

Build the in-tree fork, not an upstream release: it carries the frametrim
handlers for DSA and ARB_multi_bind call streams and shadow-based tracking of
persistent-mapped buffers, all of which modern Minecraft mod stacks need.

```sh
cmake -S "$REPO/3rdparty/apitrace" -B "$WORK/build-apitrace" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_GUI=OFF
cmake --build "$WORK/build-apitrace" --target apitrace glretrace gltrim --parallel

export APITRACE="$(find "$WORK/build-apitrace" -type f -name apitrace -perm -111 | head -n 1)"
export GLRETRACE="$(find "$WORK/build-apitrace" -type f -name glretrace -perm -111 | head -n 1)"
export GLTRIM="$(find "$WORK/build-apitrace" -type f -name gltrim -perm -111 | head -n 1)"
test -n "$APITRACE"
test -n "$GLRETRACE"
test -n "$GLTRIM"
```

On Windows, set `APITRACE` and `GLRETRACE` to the corresponding `.exe` files
and also build the `wgltrace` target: `apitrace trace` fails with "failed to
find opengl32.dll wrapper" unless `wrappers/opengl32.dll` was built. `gltrim`
may be built and run on a Linux/WSL checkout instead; traces are portable, and
trimming a multi-hundred-MB trace is much faster from a native filesystem copy
than through `/mnt/c`.

## Prepare the capture

- Set the target window size to `WIDTH` x `HEIGHT`.
- Disable unintended overlays, frame counters, notifications, and launcher UI.
- Fix language, resource packs, mods, shader pack, world seed, time, weather,
  player position, camera direction, FOV, GUI scale, and render distance.
- Trace the final OpenGL process, not the launcher.
- For Minecraft, document version, mod loader, mods, shader pack, language,
  world, time, and camera setup.

Minecraft specifics that keep the capture deterministic and small:

- Freeze the world in `level.dat`: `doDaylightCycle`, `doWeatherCycle`,
  `doMobSpawning`, `randomTickSpeed 0`, a fixed `DayTime`, and the player
  `Rotation` that frames the intended subject. The camera snaps to the saved
  rotation on world join, so composition is edited in the save, not in-game.
- `options.txt`: `pauseOnLostFocus:false`, a low `maxFps` (10 works), and a
  small `renderDistance` (3). Frame rate and render distance are the two main
  levers on trace size; a ~35 s in-world session at 10 fps lands well under
  the archive budget after repack.
- Enter the world non-interactively with `--quickPlaySingleplayer <world>` so
  every capture takes the same path from boot to gameplay.
- Keep the game window UNFOCUSED for the whole capture (focus the desktop
  right after launch, and again before closing). A focused Minecraft window
  grabs the mouse, and any physical mouse motion rotates the camera - the
  resulting goldens show a drifted view that is easy to misread as a
  rendering bug.
- On Windows with JDK 21+, pass
  `-Djdk.net.unixdomain.tmpdir=<short-path-without-spaces>`: NIO selectors
  create AF_UNIX sockets under `%TEMP%`, which fails on some hosts and kills
  the game at boot with "Unable to establish loopback connection".

## Capture

```sh
mkdir -p "$WORK/$CASE"
"$APITRACE" trace --api=gl \
  --output "$WORK/$CASE/full.trace" \
  -- <application-command> <application-args>
```

For Java:

```sh
"$APITRACE" trace --api=gl \
  --output "$WORK/$CASE/full.trace" \
  -- "$JAVA_EXE" <jvm-args> <main-class-or-jar> <game-args>
```

An `@argfile` with the full JVM+game command line keeps the invocation
reproducible across recaptures.

Keep `full.trace` until both backends are validated.

Persistent-mapped buffers: apps may legally write a `GL_MAP_PERSISTENT_BIT`
mapping and let the GPU read it without an explicit flush (Flywheel's indirect
backend writes its compute scatter descriptors this way). Stock apitrace never
records those writes, so the trimmed fixture silently loses the content that
depends on them - the symptom is geometry that renders live but disappears in
replay. The in-tree fork shadow-tracks persistent mappings unconditionally;
if a replay of `full.trace` is already missing content that the live run
showed, fix capture (wrapper) first - no amount of trimming will bring the
data back, and the case must be recaptured.

## Select target frame

Fixture selection must be frame-based. Do not trim the fixture from a full trace
by filtering arbitrary call ranges or single full-trace calls. Pick a rendered
frame, then trim with `gltrim -f`.

```sh
"$APITRACE" dump --calls=frame "$WORK/$CASE/full.trace" \
  > "$WORK/$CASE/frames.txt"
```

A quick way to bound the choice is the total frame count from a benchmark
replay:

```sh
"$GLRETRACE" -b "$WORK/$CASE/full.trace"   # "Rendered N frames in ..."
```

Pick a LATE frame (roughly `N - 20`): early frames still contain loading
screens, chunk pop-in, and animation warm-up, while the last few frames may
overlap the window-close path. Inspect `frames.txt` or snapshot the candidate
frame to confirm it contains the intended visual state, then set:

```sh
export TARGET_FRAME=<chosen-frame-number>
```

## Trim and package

```sh
"$GLTRIM" \
  -f "$TARGET_FRAME" \
  --output "$WORK/$CASE/trace.trace" \
  "$WORK/$CASE/full.trace"
```

Then VERIFY the trim before doing anything else: replay `trace.trace`,
snapshot its final swap, and compare the content against the same frame of
`full.trace`. gltrim bugs fail silently - the classic symptom is a trimmed
trace whose static world renders fine while everything driven by less common
call patterns (DSA texture binds, `glBindBuffersRange` multi-bind setup,
compute-written buffers) is missing or garbled, often with "invalid buffer
name"-style retrace warnings. If content is missing from the trimmed trace
but present in the full trace, the fix belongs in `3rdparty/apitrace`'s
frametrim, not in the fixture.

## Generate golden

Generate frame snapshots from the trimmed trace, then choose the snapshot that
matches the selected frame. The target call used by replay registration must
come from the trimmed trace, not from a call-filtered full-trace selection.

```sh
mkdir -p "$WORK/$CASE/golden"
"$APITRACE" replay --headless \
  --snapshot-prefix "$WORK/$CASE/golden/$CASE." \
  --call-nos \
  "$WORK/$CASE/trace.trace"
```

For a single-frame trim the target is simply the trimmed trace's final swap;
finding it and snapshotting just that call is much faster than dumping every
frame:

```sh
"$APITRACE" dump "$WORK/$CASE/trace.trace" | grep SwapBuffers | tail -n 1
"$GLRETRACE" -s "$WORK/$CASE/golden/$CASE." -S <final-swap-call> \
  "$WORK/$CASE/trace.trace"
```

Set `TARGET_CALL` to the call number in the chosen trimmed-trace snapshot
filename:

```sh
export TARGET_CALL=<chosen-trimmed-trace-snapshot-call>
GOLDEN_SRC="$WORK/$CASE/golden/$CASE.$(printf '%010d' "$TARGET_CALL").png"
```

Remove unintended alpha:

```sh
python3 - "$GOLDEN_SRC" "$WORK/$CASE/$CASE.$(printf '%010d' "$TARGET_CALL").png" <<'PY'
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
img = Image.open(src).convert("RGBA")
bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
bg.alpha_composite(img)
bg.convert("RGB").save(dst)
PY
```

Or copy directly:

```sh
cp "$GOLDEN_SRC" "$WORK/$CASE/$CASE.$(printf '%010d' "$TARGET_CALL").png"
```

Verify the golden CONTENT against a reference (a screenshot of the live run,
or the same scene on a known-good backend), not just that a file exists. The
subject must be present, correctly shaped, and framed as intended - a golden
captured through a drifted camera or a half-loaded scene will happily pass
authoring and then permanently enshrine the wrong image.

Package. Compress the trace itself with `repack --brotli` first - it shrinks
a gzip-resistant trace by an order of magnitude (a ~70 MB single-frame
Minecraft trim lands around 7 MB) and glretrace reads it directly:

```sh
"$APITRACE" repack --brotli "$WORK/$CASE/trace.trace" "$WORK/$CASE/trace-brotli.trace"
mkdir -p "$WORK/$CASE/archive"
cp "$WORK/$CASE/trace-brotli.trace" "$WORK/$CASE/archive/trace.trace"
tar -czf "$REPO/tools/trace_replay/fixtures/$CASE.tgz" \
  -C "$WORK/$CASE/archive" trace.trace
cp "$WORK/$CASE/$CASE.$(printf '%010d' "$TARGET_CALL").png" \
  "$REPO/tools/trace_replay/fixtures/"
```

If the case is ever re-trimmed, REDO the repack and the archive: a `.tgz`
whose repack predates the latest trim silently packages the stale trace, and
the mismatch only surfaces later as "did not create expected snapshot" when
the registered target call no longer exists.

Check the final archive size. The committed fixture archive should be less than
20 MiB, and should preferably be less than 10 MiB. If it is larger, recapture
with a shorter run or a lower frame rate / render distance instead of adding
call-based filtering.

```sh
du -h "$REPO/tools/trace_replay/fixtures/$CASE.tgz"
tar -tzf "$REPO/tools/trace_replay/fixtures/$CASE.tgz"
```

Track with Git LFS:

```sh
git lfs track "tools/trace_replay/fixtures/*.tgz"
git lfs track "tools/trace_replay/fixtures/*.png"
git add .gitattributes tools/trace_replay/fixtures/$CASE.tgz \
  tools/trace_replay/fixtures/$CASE.$(printf '%010d' "$TARGET_CALL").png
git lfs status
```

## Register the case

Both Linux ctest and the Android CI matrix are generated from the single
registry `tools/trace_replay/trace_cases.json` (via
`tools/trace_replay/trace_cases.py`); there is nothing to edit in
`CMakeLists.txt` or `apk.yml`. Append one object to `cases`:

```json
{
  "name": "case-name",
  "trace_archive": "case-name.tgz",
  "golden": "case-name.0000000000.png",
  "target_call": 0,
  "timeout_seconds": 900
}
```

Values matching the `defaults` block (854x480, `trace.trace`, ssim 0.99, zero
crop, 900 s timeout) may be omitted. Available per-case keys: `name`,
`trace_archive`, `trace_file`, `golden`, `alternate_golden`, `target_call`,
`width`, `height`, `ssim_threshold`, `crop_x/y/width/height`,
`timeout_seconds`, `ci`. Long single-frame replays of heavy in-world scenes
need a raised `timeout_seconds` (the Create fixtures use 1800).

Update `tools/trace_replay/README.md` with one fixture sentence and one golden
image link.

## Validate on Linux

```sh
cmake -S "$REPO" -B "$WORK/build-linux" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOBILEGL_BUILD_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF \
  -DMOBILEGL_BUILD_TRACE_REPLAY=ON \
  -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO
cmake --build "$WORK/build-linux" --target mobilegl_trace_replay --parallel
```

Run the registered case:

```sh
ctest --test-dir "$WORK/build-linux/tools/trace_replay" -V \
  -R "MobileGLTraceReplay\\.$CASE\\."
```

Run one backend manually:

```sh
cmake \
  -DTRACE_REPLAY_EXE="$WORK/build-linux/tools/trace_replay/mobilegl_trace_replay" \
  -DMOBILEGL_LIBRARY="$WORK/build-linux/libMobileGL.so" \
  -DTRACE_CASE_NAME="$CASE" \
  -DTRACE_ARCHIVE="$REPO/tools/trace_replay/fixtures/$CASE.tgz" \
  -DTRACE_FILE=trace.trace \
  -DTRACE_GOLDEN="$REPO/tools/trace_replay/fixtures/$CASE.$(printf '%010d' "$TARGET_CALL").png" \
  -DTRACE_BACKEND=DirectGLES \
  -DTRACE_TARGET_CALL="$TARGET_CALL" \
  -DTRACE_WIDTH="$WIDTH" \
  -DTRACE_HEIGHT="$HEIGHT" \
  -DTRACE_SSIM_THRESHOLD=0.99 \
  -DTRACE_CROP_X=0 \
  -DTRACE_CROP_Y=0 \
  -DTRACE_CROP_WIDTH=0 \
  -DTRACE_CROP_HEIGHT=0 \
  -DTRACE_OUTPUT_DIR="$WORK/$CASE/linux-DirectGLES" \
  -DTRACE_ARTIFACT_DIR="$WORK/$CASE/linux-artifacts" \
  -P "$REPO/tools/trace_replay/run_trace_case.cmake"
```

DirectGLES CI env:

```sh
export EGL_PLATFORM=surfaceless
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=3.3
export MESA_GLSL_VERSION_OVERRIDE=330
```

DirectVulkan check:

```sh
vulkaninfo | grep -E 'deviceName|VK_EXT_headless_surface'
```

## Validate on Android

Build trace APKs:

```sh
gradle --no-daemon -p "$REPO/android-plugin" \
  :app:assembleEsprytTraceRelease \
  :app:assembleMagmaTraceRelease \
  -Pmobilegl.abis=all \
  -Pmobilegl.debuggableRelease=true \
  -Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_INFO \
  --parallel
```

Release APKs are only signed when `SIGNING_STORE_PASSWORD`,
`SIGNING_KEY_ALIAS`, and `SIGNING_KEY_PASSWORD` are set and
`android-plugin/keystore.jks` exists - an unsigned build still "succeeds" but
installs fail later with `INSTALL_PARSE_FAILED_NO_CERTIFICATES`. If the
device or emulator has a trace package from a different keystore, uninstall
`top.mobilegl.plugin.espryt.trace` / `top.mobilegl.plugin.magma.trace` first
or the install fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.

Match the CI environment (`.github/workflows/apk.yml` matrix): the emulator
boots with `--gpu software` + `MOBILEGL_RETRACE_USE_ANGLE=1` for `DirectGLES`
and `--gpu lavapipe` + `MOBILEGL_VULKAN_R11G11B10F_FALLBACK=1` for
`DirectVulkan`. The emulator's ANGLE-on-Vulkan GLES stack exercises genuinely
different driver semantics than physical devices (e.g. indirect-draw
`gl_InstanceID` handling), so treat AVD-only image mismatches as real signal,
not emulator noise.

Known emulator flake: the FIRST DirectVulkan replay after a fresh lavapipe
AVD boot segfaults intermittently (~50%), for any trace; subsequent runs in
the same boot are stable. Burn a warm-up replay and discard its result before
the measured runs.

Result directories keep `result.json` from previous runs - delete the case's
result directory before each run, or an earlier failure/success can masquerade
as the current one (identical-to-the-last-digit ssim across "different" runs
is the tell).

Run DirectGLES:

```sh
sh "$REPO/android-plugin/trace-replay-ci.sh" \
  --apk-file "$REPO/android-plugin/app/build/outputs/apk/esprytTrace/release/MobileGL-EsprytTrace-release.apk" \
  --package top.mobilegl.plugin.espryt.trace \
  --backend DirectGLES \
  --result-root "$WORK/$CASE/android-result" \
  --fixture-root "$WORK/$CASE/android-fixture" \
  --case "$CASE" \
  --trace-archive "$REPO/tools/trace_replay/fixtures/$CASE.tgz" \
  --trace-file trace.trace \
  --golden "$REPO/tools/trace_replay/fixtures/$CASE.$(printf '%010d' "$TARGET_CALL").png" \
  --target-call "$TARGET_CALL" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --ssim-threshold 0.99 \
  --crop-x 0 \
  --crop-y 0 \
  --crop-width 0 \
  --crop-height 0 \
  --timeout-seconds 900
```

Run DirectVulkan with:

- APK: `MobileGL-MagmaTrace-release.apk`
- package: `top.mobilegl.plugin.magma.trace`
- backend: `DirectVulkan`

Inspect:

- `$WORK/$CASE/android-result/$CASE-DirectGLES/result.json`
- `$WORK/$CASE/android-result/$CASE-DirectGLES/$CASE-DirectGLES-actual.png`
- `$WORK/$CASE/android-result/$CASE-DirectGLES/$CASE-DirectGLES-diff.png`
- `$WORK/$CASE/android-result/$CASE-DirectGLES/retrace.log`
- `$WORK/$CASE/android-result/$CASE-DirectGLES/logcat.txt`

## Checklist

- Golden content is verified against a live-run reference (subject present,
  correct shapes, intended camera framing).
- Golden matches the committed trace and target call.
- Archive contains only `trace.trace`, and that file is the brotli repack of
  the CURRENT trim.
- Both Linux backends pass before registration in `trace_cases.json`.
- Both Android backends pass on a CI-equivalent AVD (software/ANGLE +
  lavapipe) before relying on APK CI.
- `actual.png` and `case-diff.png` are inspected.
- Fixture `.tgz` and `.png` files are tracked by Git LFS.
- No build output, extracted trace directory, temporary report, or debug text is
  staged.
