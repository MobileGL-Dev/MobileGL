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
#include <MG_State/GLState/Core.h>
#include <MG_State/EGLState/Core.h>
#include <MG_Impl/GLImpl/Texture/ProxyTexture.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <cstdlib>

namespace MobileGL {
    namespace {
        Bool g_isInitialized = false;
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
        if (!g_isInitialized) {
            return;
        }

        MGLOG_I("MobileGL closing...");
        glslang::FinalizeProcess();
        MG_Backend::pActiveBackendObject.reset();
        MG_State::pGLContext.reset();
        MG_State::pEGLContext.reset();
        MG_Impl::GLImpl::TextureImpl::pProxyTextureManager.reset();
        MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo.reset();
        MG_Backend::gBackendFunctionsTable = {};
        g_isInitialized = false;
        MG_Util::Debug::Close();

        // TODO: add and use Destroy functions for other subsystems
    }

#if defined(__linux__) || defined(__APPLE__)
    __attribute__((constructor)) static void AutoInit() {
        Initialize();
    }

    __attribute__((destructor)) static void AutoDestroy() {
        if (std::getenv("MOBILEGL_TRACE_SKIP_AUTODESTROY") != nullptr) {
            return;
        }
        Destroy();
    }
#endif

#ifdef _WIN32
    BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
        switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            Initialize();
            break;

        case DLL_PROCESS_DETACH:
            Destroy();
            break;
        }
        return TRUE;
    }
#endif
} // namespace MobileGL
