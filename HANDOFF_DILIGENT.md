# Handoff: Diligent/Vulkan GL3.2 Backend for MobileGL

Date: 2026-08-18  
Branch: `feat/diligent-vulkan-backend`  
Repo: `~/MobileGL-dev`  
Status: **Active work-in-progress. Do not mark complete yet.**

---

## 1. Goal

Implement a complete OpenGL 3.2 front-end emulation on a new Diligent/Vulkan backend inside MobileGL, instead of the DirectVulkan / DirectGLES backends.

Target state:
- Fully wire MobileGL front-end `MG_State` (buffers, VAO, program, texture, sampler, framebuffer, render-state) into Diligent.
- Implement all GL 3.2 core entry points through the Diligent backend.
- Pass local non-Android GL3.2 tests on the Turnip Adreno 750 GPU.

---

## 2. Current Branch / Commits

Latest 12 commits on `feat/diligent-vulkan-backend`:

```
2f5abf83 test(diligent): verify indexed DrawElements path from real frontend state
c31b7381 feat(diligent): add basic texture binding and textured state-draw test
8945c507 feat(diligent): clear depth in GL Clear when GL_DEPTH_BUFFER_BIT set
558d3aea feat(diligent): add offscreen depth target and depth clear
7e2f0bc8 feat(diligent): wire stencil and color-mask state into state PSO
f99786f6 feat(diligent): wire viewport/scissor state into state draws
be4cc3ce feat(diligent): wire blend/depth/cull render state into state PSO
c855e6cf feat(diligent): verify state-driven draw with real MobileGL frontend state
02e60bfa feat(diligent): add state-driven draw path (VAO/buffer/program to Diligent)
a9515c92 feat(diligent): add dynamic vertex buffer upload path
2f57582a feat(diligent): wire Clear/Draw/Present into GLFunctionsTable
beb21123 feat(diligent): add real offscreen renderer with clear and triangle draw
```

Working tree is clean.

---

## 3. Key Files

### Backend core

- `MobileGL/MG_Backend/Diligent/BackendObject_Diligent.h/.cpp`
  - `BackendObject_Diligent`
  - Creates Diligent Vulkan device/context
  - Owns `DiligentRenderer`
  - Wires `GLFunctionsTable`:
    - `Clear` (color + depth)
    - `DrawArrays`
    - `DrawElements`
    - `Present`
- `MobileGL/MG_Backend/Diligent/DiligentVulkan.h/.cpp`
  - Backend identity helper / translation unit
- `MobileGL/MG_Backend/Diligent/Renderer/DiligentRenderer.h/.cpp`
  - Offscreen RGBA8 + D32F targets
  - Clear / ClearDepth / DrawTriangle / DrawVertices
  - `CreateTestTexture` (RGBA8 texture + SRV + sampler)
  - `DrawFromState` (main front-end emulation draw path)
  - `CreatePipelineFromState`:
    - SPIR-V → Diligent shaders via SPIRV-Reflect
    - VAO attributes → input layout
    - primitive topology from GL mode
    - blend / depth / cull / stencil / color-mask state
  - `UploadVertexDataFromState`:
    - packs enabled VAO attributes from `BufferObject` into interleaved vertex buffer
    - supports `DrawArrays`, `DrawElements`, triangle-fan and line-loop expansion
  - Static texture binding to `g_Texture` through PSO static variables + SRB

### Integration changes

- `CMakeLists.txt`
  - New option `MOBILEGL_ENABLE_DILIGENT` (default ON for local)
  - DiligentCore added **after** glslang/SPIRV-Cross/xxHash/Vulkan-Headers so it reuses existing CMake targets
  - Diligent static libraries linked into `MobileGL` / `MobileGL_s`
  - New Diligent backend sources added
- `MobileGL/MG_Backend/BackendObject.h`
  - New `BackendType::DiligentVulkan`
- `MobileGL/MG_Backend/Init.cpp`
  - New backend switch case
- `MobileGL/ConfigLoader.cpp`
  - `MOBILEGL_BACKEND_TYPE=DiligentVulkan` accepted
- `MobileGL/MG_Test/CMakeLists.txt`
  - New `MobileGL/MG_Test/Backend/Diligent` subdirectory
- `MobileGL/MG_Test/Backend/Diligent/`
  - `CMakeLists.txt`
  - `SanityTest.cpp`

