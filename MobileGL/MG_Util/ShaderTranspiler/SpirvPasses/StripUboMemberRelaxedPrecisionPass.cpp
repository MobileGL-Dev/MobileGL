// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUboMemberRelaxedPrecisionPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "StripUboMemberRelaxedPrecisionPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"

#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // Marks `typeId` and every struct type reachable through its members
                // (following arrays) for decoration stripping.
                void CollectStructTypes(IRContext* context, uint32_t typeId,
                                        std::unordered_set<uint32_t>& structTypeIds) {
                    Instruction* typeInst = context->get_def_use_mgr()->GetDef(typeId);
                    if (typeInst == nullptr) return;

                    switch (typeInst->opcode()) {
                    case spv::Op::OpTypeStruct: {
                        if (!structTypeIds.insert(typeId).second) return; // already visited
                        for (uint32_t member = 0; member < typeInst->NumInOperands(); ++member) {
                            CollectStructTypes(context, typeInst->GetSingleWordInOperand(member), structTypeIds);
                        }
                        break;
                    }
                    case spv::Op::OpTypeArray:
                    case spv::Op::OpTypeRuntimeArray:
                        CollectStructTypes(context, typeInst->GetSingleWordInOperand(0), structTypeIds);
                        break;
                    default:
                        break;
                    }
                }
            } // namespace

            spvtools::opt::Pass::Status StripUboMemberRelaxedPrecisionPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Uniform blocks: StorageClass Uniform variables whose pointee struct carries
                // the Block decoration (BufferBlock/StorageBuffer SSBOs are left alone - they
                // are not stage-matched by member precision in this pipeline's ESSL output).
                std::unordered_set<uint32_t> blockStructIds;
                for (Instruction& annotation : irContext->module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate) continue;
                    if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) != spv::Decoration::Block) {
                        continue;
                    }
                    blockStructIds.insert(annotation.GetSingleWordInOperand(0));
                }
                if (blockStructIds.empty()) return Status::SuccessWithoutChange;

                std::unordered_set<uint32_t> structTypeIds;
                for (Instruction& variable : irContext->module()->types_values()) {
                    if (variable.opcode() != spv::Op::OpVariable) continue;
                    if (static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0)) !=
                        spv::StorageClass::Uniform) {
                        continue;
                    }

                    Instruction* pointerType = defUseMgr->GetDef(variable.type_id());
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) continue;
                    uint32_t pointeeId = pointerType->GetSingleWordInOperand(1);

                    // Instance-arrayed blocks: unwrap the array around the block struct.
                    Instruction* pointee = defUseMgr->GetDef(pointeeId);
                    while (pointee != nullptr && (pointee->opcode() == spv::Op::OpTypeArray ||
                                                  pointee->opcode() == spv::Op::OpTypeRuntimeArray)) {
                        pointeeId = pointee->GetSingleWordInOperand(0);
                        pointee = defUseMgr->GetDef(pointeeId);
                    }
                    if (pointee == nullptr || pointee->opcode() != spv::Op::OpTypeStruct) continue;
                    if (blockStructIds.find(pointeeId) == blockStructIds.end()) continue;

                    CollectStructTypes(irContext, pointeeId, structTypeIds);
                }
                if (structTypeIds.empty()) return Status::SuccessWithoutChange;

                std::vector<Instruction*> decorationsToRemove;
                for (Instruction& annotation : irContext->module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpMemberDecorate) continue;
                    if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2)) !=
                        spv::Decoration::RelaxedPrecision) {
                        continue;
                    }
                    if (structTypeIds.find(annotation.GetSingleWordInOperand(0)) == structTypeIds.end()) continue;
                    decorationsToRemove.push_back(&annotation);
                }
                if (decorationsToRemove.empty()) return Status::SuccessWithoutChange;

                for (Instruction* decoration : decorationsToRemove) {
                    irContext->KillInst(decoration);
                }
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<StripUboMemberRelaxedPrecisionPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
