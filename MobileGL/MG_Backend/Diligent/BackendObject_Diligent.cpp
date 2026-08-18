// MobileGL - MobileGL/MG_Backend/Diligent/BackendObject_Diligent.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#include "BackendObject_Diligent.h"
#include "DiligentVulkan.h"
#include "Renderer/DiligentRenderer.h"
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>

#include <EngineFactoryVk.h>
#include <RenderDevice.h>
#include <DeviceContext.h>

#include <exception>

namespace MobileGL::MG_Backend::DiligentBackend {
    namespace {
        const RendererInfo BuildInitialRendererInfo() {
            RendererInfo info;
            info.RendererName = "MobileGL (Diligent/Vulkan)";
            info.BackendName = "Diligent Vulkan";
            info.RendererGLInfo.TargetGLVersion = {3, 2, 0};
            info.RendererGLInfo.TargetGLSLVersion = {1, 50, 0};
            info.RendererGLInfo.IsCompatibilityProfile = false;
            return info;
        }

        DiligentRenderer* GetActiveRenderer() {
            auto* backend = dynamic_cast<BackendObject_Diligent*>(pActiveBackendObject.get());
            return backend != nullptr ? backend->GetRenderer() : nullptr;
        }

        void Clear(GLbitfield mask) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr) {
                return;
            }
            if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
                const auto& color = MG_State::pGLContext->GetClearColor();
                renderer->Clear(color.x(), color.y(), color.z(), color.w());
            }
        }

        void DrawArrays(GLenum mode, GLint first, GLsizei count) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, first, count, 0, nullptr);
            }
        }

        void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, 0, count, type, indices);
            }
        }

        void Present() {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->Present();
            }
        }
    } // namespace

    BackendObject_Diligent::BackendObject_Diligent()
        : m_rendererInfo(BuildInitialRendererInfo()) {}

    BackendObject_Diligent::~BackendObject_Diligent() {
        m_pRenderer.reset();
        m_pContext.Release();
        m_pDevice.Release();
        m_pFactoryVk = nullptr;
    }

    Bool BackendObject_Diligent::CreateDiligentDevice() {
        if (m_pDevice && m_pContext) {
            return true;
        }

        try {
            if (m_pFactoryVk == nullptr) {
                m_pFactoryVk = ::Diligent::GetEngineFactoryVk();
                if (m_pFactoryVk == nullptr) {
                    MGLOG_E("Diligent: failed to load Vulkan engine factory");
                    return false;
                }
                m_pFactoryVk->SetBreakOnError(false);
            }

            ::Diligent::Uint32 numAdapters = 0;
            m_pFactoryVk->EnumerateAdapters(::Diligent::Version{}, numAdapters, nullptr);
            if (numAdapters == 0) {
                MGLOG_W("Diligent: no Vulkan adapters available; skipping device creation");
                return false;
            }

            ::Diligent::EngineVkCreateInfo engineCI;
            ::Diligent::ImmediateContextCreateInfo ctxCI;
            ctxCI.Name = "MobileGL Diligent Main Context";
            ctxCI.QueueId = 0;
            ctxCI.Priority = ::Diligent::QUEUE_PRIORITY_MEDIUM;
            engineCI.NumImmediateContexts = 1;
            engineCI.pImmediateContextInfo = &ctxCI;

            ::Diligent::IRenderDevice* pDevice = nullptr;
            ::Diligent::IDeviceContext* pContext = nullptr;
            m_pFactoryVk->CreateDeviceAndContextsVk(engineCI, &pDevice, &pContext);
            if (pDevice == nullptr || pContext == nullptr) {
                MGLOG_E("Diligent: failed to create Vulkan device/context");
                return false;
            }

            m_pDevice.Attach(pDevice);
            m_pContext.Attach(pContext);
            MGLOG_I("Diligent: Vulkan device created");
            return true;
        } catch (const std::exception& e) {
            MGLOG_W("Diligent: Vulkan device creation failed: %s", e.what());
            return false;
        } catch (...) {
            MGLOG_W("Diligent: Vulkan device creation failed");
            return false;
        }
    }

    void BackendObject_Diligent::Initialize() {
        if (m_initialized) {
            return;
        }
        if (!CreateDiligentDevice()) {
            MGLOG_W("Diligent: backend initialization failed");
            return;
        }

        m_pRenderer = std::make_unique<DiligentRenderer>(m_pDevice, m_pContext);
        if (!m_pRenderer->Initialize(256, 256)) {
            MGLOG_W("Diligent: renderer initialization failed");
            m_pRenderer.reset();
            return;
        }

        m_functions.GL.Clear = Clear;
        m_functions.GL.DrawArrays = DrawArrays;
        m_functions.GL.DrawElements = DrawElements;
        m_functions.Present = Present;

        m_initialized = true;
    }

    DiligentRenderer* BackendObject_Diligent::GetRenderer() {
        return m_pRenderer.get();
    }

    Bool BackendObject_Diligent::InitCapabilities() {
        // Skeleton: no format probing yet. The backend advertises GL 3.2 core
        // capability, and the capability tables will be filled as resource
        // creation paths are ported.
        m_backendCapabilitiesInitialized = true;
        return true;
    }

    Bool BackendObject_Diligent::InitWindowSurface() {
        // Skeleton: no native swapchain creation yet.
        return true;
    }

    const RendererInfo& BackendObject_Diligent::GetRendererInfo() const {
        return m_rendererInfo;
    }

    String BackendObject_Diligent::GetBackendAPIVersionString() const {
        return "Diligent Vulkan 0.1 (GL 3.2 skeleton)";
    }

    const GlobalBackendFunctionsTable& BackendObject_Diligent::GetBackendFunctions() const {
        return m_functions;
    }

    const DynamicBackendParameters& BackendObject_Diligent::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    BackendType BackendObject_Diligent::GetBackendType() const {
        return BackendType::DiligentVulkan;
    }
} // namespace MobileGL::MG_Backend::DiligentBackend
