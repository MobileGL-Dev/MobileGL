// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeriveNumSubgroupsPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DeriveNumSubgroupsPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
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

                Instruction* FindBuiltinDefinition(IRContext* context, spv::BuiltIn builtin) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn) {
                            continue;
                        }
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != builtin) {
                            continue;
                        }
                        return defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                    }
                    return nullptr;
                }

                bool IsInputPointerTo(IRContext* context, const Instruction* variable, uint32_t pointeeTypeId) {
                    if (variable == nullptr || variable->opcode() != spv::Op::OpVariable ||
                        variable->NumInOperands() < 1 ||
                        static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) !=
                            spv::StorageClass::Input) {
                        return false;
                    }
                    const Instruction* pointerType = context->get_def_use_mgr()->GetDef(variable->type_id());
                    return pointerType != nullptr && pointerType->opcode() == spv::Op::OpTypePointer &&
                           pointerType->NumInOperands() >= 2 &&
                           static_cast<spv::StorageClass>(pointerType->GetSingleWordInOperand(0)) ==
                               spv::StorageClass::Input &&
                           pointerType->GetSingleWordInOperand(1) == pointeeTypeId;
                }

                bool IsUnsignedInt32(IRContext* context, uint32_t typeId) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    return type != nullptr && type->opcode() == spv::Op::OpTypeInt &&
                           type->NumInOperands() >= 2 && type->GetSingleWordInOperand(0) == 32u &&
                           type->GetSingleWordInOperand(1) == 0u;
                }

                uint32_t SynthesizeSubgroupSizeVariable(IRContext* context, uint32_t pointerTypeId) {
                    const uint32_t variableId = context->TakeNextId();
                    context->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpVariable, pointerTypeId, variableId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Input)}}}));
                    context->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {variableId}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::BuiltIn)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER,
                             {static_cast<uint32_t>(spv::BuiltIn::SubgroupSize)}}}));

                    for (Instruction& entryPoint : context->module()->entry_points()) {
                        entryPoint.AddOperand({SPV_OPERAND_TYPE_ID, {variableId}});
                    }
                    return variableId;
                }
            } // namespace

            spvtools::opt::Pass::Status DeriveNumSubgroupsPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                Instruction* numSubgroupsVar = FindBuiltinDefinition(irContext, spv::BuiltIn::NumSubgroups);
                if (numSubgroupsVar == nullptr) {
                    return Status::SuccessWithoutChange;
                }

                std::vector<Instruction*> numSubgroupsLoads;
                bool sawUnexpectedUser = false;
                const uint32_t numSubgroupsVarId = numSubgroupsVar->result_id();
                defUseMgr->ForEachUser(numSubgroupsVar, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpLoad:
                        if (user->NumInOperands() >= 1 &&
                            user->GetSingleWordInOperand(0) == numSubgroupsVarId) {
                            numSubgroupsLoads.push_back(user);
                        } else {
                            sawUnexpectedUser = true;
                        }
                        return;
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                    case spv::Op::OpDecorateString:
                    case spv::Op::OpName:
                    case spv::Op::OpEntryPoint:
                        return;
                    default:
                        sawUnexpectedUser = true;
                        return;
                    }
                });
                if (sawUnexpectedUser) {
                    return Status::Failure;
                }
                if (numSubgroupsLoads.empty()) {
                    return Status::SuccessWithoutChange;
                }

                const uint32_t valueTypeId = numSubgroupsLoads.front()->type_id();
                if (!IsUnsignedInt32(irContext, valueTypeId) ||
                    !IsInputPointerTo(irContext, numSubgroupsVar, valueTypeId)) {
                    return Status::Failure;
                }
                for (const Instruction* load : numSubgroupsLoads) {
                    if (load->type_id() != valueTypeId) {
                        return Status::Failure;
                    }
                }

                Instruction* workgroupSize = FindBuiltinDefinition(irContext, spv::BuiltIn::WorkgroupSize);
                if (workgroupSize == nullptr ||
                    (workgroupSize->opcode() != spv::Op::OpConstantComposite &&
                     workgroupSize->opcode() != spv::Op::OpSpecConstantComposite)) {
                    return Status::Failure;
                }
                const Instruction* workgroupSizeType = defUseMgr->GetDef(workgroupSize->type_id());
                if (workgroupSizeType == nullptr || workgroupSizeType->opcode() != spv::Op::OpTypeVector ||
                    workgroupSizeType->NumInOperands() < 2 ||
                    workgroupSizeType->GetSingleWordInOperand(0) != valueTypeId ||
                    workgroupSizeType->GetSingleWordInOperand(1) != 3u) {
                    return Status::Failure;
                }

                Instruction* subgroupSizeVar = FindBuiltinDefinition(irContext, spv::BuiltIn::SubgroupSize);
                if (subgroupSizeVar != nullptr &&
                    !IsInputPointerTo(irContext, subgroupSizeVar, valueTypeId)) {
                    return Status::Failure;
                }

                auto* constantMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();
                const auto* valueType = typeMgr->GetType(valueTypeId);
                if (valueType == nullptr) {
                    return Status::Failure;
                }
                const auto* one = constantMgr->GetConstant(valueType, {1u});
                const Instruction* oneInst =
                    one != nullptr ? constantMgr->GetDefiningInstruction(one, valueTypeId) : nullptr;
                if (oneInst == nullptr) {
                    return Status::Failure;
                }
                const uint32_t oneId = oneInst->result_id();

                const uint32_t subgroupSizeVarId = subgroupSizeVar != nullptr
                    ? subgroupSizeVar->result_id()
                    : SynthesizeSubgroupSizeVariable(irContext, numSubgroupsVar->type_id());
                const uint32_t workgroupSizeId = workgroupSize->result_id();

                // ceil(local invocation count / SubgroupSize): the subgroup count of a
                // full-subgroup launch. Vulkan only guarantees that partition under
                // REQUIRE_FULL_SUBGROUPS - which ProgramFactory requests whenever
                // local_size_x is a multiple of the subgroup size makes it legal
                // (VUID-VkPipelineShaderStageCreateInfo-flags-02759) - and calls the
                // tighter behaviour "encouraged" everywhere else; the DriverPost witness
                // verifies it per device where the flag cannot be set. The absence of
                // ALLOW_VARYING_SUBGROUP_SIZE pins only the SubgroupSize builtin itself.
                // `(count - 1) / size + 1` avoids an addition overflow at count + size - 1.
                for (Instruction* load : numSubgroupsLoads) {
                    const uint32_t localSizeXId = irContext->TakeNextId();
                    const uint32_t localSizeYId = irContext->TakeNextId();
                    const uint32_t localSizeZId = irContext->TakeNextId();
                    const uint32_t localSizeXYId = irContext->TakeNextId();
                    const uint32_t invocationCountId = irContext->TakeNextId();
                    const uint32_t adjustedCountId = irContext->TakeNextId();
                    const uint32_t subgroupSizeId = irContext->TakeNextId();
                    const uint32_t quotientId = irContext->TakeNextId();

                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpCompositeExtract, valueTypeId, localSizeXId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {workgroupSizeId}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpCompositeExtract, valueTypeId, localSizeYId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {workgroupSizeId}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {1u}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpCompositeExtract, valueTypeId, localSizeZId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {workgroupSizeId}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {2u}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpIMul, valueTypeId, localSizeXYId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {localSizeXId}},
                            {SPV_OPERAND_TYPE_ID, {localSizeYId}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpIMul, valueTypeId, invocationCountId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {localSizeXYId}},
                            {SPV_OPERAND_TYPE_ID, {localSizeZId}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpISub, valueTypeId, adjustedCountId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {invocationCountId}},
                            {SPV_OPERAND_TYPE_ID, {oneId}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpLoad, valueTypeId, subgroupSizeId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {subgroupSizeVarId}}}));
                    load->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpUDiv, valueTypeId, quotientId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {adjustedCountId}},
                            {SPV_OPERAND_TYPE_ID, {subgroupSizeId}}}));

                    // Preserve the original result id so every downstream use automatically sees
                    // the derived value instead of the driver's NumSubgroups builtin.
                    load->SetOpcode(spv::Op::OpIAdd);
                    load->SetInOperands(Instruction::OperandList{
                        {SPV_OPERAND_TYPE_ID, {quotientId}},
                        {SPV_OPERAND_TYPE_ID, {oneId}}});
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken DeriveNumSubgroupsPass::CreateDeriveNumSubgroupsPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<DeriveNumSubgroupsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
