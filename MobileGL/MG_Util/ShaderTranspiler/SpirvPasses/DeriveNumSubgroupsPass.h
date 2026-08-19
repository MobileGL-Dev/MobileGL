// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeriveNumSubgroupsPass.h
// Copyright (c) 2026 MobileGL-Dev
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
            // Replaces compute-stage NumSubgroups builtin loads with
            // ceil(WorkgroupSize.x * WorkgroupSize.y * WorkgroupSize.z / SubgroupSize).
            //
            // That is the value Vulkan defines for NumSubgroups when the pipeline does not
            // enable varying subgroup sizes, which MobileGL never does. Deriving it avoids
            // drivers that expose the real SubgroupId topology but return an inconsistent
            // NumSubgroups value. This is a DirectVulkan semantic repair, not a source-shader
            // rewrite; the application's subgroup arithmetic and shared-memory logic remain
            // unchanged.
            class DeriveNumSubgroupsPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "derive-num-subgroups"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDeriveNumSubgroupsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
