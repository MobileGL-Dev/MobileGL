// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUniformLocationsPass.h
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
            // glslang's relaxed GL path keeps `layout(location = N) uniform ...` as a
            // Location decoration on the UniformConstant/Uniform variable, which Vulkan
            // forbids ([VUID-StandaloneSpirv-Location-06672]). Nothing downstream reads
            // it: GL-side uniform locations come from the phase-A glslang reflection,
            // Vulkan binding assignment goes by (kind, name), and SPIRV-Cross's ESSL
            // resolves uniforms by name. Strip it so the driver-bound module is valid.
            class StripUniformLocationsPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-strip-uniform-locations"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateStripUniformLocationsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
