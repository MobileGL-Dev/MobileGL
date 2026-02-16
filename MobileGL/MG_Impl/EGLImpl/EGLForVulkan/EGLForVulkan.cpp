// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLForVulkan/EGLForVulkan.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EGLForVulkan.h"
#include "MG_Backend/DirectVulkan/DirectVulkan.h"
#include "MG_Backend/DirectVulkan/TmpImpl.h"
#include <Config.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>

#if MOBILEGL_BACKEND == MOBILEGL_BACKEND_TYPE_DIRECT_VULKAN
namespace MobileGL {
    namespace MG_Impl::EGLImpl {
        // TODO: complete EGL impl for Vulkan

        const char* demoFS = R"(#version 460
        layout(location = 0) in vec3 fragColor;
        layout(location = 0) out vec4 outColor;
        void main() {
            outColor = vec4(fragColor, 1.0);
        }
)";
        const char* demoVS = R"(#version 460
        layout(location = 0) out vec3 fragColor;
        vec2 positions[3] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
        vec3 colors[3] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
        void main() {
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
            fragColor = colors[gl_VertexID];
        }
)";
        void PrepareDemoRes() {
            MGLOG_D("EGLForVulkan::PrepareDemoRes called");

            // Create shader&program object
            auto programObject = MG_State::GLState::ProgramObject(0);
            auto vsObject = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Vertex, 0);
            vsObject->SetShaderSource(demoVS);
            vsObject->Compile();
            if (!vsObject->GetCompileStatus()) {
                MGLOG_E("Vertex shader compilation failed: %s", vsObject->GetInfoLog().c_str());
                return;
            }
            auto fsObject = MakeShared<MG_State::GLState::ShaderObject>(ShaderStage::Fragment, 1);
            fsObject->SetShaderSource(demoFS);
            fsObject->Compile();
            if (!fsObject->GetCompileStatus()) {
                MGLOG_E("Fragment shader compilation failed: %s", fsObject->GetInfoLog().c_str());
                return;
            }
            programObject.AttachShader(vsObject);
            programObject.AttachShader(fsObject);
            programObject.Link();
            if (!programObject.GetLinkStatus()) {
                MGLOG_E("Program linking failed: %s", programObject.GetInfoLog().c_str());
                return;
            }

            Vector<Uint> vsSpv;
            Vector<Uint> fsSpv;

            auto& shaderSpirvs = programObject.GetGeneratedSpirv();
            auto& attachedShaders = programObject.GetAttachedShaders();
            for (int index = 0; index < attachedShaders.size(); ++index) {
                auto& shader = attachedShaders[index];
                auto& spirvCode = shaderSpirvs[index];
                if (shader->GetShaderStage() == ShaderStage::Vertex) {
                    vsSpv = spirvCode;
                } else if (shader->GetShaderStage() == ShaderStage::Fragment) {
                    fsSpv = spirvCode;
                }
            }

            // Create pipeline
            VkPipeline trianglePipeline = MG_Backend::DirectVulkan::pVulkanRenderer->CreateGraphicsPipelineFromSpv(
                "TrianglePipeline", vsSpv, fsSpv);

            // Register render callback
            MG_Backend::DirectVulkan::pVulkanRenderer->RegisterRenderCallback(
                "DrawTriangle", [trianglePipeline](VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D extent) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline);

                    vkCmdDraw(cmd, 3, 1, 0, 0);
                });
        }

        void CreateWindowSurfaceForVulkan(NativeWindowType window) {
            MobileGL::MG_Backend::DirectVulkan::TmpImpl::InitVulkan(window);
            return;
            MG_Backend::DirectVulkan::pVulkanRenderer = MakeUnique<MG_Backend::DirectVulkan::VulkanRenderer>(window);
            MG_Backend::DirectVulkan::pVulkanRenderer->Initialize();

            // PrepareDemoRes(); // for demo use
        }

        EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                       const EGLint* attrib_list) {
            MGLOG_D("EGLForVulkan::CreateWindowSurface called with window=%p", window);
            CreateWindowSurfaceForVulkan(window);
            return (EGLSurface)1;
        }

        EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw) {
            MobileGL::MG_Backend::DirectVulkan::TmpImpl::Present();
            return EGL_TRUE;
            if (!MG_Backend::DirectVulkan::pVulkanRenderer) {
                MGLOG_E("EGLForVulkan::SwapBuffers called but VulkanRenderer is null");
                return EGL_FALSE;
            }
            MG_Backend::DirectVulkan::pVulkanRenderer->Present();
            return EGL_TRUE;
        }

        EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                                EGLint* num_config) {
            *num_config = 1;
            return EGL_TRUE;
        }

        EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list) {
            return (EGLContext)1;
        }

        EGLBoolean Initialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
            if (major) *major = 1;
            if (minor) *minor = 5;
            return EGL_TRUE;
        }

        EGLDisplay GetDisplay(NativeDisplayType display) {
            return (EGLDisplay)1;
        }

        EGLint GetError() {
            return EGL_SUCCESS;
        }

        EGLBoolean MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            return EGL_TRUE;
        }

        EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx) {
            return EGL_TRUE;
        }

        EGLBoolean DestroySurface(EGLDisplay dpy, EGLSurface surface) {
            return EGL_TRUE;
        }

        EGLBoolean Terminate(EGLDisplay dpy) {
            return EGL_TRUE;
        }

        EGLBoolean ReleaseThread(void) {
            return EGL_TRUE;
        }

        EGLContext GetCurrentContext(void) {
            return (EGLContext)1;
        }

        EGLBoolean GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
            if (attribute == EGL_NATIVE_VISUAL_ID) {
#if defined(ANDROID)
                *value = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
                return EGL_TRUE;
#elif defined(__linux__)
                *value = 0;
                return EGL_TRUE;
#elif defined(_WIN32)
                *value = 0;
                return EGL_TRUE;
#else
                *value = 0;
                return EGL_FALSE;
#endif
            }
            return EGL_TRUE;
        }

        EGLBoolean BindAPI(EGLenum api) {
            return EGL_TRUE;
        }

        EGLSurface GetCurrentSurface(EGLint readdraw) {
            return (EGLSurface)1;
        }

        EGLBoolean QuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value) {
            return EGL_TRUE;
        }

        char const* QueryString(EGLDisplay display, EGLint name) {
            return "";
        }

        EGLBoolean SwapInterval(EGLDisplay dpy, EGLint interval) {
            return EGL_TRUE;
        }

        EGLSurface CreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
            return (EGLSurface)1;
        }

        EGLBoolean BindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
            return EGL_TRUE;
        }

        EGLBoolean ReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
            return EGL_TRUE;
        }

        EGLBoolean CopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
            return EGL_TRUE;
        }

        EGLSurface CreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                                                 EGLConfig config, const EGLint* attrib_list) {
            return (EGLSurface)1;
        }

        EGLSurface CreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                       const EGLint* attrib_list) {
            return (EGLSurface)1;
        }

        EGLBoolean GetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
            if (num_config) {
                *num_config = 1;
            }
            if (configs && config_size > 0) {
                configs[0] = (EGLConfig)1;
            }
            return EGL_TRUE;
        }

        EGLDisplay GetCurrentDisplay(void) {
            return (EGLDisplay)1;
        }

        EGLenum QueryAPI(void) {
            return EGL_OPENGL_API;
        }

        EGLBoolean QueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
            if (value) {
                *value = 0;
            }
            return EGL_TRUE;
        }

        EGLBoolean SurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
            return EGL_TRUE;
        }

        EGLBoolean WaitClient(void) {
            return EGL_TRUE;
        }

        EGLBoolean WaitGL(void) {
            return EGL_TRUE;
        }

        EGLBoolean WaitNative(EGLint engine) {
            return EGL_TRUE;
        }

        EGLSync CreateSync(void* dpy, unsigned int type, long const* attrib_list) {
            return reinterpret_cast<EGLSync>(0x1);
        }

        EGLBoolean DestroySync(void* dpy, void* sync) {
            return EGL_TRUE;
        }

        EGLint ClientWaitSync(void* dpy, void* sync, int flags, unsigned long timeout) {
            return EGL_CONDITION_SATISFIED;
        }

        EGLBoolean GetSyncAttrib(void* dpy, void* sync, int attribute, long* value) {
            if (value) {
                *value = 0;
            }
            return EGL_TRUE;
        }

        EGLImage CreateImage(void* dpy, void* ctx, unsigned int target, void* buffer, long const* attrib_list) {
            return reinterpret_cast<EGLImage>(0x1);
        }

        EGLBoolean DestroyImage(void* dpy, void* image) {
            return EGL_TRUE;
        }

        EGLDisplay GetPlatformDisplay(unsigned int platform, void* native_display, long const* attrib_list) {
            return reinterpret_cast<EGLDisplay>(0x1);
        }

        EGLSurface CreatePlatformWindowSurface(void* dpy, void* config, void* native_window, long const* attrib_list) {
            return reinterpret_cast<EGLSurface>(0x1);
        }

        EGLSurface CreatePlatformPixmapSurface(void* dpy, void* config, void* native_pixmap, long const* attrib_list) {
            return reinterpret_cast<EGLSurface>(0x1);
        }

        EGLBoolean WaitSync(void* dpy, void* sync, int flags) {
            return EGL_TRUE;
        }

        __eglMustCastToProperFunctionPointerType GetProcAddress(const char* name) {
            MGLOG_D("eglGetProcAddress(%s)", name);
#if !defined(WIN32) && !defined(__APPLE__)
            void* proc = dlsym(RTLD_DEFAULT, (const char*)name);
#else
            void* proc = NULL;
#endif
            if (!proc) {
                MGLOG_W("Failed to get function: %s", (const char*)name);
                return nullptr;
            }
            return (__eglMustCastToProperFunctionPointerType)proc;
        }
    } // namespace MG_Impl::EGLImpl
} // namespace MobileGL
#endif
