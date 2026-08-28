---
name: trace-fixture-authoring
description: Author a deterministic MobileGL trace-replay fixture from a captured apitrace - build the in-tree apitrace fork, capture a reproducible scene, frame-trim with gltrim, generate and verify a golden image, package under the archive-size budget, register the case in trace_cases.json, and validate on Linux and Android. Use when adding or re-trimming a trace_replay regression fixture.
---

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
- `options.txt`: `pauseOnLostFocus:false`, a low `maxFps` (10 works), a small
  `renderDistance` (3), and the capture resolution pinned to `WIDTH` x `HEIGHT`
  (854x480) via `overrideWidth`/`overrideHeight` (or `--width`/`--height`).
  These are the main levers on fixture size: a low frame rate keeps the full
  trace short, a small render distance keeps per-frame geometry down, and the
  854x480 resolution keeps every render target the frame references small (a
  trimmed frame's framebuffer/attachment textures scale with resolution
  squared). A ~35 s in-world session at 10 fps and 854x480 lands well under the
  archive budget after repack.
- `maxFps` has a practical floor: Minecraft ignores values below ~10 and falls
  back to unlimited/vsync (a `maxFps:1` capture rendered ~60 fps and ballooned
  the trace). 10 is as low as this lever goes, so do not count on a lower frame
  rate to shrink the frame count further.
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

NEVER put a real credential on the traced command line. apitrace records the
traced process's argv into the trace as a `process.commandLine` property, so
anything passed there - `--accessToken`, session tokens, API keys - is embedded
in the trace and ships inside the committed fixture. Minecraft never validates
`--accessToken` for singleplayer, so pass a placeholder (`--accessToken 0`);
`--username`/`--uuid` are public and may stay real. Before packaging, grep the
UNCOMPRESSED trace for the secret to confirm it is absent:

```sh
"$APITRACE" repack "$WORK/$CASE/trace.trace" /tmp/plain.trace   # decompress
grep -ac "<secret-prefix>" /tmp/plain.trace                     # must be 0
```

If a secret has already been captured, it can be scrubbed in place instead of
recapturing: apitrace's snappy container is `[length][raw snappy]` chunks with
no checksum, and a high-entropy secret is stored as literal bytes, so replacing
those bytes with an EQUAL-LENGTH filler keeps the container valid and leaves the
GL call stream byte-identical. Blank every maximal run of the secret (it splits
across chunks), then verify: frame count unchanged, the decompressed trace no
longer contains the secret, and the replayed target frame still matches the
golden. Treat any already-pushed trace as leaked regardless - rotate the
credential, since a force-push does not purge the LFS object from the remote.

Watch for vendor-gated shader paths. Shader packs branch on the GL vendor that
Iris injects (`MC_GL_VENDOR_NVIDIA` / `_AMD` / ...) and compile a
vendor-exclusive path, so capturing on an NVIDIA card can bake NVIDIA-only GLSL
into the fixture (iterationRP selects `subgroupPartitionNV` /
`GL_NV_shader_subgroup_partitioned` instead of the portable
`subgroupShuffleXor`). Iris resolves the `#ifdef` before `glShaderSource`, so
only the taken branch is in the trace and the fixture cannot replay on the
mobile GPUs MobileGL targets. Rather than hunting for a second GPU (the Windows
per-app GPU preference does NOT change which OpenGL ICD is loaded), mask the
vendor at capture time with apitrace's own config - point `GLTRACE_CONF` at a
file containing:

```
GL_VENDOR = "NoVIDIA (MobileGL spoof)"
GL_RENDERER = "NoVIDIA (MobileGL spoof)"
```

The wrapper then returns that from `glGetString`, so the pack compiles the
portable path while still running on the fast driver. Pick a string that does
NOT contain the real vendor name as a substring (Iris matches by substring, so
"Not NVIDIA ..." would still match) and that is self-describing, so nobody later
mistakes the trace for a capture on different hardware. Afterwards, grep the
decoded trace to confirm the vendor-exclusive symbols are gone:

```sh
"$APITRACE" dump "$WORK/$CASE/full.trace" | grep -c subgroupPartitionNV   # must be 0
```

Software rasterisers are not a substitute here: llvmpipe exposes no
`GL_KHR_shader_subgroup` at all, and packs that use subgroup ops unguarded
cannot run on it in any vendor configuration.

Keep `full.trace` until both backends are validated.

Persistent-mapped buffers: apps may legally write a `GL_MAP_PERSISTENT_BIT`
mapping and let the GPU read it without an explicit flush (Flywheel's indirect
backend writes its compute scatter descriptors this way). Stock apitrace never
records those writes, so the trimmed fixture silently loses the content that
depends on them - the symptom is geometry that renders live but disappears in
replay. The in-tree fork shadow-tracks persistent mappings unconditionally;
if a replay of `full.trace` is already missing content that the live run
showed, fix capture (wrapper) first - no amount of trimming will bring the
data back, and the case must be recaptured. There is also a replay-side
requirement: MobileGL only forwards such never-flushed writes when
`MOBILEGL_COHERENT_AS_FLUSH=1`, so register the case with
`"coherent_as_flush": true` (see "Register the case").

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

