// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/BackendObject_DirectVulkanTMP.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObject_DirectVulkanTMP.h"
#include "MG_Backend/BackendObject.h"
#include "DirectVulkanTMP.h"
#include "TmpImpl.h"
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Backend::DirectVulkanTMP {
    BackendObject_DirectVulkanTMP::~BackendObject_DirectVulkanTMP() = default;

    void BackendObject_DirectVulkanTMP::InitWindowSurface() {
        auto nativeWindow = reinterpret_cast<NativeWindowType>(m_windowHandle.Handle);
        if (!DirectVulkanTMP::InitWindowSurface(nativeWindow)) {
            MGLOG_E("Failed to initialize window surface for DirectVulkanTMP backend");
        }
    }

    void BackendObject_DirectVulkanTMP::Initialize() {
        m_initialized = true;
    }

    void BackendObject_DirectVulkanTMP::InitCapabilities() {
        if (!m_initialized) {
            MGLOG_E("Cannot initialize capabilities before backend is initialized");
            return;
        }

        MG_Util::BackendLoader::QueryVulkanCapabilities(m_vulkanCaps,
                                                        DirectVulkanTMP::GetVulkanState().ctx->GetPhysicalDevice());
        UpdateDynamicBackendParameters();
    }

    const RendererInfo& BackendObject_DirectVulkanTMP::GetRendererInfo() const {
        static RendererInfo RendererInfo = {
            .RendererName = "Magma-TMP",          // Renderer Name
            .BackendName = "Direct (Vulkan) TMP", // Backend Name
            .ExtraVendor = Nullopt,               // Extra vendor
            .RendererGLInfo =
                {
                    .TargetGLVersion = {3, 3, 0},                      // Target OpenGL Version
                    .TargetGLSLVersion = {4, 6, 0},                    // Target Shading Language Version
                    .Extensions = {V_OpenGL30, V_OpenGL31, V_OpenGL32, // OpenGL Extensions
                                   V_OpenGL33},
                    .IsCompatibilityProfile = false // Is Compatibility Profile
                },
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false} // Backend Capability
        };
        return RendererInfo;
    }

    String BackendObject_DirectVulkanTMP::GetBackendAPIVersionString() const {
        if (!m_initialized) {
            return "<uninitialized DirectVulkanTMP backend>";
        }
        // Format:
        // <GPU Name>, Vulkan <Vulkan Version>, Driver <Driver Version>
        // TODO
        String str = m_vulkanCaps.DeviceName + ", Vulkan " + m_vulkanCaps.VulkanAPIVersion.toString() + ", Driver " +
                     m_vulkanCaps.DriverVersionString;
        return str;
    }

    BackendType BackendObject_DirectVulkanTMP::GetBackendType() const {
        return BackendType::DirectVulkanTMP;
    }

    const GlobalBackendFunctionsTable& BackendObject_DirectVulkanTMP::GetBackendFunctions() const {
        static GlobalBackendFunctionsTable funcsTable;
        static Bool funcsTableInitialized = false;
        if (!funcsTableInitialized) {
            funcsTable.Present = DirectVulkanTMP::Present;
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

    const DynamicBackendParameters& BackendObject_DirectVulkanTMP::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    void BackendObject_DirectVulkanTMP::UpdateDynamicBackendParameters() {
        m_dynamicParameters.UniformBufferOffsetAlignment = m_vulkanCaps.UniformBufferOffsetAlignment;
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP
