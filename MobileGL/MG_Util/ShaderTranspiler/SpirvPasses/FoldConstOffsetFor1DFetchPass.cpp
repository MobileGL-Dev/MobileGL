// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FoldConstOffsetFor1DFetchPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FoldConstOffsetFor1DFetchPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                // Number of ImageOperands ids that precede the ConstOffset id: one per
                // lower-order bit set in the mask, except Grad which carries two ids.
                uint32_t CountIdsBeforeConstOffset(uint32_t mask) {
                    uint32_t count = 0;
                    if (mask & static_cast<uint32_t>(spv::ImageOperandsMask::Bias)) count += 1;
                    if (mask & static_cast<uint32_t>(spv::ImageOperandsMask::Lod)) count += 1;
                    if (mask & static_cast<uint32_t>(spv::ImageOperandsMask::Grad)) count += 2;
                    return count;
                }
            } // namespace

            spvtools::opt::Pass::Status FoldConstOffsetFor1DFetchPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                Bool modified = false;

                constexpr uint32_t kConstOffsetBit =
                    static_cast<uint32_t>(spv::ImageOperandsMask::ConstOffset);

                for (auto& function : *get_module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            if (inst.opcode() != spv::Op::OpImageFetch) continue;
                            // In-operands: image, coordinate, [ImageOperands mask, ids...].
                            if (inst.NumInOperands() < 3) continue;
                            const uint32_t operandsMask = inst.GetSingleWordInOperand(2);
                            if ((operandsMask & kConstOffsetBit) == 0) continue;

                            Instruction* imageInst = defUseMgr->GetDef(inst.GetSingleWordInOperand(0));
                            if (imageInst == nullptr) continue;
                            Instruction* imageType = defUseMgr->GetDef(imageInst->type_id());
                            if (imageType == nullptr || imageType->opcode() != spv::Op::OpTypeImage ||
                                static_cast<spv::Dim>(imageType->GetSingleWordInOperand(1)) != spv::Dim::Dim1D) {
                                continue;
                            }

                            const uint32_t offsetOperandIndex = 3 + CountIdsBeforeConstOffset(operandsMask);
                            const uint32_t offsetId = inst.GetSingleWordInOperand(offsetOperandIndex);

                            const uint32_t coordId = inst.GetSingleWordInOperand(1);
                            Instruction* coordType = defUseMgr->GetDef(defUseMgr->GetDef(coordId)->type_id());

                            InstructionBuilder builder(
                                irContext, &inst,
                                IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                            uint32_t newCoordId = 0;
                            if (coordType->opcode() == spv::Op::OpTypeVector) {
                                // Arrayed 1D fetch: component 0 is the texel coordinate,
                                // component 1 the layer - only component 0 takes the offset.
                                const uint32_t componentTypeId = coordType->GetSingleWordInOperand(0);
                                Instruction* extracted = builder.AddCompositeExtract(componentTypeId, coordId, {0});
                                Instruction* sum =
                                    builder.AddIAdd(componentTypeId, extracted->result_id(), offsetId);
                                Instruction* inserted = builder.AddInstruction(spvtools::MakeUnique<Instruction>(
                                    irContext, spv::Op::OpCompositeInsert, coordType->result_id(),
                                    irContext->TakeNextId(),
                                    std::initializer_list<Operand>{
                                        {SPV_OPERAND_TYPE_ID, {sum->result_id()}},
                                        {SPV_OPERAND_TYPE_ID, {coordId}},
                                        {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0}}}));
                                newCoordId = inserted->result_id();
                            } else {
                                Instruction* sum = builder.AddIAdd(coordType->result_id(), coordId, offsetId);
                                newCoordId = sum->result_id();
                            }

                            const uint32_t newMask = operandsMask & ~kConstOffsetBit;
                            // 3 fixed operands + the offset id: anything beyond that is another
                            // image-operand id that must keep the mask word alive.
                            const Bool otherOperandIdsRemain = inst.NumInOperands() > 4;

                            irContext->ForgetUses(&inst);
                            std::vector<Operand> newOperands;
                            newOperands.push_back(inst.GetInOperand(0));
                            newOperands.push_back({SPV_OPERAND_TYPE_ID, {newCoordId}});
                            if (newMask != 0 || otherOperandIdsRemain) {
                                Operand maskOperand = inst.GetInOperand(2);
                                maskOperand.words[0] = newMask;
                                newOperands.push_back(maskOperand);
                                for (uint32_t i = 3; i < inst.NumInOperands(); ++i) {
                                    if (i == offsetOperandIndex) continue;
                                    newOperands.push_back(inst.GetInOperand(i));
                                }
                            }
                            inst.SetInOperands(std::move(newOperands));
                            irContext->AnalyzeUses(&inst);

                            modified = true;
                        }
                    }
                }

                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken FoldConstOffsetFor1DFetchPass::CreateFoldConstOffsetFor1DFetchPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<FoldConstOffsetFor1DFetchPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
