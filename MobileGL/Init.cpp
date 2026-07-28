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
#include <MG_Impl/GLImpl/Sync/GL_Sync.h>

#include <atomic>
#include <mutex>

namespace MobileGL {
    namespace {
        std::atomic<Bool> g_isInitialized = false;
        thread_local Bool tl_initializing = false;

        std::mutex& InitMutex() {
            static std::mutex mutex;
            return mutex;
        }

        void DestroyImpl(Bool logLifecycle) {
            if (!g_isInitialized) {
                return;
            }

            if (logLifecycle) {
                MGLOG_I("MobileGL closing...");
            }
            glslang::FinalizeProcess();
            // GL syncs die with their contexts, and every context is gone by the
            // time full teardown runs: drain the live-sync registry while the
            // backend function table can still release the backend handles (and
            // before a re-initialized library could pair them with the wrong
            // backend's DeleteSync).
            MG_Impl::GLImpl::DestroyAllSyncObjects();
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

    void EnsureInitialized() {
        if (g_isInitialized.load(std::memory_order_acquire)) {
            return;
        }
        // Re-entrant call while this thread is already inside Initialize()
        // (e.g. an init step routing back through a public entry point).
        if (tl_initializing) {
            return;
        }
        const std::lock_guard<std::mutex> lock(InitMutex());
        if (g_isInitialized.load(std::memory_order_acquire)) {
            return;
        }
        tl_initializing = true;
        Initialize();
        tl_initializing = false;
    }

    void Destroy() {
        DestroyImpl(true);
    }

    // MobileGL's lifecycle is owned entirely by the host-API layers
    // (EGL/WGL/CGL): initialization happens lazily on the first entry point
    // via EnsureInitialized(), and full teardown happens deterministically
    // when the last EGL display is terminated with nothing current (EGLImpl
    // calls Destroy()). There is intentionally no static constructor, no
    // static destructor, and no DllMain: the global singletons use
    // leak-at-exit storage (see GlobalObjects.cpp), so a process that exits
    // without eglTerminate simply leaks them to the OS instead of running
    // backend destructors during static teardown.
} // namespace MobileGL
