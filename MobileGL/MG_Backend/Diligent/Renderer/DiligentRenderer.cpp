// MobileGL - MobileGL/MG_Backend/Diligent/Renderer/DiligentRenderer.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#include "DiligentRenderer.h"
#include <RenderDevice.h>
#include <DeviceContext.h>
#include <Texture.h>
#include <Buffer.h>
#include <Shader.h>
#include <PipelineState.h>
#include <InputLayout.h>

namespace MobileGL::MG_Backend::DiligentBackend {
    namespace {
        constexpr Uint32 kTriangleVertexCount = 3;

        const char* GetTriangleVS() {
            return R"(layout(location = 0) in vec2 Position;
void main()
{
    gl_Position = vec4(Position, 0.0, 1.0);
}
)";
        }

        const char* GetTrianglePS() {
            return R"(layout(location = 0) out vec4 Color;
void main()
{
    Color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";
        }

        struct TriangleVertex {
            Float X;
            Float Y;
        };
    } // namespace

    DiligentRenderer::DiligentRenderer(::Diligent::IRenderDevice* device, ::Diligent::IDeviceContext* context)
        : m_pDevice(device), m_pContext(context) {}

    DiligentRenderer::~DiligentRenderer() {
        m_pVertexBuffer.Release();
        m_pPSO.Release();
        m_pColorRTV.Release();
        m_pColorTarget.Release();
    }

    Bool DiligentRenderer::Initialize(Uint32 width, Uint32 height) {
        if (m_initialized) {
            return true;
        }
        if (m_pDevice == nullptr || m_pContext == nullptr) {
            return false;
        }

        m_width = width > 0 ? width : 256;
        m_height = height > 0 ? height : 256;

        if (!CreateOffscreenTargets()) {
            return false;
        }
        if (!CreatePipeline()) {
            return false;
        }
        if (!CreateVertexBuffer()) {
            return false;
        }

        m_initialized = true;
        return true;
    }

    Bool DiligentRenderer::CreateOffscreenTargets() {
        ::Diligent::TextureDesc texDesc;
        texDesc.Name = "MobileGL Diligent offscreen color target";
        texDesc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        texDesc.Format = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        texDesc.Width = m_width;
        texDesc.Height = m_height;
        texDesc.MipLevels = 1;
        texDesc.BindFlags = ::Diligent::BIND_RENDER_TARGET | ::Diligent::BIND_SHADER_RESOURCE;
        texDesc.Usage = ::Diligent::USAGE_DEFAULT;

        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pColorTarget;
        m_pDevice->CreateTexture(texDesc, nullptr, &pColorTarget);
        if (!pColorTarget) {
            MGLOG_E("DiligentRenderer: failed to create offscreen color target");
            return false;
        }

        m_pColorTarget = pColorTarget;
        m_pColorRTV = m_pColorTarget->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
        if (!m_pColorRTV) {
            MGLOG_E("DiligentRenderer: failed to get offscreen RTV");
            return false;
        }
        return true;
    }

