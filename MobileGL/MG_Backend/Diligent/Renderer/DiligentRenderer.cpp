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
#include <Sampler.h>
#include <ShaderResourceBinding.h>
#include <ShaderResourceVariable.h>

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

        ::Diligent::STENCIL_OP ConvertStencilOp(StencilOperation op) {
            switch (op) {
            case StencilOperation::Keep: return ::Diligent::STENCIL_OP_KEEP;
            case StencilOperation::Zero: return ::Diligent::STENCIL_OP_ZERO;
            case StencilOperation::Replace: return ::Diligent::STENCIL_OP_REPLACE;
            case StencilOperation::IncrementClamp: return ::Diligent::STENCIL_OP_INCR_SAT;
            case StencilOperation::DecrementClamp: return ::Diligent::STENCIL_OP_DECR_SAT;
            case StencilOperation::Invert: return ::Diligent::STENCIL_OP_INVERT;
            case StencilOperation::IncrementWrap: return ::Diligent::STENCIL_OP_INCR_WRAP;
            case StencilOperation::DecrementWrap: return ::Diligent::STENCIL_OP_DECR_WRAP;
            default: return ::Diligent::STENCIL_OP_KEEP;
            }
        }

        Bool IsDepthFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
                return true;
            default:
                return false;
            }
        }

        ::Diligent::TEXTURE_FORMAT ConvertInternalFormatToDiligent(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8: return ::Diligent::TEX_FORMAT_R8_UNORM;
            case TextureInternalFormat::R8Snorm: return ::Diligent::TEX_FORMAT_R8_SNORM;
            case TextureInternalFormat::R8I: return ::Diligent::TEX_FORMAT_R8_SINT;
            case TextureInternalFormat::R8UI: return ::Diligent::TEX_FORMAT_R8_UINT;
            case TextureInternalFormat::R16: return ::Diligent::TEX_FORMAT_R16_UNORM;
            case TextureInternalFormat::R16Snorm: return ::Diligent::TEX_FORMAT_R16_SNORM;
            case TextureInternalFormat::R16I: return ::Diligent::TEX_FORMAT_R16_SINT;
            case TextureInternalFormat::R16UI: return ::Diligent::TEX_FORMAT_R16_UINT;
            case TextureInternalFormat::R16F: return ::Diligent::TEX_FORMAT_R16_FLOAT;
            case TextureInternalFormat::R32I: return ::Diligent::TEX_FORMAT_R32_SINT;
            case TextureInternalFormat::R32UI: return ::Diligent::TEX_FORMAT_R32_UINT;
            case TextureInternalFormat::R32F: return ::Diligent::TEX_FORMAT_R32_FLOAT;
            case TextureInternalFormat::RG8: return ::Diligent::TEX_FORMAT_RG8_UNORM;
            case TextureInternalFormat::RG8Snorm: return ::Diligent::TEX_FORMAT_RG8_SNORM;
            case TextureInternalFormat::RG8I: return ::Diligent::TEX_FORMAT_RG8_SINT;
            case TextureInternalFormat::RG8UI: return ::Diligent::TEX_FORMAT_RG8_UINT;
            case TextureInternalFormat::RG16: return ::Diligent::TEX_FORMAT_RG16_UNORM;
            case TextureInternalFormat::RG16Snorm: return ::Diligent::TEX_FORMAT_RG16_SNORM;
            case TextureInternalFormat::RG16I: return ::Diligent::TEX_FORMAT_RG16_SINT;
            case TextureInternalFormat::RG16UI: return ::Diligent::TEX_FORMAT_RG16_UINT;
            case TextureInternalFormat::RG16F: return ::Diligent::TEX_FORMAT_RG16_FLOAT;
            case TextureInternalFormat::RG32I: return ::Diligent::TEX_FORMAT_RG32_SINT;
            case TextureInternalFormat::RG32UI: return ::Diligent::TEX_FORMAT_RG32_UINT;
            case TextureInternalFormat::RG32F: return ::Diligent::TEX_FORMAT_RG32_FLOAT;
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16F:
                // Diligent does not expose unsized/three-channel non-ETC2 RGB formats.
                // RGB32 is the only three-channel format available.
                return ::Diligent::TEX_FORMAT_UNKNOWN;
            case TextureInternalFormat::RGB32F: return ::Diligent::TEX_FORMAT_RGB32_FLOAT;
            case TextureInternalFormat::RGBA8: return ::Diligent::TEX_FORMAT_RGBA8_UNORM;
            case TextureInternalFormat::RGBA8Snorm: return ::Diligent::TEX_FORMAT_RGBA8_SNORM;
            case TextureInternalFormat::RGBA8I: return ::Diligent::TEX_FORMAT_RGBA8_SINT;
            case TextureInternalFormat::RGBA8UI: return ::Diligent::TEX_FORMAT_RGBA8_UINT;
            case TextureInternalFormat::RGBA16: return ::Diligent::TEX_FORMAT_RGBA16_UNORM;
            case TextureInternalFormat::RGBA16Snorm: return ::Diligent::TEX_FORMAT_RGBA16_SNORM;
            case TextureInternalFormat::RGBA16I: return ::Diligent::TEX_FORMAT_RGBA16_SINT;
            case TextureInternalFormat::RGBA16UI: return ::Diligent::TEX_FORMAT_RGBA16_UINT;
            case TextureInternalFormat::RGBA16F: return ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
            case TextureInternalFormat::RGBA32I: return ::Diligent::TEX_FORMAT_RGBA32_SINT;
            case TextureInternalFormat::RGBA32UI: return ::Diligent::TEX_FORMAT_RGBA32_UINT;
            case TextureInternalFormat::RGBA32F: return ::Diligent::TEX_FORMAT_RGBA32_FLOAT;
            case TextureInternalFormat::SRGB8: return ::Diligent::TEX_FORMAT_UNKNOWN;
            case TextureInternalFormat::SRGB8Alpha8: return ::Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
            case TextureInternalFormat::R11FG11FB10F: return ::Diligent::TEX_FORMAT_R11G11B10_FLOAT;
            case TextureInternalFormat::RGB9E5: return ::Diligent::TEX_FORMAT_RGB9E5_SHAREDEXP;
            case TextureInternalFormat::RGB10A2: return ::Diligent::TEX_FORMAT_RGB10A2_UNORM;
            case TextureInternalFormat::RGB10A2UI: return ::Diligent::TEX_FORMAT_RGB10A2_UINT;
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
                return ::Diligent::TEX_FORMAT_D32_FLOAT;
            case TextureInternalFormat::DepthComponent16:
                return ::Diligent::TEX_FORMAT_D16_UNORM;
            case TextureInternalFormat::DepthComponent24:
                return ::Diligent::TEX_FORMAT_D24_UNORM_S8_UINT;
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::DepthStencil:
                return ::Diligent::TEX_FORMAT_D24_UNORM_S8_UINT;
            case TextureInternalFormat::Depth32FStencil8:
                return ::Diligent::TEX_FORMAT_D32_FLOAT_S8X24_UINT;
            case TextureInternalFormat::Red:
            case TextureInternalFormat::RGBA: // The frontend keeps unsized RGBA as RGBA8 in practice.
                return ::Diligent::TEX_FORMAT_RGBA8_UNORM;
            case TextureInternalFormat::RG:
                return ::Diligent::TEX_FORMAT_RG8_UNORM;
            case TextureInternalFormat::RGB:
                return ::Diligent::TEX_FORMAT_UNKNOWN;
            default:
                return ::Diligent::TEX_FORMAT_UNKNOWN;
            }
        }

        ::Diligent::RESOURCE_DIMENSION ConvertTextureTargetToDiligent(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D: return ::Diligent::RESOURCE_DIM_TEX_1D;
            case TextureTarget::Texture1DArray: return ::Diligent::RESOURCE_DIM_TEX_1D_ARRAY;
            case TextureTarget::Texture2D:
            case TextureTarget::TextureRectangle:
            case TextureTarget::Texture2DMultisample:
                return ::Diligent::RESOURCE_DIM_TEX_2D;
            case TextureTarget::Texture2DArray:
            case TextureTarget::Texture2DMultisampleArray:
                return ::Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
            case TextureTarget::Texture3D: return ::Diligent::RESOURCE_DIM_TEX_3D;
            case TextureTarget::TextureCubeMap: return ::Diligent::RESOURCE_DIM_TEX_CUBE;
            case TextureTarget::TextureCubeMapArray: return ::Diligent::RESOURCE_DIM_TEX_CUBE_ARRAY;
            default: return ::Diligent::RESOURCE_DIM_UNDEFINED;
            }
        }

        ::Diligent::TEXTURE_ADDRESS_MODE ConvertWrapMode(SamplerWrapMode mode) {
            switch (mode) {
            case SamplerWrapMode::ClampToEdge: return ::Diligent::TEXTURE_ADDRESS_CLAMP;
            case SamplerWrapMode::MirroredRepeat: return ::Diligent::TEXTURE_ADDRESS_MIRROR;
            case SamplerWrapMode::Repeat: return ::Diligent::TEXTURE_ADDRESS_WRAP;
            case SamplerWrapMode::ClampToBorder: return ::Diligent::TEXTURE_ADDRESS_BORDER;
            case SamplerWrapMode::MirrorClampToEdge: return ::Diligent::TEXTURE_ADDRESS_MIRROR_ONCE;
            default: return ::Diligent::TEXTURE_ADDRESS_WRAP;
            }
        }

        ::Diligent::COMPARISON_FUNCTION ConvertCompareFunc(SamplerCompareFunc func) {
            switch (func) {
            case SamplerCompareFunc::Never: return ::Diligent::COMPARISON_FUNC_NEVER;
            case SamplerCompareFunc::Less: return ::Diligent::COMPARISON_FUNC_LESS;
            case SamplerCompareFunc::Equal: return ::Diligent::COMPARISON_FUNC_EQUAL;
            case SamplerCompareFunc::LessEqual: return ::Diligent::COMPARISON_FUNC_LESS_EQUAL;
            case SamplerCompareFunc::Greater: return ::Diligent::COMPARISON_FUNC_GREATER;
            case SamplerCompareFunc::NotEqual: return ::Diligent::COMPARISON_FUNC_NOT_EQUAL;
            case SamplerCompareFunc::GreaterEqual: return ::Diligent::COMPARISON_FUNC_GREATER_EQUAL;
            case SamplerCompareFunc::Always: return ::Diligent::COMPARISON_FUNC_ALWAYS;
            default: return ::Diligent::COMPARISON_FUNC_ALWAYS;
            }
        }

        ::Diligent::FILTER_TYPE ConvertFilterType(SamplerFilterMode filter, SamplerMipmapMode mipmap,
                                                 Bool comparison, Float maxAnisotropy) {
            if (maxAnisotropy > 1.0f) {
                return comparison ? ::Diligent::FILTER_TYPE_COMPARISON_ANISOTROPIC
                                  : ::Diligent::FILTER_TYPE_ANISOTROPIC;
            }
            const Bool linearFilter = filter == SamplerFilterMode::Linear;
            const Bool linearMip = mipmap == SamplerMipmapMode::Linear;
            const Bool pointMip = mipmap == SamplerMipmapMode::Nearest || mipmap == SamplerMipmapMode::None;
            if (!comparison) {
                if (linearFilter) {
                    return linearMip ? ::Diligent::FILTER_TYPE_LINEAR : ::Diligent::FILTER_TYPE_POINT;
                }
                return pointMip ? ::Diligent::FILTER_TYPE_POINT : ::Diligent::FILTER_TYPE_LINEAR;
            }
            if (linearFilter) {
                return linearMip ? ::Diligent::FILTER_TYPE_COMPARISON_LINEAR : ::Diligent::FILTER_TYPE_COMPARISON_POINT;
            }
            return pointMip ? ::Diligent::FILTER_TYPE_COMPARISON_POINT : ::Diligent::FILTER_TYPE_COMPARISON_LINEAR;
        }
    } // namespace

    DiligentRenderer::DiligentRenderer(::Diligent::IRenderDevice* device, ::Diligent::IDeviceContext* context)
        : m_pDevice(device), m_pContext(context) {}

    DiligentRenderer::~DiligentRenderer() {
        m_pVertexBuffer.Release();
        m_pPSO.Release();
        m_pColorRTV.Release();
        m_pColorTarget.Release();
        m_pDepthDSV.Release();
        m_pDepthTarget.Release();
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

        ::Diligent::TextureDesc depthDesc;
        depthDesc.Name = "MobileGL Diligent offscreen depth target";
        depthDesc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        depthDesc.Format = ::Diligent::TEX_FORMAT_D32_FLOAT;
        depthDesc.Width = m_width;
        depthDesc.Height = m_height;
        depthDesc.MipLevels = 1;
        depthDesc.BindFlags = ::Diligent::BIND_DEPTH_STENCIL;
        depthDesc.Usage = ::Diligent::USAGE_DEFAULT;

        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pDepthTarget;
        m_pDevice->CreateTexture(depthDesc, nullptr, &pDepthTarget);
        if (!pDepthTarget) {
            MGLOG_E("DiligentRenderer: failed to create offscreen depth target");
            return false;
        }
        m_pDepthTarget = pDepthTarget;
        m_pDepthDSV = m_pDepthTarget->GetDefaultView(::Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
        if (!m_pDepthDSV) {
            MGLOG_E("DiligentRenderer: failed to get offscreen DSV");
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
        if (!m_initialized || !m_pContext) {
            return;
        }
        Vector<::Diligent::ITextureView*> rtvs;
        ::Diligent::ITextureView* dsv = nullptr;
        if (!ResolveCurrentRenderTargets(rtvs, dsv) || rtvs.empty()) {
            return;
        }
        m_pContext->SetRenderTargets(static_cast<::Diligent::Uint32>(rtvs.size()), rtvs.data(), dsv,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const Float clearColor[] = {r, g, b, a};
        for (auto* rtv : rtvs) {
            m_pContext->ClearRenderTarget(rtv, clearColor, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    void DiligentRenderer::ClearDepth(Float depth) {
        if (!m_initialized || !m_pContext) {
            return;
        }
        Vector<::Diligent::ITextureView*> rtvs;
        ::Diligent::ITextureView* dsv = nullptr;
        if (!ResolveCurrentRenderTargets(rtvs, dsv) || dsv == nullptr) {
            return;
        }
        m_pContext->SetRenderTargets(static_cast<::Diligent::Uint32>(rtvs.size()), rtvs.data(), dsv,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_pContext->ClearDepthStencil(dsv, ::Diligent::CLEAR_DEPTH_FLAG, depth, 0,
                                      ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    void DiligentRenderer::DrawTriangle() {
        if (!m_initialized || !m_pContext || !m_pPSO) {
            return;
        }

        ::Diligent::ITextureView* rtvs[] = {m_pColorRTV};
        m_pContext->SetRenderTargets(1, rtvs, m_pDepthDSV, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

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
        m_pContext->SetRenderTargets(1, rtvs, m_pDepthDSV, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

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

    Bool DiligentRenderer::CreateTestTexture(const void* data, Uint32 width, Uint32 height) {
        if (m_pDevice == nullptr || data == nullptr || width == 0 || height == 0) {
            return false;
        }

        ::Diligent::TextureDesc texDesc;
        texDesc.Name = "MobileGL Diligent test texture";
        texDesc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        texDesc.Format = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.BindFlags = ::Diligent::BIND_SHADER_RESOURCE;
        texDesc.Usage = ::Diligent::USAGE_DEFAULT;

        ::Diligent::TextureSubResData subResData;
        subResData.pData = data;
        subResData.Stride = static_cast<::Diligent::Uint64>(width) * 4;

        ::Diligent::TextureData texData;
        texData.pSubResources = &subResData;
        texData.NumSubresources = 1;

        m_pDevice->CreateTexture(texDesc, &texData, &m_pTestTexture);
        if (!m_pTestTexture) {
            return false;
        }
        m_pTestSRV = m_pTestTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        if (!m_pTestSRV) {
            return false;
        }

        ::Diligent::SamplerDesc samplerDesc;
        m_pDevice->CreateSampler(samplerDesc, &m_pTestSampler);
        if (m_pTestSampler) {
            m_pTestSRV->SetSampler(m_pTestSampler);
        }
        return m_pTestSampler != nullptr;
    }

    ::Diligent::ITextureView* DiligentRenderer::SyncTexture(MG_State::GLState::ITextureObject& texture) {
        if (m_pDevice == nullptr || m_pContext == nullptr || texture.GetFormat() == TextureInternalFormat::Unknown) {
            return nullptr;
        }

        const auto format = ConvertInternalFormatToDiligent(texture.GetFormat());
        const auto dimension = ConvertTextureTargetToDiligent(texture.GetTarget());
        if (format == ::Diligent::TEX_FORMAT_UNKNOWN || dimension == ::Diligent::RESOURCE_DIM_UNDEFINED) {
            MGLOG_W_ONCE("DiligentRenderer::SyncTexture: unsupported texture target/format (target=%d format=%d)",
                         static_cast<Int>(texture.GetTarget()), static_cast<Int>(texture.GetFormat()));
            return nullptr;
        }

        const Uint64 lifetimeId = texture.GetLifetimeId();
        TextureResource* resource = nullptr;
        auto it = m_textureCache.find(lifetimeId);
        if (it != m_textureCache.end()) {
            resource = &it->second;
        }

        const Bool needCreate = resource == nullptr || !resource->Texture;
        const Bool needUpload = needCreate || resource->ContentVersion != texture.GetContentVersion();
        const Bool needViewUpdate = needCreate || resource->ParamsVersion != texture.GetTextureParamsVersion();

        if (needCreate) {
            TextureResource newResource;
            newResource.ContentVersion = texture.GetContentVersion();
            newResource.ParamsVersion = texture.GetTextureParamsVersion();
            newResource.IsDepth = IsDepthFormat(texture.GetFormat());

            ::Diligent::TextureDesc texDesc;
            texDesc.Name = "MobileGL state texture";
            texDesc.Type = dimension;
            texDesc.Format = format;
            const IntVec3 baseSize = texture.GetBaseSize();
            texDesc.Width = static_cast<::Diligent::Uint32>(std::max(baseSize.x(), 1));
            texDesc.Height = static_cast<::Diligent::Uint32>(std::max(baseSize.y(), 1));
            texDesc.ArraySize = 1;
            if (texture.GetTarget() == TextureTarget::TextureCubeMap ||
                texture.GetTarget() == TextureTarget::TextureCubeMapArray) {
                texDesc.ArraySize = 6;
            } else if (texture.GetTarget() == TextureTarget::Texture2DArray ||
                       texture.GetTarget() == TextureTarget::Texture1DArray) {
                texDesc.ArraySize = static_cast<::Diligent::Uint32>(std::max(baseSize.z(), 1));
            }
            texDesc.MipLevels = 1;
            if (auto* mip = AsMipmapTexture(&texture)) {
                texDesc.MipLevels = std::max<::Diligent::Uint32>(mip->GetMipmapLevelCount(), 1);
            }
            texDesc.Usage = ::Diligent::USAGE_DEFAULT;
            texDesc.BindFlags = ::Diligent::BIND_SHADER_RESOURCE;
            if (newResource.IsDepth) {
                texDesc.BindFlags |= ::Diligent::BIND_DEPTH_STENCIL;
            } else {
                texDesc.BindFlags |= ::Diligent::BIND_RENDER_TARGET;
            }

            ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pTexture;
            m_pDevice->CreateTexture(texDesc, nullptr, &pTexture);
            if (!pTexture) {
                MGLOG_W_ONCE("DiligentRenderer::SyncTexture: failed to create texture (id=%llu)", lifetimeId);
                return nullptr;
            }
            newResource.Texture = pTexture;
            newResource.SRV = pTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
            if (!newResource.SRV) {
                MGLOG_W_ONCE("DiligentRenderer::SyncTexture: failed to create SRV (id=%llu)", lifetimeId);
                return nullptr;
            }
            if (newResource.IsDepth) {
                newResource.DSV = pTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
            } else if ((texDesc.BindFlags & ::Diligent::BIND_RENDER_TARGET) != 0) {
                newResource.RTV = pTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
            }

            auto inserted = m_textureCache.emplace(lifetimeId, std::move(newResource));
            resource = &inserted.first->second;
        } else if (needViewUpdate && resource->Texture) {
            // Recreate views if the texture's parameter/shape version moved. The content
            // upload below keeps the same ITexture; for now only the default views are used.
            resource->ParamsVersion = texture.GetTextureParamsVersion();
        }

        if (needUpload && resource->Texture) {
            auto* mip = AsMipmapTexture(&texture);
            if (mip != nullptr) {
                const auto& uploadTargets = texture.GetUploadTargets();
                for (const auto uploadTarget : uploadTargets) {
                    Uint32 slice = 0;
                    if (texture.GetTarget() == TextureTarget::TextureCubeMap &&
                        uploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
                        uploadTarget <= TextureUploadTarget::CubeMapNegativeZ) {
                        slice = static_cast<Uint32>(uploadTarget) -
                                static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
                    }
                    const Uint32 levelCount = mip->GetMipmapLevelCount();
                    for (Uint32 level = 0; level < levelCount; ++level) {
                        const IntVec3 levelSize = mip->GetMipmapTexelSize(uploadTarget, level);
                        if (levelSize.x() <= 0 || levelSize.y() <= 0) {
                            continue;
                        }
                        const SizeT byteSize = mip->GetMipmapByteSize(uploadTarget, level);
                        if (byteSize == 0) {
                            continue;
                        }
                        const void* data = mip->MapMipmapData(uploadTarget, level);
                        if (data == nullptr) {
                            continue;
                        }
                        const Uint32 depth = std::max(levelSize.z(), 1);
                        const Uint32 rowSize = static_cast<Uint32>(
                            byteSize / static_cast<SizeT>(std::max(levelSize.y(), 1)) / static_cast<SizeT>(depth));
                        ::Diligent::TextureSubResData subResData;
                        subResData.pData = data;
                        subResData.Stride = rowSize;
                        ::Diligent::Box dstBox{0, static_cast<::Diligent::Uint32>(levelSize.x()),
                                               0, static_cast<::Diligent::Uint32>(levelSize.y()),
                                               0, depth};
                        m_pContext->UpdateTexture(resource->Texture, level, slice, dstBox, subResData,
                                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                        mip->MarkStorageDirty(uploadTarget, level, false);
                    }
                }
            }
            resource->ContentVersion = texture.GetContentVersion();
        }

        return resource->SRV;
    }

    ::Diligent::ITextureView* DiligentRenderer::SyncTextureForAttachment(MG_State::GLState::ITextureObject& texture,
                                                                          Bool depth) {
        auto* srvOrView = SyncTexture(texture);
        if (srvOrView == nullptr) {
            return nullptr;
        }
        auto it = m_textureCache.find(texture.GetLifetimeId());
        if (it == m_textureCache.end()) {
            return nullptr;
        }
        TextureResource& resource = it->second;
        if (depth) {
            return resource.DSV ? resource.DSV.RawPtr() : nullptr;
        }
        return resource.RTV ? resource.RTV.RawPtr() : nullptr;
    }

    ::Diligent::ITextureView* DiligentRenderer::SyncRenderbuffer(
        MG_State::GLState::RenderbufferObject& renderbuffer) {
        if (m_pDevice == nullptr || !renderbuffer.IsAllocated()) {
            return nullptr;
        }

        const auto format = ConvertInternalFormatToDiligent(renderbuffer.GetInternalFormat());
        if (format == ::Diligent::TEX_FORMAT_UNKNOWN) {
            return nullptr;
        }
        const Bool depth = IsDepthFormat(renderbuffer.GetInternalFormat());

        ::Diligent::TextureDesc desc;
        desc.Name = "MobileGL state renderbuffer";
        desc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        desc.Format = format;
        desc.Width = static_cast<::Diligent::Uint32>(renderbuffer.GetWidth());
        desc.Height = static_cast<::Diligent::Uint32>(renderbuffer.GetHeight());
        desc.MipLevels = 1;
        desc.Usage = ::Diligent::USAGE_DEFAULT;
        desc.BindFlags = depth ? ::Diligent::BIND_DEPTH_STENCIL : ::Diligent::BIND_RENDER_TARGET;

        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pTexture;
        m_pDevice->CreateTexture(desc, nullptr, &pTexture);
        if (!pTexture) {
            return nullptr;
        }

        const auto viewType = depth ? ::Diligent::TEXTURE_VIEW_DEPTH_STENCIL : ::Diligent::TEXTURE_VIEW_RENDER_TARGET;
        return pTexture->GetDefaultView(viewType);
    }

    Bool DiligentRenderer::ResolveCurrentRenderTargets(Vector<::Diligent::ITextureView*>& rtvs,
                                                       ::Diligent::ITextureView*& dsv) {
        rtvs.clear();
        dsv = nullptr;
        if (!m_initialized) {
            return false;
        }

        if (MG_State::pGLContext == nullptr) {
            if (m_pColorRTV) {
                rtvs.push_back(m_pColorRTV.RawPtr());
            }
            dsv = m_pDepthDSV ? m_pDepthDSV.RawPtr() : nullptr;
            return !rtvs.empty();
        }

        auto drawFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        if (!drawFbo || drawFbo->IsDefaultFramebuffer()) {
            if (m_pColorRTV) {
                rtvs.push_back(m_pColorRTV.RawPtr());
            }
            dsv = m_pDepthDSV ? m_pDepthDSV.RawPtr() : nullptr;
            return !rtvs.empty();
        }

        if (!drawFbo->CheckCompleteness()) {
            return false;
        }

        const auto& drawBuffers = drawFbo->GetDrawBuffers();
        for (const auto attachmentType : drawBuffers) {
            if (attachmentType == FramebufferAttachmentType::None) {
                continue;
            }
            const auto& attachment = drawFbo->GetAttachment(attachmentType);
            if (attachment.IsEmpty()) {
                continue;
            }
            ::Diligent::ITextureView* view = nullptr;
            if (attachment.IsTexture()) {
                view = SyncTextureForAttachment(*attachment.GetTexture(), false);
            } else if (attachment.IsRenderbuffer()) {
                view = SyncRenderbuffer(*attachment.GetRenderbuffer());
            }
            if (view != nullptr) {
                rtvs.push_back(view);
            }
        }

        const auto& depthAttachment = drawFbo->GetAttachment(FramebufferAttachmentType::Depth);
        const auto& stencilAttachment = drawFbo->GetAttachment(FramebufferAttachmentType::Stencil);
        if (depthAttachment.IsTexture()) {
            dsv = SyncTextureForAttachment(*depthAttachment.GetTexture(), true);
        } else if (depthAttachment.IsRenderbuffer()) {
            dsv = SyncRenderbuffer(*depthAttachment.GetRenderbuffer());
        } else if (stencilAttachment.IsTexture()) {
            dsv = SyncTextureForAttachment(*stencilAttachment.GetTexture(), true);
        } else if (stencilAttachment.IsRenderbuffer()) {
            dsv = SyncRenderbuffer(*stencilAttachment.GetRenderbuffer());
        }
        return !rtvs.empty();
    }

    ::Diligent::ISampler* DiligentRenderer::SyncSampler(const MG_State::GLState::SamplerObject& sampler) {
        if (m_pDevice == nullptr) {
            return nullptr;
        }
        const Uint64 lifetimeId = sampler.GetLifetimeId();
        auto it = m_samplerCache.find(lifetimeId);
        if (it != m_samplerCache.end() && it->second.Sampler && it->second.Version == sampler.GetVersion()) {
            return it->second.Sampler;
        }

        const auto& params = sampler.GetAllSamplerParameters();
        ::Diligent::SamplerDesc desc;
        desc.AddressU = ConvertWrapMode(params.wrapS);
        desc.AddressV = ConvertWrapMode(params.wrapT);
        desc.AddressW = ConvertWrapMode(params.wrapR);
        desc.MipLODBias = params.lodBias;
        desc.MaxAnisotropy = static_cast<::Diligent::Uint32>(std::max(0.0f, params.maxAnisotropy));
        desc.MinLOD = params.minLod;
        desc.MaxLOD = params.maxLod;
        desc.ComparisonFunc = ConvertCompareFunc(params.compareFunc);
        desc.BorderColor[0] = params.borderColor.x();
        desc.BorderColor[1] = params.borderColor.y();
        desc.BorderColor[2] = params.borderColor.z();
        desc.BorderColor[3] = params.borderColor.w();
        const Bool compare = params.compareMode == SamplerCompareMode::CompareToTexture;
        desc.MinFilter = ConvertFilterType(params.minFilter, params.mipmapMode, compare, params.maxAnisotropy);
        desc.MagFilter = ConvertFilterType(params.magFilter, params.mipmapMode, compare, params.maxAnisotropy);
        desc.MipFilter = ConvertFilterType(params.minFilter, params.mipmapMode, compare, params.maxAnisotropy);

        ::Diligent::RefCntAutoPtr<::Diligent::ISampler> pSampler;
        m_pDevice->CreateSampler(desc, &pSampler);
        if (!pSampler) {
            return nullptr;
        }

        SamplerResource resource;
        resource.Sampler = pSampler;
        resource.Version = sampler.GetVersion();
        m_samplerCache[lifetimeId] = std::move(resource);
        return pSampler;
    }

    Bool DiligentRenderer::UploadUBOFromState(const MG_State::GLState::ProgramObject& program) {
        if (m_pDevice == nullptr || m_pContext == nullptr) {
            return false;
        }
        const Uint uboSize = program.GetUBOSize();
        if (uboSize == 0) {
            m_pUBO.Release();
            m_uboSize = 0;
            m_uboContentVersion = 0;
            m_uboProgramLifetimeId = 0;
            return true;
        }

        const Bool recreate = !m_pUBO || m_uboProgramLifetimeId != program.GetLifetimeId() ||
                              m_uboSize != uboSize;
        if (recreate) {
            m_pUBO.Release();
            ::Diligent::BufferDesc desc;
            desc.Name = "MobileGL state global UBO";
            desc.BindFlags = ::Diligent::BIND_UNIFORM_BUFFER;
            desc.Size = uboSize;
            desc.Usage = ::Diligent::USAGE_DEFAULT;

            ::Diligent::BufferData initialData;
            initialData.pData = program.GetUBOData();
            initialData.DataSize = uboSize;
            m_pDevice->CreateBuffer(desc, &initialData, &m_pUBO);
            if (!m_pUBO) {
                MGLOG_E("DiligentRenderer::UploadUBOFromState: failed to create UBO buffer");
                return false;
            }
            m_uboSize = uboSize;
            m_uboContentVersion = program.GetUBOContentVersion();
            m_uboProgramLifetimeId = program.GetLifetimeId();
        } else if (m_uboContentVersion != program.GetUBOContentVersion()) {
            m_pContext->UpdateBuffer(m_pUBO, 0, uboSize, program.GetUBOData(),
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            m_uboContentVersion = program.GetUBOContentVersion();
        }
        return m_pUBO != nullptr;
    }

    Bool DiligentRenderer::BindShaderResourcesFromState(const MG_State::GLState::ProgramObject& program) {
        if (m_pStateSRB == nullptr) {
            return false;
        }

        const auto& spirvs = program.GetGeneratedSpirv();
        for (const auto& spv : spirvs) {
            if (spv.empty()) {
                continue;
            }
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(spv.size() * sizeof(unsigned), spv.data(), &module) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                continue;
            }

            ::Diligent::SHADER_TYPE stage = ::Diligent::SHADER_TYPE_UNKNOWN;
            if (!GetSpirvStage(spv, stage)) {
                spvReflectDestroyShaderModule(&module);
                continue;
            }

            Uint32 bindingCount = 0;
            spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
            Vector<SpvReflectDescriptorBinding*> bindings(static_cast<SizeT>(bindingCount));
            if (bindingCount > 0) {
                spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());
            }

            for (Uint32 b = 0; b < bindingCount; ++b) {
                const auto* binding = bindings[b];
                if (binding == nullptr || binding->name == nullptr) {
                    continue;
                }
                if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    const Int location = program.GetUniformLocation(binding->name);
                    if (location < 0) {
                        continue;
                    }
                    const Int unit = program.GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location));
                    if (unit < 0 || MG_State::pGLContext == nullptr) {
                        continue;
                    }
                    auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                    TextureTarget target = TextureTarget::Unknown;
                    switch (binding->image.dim) {
                    case SpvDim1D: target = TextureTarget::Texture1D; break;
                    case SpvDim2D: target = binding->image.arrayed ? TextureTarget::Texture2DArray : TextureTarget::Texture2D; break;
                    case SpvDim3D: target = TextureTarget::Texture3D; break;
                    case SpvDimCube: target = binding->image.arrayed ? TextureTarget::TextureCubeMapArray : TextureTarget::TextureCubeMap; break;
                    default: break;
                    }
                    if (target == TextureTarget::Unknown) {
                        continue;
                    }
                    auto texture = textureUnit.GetBindingSlot(target).GetBoundObject();
                    if (!texture || MG_State::GLState::IsUndefinedDefaultTexture(texture.get())) {
                        // Preserve the legacy test-texture path: when no real front-end
                        // texture is bound, fall back to CreateTestTexture's SRV.
                        if (m_pTestSRV) {
                            if (auto* variable = m_pStateSRB->GetVariableByName(stage, binding->name)) {
                                variable->Set(m_pTestSRV);
                            }
                        }
                        continue;
                    }
                    const MG_State::GLState::SamplerObject* samplerToUse = textureUnit.GetSamplerObject().get();
                    if (samplerToUse == nullptr) {
                        samplerToUse = texture->GetSamplerObject().get();
                    }
                    const Bool incomplete = samplerToUse == nullptr ||
                                            MG_State::GLState::SamplesAsIncompleteTexture(texture.get(), samplerToUse);
                    if (incomplete) {
                        if (m_pTestSRV) {
                            if (auto* variable = m_pStateSRB->GetVariableByName(stage, binding->name)) {
                                variable->Set(m_pTestSRV);
                            }
                        }
                        continue;
                    }
                    auto* srv = SyncTexture(*texture);
                    if (srv == nullptr) {
                        continue;
                    }
                    auto* sampler = SyncSampler(*samplerToUse);
                    if (sampler == nullptr) {
                        continue;
                    }
                    srv->SetSampler(sampler);
                    auto* variable = m_pStateSRB->GetVariableByName(stage, binding->name);
                    if (variable != nullptr) {
                        variable->Set(srv);
                    }
                } else if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    // MobileGL routes all default-block uniforms into one synthesized
                    // MGL_GLOBAL_UBO. SPIRV-Reflect may report an empty block name for the
                    // synthesized block, so always resolve the Diligent variable by the
                    // frontend's fixed global-UBO name.
                    if (!UploadUBOFromState(program) || !m_pUBO) {
                        continue;
                    }
                    auto* variable = m_pStateSRB->GetVariableByName(stage, "MGL_GLOBAL_UBO");
                    if (variable == nullptr && binding->name != nullptr && binding->name[0] != '\0') {
                        variable = m_pStateSRB->GetVariableByName(stage, binding->name);
                    }
                    if (variable != nullptr) {
                        variable->Set(m_pUBO);
                    }
                }
            }

            spvReflectDestroyShaderModule(&module);
        }
        return true;
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

        Vector<::Diligent::ITextureView*> rtvs;
        ::Diligent::ITextureView* dsv = nullptr;
        if (!ResolveCurrentRenderTargets(rtvs, dsv)) {
            return;
        }
        m_pContext->SetRenderTargets(static_cast<::Diligent::Uint32>(rtvs.size()), rtvs.data(), dsv,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        auto glViewport = MG_State::pGLContext->GetViewport();
        if (glViewport.z() <= 0 || glViewport.w() <= 0) {
            glViewport = IntVec4(0, 0, static_cast<Int32>(m_width), static_cast<Int32>(m_height));
        }
        ::Diligent::Viewport viewport{
            static_cast<Float>(glViewport.x()),
            static_cast<Float>(glViewport.y()),
            static_cast<Float>(glViewport.z()),
            static_cast<Float>(glViewport.w()),
            0.0f, 1.0f};
        m_pContext->SetViewports(1, &viewport, m_width, m_height);

        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            const auto scissor = MG_State::pGLContext->GetScissorBox();
            ::Diligent::Rect rect{
                scissor.x(),
                scissor.y(),
                scissor.x() + scissor.z(),
                scissor.y() + scissor.w()};
            m_pContext->SetScissorRects(1, &rect, m_width, m_height);
        } else {
            ::Diligent::Rect rect{0, 0, static_cast<::Diligent::Int32>(m_width),
                                  static_cast<::Diligent::Int32>(m_height)};
            m_pContext->SetScissorRects(1, &rect, m_width, m_height);
        }

        ::Diligent::IBuffer* pVBs[] = {m_pVertexBuffer};
        m_pContext->SetVertexBuffers(0, 1, pVBs, nullptr,
                                     ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                     ::Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
        m_pContext->SetPipelineState(m_pPSO);
        const auto& drawProgram = MG_State::pGLContext->GetProgramForDraw();
        if (drawProgram) {
            BindShaderResourcesFromState(*drawProgram);
        }
        if (MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest)) {
            const auto& front = MG_State::pGLContext->GetStencilState(StencilFace::Front);
            m_pContext->SetStencilRef(static_cast<::Diligent::Uint32>(front.Ref));
        }
        if (m_pStateSRB) {
            m_pContext->CommitShaderResources(m_pStateSRB, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }

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

        // Cheap last-PSO cache: the pipeline depends only on the program, the
        // render-state subset baked into the PSO, the primitive topology and the
        // enabled vertex-attribute layout. Textures/UBOs are bound dynamically, so
        // they do not participate in this key.
        const auto& cachedVao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& cachedAttributes = cachedVao.GetAllAttributes();
        Uint64 psoKey = program->GetLifetimeId();
        psoKey = psoKey * 1099511628211ull + MG_State::pGLContext->GetPipelineStateVersion();
        psoKey = psoKey * 1099511628211ull + static_cast<Uint64>(mode);
        for (Uint32 i = 0; i < cachedVao.MAX_VERTEX_ATTRIBS; ++i) {
            const auto& attr = cachedAttributes[i];
            if (!attr.Enabled) {
                continue;
            }
            psoKey = psoKey * 1099511628211ull + i;
            psoKey = psoKey * 1099511628211ull + static_cast<Uint64>(attr.Size);
            psoKey = psoKey * 1099511628211ull + static_cast<Uint64>(attr.Type);
            psoKey = psoKey * 1099511628211ull + (attr.Normalized ? 1ull : 0ull);
        }
        if (m_hasCachedPSO && m_lastPSOKey == psoKey && m_pPSO && m_pStateSRB) {
            return true;
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
        psoDesc.ResourceLayout.DefaultVariableType = ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        graphicsPipeline.NumRenderTargets = 1;
        graphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        graphicsPipeline.DSVFormat = ::Diligent::TEX_FORMAT_D32_FLOAT;
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

        const Bool stencilEnabled = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::StencilTest);
        if (stencilEnabled) {
            const auto& front = MG_State::pGLContext->GetStencilState(StencilFace::Front);
            const auto& back = MG_State::pGLContext->GetStencilState(StencilFace::Back);
            auto& dsDesc = graphicsPipeline.DepthStencilDesc;
            dsDesc.StencilEnable = true;
            dsDesc.StencilReadMask = static_cast<::Diligent::Uint8>(front.ValueMask & 0xFFu);
            dsDesc.StencilWriteMask = static_cast<::Diligent::Uint8>(front.WriteMask & 0xFFu);
            dsDesc.FrontFace.StencilFailOp = ConvertStencilOp(front.FailOp);
            dsDesc.FrontFace.StencilDepthFailOp = ConvertStencilOp(front.PassDepthFailOp);
            dsDesc.FrontFace.StencilPassOp = ConvertStencilOp(front.PassDepthPassOp);
            dsDesc.FrontFace.StencilFunc = ConvertDepthFunc(front.Func);
            dsDesc.BackFace.StencilFailOp = ConvertStencilOp(back.FailOp);
            dsDesc.BackFace.StencilDepthFailOp = ConvertStencilOp(back.PassDepthFailOp);
            dsDesc.BackFace.StencilPassOp = ConvertStencilOp(back.PassDepthPassOp);
            dsDesc.BackFace.StencilFunc = ConvertDepthFunc(back.Func);
        }

        const auto colorMask = MG_State::pGLContext->GetColorMask();
        auto& rtBlend = graphicsPipeline.BlendDesc.RenderTargets[0];
        rtBlend.RenderTargetWriteMask = ::Diligent::COLOR_MASK_NONE;
        if (colorMask.x()) rtBlend.RenderTargetWriteMask |= ::Diligent::COLOR_MASK_RED;
        if (colorMask.y()) rtBlend.RenderTargetWriteMask |= ::Diligent::COLOR_MASK_GREEN;
        if (colorMask.z()) rtBlend.RenderTargetWriteMask |= ::Diligent::COLOR_MASK_BLUE;
        if (colorMask.w()) rtBlend.RenderTargetWriteMask |= ::Diligent::COLOR_MASK_ALPHA;

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

        m_pStateSRB = nullptr;
        m_pPSO->CreateShaderResourceBinding(&m_pStateSRB, true);
        if (!m_pStateSRB) {
            MGLOG_E("DiligentRenderer: failed to create state SRB");
            return false;
        }
        BindShaderResourcesFromState(*program);
        m_lastPSOKey = psoKey;
        m_hasCachedPSO = true;
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

        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> pSrcTexture = m_pColorTarget;
        if (MG_State::pGLContext != nullptr) {
            auto readFbo = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (readFbo && !readFbo->IsDefaultFramebuffer()) {
                auto readBuffer = readFbo->GetReadBuffer();
                if (readBuffer == FramebufferAttachmentType::None) {
                    readBuffer = FramebufferAttachmentType::Color0;
                }
                const auto& attachment = readFbo->GetAttachment(readBuffer);
                if (attachment.IsTexture()) {
                    if (SyncTexture(*attachment.GetTexture()) != nullptr) {
                        auto it = m_textureCache.find(attachment.GetTexture()->GetLifetimeId());
                        if (it != m_textureCache.end() && it->second.Texture) {
                            pSrcTexture = it->second.Texture;
                        }
                    }
                }
                // Renderbuffer readback is not wired yet; fall back to the default target.
            }
        }

        const auto& srcDesc = pSrcTexture->GetDesc();
        ::Diligent::TextureDesc stagingDesc;
        stagingDesc.Name = "MobileGL Diligent staging readback";
        stagingDesc.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        stagingDesc.Format = ::Diligent::TEX_FORMAT_RGBA8_UNORM;
        stagingDesc.Width = srcDesc.Width;
        stagingDesc.Height = srcDesc.Height;
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
            pSrcTexture, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
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
