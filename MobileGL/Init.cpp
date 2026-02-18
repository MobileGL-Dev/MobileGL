// MobileGL - MobileGL/Init.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Init.h"
#include "Config.h"
#include <MG_Impl/Init.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>
#include <MG_Impl/GLImpl/Texture/ProxyTexture.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>

namespace MobileGL {
    void MG_Initialize() {
        MG_Util::Debug::InitFile();
        MGLOG_I("Initializing MobileGL...");
        MG_State::Init();
        MGLOG_D("MobileGL State initialized");
        MG_Backend::Init();
        MGLOG_D("MobileGL Backend initialized");
        MG_Impl::Init();
        MGLOG_D("MobileGL Implementation initialized");
        glslang::InitializeProcess();
        MGLOG_D("glslang initialized");
        MGLOG_I("MobileGL initialized");
    }

    void MG_Destroy() {
        MGLOG_I("MobileGL closing...");
        glslang::FinalizeProcess();
        delete MG_State::pGLContext;
        delete MG_Impl::GLImpl::TextureImpl::pProxyTextureManager;
        delete MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo;
        MG_Util::Debug::Close();

        // TODO: add and use Destroy functions for other subsystems
    }

#if defined(__linux__) || defined(__APPLE__)
    __attribute__((constructor)) static void AutoInit() {
        MG_Initialize();
    }

    __attribute__((destructor)) static void AutoDestroy() {
        MG_Destroy();
    }
#endif

#ifdef _WIN32
    BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
        switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            MG_Initialize();
            break;

        case DLL_PROCESS_DETACH:
            MG_Destroy();
            break;
        }
        return TRUE;
    }
#endif
} // namespace MobileGL
