// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DemoteFloat64Pass.h
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
            // Rewrites every 64-bit float in the module to a 32-bit one: `OpTypeFloat 64` becomes
            // `OpTypeFloat 32` in place, so every vector, matrix, array, struct, pointer and
            // function type that named it keeps its <id> and every decoration attached to it, and
            // only the meaning of the leaf type changes. The double literals are re-encoded, the
            // now-width-preserving OpFConvert pairs collapse to their operand, and the Float64
            // capability goes away.
            //
            // Why demote at all: no mobile GPU has it. Adreno and Mali both report
            // VkPhysicalDeviceFeatures::shaderFloat64 == VK_FALSE (the Magma POST has a row for it),
            // so a module declaring Float64 cannot become a pipeline there; and ESSL has no 64-bit
            // float type at all, so SPIRV-Cross throws "FP64 not supported in ES profile" and the
            // Espryt path never even reaches the driver. Demotion is what makes `double` in an
            // application's GLSL compile and run everywhere, at fp32 precision.
            //
            // BLOCK LAYOUT IS RE-DERIVED, NOT PRESERVED, and that was not the first choice - see
            // BlockRelayout in the .cpp for the measurement that forced it. Preserving the 64-bit
            // offsets (float + 4 bytes of padding in each slot) keeps the application's byte layout
            // intact and is what a Vulkan-only implementation would do, but GLSL ES has no member
            // `layout(offset=)`, so SPIRV-Cross recomputes std140/std430 from the declared types
            // and refuses any block whose stated offsets disagree - "Buffer block cannot be
            // expressed as any of std430, std140, scalar". Every shader with a double in a block
            // would then fail to transpile for Espryt at all, which is the case this demotion
            // exists to fix. Nor can padding members rescue it: a dmat4 member carries
            // MatrixStride 32 and std140 requires 16 for the demoted mat4, and padding BETWEEN
            // members cannot change a stride INSIDE one.
            //
            // What re-deriving costs, stated plainly: a block that held 64-bit members changes its
            // driver-visible byte layout, so an application that hard-codes std140 offsets it
            // computed for doubles addresses the wrong bytes. Applications that query their offsets
            // are unaffected. MobileGL's own default-uniform block is unaffected by construction:
            // the frontend builds its uniform routing by reflecting the module this pass produced
            // (ProgramSpirvTask::BuildGlobalUboRouting), so glUniform*d - which narrows to float
            // for the same reason - writes exactly where the demoted shader reads. Blocks with no
            // 64-bit member anywhere are never touched.
            //
            // Declines (leaves the module byte-identical, so the caller's existing "this module
            // still declares Float64" failure path reports it) when the module contains an
            // operation whose validity depends on the operand really being 64 bits wide:
            //   - OpBitcast across the boundary - packDouble2x32 / doubleBitsToUint64 and friends,
            //     where SPIR-V requires both sides to have the same total bit width;
            //   - GLSL.std.450 PackDouble2x32 / UnpackDouble2x32, which are defined only for a
            //     64-bit float result/operand.
            //
            // ORDERING: must run before PackDoubleVertexInputsPass, which introduces exactly the
            // OpBitcast this pass declines on. After demotion no 64-bit vertex input is left, so
            // that pass becomes a no-op rather than a conflict.
            //
            // The in-place rewrite creates duplicate type declarations by construction - a module
            // that had both `double` and `float` ends up with two `OpTypeFloat 32`, and spirv-val
            // rejects that ("Duplicate non-aggregate type declarations are not allowed") - so the
            // pass merges them itself afterwards. Deliberately NOT by registering spvtools'
            // RemoveDuplicates alongside it: that pass also merges structurally identical STRUCTS
            // and calls KillNamesAndDecorates on the loser, which would silently delete the OpName
            // of one of two distinct-but-identically-shaped interface blocks - and OpName is how
            // MobileGL resolves block and varying names. Only the non-aggregate types spirv-val
            // actually forbids duplicates of are merged here; arrays and structs are left alone,
            // which also keeps a `double[]`'s ArrayStride from being merged into a `float[]`'s.
            class DemoteFloat64Pass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-demote-float64"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDemoteFloat64Pass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
