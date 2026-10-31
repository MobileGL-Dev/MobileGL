// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FullCoverageSampleMaskPass.h
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
            // GL applies a fragment shader's gl_SampleMask only among the multisample fragment
            // operations, which make no change "if MULTISAMPLE is disabled, or if the value of
            // SAMPLE_BUFFERS is not one" (GL 4.6 core 17.3.3). Vulkan ANDs the SampleMask output
            // with coverage at every rasterizationSamples, so at one sample a mask with bit 0
            // clear discards the fragment (KHR-GL4x.sample_variables.mask.*.samples_0 drew nothing
            // into a single-sample texture). This pass builds the fragment module such a draw
            // uses: every store to the SampleMask output stores all-ones, which leaves coverage
            // as the rasterizer produced it.
            //
            // Kept as a second module beside the original, like ZeroLayerOutputPass, so the
            // multisample draws of the same program keep the shader's mask.
            //
            // Handles whole-array stores and stores through an access chain into the array. Any
            // other use of the builtin (a read-back of the output, a chain used for anything but
            // a store) fails the pass, and the caller then keeps the original module.
            class FullCoverageSampleMaskPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "full-coverage-sample-mask"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFullCoverageSampleMaskPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
