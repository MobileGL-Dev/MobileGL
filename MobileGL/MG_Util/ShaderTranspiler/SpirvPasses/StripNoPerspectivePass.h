// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripNoPerspectivePass.h
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
            // Removes the NoPerspective decoration from every interface variable and block member.
            // DirectGLES fallback only, for devices that lack GL_NV_shader_noperspective_interpolation:
            // SPIRV-Cross renders a NoPerspective-decorated varying as ESSL `noperspective` plus
            // `#extension GL_NV_shader_noperspective_interpolation : require`, which such a driver
            // rejects. Dropping the decoration falls the varying back to smooth (perspective-correct)
            // interpolation - the same visible result the old text-level strip produced, but without
            // corrupting identifiers and without touching DirectVulkan, where NoPerspective is native.
            // (The exact screen-linear emulation via gl_Position.w / gl_FragCoord.w is a later step.)
            class StripNoPerspectivePass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "strip-noperspective"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateStripNoPerspectivePass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