    Bool DiligentRenderer::CreatePipeline() {
        ::Diligent::GraphicsPipelineStateCreateInfo psoCI;
        auto& psoDesc = psoCI.PSODesc;
        auto& graphicsPipeline = psoCI.GraphicsPipeline;

        psoDesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
        psoDesc.Name = "MobileGL Diligent triangle PSO";
        graphicsPipeline.NumRenderTargets = 1;
        graphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        graphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
        graphicsPipeline.DepthStencilDesc.DepthEnable = false;

        ::Diligent::LayoutElement layoutElems[] = {
            {0, 0, 2, ::Diligent::VT_FLOAT32},
        };
        graphicsPipeline.InputLayout.LayoutElements = layoutElems;
        graphicsPipeline.InputLayout.NumElements = 1;

        ::Diligent::ShaderCreateInfo shaderCI;
        shaderCI.SourceLanguage = ::Diligent::SHADER_SOURCE_LANGUAGE_GLSL;
        shaderCI.EntryPoint = "main";

        ::Diligent::RefCntAutoPtr<::Diligent::IShader> pVS;
        shaderCI.Desc = {"MobileGL Diligent triangle VS", ::Diligent::SHADER_TYPE_VERTEX, true};
        shaderCI.Source = GetTriangleVS();
        m_pDevice->CreateShader(shaderCI, &pVS);
        if (!pVS) {
            MGLOG_E("DiligentRenderer: failed to create vertex shader");
            return false;
        }

        ::Diligent::RefCntAutoPtr<::Diligent::IShader> pPS;
        shaderCI.Desc = {"MobileGL Diligent triangle PS", ::Diligent::SHADER_TYPE_PIXEL, true};
        shaderCI.Source = GetTrianglePS();
        m_pDevice->CreateShader(shaderCI, &pPS);
        if (!pPS) {
            MGLOG_E("DiligentRenderer: failed to create pixel shader");
            return false;
        }

        psoCI.pVS = pVS;
        psoCI.pPS = pPS;
        m_pDevice->CreateGraphicsPipelineState(psoCI, &m_pPSO);
        if (!m_pPSO) {
            MGLOG_E("DiligentRenderer: failed to create pipeline state");
            return false;
        }
        return true;
    }

    Bool DiligentRenderer::CreateVertexBuffer() {
        const TriangleVertex vertices[] = {
            {-0.5f, -0.5f},
            { 0.5f, -0.5f},
            { 0.0f,  0.5f},
        };

        ::Diligent::BufferDesc buffDesc;
        buffDesc.Name = "MobileGL Diligent triangle vertex buffer";
        buffDesc.BindFlags = ::Diligent::BIND_VERTEX_BUFFER;
        buffDesc.Size = sizeof(vertices);

        ::Diligent::BufferData initialData;
        initialData.pData = vertices;
        initialData.DataSize = sizeof(vertices);

        m_pDevice->CreateBuffer(buffDesc, &initialData, &m_pVertexBuffer);
        if (!m_pVertexBuffer) {
            MGLOG_E("DiligentRenderer: failed to create vertex buffer");
            return false;
        }
        return true;
    }

