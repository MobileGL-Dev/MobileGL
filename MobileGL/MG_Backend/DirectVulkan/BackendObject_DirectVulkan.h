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
        ~BackendObject_DirectVulkan() override;

        void Initialize() override;
        void InitWindowSurface() override;
        void InitCapabilities() override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;

    private:
        void UpdateDynamicBackendParameters();

        Bool m_initialized = false;
        DynamicBackendParameters m_dynamicParameters;
        MG_External::VulkanCapabilities m_vulkanCaps;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
