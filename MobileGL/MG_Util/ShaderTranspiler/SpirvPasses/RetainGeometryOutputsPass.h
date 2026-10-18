// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RetainGeometryOutputsPass.h
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
            // Geometry outputs keep their last written value across EmitVertex(), as on every
            // desktop GL driver. GLSL leaves them undefined after an emit, but shaders rely on the
            // desktop behaviour: iterationT's terrain stage writes `flat out float
            // textureResolution` once and then emits three vertices, and the flat value is taken
            // from the LAST one (GL's default provoking vertex), which a driver that drops the
            // value after the first emit renders as random black triangles.
            //
            // Each output whose value from before an EARLIER emit can be observed - at a later emit
            // or by a read - gets a Private shadow; every load, store and access chain of it moves
            // to the shadow, which is copied into the real output immediately before every emit
            // (stream emits included). Block members are carried one by one, and only those the
            // shader writes, so no unwritten builtin member becomes active. A must-be-written
            // dataflow over each function's CFG finds them; a stage that writes every output
            // before every emit is left byte-identical. Outputs whose pointer escapes into a call,
            // and arrays of blocks, are left alone.
            class RetainGeometryOutputsPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "retain-geometry-outputs"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateRetainGeometryOutputsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
