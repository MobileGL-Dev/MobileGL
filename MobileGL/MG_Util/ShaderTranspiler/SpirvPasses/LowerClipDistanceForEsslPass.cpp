// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerClipDistanceForEsslPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LowerClipDistanceForEsslPass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::BasicBlock;
                using spvtools::opt::Function;
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                spv::ExecutionModel EntryExecutionModel(IRContext* ctx) {
                    for (Instruction& ep : ctx->module()->entry_points()) {
                        return static_cast<spv::ExecutionModel>(ep.GetSingleWordInOperand(0));
                    }
                    return spv::ExecutionModel::Max;
                }

                uint32_t EntryFunctionId(IRContext* ctx) {
                    for (Instruction& ep : ctx->module()->entry_points()) {
                        // OpEntryPoint <model> <function> "name" <interface...>
                        return ep.GetSingleWordInOperand(1);
                    }
                    return 0;
                }

                uint32_t VariablePointeeType(IRContext* ctx, Instruction* var) {
                    Instruction* ptrType = ctx->get_def_use_mgr()->GetDef(var->type_id());
                    // OpTypePointer <storage-class> <pointee>
                    return ptrType->GetSingleWordInOperand(1);
                }

                uint32_t PointerTypeTo(IRContext* ctx, uint32_t pointeeId, spv::StorageClass sc) {
                    analysis::Type* pointee = ctx->get_type_mgr()->GetType(pointeeId);
                    analysis::Pointer ptr(pointee, sc);
                    return ctx->get_type_mgr()->GetTypeInstruction(&ptr);
                }

                uint32_t IntConstant(IRContext* ctx, bool isSigned, uint32_t value) {
                    analysis::Integer i(32, isSigned);
                    analysis::Type* reg = ctx->get_type_mgr()->GetRegisteredType(&i);
                    const analysis::Constant* c = ctx->get_constant_mgr()->GetConstant(reg, {value});
                    return ctx->get_constant_mgr()->GetDefiningInstruction(c)->result_id();
                }

                uint32_t UintType(IRContext* ctx) {
                    analysis::Integer i(32, false);
                    return ctx->get_type_mgr()->GetTypeInstruction(&i);
                }

                uint32_t BoolType(IRContext* ctx) {
                    analysis::Bool b;
                    return ctx->get_type_mgr()->GetTypeInstruction(&b);
                }

                // Constant length of OpTypeArray |arrayTypeId| (0 when not a sized constant).
                uint32_t ArrayLength(IRContext* ctx, uint32_t arrayTypeId) {
                    Instruction* arrayType = ctx->get_def_use_mgr()->GetDef(arrayTypeId);
                    if (arrayType == nullptr || arrayType->opcode() != spv::Op::OpTypeArray) {
                        return 0;
                    }
                    Instruction* length = ctx->get_def_use_mgr()->GetDef(arrayType->GetSingleWordInOperand(1));
                    if (length == nullptr || length->opcode() != spv::Op::OpConstant) {
                        return 0;
                    }
                    return length->GetSingleWordInOperand(0);
                }

                bool IsConstantWithValue(IRContext* ctx, uint32_t id, uint32_t value) {
                    Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
                    return def != nullptr && def->opcode() == spv::Op::OpConstant &&
                           def->GetSingleWordInOperand(0) == value;
                }

                bool IsAccessChain(const Instruction* inst) {
                    return inst->opcode() == spv::Op::OpAccessChain ||
                           inst->opcode() == spv::Op::OpInBoundsAccessChain;
                }

                Instruction* AddPrivateVariable(IRContext* ctx, uint32_t pointeeTypeId, const char* name) {
                    const uint32_t ptrType = PointerTypeTo(ctx, pointeeTypeId, spv::StorageClass::Private);
                    const uint32_t varId = ctx->TakeNextId();
                    ctx->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpVariable, ptrType, varId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Private)}}}));
                    ctx->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpName, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {varId}},
                            {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
                    return ctx->get_def_use_mgr()->GetDef(varId);
                }

                // Retargets |chain| onto |newBaseId|, dropping the first |dropIndexCount| index
                // operands and switching the result pointer's storage class to Private.
                void RetargetChainToPrivate(IRContext* ctx, Instruction* chain, uint32_t newBaseId,
                                            uint32_t dropIndexCount) {
                    Instruction* chainPtrType = ctx->get_def_use_mgr()->GetDef(chain->type_id());
                    const uint32_t pointeeId = chainPtrType->GetSingleWordInOperand(1);
                    const uint32_t newPtrType = PointerTypeTo(ctx, pointeeId, spv::StorageClass::Private);

                    ctx->ForgetUses(chain);
                    std::vector<Operand> newOperands;
                    newOperands.push_back({SPV_OPERAND_TYPE_ID, {newBaseId}});
                    for (uint32_t i = 1 + dropIndexCount; i < chain->NumInOperands(); ++i) {
                        newOperands.push_back(chain->GetInOperand(i));
                    }
                    chain->SetResultType(newPtrType);
                    chain->SetInOperands(std::move(newOperands));
                    ctx->AnalyzeUses(chain);
                }

                // ---- Output side --------------------------------------------------------------

                struct OutputTarget {
                    Instruction* var = nullptr;   // Output gl_PerVertex block or standalone builtin
                    bool isBlockMember = false;
                    uint32_t memberIndex = 0;     // valid when isBlockMember
                    uint32_t arrayTypeId = 0;     // float[N]
                    uint32_t elemTypeId = 0;      // float
                    uint32_t arrayLen = 0;        // N
                };

                // Inserts "gl_ClipDistance[k] = mg_ClipDistance[k]" for every literal k before
                // |before|. Constant-index writes are the only write shape Adreno links correctly.
                void InsertFlushBefore(IRContext* ctx, Instruction* before, const OutputTarget& target,
                                       uint32_t mgVarId) {
                    const uint32_t ptrPrivElem =
                        PointerTypeTo(ctx, target.elemTypeId, spv::StorageClass::Private);
                    const uint32_t ptrOutElem =
                        PointerTypeTo(ctx, target.elemTypeId, spv::StorageClass::Output);
                    for (uint32_t k = 0; k < target.arrayLen; ++k) {
                        const uint32_t kConst = IntConstant(ctx, true, k);
                        const uint32_t srcChainId = ctx->TakeNextId();
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpAccessChain, ptrPrivElem, srcChainId,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {mgVarId}},
                                                           {SPV_OPERAND_TYPE_ID, {kConst}}}));
                        const uint32_t valId = ctx->TakeNextId();
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpLoad, target.elemTypeId, valId,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {srcChainId}}}));
                        const uint32_t dstChainId = ctx->TakeNextId();
                        std::vector<Operand> dstOperands;
                        dstOperands.push_back({SPV_OPERAND_TYPE_ID, {target.var->result_id()}});
                        if (target.isBlockMember) {
                            dstOperands.push_back(
                                {SPV_OPERAND_TYPE_ID, {IntConstant(ctx, true, target.memberIndex)}});
                        }
                        dstOperands.push_back({SPV_OPERAND_TYPE_ID, {kConst}});
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpAccessChain, ptrOutElem, dstChainId, dstOperands));
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpStore, 0, 0,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {dstChainId}},
                                                           {SPV_OPERAND_TYPE_ID, {valId}}}));
                    }
                }

                bool LowerOutputClipDistance(IRContext* ctx, bool isGeometry) {
                    auto* defUse = ctx->get_def_use_mgr();

                    // Collect (struct type, member) pairs decorated BuiltIn ClipDistance and
                    // standalone variables decorated BuiltIn ClipDistance.
                    std::vector<std::pair<uint32_t, uint32_t>> memberTargets; // (structId, member)
                    std::vector<uint32_t> plainTargets;                       // variable ids
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpMemberDecorate && ann.NumInOperands() >= 4 &&
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) ==
                                spv::Decoration::BuiltIn &&
                            static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(3)) ==
                                spv::BuiltIn::ClipDistance) {
                            memberTargets.emplace_back(ann.GetSingleWordInOperand(0),
                                                       ann.GetSingleWordInOperand(1));
                        } else if (ann.opcode() == spv::Op::OpDecorate && ann.NumInOperands() >= 3 &&
                                   static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) ==
                                       spv::Decoration::BuiltIn &&
                                   static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(2)) ==
                                       spv::BuiltIn::ClipDistance) {
                            plainTargets.push_back(ann.GetSingleWordInOperand(0));
                        }
                    }

                    std::vector<OutputTarget> targets;
                    for (Instruction& inst : ctx->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable ||
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) !=
                                spv::StorageClass::Output) {
                            continue;
                        }
                        const uint32_t pointee = VariablePointeeType(ctx, &inst);
                        for (const auto& [structId, member] : memberTargets) {
                            if (pointee != structId) continue;
                            Instruction* structType = defUse->GetDef(structId);
                            if (structType == nullptr || member >= structType->NumInOperands()) continue;
                            OutputTarget target;
                            target.var = &inst;
                            target.isBlockMember = true;
                            target.memberIndex = member;
                            target.arrayTypeId = structType->GetSingleWordInOperand(member);
                            target.arrayLen = ArrayLength(ctx, target.arrayTypeId);
                            targets.push_back(target);
                        }
                        for (const uint32_t varId : plainTargets) {
                            if (inst.result_id() != varId) continue;
                            OutputTarget target;
                            target.var = &inst;
                            target.isBlockMember = false;
                            target.arrayTypeId = pointee;
                            target.arrayLen = ArrayLength(ctx, target.arrayTypeId);
                            targets.push_back(target);
                        }
                    }

                    bool changed = false;
                    for (OutputTarget& target : targets) {
                        if (target.arrayLen == 0) continue;
                        Instruction* arrayType = defUse->GetDef(target.arrayTypeId);
                        target.elemTypeId = arrayType->GetSingleWordInOperand(0);

                        // Collect the accesses to redirect. For the block form only chains whose
                        // leading index selects the ClipDistance member count; for the standalone
                        // form every chain plus whole-variable loads/stores.
                        std::vector<Instruction*> chains;
                        std::vector<Instruction*> directAccesses;
                        bool unsupportedUse = false;
                        defUse->ForEachUser(target.var, [&](Instruction* user) {
                            if (IsAccessChain(user) &&
                                user->GetSingleWordInOperand(0) == target.var->result_id()) {
                                if (target.isBlockMember) {
                                    if (user->NumInOperands() >= 2 &&
                                        IsConstantWithValue(ctx, user->GetSingleWordInOperand(1),
                                                            target.memberIndex)) {
                                        chains.push_back(user);
                                    }
                                } else {
                                    chains.push_back(user);
                                }
                            } else if (!target.isBlockMember) {
                                if (user->opcode() == spv::Op::OpLoad ||
                                    (user->opcode() == spv::Op::OpStore &&
                                     user->GetSingleWordInOperand(0) == target.var->result_id())) {
                                    directAccesses.push_back(user);
                                } else if (user->opcode() == spv::Op::OpCopyMemory) {
                                    unsupportedUse = true;
                                }
                            }
                        });
                        if (unsupportedUse || (chains.empty() && directAccesses.empty())) {
                            continue;
                        }

                        Instruction* mgVar = AddPrivateVariable(ctx, target.arrayTypeId, "mg_ClipDistance");
                        const uint32_t mgVarId = mgVar->result_id();

                        for (Instruction* chain : chains) {
                            const uint32_t dropCount = target.isBlockMember ? 1u : 0u;
                            if (chain->NumInOperands() == 1 + dropCount) {
                                // Pointer to the whole float[N]: reuse the private variable itself.
                                ctx->ReplaceAllUsesWith(chain->result_id(), mgVarId);
                                ctx->KillInst(chain);
                            } else {
                                RetargetChainToPrivate(ctx, chain, mgVarId, dropCount);
                            }
                        }
                        for (Instruction* access : directAccesses) {
                            ctx->ForgetUses(access);
                            access->SetInOperand(0, {mgVarId});
                            ctx->AnalyzeUses(access);
                        }

                        // Flush the shadow into the real builtin: geometry right before every
                        // EmitVertex, vertex before every return of the entry point. The flush is
                        // also what keeps the builtin statically used for cross-stage IO matching.
                        std::vector<Instruction*> flushSites;
                        if (isGeometry) {
                            for (Function& function : *ctx->module()) {
                                function.ForEachInst([&](Instruction* inst) {
                                    if (inst->opcode() == spv::Op::OpEmitVertex) {
                                        flushSites.push_back(inst);
                                    }
                                });
                            }
                        } else {
                            const uint32_t entryFuncId = EntryFunctionId(ctx);
                            for (Function& function : *ctx->module()) {
                                if (function.result_id() != entryFuncId) continue;
                                function.ForEachInst([&](Instruction* inst) {
                                    if (inst->opcode() == spv::Op::OpReturn ||
                                        inst->opcode() == spv::Op::OpReturnValue) {
                                        flushSites.push_back(inst);
                                    }
                                });
                            }
                        }
                        for (Instruction* site : flushSites) {
                            InsertFlushBefore(ctx, site, target, mgVarId);
                        }

                        changed = true;
                    }
                    return changed;
                }

                // ---- Input side (geometry gl_in) ----------------------------------------------

                bool LowerInputClipDistance(IRContext* ctx) {
                    auto* defUse = ctx->get_def_use_mgr();
                    auto* typeMgr = ctx->get_type_mgr();

                    // Locate the gl_in block member decorated ClipDistance.
                    Instruction* glInVar = nullptr;
                    uint32_t memberIndex = 0;
                    uint32_t arrayTypeId = 0; // float[N]
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() != spv::Op::OpMemberDecorate || ann.NumInOperands() < 4 ||
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) !=
                                spv::Decoration::BuiltIn ||
                            static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(3)) !=
                                spv::BuiltIn::ClipDistance) {
                            continue;
                        }
                        const uint32_t structId = ann.GetSingleWordInOperand(0);
                        const uint32_t member = ann.GetSingleWordInOperand(1);
                        for (Instruction& inst : ctx->module()->types_values()) {
                            if (inst.opcode() != spv::Op::OpVariable ||
                                static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) !=
                                    spv::StorageClass::Input) {
                                continue;
                            }
                            const uint32_t pointee = VariablePointeeType(ctx, &inst);
                            Instruction* pointeeType = defUse->GetDef(pointee);
                            if (pointeeType == nullptr || pointeeType->opcode() != spv::Op::OpTypeArray ||
                                pointeeType->GetSingleWordInOperand(0) != structId) {
                                continue;
                            }
                            Instruction* structType = defUse->GetDef(structId);
                            if (structType == nullptr || member >= structType->NumInOperands()) continue;
                            glInVar = &inst;
                            memberIndex = member;
                            arrayTypeId = structType->GetSingleWordInOperand(member);
                            break;
                        }
                        if (glInVar != nullptr) break;
                    }
                    if (glInVar == nullptr) {
                        return false;
                    }

                    const uint32_t clipCount = ArrayLength(ctx, arrayTypeId);
                    const uint32_t vertexCount = ArrayLength(ctx, VariablePointeeType(ctx, glInVar));
                    if (clipCount == 0 || vertexCount == 0) {
                        return false;
                    }

                    // Every gl_in chain that selects the ClipDistance member:
                    // (vertex, member) yields a whole float[N], (vertex, member, k) an element.
                    std::vector<Instruction*> chains;
                    defUse->ForEachUser(glInVar, [&](Instruction* user) {
                        if (IsAccessChain(user) && user->GetSingleWordInOperand(0) == glInVar->result_id() &&
                            user->NumInOperands() >= 3 &&
                            IsConstantWithValue(ctx, user->GetSingleWordInOperand(2), memberIndex)) {
                            chains.push_back(user);
                        }
                    });
                    if (chains.empty()) {
                        return false;
                    }

                    Instruction* arrayTypeInst = defUse->GetDef(arrayTypeId);
                    const uint32_t elemTypeId = arrayTypeInst->GetSingleWordInOperand(0);

                    // Private mg_ClipDistanceIn = float[vertexCount][clipCount].
                    const uint32_t vertexCountConst = IntConstant(ctx, false, vertexCount);
                    analysis::Type* innerType = typeMgr->GetType(arrayTypeId);
                    analysis::Array outerArray(
                        innerType, analysis::Array::LengthInfo{
                                       vertexCountConst,
                                       {analysis::Array::LengthInfo::kConstant, vertexCount}});
                    const uint32_t outerArrayTypeId = typeMgr->GetTypeInstruction(&outerArray);
                    Instruction* mgInVar = AddPrivateVariable(ctx, outerArrayTypeId, "mg_ClipDistanceIn");
                    const uint32_t mgInVarId = mgInVar->result_id();

                    // Copy loop at the top of the entry point:
                    //   for (uint t = 0; t < vertexCount * clipCount; ++t)
                    //       mg_ClipDistanceIn[t / clipCount][t % clipCount] =
                    //           gl_in[t / clipCount].gl_ClipDistance[t % clipCount];
                    // Both gl_in indices are loop-derived (dynamic): constant-index element reads
                    // miscompile and whole-array reads crash the Adreno compiler.
                    const uint32_t entryFuncId = EntryFunctionId(ctx);
                    Function* entryFn = nullptr;
                    for (Function& function : *ctx->module()) {
                        if (function.result_id() == entryFuncId) {
                            entryFn = &function;
                            break;
                        }
                    }
                    if (entryFn == nullptr || entryFn->begin() == entryFn->end()) {
                        return false;
                    }

                    const uint32_t uintTypeId = UintType(ctx);
                    const uint32_t boolTypeId = BoolType(ctx);
                    const uint32_t ptrFnUint = PointerTypeTo(ctx, uintTypeId, spv::StorageClass::Function);
                    const uint32_t ptrInElem = PointerTypeTo(ctx, elemTypeId, spv::StorageClass::Input);
                    const uint32_t ptrPrivElem = PointerTypeTo(ctx, elemTypeId, spv::StorageClass::Private);
                    const uint32_t uint0 = IntConstant(ctx, false, 0);
                    const uint32_t uint1 = IntConstant(ctx, false, 1);
                    const uint32_t uintN = IntConstant(ctx, false, clipCount);
                    const uint32_t uintTotal = IntConstant(ctx, false, vertexCount * clipCount);
                    const uint32_t memberConst = IntConstant(ctx, true, memberIndex);

                    BasicBlock* entryBlock = &*entryFn->begin();
                    auto splitPoint = entryBlock->begin();
                    while (splitPoint != entryBlock->end() &&
                           splitPoint->opcode() == spv::Op::OpVariable) {
                        ++splitPoint;
                    }

                    // Loop counter lives with the other function-local variables.
                    const uint32_t counterVarId = ctx->TakeNextId();
                    splitPoint->InsertBefore(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpVariable, ptrFnUint, counterVarId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Function)}}}));

                    const uint32_t restLabelId = ctx->TakeNextId();
                    BasicBlock* restBlock = entryBlock->SplitBasicBlock(ctx, restLabelId, splitPoint);

                    const uint32_t headerLabelId = ctx->TakeNextId();
                    const uint32_t checkLabelId = ctx->TakeNextId();
                    const uint32_t bodyLabelId = ctx->TakeNextId();
                    const uint32_t continueLabelId = ctx->TakeNextId();

                    auto makeBlock = [&](uint32_t labelId) {
                        return spvtools::MakeUnique<BasicBlock>(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpLabel, 0, labelId, std::initializer_list<Operand>{}));
                    };
                    auto addInst = [&](BasicBlock* block, spv::Op opcode, uint32_t typeId,
                                       uint32_t resultId, std::vector<Operand> operands) {
                        block->AddInstruction(spvtools::MakeUnique<Instruction>(
                            ctx, opcode, typeId, resultId, std::move(operands)));
                    };

                    // entry: t = 0; branch header
                    addInst(entryBlock, spv::Op::OpStore, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {counterVarId}}, {SPV_OPERAND_TYPE_ID, {uint0}}});
                    addInst(entryBlock, spv::Op::OpBranch, 0, 0, {{SPV_OPERAND_TYPE_ID, {headerLabelId}}});

                    // header: structured loop header
                    auto headerBlock = makeBlock(headerLabelId);
                    addInst(headerBlock.get(), spv::Op::OpLoopMerge, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {restLabelId}},
                             {SPV_OPERAND_TYPE_ID, {continueLabelId}},
                             {SPV_OPERAND_TYPE_LOOP_CONTROL,
                              {static_cast<uint32_t>(spv::LoopControlMask::MaskNone)}}});
                    addInst(headerBlock.get(), spv::Op::OpBranch, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {checkLabelId}}});

                    // check: t < vertexCount * clipCount ?
                    auto checkBlock = makeBlock(checkLabelId);
                    const uint32_t tCheckId = ctx->TakeNextId();
                    addInst(checkBlock.get(), spv::Op::OpLoad, uintTypeId, tCheckId,
                            {{SPV_OPERAND_TYPE_ID, {counterVarId}}});
                    const uint32_t condId = ctx->TakeNextId();
                    addInst(checkBlock.get(), spv::Op::OpULessThan, boolTypeId, condId,
                            {{SPV_OPERAND_TYPE_ID, {tCheckId}}, {SPV_OPERAND_TYPE_ID, {uintTotal}}});
                    addInst(checkBlock.get(), spv::Op::OpBranchConditional, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {condId}},
                             {SPV_OPERAND_TYPE_ID, {bodyLabelId}},
                             {SPV_OPERAND_TYPE_ID, {restLabelId}}});

                    // body: mg_ClipDistanceIn[t / N][t % N] = gl_in[t / N].gl_ClipDistance[t % N]
                    auto bodyBlock = makeBlock(bodyLabelId);
                    const uint32_t tBodyId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpLoad, uintTypeId, tBodyId,
                            {{SPV_OPERAND_TYPE_ID, {counterVarId}}});
                    const uint32_t vertexIdxId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpUDiv, uintTypeId, vertexIdxId,
                            {{SPV_OPERAND_TYPE_ID, {tBodyId}}, {SPV_OPERAND_TYPE_ID, {uintN}}});
                    const uint32_t clipIdxId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpUMod, uintTypeId, clipIdxId,
                            {{SPV_OPERAND_TYPE_ID, {tBodyId}}, {SPV_OPERAND_TYPE_ID, {uintN}}});
                    const uint32_t srcChainId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpAccessChain, ptrInElem, srcChainId,
                            {{SPV_OPERAND_TYPE_ID, {glInVar->result_id()}},
                             {SPV_OPERAND_TYPE_ID, {vertexIdxId}},
                             {SPV_OPERAND_TYPE_ID, {memberConst}},
                             {SPV_OPERAND_TYPE_ID, {clipIdxId}}});
                    const uint32_t valId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpLoad, elemTypeId, valId,
                            {{SPV_OPERAND_TYPE_ID, {srcChainId}}});
                    const uint32_t dstChainId = ctx->TakeNextId();
                    addInst(bodyBlock.get(), spv::Op::OpAccessChain, ptrPrivElem, dstChainId,
                            {{SPV_OPERAND_TYPE_ID, {mgInVarId}},
                             {SPV_OPERAND_TYPE_ID, {vertexIdxId}},
                             {SPV_OPERAND_TYPE_ID, {clipIdxId}}});
                    addInst(bodyBlock.get(), spv::Op::OpStore, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {dstChainId}}, {SPV_OPERAND_TYPE_ID, {valId}}});
                    addInst(bodyBlock.get(), spv::Op::OpBranch, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {continueLabelId}}});

                    // continue: ++t
                    auto continueBlock = makeBlock(continueLabelId);
                    const uint32_t tContinueId = ctx->TakeNextId();
                    addInst(continueBlock.get(), spv::Op::OpLoad, uintTypeId, tContinueId,
                            {{SPV_OPERAND_TYPE_ID, {counterVarId}}});
                    const uint32_t tIncId = ctx->TakeNextId();
                    addInst(continueBlock.get(), spv::Op::OpIAdd, uintTypeId, tIncId,
                            {{SPV_OPERAND_TYPE_ID, {tContinueId}}, {SPV_OPERAND_TYPE_ID, {uint1}}});
                    addInst(continueBlock.get(), spv::Op::OpStore, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {counterVarId}}, {SPV_OPERAND_TYPE_ID, {tIncId}}});
                    addInst(continueBlock.get(), spv::Op::OpBranch, 0, 0,
                            {{SPV_OPERAND_TYPE_ID, {headerLabelId}}});

                    BasicBlock* headerPtr = entryFn->InsertBasicBlockBefore(std::move(headerBlock), restBlock);
                    BasicBlock* checkPtr = entryFn->InsertBasicBlockAfter(std::move(checkBlock), headerPtr);
                    BasicBlock* bodyPtr = entryFn->InsertBasicBlockAfter(std::move(bodyBlock), checkPtr);
                    entryFn->InsertBasicBlockAfter(std::move(continueBlock), bodyPtr);

                    // Redirect the pre-existing accesses to the shadow copy.
                    for (Instruction* chain : chains) {
                        if (chain->NumInOperands() == 3) {
                            // (vertex, member): whole float[N] of one vertex.
                            Instruction* chainPtrType = defUse->GetDef(chain->type_id());
                            const uint32_t pointeeId = chainPtrType->GetSingleWordInOperand(1);
                            const uint32_t newPtrType =
                                PointerTypeTo(ctx, pointeeId, spv::StorageClass::Private);
                            ctx->ForgetUses(chain);
                            std::vector<Operand> newOperands;
                            newOperands.push_back({SPV_OPERAND_TYPE_ID, {mgInVarId}});
                            newOperands.push_back(chain->GetInOperand(1));
                            chain->SetResultType(newPtrType);
                            chain->SetInOperands(std::move(newOperands));
                            ctx->AnalyzeUses(chain);
                        } else {
                            // (vertex, member, k, ...): drop the member index.
                            Instruction* chainPtrType = defUse->GetDef(chain->type_id());
                            const uint32_t pointeeId = chainPtrType->GetSingleWordInOperand(1);
                            const uint32_t newPtrType =
                                PointerTypeTo(ctx, pointeeId, spv::StorageClass::Private);
                            ctx->ForgetUses(chain);
                            std::vector<Operand> newOperands;
                            newOperands.push_back({SPV_OPERAND_TYPE_ID, {mgInVarId}});
                            newOperands.push_back(chain->GetInOperand(1));
                            for (uint32_t i = 3; i < chain->NumInOperands(); ++i) {
                                newOperands.push_back(chain->GetInOperand(i));
                            }
                            chain->SetResultType(newPtrType);
                            chain->SetInOperands(std::move(newOperands));
                            ctx->AnalyzeUses(chain);
                        }
                    }

                    return true;
                }
            } // namespace

            spvtools::opt::Pass::Status LowerClipDistanceForEsslPass::Process() {
                auto* ctx = context();
                const spv::ExecutionModel model = EntryExecutionModel(ctx);
                const bool isVertex = model == spv::ExecutionModel::Vertex;
                const bool isGeometry = model == spv::ExecutionModel::Geometry;
                if (!isVertex && !isGeometry) {
                    return Status::SuccessWithoutChange;
                }

                bool changed = LowerOutputClipDistance(ctx, isGeometry);
                if (isGeometry) {
                    changed |= LowerInputClipDistance(ctx);
                }

                if (!changed) {
                    return Status::SuccessWithoutChange;
                }
                ctx->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            LowerClipDistanceForEsslPass::CreateLowerClipDistanceForEsslPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<LowerClipDistanceForEsslPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
