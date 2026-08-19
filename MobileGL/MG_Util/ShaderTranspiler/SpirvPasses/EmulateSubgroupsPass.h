// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateSubgroupsPass.h
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
            // Lowers every GL_KHR_shader_subgroup construct in a compute module onto a
            // 32-lane VIRTUAL subgroup implemented with workgroup-shared memory. Virtual
            // subgroups partition the workgroup by gl_LocalInvocationIndex:
            // lane = index & 31, id = index >> 5, count = ceil(invocations / 32).
            //
            // This is a LAST-RESORT path, never a substitute for real subgroups: it only
            // runs when MOBILEGL_MAGMA_EMULATE_SUBGROUP=1 is set explicitly and the device
            // has no native subgroup support at all (SubgroupSupportPolicy.h). A device
            // with native subgroup operations - however narrow - uses them natively, with
            // FixIterationRPSubgroupScratchPass patching the known pack bug instead.
            //
            // Lowered constructs:
            //   - the builtins gl_SubgroupSize / gl_SubgroupInvocationID / gl_SubgroupID /
            //     gl_NumSubgroups and the five gl_Subgroup*Mask ballot builtins;
            //   - OpGroupNonUniform{Elect,All,Any,AllEqual,Broadcast,BroadcastFirst,
            //     Ballot,InverseBallot,BallotBitExtract,BallotBitCount,BallotFind{L,M}SB,
            //     Shuffle,ShuffleXor,ShuffleUp,ShuffleDown,
            //     <arithmetic/min/max/bitwise/logical reduce+scans+clustered>,
            //     QuadBroadcast,QuadSwap};
            //   - subgroupBarrier()/subgroupMemoryBarrier*() (their Subgroup scopes widen
            //     to Workgroup, which is strictly stronger).
            // The output uses no GroupNonUniform* instruction or capability at all, which
            // is what lets it run on devices with no subgroup feature bits.
            //
            // Semantic contract, narrower than native subgroups in exactly one way: every
            // emulated exchange synchronizes through OpControlBarrier, so subgroup
            // operations must sit in WORKGROUP-uniform control flow (the shape every
            // Iris-style pack reduction has). GLSL already imposes this for barrier();
            // a subgroup op in divergent flow - legal on native subgroups - is undefined
            // here.
            //
            // Fails (Status::Failure, leaving the input module unchanged) on anything it
            // cannot lower faithfully: extended subgroup ops (partitioned-NV, rotate,
            // quad-all/any), non-32-bit participating types, spec-constant workgroup
            // sizes, a subgroup builtin reached by anything but a direct OpLoad, or a
            // module whose lowering would add more workgroup scratch than
            // maxWorkgroupScratchBytes (pass the device's maxComputeSharedMemorySize;
            // 0 falls back to the 16384-byte Vulkan minimum).
            class EmulateSubgroupsPass : public spvtools::opt::Pass {
            public:
                explicit EmulateSubgroupsPass(Uint32 maxWorkgroupScratchBytes)
                    : m_maxWorkgroupScratchBytes(maxWorkgroupScratchBytes) {}

                const char* name() const override { return "emulate-subgroups"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateEmulateSubgroupsPass(
                    Uint32 maxWorkgroupScratchBytes);

            private:
                Uint32 m_maxWorkgroupScratchBytes;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
