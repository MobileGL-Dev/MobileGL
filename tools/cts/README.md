# Running the OpenGL CTS against MobileGL

This directory contains three supported paths:

- Android arm64 / MobileGL EGL: the KHR-GL33 workflow documented below and in
  `skills/gl-cts-on-mobilegl/SKILL.md`.
- Windows x64 / MobileGL WGL: the GL30-GL46 pipeline in
  `scripts/wgl_glcts_pipeline.py`, documented by
  `skills/wgl-gl-cts-on-mobilegl/SKILL.md`.
- Desktop Linux x64 / MobileGL EGL: `scripts/run_cts_local.py` against the
  `mobilegl-desktop` VK-GL-CTS target, documented in "Desktop Linux workflow"
  below. This is the path that needs no device and no GPU.

Windows prerequisites are Git, Python 3.9+, CMake, Visual Studio 2022's Desktop
C++ workload, and a Vulkan SDK visible to CMake. DirectVulkan also needs a
working Vulkan loader plus a GPU-vendor ICD and driver; the SDK alone is not a
GPU driver.

For Windows, start with:

```powershell
python tools\cts\scripts\wgl_glcts_pipeline.py --help
```

The pipeline builds MobileGL as a drop-in `opengl32.dll`, builds or reuses
`glcts.exe`, checks that WGL loaded MobileGL rather than the system driver,
resumes individual suites after crashes/timeouts, and writes Markdown plus JSON
reports below the printed `runs/<first-16-of-run-fingerprint>` directory. Its
manifest records provenance and the runner settings used to validate a resume.

## Desktop Linux workflow

The `mobilegl-desktop` target builds `glcts` as an ordinary host executable that
reaches OpenGL only through `libMobileGL.so`. Both backends run headless with no
GPU at all, which makes this the cheapest way to measure a single test group
while working on it.

Apt packages: `mesa-vulkan-drivers` (lavapipe, for DirectVulkan),
`libegl1-mesa-dev` and `libgles2-mesa-dev` (the system EGL/ES that DirectGLES
drives), `libvulkan-dev`, `ninja-build`.

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF
cmake --build build-linux --parallel "$(nproc)"

python tools/cts/scripts/sync_to_cts.py "$CTS"
git -C "$CTS" apply <path-to>/tools/cts/patches/0001-fbo-color-texture-attachment.patch
cmake -S "$CTS" -B "$CTS/build-cts" -G Ninja -DDEQP_TARGET=mobilegl-desktop \
      -DCMAKE_BUILD_TYPE=Release
ninja -C "$CTS/build-cts" glcts

python tools/cts/scripts/run_cts_local.py --backend DirectGLES \
    --glcts "$CTS/build-cts/external/openglcts/modules/glcts" \
    --lib build-linux/libMobileGL.so \
    --caselist cases.txt --outdir runs/gles --env EGL_PLATFORM=surfaceless
python tools/cts/scripts/qpa_report.py runs/gles --label DirectGLES
```

`EGL_PLATFORM=surfaceless` is not optional for DirectGLES: without a `/dev/dri`
node Mesa's EGL fails `eglInitialize` on the default display, and MobileGL
reports that as `EGL_BAD_ALLOC` out of `eglCreatePbufferSurface`. DirectVulkan
needs nothing extra - lavapipe exposes `VK_EXT_headless_surface`, which is what
the desktop platform port's pbuffer path requires.

Two known differences from a real GPU, both MobileGL's rather than the harness's:
DirectVulkan reads back zeros from the **default** framebuffer (a user FBO,
renderbuffer- or texture-attached, is correct on both backends), and lavapipe
supports renderbuffer formats that Adreno reports as unsupported, so the Android
runs see `NotSupported` where these do not.

### Reference results: KHR-GL45.direct_state_access

`opengl-cts-4.6.8.1`, the 371 `direct_state_access` cases of the `gl45-main`
mustpass list, lavapipe / Mesa 25.2.8, `--deqp-surface-type=fbo`.

| backend | conformance | strict Pass | Fail | InternalError | Crash |
| --- | ---: | ---: | ---: | ---: | ---: |
| DirectGLES | **74.66%** | 67.12% | 86 | 8 | 0 |
| DirectVulkan | **73.05%** | 72.78% | 89 | 8 | 3 |

`textures_storage_multisample_*` is 54 of what remains on either backend and
needs a feature rather than a fix: a multisample texture has to be attachable to
a framebuffer and then sampleable through `sampler2DMS`, and the array half of
the group additionally needs layered attachments, which the framebuffer
attachment model does not represent yet. `program_pipelines_*` needs
ARB_separate_shader_objects, which is stubbed throughout.

## Android KHR-GL33 workflow

Goal: measure how much of the OpenGL 3.3 core-profile conformance suite MobileGL
passes, separately for each backend (`DirectGLES`, `DirectVulkan`).

## How MobileGL is reached from a test binary

MobileGL ships its own EGL implementation alongside its desktop-GL implementation
in a single `libMobileGL.so`. A plain arm64 ELF in `/data/local/tmp` can therefore
drive it with no APK and no Activity:

1. `setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES"|"DirectVulkan")` **before** the
   library is mapped — MobileGL parses its configuration from an ELF constructor.
