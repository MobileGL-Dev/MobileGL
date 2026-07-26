// MobileGL - MobileGL/Init.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Init.h"
#include "Config.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/DirectVulkan/DirectVulkan.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/EGLState/Core.h>
#include <MG_Impl/GLImpl/Texture/ProxyTexture.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>

namespace MobileGL {
    namespace {
        Bool g_isInitialized = false;

        void DestroyImpl(Bool logLifecycle) {
            if (!g_isInitialized) {
                return;
            }

            if (logLifecycle) {
                MGLOG_I("MobileGL closing...");
            }
            glslang::FinalizeProcess();
            MG_Backend::pActiveBackendObject.reset();
            MG_State::pGLContext.reset();
            MG_State::pEGLContext.reset();
            MG_Impl::GLImpl::TextureImpl::pProxyTextureManager.reset();
            MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo.reset();
            MG_Backend::gBackendFunctionsTable = {};
            g_isInitialized = false;
            if (logLifecycle) {
                MG_Util::Debug::Close();
            }

            // TODO: add and use Destroy functions for other subsystems
        }
    }

    void Initialize() {
        if (g_isInitialized) {
            MGLOG_D("MobileGL already initialized; skipping duplicate Initialize()");
            return;
        }

        MG_Util::Debug::InitFile();
        MGLOG_I("Initializing MobileGL...");
        MG_ConfigLoader::Init();
        MGLOG_I("Config loaded");
        MG_State::Init();
        MGLOG_D("MG_State initialized");
        MG_Backend::Init();
        MGLOG_D("MG_Backend initialized");
        MG_Impl::Init();
        MGLOG_D("MG_Impl initialized");
        glslang::InitializeProcess();
        MGLOG_D("glslang initialized");
        g_isInitialized = true;
        MGLOG_I("MobileGL initialized");
    }

    void Destroy() {
        DestroyImpl(true);
    }

#if defined(__linux__) || defined(__APPLE__)
    __attribute__((constructor)) static void AutoInit() {
        Initialize();
    }

    __attribute__((destructor)) static void AutoDestroy() {
        if (MG_Config::Features.TraceSkipAutodestroy) {
            return;
        }
#if defined(__APPLE__)
        // macOS injected dylibs can run destructors after logging/backend static state is already torn down.
        return;
#else
        DestroyImpl(false);
#endif
    }
#endif

#ifdef _WIN32
    namespace {
        // Called on DLL_PROCESS_DETACH, which runs BEFORE the CRT executes this
        // DLL's static destructors. Releasing (deliberately leaking) the global
        // singletons here keeps those destructors from tearing down the backend
        // inside a dying process - other threads have already been terminated,
        // so Vulkan/GLES cleanup crashes with the process half-dead. The process
        // is exiting; the OS reclaims everything.
        // No logging here: this runs under the loader lock after every other
        // thread was terminated, where the log mutex/heap/stdio may be in any
        // state. Plain pointer stores only.
        void AbandonAtProcessExit() {
            MG_Backend::DirectVulkan::pVulkanRenderer.release();
            MG_Backend::pActiveBackendObject.release();
            MG_State::pGLContext.release();
            MG_State::pEGLContext.release();
            MG_Impl::GLImpl::TextureImpl::pProxyTextureManager.release();
            MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo.release();
        }
    } // namespace
#endif
} // namespace MobileGL

#ifdef _WIN32
// Initialization is NOT done here: MobileGL::Initialize() loads libraries
// (Vulkan/ANGLE) and spins up glslang, none of which is safe under the loader
// lock. The WGL host layer calls Initialize() lazily on its first entry point.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        MobileGL::AbandonAtProcessExit();
    }
    return TRUE;
}
#endif
