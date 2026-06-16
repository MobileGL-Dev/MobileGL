# MobileGL trace replay

This directory builds a Linux command line replay runner for apitrace files. It is meant for CI coverage of MobileGL
without Android, APK packaging, FCL, or plugin runtime dependencies.

The bundled fixtures cover:

- `OpenRA`: sourced from GL4ES' apitrace corpus. See [GL4ES_TRACES.md](GL4ES_TRACES.md) for the current inventory and
  traces rejected because they rely on compatibility/fixed-function OpenGL.
- `minecraft-1.21.4-startup`: captured from Minecraft 1.21.4's Mojang Studios startup screen. The fixture is trimmed
  to call 92195, immediately after the startup screen draw into Minecraft's offscreen framebuffer.
- `minecraft-1.21.4-main-menu`: captured from Minecraft 1.21.4's English (US) main menu. The fixture is trimmed to
  call 481787, immediately after the main menu draw into Minecraft's offscreen framebuffer.

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

The Minecraft startup fixture uses `tools/trace_replay/fixtures/minecraft-1.21.4-startup.tgz`, golden image
`minecraft-1.21.4-startup.0000092195.png`, `--target-call 92195`, `--width 854`, and `--height 480`.
The Minecraft main-menu fixture uses `tools/trace_replay/fixtures/minecraft-1.21.4-main-menu.tgz`, golden image
`minecraft-1.21.4-main-menu.0000481787.png`, `--target-call 481787`, `--width 854`, `--height 480`, and
`--tolerance 500`.
Each replay that has a golden image writes `${case}-diff.png` in the backend output directory.

The Linux runner loads MobileGL by path with `dlopen`. It uses an EGL pbuffer for `DirectGLES` and maps the pbuffer
path to `VK_EXT_headless_surface` for `DirectVulkan`, so CI does not need an X11 or Wayland window.
