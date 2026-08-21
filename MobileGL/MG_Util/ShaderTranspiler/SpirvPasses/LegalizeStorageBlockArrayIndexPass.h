// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeStorageBlockArrayIndexPass.h
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
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // GL 4.3 lets an ARRAY OF SHADER STORAGE BLOCKS be indexed with any
            // dynamically-uniform expression (GL 4.6 core / GLSL 4.30 4.1.9). GLSL ES keeps
            // the stricter ES 3.1 rule - the index must be a *constant integral expression* -
            // and the Qualcomm ES compiler enforces it to the letter:
            //
            //     '[' : indexing into an SSBO array using a non-constant expression is not
            //           permitted
            //
            // glslang keeps the whole array as ONE SPIR-V variable, so SPIRV-Cross prints
            // `layout(binding = N, std430) buffer Blk { ... } arr[4];` plus `arr[i]` verbatim
            // and the stage never compiles. The backend program then links nothing and every
            // draw or dispatch that uses it is a silent no-op, which reads back as "the buffer
            // was never written" rather than as an error - the frontend has already published
            // GL_LINK_STATUS = TRUE from glslang's own link.
            //
            // Verified on the device: an Adreno 830 ES probe with no MobileGL in the loop
            // rejects the non-constant subscript with AND without GL_EXT_gpu_shader5 (which
            // the driver does advertise), and accepts a constant one. So the ES 3.2
            // "dynamically uniform" relaxation is not a way out - every index really has to
            // become a compile-time constant.
            //
            // Two modes, used as two halves of one legalization in
            // ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl - the same shape, for
            // the same reasons, as LegalizeFragmentOutputIndexPass:
            //
            //   MarkLoopsForUnroll - `for (int i = 0; i < 4; ++i) arr[i].x = ...` is the
            //     common shape, and full unrolling turns its index into a literal at no cost
            //     in emitted code. spirv-opt's CreateLoopUnrollPass only touches loops whose
            //     OpLoopMerge carries the Unroll control, so this mode sets that hint on
            //     exactly the loops that enclose an offending access chain, and only when
            //     their trip count is known and small. Must run AFTER ssa-rewrite: both the
            //     trip-count check and the unroller need the induction variable as an OpPhi.
            //
            //   LowerToConstantSwitch - the fallback for a genuinely dynamic index
            //     (uniform-sourced, which is what the CTS indirect-addressing and resource-max
            //     cases use). A write through such a chain becomes an OpSwitch over the
            //     array's range with one constant-indexed store per case; a read becomes one
            //     constant-indexed load per element combined with OpSelect. This is what ANGLE
            //     does for the same ES 3.1 rule.
            //
            // Storage blocks only. A UNIFORM block array is a different namespace with its own
            // (less strictly enforced) rule and no observed failure, so it is deliberately left
            // alone rather than lowered on speculation.
            //
            // DirectGLES transpile path only: the original module is legal for Vulkan, which
            // has no such restriction, and DirectVulkan must keep seeing the array as one
            // descriptor array.
            //
            // The pass DECLINES - leaving the module untouched rather than half-transforming
            // it - whenever it meets a shape it cannot rewrite exactly: a pointer handed to a
            // function or chained further, an atomic or an OpArrayLength through the chain, a
            // load carrying memory operands, a spec-constant array length, an index that is
            // not a 32-bit integer, or a store sitting in a loop header block (splitting there
            // would move the OpLoopMerge away from the back edge's target).
            class LegalizeStorageBlockArrayIndexPass final : public spvtools::opt::Pass {
            public:
                enum class Mode {
                    MarkLoopsForUnroll,
                    LowerToConstantSwitch,
                };

                explicit LegalizeStorageBlockArrayIndexPass(Mode mode) : m_mode(mode) {}

                const char* name() const override {
                    return m_mode == Mode::MarkLoopsForUnroll
                               ? "mobilegl-mark-storage-block-array-index-loops"
                               : "mobilegl-lower-storage-block-array-index";
                }

                Status Process() override;

                static spvtools::Optimizer::PassToken CreateMarkLoopsForUnrollPass();
                static spvtools::Optimizer::PassToken CreateLowerToConstantSwitchPass();

                // The detection half, on a serialized module: true when an array of storage
                // blocks is indexed with anything but an OpConstant. Cheap enough to gate the
                // whole legalization on (one BuildModule, no serialization) and used again
                // after the folding chain to decide whether the fallback has to run at all.
                static bool BinaryHasDynamicStorageBlockArrayIndexing(const std::vector<uint32_t>& binary);

            private:
                enum class LoweringOutcome {
                    // The shape is not one this pass can rewrite exactly; the module keeps
                    // the illegal chain rather than a half-transform of it.
                    Declined,
                    Changed,
                };

                Status MarkLoopsForUnroll();
                Status LowerToConstantSwitch();

                LoweringOutcome LowerOneChain(spvtools::opt::Instruction* accessChain, uint32_t arrayLength);
                LoweringOutcome LowerStore(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                           spvtools::opt::Instruction* store);
                LoweringOutcome LowerLoad(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                          spvtools::opt::Instruction* load);

                Mode m_mode;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
