# Running the OpenGL CTS against MobileGL

This directory contains two supported paths:

- Android arm64 / MobileGL EGL: the KHR-GL33 workflow documented below and in
  `skills/gl-cts-on-mobilegl/SKILL.md`.
- Windows x64 / MobileGL WGL: the GL30-GL46 pipeline in
  `scripts/wgl_glcts_pipeline.py`, documented by
  `skills/wgl-gl-cts-on-mobilegl/SKILL.md`.

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
