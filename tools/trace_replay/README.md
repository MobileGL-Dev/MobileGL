# MobileGL trace replay

This directory builds a Linux command line replay runner for apitrace files. It is meant for CI coverage of MobileGL
without Android, APK packaging, FCL, or plugin runtime dependencies.

The bundled `openra` fixture comes from GL4ES' apitrace corpus. See
[GL4ES_TRACES.md](GL4ES_TRACES.md) for the current inventory and the traces that
were rejected because they rely on compatibility/fixed-function OpenGL.

Build from the MobileGL repository root:

```sh
cmake -S . -B build-test -G Ninja \
  -DMOBILEGL_BUILD_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF \
  -DMOBILEGL_BUILD_TRACE_REPLAY=ON
cmake --build build-test
```

Run the OpenRA fixture test:

```sh
ctest --test-dir build-test -V -R MobileGLTraceReplay.OpenRA
```

Run the CLI directly:

```sh
build-test/tools/trace_replay/mobilegl_trace_replay \
  --trace openra.trace \
  --golden openra.0000031249.png \
  --output out/openra \
  --backend DirectGLES \
  --mobilegl-library build-test/libMobileGL.so \
  --target-call 31249 \
  --width 640 \
  --height 480 \
  --crop-x 1 \
  --crop-y 1 \
  --crop-width 638 \
  --crop-height 478 \
  --tolerance 20 \
  --fuzz-percent 20
```

The Linux runner loads MobileGL by path with `dlopen`. It uses an EGL pbuffer for `DirectGLES` and maps the pbuffer
path to `VK_EXT_headless_surface` for `DirectVulkan`, so CI does not need an X11 or Wayland window.
