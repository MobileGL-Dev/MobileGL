<h1 align="center">MobileGL</h1>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B-00599c?style=flat&logo=c%2B%2B" alt="C++">
  <img src="https://img.shields.io/badge/License-GNU%20LGPL%203.0-00399c?style=flat" alt="GNU LGPL 3.0">
  <img src="https://img.shields.io/badge/Status-Development-0078d7?style=flat" alt="Development">
</p>

<p align="center"><em>
A desktop OpenGL implementation
</em></p>

MobileGL is a *free* and *open-source* project that implements a desktop **OpenGL** API. The goal is to provide a complete desktop OpenGL implementation with a state management layer and multi-backend support.

> [!NOTE]
>
> **Status:** In development. Parts of the codebase are incomplete. Current short-term target: **OpenGL 4.2 (Core Profile)**.

## Project positioning

MobileGL is an implementation of a desktop OpenGL library. It aims to provide:

* Full OpenGL state management.
* A front-end that exposes OpenGL functions.
* Multiple independent backend implementations, where each backend targets a specific graphics API and remains fully isolated from others.

This project is intended as an implementation/translation layer.

## Key components

The repository is organized into following top-level modules:

1. **MG_State** — state tracking and management logic for Graphics APIs.
2. **MG_Impl** — front-end implementations of Graphics APIs that interact with `MG_State` and `MG_Backend`.
3. **MG_Backend** — per-backend translation layer that maps front-end Graphics APIs' semantics and state into concrete backend API calls (e.g. OpenGL ES, Vulkan).
4. **MG_Util** and other utility modules.

## Third-party components

MobileGL reuses several open-source projects:

