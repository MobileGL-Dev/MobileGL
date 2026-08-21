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
            // THE MEASURED COST, so the next wave does not re-diagnose it. Four GL 4.3 conformance
            // cases fail on BOTH backends and on both an Adreno 830 and a Mali G925 - i.e. on every
            // device, because no device has shaderFloat64 and the demotion therefore always runs:
            //
            //   KHR-GL43.shader_storage_buffer_object.basic-stdLayout-case3
            //   KHR-GL43.compute_shader.fp64-case1
            //   KHR-GL43.compute_shader.fp64-case3
            //   ...and the std430 half of the same stdLayout case.
            //
            // They fail in the two ways this comment predicts and in no other. stdLayout-case3
            // copies a block byte for byte: the output matches the input for bytes [0, 76) and is
            // zero from there on, which is exactly the block's size once every double became a
            // float and the layout repacked tightly. fp64-case1 reports ceil(2.2) as 2: the
            // uniform's double 2.0 is 0x4000000000000000, the demoted read takes its low 32 bits
            // (0.0), ceil(0.0 + 0.2) = 1.0f = 0x3F800000 lands in the low half of the 8-byte
            // output slot and the whole thing prints as 2.
            //
            // Fixing them means NOT demoting a double that lives in a buffer block, and carrying
            // it as a uvec2 word pair instead - preserving the application's byte layout exactly,
            // unpacking to fp32 for arithmetic and repacking on store. That is a large pass with
            // the same dmat problem the paragraph above describes (a uvec2 representation cannot
            // express a matrix stride either, so it would have to decline dmat types), and the
            // default-uniform routing above reflects the demoted module, so a representation
            // change there ripples into every glUniform*d. Four of 16085 cases; deliberately not
            // attempted. compute_shader.fp64-case2 passes today and any attempt has to keep it
            // green.
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
