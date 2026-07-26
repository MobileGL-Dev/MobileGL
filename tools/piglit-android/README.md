# piglit on Android against MobileGL

Run [piglit](https://gitlab.freedesktop.org/mesa/piglit) desktop-GL tests on a
connected Android device with **MobileGL as the OpenGL implementation**, for
both backends:

- `DirectGLES` — MobileGL over the system GLES driver (or ANGLE with
  `--use-angle`)
- `DirectVulkan` — MobileGL over the system Vulkan driver

No APK and no on-device python: piglit test binaries run as the adb shell user
from `/data/local/tmp`, create contexts through waffle's `surfaceless_egl`
platform, and waffle is pointed at `libMobileGL.so` (which exports the full
`egl*`/`gl*` API under real names).

## How it fits together

```
piglit test binary (aarch64, bionic)
  └─ waffle surfaceless_egl (patched)
       ├─ WAFFLE_EGL_LIBRARY=libMobileGL.so   → dlopen MobileGL as the EGL impl
       ├─ WAFFLE_GL_LIBRARY=libMobileGL.so    → waffle_dl_sym resolves gl* here
       ├─ WAFFLE_FORCE_GL_CONTEXT_VERSION=33core
       │    upgrades low compat context requests to GL 3.3 core (never
       │    downgrades) so piglit's supports_gl_compat_version=10 tests run
       └─ WAFFLE_ANDROID_WINDOW=imagereader   (DirectVulkan only)
            windows are AImageReader ANativeWindows instead of EGL pbuffers,
            because Android ICDs lack VK_EXT_headless_surface which the
            MobileGL pbuffer path needs
  └─ libMobileGL.so
       ├─ DirectGLES: dlopens the real system libEGL.so internally
       └─ DirectVulkan: links libvulkan.so
```

Key rule: **never name MobileGL `libEGL.so`** anywhere on `LD_LIBRARY_PATH` —
the DirectGLES backend loads the system driver with a bare-soname
`dlopen("libEGL.so")` and would recursively pick itself up.

## One-time setup (host: macOS/Linux with the Android NDK)

```sh
WORK=path/to/workdir && cd $WORK
git clone --depth 1 https://gitlab.freedesktop.org/mesa/piglit.git
git clone --depth 1 https://gitlab.freedesktop.org/mesa/waffle.git
git -C waffle apply $MOBILEGL/tools/piglit-android/patches/waffle-mobilegl-android.patch
git -C piglit apply $MOBILEGL/tools/piglit-android/patches/piglit-mobilegl-android.patch
python3 -m venv venv && ./venv/bin/pip install mako numpy packaging
```

The piglit patch matters beyond build fixes: upstream's
`piglit_dispatch_default_init` runs while the waffle framework is still being
constructed (`gl_fw` is NULL), so the waffle resolvers were never installed and
GL functions bound through the **system** libEGL's `eglGetProcAddress` — every
test silently ran on the raw GLES driver instead of MobileGL.

Build MobileGL for Android:

```sh
cmake -S $MOBILEGL -B $MOBILEGL/build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build $MOBILEGL/build-android-arm64 --target MobileGL -j
$NDK/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded \
  -o $WORK/libMobileGL-stripped.so $MOBILEGL/build-android-arm64/libMobileGL.so
```

Cross-build waffle (meson; a cross file and a stub `egl.pc` pointing at
MobileGL's bundled EGL 1.5 headers are needed — see `cross-example/`):

```sh
cd $WORK/waffle
meson setup build-android --cross-file $WORK/cross/android-arm64.ini \
  -Dbuildtype=release -Dsurfaceless_egl=enabled \
  -Dglx=disabled -Dx11_egl=disabled -Dgbm=disabled -Dwayland=disabled \
  -Dbuild-tests=false -Dbuild-examples=false -Dprefix=$WORK/prefix
ninja -C build-android && meson install -C build-android
```

Cross-build piglit (needs `PKG_CONFIG_LIBDIR` with the installed `waffle-1.pc`
plus the stub `egl.pc`):

```sh
cd $WORK/piglit && export PKG_CONFIG_LIBDIR=$WORK/prefix/lib/pkgconfig:$WORK/cross/pkgconfig
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIGLIT_USE_WAFFLE=ON -DPIGLIT_BUILD_GL_TESTS=ON \
  -DPIGLIT_BUILD_GLES1_TESTS=OFF -DPIGLIT_BUILD_GLES2_TESTS=OFF \
  -DPIGLIT_BUILD_GLES3_TESTS=OFF -DPIGLIT_BUILD_EGL_TESTS=OFF \
  -DPIGLIT_BUILD_GLX_TESTS=OFF -DPIGLIT_BUILD_WGL_TESTS=OFF \
  -DPIGLIT_BUILD_CL_TESTS=OFF -DPIGLIT_BUILD_VK_TESTS=OFF \
  -DPIGLIT_BUILD_DMA_BUF_TESTS=OFF -DPIGLIT_USE_GBM=OFF \
  -DPIGLIT_USE_WAYLAND=OFF -DPIGLIT_USE_X11=OFF \
  -DPYTHON_EXECUTABLE=$WORK/venv/bin/python \
  -DOPENGL_INCLUDE_DIR=$MOBILEGL/include \
  -DOPENGL_gl_LIBRARY=$SYSROOT/usr/lib/aarch64-linux-android/26/libEGL.so \
  -DGLEXT_INCLUDE_DIR=$MOBILEGL/include
ninja -C build-android
```

## Selecting tests

Enumerate on the host with piglit's own profiles (no device needed):

```sh
cd $WORK/piglit
for prof in opengl shader glslparser; do
  PIGLIT_BUILD_DIR=$PWD/build-android ./venv/bin/python ./piglit print-cmd \
    -t "spec@!opengl 3[.]" -t "spec@glsl-3[.]30" $prof
done > /tmp/gl33.list
```

Group names use `@` separators (`spec@!opengl 3.3@minmax`). The version groups
(`spec@!opengl 1.x…3.3`), GLSL groups (`spec@glsl-1.10…3.30`) plus the ARB
extension groups folded into GL 3.1–3.3 core give a comprehensive "GL 3.3
core" suite (~15k tests).

## Running

```sh
python3 $MOBILEGL/tools/piglit-android/run_piglit_android.py \
  --piglit-root $WORK/piglit --list /tmp/gl33.list \
  --backend DirectGLES \
  --mobilegl-lib $WORK/libMobileGL-stripped.so \
  --waffle-lib $WORK/waffle/build-android/src/waffle/libwaffle-1.so \
  --out results-gles
# then the same with --backend DirectVulkan --out results-vk
```

The runner pushes binaries/libs/data (incremental; `--repush` forces), executes
tests serially in chunked on-device shell scripts under `timeout`, parses the
`PIGLIT: {...}` result lines, and writes `results.json` + `summary.txt` +
`raw.log`. Exit-code semantics: parsed result wins; nonzero exit without a
result line = `crash`; toybox timeout exits = `timeout`.

Quick sanity check for the whole stack (waffle build also produces `wflinfo`):

```sh
adb shell 'cd /data/local/tmp/piglit-mgl && env LD_LIBRARY_PATH=$PWD/lib \
  WAFFLE_EGL_LIBRARY=libMobileGL.so WAFFLE_GL_LIBRARY=libMobileGL.so \
  MOBILEGL_BACKEND_TYPE=DirectVulkan WAFFLE_ANDROID_WINDOW=imagereader \
  ./wflinfo --platform surfaceless_egl --api gl --version 3.3 --profile core'
```

Expect `OpenGL version string: 3.3.0 MobileGL …, Direct (Vulkan) Backend`.

## Known caveats

- MSAA winsys configs never match (MobileGL exposes two RGBA8888 configs,
  samples=0); MSAA FBO tests are unaffected.
- Tests that genuinely require compatibility-profile features will fail on the
  forced 3.3 core context; that is honest for a core-only implementation.
- `eglTerminate` at test exit now tears MobileGL down deterministically (see
  the EGL-lifecycle refactor); a device-side `mobilegl.log` is written per run
  directory for debugging.
