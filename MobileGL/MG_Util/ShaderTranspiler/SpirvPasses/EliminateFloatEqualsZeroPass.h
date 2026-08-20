// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EliminateFloatEqualsZeroPass.h
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
            // Keeps the driver's float-EQUALITY instruction out of the module: every scalar
            // comparison against a constant 0.0 is re-spelled through GLSL.std.450 FAbs, so no
            // OpFOrdEqual / OpFUnordEqual / OpFOrdNotEqual / OpFUnordNotEqual against zero ever
            // reaches a shader compiler that gets exact float compare wrong.
            //
            // The rewrite is EXACT, not a tolerance. `x == 0.0` becomes `abs(x) <= 0.0` and
            // `x != 0.0` becomes `abs(x) > 0.0`, both against the module's own zero constant:
            // |x| <= 0 holds for +0 and -0 and for nothing else, so the two forms agree on every
            // input, at any float width, with or without denormal flushing. The ordered/unordered
            // half of the opcode is carried across unchanged, which is what keeps NaN answering
            // the way it did before.
            //
            // It used to be an epsilon ball (abs(x) < 1e-4). That silently classified any
            // legitimately small value as zero - KHR-GL3x.buffer_objects.triangles renders a
            // specular term of ~6e-5 at a large render target and came out black - so the fuzz is
            // gone; the reason the pass exists never needed it.
            class EliminateFloatEqualsZeroPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "float-equals-zero-elimination"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateEliminateFloatEqualsZeroPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
