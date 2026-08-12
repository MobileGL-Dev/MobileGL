// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/SplitArrayVertexInputsPass.h
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
            // Replaces an ARRAY vertex input with one input variable per element, at
            // consecutive locations, and demotes the original array to a Private global
            // seeded once at the top of the entry point.
            //
            // GLSL ES has no array vertex inputs at all (GLSL ES 3.20 4.3.4: a vertex shader
            // input "cannot be ... arrays"), and SPIRV-Cross does not emulate the difference:
            // it refuses the whole module with "OpenGL ES doesn't support array input
            // variables in vertex shader". The stage then never reaches the driver, the
            // program links without a vertex shader, and every draw using it is a silent
            // no-op - which is how the entire KHR-GL43.vertex_attrib_binding.basic-input*
            // family (its capture program declares `in vec4 vs_in_attrib[16]`) failed on
            // DirectGLES with no symptom other than "the draw captured zeros".
            //
            // Desktop GL DOES allow the declaration, and it means exactly what the split
            // produces: element i of an input array consumes location base+i (GL 4.6 core
            // 11.1.1). So the split is a spelling change, not a semantic one - the same
            // vertex attributes feed the same components, and the frontend's reflection
            // (which the backends bind attributes from) is not involved.
            //
            // Downstream code is untouched on purpose: the original variable keeps its id and
            // its array type, so every OpAccessChain into it - including the DYNAMICALLY
            // indexed ones a `for` loop produces, which is precisely what an input array is
            // usually written for - stays valid against the Private copy.
            //
            // DirectGLES only. Vulkan takes array vertex inputs as they are.
            class SplitArrayVertexInputsPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-split-array-vertex-inputs"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateSplitArrayVertexInputsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
