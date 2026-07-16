// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUboMemberRelaxedPrecisionPass.h
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
            // Removes RelaxedPrecision member decorations from every struct type reachable
            // from a uniform-block variable (the block struct itself and any structs nested
            // in it through members or arrays).
            //
            // Rationale: ESSL requires matched uniform blocks to declare members with
            // identical precision in every stage, but SPIRV-Cross prints a member's
            // qualifier relative to the stage's DEFAULT precision (highp in the vertex
            // stage, mediump in the fragment stage). A RelaxedPrecision member therefore
            // comes out as an explicit "mediump" in the vertex shader but UNQUALIFIED in
            // the fragment shader - and once ForceSupporterOutput swaps the fragment
            // header to "precision highp float;", that unqualified member reads back as
            // highp and the ES driver refuses to link ("definitions of uniform block ...
            // do not match", GL CTS KHR-GL33.shaders.uniform_block struct sub-groups).
            // Dropping the hint promotes the member to highp in BOTH stages, which is
            // always conformant and matches the std140 data layout either way. Only meant
            // for the DirectGLES transpile path - block member precision is a per-member
            // hint with no layout effect, and no other emission behavior changes.
            class StripUboMemberRelaxedPrecisionPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "strip-ubo-member-relaxed-precision"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateStripUboMemberRelaxedPrecisionPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
