// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerPixelCenterIntegerPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LowerPixelCenterIntegerPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <bit>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // OpExecutionMode <entry-point> <mode> [literals...]
                constexpr uint32_t kExecutionModeModeOperand = 1;

                // Returns the Input OpVariable decorated BuiltIn FragCoord, or nullptr if none.
                Instruction* FindFragCoordVariable(IRContext* context) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn)
                            continue;
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != spv::BuiltIn::FragCoord)
                            continue;
                        Instruction* variable = defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                        if (variable != nullptr && variable->opcode() == spv::Op::OpVariable &&
                            static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Input) {
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

                // One gl_FragCoord read to shift: the load and the id of the constant to subtract.
                struct ShiftedLoad {
                    Instruction* load = nullptr;
                    uint32_t offsetId = 0;
                };
            } // namespace

            spvtools::opt::Pass::Status LowerPixelCenterIntegerPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* constantMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                std::vector<Instruction*> modes;
                for (Instruction& mode : irContext->module()->execution_modes()) {
                    if (mode.opcode() == spv::Op::OpExecutionMode &&
                        mode.NumInOperands() > kExecutionModeModeOperand &&
                        static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(kExecutionModeModeOperand)) ==
                            spv::ExecutionMode::PixelCenterInteger) {
                        modes.push_back(&mode);
                    }
                }
                if (modes.empty()) return Status::SuccessWithoutChange;

                // Gather every read and its offset constant before mutating anything, so a shape
                // the pass refuses leaves the module exactly as it was.
                std::vector<ShiftedLoad> loads;
                Bool refused = false;
                const auto scalarOffset = [&](uint32_t floatTypeId, float value) -> uint32_t {
                    const auto* floatType = typeMgr->GetType(floatTypeId);
                    if (floatType == nullptr || floatType->AsFloat() == nullptr) return 0;
                    const auto* constant = constantMgr->GetConstant(floatType, {std::bit_cast<uint32_t>(value)});
                    const Instruction* inst = constant ? constantMgr->GetDefiningInstruction(constant, floatTypeId) : nullptr;
                    return inst ? inst->result_id() : 0;
                };

                if (Instruction* fragCoord = FindFragCoordVariable(irContext)) {
                    const uint32_t fragCoordId = fragCoord->result_id();
                    defUseMgr->ForEachUser(fragCoord, [&](Instruction* user) {
                        if (refused || IsDeclarationUse(user)) return;
                        if (user->opcode() == spv::Op::OpLoad && user->GetSingleWordInOperand(0) == fragCoordId) {
                            // The whole vec4: subtract (0.5, 0.5, 0, 0).
                            const auto* vecType = typeMgr->GetType(user->type_id());
                            const auto* vector = vecType ? vecType->AsVector() : nullptr;
                            if (vector == nullptr || vector->element_count() != 4) {
                                refused = true;
                                return;
                            }
                            const uint32_t floatTypeId = typeMgr->GetId(vector->element_type());
                            const uint32_t half = scalarOffset(floatTypeId, 0.5f);
                            const uint32_t zero = scalarOffset(floatTypeId, 0.0f);
                            const auto* composite =
                                (half && zero) ? constantMgr->GetConstant(vecType, {half, half, zero, zero}) : nullptr;
                            const Instruction* inst =
                                composite ? constantMgr->GetDefiningInstruction(composite, user->type_id()) : nullptr;
                            if (inst == nullptr) {
                                refused = true;
                                return;
                            }
                            loads.push_back({user, inst->result_id()});
                            return;
                        }
                        if ((user->opcode() == spv::Op::OpAccessChain ||
                             user->opcode() == spv::Op::OpInBoundsAccessChain) &&
                            user->NumInOperands() == 2 && user->GetSingleWordInOperand(0) == fragCoordId) {
                            const auto* index = constantMgr->FindDeclaredConstant(user->GetSingleWordInOperand(1));
                            if (index == nullptr || index->AsScalarConstant() == nullptr) {
                                refused = true;
                                return;
                            }
                            const Uint64 component = index->GetZeroExtendedValue();
                            defUseMgr->ForEachUser(user, [&](Instruction* chainUser) {
                                if (refused || IsDeclarationUse(chainUser)) return;
                                if (chainUser->opcode() != spv::Op::OpLoad) {
                                    refused = true;
                                    return;
                                }
                                // z and w keep their values; only the centre of x and y moves.
                                if (component >= 2) return;
                                const uint32_t half = scalarOffset(chainUser->type_id(), 0.5f);
                                if (half == 0) {
                                    refused = true;
                                    return;
                                }
                                loads.push_back({chainUser, half});
                            });
                            return;
                        }
                        refused = true;
                    });
                }
                if (refused) return Status::Failure;

                for (Instruction* mode : modes) irContext->KillInst(mode);

                // `%r = OpLoad %ty %ptr` becomes `%fresh = OpLoad %ty %ptr` followed by
                // `%r = OpFSub %ty %fresh %offset`: every existing use of %r picks the shifted
                // value up without being rewritten.
                for (const ShiftedLoad& shifted : loads) {
                    const uint32_t freshId = TakeNextId();
                    if (freshId == 0) return Status::Failure;
                    const uint32_t resultId = shifted.load->result_id();
                    const uint32_t typeId = shifted.load->type_id();
                    // A load is never the last instruction of its block (a terminator follows),
                    // so inserting before its successor places the subtraction right after it.
                    Instruction* next = shifted.load->NextNode();
                    if (next == nullptr) return Status::Failure;
                    shifted.load->SetResultId(freshId);
                    next->InsertBefore(MakeUnique<Instruction>(
                        irContext, spv::Op::OpFSub, typeId, resultId,
                        Instruction::OperandList{{SPV_OPERAND_TYPE_ID, {freshId}},
                                                 {SPV_OPERAND_TYPE_ID, {shifted.offsetId}}}));
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken LowerPixelCenterIntegerPass::CreateLowerPixelCenterIntegerPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<LowerPixelCenterIntegerPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