### Local test files

- `MobileGL/MG_Test/Backend/Diligent/SanityTest.cpp`
  - `CreatesDiligentDeviceAndAdvertisesGL32`
  - `ClearsAndDrawsTriangleOffscreen`
  - `DrawsFromMobileGLState`
  - `DrawsTexturedFromMobileGLState`
  - `DrawsIndexedFromMobileGLState`
  - `DrawsRealTexturedFromMobileGLState`
  - `DrawsUniformFromMobileGLState`
  - `DrawsToOffscreenFramebufferFromMobileGLState`
  - `DrawsWithScissorFromMobileGLState`
  - `DrawsWithBlendFromMobileGLState`
  - `DrawsWithDepthTestFromMobileGLState`
  - `DrawsNamedUniformBlockFromMobileGLState`
  - `DrawsWithStencilTestFromMobileGLState`

---

## 4. What Works Today

Verified locally on Turnip Adreno 750:

- Diligent device/context creation
- GL 3.2 / GLSL 1.50 capability advertisement
- Offscreen color + depth rendering
- Clear color and depth
- Real mobilegl front-end state-driven drawing:
  - Program SPIR-V → Diligent shaders
  - VAO attributes + bound GL buffer → interleaved vertex buffer
  - `DrawArrays` path
  - `DrawElements` path (index buffer)
- Texture basics:
  - Offscreen texture creation
  - CPU → Diligent texture (`CreateTestTexture`)
  - Static sampler2D binding to `g_Texture`
  - Textured draw test passes
- Render state:
  - Blend enable/factors/equations
  - Stencil clear + test enabled on a D24S8 default depth/stencil target
  - Depth test enable/func/write mask
  - Cull face enable/mode/front-face winding
  - Stencil test enable/masks/ops/func/ref
  - Color write mask
  - Viewport
  - Scissor rect
- Texture/sampler full integration:
  - `ITextureObject` → Diligent `ITexture` + SRV with automatic dirty upload
  - `SamplerObject` / texture-object sampler → Diligent `ISampler`
  - Real front-end `glTexImage2D` path (not only `CreateTestTexture`) verified
- Global UBO upload:
  - Front-end `glUniform*` shadow → Diligent uniform buffer bound as `MGL_GLOBAL_UBO`
- User framebuffer mapping:
  - Current draw/read FBO resolves texture attachments to Diligent RTV/DSV
  - `ReadPixels` can read back from a user FBO color attachment
- More GL entry points wired:
  - `DrawRangeElements` / `DrawRangeElementsBaseVertex`
  - `MultiDrawArrays` / `MultiDrawElements` / `MultiDrawElementsBaseVertex`
  - `DrawArraysInstanced` / `DrawElementsInstanced` family
  - Indirect draw CPU fallback: `DrawArraysIndirect`, `DrawElementsIndirect`, `MultiDraw*Indirect`, `*IndirectCount`
  - `ClearBufferfv` / `ClearBufferiv` / `ClearBufferuiv`
  - `BlitFramebuffer` (same-size color copy between current read/draw FBOs)
  - `CopyTexImage2D` / `CopyTexSubImage2D` (whole-color copy fallback)
  - `GetTexImage` / `GetTextureImage` (RGBA8 readback)
  - `ReadPixels` from default and user color attachments
- Primitive expansion:
  - `GL_TRIANGLE_FAN` expanded to triangle list
  - `GL_LINE_LOOP` expanded to line strip
- Local test result:

```
[  PASSED  ] 13 tests
```

---

## 5. How to Build and Run Locally

From repo root `~/MobileGL-dev`:

```bash
cmake -S . -B build-diligent -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMOBILEGL_ENABLE_DILIGENT=ON \
  -DMOBILEGL_BUILD_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/3rdparty/DiligentCore/ThirdParty/googletest"

cmake --build build-diligent --target DiligentVulkanSanityTest -j 4

./build-diligent/MobileGL/MG_Test/Backend/Diligent/DiligentVulkanSanityTest --gtest_color=no
```

Notes:
- `MOBILEGL_BUILD_BENCHMARK=OFF` avoids network fetch of google/benchmark in this environment.
- `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` pins googletest to DiligentCore's bundled copy, avoiding flaky network clone.
- Max 4 cores is intentional: use `-j 4`.

---

## 6. Environment Notes

