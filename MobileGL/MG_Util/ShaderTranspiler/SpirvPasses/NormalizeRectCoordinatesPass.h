// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/NormalizeRectCoordinatesPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // GL_TEXTURE_RECTANGLE has no counterpart in either target API: ESSL has no
            // rectangle sampler at all, and Vulkan's SPIR-V environment does not allow
            // Dim::Rect. Both emulate it on a plain 2D texture, which differs in exactly one
            // way - a rectangle lookup addresses texels while a 2D one addresses [0,1].
            //
            // This pass closes that difference in the module itself, so neither backend has
            // to reason about it: every lookup that takes normalized coordinates gets its
            // coordinate divided by the texture's size, and the image type is then rewritten
            // to 2D. texelFetch is untouched (integer texel coordinates mean the same thing
            // on both), and so is the Dref *sample* form, whose coordinate carries the
            // compare value in the last component - the module keeps its rectangle type and
            // the caller declines it.
            class NormalizeRectCoordinatesPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-normalize-rect-coordinates"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateNormalizeRectCoordinatesPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
