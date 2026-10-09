# Building MobileGL

MobileGL builds with CMake (all platforms) and with Gradle for the Android packages. It is C++23.
The commands below come from the project's CI workflows (`.github/workflows/test.yml` and
`apk.yml`), which build these exact shapes.

## Get the sources

```sh
git clone https://github.com/MobileGL-Dev/MobileGL.git
```

```sh
git -C MobileGL submodule update --init --recursive
```

glslang needs its external sources (SPIRV-Tools and SPIRV-Headers) fetched once:

```sh
cd MobileGL/3rdparty/glslang && python3 update_glslang_sources.py
```

Without this step, configuring fails with a message that SPIR-V tools were not found.

Large trace fixtures in the repository are stored with Git LFS. You do not need them to build. If
cloning or adding a worktree fails while fetching them, set `GIT_LFS_SKIP_SMUDGE=1`.

## Toolchains

| Target | Compiler | Other tools |
|---|---|---|
| Linux | Clang (CI: clang 20 with lld). The test and benchmark targets require Clang. | CMake 3.22.1 or newer (CI pins **3.31.10**), Ninja, Python 3, Vulkan development files, EGL and GLES development headers (Ubuntu: `libvulkan-dev libegl-dev libgles-dev`). For the GBM module: `gbm.h` and `gbm_backend_abi.h` from your distribution's GBM development package. |
| Android | NDK `27.3.13750724` (the version Gradle pins) | JDK 17, Gradle (CI: 8.10.2; the plugin uses Android Gradle Plugin 8.6.0), Android SDK platform 34, the SDK's CMake `3.22.1` |
| Windows | MSVC (Visual Studio 2022). See the build status note in [windows.md](windows.md). | CMake, Python 3, Vulkan SDK |

The minimum Android API level is **26**. Configuring for a lower level is an error; a higher level is
allowed, but the code must not use newer APIs. A separate API 29 build is planned and not available
yet.

## Linux

Shipping-style monolith library:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

```sh
cmake --build build --target MobileGL
```

Split build (`inproc`, `spawn`, TCP; adds the server executable and the GBM module):

```sh
cmake -S . -B build-split -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

```sh
cmake --build build-split --target MobileGL MobileGLServer mobilegl_gbm
```

Drop `mobilegl_gbm` from the target list if configure printed
`gbm.h / gbm_backend_abi.h not found, mobilegl_gbm is not built`.

`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is what CI passes. It is required only when configuring with
CMake 4.x, which refuses some submodules' old minimum versions.

For an aarch64 Linux target (for example a Linux container on an Android device), cross-compile with
your own CMake toolchain file and a sysroot of the target distribution (`--toolchain <file>`); there is
nothing MobileGL-specific about it. Build the client with `MOBILEGL_BUILD_DISAGGREGATED=ON`.

### Tests

Tests need Clang. Configure without the `MOBILEGL_BUILD_TEST=OFF` /
`MOBILEGL_BUILD_BENCHMARK=OFF` options (both default to `ON`; configure then downloads googletest and
Google Benchmark), build everything, and run the unit label. Use **CMake 3.31.x** for this: CI pins
3.31.10, because the test registrations carry escaped environment and label lists that newer CMake
releases handle differently. Under CMake 4.x, tests can start with the wrong environment.

```sh
cmake --build build
```

```sh
ctest --test-dir build --output-on-failure -L unit
```

GPU integration tests need `-DMOBILEGL_BUILD_INTEGRATION_TEST=ON` and a working GPU driver. Their
label is `integration-gpu`. Do not run two `ctest` suites at the same time.

## Android: direct CMake (NDK)

For embedding the library in your own app, or for the server of an app that hosts one:

```sh
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
```

```sh
cmake --build build-android --target MobileGL MobileGLServer
```

Tests and benchmarks are always off on Android. Package `libMobileGL.so` (and, if you run the
offscreen server, `libMobileGLServer.so`) under the APK's `lib/arm64-v8a/`. `libMobileGLServer.so`
is an executable with a library name, because that is the only place in an APK from which Android
lets an app run executables. Extract native libraries at install time
(`useLegacyPackaging = true`) so it exists on disk.

## Android plugin APK

The Gradle project is `android-plugin/` (root project `MobileGLPlugin`, modules `:app` and
`:MobileGL`, which is the repository root). It has no Gradle wrapper; use a Gradle installation
(CI uses 8.10.2). The build needs network access to Google's Maven repository, Maven Central and
`jitpack.io` (for the renderer-plugin build DSL).

`android-plugin/local.properties` (or `ANDROID_HOME`) must name the SDK. If the NDK or the SDK's CMake
is missing:

```sh
sdkmanager "ndk;27.3.13750724" "cmake;3.22.1"
```

Release build, with `inproc` in the launcher environment (the same command CI runs):

```sh
gradle -p android-plugin :app:assemblePluginRelease -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON
```

Output: `android-plugin/app/build/outputs/apk/plugin/release/MobileGL-plugin-release-<git-hash>.apk`.

A debug build, debug-signed and installable as is:

```sh
gradle -p android-plugin :app:assemblePluginDebug -Pmobilegl.buildDisaggregated=ON
```

Its file is also named `MobileGL-plugin-release-<git-hash>.apk`, but it is in `.../apk/plugin/debug/`.

### Signing

