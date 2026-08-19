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

                // The Vulkan minimum for maxComputeSharedMemorySize, used when the caller
                // could not tell us the device's real limit.
                constexpr uint32_t kMinimumSharedMemoryBytes = 16384u;

                // The narrowest subgroup width iterationRP's declarations are sized for.
                // At or above it both shipped shapes fit and nothing may be rewritten.
                constexpr uint32_t kPackAssumedSubgroupWidth = 16u;

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

                // A 32-bit float scalar or vector - the shape of every accumulator the
                // pack runs through its scans (float, vec2 and vec4 all appear). Returns
                // the component count, or 0 for anything else.
                uint32_t Float32ComponentCount(IRContext* context, uint32_t typeId) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    const Instruction* type = defUseMgr->GetDef(typeId);
                    if (type == nullptr) return 0u;
                    uint32_t components = 1u;
                    if (type->opcode() == spv::Op::OpTypeVector) {
                        components = type->GetSingleWordInOperand(1);
                        if (components < 2u || components > 4u) return 0u;
                        type = defUseMgr->GetDef(type->GetSingleWordInOperand(0));
                        if (type == nullptr) return 0u;
                    }
                    if (type->opcode() != spv::Op::OpTypeFloat ||
                        type->GetSingleWordInOperand(0) != 32u) {
                        return 0u;
                    }
                    return components;
                }

                uint32_t RoundUp(uint32_t value, uint32_t alignment) {
                    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
                }

                // Size AND alignment of a workgroup-storage type. Drivers lay shared
                // memory out at natural alignment and the limit
                // (VUID-RuntimeSpirv-Workgroup-06530) counts the padding that produces,
                // so a model that sums unpadded sizes would under-count exactly where the
                // budget check matters. Returns false for anything not modelled here,
                // which the caller answers by declining to grow at all rather than by
                // certifying growth against a total it knows is an underestimate.
                bool WorkgroupTypeLayout(IRContext* context, uint32_t typeId, uint32_t* size,
                                         uint32_t* alignment, uint32_t depth = 0u) {
                    if (depth > 8u) return false;
                    auto* defUseMgr = context->get_def_use_mgr();
                    const Instruction* type = defUseMgr->GetDef(typeId);
                    if (type == nullptr) return false;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeBool:
                        *size = 4u;
                        *alignment = 4u;
                        return true;
                    case spv::Op::OpTypeInt:
                    case spv::Op::OpTypeFloat: {
                        const uint32_t width = type->GetSingleWordInOperand(0) / 8u;
                        if (width == 0u) return false;
                        *size = width;
                        *alignment = width;
                        return true;
                    }
                    case spv::Op::OpTypeVector: {
                        uint32_t componentSize = 0u;
                        uint32_t componentAlignment = 0u;
                        if (!WorkgroupTypeLayout(context, type->GetSingleWordInOperand(0),
                                                 &componentSize, &componentAlignment, depth + 1u)) {
                            return false;
                        }
                        const uint32_t components = type->GetSingleWordInOperand(1);
                        if (components < 2u || components > 4u) return false;
                        *size = componentSize * components;
                        // A three-component vector aligns like a four-component one.
                        *alignment = componentSize * (components == 3u ? 4u : components);
                        return true;
                    }
                    case spv::Op::OpTypeMatrix:
                    case spv::Op::OpTypeArray: {
                        uint32_t elementSize = 0u;
                        uint32_t elementAlignment = 0u;
                        if (!WorkgroupTypeLayout(context, type->GetSingleWordInOperand(0), &elementSize,
                                                 &elementAlignment, depth + 1u)) {
                            return false;
                        }
                        uint32_t count = 0u;
                        if (type->opcode() == spv::Op::OpTypeMatrix) {
                            count = type->GetSingleWordInOperand(1);
                        } else {
                            const Instruction* length =
                                defUseMgr->GetDef(type->GetSingleWordInOperand(1));
                            if (length == nullptr || length->opcode() != spv::Op::OpConstant) {
                                return false;  // spec-constant length: not sizeable here
                            }
                            count = length->GetSingleWordInOperand(0);
                        }
                        *size = RoundUp(elementSize, elementAlignment) * count;
                        *alignment = elementAlignment;
                        return true;
                    }
                    case spv::Op::OpTypeStruct: {
                        uint32_t offset = 0u;
                        uint32_t structAlignment = 1u;
                        for (uint32_t i = 0; i < type->NumInOperands(); ++i) {
                            uint32_t memberSize = 0u;
                            uint32_t memberAlignment = 0u;
                            if (!WorkgroupTypeLayout(context, type->GetSingleWordInOperand(i),
                                                     &memberSize, &memberAlignment, depth + 1u)) {
                                return false;
                            }
                            offset = RoundUp(offset, memberAlignment) + memberSize;
                            if (memberAlignment > structAlignment) structAlignment = memberAlignment;
                        }
                        *size = RoundUp(offset, structAlignment);
                        *alignment = structAlignment;
                        return true;
                    }
                    default:
                        return false;
                    }
                }

                // The group operations the pack's prefix scans use.
                bool IsScanOrReduce(spv::GroupOperation operation) {
                    return operation == spv::GroupOperation::Reduce ||
                           operation == spv::GroupOperation::InclusiveScan ||
                           operation == spv::GroupOperation::ExclusiveScan;
                }
            } // namespace

            spvtools::opt::Pass::Status FixIterationRPSubgroupScratchPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Without a known device width there is no topology to compare against;
                // and a width the pack already assumed needs no patch at all. Both of
                // iterationRP's shapes are sized for >= 16 lanes (512/16 = 32 entries,
                // 1024/16 = 64), so every module on such a device - the pack's or anyone
                // else's - must pass through byte-identical. The per-array length test
                // further down is the second gate, not a replacement for this one.
                if (m_nativeSubgroupSize == 0u || m_nativeSubgroupSize >= kPackAssumedSubgroupWidth) {
                    return Status::SuccessWithoutChange;
                }

                for (const Instruction& entryPoint : irContext->module()->entry_points()) {
                    if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) !=
                        spv::ExecutionModel::GLCompute) {
                        return Status::SuccessWithoutChange;
                    }
                }

                // Fingerprint 1: a literal workgroup size, so the subgroup count the
                // dispatch actually partitions into is known here.
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
                if (!haveLocalSize || localSize[0] == 0u || localSize[1] == 0u || localSize[2] == 0u) {
                    return Status::SuccessWithoutChange;
                }
                const uint64_t totalInvocations =
                    static_cast<uint64_t>(localSize[0]) * localSize[1] * localSize[2];
                if (totalInvocations == 0u || totalInvocations > (1u << 20)) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t requiredLength = static_cast<uint32_t>(
                    (totalInvocations + m_nativeSubgroupSize - 1u) / m_nativeSubgroupSize);

                // Fingerprint 2: a subgroup scan over a 32-bit float value - the pack's
                // prefix-sum reduction, and the reason its scratch is indexed per subgroup.
                bool sawFloatSubgroupScan = false;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            if (inst.opcode() != spv::Op::OpGroupNonUniformFAdd &&
                                inst.opcode() != spv::Op::OpGroupNonUniformFMin &&
                                inst.opcode() != spv::Op::OpGroupNonUniformFMax) {
                                continue;
                            }
                            if (inst.NumInOperands() < 2) continue;
                            if (!IsScanOrReduce(static_cast<spv::GroupOperation>(
                                    inst.GetSingleWordInOperand(1)))) {
                                continue;
                            }
                            if (Float32ComponentCount(irContext, inst.type_id()) != 0u) {
                                sawFloatSubgroupScan = true;
                            }
                        }
                    }
                }
                if (!sawFloatSubgroupScan) {
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

                // The pack indexes its scratch with gl_SubgroupID ITSELF, so only values
                // that ARE that id qualify - not everything computed from it. An index
                // that is masked or clamped (cache[gl_SubgroupID & 3u]) is bounded by
                // construction and is none of this pass's business; accepting it would
                // turn a targeted repair into a general array resizer. Identity survives
                // OpCopyObject, a signedness OpBitcast, and the Function/Private spill
                // glslang emits for a builtin load - and nothing else. A spill variable
                // counts only when EVERY store into it is the id.
                std::unordered_map<uint32_t, bool> subgroupIdValues;    // result id IS the id
                std::unordered_map<uint32_t, bool> subgroupIdVariables; // spill holding only it
                bool changedIdentity = true;
                while (changedIdentity) {
                    changedIdentity = false;

                    std::unordered_map<uint32_t, uint32_t> totalStores;
                    std::unordered_map<uint32_t, uint32_t> idStores;
                    for (auto& function : *irContext->module()) {
                        for (auto& block : function) {
                            for (auto& inst : block) {
                                if (inst.opcode() != spv::Op::OpStore) continue;
                                const uint32_t pointerId = inst.GetSingleWordInOperand(0);
                                const Instruction* target = defUseMgr->GetDef(pointerId);
                                if (target == nullptr || target->opcode() != spv::Op::OpVariable) {
                                    continue;
                                }
                                const auto storageClass = static_cast<spv::StorageClass>(
                                    target->GetSingleWordInOperand(0));
                                if (storageClass != spv::StorageClass::Function &&
                                    storageClass != spv::StorageClass::Private) {
                                    continue;
                                }
                                totalStores[pointerId] += 1u;
                                if (subgroupIdValues.count(inst.GetSingleWordInOperand(1))) {
                                    idStores[pointerId] += 1u;
                                }
                            }
                        }
                    }
                    for (const auto& entry : totalStores) {
                        if (entry.second != 0u && idStores[entry.first] == entry.second &&
                            !subgroupIdVariables.count(entry.first)) {
                            subgroupIdVariables[entry.first] = true;
                            changedIdentity = true;
                        }
                    }

                    for (auto& function : *irContext->module()) {
                        for (auto& block : function) {
                            for (auto& inst : block) {
                                if (inst.result_id() == 0 ||
                                    subgroupIdValues.count(inst.result_id())) {
                                    continue;
                                }
                                bool isSubgroupId = false;
                                switch (inst.opcode()) {
                                case spv::Op::OpLoad: {
                                    const uint32_t pointerId = inst.GetSingleWordInOperand(0);
                                    isSubgroupId = pointerId == subgroupIdVariableId ||
                                                   subgroupIdVariables.count(pointerId) != 0u;
                                    break;
                                }
                                case spv::Op::OpCopyObject:
                                case spv::Op::OpBitcast:
                                    isSubgroupId =
                                        subgroupIdValues.count(inst.GetSingleWordInOperand(0)) != 0u;
                                    break;
                                default:
                                    break;
                                }
                                if (isSubgroupId) {
                                    subgroupIdValues[inst.result_id()] = true;
                                    changedIdentity = true;
                                }
                            }
                        }
                    }
                }
                if (subgroupIdValues.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Fingerprint 3: workgroup-shared float arrays indexed by gl_SubgroupID
                // itself - the under-declared prefixSumCache.
                std::map<uint32_t, Instruction*> candidates;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            if (inst.opcode() != spv::Op::OpAccessChain &&
                                inst.opcode() != spv::Op::OpInBoundsAccessChain) {
                                continue;
                            }
                            if (inst.NumInOperands() < 2) continue;
                            if (!subgroupIdValues.count(inst.GetSingleWordInOperand(1))) continue;
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

                // Everything that survives the filter, with the bytes each grown array
                // will need. Nothing is mutated until the whole set fits the device's
                // shared-memory budget, so a module is never left half-grown.
                struct Growth {
                    Instruction* variable = nullptr;
                    uint32_t elementTypeId = 0;
                    uint32_t lengthTypeId = 0;
                    uint32_t addedBytes = 0;
                };
                std::vector<Growth> growths;
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
                    const uint32_t components = Float32ComponentCount(irContext, elementTypeId);
                    if (components == 0u) continue;
                    const Instruction* lengthConstant =
                        defUseMgr->GetDef(arrayType->GetSingleWordInOperand(1));
                    if (lengthConstant == nullptr || lengthConstant->opcode() != spv::Op::OpConstant) {
                        continue;
                    }
                    const uint32_t currentLength = lengthConstant->GetSingleWordInOperand(0);

                    // The pack's own assumption holds on this device: the declared array
                    // already covers every subgroup the workgroup partitions into. That is
                    // every >= 16-lane device for the shapes iterationRP ships, and those
                    // modules must pass through byte-identical.
                    if (currentLength >= requiredLength) continue;

                    // vec3 strides at its 16-byte alignment, so charge the padded stride.
                    const uint32_t elementStride = (components == 3u ? 4u : components) * 4u;
                    growths.push_back(Growth{variable, elementTypeId, lengthConstant->type_id(),
                                             (requiredLength - currentLength) * elementStride});
                }
                if (growths.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Growing must not push the module past what the device can launch: a
                // pipeline that fails to create is worse than the pack's own overrun.
                {
                    uint64_t declaredBytes = 0;
                    bool sawUnsizeable = false;
                    for (auto& global : irContext->module()->types_values()) {
                        if (global.opcode() != spv::Op::OpVariable ||
                            static_cast<spv::StorageClass>(global.GetSingleWordInOperand(0)) !=
                                spv::StorageClass::Workgroup) {
                            continue;
                        }
                        const Instruction* pointerType = defUseMgr->GetDef(global.type_id());
                        uint32_t bytes = 0u;
                        uint32_t alignment = 0u;
                        if (pointerType == nullptr ||
                            pointerType->opcode() != spv::Op::OpTypePointer ||
                            !WorkgroupTypeLayout(irContext, pointerType->GetSingleWordInOperand(1),
                                                 &bytes, &alignment)) {
                            sawUnsizeable = true;
                            break;
                        }
                        declaredBytes = RoundUp(static_cast<uint32_t>(declaredBytes), alignment) + bytes;
                    }
                    // A declaration this pass cannot size leaves the total an
                    // underestimate, so the growth cannot be certified against the device
                    // limit at all - decline rather than guess.
                    if (sawUnsizeable) {
                        return Status::SuccessWithoutChange;
                    }
                    for (const Growth& growth : growths) declaredBytes += growth.addedBytes;

                    const uint32_t deviceBudget = m_maxWorkgroupScratchBytes != 0u
                                                      ? m_maxWorkgroupScratchBytes
                                                      : kMinimumSharedMemoryBytes;
                    if (declaredBytes > deviceBudget) {
                        return Status::SuccessWithoutChange;
                    }
                }

                for (const Growth& growth : growths) {
                    // Build the grown array type. All three new instructions are inserted
                    // immediately BEFORE the variable so definition-before-use holds in the
                    // module's global section (manager-created instructions append to its
                    // end, after the variable). The new length constant reuses the old
                    // one's integer type, whatever signedness glslang gave it (a duplicate
                    // scalar constant is legal SPIR-V); the fresh array type makes the
                    // pointer type unique by construction, so neither collides with an
                    // existing declaration.
                    Instruction* variable = growth.variable;
                    const uint32_t newLengthId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpConstant, growth.lengthTypeId, newLengthId,
                        Instruction::OperandList{{SPV_OPERAND_TYPE_TYPED_LITERAL_NUMBER,
                                                  {requiredLength}}}));
                    const uint32_t newArrayTypeId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpTypeArray, 0, newArrayTypeId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_ID, {growth.elementTypeId}},
                            {SPV_OPERAND_TYPE_ID, {newLengthId}}}));
                    const uint32_t newPointerTypeId = irContext->TakeNextId();
                    variable->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpTypePointer, 0, newPointerTypeId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Workgroup)}},
                            {SPV_OPERAND_TYPE_ID, {newArrayTypeId}}}));

                    variable->SetResultType(newPointerTypeId);
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            FixIterationRPSubgroupScratchPass::CreateFixIterationRPSubgroupScratchPass(
                const Uint32 nativeSubgroupSize, const Uint32 maxWorkgroupScratchBytes) {
                return spvtools::Optimizer::PassToken(MakeUnique<FixIterationRPSubgroupScratchPass>(
                    nativeSubgroupSize, maxWorkgroupScratchBytes));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
