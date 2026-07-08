// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerDrawParametersPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LowerDrawParametersPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                const char* LoweredNameForBuiltin(spv::BuiltIn builtin) {
                    switch (builtin) {
                    case spv::BuiltIn::BaseVertex:
                        return "mg_BaseVertex";
                    case spv::BuiltIn::BaseInstance:
                        return "mg_BaseInstance";
                    case spv::BuiltIn::DrawIndex:
                        return "mg_DrawID";
                    default:
                        return nullptr;
                    }
                }

                void ReplaceName(IRContext* context, uint32_t id, const char* name) {
                    for (auto& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName && debugInst.GetSingleWordInOperand(0) == id) {
                            debugInst.SetInOperand(
                                1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(name));
                            return;
                        }
                    }
                    context->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpName, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {id}},
                            {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
                }

                void RemoveFromEntryPointInterfaces(IRContext* context, uint32_t id) {
                    for (Instruction& entryPoint : context->module()->entry_points()) {
                        std::vector<Operand> newOperands;
                        Bool changed = false;
                        for (uint32_t i = 0; i < entryPoint.NumInOperands(); ++i) {
                            const Operand& operand = entryPoint.GetInOperand(i);
                            // Interface ids start after execution model, entry-point id and name.
                            if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID &&
                                entryPoint.GetSingleWordInOperand(i) == id) {
                                changed = true;
                                continue;
                            }
                            newOperands.push_back(operand);
                        }
                        if (changed) {
                            entryPoint.SetInOperands(std::move(newOperands));
                        }
                    }
                }
            } // namespace

            spvtools::opt::Pass::Status LowerDrawParametersPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Collect the BuiltIn decorations we want to lower first; mutating while
                // iterating annotations invalidates the range.
                struct LoweredVariable {
                    Instruction* variable = nullptr;
                    Instruction* decoration = nullptr;
                    const char* name = nullptr;
                };
                std::vector<LoweredVariable> targets;

                for (auto& annotation : irContext->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate ||
                        static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn) {
                        continue;
                    }

                    const auto builtin = static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2));
                    const char* loweredName = LoweredNameForBuiltin(builtin);
                    if (loweredName == nullptr) {
                        continue;
                    }

                    Instruction* variable = defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                    if (variable == nullptr || variable->opcode() != spv::Op::OpVariable ||
                        static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) !=
                            spv::StorageClass::Input) {
                        continue;
                    }

                    targets.push_back({variable, &annotation, loweredName});
                }

                if (targets.empty()) {
                    return Status::SuccessWithoutChange;
                }

                auto* typeMgr = irContext->get_type_mgr();

                for (auto& target : targets) {
                    Instruction* variable = target.variable;
                    const uint32_t variableId = variable->result_id();

                    // Demote the Input builtin to a plain Private global.
                    Instruction* pointerType = defUseMgr->GetDef(variable->type_id());
                    const uint32_t pointeeTypeId = pointerType->GetSingleWordInOperand(1);
                    const uint32_t privatePointerTypeId =
                        typeMgr->FindPointerToType(pointeeTypeId, spv::StorageClass::Private);
                    variable->SetResultType(privatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<uint32_t>(spv::StorageClass::Private)});

                    irContext->KillInst(target.decoration);
                    RemoveFromEntryPointInterfaces(irContext, variableId);
                    ReplaceName(irContext, variableId, target.name);
                }

                // The DrawParameters capability only covered these builtins; it must not leak
                // into the ESSL decompile.
                std::vector<Instruction*> deadModuleInsts;
                for (auto& capability : irContext->module()->capabilities()) {
                    if (static_cast<spv::Capability>(capability.GetSingleWordInOperand(0)) ==
                        spv::Capability::DrawParameters) {
                        deadModuleInsts.push_back(&capability);
                    }
                }
                for (auto& extension : irContext->module()->extensions()) {
                    if (extension.GetInOperand(0).AsString() == "SPV_KHR_shader_draw_parameters") {
                        deadModuleInsts.push_back(&extension);
                    }
                }
                for (auto* inst : deadModuleInsts) {
                    irContext->KillInst(inst);
                }

                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken LowerDrawParametersPass::CreateLowerDrawParametersPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<LowerDrawParametersPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
