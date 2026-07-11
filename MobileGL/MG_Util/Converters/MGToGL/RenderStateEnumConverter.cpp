// MobileGL - MobileGL/MG_Util/Converters/MGToGL/RenderStateEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderStateEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {

        GLenum ConvertBlendFactorToGLEnum(BlendFactor v) {
            switch (v) {
            case BlendFactor::Zero:
                return GL_ZERO;
            case BlendFactor::One:
                return GL_ONE;
            case BlendFactor::SrcColor:
                return GL_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor:
                return GL_ONE_MINUS_SRC_COLOR;
            case BlendFactor::DstColor:
                return GL_DST_COLOR;
            case BlendFactor::OneMinusDstColor:
                return GL_ONE_MINUS_DST_COLOR;
            case BlendFactor::SrcAlpha:
                return GL_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha:
                return GL_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha:
                return GL_DST_ALPHA;
            case BlendFactor::OneMinusDstAlpha:
                return GL_ONE_MINUS_DST_ALPHA;
            case BlendFactor::ConstantColor:
                return GL_CONSTANT_COLOR;
            case BlendFactor::OneMinusConstantColor:
                return GL_ONE_MINUS_CONSTANT_COLOR;
            case BlendFactor::ConstantAlpha:
                return GL_CONSTANT_ALPHA;
            case BlendFactor::OneMinusConstantAlpha:
                return GL_ONE_MINUS_CONSTANT_ALPHA;
            case BlendFactor::Src1Color:
                return GL_SRC1_COLOR;
            case BlendFactor::OneMinusSrc1Color:
                return GL_ONE_MINUS_SRC1_COLOR;
            case BlendFactor::Src1Alpha:
                return GL_SRC1_ALPHA;
            case BlendFactor::OneMinusSrc1Alpha:
                return GL_ONE_MINUS_SRC1_ALPHA;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertBlendEquationToGLEnum(BlendEquation v) {
            switch (v) {
            case BlendEquation::Add:
                return GL_FUNC_ADD;
            case BlendEquation::Subtract:
                return GL_FUNC_SUBTRACT;
            case BlendEquation::ReverseSubtract:
                return GL_FUNC_REVERSE_SUBTRACT;
            case BlendEquation::Min:
                return GL_MIN;
            case BlendEquation::Max:
                return GL_MAX;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertLogicOperationToGLEnum(LogicOperation v) {
            switch (v) {
            case LogicOperation::Clear:
                return GL_CLEAR;
            case LogicOperation::And:
                return GL_AND;
            case LogicOperation::AndReverse:
                return GL_AND_REVERSE;
            case LogicOperation::Copy:
                return GL_COPY;
            case LogicOperation::AndInverted:
                return GL_AND_INVERTED;
            case LogicOperation::Noop:
                return GL_NOOP;
            case LogicOperation::Xor:
                return GL_XOR;
            case LogicOperation::Or:
                return GL_OR;
            case LogicOperation::Nor:
                return GL_NOR;
            case LogicOperation::Equiv:
                return GL_EQUIV;
            case LogicOperation::Invert:
                return GL_INVERT;
            case LogicOperation::OrReverse:
                return GL_OR_REVERSE;
            case LogicOperation::CopyInverted:
                return GL_COPY_INVERTED;
            case LogicOperation::OrInverted:
                return GL_OR_INVERTED;
            case LogicOperation::Nand:
                return GL_NAND;
            case LogicOperation::Set:
                return GL_SET;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertDepthTestFuncToGLEnum(DepthTestFunc v) {
            switch (v) {
            case DepthTestFunc::Never:
                return GL_NEVER;
            case DepthTestFunc::Less:
                return GL_LESS;
            case DepthTestFunc::Equal:
                return GL_EQUAL;
            case DepthTestFunc::LessEqual:
                return GL_LEQUAL;
            case DepthTestFunc::Greater:
                return GL_GREATER;
            case DepthTestFunc::NotEqual:
                return GL_NOTEQUAL;
            case DepthTestFunc::GreaterEqual:
                return GL_GEQUAL;
            case DepthTestFunc::Always:
                return GL_ALWAYS;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertStencilOperationToGLEnum(StencilOperation v) {
            switch (v) {
            case StencilOperation::Keep:
                return GL_KEEP;
            case StencilOperation::Zero:
                return GL_ZERO;
            case StencilOperation::Replace:
                return GL_REPLACE;
            case StencilOperation::IncrementClamp:
                return GL_INCR;
            case StencilOperation::DecrementClamp:
                return GL_DECR;
            case StencilOperation::Invert:
                return GL_INVERT;
            case StencilOperation::IncrementWrap:
                return GL_INCR_WRAP;
            case StencilOperation::DecrementWrap:
                return GL_DECR_WRAP;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertPixelStoreParamToGLEnum(PixelStoreParam v) {
            switch (v) {
            case PixelStoreParam::PackAlignment:
                return GL_PACK_ALIGNMENT;
            case PixelStoreParam::PackRowLength:
                return GL_PACK_ROW_LENGTH;
            case PixelStoreParam::PackImageHeight:
                return GL_PACK_IMAGE_HEIGHT;
            case PixelStoreParam::PackSkipRows:
                return GL_PACK_SKIP_ROWS;
            case PixelStoreParam::PackSkipPixels:
                return GL_PACK_SKIP_PIXELS;
            case PixelStoreParam::PackSkipImages:
                return GL_PACK_SKIP_IMAGES;
            case PixelStoreParam::PackSwapBytes:
                return GL_PACK_SWAP_BYTES;
            case PixelStoreParam::PackLSBFirst:
                return GL_PACK_LSB_FIRST;
            case PixelStoreParam::UnpackAlignment:
                return GL_UNPACK_ALIGNMENT;
            case PixelStoreParam::UnpackRowLength:
                return GL_UNPACK_ROW_LENGTH;
            case PixelStoreParam::UnpackImageHeight:
                return GL_UNPACK_IMAGE_HEIGHT;
            case PixelStoreParam::UnpackSkipRows:
                return GL_UNPACK_SKIP_ROWS;
            case PixelStoreParam::UnpackSkipPixels:
                return GL_UNPACK_SKIP_PIXELS;
            case PixelStoreParam::UnpackSkipImages:
                return GL_UNPACK_SKIP_IMAGES;
            case PixelStoreParam::UnpackSwapBytes:
                return GL_UNPACK_SWAP_BYTES;
            case PixelStoreParam::UnpackLSBFirst:
                return GL_UNPACK_LSB_FIRST;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertCullFaceModeToGLEnum(CullFaceMode v) {
            switch (v) {
            case CullFaceMode::Front:
                return GL_FRONT;
            case CullFaceMode::Back:
                return GL_BACK;
            case CullFaceMode::FrontAndBack:
                return GL_FRONT_AND_BACK;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertFrontFaceModeToGLEnum(FrontFaceMode v) {
            switch (v) {
            case FrontFaceMode::CounterClockwise:
                return GL_CCW;
            case FrontFaceMode::Clockwise:
                return GL_CW;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertProvokingVertexModeToGLEnum(ProvokingVertexMode v) {
            switch (v) {
            case ProvokingVertexMode::FirstVertex:
                return GL_FIRST_VERTEX_CONVENTION;
            case ProvokingVertexMode::LastVertex:
                return GL_LAST_VERTEX_CONVENTION;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertCapabilityInputToGLEnum(CapabilityInput v) {
            switch (v) {
            case CapabilityInput::Blend:
                return GL_BLEND;
            case CapabilityInput::ClipDistance0:
                return GL_CLIP_DISTANCE0;
            case CapabilityInput::ClipDistance1:
                return GL_CLIP_DISTANCE1;
            case CapabilityInput::ClipDistance2:
                return GL_CLIP_DISTANCE2;
            case CapabilityInput::ClipDistance3:
                return GL_CLIP_DISTANCE3;
            case CapabilityInput::ClipDistance4:
                return GL_CLIP_DISTANCE4;
            case CapabilityInput::ClipDistance5:
                return GL_CLIP_DISTANCE5;
            case CapabilityInput::ClipDistance6:
                return GL_CLIP_DISTANCE6;
            case CapabilityInput::ClipDistance7:
                return GL_CLIP_DISTANCE7;
            case CapabilityInput::ColorLogicOp:
                return GL_COLOR_LOGIC_OP;
            case CapabilityInput::CullFace:
                return GL_CULL_FACE;
            case CapabilityInput::DebugOutput:
                return GL_DEBUG_OUTPUT;
            case CapabilityInput::DebugOutputSynchronous:
                return GL_DEBUG_OUTPUT_SYNCHRONOUS;
            case CapabilityInput::DepthClamp:
                return GL_DEPTH_CLAMP;
            case CapabilityInput::DepthTest:
                return GL_DEPTH_TEST;
            case CapabilityInput::Dither:
                return GL_DITHER;
            case CapabilityInput::FramebufferSrgb:
                return GL_FRAMEBUFFER_SRGB;
            case CapabilityInput::LineSmooth:
                return GL_LINE_SMOOTH;
            case CapabilityInput::Multisample:
                return GL_MULTISAMPLE;
            case CapabilityInput::PolygonOffsetFill:
                return GL_POLYGON_OFFSET_FILL;
            case CapabilityInput::PolygonOffsetLine:
                return GL_POLYGON_OFFSET_LINE;
            case CapabilityInput::PolygonOffsetPoint:
                return GL_POLYGON_OFFSET_POINT;
            case CapabilityInput::PolygonSmooth:
                return GL_POLYGON_SMOOTH;
            case CapabilityInput::PrimitiveRestart:
                return GL_PRIMITIVE_RESTART;
            case CapabilityInput::PrimitiveRestartFixedIndex:
                return GL_PRIMITIVE_RESTART_FIXED_INDEX;
            case CapabilityInput::RasterizerDiscard:
                return GL_RASTERIZER_DISCARD;
            case CapabilityInput::SampleAlphaToCoverage:
                return GL_SAMPLE_ALPHA_TO_COVERAGE;
            case CapabilityInput::SampleAlphaToOne:
                return GL_SAMPLE_ALPHA_TO_ONE;
            case CapabilityInput::SampleCoverage:
                return GL_SAMPLE_COVERAGE;
            case CapabilityInput::SampleShading:
                return GL_SAMPLE_SHADING;
            case CapabilityInput::SampleMask:
                return GL_SAMPLE_MASK;
            case CapabilityInput::ScissorTest:
                return GL_SCISSOR_TEST;
            case CapabilityInput::StencilTest:
                return GL_STENCIL_TEST;
            case CapabilityInput::TextureCubeMapSeamless:
                return GL_TEXTURE_CUBE_MAP_SEAMLESS;
            case CapabilityInput::ProgramPointSize:
                return GL_PROGRAM_POINT_SIZE;
            default:
                return GL_UNKNOWN_MGL;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
