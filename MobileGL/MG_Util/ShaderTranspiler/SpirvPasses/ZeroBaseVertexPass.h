// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ZeroBaseVertexPass.h
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
            // GL and Vulkan disagree about gl_BaseVertex on NON-INDEXED draws: GL defines it as
            // "the value passed to the baseVertex parameter, or zero for a command that has
            // none", so every DrawArrays form reads zero, while Vulkan's BaseVertex builtin
            // carries the draw's firstVertex there. (For indexed draws both mean the same thing,
            // GL's basevertex / Vulkan's vertexOffset, so those must keep the native builtin.)
            //
            // This pass produces the non-indexed variant of a vertex shader by replacing every
            // read of the BaseVertex builtin with a constant zero. The variable itself is left
            // declared - removing it would also have to reason about the DrawParameters
            // capability that a BaseInstance read in the same module still needs.
            //
            // Vulkan backend only, and only for the ZeroBaseVertex program variant: the
            // DirectGLES path has no BaseVertex builtin at all (see LowerDrawParametersPass).
            class ZeroBaseVertexPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "zero-base-vertex"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateZeroBaseVertexPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
