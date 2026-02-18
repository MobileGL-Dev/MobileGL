// MobileGL - MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BackendObject.h"
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Backend::DirectGLES {
    class BackendObject_DirectGLES : public BackendObject {
    public:
        ~BackendObject_DirectGLES() override;
        void Initialize() override;
        void InitWindowSurface() override;

        const RendererInfo& GetRendererInfo() const override;
        String GetBackendAPIVersionString() const override;
        const GlobalBackendFunctionsTable& GetBackendFunctions() const override;
        const DynamicBackendParameters& GetDynamicParameters() const override;
        BackendType GetBackendType() const override;

        const MG_External::GLESFunctionsTable& GetGLESFunctions() const;
        const MG_External::EGLFunctionsTable& GetEGLFunctions() const;

    private:
        void UpdateDynamicBackendParameters();

        Bool m_initialized = false;
        MG_External::EGLFunctionsTable m_EGLFunctions;
        MG_External::GLESFunctionsTable m_GLESFunctions;
        MG_External::GLESCapabilities m_GLESCapabilities;
        DynamicBackendParameters m_dynamicParameters;
    };
} // namespace MobileGL::MG_Backend::DirectGLES
