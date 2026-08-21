// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeResourceArrayIndexPass.h
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
            // Desktop GL lets an ARRAY OF SHADER STORAGE BLOCKS and an ARRAY OF IMAGE UNIFORMS
            // alike be indexed with any dynamically-uniform expression (GL 4.6 core / GLSL 4.30
            // 4.1.9). GLSL ES keeps the stricter ES 3.1 rule for BOTH - the index must be a
            // *constant integral expression* - and the drivers enforce it to the letter:
            //
            //     Qualcomm, storage blocks:
            //       '[' : indexing into an SSBO array using a non-constant expression is not
            //             permitted
            //     Mesa, images:
            //       image arrays indexed with non-constant expressions are forbidden in GLSL ES
            //
            // glslang keeps the whole array as ONE SPIR-V variable, so SPIRV-Cross prints
            // `layout(binding = N, std430) buffer Blk { ... } arr[4];` plus `arr[i]` - or
            // `uniform image2D g_image[4];` plus `imageStore(g_image[i], ...)` - verbatim, and
            // the stage never compiles. The backend program then links nothing and every draw
            // or dispatch that uses it is a silent no-op, which reads back as "the buffer was
            // never written" rather than as an error - the frontend has already published
            // GL_LINK_STATUS = TRUE from glslang's own link.
            //
            // Verified on the device for the storage-block half: an Adreno 830 ES probe with no
            // MobileGL in the loop rejects the non-constant subscript with AND without
            // GL_EXT_gpu_shader5 (which the driver does advertise), and accepts a constant one.
            // Verified again for the image half on llvmpipe / Mesa 26.1.4 at ES 3.2, on a raw
            // GLES probe: a scalar image and an array with literal subscripts both write the
            // units they name, and both a loop-variable subscript and a `const int[]` table
            // lookup are refused with the message above. So the ES 3.2 "dynamically uniform"
            // relaxation is not a way out for either resource - every index really has to
            // become a compile-time constant.
            //
            // SAMPLER arrays are deliberately NOT covered. ESSL 3.20 4.1.7 does allow a sampler
            // array a dynamically-uniform index, and the same probe confirms it: a sampler array
            // subscripted by a loop variable, and one reached through a const table, both compile
            // and link. Lowering them would cost code for a rule that does not exist.
            //
            // Two modes, used as two halves of one legalization in
            // ShaderCompiler::LegalizeResourceArrayIndexingForEssl - the same shape, for
            // the same reasons, as LegalizeFragmentOutputIndexPass:
            //
            //   MarkLoopsForUnroll - `for (int i = 0; i < 4; ++i) arr[i].x = ...` is the
            //     common shape, and full unrolling turns its index into a literal at no cost
            //     in emitted code. spirv-opt's CreateLoopUnrollPass only touches loops whose
            //     OpLoopMerge carries the Unroll control, so this mode sets that hint on
            //     exactly the loops that enclose an offending access chain, and only when
            //     their trip count is known and small. Must run AFTER ssa-rewrite: both the
            //     trip-count check and the unroller need the induction variable as an OpPhi.
            //     Resource-kind-blind: the offending chain is the same instruction either way.
            //
            //   LowerToConstantSwitch - the fallback for a genuinely dynamic index
            //     (uniform-sourced, which is what the CTS indirect-addressing and resource-max
            //     cases use). A write through such a chain becomes an OpSwitch over the
            //     array's range with one constant-indexed access per case; a read becomes one
            //     constant-indexed access per element combined with OpSelect. This is what ANGLE
            //     does for the same ES 3.1 rule.
            //
            //     This half IS kind-specific, because the two resources are consumed
            //     differently. A storage block is reached by OpStore/OpLoad THROUGH the access
            //     chain, so the chain's own users are rewritten. An image's access chain is
            //     first OpLoad-ed into an opaque image OBJECT, which OpImageWrite/OpImageRead
            //     then consume - and an opaque type may not be selected (OpSelect is restricted
            //     to pointers, scalars and vectors before SPIR-V 1.4, and ESSL has no ternary on
            //     image types at all), so it is the image OPERATION that is duplicated per
            //     element, not the loaded object.
            //
            // A UNIFORM block array is a different namespace with its own (less strictly
            // enforced) rule and no observed failure, so it is deliberately left alone rather
            // than lowered on speculation.
            //
            // DirectGLES transpile path only: the original module is legal for Vulkan, which
            // has no such restriction, and DirectVulkan must keep seeing the array as one
            // descriptor array.
            //
            // The pass DECLINES - leaving the module untouched rather than half-transforming
            // it - whenever it meets a shape it cannot rewrite exactly: a pointer handed to a
            // function or chained further, an atomic or an OpArrayLength through the chain, a
            // load carrying memory operands, a spec-constant array length, an index that is
            // not a 32-bit integer, an image operation other than a plain read or write (an
            // OpImageTexelPointer, i.e. an imageAtomic*, above all - executing it per element
            // would perform the other elements' atomics too), or a store sitting in a loop
            // header block (splitting there would move the OpLoopMerge away from the back
            // edge's target).
            class LegalizeResourceArrayIndexPass final : public spvtools::opt::Pass {
            public:
                enum class Mode {
                    MarkLoopsForUnroll,
                    LowerToConstantSwitch,
                };

                explicit LegalizeResourceArrayIndexPass(Mode mode) : m_mode(mode) {}

                const char* name() const override {
                    return m_mode == Mode::MarkLoopsForUnroll
                               ? "mobilegl-mark-resource-array-index-loops"
                               : "mobilegl-lower-resource-array-index";
                }

                Status Process() override;

                static spvtools::Optimizer::PassToken CreateMarkLoopsForUnrollPass();
                static spvtools::Optimizer::PassToken CreateLowerToConstantSwitchPass();

                // The detection half, on a serialized module: true when an array of storage
                // blocks or of image uniforms is indexed with anything but an OpConstant. Cheap
                // enough to gate the whole legalization on (one BuildModule, no serialization)
                // and used again after the folding chain to decide whether the fallback has to
                // run at all.
                static bool BinaryHasDynamicResourceArrayIndexing(const std::vector<uint32_t>& binary);

            private:
                enum class LoweringOutcome {
                    // The shape is not one this pass can rewrite exactly; the module keeps
                    // the illegal chain rather than a half-transform of it.
                    Declined,
                    Changed,
                };

                Status MarkLoopsForUnroll();
                Status LowerToConstantSwitch();

                LoweringOutcome LowerOneChain(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                              bool isImageArray);
                LoweringOutcome LowerStore(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                           spvtools::opt::Instruction* store);
                LoweringOutcome LowerLoad(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                          spvtools::opt::Instruction* load);
                LoweringOutcome LowerImageChain(spvtools::opt::Instruction* accessChain, uint32_t arrayLength);
                void KillImageChainIfDead(spvtools::opt::Instruction* accessChain,
                                          spvtools::opt::Instruction* load);
                LoweringOutcome LowerImageWrite(spvtools::opt::Instruction* accessChain, uint32_t arrayLength,
                                                spvtools::opt::Instruction* load,
                                                spvtools::opt::Instruction* imageWrite);
                LoweringOutcome LowerImageReadOrQuery(spvtools::opt::Instruction* accessChain,
                                                      uint32_t arrayLength,
                                                      spvtools::opt::Instruction* load,
                                                      spvtools::opt::Instruction* consumer);

                Mode m_mode;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
