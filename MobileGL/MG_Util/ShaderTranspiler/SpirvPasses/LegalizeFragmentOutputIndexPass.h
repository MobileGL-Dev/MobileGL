// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeFragmentOutputIndexPass.h
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
            // GLSL ES requires a *constant integral expression* to index a fragment output
            // array (GLSL ES 3.00 4.3.6 / 3.20 4.4.2); SPIR-V has no such rule, so a shader
            // that writes `coeff[i]` from a loop reaches SPIRV-Cross intact and comes out as
            // ESSL a strict driver rejects outright:
            //
            //     '[' : array indexes for fragment outputs must be constant integral expressions
            //
            // The program then links nothing and every draw that uses it is a silent no-op.
            // Mesa accepts the same source, which is why this only ever showed on the ANGLE
            // lane (see tools/trace_replay/README.md, improved-transparency-minecraft-26.3:
            // the whole translucent layer disappears because the OIT coefficient shader is
            // exactly this shape).
            //
            // Two modes, used as two halves of one legalization in
            // ShaderCompiler::LegalizeFragmentOutputIndexingForEssl:
            //
            //   MarkLoopsForUnroll - the companion the stock unroller needs. spirv-opt's
            //     CreateLoopUnrollPass only touches loops whose OpLoopMerge carries the
            //     Unroll loop control (LoopUtils::HasUnrollLoopControl), which glslang emits
            //     only for an explicit [[unroll]]. This mode sets that hint on the loops that
            //     actually enclose an offending access chain - and only those, so an
            //     unrelated long loop elsewhere in the same shader is never unrolled - and
            //     only when their trip count is known and small, so legalizing a shader can
            //     never explode it. With the hint set, the stock chain (ssa-rewrite,
            //     loop-unroll, ccp, simplification, dead-branch-elim) folds a loop-derived
            //     index to a literal, which is what the real-world shaders (the OIT one
            //     included) need. Must run AFTER ssa-rewrite: both the trip-count check and
            //     the unroller itself need the induction variable as an OpPhi.
            //
            //   LowerToConstantSwitch - the fallback for an index that is *genuinely*
            //     dynamic (uniform-derived, a non-constant trip count, vertex data). It
            //     rewrites each write through such an access chain into an OpSwitch over the
            //     array's range with one constant-indexed store per case - the SPIR-V of
            //     `switch (i) { case 0: o[0] = v; break; case 1: o[1] = v; break; }` - and
            //     each read into per-element constant-indexed loads combined with OpSelect.
            //     An out-of-range index stores nothing, which is what indexing an output
            //     array out of range already meant.
            //
            // Fragment stage only: every other stage may index an output array dynamically
            // in ESSL, and on DirectVulkan the original SPIR-V is legal as-is. The pass
            // declines (leaving the module untouched) rather than half-transforming whenever
            // it meets a shape it cannot rewrite exactly - a pointer handed to a function, a
            // spec-constant array length, an index type that is not a 32-bit integer, or a
            // store sitting in a loop header block, where splitting would move the
            // OpLoopMerge away from the back edge's target.
            class LegalizeFragmentOutputIndexPass final : public spvtools::opt::Pass {
            public:
                enum class Mode {
                    MarkLoopsForUnroll,
                    LowerToConstantSwitch,
                };

                explicit LegalizeFragmentOutputIndexPass(Mode mode) : m_mode(mode) {}

                const char* name() const override {
                    return m_mode == Mode::MarkLoopsForUnroll ? "mobilegl-mark-fragment-output-index-loops"
                                                              : "mobilegl-lower-fragment-output-index";
                }

                Status Process() override;

                static spvtools::Optimizer::PassToken CreateMarkLoopsForUnrollPass();
                static spvtools::Optimizer::PassToken CreateLowerToConstantSwitchPass();

                // The detection half, on a serialized module: true when a fragment entry
                // point indexes an Output-storage array with anything but an OpConstant.
                // Cheap enough to gate the whole legalization on (one BuildModule, no
                // serialization) and used again after the folding chain to decide whether
                // the fallback has to run at all.
                static bool BinaryHasDynamicOutputIndexing(const std::vector<uint32_t>& binary);

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
