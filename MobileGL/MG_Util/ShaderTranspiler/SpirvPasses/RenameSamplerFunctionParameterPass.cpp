// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameSamplerFunctionParameterPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenameSamplerFunctionParameterPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                const char* GetCompatName(StringView name) {
                    if (name == "sampler") return "MGL_COMPAT_sampler";
                    if (name == "new") return "MGL_COMPAT_new";
                    return nullptr;
                }

                const char* GetConflictingFunctionParameterCompatName(spvtools::opt::IRContext* context,
                                                                       spvtools::opt::Instruction& nameInst) {
                    if (nameInst.opcode() != spv::Op::OpName || nameInst.NumInOperands() < 2) {
                        return nullptr;
                    }

                    const char* compatName = GetCompatName(nameInst.GetInOperand(1).AsString());
                    if (compatName == nullptr) {
                        return nullptr;
                    }

                    auto* defUseMgr = context->get_def_use_mgr();
                    const Uint32 targetId = nameInst.GetSingleWordInOperand(0);
                    const auto* target = defUseMgr->GetDef(targetId);
                    return target != nullptr && target->opcode() == spv::Op::OpFunctionParameter ? compatName : nullptr;
                }
            } // namespace

            spvtools::opt::Pass::Status RenameSamplerFunctionParameterPass::Process() {
                Bool modified = false;
                auto* irContext = context();

                for (auto& debugInst : irContext->debugs2()) {
                    const char* compatName = GetConflictingFunctionParameterCompatName(irContext, debugInst);
                    if (compatName == nullptr) {
                        continue;
                    }

                    debugInst.SetInOperand(
                        1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(compatName));
                    modified = true;
                }

                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken RenameSamplerFunctionParameterPass::CreateRenameSamplerFunctionParameterPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<RenameSamplerFunctionParameterPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
