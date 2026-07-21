// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateNoPerspectivePass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EmulateNoPerspectivePass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <algorithm>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
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

                uint32_t VariablePointeeType(IRContext* ctx, Instruction* var) {
                    Instruction* ptrType = ctx->get_def_use_mgr()->GetDef(var->type_id());
                    // OpTypePointer <storage-class> <pointee>
                    return ptrType->GetSingleWordInOperand(1);
                }

                // If |typeId| is float or a vector of float, returns true and reports the scalar float
                // type and whether it is a vector. Matrices, structs, ints etc. are not emulatable.
                bool IsFloatScalarOrVector(IRContext* ctx, uint32_t typeId, uint32_t& floatTypeId, bool& isVector) {
                    Instruction* t = ctx->get_def_use_mgr()->GetDef(typeId);
                    if (t == nullptr) return false;
                    if (t->opcode() == spv::Op::OpTypeFloat) {
                        floatTypeId = typeId;
                        isVector = false;
                        return true;
                    }
                    if (t->opcode() == spv::Op::OpTypeVector) {
                        const uint32_t comp = t->GetSingleWordInOperand(0);
                        Instruction* ct = ctx->get_def_use_mgr()->GetDef(comp);
                        if (ct != nullptr && ct->opcode() == spv::Op::OpTypeFloat) {
                            floatTypeId = comp;
                            isVector = true;
                            return true;
                        }
                    }
                    return false;
                }

                uint32_t PointerTypeTo(IRContext* ctx, uint32_t pointeeId, spv::StorageClass sc) {
                    analysis::Type* pointee = ctx->get_type_mgr()->GetType(pointeeId);
                    analysis::Pointer ptr(pointee, sc);
                    return ctx->get_type_mgr()->GetTypeInstruction(&ptr);
                }

                uint32_t V4FloatType(IRContext* ctx) {
                    analysis::Float f(32);
                    analysis::Type* freg = ctx->get_type_mgr()->GetRegisteredType(&f);
                    analysis::Vector v(freg, 4);
                    return ctx->get_type_mgr()->GetTypeInstruction(&v);
                }

                uint32_t FloatType(IRContext* ctx) {
                    analysis::Float f(32);
                    return ctx->get_type_mgr()->GetTypeInstruction(&f);
                }

                uint32_t SignedIntConstant(IRContext* ctx, int32_t value) {
                    analysis::Integer i(32, true);
                    analysis::Type* reg = ctx->get_type_mgr()->GetRegisteredType(&i);
                    const analysis::Constant* c =
                        ctx->get_constant_mgr()->GetConstant(reg, {static_cast<uint32_t>(value)});
                    return ctx->get_constant_mgr()->GetDefiningInstruction(c)->result_id();
                }

                // Multiply |valueId| (of type |valueTypeId|) by the scalar |scalarId|, inserting the op
                // before |before|. Returns the product's id.
                uint32_t InsertScale(IRContext* ctx, Instruction* before, uint32_t valueTypeId,
                                     uint32_t valueId, uint32_t scalarId, bool isVector) {
                    const uint32_t productId = ctx->TakeNextId();
                    const spv::Op op = isVector ? spv::Op::OpVectorTimesScalar : spv::Op::OpFMul;
                    before->InsertBefore(spvtools::MakeUnique<Instruction>(
                        ctx, op, valueTypeId, productId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {valueId}},
                                                       {SPV_OPERAND_TYPE_ID, {scalarId}}}));
                    return productId;
                }

                // --- Vertex stage: gl_Position discovery ------------------------------------------

                // Finds gl_Position as member |memberIndex| of a gl_PerVertex-style block whose Output
                // variable is |blockVarId|; |v4floatTypeId| is that member's (vec4) type. Returns false
                // if gl_Position is not a block member (older plain-variable form is left to the strip).
                bool FindPositionBlock(IRContext* ctx, uint32_t& blockVarId, uint32_t& memberIndex,
                                       uint32_t& v4floatTypeId) {
                    uint32_t structId = 0;
                    uint32_t member = 0;
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpMemberDecorate && ann.NumInOperands() >= 4 &&
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) ==
                                spv::Decoration::BuiltIn &&
                            static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(3)) ==
                                spv::BuiltIn::Position) {
                            structId = ann.GetSingleWordInOperand(0);
                            member = ann.GetSingleWordInOperand(1);
                            break;
                        }
                    }
                    if (structId == 0) return false;

                    Instruction* structType = ctx->get_def_use_mgr()->GetDef(structId);
                    if (structType == nullptr || member >= structType->NumInOperands()) return false;
                    v4floatTypeId = structType->GetSingleWordInOperand(member);

                    for (Instruction& inst : ctx->module()->types_values()) {
                        if (inst.opcode() == spv::Op::OpVariable &&
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Output &&
                            VariablePointeeType(ctx, &inst) == structId) {
                            blockVarId = inst.result_id();
                            memberIndex = member;
                            return true;
                        }
                    }
                    return false;
                }

                // --- Fragment stage: gl_FragCoord discovery/synthesis -----------------------------

                Instruction* FindBuiltinInput(IRContext* ctx, spv::BuiltIn builtin) {
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() != spv::Op::OpDecorate || ann.NumInOperands() < 3) continue;
                        if (static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn)
                            continue;
                        if (static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(2)) != builtin) continue;
                        Instruction* var = ctx->get_def_use_mgr()->GetDef(ann.GetSingleWordInOperand(0));
                        if (var != nullptr && var->opcode() == spv::Op::OpVariable &&
                            static_cast<spv::StorageClass>(var->GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Input) {
                            return var;
                        }
                    }
                    return nullptr;
                }

                uint32_t SynthesizeFragCoord(IRContext* ctx, uint32_t v4floatTypeId) {
                    const uint32_t ptrType = PointerTypeTo(ctx, v4floatTypeId, spv::StorageClass::Input);
                    const uint32_t varId = ctx->TakeNextId();
                    ctx->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpVariable, ptrType, varId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Input)}}}));
                    ctx->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {varId}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::BuiltIn)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER,
                             {static_cast<uint32_t>(spv::BuiltIn::FragCoord)}}}));
                    for (Instruction& ep : ctx->module()->entry_points()) {
                        ep.AddOperand({SPV_OPERAND_TYPE_ID, {varId}});
                    }
                    return varId;
                }
            } // namespace

            spvtools::opt::Pass::Status EmulateNoPerspectivePass::Process() {
                auto* ctx = context();
                const spv::ExecutionModel model = EntryExecutionModel(ctx);
                const bool isVertex = model == spv::ExecutionModel::Vertex;
                const bool isFragment = model == spv::ExecutionModel::Fragment;

                // Collect NoPerspective-decorated plain variables and every NoPerspective annotation.
                std::vector<uint32_t> plainVarIds;
                std::vector<Instruction*> decorationsToKill;
                for (Instruction& ann : ctx->annotations()) {
                    if (ann.opcode() == spv::Op::OpDecorate && ann.NumInOperands() >= 2 &&
                        static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) ==
                            spv::Decoration::NoPerspective) {
                        plainVarIds.push_back(ann.GetSingleWordInOperand(0));
                        decorationsToKill.push_back(&ann);
                    } else if (ann.opcode() == spv::Op::OpMemberDecorate && ann.NumInOperands() >= 3 &&
                               static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) ==
                                   spv::Decoration::NoPerspective) {
                        // Block-member noperspective is not emulated here; the decoration is stripped
                        // (smooth fallback) so SPIRV-Cross does not require the NV extension.
                        decorationsToKill.push_back(&ann);
                    }
                }

                if (decorationsToKill.empty()) {
                    return Status::SuccessWithoutChange;
                }

                const spv::StorageClass wantStorage =
                    isVertex ? spv::StorageClass::Output : spv::StorageClass::Input;

                // Emulatable = plain variable of the stage's interface direction, float or floatN.
                struct Target {
                    Instruction* var;
                    uint32_t typeId;
                    uint32_t floatTypeId;
                    bool isVector;
                };
                std::vector<Target> targets;
                if (isVertex || isFragment) {
                    for (const uint32_t id : plainVarIds) {
                        Instruction* var = ctx->get_def_use_mgr()->GetDef(id);
                        if (var == nullptr || var->opcode() != spv::Op::OpVariable) continue;
                        if (static_cast<spv::StorageClass>(var->GetSingleWordInOperand(0)) != wantStorage)
                            continue;
                        const uint32_t pointee = VariablePointeeType(ctx, var);
                        uint32_t floatTypeId = 0;
                        bool isVector = false;
                        if (IsFloatScalarOrVector(ctx, pointee, floatTypeId, isVector)) {
                            targets.push_back({var, pointee, floatTypeId, isVector});
                        }
                    }
                }

                // Force highp on the varyings we emulate: the a*w round-trip overflows a mediump (fp16)
                // varying at large clip-space w. Dropping RelaxedPrecision makes SPIRV-Cross emit them
                // highp on both stages, keeping the emulation exact. Only touches emulated variables.
                if (!targets.empty()) {
                    std::vector<uint32_t> targetIds;
                    targetIds.reserve(targets.size());
                    for (const Target& t : targets) targetIds.push_back(t.var->result_id());
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpDecorate && ann.NumInOperands() >= 2 &&
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) ==
                                spv::Decoration::RelaxedPrecision &&
                            std::find(targetIds.begin(), targetIds.end(),
                                      ann.GetSingleWordInOperand(0)) != targetIds.end()) {
                            decorationsToKill.push_back(&ann);
                        }
                    }
                }

                if (isVertex && !targets.empty()) {
                    uint32_t blockVarId = 0;
                    uint32_t memberIndex = 0;
                    uint32_t v4floatTypeId = 0;
                    if (FindPositionBlock(ctx, blockVarId, memberIndex, v4floatTypeId)) {
                        const uint32_t ptrOutV4 =
                            PointerTypeTo(ctx, v4floatTypeId, spv::StorageClass::Output);
                        const uint32_t memberConst = SignedIntConstant(ctx, static_cast<int32_t>(memberIndex));
                        const uint32_t floatTy = FloatType(ctx);

                        uint32_t entryFuncId = 0;
                        for (Instruction& ep : ctx->module()->entry_points()) {
                            // OpEntryPoint <model> <function> "name" <interface...>
                            entryFuncId = ep.GetSingleWordInOperand(1);
                            break;
                        }

                        // Pre-multiply every target output by gl_Position.w before each return of the
                        // ENTRY function only. glslang does not inline, so a called helper survives as
                        // its own OpFunction; instrumenting its returns too would scale the varying
                        // more than once (w^2), breaking the identity.
                        for (auto funcIt = ctx->module()->begin(); funcIt != ctx->module()->end(); ++funcIt) {
                            if (funcIt->result_id() != entryFuncId) continue;
                            funcIt->ForEachInst([&](Instruction* inst) {
                                if (inst->opcode() != spv::Op::OpReturn &&
                                    inst->opcode() != spv::Op::OpReturnValue) {
                                    return;
                                }
                                const uint32_t posPtrId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpAccessChain, ptrOutV4, posPtrId,
                                    std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {blockVarId}},
                                                                   {SPV_OPERAND_TYPE_ID, {memberConst}}}));
                                const uint32_t posId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpLoad, v4floatTypeId, posId,
                                    std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {posPtrId}}}));
                                const uint32_t wId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpCompositeExtract, floatTy, wId,
                                    std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {posId}},
                                                                   {SPV_OPERAND_TYPE_LITERAL_INTEGER, {3u}}}));
                                for (const Target& t : targets) {
                                    const uint32_t valId = ctx->TakeNextId();
                                    inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                        ctx, spv::Op::OpLoad, t.typeId, valId,
                                        std::initializer_list<Operand>{
                                            {SPV_OPERAND_TYPE_ID, {t.var->result_id()}}}));
                                    const uint32_t scaledId =
                                        InsertScale(ctx, inst, t.typeId, valId, wId, t.isVector);
                                    inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                        ctx, spv::Op::OpStore, 0, 0,
                                        std::initializer_list<Operand>{
                                            {SPV_OPERAND_TYPE_ID, {t.var->result_id()}},
                                            {SPV_OPERAND_TYPE_ID, {scaledId}}}));
                                }
                            });
                        }
                    }
                }

                if (isFragment && !targets.empty()) {
                    Instruction* fragCoord = FindBuiltinInput(ctx, spv::BuiltIn::FragCoord);
                    uint32_t fragCoordId = 0;
                    uint32_t v4floatTypeId = 0;
                    if (fragCoord != nullptr) {
                        fragCoordId = fragCoord->result_id();
                        v4floatTypeId = VariablePointeeType(ctx, fragCoord);
                    } else {
                        v4floatTypeId = V4FloatType(ctx);
                        fragCoordId = SynthesizeFragCoord(ctx, v4floatTypeId);
                    }
                    const uint32_t floatTy = FloatType(ctx);

                    auto* defUse = ctx->get_def_use_mgr();
                    for (const Target& t : targets) {
                        // Collect every load that reads the varying. glslang lowers a whole-variable
                        // read to OpLoad(var), but a single-component read (v.x) to
                        // OpAccessChain(var) + OpLoad(chain). Both must be scaled; the identity is
                        // per-component, so scaling one loaded component by gl_FragCoord.w is valid.
                        std::vector<Instruction*> loads;
                        defUse->ForEachUser(t.var, [&](Instruction* user) {
                            if (user->opcode() == spv::Op::OpLoad &&
                                user->GetSingleWordInOperand(0) == t.var->result_id()) {
                                loads.push_back(user);
                            } else if (user->opcode() == spv::Op::OpAccessChain &&
                                       user->GetSingleWordInOperand(0) == t.var->result_id()) {
                                const uint32_t chainId = user->result_id();
                                defUse->ForEachUser(user, [&](Instruction* chainUser) {
                                    if (chainUser->opcode() == spv::Op::OpLoad &&
                                        chainUser->GetSingleWordInOperand(0) == chainId) {
                                        loads.push_back(chainUser);
                                    }
                                });
                            }
                        });

                        // Rewrite `%r = OpLoad %ty %ptr` into
                        //   %orig = OpLoad %ty %ptr
                        //   %fc   = OpLoad %v4float %fragCoord
                        //   %w    = OpCompositeExtract %float %fc 3
                        //   %r    = OpVectorTimesScalar/OpFMul %ty %orig %w   (reuse %r: uses stay intact)
                        // The op is chosen from the LOAD's own result type: a whole-vector load scales
                        // with OpVectorTimesScalar, a scalar component load with OpFMul.
                        for (Instruction* load : loads) {
                            const uint32_t loadType = load->type_id();
                            uint32_t componentFloat = 0;
                            bool loadIsVector = false;
                            if (!IsFloatScalarOrVector(ctx, loadType, componentFloat, loadIsVector)) {
                                continue;
                            }
                            const uint32_t ptrId = load->GetSingleWordInOperand(0);
                            const uint32_t origId = ctx->TakeNextId();
                            load->InsertBefore(spvtools::MakeUnique<Instruction>(
                                ctx, spv::Op::OpLoad, loadType, origId,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {ptrId}}}));
                            const uint32_t fcId = ctx->TakeNextId();
                            load->InsertBefore(spvtools::MakeUnique<Instruction>(
                                ctx, spv::Op::OpLoad, v4floatTypeId, fcId,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {fragCoordId}}}));
                            const uint32_t wId = ctx->TakeNextId();
                            load->InsertBefore(spvtools::MakeUnique<Instruction>(
                                ctx, spv::Op::OpCompositeExtract, floatTy, wId,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {fcId}},
                                                               {SPV_OPERAND_TYPE_LITERAL_INTEGER, {3u}}}));
                            load->SetOpcode(loadIsVector ? spv::Op::OpVectorTimesScalar : spv::Op::OpFMul);
                            load->SetInOperands(Instruction::OperandList{
                                {SPV_OPERAND_TYPE_ID, {origId}}, {SPV_OPERAND_TYPE_ID, {wId}}});
                        }
                    }
                }

                // Strip every NoPerspective decoration: emulated varyings now transport smooth, and
                // non-emulatable ones fall back to smooth.
                for (Instruction* dec : decorationsToKill) {
                    ctx->KillInst(dec);
                }

                ctx->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken EmulateNoPerspectivePass::CreateEmulateNoPerspectivePass() {
                return spvtools::Optimizer::PassToken(MakeUnique<EmulateNoPerspectivePass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
