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
- minecraft-1.21.4-in-world: captured from Minecraft 1.21.4 after entering a singleplayer world.
  ![Minecraft 1.21.4 in-world golden](fixtures/minecraft-1.21.4-in-world.0000280000.png)
- minecraft-1.21.4-fabric-sodium-in-world: captured from Minecraft 1.21.4 Fabric with Sodium after entering a
  singleplayer world with Fancy graphics.
  ![Minecraft 1.21.4 Fabric Sodium in-world golden](fixtures/minecraft-1.21.4-fabric-sodium-in-world.0000923340.png)
- minecraft-1.21.4-fabric-iris-bsl-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and BSL
  Shaders after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris BSL in-world golden](fixtures/minecraft-1.21.4-fabric-iris-bsl-in-world.0000110725.png)
- minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  MakeUP UltraFast after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris MakeUP UltraFast in-world golden](fixtures/minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world.0000095322.png)
- minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris,
  and Super Duper Vanilla after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Super Duper Vanilla in-world golden](fixtures/minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world.0000141559.png)
- minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world: captured from Minecraft 1.21.4 Fabric with Sodium,
  Iris, and Complementary Reimagined after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Complementary Reimagined in-world golden](fixtures/minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world.0000151297.png)
- minecraft-1.21.4-fabric-iris-complementary-unbound-in-world: captured from Minecraft 1.21.4 Fabric with Sodium,
  Iris, and Complementary Unbound after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Complementary Unbound in-world golden](fixtures/minecraft-1.21.4-fabric-iris-complementary-unbound-in-world.0000146559.png)
- minecraft-1.21.4-fabric-iris-mellow-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and Mellow
  after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Mellow in-world golden](fixtures/minecraft-1.21.4-fabric-iris-mellow-in-world.0000096143.png)
- minecraft-1.21.4-fabric-iris-nostalgia-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  Nostalgia after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Nostalgia in-world golden](fixtures/minecraft-1.21.4-fabric-iris-nostalgia-in-world.0000153808-linux-mesa.png)
- minecraft-1.21.4-fabric-iris-bliss-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and Bliss
  after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Bliss in-world golden](fixtures/minecraft-1.21.4-fabric-iris-bliss-in-world.0000113511.png)
- minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  Chocapic V6 Lite after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Chocapic V6 Lite in-world golden](fixtures/minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world.0000125124-linux-mesa.png)
- minecraft-1.21.4-fabric-iris-iterationt-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  iterationT after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris iterationT in-world golden](fixtures/minecraft-1.21.4-fabric-iris-iterationt-in-world.0000110538.png)
- minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  iterationT after entering a singleplayer world, with Iris' DSA path disabled.
  ![Minecraft 1.21.4 Fabric Iris iterationT no-DSA in-world golden](fixtures/minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world.0000115019.png)
- minecraft-1.21.4-fabric-iris-photon-v1.1-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  Photon v1.1 after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Photon v1.1 in-world golden](fixtures/minecraft-1.21.4-fabric-iris-photon-v1.1-in-world.0000159866.png)
- minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  Photon v1.3b after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Photon v1.3b in-world golden](fixtures/minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world.0000172128.png)
- minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world: captured from Minecraft 1.21.4 Fabric with Sodium,
  Iris, and Derivative Main d24.4.14 after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Derivative Main d24.4.14 in-world golden](fixtures/minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world.0000145353.png)
- minecraft-1.21.4-fabric-iris-sundial-lite-in-world: captured from Minecraft 1.21.4 Fabric with Sodium, Iris, and
  Sundial Lite after entering a singleplayer world.
  ![Minecraft 1.21.4 Fabric Iris Sundial Lite in-world golden](fixtures/minecraft-1.21.4-fabric-iris-sundial-lite-in-world.0000150023.png)

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
  --ssim-threshold 0.99
```

## Android device replay

Build and install a trace APK from the repository root:

```sh
gradle --no-daemon -p android-plugin :app:assembleEsprytTraceDebug
adb install -r android-plugin/app/build/outputs/apk/esprytTrace/debug/MobileGL-EsprytTrace-debug.apk
```

Prepare a fixture and copy it into the app-private directory:

```sh
mkdir -p /tmp/mobilegl-openra
tar -xzf tools/trace_replay/fixtures/openra.tgz -C /tmp/mobilegl-openra
adb push /tmp/mobilegl-openra/openra.trace /data/local/tmp/mobilegl-openra.trace
adb push tools/trace_replay/fixtures/openra.0000031249.png /data/local/tmp/mobilegl-openra.golden.png

PKG=top.mobilegl.plugin.espryt.trace
APP_DIR=/data/user/0/$PKG/files/trace-replay
adb shell run-as $PKG rm -rf files/trace-replay
adb shell run-as $PKG mkdir -p files/trace-replay/input files/trace-replay/output
adb shell run-as $PKG cp /data/local/tmp/mobilegl-openra.trace files/trace-replay/input/openra.trace
adb shell run-as $PKG cp /data/local/tmp/mobilegl-openra.golden.png files/trace-replay/input/openra.golden.png
```

Launch the standalone trace runner Activity:

```sh
adb shell am start -W -a top.mobilegl.plugin.TRACE_REPLAY \
  -n $PKG/top.mobilegl.plugin.trace.TraceReplayActivity \
  --es trace_path $APP_DIR/input/openra.trace \
  --es golden_path $APP_DIR/input/openra.golden.png \
  --es output_dir $APP_DIR/output \
  --es diff_path $APP_DIR/output/openra-diff.png \
  --es backend DirectGLES \
  --el target_call 31249 \
  --ei width 640 \
  --ei height 480 \
  --ei crop_x 1 \
  --ei crop_y 1 \
  --ei crop_width 638 \
  --ei crop_height 478 \
  --es ssim_threshold 0.99
```

Read back the result and images:

```sh
adb shell run-as $PKG cat files/trace-replay/output/result.json
adb exec-out run-as $PKG cat files/trace-replay/output/actual.png > openra-actual.png
adb exec-out run-as $PKG cat files/trace-replay/output/openra-diff.png > openra-diff.png
```

For the Vulkan backend, build and install `:app:assembleMagmaTraceDebug`, set
`PKG=top.mobilegl.plugin.magma.trace`, and pass `--es backend DirectVulkan`.
DirectGLES also renders to the Activity surface by default; pass
`--ez use_pbuffer true` to use the offscreen pbuffer path.
