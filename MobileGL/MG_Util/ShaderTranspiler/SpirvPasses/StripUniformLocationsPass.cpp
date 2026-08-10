// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUniformLocationsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "StripUniformLocationsPass.h"

#include "source/opt/ir_context.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            spvtools::opt::Pass::Status StripUniformLocationsPass::Process() {
                using spvtools::opt::Instruction;

                std::vector<Instruction*> toKill;
                for (auto& annotation : get_module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate) {
                        continue;
                    }
                    if (annotation.GetSingleWordInOperand(1) !=
                        static_cast<uint32_t>(spv::Decoration::Location)) {
                        continue;
                    }
                    Instruction* target =
                        get_def_use_mgr()->GetDef(annotation.GetSingleWordInOperand(0));
                    if (target == nullptr || target->opcode() != spv::Op::OpVariable) {
                        continue;
                    }
                    switch (spv::StorageClass(target->GetSingleWordInOperand(0))) {
                        case spv::StorageClass::UniformConstant:
                        case spv::StorageClass::Uniform:
                        case spv::StorageClass::StorageBuffer:
                            toKill.push_back(&annotation);
                            break;
                        default:
                            break;
                    }
                }

                for (Instruction* inst : toKill) {
                    context()->KillInst(inst);
                }
                return toKill.empty() ? Status::SuccessWithoutChange : Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken StripUniformLocationsPass::CreateStripUniformLocationsPass() {
                return spvtools::Optimizer::PassToken(
                    spvtools::MakeUnique<StripUniformLocationsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
