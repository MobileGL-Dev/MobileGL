---
name: piglit-on-android
description: Run piglit desktop-GL tests on a connected Android device with MobileGL as the OpenGL implementation - cross-build waffle (patched) and piglit for aarch64, push to /data/local/tmp, execute over adb against the DirectGLES (system driver or ANGLE) and DirectVulkan backends, and produce pass/fail/crash summaries. Use when validating MobileGL's OpenGL 3.3 core conformance on-device or bisecting a piglit regression.
---

# piglit on Android against MobileGL

## Variables

```sh
export REPO="$PWD"                        # MobileGL checkout
export WORK="$HOME/piglit-android"        # piglit/waffle workdir (outside the repo)
export NDK="$HOME/Library/Android/sdk/ndk/27.3.13750724"
export TOOLS="$REPO/tools/piglit-android"
export DEVICE_DIR="/data/local/tmp/piglit-mgl"
```

## Architecture (read first)

- MobileGL exports the full `egl*`/`gl*` API with real symbol names from
  `libMobileGL.so`; waffle's `surfaceless_egl` platform is pointed at it via
  `WAFFLE_EGL_LIBRARY=libMobileGL.so` and `WAFFLE_GL_LIBRARY=libMobileGL.so`.
- **Never ship MobileGL under the name `libEGL.so`**: the DirectGLES backend
  dlopens the system driver by the bare soname `libEGL.so` and would load
  itself recursively.
- `WAFFLE_FORCE_GL_CONTEXT_VERSION=33core` (patched waffle) upgrades piglit's
  low compat context requests (`supports_gl_compat_version=10`) to GL 3.3
  core; without it those tests see a raw backend version string and skip.
- DirectVulkan cannot use MobileGL's EGL-pbuffer path on real devices (Android
  ICDs lack `VK_EXT_headless_surface`), so the patched waffle creates windows
  from an `AImageReader` ANativeWindow: `WAFFLE_ANDROID_WINDOW=imagereader`.
  The runner sets this automatically for `--backend DirectVulkan`.
- The piglit patch also fixes `piglit_dispatch_default_init` to always install
  the waffle resolvers; upstream silently bound gl* through the SYSTEM
  libEGL's `eglGetProcAddress`, running every test on the raw GLES driver.
- MobileGL's lifecycle is owned by the EGL layer (lazy init on first EGL call,
  full teardown on the last `eglTerminate`), so piglit processes start and
  exit cleanly with no static ctor/dtor involvement.

## Steps

1. **Clone + patch** piglit and waffle (shallow clones are fine; never pull
   git-LFS):

   ```sh
   mkdir -p "$WORK" && cd "$WORK"
   git clone --depth 1 https://gitlab.freedesktop.org/mesa/piglit.git
   git clone --depth 1 https://gitlab.freedesktop.org/mesa/waffle.git
   git -C waffle apply "$TOOLS/patches/waffle-mobilegl-android.patch"
   git -C piglit apply "$TOOLS/patches/piglit-mobilegl-android.patch"
   python3 -m venv venv && ./venv/bin/pip install mako numpy packaging
   ```

2. **Build libMobileGL.so** for arm64 (plain CMake + NDK; gradle not needed)
   and strip it. See `$TOOLS/README.md` for exact invocations.

3. **Cross-build waffle** with meson (`surfaceless_egl` enabled, everything
   else disabled) using `$TOOLS/cross-example/android-arm64.ini` and the stub
   `egl.pc` (Cflags → `$REPO/include` for EGL 1.5 headers). `meson install` to
   `$WORK/prefix` so piglit's pkg-config finds `waffle-1.pc`.

4. **Cross-build piglit** with the NDK toolchain file; the full CMake flag set
   is in `$TOOLS/README.md`. All GL test binaries land in
   `piglit/build-android/bin` (~1600), shared utils in `lib/`.

5. **Smoke test** with wflinfo on both backends before any long run:

   ```sh
   adb shell "cd $DEVICE_DIR && env LD_LIBRARY_PATH=$DEVICE_DIR/lib \
     WAFFLE_EGL_LIBRARY=libMobileGL.so WAFFLE_GL_LIBRARY=libMobileGL.so \
     MOBILEGL_BACKEND_TYPE=DirectVulkan WAFFLE_ANDROID_WINDOW=imagereader \
     ./wflinfo --platform surfaceless_egl --api gl --version 3.3 --profile core"
   ```

   Expect `3.3.0 MobileGL … Direct (Vulkan) Backend` and exit code 0.

6. **Enumerate tests on the host** with `piglit print-cmd` (profiles: `opengl`,
   `shader`, `glslparser`; group separator is `@`). For the GL 3.3 core suite
   use the version groups + GLSL groups + the ARB extension groups folded into
   3.1–3.3 core (~15k tests).

7. **Run per backend** with the driver script (serial, chunked, per-test
   timeout; DirectGLES first, then DirectVulkan):

   ```sh
   python3 "$TOOLS/run_piglit_android.py" \
     --piglit-root "$WORK/piglit" --list gl33-core.list \
     --backend DirectGLES \
     --mobilegl-lib "$WORK/libMobileGL-stripped.so" \
     --waffle-lib "$WORK/waffle/build-android/src/waffle/libwaffle-1.so" \
     --out results-gles
   ```

   `--use-angle` switches DirectGLES to packaged ANGLE; default is the system
   GLES driver. `--repush` forces re-pushing bin/lib/tests trees (needed after
   rebuilds or if the device wiped `/data/local/tmp`).

8. **Compare backends**: `results.json` has per-test status
   (`pass/fail/skip/crash/timeout/missing`) and output tails for non-passes;
   `summary.txt` lists the bad tests. Diff the two runs' failing sets to
   separate frontend issues (fail on both) from backend-specific ones.

## Gotchas

- A test crash after `PIGLIT: {"result": ...}` was printed is classified by
  the result line, not the exit code, except that unknown nonzero exits count
  as `crash`.
- The device may clear `/data/local/tmp` (vendor cleaners); `--repush` recovers.
- Keep chunks ≤ ~250 tests: one adb connection per chunk bounds the damage of
  USB hiccups, and progress prints per chunk.
- MobileGL writes `$DEVICE_DIR/mobilegl.log` (set by the runner); check it and
  `adb logcat -b crash` when triaging crashes.
