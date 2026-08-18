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

#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <spirv_reflect.h>

#include <vector>

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

        SizeT GetDataTypeSize(DataType type) {
            switch (type) {
            case DataType::Int8:
            case DataType::Uint8:
                return 1;
            case DataType::Int16:
            case DataType::Uint16:
            case DataType::Float16:
                return 2;
            case DataType::Int32:
            case DataType::Uint32:
            case DataType::Float32:
            case DataType::Fixed32:
            case DataType::Int2101010Rev:
            case DataType::Uint2101010Rev:
                return 4;
            case DataType::Float64:
                return 8;
            default:
                return 4;
            }
        }

        ::Diligent::VALUE_TYPE GetValueType(DataType type) {
            switch (type) {
            case DataType::Int8: return ::Diligent::VT_INT8;
            case DataType::Uint8: return ::Diligent::VT_UINT8;
            case DataType::Int16: return ::Diligent::VT_INT16;
            case DataType::Uint16: return ::Diligent::VT_UINT16;
            case DataType::Int32: return ::Diligent::VT_INT32;
            case DataType::Uint32: return ::Diligent::VT_UINT32;
            case DataType::Float16: return ::Diligent::VT_FLOAT16;
            case DataType::Float32: return ::Diligent::VT_FLOAT32;
            case DataType::Float64: return ::Diligent::VT_FLOAT64;
            default: return ::Diligent::VT_FLOAT32;
            }
        }

        Bool GetSpirvStage(const Vector<unsigned>& spv, ::Diligent::SHADER_TYPE& outStage) {
            if (spv.empty()) {
                return false;
            }
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(spv.size() * sizeof(unsigned), spv.data(), &module) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                return false;
            }
            const SpvReflectShaderStageFlagBits stage = module.shader_stage;
            spvReflectDestroyShaderModule(&module);
            switch (stage) {
            case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
                outStage = ::Diligent::SHADER_TYPE_VERTEX;
                return true;
            case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
                outStage = ::Diligent::SHADER_TYPE_PIXEL;
                return true;
            default:
                return false;
            }
        }

        ::Diligent::BLEND_FACTOR ConvertBlendFactor(BlendFactor factor) {
            switch (factor) {
            case BlendFactor::Zero: return ::Diligent::BLEND_FACTOR_ZERO;
            case BlendFactor::One: return ::Diligent::BLEND_FACTOR_ONE;
            case BlendFactor::SrcColor: return ::Diligent::BLEND_FACTOR_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor: return ::Diligent::BLEND_FACTOR_INV_SRC_COLOR;
            case BlendFactor::DstColor: return ::Diligent::BLEND_FACTOR_DEST_COLOR;
            case BlendFactor::OneMinusDstColor: return ::Diligent::BLEND_FACTOR_INV_DEST_COLOR;
            case BlendFactor::SrcAlpha: return ::Diligent::BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha: return ::Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
            case BlendFactor::DstAlpha: return ::Diligent::BLEND_FACTOR_DEST_ALPHA;
            case BlendFactor::OneMinusDstAlpha: return ::Diligent::BLEND_FACTOR_INV_DEST_ALPHA;
            case BlendFactor::ConstantColor: return ::Diligent::BLEND_FACTOR_BLEND_FACTOR;
            case BlendFactor::OneMinusConstantColor: return ::Diligent::BLEND_FACTOR_INV_BLEND_FACTOR;
            case BlendFactor::Src1Color: return ::Diligent::BLEND_FACTOR_SRC1_COLOR;
            case BlendFactor::OneMinusSrc1Color: return ::Diligent::BLEND_FACTOR_INV_SRC1_COLOR;
            case BlendFactor::Src1Alpha: return ::Diligent::BLEND_FACTOR_SRC1_ALPHA;
            case BlendFactor::OneMinusSrc1Alpha: return ::Diligent::BLEND_FACTOR_INV_SRC1_ALPHA;
            default: return ::Diligent::BLEND_FACTOR_ONE;
            }
        }

        ::Diligent::BLEND_OPERATION ConvertBlendEquation(BlendEquation equation) {
            switch (equation) {
            case BlendEquation::Add: return ::Diligent::BLEND_OPERATION_ADD;
            case BlendEquation::Subtract: return ::Diligent::BLEND_OPERATION_SUBTRACT;
            case BlendEquation::ReverseSubtract: return ::Diligent::BLEND_OPERATION_REV_SUBTRACT;
            case BlendEquation::Min: return ::Diligent::BLEND_OPERATION_MIN;
            case BlendEquation::Max: return ::Diligent::BLEND_OPERATION_MAX;
            default: return ::Diligent::BLEND_OPERATION_ADD;
            }
        }

        ::Diligent::COMPARISON_FUNCTION ConvertDepthFunc(DepthTestFunc func) {
            switch (func) {
            case DepthTestFunc::Never: return ::Diligent::COMPARISON_FUNC_NEVER;
            case DepthTestFunc::Less: return ::Diligent::COMPARISON_FUNC_LESS;
            case DepthTestFunc::Equal: return ::Diligent::COMPARISON_FUNC_EQUAL;
            case DepthTestFunc::LessEqual: return ::Diligent::COMPARISON_FUNC_LESS_EQUAL;
            case DepthTestFunc::Greater: return ::Diligent::COMPARISON_FUNC_GREATER;
            case DepthTestFunc::NotEqual: return ::Diligent::COMPARISON_FUNC_NOT_EQUAL;
            case DepthTestFunc::GreaterEqual: return ::Diligent::COMPARISON_FUNC_GREATER_EQUAL;
            case DepthTestFunc::Always: return ::Diligent::COMPARISON_FUNC_ALWAYS;
            default: return ::Diligent::COMPARISON_FUNC_ALWAYS;
            }
        }

        ::Diligent::CULL_MODE ConvertCullMode(CullFaceMode mode) {
            switch (mode) {
            case CullFaceMode::Front: return ::Diligent::CULL_MODE_FRONT;
            case CullFaceMode::Back: return ::Diligent::CULL_MODE_BACK;
            case CullFaceMode::FrontAndBack: return ::Diligent::CULL_MODE_NONE;
            default: return ::Diligent::CULL_MODE_NONE;
            }
        }
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

            m_pVertexBuffer.Release();
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

    void DiligentRenderer::DrawFromState(GLenum mode, GLint first, GLsizei count, GLenum type, const void* indices) {
        if (!m_initialized || !m_pContext || MG_State::pGLContext == nullptr) {
            return;
        }
        if (!CreatePipelineFromState(mode)) {
            return;
        }
        if (!UploadVertexDataFromState(mode, first, count, type, indices)) {
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
        drawAttrs.NumVertices = m_lastDrawVertexCount;
        drawAttrs.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        m_pContext->Draw(drawAttrs);
    }

    Bool DiligentRenderer::CreatePipelineFromState(GLenum mode) {
        if (MG_State::pGLContext == nullptr) {
            return false;
        }

        const auto& program = MG_State::pGLContext->GetProgramForDraw();
        if (!program || !program->GetSpirvStatus()) {
            return false;
        }

        const auto& spirvs = program->GetGeneratedSpirv();
        ::Diligent::RefCntAutoPtr<::Diligent::IShader> pVS;
        ::Diligent::RefCntAutoPtr<::Diligent::IShader> pPS;
        for (const auto& spv : spirvs) {
            ::Diligent::SHADER_TYPE stage = ::Diligent::SHADER_TYPE_UNKNOWN;
            if (!GetSpirvStage(spv, stage)) {
                continue;
            }
            ::Diligent::ShaderCreateInfo shaderCI;
            shaderCI.ByteCode = spv.data();
            shaderCI.ByteCodeSize = spv.size() * sizeof(unsigned);
            shaderCI.Desc = {"MobileGL Diligent state shader", stage, true};
            ::Diligent::RefCntAutoPtr<::Diligent::IShader> pShader;
            m_pDevice->CreateShader(shaderCI, &pShader);
            if (!pShader) {
                MGLOG_E("DiligentRenderer: failed to create state shader");
                return false;
            }
            if (stage == ::Diligent::SHADER_TYPE_VERTEX) {
                pVS = pShader;
            } else if (stage == ::Diligent::SHADER_TYPE_PIXEL) {
                pPS = pShader;
            }
        }
        if (!pVS || !pPS) {
            MGLOG_W("DiligentRenderer: state program has no vertex/pixel SPIR-V");
            return false;
        }

        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& attributes = vao.GetAllAttributes();

        Vector<::Diligent::LayoutElement> layoutElements;
        Vector<Uint32> activeAttribs;
        for (Uint32 i = 0; i < vao.MAX_VERTEX_ATTRIBS; ++i) {
            const auto& attr = attributes[i];
            if (!attr.Enabled) {
                continue;
            }
            ::Diligent::LayoutElement elem{};
            elem.InputIndex = i;
            elem.BufferSlot = 0;
            elem.NumComponents = static_cast<::Diligent::Uint32>(attr.Size);
            elem.ValueType = GetValueType(attr.Type);
            elem.IsNormalized = attr.Normalized;
            activeAttribs.push_back(i);
            layoutElements.push_back(elem);
        }
        if (layoutElements.empty()) {
            MGLOG_W("DiligentRenderer: no enabled vertex attributes");
            return false;
        }

        ::Diligent::GraphicsPipelineStateCreateInfo psoCI;
        auto& psoDesc = psoCI.PSODesc;
        auto& graphicsPipeline = psoCI.GraphicsPipeline;
        psoDesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
        psoDesc.Name = "MobileGL Diligent state PSO";
        graphicsPipeline.NumRenderTargets = 1;
        graphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        switch (mode) {
        case GL_POINTS:
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case GL_LINES:
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case GL_LINE_STRIP:
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case GL_TRIANGLES:
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        case GL_TRIANGLE_STRIP:
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        default:
            // GL_TRIANGLE_FAN and GL_LINE_LOOP are emulated in UploadVertexData.
            graphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        }
        const Bool blendEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::Blend);
        const Bool depthEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::DepthTest);
        const Bool cullEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::CullFace);
        const Bool scissorEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest);
        (void)scissorEnabled;

        if (blendEnabled) {
            BlendFactor srcRGB = BlendFactor::One;
            BlendFactor dstRGB = BlendFactor::Zero;
            BlendFactor srcAlpha = BlendFactor::One;
            BlendFactor dstAlpha = BlendFactor::Zero;
            BlendEquation eqRGB = BlendEquation::Add;
            BlendEquation eqAlpha = BlendEquation::Add;
            MG_State::pGLContext->GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
            MG_State::pGLContext->GetBlendEquation(eqRGB, eqAlpha);

            auto& rtBlend = graphicsPipeline.BlendDesc.RenderTargets[0];
            rtBlend.BlendEnable = true;
            rtBlend.SrcBlend = ConvertBlendFactor(srcRGB);
            rtBlend.DestBlend = ConvertBlendFactor(dstRGB);
            rtBlend.BlendOp = ConvertBlendEquation(eqRGB);
            rtBlend.SrcBlendAlpha = ConvertBlendFactor(srcAlpha);
            rtBlend.DestBlendAlpha = ConvertBlendFactor(dstAlpha);
            rtBlend.BlendOpAlpha = ConvertBlendEquation(eqAlpha);
        }

        graphicsPipeline.RasterizerDesc.CullMode = cullEnabled ? ConvertCullMode(MG_State::pGLContext->GetCullFaceMode())
                                                               : ::Diligent::CULL_MODE_NONE;
        graphicsPipeline.RasterizerDesc.FrontCounterClockwise =
            MG_State::pGLContext->GetFrontFaceMode() == FrontFaceMode::CounterClockwise;
        graphicsPipeline.DepthStencilDesc.DepthEnable = depthEnabled;
        graphicsPipeline.DepthStencilDesc.DepthWriteEnable = MG_State::pGLContext->GetDepthMask();
        graphicsPipeline.DepthStencilDesc.DepthFunc = ConvertDepthFunc(MG_State::pGLContext->GetDepthFunc());
        graphicsPipeline.InputLayout.LayoutElements = layoutElements.data();
        graphicsPipeline.InputLayout.NumElements = static_cast<::Diligent::Uint32>(layoutElements.size());

        psoCI.pVS = pVS;
        psoCI.pPS = pPS;
        m_pPSO.Release();
        m_pDevice->CreateGraphicsPipelineState(psoCI, &m_pPSO);
        if (!m_pPSO) {
            MGLOG_E("DiligentRenderer: failed to create state pipeline");
            return false;
        }
        return true;
    }

    Bool DiligentRenderer::UploadVertexDataFromState(GLenum mode, GLint first, GLsizei count, GLenum type,
                                                     const void* indices) {
        if (MG_State::pGLContext == nullptr) {
            return false;
        }

        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& attributes = vao.GetAllAttributes();

        Vector<Uint32> activeAttribs;
        for (Uint32 i = 0; i < vao.MAX_VERTEX_ATTRIBS; ++i) {
            if (attributes[i].Enabled) {
                activeAttribs.push_back(i);
            }
        }
        if (activeAttribs.empty()) {
            return false;
        }

        // Resolve the vertex index list (DrawArrays first..first+count or
        // DrawElements index buffer/client memory).
        Vector<Uint32> indicesData;
        const Uint32 vertexCount = static_cast<Uint32>(count);
        if (mode == GL_TRIANGLE_FAN) {
            // Expand a triangle fan into a triangle list.
            indicesData.reserve((vertexCount - 2) * 3);
            for (Uint32 i = 1; i + 1 < vertexCount; ++i) {
                indicesData.push_back(0);
                indicesData.push_back(i);
                indicesData.push_back(i + 1);
            }
        } else if (mode == GL_LINE_LOOP) {
            indicesData.reserve(vertexCount + 1);
            for (Uint32 i = 0; i < vertexCount; ++i) {
                indicesData.push_back(i);
            }
            if (vertexCount > 0) {
                indicesData.push_back(0);
            }
        } else if (type != 0 || indices != nullptr) {
            // DrawElements path.
            const auto& indexSlot = vao.GetIndexBufferBindingSlot();
            const auto& indexBuffer = indexSlot.GetBoundObject();
            const Uint8* indexBase = nullptr;
            if (indexBuffer) {
                indexBuffer->SyncPersistentMappedRange();
                indexBase = indexBuffer->MappedData();
                if (indexBase == nullptr) {
                    return false;
                }
                indexBase += reinterpret_cast<SizeT>(indices);
            } else {
                indexBase = static_cast<const Uint8*>(indices);
                if (indexBase == nullptr) {
                    return false;
                }
            }

            const SizeT indexSize = GetDataTypeSize(
                type == GL_UNSIGNED_BYTE ? DataType::Uint8 :
                type == GL_UNSIGNED_SHORT ? DataType::Uint16 : DataType::Uint32);
            indicesData.reserve(vertexCount);
            for (Uint32 i = 0; i < vertexCount; ++i) {
                if (indexSize == 1) {
                    indicesData.push_back(indexBase[i]);
                } else if (indexSize == 2) {
                    indicesData.push_back(reinterpret_cast<const Uint16*>(indexBase)[i]);
                } else {
                    indicesData.push_back(reinterpret_cast<const Uint32*>(indexBase)[i]);
                }
            }
        }

        const Uint32 drawVertexCount = indicesData.empty() ? vertexCount : static_cast<Uint32>(indicesData.size());
        if (drawVertexCount == 0) {
            return false;
        }

        // Pack enabled attributes into a single interleaved vertex buffer.
        SizeT vertexStride = 0;
        for (Uint32 attrIndex : activeAttribs) {
            vertexStride += GetDataTypeSize(attributes[attrIndex].Type) * static_cast<SizeT>(attributes[attrIndex].Size);
        }

        Vector<Uint8> vertexData(static_cast<SizeT>(drawVertexCount) * vertexStride);
        for (Uint32 vi = 0; vi < drawVertexCount; ++vi) {
            const Uint32 srcIndex = indicesData.empty() ? (static_cast<Uint32>(first) + vi) : indicesData[vi];
            Uint8* dst = vertexData.data() + static_cast<SizeT>(vi) * vertexStride;
            for (Uint32 attrIndex : activeAttribs) {
                const auto& attr = attributes[attrIndex];
                if (!attr.Buffer) {
                    return false;
                }
                attr.Buffer->SyncPersistentMappedRange();
                const Uint8* src = attr.Buffer->MappedData();
                if (src == nullptr) {
                    return false;
                }
                const SizeT elemSize = GetDataTypeSize(attr.Type) * static_cast<SizeT>(attr.Size);
                const SizeT srcOffset = attr.Offset + static_cast<SizeT>(srcIndex) * static_cast<SizeT>(attr.Stride);
                if (srcOffset + elemSize > attr.Buffer->GetSize()) {
                    return false;
                }
                std::memcpy(dst, src + srcOffset, elemSize);
                dst += elemSize;
            }
        }

        if (!m_pVertexBuffer || m_pVertexBuffer->GetDesc().Size < vertexData.size()) {
            ::Diligent::BufferDesc buffDesc;
            buffDesc.Name = "MobileGL Diligent state vertex buffer";
            buffDesc.BindFlags = ::Diligent::BIND_VERTEX_BUFFER;
            buffDesc.Size = vertexData.size();
            ::Diligent::BufferData initialData;
            initialData.pData = vertexData.data();
            initialData.DataSize = static_cast<::Diligent::Uint32>(vertexData.size());
            m_pVertexBuffer.Release();
            m_pDevice->CreateBuffer(buffDesc, &initialData, &m_pVertexBuffer);
            if (!m_pVertexBuffer) {
                return false;
            }
        } else {
            m_pContext->UpdateBuffer(m_pVertexBuffer, 0, vertexData.size(), vertexData.data(),
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        m_lastDrawVertexCount = drawVertexCount;
        return true;
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