- Host: Linux `aarch64`, glibc 2.43 (Fedora container on Android/Droidspaces)
- GPU: Turnip Adreno 750, Vulkan API 1.4.354
- GPU nodes available:
  - `/dev/dri/renderD128`
  - `/dev/kgsl-3d0`
- Android SDK/NDK: `~/android-sdk` (aarch64 glibc)
  - NDK `27.3.13750724`
  - CMake `3.22.1`
- JDK/Gradle for APK builds:
  - `~/android-build-tools/jdk17`
  - `~/android-build-tools/gradle/gradle-8.10.2`

---

## 7. Known Limitations / Not Yet Implemented

- User framebuffers now support texture color attachments and depth/stencil texture or renderbuffer attachments; renderbuffer readback and multi-target color attachments remain partial.
- Textures auto-sync `ITextureObject` → Diligent resources, including mip levels and sampler state; compressed textures and integer/3-channel formats that Diligent lacks are still skipped.
- Global UBO (default-block `glUniform*`) and named application UBO blocks (through `glBindBufferBase`/`glUniformBlockBinding`) now upload and bind; SSBOs are still not fed from frontend buffer bindings.
- No swapchain / EGL window surface presentation yet; `Present()` only flushes.
- No transform feedback / queries / sync / readback of non-color resources.
- Draw range, multi-draw, instanced-draw wrappers, clear-buffer, blit, read-pixels, CopyTexImage*, GetTexImage/GetTextureImage and indirect draws are now wired; buffer subdata paths still remain.
- A last-PSO cache now avoids recreating the pipeline when program/render-state/topology/VAO layout is unchanged; texture/UBO resources are still rebound dynamically per draw.
- The `GLFunctionsTable` is only partially populated.

---

## 8. Recommended Next Steps

1. **Framebuffer / Renderbuffer mapping**
   - [x] Map `MG_State::GLState::FramebufferObject` attachments to Diligent `ITextureView` / `ITexture`.
   - [x] Support default framebuffer as current offscreen target.
   - [~] Support `glBindFramebuffer`, `glFramebufferTexture2D`, renderbuffer color/depth attachments (texture color + depth/renderbuffer work; renderbuffer readback and multiple color targets still partial).

2. **Texture / Sampler full integration**
   - [x] Translate MobileGL `ITextureObject` to Diligent `ITexture` and cache by `GetLifetimeId()`.
   - [x] Propagate texture unit bindings into the PSO SRB.
   - [x] Translate `SamplerObject` state into Diligent `SamplerDesc`.

3. **Uniform / UBO support**
   - [x] Create Diligent buffer for `ProgramObject::GetUBOData()` / `GetUBOSize()`.
   - [x] Bind the global UBO as a dynamic shader resource.
   - [x] Handle per-program uniform block bindings / named UBO blocks.

4. **PSO / resource caching**
   - [~] Cache PSOs by program + VAO config + render state + topology (single last-PSO fast path).
   - [~] Cache textures and samplers; buffers/SRBs can still be re-bound per draw.

5. **More GL 3.2 entry points**
   - [x] `DrawRangeElements`
   - [x] `MultiDraw*`
   - [x] `BlitFramebuffer` (same-size color copy)
   - [x] `ReadPixels` from non-default framebuffer
   - [x] `CopyTexImage*` wired as whole-color copy
   - [x] `GetTexImage` / `GetTextureImage` (RGBA8)
   - [x] Indirect draws (CPU fallback)

6. **Expand local test suite**
   - [x] Scissor test
   - [x] Blend test
   - [x] Texture filtering / sampler state test
   - [x] framebuffer offscreen render-to-texture test
   - [x] Depth test visual test
   - [x] Stencil test

---

## 9. Handoff Notes for Next Agent

- Do **not** reference `origin/Deprecated/Feat/Diligent`; that old implementation is intentionally ignored.
- Work from this branch, keep tests green.
- The command `./build-diligent/.../DiligentVulkanSanityTest` runs all 5 Diligent tests.
- If a new test crashes during shader resource binding, remember Diligent texture SRVs need a sampler attached via `ITextureView::SetSampler()` before `InitializeStaticSRBResources()`.
- When re-creating a PSO or buffer, call `Release()` (or assign `nullptr`) before the create call to avoid Diligent debug “Overwriting reference” assertions.
