// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PackDoubleVertexInputsPass.h
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
            // Re-declares every 64-bit floating-point *vertex input* as the 32-bit unsigned word
            // pair that holds the same bytes (double -> uvec2, dvec2 -> uvec4), demotes the
            // original variable to a Private global, and seeds it once at the top of the entry
            // point with an OpBitcast of the new input. Everything downstream keeps loading the
            // same id and the same double type, so no other instruction is rewritten.
            //
            // Why not just use VK_FORMAT_R64*_SFLOAT: those formats are optional, and lavapipe
            // reports zero bufferFeatures for all four of them, so a 64-bit vertex fetch is
            // impossible there even though shaderFloat64 is supported. The word-pair form needs no
            // format capability at all and is bit-exact, so it is applied unconditionally rather
            // than as a fallback - which also keeps it in lockstep with
            // VertexInputStateFactory::ToVkVertexFormat, since both branch on nothing but "is this
            // a 64-bit vertex input".
            //
            // DirectVulkan only. The 64-bit *arithmetic* still needs the Float64 capability, i.e.
            // an enabled VkPhysicalDeviceFeatures::shaderFloat64.
            class PackDoubleVertexInputsPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-pack-double-vertex-inputs"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreatePackDoubleVertexInputsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
