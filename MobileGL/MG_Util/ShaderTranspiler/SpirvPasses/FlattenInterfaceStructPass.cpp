// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenInterfaceStructPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FlattenInterfaceStructPass.h"

#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/string_utils.h"
#include "spirv.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace {
    using spvtools::opt::Instruction;
    using spvtools::opt::IRContext;
    using spvtools::opt::Operand;

    constexpr const char* kTargetTypeName = "DailyWeatherVariation";
    constexpr const char* kTargetInterfaceName = "daily_weather_variation";

    struct FlattenedMemberSpec {
        const char* name;
        uint32_t locationSpan;
    };

    constexpr std::array<FlattenedMemberSpec, 9> kFlattenedMembers = {{
        {"clouds_cumulus_coverage", 1},
        {"clouds_altocumulus_coverage", 1},
        {"clouds_cirrus_coverage", 1},
        {"clouds_cumulus_congestus_amount", 1},
        {"clouds_stratus_amount", 1},
        {"fogginess", 1},
        {"aurora_amount", 1},
        {"nlc_amount", 1},
        {"aurora_colors", 2},
    }};

    struct FlattenedMemberVariable {
        uint32_t memberIndex;
        uint32_t typeId;
        uint32_t location;
        Instruction* variable;
    };

    bool HasExactName(IRContext* context, uint32_t id, const char* expected) {
        auto names = context->GetNames(id);
        for (auto it = names.begin(); it != names.end(); ++it) {
            Instruction* const nameInst = it->second;
            if (nameInst->opcode() == spv::Op::OpName && nameInst->GetInOperand(1).AsString() == expected) {
                return true;
            }
        }
        return false;
    }

    bool HasExactMemberName(IRContext* context, uint32_t structTypeId, uint32_t memberIndex, const char* expected) {
        Instruction* const nameInst = context->GetMemberName(structTypeId, memberIndex);
        return nameInst != nullptr && nameInst->GetInOperand(2).AsString() == expected;
    }

    bool IsFloat32Type(const Instruction* typeInst) {
        return typeInst != nullptr && typeInst->opcode() == spv::Op::OpTypeFloat &&
               typeInst->GetSingleWordInOperand(0) == 32;
    }

    bool IsFloatVector(const Instruction* typeInst, spvtools::opt::analysis::DefUseManager* defUseMgr,
                       uint32_t componentCount) {
        if (typeInst == nullptr || typeInst->opcode() != spv::Op::OpTypeVector) {
            return false;
        }

        if (typeInst->GetSingleWordInOperand(1) != componentCount) {
            return false;
        }

        return IsFloat32Type(defUseMgr->GetDef(typeInst->GetSingleWordInOperand(0)));
    }

    bool IsMat2x3Float(const Instruction* typeInst, spvtools::opt::analysis::DefUseManager* defUseMgr) {
        if (typeInst == nullptr || typeInst->opcode() != spv::Op::OpTypeMatrix) {
            return false;
        }

        if (typeInst->GetSingleWordInOperand(1) != 2) {
            return false;
        }

        return IsFloatVector(defUseMgr->GetDef(typeInst->GetSingleWordInOperand(0)), defUseMgr, 3);
    }

    bool MatchesMemberType(const Instruction* typeInst, spvtools::opt::analysis::DefUseManager* defUseMgr,
                           size_t memberIndex) {
        switch (memberIndex) {
        case 0:
        case 1:
        case 2:
            return IsFloatVector(typeInst, defUseMgr, 2);
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            return IsFloat32Type(typeInst);
        case 8:
            return IsMat2x3Float(typeInst, defUseMgr);
        default:
            return false;
        }
    }

    bool MatchesTargetStruct(IRContext* context, spvtools::opt::analysis::DefUseManager* defUseMgr,
                             const Instruction* typeInst) {
        if (typeInst == nullptr || typeInst->opcode() != spv::Op::OpTypeStruct ||
            typeInst->NumInOperands() != kFlattenedMembers.size()) {
            return false;
        }

        if (!HasExactName(context, typeInst->result_id(), kTargetTypeName)) {
            return false;
        }

        for (size_t memberIndex = 0; memberIndex < kFlattenedMembers.size(); ++memberIndex) {
            const uint32_t memberTypeId = typeInst->GetSingleWordInOperand(static_cast<uint32_t>(memberIndex));
            if (!MatchesMemberType(defUseMgr->GetDef(memberTypeId), defUseMgr, memberIndex)) {
                return false;
            }

            if (!HasExactMemberName(context, typeInst->result_id(), static_cast<uint32_t>(memberIndex),
                                    kFlattenedMembers[memberIndex].name)) {
                return false;
            }
        }

        return true;
    }

    Instruction* GetPointeeType(spvtools::opt::analysis::DefUseManager* defUseMgr, const Instruction* variable) {
        Instruction* const pointerType = defUseMgr->GetDef(variable->type_id());
        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
            return nullptr;
        }

        return defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
    }

    bool ValidateAccessChainUse(IRContext* context, spvtools::opt::analysis::DefUseManager* defUseMgr,
                                const Instruction* accessChain) {
        if (accessChain->NumInOperands() < 2) {
            return false;
        }

        const Instruction* const indexInst = defUseMgr->GetDef(accessChain->GetSingleWordInOperand(1));
        const auto* const indexConst = context->get_constant_mgr()->GetConstantFromInst(indexInst);
        if (indexConst == nullptr) {
            return false;
        }

        const int64_t memberIndex = indexConst->GetSignExtendedValue();
        return memberIndex >= 0 && memberIndex < static_cast<int64_t>(kFlattenedMembers.size());
    }

    int64_t GetAccessChainMemberIndex(IRContext* context, spvtools::opt::analysis::DefUseManager* defUseMgr,
                                      const Instruction* accessChain) {
        const Instruction* const indexInst = defUseMgr->GetDef(accessChain->GetSingleWordInOperand(1));
        return context->get_constant_mgr()->GetConstantFromInst(indexInst)->GetSignExtendedValue();
    }

    void AddName(IRContext* context, uint32_t id, const std::string& name) {
        context->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
            context, spv::Op::OpName, 0, 0,
            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {id}},
                                           {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
    }

    void AddLocationDecoration(IRContext* context, uint32_t id, uint32_t location) {
        context->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
            context, spv::Op::OpDecorate, 0, 0,
            std::initializer_list<Operand>{
                {SPV_OPERAND_TYPE_ID, {id}},
                {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(spv::Decoration::Location)}},
                {SPV_OPERAND_TYPE_LITERAL_INTEGER, {location}}}));
    }

    std::optional<uint32_t> GetVariableDecorationLiteral(IRContext* context, uint32_t id, spv::Decoration kind) {
        for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(id, false)) {
            if (decoration->opcode() != spv::Op::OpDecorate ||
                static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(1)) != kind ||
                decoration->NumInOperands() < 3) {
                continue;
            }

            return decoration->GetSingleWordInOperand(2);
        }

        return std::nullopt;
    }

    std::optional<uint32_t> GetMemberDecorationLiteral(IRContext* context, uint32_t structTypeId, uint32_t memberIndex,
                                                       spv::Decoration kind) {
        for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(structTypeId, false)) {
            if (decoration->opcode() != spv::Op::OpMemberDecorate ||
                decoration->GetSingleWordInOperand(1) != memberIndex ||
                static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(2)) != kind ||
                decoration->NumInOperands() < 4) {
                continue;
            }

            return decoration->GetSingleWordInOperand(3);
        }

        return std::nullopt;
    }

    void CloneVariableDecorations(IRContext* context, const Instruction* source, Instruction* target) {
        for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(source->result_id(), false)) {
            if (decoration->opcode() != spv::Op::OpDecorate) {
                continue;
            }

            if (static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(1)) == spv::Decoration::Location) {
                continue;
            }

            std::unique_ptr<Instruction> clone(decoration->Clone(context));
            clone->SetInOperand(0, {target->result_id()});
            context->AddAnnotationInst(std::move(clone));
        }
    }

    void CloneMemberDecorations(IRContext* context, const Instruction* structType, uint32_t memberIndex,
                                Instruction* target) {
        for (Instruction* decoration :
             context->get_decoration_mgr()->GetDecorationsFor(structType->result_id(), false)) {
            if (decoration->opcode() != spv::Op::OpMemberDecorate ||
                decoration->GetSingleWordInOperand(1) != memberIndex) {
                continue;
            }

            const auto kind = static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(2));
            if (kind == spv::Decoration::Location) {
                continue;
            }

            auto newDecoration = spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpDecorate, 0, 0,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {target->result_id()}},
                                               {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(kind)}}});
            for (uint32_t operandIndex = 3; operandIndex < decoration->NumInOperands(); ++operandIndex) {
                newDecoration->AddOperand(Operand(decoration->GetInOperand(operandIndex)));
            }
            context->AddAnnotationInst(std::move(newDecoration));
        }
    }

    Instruction* CreateFlattenedVariable(IRContext* context, spvtools::opt::analysis::DefUseManager* defUseMgr,
                                         spvtools::opt::analysis::TypeManager* typeMgr,
                                         const Instruction* sourceVariable, const Instruction* structType,
                                         uint32_t memberIndex, uint32_t location) {
        const auto storageClass = static_cast<spv::StorageClass>(sourceVariable->GetSingleWordInOperand(0));
        const uint32_t memberTypeId = structType->GetSingleWordInOperand(memberIndex);
        const uint32_t pointerTypeId = typeMgr->FindPointerToType(memberTypeId, storageClass);
        const uint32_t resultId = context->module()->TakeNextIdBound();

        auto variable = spvtools::MakeUnique<Instruction>(
            context, spv::Op::OpVariable, pointerTypeId, resultId,
            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<uint32_t>(storageClass)}}});
        Instruction* const variablePtr = variable.get();
        context->AddGlobalValue(std::move(variable));
        context->AnalyzeDefUse(variablePtr);

        CloneVariableDecorations(context, sourceVariable, variablePtr);
        CloneMemberDecorations(context, structType, memberIndex, variablePtr);
        AddLocationDecoration(context, resultId, location);

        std::string name = kTargetInterfaceName;
        name += "_";
        name += kFlattenedMembers[memberIndex].name;
        AddName(context, resultId, name);

        return variablePtr;
    }

    bool ReplaceAccessChain(IRContext* context, spvtools::opt::analysis::DefUseManager* defUseMgr,
                            Instruction* accessChain, const std::vector<FlattenedMemberVariable>& replacements) {
        const int64_t memberIndex = GetAccessChainMemberIndex(context, defUseMgr, accessChain);
        const Instruction* const replacementVar = replacements[static_cast<size_t>(memberIndex)].variable;

        if (accessChain->NumInOperands() > 2) {
            const uint32_t replacementId = context->module()->TakeNextIdBound();
            auto replacementChain = spvtools::MakeUnique<Instruction>(
                context, accessChain->opcode(), accessChain->type_id(), replacementId,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {replacementVar->result_id()}}});

            for (uint32_t operandIndex = 2; operandIndex < accessChain->NumInOperands(); ++operandIndex) {
                replacementChain->AddOperand(Operand(accessChain->GetInOperand(operandIndex)));
            }

            replacementChain->UpdateDebugInfoFrom(accessChain);
            auto insertPoint =
                spvtools::opt::BasicBlock::iterator(accessChain).InsertBefore(std::move(replacementChain));
            context->AnalyzeDefUse(&*insertPoint);
            context->set_instr_block(&*insertPoint, context->get_instr_block(accessChain));
            context->ReplaceAllUsesWith(accessChain->result_id(), replacementId);
        } else {
            context->ReplaceAllUsesWith(accessChain->result_id(), replacementVar->result_id());
        }

        context->KillNamesAndDecorates(accessChain->result_id());
        context->KillInst(accessChain);
        return true;
    }

    bool ReplaceWholeLoad(IRContext* context, Instruction* load,
                          const std::vector<FlattenedMemberVariable>& replacements) {
        spvtools::opt::BasicBlock* const block = context->get_instr_block(load);
        std::vector<Instruction*> memberLoads;
        memberLoads.reserve(replacements.size());

        auto where = spvtools::opt::BasicBlock::iterator(load);
        for (const auto& replacement : replacements) {
            const uint32_t loadId = context->module()->TakeNextIdBound();
            auto memberLoad = spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpLoad, replacement.typeId, loadId,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {replacement.variable->result_id()}}});
            for (uint32_t operandIndex = 1; operandIndex < load->NumInOperands(); ++operandIndex) {
                memberLoad->AddOperand(Operand(load->GetInOperand(operandIndex)));
            }
            where = where.InsertBefore(std::move(memberLoad));
            where->UpdateDebugInfoFrom(load);
            context->AnalyzeDefUse(&*where);
            context->set_instr_block(&*where, block);
            memberLoads.push_back(&*where);
        }

        const uint32_t compositeId = context->module()->TakeNextIdBound();
        auto compositeConstruct = spvtools::MakeUnique<Instruction>(
            context, spv::Op::OpCompositeConstruct, load->type_id(), compositeId, std::initializer_list<Operand>{});
        for (Instruction* memberLoad : memberLoads) {
            compositeConstruct->AddOperand({SPV_OPERAND_TYPE_ID, {memberLoad->result_id()}});
        }

        where = spvtools::opt::BasicBlock::iterator(load).InsertBefore(std::move(compositeConstruct));
        where->UpdateDebugInfoFrom(load);
        context->AnalyzeDefUse(&*where);
        context->set_instr_block(&*where, block);
        context->ReplaceAllUsesWith(load->result_id(), compositeId);
        context->KillNamesAndDecorates(load->result_id());
        context->KillInst(load);
        return true;
    }

    bool ReplaceWholeStore(IRContext* context, Instruction* store,
                           const std::vector<FlattenedMemberVariable>& replacements) {
        spvtools::opt::BasicBlock* const block = context->get_instr_block(store);
        auto where = spvtools::opt::BasicBlock::iterator(store);
        const uint32_t storedValueId = store->GetSingleWordInOperand(1);

        for (const auto& replacement : replacements) {
            const uint32_t extractId = context->module()->TakeNextIdBound();
            auto extract = spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpCompositeExtract, replacement.typeId, extractId,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {storedValueId}},
                                               {SPV_OPERAND_TYPE_LITERAL_INTEGER, {replacement.memberIndex}}});
            auto iter = where.InsertBefore(std::move(extract));
            iter->UpdateDebugInfoFrom(store);
            context->AnalyzeDefUse(&*iter);
            context->set_instr_block(&*iter, block);

            auto memberStore = spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpStore, 0, 0,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {replacement.variable->result_id()}},
                                               {SPV_OPERAND_TYPE_ID, {extractId}}});
            for (uint32_t operandIndex = 2; operandIndex < store->NumInOperands(); ++operandIndex) {
                memberStore->AddOperand(Operand(store->GetInOperand(operandIndex)));
            }
            iter = where.InsertBefore(std::move(memberStore));
            iter->UpdateDebugInfoFrom(store);
            context->AnalyzeDefUse(&*iter);
            context->set_instr_block(&*iter, block);
        }

        context->KillInst(store);
        return true;
    }

    void RewriteEntryPoints(Instruction* targetVariable, const std::vector<FlattenedMemberVariable>& replacements,
                            spvtools::opt::Module* module) {
        for (Instruction& entryPoint : module->entry_points()) {
            bool replaced = false;
            std::vector<Operand> newOperands;
            newOperands.reserve(entryPoint.NumInOperands() + replacements.size());
            for (uint32_t operandIndex = 0; operandIndex < entryPoint.NumInOperands(); ++operandIndex) {
                if (operandIndex < 3) {
                    newOperands.push_back(entryPoint.GetInOperand(operandIndex));
                    continue;
                }

                if (entryPoint.GetSingleWordInOperand(operandIndex) != targetVariable->result_id()) {
                    newOperands.push_back(entryPoint.GetInOperand(operandIndex));
                    continue;
                }

                replaced = true;
                for (const auto& replacement : replacements) {
                    newOperands.push_back({SPV_OPERAND_TYPE_ID, {replacement.variable->result_id()}});
                }
            }

            if (replaced) {
                entryPoint.SetInOperands(std::move(newOperands));
            }
        }
    }
} // namespace

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            spvtools::opt::Pass::Status FlattenInterfaceStructPass::Process() {
                using namespace spvtools;
                using namespace spvtools::opt;

                IRContext* const irContext = context();
                analysis::DefUseManager* const defUseMgr = irContext->get_def_use_mgr();
                analysis::TypeManager* const typeMgr = irContext->get_type_mgr();

                Instruction* targetVariable = nullptr;
                Instruction* targetStructType = nullptr;
                for (Instruction& globalInst : get_module()->types_values()) {
                    if (globalInst.opcode() != spv::Op::OpVariable) {
                        continue;
                    }

                    const auto storageClass = static_cast<spv::StorageClass>(globalInst.GetSingleWordInOperand(0));
                    if (storageClass != spv::StorageClass::Input && storageClass != spv::StorageClass::Output) {
                        continue;
                    }

                    Instruction* const pointeeType = GetPointeeType(defUseMgr, &globalInst);
                    if (!MatchesTargetStruct(irContext, defUseMgr, pointeeType) ||
                        !HasExactName(irContext, globalInst.result_id(), kTargetInterfaceName)) {
                        continue;
                    }

                    targetVariable = &globalInst;
                    targetStructType = pointeeType;
                    break;
                }

                if (targetVariable == nullptr || targetStructType == nullptr) {
                    return Status::SuccessWithoutChange;
                }

                std::vector<Instruction*> directAccessChains;
                std::vector<Instruction*> directLoads;
                std::vector<Instruction*> directStores;
                const bool supportedUsers = defUseMgr->WhileEachUser(targetVariable, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpAccessChain:
                    case spv::Op::OpInBoundsAccessChain:
                        directAccessChains.push_back(user);
                        return ValidateAccessChainUse(irContext, defUseMgr, user);
                    case spv::Op::OpLoad:
                        directLoads.push_back(user);
                        return true;
                    case spv::Op::OpStore:
                        directStores.push_back(user);
                        return true;
                    case spv::Op::OpName:
                    case spv::Op::OpDecorate:
                    case spv::Op::OpEntryPoint:
                        return true;
                    default:
                        return false;
                    }
                });
                if (!supportedUsers) {
                    return Status::SuccessWithoutChange;
                }

                std::vector<FlattenedMemberVariable> replacements;
                replacements.reserve(kFlattenedMembers.size());
                uint32_t locationCursor =
                    GetVariableDecorationLiteral(irContext, targetVariable->result_id(), spv::Decoration::Location)
                        .value_or(0u);
                for (uint32_t memberIndex = 0; memberIndex < static_cast<uint32_t>(kFlattenedMembers.size());
                     ++memberIndex) {
                    const uint32_t location = GetMemberDecorationLiteral(irContext, targetStructType->result_id(),
                                                                         memberIndex, spv::Decoration::Location)
                                                  .value_or(locationCursor);
                    Instruction* const memberVar = CreateFlattenedVariable(
                        irContext, defUseMgr, typeMgr, targetVariable, targetStructType, memberIndex, location);
                    replacements.push_back(
                        {memberIndex, targetStructType->GetSingleWordInOperand(memberIndex), location, memberVar});
                    locationCursor = location + kFlattenedMembers[memberIndex].locationSpan;
                }

                RewriteEntryPoints(targetVariable, replacements, get_module());

                for (Instruction* accessChain : directAccessChains) {
                    ReplaceAccessChain(irContext, defUseMgr, accessChain, replacements);
                }

                for (Instruction* load : directLoads) {
                    ReplaceWholeLoad(irContext, load, replacements);
                }

                for (Instruction* store : directStores) {
                    ReplaceWholeStore(irContext, store, replacements);
                }

                irContext->KillNamesAndDecorates(targetVariable->result_id());
                irContext->KillInst(targetVariable);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken FlattenInterfaceStructPass::CreateFlattenInterfaceStructPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<FlattenInterfaceStructPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL