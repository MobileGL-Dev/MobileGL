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
                constexpr const char* kConflictingName = "sampler";
                constexpr const char* kCompatName = "MGL_COMPAT_sampler";

                Bool IsNamedSamplerFunctionParameter(spvtools::opt::IRContext* context,
                                                     spvtools::opt::Instruction& nameInst) {
                    if (nameInst.opcode() != spv::Op::OpName || nameInst.NumInOperands() < 2) {
                        return false;
                    }

                    if (nameInst.GetInOperand(1).AsString() != kConflictingName) {
                        return false;
                    }

                    auto* defUseMgr = context->get_def_use_mgr();
                    const Uint32 targetId = nameInst.GetSingleWordInOperand(0);
                    const auto* target = defUseMgr->GetDef(targetId);
                    return target != nullptr && target->opcode() == spv::Op::OpFunctionParameter;
                }
            } // namespace

            spvtools::opt::Pass::Status RenameSamplerFunctionParameterPass::Process() {
                Bool modified = false;
                auto* irContext = context();

                for (auto& debugInst : irContext->debugs2()) {
                    if (!IsNamedSamplerFunctionParameter(irContext, debugInst)) {
                        continue;
                    }

                    debugInst.SetInOperand(
                        1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(kCompatName));
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
