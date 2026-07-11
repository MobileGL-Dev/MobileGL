// MobileGL - MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/RenderState/RenderState.h>

namespace MobileGL {
    namespace MG_Util {
        VkPrimitiveTopology ConvertPrimitiveModeToVkEnum(GLenum mode);
        // Maps GL_FILL/GL_LINE/GL_POINT to the matching VkPolygonMode. LINE/POINT require the
        // fillModeNonSolid device feature; the caller is responsible for falling back to FILL when
        // that feature is unavailable.
        VkPolygonMode ConvertPolygonModeToVkEnum(GLenum mode);
        VkCullModeFlags ConvertCullFaceModeToVkEnum(CullFaceMode value, Bool invertClockwise = false);
        VkLogicOp ConvertLogicOperationToVkEnum(LogicOperation value);
        VkCompareOp ConvertDepthTestFuncToVkEnum(DepthTestFunc value);
        VkStencilOp ConvertStencilOperationToVkEnum(StencilOperation value);
        VkBlendFactor ConvertBlendFactorToVkEnum(BlendFactor value);
        VkBlendOp ConvertBlendEquationToVkEnum(BlendEquation value);
    } // namespace MG_Util
} // namespace MobileGL
