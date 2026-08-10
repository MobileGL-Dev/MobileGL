// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PrivateToEntryLocalPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Derived from SPIRV-Tools' PrivateToLocalPass (source/opt/private_to_local_pass.cpp,
// Copyright (c) 2017 Google Inc., Apache License 2.0). The one behavioral difference is
// the entry-point restriction in FindEntryLocalFunction; see the header for why.

#include "PrivateToEntryLocalPass.h"

#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"
#include "source/spirv_constant.h"
#include "source/util/make_unique.h"

#include <cassert>
#include <utility>
#include <vector>
#include <unordered_set>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::BasicBlock;
                using spvtools::opt::Function;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;

                constexpr uint32_t kVariableStorageClassInIdx = 0;
                constexpr uint32_t kSpvTypePointerTypeIdInIdx = 1;
            } // namespace

            spvtools::opt::Pass::Status PrivateToEntryLocalPass::Process() {
                // Private variables require the Shader capability; with Addresses the
                // rewrite below is not guaranteed sound (variable pointers may escape).
                if (context()->get_feature_mgr()->HasCapability(spv::Capability::Addresses)) {
                    return Status::SuccessWithoutChange;
                }

                std::vector<std::pair<Instruction*, Function*>> variablesToMove;
                std::unordered_set<uint32_t> localizedVariables;
                for (auto& inst : context()->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) {
                        continue;
                    }
                    if (spv::StorageClass(inst.GetSingleWordInOperand(kVariableStorageClassInIdx)) !=
                        spv::StorageClass::Private) {
                        continue;
                    }
                    Function* targetFunction = FindEntryLocalFunction(inst);
                    if (targetFunction != nullptr) {
                        variablesToMove.push_back({&inst, targetFunction});
                    }
                }

                const bool modified = !variablesToMove.empty();
                for (auto& p : variablesToMove) {
                    if (!MoveVariable(p.first, p.second)) {
                        return Status::Failure;
                    }
                    localizedVariables.insert(p.first->result_id());
                }

                if (get_module()->version() >= SPV_SPIRV_VERSION_WORD(1, 4)) {
                    // SPIR-V 1.4+ lists statically-used Private variables on OpEntryPoint;
                    // drop the ones that just stopped being Private. Dead code for the 1.3
                    // modules MobileGL emits, kept for robustness.
                    for (auto& entry : get_module()->entry_points()) {
                        std::vector<Operand> newOperands;
                        for (uint32_t i = 0; i < entry.NumInOperands(); ++i) {
                            // Execution model, function id and name are always kept.
                            if (i < 3 || !localizedVariables.count(entry.GetSingleWordInOperand(i))) {
                                newOperands.push_back(entry.GetInOperand(i));
                            }
                        }
                        if (newOperands.size() != entry.NumInOperands()) {
                            entry.SetInOperands(std::move(newOperands));
                            context()->AnalyzeUses(&entry);
                        }
                    }
                }

                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            Function* PrivateToEntryLocalPass::FindEntryLocalFunction(const Instruction& inst) const {
                bool foundFirstUse = false;
                Function* targetFunction = nullptr;
                const uint32_t variableId = inst.result_id();
                context()->get_def_use_mgr()->ForEachUser(
                    variableId, [&targetFunction, &foundFirstUse, variableId, this](Instruction* use) {
                        BasicBlock* currentBlock = context()->get_instr_block(use);
                        if (currentBlock == nullptr) {
                            // Module-scope users: OpName, decorations, the OpEntryPoint
                            // interface list. None of them pins the variable to a function,
                            // but a debug-info extended instruction would go stale after the
                            // move, so treat it as disqualifying.
                            if (use->opcode() == spv::Op::OpExtInst) {
                                foundFirstUse = true;
                                targetFunction = nullptr;
                            }
                            return;
                        }
                        if (!IsValidUse(use, variableId)) {
                            foundFirstUse = true;
                            targetFunction = nullptr;
                            return;
                        }
                        Function* currentFunction = currentBlock->GetParent();
                        if (!foundFirstUse) {
                            foundFirstUse = true;
                            targetFunction = currentFunction;
                        } else if (targetFunction != currentFunction) {
                            targetFunction = nullptr;
                        }
                    });
                if (targetFunction != nullptr && !IsEntryPointFunction(targetFunction)) {
                    // The whole point of this derivative: a helper can be called more than
                    // once per invocation, and Function storage would reset the variable at
                    // every call.
                    return nullptr;
                }
                return targetFunction;
            }

            bool PrivateToEntryLocalPass::IsEntryPointFunction(Function* function) const {
                for (auto& entry : get_module()->entry_points()) {
                    if (entry.GetSingleWordInOperand(1) == function->result_id()) {
                        return true;
                    }
                }
                return false;
            }

            bool PrivateToEntryLocalPass::IsValidUse(const Instruction* inst, uint32_t variableId) const {
                // The cases here have to match the cases in UpdateUse: a use the rewrite
                // does not know how to update disqualifies the variable.
                switch (inst->opcode()) {
                    case spv::Op::OpLoad:
                    case spv::Op::OpImageTexelPointer: // treat like a load
                        return true;
                    case spv::Op::OpStore:
                        // Storing the variable's ADDRESS somewhere else escapes it.
                        return inst->GetOperand(1).AsId() != variableId;
                    case spv::Op::OpAccessChain:
                        return context()->get_def_use_mgr()->WhileEachUser(
                            inst, [this, inst](const Instruction* user) {
                                return IsValidUse(user, inst->result_id());
                            });
                    case spv::Op::OpName:
                        return true;
                    default:
                        return spvOpcodeIsDecoration(inst->opcode());
                }
            }

            bool PrivateToEntryLocalPass::MoveVariable(Instruction* variable, Function* function) {
                // Remove from the global section and re-insert at the head of the entry
                // function's first block, Function-storage variables' one legal position.
                variable->RemoveFromList();
                std::unique_ptr<Instruction> var(variable); // take ownership
                context()->ForgetUses(variable);

                variable->SetInOperand(kVariableStorageClassInIdx,
                                       {uint32_t(spv::StorageClass::Function)});

                const uint32_t newTypeId = GetNewType(variable->type_id());
                if (newTypeId == 0) {
                    return false;
                }
                variable->SetResultType(newTypeId);

                context()->AnalyzeUses(variable);
                context()->set_instr_block(variable, &*function->begin());
                function->begin()->begin()->InsertBefore(std::move(var));

                return UpdateUses(variable);
            }

            uint32_t PrivateToEntryLocalPass::GetNewType(uint32_t oldTypeId) {
                auto* typeMgr = context()->get_type_mgr();
                Instruction* oldTypeInst = get_def_use_mgr()->GetDef(oldTypeId);
                const uint32_t pointeeTypeId =
                    oldTypeInst->GetSingleWordInOperand(kSpvTypePointerTypeIdInIdx);
                const uint32_t newTypeId =
                    typeMgr->FindPointerToType(pointeeTypeId, spv::StorageClass::Function);
                if (newTypeId != 0) {
                    context()->UpdateDefUse(context()->get_def_use_mgr()->GetDef(newTypeId));
                }
                return newTypeId;
            }

            bool PrivateToEntryLocalPass::UpdateUse(Instruction* inst, Instruction* user) {
                // The cases here have to match the cases in IsValidUse.
                switch (inst->opcode()) {
                    case spv::Op::OpLoad:
                    case spv::Op::OpStore:
                    case spv::Op::OpImageTexelPointer: // treat like a load
                        // Fine as-is: their type is the pointed-to type, which is unchanged.
                        break;
                    case spv::Op::OpAccessChain: {
                        context()->ForgetUses(inst);
                        const uint32_t newTypeId = GetNewType(inst->type_id());
                        if (newTypeId == 0) {
                            return false;
                        }
                        inst->SetResultType(newTypeId);
                        context()->AnalyzeUses(inst);
                        if (!UpdateUses(inst)) {
                            return false;
                        }
                        break;
                    }
                    case spv::Op::OpName:
                    case spv::Op::OpEntryPoint: // handled separately in Process()
                        break;
                    default:
                        assert(spvOpcodeIsDecoration(inst->opcode()) &&
                               "PrivateToEntryLocalPass: unexpected use opcode");
                        break;
                }
                (void)user;
                return true;
            }

            bool PrivateToEntryLocalPass::UpdateUses(Instruction* inst) {
                const uint32_t id = inst->result_id();
                std::vector<Instruction*> uses;
                context()->get_def_use_mgr()->ForEachUser(id,
                                                          [&uses](Instruction* use) { uses.push_back(use); });
                for (Instruction* use : uses) {
                    if (!UpdateUse(use, inst)) {
                        return false;
                    }
                }
                return true;
            }

            spvtools::Optimizer::PassToken PrivateToEntryLocalPass::CreatePrivateToEntryLocalPass() {
                return spvtools::Optimizer::PassToken(
                    spvtools::MakeUnique<PrivateToEntryLocalPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
