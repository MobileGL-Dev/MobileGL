// MobileGL - MobileGL/MG_Util/Converters/GLToMG/RenderStateEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderStateEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        BlendFactor ConvertGLEnumToBlendFactor(GLenum v) {
            switch (v) {
            case GL_ZERO:
                return BlendFactor::Zero;
            case GL_ONE:
                return BlendFactor::One;
            case GL_SRC_COLOR:
                return BlendFactor::SrcColor;
            case GL_ONE_MINUS_SRC_COLOR:
                return BlendFactor::OneMinusSrcColor;
            case GL_DST_COLOR:
                return BlendFactor::DstColor;
            case GL_ONE_MINUS_DST_COLOR:
                return BlendFactor::OneMinusDstColor;
            case GL_SRC_ALPHA:
                return BlendFactor::SrcAlpha;
            case GL_ONE_MINUS_SRC_ALPHA:
                return BlendFactor::OneMinusSrcAlpha;
            case GL_DST_ALPHA:
                return BlendFactor::DstAlpha;
            case GL_ONE_MINUS_DST_ALPHA:
                return BlendFactor::OneMinusDstAlpha;
            case GL_CONSTANT_COLOR:
                return BlendFactor::ConstantColor;
            case GL_ONE_MINUS_CONSTANT_COLOR:
                return BlendFactor::OneMinusConstantColor;
            case GL_CONSTANT_ALPHA:
                return BlendFactor::ConstantAlpha;
            case GL_ONE_MINUS_CONSTANT_ALPHA:
                return BlendFactor::OneMinusConstantAlpha;
            case GL_SRC1_COLOR:
                return BlendFactor::Src1Color;
            case GL_ONE_MINUS_SRC1_COLOR:
                return BlendFactor::OneMinusSrc1Color;
            case GL_SRC1_ALPHA:
                return BlendFactor::Src1Alpha;
            case GL_ONE_MINUS_SRC1_ALPHA:
                return BlendFactor::OneMinusSrc1Alpha;
            default:
                return BlendFactor::Unknown;
            }
        }

        BlendEquation ConvertGLEnumToBlendEquation(GLenum v) {
            switch (v) {
            case GL_FUNC_ADD:
                return BlendEquation::Add;
            case GL_FUNC_SUBTRACT:
                return BlendEquation::Subtract;
            case GL_FUNC_REVERSE_SUBTRACT:
                return BlendEquation::ReverseSubtract;
            case GL_MIN:
                return BlendEquation::Min;
            case GL_MAX:
                return BlendEquation::Max;
            default:
                return BlendEquation::Unknown;
            }
        }

        LogicOperation ConvertGLEnumToLogicOperation(GLenum v) {
            switch (v) {
            case GL_CLEAR:
                return LogicOperation::Clear;
            case GL_AND:
                return LogicOperation::And;
            case GL_AND_REVERSE:
                return LogicOperation::AndReverse;
            case GL_COPY:
                return LogicOperation::Copy;
            case GL_AND_INVERTED:
                return LogicOperation::AndInverted;
            case GL_NOOP:
                return LogicOperation::Noop;
            case GL_XOR:
                return LogicOperation::Xor;
            case GL_OR:
                return LogicOperation::Or;
            case GL_NOR:
                return LogicOperation::Nor;
            case GL_EQUIV:
                return LogicOperation::Equiv;
            case GL_INVERT:
                return LogicOperation::Invert;
            case GL_OR_REVERSE:
                return LogicOperation::OrReverse;
            case GL_COPY_INVERTED:
                return LogicOperation::CopyInverted;
            case GL_OR_INVERTED:
                return LogicOperation::OrInverted;
            case GL_NAND:
                return LogicOperation::Nand;
            case GL_SET:
                return LogicOperation::Set;
            default:
                return LogicOperation::Unknown;
            }
        }

        DepthTestFunc ConvertGLEnumToDepthTestFunc(GLenum v) {
            switch (v) {
            case GL_NEVER:
                return DepthTestFunc::Never;
            case GL_LESS:
                return DepthTestFunc::Less;
            case GL_EQUAL:
                return DepthTestFunc::Equal;
            case GL_LEQUAL:
                return DepthTestFunc::LessEqual;
            case GL_GREATER:
                return DepthTestFunc::Greater;
            case GL_NOTEQUAL:
                return DepthTestFunc::NotEqual;
            case GL_GEQUAL:
                return DepthTestFunc::GreaterEqual;
            case GL_ALWAYS:
                return DepthTestFunc::Always;
            default:
                return DepthTestFunc::Unknown;
            }
        }

        StencilOperation ConvertGLEnumToStencilOperation(GLenum v) {
            switch (v) {
            case GL_KEEP:
                return StencilOperation::Keep;
            case GL_ZERO:
                return StencilOperation::Zero;
            case GL_REPLACE:
                return StencilOperation::Replace;
            case GL_INCR:
                return StencilOperation::IncrementClamp;
            case GL_DECR:
                return StencilOperation::DecrementClamp;
            case GL_INVERT:
                return StencilOperation::Invert;
            case GL_INCR_WRAP:
                return StencilOperation::IncrementWrap;
            case GL_DECR_WRAP:
                return StencilOperation::DecrementWrap;
            default:
                return StencilOperation::Unknown;
            }
        }

        PixelStoreParam ConvertGLEnumToPixelStoreParam(GLenum v) {
            switch (v) {
            case GL_PACK_ALIGNMENT:
                return PixelStoreParam::PackAlignment;
            case GL_PACK_ROW_LENGTH:
                return PixelStoreParam::PackRowLength;
            case GL_PACK_IMAGE_HEIGHT:
                return PixelStoreParam::PackImageHeight;
            case GL_PACK_SKIP_ROWS:
                return PixelStoreParam::PackSkipRows;
            case GL_PACK_SKIP_PIXELS:
                return PixelStoreParam::PackSkipPixels;
            case GL_PACK_SKIP_IMAGES:
                return PixelStoreParam::PackSkipImages;
            case GL_PACK_SWAP_BYTES:
                return PixelStoreParam::PackSwapBytes;
            case GL_PACK_LSB_FIRST:
                return PixelStoreParam::PackLSBFirst;
            case GL_UNPACK_ALIGNMENT:
                return PixelStoreParam::UnpackAlignment;
            case GL_UNPACK_ROW_LENGTH:
                return PixelStoreParam::UnpackRowLength;
            case GL_UNPACK_IMAGE_HEIGHT:
                return PixelStoreParam::UnpackImageHeight;
            case GL_UNPACK_SKIP_ROWS:
                return PixelStoreParam::UnpackSkipRows;
            case GL_UNPACK_SKIP_PIXELS:
                return PixelStoreParam::UnpackSkipPixels;
            case GL_UNPACK_SKIP_IMAGES:
                return PixelStoreParam::UnpackSkipImages;
            case GL_UNPACK_SWAP_BYTES:
                return PixelStoreParam::UnpackSwapBytes;
            case GL_UNPACK_LSB_FIRST:
                return PixelStoreParam::UnpackLSBFirst;
            default:
                return PixelStoreParam::Unknown;
            }
        }

        CullFaceMode ConvertGLEnumToCullFaceMode(GLenum v) {
            switch (v) {
            case GL_FRONT:
                return CullFaceMode::Front;
            case GL_BACK:
                return CullFaceMode::Back;
            case GL_FRONT_AND_BACK:
                return CullFaceMode::FrontAndBack;
            default:
                return CullFaceMode::Unknown;
            }
        }

        FrontFaceMode ConvertGLEnumToFrontFaceMode(GLenum v) {
            switch (v) {
            case GL_CCW:
                return FrontFaceMode::CounterClockwise;
            case GL_CW:
                return FrontFaceMode::Clockwise;
            default:
                return FrontFaceMode::Unknown;
            }
        }

        ProvokingVertexMode ConvertGLEnumToProvokingVertexMode(GLenum v) {
            switch (v) {
            case GL_FIRST_VERTEX_CONVENTION:
                return ProvokingVertexMode::FirstVertex;
            case GL_LAST_VERTEX_CONVENTION:
                return ProvokingVertexMode::LastVertex;
            default:
                return ProvokingVertexMode::Unknown;
            }
        }

        CapabilityInput ConvertGLEnumToCapabilityInput(GLenum v) {
            switch (v) {
            case GL_BLEND:
                return CapabilityInput::Blend;
            case GL_CLIP_DISTANCE0:
                return CapabilityInput::ClipDistance0;
            case GL_CLIP_DISTANCE1:
                return CapabilityInput::ClipDistance1;
            case GL_CLIP_DISTANCE2:
                return CapabilityInput::ClipDistance2;
            case GL_CLIP_DISTANCE3:
                return CapabilityInput::ClipDistance3;
            case GL_CLIP_DISTANCE4:
                return CapabilityInput::ClipDistance4;
            case GL_CLIP_DISTANCE5:
                return CapabilityInput::ClipDistance5;
            case GL_CLIP_DISTANCE6:
                return CapabilityInput::ClipDistance6;
            case GL_CLIP_DISTANCE7:
                return CapabilityInput::ClipDistance7;
            case GL_COLOR_LOGIC_OP:
                return CapabilityInput::ColorLogicOp;
            case GL_CULL_FACE:
                return CapabilityInput::CullFace;
            case GL_DEBUG_OUTPUT:
                return CapabilityInput::DebugOutput;
            case GL_DEBUG_OUTPUT_SYNCHRONOUS:
                return CapabilityInput::DebugOutputSynchronous;
            case GL_DEPTH_CLAMP:
                return CapabilityInput::DepthClamp;
            case GL_DEPTH_TEST:
                return CapabilityInput::DepthTest;
            case GL_DITHER:
                return CapabilityInput::Dither;
            case GL_FRAMEBUFFER_SRGB:
                return CapabilityInput::FramebufferSrgb;
            case GL_LINE_SMOOTH:
                return CapabilityInput::LineSmooth;
            case GL_MULTISAMPLE:
                return CapabilityInput::Multisample;
            case GL_POLYGON_OFFSET_FILL:
                return CapabilityInput::PolygonOffsetFill;
            case GL_POLYGON_OFFSET_LINE:
                return CapabilityInput::PolygonOffsetLine;
            case GL_POLYGON_OFFSET_POINT:
                return CapabilityInput::PolygonOffsetPoint;
            case GL_POLYGON_SMOOTH:
                return CapabilityInput::PolygonSmooth;
            case GL_PRIMITIVE_RESTART:
                return CapabilityInput::PrimitiveRestart;
            case GL_PRIMITIVE_RESTART_FIXED_INDEX:
                return CapabilityInput::PrimitiveRestartFixedIndex;
            case GL_RASTERIZER_DISCARD:
                return CapabilityInput::RasterizerDiscard;
            case GL_SAMPLE_ALPHA_TO_COVERAGE:
                return CapabilityInput::SampleAlphaToCoverage;
            case GL_SAMPLE_ALPHA_TO_ONE:
                return CapabilityInput::SampleAlphaToOne;
            case GL_SAMPLE_COVERAGE:
                return CapabilityInput::SampleCoverage;
            case GL_SAMPLE_SHADING:
                return CapabilityInput::SampleShading;
            case GL_SAMPLE_MASK:
                return CapabilityInput::SampleMask;
            case GL_SCISSOR_TEST:
                return CapabilityInput::ScissorTest;
            case GL_STENCIL_TEST:
                return CapabilityInput::StencilTest;
            case GL_TEXTURE_CUBE_MAP_SEAMLESS:
                return CapabilityInput::TextureCubeMapSeamless;
            case GL_PROGRAM_POINT_SIZE:
                return CapabilityInput::ProgramPointSize;
            default:
                return CapabilityInput::Unknown;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
