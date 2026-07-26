---
name: gl-cts-on-mobilegl
description: Run the Khronos OpenGL CTS (VK-GL-CTS glcts, KHR-GL33) against MobileGL on an Android device and compute a per-backend conformance rate. Use when measuring OpenGL 3.3 core conformance for DirectGLES or DirectVulkan, building glcts for Android arm64, porting a dEQP tcu::Platform onto MobileGL, or triaging CTS failures, crashes, and cases that hang the device.
---

# OpenGL CTS on MobileGL (Android)

## Overview

`glcts` from VK-GL-CTS is built as a **standalone arm64 executable** and run from
`adb shell`. It reaches OpenGL only through `libMobileGL.so`, which supplies both
EGL and desktop GL, so a result is unambiguously MobileGL's and never the system
GL stack's. No APK and no Activity are involved.

The port lives in this repository under `MobileGL/tools/cts/` and is copied into
a VK-GL-CTS checkout by `scripts/sync_to_cts.py`, so it survives a throwaway CTS
clone.

Set up paths first:

```sh
export MG=<path-to-MobileGL-worktree>          # do builds in a worktree, not the shared tree
export CTS=<path-to-VK-GL-CTS-checkout>
export NDK="$ANDROID_HOME/ndk/27.3.13750724"
export SERIAL=<adb-device-serial>
```

## Prerequisites

- Android NDK r27 (the repo builds MobileGL with 27.3.13750724), CMake, Ninja, Python 3.
- A rooted-or-not Android device with `adb`; ~600 MB free under `/data/local/tmp`.
- **A device you can physically power-cycle.** Some cases hang the GPU hard
  enough to reboot it — see "Cases that take the device down".
- On Windows, invoke `python`, not `python3`: the latter resolves to the
  Microsoft Store alias stub and exits 49.

## Step 1 — build libMobileGL.so

Build in a git worktree (other agents share the main tree). A fresh worktree is
missing glslang's bundled SPIR-V Tools, which is a hard configure blocker
because `ENABLE_OPT` is forced on:

```sh
cp -r <main-tree>/3rdparty/glslang/External/* "$MG/3rdparty/glslang/External/"
./gradlew -p "$MG/android-plugin" :app:assembleTraceRelease
```

The stripped library lands in
`android-plugin/app/build/intermediates/stripped_native_libs/traceRelease/.../arm64-v8a/libMobileGL.so`.

## Step 2 — get VK-GL-CTS and its externals

Use a **release tag**, not `main`, so the mustpass list — and therefore the
reported rate — is citable:

```sh
git -C "$CTS" checkout opengl-cts-4.6.8.1
cd "$CTS" && python external/fetch_sources.py
```

## Step 3 — build glcts for Android arm64

```sh
python "$MG/tools/cts/scripts/sync_to_cts.py" "$CTS"

cmake -S "$CTS" -B build-cts-a64 -G Ninja \
  -DDEQP_TARGET=mobilegl -DDEQP_TARGET_TOOLCHAIN=ndk-modern \
  -DANDROID_NDK_PATH="$NDK" -DDE_ANDROID_API=26 -DANDROID_ABI=arm64-v8a \
  -DCMAKE_BUILD_TYPE=Release
ninja -C build-cts-a64 glcts
"$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip build-cts-a64/external/openglcts/modules/glcts
```

Confirm the configure output says `DE_OS = DE_OS_ANDROID`, `DE_CPU =
DE_CPU_ARM_64` and `DEQP_ANDROID_BUILD = EXE`. Two things make that work and
both are easy to get wrong:

- `DEQP_TARGET_TOOLCHAIN=ndk-modern` is required. dEQP includes `Defs.cmake`
  *before* the target file, so a target cannot set `DE_OS` itself. Without the
  toolchain hook the build mis-detects as `DE_OS_UNIX`/`x86_64` and dies on
  `__assert_fail` (bionic has `__assert2`).
- The target sets `DEQP_ANDROID_EXE ON`. Otherwise dEQP builds the modules into
  the `libdeqp.so` an APK would load and no `glcts` executable exists.

`KHR-GL33` needs no ungating — the package registry registers it unconditionally;
only the `dEQP-*` packages are `#if DE_OS != DE_OS_ANDROID`.

## Step 4 — deploy

```sh
adb -s $SERIAL shell mkdir -p /data/local/tmp/mgcts
adb -s $SERIAL push build-cts-a64/external/openglcts/modules/glcts /data/local/tmp/mgcts/
adb -s $SERIAL push build-cts-a64/external/openglcts/modules/gl_cts /data/local/tmp/mgcts/
adb -s $SERIAL push <libMobileGL.so> /data/local/tmp/mgcts/
adb -s $SERIAL shell chmod 755 /data/local/tmp/mgcts/glcts
```

## Step 5 — preflight

Never start a multi-hour run without this. It proves the device/library pair
yields a 3.3 core context and that FBO readback is correct, in about a second:

```sh
adb -s $SERIAL shell 'cd /data/local/tmp/mgcts && LD_LIBRARY_PATH=. ./mgprobe \
    --backend DirectVulkan --surface imagereader --lib ./libMobileGL.so'
```

