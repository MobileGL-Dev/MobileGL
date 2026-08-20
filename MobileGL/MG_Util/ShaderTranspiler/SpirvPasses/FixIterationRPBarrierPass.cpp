// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPBarrierPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FixIterationRPBarrierPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL::MG_Util::ShaderTranspiler {
    namespace {
        using spvtools::opt::Instruction;
        using spvtools::opt::IRContext;
        using spvtools::opt::Operand;

        const Instruction* RootVariable(IRContext* context, uint32_t pointerId) {
            const Instruction* def = context->get_def_use_mgr()->GetDef(pointerId);
            while (def != nullptr) {
                switch (def->opcode()) {
                case spv::Op::OpVariable:
                    return def;
                case spv::Op::OpAccessChain:
                case spv::Op::OpInBoundsAccessChain:
                case spv::Op::OpCopyObject:
                    def = context->get_def_use_mgr()->GetDef(def->GetSingleWordInOperand(0));
                    break;
                default:
                    return nullptr;
                }
            }
            return nullptr;
        }

        bool IsUintConstant(IRContext* context, uint32_t id, uint32_t wanted) {
            const Instruction* def = context->get_def_use_mgr()->GetDef(id);
            return def != nullptr && def->opcode() == spv::Op::OpConstant && def->NumInOperands() == 1u &&
                   def->GetSingleWordInOperand(0) == wanted;
        }

        bool IsZeroElementPointer(IRContext* context, uint32_t pointerId, const Instruction** root) {
            const Instruction* pointer = context->get_def_use_mgr()->GetDef(pointerId);
            if (pointer == nullptr ||
                (pointer->opcode() != spv::Op::OpAccessChain && pointer->opcode() != spv::Op::OpInBoundsAccessChain) ||
                pointer->NumInOperands() < 2u) {
                return false;
            }
            for (uint32_t i = 1u; i < pointer->NumInOperands(); ++i) {
                if (!IsUintConstant(context, pointer->GetSingleWordInOperand(i), 0u)) return false;
            }
            *root = RootVariable(context, pointerId);
            return *root != nullptr;
        }

        bool IsWorkgroupVec2Array(IRContext* context, const Instruction* variable) {
            if (variable == nullptr || variable->opcode() != spv::Op::OpVariable || variable->NumInOperands() < 1u ||
                static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) != spv::StorageClass::Workgroup) {
                return false;
            }
            auto* defUseMgr = context->get_def_use_mgr();
            const Instruction* pointerType = defUseMgr->GetDef(variable->type_id());
            if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer ||
                pointerType->NumInOperands() < 2u) {
                return false;
            }
            const Instruction* arrayType = defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
            if (arrayType == nullptr || arrayType->opcode() != spv::Op::OpTypeArray ||
                arrayType->NumInOperands() < 2u) {
                return false;
            }
            const Instruction* length = defUseMgr->GetDef(arrayType->GetSingleWordInOperand(1));
            if (length == nullptr || length->opcode() != spv::Op::OpConstant || length->NumInOperands() != 1u) {
                return false;
            }
            const uint32_t arrayLength = length->GetSingleWordInOperand(0);
            if (arrayLength < 32u || arrayLength > 512u) return false;

            const Instruction* vectorType = defUseMgr->GetDef(arrayType->GetSingleWordInOperand(0));
            if (vectorType == nullptr || vectorType->opcode() != spv::Op::OpTypeVector ||
                vectorType->NumInOperands() < 2u || vectorType->GetSingleWordInOperand(1) != 2u) {
                return false;
            }
            const Instruction* scalarType = defUseMgr->GetDef(vectorType->GetSingleWordInOperand(0));
            return scalarType != nullptr && scalarType->opcode() == spv::Op::OpTypeFloat &&
                   scalarType->NumInOperands() == 1u && scalarType->GetSingleWordInOperand(0) == 32u;
        }

        bool IsVec2FloatInclusiveAdd(IRContext* context, const Instruction* inst) {
            if (inst->opcode() != spv::Op::OpGroupNonUniformFAdd || inst->NumInOperands() < 3u ||
                static_cast<spv::GroupOperation>(inst->GetSingleWordInOperand(1)) !=
                    spv::GroupOperation::InclusiveScan) {
                return false;
            }
            const Instruction* vectorType = context->get_def_use_mgr()->GetDef(inst->type_id());
            if (vectorType == nullptr || vectorType->opcode() != spv::Op::OpTypeVector ||
                vectorType->NumInOperands() < 2u || vectorType->GetSingleWordInOperand(1) != 2u) {
                return false;
            }
            const Instruction* scalarType = context->get_def_use_mgr()->GetDef(vectorType->GetSingleWordInOperand(0));
            return scalarType != nullptr && scalarType->opcode() == spv::Op::OpTypeFloat &&
                   scalarType->NumInOperands() == 1u && scalarType->GetSingleWordInOperand(0) == 32u;
        }

        bool HasProgram203LocalSize(IRContext* context) {
            for (const Instruction& entryPoint : context->module()->entry_points()) {
                if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) !=
                    spv::ExecutionModel::GLCompute) {
                    return false;
                }
            }
            for (const Instruction& mode : context->module()->execution_modes()) {
                if (mode.opcode() == spv::Op::OpExecutionMode && mode.NumInOperands() >= 5u &&
                    static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(1)) == spv::ExecutionMode::LocalSize) {
                    return mode.GetSingleWordInOperand(2) == 32u && mode.GetSingleWordInOperand(3) == 16u &&
                           mode.GetSingleWordInOperand(4) == 1u;
                }
            }
            return false;
        }

        bool IsStoreToRoot(IRContext* context, const Instruction* inst, const Instruction* root) {
            return inst->opcode() == spv::Op::OpStore && inst->NumInOperands() >= 2u &&
                   RootVariable(context, inst->GetSingleWordInOperand(0)) == root;
        }
    } // namespace

    spvtools::opt::Pass::Status FixIterationRPBarrierPass::Process() {
        auto* irContext = context();
        if (!HasProgram203LocalSize(irContext)) return Status::SuccessWithoutChange;

        for (auto& function : *irContext->module()) {
            std::vector<Instruction*> instructions;
            std::vector<size_t> scans;
            for (auto& block : function) {
                for (auto& inst : block) {
                    if (IsVec2FloatInclusiveAdd(irContext, &inst)) scans.push_back(instructions.size());
                    instructions.push_back(&inst);
                }
            }
            // Program 203 has exactly two vec2 inclusive adds: the luminance reduction
            // and the weighted-exposure reduction. More or fewer is not our fingerprint.
            if (scans.size() != 2u) continue;

            const size_t firstScan = scans[0];
            const size_t secondScan = scans[1];
            const Instruction* scratch = nullptr;
            size_t averageLoad = instructions.size();

            for (size_t i = firstScan + 1u; i < secondScan; ++i) {
                Instruction* inst = instructions[i];
                if (inst->opcode() != spv::Op::OpLoad || inst->NumInOperands() < 1u) continue;
                const Instruction* root = nullptr;
                if (!IsZeroElementPointer(irContext, inst->GetSingleWordInOperand(0), &root) ||
                    !IsWorkgroupVec2Array(irContext, root)) {
                    continue;
                }
                // The broadcast is read as prefixSumCache[0].x, hence a scalar load.
                const Instruction* type = irContext->get_def_use_mgr()->GetDef(inst->type_id());
                if (type == nullptr || type->opcode() != spv::Op::OpTypeFloat || type->NumInOperands() != 1u ||
                    type->GetSingleWordInOperand(0) != 32u) {
                    continue;
                }
                scratch = root;
                averageLoad = i;
                break;
            }
            if (scratch == nullptr) continue;

            bool sawZeroBroadcastStore = false;
            bool sawPublishBarrier = false;
            for (size_t i = firstScan + 1u; i < averageLoad; ++i) {
                const Instruction* root = nullptr;
                if (instructions[i]->opcode() == spv::Op::OpStore &&
                    IsZeroElementPointer(irContext, instructions[i]->GetSingleWordInOperand(0), &root) &&
                    root == scratch) {
                    sawZeroBroadcastStore = true;
                } else if (sawZeroBroadcastStore && instructions[i]->opcode() == spv::Op::OpControlBarrier) {
                    sawPublishBarrier = true;
                }
            }
            if (!sawZeroBroadcastStore || !sawPublishBarrier) continue;

            bool alreadySynchronized = false;
            for (size_t i = averageLoad + 1u; i < secondScan; ++i) {
                if (instructions[i]->opcode() == spv::Op::OpControlBarrier) {
                    alreadySynchronized = true;
                    break;
                }
            }
            if (alreadySynchronized) return Status::SuccessWithoutChange;

            bool secondPhaseReusesScratch = false;
            for (size_t i = secondScan + 1u; i < instructions.size(); ++i) {
                if (IsStoreToRoot(irContext, instructions[i], scratch)) {
                    secondPhaseReusesScratch = true;
                    break;
                }
            }
            if (!secondPhaseReusesScratch) continue;

            auto* constantMgr = irContext->get_constant_mgr();
            const uint32_t scopeId = constantMgr->GetUIntConstId(static_cast<uint32_t>(spv::Scope::Workgroup));
            const uint32_t semanticsId =
                constantMgr->GetUIntConstId(static_cast<uint32_t>(spv::MemorySemanticsMask::AcquireRelease) |
                                            static_cast<uint32_t>(spv::MemorySemanticsMask::WorkgroupMemory));
            if (scopeId == 0u || semanticsId == 0u) return Status::Failure;

            instructions[secondScan]->InsertBefore(spvtools::MakeUnique<Instruction>(
                irContext, spv::Op::OpControlBarrier, 0u, 0u,
                Instruction::OperandList{Operand{SPV_OPERAND_TYPE_ID, {scopeId}},
                                         Operand{SPV_OPERAND_TYPE_ID, {scopeId}},
                                         Operand{SPV_OPERAND_TYPE_ID, {semanticsId}}}));
            irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
            return Status::SuccessWithChange;
        }
        return Status::SuccessWithoutChange;
    }

    spvtools::Optimizer::PassToken FixIterationRPBarrierPass::CreateFixIterationRPBarrierPass() {
        return spvtools::Optimizer::PassToken(spvtools::MakeUnique<FixIterationRPBarrierPass>());
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
