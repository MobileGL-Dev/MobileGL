// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerClipDistanceForEsslPass.h
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
            // Adreno's ESSL compiler mishandles gl_ClipDistance (device-verified on Adreno 750):
            //   - writes through non-constant indices silently fail to link,
            //   - reads of gl_in[i].gl_ClipDistance[k] with a CONSTANT k >= 1 fail to compile
            //     ("array indexing out of boundary") while dynamic-index reads work,
            //   - compiling a whole-array read of gl_in[i].gl_ClipDistance segfaults the
            //     compiler backend (libllvm-qgl.so).
            // This pass shadows the builtin so the decompiled ESSL only ever touches it in the
            // shapes Adreno accepts. Output side (vertex + geometry): all accesses to the
            // Output ClipDistance (gl_PerVertex member or standalone variable) are redirected
            // to a Private mg_ClipDistance array, and a flush writing the real builtin with
            // literal constant indices is inserted before every OpEmitVertex (geometry) or
            // every return of the entry point (vertex). Input side (geometry): accesses to
            // gl_in[...].gl_ClipDistance are redirected to a Private mg_ClipDistanceIn
            // array-of-arrays filled once at the top of the entry point by a structured loop
            // whose gl_in reads use dynamic (loop-variable) indices. The builtin members stay
            // statically referenced by the flush/copy so cross-stage IO matching is intact.
            // Only meant for the DirectGLES transpile path on Qualcomm devices.
            class LowerClipDistanceForEsslPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "lower-clip-distance-for-essl"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateLowerClipDistanceForEsslPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
