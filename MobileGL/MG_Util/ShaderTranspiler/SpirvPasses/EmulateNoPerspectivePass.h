// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateNoPerspectivePass.h
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
            // Emulates 'noperspective' (screen-linear) interpolation on GLES devices that lack
            // GL_NV_shader_noperspective_interpolation, so no NV extension is required. The hardware
            // interpolates perspective-correct; screen-linear L(a) is recovered from the identity
            //     L(a) = P(a * w) * gl_FragCoord.w
            // where P is perspective-correct interpolation and w is the vertex clip-space w. So each
            // NoPerspective-decorated output is pre-multiplied by gl_Position.w in the vertex stage
            // and each NoPerspective-decorated input is multiplied by gl_FragCoord.w in the fragment
            // stage; the decoration is then removed so the varying transports smooth. This is exact
            // (modulo float precision - the emulated varyings want highp).
            //
            // Scope: plain interface variables of float or floatN type. Anything it cannot emulate
            // (interface-block members, matrices, or a stage lacking the needed builtin) has its
            // NoPerspective decoration stripped instead, degrading to smooth - the same result the
            // extension-less fallback produced before, and never invalid SPIR-V. DirectGLES only.
            class EmulateNoPerspectivePass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "emulate-noperspective"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateEmulateNoPerspectivePass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
