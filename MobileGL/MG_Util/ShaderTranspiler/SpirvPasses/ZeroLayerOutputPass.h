// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ZeroLayerOutputPass.h
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
            // GL ignores gl_Layer when the framebuffer is not layered (GL 4.6 core 9.8): every
            // primitive lands in the one attached layer whatever the shader wrote. Vulkan has no
            // such rule - a Layer value at or past the framebuffer's layer count is undefined,
            // and Adreno offsets the primitive by it, out of the attachment
            // (KHR-GL4x.texture_cube_map_array.color_depth_attachments renders layer-face 1's
            // draw into face 2 of a single-layer attachment, and nothing into face 1). This pass
            // builds the module a non-layered framebuffer needs: every store to the Layer output
            // stores 0 instead.
            //
            // The renderer keeps this as a second module beside the original rather than
            // selecting with a specialization constant: a `layered ? value : 0` select on a
            // boolean specialization constant did not change the result on Adreno 750, while a
            // constant 0 store did.
            //
            // A use of the builtin other than a direct whole-variable store fails the pass, and
            // the caller then keeps the original module.
            class ZeroLayerOutputPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "zero-layer-output"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateZeroLayerOutputPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
