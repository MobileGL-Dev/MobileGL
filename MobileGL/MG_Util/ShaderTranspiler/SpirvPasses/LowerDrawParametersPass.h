// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerDrawParametersPass.h
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
            // ESSL has no gl_DrawID / gl_BaseInstance / gl_BaseVertex builtins and SPIRV-Cross
            // refuses to emit them for ES targets. This pass demotes the DrawIndex /
            // BaseInstance / BaseVertex builtin inputs to plain Private globals with
            // well-known names (mg_DrawID / mg_BaseInstance / mg_BaseVertex) so the decompiled
            // ESSL declares ordinary globals; the DirectGLES program manager then upgrades the
            // declarations to uniforms and feeds them per (sub-)draw. Only meant for the
            // DirectGLES transpile path - the Vulkan backend keeps the native builtins.
            class LowerDrawParametersPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "lower-draw-parameters"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateLowerDrawParametersPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