    void DiligentRenderer::Clear(Float r, Float g, Float b, Float a) {
        if (!m_initialized || !m_pContext || !m_pColorRTV) {
            return;
        }
        ::Diligent::ITextureView* rtvs[] = {m_pColorRTV};
        m_pContext->SetRenderTargets(1, rtvs, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const Float clearColor[] = {r, g, b, a};
        m_pContext->ClearRenderTarget(m_pColorRTV, clearColor, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    void DiligentRenderer::DrawTriangle() {
        if (!m_initialized || !m_pContext || !m_pPSO) {
            return;
        }

        ::Diligent::ITextureView* rtvs[] = {m_pColorRTV};
        m_pContext->SetRenderTargets(1, rtvs, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        ::Diligent::Viewport viewport{0, 0, static_cast<Float>(m_width), static_cast<Float>(m_height), 0.0f, 1.0f};
        m_pContext->SetViewports(1, &viewport, m_width, m_height);

        ::Diligent::IBuffer* pVBs[] = {m_pVertexBuffer};
        m_pContext->SetVertexBuffers(0, 1, pVBs, nullptr,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                     ::Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);

        m_pContext->SetPipelineState(m_pPSO);
        ::Diligent::DrawAttribs drawAttrs;
        drawAttrs.NumVertices = kTriangleVertexCount;
        drawAttrs.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        m_pContext->Draw(drawAttrs);
    }

    void DiligentRenderer::DrawVertices(const Float* vertices, Uint32 vertexCount) {
        if (!m_initialized || !m_pContext || !m_pPSO || vertices == nullptr || vertexCount == 0) {
            return;
        }

        const Uint64 dataSize = static_cast<Uint64>(vertexCount) * 2 * sizeof(Float);
        if (dataSize > m_pVertexBuffer->GetDesc().Size) {
            ::Diligent::BufferDesc buffDesc;
            buffDesc.Name = "MobileGL Diligent dynamic vertex buffer";
            buffDesc.BindFlags = ::Diligent::BIND_VERTEX_BUFFER;
            buffDesc.Size = dataSize;

            ::Diligent::BufferData initialData;
            initialData.pData = vertices;
            initialData.DataSize = static_cast<Uint32>(dataSize);

            m_pDevice->CreateBuffer(buffDesc, &initialData, &m_pVertexBuffer);
            if (!m_pVertexBuffer) {
                MGLOG_E("DiligentRenderer: failed to create dynamic vertex buffer");
                return;
            }
        } else {
            m_pContext->UpdateBuffer(m_pVertexBuffer, 0, dataSize, vertices,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }

        ::Diligent::ITextureView* rtvs[] = {m_pColorRTV};
        m_pContext->SetRenderTargets(1, rtvs, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        ::Diligent::Viewport viewport{0, 0, static_cast<Float>(m_width), static_cast<Float>(m_height), 0.0f, 1.0f};
        m_pContext->SetViewports(1, &viewport, m_width, m_height);

        ::Diligent::IBuffer* pVBs[] = {m_pVertexBuffer};
        m_pContext->SetVertexBuffers(0, 1, pVBs, nullptr,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                     ::Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);

        m_pContext->SetPipelineState(m_pPSO);
        ::Diligent::DrawAttribs drawAttrs;
        drawAttrs.NumVertices = vertexCount;
        drawAttrs.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        m_pContext->Draw(drawAttrs);
    }

    void DiligentRenderer::ReadPixels(Uint32 x, Uint32 y, Uint32 width, Uint32 height, void* pixels) {
        if (!m_initialized || !m_pContext || !m_pColorTarget || pixels == nullptr) {
            return;
        }

        ::Diligent::TextureDesc stagingDesc;
        stagingDesc.Name = "MobileGL Diligent staging readback";
        stagingDesc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        stagingDesc.Format = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        stagingDesc.Width = m_width;
        stagingDesc.Height = m_height;
        stagingDesc.MipLevels = 1;
        stagingDesc.Usage = ::Diligent::USAGE_STAGING;
        stagingDesc.CPUAccessFlags = ::Diligent::CPU_ACCESS_READ;

        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pStaging;
        m_pDevice->CreateTexture(stagingDesc, nullptr, &pStaging);
        if (!pStaging) {
            MGLOG_E("DiligentRenderer: failed to create staging texture");
            return;
        }

        ::Diligent::CopyTextureAttribs copyAttribs{
            m_pColorTarget, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
            pStaging, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        copyAttribs.SrcMipLevel = 0;
        copyAttribs.DstMipLevel = 0;
        copyAttribs.SrcSlice = 0;
        copyAttribs.DstSlice = 0;
        m_pContext->CopyTexture(copyAttribs);
        m_pContext->WaitForIdle();

        ::Diligent::MappedTextureSubresource mapped;
        m_pContext->MapTextureSubresource(pStaging, 0, 0, ::Diligent::MAP_READ,
                                          ::Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
        if (mapped.pData == nullptr) {
            MGLOG_E("DiligentRenderer: failed to map staging texture");
            return;
        }

        const Uint8* srcBase = static_cast<const Uint8*>(mapped.pData) + y * mapped.Stride + x * 4;
        Uint8* dst = static_cast<Uint8*>(pixels);
        for (Uint32 row = 0; row < height; ++row) {
            std::memcpy(dst + row * width * 4, srcBase + row * mapped.Stride, width * 4);
        }

        m_pContext->UnmapTextureSubresource(pStaging, 0, 0);
    }

    void DiligentRenderer::Present() {
        // Offscreen renderer: nothing to present yet.
        if (m_pContext) {
            m_pContext->Flush();
        }
    }
} // namespace MobileGL::MG_Backend::DiligentBackend
