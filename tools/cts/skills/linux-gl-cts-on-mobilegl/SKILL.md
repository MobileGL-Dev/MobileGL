---
name: linux-gl-cts-on-mobilegl
description: Run the Khronos OpenGL CTS (VK-GL-CTS glcts) against MobileGL on desktop Linux with no GPU and no device, and report a per-backend conformance rate for Espryt (DirectGLES) and Magma (DirectVulkan). Use when measuring or iterating on the conformance of one test group, when a fix needs a before/after number, or when neither an Android device nor a Windows GPU box is available.
---

# OpenGL CTS on MobileGL (desktop Linux)

## Overview

`glcts` is built as an ordinary x86-64 host executable that reaches OpenGL only
through `libMobileGL.so`, using the `mobilegl-desktop` VK-GL-CTS target and the
`tcu::Platform` port in `tools/cts/platform/`. Nothing links `libGL` or `libEGL`,
so a result is unambiguously MobileGL's.

Both backends run headless on software rendering, so this needs no GPU at all:

- **Espryt** (`DirectGLES`) drives Mesa's OpenGL ES through the system EGL.
- **Magma** (`DirectVulkan`) runs on lavapipe, whose `VK_EXT_headless_surface`
  is what the desktop platform port's pbuffer path requires.

Always report the two backends **separately**. They are different
implementations of the same front end, they fail different cases, and a single
combined number hides which one a change moved.

## Prerequisites

```sh
sudo apt-get install -y ninja-build cmake libvulkan-dev \
    libegl1-mesa-dev libgles2-mesa-dev mesa-vulkan-drivers
```

`mesa-vulkan-drivers` is what installs lavapipe; without it DirectVulkan has no
ICD and `eglInitialize` fails inside the backend. A C++23 toolchain is required
(GCC 13+, or Clang 20+ — Clang 18 defines `__cpp_concepts` as 201907L, which
switches libstdc++'s `<expected>` off and the build fails in
`MG_Util/ShaderTranspiler/Types.h`).

```sh
export MG=<path-to-MobileGL-worktree>
export CTS=<path-to-VK-GL-CTS-checkout>
```

## Step 1 — build libMobileGL.so

```sh
git -C "$MG" submodule update --init --recursive
python3 "$MG/3rdparty/glslang/update_glslang_sources.py"   # SPIRV-Tools; ENABLE_OPT is forced on

cmake -S "$MG" -B "$MG/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build "$MG/build-linux" --parallel "$(nproc)"
```

Add `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=FALSE` when iterating. The
Release configuration turns LTO on, which makes every relink cost minutes for no
behavioural difference; a conformance number is identical either way.

## Step 2 — get VK-GL-CTS and build glcts

Use a release tag so the mustpass list, and therefore the reported rate, is
citable.

```sh
git -C "$CTS" checkout opengl-cts-4.6.8.1
python3 "$CTS/external/fetch_sources.py"

python3 "$MG/tools/cts/scripts/sync_to_cts.py" "$CTS"
git -C "$CTS" apply "$MG/tools/cts/patches/0001-fbo-color-texture-attachment.patch"

cmake -S "$CTS" -B "$CTS/build-cts" -G Ninja -DDEQP_TARGET=mobilegl-desktop \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
ninja -C "$CTS/build-cts" glcts
```

Confirm the configure output says `*** Using MobileGL desktop target`. Budget a
couple of hours for the `glcts` link on a small machine; it is a one-time cost
that ccache makes cheap afterwards.

If `fetch_sources.py` dies with `HTTP Error 403` it is an egress policy blocking
the GitHub archive downloads (zlib, libpng), not a broken checkout: `git clone`
still works, so clone the package at the tag the script pins into
`external/<pkg>/src` by hand — for libpng also copy
`scripts/pnglibconf.h.prebuilt` to `src/pnglibconf.h`, which is what the
script's post-extract step does.