Temporal shaders (auto-exposure / eye adaptation, TAA, temporal reflections -
e.g. the iterationRP shader pack) break a single-frame `gltrim -f`: the target
frame reads its predecessors' feedback buffers, which the isolated frame no
longer contains, so a mid-sequence frame replays overexposed to white (and any
timed name/version overlay the pack draws in its first seconds silently drops).
The symptom is a trimmed frame that looks blown-out or washed while the same
frame of `full.trace` renders correctly, and it gets worse the later the frame.
When a single-frame trim of such a pack cannot be made to render correctly, keep
the temporal history instead of the dependency slice: select an early in-world
target frame and trim a PREFIX with `apitrace trim --calls=0-<target-swap-call>`
(it preserves call numbers, so `target_call` is just that swap call). The prefix
replays every frame up to the target, so its temporal buffers are correct.
Prefer the earliest frame that already shows the intended subject - fewer lead-in
frames means a smaller archive and a faster CI replay. This deviates from the
single-frame rule deliberately; note it in the README entry.

## Generate golden

Generate frame snapshots from the trimmed trace, then choose the snapshot that
matches the selected frame. The target call used by replay registration must
come from the trimmed trace, not from a call-filtered full-trace selection.

Generate the golden with the same GL stack the scene was captured on. A headless
software renderer (llvmpipe) is fine for vanilla and light packs, but heavy
ray-traced shader packs (compute-driven atmosphere LUTs, screen-space tracing -
e.g. iterationRP) render as solid black or blown-out white under llvmpipe. Drive
the golden from a real GPU instead: on Windows a stock `glretrace.exe` (an
upstream apitrace release works for replay even on an in-tree-fork trace) replays
the trace on the discrete GPU and snapshots the target call. Read the resulting
PNG back and confirm the subject actually rendered before trusting it as golden.

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

Some packs have an irreducible size floor: a large static lookup table baked
into the pack (iterationRP ships a ~17 MiB half-float atmosphere LUT that the
target frame samples) lands in the trace once and does not compress, so every
variant - single frame, prefix, or full - sits near the same size regardless of
frame count. When the floor alone exceeds the budget, neither a lower frame rate
nor fewer frames helps; confirm the fixture is worth the exception and record the
measured size in the case's README entry rather than chasing an unreachable
target.

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
`timeout_seconds`, `ci`, `coherent_as_flush`. Long single-frame replays of
heavy in-world scenes need a raised `timeout_seconds` (the Create fixtures
use 1800). Set `"coherent_as_flush": true` for Flywheel-style engines that
let the GPU read persistent `GL_MAP_FLUSH_EXPLICIT_BIT` mappings they never
flush (both Create fixtures need it); the case then replays with
`MOBILEGL_COHERENT_AS_FLUSH=1` on every runner.

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

Build one generic trace APK. Both backends use this APK and the same package;
select the backend at replay time with `--backend`.

```sh
gradle --no-daemon -p "$REPO/android-plugin" \
  :app:assembleTraceRelease \
  -Pmobilegl.abis=all \
  -Pmobilegl.debuggableRelease=true \
  -Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_INFO \
  --parallel

TRACE_APK=$(find "$REPO/android-plugin/app/build/outputs/apk/trace/release" \
  -maxdepth 1 -name 'MobileGL-plugin-trace-release-*.apk' -print -quit)
TRACE_PACKAGE=top.mobilegl.plugin.trace
```

Release APKs are only signed when `SIGNING_STORE_PASSWORD`,
`SIGNING_KEY_ALIAS`, and `SIGNING_KEY_PASSWORD` are set and
`android-plugin/keystore.jks` exists - an unsigned build still "succeeds" but
installs fail later with `INSTALL_PARSE_FAILED_NO_CERTIFICATES`. If the device
or emulator already has `top.mobilegl.plugin.trace` from a different keystore,
uninstall that one package first or the install fails with
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`.

Match the CI environment (`.github/workflows/apk.yml` matrix): the emulator
boots with `--gpu software` + `MOBILEGL_ESPRYT_USE_ANGLE=1` for `DirectGLES`
and `--gpu lavapipe` + `MOBILEGL_MAGMA_R11G11B10F_FALLBACK=1` for
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

Run both backends against the same APK:

```sh
run_android_retrace() {
  backend="$1"
  shift
  sh "$REPO/android-plugin/trace-replay-ci.sh" \
    --apk-file "$TRACE_APK" \
    --package "$TRACE_PACKAGE" \
    --backend "$backend" \
    --result-root "$WORK/$CASE/android-result-$backend" \
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
    --timeout-seconds 900 \
    "$@"
}

run_android_retrace DirectGLES --use-pbuffer
run_android_retrace DirectVulkan
```

Inspect:

- `$WORK/$CASE/android-result-DirectGLES/$CASE-DirectGLES/result.json`
- `$WORK/$CASE/android-result-DirectVulkan/$CASE-DirectVulkan/result.json`
- Each backend's `*-actual.png`, `*-diff.png`, `retrace.log`, and `logcat.txt`

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
