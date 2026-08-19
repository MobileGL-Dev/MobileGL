// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPSubgroupScratchPass.h
// Copyright (c) 2026 MobileGL-Dev
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
            // Patches ONE known shader-pack defect: iterationRP's auto-exposure reduction
            // declares `shared vec2 prefixSumCache[32]` for its 512-invocation workgroup
            // and stores per-subgroup subtotals through prefixSumCache[gl_SubgroupID].
            // The pack hard-sized that scratch for the >=16-lane subgroups desktop GL
            // drivers ship; on a narrower Vulkan device (lavapipe's 8 lanes -> 64
            // subgroups) every subgroup past entry 31 indexes shared memory out of
            // bounds - on a CPU rasterizer that is literal heap corruption. The
            // reduction ALGORITHM is width-agnostic (its combine loop is sized by
            // gl_NumSubgroups), so the faithful repair is to grow the one under-declared
            // array to ceil(512 / native width) and change nothing else. This is the
            // pack author's bug, not MobileGL's; the patch is therefore deliberately
            // NOT a general mechanism - it only rewrites modules that positively match
            // iterationRP's reduction fingerprint:
            //   - GLCompute entry point with local size exactly 32x16x1;
            //   - a subgroupInclusiveAdd on a vec2 (OpGroupNonUniformFAdd InclusiveScan,
            //     the pack's luminance/exposure accumulator signature);
            //   - a workgroup-shared array of exactly vec2[32] whose access-chain index
            //     is data-dependent on gl_SubgroupID.
            // Matching at the SPIR-V level keeps the recognition robust against
            // whitespace/identifier-level drift that made the old source-text template
            // rewrite (removed in 7769156) so brittle, while still refusing to touch
            // anything that is not this pack's reduction. On devices whose native width
            // already satisfies the pack's assumption (>= 16 lanes: desktop GL, Adreno),
            // the grown length equals or undershoots the declared 32 and every module
            // passes through byte-identical.
            //
            // The pass never fails a module: anything it cannot prove is this exact
            // pattern - or cannot grow safely (a whole-array use, a spec-constant
            // length, an initializer) - is left exactly as it was.
            class FixIterationRPSubgroupScratchPass : public spvtools::opt::Pass {
            public:
                explicit FixIterationRPSubgroupScratchPass(Uint32 nativeSubgroupSize)
                    : m_nativeSubgroupSize(nativeSubgroupSize) {}

                const char* name() const override { return "fix-iterationrp-subgroup-scratch"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFixIterationRPSubgroupScratchPass(
                    Uint32 nativeSubgroupSize);

            private:
                Uint32 m_nativeSubgroupSize;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
