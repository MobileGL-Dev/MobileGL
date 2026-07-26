// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FoldConstOffsetFor1DFetchPass.h
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
            // SPIRV-Cross emulates 1D textures as 2D for ES targets: it widens the texelFetch
            // coordinate to ivec2 but keeps the ConstOffset image operand scalar, and ESSL has
            // no texelFetchOffset(sampler2D, ivec2, int, scalar-offset) overload, so drivers
            // (Adreno) reject the transpiled shader. This pass folds the constant offset into
            // the integer coordinate before the fetch - texelFetchOffset(t, P, l, o) ==
            // texelFetch(t, P + o, l) per the GLSL spec - and drops the ConstOffset operand,
            // so SPIRV-Cross emits a plain texelFetch. For arrayed 1D fetches only coordinate
            // component 0 is offset (component 1 is the layer). Only meant for the DirectGLES
            // transpile path.
            class FoldConstOffsetFor1DFetchPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "fold-const-offset-for-1d-fetch"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFoldConstOffsetFor1DFetchPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
