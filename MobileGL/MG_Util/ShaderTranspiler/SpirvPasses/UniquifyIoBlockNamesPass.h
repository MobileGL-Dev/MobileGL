// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/UniquifyIoBlockNamesPass.h
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

#include <map>
#include <set>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Renames the STRUCT of an inter-stage interface block, so a block name a stage
            // declares in both directions at once gets one spelling per producing stage.
            //
            // WHY. Desktop GLSL keeps SEPARATE name namespaces for input and output interface
            // blocks, so a single stage may legally write
            //
            //     in  TCSOutputBlock { ... } input_block[];
            //     out TCSOutputBlock { ... } output_block;
            //
            // which is exactly what the tessellation evaluation stage of
            // KHR-GL42/43.shading_language_420pack.length_of_vector_and_matrix_* and
            // .qualifier_order_block_* does. glslang accepts it deliberately (ParseHelper
            // errors only when the two share a storage qualifier) and SPIRV-Cross re-emits
            // BOTH under the name TCSOutputBlock, because it too splits the namespace
            // (block_input_names vs block_output_names). The generated ESSL 3.20 then declares
            // two different blocks called TCSOutputBlock in one shader. Adreno's ES compiler
            // keeps them apart; Mali's does not - the stage compiles, the program links, and
            // the output block's payload never reaches the next stage, which is all 22 of
            // that group's Mali failures and none of Adreno's or DirectVulkan's.
            //
            // WHAT. The rename is planned program-wide by the CALLER and keyed on the
            // PRODUCING stage, so a producer and its consumer keep naming the same block:
            // the tessellation control stage's `out TCSOutputBlock` and the evaluation
            // stage's `in TCSOutputBlock` both become <name>_mgio<TCS>, while the evaluation
            // stage's own `out TCSOutputBlock` and the geometry stage's `in TCSOutputBlock`
            // both become <name>_mgio<TES>. Only the block TYPE name changes; instance names,
            // member names, locations and every decoration are left exactly as they were, and
            // ES matches inter-stage blocks by block name plus member sequence.
            //
            // DirectGLES only: DirectVulkan hands the module to the driver as SPIR-V, where
            // the two blocks are distinct type ids and the debug names carry no meaning.
            class UniquifyIoBlockNamesPass : public spvtools::opt::Pass {
            public:
                // `inputBlockRenames` applies to blocks this stage CONSUMES and
                // `outputBlockRenames` to blocks it PRODUCES, both keyed by the block's
                // current name. `renamedBlockNames` receives the ORIGINAL names this stage
                // actually rewrote, so the caller can adopt the re-serialised module only
                // when there was something to rewrite.
                UniquifyIoBlockNamesPass(const std::map<String, String>& inputBlockRenames,
                                         const std::map<String, String>& outputBlockRenames,
                                         std::set<String>* renamedBlockNames)
                    : m_inputBlockRenames(inputBlockRenames), m_outputBlockRenames(outputBlockRenames),
                      m_renamedBlockNames(renamedBlockNames) {}

                const char* name() const override { return "mobilegl-uniquify-io-block-names"; }
                Status Process() override;

                // Reads a module WITHOUT rewriting it, for the caller's gate. Adds to
                // `outCollidingBlockNames` every block name this module declares in BOTH Input
                // and Output storage under two DIFFERENT struct types - the only shape the
                // rename above can repair - and to `outDeclaredNames` every name the module
                // spells, so the caller can pick a replacement that collides with none of them.
                // Builtin blocks (gl_PerVertex and friends) are never reported.
                static void ProbeIoBlockNames(spvtools::opt::IRContext* irContext,
                                              std::set<String>& outCollidingBlockNames,
                                              std::set<String>& outDeclaredNames);

                static spvtools::Optimizer::PassToken CreateUniquifyIoBlockNamesPass(
                    const std::map<String, String>& inputBlockRenames,
                    const std::map<String, String>& outputBlockRenames,
                    std::set<String>* renamedBlockNames);

            private:
                std::map<String, String> m_inputBlockRenames;
                std::map<String, String> m_outputBlockRenames;
                std::set<String>* m_renamedBlockNames = nullptr;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