* **SPIRV-Cross** by **KhronosGroup** - [Apache License 2.0](https://github.com/KhronosGroup/SPIRV-Cross/blob/master/LICENSE): [github](https://github.com/KhronosGroup/SPIRV-Cross)
* **glslang** by **KhronosGroup** - [Various Licenses](https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt): [github](https://github.com/KhronosGroup/glslang)
* **DiligentCore** by **Diligent Graphics** - [Apache License 2.0](https://github.com/DiligentGraphics/DiligentCore/blob/master/License.txt): [github](https://github.com/DiligentGraphics/DiligentCore)
* **flat_hash_map** by **Malte Skarupke** - [Boost Software License 1.0](https://github.com/MobileGL-Dev/flat_hash_map/blob/master/LICENSE): [github](https://github.com/MobileGL-Dev/flat_hash_map)

Refer to each component's repository for exact license texts. Any bundled third-party code in this repository is included under the upstream project's license.

## Compatibility & target

* **Short-term target:** `OpenGL 4.2 (Core Profile)`.
* **Current development focus:**
  * Performance improvement
  * `MG_State` and `MG_Impl` for `OpenGL 4.2 (Core Profile)`
  * `Direct (Vulkan)` backend
  * `Direct (OpenGL ES)` backend

## Build Instructions

We currently provide **no releases** and **no precompiled binaries**.  
If you want to try the project right now, you’ll need to build it yourself:

1. Clone the repository:

   ```sh
   git clone https://github.com/MobileGL-Dev/MobileGL.git
   ```

2. Initialize and update all submodules recursively:

   ```sh
   git submodule update --init --recursive
   ```

3. Follow glslang’s own documentation for its required initiation.

4. Configure and build the project with CMake:

   ```sh
   cmake -B build
   cmake --build build
   ```
   
   or do it in a modern way:
   
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build build
   ```
   
   Alternatively, you can use platform-specific build commands as needed.

### Build For macOS

On macOS, MobileGL can be built as a dylib that exposes the normal OpenGL/CGL/NSOpenGL entry points and routes them to the `DirectVulkan` backend. This is useful for running applications such as Minecraft through their stock GLFW/LWJGL OpenGL path while MobileGL is injected before context creation.

Prerequisites:

* macOS with Clang and Ninja.
* Vulkan loader and MoltenVK installed. With Homebrew, the MoltenVK ICD is commonly located at `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`.

Configure and build:

```sh
cmake -S . -B build-macos-magma \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOBILEGL_BACKEND_TYPE=DirectVulkan \
  -DMOBILEGL_BUILD_TEST=OFF \
  -DMOBILEGL_BUILD_BENCHMARK=OFF

cmake --build build-macos-magma --target MobileGL -j8
```

The dylib will be generated at:

```sh
build-macos-magma/libMobileGL.dylib
```

To run Minecraft by MobileGL from a launcher like PrismLauncher, keep the stock LWJGL/GLFW natives and add a wrapper command to the instance settings:

```sh
env DYLD_INSERT_LIBRARIES=/absolute/path/to/MobileGL/build-macos-magma/libMobileGL.dylib MOBILEGL_BACKEND_TYPE=DirectVulkan VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
```

Also make sure the JVM arguments include:

```sh
-XstartOnFirstThread
```

`DYLD_INSERT_LIBRARIES` must be active before GLFW creates its OpenGL context. After startup, the Minecraft F3 screen should report MobileGL and the `Direct (Vulkan)` backend if the injection worked.

## Build Options

| Option                       | Description                                           | Default |
|------------------------------| ----------------------------------------------------- | ------- |
| `MOBILEGL_BUILD_TEST`        | Build MobileGL tests (requires Clang)                 | ON      |
| `MOBILEGL_BUILD_BENCHMARK`   | Build MobileGL benchmarks (requires Clang)            | ON      |
| `MOBILEGL_FORCE_RELEASE_OPT` | Enable O3 and LTO in Debug build                      | ON      |
| `MOBILEGL_ENABLE_TRACY`      | Enable Tracy profiler for performance analysis        | OFF     |

   **Notes:**

* The project requires C++23.
* `MG_Test` and `MG_Benchmark` can only be built with Clang, not GCC. To enforce Clang, add `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` to your command.
* On Android, tests and benchmarks are always disabled.

## Environment Variables

MobileGL supports runtime configuration via environment variables.

### Supported Keys

| Variable                | Description                                      | Allowed Values                       | Default        |
|-------------------------|--------------------------------------------------|--------------------------------------|----------------|
| `MOBILEGL_BACKEND_TYPE` | Select active backend implementation at startup. | `DirectGLES`, `DirectVulkan`         | `DirectGLES`   |
| `MOBILEGL_DISABLE_TIMERQUERY` | Disable GPU timer-query exposure and use. | `0`, `1` | `0` |
| `MOBILEGL_USE_ANGLE` | Load ANGLE EGL/GLES libraries. | `0`, `1` | `0` |
| `MOBILEGL_DISABLE_SUBGROUP` | Disable Vulkan shader subgroup support. | `0`, `1` | `0` |
| `MOBILEGL_ADVERTISE_FP64` | Advertise `GL_ARB_gpu_shader_fp64`. GLSL `double`/`dvec`/`dmat` compile and run either way - they are narrowed to 32 bits - so this only changes whether an application is told it has 64-bit precision, which it does not. | `0`, `1` | `0` |
| `MOBILEGL_MAGMA_R11G11B10F_FALLBACK` | Use Magma's R11G11B10F format fallback. | `0`, `1` | `0` |
| `MOBILEGL_MAGMA_FRAMESINFLIGHT` | Set Magma frames in flight. | Integer `1`–`64` | `3` |
| `MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER` | Avoid sampler mipmap minification filters. | `0`, `1` | `0` |
| `MOBILEGL_COHERENT_AS_FLUSH` | Treat persistent `GL_MAP_FLUSH_EXPLICIT_BIT` maps as coherent (app-compat for engines like Flywheel that never flush them). | `0`, `1` | `0` |
| `MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION` | Always emulate depth/stencil `glReadPixels`/`glGetTexImage` by shader sampling on Espryt, instead of using the driver's own depth/stencil readback where it has one. | `0`, `1` | `0` |
| `VK_ICD_FILENAMES`      | Select the Vulkan ICD used by the Vulkan loader. | Path to an ICD JSON file             | Loader default |

## License

This project is distributed under **GNU LGPL v3.0**. See the `LICENSE` file in the repository for detailed information.
