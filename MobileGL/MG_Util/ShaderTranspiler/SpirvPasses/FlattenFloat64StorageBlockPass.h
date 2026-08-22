// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenFloat64StorageBlockPass.h
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
            // Rewrites a SHADER STORAGE BLOCK that contains a 64-bit float into a flat
            // `uint` word array, and turns every access to it into address arithmetic over
            // that array. The application's byte layout survives exactly; the VALUES are
            // still narrowed to 32-bit floats, because that is all the target has.
            //
            // Registered ONLY on the demoting path, immediately before DemoteFloat64Pass, and
            // capability-gated with it (ShaderCompiler::SanitizeAndOptimizeBinary). Where the
            // backend consumes 64-bit floats itself there is no narrowing for this to preserve a
            // layout across, and flattening a block the driver would have laid out correctly by
            // itself would only cost the shader its index arithmetic.
            //
            // WHY THIS EXISTS. DemoteFloat64Pass rewrites `double` to `float` in place and
            // lets SPIRV-Cross re-derive the block's packing from the declared types, because
            // GLSL ES has no member `layout(offset=)` and SPIRV-Cross refuses any block whose
            // stated offsets it cannot express as std140 or std430. That re-derivation moves
            // every member past the first double: the block a shader reads and writes stops
            // being the block the application filled. Byte-for-byte, on the shape
            // KHR-GL43.shader_storage_buffer_object.basic-stdLayout-case3 uses, the output
            // matched the input up to the first double's slot and was zero from there on -
            // the demoted block is simply shorter than the one that was bound.
            //
            // A flat `uint[]` has no layout to re-derive: one member, offset 0, ArrayStride 4,
            // which IS std430, so SPIRV-Cross prints it unconditionally and the driver lays it
            // out the only way it can. Every member's real byte offset - the std140 or std430
            // one glslang computed WITH the doubles in place - then lives in the index
            // arithmetic this pass emits, not in the declaration. The two ways the earlier
            // attempt at this was blocked both disappear with it:
            //
            //   * dmat: a `uvec2`-per-double representation cannot express a MatrixStride, so
            //     it would have had to decline matrices of doubles. Here a stride is a number
            //     in an address computation and nothing else, so dmat needs no special case.
            //   * the default-uniform block: its routing is built by reflecting the DEMOTED
            //     module (ProgramSpirvTask::BuildGlobalUboRouting), so changing how a double
            //     is carried there would ripple into every glUniform*d. This pass touches
            //     StorageBuffer blocks only and never that one.
            //
            // WHAT GL SEES IS UNCHANGED, and becomes CORRECT rather than merely unchanged:
            // glGetProgramResourceiv answers from glslang's reflection of the pre-demotion
            // module (ProgramInterface.cpp reads TObjectReflection::offset), i.e. the true
            // fp64 offsets. Before this pass those offsets described a layout no shader used;
            // now they describe the one it does.
            //
            // PRECISION, stated plainly. A double still becomes a float: the load narrows the
            // stored binary64 to binary32 and the store widens it back, so a value that does
            // not survive a round trip through 32 bits does not survive this either. The
            // narrowing truncates the discarded mantissa bits rather than rounding to nearest,
            // and flushes what binary32 can only hold as a subnormal to a signed zero; NaN
            // stays NaN and an out-of-range magnitude becomes an infinity. That is the same
            // fp32 promise DemoteFloat64Pass already makes - what changes is only that the
            // BYTES around the value stay where the application put them.
            //
            // DECLINES, leaving the block exactly as it was for DemoteFloat64Pass to handle the
            // old way, whenever it meets something it cannot rewrite exactly:
            //   - a block whose variable is used as anything but an access-chain base (loaded
            //     whole, handed to a function, asked its OpArrayLength);
            //   - an access chain that is not rooted at the variable, or whose result feeds
            //     anything but a plain OpLoad / OpStore (an atomic, OpCopyMemory, a further
            //     chain);
            //   - a non-constant index into a struct, a runtime array anywhere in the block, a
            //     RowMajor matrix (its columns are not contiguous, so a whole-column access is
            //     not one range), a member width other than 32 or 64 bits, or an offset or
            //     stride that is not a multiple of 4;
            //   - a load or store whose type decomposes into more scalars than the cap below,
            //     so legalizing a block can never explode the module.
            //
            // ORDERING: must run BEFORE DemoteFloat64Pass, which is what turns the doubles this
            // pass leaves in the function body into floats - the OpFConvert pairs emitted here
            // are width-preserving by then and collapse to their operands. It emits only 32-bit
            // OpBitcasts, so it never trips that pass's "bitcast across the 64-bit boundary"
            // decline.
            class FlattenFloat64StorageBlockPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-flatten-float64-storage-block"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateFlattenFloat64StorageBlockPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
