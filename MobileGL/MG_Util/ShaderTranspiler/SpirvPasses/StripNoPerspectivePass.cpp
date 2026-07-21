// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripNoPerspectivePass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "StripNoPerspectivePass.h"

#include "spirv.hpp"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // OpDecorate       <target-id> <decoration> [literals...]
                // OpMemberDecorate <struct-id>  <member> <decoration> [literals...]
                constexpr uint32_t kDecorateDecorationOperand = 1;
                constexpr uint32_t kMemberDecorateDecorationOperand = 2;
            } // namespace

            spvtools::opt::Pass::Status StripNoPerspectivePass::Process() {
                auto* irContext = context();

                // Collect first: KillInst mutates the annotation list being walked.
                std::vector<Instruction*> toKill;
                for (Instruction& annotation : irContext->annotations()) {
                    uint32_t decorationOperand = 0;
                    if (annotation.opcode() == spv::Op::OpDecorate) {
                        decorationOperand = kDecorateDecorationOperand;
                    } else if (annotation.opcode() == spv::Op::OpMemberDecorate) {
                        decorationOperand = kMemberDecorateDecorationOperand;
                    } else {
                        continue;
                    }

                    if (annotation.NumInOperands() <= decorationOperand) {
                        continue;
                    }
                    if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(decorationOperand)) ==
                        spv::Decoration::NoPerspective) {
                        toKill.push_back(&annotation);
                    }
                }

                if (toKill.empty()) {
                    return Status::SuccessWithoutChange;
                }

                for (Instruction* inst : toKill) {
                    irContext->KillInst(inst);
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken StripNoPerspectivePass::CreateStripNoPerspectivePass() {
                return spvtools::Optimizer::PassToken(MakeUnique<StripNoPerspectivePass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
