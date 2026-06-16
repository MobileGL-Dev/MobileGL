# MobileGL trace replay

This directory builds a Linux command line replay runner for [apitrace](https://github.com/apitrace/apitrace) files. It
is an integration testing infrastructure of MobileGL.

The bundled fixtures cover:

- OpenRA: sourced from GL4ES' apitrace corpus.
  ![OpenRA golden](fixtures/openra.0000031249.png)
- minecraft-1.21.4-startup: captured from Minecraft 1.21.4's startup screen.
  ![Minecraft 1.21.4 startup golden](fixtures/minecraft-1.21.4-startup.0000092195.png)
- minecraft-1.21.4-main-menu: captured from Minecraft 1.21.4's main menu.
  ![Minecraft 1.21.4 main menu golden](fixtures/minecraft-1.21.4-main-menu.0000481787.png)

Build from the MobileGL repository root:

```sh
cmake -S . -B build-test -G Ninja \
  -DMOBILEGL_BUILD_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF \
  -DMOBILEGL_BUILD_TRACE_REPLAY=ON
cmake --build build-test
```

Run the fixture tests:

```sh
ctest --test-dir build-test -V -R 'MobileGLTraceReplay\.'
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
