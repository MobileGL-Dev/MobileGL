// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FullCoverageSampleMaskPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FullCoverageSampleMaskPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // Returns the Output OpVariable decorated BuiltIn SampleMask, or nullptr if none.
                Instruction* FindSampleMaskOutputVariable(IRContext* context) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn)
                            continue;
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != spv::BuiltIn::SampleMask)
                            continue;
                        Instruction* variable = defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                        if (variable != nullptr && variable->opcode() == spv::Op::OpVariable &&
                            static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Output) {
                            return variable;
                        }
                    }
                    return nullptr;
                }

                Bool IsDeclarationUse(const Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                    case spv::Op::OpDecorateString:
                    case spv::Op::OpName:
                    case spv::Op::OpEntryPoint:
                        return true;
                    default:
                        return false;
                    }
                }
            } // namespace

            spvtools::opt::Pass::Status FullCoverageSampleMaskPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* constantMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                Instruction* sampleMask = FindSampleMaskOutputVariable(irContext);
                if (sampleMask == nullptr) return Status::SuccessWithoutChange;

                // Gather the stores before mutating anything, so a shape the pass refuses leaves
                // the module exactly as it was. Whole-array stores and element stores are kept
                // apart: they need an all-ones array and an all-ones element respectively.
                const uint32_t sampleMaskId = sampleMask->result_id();
                std::vector<Instruction*> arrayStores;
                std::vector<Instruction*> elementStores;
                Bool refused = false;
                defUseMgr->ForEachUser(sampleMask, [&](Instruction* user) {
                    if (refused || IsDeclarationUse(user)) return;
                    if (user->opcode() == spv::Op::OpStore && user->GetSingleWordInOperand(0) == sampleMaskId) {
                        arrayStores.push_back(user);
                        return;
                    }
                    if ((user->opcode() == spv::Op::OpAccessChain ||
                         user->opcode() == spv::Op::OpInBoundsAccessChain) &&
                        user->NumInOperands() == 2) {
                        const uint32_t chainId = user->result_id();
                        defUseMgr->ForEachUser(user, [&](Instruction* chainUser) {
                            if (refused || IsDeclarationUse(chainUser)) return;
                            if (chainUser->opcode() == spv::Op::OpStore &&
                                chainUser->GetSingleWordInOperand(0) == chainId) {
                                elementStores.push_back(chainUser);
                                return;
                            }
                            refused = true;
                        });
                        return;
                    }
                    refused = true;
                });
                if (refused) return Status::Failure;
                if (arrayStores.empty() && elementStores.empty()) return Status::SuccessWithoutChange;

                const auto* pointerType = typeMgr->GetType(sampleMask->type_id());
                const auto* pointer = pointerType ? pointerType->AsPointer() : nullptr;
                const auto* arrayType = pointer ? pointer->pointee_type() : nullptr;
                const auto* array = arrayType ? arrayType->AsArray() : nullptr;
                const auto* elementType = array ? array->element_type() : nullptr;
                const auto* integer = elementType ? elementType->AsInteger() : nullptr;
                if (integer == nullptr || integer->width() != 32) return Status::Failure;

                const uint32_t elementTypeId = typeMgr->GetId(elementType);
                const auto* allOnes = constantMgr->GetConstant(elementType, {0xffffffffu});
                const Instruction* allOnesInst =
                    allOnes ? constantMgr->GetDefiningInstruction(allOnes, elementTypeId) : nullptr;
                if (elementTypeId == 0 || allOnesInst == nullptr) return Status::Failure;
                for (Instruction* store : elementStores)
                    store->SetInOperand(1, {allOnesInst->result_id()});

                if (!arrayStores.empty()) {
                    // The array's length is a constant (the builtin is sized at link time); a
                    // specialization-constant length is not something this pass can fill.
                    const auto& lengthInfo = array->length_info();
                    if (lengthInfo.words.size() != 2 ||
                        lengthInfo.words[0] != spvtools::opt::analysis::Array::LengthInfo::kConstant) {
                        return Status::Failure;
                    }
                    const uint32_t length = lengthInfo.words[1];
                    const uint32_t arrayTypeId = typeMgr->GetId(arrayType);
                    std::vector<uint32_t> elementIds(length, allOnesInst->result_id());
                    const auto* allOnesArray = constantMgr->GetConstant(arrayType, elementIds);
                    const Instruction* allOnesArrayInst =
                        allOnesArray ? constantMgr->GetDefiningInstruction(allOnesArray, arrayTypeId) : nullptr;
                    if (arrayTypeId == 0 || allOnesArrayInst == nullptr) return Status::Failure;
                    for (Instruction* store : arrayStores)
                        store->SetInOperand(1, {allOnesArrayInst->result_id()});
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken FullCoverageSampleMaskPass::CreateFullCoverageSampleMaskPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<FullCoverageSampleMaskPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