2. `dlopen("libMobileGL.so")`, then `dlsym` the `egl*` and `gl*` entry points.
   MobileGL exports 45 EGL symbols and the desktop GL functions directly;
   `eglGetProcAddress` resolves the same set.
3. `eglBindAPI(EGL_OPENGL_API)`, choose a config with `EGL_RENDERABLE_TYPE =
   EGL_OPENGL_BIT`, then `eglCreateContext` with
   `EGL_CONTEXT_OPENGL_PROFILE_MASK = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT` and
   major/minor `3`/`3`.

This yields a genuine GL 3.3 core context (`GL_CONTEXT_PROFILE_MASK == 0x1`).

## Surface type, per backend

| backend | pbuffer (headless) | window |
|---|---|---|
| `DirectGLES` | works | works |
| `DirectVulkan` | **unusable** | works |

`DirectVulkan`'s pbuffer path builds a headless `VkSurfaceKHR` and so requires the
`VK_EXT_headless_surface` instance extension, which Adreno's Android driver does
not expose. It fails inside `eglMakeCurrent`, not at surface creation.

The workaround that keeps everything in a shell process: obtain a real
`ANativeWindow` from **`AImageReader`** (`AImageReader_newWithUsage` +
`AImageReader_getWindow`). It is an ordinary BufferQueue producer, so
`vkCreateAndroidSurfaceKHR` accepts it, and no Activity is involved. Register an
`onImageAvailable` listener that acquires and deletes each image — otherwise the
producer blocks once `maxImages` buffers are in flight and the next swap hangs.

## Why the suite must render into an FBO

On a window surface, `DirectVulkan`'s `glReadPixels` from the **default
framebuffer** returns all zeros, with no GL error, both before and after
`eglSwapBuffers`. `DirectGLES` on the identical window is correct, and readback
from a **user FBO is correct on both backends**.

Verified on two SoCs and two drivers, so this is MobileGL's behaviour rather than
a driver quirk:

| device | GPU | driver | default-FB | user FBO |
|---|---|---|---|---|
| Xiaomi 24129PN74C | Adreno 830 | Vulkan 1.3.284 / 512.800.46 | zeros | ok |
| Lenovo TB321FU | Adreno 750 | Vulkan 1.3.128 / 512.762.28 | zeros | ok |

dEQP verifies nearly every case through `glReadPixels`, so running it against the
default framebuffer would score `DirectVulkan` near zero for a reason unrelated to
conformance. The runs therefore use `--deqp-surface-type=fbo`, uniformly for both
backends so the two numbers stay comparable.

## Other constraints the harness must respect

- `eglMakeCurrent` requires **draw == read** and rejects `EGL_NO_SURFACE` with
  `EGL_BAD_MATCH`. dEQP's `surfaceless` platform is therefore unusable, which is
  why this port supplies its own `tcu::Platform`.
- MobileGL aborts during static teardown (`FORTIFY: pthread_mutex_lock called on a
  destroyed mutex`) *after* all work completes. Flush and `_exit()` so the exit
  code and the `.qpa` log survive.

## Contents

    probe/mgprobe.c        preflight gate: one backend x one surface type, checks
                           context version/profile and both readback paths
    scripts/qpa_report.py  .qpa -> pass rate, status histogram, worst groups

### Preflight

    aarch64-linux-android26-clang -O1 -o mgprobe mgprobe.c -ldl -llog -landroid -lmediandk
    adb push mgprobe libMobileGL.so /data/local/tmp/mgcts/
    adb shell 'cd /data/local/tmp/mgcts && LD_LIBRARY_PATH=. ./mgprobe \
        --backend DirectVulkan --surface imagereader --lib ./libMobileGL.so'

Exit status is 0 when a 3.3 core context came up and FBO readback is correct.
Default-framebuffer readback is reported but deliberately does not gate.
