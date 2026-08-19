// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPSubgroupScratchPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FixIterationRPSubgroupScratchPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // iterationRP's reduction fingerprint, spelled out.
                constexpr uint32_t kIterationRPLocalSizeX = 32u;
                constexpr uint32_t kIterationRPLocalSizeY = 16u;
                constexpr uint32_t kIterationRPLocalSizeZ = 1u;
                constexpr uint32_t kIterationRPInvocations =
                    kIterationRPLocalSizeX * kIterationRPLocalSizeY * kIterationRPLocalSizeZ;
                constexpr uint32_t kIterationRPScratchLength = 32u;

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

                // Walks an access-chain pointer expression back to the variable it is
                // rooted at; returns nullptr for anything that is not a plain chain.
                const Instruction* RootVariable(IRContext* context, uint32_t pointerId) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    const Instruction* def = defUseMgr->GetDef(pointerId);
                    while (def != nullptr) {
                        switch (def->opcode()) {
                        case spv::Op::OpVariable:
                            return def;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain:
                        case spv::Op::OpCopyObject:
                            def = defUseMgr->GetDef(def->GetSingleWordInOperand(0));
                            break;
                        default:
                            return nullptr;
                        }
                    }
                    return nullptr;
                }

                // vec2 of 32-bit float - the type of iterationRP's luminance/exposure
                // accumulator and of its prefixSumCache entries.
                bool IsVec2Float32(IRContext* context, uint32_t typeId) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    if (type == nullptr || type->opcode() != spv::Op::OpTypeVector ||
                        type->GetSingleWordInOperand(1) != 2u) {
                        return false;
                    }
                    const Instruction* component =
                        context->get_def_use_mgr()->GetDef(type->GetSingleWordInOperand(0));
                    return component != nullptr && component->opcode() == spv::Op::OpTypeFloat &&
                           component->GetSingleWordInOperand(0) == 32u;
                }
            } // namespace

            spvtools::opt::Pass::Status FixIterationRPSubgroupScratchPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // A device whose native width already satisfies the pack's assumption
                // (>= 16 lanes -> at most 32 subgroups) needs no patch at all.
                if (m_nativeSubgroupSize == 0u || m_nativeSubgroupSize >= 16u) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t requiredLength =
                    (kIterationRPInvocations + m_nativeSubgroupSize - 1u) / m_nativeSubgroupSize;

                for (const Instruction& entryPoint : irContext->module()->entry_points()) {
                    if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) !=
                        spv::ExecutionModel::GLCompute) {
                        return Status::SuccessWithoutChange;
                    }
                }

                // Fingerprint 1: the pack's exposure-pass workgroup shape, 32x16x1.
                const auto resolveUintConstant = [&](uint32_t id, uint32_t* value) {
                    const Instruction* def = defUseMgr->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant) return false;
                    *value = def->GetSingleWordInOperand(0);
                    return true;
                };
                uint32_t localSize[3] = {0, 0, 0};
                bool haveLocalSize = false;
                if (Instruction* workgroupSize =
                        FindBuiltinDefinition(irContext, spv::BuiltIn::WorkgroupSize)) {
                    if (workgroupSize->opcode() == spv::Op::OpConstantComposite &&
                        workgroupSize->NumInOperands() == 3) {
                        haveLocalSize =
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(0), &localSize[0]) &&
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(1), &localSize[1]) &&
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(2), &localSize[2]);
                    }
                }
                if (!haveLocalSize) {
                    for (const Instruction& mode : irContext->module()->execution_modes()) {
                        if (mode.opcode() == spv::Op::OpExecutionMode &&
                            static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(1)) ==
                                spv::ExecutionMode::LocalSize) {
                            localSize[0] = mode.GetSingleWordInOperand(2);
                            localSize[1] = mode.GetSingleWordInOperand(3);
                            localSize[2] = mode.GetSingleWordInOperand(4);
                            haveLocalSize = true;
                            break;
                        }
                    }
                }
                if (!haveLocalSize || localSize[0] != kIterationRPLocalSizeX ||
                    localSize[1] != kIterationRPLocalSizeY || localSize[2] != kIterationRPLocalSizeZ) {
                    return Status::SuccessWithoutChange;
                }

                // Fingerprint 2: the reduction's subgroupInclusiveAdd on a vec2.
                bool sawVec2InclusiveAdd = false;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            if (inst.opcode() == spv::Op::OpGroupNonUniformFAdd &&
                                static_cast<spv::GroupOperation>(inst.GetSingleWordInOperand(1)) ==
                                    spv::GroupOperation::InclusiveScan &&
                                IsVec2Float32(irContext, inst.type_id())) {
                                sawVec2InclusiveAdd = true;
                            }
                        }
                    }
                }
                if (!sawVec2InclusiveAdd) {
                    return Status::SuccessWithoutChange;
                }

                // gl_SubgroupID, whose value range the pack's scratch size bakes in.
                const Instruction* subgroupIdVariable =
                    FindBuiltinDefinition(irContext, spv::BuiltIn::SubgroupId);
                if (subgroupIdVariable == nullptr ||
                    subgroupIdVariable->opcode() != spv::Op::OpVariable) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t subgroupIdVariableId = subgroupIdVariable->result_id();

                // Conservative taint walk over values, and through Function/Private
                // temporaries by variable (glslang routinely spills builtin loads into
                // locals before they reach an index expression). Over-tainting is safe:
                // the candidate filter below still demands the exact vec2[32] shape.
                std::unordered_map<uint32_t, bool> valueTainted;   // result id -> tainted
                std::unordered_map<uint32_t, bool> variableTainted; // variable id -> tainted
                bool changedTaint = true;
                while (changedTaint) {
                    changedTaint = false;
                    for (auto& function : *irContext->module()) {
                        for (auto& block : function) {
                            for (auto& inst : block) {
                                const spv::Op opcode = inst.opcode();
                                if (opcode == spv::Op::OpStore) {
                                    if (!valueTainted.count(inst.GetSingleWordInOperand(1))) continue;
                                    const Instruction* root =
                                        RootVariable(irContext, inst.GetSingleWordInOperand(0));
                                    if (root == nullptr) continue;
                                    if (!variableTainted.count(root->result_id())) {
                                        variableTainted[root->result_id()] = true;
                                        changedTaint = true;
                                    }
                                    continue;
                                }
                                if (inst.result_id() == 0 || valueTainted.count(inst.result_id())) {
                                    continue;
                                }
                                bool tainted = false;
                                if (opcode == spv::Op::OpLoad) {
                                    const uint32_t pointerId = inst.GetSingleWordInOperand(0);
                                    if (pointerId == subgroupIdVariableId) tainted = true;
                                    const Instruction* root = RootVariable(irContext, pointerId);
                                    if (root != nullptr && variableTainted.count(root->result_id())) {
                                        tainted = true;
                                    }
                                } else {
                                    inst.ForEachInId([&](const uint32_t* operandId) {
                                        if (valueTainted.count(*operandId)) tainted = true;
                                    });
                                }
                                if (tainted) {
                                    valueTainted[inst.result_id()] = true;
                                    changedTaint = true;
                                }
                            }
                        }
                    }
                }
                if (valueTainted.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Fingerprint 3: workgroup-shared vec2[32] arrays whose access-chain
                // index depends on gl_SubgroupID - the under-declared prefixSumCache.
                std::map<uint32_t, Instruction*> candidates;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            if (inst.opcode() != spv::Op::OpAccessChain &&
                                inst.opcode() != spv::Op::OpInBoundsAccessChain) {
                                continue;
                            }
                            if (inst.NumInOperands() < 2) continue;
                            if (!valueTainted.count(inst.GetSingleWordInOperand(1))) continue;
                            Instruction* baseVariable =
                                defUseMgr->GetDef(inst.GetSingleWordInOperand(0));
                            if (baseVariable == nullptr ||
                                baseVariable->opcode() != spv::Op::OpVariable ||
                                static_cast<spv::StorageClass>(
                                    baseVariable->GetSingleWordInOperand(0)) !=
                                    spv::StorageClass::Workgroup) {
                                continue;
                            }
                            candidates.emplace(baseVariable->result_id(), baseVariable);
                        }
                    }
                }
                if (candidates.empty()) {
                    return Status::SuccessWithoutChange;
                }

                bool changedModule = false;
                for (auto& entry : candidates) {
                    Instruction* variable = entry.second;

                    // The variable must be reached exclusively through access chains (plus
                    // debug/decoration instructions): a whole-array load, store, or copy
                    // would change type with the array and is left alone.
                    bool onlyAccessChains = true;
                    const uint32_t variableId = variable->result_id();
                    defUseMgr->ForEachUser(variable, [&](Instruction* user) {
                        switch (user->opcode()) {
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain:
                            if (user->GetSingleWordInOperand(0) != variableId) {
                                onlyAccessChains = false;
                            }
                            return;
                        case spv::Op::OpName:
                        case spv::Op::OpDecorate:
                            return;
                        default:
                            onlyAccessChains = false;
                            return;
                        }
                    });
                    if (!onlyAccessChains) continue;
                    if (variable->NumInOperands() > 1) continue; // initializer: leave alone

                    const Instruction* pointerType = defUseMgr->GetDef(variable->type_id());
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                        continue;
                    }
                    const Instruction* arrayType =
                        defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
                    if (arrayType == nullptr || arrayType->opcode() != spv::Op::OpTypeArray) {
                        continue;
                    }
                    const uint32_t elementTypeId = arrayType->GetSingleWordInOperand(0);
                    if (!IsVec2Float32(irContext, elementTypeId)) continue;
                    const Instruction* lengthConstant =
                        defUseMgr->GetDef(arrayType->GetSingleWordInOperand(1));
                    uint32_t currentLength = 0;
                    if (lengthConstant == nullptr ||
                        lengthConstant->opcode() != spv::Op::OpConstant ||
                        !((currentLength = lengthConstant->GetSingleWordInOperand(0),
                           currentLength == kIterationRPScratchLength))) {
                        continue;
                    }
                    if (currentLength >= requiredLength) continue;

                    // Build the grown array type. All three new instructions are inserted
                    // immediately BEFORE the variable so definition-before-use holds in the
                    // module's global section (manager-created instructions append to its
                    // end, after the variable). The new length constant reuses the old
                    // one's integer type, whatever signedness glslang gave it (a duplicate
                    // scalar constant is legal SPIR-V); the fresh array type makes the
                    // pointer type unique by construction, so neither collides with an
                    // existing declaration.
                    const uint32_t lengthTypeId = lengthConstant->type_id();
                    const uint32_t newLengthId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpConstant, lengthTypeId, newLengthId,
                        Instruction::OperandList{{SPV_OPERAND_TYPE_TYPED_LITERAL_NUMBER,
                                                  {requiredLength}}}));
                    const uint32_t newArrayTypeId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpTypeArray, 0, newArrayTypeId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_ID, {elementTypeId}},
                            {SPV_OPERAND_TYPE_ID, {newLengthId}}}));
                    const uint32_t newPointerTypeId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpTypePointer, 0, newPointerTypeId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Workgroup)}},
                            {SPV_OPERAND_TYPE_ID, {newArrayTypeId}}}));

                    variable->SetResultType(newPointerTypeId);
                    changedModule = true;
                }

                if (!changedModule) {
                    return Status::SuccessWithoutChange;
                }
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            FixIterationRPSubgroupScratchPass::CreateFixIterationRPSubgroupScratchPass(
                const Uint32 nativeSubgroupSize) {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<FixIterationRPSubgroupScratchPass>(nativeSubgroupSize));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
