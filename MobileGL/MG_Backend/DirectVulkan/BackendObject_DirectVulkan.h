// MobileGL - MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BackendObject.h"
#include <MG_Util/BackendLoaders/Vulkan/Loader.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    class BackendObject_DirectVulkan : public BackendObject {
    public:
        BackendObject_DirectVulkan();
        ~BackendObject_DirectVulkan() override;

        void Initialize() override;
        Bool InitWindowSurface() override;
        Bool InitCapabilities() override;
        Bool InitializeEGLDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) override;
        Bool CreateEGLWindowSurface(EGLSurface surface, const WindowHandle& handle) override;
        Bool ResizeEGLWindowSurface(EGLSurface surface, Uint32 width, Uint32 height) override;
        Bool CreateEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) override;
        Bool MakeEGLCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) override;
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override;
        void ReleaseEGLSurface(EGLSurface surface) override;
        void ReleaseEGLResources() override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;
        void ApplyVulkanCapabilitiesForTesting(const MG_External::VulkanCapabilities& capabilities);

    private:
        Bool InitPbufferSurface(EGLint width, EGLint height) override;
        void UpdateAdvertisedExtensions();
        void UpdateDynamicBackendParameters();

        Bool m_initialized = false;
        DynamicBackendParameters m_dynamicParameters;
        MG_External::VulkanCapabilities m_vulkanCaps;
        RendererInfo m_rendererInfo;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