Expect `PASS ... user_fbo=ok`. `default_fb=broken` on DirectVulkan is expected
and does not gate — see below.

## Step 6 — run

```sh
python "$MG/tools/cts/scripts/run_cts.py" \
    --serial $SERIAL --backend DirectGLES \
    --caselist .../mustpass/gl/khronos_mustpass/main/gl33-main.txt \
    --outdir runs/gles --skip-file runs/skip.txt
```

The runner re-invokes `glcts` with only the cases that have no result yet, so a
crash costs one case rather than the run. It distinguishes a crashed *case* from
a dead *device* by checking the device still answers a shell command — without
that check a dead device looks like every remaining case crashing, which yields
a completely bogus but plausible-looking conformance number. On a device reboot
it waits, re-pulls the partial `.qpa` (which survives on `/data/local/tmp`),
records the case that was open as `DeviceHang`, and quarantines it.

## Step 7 — report

```sh
python "$MG/tools/cts/scripts/qpa_report.py" runs/gles --label DirectGLES
```

Pass rate counts `Pass`, `NotSupported`, `QualityWarning`, `CompatibilityWarning`
and `Waiver` as non-failures, matching how Khronos scores a submission; the
strict rate counts only `Pass`. Quarantined and never-reached cases are reported
separately and excluded from the rates, so a partial run cannot read as a
complete one.

## Required flags, and why

| Flag | Why it is not optional |
| --- | --- |
| `--deqp-surface-type=fbo` | On DirectVulkan, `glReadPixels` from the **default framebuffer returns all zeros** with no GL error. dEQP verifies nearly everything through `glReadPixels`, so rendering to the surface scores DirectVulkan near zero for a reason unrelated to conformance. FBO readback is correct on both backends. Use it for **both** backends so the two numbers stay comparable. |
| `--deqp-terminate-on-device-lost=disable` | Defaults to *enable*, which calls `glGetGraphicsResetStatus()` after every case. That is GL 4.5 / `KHR_robustness`, absent from GL 3.3 core, so the pointer is null and the process segfaults on the first case. Desktop drivers expose the extension, which is why upstream never trips on it. |

## Cases that take the device down

Some cases hang the GPU hard enough that the device reboots or stops answering
adb entirely. Keep them in a `--skip-file`, and expect to find more:

- `KHR-GL33.clip_distance.functional` — wedged an Adreno 750 tablet; it rebooted
  and then stopped responding to adb altogether.
- `KHR-GL33.framebuffer_blit.multisampled_to_singlesampled_blit_color_config_test`
  — rebooted an Adreno 830 phone after 862 cases, on DirectGLES.

When a run dies, pull `/data/local/tmp/mgcts/chunk.qpa` — it survives the reboot,
and the last `#beginTestCaseResult` with no matching `#endTestCaseResult` names
the case that did it.

## MobileGL constraints the port works around

- **DirectVulkan cannot use an EGL pbuffer.** That path needs
  `VK_EXT_headless_surface`, which Adreno's Android driver does not expose; it
  fails inside `eglMakeCurrent`. The platform therefore gets a real
  `ANativeWindow` from **`AImageReader`** — an ordinary BufferQueue producer that
  `vkCreateAndroidSurfaceKHR` accepts, with no Activity. An `onImageAvailable`
  listener must drain the queue or the producer blocks once `maxImages` buffers
  are in flight and the next swap deadlocks.
- **`eglMakeCurrent` requires draw == read** and rejects `EGL_NO_SURFACE` with
  `EGL_BAD_MATCH`, so dEQP's `surfaceless` platform cannot be used at all, and
  `--deqp-surface-type=fbo` (which asks the platform for `SURFACETYPE_DONT_CARE`)
  must still be given a real surface.
- **Every EGL call must go through the dynamically loaded library.** dEQP's
  `surfaceless` platform mixes wrapper calls with globally linked `egl*` symbols;
  copying that on Android silently reaches the system EGL and invalidates the
  measurement. The `mobilegl` target links no `libEGL`/`libGLESv*` at all.
- **Desktop-GL configs need `EGL_OPENGL_BIT`.** The surfaceless port always asks
  for an ES bit, which can never satisfy a GL 3.3 core context.
- MobileGL aborts during static teardown (`FORTIFY: pthread_mutex_lock called on
  a destroyed mutex`) *after* the work is done; flush and `_exit()` in any small
  tool, or its exit code and output are lost.

## Contents

    platform/tcuMobileGLPlatform.{cpp,hpp}   dEQP tcu::Platform for MobileGL
    targets/mobilegl.cmake                   VK-GL-CTS target (-DDEQP_TARGET=mobilegl)
    targets/ndk-modern.cmake                 NDK toolchain hook (sets DE_OS/DE_CPU)
    probe/mgprobe.c                          preflight gate
    scripts/sync_to_cts.py                   inject the port into a CTS checkout
    scripts/run_cts.py                       crash- and reboot-resuming runner
    scripts/qpa_report.py                    .qpa -> conformance rate
