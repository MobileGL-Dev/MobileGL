// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ExtendNarrowIntegerVertexInputsPass.h
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
#include <MG_Util/ShaderTranspiler/NarrowIntegerVertexInputs.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Gives the integer vertex inputs at the listed locations the value GL defines for
            // narrow integer attribute data (see NarrowIntegerVertexInputs). Each such input is
            // re-declared as a fresh Input variable carrying the original's interface decorations,
            // the original is demoted to a Private global, and the entry point seeds it once, at its
            // top, with the corrected value: the low 8/16 bits masked off for a zero-extension, or
            // sign-extended for a sign-extension. Every existing load and access chain keeps its id
            // and type, so no other instruction is rewritten, and an array input is corrected per
            // element (element i sits at location base + i). Inputs at other locations, and modules
            // that are not vertex shaders, are left untouched.
            //
            // ConstantMasks bakes the listed locations into the module. RuntimeWidths seeds every
            // 32-bit integer input below location 32 from two Private uint globals instead (see
            // kNarrowIntegerWidths8GlobalName), so one module serves every data width; a width bit of
            // 0 leaves the value as fetched.
            class ExtendNarrowIntegerVertexInputsPass : public spvtools::opt::Pass {
            public:
                enum class Mode : Uint8 {
                    ConstantMasks,
                    RuntimeWidths,
                };

                explicit ExtendNarrowIntegerVertexInputsPass(const NarrowIntegerVertexInputs& inputs)
                    : m_inputs(inputs) {}
                // RuntimeWidths; `outChanged` (optional) reports whether the module was rewritten.
                explicit ExtendNarrowIntegerVertexInputsPass(Bool* outChanged)
                    : m_mode(Mode::RuntimeWidths), m_outChanged(outChanged) {}
                const char* name() const override { return "mobilegl-extend-narrow-integer-vertex-inputs"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateExtendNarrowIntegerVertexInputsPass(
                    const NarrowIntegerVertexInputs& inputs);
                static spvtools::Optimizer::PassToken CreateRuntimeWidthsPass(Bool* outChanged);

            private:
                NarrowIntegerVertexInputs m_inputs;
                Mode m_mode = Mode::ConstantMasks;
                Bool* m_outChanged = nullptr;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
