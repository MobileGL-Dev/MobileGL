// MobileGL - MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectGLES.h"
#include "MG_Backend/BackendObject.h"
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <format>

namespace MobileGL::MG_Backend::DirectGLES {
    BackendObject_DirectGLES::~BackendObject_DirectGLES() {
        DestroyEGLContext();
    }

    void BackendObject_DirectGLES::InitWindowSurface() {
        // Only use EGL for now
        auto nativeWindow = reinterpret_cast<NativeWindowType>(m_windowHandle.Handle);
        if (!DirectGLES::InitWindowSurface(nativeWindow)) {
            MGLOG_E("Failed to initialize window surface for DirectGLES backend");
        }
    }

    void BackendObject_DirectGLES::Initialize() {
        if (!MG_Util::BackendLoader::AcquireEGLFunctions(m_EGLFunctions)) {
            MGLOG_E("Failed to acquire EGL functions for DirectGLES backend");
            return;
        }
        if (!MG_Util::BackendLoader::AcquireGLESFunctions(m_GLESFunctions, m_EGLFunctions.eglGetProcAddress)) {
            MGLOG_E("Failed to acquire GLES functions for DirectGLES backend");
            return;
        }
        m_initialized = true;

        DirectGLES::SetEGLFuncsTable(m_EGLFunctions);
        DirectGLES::SetGLESFuncsTable(m_GLESFunctions);
    }

    void BackendObject_DirectGLES::InitCapabilities() {
        if (!m_initialized) {
            MGLOG_E("DirectGLES backend not initialized");
            return;
        }

        if (!MG_Util::BackendLoader::FillInGLESCapabilities(m_GLESCapabilities, m_GLESFunctions)) {
            MGLOG_E("Failed to fill in GLES capabilities for DirectGLES backend");
            return;
        }
        DirectGLES::SetGLESCapabilities(m_GLESCapabilities);
        UpdateDynamicBackendParameters();
    }

    const RendererInfo& BackendObject_DirectGLES::GetRendererInfo() const {
        static RendererInfo RendererInfo = {
            .RendererName = "Espryt",            // Renderer Name
            .BackendName = "Direct (OpenGL ES)", // Backend Name
            .ExtraVendor = Nullopt,              // Extra vendor
            .RendererGLInfo =
                {
                    .TargetGLVersion = {3, 3, 0},                      // Target OpenGL Version
                    .TargetGLSLVersion = {4, 6, 0},                    // Target Shading Language Version
                    .Extensions = {V_OpenGL30, V_OpenGL31, V_OpenGL32, // OpenGL Extensions
                                   V_OpenGL33, E_GL_ARB_draw_buffers_blend},
                    .IsCompatibilityProfile = false // Is Compatibility Profile
                },
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false} // Backend Capability
        };
        return RendererInfo;
    }

    String BackendObject_DirectGLES::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectGLES backend>";
        }
        // Format:
        // <OpenGL ES Renderer>, OpenGL ES <OpenGL ES Version>
        String versionString = std::format("{}, OpenGL ES {}.{}", m_GLESCapabilities.GLESRendererString,
                                           m_GLESCapabilities.GLESVersion.Major, m_GLESCapabilities.GLESVersion.Minor);
        return versionString;
    }

    BackendType BackendObject_DirectGLES::GetBackendType() const {
        return BackendType::DirectGLES;
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectGLES::GetBackendFunctions() const {
        static GlobalBackendFunctionsTable funcsTable;
        static Bool funcsTableInitialized = false;
        if (!funcsTableInitialized) {
            funcsTable.Present = DirectGLES::Present;
            funcsTable.GL.DrawArrays = DrawArrays;
            funcsTable.GL.DrawElements = DrawElements;
            funcsTable.GL.DrawElementsBaseVertex = DrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElements = MultiDrawElements;
            funcsTable.GL.MultiDrawElementsBaseVertex = MultiDrawElementsBaseVertex;
            funcsTable.GL.MultiDrawElementsIndirect = MultiDrawElementsIndirect;
            funcsTable.GL.MultiDrawArraysIndirect = MultiDrawArraysIndirect;
            funcsTable.GL.DrawRangeElementsBaseVertex = DrawRangeElementsBaseVertex;
            funcsTable.GL.DrawRangeElements = DrawRangeElements;
            funcsTable.GL.DrawElementsInstancedBaseVertexBaseInstance = DrawElementsInstancedBaseVertexBaseInstance;
            funcsTable.GL.DrawElementsInstancedBaseVertex = DrawElementsInstancedBaseVertex;
            funcsTable.GL.DrawElementsInstancedBaseInstance = DrawElementsInstancedBaseInstance;
            funcsTable.GL.DrawElementsInstanced = DrawElementsInstanced;
            funcsTable.GL.DrawArraysInstancedBaseInstance = DrawArraysInstancedBaseInstance;
            funcsTable.GL.DrawArraysInstanced = DrawArraysInstanced;
            funcsTable.GL.DrawElementsIndirect = DrawElementsIndirect;
            funcsTable.GL.DrawArraysIndirect = DrawArraysIndirect;
            funcsTable.GL.Clear = Clear;
            funcsTable.GL.ClearBufferfi = ClearBufferfi;
            funcsTable.GL.ClearBufferfv = ClearBufferfv;
            funcsTable.GL.ClearBufferuiv = ClearBufferuiv;
            funcsTable.GL.ClearBufferiv = ClearBufferiv;
            funcsTable.GL.BlitFramebuffer = BlitFramebuffer;
            funcsTable.GL.CopyTexImage2D = CopyTexImage2D;
            funcsTable.GL.CopyTexSubImage2D = CopyTexSubImage2D;
            funcsTable.GL.GenerateMipmap = GenerateMipmap;
            funcsTable.GL.ReadPixels = ReadPixels;
            funcsTable.GL.GetTexImage = GetTexImage;
            funcsTableInitialized = true;
        }
        return funcsTable;
    }

    const DynamicBackendParameters& BackendObject_DirectGLES::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    void BackendObject_DirectGLES::UpdateDynamicBackendParameters() {
        m_dynamicParameters.UniformBufferOffsetAlignment = m_GLESCapabilities.UniformBufferOffsetAlignment;
    }

    const MG_External::GLESFunctionsTable& BackendObject_DirectGLES::GetGLESFunctions() const {
        return m_GLESFunctions;
    }

    const MG_External::EGLFunctionsTable& BackendObject_DirectGLES::GetEGLFunctions() const {
        return m_EGLFunctions;
    }
} // namespace MobileGL::MG_Backend::DirectGLES
