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

```sh
cmake -S "$REPO/3rdparty/apitrace" -B "$WORK/build-apitrace" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_GUI=OFF
cmake --build "$WORK/build-apitrace" --target apitrace glretrace --parallel

export APITRACE="$(find "$WORK/build-apitrace" -type f -name apitrace -perm -111 | head -n 1)"
export GLRETRACE="$(find "$WORK/build-apitrace" -type f -name glretrace -perm -111 | head -n 1)"
test -n "$APITRACE"
test -n "$GLRETRACE"
```

On Windows, set `APITRACE` and `GLRETRACE` to the corresponding `.exe` files.

## Prepare the capture

- Set the target window size to `WIDTH` x `HEIGHT`.
- Disable unintended overlays, frame counters, notifications, and launcher UI.
- Fix language, resource packs, mods, shader pack, world seed, time, weather,
  player position, camera direction, FOV, GUI scale, and render distance.
- Trace the final OpenGL process, not the launcher.
- For Minecraft, document version, mod loader, mods, shader pack, language,
  world, time, and camera setup.

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

Keep `full.trace` until both backends are validated.

## Select target frame

Fixture selection must be frame-based. Do not trim the fixture from a full trace
by filtering arbitrary call ranges or single full-trace calls. Pick a rendered
frame, then trim with `gltrim -f`.

```sh
"$APITRACE" dump --calls=frame "$WORK/$CASE/full.trace" \
  > "$WORK/$CASE/frames.txt"
```

Inspect `frames.txt`, identify the frame that contains the intended visual
state, and set:

```sh
export TARGET_FRAME=<chosen-frame-number>
```

## Trim and package

```sh
"$APITRACE" gltrim \
  -f "$TARGET_FRAME" \
  --output "$WORK/$CASE/trace.trace" \
  "$WORK/$CASE/full.trace"
```

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

Package:

```sh
mkdir -p "$WORK/$CASE/archive"
cp "$WORK/$CASE/trace.trace" "$WORK/$CASE/archive/trace.trace"
tar -czf "$REPO/tools/trace_replay/fixtures/$CASE.tgz" \
  -C "$WORK/$CASE/archive" trace.trace
cp "$WORK/$CASE/$CASE.$(printf '%010d' "$TARGET_CALL").png" \
  "$REPO/tools/trace_replay/fixtures/"
```

Check the final archive size. The committed fixture archive should be less than
20 MiB, and should preferably be less than 10 MiB. If it is larger, recapture
with a shorter run or trim a smaller frame-only fixture instead of adding
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

## Register Linux replay

Edit `tools/trace_replay/CMakeLists.txt`:

```cmake
add_trace_replay_test_for_backends(case-name
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/case-name.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/case-name.0000000000.png
        TARGET_CALL 0
        WIDTH 854
        HEIGHT 480
        SSIM_THRESHOLD 0.99)
```

Optional crop:

```cmake
        CROP_X 1
        CROP_Y 1
        CROP_WIDTH 852
        CROP_HEIGHT 478
```

## Register Android replay

Edit `.github/workflows/apk.yml` `TRACE_REPLAY_CASES`:

```text
case-name|tools/trace_replay/fixtures/case-name.tgz|trace.trace|tools/trace_replay/fixtures/case-name.0000000000.png|0|854|480|0.99|0|0|0|0|900
```

Field order:

```text
case|trace archive|trace file|golden|target call|width|height|ssim threshold|crop x|crop y|crop width|crop height|timeout seconds
```

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

- Golden matches the committed trace and target call.
- Archive contains only `trace.trace`.
- Both Linux backends pass before Linux CI registration.
- Both Android backends pass before APK CI registration.
- `actual.png` and `case-diff.png` are inspected.
- Fixture `.tgz` and `.png` files are tracked by Git LFS.
- No build output, extracted trace directory, temporary report, or debug text is
  staged.
