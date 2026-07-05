// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecomposeWorkgroupVec3Pass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DecomposeWorkgroupVec3Pass.h"

#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "spirv.hpp"

#include <cassert>
#include <unordered_map>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::IRContext;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;
                using spvtools::opt::BasicBlock;
                using spvtools::opt::analysis::Array;
                using spvtools::opt::analysis::Type;

                // Returns the element count if |typeInst| is a 3-component vector of a
                // numeric/bool scalar (i.e. vec3/ivec3/uvec3/bvec3). Returns 0 otherwise.
                uint32_t IsVec3Type(const Instruction* typeInst) {
                    if (typeInst == nullptr || typeInst->opcode() != spv::Op::OpTypeVector) {
                        return 0;
                    }
                    if (typeInst->GetSingleWordInOperand(1) != 3) {
                        return 0;
                    }
                    return 3;
                }

                // Walks the type tree (array -> array -> ... -> leaf) and returns the leaf
                // element type id, peeling OpTypeArray layers. Returns 0 if a non-array,
                // non-vec3 type is encountered before reaching a vec3 leaf.
                uint32_t FindVec3LeafTypeId(IRContext* context, uint32_t typeId) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    while (true) {
                        Instruction* typeInst = defUseMgr->GetDef(typeId);
                        if (typeInst == nullptr) {
                            return 0;
                        }
                        if (IsVec3Type(typeInst) != 0) {
                            return typeId;
                        }
                        if (typeInst->opcode() == spv::Op::OpTypeArray) {
                            typeId = typeInst->GetSingleWordInOperand(0);
                            continue;
                        }
                        return 0;
                    }
                }

                // Recursively rebuilds a pointee type, replacing the vec3 leaf with a
                // scalar array [3]. Returns the new type id.
                uint32_t RebuildPointeeType(IRContext* context, uint32_t typeId,
                                            uint32_t scalarArr3TypeId,
                                            const std::unordered_map<uint32_t, uint32_t>& vec3ToArr3) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    auto* typeMgr = context->get_type_mgr();
                    Instruction* typeInst = defUseMgr->GetDef(typeId);
                    assert(typeInst != nullptr);

                    if (typeInst->opcode() == spv::Op::OpTypeVector) {
                        // Should be a vec3; replace with the scalar array [3].
                        auto it = vec3ToArr3.find(typeId);
                        assert(it != vec3ToArr3.end());
                        return it->second;
                    }
                    if (typeInst->opcode() == spv::Op::OpTypeArray) {
                        const uint32_t oldElemId = typeInst->GetSingleWordInOperand(0);
                        const uint32_t newElemId =
                            RebuildPointeeType(context, oldElemId, scalarArr3TypeId, vec3ToArr3);
                        if (newElemId == oldElemId) {
                            return typeId;
                        }
                        const uint32_t lengthId = typeInst->GetSingleWordInOperand(1);
                        const Array* oldArrTy =
                            typeMgr->GetType(typeId)->AsArray();
                        Array newArrTy(typeMgr->GetType(newElemId),
                                                 oldArrTy->length_info());
                        Type* regNewArrTy = typeMgr->GetRegisteredType(&newArrTy);
                        return typeMgr->GetTypeInstruction(regNewArrTy);
                    }
                    // Unsupported leaf (struct, matrix, etc.) - should not happen for v1.
                    assert(false && "DecomposeWorkgroupVec3Pass: unsupported type in pointee");
                    return typeId;
                }

                uint32_t GetArrayLength(IRContext* context, const Instruction* arrayTypeInst) {
                    assert(arrayTypeInst != nullptr && arrayTypeInst->opcode() == spv::Op::OpTypeArray);
                    const uint32_t lengthId = arrayTypeInst->GetSingleWordInOperand(1);
                    const Instruction* lengthInst = context->get_def_use_mgr()->GetDef(lengthId);
                    if (lengthInst == nullptr || lengthInst->opcode() != spv::Op::OpConstant ||
                        lengthInst->NumInOperands() == 0) {
                        return 0;
                    }
                    return lengthInst->GetSingleWordInOperand(0);
                }

                // Inserts an instruction before |where|, sets its debug info, and updates
                // def-use and block mapping. Returns a pointer to the inserted instruction.
                Instruction* InsertBefore(BasicBlock* block, Instruction* where,
                                          std::unique_ptr<Instruction> inst, IRContext* context) {
                    auto iter = BasicBlock::iterator(where).InsertBefore(std::move(inst));
                    if (where != nullptr) {
                        iter->UpdateDebugInfoFrom(where);
                    }
                    context->AnalyzeDefUse(&*iter);
                    context->set_instr_block(&*iter, block);
                    return &*iter;
                }

                uint32_t BuildCompositeLoad(IRContext* context, BasicBlock* block, Instruction* where,
                                            uint32_t originalTypeId, uint32_t decomposedPtrId,
                                            const std::unordered_map<uint32_t, uint32_t>& vec3ToArr3,
                                            const Instruction* sourceLoad) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    auto* typeMgr = context->get_type_mgr();
                    auto* constMgr = context->get_constant_mgr();
                    Instruction* originalTypeInst = defUseMgr->GetDef(originalTypeId);
                    assert(originalTypeInst != nullptr);

                    if (originalTypeInst->opcode() == spv::Op::OpTypeVector) {
                        const auto vecIt = vec3ToArr3.find(originalTypeId);
                        assert(vecIt != vec3ToArr3.end());
                        const uint32_t scalarTypeId = originalTypeInst->GetSingleWordInOperand(0);
                        const uint32_t ptrScalarTypeId =
                            typeMgr->FindPointerToType(scalarTypeId, spv::StorageClass::Workgroup);
                        std::vector<uint32_t> componentIds;
                        componentIds.reserve(3);
                        for (uint32_t c = 0; c < 3; ++c) {
                            const uint32_t constCId = constMgr->GetUIntConstId(c);
                            const uint32_t acId = context->TakeNextId();
                            auto acInst = MakeUnique<Instruction>(
                                context, spv::Op::OpAccessChain, ptrScalarTypeId, acId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {decomposedPtrId}},
                                    {SPV_OPERAND_TYPE_ID, {constCId}}});
                            InsertBefore(block, where, std::move(acInst), context);

                            const uint32_t loadId = context->TakeNextId();
                            auto loadInst = MakeUnique<Instruction>(
                                context, spv::Op::OpLoad, scalarTypeId, loadId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {acId}}});
                            for (uint32_t opIdx = 1; opIdx < sourceLoad->NumInOperands(); ++opIdx) {
                                loadInst->AddOperand(Operand(sourceLoad->GetInOperand(opIdx)));
                            }
                            Instruction* loadPtr = InsertBefore(block, where, std::move(loadInst), context);
                            componentIds.push_back(loadPtr->result_id());
                        }

                        const uint32_t compositeId = context->TakeNextId();
                        auto composite = MakeUnique<Instruction>(
                            context, spv::Op::OpCompositeConstruct, originalTypeId, compositeId,
                            std::initializer_list<Operand>{});
                        for (uint32_t componentId : componentIds) {
                            composite->AddOperand({SPV_OPERAND_TYPE_ID, {componentId}});
                        }
                        InsertBefore(block, where, std::move(composite), context);
                        return compositeId;
                    }

                    if (originalTypeInst->opcode() == spv::Op::OpTypeArray) {
                        const uint32_t originalElemTypeId = originalTypeInst->GetSingleWordInOperand(0);
                        const uint32_t vec3LeafId = FindVec3LeafTypeId(context, originalElemTypeId);
                        assert(vec3LeafId != 0);
                        const auto arr3It = vec3ToArr3.find(vec3LeafId);
                        assert(arr3It != vec3ToArr3.end());
                        const uint32_t decomposedElemTypeId =
                            originalElemTypeId == vec3LeafId
                                ? arr3It->second
                                : RebuildPointeeType(context, originalElemTypeId, arr3It->second, vec3ToArr3);
                        const uint32_t ptrDecomposedElemTypeId =
                            typeMgr->FindPointerToType(decomposedElemTypeId, spv::StorageClass::Workgroup);
                        const uint32_t length = GetArrayLength(context, originalTypeInst);
                        assert(length != 0);

                        std::vector<uint32_t> elementIds;
                        elementIds.reserve(length);
                        for (uint32_t i = 0; i < length; ++i) {
                            const uint32_t constIId = constMgr->GetUIntConstId(i);
                            const uint32_t elemPtrId = context->TakeNextId();
                            auto elemAc = MakeUnique<Instruction>(
                                context, spv::Op::OpAccessChain, ptrDecomposedElemTypeId, elemPtrId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {decomposedPtrId}},
                                    {SPV_OPERAND_TYPE_ID, {constIId}}});
                            InsertBefore(block, where, std::move(elemAc), context);
                            elementIds.push_back(BuildCompositeLoad(context, block, where, originalElemTypeId,
                                                                    elemPtrId, vec3ToArr3, sourceLoad));
                        }

                        const uint32_t compositeId = context->TakeNextId();
                        auto composite = MakeUnique<Instruction>(
                            context, spv::Op::OpCompositeConstruct, originalTypeId, compositeId,
                            std::initializer_list<Operand>{});
                        for (uint32_t elementId : elementIds) {
                            composite->AddOperand({SPV_OPERAND_TYPE_ID, {elementId}});
                        }
                        InsertBefore(block, where, std::move(composite), context);
                        return compositeId;
                    }

                    assert(false && "DecomposeWorkgroupVec3Pass: unsupported composite load type");
                    return 0;
                }

                void BuildCompositeStore(IRContext* context, BasicBlock* block, Instruction* where,
                                         uint32_t originalTypeId, uint32_t decomposedPtrId, uint32_t valueId,
                                         const std::unordered_map<uint32_t, uint32_t>& vec3ToArr3,
                                         const Instruction* sourceStore) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    auto* typeMgr = context->get_type_mgr();
                    auto* constMgr = context->get_constant_mgr();
                    Instruction* originalTypeInst = defUseMgr->GetDef(originalTypeId);
                    assert(originalTypeInst != nullptr);

                    if (originalTypeInst->opcode() == spv::Op::OpTypeVector) {
                        const auto vecIt = vec3ToArr3.find(originalTypeId);
                        assert(vecIt != vec3ToArr3.end());
                        const uint32_t scalarTypeId = originalTypeInst->GetSingleWordInOperand(0);
                        const uint32_t ptrScalarTypeId =
                            typeMgr->FindPointerToType(scalarTypeId, spv::StorageClass::Workgroup);
                        for (uint32_t c = 0; c < 3; ++c) {
                            const uint32_t extractId = context->TakeNextId();
                            auto extract = MakeUnique<Instruction>(
                                context, spv::Op::OpCompositeExtract, scalarTypeId, extractId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {valueId}},
                                    {SPV_OPERAND_TYPE_LITERAL_INTEGER, {c}}});
                            InsertBefore(block, where, std::move(extract), context);

                            const uint32_t constCId = constMgr->GetUIntConstId(c);
                            const uint32_t acId = context->TakeNextId();
                            auto acInst = MakeUnique<Instruction>(
                                context, spv::Op::OpAccessChain, ptrScalarTypeId, acId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {decomposedPtrId}},
                                    {SPV_OPERAND_TYPE_ID, {constCId}}});
                            InsertBefore(block, where, std::move(acInst), context);

                            auto compStore = MakeUnique<Instruction>(
                                context, spv::Op::OpStore, 0, 0,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {acId}},
                                    {SPV_OPERAND_TYPE_ID, {extractId}}});
                            for (uint32_t opIdx = 2; opIdx < sourceStore->NumInOperands(); ++opIdx) {
                                compStore->AddOperand(Operand(sourceStore->GetInOperand(opIdx)));
                            }
                            InsertBefore(block, where, std::move(compStore), context);
                        }
                        return;
                    }

                    if (originalTypeInst->opcode() == spv::Op::OpTypeArray) {
                        const uint32_t originalElemTypeId = originalTypeInst->GetSingleWordInOperand(0);
                        const uint32_t vec3LeafId = FindVec3LeafTypeId(context, originalElemTypeId);
                        assert(vec3LeafId != 0);
                        const auto arr3It = vec3ToArr3.find(vec3LeafId);
                        assert(arr3It != vec3ToArr3.end());
                        const uint32_t decomposedElemTypeId =
                            originalElemTypeId == vec3LeafId
                                ? arr3It->second
                                : RebuildPointeeType(context, originalElemTypeId, arr3It->second, vec3ToArr3);
                        const uint32_t ptrDecomposedElemTypeId =
                            typeMgr->FindPointerToType(decomposedElemTypeId, spv::StorageClass::Workgroup);
                        const uint32_t length = GetArrayLength(context, originalTypeInst);
                        assert(length != 0);

                        for (uint32_t i = 0; i < length; ++i) {
                            const uint32_t extractId = context->TakeNextId();
                            auto extract = MakeUnique<Instruction>(
                                context, spv::Op::OpCompositeExtract, originalElemTypeId, extractId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {valueId}},
                                    {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i}}});
                            InsertBefore(block, where, std::move(extract), context);

                            const uint32_t constIId = constMgr->GetUIntConstId(i);
                            const uint32_t elemPtrId = context->TakeNextId();
                            auto elemAc = MakeUnique<Instruction>(
                                context, spv::Op::OpAccessChain, ptrDecomposedElemTypeId, elemPtrId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {decomposedPtrId}},
                                    {SPV_OPERAND_TYPE_ID, {constIId}}});
                            InsertBefore(block, where, std::move(elemAc), context);

                            BuildCompositeStore(context, block, where, originalElemTypeId, elemPtrId,
                                                extractId, vec3ToArr3, sourceStore);
                        }
                        return;
                    }

                    assert(false && "DecomposeWorkgroupVec3Pass: unsupported composite store type");
                }
            } // namespace

            spvtools::opt::Pass::Status DecomposeWorkgroupVec3Pass::Process() {
                using namespace spvtools;
                using namespace spvtools::opt;

                IRContext* const ctx = context();
                analysis::DefUseManager* const defUseMgr = ctx->get_def_use_mgr();
                analysis::TypeManager* const typeMgr = ctx->get_type_mgr();
                analysis::ConstantManager* const constMgr = ctx->get_constant_mgr();

                // =====================================================================
                // Phase 1: Build type mappings.
                //   vec3TypeIds: candidate OpTypeVector ids.
                //
                // Do not register replacement types until Phase 2 proves a Workgroup
                // variable really needs rewriting. SPIRV-Tools verifies that a pass
                // returning SuccessWithoutChange leaves the binary unchanged.
                // =====================================================================
                std::unordered_set<uint32_t> vec3TypeIds;
                std::unordered_map<uint32_t, uint32_t> vec3ToArr3;
                std::unordered_map<uint32_t, uint32_t> ptrCompositeToDecomposed;

                for (Instruction& typeInst : ctx->types_values()) {
                    if (typeInst.opcode() == spv::Op::OpTypeVector &&
                        IsVec3Type(&typeInst) != 0) {
                        vec3TypeIds.insert(typeInst.result_id());
                    }
                }

                if (vec3TypeIds.empty()) {
                    return Status::SuccessWithoutChange;
                }

                auto getOrCreateArr3ForVec3 = [&](uint32_t vec3TypeId) -> uint32_t {
                    auto existing = vec3ToArr3.find(vec3TypeId);
                    if (existing != vec3ToArr3.end()) {
                        return existing->second;
                    }

                    Instruction* vec3TypeInst = defUseMgr->GetDef(vec3TypeId);
                    assert(vec3TypeInst != nullptr);
                    const uint32_t scalarTypeId = vec3TypeInst->GetSingleWordInOperand(0);
                    analysis::Type* scalarType = typeMgr->GetType(scalarTypeId);
                    if (scalarType == nullptr) {
                        return 0;
                    }

                    const uint32_t const3Id = constMgr->GetUIntConstId(3);
                    analysis::Array::LengthInfo lengthInfo{
                        const3Id,
                        {analysis::Array::LengthInfo::kConstant, 3},
                    };
                    analysis::Array arrType(scalarType, lengthInfo);
                    analysis::Type* regArrType = typeMgr->GetRegisteredType(&arrType);
                    const uint32_t arrTypeId = typeMgr->GetTypeInstruction(regArrType);
                    vec3ToArr3[vec3TypeId] = arrTypeId;
                    return arrTypeId;
                };

                bool modified = false;

                // =====================================================================
                // Phase 2: Rebuild Workgroup variable pointee types.
                //   For each OpVariable in Workgroup storage class whose pointee contains
                //   a vec3 leaf, replace the pointee type with the decomposed version.
                // =====================================================================
                for (Instruction& varInst : ctx->types_values()) {
                    if (varInst.opcode() != spv::Op::OpVariable) {
                        continue;
                    }
                    const auto sc = static_cast<spv::StorageClass>(
                        varInst.GetSingleWordInOperand(0));
                    if (sc != spv::StorageClass::Workgroup) {
                        continue;
                    }
                    // varInst.type_id() is the OpTypePointer. Get pointee.
                    Instruction* ptrTypeInst = defUseMgr->GetDef(varInst.type_id());
                    const uint32_t pointeeId = ptrTypeInst->GetSingleWordInOperand(1);
                    // Quick check: only rebuild if pointee tree contains a vec3 leaf.
                    const uint32_t vec3LeafId = FindVec3LeafTypeId(ctx, pointeeId);
                    if (vec3LeafId == 0) {
                        continue;
                    }
                    const uint32_t arr3LeafId = getOrCreateArr3ForVec3(vec3LeafId);
                    if (arr3LeafId == 0) {
                        continue;
                    }
                    // If the pointee is itself a direct vec3 (no array wrapping), handle it
                    // via the vec3ToArr3 map directly.
                    uint32_t newPointeeId;
                    if (pointeeId == vec3LeafId) {
                        newPointeeId = arr3LeafId;
                    } else {
                        newPointeeId = RebuildPointeeType(ctx, pointeeId,
                                                          arr3LeafId, vec3ToArr3);
                    }
                    const uint32_t newPtrId = typeMgr->FindPointerToType(
                        newPointeeId, spv::StorageClass::Workgroup);
                    varInst.SetResultType(newPtrId);
                    defUseMgr->AnalyzeInstUse(&varInst);
                    modified = true;
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }

                for (Instruction& typeInst : ctx->types_values()) {
                    if (typeInst.opcode() != spv::Op::OpTypePointer) {
                        continue;
                    }
                    const auto sc = static_cast<spv::StorageClass>(
                        typeInst.GetSingleWordInOperand(0));
                    if (sc != spv::StorageClass::Workgroup) {
                        continue;
                    }
                    const uint32_t pointeeId = typeInst.GetSingleWordInOperand(1);
                    const uint32_t vec3LeafId = FindVec3LeafTypeId(ctx, pointeeId);
                    if (vec3LeafId == 0) {
                        continue;
                    }
                    auto it = vec3ToArr3.find(vec3LeafId);
                    if (it == vec3ToArr3.end()) {
                        continue;
                    }
                    const uint32_t newPointeeId =
                        pointeeId == vec3LeafId
                            ? it->second
                            : RebuildPointeeType(ctx, pointeeId, it->second, vec3ToArr3);
                    const uint32_t newPtrId =
                        typeMgr->FindPointerToType(newPointeeId, spv::StorageClass::Workgroup);
                    ptrCompositeToDecomposed[typeInst.result_id()] = newPtrId;
                }

                // =====================================================================
                // Phase 3: Rewrite access chain result types.
                //   Any OpAccessChain/OpInBoundsAccessChain whose result type is a
                //   Workgroup pointer whose pointee contains vec3 must now produce the
                //   decomposed pointer type. Access chains that go one level deeper to a
                //   scalar component already have result type ptr_scalar and are unaffected.
                // =====================================================================
                // We also need to handle chained access chains: an access chain whose
                // base is itself an access chain that was rewritten. Since the result
                // type of the rewritten chain changed, dependent chains need their result
                // type updated too if they were ptr_vec3. We iterate function bodies.
                std::vector<Instruction*> accessChainsToFix;
                for (auto& func : *ctx->module()) {
                    for (auto& bb : func) {
                        for (auto& inst : bb) {
                            if (inst.opcode() != spv::Op::OpAccessChain &&
                                inst.opcode() != spv::Op::OpInBoundsAccessChain) {
                                continue;
                            }
                            auto it = ptrCompositeToDecomposed.find(inst.type_id());
                            if (it != ptrCompositeToDecomposed.end()) {
                                accessChainsToFix.push_back(&inst);
                            }
                        }
                    }
                }
                for (Instruction* ac : accessChainsToFix) {
                    const uint32_t newTypeId = ptrCompositeToDecomposed[ac->type_id()];
                    ac->SetResultType(newTypeId);
                    defUseMgr->AnalyzeInstUse(ac);
                }

                // Build the set of decomposed pointer type ids for fast lookup when matching
                // loads/stores.
                std::unordered_set<uint32_t> decomposedPtrTypeIds;
                for (const auto& [oldPtr, newPtr] : ptrCompositeToDecomposed) {
                    decomposedPtrTypeIds.insert(newPtr);
                }

                // =====================================================================
                // Phase 4: Rewrite whole-composite OpLoad.
                //   OpLoad %v3float %ptr             -> vec3 component loads
                //   OpLoad %arr_v3float %ptr_to_row  -> per-element vec3 loads + array construct
                // =====================================================================
                std::vector<Instruction*> loadsToRewrite;
                for (auto& func : *ctx->module()) {
                    for (auto& bb : func) {
                        for (auto& inst : bb) {
                            if (inst.opcode() != spv::Op::OpLoad) {
                                continue;
                            }
                            if (FindVec3LeafTypeId(ctx, inst.type_id()) == 0) {
                                continue;
                            }
                            // Check the pointer operand's type.
                            const uint32_t ptrId = inst.GetSingleWordInOperand(0);
                            Instruction* ptrDef = defUseMgr->GetDef(ptrId);
                            if (ptrDef == nullptr) {
                                continue;
                            }
                            if (decomposedPtrTypeIds.find(ptrDef->type_id()) == decomposedPtrTypeIds.end()) {
                                continue;
                            }
                            loadsToRewrite.push_back(&inst);
                        }
                    }
                }

                for (Instruction* load : loadsToRewrite) {
                    BasicBlock* block = ctx->get_instr_block(load);
                    const uint32_t ptrId = load->GetSingleWordInOperand(0);
                    const uint32_t compositeId =
                        BuildCompositeLoad(ctx, block, load, load->type_id(), ptrId, vec3ToArr3, load);

                    ctx->ReplaceAllUsesWith(load->result_id(), compositeId);
                    ctx->KillNamesAndDecorates(load->result_id());
                    ctx->KillInst(load);
                }

                // =====================================================================
                // Phase 5: Rewrite whole-composite OpStore.
                //   OpStore %ptr %vec3val      -> per-component scalar stores
                //   OpStore %ptr %arr_v3float  -> per-element vec3 stores
                // =====================================================================
                std::vector<Instruction*> storesToRewrite;
                for (auto& func : *ctx->module()) {
                    for (auto& bb : func) {
                        for (auto& inst : bb) {
                            if (inst.opcode() != spv::Op::OpStore) {
                                continue;
                            }
                            const uint32_t ptrId = inst.GetSingleWordInOperand(0);
                            Instruction* ptrDef = defUseMgr->GetDef(ptrId);
                            if (ptrDef == nullptr) {
                                continue;
                            }
                            if (decomposedPtrTypeIds.find(ptrDef->type_id()) == decomposedPtrTypeIds.end()) {
                                continue;
                            }
                            const uint32_t valId = inst.GetSingleWordInOperand(1);
                            Instruction* valDef = defUseMgr->GetDef(valId);
                            if (valDef == nullptr || FindVec3LeafTypeId(ctx, valDef->type_id()) == 0) {
                                continue;
                            }
                            storesToRewrite.push_back(&inst);
                        }
                    }
                }

                for (Instruction* store : storesToRewrite) {
                    BasicBlock* block = ctx->get_instr_block(store);
                    const uint32_t ptrId = store->GetSingleWordInOperand(0);
                    const uint32_t valId = store->GetSingleWordInOperand(1);
                    Instruction* valDef = defUseMgr->GetDef(valId);
                    BuildCompositeStore(ctx, block, store, valDef->type_id(), ptrId, valId, vec3ToArr3, store);
                    ctx->KillInst(store);
                }

                // =====================================================================
                // Phase 6: Assert on unsupported atomic/CopyMemory on vec3 pointers.
                //   After phases 3-5, any remaining instruction whose pointer operand's
                //   type is a decomposed Workgroup pointer indicates an unsupported pattern.
                // =====================================================================
                for (auto& func : *ctx->module()) {
                    for (auto& bb : func) {
                        for (auto& inst : bb) {
                            const spv::Op op = inst.opcode();
                            bool isAtomic = (op == spv::Op::OpAtomicLoad ||
                                             op == spv::Op::OpAtomicStore ||
                                             op == spv::Op::OpAtomicExchange ||
                                             op == spv::Op::OpAtomicCompareExchange ||
                                             op == spv::Op::OpAtomicIAdd ||
                                             op == spv::Op::OpAtomicISub ||
                                             op == spv::Op::OpAtomicSMin ||
                                             op == spv::Op::OpAtomicUMin ||
                                             op == spv::Op::OpAtomicSMax ||
                                             op == spv::Op::OpAtomicUMax ||
                                             op == spv::Op::OpAtomicAnd ||
                                             op == spv::Op::OpAtomicOr ||
                                             op == spv::Op::OpAtomicXor);
                            bool isCopyMem = (op == spv::Op::OpCopyMemory ||
                                              op == spv::Op::OpCopyMemorySized);
                            if (!isAtomic && !isCopyMem) {
                                continue;
                            }
                            // Check pointer operands.
                            const uint32_t ptrOperandId = inst.GetSingleWordInOperand(0);
                            Instruction* ptrDef = defUseMgr->GetDef(ptrOperandId);
                            if (ptrDef == nullptr) {
                                continue;
                            }
                            if (decomposedPtrTypeIds.find(ptrDef->type_id()) !=
                                decomposedPtrTypeIds.end()) {
                                MOBILEGL_ASSERT(false,
                                    "DecomposeWorkgroupVec3Pass: unsupported atomic/CopyMemory "
                                    "on workgroup vec3 pointer");
                                return Status::Failure;
                            }
                        }
                    }
                }

                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            DecomposeWorkgroupVec3Pass::CreateDecomposeWorkgroupVec3Pass() {
                return spvtools::Optimizer::PassToken(MakeUnique<DecomposeWorkgroupVec3Pass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
