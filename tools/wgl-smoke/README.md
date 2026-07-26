# WGL smoke tests

Manual bring-up tests for the Windows host layer (drop-in `opengl32.dll`).
Both replicate the GLFW/LWJGL bootstrap Minecraft uses and verify a rendered
triangle via `glReadPixels`; exit code 0 means pass.

- `wgl_smoke.cpp` - hand-rolled Win32/WGL sequence (dummy context on a hidden
  helper window, ARB pixel format + 3.2 core forward-compatible context,
  gdi32 pixel-format forwarding, swap loop, readback).
- `glfw_smoke.cpp` - the same flow driven by a real GLFW binary, which also
  exercises GLFW's zero-area helper window (the case that needs the
  DirectVulkan no-swapchain-at-init path).

Build (x64 Native Tools prompt), with GLFW binaries for the second test:

    cl /nologo /EHsc /W3 wgl_smoke.cpp user32.lib gdi32.lib
    cl /nologo /EHsc /W3 glfw_smoke.cpp /I%GLFW%\include %GLFW%\lib-vc2022\glfw3dll.lib user32.lib gdi32.lib

Run with the MobileGL `opengl32.dll` copy (and `glfw3.dll`, plus ANGLE's
`libEGL.dll`/`libGLESv2.dll`/`d3dcompiler_47.dll` for DirectGLES) in the exe
directory:

    set MOBILEGL_BACKEND_TYPE=DirectVulkan   (or DirectGLES)
    set MOBILEGL_LOG_FILE_PATH=smoke.log
    wgl_smoke.exe && glfw_smoke.exe
