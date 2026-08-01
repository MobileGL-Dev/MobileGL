// MobileGL - MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderStateEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        VkPrimitiveTopology ConvertPrimitiveModeToVkEnum(GLenum mode) {
            switch (mode) {
            case GL_POINTS:
                return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case GL_LINES:
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case GL_LINE_STRIP:
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case GL_TRIANGLES:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case GL_TRIANGLE_STRIP:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case GL_TRIANGLE_FAN:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            case GL_LINE_LOOP:
                // DrawArrays/DrawElements rewrite line loops into closed indexed
                // strips; entry points without that rewrite (instanced/indirect)
                // degrade to an open strip, which only misses the closing segment.
                MGLOG_W("GL_LINE_LOOP without index rewrite; drawing as LINE_STRIP");
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case GL_LINES_ADJACENCY:
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
            case GL_LINE_STRIP_ADJACENCY:
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
            case GL_TRIANGLES_ADJACENCY:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
            case GL_TRIANGLE_STRIP_ADJACENCY:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
            default:
                MGLOG_W("Unrecognized primitive topology");
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        VkPolygonMode ConvertPolygonModeToVkEnum(GLenum mode) {
            switch (mode) {
            case GL_FILL:
                return VK_POLYGON_MODE_FILL;
            case GL_LINE:
                return VK_POLYGON_MODE_LINE;
            case GL_POINT:
                return VK_POLYGON_MODE_POINT;
            default:
                MGLOG_W("Unrecognized polygon mode");
                return VK_POLYGON_MODE_FILL;
            }
        }

        VkCullModeFlags ConvertCullFaceModeToVkEnum(CullFaceMode v, Bool invertClockwise) {
            switch (v) {
            case CullFaceMode::Front:
                return invertClockwise ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
            case CullFaceMode::Back:
                return invertClockwise ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            case CullFaceMode::FrontAndBack:
                return VK_CULL_MODE_FRONT_AND_BACK;
            case CullFaceMode::Unknown:
            case CullFaceMode::CullFaceModeCount:
            default:
                MGLOG_W("Unrecognized cull face mode");
                return VK_CULL_MODE_BACK_BIT;
            }
        }

        VkLogicOp ConvertLogicOperationToVkEnum(LogicOperation v) {
            switch (v) {
            case LogicOperation::Clear:
                return VK_LOGIC_OP_CLEAR;
            case LogicOperation::And:
                return VK_LOGIC_OP_AND;
            case LogicOperation::AndReverse:
                return VK_LOGIC_OP_AND_REVERSE;
            case LogicOperation::Copy:
                return VK_LOGIC_OP_COPY;
            case LogicOperation::AndInverted:
                return VK_LOGIC_OP_AND_INVERTED;
            case LogicOperation::Noop:
                return VK_LOGIC_OP_NO_OP;
            case LogicOperation::Xor:
                return VK_LOGIC_OP_XOR;
            case LogicOperation::Or:
                return VK_LOGIC_OP_OR;
            case LogicOperation::Nor:
                return VK_LOGIC_OP_NOR;
            case LogicOperation::Equiv:
                return VK_LOGIC_OP_EQUIVALENT;
            case LogicOperation::Invert:
                return VK_LOGIC_OP_INVERT;
            case LogicOperation::OrReverse:
                return VK_LOGIC_OP_OR_REVERSE;
            case LogicOperation::CopyInverted:
                return VK_LOGIC_OP_COPY_INVERTED;
            case LogicOperation::OrInverted:
                return VK_LOGIC_OP_OR_INVERTED;
            case LogicOperation::Nand:
                return VK_LOGIC_OP_NAND;
            case LogicOperation::Set:
                return VK_LOGIC_OP_SET;
            default:
                return VK_LOGIC_OP_COPY;
            }
        }

        VkCompareOp ConvertDepthTestFuncToVkEnum(DepthTestFunc v) {
            switch (v) {
            case DepthTestFunc::Never:
                return VK_COMPARE_OP_NEVER;
            case DepthTestFunc::Less:
                return VK_COMPARE_OP_LESS;
            case DepthTestFunc::Equal:
                return VK_COMPARE_OP_EQUAL;
            case DepthTestFunc::LessEqual:
                return VK_COMPARE_OP_LESS_OR_EQUAL;
            case DepthTestFunc::Greater:
                return VK_COMPARE_OP_GREATER;
            case DepthTestFunc::NotEqual:
                return VK_COMPARE_OP_NOT_EQUAL;
            case DepthTestFunc::GreaterEqual:
                return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case DepthTestFunc::Always:
            default:
                return VK_COMPARE_OP_ALWAYS;
            }
        }

        VkStencilOp ConvertStencilOperationToVkEnum(StencilOperation v) {
            switch (v) {
            case StencilOperation::Keep:
                return VK_STENCIL_OP_KEEP;
            case StencilOperation::Zero:
                return VK_STENCIL_OP_ZERO;
            case StencilOperation::Replace:
                return VK_STENCIL_OP_REPLACE;
            case StencilOperation::IncrementClamp:
                return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case StencilOperation::DecrementClamp:
                return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case StencilOperation::Invert:
                return VK_STENCIL_OP_INVERT;
            case StencilOperation::IncrementWrap:
                return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case StencilOperation::DecrementWrap:
                return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default:
                return VK_STENCIL_OP_KEEP;
            }
        }

        VkBlendFactor ConvertBlendFactorToVkEnum(BlendFactor v) {
            switch (v) {
            case BlendFactor::Zero:
                return VK_BLEND_FACTOR_ZERO;
            case BlendFactor::One:
                return VK_BLEND_FACTOR_ONE;
            case BlendFactor::SrcColor:
                return VK_BLEND_FACTOR_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor:
                return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BlendFactor::DstColor:
                return VK_BLEND_FACTOR_DST_COLOR;
            case BlendFactor::OneMinusDstColor:
                return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BlendFactor::SrcAlpha:
                return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha:
                return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha:
                return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendFactor::OneMinusDstAlpha:
                return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case BlendFactor::ConstantColor:
                return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case BlendFactor::OneMinusConstantColor:
                return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case BlendFactor::ConstantAlpha:
                return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case BlendFactor::OneMinusConstantAlpha:
                return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case BlendFactor::Src1Color:
                return VK_BLEND_FACTOR_SRC1_COLOR;
            case BlendFactor::OneMinusSrc1Color:
                return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case BlendFactor::Src1Alpha:
                return VK_BLEND_FACTOR_SRC1_ALPHA;
            case BlendFactor::OneMinusSrc1Alpha:
                return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            default:
                return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp ConvertBlendEquationToVkEnum(BlendEquation v) {
            switch (v) {
            case BlendEquation::Add:
                return VK_BLEND_OP_ADD;
            case BlendEquation::Subtract:
                return VK_BLEND_OP_SUBTRACT;
            case BlendEquation::ReverseSubtract:
                return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BlendEquation::Min:
                return VK_BLEND_OP_MIN;
            case BlendEquation::Max:
                return VK_BLEND_OP_MAX;
            default:
                return VK_BLEND_OP_ADD;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
