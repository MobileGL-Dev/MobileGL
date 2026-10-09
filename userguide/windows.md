# MobileGL on Windows (WGL `opengl32.dll` drop-in)

On Windows, MobileGL builds as `MobileGL.dll` plus an identical copy named `opengl32.dll`. The copy
exports the WGL entry points (`wglCreateContext`, `wglGetProcAddress`, `wglSwapBuffers`, ...) and
the OpenGL functions. A program that loads `opengl32.dll` from its own directory gets MobileGL
instead of the system OpenGL driver.

## When to use it

- You want to run a Windows OpenGL program (for example Minecraft through GLFW/LWJGL) on MobileGL,
  to test MobileGL or to compare its backends on desktop hardware.

When not to use it:

- Split rendering. On Windows only monolith is supported; the server executable is not built for
  Windows.
- As a system-wide driver. Do not replace `C:\Windows\System32\opengl32.dll`. The drop-in works per
  application directory.

> **Build status on the current branch:** building `feat/disaggregated` with MSVC 19.38
> (Visual Studio 2022 17.8) fails with `error C2610` in `MobileGL/MG_Util/Damage/Damage.h` (a
> defaulted `operator==`) and the same error in several Magma headers. Newer MSVC toolsets were not
> tested. The steps below are the ones the Windows tooling in this repository uses
> (`tools/cts/scripts/wgl_glcts_pipeline.py`); they apply once the tree builds with your compiler.

## Requirements

- Windows x64, Visual Studio 2022 with the Desktop C++ workload, CMake, Python 3, Git.
- The Vulkan SDK, so that CMake's `find_package(Vulkan)` finds it.
- For **Magma**: a Vulkan driver for your GPU (`vulkan-1.dll` and a vendor ICD). The SDK alone
  provides no GPU device.
- For **Espryt**: Espryt on Windows always runs on ANGLE. Put matching x64 `libEGL.dll`,
  `libGLESv2.dll` and `d3dcompiler_47.dll` from one ANGLE build next to the program.

## Build

Prepare the sources first (submodules and glslang's external sources, see
[building.md](building.md#get-the-sources)). Then, from the repository root:

```powershell
cmake -S . -B C:\mgl-build -G "Visual Studio 17 2022" -A x64 -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF
```

```powershell
cmake --build C:\mgl-build --config Release --target MobileGL
```

Keep the build directory path short: a full build under a deep path can exceed Windows' path length
limit. The result is `C:\mgl-build\Release\MobileGL.dll` and `C:\mgl-build\Release\opengl32.dll`
(with the Ninja generator, both are directly in the build directory).

## Install

Copy `opengl32.dll` into the directory of the executable that creates the GL context. Windows
searches the application's directory before `System32`, so the copy wins.

| Program | Where `opengl32.dll` goes |
|---|---|
| A native `.exe` | next to the `.exe` |
| A Java program (Minecraft) | next to the `java.exe` / `javaw.exe` that runs it, in the runtime's `bin` directory. Use a private copy of the Java runtime so other programs are not affected. |

For Espryt, put the three ANGLE DLLs in the same directory.

## Configure and run

Settings are environment variables ([configuration.md](configuration.md)). Set them in the shell
that starts the program, or in your launcher's environment settings:

```powershell
$env:MOBILEGL_BACKEND_TYPE = "DirectVulkan"
```

```powershell
$env:MOBILEGL_LOG_FILE_PATH = "$PWD\mobilegl.log"
```

MobileGL does not log to the console on Windows, and writes no log file unless
`MOBILEGL_LOG_FILE_PATH` is set.

## How to tell it works

- The program reports `GL_RENDERER` = `Magma (MobileGL Core) (...)` or `Espryt (MobileGL Core) (...)`.
  In Minecraft, look at the F3 screen.
- The log file exists and contains `Config: Active backend type set to DirectVulkan`.
- For Espryt, the log has `Loaded GL backend library: libGLESv2.dll` / `libEGL.dll`.
- `tools/wgl-smoke/` has two small test programs (a hand-written WGL bootstrap and a GLFW one) that
  render a triangle and check it with `glReadPixels`. Exit code 0 means pass. Build instructions are
  in `tools/wgl-smoke/README.md`.
- `tools/cts/scripts/wgl_glcts_pipeline.py` runs the Khronos OpenGL conformance suites against the
  drop-in. Its preflight checks that the suite really loaded MobileGL.

If the program still reports your GPU vendor's renderer, it loaded the system `opengl32.dll`:
check that the copy sits next to the executable that actually creates the context.
