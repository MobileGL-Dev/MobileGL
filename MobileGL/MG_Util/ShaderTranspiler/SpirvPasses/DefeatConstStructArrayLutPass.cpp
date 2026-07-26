// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DefeatConstStructArrayLutPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DefeatConstStructArrayLutPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::BasicBlock;
                using spvtools::opt::Function;
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                uint32_t PointerTypeTo(IRContext* ctx, uint32_t pointeeId, spv::StorageClass sc) {
                    analysis::Type* pointee = ctx->get_type_mgr()->GetType(pointeeId);
                    analysis::Pointer ptr(pointee, sc);
                    return ctx->get_type_mgr()->GetTypeInstruction(&ptr);
                }

                uint32_t SignedIntConstant(IRContext* ctx, uint32_t value) {
                    analysis::Integer i(32, true);
                    analysis::Type* reg = ctx->get_type_mgr()->GetRegisteredType(&i);
                    const analysis::Constant* c = ctx->get_constant_mgr()->GetConstant(reg, {value});
                    return ctx->get_constant_mgr()->GetDefiningInstruction(c)->result_id();
                }

                // True when |var| (a Function-storage OpVariable) points to an array of structs.
                // Reports the struct type id on success.
                bool IsArrayOfStructsVariable(IRContext* ctx, Instruction* var, uint32_t& structTypeId) {
                    auto* defUse = ctx->get_def_use_mgr();
                    Instruction* ptrType = defUse->GetDef(var->type_id());
                    if (ptrType == nullptr || ptrType->opcode() != spv::Op::OpTypePointer) return false;
                    Instruction* pointee = defUse->GetDef(ptrType->GetSingleWordInOperand(1));
                    if (pointee == nullptr || pointee->opcode() != spv::Op::OpTypeArray) return false;
                    Instruction* element = defUse->GetDef(pointee->GetSingleWordInOperand(0));
                    if (element == nullptr || element->opcode() != spv::Op::OpTypeStruct) return false;
                    structTypeId = element->result_id();
                    return true;
                }
            } // namespace

            spvtools::opt::Pass::Status DefeatConstStructArrayLutPass::Process() {
                auto* ctx = context();
                auto* defUse = ctx->get_def_use_mgr();
                bool modified = false;

                for (Function& function : *get_module()) {
                    if (function.begin() == function.end()) continue;
                    BasicBlock* entryBlock = &*function.begin();

                    // Candidate variables: Function-storage arrays of structs declared in this
                    // function's entry block (where OpVariables must live).
                    struct Candidate {
                        Instruction* var;
                        uint32_t structTypeId;
                    };
                    std::vector<Candidate> candidates;
                    for (Instruction& inst : *entryBlock) {
                        if (inst.opcode() != spv::Op::OpVariable) break;
                        // Variables with initializers keep SPIRV-Cross's initializer path; the
                        // glslang pattern under attack is initializer-free with one OpStore.
                        if (inst.NumInOperands() > 1) continue;
                        uint32_t structTypeId = 0;
                        if (IsArrayOfStructsVariable(ctx, &inst, structTypeId)) {
                            candidates.push_back({&inst, structTypeId});
                        }
                    }

                    for (const Candidate& candidate : candidates) {
                        Instruction* var = candidate.var;

                        // The variable qualifies only when its single write is one direct
                        // OpStore of an OpConstantComposite; any other write shape already
                        // defeats SPIRV-Cross's LUT promotion, so it is left untouched.
                        Instruction* singleStore = nullptr;
                        bool disqualified = false;
                        defUse->ForEachUser(var, [&](Instruction* user) {
                            if (user->opcode() == spv::Op::OpStore &&
                                user->GetSingleWordInOperand(0) == var->result_id()) {
                                if (singleStore != nullptr) {
                                    disqualified = true;
                                } else {
                                    singleStore = user;
                                }
                            } else if (user->opcode() == spv::Op::OpCopyMemory) {
                                disqualified = true;
                            } else if (user->opcode() == spv::Op::OpAccessChain ||
                                       user->opcode() == spv::Op::OpInBoundsAccessChain) {
                                defUse->ForEachUser(user, [&](Instruction* chainUser) {
                                    if (chainUser->opcode() == spv::Op::OpStore ||
                                        chainUser->opcode() == spv::Op::OpCopyMemory) {
                                        disqualified = true;
                                    }
                                });
                            }
                        });
                        if (disqualified || singleStore == nullptr) continue;

                        Instruction* composite = defUse->GetDef(singleStore->GetSingleWordInOperand(1));
                        if (composite == nullptr ||
                            composite->opcode() != spv::Op::OpConstantComposite) {
                            continue;
                        }

                        // The store must sit in the entry block: that is the only placement
                        // SPIRV-Cross treats as a LUT initializer.
                        bool storeInEntryBlock = false;
                        for (Instruction& inst : *entryBlock) {
                            if (&inst == singleStore) {
                                storeInEntryBlock = true;
                                break;
                            }
                        }
                        if (!storeInEntryBlock) continue;

                        // Split the composite store into one constant-index store per element.
                        const uint32_t ptrFnStruct =
                            PointerTypeTo(ctx, candidate.structTypeId, spv::StorageClass::Function);
                        for (uint32_t element = 0; element < composite->NumInOperands(); ++element) {
                            const uint32_t elementConstId = composite->GetSingleWordInOperand(element);
                            const uint32_t chainId = ctx->TakeNextId();
                            Instruction* chain =
                                singleStore->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpAccessChain, ptrFnStruct, chainId,
                                    std::initializer_list<Operand>{
                                        {SPV_OPERAND_TYPE_ID, {var->result_id()}},
                                        {SPV_OPERAND_TYPE_ID, {SignedIntConstant(ctx, element)}}}));
                            ctx->AnalyzeDefUse(chain);
                            Instruction* store =
                                singleStore->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpStore, 0, 0,
                                    std::initializer_list<Operand>{
                                        {SPV_OPERAND_TYPE_ID, {chainId}},
                                        {SPV_OPERAND_TYPE_ID, {elementConstId}}}));
                            ctx->AnalyzeDefUse(store);
                        }
                        ctx->KillInst(singleStore);
                        modified = true;
                    }
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }
                ctx->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            DefeatConstStructArrayLutPass::CreateDefeatConstStructArrayLutPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<DefeatConstStructArrayLutPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