A release APK is signed only when `android-plugin/keystore.jks` exists **and** the environment
variables `SIGNING_STORE_PASSWORD`, `SIGNING_KEY_ALIAS` and `SIGNING_KEY_PASSWORD` are set (they belong
to the project's own key). Otherwise the release APK is unsigned; sign it with your own key:

```sh
apksigner sign --ks my-release.jks --out MobileGL-plugin-signed.apk MobileGL-plugin-release-<git-hash>.apk
```

### FCL-embedded library

When MobileGL is a module of an FCL build (see [android-fcl.md](android-fcl.md)), FCL's Gradle build
compiles it. The same `mobilegl.*` properties apply when passed to FCL's Gradle; the differences from
the plugin are in the table below.

## Gradle properties

Pass with `-P<name>=<value>`. Some can also come from an environment variable.

| Property | Env fallback | Default | Effect |
|---|---|---|---|
| `mobilegl.enableLto` | `MOBILEGL_ENABLE_LTO` | `ON` | ThinLTO and `-O3` for the native library. `OFF` builds faster. |
| `mobilegl.buildDisaggregated` | `MOBILEGL_BUILD_DISAGGREGATED` | plugin: `ON`; FCL-embedded: `OFF` | Build the split layer and the server. In the plugin, passing it explicitly as `ON` also adds `MOBILEGL_TRANSPORT=inproc` to the launcher environment. |
| `mobilegl.buildDisaggregatedInproc` | `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | follows `mobilegl.buildDisaggregated` | Implies the above; the same launcher-environment effect. |
| `mobilegl.abis` | `MOBILEGL_ABIS` | `arm64-v8a` | Comma-separated ABIs, or `all` (= `arm64-v8a,x86_64`). Plugin only; the FCL-embedded build is always arm64-v8a. |
| `mobilegl.logLevel` | `MOBILEGL_LOG_ACTIVE_LEVEL` | `MOBILEGL_LOG_LEVEL_INFO` | Lowest log level compiled in: `MOBILEGL_LOG_LEVEL_DEBUG`, `_INFO`, `_WARN`, `_ERROR`, `_FATAL` |
| `mobilegl.cmakeCompilerLauncher` | `MOBILEGL_CMAKE_COMPILER_LAUNCHER` | none | Compiler launcher, for example `ccache` |
| `mobilegl.applicationIdSuffix` | | none | Suffix for the plugin's application ID, to install next to another copy |
| `mobilegl.apkSuffix` | `MOBILEGL_APK_SUFFIX` | git short hash | Suffix in the APK file name |
| `mobilegl.debuggableRelease` | | `false` | Make the release build debuggable |

A `-P` property wins over its environment variable. The plugin's Gradle scripts and the library's script
all read the `MOBILEGL_BUILD_DISAGGREGATED` and `MOBILEGL_BUILD_DISAGGREGATED_INPROC` fallbacks.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `MOBILEGL_BUILD_DISAGGREGATED` | `OFF` | Build the split layer (`inproc`, `spawn`, TCP), the server executable `libMobileGLServer.so` (not on Windows) and, on Linux, the shared-image API and `mobilegl_gbm.so`. Needs the `3rdparty/flatbuffers` submodule; without it the option is turned off with a warning. |
| `MOBILEGL_BUILD_DISAGGREGATED_INPROC` | `OFF` | Implies `MOBILEGL_BUILD_DISAGGREGATED`. CI and Gradle set both. |
| `MOBILEGL_ENABLE_LTO` | `OFF` | ThinLTO (or the compiler's IPO) plus `-O3`, section garbage collection and identical-code folding. Gradle turns it on. |
| `MOBILEGL_FORCE_RELEASE_OPT` | `ON` | Apply the `MOBILEGL_ENABLE_LTO` flags in Debug builds too. Has no effect without `MOBILEGL_ENABLE_LTO=ON`. |
| `MOBILEGL_LOG_ACTIVE_LEVEL` | `MOBILEGL_LOG_LEVEL_INFO` | Lowest log level compiled in |
| `MOBILEGL_BUILD_TEST` | `ON` (forced `OFF` on Android) | Unit tests (Clang only) |
| `MOBILEGL_BUILD_BENCHMARK` | `ON` (forced `OFF` on Android) | Benchmarks (Clang only) |
| `MOBILEGL_BUILD_INTEGRATION_TEST` | `OFF` | GPU integration tests |
| `MOBILEGL_BUILD_TRACE_REPLAY` | `OFF` | apitrace replay runner (needs the `3rdparty/apitrace` submodule) |
| `MOBILEGL_ENABLE_TRACY` | `OFF` | Tracy profiler |
| `MOBILEGL_BUILD_STAMP` | empty (git commit) | Build identity used by the client/server handshake when `git rev-parse HEAD` does not work in the source tree |

## Artifacts

| Build | Files |
|---|---|
| Linux | `libMobileGL.so`, `libMobileGL_s.a`, symlinks `libEGL.so`, `libEGL.so.1`, `libGL.so`, `libGL.so.1`, `libGLX_mobilegl.so.0`; split: `libMobileGLServer.so`, `MobileGL/MG_Gbm/mobilegl_gbm.so` |
| Windows | `MobileGL.dll`, `opengl32.dll` (a copy), `MobileGL_s.lib` |
| Android (CMake) | `libMobileGL.so`; split: `libMobileGLServer.so` |
| Android (Gradle) | `MobileGL-plugin-release-<hash>.apk` (plugin flavor), `MobileGL-plugin-trace-release-<hash>.apk` (trace flavor, development only) |

Client and server talk only if they come from the same source revision (the wire layout must match;
for a different build of a compatible wire, the client logs a warning). Build both ends from one commit.
