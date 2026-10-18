// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripCompatPerVertexMembersPass.h
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
            // Gives a compatibility-profile stage the per-vertex block its core twin has.
            //
            // Under a compatibility-profile #version glslang declares gl_PerVertex (and the gl_in /
            // gl_out arrays of it) with the legacy members gl_ClipVertex, gl_FrontColor,
            // gl_BackColor, gl_FrontSecondaryColor, gl_BackSecondaryColor, gl_TexCoord[] and
            // gl_FogFragCoord between the core ones, and emits the whole block whenever any
            // member is used. Those members have no SPIR-V BuiltIn, and Vulkan requires every
            // member of a built-in block to carry one, so such a module is invalid even when it
            // writes nothing but gl_Position, and DirectVulkan declines invalid modules.
            //
            // A Block struct mixing BuiltIn and plain members loses its plain members when no
            // access chain reaches them; the rest are renumbered. A block whose legacy member is
            // accessed, or that is reached in any way other than constant-indexed access chains
            // (a whole-block load, store or copy, a pointer escaping into a call), is left as is.
            class StripCompatPerVertexMembersPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-strip-compat-per-vertex-members"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateStripCompatPerVertexMembersPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
