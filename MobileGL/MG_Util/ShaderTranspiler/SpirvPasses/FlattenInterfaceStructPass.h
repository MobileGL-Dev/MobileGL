// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenInterfaceStructPass.h
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
            // This is a custom SPIR-V optimization pass that flattens the "DailyWeatherVariation" interface struct into
            // separate variables. This is actually a workaround for Adreno drivers that fail to work with struct as an
            // interface block. Mali drivers don't have this issue, but we want to keep the shader code consistent
            // across platforms.
            class FlattenInterfaceStructPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "flatten-interface-struct"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFlattenInterfaceStructPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL