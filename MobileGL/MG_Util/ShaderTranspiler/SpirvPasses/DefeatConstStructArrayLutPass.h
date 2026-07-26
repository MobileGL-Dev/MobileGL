// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DefeatConstStructArrayLutPass.h
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
            // SPIRV-Cross hoists a Function-storage array variable whose only write is a single
            // constant-composite store into a global `const struct[]` LUT (variable_is_lut).
            // Adreno's ESSL compiler cannot dynamically index such a global const struct array
            // ("Cannot offset into the structure" - device-verified on Adreno 750). Splitting
            // the one composite store into per-element constant-index stores makes
            // variable_is_lut fail, so SPIRV-Cross keeps the array as an ordinary local that
            // Adreno indexes fine. Scalar/vector const arrays are unaffected on Adreno and are
            // left alone - only arrays OF STRUCTS are rewritten. Only meant for the DirectGLES
            // transpile path on Qualcomm devices.
            class DefeatConstStructArrayLutPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "defeat-const-struct-array-lut"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDefeatConstStructArrayLutPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
