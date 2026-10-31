# MobileGL embedded in FCL (Android, monolith)

Some FCL (Fold Craft Launcher) builds carry MobileGL as a Gradle module and ship `libMobileGL.so`
inside the launcher APK. This page covers that embedded copy. For the separately installed renderer
plugin, see [android-plugin.md](android-plugin.md).

## When to use it

- You play Minecraft on Android through an FCL build that already lists MobileGL renderers.
- You want the simplest setup with the least overhead: everything runs inside the game process.

When not to use it:

- You want `inproc`, `spawn` or TCP. The embedded copy is built **without** the split layer
  (`MOBILEGL_BUILD_DISAGGREGATED=OFF`), so `MOBILEGL_TRANSPORT` is ignored and it always runs
  monolith. Use the [plugin APK](android-plugin.md) instead.
- Your launcher does not embed MobileGL. Use the plugin.

## Requirements

- Android 8.0 (API 26) or newer, arm64-v8a. The embedded build packages arm64-v8a only.
- Espryt needs an OpenGL ES 3.x driver. Magma needs a Vulkan driver.

## How it is built and integrated

The FCL source tree has MobileGL as a git submodule and Gradle module:

| Where | What |
|---|---|
| `.gitmodules` | submodule `MobileGL` -> `https://github.com/MobileGL-Dev/MobileGL.git` |
| `settings.gradle.kts` | `include(":MobileGL")` |
| `FCLauncher/build.gradle.kts` | `implementation(project(":MobileGL"))` |

MobileGL's `build.gradle` detects that it is not the standalone plugin build (the root project is
not `MobileGLPlugin`) and then:

- builds arm64-v8a only;
- builds without the split layer (`MOBILEGL_BUILD_DISAGGREGATED=OFF`), so there is no
  `libMobileGLServer.so`;
- builds with ThinLTO and `-O3` (`MOBILEGL_ENABLE_LTO=ON`) unless you pass
  `-Pmobilegl.enableLto=OFF`;
- uses NDK `27.3.13750724` and the Android SDK's CMake `3.22.1`.

Before the first build, initialize MobileGL's submodules and glslang's external sources. Run these
inside the FCL checkout:

```sh
git submodule update --init --recursive
```

```sh
cd MobileGL/3rdparty/glslang && python3 update_glslang_sources.py
```

Then build FCL as its own documentation describes. The `mobilegl.*` Gradle properties from
[building.md](building.md#gradle-properties) apply when passed to FCL's Gradle invocation (for
example `-Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_DEBUG`).

## Choosing the renderer and backend

In FCL, open the game settings and pick the renderer under **OpenGL Implementation (Renderer)**. The
embedded copy appears as two entries. FCL sets the backend from the entry you pick:

| FCL entry | Sets |
|---|---|
| `MobileGL (Dev, Espryt)` | `MOBILEGL_BACKEND_TYPE=DirectGLES` |
| `MobileGL (Dev, Magma)` | `MOBILEGL_BACKEND_TYPE=DirectVulkan` |

The entry names above are from the FCL build that this guide was checked against; other FCL builds
may label them differently.

Which backend to pick: try Espryt first (the default). Use Magma if your device's Vulkan driver is
better than its GLES driver. Compare with the in-game frame rate; there is no universal answer.

## Other settings

FCL's **Environment Variables** editor (game settings, advanced section) takes one `KEY=VALUE` per
line. FCL applies it after the renderer's own variables, so it can also override
`MOBILEGL_BACKEND_TYPE`. Example:

```text
MOBILEGL_FRAMES_IN_FLIGHT=2
MOBILEGL_RELAXED_SEMANTICS=1
```

Useful keys are listed in [configuration.md](configuration.md). The FCL build checked for this guide
stores this editor's contents launcher-wide, not per instance.

Without access to the launcher's settings (for example while testing a launcher you cannot
rebuild), Android system properties work from `adb`, with no root needed:

| Property | Effect |
|---|---|
| `debug.mobilegl.backend` | Overrides `MOBILEGL_BACKEND_TYPE`, even when the launcher set it. |
| `debug.mobilegl.env` | `KEY=VALUE;KEY=VALUE` list of settings. It ranks **below** the process environment: it fills in only keys the launcher did not set. It holds at most 91 characters (Android's property length limit). |
| `debug.mobilegl.fps_log` | `1` logs the frame rate about every two seconds (same as `MOBILEGL_FPS_LOG=1`). |

```sh
adb shell setprop debug.mobilegl.backend DirectVulkan
```

```sh
adb shell setprop debug.mobilegl.env "MOBILEGL_FRAMES_IN_FLIGHT=2;MOBILEGL_RELAXED_SEMANTICS=1"
```

These properties apply to every app on the device that loads MobileGL, and they last until reboot.
Clear one by setting it to an empty string:

```sh
adb shell setprop debug.mobilegl.env ""
```

`debug.mobilegl.env` covers most settings in [configuration.md](configuration.md). The ones marked
*env only* there are read straight from the process environment and ignore it, notably
`MOBILEGL_LOG_FILE_PATH` and `MOBILEGL_IPC_TOKEN`. `MOBILEGL_FPS_LOG` and
`MOBILEGL_GPU_HANG_BUDGET_MS` have their own properties (`debug.mobilegl.fps_log`,
`debug.mobilegl.gpu_hang_budget_ms`).

## How to tell it works

- Minecraft's F3 screen shows `Espryt (MobileGL Core)` or `Magma (MobileGL Core)` as the renderer.
- logcat shows MobileGL's lines:

```sh
adb logcat -s MobileGL
```

  Look for `Config: Active backend type set to DirectGLES` (or `DirectVulkan`).
- MobileGL also writes `/sdcard/MG/latest.log`, if the process may write there.
