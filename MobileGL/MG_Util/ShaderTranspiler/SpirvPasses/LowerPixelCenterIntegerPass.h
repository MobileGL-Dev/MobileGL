// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerPixelCenterIntegerPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Lowers `layout(pixel_center_integer) in vec4 gl_FragCoord;` for the Vulkan
            // environment, which forbids the PixelCenterInteger execution mode
            // (VUID-StandaloneSpirv-PixelCenterInteger-04654) - a module carrying it fails
            // validation and DirectVulkan refuses every draw of the program. The mode only moves
            // the pixel centre from (n + 0.5) to n, so the pass drops it and subtracts 0.5 from
            // x and y of every gl_FragCoord read: whole-vector loads get vec4(0.5, 0.5, 0, 0),
            // component loads through a constant-index access chain get 0.5 for x and y.
            //
            // Any other use of the builtin (a non-constant component index, the pointer handed
            // to a function) fails the pass rather than half-rewriting it; the caller then keeps
            // the original module, which is no worse than before.
            class LowerPixelCenterIntegerPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "lower-pixel-center-integer"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateLowerPixelCenterIntegerPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
