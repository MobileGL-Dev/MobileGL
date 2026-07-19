---
name: trace-fixture-authoring-on-android-fcl
description: Capture an on-device Android apitrace from FCL's MobileGL renderers. Use when preparing a reproducible MobileGL DirectGLES, Magma (DirectVulkan), or SimpleFPEWrapper rendering trace, marking the frame of a visual defect, pulling the resulting full.trace, or turning a device capture into a replay fixture.
---

# MobileGL Android trace capture

## Overview

Use FCL's Android `egltrace.so` wrapper, not Perfetto. When enabled before
launch, it records the complete EGL/GL call stream to `full.trace`. The game's
**MobileGL Trace → Capture** control marks the next swap frame in
`capture-result.json`; it does not start or stop recording and does not produce
a one-frame trace by itself.

Run commands from the FoldCraftLauncher repository root:

```sh
export REPO="$PWD"
export CAPTURE="$REPO/MobileGL/tools/trace_replay/skills/trace-fixture-authoring-on-android-fcl/scripts"
export SERIAL=<adb-device-serial> # omit --serial only if exactly one device is attached
```

The capture scripts are bundled inside this skill under `scripts/`; they
auto-detect the FoldCraftLauncher repository root from their own location, so
`--repo` only needs to be passed for a non-standard checkout layout.

## Prerequisites

- Use an FCL build containing `MobileGLTraceCapture` and the in-game Capture
  menu entry.
- Select one of these renderers: MobileGL (DirectGLES), MobileGL Magma
  (DirectVulkan), or SimpleFPEWrapper. MobileGlues is not supported by this
  capture wrapper.
- Install `adb` and make it available on `PATH`; authorize USB debugging.
- Build the wrapper with Android NDK, CMake, Ninja, Python 3, and the checked
  out in-tree `MobileGL/3rdparty/apitrace` submodule.

Confirm the attached device and ABI before building. The wrapper ABI must match
the device process ABI.

```sh
adb devices -l
adb -s "$SERIAL" shell getprop ro.product.cpu.abi
```

Use `arm64-v8a` for the usual `arm64-v8a` result; use the matching NDK ABI for
other devices.

## Build and install the wrapper

Build once per ABI or after changing apitrace/wrapper sources:

```sh
python3 "$CAPTURE/build_android_egltrace.py" --abi arm64-v8a
```

This generates `egltrace.so` under the skill's `scripts/out/` directory. Push
it and write FCL's enable sentinel before launching the game:

```sh
python3 "$CAPTURE/adb_capture.py" --serial "$SERIAL" install-wrapper
python3 "$CAPTURE/adb_capture.py" --serial "$SERIAL" enable
```

The device-side control directory is `/sdcard/FCL/mobilegl-trace`. FCL copies
the shared `egltrace.so` into its private files directory at launch, replaces
the renderer's EGL library with it, and forwards to the real MobileGL library.

## Capture a reproduction

1. Start FCL after the wrapper and enable sentinel are in place. Select the
   intended supported MobileGL renderer and launch the game.
2. Trace mode forces the game to `854x480`; account for that when reproducing
   and comparing output.
3. Reproduce the issue. Start close to the target scene because tracing starts
   when the game launches and trace files can grow rapidly.
4. At the desired visual state, open FCL's right-side game menu and press
   **MobileGL Trace → Capture**. Let at least one frame present afterward.
5. Exit the game cleanly, then pull the latest session:

```sh
python3 "$CAPTURE/adb_capture.py" --serial "$SERIAL" pull-latest
```

The default local result is:

```text
.trace-work/pulled-mobilegl-captures/capture-YYYYMMDD-HHMMSS-<renderer>/
  full.trace
  capture-status.json
  capture-result.json
```

`capture-result.json` must show `"status": "captured"`. Its `targetFrame`
is the one-based swap count used by `gltrim`; `zeroBasedFrame` is included for
tools that use zero-based indexing.

## Diagnose setup failures

Inspect the active device session directly:

```sh
adb -s "$SERIAL" shell cat /sdcard/FCL/mobilegl-trace/latest-session.txt
adb -s "$SERIAL" shell ls -lh /sdcard/FCL/mobilegl-trace
adb -s "$SERIAL" shell cat /sdcard/FCL/mobilegl-trace/capture-*/capture-status.json
adb -s "$SERIAL" shell cat /sdcard/FCL/mobilegl-trace/capture-*/capture-result.json
```

If `capture-status.json` reports a missing `egltrace.so`, rebuild/push the
correct ABI and relaunch. If `capture-result.json` is absent, Capture was
pressed without an active trace session, or no subsequent `eglSwapBuffers`
occurred. The menu button itself only writes `capture-once.request`.

Disable tracing when finished; otherwise the next supported MobileGL launch
will trace again:

```sh
python3 "$CAPTURE/adb_capture.py" --serial "$SERIAL" disable
```

## Create a replay fixture (optional)

Keep the raw `full.trace` until replay validation succeeds. To frame-trim and
package the marked frame for MobileGL trace replay, use the existing helper:

```sh
python3 "$CAPTURE/package_capture_fixture.py" \
  --serial "$SERIAL" \
  --case <case-name> \
  --apitrace <path-to-in-tree-apitrace>
```

It pulls the latest capture if necessary, uses `capture-result.json` to select
the frame, runs `apitrace gltrim`, creates a golden image, and enforces the
fixture archive-size limit. Follow `../trace-fixture-authoring/SKILL.md` for
deterministic scene setup, verification, and registry changes.
