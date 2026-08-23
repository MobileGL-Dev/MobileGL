// MobileGL - MobileGL/MG_Backend/Diligent/BackendObject_Diligent.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#pragma once

#include <Includes.h>
#include "../BackendObject.h"

// X11 (pulled in by Includes.h through Vulkan-Headers) defines True/False as
// macros, which collide with Diligent's Bool constants in BasicTypes.h.
#if defined(True)
#undef True
#endif
#if defined(False)
#undef False
#endif

#include <RefCntAutoPtr.hpp>

namespace Diligent {
    struct IEngineFactoryVk;
    struct IRenderDevice;
    struct IDeviceContext;
}

namespace MobileGL::MG_Backend::DiligentBackend {
    class DiligentRenderer;

    // New Diligent/Vulkan backend, implemented from scratch on top of
    // DiligentCore. The backend object owns the Diligent device/context and
    // currently advertises OpenGL 3.2 core capability; the GL function table
    // is intentionally empty until drawing/resource paths are ported.
    class BackendObject_Diligent : public BackendObject {
    public:
        BackendObject_Diligent();
        ~BackendObject_Diligent() override;

        void Initialize() override;
        Bool InitCapabilities() override;
        Bool InitWindowSurface() override;
        Bool InitPbufferSurface(EGLint width, EGLint height) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        void OnEGLSurfaceReleased(EGLSurface surface) override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;
        void ReleaseEGLResources() override;

        DiligentRenderer* GetRenderer();

    private:
        Bool CreateDiligentDevice();

        RendererInfo m_rendererInfo;
        DynamicBackendParameters m_dynamicParameters;
        GlobalBackendFunctionsTable m_functions{};
        ::Diligent::IEngineFactoryVk* m_pFactoryVk = nullptr;
        ::Diligent::RefCntAutoPtr<::Diligent::IRenderDevice> m_pDevice;
        ::Diligent::RefCntAutoPtr<::Diligent::IDeviceContext> m_pContext;
        std::unique_ptr<DiligentRenderer> m_pRenderer;
        Bool m_initialized = false;
    };
} // namespace MobileGL::MG_Backend::DiligentBackend