## Step 3 — run, once per backend

```sh
cd "$MG"
for BACKEND in DirectGLES DirectVulkan; do
  python3 tools/cts/scripts/run_cts_local.py --backend "$BACKEND" \
      --glcts "$CTS/build-cts/external/openglcts/modules/glcts" \
      --lib   build-linux/libMobileGL.so \
      --caselist cases.txt --outdir "runs/${BACKEND}" \
      --env EGL_PLATFORM=surfaceless
  python3 tools/cts/scripts/qpa_report.py "runs/${BACKEND}" --label "$BACKEND"
done
```

`cases.txt` is any subset of a mustpass list. For one group, filter the list
rather than running the whole suite:

```sh
grep direct_state_access \
  "$CTS"/external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass/main/gl45-main.txt \
  > cases.txt
```

`run_cts_local.py` re-invokes `glcts` with only the cases that have no result
yet, so a crash costs one case rather than the run, and it records the case that
was open when the process died as `Crash`. `qpa_report.py` scores `Pass`,
`NotSupported` and the warning statuses as non-failures, the way Khronos scores
a submission, and reports the strict `Pass`-only rate alongside.

## Required flags, and why

| Flag | Why it is not optional |
| --- | --- |
| `--env EGL_PLATFORM=surfaceless` | For DirectGLES. With no `/dev/dri` node Mesa's EGL fails `eglInitialize` on the default display, and MobileGL surfaces that as `EGL_BAD_ALLOC` out of `eglCreatePbufferSurface` — which points at the wrong call entirely. Harmless for DirectVulkan, so pass it to both. |
| `--deqp-surface-type=fbo` (the runner's default) | DirectVulkan reads back zeros from the **default** framebuffer. dEQP verifies nearly everything through `glReadPixels`, so rendering to the surface scores Magma near zero for a reason unrelated to conformance. Use it for both backends so the two numbers stay comparable. |
| `--deqp-terminate-on-device-lost=disable` (supplied by the runner) | Its default calls `glGetGraphicsResetStatus()` after every case. That is GL 4.5 / `KHR_robustness`, absent from what MobileGL exports, so the pointer is null and the process segfaults on the first case. |

## What this environment does and does not tell you

Reproducible here, and MobileGL's own rather than a driver quirk:

- DirectVulkan's default-framebuffer readback returns zeros; a user FBO,
  renderbuffer- or texture-attached, is correct on both backends. This is the
  same defect the Android runs work around, so it can be debugged without a
  phone.

Different from a real GPU, so do not read conformance into it:

- lavapipe supports renderbuffer formats Adreno reports as unsupported, so the
  Android runs see `NotSupported` where these do not, and vice versa.
- Backend limits differ. `GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT` is 16 on llvmpipe
  and on lavapipe; a device that reports 1 will not exercise the same paths.
- Everything is a software rasterizer, so a case that fails only under real
  timing or real tiling will not fail here.

## Reference results: KHR-GL45.direct_state_access

`opengl-cts-4.6.8.1`, the 371 `direct_state_access` cases of the `gl45-main`
mustpass list, Mesa 25.2.8, `--deqp-surface-type=fbo`.

| backend | renderer | conformance | strict Pass | Fail | InternalError | Crash |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| DirectGLES | Espryt | **74.93%** | 67.39% | 85 | 8 | 0 |
| DirectVulkan | Magma | **73.32%** | 73.05% | 88 | 8 | 3 |

## Contents

    platform/tcuMobileGLPlatform.{cpp,hpp}   dEQP tcu::Platform for MobileGL
    targets/mobilegl-desktop.cmake           VK-GL-CTS target (-DDEQP_TARGET=mobilegl-desktop)
    scripts/sync_to_cts.py                   inject the port into a CTS checkout
    scripts/run_cts_local.py                 crash-resuming local-host runner
    scripts/qpa_report.py                    .qpa -> conformance rate
