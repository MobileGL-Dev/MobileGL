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
            // Patches ONE known shader-pack defect: iterationRP hard-sizes the scratch
            // its subgroup prefix scans write through prefixSumCache[gl_SubgroupID].
            // The pack ships that idiom twice, sized for the >= 16-lane subgroups
            // desktop GL drivers give it:
            //   - the auto-exposure reduction: 32x16 (512 invocations), vec2[32];
            //   - the RTW importance warp: 1024 invocations, float[64].
            // On a narrower Vulkan device (lavapipe's 8 lanes -> 64 and 128 subgroups)
            // every subgroup past the last declared entry indexes shared memory out of
            // bounds - on a CPU rasterizer that is literal heap corruption. Both
            // reduction ALGORITHMS are width-agnostic (their combine loops are sized by
            // gl_NumSubgroups), so the faithful repair is to grow the under-declared
            // arrays to ceil(invocations / native width) and change nothing else.
            //
            // This is the pack author's bug, not MobileGL's, so the patch is
            // deliberately NOT a general "resize shared arrays" mechanism. It rewrites
            // an array only when the module positively matches the pack's reduction
            // idiom AND the device's own topology proves the declaration too small:
            //   - GLCompute entry point with a literal workgroup size;
            //   - a subgroup scan/reduce over a 32-bit float scalar or vector
            //     (OpGroupNonUniformF{Add,Min,Max}), the pack's accumulator signature;
            //   - a workgroup-shared array of 32-bit float scalars/vectors whose
            //     access-chain index is data-dependent on gl_SubgroupID;
            //   - a declared length strictly below ceil(invocations / native width).
            // That last clause is what keeps the patch inert wherever the pack is
            // correct: on any device whose width satisfies the pack's assumption
            // (>= 16 lanes: desktop GL, Adreno) both shapes already fit and every
            // module passes through byte-identical. Matching at the SPIR-V level keeps
            // recognition robust against the whitespace/identifier drift that made the
            // old source-text template rewrite (removed in 7769156) so brittle.
            //
            // The pass never fails a module: anything it cannot prove is this pattern -
            // or cannot grow safely (a whole-array use, a spec-constant length, an
            // initializer, or growth that would not fit maxWorkgroupScratchBytes) - is
            // left exactly as it was. Pass the device's maxComputeSharedMemorySize as
            // maxWorkgroupScratchBytes; 0 falls back to the 16384-byte Vulkan minimum.
            class FixIterationRPSubgroupScratchPass : public spvtools::opt::Pass {
            public:
                FixIterationRPSubgroupScratchPass(Uint32 nativeSubgroupSize,
                                                  Uint32 maxWorkgroupScratchBytes)
                    : m_nativeSubgroupSize(nativeSubgroupSize),
                      m_maxWorkgroupScratchBytes(maxWorkgroupScratchBytes) {}

                const char* name() const override { return "fix-iterationrp-subgroup-scratch"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFixIterationRPSubgroupScratchPass(
                    Uint32 nativeSubgroupSize, Uint32 maxWorkgroupScratchBytes);

            private:
                Uint32 m_nativeSubgroupSize;
                Uint32 m_maxWorkgroupScratchBytes;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
