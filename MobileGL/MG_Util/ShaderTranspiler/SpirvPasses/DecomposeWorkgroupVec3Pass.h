// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecomposeWorkgroupVec3Pass.h
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
            // Decomposes vec3/ivec3/uvec3/bvec3 variables in the Workgroup storage class
            // (GLSL `shared` memory) into scalar arrays (e.g. `shared vec3 arr[N]` ->
            // `shared float arr[N][3]`). Whole-vector loads/stores are rewritten into
            // per-component scalar loads/stores. This works around drivers (e.g.
            // ANGLE/Metal) that reject `shared vec3` due to workgroup memory alignment.
            //
            // Component-level accesses (e.g. `arr[i].x`) require no rewriting because a
            // trailing component index into a `float[3]` yields the same scalar pointer
            // as it did for a `vec3`.
            class DecomposeWorkgroupVec3Pass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "decompose-workgroup-vec3"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDecomposeWorkgroupVec3Pass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
